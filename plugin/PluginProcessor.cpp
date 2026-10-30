// NOTE: written without access to a Mac/JUCE; needs a first build on macOS.
#include "PluginProcessor.h"

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
}

VonDeLarsenProcessor::~VonDeLarsenProcessor()
{
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
                                                NormalisableRange<float>(6.f, 30.f), 18.f));
    l.add(std::make_unique<AudioParameterFloat>(ParameterID { "nrAmount", 1 }, "Noise reduction (dB)",
                                                NormalisableRange<float>(0.f, 30.f), 15.f));
    l.add(std::make_unique<AudioParameterBool>(ParameterID { "nnEnable", 1 }, "Neural Engine mask", true));
    l.add(std::make_unique<AudioParameterFloat>(ParameterID { "nnMix", 1 }, "Neural share (0 = classic only)",
                                                NormalisableRange<float>(0.f, 1.f), 0.7f));
    return l;
}

void VonDeLarsenProcessor::loadNeuralModel()
{
    const auto f = modelFile();
    if (!f.exists()) return; // no model installed: the classic DSP path is used
    for (auto& c : ch_) {
        if (c.mask) continue;
        std::string err;
        c.mask = vc::makeCoreMlMaskProvider(f.getFullPathName().toStdString(), vc::SpectralDenoiser::kBins, err);
        if (!c.mask) DBG("Neural model not loaded: " << err);
    }
}

void VonDeLarsenProcessor::prepareToPlay(double sr, int)
{
    fs_ = sr;
    loadNeuralModel();
    for (auto& c : ch_) {
        c.feedback.prepare(sr);
        c.denoiser.prepare(sr);
    }
    setLatencySamples(ch_[0].denoiser.latencySamples()); // the feedback notches add none
}

bool VonDeLarsenProcessor::isBusesLayoutSupported(const BusesLayout& l) const
{
    const auto in = l.getMainInputChannelSet();
    return in == l.getMainOutputChannelSet() && (in == juce::AudioChannelSet::mono() || in == juce::AudioChannelSet::stereo());
}

void VonDeLarsenProcessor::processBlock(juce::AudioBuffer<float>& buf, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    const bool useNn = nn_->load() > 0.5f;
    const int n = buf.getNumSamples();
    const int nCh = juce::jmin(buf.getNumChannels(), kMaxChannels);

    for (int c = 0; c < nCh; ++c) {
        auto& ch = ch_[static_cast<size_t>(c)];
        ch.feedback.setSensitivity(sens_->load());
        ch.feedback.setMaxCutDb(maxCut_->load());
        ch.denoiser.setReductionDb(nr_->load());
        ch.denoiser.setMaskProvider(useNn ? ch.mask.get() : nullptr);
        ch.denoiser.setNeuralMix(mix_->load());

        float* x = buf.getWritePointer(c);
        ch.feedback.setVoiceHint(ch.denoiser.neuralMask()); // neural voice/noise hint steers the howl detector
        ch.feedback.process(x, n);            // howl first: it must see the raw mic signal
        ch.denoiser.process(x, x, n);
    }
}

void VonDeLarsenProcessor::getStateInformation(juce::MemoryBlock& dest)
{
    if (auto xml = apvts.copyState().createXml()) copyXmlToBinary(*xml, dest);
}

void VonDeLarsenProcessor::setStateInformation(const void* data, int size)
{
    if (auto xml = getXmlFromBinary(data, size))
        if (xml->hasTagName(apvts.state.getType())) apvts.replaceState(juce::ValueTree::fromXml(*xml));
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new VonDeLarsenProcessor(); }
