cmake_minimum_required(VERSION 3.22)
project(VonDeLarsen VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release)
endif()

option(VDL_BUILD_TESTS "Build the portable DSP tests" ON)
option(VDL_BUILD_PLUGIN "Build the JUCE plugin (macOS, Apple Silicon)" ${APPLE})

add_subdirectory(core)

if(VDL_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(VDL_BUILD_PLUGIN)
    add_subdirectory(plugin)
endif()
