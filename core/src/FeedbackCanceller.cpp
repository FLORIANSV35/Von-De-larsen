#include "vc/FeedbackCanceller.h"

#include <algorithm>
#include <cmath>

namespace vc {

namespace {
constexpr int kTimeMask = 16383; // time-domain rings: 16384 samples
}

FeedbackCanceller::FeedbackCanceller()
    : fft_(kFftSize)
    , h_(static_cast<size_t>(kPartitions) * kBins)
    , uRing_(kTimeMask + 1, 0.f), eRing_(kTimeMask + 1, 0.f), powSm_(kBins, 1e-6f), yHat_(kBlock, 0.f)
    , work_(kFftSize)
{
    prepare(48000, 128);
}

void FeedbackCanceller::prepare(double, int maxHostBlock)
{
    maxBlock_ = std::clamp(maxHostBlock, 16, 8192);
    D_ = (maxBlock_ + kBlock - 1) / kBlock + 1;
    ring_ = kPartitions + D_ + 2;
    uSpec_.assign(static_cast<size_t>(ring_) * kBins, cf(0.f, 0.f));
    uSpecW_.assign(static_cast<size_t>(ring_) * kBins, cf(0.f, 0.f));
    yTmp_.assign(static_cast<size_t>(maxBlock_), 0.f);
    tapScratch_.assign(kTaps, 0.f);
    yRing_.assign(kTimeMask + 1, 0.f);
    reset();
}

void FeedbackCanceller::reset()
{
    std::fill(h_.begin(), h_.end(), cf(0.f, 0.f));
    std::fill(uSpec_.begin(), uSpec_.end(), cf(0.f, 0.f));
    std::fill(uSpecW_.begin(), uSpecW_.end(), cf(0.f, 0.f));
    a_.fill(0.f);
    rs_.fill(0.0);
    std::fill(uRing_.begin(), uRing_.end(), 0.f);
    std::fill(eRing_.begin(), eRing_.end(), 0.f);
    std::fill(powSm_.begin(), powSm_.end(), 1e-6f);
    std::fill(yHat_.begin(), yHat_.end(), 0.f);
    std::fill(yRing_.begin(), yRing_.end(), 0.f);
    capped_ = 0;
    tE_ = tU_ = 0;
    hadOverflow_ = false;
    px_ = pe_ = 1e-12f;
    cancelDb_ = 0.f;
    badBlocks_ = 0;
}

// Prediction-error filter of the voice, from the last 1024 samples of the cleaned input (Levinson-Durbin).
// Whitening both the reference and the error with it removes what a voice lets one predict about itself,
// so the filter cannot learn to predict the voice from its own output (the bias that makes it diverge).
void FeedbackCanceller::updateLpc(long m) noexcept
{
    constexpr int N = 1024;
    std::array<float, N> w;
    for (int i = 0; i < N; ++i) {
        const long s = m * kBlock + kBlock - N + i;
        const float win = 0.5f - 0.5f * std::cos(6.2831853f * (static_cast<float>(i) + 0.5f) / static_cast<float>(N));
        w[static_cast<size_t>(i)] = (s < 0 ? 0.f : eRing_[static_cast<size_t>(s & kTimeMask)]) * win;
    }
    std::array<double, kLpcOrder + 1> r {};
    for (int lag = 0; lag <= kLpcOrder; ++lag) {
        double sum = 0;
        for (int i = lag; i < N; ++i) sum += static_cast<double>(w[static_cast<size_t>(i)]) * w[static_cast<size_t>(i - lag)];
        r[static_cast<size_t>(lag)] = sum;
    }
    // Smoothed over about a second, so that the model describes the voice and not a howl that is just being born
    // (a model of the howl would whiten it away, and the filter would never see what it has to cancel).
    for (int lag = 0; lag <= kLpcOrder; ++lag) rs_[static_cast<size_t>(lag)] = 0.9973 * rs_[static_cast<size_t>(lag)] + 0.0027 * r[static_cast<size_t>(lag)];
    r = rs_;
    if (r[0] < 1e-9) return; // silence: keep the previous model
    r[0] = r[0] * 1.0001 + 1e-9;
    std::array<double, kLpcOrder + 1> a {}, tmp {};
    double err = r[0];
    for (int i = 1; i <= kLpcOrder; ++i) {
        double acc = r[static_cast<size_t>(i)];
        for (int j = 1; j < i; ++j) acc -= a[static_cast<size_t>(j)] * r[static_cast<size_t>(i - j)];
        const double k = acc / err;
        tmp = a;
        a[static_cast<size_t>(i)] = k;
        for (int j = 1; j < i; ++j) a[static_cast<size_t>(j)] = tmp[static_cast<size_t>(j)] - k * tmp[static_cast<size_t>(i - j)];
        err *= (1.0 - k * k);
        if (err <= 0) break;
    }
    for (int i = 1; i <= kLpcOrder; ++i) a_[static_cast<size_t>(i)] = static_cast<float>(a[static_cast<size_t>(i)]);
}

// Output block m: sum over partitions of H_p * U_{m-D-p}, keep the last half of the circular convolution.
void FeedbackCanceller::computeOutputBlock(long m) noexcept
{
    std::fill(work_.begin(), work_.begin() + kBins, cf(0.f, 0.f));
    for (int p = 0; p < kPartitions; ++p) {
        const long j = m - D_ - p;
        if (j < 0) break;
        const cf* hp = h(p);
        const cf* u = spec(j);
        for (int k = 0; k < kBins; ++k) work_[static_cast<size_t>(k)] += hp[k] * u[k];
    }
    for (int k = 1; k < kBlock; ++k) work_[static_cast<size_t>(kFftSize - k)] = std::conj(work_[static_cast<size_t>(k)]);
    work_[0] = cf(work_[0].real(), 0.f);
    work_[static_cast<size_t>(kBlock)] = cf(work_[static_cast<size_t>(kBlock)].real(), 0.f);
    fft_.inverse(work_.data());
    for (int i = 0; i < kBlock; ++i) yHat_[static_cast<size_t>(i)] = work_[static_cast<size_t>(kBlock + i)].real();
}

// Spectrum of the output over blocks j-1 and j (a full 2-block window), kept for the output and for adaptation.
void FeedbackCanceller::storeSpectrum(long j) noexcept
{
    for (int i = 0; i < kFftSize; ++i) {
        const long s = (j - 1) * kBlock + i;
        work_[static_cast<size_t>(i)] = cf(s < 0 ? 0.f : uRing_[static_cast<size_t>(s & kTimeMask)], 0.f);
    }
    fft_.forward(work_.data());
    cf* dst = spec(j);
    for (int k = 0; k < kBins; ++k) dst[k] = work_[static_cast<size_t>(k)];

    // the same window, whitened with the voice model
    for (int i = 0; i < kFftSize; ++i) {
        const long s = (j - 1) * kBlock + i;
        float v = s < 0 ? 0.f : uRing_[static_cast<size_t>(s & kTimeMask)];
        for (int q = 1; q <= kLpcOrder; ++q) {
            const long sq = s - q;
            if (sq >= 0) v -= a_[static_cast<size_t>(q)] * uRing_[static_cast<size_t>(sq & kTimeMask)];
        }
        work_[static_cast<size_t>(i)] = cf(s < 0 ? 0.f : v, 0.f);
    }
    fft_.forward(work_.data());
    cf* dstW = specW(j);
    for (int k = 0; k < kBins; ++k) dstW[k] = work_[static_cast<size_t>(k)];
}

void FeedbackCanceller::adaptBlock(long m) noexcept
{
    if (whiten_) updateLpc(m);
    else a_.fill(0.f);
    // error spectrum: the block of cleaned input, whitened with the voice model, zero-padded in front
    for (int i = 0; i < kBlock; ++i) work_[static_cast<size_t>(i)] = cf(0.f, 0.f);
    double pe = 0, px = 0;
    for (int i = 0; i < kBlock; ++i) {
        const long s = m * kBlock + i;
        const float e = eRing_[static_cast<size_t>(s & kTimeMask)];
        float ew = e;
        for (int q = 1; q <= kLpcOrder; ++q) {
            const long sq = s - q;
            if (sq >= 0) ew -= a_[static_cast<size_t>(q)] * eRing_[static_cast<size_t>(sq & kTimeMask)];
        }
        work_[static_cast<size_t>(kBlock + i)] = cf(ew, 0.f);
        pe += static_cast<double>(e) * e;
    }
    fft_.forward(work_.data());

    // per-bin power of the reference over the whole filter (the NLMS normalisation)
    std::array<float, kBins> pw {};
    for (int p = 0; p < kPartitions; ++p) {
        const long j = m - D_ - p;
        if (j < 0) break;
        const cf* u = specW(j);
        for (int k = 0; k < kBins; ++k) pw[static_cast<size_t>(k)] += std::norm(u[k]);
    }
    float mean = 0.f;
    for (int k = 0; k < kBins; ++k) {
        powSm_[static_cast<size_t>(k)] = 0.8f * powSm_[static_cast<size_t>(k)] + 0.2f * pw[static_cast<size_t>(k)];
        mean += powSm_[static_cast<size_t>(k)];
    }
    mean /= kBins;
    const float floorPow = 1e-3f * mean + 1e-9f;

    for (int p = 0; p < kPartitions && adapt_; ++p) {
        const long j = m - D_ - p;
        if (j < 0) break;
        cf* hp = h(p);
        const cf* u = specW(j);
        for (int k = 0; k < kBins; ++k)
            hp[k] += ((learning_ ? muLearn_ : muRun_) / (powSm_[static_cast<size_t>(k)] + floorPow)) * std::conj(u[k]) * work_[static_cast<size_t>(k)];
    }

    // gradient constraint on one partition per block: its impulse response lives in the first half only
    cf* hc = h(static_cast<int>(m % kPartitions));
    for (int k = 0; k < kBins; ++k) work_[static_cast<size_t>(k)] = hc[k];
    for (int k = 1; k < kBlock; ++k) work_[static_cast<size_t>(kFftSize - k)] = std::conj(work_[static_cast<size_t>(k)]);
    work_[0] = cf(work_[0].real(), 0.f);
    work_[static_cast<size_t>(kBlock)] = cf(work_[static_cast<size_t>(kBlock)].real(), 0.f);
    fft_.inverse(work_.data());
    for (int i = kBlock; i < kFftSize; ++i) work_[static_cast<size_t>(i)] = cf(0.f, 0.f);
    fft_.forward(work_.data());
    for (int k = 0; k < kBins; ++k) hc[k] = work_[static_cast<size_t>(k)];

    // bookkeeping: how much is removed, and a guard against divergence (the output must never exceed the input)
    for (int i = 0; i < kBlock; ++i) {
        const size_t at = static_cast<size_t>((m * kBlock + i) & kTimeMask);
        const float xv = eRing_[at] + yRing_[at]; // x = e + what was subtracted
        px += static_cast<double>(xv) * xv;
    }
    px_ = 0.9f * px_ + 0.1f * static_cast<float>(px / kBlock);
    pe_ = 0.9f * pe_ + 0.1f * static_cast<float>(pe / kBlock);
    cancelDb_ = 10.f * std::log10((px_ + 1e-12f) / (pe_ + 1e-12f));
    badBlocks_ = pe_ > 2.f * px_ + 1e-10f ? badBlocks_ + 1 : 0;
    if (badBlocks_ > 40) { // diverging: forget what was learned
        std::fill(h_.begin(), h_.end(), cf(0.f, 0.f));
        badBlocks_ = 0;
    }
}

void FeedbackCanceller::cancel(float* x, int n) noexcept
{
    const bool ok = n <= maxBlock_;
    // 1. the predicted feedback for these samples
    for (int i = 0; i < n; ++i) {
        const long s = tE_ + i;
        const int pos = static_cast<int>(s % kBlock);
        if (pos == 0) {
            if (ok) computeOutputBlock(s / kBlock);
            else std::fill(yHat_.begin(), yHat_.end(), 0.f);
        }
        if (ok) yTmp_[static_cast<size_t>(i)] = yHat_[static_cast<size_t>(pos)];
    }
    // 2. never subtract more than the input holds: the cancelled signal may not carry more energy than the input
    float g = 1.f;
    if (ok) {
        double ex = 0;
        for (int i = 0; i < n; ++i) ex += static_cast<double>(x[i]) * x[i];
        for (int tries = 0; tries < 4; ++tries) {
            double ee = 0;
            for (int i = 0; i < n; ++i) { const double e = x[i] - g * yTmp_[static_cast<size_t>(i)]; ee += e * e; }
            if (ee <= ex * 1.1 + 1e-12) break;
            g *= 0.5f;
        }
        if (g < 1.f) ++capped_;
    }
    // 3. write the cleaned input, and adapt whenever a block is complete
    for (int i = 0; i < n; ++i) {
        const long s = tE_ + i;
        const int pos = static_cast<int>(s % kBlock);
        const float sub = ok ? g * yTmp_[static_cast<size_t>(i)] : 0.f;
        const float e = x[i] - sub;
        yRing_[static_cast<size_t>(s & kTimeMask)] = sub;
        eRing_[static_cast<size_t>(s & kTimeMask)] = e;
        x[i] = e;
        if (pos == kBlock - 1 && ok && s >= static_cast<long>(D_ + 1) * kBlock) {
            adaptBlock(s / kBlock);
        }
    }
    tE_ += n;
}

void FeedbackCanceller::exportTaps(float* taps) const
{
    std::vector<cf> w(kFftSize);
    for (int p = 0; p < kPartitions; ++p) {
        const cf* hp = h_.data() + static_cast<size_t>(p) * kBins;
        for (int k = 0; k < kBins; ++k) w[static_cast<size_t>(k)] = hp[k];
        for (int k = 1; k < kBlock; ++k) w[static_cast<size_t>(kFftSize - k)] = std::conj(w[static_cast<size_t>(k)]);
        w[0] = cf(w[0].real(), 0.f);
        w[static_cast<size_t>(kBlock)] = cf(w[static_cast<size_t>(kBlock)].real(), 0.f);
        fft_.inverse(w.data());
        for (int i = 0; i < kBlock; ++i) taps[p * kBlock + i] = w[static_cast<size_t>(i)].real();
    }
}

// `taps` were learned with a first tap at dminSamples; this canceller's first tap is at minLoopDelaySamples().
void FeedbackCanceller::importTaps(const float* taps, int dminSamples) noexcept
{
    const int shift = D_ * kBlock - dminSamples; // positive: the old early taps are now out of range
    for (int i = 0; i < kTaps; ++i) {
        const int src = i + shift;
        tapScratch_[static_cast<size_t>(i)] = (src >= 0 && src < kTaps) ? taps[src] : 0.f;
    }
    for (int p = 0; p < kPartitions; ++p) {
        for (int i = 0; i < kFftSize; ++i) work_[static_cast<size_t>(i)] = cf(i < kBlock ? tapScratch_[static_cast<size_t>(p * kBlock + i)] : 0.f, 0.f);
        fft_.forward(work_.data());
        cf* hp = h_.data() + static_cast<size_t>(p) * kBins;
        for (int k = 0; k < kBins; ++k) hp[k] = work_[static_cast<size_t>(k)];
    }
}

float FeedbackCanceller::peakLoopGainDb() const
{
    std::vector<float> taps(kTaps);
    exportTaps(taps.data());
    constexpr int N = 8192;
    Fft big(N);
    std::vector<Fft::cf> w(N, Fft::cf(0.f, 0.f));
    for (int i = 0; i < kTaps; ++i) w[static_cast<size_t>(i)] = Fft::cf(taps[static_cast<size_t>(i)], 0.f);
    big.forward(w.data());
    float peak = 0.f;
    for (int k = 0; k <= N / 2; ++k) peak = std::max(peak, std::abs(w[static_cast<size_t>(k)]));
    return 20.f * std::log10(peak + 1e-9f);
}

void FeedbackCanceller::update(const float* out, int n) noexcept
{
    for (int i = 0; i < n; ++i) {
        const long s = tU_ + i;
        uRing_[static_cast<size_t>(s & kTimeMask)] = out[i];
        if ((s % kBlock) == kBlock - 1) storeSpectrum(s / kBlock);
    }
    tU_ += n;
}

} // namespace vc
