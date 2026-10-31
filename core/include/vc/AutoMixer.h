#pragma once
#include <array>
#include <atomic>

namespace vc {

// Gain-sharing automixer in the style of the Dan Dugan system. The squared gains of all channels always add
// up to one: g_i^2 = Q_i / sum(Q), where Q_i is the smoothed power of channel i. One person speaking gets
// unity gain and the other microphones fall to the floor; two people speaking equally get -3 dB each; when
// nobody speaks the gains stay shared evenly, so the number of open microphones never adds noise or
// feedback ("NOM" attenuation).
//
// The power of a channel is weighted by how much it looks like speech (the denoiser's voice score), so a
// microphone that only hears noise or a neighbour's bleed does not take gain from the one being spoken into.
// Gains open in about 10 ms and close in about 100 ms; the power detector attacks in 5 ms and releases in
// 150 ms. During digital silence the gains are held. Zero latency, in place, no allocation.
class AutoMixer {
public:
    static constexpr int kMaxChannels = 8;

    void prepare(double sampleRate);
    void reset();

    // How far a silent microphone may fall, 0..60 dB (thread-safe).
    void setDepthDb(float db) { depth_.store(db < 0.f ? 0.f : db > 60.f ? 60.f : db, std::memory_order_relaxed); }

    // ch[i] points at n samples of channel i, processed in place. voice[i] is 0..1 (SpectralDenoiser::voiceScore()).
    // With enabled == false, or fewer than two channels, the gains glide back to unity.
    void process(float* const* ch, const float* voice, int nCh, int n, bool enabled) noexcept;

    // Current gain of a channel in dB (<= 0), for meters. Thread-safe.
    float gainDb(int ch) const noexcept { return gainDb_[static_cast<size_t>(ch)].load(std::memory_order_relaxed); }

private:
    double fs_ = 48000;
    std::atomic<float> depth_ { 20.f };
    std::array<float, kMaxChannels> power_ {}, gain_ {}, target_ {};
    std::array<std::atomic<float>, kMaxChannels> gainDb_ {};
};

} // namespace vc
