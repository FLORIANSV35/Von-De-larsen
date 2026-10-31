juce_add_plugin(VonDeLarsen
    PRODUCT_NAME "Von De Larsen"
    COMPANY_NAME "VonDeLarsen"
    ICON_BIG "${CMAKE_CURRENT_SOURCE_DIR}/Icon.png"
    ICON_SMALL "${CMAKE_CURRENT_SOURCE_DIR}/Icon.png"
    PLUGIN_MANUFACTURER_CODE Vdls
    PLUGIN_CODE Vdl1
    FORMATS AU VST3
    AU_MAIN_TYPE kAudioUnitType_Effect
    VST3_CATEGORIES Fx
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    COPY_PLUGIN_AFTER_BUILD TRUE)

target_sources(VonDeLarsen PRIVATE PluginProcessor.cpp VdlEditor.cpp VdlLookAndFeel.cpp)
target_compile_definitions(VonDeLarsen PUBLIC
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0)
target_link_libraries(VonDeLarsen
    PRIVATE vc_core juce::juce_audio_utils
    PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)

# Developer tool: renders the editor to a PNG (see PreviewMain.cpp). Build only this target, it installs nothing.
option(VDL_BUILD_UI_PREVIEW "Console tool that renders the editor to a PNG" OFF)
if(VDL_BUILD_UI_PREVIEW)
    juce_add_console_app(VonDeLarsenPreview PRODUCT_NAME "VDL Preview")
    target_sources(VonDeLarsenPreview PRIVATE PreviewMain.cpp PluginProcessor.cpp VdlEditor.cpp VdlLookAndFeel.cpp)
    target_compile_definitions(VonDeLarsenPreview PUBLIC
        JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0
        JUCE_MODAL_LOOPS_PERMITTED=1) # the high cpu self-test pumps the message loop
    target_link_libraries(VonDeLarsenPreview
        PRIVATE vc_core juce::juce_audio_utils
        PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)
endif()
