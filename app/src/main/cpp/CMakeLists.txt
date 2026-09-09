cmake_minimum_required(VERSION 3.22.1)
project("usbip_server")

# Require C++17 or higher for std::atomic alignment and cacheline spacing
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(${CMAKE_PROJECT_NAME} SHARED
        usbip_server.cpp
        SyntheticInputJni.cpp
        SyntheticEventTranslator.cpp
        SyntheticUrbHandler.cpp)

find_library(log-lib log)

target_link_libraries(${CMAKE_PROJECT_NAME}
        ${log-lib})
