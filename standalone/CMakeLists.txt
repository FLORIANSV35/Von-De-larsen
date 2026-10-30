juce_add_gui_app(VonDeLarsenApp
    PRODUCT_NAME "Von De Larsen"
    COMPANY_NAME "VonDeLarsen"
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT "Von De Larsen processes your microphone inputs in real time.")

target_sources(VonDeLarsenApp PRIVATE Main.cpp)
target_compile_definitions(VonDeLarsenApp PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(VonDeLarsenApp
    PRIVATE vc_core juce::juce_audio_utils juce::juce_audio_devices juce::juce_gui_basics
    PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)
