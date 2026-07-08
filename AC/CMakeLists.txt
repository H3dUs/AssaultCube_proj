cmake_minimum_required(VERSION 3.10)
project(AC_ALL CXX)

set(CMAKE_CXX_STANDARD 20)

# === ЖЕСТКИЙ СБРОС НА СТАТИЧЕСКУЮ ЛИНКОВКУ (/MT) ===
if(MSVC)
    # Заменяем динамический рантайм (/MD) на статический (/MT) во всех флагах компилятора
    string(REPLACE "/MD" "/MT" CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE}")
    string(REPLACE "/MD" "/MT" CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")
    
    # На всякий случай дублируем современным методом CMake
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# === IMGUI ===
set(IMGUI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/imgui)

include_directories(
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
)

add_library(imgui_lib STATIC
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/backends/imgui_impl_win32.cpp
    ${IMGUI_DIR}/backends/imgui_impl_opengl2.cpp
)

target_link_libraries(imgui_lib opengl32)

add_library(${PROJECT_NAME} SHARED main.cpp)
target_link_libraries(${PROJECT_NAME} imgui_lib opengl32)
