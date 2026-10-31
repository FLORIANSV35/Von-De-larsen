// "Poste de pilotage" editor. All drawing happens in a fixed 720x660 design space that is scaled to the window.
#pragma once
#include <memory>

#include <juce_audio_processors/juce_audio_processors.h>

#include "PluginProcessor.h"
#include "VdlLookAndFeel.h"

class VdlEditor : public juce::AudioProcessorEditor {
public:
    static constexpr int kW = 720, kH = 810;

    explicit VdlEditor(VonDeLarsenProcessor&);
    ~VdlEditor() override;

    void paint(juce::Graphics&) override;
    void resized() override;
    void showHelp(bool show); // the "?" notice

private:
    class Content;

    vdl::LookAndFeel laf_;
    juce::TooltipWindow tooltip_ { this, 700 };
    std::unique_ptr<Content> content_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(VdlEditor)
};
