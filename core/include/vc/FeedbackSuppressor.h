#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <vector>

#include "vc/Biquad.h"
#include "vc/Fft.h"

namespace vc {

// Howl (larsen) suppressor, built to react in tens of milliseconds while a voice is pushed very loud.
//
// Three layers, all in the time domain with zero latency, steered by a 1024-point analysis of the input that
// is refreshed every 128 samples and never delays the audio:
//  1. Notch filters. A sharp spectral peak that persists and is strong or growing is a howl; the notch
//     opens at once (12 dB after 3 frames, about 8 ms) and deepens while the peak lives. A new detection
//     next to an existing notch widens it instead of using another slot, so a broad room resonance is
//     covered by a few wide notches rather than by many narrow ones.
//  2. Runaway guard. If one tone takes over the spectrum and keeps growing for about 20 ms, the loop gain is
//     above one by more than the notches can fix yet. The guard cuts the whole signal at once by the
//     measured growth (a few dB more than the excess), then lets go slowly once the howl is gone.
//  3. Harmonic check. Voiced speech has partners at integer multiples and is never treated as a howl, unless
//     the peak stands 15 dB above all of its partners (a clipped loudspeaker adds harmonics to a howl).
class FeedbackSuppressor {
public:
    static constexpr int kFft = 1024;
    static constexpr int kHop = 128;
    static constexpr int kMaxNotches = 12;

    FeedbackSuppressor();

    void prepare(double sampleRate);
    void reset();

    // Thread-safe, may be called while processing.
    void setSensitivity(float s01) { sens_.store(s01, std::memory_order_relaxed); }
    void setMaxCutDb(float db) { maxCut_.store(db, std::memory_order_relaxed); }
    // Learn mode, for the room set-up, with someone speaking into the microphone while the output gain climbs
    // slowly (see RingOut). Every notch the detector opens is kept for good: it is never released and survives
    // reset(). Up to kMaxFixed such notches. Leave learn mode and they stay; clearLearned() removes them.
    void setLearn(bool on) { learn_.store(on, std::memory_order_relaxed); }
    bool learning() const { return learn_.load(std::memory_order_relaxed); }

    struct Notch { float freq = 0, cutDb = 0, q = 20.f; };
    static constexpr int kMaxFixed = 8;
    // Learned notches, readable from any thread (a snapshot refreshed every hop).
    int learnedCount() const noexcept;
    Notch learnedNotch(int index) const noexcept;
    // Replace the learned notches (message thread). They are applied at the next hop, with no click.
    void setLearnedNotches(const Notch* notches, int count) noexcept;
    void clearLearned() noexcept { setLearnedNotches(nullptr, 0); }
    // Every active notch, for a display: frequency, depth, learned or live.
    struct SlotView { float hz = 0, cutDb = 0; bool fixed = false; };
    int slotViews(SlotView* out, int maxCount) const noexcept;

    // EXPERIMENTAL, off by default. Catches a howl while it is still a broad band (about 20 ms) by looking for steady
    // exponential growth in one band, with the neural hint to leave voice alone. On 243 s of real speech it made 394
    // false notches and a 40 dB guard cut: at a word onset the mask still describes the silence before it. Do not enable.
    void setFastPath(bool on) { fastPath_.store(on, std::memory_order_relaxed); }

    // Optional hint from a neural voice mask (129 gains, 0 = noise/tone, 1 = voice), indexed on the
    // 256-point grid. Where it says "not voice" the detector reacts sooner; where it says "voice" it is
    // slightly more cautious. It only shifts thresholds, so a wrong hint can never block a real howl.
    // The pointer must stay valid until the next process() call. nullptr clears it.
    void setVoiceHint(const float* mask129) noexcept { hint_ = mask129; }

    void process(float* x, int n) noexcept; // in place, 0 samples of latency

    // Flight-log counters, readable from any thread. They are not cleared by prepare()/reset().
    struct Stats {
        std::uint32_t events = 0;   // howl frequencies caught (a new notch is opened)
        double avoidedSeconds = 0;  // time during which a notch or the guard was holding a howl
        float cutDb = 0;            // total cut applied right now by all notches and the guard (dB)
        float peakCutDb = 0;        // highest total cut seen since the last resetStats()
        float lastFreqHz = 0;       // frequency of the most recent catch
        float guardDb = 0;          // broadband cut applied right now by the runaway guard (dB)
    };
    Stats stats() const noexcept;
    void resetStats() noexcept;

    int activeNotches() const;
    float notchFrequency(int slot) const { return slots_[static_cast<size_t>(slot)].freq; }
    float notchCutDb(int slot) const { return slots_[static_cast<size_t>(slot)].active ? slots_[static_cast<size_t>(slot)].curCut : 0.f; }
    float notchQ(int slot) const { return slots_[static_cast<size_t>(slot)].q; }

private:
    struct Slot {
        bool active = false;
        float freq = 0, q = 20.f, curCut = 0, targetCut = 0;
        long lastDetect = 0, lastRaise = 0;
        bool fixed = false; // a learned notch: permanent
        Biquad bq;
    };

    void analyze() noexcept;
    bool looksHarmonic(int bin) const noexcept;
    void detected(int bin) noexcept;
    void updateSlots() noexcept;
    void runawayGuard() noexcept;
    void raiseGuard(float growthDb) noexcept;
    void trackExponentialGrowth(float topLevelDb) noexcept;

    Fft fft_;
    double fs_ = 48000, binHz_ = 48000.0 / kFft;
    float scale_ = 1;
    int persistFrames_ = 19, releaseFrames_ = 3750, guardHoldFrames_ = 56, kLo_ = 8, kHi_ = 200;
    long frame_ = 0;
    int wr_ = 0, hopCount_ = 0;
    std::atomic<float> sens_ { 0.5f }, maxCut_ { 18.0f };
    std::atomic<bool> fastPath_ { false }, learn_ { false };
    const float* hint_ = nullptr;
    float thr_ = 18, strong_ = 30, growth_ = 2, growFast_ = 4;

    std::vector<float> ring_, win_, pw_, lv_, pnpr_;
    std::array<std::vector<float>, 4> hist_; // levels of the last four frames, per bin
    int histPos_ = 0;
    std::vector<Fft::cf> buf_;
    std::vector<int> persist_, miss_, refAge_;
    std::vector<float> lvl0_;
    std::vector<char> cand_;
    std::array<Slot, kMaxNotches> slots_;

    // runaway guard
    std::array<float, 32> tPow_ {}; // power of the raw input per hop, measured in the time domain (no window smearing)
    std::array<std::vector<float>, 16> bandHist_; // per bin, 3-bin band level in dB, last 8 frames
    int bandPos_ = 0;
    std::vector<float> growthScratch_; // growth of the active bins, to find the typical one
    double hopSq_ = 0;
    int tPos_ = 0, hotCount_ = 0;
    long lastRunaway_ = -1000000, refractoryUntil_ = 0;
    float floorNotchSum_ = 0.f, guardFloorDb_ = 0.f, guardTargetDb_ = 0.f, guardTargetLin_ = 1.f, guardLin_ = 1.f, guardAtt_ = 0.1f, guardRel_ = 0.001f;
    float guardReleasePerHop_ = 0.008f, guardFloorDecayPerHop_ = 0.0008f;

    // learned notches: message thread -> audio thread hand-over (a seqlock) and a snapshot for display
    static constexpr int kSnap = kMaxNotches;
    std::atomic<unsigned> loadSeq_ { 0 };
    std::array<std::atomic<float>, kMaxFixed * 3> loadBuf_ {};
    std::atomic<int> loadCount_ { 0 };
    unsigned appliedSeq_ = 0;
    std::array<std::atomic<float>, kSnap> snapHz_ {}, snapCut_ {}, snapQ_ {};
    std::array<std::atomic<int>, kSnap> snapFixed_ {}; // 0 = inactive, 1 = live, 2 = learned
    void applyPendingLoad() noexcept;
    void publishSnapshot() noexcept;
    int fixedCount() const noexcept;

    bool anyActive_ = false; // audio thread only
    std::atomic<std::uint32_t> events_ { 0 };
    std::atomic<std::uint64_t> avoidedSamples_ { 0 };
    std::atomic<float> cutSum_ { 0.f }, peakCut_ { 0.f }, lastFreq_ { 0.f }, guardOut_ { 0.f };
};

} // namespace vc
