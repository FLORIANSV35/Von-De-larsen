#pragma once
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

    void setReductionDb(float db); // maximum attenuation, 0..40 dB (thread-safe)
    // Share of the neural mask in the final gain, 0..1 (thread-safe). The gain is a
    // geometric blend  g = g_wiener^(1-mix) * g_neural^mix, so 0 = classic only.
    void setNeuralMix(float mix) { mix_.store(mix, std::memory_order_relaxed); }
    void setMaskProvider(IMaskProvider* p) { provider_ = p; } // call while not processing

    int latencySamples() const { return kN; }
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
    bool haveFresh_ = false;
    IMaskProvider* provider_ = nullptr;

    std::vector<float> hist_, win_, acc_, outBlock_;
    std::vector<float> ps_, pn_, prevClean_, feat_, nn_, gain_;
    std::vector<Fft::cf> buf_;
    int wr_ = 0, pos_ = 0, nnAge_ = 1 << 20;
    bool first_ = true;
};

} // namespace vc
