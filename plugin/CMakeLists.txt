set(CMAKE_OSX_ARCHITECTURES arm64 CACHE STRING "" FORCE) # Neural Engine = Apple Silicon
set(CMAKE_OSX_DEPLOYMENT_TARGET 13.0 CACHE STRING "" FORCE)

include(FetchContent)
FetchContent_Declare(JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG 8.0.4
    GIT_SHALLOW ON)
FetchContent_MakeAvailable(JUCE)

juce_add_plugin(VonDeLarsen
    PRODUCT_NAME "Von De Larsen"
    COMPANY_NAME "VonDeLarsen"
    PLUGIN_MANUFACTURER_CODE Vdls
    PLUGIN_CODE Vdl1
    FORMATS AU VST3 Standalone
    AU_MAIN_TYPE kAudioUnitType_Effect
    VST3_CATEGORIES Fx
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    COPY_PLUGIN_AFTER_BUILD TRUE)

target_sources(VonDeLarsen PRIVATE PluginProcessor.cpp)
target_compile_definitions(VonDeLarsen PUBLIC
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0)
target_link_libraries(VonDeLarsen
    PRIVATE vc_core juce::juce_audio_utils
    PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)
