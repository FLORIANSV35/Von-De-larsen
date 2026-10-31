// NOTE: written without access to a Mac/JUCE; needs a first build on macOS.
#include "PluginProcessor.h"

#include "VdlEditor.h"

namespace {
juce::File modelFile()
{
    return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
        .getChildFile("VonDeLarsen/denoise.mlmodelc");
}

} // namespace

VonDeLarsenProcessor::VonDeLarsenProcessor()
    : AudioProcessor(BusesProperties()
                         .withInput("Input", juce::AudioChannelSet::stereo(), true)
                         .withOutput("Output", juce::AudioChannelSet::stereo(), true))
    , apvts(*this, nullptr, "STATE", createLayout())
{
    sens_ = apvts.getRawParameterValue("fbSens");
    maxCut_ = apvts.getRawParameterValue("fbCut");
    nr_ = apvts.getRawParameterValue("nrAmount");
    nn_ = apvts.getRawParameterValue("nnEnable");
    mix_ = apvts.getRawParameterValue("nnMix");
    gate_ = apvts.getRawParameterValue("gateOn");
    bypass_ = apvts.getRawParameterValue("bypass");
    autoOn_ = apvts.getRawParameterValue("autoMixOn");
    autoDepth_ = apvts.getRawParameterValue("autoMixDepth");
    gateDepth_ = apvts.getRawParameterValue("gateDepth");
    highCpu_ = apvts.getRawParameterValue("highCpu");
}

VonDeLarsenProcessor::~VonDeLarsenProcessor()
{
    cancelPendingUpdate();
    for (auto& c : ch_) {
        c.denoiser.setMaskProvider(nullptr);
        if (c.mask) c.mask->stop();
    }
}

juce::AudioProcessorValueTreeState::ParameterLayout VonDeLarsenProcessor::createLayout()
{
    using namespace juce;
    AudioProcessorValueTreeState::ParameterLayout l;
    l.add(std::make_unique<AudioParameterFloat>(ParameterID { "fbSens", 1 }, "Feedback sensitivity",
                                                NormalisableRange<float>(0.f, 1.f), 0.5f));
    l.add(std::make_unique<AudioParameterFloat>(ParameterID { "fbCut", 1 }, "Feedback max cut (dB)",
                                                NormalisableRange<float>(6.f, 40.f), 24.f));
    l.add(std::make_unique<AudioParameterFloat>(ParameterID { "nrAmount", 1 }, "Noise reduction (dB)",
                                                NormalisableRange<float>(0.f, 30.f), 15.f));
    l.add(std::make_unique<AudioParameterBool>(ParameterID { "nnEnable", 1 }, "Neural Engine mask", true));
    l.add(std::make_unique<AudioParameterFloat>(ParameterID { "nnMix", 1 }, "Neural share (0 = classic only)",
                                                NormalisableRange<float>(0.f, 1.f), 0.7f));
    l.add(std::make_unique<AudioParameterBool>(ParameterID { "gateOn", 1 }, "Voice gate", true));
    l.add(std::make_unique<AudioParameterFloat>(ParameterID { "gateDepth", 1 }, "Voice gate depth (dB)",
                                                NormalisableRange<float>(0.f, 40.f, 1.f), 12.f));
    l.add(std::make_unique<AudioParameterBool>(ParameterID { "bypass", 1 }, "Bypass", false));
    l.add(std::make_unique<AudioParameterBool>(ParameterID { "highCpu", 1 }, "High CPU (low latency)", false));
    l.add(std::make_unique<AudioParameterBool>(ParameterID { "autoMixOn", 1 }, "Automixer", false));
    l.add(std::make_unique<AudioParameterFloat>(ParameterID { "autoMixDepth", 1 }, "Automixer silent mic attenuation (dB)",
                                                NormalisableRange<float>(6.f, 40.f, 1.f), 20.f));
    return l;
}

void VonDeLarsenProcessor::loadNeuralModel(int channels)
{
    const auto f = modelFile();
    if (!f.exists()) return; // no model installed: the classic DSP path is used
    for (int i = 0; i < channels; ++i) {
        auto& c = ch_[static_cast<size_t>(i)];
        if (c.mask) continue;
        std::string err;
        c.mask = vc::makeCoreMlMaskProvider(f.getFullPathName().toStdString(), vc::SpectralDenoiser::kBins, err);
        if (!c.mask) DBG("Neural model not loaded: " << err);
    }
}

void VonDeLarsenProcessor::prepareToPlay(double sr, int)
{
    fs_ = sr;
    const int n = active_.load();
    loadNeuralModel(n);
    lowLatency_ = highCpu_->load() > 0.5f;
    for (int i = 0; i < n; ++i) {
        auto& c = ch_[static_cast<size_t>(i)];
        c.feedback.prepare(sr);
        c.denoiser.setLowLatency(lowLatency_);
        c.denoiser.prepare(sr);
    }
    mixer_.prepare(sr);
    // bypassed sound is delayed like processed sound; the buffer is sized for the longest latency
    bypassMix_.prepare(sr, vc::SpectralDenoiser::kN);
    bypassMix_.setLatency(ch_[0].denoiser.latencySamples());
    prepared_.store(n);
    latencyNow_.store(ch_[0].denoiser.latencySamples());
    setLatencySamples(latencyNow_.load()); // the feedback notches add none
}

juce::AudioProcessorEditor* VonDeLarsenProcessor::createEditor() { return new VdlEditor(*this); }

VonDeLarsenProcessor::Telemetry VonDeLarsenProcessor::telemetry() const
{
    unsigned ev = 0;
    double avoided = 0;
    float cut = 0, peak = 0, last = 0, guard = 0;
    bool voice = false;
    for (int i = 0; i < active_.load(); ++i) {
        voice = voice || ch_[static_cast<size_t>(i)].denoiser.voiceDetected();
        const auto s = ch_[static_cast<size_t>(i)].feedback.stats();
        ev = juce::jmax(ev, s.events);
        avoided = juce::jmax(avoided, s.avoidedSeconds);
        cut = juce::jmax(cut, s.cutDb);
        peak = juce::jmax(peak, s.peakCutDb);
        guard = juce::jmax(guard, s.guardDb);
        if (last <= 0.f) last = s.lastFreqHz;
    }
    Telemetry t;
    t.events = baseEvents_ + ev;
    t.avoidedSeconds = baseAvoided_ + avoided;
    t.flightSeconds = baseFlight_ + static_cast<double>(flightSamples_.load(std::memory_order_relaxed)) / fs_;
    t.altitudeM = cut * kMetresPerDb;
    t.recordM = juce::jmax(baseRecordM_, peak * kMetresPerDb);
    t.lastFreqHz = last > 0.f ? last : baseLastHz_;
    t.alert = cut > 0.5f;
    t.guardDb = guard;
    t.gateOn = gate_->load() > 0.5f;
    t.voice = t.gateOn && voice;
    t.channels = active_.load();
    t.bypassed = bypass_->load() > 0.5f;
    t.cpuLoad = cpuLoad_.load();
    t.learnPhase = learnPhase_.load();
    t.learning = t.learnPhase != 0;
    t.learnWaiting = learnWaiting_.load();
    t.learnGainDb = learnGain_.load();
    t.learnHowls = learnHowls_.load();
    t.learnResult = learnResult_.load();
    t.learnMarginDb = learnMargin_.load();
    for (int i = 0; i < active_.load(); ++i) t.learnedCount = juce::jmax(t.learnedCount, ch_[static_cast<size_t>(i)].feedback.learnedCount());
    {
        vc::FeedbackSuppressor::SlotView views[vc::FeedbackSuppressor::kMaxNotches];
        const int nv = ch_[0].feedback.slotViews(views, vc::FeedbackSuppressor::kMaxNotches);
        for (int i = 0; i < nv; ++i) t.notches[static_cast<size_t>(i)] = { views[i].hz, views[i].cutDb, views[i].fixed };
        t.notchViews = nv;
    }
    t.autoMixOn = autoOn_->load() > 0.5f && t.channels >= 2;
    for (int i = 0; i < t.channels; ++i) t.autoGainDb[static_cast<size_t>(i)] = mixer_.gainDb(i);
    return t;
}

void VonDeLarsenProcessor::resetTelemetry()
{
    baseEvents_ = 0;
    baseAvoided_ = baseFlight_ = 0;
    baseRecordM_ = baseLastHz_ = 0;
    for (auto& c : ch_) c.feedback.resetStats();
    flightSamples_.store(0, std::memory_order_relaxed);
}

void VonDeLarsenProcessor::startLearn() { learnCmd_.store(1); }
void VonDeLarsenProcessor::stopLearn() { learnCmd_.store(2); }

void VonDeLarsenProcessor::clearLearned()
{
    for (auto& c : ch_) c.feedback.clearLearned();
    learnResult_.store(0);
}

void VonDeLarsenProcessor::setLearnedNotches(int channel, const vc::FeedbackSuppressor::Notch* notches, int count)
{
    if (channel >= 0 && channel < kMaxChannels) ch_[static_cast<size_t>(channel)].feedback.setLearnedNotches(notches, count);
}

bool VonDeLarsenProcessor::isBusesLayoutSupported(const BusesLayout& l) const
{
    const auto in = l.getMainInputChannelSet();
    return in == l.getMainOutputChannelSet() && (in == juce::AudioChannelSet::mono() || in == juce::AudioChannelSet::stereo());
}

void VonDeLarsenProcessor::processBlock(juce::AudioBuffer<float>& buf, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    const auto cpuStart = juce::Time::getHighResolutionTicks();
    const bool useNn = nn_->load() > 0.5f;
    const int n = buf.getNumSamples();
    const int nCh = juce::jmin(buf.getNumChannels(), prepared_.load(std::memory_order_relaxed));
    flightSamples_.fetch_add(static_cast<std::uint64_t>(n), std::memory_order_relaxed);

    // High CPU: a shorter analysis window halves the latency. Switching resets the denoisers (no allocation) and
    // tells the host the new latency from the message thread.
    if ((highCpu_->load() > 0.5f) != lowLatency_) {
        lowLatency_ = !lowLatency_;
        for (int c = 0; c < nCh; ++c) ch_[static_cast<size_t>(c)].denoiser.setLowLatency(lowLatency_);
        if (nCh > 0) {
            bypassMix_.setLatency(ch_[0].denoiser.latencySamples());
            latencyNow_.store(ch_[0].denoiser.latencySamples());
            triggerAsyncUpdate();
        }
    }

    const float* dry[vc::BypassMixer::kMaxChannels];
    for (int c = 0; c < nCh; ++c) dry[c] = buf.getReadPointer(c);
    bypassMix_.push(dry, nCh, n); // keep the untouched input for the bypass

    // room learning: start or stop on request, and note the input level before the chain touches the buffer
    auto startRingOut = [this, nCh] {
        ring_.prepare(fs_);
        ring_.start(6.f, 18.f, 0.5f, 240.f);
        for (int c = 0; c < nCh; ++c) ch_[static_cast<size_t>(c)].feedback.setLearn(true);
        learnHowls_.store(0);
        learnPhase_.store(1);
    };
    const int learnCmd = learnCmd_.exchange(0);
    if (learnCmd == 1 && !ring_.active() && nCh > 0) {
        learnResult_.store(0);
        startRingOut();
    } else if (learnCmd == 2 && ring_.active()) {
        ring_.stop();
    }
    float inputDb = -120.f;
    if (ring_.active()) {
        double e = 0;
        for (int c = 0; c < nCh; ++c) {
            double ce = 0;
            for (int i = 0; i < n; ++i) ce += static_cast<double>(dry[c][i]) * dry[c][i];
            e = std::max(e, ce / juce::jmax(1, n));
        }
        inputDb = static_cast<float>(10.0 * std::log10(e + 1e-12));
    }

    for (int c = 0; c < nCh; ++c) {
        auto& ch = ch_[static_cast<size_t>(c)];
        ch.feedback.setSensitivity(sens_->load());
        ch.feedback.setMaxCutDb(maxCut_->load());
        ch.denoiser.setReductionDb(nr_->load());
        ch.denoiser.setMaskProvider(useNn ? ch.mask.get() : nullptr);
        ch.denoiser.setNeuralMix(mix_->load());
        ch.denoiser.setGate(gate_->load() > 0.5f, gateDepth_->load());

        float* x = buf.getWritePointer(c);
        ch.feedback.setVoiceHint(ch.denoiser.neuralMask()); // neural voice/noise hint steers the howl detector
        ch.feedback.process(x, n);            // howl first: it must see the raw mic signal
        ch.denoiser.process(x, x, n);
    }

    // Dugan-style gain sharing between microphones (needs two or more channels)
    float* chans[vc::AutoMixer::kMaxChannels];
    float scores[vc::AutoMixer::kMaxChannels];
    for (int c = 0; c < nCh; ++c) {
        chans[c] = buf.getWritePointer(c);
        scores[c] = ch_[static_cast<size_t>(c)].denoiser.voiceScore();
    }
    mixer_.setDepthDb(autoDepth_->load());
    mixer_.process(chans, scores, nCh, n, autoOn_->load() > 0.5f);

    if (ring_.active()) {
        std::uint32_t events = 0;
        bool voice = false;
        float peak = 0.f;
        for (int c = 0; c < nCh; ++c) {
            events += ch_[static_cast<size_t>(c)].feedback.stats().events;
            voice = voice || scores[c] > 0.5f;
            for (int i = 0; i < n; ++i) peak = std::max(peak, std::fabs(chans[c][i]));
        }
        if (bypass_->load() > 0.5f) ring_.stop();
        ring_.update(n, events, inputDb, peak * learnGainApplied_, voice);
        learnHowls_.store(ring_.howlsFound());
        learnWaiting_.store(ring_.waitingForVoice());
        learnPhase_.store(ring_.state() == vc::RingOut::State::Settling ? 2 : ring_.state() == vc::RingOut::State::Finishing ? 3 : 1);
        if (ring_.finished()) {
            for (int c = 0; c < nCh; ++c) ch_[static_cast<size_t>(c)].feedback.setLearn(false);
            learnMargin_.store(ring_.marginReachedDb());
            learnResult_.store(ring_.aborted() ? 3 : ring_.timedOut() ? 4 : ring_.howlsFound() == 0 ? 2 : 1);
            learnPhase_.store(0);
            learnWaiting_.store(false);
        }
    }
    {   // the climb: more gain on what goes to the loudspeakers, ramped across the block
        const float target = ring_.active() || learnGainApplied_ != 1.f ? std::pow(10.0f, ring_.gainDb() / 20.0f) : 1.f;
        learnGain_.store(ring_.gainDb());
        if (target != 1.f || learnGainApplied_ != 1.f) {
            for (int c = 0; c < nCh; ++c)
                for (int i = 0; i < n; ++i) chans[c][i] *= learnGainApplied_ + (target - learnGainApplied_) * static_cast<float>(i + 1) / static_cast<float>(n);
            learnGainApplied_ = target;
        }
    }
    bypassMix_.mix(chans, nCh, n, bypass_->load() > 0.5f);

    const double used = juce::Time::highResolutionTicksToSeconds(juce::Time::getHighResolutionTicks() - cpuStart);
    cpuLoad_.store(0.9f * cpuLoad_.load() + 0.1f * static_cast<float>(used / (static_cast<double>(n) / fs_)));
}

void VonDeLarsenProcessor::getStateInformation(juce::MemoryBlock& dest)
{
    const auto t = telemetry();
    apvts.state.setProperty("tmEvents", static_cast<int>(t.events), nullptr);
    apvts.state.setProperty("tmAvoided", t.avoidedSeconds, nullptr);
    apvts.state.setProperty("tmFlight", t.flightSeconds, nullptr);
    apvts.state.setProperty("tmRecord", static_cast<double>(t.recordM), nullptr);
    apvts.state.setProperty("tmLastHz", static_cast<double>(t.lastFreqHz), nullptr);
    for (int i = 0; i < kMaxChannels; ++i) { // learned notches, one list per channel: "freq:cut:q;freq:cut:q"
        juce::String list;
        const auto& fb = ch_[static_cast<size_t>(i)].feedback;
        for (int k = 0; k < fb.learnedCount(); ++k) {
            const auto nt = fb.learnedNotch(k);
            list << juce::String(nt.freq, 1) << ":" << juce::String(nt.cutDb, 1) << ":" << juce::String(nt.q, 2) << ";";
        }
        apvts.state.setProperty("learned" + juce::String(i), list, nullptr);
    }
    if (auto xml = apvts.copyState().createXml()) copyXmlToBinary(*xml, dest);
}

void VonDeLarsenProcessor::setStateInformation(const void* data, int size)
{
    if (auto xml = getXmlFromBinary(data, size))
        if (xml->hasTagName(apvts.state.getType())) {
            apvts.replaceState(juce::ValueTree::fromXml(*xml));
            resetTelemetry();
            baseEvents_ = static_cast<unsigned>(juce::jmax(0, static_cast<int>(apvts.state.getProperty("tmEvents", 0))));
            baseAvoided_ = static_cast<double>(apvts.state.getProperty("tmAvoided", 0.0));
            baseFlight_ = static_cast<double>(apvts.state.getProperty("tmFlight", 0.0));
            baseRecordM_ = static_cast<float>(static_cast<double>(apvts.state.getProperty("tmRecord", 0.0)));
            baseLastHz_ = static_cast<float>(static_cast<double>(apvts.state.getProperty("tmLastHz", 0.0)));
            for (int i = 0; i < kMaxChannels; ++i) {
                vc::FeedbackSuppressor::Notch list[vc::FeedbackSuppressor::kMaxFixed];
                int count = 0;
                for (const auto& item : juce::StringArray::fromTokens(apvts.state.getProperty("learned" + juce::String(i), "").toString(), ";", ""))
                    if (count < vc::FeedbackSuppressor::kMaxFixed) {
                        const auto parts = juce::StringArray::fromTokens(item, ":", "");
                        if (parts.size() == 3) list[count++] = { parts[0].getFloatValue(), parts[1].getFloatValue(), parts[2].getFloatValue() };
                    }
                ch_[static_cast<size_t>(i)].feedback.setLearnedNotches(list, count);
            }
        }
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new VonDeLarsenProcessor(); }
