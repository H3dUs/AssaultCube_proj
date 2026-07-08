#include <Windows.h>
#include <iostream>
#include <cmath>
#include <cstdio>
#include <vector>

#include "imgui/imgui.h"
#include "imgui/backends/imgui_impl_win32.h"
#include "imgui/backends/imgui_impl_opengl2.h"

// === ГЛОБАЛЫ ===
uintptr_t jmpBackAddr;
uintptr_t localPlayerPtrAddr;
HMODULE g_hModule = nullptr;

// === ОФФСЕТЫ AC 1.3.0.2 ===
constexpr uintptr_t OFF_ENTITY_LIST = 0x18AC04;
constexpr uintptr_t OFF_PLAYER_COUNT = 0x18AC0C;
constexpr uintptr_t OFF_LP = 0x17E0A8;

constexpr int OFF_POS_X = 0x4;
constexpr int OFF_POS_Y = 0x8;
constexpr int OFF_POS_Z = 0xC;

constexpr int OFF_YAW = 0x34;
constexpr int OFF_PITCH = 0x38;
constexpr int OFF_HP = 0xEC;
constexpr int OFF_TEAM = 0x30C;

// Отдача камеры: перепрыгиваем subss xmm1, xmm0 чтобы не сломать xmm1
// Точка сложения дельты с текущим Pitch (длина 5 байт)
constexpr uintptr_t OFF_RECOIL_PHYSICS = 0xC2EB9;  // addss xmm2, [esi+38]
constexpr unsigned int RECOIL_PATCH_SIZE = 5;

// Пушбэк: NOP-аем запись координат откидывания
constexpr uintptr_t OFF_PUSHBACK_1 = 0xBFD7D;       // movq [edx+04], xmm0
constexpr uintptr_t OFF_PUSHBACK_2 = 0xBFE02;       // movss [edx+08], xmm0
constexpr unsigned int PUSHBACK_PATCH_SIZE = 5;

constexpr float SMOOTH = 1.0f;
constexpr float AIM_FOV = 100.0f;
constexpr float PI = 3.14159265f;

// Глобальные флаги синхронизации
bool noRecoilActive = false;
bool aimbotActive = false;
bool godModeActive = false;
bool menuOpen = false;
bool shouldShutdown = false;
bool renderCleanedUp = false; // Сигнал завершения очистки рендер-потока
bool imguiShutdownDone = false; // Флаг однократного завершения ImGui

// Оригинальный wglSwapBuffers
typedef BOOL(__stdcall* WglSwapBuffers_t)(HDC hdc);
WglSwapBuffers_t realWglSwapBuffers = nullptr; // Чистый адрес из opengl32.dll
WglSwapBuffers_t oWglSwapBuffers = nullptr; // Указывает на gateway

// Оригинальный WndProc
WNDPROC oWndProc = nullptr;

// Трамплин хука wglSwapBuffers
struct TrampolineHook {
    BYTE* src;
    BYTE* dst;
    BYTE  bytesToPatch[5];
    BYTE  originalBytes[5];
    SIZE_T size;
    BYTE* gateway;

    TrampolineHook(BYTE* src, BYTE* dst, SIZE_T size) :
        src(src), dst(dst), size(size), gateway(nullptr) {
        // Сохраняем оригинальные байты
        memcpy(originalBytes, src, size);

        // Создаем gateway
        gateway = (BYTE*)VirtualAlloc(nullptr, size + 5, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (gateway) {
            memcpy(gateway, src, size);
            uintptr_t gatewayRelativeAddr = (uintptr_t)(src - gateway - 5);
            gateway[size] = 0xE9;
            *(uintptr_t*)(gateway + size + 1) = gatewayRelativeAddr;

            // Подготавливаем патч для src
            uintptr_t hookRelativeAddr = (uintptr_t)(dst - src - 5);
            bytesToPatch[0] = 0xE9; // JMP
            *(uintptr_t*)(bytesToPatch + 1) = hookRelativeAddr;
        }
    }

    ~TrampolineHook() {
        if (gateway) {
            VirtualFree(gateway, 0, MEM_RELEASE);
        }
    }

    void Enable() {
        if (gateway) {
            DWORD oldProtect;
            VirtualProtect(src, size, PAGE_EXECUTE_READWRITE, &oldProtect);
            memcpy(src, bytesToPatch, size);
            VirtualProtect(src, size, oldProtect, &oldProtect);
        }
    }

    void Disable() {
        if (gateway) {
            DWORD oldProtect;
            VirtualProtect(src, size, PAGE_EXECUTE_READWRITE, &oldProtect);
            memcpy(src, originalBytes, size);
            VirtualProtect(src, size, oldProtect, &oldProtect);
        }
    }
};

TrampolineHook* wglSwapBuffersHook = nullptr;

inline bool IsValidScreenPtr(uintptr_t ptr) {
    return (ptr >= 0x10000 && ptr < 0x7FFFFFFF);
}

template<typename T>
bool SafeRead(uintptr_t addr, T& out) {
    if (addr < 0x10000 || addr > 0x7FFFFFFF) return false;
    __try {
        out = *(volatile T*)addr;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

template<typename T>
bool SafeWrite(uintptr_t addr, const T& val) {
    if (addr < 0x10000 || addr > 0x7FFFFFFF) return false;
    __try {
        *(volatile T*)addr = val;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// === ПЕЩЕРА ГОДМОДА ===
void __declspec(naked) DamageCodeCave() {
    __asm {
        push ecx
        mov  ecx, [localPlayerPtrAddr]
        mov  ecx, [ecx]
        add  ecx, 0xE8
        cmp  ebx, ecx
        pop  ecx
        jne  is_bot

        mov  eax, esi
        pop  esi
        jmp[jmpBackAddr]

        is_bot:
        sub  [ebx + 0x04], esi
        mov  eax, esi
        pop  esi
        jmp  [jmpBackAddr]
    }
}

// === ХУКИ И ПАТЧИ ===
void Hook(BYTE* src, BYTE* dst, unsigned int size) {
    DWORD oldProtect;
    VirtualProtect(src, size, PAGE_EXECUTE_READWRITE, &oldProtect);
    uintptr_t relativeAddress = (uintptr_t)(dst - src - 5);
    *src = 0xE9;
    *(uintptr_t*)(src + 1) = relativeAddress;
    for (unsigned int i = 5; i < size; i++) {
        src[i] = 0x90;
    }
    VirtualProtect(src, size, oldProtect, &oldProtect);
}

void Unhook(BYTE* src, BYTE* originalBytes, unsigned int size) {
    DWORD oldProtect;
    VirtualProtect(src, size, PAGE_EXECUTE_READWRITE, &oldProtect);
    memcpy(src, originalBytes, size);
    VirtualProtect(src, size, oldProtect, &oldProtect);
}

void Patch(BYTE* dst, BYTE* src, unsigned int size) {
    DWORD oldProtect;
    VirtualProtect(dst, size, PAGE_EXECUTE_READWRITE, &oldProtect);
    memcpy(dst, src, size);
    VirtualProtect(dst, size, oldProtect, &oldProtect);
}

// === МАТЕМАТИКА ===
inline float AngleDelta(float cur, float tgt) {
    float d = tgt - cur;
    d = fmodf(d + 180.0f, 360.0f);
    if (d < 0.0f) d += 360.0f;
    return d - 180.0f;
}

void CalcAngle(float* from, float* to, float* yaw, float* pitch) {
    float dx = to[0] - from[0];
    float dy = to[1] - from[1];
    float dz = to[2] - from[2];

    float dist = sqrtf(dx * dx + dy * dy);
    if (dist < 0.001f) dist = 0.001f;

    *yaw = atan2f(dy, dx) * (180.0f / PI) + 90.0f;
    *pitch = atan2f(dz, dist) * (180.0f / PI);

    *yaw = fmodf(*yaw, 360.0f);
    if (*yaw < 0.0f) *yaw += 360.0f;
}

float GetFov(float myYaw, float myPitch, float tYaw, float tPitch) {
    float dy = AngleDelta(myYaw, tYaw);
    float dp = tPitch - myPitch;
    return sqrtf(dy * dy + dp * dp);
}

// === ПОТОК АИМБОТА (Чистый, без анти-отдачи) ===
DWORD WINAPI AimbotThread(LPVOID param) {
    uintptr_t gameBase = (uintptr_t)param;

    while (!shouldShutdown) {
        if (aimbotActive && (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0) {
            uintptr_t localPlayer = 0;
            if (!SafeRead(gameBase + OFF_LP, localPlayer) || !IsValidScreenPtr(localPlayer)) {
                Sleep(1);
                continue;
            }

            float myPos[3] = { 0 };
            float myYaw = 0, myPitch = 0;
            int myTeam = -1;
            bool ok = true;

            ok &= SafeRead(localPlayer + OFF_POS_X, myPos[0]);
            ok &= SafeRead(localPlayer + OFF_POS_Y, myPos[1]);
            ok &= SafeRead(localPlayer + OFF_POS_Z, myPos[2]);
            ok &= SafeRead(localPlayer + OFF_YAW, myYaw);
            ok &= SafeRead(localPlayer + OFF_PITCH, myPitch);
            ok &= SafeRead(localPlayer + OFF_TEAM, myTeam);

            if (ok) {
                uintptr_t entListPtr = 0;
                int numPlayers = 0;
                SafeRead(gameBase + OFF_ENTITY_LIST, entListPtr);
                SafeRead(gameBase + OFF_PLAYER_COUNT, numPlayers);

                if (numPlayers >= 2 && numPlayers <= 32 && IsValidScreenPtr(entListPtr)) {
                    float bestFov = AIM_FOV;
                    float bestYaw = 0, bestPitch = 0;
                    bool found = false;

                    for (int i = 0; i < numPlayers; ++i) {
                        uintptr_t ent = 0;
                        if (!SafeRead(entListPtr + i * 4, ent)) continue;
                        if (!ent || ent == localPlayer || !IsValidScreenPtr(ent)) continue;

                        int hp = 0, team = 0;
                        if (!SafeRead(ent + OFF_HP, hp)) continue;
                        if (!SafeRead(ent + OFF_TEAM, team)) continue;

                        if (hp <= 0 || hp > 100 || team == myTeam) continue;

                        float enemyPos[3] = { 0 };
                        bool eok = true;
                        eok &= SafeRead(ent + OFF_POS_X, enemyPos[0]);
                        eok &= SafeRead(ent + OFF_POS_Y, enemyPos[1]);
                        eok &= SafeRead(ent + OFF_POS_Z, enemyPos[2]);

                        if (!eok) continue;

                        if (fabsf(enemyPos[0]) > 1e6f || fabsf(enemyPos[1]) > 1e6f || fabsf(enemyPos[2]) > 1e6f)
                            continue;

                        float tYaw, tPitch;
                        CalcAngle(myPos, enemyPos, &tYaw, &tPitch);

                        float fov = GetFov(myYaw, myPitch, tYaw, tPitch);
                        if (fov < bestFov) {
                            bestFov = fov;
                            bestYaw = tYaw;
                            bestPitch = tPitch;
                            found = true;
                        }
                    }

                    if (found) {
                        float dYaw = AngleDelta(myYaw, bestYaw);
                        float dPitch = bestPitch - myPitch;

                        float targetYaw = myYaw + (dYaw / SMOOTH);
                        float targetPitch = myPitch + (dPitch / SMOOTH);

                        targetYaw = fmodf(targetYaw, 360.0f);
                        if (targetYaw < 0.0f) targetYaw += 360.0f;

                        if (targetPitch > 85.0f) targetPitch = 85.0f;
                        if (targetPitch < -85.0f) targetPitch = -85.0f;

                        SafeWrite(localPlayer + OFF_YAW, targetYaw);
                        SafeWrite(localPlayer + OFF_PITCH, targetPitch);
                    }
                }
            }
        }
        Sleep(1);
    }
    return 0;
}

// === HOOK WNDPROC ===
LRESULT APIENTRY hkWndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    if (menuOpen) {
        if (ImGui_ImplWin32_WndProcHandler(hwnd, uMsg, wParam, lParam)) {
            // Сообщение обработано ImGui - блокируем его
            return TRUE;
        }
    }
    // Все остальные сообщения передаём в оригинальный WndProc
    return CallWindowProc(oWndProc, hwnd, uMsg, wParam, lParam);
}

// === HOOK WGLSWAPBUFFERS ===
BOOL __stdcall hkwglSwapBuffers(HDC hdc) {
    static bool init = false;
    if (!init) {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;     // Enable Keyboard Controls
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;      // Enable Gamepad Controls

        ImGui::StyleColorsDark();

        HWND hwnd = WindowFromDC(hdc);
        oWndProc = (WNDPROC)SetWindowLongPtr(hwnd, GWL_WNDPROC, (LONG_PTR)hkWndProc);

        ImGui_ImplWin32_Init(hwnd);
        ImGui_ImplOpenGL2_Init();
        init = true;
    }

    if (shouldShutdown) {
        if (!imguiShutdownDone) {
            // Cleanup ImGui in the rendering thread
            ImGui_ImplOpenGL2_Shutdown();
            ImGui_ImplWin32_Shutdown();
            ImGui::DestroyContext();

            // Восстанавливаем оригинальный WndProc
            HWND hwnd = WindowFromDC(hdc);
            SetWindowLongPtr(hwnd, GWL_WNDPROC, (LONG_PTR)oWndProc);
            
            imguiShutdownDone = true;
            renderCleanedUp = true; // Сигнализируем о завершении очистки
        }
        return oWglSwapBuffers(hdc);
    }

    ImGui_ImplOpenGL2_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    // Render menu
    if (menuOpen) {
        ImGui::Begin("AC Multihack Menu", &menuOpen, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_AlwaysAutoResize);
        ImGui::Checkbox("Aimbot", &aimbotActive);
        ImGui::Checkbox("Godmode", &godModeActive);
        ImGui::End();
    }

    ImGui::EndFrame();
    ImGui::Render();
    ImGui_ImplOpenGL2_RenderDrawData(ImGui::GetDrawData());

    return oWglSwapBuffers(hdc);
}

// === ОСНОВНОЙ ПОТОК ===
DWORD WINAPI MainThread(HMODULE hModule) {
    g_hModule = hModule;
    AllocConsole();
    FILE* fDummy;
    freopen_s(&fDummy, "CONOUT$", "w", stdout);

    std::cout << "[+] AC MULTIHACK INJECTED\n";
    std::cout << "[+] Hold [RMB] for Aimbot\n";
    std::cout << "[+] [NUMPAD 1] GodMode\n";
    std::cout << "[+] [NUMPAD 3] No Recoil & No Pushback\n";
    std::cout << "[+] [DELETE] Unload\n\n";

    uintptr_t gameBase = (uintptr_t)GetModuleHandleA(nullptr);
    localPlayerPtrAddr = gameBase + OFF_LP;

    // === GodMode setup ===
    uintptr_t godHookOffset = 0x1C223;
    BYTE* godHookAddr = (BYTE*)(gameBase + godHookOffset);
    jmpBackAddr = (uintptr_t)(godHookAddr + 6);
    BYTE originalGodBytes[] = { 0x29, 0x73, 0x04, 0x8B, 0xC6, 0x5E };

    // === No Recoil setup (Angle Fix метод) ===
    BYTE* recoilPhysicsAddr = (BYTE*)(gameBase + OFF_RECOIL_PHYSICS);
    BYTE origRecoilBytes[RECOIL_PATCH_SIZE] = { 0 };

    // === No Pushback setup (NOP метод) ===
    BYTE* pushback1Addr = (BYTE*)(gameBase + OFF_PUSHBACK_1);
    BYTE* pushback2Addr = (BYTE*)(gameBase + OFF_PUSHBACK_2);
    BYTE origPush1Bytes[PUSHBACK_PATCH_SIZE] = { 0 };
    BYTE origPush2Bytes[PUSHBACK_PATCH_SIZE] = { 0 };

    bool addrsOk = IsValidScreenPtr((uintptr_t)recoilPhysicsAddr) &&
        IsValidScreenPtr((uintptr_t)pushback1Addr) &&
        IsValidScreenPtr((uintptr_t)pushback2Addr);

    if (addrsOk) {
        memcpy(origRecoilBytes, recoilPhysicsAddr, RECOIL_PATCH_SIZE);
        memcpy(origPush1Bytes, pushback1Addr, PUSHBACK_PATCH_SIZE);
        memcpy(origPush2Bytes, pushback2Addr, PUSHBACK_PATCH_SIZE);

        // Выводим оригинальные байты для уверенности
        std::cout << "[?] Recoil bytes at 0x" << std::hex << (uintptr_t)recoilPhysicsAddr << ": ";
        for (int i = 0; i < RECOIL_PATCH_SIZE; i++) printf("%02X ", origRecoilBytes[i]);
        std::cout << "\n[?] Pushback 1 bytes at 0x" << (uintptr_t)pushback1Addr << ": ";
        for (int i = 0; i < PUSHBACK_PATCH_SIZE; i++) printf("%02X ", origPush1Bytes[i]);
        std::cout << "\n[?] Pushback 2 bytes at 0x" << (uintptr_t)pushback2Addr << ": ";
        for (int i = 0; i < PUSHBACK_PATCH_SIZE; i++) printf("%02X ", origPush2Bytes[i]);
        std::cout << std::dec << "\n\n";
    }
    else {
        std::cout << "[!] Invalid addresses, features disabled.\n\n";
    }

    // Hook wglSwapBuffers (fixed to avoid recursion)
    HMODULE hOpenGL = GetModuleHandleA("opengl32.dll");
    WglSwapBuffers_t realWglSwapBuffers = (WglSwapBuffers_t)GetProcAddress(hOpenGL, "wglSwapBuffers");
    // Получаем оригинальный адрес функции
    realWglSwapBuffers = (WglSwapBuffers_t)GetProcAddress(hOpenGL, "wglSwapBuffers");
    
    // Инициализируем хук с реальным адресом
    wglSwapBuffersHook = new TrampolineHook((BYTE*)realWglSwapBuffers, (BYTE*)hkwglSwapBuffers, 5);
    
    // Перенаправляем вызовы через gateway
    oWglSwapBuffers = (WglSwapBuffers_t)wglSwapBuffersHook->gateway;
    wglSwapBuffersHook->Enable();
    
    // Запускаем поток аимбота
    CreateThread(nullptr, 0, AimbotThread, (LPVOID)gameBase, 0, nullptr);


    bool godKeyPressed = false;
    bool recoilKeyPressed = false;
    bool insertKeyPressed = false;

    while (!shouldShutdown) {
        if (GetAsyncKeyState(VK_DELETE) & 0x8000) {
            shouldShutdown = true;
        }

        // --- Toggle ImGui Menu (INSERT) ---
        if ((GetAsyncKeyState(VK_INSERT) & 0x8000) && !insertKeyPressed) {
            insertKeyPressed = true;
            menuOpen = !menuOpen;
            ImGuiIO& io = ImGui::GetIO();
            io.MouseDrawCursor = menuOpen; // Show/hide ImGui mouse cursor
        }
        else if (!(GetAsyncKeyState(VK_INSERT) & 0x8000)) insertKeyPressed = false;

        // --- Handle GodMode activation from ImGui --- 
        static bool lastGodModeActive = false;
        if (godModeActive != lastGodModeActive) {
            if (godModeActive) {
                Hook(godHookAddr, (BYTE*)DamageCodeCave, 6);
                std::cout << "[+] GodMode: ON\n";
            }
            else {
                Unhook(godHookAddr, originalGodBytes, 6);
                std::cout << "[-] GodMode: OFF\n";
            }
            lastGodModeActive = godModeActive;
        }

        // --- Toggle No Recoil & Pushback (NUMPAD 3) ---
        if ((GetAsyncKeyState(VK_NUMPAD3) & 0x8000) && !recoilKeyPressed && addrsOk) {
            recoilKeyPressed = true;
            bool newState = !noRecoilActive;
            noRecoilActive = newState;

            if (newState) {
                // 1. АНТИ-ОТДАЧА: заменяем addss на movss, чтобы обнулить влияние отдачи
                // Оригинальные байты addss xmm2, [esi+38]: F3 0F 58 56 38
                // Байты патча movss xmm2, [esi+38]:        F3 0F 10 56 38
                BYTE recoilPatch[RECOIL_PATCH_SIZE] = { 0xF3, 0x0F, 0x10, 0x56, 0x38 };
                Patch(recoilPhysicsAddr, recoilPatch, RECOIL_PATCH_SIZE);

                // 2. ПУШБЭК: Классический NOP
                BYTE pushNops[PUSHBACK_PATCH_SIZE];
                memset(pushNops, 0x90, PUSHBACK_PATCH_SIZE);
                Patch(pushback1Addr, pushNops, PUSHBACK_PATCH_SIZE);
                Patch(pushback2Addr, pushNops, PUSHBACK_PATCH_SIZE);

                std::cout << "[+] No Recoil (Angle Fix) & No Pushback: ON\n";
            }
            else {
                // Восстанавливаем всё как было
                Patch(recoilPhysicsAddr, origRecoilBytes, RECOIL_PATCH_SIZE);
                Patch(pushback1Addr, origPush1Bytes, PUSHBACK_PATCH_SIZE);
                Patch(pushback2Addr, origPush2Bytes, PUSHBACK_PATCH_SIZE);

                std::cout << "[-] No Recoil & No Pushback: OFF\n";
            }
        }
        else if (!(GetAsyncKeyState(VK_NUMPAD3) & 0x8000)) recoilKeyPressed = false;

        // --- Infinite Ammo ---
        uintptr_t localPlayerVal = 0;
        if (SafeRead(localPlayerPtrAddr, localPlayerVal) && IsValidScreenPtr(localPlayerVal)) {
            int ammo = 0;
            if (SafeRead(localPlayerVal + 0x140, ammo) && ammo < 100) {
                SafeWrite(localPlayerVal + 0x140, 1000);
            }
        }

        Sleep(5);
    }

    // Wait for rendering thread to clean up
    while (!renderCleanedUp) { Sleep(10); }

    // Safe hook destruction
    if (wglSwapBuffersHook) {
        wglSwapBuffersHook->Disable();
        Sleep(50); // Гарантированный выход из рендер-потока
        delete wglSwapBuffersHook;
        wglSwapBuffersHook = nullptr;
    }

    // === ОЧИСТКА ===
    if (godModeActive) Unhook(godHookAddr, originalGodBytes, 6);

    if (noRecoilActive && addrsOk) {
        Patch(recoilPhysicsAddr, origRecoilBytes, RECOIL_PATCH_SIZE);
        Patch(pushback1Addr, origPush1Bytes, PUSHBACK_PATCH_SIZE);
        Patch(pushback2Addr, origPush2Bytes, PUSHBACK_PATCH_SIZE);
    }

    std::cout << "CHEAT CLOSED\n";
    if (fDummy) fclose(fDummy);
    FreeConsole();
    FreeLibraryAndExitThread(hModule, 0);
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    if (ul_reason_for_call == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        CreateThread(nullptr, 0, (LPTHREAD_START_ROUTINE)MainThread, hModule, 0, nullptr);
    }
    return TRUE;
}
