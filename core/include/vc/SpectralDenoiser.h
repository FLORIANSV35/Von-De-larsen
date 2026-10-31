#pragma once
#include <algorithm>
#include <atomic>
#include <vector>

#include "vc/AsyncMaskProvider.h"
#include "vc/Fft.h"

namespace vc {

// Voice noise reducer. 256-sample sqrt-Hann STFT with 50% overlap, so the
// algorithmic latency is exactly kN samples (5.3 ms at 48 kHz).
//
// The per-bin gain comes from a classic decision-directed Wiener estimator
// with minimum-tracking noise PSD. When an IMaskProvider (CoreML / Neural
// Engine) is attached and delivers fresh masks, those replace the DSP gains.
// A mask is computed from the previous hop's spectrum and applied to the
// current one: one hop of staleness, but zero extra audio latency.
class SpectralDenoiser {
public:
    static constexpr int kN = 256;
    static constexpr int kHop = kN / 2;
    static constexpr int kBins = kN / 2 + 1;
    static constexpr int kMaxStaleHops = 4;

    SpectralDenoiser();

    void prepare(double sampleRate);
    void reset();

    // Low-latency mode ("High CPU"): the analysis window shrinks to kN/2 samples and the hop to kN/8, so the
    // latency halves (2.7 ms at 48 kHz) and the filtering runs four times more often. The neural model keeps
    // its own cadence: it still sees a full kN window every kHop samples, and its latest mask is applied to
    // every short frame. Call while not processing; it re-prepares the denoiser (state is reset).
    void setLowLatency(bool on) { lowLatency_ = on; prepare(fs_); }
    bool lowLatency() const { return lowLatency_; }

    void setReductionDb(float db); // maximum attenuation, 0..40 dB (thread-safe)
    // Share of the neural mask in the final gain, 0..1 (thread-safe). The gain is a
    // geometric blend  g = g_wiener^(1-mix) * g_neural^mix, so 0 = classic only.
    void setNeuralMix(float mix) { mix_.store(mix, std::memory_order_relaxed); }
    void setMaskProvider(IMaskProvider* p) { provider_ = p; } // call while not processing

    // Voice gate (thread-safe). When enabled and no voice is detected, the output is attenuated by up to
    // depthDb (0..40). Voice is judged once per hop from the speech band (~190 Hz to 3.9 kHz): the neural
    // mask when one is delivered (weighted by the neural share), otherwise the signal-to-noise ratio against
    // the tracked noise floor. The gate opens at once, holds 350 ms, then closes over 200 ms. Off by default.
    void setGate(bool on, float depthDb)
    {
        gateOn_.store(on, std::memory_order_relaxed);
        gateDepthDb_.store(std::clamp(depthDb, 0.f, 40.f), std::memory_order_relaxed);
    }
    // True while the gate considers that someone is speaking (also true during the hold time).
    bool voiceDetected() const { return voice_.load(std::memory_order_relaxed); }
    // Latest voice score 0..1 (0 = noise, 1 = voice), computed every hop even when the gate is off.
    float voiceScore() const { return score_.load(std::memory_order_relaxed); }

    int latencySamples() const { return win_len_; }
    bool usingNeuralMask() const { return usingNn_.load(std::memory_order_relaxed); }
    // Latest neural mask (129 gains) if fresh, else nullptr. Audio thread only; valid until the next process().
    const float* neuralMask() const { return haveFresh_ ? nn_.data() : nullptr; }

    void process(const float* in, float* out, int n) noexcept; // in == out allowed

private:
    void runFrame() noexcept;

    Fft fft_;
    double fs_ = 48000;
    float rise_ = 1.001f;
    std::atomic<float> gMin_ { 0.178f };
    std::atomic<float> mix_ { 0.7f };
    std::atomic<bool> usingNn_ { false };

    std::atomic<bool> gateOn_ { false }, voice_ { true };
    std::atomic<float> gateDepthDb_ { 20.f }, score_ { 0.f };
    int voiceHi_ = 21, holdHops_ = 56, releaseHops_ = 75, holdLeft_ = 0;
    bool open_ = true;
    float gateDb_ = 0.f, prevGateLin_ = 1.f;
    bool haveFresh_ = false;
    IMaskProvider* provider_ = nullptr;

    bool lowLatency_ = false;
    int win_len_ = kN, hop_ = kHop, nnEvery_ = 1, nnPhase_ = 0, staleLimit_ = kMaxStaleHops;
    float smooth_ = 0.8f, ddAlpha_ = 0.92f, outScale_ = 1.f;
    std::vector<float> winFull_;
    std::vector<Fft::cf> featBuf_;

    std::vector<float> hist_, win_, acc_, outBlock_;
    std::vector<float> ps_, pn_, prevClean_, feat_, nn_, gain_;
    std::vector<Fft::cf> buf_;
    int wr_ = 0, pos_ = 0, nnAge_ = 1 << 20;
    bool first_ = true;
};

} // namespace vc
