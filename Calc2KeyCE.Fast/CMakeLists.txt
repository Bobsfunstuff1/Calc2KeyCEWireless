cmake_minimum_required(VERSION 3.10)

project(Calc2KeyCE VERSION 1.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_BUILD_TYPE Debug)
set(CMAKE_CXX_FLAGS_DEBUG "-g -O0")


file(GLOB SOURCES "*.cpp")
file(GLOB IMGUI_SOURCES "imgui/*.cpp")

set(IMGUI_BACKENDS
    imgui/backends/imgui_impl_sdl2.cpp
    imgui/backends/imgui_impl_opengl3.cpp
)
set(COMPRESSION_SOURCES compression.c compression.h)

add_executable(Calc2KeyCE 
${SOURCES} 
${IMGUI_SOURCES} 
${IMGUI_BACKENDS} 
${COMPRESSION_SOURCES} 
)

find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED sdl2)
pkg_check_modules(GLEW REQUIRED glew)
find_package(OpenGL REQUIRED)
pkg_check_modules(LIBUSB REQUIRED libusb-1.0)
pkg_check_modules(X11 REQUIRED x11)
pkg_check_modules(LEPTONICA REQUIRED lept)

target_include_directories(Calc2KeyCE PRIVATE
    ${SDL2_INCLUDE_DIRS}
    ${GLEW_INCLUDE_DIRS}
    ${LIBUSB_INCLUDE_DIRS}
    ${X11_INCLUDE_DIRS}
    ${LEPTONICA_INCLUDE_DIRS}
    ${CMAKE_CURRENT_SOURCE_DIR}
    imgui
    imgui/backends
)

target_link_libraries(Calc2KeyCE PRIVATE
    ${SDL2_LIBRARIES}
    ${GLEW_LIBRARIES}
    ${OPENGL_LIBRARIES}
    ${LIBUSB_LIBRARIES}
    ${X11_LIBRARIES}
    ${LEPTONICA_LIBRARIES}
    pthread
    dl
    z
    Xtst
    X11
    Xext
)
