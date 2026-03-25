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

set(_leptonica_roots "")
if (DEFINED LEPTONICA_ROOT AND NOT LEPTONICA_ROOT STREQUAL "")
    list(APPEND _leptonica_roots "${LEPTONICA_ROOT}")
endif()
if (DEFINED ENV{LEPTONICA_ROOT} AND NOT "$ENV{LEPTONICA_ROOT}" STREQUAL "")
    list(APPEND _leptonica_roots "$ENV{LEPTONICA_ROOT}")
endif()

set(LEPTONICA_INCLUDE_DIR "")
foreach(_leptonica_root IN LISTS _leptonica_roots)
    if (EXISTS "${_leptonica_root}/include/leptonica/allheaders.h")
        set(LEPTONICA_INCLUDE_DIR "${_leptonica_root}/include")
        break()
    elseif (EXISTS "${_leptonica_root}/include/allheaders.h")
        set(LEPTONICA_INCLUDE_DIR "${_leptonica_root}/include")
        break()
    endif()
endforeach()

find_package(Leptonica CONFIG QUIET)

if (TARGET leptonica)
    set(LEPTONICA_TARGET leptonica)
elseif (TARGET Leptonica::Leptonica)
    set(LEPTONICA_TARGET Leptonica::Leptonica)
endif()

if (NOT LEPTONICA_INCLUDE_DIR)
    find_path(LEPTONICA_INCLUDE_DIR
        NAMES leptonica/allheaders.h allheaders.h
        HINTS ${_leptonica_roots}
        PATH_SUFFIXES include include/leptonica
    )
endif()

if (NOT LEPTONICA_INCLUDE_DIR AND NOT LEPTONICA_TARGET)
    find_path(LEPTONICA_INCLUDE_DIR
        NAMES leptonica/allheaders.h allheaders.h
        PATH_SUFFIXES include include/leptonica
    )
endif()

if (NOT LEPTONICA_TARGET)
    find_library(LEPTONICA_LIBRARY
        NAMES leptonica lept leptonica-1.85.0
        HINTS ${_leptonica_roots}
        PATH_SUFFIXES lib
    )

    if (NOT LEPTONICA_INCLUDE_DIR OR NOT LEPTONICA_LIBRARY)
        message(FATAL_ERROR "Leptonica was not found. Set LEPTONICA_ROOT or add it to CMAKE_PREFIX_PATH.")
    endif()

    cmake_path(GET LEPTONICA_INCLUDE_DIR FILENAME _leptonica_include_leaf)
    if (_leptonica_include_leaf STREQUAL "leptonica")
        cmake_path(GET LEPTONICA_INCLUDE_DIR PARENT_PATH LEPTONICA_INCLUDE_DIR)
    endif()

    target_include_directories(Calc2KeyCEBridgeHostWin PRIVATE
        ${LEPTONICA_INCLUDE_DIR}
    )
endif()

if (LEPTONICA_INCLUDE_DIR)
    target_include_directories(Calc2KeyCEBridgeHostWin PRIVATE
        ${LEPTONICA_INCLUDE_DIR}
    )
endif()

target_link_libraries(Calc2KeyCEBridgeHostWin PRIVATE
    ws2_32
    user32
    gdi32
    dxgi
    comdlg32
    d3d11
    $<$<BOOL:${LEPTONICA_TARGET}>:${LEPTONICA_TARGET}>
    $<$<NOT:$<BOOL:${LEPTONICA_TARGET}>>:${LEPTONICA_LIBRARY}>
)
