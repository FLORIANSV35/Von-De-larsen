cmake_minimum_required(VERSION 3.22)
project(VonDeLarsen VERSION 0.1.0 LANGUAGES C CXX)

if(APPLE)
    # Enabled here, at the top level, so that JUCE's Objective-C++ module sources see the compile rules.
    enable_language(OBJC OBJCXX)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release)
endif()

option(VDL_BUILD_TESTS "Build the portable DSP tests" ON)
option(VDL_BUILD_PLUGIN "Build the JUCE AU/VST3 plugin (macOS, Apple Silicon)" ${APPLE})
option(VDL_BUILD_STANDALONE "Build the multichannel standalone app (1-8 channels)" ${APPLE})

if(VDL_BUILD_PLUGIN OR VDL_BUILD_STANDALONE)
    set(CMAKE_OSX_ARCHITECTURES arm64 CACHE STRING "" FORCE) # Neural Engine = Apple Silicon
    set(CMAKE_OSX_DEPLOYMENT_TARGET 13.0 CACHE STRING "" FORCE)
    include(FetchContent)
    FetchContent_Declare(JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG 8.0.4
        GIT_SHALLOW ON)
    FetchContent_MakeAvailable(JUCE)
endif()

add_subdirectory(core)

if(VDL_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(VDL_BUILD_PLUGIN)
    add_subdirectory(plugin)
endif()

if(VDL_BUILD_STANDALONE)
    add_subdirectory(standalone)
endif()
