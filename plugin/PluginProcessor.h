#pragma once
#include <array>
#include <memory>

#include <juce_audio_processors/juce_audio_processors.h>

#include "vc/CoreMlMaskProvider.h"
#include "vc/FeedbackSuppressor.h"
#include "vc/SpectralDenoiser.h"

class VonDeLarsenProcessor : public juce::AudioProcessor {
public:
    VonDeLarsenProcessor();
    ~VonDeLarsenProcessor() override;

    void prepareToPlay(double sampleRate, int maxBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported(const BusesLayout&) const override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override { return new juce::GenericAudioProcessorEditor(*this); }
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "Von De Larsen"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}

    void getStateInformation(juce::MemoryBlock&) override;
    void setStateInformation(const void*, int) override;

    juce::AudioProcessorValueTreeState apvts;

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
    void loadNeuralModel();

    static constexpr int kMaxChannels = 2;
    struct Channel {
        vc::FeedbackSuppressor feedback;
        vc::SpectralDenoiser denoiser;
        std::unique_ptr<vc::AsyncMaskProvider> mask; // owned here, attached to `denoiser`
    };
    std::array<Channel, kMaxChannels> ch_;

    std::atomic<float>* sens_ = nullptr;
    std::atomic<float>* maxCut_ = nullptr;
    std::atomic<float>* nr_ = nullptr;
    std::atomic<float>* nn_ = nullptr;
    std::atomic<float>* mix_ = nullptr;
    double fs_ = 48000;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(VonDeLarsenProcessor)
};
