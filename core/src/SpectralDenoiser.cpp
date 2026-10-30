#include "vc/SpectralDenoiser.h"

#include <algorithm>
#include <cmath>

namespace vc {

namespace {
constexpr float kNoiseRiseDbPerSec = 6.0f; // how fast the noise floor may follow an increase
constexpr float kNoiseBias = 2.5f;         // min-tracking underestimates the mean
constexpr float kSmooth = 0.8f;            // power smoothing for the noise tracker
constexpr float kDdAlpha = 0.92f;          // decision-directed weight
} // namespace

SpectralDenoiser::SpectralDenoiser()
    : fft_(kN)
    , hist_(kN, 0.f), win_(kN), acc_(kN, 0.f), outBlock_(kHop, 0.f)
    , ps_(kBins), pn_(kBins), prevClean_(kBins), feat_(kBins), nn_(kBins), gain_(kBins)
    , buf_(kN)
{
    for (int j = 0; j < kN; ++j) win_[static_cast<size_t>(j)] = static_cast<float>(std::sin(3.14159265358979323846 * j / kN));
    prepare(48000);
}

void SpectralDenoiser::prepare(double fs)
{
    fs_ = fs;
    rise_ = static_cast<float>(std::pow(10.0, (kNoiseRiseDbPerSec / 10.0) * kHop / fs));
    reset();
}

void SpectralDenoiser::reset()
{
    std::fill(hist_.begin(), hist_.end(), 0.f);
    std::fill(acc_.begin(), acc_.end(), 0.f);
    std::fill(outBlock_.begin(), outBlock_.end(), 0.f);
    std::fill(prevClean_.begin(), prevClean_.end(), 0.f);
    wr_ = pos_ = 0;
    nnAge_ = 1 << 20;
    first_ = true;
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
        if (++pos_ == kHop) {
            pos_ = 0;
            runFrame();
        }
    }
}

void SpectralDenoiser::runFrame() noexcept
{
    for (int j = 0; j < kN; ++j)
        buf_[static_cast<size_t>(j)] = Fft::cf(hist_[static_cast<size_t>((wr_ + j) & (kN - 1))] * win_[static_cast<size_t>(j)], 0.f);
    fft_.forward(buf_.data());

    const float gMin = gMin_.load(std::memory_order_relaxed);
    constexpr float amp = 3.14159265f / kN; // sum(win) = 2N/pi -> amplitude normalisation

    bool useNn = false;
    if (provider_) {
        for (int k = 0; k < kBins; ++k) {
            const float p = std::norm(buf_[static_cast<size_t>(k)]) * amp * amp;
            feat_[static_cast<size_t>(k)] = 10.0f * std::log10(p + 1e-10f);
        }
        provider_->submit(feat_.data(), kBins);
        if (provider_->fetch(nn_.data(), kBins)) nnAge_ = 0;
        else if (nnAge_ < (1 << 20)) ++nnAge_;
        useNn = nnAge_ <= kMaxStaleHops;
    }
    usingNn_.store(useNn, std::memory_order_relaxed);

    for (int k = 0; k < kBins; ++k) {
        const size_t i = static_cast<size_t>(k);
        const float p = std::norm(buf_[i]);
        if (first_) ps_[i] = pn_[i] = p;
        ps_[i] = kSmooth * ps_[i] + (1.f - kSmooth) * p;
        pn_[i] = ps_[i] < pn_[i] ? ps_[i] : pn_[i] * rise_;
        const float noise = std::max(pn_[i] * kNoiseBias, 1e-12f);

        float g;
        if (useNn) {
            g = nn_[i];
        } else {
            const float gamma = p / noise;
            const float xi = std::max(kDdAlpha * prevClean_[i] / noise + (1.f - kDdAlpha) * std::max(gamma - 1.f, 0.f), 1e-3f);
            g = xi / (1.f + xi);
        }
        g = std::clamp(g, gMin, 1.f);
        prevClean_[i] = g * g * p;
        gain_[i] = g;
    }
    first_ = false;

    for (int k = 0; k < kBins; ++k) buf_[static_cast<size_t>(k)] *= gain_[static_cast<size_t>(k)];
    for (int k = 1; k < kBins - 1; ++k) buf_[static_cast<size_t>(kN - k)] = std::conj(buf_[static_cast<size_t>(k)]);
    fft_.inverse(buf_.data());

    for (int j = 0; j < kN; ++j) acc_[static_cast<size_t>(j)] += buf_[static_cast<size_t>(j)].real() * win_[static_cast<size_t>(j)];
    std::copy(acc_.begin(), acc_.begin() + kHop, outBlock_.begin());
    std::copy(acc_.begin() + kHop, acc_.end(), acc_.begin());
    std::fill(acc_.begin() + kHop, acc_.end(), 0.f);
}

} // namespace vc
