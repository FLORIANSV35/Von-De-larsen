#pragma once
#include <algorithm>
#include <array>
#include <vector>

namespace vc {

// Bypass that stays in time. The input is kept in a short history, and when bypass is on the processed
// signal is cross-faded (10 ms) into the input delayed by the processing latency, so switching never shifts
// the timing and never clicks. The processing keeps running underneath, so switching back is instant.
//
// Per block, call push() with the untouched input BEFORE processing, then mix() with the processed audio
// (in place) AFTER. No allocation after prepare().
class BypassMixer {
public:
    static constexpr int kMaxChannels = 8;
    static constexpr int kMaxBlock = 16384; // longer blocks are passed through processed (no bypass)

    void prepare(double sampleRate, int latencySamples); // also the largest latency setLatency() may ask for
    void setLatency(int latencySamples) noexcept { latency_ = std::min(std::max(0, latencySamples), maxLatency_); }
    void reset();

    void push(const float* const* in, int nCh, int n) noexcept;
    void mix(float* const* out, int nCh, int n, bool bypass) noexcept;

private:
    int latency_ = 256, maxLatency_ = 256, mask_ = 0, wr_ = 0;
    float step_ = 0.f; // fade increment per sample
    float amount_ = 0.f; // 0 = processed, 1 = bypassed
    bool pushed_ = false;
    std::array<std::vector<float>, kMaxChannels> hist_;
};

} // namespace vc
