cmake_minimum_required(VERSION 3.22)
project(plugin_probe VERSION 0.1 LANGUAGES C CXX OBJC OBJCXX)
set(CMAKE_CXX_STANDARD 17)

include(FetchContent)
FetchContent_Declare(JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG 8.0.4
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(JUCE)

juce_add_console_app(probe PRODUCT_NAME probe)
target_sources(probe PRIVATE probe.cpp)
target_compile_definitions(probe PRIVATE JUCE_PLUGINHOST_VST3=1 JUCE_PLUGINHOST_AU=0 JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0
    JUCE_MODAL_LOOPS_PERMITTED=1)
target_link_libraries(probe PRIVATE juce::juce_audio_processors juce::juce_audio_basics juce::juce_events
    juce::juce_core juce::juce_recommended_config_flags)
