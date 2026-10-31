juce_add_gui_app(VonDeLarsenApp
    PRODUCT_NAME "Von De Larsen"
    COMPANY_NAME "VonDeLarsen"
    ICON_BIG "${CMAKE_CURRENT_SOURCE_DIR}/Icon.png"   # the rocket, drawn by plugin/PreviewMain.cpp (VonDeLarsenPreview out.png icon)
    ICON_SMALL "${CMAKE_CURRENT_SOURCE_DIR}/Icon.png"
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT "Von De Larsen processes your microphone inputs in real time.")

# Same engine and editor as the plugin (shared sources live in ../plugin).
target_sources(VonDeLarsenApp PRIVATE Main.cpp
    ../plugin/PluginProcessor.cpp ../plugin/VdlEditor.cpp ../plugin/VdlLookAndFeel.cpp)
target_include_directories(VonDeLarsenApp PRIVATE ../plugin)
target_compile_definitions(VonDeLarsenApp PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(VonDeLarsenApp
    PRIVATE vc_core juce::juce_audio_utils juce::juce_audio_devices juce::juce_gui_basics
    PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)
