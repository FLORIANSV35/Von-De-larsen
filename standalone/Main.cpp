// Standalone: the same engine and the same "Poste de pilotage" editor as the plugin, on 1 to 8 channels.
// The audio callback feeds a VonDeLarsenProcessor directly, so no plugin host is involved.
#include <atomic>
#include <memory>

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "PluginProcessor.h"
#include "VdlEditor.h"

namespace {
constexpr int kMaxChannels = VonDeLarsenProcessor::kMaxChannels;
const juce::Colour kBackground { 0xff0a0a0d };

juce::File settingsFile()
{
    return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory).getChildFile("VonDeLarsen/standalone.xml");
}
} // namespace

// Moves audio between the device and the processor.
class Engine : public juce::AudioIODeviceCallback {
public:
    explicit Engine(VonDeLarsenProcessor& p) : proc_(p) {}

    double deviceLatencyMs() const { return deviceLatencyMs_.load(); }

    void audioDeviceAboutToStart(juce::AudioIODevice* d) override
    {
        const double sr = d->getCurrentSampleRate();
        const int bs = d->getCurrentBufferSizeSamples();
        scratch_.setSize(kMaxChannels, juce::jmax(bs, 4096)); // allocated here, never on the audio thread
        scratch_.clear();
        // No phantom channels: a microphone with one input is one channel, not two.
        const int inputs = d->getActiveInputChannels().countNumberOfSetBits();
        if (inputs >= 1 && inputs < proc_.activeChannels()) proc_.setActiveChannels(inputs);
        proc_.setRateAndBufferSizeDetails(sr, bs);
        proc_.prepareToPlay(sr, bs);
        deviceLatencyMs_.store(1000.0 * (d->getInputLatencyInSamples() + d->getOutputLatencyInSamples()) / sr);
        ready_.store(true);
    }

    void audioDeviceStopped() override { ready_.store(false); }

    void audioDeviceIOCallbackWithContext(const float* const* in, int numIn, float* const* out, int numOut, int n,
                                          const juce::AudioIODeviceCallbackContext&) override
    {
        const int ch = proc_.activeChannels();
        if (!ready_.load() || n > scratch_.getNumSamples()) {
            for (int c = 0; c < numOut; ++c) juce::FloatVectorOperations::clear(out[c], n);
            return;
        }
        for (int c = 0; c < ch; ++c) {
            if (c < numIn && in[c] != nullptr) scratch_.copyFrom(c, 0, in[c], n);
            else scratch_.clear(c, 0, n);
        }
        juce::AudioBuffer<float> view(scratch_.getArrayOfWritePointers(), ch, n);
        proc_.processBlock(view, midi_);
        for (int c = 0; c < numOut; ++c) {
            if (c < ch) juce::FloatVectorOperations::copy(out[c], scratch_.getReadPointer(c), n);
            else if (ch == 1) juce::FloatVectorOperations::copy(out[c], scratch_.getReadPointer(0), n); // one microphone feeds every speaker
            else juce::FloatVectorOperations::clear(out[c], n);
        }
    }

private:
    VonDeLarsenProcessor& proc_;
    juce::AudioBuffer<float> scratch_;
    juce::MidiBuffer midi_;
    std::atomic<bool> ready_ { false };
    std::atomic<double> deviceLatencyMs_ { 0.0 };
};

class App : public juce::JUCEApplication {
public:
    const juce::String getApplicationName() override { return "Von De Larsen"; }
    const juce::String getApplicationVersion() override { return "0.1.0"; }

    void initialise(const juce::String&) override
    {
        processor_ = std::make_unique<VonDeLarsenProcessor>();

        std::unique_ptr<juce::XmlElement> saved = juce::XmlDocument::parse(settingsFile());
        if (saved != nullptr) {
            if (auto* st = saved->getChildByName("State")) {
                juce::MemoryBlock mb;
                if (mb.fromBase64Encoding(st->getAllSubText())) processor_->setStateInformation(mb.getData(), static_cast<int>(mb.getSize()));
            }
            if (auto* ch = saved->getChildByName("Channels")) processor_->setActiveChannels(ch->getIntAttribute("n", 2));
        }

        if (saved == nullptr) // first launch: several microphones, so share the gain between them
            if (auto* a = processor_->apvts.getParameter("autoMixOn")) a->setValueNotifyingHost(1.f);

        engine_ = std::make_unique<Engine>(*processor_);
        processor_->hostHooks.setChannels = [this](int n) {
            deviceManager_.removeAudioCallback(engine_.get()); // stops, then restarts with the new channel count
            processor_->setActiveChannels(n);
            deviceManager_.addAudioCallback(engine_.get());
        };
        processor_->hostHooks.openAudioSettings = [this] { showAudioSettings(); };
        processor_->hostHooks.deviceLatencyMs = [this] { return engine_->deviceLatencyMs(); };

        const juce::XmlElement* deviceXml = saved != nullptr ? saved->getChildByName("Device") : nullptr;
        deviceManager_.initialise(kMaxChannels, kMaxChannels, deviceXml, true);
        deviceManager_.addAudioCallback(engine_.get());
        window_ = std::make_unique<Window>(*processor_);
    }

    void shutdown() override
    {
        window_.reset();
        deviceManager_.removeAudioCallback(engine_.get());

        juce::XmlElement root("VDL");
        juce::MemoryBlock mb;
        processor_->getStateInformation(mb);
        root.createNewChildElement("State")->addTextElement(mb.toBase64Encoding());
        root.createNewChildElement("Channels")->setAttribute("n", processor_->activeChannels());
        if (auto dev = deviceManager_.createStateXml()) root.addChildElement(dev.release());
        const auto f = settingsFile();
        f.getParentDirectory().createDirectory();
        root.writeTo(f);

        deviceManager_.closeAudioDevice();
        engine_.reset();
        processor_.reset();
    }

private:
    struct Window : juce::DocumentWindow {
        explicit Window(VonDeLarsenProcessor& p) : DocumentWindow("Von De Larsen", kBackground, DocumentWindow::allButtons)
        {
            setUsingNativeTitleBar(true);
            auto* editor = p.createEditor();
            editor->setResizable(false, false); // the window resizes, not the editor
            float scale = 1.f; // fit the screen: the editor is tall, and the title bar and the Dock take room
            if (const auto* display = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay())
                scale = juce::jmin(1.f, static_cast<float>(display->userArea.getHeight() - 90) / static_cast<float>(VdlEditor::kH));
            editor->setSize(juce::roundToInt(VdlEditor::kW * scale), juce::roundToInt(VdlEditor::kH * scale));
            setContentOwned(editor, true);
            setResizable(true, true);
            setResizeLimits(540, 520, 1440, 1320);
            centreWithSize(getWidth(), getHeight());
            setVisible(true);
        }
        void closeButtonPressed() override { juce::JUCEApplication::getInstance()->systemRequestedQuit(); }
    };

    void showAudioSettings()
    {
        auto* sel = new juce::AudioDeviceSelectorComponent(deviceManager_, 0, kMaxChannels, 0, kMaxChannels, false, false, false, false);
        sel->setSize(520, 420);
        juce::DialogWindow::LaunchOptions o;
        o.content.setOwned(sel);
        o.dialogTitle = juce::String::fromUTF8("R\xc3\xa9glages audio");
        o.dialogBackgroundColour = kBackground;
        o.escapeKeyTriggersCloseButton = true;
        o.useNativeTitleBar = true;
        o.resizable = false;
        o.launchAsync();
    }

    juce::AudioDeviceManager deviceManager_;
    std::unique_ptr<VonDeLarsenProcessor> processor_;
    std::unique_ptr<Engine> engine_;
    std::unique_ptr<Window> window_;
};

START_JUCE_APPLICATION(App)
