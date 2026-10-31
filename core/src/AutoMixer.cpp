#include "vc/AutoMixer.h"

#include <algorithm>
#include <cmath>

namespace vc {

namespace {
constexpr float kPowerAttackSec = 0.005f;
constexpr float kPowerReleaseSec = 0.150f;
constexpr float kGainOpenSec = 0.010f;
constexpr float kGainCloseSec = 0.100f;
constexpr float kSilence = 1e-9f;    // total weighted power (about -90 dBFS) under which gains are held
constexpr float kVoiceFloor = 0.01f; // weight of a channel that looks like pure noise (-20 dB)

inline float coef(int n, double fs, float sec) { return 1.f - std::exp(-static_cast<float>(n) / (static_cast<float>(fs) * sec)); }
} // namespace

void AutoMixer::prepare(double fs)
{
    fs_ = fs;
    reset();
}

void AutoMixer::reset()
{
    power_.fill(0.f);
    gain_.fill(1.f);
    target_.fill(1.f);
    for (auto& g : gainDb_) g.store(0.f, std::memory_order_relaxed);
}

void AutoMixer::process(float* const* ch, const float* voice, int nCh, int n, bool enabled) noexcept
{
    nCh = std::min(nCh, kMaxChannels);
    if (n <= 0) return;
    const bool active = enabled && nCh >= 2;

    if (active) {
        const float aAtt = coef(n, fs_, kPowerAttackSec), aRel = coef(n, fs_, kPowerReleaseSec);
        std::array<float, kMaxChannels> q {};
        float total = 0.f;
        for (int c = 0; c < nCh; ++c) {
            const float* x = ch[c];
            double e = 0;
            for (int i = 0; i < n; ++i) e += static_cast<double>(x[i]) * x[i];
            const float pb = static_cast<float>(e / n);
            auto& p = power_[static_cast<size_t>(c)];
            p += (pb - p) * (pb > p ? aAtt : aRel);
            const float s = std::clamp(voice[c], 0.f, 1.f);
            q[static_cast<size_t>(c)] = p * (kVoiceFloor + (1.f - kVoiceFloor) * s * s);
            total += q[static_cast<size_t>(c)];
        }
        if (total > kSilence) {
            const float floorLin = std::pow(10.f, -depth_.load(std::memory_order_relaxed) / 20.f);
            for (int c = 0; c < nCh; ++c)
                target_[static_cast<size_t>(c)] = std::clamp(std::sqrt(q[static_cast<size_t>(c)] / total), floorLin, 1.f);
        } // else: hold the last targets
    } else {
        target_.fill(1.f);
    }

    const float aOpen = coef(n, fs_, kGainOpenSec), aClose = coef(n, fs_, kGainCloseSec);
    for (int c = 0; c < nCh; ++c) {
        auto& g = gain_[static_cast<size_t>(c)];
        const float t = target_[static_cast<size_t>(c)];
        const float gNew = g + (t - g) * (t > g ? aOpen : aClose);
        if (gNew != 1.f || g != 1.f) {
            float* x = ch[c];
            for (int i = 0; i < n; ++i) x[i] *= g + (gNew - g) * static_cast<float>(i + 1) / static_cast<float>(n);
        }
        g = gNew;
        gainDb_[static_cast<size_t>(c)].store(20.f * std::log10(std::max(gNew, 1e-6f)), std::memory_order_relaxed);
    }
}

} // namespace vc
