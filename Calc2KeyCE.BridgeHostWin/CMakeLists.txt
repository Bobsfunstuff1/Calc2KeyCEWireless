cmake_minimum_required(VERSION 3.20)

project(Calc2KeyCEBridgeHostWin LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(Calc2KeyCEBridgeHostWin WIN32
    bridge_host_main.cpp
    ../Calc2KeyCE.Fast/bridge_protocol.cpp
    ../Calc2KeyCE.Fast/compression.c
    ../Calc2KeyCE.Fast/imgui/imgui.cpp
    ../Calc2KeyCE.Fast/imgui/imgui_draw.cpp
    ../Calc2KeyCE.Fast/imgui/imgui_tables.cpp
    ../Calc2KeyCE.Fast/imgui/imgui_widgets.cpp
    ../Calc2KeyCE.Fast/imgui/backends/imgui_impl_win32.cpp
    ../Calc2KeyCE.Fast/imgui/backends/imgui_impl_dx11.cpp
)

target_include_directories(Calc2KeyCEBridgeHostWin PRIVATE
    .
    ../Calc2KeyCE.Fast
    ../Calc2KeyCE.Fast/imgui
    ../Calc2KeyCE.Fast/imgui/backends
)

find_path(LEPTONICA_INCLUDE_DIR
    NAMES leptonica/allheaders.h
    HINTS
        ENV LEPTONICA_ROOT
    PATH_SUFFIXES include
)

find_library(LEPTONICA_LIBRARY
    NAMES leptonica lept leptonica-1.85.0
    HINTS
        ENV LEPTONICA_ROOT
    PATH_SUFFIXES lib
)

if (NOT LEPTONICA_INCLUDE_DIR OR NOT LEPTONICA_LIBRARY)
    message(FATAL_ERROR "Leptonica was not found. Set LEPTONICA_ROOT or add it to CMAKE_PREFIX_PATH.")
endif()

target_include_directories(Calc2KeyCEBridgeHostWin PRIVATE
    ${LEPTONICA_INCLUDE_DIR}
)

target_link_libraries(Calc2KeyCEBridgeHostWin PRIVATE
    ws2_32
    user32
    gdi32
    dxgi
    comdlg32
    d3d11
    ${LEPTONICA_LIBRARY}
    gif
    jpeg
    libpng16
    tiff
    zlib
    lzma
    libwebp
    libwebpdecoder
    libwebpdemux
    libwebpmux
)
