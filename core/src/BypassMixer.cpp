#include "vc/BypassMixer.h"

#include <algorithm>

namespace vc {

void BypassMixer::prepare(double fs, int latencySamples)
{
    latency_ = maxLatency_ = std::max(0, latencySamples);
    int size = 1;
    while (size < latency_ + kMaxBlock + 1) size <<= 1;
    mask_ = size - 1;
    for (auto& h : hist_) h.assign(static_cast<size_t>(size), 0.f);
    step_ = 1.f / std::max(1.f, static_cast<float>(0.010 * fs));
    reset();
}

void BypassMixer::reset()
{
    for (auto& h : hist_) std::fill(h.begin(), h.end(), 0.f);
    wr_ = 0;
    amount_ = 0.f;
    pushed_ = false;
}

void BypassMixer::push(const float* const* in, int nCh, int n) noexcept
{
    pushed_ = false;
    if (n <= 0 || n > kMaxBlock) return;
    nCh = std::min(nCh, kMaxChannels);
    for (int c = 0; c < nCh; ++c) {
        auto& h = hist_[static_cast<size_t>(c)];
        for (int i = 0; i < n; ++i) h[static_cast<size_t>((wr_ + i) & mask_)] = in[c][i];
    }
    wr_ = (wr_ + n) & mask_;
    pushed_ = true;
}

void BypassMixer::mix(float* const* out, int nCh, int n, bool bypass) noexcept
{
    if (!pushed_ || n <= 0) return;
    pushed_ = false;
    nCh = std::min(nCh, kMaxChannels);
    const float target = bypass ? 1.f : 0.f;
    if (amount_ == target) {
        if (target == 0.f) return; // fully processed: nothing to do
        for (int c = 0; c < nCh; ++c) {
            const auto& h = hist_[static_cast<size_t>(c)];
            for (int i = 0; i < n; ++i) out[c][i] = h[static_cast<size_t>((wr_ - n + i - latency_) & mask_)];
        }
        return;
    }
    const float dir = target > amount_ ? step_ : -step_;
    float last = amount_;
    for (int c = 0; c < nCh; ++c) {
        const auto& h = hist_[static_cast<size_t>(c)];
        float a = amount_;
        for (int i = 0; i < n; ++i) {
            a += dir;
            if ((dir > 0.f && a > target) || (dir < 0.f && a < target)) a = target;
            const float dry = h[static_cast<size_t>((wr_ - n + i - latency_) & mask_)];
            out[c][i] += (dry - out[c][i]) * a;
        }
        last = a;
    }
    amount_ = last;
}

} // namespace vc
