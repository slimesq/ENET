cmake_minimum_required(VERSION 3.20)

project(ENET LANGUAGES CXX)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "ENET uses Linux epoll, POSIX sockets, and /proc; configure it on Linux.")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

find_package(Threads REQUIRED)

option(ENET_BUILD_LOGIN_SERVER "Build LoginSvr (requires MySQL client headers and library)" ON)

add_subdirectory(EdoyunNet)
add_subdirectory(RtmpServer)
add_subdirectory(SigServer)
add_subdirectory(LoadBanceServer)
if(ENET_BUILD_LOGIN_SERVER)
    add_subdirectory(LoginServer)
endif()
