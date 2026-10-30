#include "vc/FeedbackSuppressor.h"

#include <algorithm>
#include <cmath>

namespace vc {

namespace {
constexpr float kNotchQ = 30.0f;
constexpr float kRelatedPeakDb = 6.0f; // a peak this sharp counts as a harmonic partner
constexpr float kMinLevelDb = -70.0f; // ignore anything quieter than this (dBFS)
inline float lerp(float a, float b, float t) { return a + (b - a) * t; }
} // namespace

FeedbackSuppressor::FeedbackSuppressor()
    : fft_(kFft)
    , ring_(kFft, 0.f), win_(kFft), pw_(kFft / 2 + 1), lv_(kFft / 2 + 1), pnpr_(kFft / 2 + 1)
    , buf_(kFft)
    , persist_(kFft / 2 + 1), miss_(kFft / 2 + 1), refAge_(kFft / 2 + 1)
    , lvl0_(kFft / 2 + 1), cand_(kFft / 2 + 1)
{
    double sum = 0;
    for (int i = 0; i < kFft; ++i) {
        win_[static_cast<size_t>(i)] = static_cast<float>(0.5 - 0.5 * std::cos(6.283185307179586 * i / kFft));
        sum += win_[static_cast<size_t>(i)];
    }
    scale_ = static_cast<float>(2.0 / sum); // a sine of amplitude A reads A
    prepare(48000);
}

void FeedbackSuppressor::prepare(double fs)
{
    fs_ = fs;
    binHz_ = fs / kFft;
    persistFrames_ = std::max(4, static_cast<int>(std::lround(0.05 * fs / kHop)));
    releaseFrames_ = static_cast<int>(std::lround(10.0 * fs / kHop)); // hold a notch 10 s after the last detection
    kLo_ = std::max(8, static_cast<int>(std::ceil(150.0 / binHz_)));
    kHi_ = std::min(kFft / 2 - 8, static_cast<int>(std::floor(std::min(10000.0, 0.45 * fs) / binHz_)));
    reset();
}

void FeedbackSuppressor::reset()
{
    std::fill(ring_.begin(), ring_.end(), 0.f);
    std::fill(persist_.begin(), persist_.end(), 0);
    std::fill(miss_.begin(), miss_.end(), 0);
    std::fill(refAge_.begin(), refAge_.end(), 0);
    std::fill(lvl0_.begin(), lvl0_.end(), 0.f);
    for (auto& s : slots_) s = Slot {};
    wr_ = hopCount_ = 0;
    frame_ = 0;
}

int FeedbackSuppressor::activeNotches() const
{
    int n = 0;
    for (const auto& s : slots_) n += s.active ? 1 : 0;
    return n;
}

void FeedbackSuppressor::process(float* x, int n) noexcept
{
    for (int i = 0; i < n; ++i) {
        const float in = x[i];
        ring_[static_cast<size_t>(wr_)] = in;
        wr_ = (wr_ + 1) & (kFft - 1);
        float y = in;
        for (auto& s : slots_)
            if (s.active) y = s.bq.process(y);
        x[i] = y;
        if (++hopCount_ == kHop) {
            hopCount_ = 0;
            analyze();
        }
    }
}

void FeedbackSuppressor::analyze() noexcept
{
    ++frame_;
    for (int i = 0; i < kFft; ++i)
        buf_[static_cast<size_t>(i)] = Fft::cf(ring_[static_cast<size_t>((wr_ + i) & (kFft - 1))] * win_[static_cast<size_t>(i)], 0.f);
    fft_.forward(buf_.data());

    const int nb = kFft / 2 + 1;
    for (int k = 0; k < nb; ++k) {
        const float p = std::norm(buf_[static_cast<size_t>(k)]) * scale_ * scale_;
        pw_[static_cast<size_t>(k)] = p;
        lv_[static_cast<size_t>(k)] = 10.0f * std::log10(p + 1e-12f);
    }

    const float s = std::clamp(sens_.load(std::memory_order_relaxed), 0.f, 1.f);
    thr_ = lerp(22.f, 12.f, s);
    strong_ = lerp(34.f, 24.f, s);
    growth_ = lerp(3.f, 1.5f, s);

    std::fill(cand_.begin(), cand_.end(), 0);
    for (int k = kLo_; k <= kHi_; ++k) {
        const size_t i = static_cast<size_t>(k);
        if (lv_[i] < kMinLevelDb || !(lv_[i] > lv_[i - 1] && lv_[i] >= lv_[i + 1])) continue;
        float sum = 0;
        for (int d = 3; d <= 8; ++d) sum += pw_[static_cast<size_t>(k - d)] + pw_[static_cast<size_t>(k + d)];
        const float nb_db = 10.0f * std::log10(sum / 12.0f + 1e-12f);
        pnpr_[i] = lv_[i] - nb_db;
        if (pnpr_[i] >= kRelatedPeakDb) cand_[i] = pnpr_[i] >= thr_ ? 2 : 1; // 1: weak peak (harmonic check only), 2: candidate
    }

    for (int k = kLo_; k <= kHi_; ++k) {
        const size_t i = static_cast<size_t>(k);
        if (cand_[i] == 2) {
            if (persist_[i] == 0 || refAge_[i] >= 2 * persistFrames_) {
                lvl0_[i] = lv_[i];
                refAge_[i] = 0;
            }
            ++persist_[i];
            ++refAge_[i];
            miss_[i] = 0;
            if (persist_[i] >= persistFrames_) {
                const bool growing = refAge_[i] >= persistFrames_ / 2 && (lv_[i] - lvl0_[i]) >= growth_;
                if ((growing || pnpr_[i] >= strong_) && !looksHarmonic(k)) detected(k);
            }
        } else if (persist_[i] > 0 && ++miss_[i] > 2) {
            persist_[i] = miss_[i] = refAge_[i] = 0;
        }
    }
    updateSlots();
}

// A howl is an isolated tone; voiced speech/singing has other peaks at integer
// multiples (or sub-multiples) of the same fundamental.
bool FeedbackSuppressor::looksHarmonic(int k) const noexcept
{
    auto peakNear = [this](float bin) {
        const int c = static_cast<int>(std::lround(bin));
        if (c - 1 < kLo_ || c + 1 > kHi_) return false;
        return cand_[static_cast<size_t>(c - 1)] || cand_[static_cast<size_t>(c)] || cand_[static_cast<size_t>(c + 1)]; // any peak
    };
    const float kf = static_cast<float>(k);
    int related = 0;
    for (float ratio : { 2.f, 3.f, 0.5f, 1.f / 3.f })
        related += peakNear(kf * ratio) ? 1 : 0;
    return related >= 2;
}

void FeedbackSuppressor::detected(int k) noexcept
{
    const size_t i = static_cast<size_t>(k);
    const float a = lv_[i - 1], b = lv_[i], c = lv_[i + 1];
    const float den = a - 2.f * b + c;
    const float d = std::fabs(den) > 1e-6f ? std::clamp(0.5f * (a - c) / den, -0.5f, 0.5f) : 0.f;
    const float f = (static_cast<float>(k) + d) * static_cast<float>(binHz_);
    const float maxCut = maxCut_.load(std::memory_order_relaxed);

    Slot* hit = nullptr;
    for (auto& sl : slots_)
        if (sl.active && std::fabs(sl.freq - f) < 1.2f * static_cast<float>(binHz_)) { hit = &sl; break; }

    if (!hit) {
        Slot* victim = nullptr;
        for (auto& sl : slots_)
            if (!sl.active) { victim = &sl; break; }
        if (!victim) {
            victim = &slots_[0];
            for (auto& sl : slots_)
                if (sl.lastDetect < victim->lastDetect) victim = &sl;
        }
        *victim = Slot {};
        victim->active = true;
        victim->freq = f;
        victim->targetCut = std::min(6.f, maxCut);
        hit = victim;
    } else {
        hit->targetCut = std::min(maxCut, hit->targetCut + 2.f);
    }
    hit->lastDetect = frame_;
}

void FeedbackSuppressor::updateSlots() noexcept
{
    for (auto& sl : slots_) {
        if (!sl.active) continue;
        if (frame_ - sl.lastDetect > releaseFrames_) sl.targetCut = std::max(0.f, sl.targetCut - 0.1f);
        const float before = sl.curCut;
        if (sl.curCut < sl.targetCut) sl.curCut = std::min(sl.targetCut, sl.curCut + 1.0f);
        else sl.curCut = std::max(sl.targetCut, sl.curCut - 0.1f);
        if (sl.curCut != before) sl.bq.setNotch(fs_, sl.freq, kNotchQ, sl.curCut);
        if (sl.targetCut <= 0.f && sl.curCut <= 0.05f) sl = Slot {};
    }
}

} // namespace vc
