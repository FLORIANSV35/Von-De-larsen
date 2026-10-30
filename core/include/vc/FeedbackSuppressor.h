#pragma once
#include <array>
#include <atomic>
#include <vector>

#include "vc/Biquad.h"
#include "vc/Fft.h"

namespace vc {

// Howl (larsen) suppressor: zero-latency narrow notch filters in the time
// domain, steered by a 1024-point analysis of the input that never delays the
// audio. A bin is flagged when it is a sharp spectral peak (peak-to-neighbour
// ratio) that persists and either grows or is nearly a pure tone; speech
// harmonics move and have lower ratios, so they are left alone.
class FeedbackSuppressor {
public:
    static constexpr int kFft = 1024;
    static constexpr int kHop = 256;
    static constexpr int kMaxNotches = 8;

    FeedbackSuppressor();

    void prepare(double sampleRate);
    void reset();

    // Thread-safe, may be called while processing.
    void setSensitivity(float s01) { sens_.store(s01, std::memory_order_relaxed); }
    void setMaxCutDb(float db) { maxCut_.store(db, std::memory_order_relaxed); }

    void process(float* x, int n) noexcept; // in place, 0 samples of latency

    int activeNotches() const;
    float notchFrequency(int slot) const { return slots_[static_cast<size_t>(slot)].freq; }
    float notchCutDb(int slot) const { return slots_[static_cast<size_t>(slot)].active ? slots_[static_cast<size_t>(slot)].curCut : 0.f; }

private:
    struct Slot {
        bool active = false;
        float freq = 0, curCut = 0, targetCut = 0;
        long lastDetect = 0;
        Biquad bq;
    };

    void analyze() noexcept;
    bool looksHarmonic(int bin) const noexcept;
    void detected(int bin) noexcept;
    void updateSlots() noexcept;

    Fft fft_;
    double fs_ = 48000, binHz_ = 48000.0 / kFft;
    float scale_ = 1;
    int persistFrames_ = 10, releaseFrames_ = 1800, kLo_ = 8, kHi_ = 200;
    long frame_ = 0;
    int wr_ = 0, hopCount_ = 0;
    std::atomic<float> sens_ { 0.5f }, maxCut_ { 18.0f };
    float thr_ = 18, strong_ = 30, growth_ = 2;

    std::vector<float> ring_, win_, pw_, lv_, pnpr_;
    std::vector<Fft::cf> buf_;
    std::vector<int> persist_, miss_, refAge_;
    std::vector<float> lvl0_;
    std::vector<char> cand_;
    std::array<Slot, kMaxNotches> slots_;
};

} // namespace vc
