#include "vc/SpectralDenoiser.h"

#include <algorithm>
#include <cmath>

namespace vc {

namespace {
constexpr float kNoiseRiseDbPerSec = 6.0f; // how fast the noise floor may follow an increase
constexpr float kNoiseBias = 2.5f;         // min-tracking underestimates the mean
constexpr float kSmooth = 0.8f;            // power smoothing for the noise tracker
constexpr float kDdAlpha = 0.92f;          // decision-directed weight
constexpr float kVoiceHiHz = 3900.0f;      // top of the band used to judge voice
constexpr float kGateHoldSec = 0.35f;
constexpr float kGateReleaseSec = 0.20f;
} // namespace

SpectralDenoiser::SpectralDenoiser()
    : fft_(kN)
    , winFull_(kN), featBuf_(kN)
    , hist_(kN, 0.f), win_(kN), acc_(kN, 0.f), outBlock_(kHop, 0.f)
    , ps_(kBins), pn_(kBins), prevClean_(kBins), feat_(kBins), nn_(kBins), gain_(kBins)
    , buf_(kN)
{
    prepare(48000);
}

void SpectralDenoiser::prepare(double fs)
{
    fs_ = fs;
    win_len_ = lowLatency_ ? kN / 2 : kN;
    hop_ = lowLatency_ ? kN / 8 : kHop;
    nnEvery_ = kHop / hop_;
    staleLimit_ = kMaxStaleHops * nnEvery_;
    smooth_ = std::pow(kSmooth, static_cast<float>(hop_) / kHop); // the per-hop constants keep their time constants
    ddAlpha_ = std::pow(kDdAlpha, static_cast<float>(hop_) / kHop);
    outScale_ = 2.0f * hop_ / win_len_;
    win_.assign(static_cast<size_t>(kN), 0.f);
    for (int j = 0; j < win_len_; ++j) win_[static_cast<size_t>(j)] = static_cast<float>(std::sin(3.14159265358979323846 * j / win_len_));
    for (int j = 0; j < kN; ++j) winFull_[static_cast<size_t>(j)] = static_cast<float>(std::sin(3.14159265358979323846 * j / kN));
    rise_ = static_cast<float>(std::pow(10.0, (kNoiseRiseDbPerSec / 10.0) * hop_ / fs));
    voiceHi_ = std::clamp(static_cast<int>(kVoiceHiHz / (fs / kN)), 2, kBins - 2);
    holdHops_ = std::max(1, static_cast<int>(std::lround(kGateHoldSec * fs / hop_)));
    releaseHops_ = std::max(1, static_cast<int>(std::lround(kGateReleaseSec * fs / hop_)));
    reset();
}

void SpectralDenoiser::reset()
{
    std::fill(hist_.begin(), hist_.end(), 0.f);
    std::fill(acc_.begin(), acc_.end(), 0.f);
    std::fill(outBlock_.begin(), outBlock_.end(), 0.f);
    std::fill(prevClean_.begin(), prevClean_.end(), 0.f);
    wr_ = pos_ = 0;
    nnPhase_ = nnEvery_ - 1; // the first frame feeds the model
    nnAge_ = 1 << 20;
    first_ = true;
    open_ = true;
    holdLeft_ = 0;
    gateDb_ = 0.f;
    prevGateLin_ = 1.f;
    voice_.store(true, std::memory_order_relaxed);
}

void SpectralDenoiser::setReductionDb(float db)
{
    gMin_.store(std::pow(10.0f, -std::clamp(db, 0.f, 40.f) / 20.0f), std::memory_order_relaxed);
}

void SpectralDenoiser::process(const float* in, float* out, int n) noexcept
{
    for (int i = 0; i < n; ++i) {
        hist_[static_cast<size_t>(wr_)] = in[i];
        wr_ = (wr_ + 1) & (kN - 1);
        out[i] = outBlock_[static_cast<size_t>(pos_)];
        if (++pos_ == hop_) {
            pos_ = 0;
            runFrame();
        }
    }
}

void SpectralDenoiser::runFrame() noexcept
{
    // the last win_len_ samples, zero padded to the FFT size
    for (int j = 0; j < kN; ++j)
        buf_[static_cast<size_t>(j)] = j < win_len_
            ? Fft::cf(hist_[static_cast<size_t>((wr_ - win_len_ + j) & (kN - 1))] * win_[static_cast<size_t>(j)], 0.f)
            : Fft::cf(0.f, 0.f);
    fft_.forward(buf_.data());

    const float gMin = gMin_.load(std::memory_order_relaxed);
    constexpr float amp = 3.14159265f / kN; // sum(win) = 2N/pi -> amplitude normalisation

    bool useNn = false;
    if (provider_) {
        if (++nnPhase_ >= nnEvery_) { // the model sees a full-length window at its own cadence
            nnPhase_ = 0;
            const Fft::cf* spec = buf_.data();
            if (win_len_ != kN) {
                for (int j = 0; j < kN; ++j)
                    featBuf_[static_cast<size_t>(j)] = Fft::cf(hist_[static_cast<size_t>((wr_ + j) & (kN - 1))] * winFull_[static_cast<size_t>(j)], 0.f);
                fft_.forward(featBuf_.data());
                spec = featBuf_.data();
            }
            for (int k = 0; k < kBins; ++k) {
                const float p = std::norm(spec[k]) * amp * amp;
                feat_[static_cast<size_t>(k)] = 10.0f * std::log10(p + 1e-10f);
            }
            provider_->submit(feat_.data(), kBins);
        }
        if (provider_->fetch(nn_.data(), kBins)) nnAge_ = 0;
        else if (nnAge_ < (1 << 20)) ++nnAge_;
        useNn = nnAge_ <= staleLimit_;
    }
    usingNn_.store(useNn, std::memory_order_relaxed);
    haveFresh_ = useNn;
    const float mix = useNn ? std::clamp(mix_.load(std::memory_order_relaxed), 0.f, 1.f) : 0.f;

    float bandSig = 0.f, bandNoise = 0.f, bandNn = 0.f;
    for (int k = 0; k < kBins; ++k) {
        const size_t i = static_cast<size_t>(k);
        const float p = std::norm(buf_[i]);
        if (first_) ps_[i] = pn_[i] = p;
        ps_[i] = smooth_ * ps_[i] + (1.f - smooth_) * p;
        pn_[i] = ps_[i] < pn_[i] ? ps_[i] : pn_[i] * rise_;
        const float noise = std::max(pn_[i] * kNoiseBias, 1e-12f);
        if (k >= 1 && k <= voiceHi_) {
            bandSig += p;
            bandNoise += noise;
            if (useNn) bandNn += p * nn_[i];
        }

        const float gamma = p / noise;
        const float xi = std::max(ddAlpha_ * prevClean_[i] / noise + (1.f - ddAlpha_) * std::max(gamma - 1.f, 0.f), 1e-3f);
        float g = xi / (1.f + xi); // classic Wiener gain, always computed: it keeps the noise tracker honest
        if (mix > 0.f) g = std::exp((1.f - mix) * std::log(g) + mix * std::log(std::max(nn_[i], 1e-4f)));
        g = std::clamp(g, gMin, 1.f);
        prevClean_[i] = g * g * p;
        gain_[i] = g;
    }
    first_ = false;

    // ---- voice gate: decide once per hop, applied to this hop's output block below
    const float snrDb = 10.0f * std::log10((bandSig + 1e-12f) / (bandNoise + 1e-12f));
    const float vClassic = std::clamp((snrDb - 1.5f) / 6.0f, 0.f, 1.f);
    const float vNeural = std::clamp((bandNn / (bandSig + 1e-12f) - 0.25f) / 0.35f, 0.f, 1.f);
    const float v = (1.f - mix) * vClassic + mix * vNeural;
    score_.store(v, std::memory_order_relaxed);
    const bool gateOn = gateOn_.load(std::memory_order_relaxed);
    if (gateOn) {
        if (v > (open_ ? 0.3f : 0.5f)) { open_ = true; holdLeft_ = holdHops_; }
        else if (holdLeft_ > 0) --holdLeft_;
        else open_ = false;
        const float depth = gateDepthDb_.load(std::memory_order_relaxed);
        if (open_) gateDb_ = 0.f;
        else gateDb_ = std::max(-depth, gateDb_ - depth / static_cast<float>(releaseHops_));
        gateDb_ = std::max(gateDb_, -depth);
    } else {
        open_ = true;
        holdLeft_ = 0;
        gateDb_ = 0.f;
    }
    voice_.store(open_, std::memory_order_relaxed);

    for (int k = 0; k < kBins; ++k) buf_[static_cast<size_t>(k)] *= gain_[static_cast<size_t>(k)];
    for (int k = 1; k < kBins - 1; ++k) buf_[static_cast<size_t>(kN - k)] = std::conj(buf_[static_cast<size_t>(k)]);
    fft_.inverse(buf_.data());

    for (int j = 0; j < win_len_; ++j) acc_[static_cast<size_t>(j)] += buf_[static_cast<size_t>(j)].real() * win_[static_cast<size_t>(j)] * outScale_;
    std::copy(acc_.begin(), acc_.begin() + hop_, outBlock_.begin());
    const float gateLin = std::pow(10.0f, gateDb_ / 20.0f);
    if (gateLin != 1.f || prevGateLin_ != 1.f) { // ramp across the block: no clicks when the gate moves
        for (int j = 0; j < hop_; ++j)
            outBlock_[static_cast<size_t>(j)] *= prevGateLin_ + (gateLin - prevGateLin_) * static_cast<float>(j + 1) / hop_;
        prevGateLin_ = gateLin;
    }
    std::copy(acc_.begin() + hop_, acc_.end(), acc_.begin());
    std::fill(acc_.end() - hop_, acc_.end(), 0.f);
}

} // namespace vc
