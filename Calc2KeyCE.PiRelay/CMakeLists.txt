cmake_minimum_required(VERSION 3.10)

project(Calc2KeyPiRelay VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBUSB REQUIRED libusb-1.0)

add_executable(Calc2KeyPiRelay
    main.cpp
    ../Calc2KeyCE.Fast/bridge_client.cpp
    ../Calc2KeyCE.Fast/bridge_protocol.cpp
)

add_executable(Calc2PiConsoleRelay
    terminal_main.cpp
)

target_include_directories(Calc2KeyPiRelay PRIVATE
    ${LIBUSB_INCLUDE_DIRS}
    ../Calc2KeyCE.Fast
)

target_link_libraries(Calc2KeyPiRelay PRIVATE
    ${LIBUSB_LIBRARIES}
    pthread
)

target_include_directories(Calc2PiConsoleRelay PRIVATE
    ${LIBUSB_INCLUDE_DIRS}
)

target_link_libraries(Calc2PiConsoleRelay PRIVATE
    ${LIBUSB_LIBRARIES}
    pthread
    util
)
