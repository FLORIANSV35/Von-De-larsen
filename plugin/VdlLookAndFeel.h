// Look of the "Poste de pilotage" editor: black panel, red LEDs, radar-glass knobs.
#pragma once
#include <juce_gui_basics/juce_gui_basics.h>

namespace vdl {

class LookAndFeel : public juce::LookAndFeel_V4 {
public:
    LookAndFeel();

    // Slider property "kind": 0 = big rocket dial, 1 = small dial. Both draw a value box under the dial.
    void drawRotarySlider(juce::Graphics&, int x, int y, int w, int h, float pos, float a0, float a1, juce::Slider&) override;
    void drawLinearSlider(juce::Graphics&, int x, int y, int w, int h, float sliderPos, float minPos, float maxPos,
                          juce::Slider::SliderStyle, juce::Slider&) override;
    int getSliderThumbRadius(juce::Slider& s) override { return static_cast<int>(s.getProperties()["kind"]) == 2 ? 7 : 11; }

    void drawToggleButton(juce::Graphics&, juce::ToggleButton&, bool highlighted, bool down) override;

    // Buttons: component ID "help" = round "?" button, anything else = small outlined text button.
    void drawButtonBackground(juce::Graphics&, juce::Button&, const juce::Colour&, bool highlighted, bool down) override;
    void drawButtonText(juce::Graphics&, juce::TextButton&, bool highlighted, bool down) override;
};

} // namespace vdl
