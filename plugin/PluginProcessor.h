#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>

#include <juce_audio_processors/juce_audio_processors.h>

#include "vc/AutoMixer.h"
#include "vc/BypassMixer.h"
#include "vc/CoreMlMaskProvider.h"
#include "vc/RingOut.h"
#include "vc/FeedbackSuppressor.h"
#include "vc/SpectralDenoiser.h"

class VonDeLarsenProcessor : public juce::AudioProcessor, private juce::AsyncUpdater {
public:
    VonDeLarsenProcessor();
    ~VonDeLarsenProcessor() override;

    void prepareToPlay(double sampleRate, int maxBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported(const BusesLayout&) const override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    // The host's own bypass switch drives our "bypass" parameter.
    juce::AudioProcessorParameter* getBypassParameter() const override { return apvts.getParameter("bypass"); }
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

    // Flight log shown in the editor. Totals are saved with the project.
    struct Telemetry {
        unsigned events = 0;          // howls caught (busiest channel)
        double avoidedSeconds = 0;    // time a notch was holding a howl
        double flightSeconds = 0;     // time spent processing audio
        float altitudeM = 0;          // "altitude" right now: kMetresPerDb per dB of howl cut being applied
        float recordM = 0;            // highest altitude reached
        float lastFreqHz = 0;         // frequency of the latest catch
        bool alert = false;           // a howl is being held right now
        bool gateOn = false;          // the voice gate is enabled
        bool voice = false;           // voice detected on at least one channel (gate open)
        float guardDb = 0;            // broadband cut the runaway guard applies right now (the loop gain is too high)
        bool bypassed = false;        // processing is bypassed (the sound goes through untouched)
        // room learning
        bool learning = false;        // a learning run is under way
        int learnPhase = 0;           // 0 idle, 1 climbing, 2 settling on a howl, 3 returning to normal
        float cpuLoad = 0;            // share of the audio time the plugin uses, 0..1
        bool learnWaiting = false;    // the climb waits for someone to speak
        float learnGainDb = 0;        // extra output gain applied by the climb
        int learnHowls = 0;           // howls found during this run
        int learnResult = 0;          // 0 none yet, 1 learned, 2 calm room, 3 aborted (too loud), 4 timed out
        float learnMarginDb = 0;      // gain margin the room was tested to, past the first howl
        int learnedCount = 0;         // permanent notches kept (busiest channel)
        struct NotchView { float hz = 0, cutDb = 0; bool fixed = false; };
        std::array<NotchView, vc::FeedbackSuppressor::kMaxNotches> notches {}; // channel 1, for the frequency strip
        int notchViews = 0;
        bool autoMixOn = false;       // the automixer is enabled and there are at least two channels
        int channels = 0;             // channels in use
        std::array<float, vc::AutoMixer::kMaxChannels> autoGainDb {}; // automixer gain per channel (dB)
    };
    // Engine capacity. The plugin buses are mono/stereo; the standalone drives up to 8 channels directly.
    static constexpr int kMaxChannels = 8;
    int activeChannels() const { return active_.load(std::memory_order_relaxed); }
    // Call from the message thread while audio is stopped, then prepareToPlay() again.
    void setActiveChannels(int n) { active_.store(juce::jlimit(1, kMaxChannels, n), std::memory_order_relaxed); }

    // Optional hooks the standalone app fills in; the editor shows the matching controls when they are set.
    struct HostHooks {
        std::function<void()> openAudioSettings;
        std::function<void(int)> setChannels;
        std::function<double()> deviceLatencyMs;
    };
    HostHooks hostHooks;

    static constexpr float kMetresPerDb = 50.f;
    static constexpr double kKmPerSecond = 11.2;     // escape velocity
    static constexpr double kMoonKm = 384400.0;
    Telemetry telemetry() const;
    void resetTelemetry();

    // Room learning. Someone speaks into the microphone while the output gain climbs slowly, only while they speak.
    // Every howl that appears becomes a permanent notch, saved with the project. Message thread.
    void startLearn();
    void stopLearn();
    void clearLearned();
    void setLearnedNotches(int channel, const vc::FeedbackSuppressor::Notch* notches, int count);

    juce::AudioProcessorValueTreeState apvts;

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
    void loadNeuralModel(int channels);

    struct Channel {
        vc::FeedbackSuppressor feedback;
        vc::SpectralDenoiser denoiser;
        std::unique_ptr<vc::AsyncMaskProvider> mask;   // owned here, attached to `denoiser`
    };
    std::array<Channel, kMaxChannels> ch_;

    std::atomic<float>* sens_ = nullptr;
    std::atomic<float>* maxCut_ = nullptr;
    std::atomic<float>* nr_ = nullptr;
    std::atomic<float>* nn_ = nullptr;
    std::atomic<float>* mix_ = nullptr;
    std::atomic<float>* gate_ = nullptr;
    std::atomic<float>* bypass_ = nullptr;
    std::atomic<float> cpuLoad_ { 0.f }; // share of the audio time the plugin uses, smoothed
    vc::RingOut ring_;
    std::atomic<int> learnCmd_ { 0 }, learnPhase_ { 0 }, learnResult_ { 0 }, learnHowls_ { 0 };
    std::atomic<bool> learnWaiting_ { false };
    std::atomic<float> learnGain_ { 0.f }, learnMargin_ { 0.f };
    float learnGainApplied_ = 1.f; // audio thread
    vc::BypassMixer bypassMix_;
    std::atomic<float>* autoOn_ = nullptr;
    std::atomic<float>* autoDepth_ = nullptr;
    vc::AutoMixer mixer_;
    std::atomic<float>* gateDepth_ = nullptr;
    std::atomic<float>* highCpu_ = nullptr;
    bool lowLatency_ = false; // audio thread: the mode the denoisers are in
    std::atomic<int> latencyNow_ { vc::SpectralDenoiser::kN };
    void handleAsyncUpdate() override { setLatencySamples(latencyNow_.load()); } // message thread
    double fs_ = 48000;

    std::atomic<int> active_ { 2 };   // channels the host wants processed
    std::atomic<int> prepared_ { 0 }; // channels prepared by the last prepareToPlay()
    std::atomic<std::uint64_t> flightSamples_ { 0 };
    // Totals restored from the project state; only touched on the message thread.
    unsigned baseEvents_ = 0;
    double baseAvoided_ = 0, baseFlight_ = 0;
    float baseRecordM_ = 0, baseLastHz_ = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(VonDeLarsenProcessor)
};
