cmake_minimum_required(VERSION 3.10)
project(steamvr_battery_indicator CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

add_executable(steamvr_battery_indicator src/main.cpp)
target_include_directories(steamvr_battery_indicator PRIVATE src)

if(WIN32)
    # No extra link libraries: the OpenVR runtime is loaded at run time.
else()
    target_link_libraries(steamvr_battery_indicator PRIVATE dl)
endif()

# The bundled OpenVR header produces many unused-variable warnings; keep the
# build output readable while still flagging real problems in our own files.
if(MSVC)
    target_compile_options(steamvr_battery_indicator PRIVATE /W3 /wd4996)
else()
    target_compile_options(steamvr_battery_indicator PRIVATE -Wall -Wextra -Wno-unused-variable)
endif()
