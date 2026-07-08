cmake_minimum_required(VERSION 3.10)
project(AC_ALL CXX)

set(CMAKE_CXX_STANDARD 20)

# === ЖЕСТКИЙ СБРОС НА СТАТИЧЕСКУЮ ЛИНКОВКУ (/MT) ===
if(MSVC)
    # Используем современный метод CMake для статической линковки рантайма (/MT для Release, /MTd для Debug)
    # Убираем старые флаги /MD*, чтобы не было конфликтов
    foreach(flag_var CMAKE_CXX_FLAGS CMAKE_CXX_FLAGS_RELEASE CMAKE_CXX_FLAGS_DEBUG CMAKE_CXX_FLAGS_MINSIZEREL CMAKE_CXX_FLAGS_RELWITHDEBINFO)
        string(REGEX REPLACE "/MDd" "/MTd" ${flag_var} "${${flag_var}}")
        string(REGEX REPLACE "/MD" "/MT" ${flag_var} "${${flag_var}}")
    endforeach()
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
