// Multichannel standalone: N microphone inputs (1..8) -> N processed outputs.
// NOTE: written without access to a Mac/JUCE; needs a first build on macOS.
#include <array>
#include <atomic>
#include <memory>

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "vc/CoreMlMaskProvider.h"
#include "vc/FeedbackSuppressor.h"
#include "vc/SpectralDenoiser.h"

namespace {
constexpr int kMaxChannels = 8;

juce::File modelFile()
{
    return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
        .getChildFile("VonDeLarsen/denoise.mlmodelc");
}

struct Channel {
    vc::FeedbackSuppressor feedback;
    vc::SpectralDenoiser denoiser;
    std::unique_ptr<vc::AsyncMaskProvider> mask;
};
} // namespace

class MainComponent : public juce::Component, private juce::AudioIODeviceCallback, private juce::Timer {
public:
    MainComponent()
    {
        addAndMakeVisible(channelsLabel_);
        channelsLabel_.setText("Channels", juce::dontSendNotification);
        addAndMakeVisible(channels_);
        for (int i = 1; i <= kMaxChannels; ++i) channels_.addItem(juce::String(i), i);
        channels_.onChange = [this] { setActiveChannels(channels_.getSelectedId()); };

        setupSlider(sens_, sensLabel_, "Feedback sensitivity", 0.0, 1.0, 0.5);
        setupSlider(cut_, cutLabel_, "Feedback max cut (dB)", 6.0, 30.0, 18.0);
        setupSlider(nr_, nrLabel_, "Noise reduction (dB)", 0.0, 30.0, 15.0);
        addAndMakeVisible(neural_);
        neural_.setButtonText("Neural Engine mask (needs model)");
        neural_.setToggleState(true, juce::dontSendNotification);
        neural_.onClick = [this] { pNeural_.store(neural_.getToggleState()); };

        addAndMakeVisible(status_);
        status_.setJustificationType(juce::Justification::topLeft);

        // 8 in / 8 out is the maximum; the selector below lets the user pick any subset.
        deviceManager_.initialise(kMaxChannels, kMaxChannels, nullptr, true);
        selector_ = std::make_unique<juce::AudioDeviceSelectorComponent>(
            deviceManager_, 0, kMaxChannels, 0, kMaxChannels, false, false, false, false);
        addAndMakeVisible(*selector_);

        channels_.setSelectedId(2, juce::dontSendNotification);
        setActiveChannels(2);
        deviceManager_.addAudioCallback(this);
        setSize(560, 640);
        startTimerHz(4);
    }

    ~MainComponent() override
    {
        deviceManager_.removeAudioCallback(this);
        for (auto& c : ch_) {
            c.denoiser.setMaskProvider(nullptr);
            if (c.mask) c.mask->stop();
        }
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced(16);
        auto row = r.removeFromTop(28);
        channelsLabel_.setBounds(row.removeFromLeft(160));
        channels_.setBounds(row.removeFromLeft(80));
        r.removeFromTop(8);
        for (auto* pair : { &sens_, &cut_, &nr_ }) {
            auto s = r.removeFromTop(28);
            (pair == &sens_ ? sensLabel_ : pair == &cut_ ? cutLabel_ : nrLabel_).setBounds(s.removeFromLeft(180));
            pair->setBounds(s);
        }
        neural_.setBounds(r.removeFromTop(28));
        status_.setBounds(r.removeFromTop(90));
        selector_->setBounds(r);
    }

private:
    void setupSlider(juce::Slider& s, juce::Label& l, const juce::String& name, double lo, double hi, double v)
    {
        s.setRange(lo, hi);
        s.setValue(v);
        s.onValueChange = [this] { // mirror UI values into atomics: the audio thread never touches JUCE widgets
            pSens_.store(static_cast<float>(sens_.getValue()));
            pCut_.store(static_cast<float>(cut_.getValue()));
            pNr_.store(static_cast<float>(nr_.getValue()));
        };
        l.setText(name, juce::dontSendNotification);
        addAndMakeVisible(s);
        addAndMakeVisible(l);
    }

    // Message thread. New channels get their model attached *before* they become
    // active, so the audio thread never sees a half-configured channel.
    void setActiveChannels(int n)
    {
        const auto f = modelFile();
        if (f.exists()) {
            for (int c = 0; c < n; ++c) {
                auto& ch = ch_[static_cast<size_t>(c)];
                if (ch.mask) continue;
                std::string err;
                ch.mask = vc::makeCoreMlMaskProvider(f.getFullPathName().toStdString(), vc::SpectralDenoiser::kBins, err);
                if (ch.mask) ch.denoiser.setMaskProvider(ch.mask.get());
                else DBG("Neural model not loaded: " << err);
            }
        }
        active_.store(n);
    }

    void audioDeviceAboutToStart(juce::AudioIODevice* d) override
    {
        const double sr = d->getCurrentSampleRate();
        for (auto& c : ch_) {
            c.feedback.prepare(sr);
            c.denoiser.prepare(sr);
        }
        lastActive_ = 0;
    }
    void audioDeviceStopped() override {}

    void audioDeviceIOCallbackWithContext(const float* const* in, int numIn, float* const* out, int numOut,
                                          int numSamples, const juce::AudioIODeviceCallbackContext&) override
    {
        const int n = active_.load();
        if (n > lastActive_) // channels just switched on: start from a clean state
            for (int c = lastActive_; c < n; ++c) {
                ch_[static_cast<size_t>(c)].feedback.reset();
                ch_[static_cast<size_t>(c)].denoiser.reset();
            }
        lastActive_ = n;

        const float sens = pSens_.load(), cut = pCut_.load(), nr = pNr_.load();
        const bool useNn = pNeural_.load();

        for (int c = 0; c < numOut; ++c) {
            float* o = out[c];
            if (c >= n || c >= numIn || in[c] == nullptr) {
                juce::FloatVectorOperations::clear(o, numSamples);
                continue;
            }
            auto& ch = ch_[static_cast<size_t>(c)];
            ch.feedback.setSensitivity(sens);
            ch.feedback.setMaxCutDb(cut);
            ch.denoiser.setReductionDb(nr);
            if (!useNn && ch.mask) ch.denoiser.setMaskProvider(nullptr);
            else if (useNn && ch.mask) ch.denoiser.setMaskProvider(ch.mask.get());
            juce::FloatVectorOperations::copy(o, in[c], numSamples);
            ch.feedback.process(o, numSamples);
            ch.denoiser.process(o, o, numSamples);
        }
    }

    void timerCallback() override
    {
        juce::String s;
        if (auto* d = deviceManager_.getCurrentAudioDevice()) {
            const int dev = d->getInputLatencyInSamples() + d->getOutputLatencyInSamples();
            const int algo = ch_[0].denoiser.latencySamples();
            const double sr = d->getCurrentSampleRate();
            s << "Latency: plugin " << juce::String(1000.0 * algo / sr, 1) << " ms + device "
              << juce::String(1000.0 * dev / sr, 1) << " ms = " << juce::String(1000.0 * (algo + dev) / sr, 1) << " ms\n";
        } else {
            s << "No audio device\n";
        }
        s << "Notches per channel: ";
        for (int c = 0; c < active_.load(); ++c) s << ch_[static_cast<size_t>(c)].feedback.activeNotches() << (c + 1 < active_.load() ? " / " : "");
        s << "\nNeural masks: ";
        for (int c = 0; c < active_.load(); ++c) s << (ch_[static_cast<size_t>(c)].denoiser.usingNeuralMask() ? "on" : "off") << (c + 1 < active_.load() ? " / " : "");
        status_.setText(s, juce::dontSendNotification);
    }

    juce::AudioDeviceManager deviceManager_;
    std::unique_ptr<juce::AudioDeviceSelectorComponent> selector_;
    juce::Label channelsLabel_, sensLabel_, cutLabel_, nrLabel_, status_;
    juce::ComboBox channels_;
    juce::Slider sens_, cut_, nr_;
    juce::ToggleButton neural_;

    std::array<Channel, kMaxChannels> ch_;
    std::atomic<int> active_ { 2 };
    std::atomic<float> pSens_ { 0.5f }, pCut_ { 18.f }, pNr_ { 15.f };
    std::atomic<bool> pNeural_ { true };
    int lastActive_ = 0; // audio thread only
};

class App : public juce::JUCEApplication {
public:
    const juce::String getApplicationName() override { return "Von De Larsen"; }
    const juce::String getApplicationVersion() override { return "0.1.0"; }
    void initialise(const juce::String&) override { window_ = std::make_unique<Window>(); }
    void shutdown() override { window_.reset(); }

private:
    struct Window : juce::DocumentWindow {
        Window() : DocumentWindow("Von De Larsen", juce::Colours::darkgrey, DocumentWindow::allButtons)
        {
            setUsingNativeTitleBar(true);
            setContentOwned(new MainComponent(), true);
            centreWithSize(getWidth(), getHeight());
            setVisible(true);
        }
        void closeButtonPressed() override { juce::JUCEApplication::getInstance()->systemRequestedQuit(); }
    };
    std::unique_ptr<Window> window_;
};

START_JUCE_APPLICATION(App)
