#pragma once
#include <array>
#include <vector>

#include "vc/Fft.h"

namespace vc {

// Adaptive feedback canceller. EXPERIMENTAL, not used by the plugin: it can only be trained with an audible noise sent to the
// loudspeakers (see README), which the room set-up must not do. It learns, continuously, how what is sent to the loudspeakers
// comes back into the microphone through the room, and subtracts that predicted return from the input before the
// rest of the chain sees it. Unlike the detectors it never has to guess whether a sound is a voice: it only
// cancels what it can predict from the output, and a word that has just begun cannot be predicted from it.
//
// A partitioned-block frequency-domain NLMS filter (overlap-save), 32 partitions of 128 taps = 4096 taps, 85 ms
// at 48 kHz. The first tap is delayed by `D` blocks so that the filter only needs output samples that already
// exist when a block is processed: the loop delay (output buffer + acoustic path + input buffer) must exceed
// that, about 5 ms for host blocks up to 128 samples and 13 ms for 512.
//
// It is trained, not tracked. The room is measured once, during the room set-up, with an independent noise sent to
// the loudspeakers while nobody speaks (setLearning(true), setAdapt(true), about 10 s); then the filter is frozen
// (setAdapt(false)). Measured on a simulated room, closed loop: noise at -20 dBFS gives a filter within -30 dB of
// the true room, -25 dBFS within -25 dB, -35 dBFS only -15 dB, and with the voice alone as the excitation the
// filter does not converge at all: a voice resembles what the microphone already hears from the talker, and a
// filter that keeps learning predicts the voice from its own output, diverges, and eats the voice. A frozen filter
// cannot. A guard also forbids the cancelled signal from carrying more than 10% more energy than the input.
// (Prediction-error whitening, setWhitening(true), is off by default: it whitens away the room's resonances.)
//
// Per host block, call cancel() on the input BEFORE the rest of the chain, then update() with what was sent to
// the loudspeakers AFTER it. The same n both times, and n <= the maxHostBlock given to prepare().
// Cost: about 130 Mflop/s per channel.
class FeedbackCanceller {
public:
    static constexpr int kBlock = 128;
    static constexpr int kPartitions = 32;
    static constexpr int kFftSize = 2 * kBlock;
    static constexpr int kBins = kBlock + 1;
    static constexpr int kLpcOrder = 32;

    FeedbackCanceller();

    void prepare(double sampleRate, int maxHostBlock);
    void reset();
    void setStep(float mu) { muRun_ = mu; }
    void setLearning(bool on) { learning_ = on; }
    // Whether the filter may adapt in the next block. The caller says false while someone is speaking: then the
    // microphone holds the voice itself, which resembles what comes out of the loudspeakers and would bias the
    // learning. In the pauses the microphone only hears the room answering, which is exactly what must be learned.
    void setAdapt(bool on) { adapt_ = on; }
    // Prediction-error whitening of the adaptation signals (on by default).
    void setWhitening(bool on) { whiten_ = on; }
    bool learning() const { return learning_; }
    static constexpr int kTaps = kPartitions * kBlock;
    // The learned loop response as kTaps time-domain taps, relative to minLoopDelaySamples(), and back.
    void exportTaps(float* taps) const;
    void importTaps(const float* taps, int dminSamples) noexcept; // allocation-free, may be called on the audio thread
    // The strongest return the learned filter predicts, in dB (0 dB = the loop is at unity there): how close the
    // room is to howling. Message thread (it allocates).
    float peakLoopGainDb() const;

    void cancel(float* x, int n) noexcept;
    void update(const float* out, int n) noexcept;

    // How much the canceller currently removes from the input, dB (0 = nothing), smoothed.
    float cancellationDb() const noexcept { return cancelDb_; }
    int minLoopDelaySamples() const noexcept { return D_ * kBlock; }
    // Blocks in which the energy guard had to scale the subtraction down.
    int cappedBlocks() const noexcept { return capped_; }

private:
    using cf = Fft::cf;
    void updateLpc(long m) noexcept;
    void computeOutputBlock(long m) noexcept;
    void adaptBlock(long m) noexcept;
    void storeSpectrum(long j) noexcept;
    cf* spec(long j) { return uSpec_.data() + static_cast<size_t>(((j % ring_) + ring_) % ring_) * kBins; }
    const cf* spec(long j) const { return uSpec_.data() + static_cast<size_t>(((j % ring_) + ring_) % ring_) * kBins; }
    cf* specW(long j) { return uSpecW_.data() + static_cast<size_t>(((j % ring_) + ring_) % ring_) * kBins; }
    const cf* specW(long j) const { return uSpecW_.data() + static_cast<size_t>(((j % ring_) + ring_) % ring_) * kBins; }
    cf* h(int p) { return h_.data() + static_cast<size_t>(p) * kBins; }

    Fft fft_;
    int D_ = 2, ring_ = 36, maxBlock_ = 128;
    float muRun_ = 0.f, muLearn_ = 0.12f;
    bool learning_ = false, adapt_ = true, whiten_ = false;
    std::vector<float> tapScratch_;
    std::vector<float> yTmp_, yRing_; // yRing_: the prediction actually subtracted, per sample
    int capped_ = 0;
    std::vector<cf> h_, uSpec_, uSpecW_; // uSpecW_: the output whitened by the voice model, for adaptation only
    std::array<double, kLpcOrder + 1> rs_ {}; // autocorrelation of the cleaned input, smoothed over about a second
    std::array<float, kLpcOrder + 1> a_ {}; // prediction-error filter of the voice: w[t] = x[t] - sum a[i] x[t-i]
    std::vector<float> uRing_, eRing_, powSm_, yHat_;
    std::vector<cf> work_;
    long tE_ = 0, tU_ = 0;
    bool hadOverflow_ = false;
    float px_ = 0.f, pe_ = 0.f, cancelDb_ = 0.f;
    int badBlocks_ = 0;
};

} // namespace vc
