#include "vc/FeedbackSuppressor.h"

#include <algorithm>
#include <cmath>

namespace vc {

namespace {
constexpr float kNotchQ0 = 20.0f;      // a new notch
constexpr float kNotchQMin = 6.0f;     // widest a notch may become
constexpr float kRelatedPeakDb = 6.0f; // a peak this sharp counts as a harmonic partner
constexpr float kDominantDb = 15.0f;   // a peak this far above its partners is not a voice
constexpr float kMinLevelDb = -70.0f;  // ignore anything quieter than this (dBFS)
constexpr float kTopWindowDb = 24.0f;  // a howl worth acting on is among the dominant components of the spectrum
constexpr int kConfirmStrong = 8;      // frames (21 ms) a strong, stationary peak must last
constexpr int kConfirmGrowing = 10;    // frames (27 ms) a peak must last and still be growing: a note attack stops growing by then
constexpr float kFirstCutDb = 12.0f;   // cut applied the moment a notch opens
constexpr float kRampDbPerHop = 4.0f;
constexpr float kGuardMaxDb = 30.0f;
inline float lerp(float a, float b, float t) { return a + (b - a) * t; }
} // namespace

FeedbackSuppressor::FeedbackSuppressor()
    : fft_(kFft)
    , ring_(kFft, 0.f), win_(kFft), pw_(kFft / 2 + 1), lv_(kFft / 2 + 1), pnpr_(kFft / 2 + 1)
    , buf_(kFft)
    , persist_(kFft / 2 + 1), miss_(kFft / 2 + 1), refAge_(kFft / 2 + 1)
    , lvl0_(kFft / 2 + 1), cand_(kFft / 2 + 1)
{
    for (auto& h : hist_) h.assign(kFft / 2 + 1, -120.f);
    for (auto& h : bandHist_) h.assign(kFft / 2 + 1, -120.f);
    growthScratch_.reserve(kFft / 2 + 1);
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
    persistFrames_ = std::max(6, static_cast<int>(std::lround(0.05 * fs / kHop)));
    releaseFrames_ = static_cast<int>(std::lround(10.0 * fs / kHop)); // hold a notch 10 s after the last detection
    guardHoldFrames_ = static_cast<int>(std::lround(0.15 * fs / kHop));
    guardReleasePerHop_ = static_cast<float>(2.0 * kHop / fs);       // let go at 2 dB per second...
    guardFloorDecayPerHop_ = static_cast<float>(0.3 * kHop / fs);    // ...but never below what the loop proved it needs (decays at 0.3 dB/s)
    guardAtt_ = 1.f - std::exp(-1.f / static_cast<float>(0.0005 * fs));
    guardRel_ = 1.f - std::exp(-1.f / static_cast<float>(0.05 * fs));
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
    for (auto& h : hist_) std::fill(h.begin(), h.end(), -120.f);
    std::array<Slot, kMaxFixed> keep;
    int kept = 0;
    for (const auto& s : slots_)
        if (s.active && s.fixed && kept < kMaxFixed) keep[static_cast<size_t>(kept++)] = s; // learned notches survive a reset
    for (auto& s : slots_) s = Slot {};
    for (int i = 0; i < kept; ++i) {
        Slot s = keep[static_cast<size_t>(i)];
        s.bq.clear();
        s.bq.setNotch(fs_, s.freq, s.q, s.curCut);
        slots_[static_cast<size_t>(i)] = s;
    }
    tPow_.fill(0.f);
    hopSq_ = 0;
    for (auto& h : bandHist_) std::fill(h.begin(), h.end(), -120.f);
    bandPos_ = 0;
    wr_ = hopCount_ = histPos_ = tPos_ = hotCount_ = 0;
    frame_ = 0;
    lastRunaway_ = -1000000;
    refractoryUntil_ = 0;
    guardTargetDb_ = guardFloorDb_ = floorNotchSum_ = 0.f;
    guardTargetLin_ = guardLin_ = 1.f;
    anyActive_ = false;
}

FeedbackSuppressor::Stats FeedbackSuppressor::stats() const noexcept
{
    Stats st;
    st.events = events_.load(std::memory_order_relaxed);
    st.avoidedSeconds = static_cast<double>(avoidedSamples_.load(std::memory_order_relaxed)) / fs_;
    st.cutDb = cutSum_.load(std::memory_order_relaxed);
    st.peakCutDb = peakCut_.load(std::memory_order_relaxed);
    st.lastFreqHz = lastFreq_.load(std::memory_order_relaxed);
    st.guardDb = guardOut_.load(std::memory_order_relaxed);
    return st;
}

void FeedbackSuppressor::resetStats() noexcept
{
    events_.store(0, std::memory_order_relaxed);
    avoidedSamples_.store(0, std::memory_order_relaxed);
    peakCut_.store(0.f, std::memory_order_relaxed);
    lastFreq_.store(0.f, std::memory_order_relaxed);
}

int FeedbackSuppressor::fixedCount() const noexcept
{
    int n = 0;
    for (const auto& s : slots_) n += (s.active && s.fixed) ? 1 : 0;
    return n;
}

int FeedbackSuppressor::learnedCount() const noexcept
{
    int n = 0;
    for (const auto& f : snapFixed_) n += f.load(std::memory_order_relaxed) == 2 ? 1 : 0;
    return n;
}

FeedbackSuppressor::Notch FeedbackSuppressor::learnedNotch(int index) const noexcept
{
    int seen = 0;
    for (int i = 0; i < kSnap; ++i) {
        const size_t k = static_cast<size_t>(i);
        if (snapFixed_[k].load(std::memory_order_relaxed) != 2) continue;
        if (seen++ == index) return { snapHz_[k].load(std::memory_order_relaxed), snapCut_[k].load(std::memory_order_relaxed), snapQ_[k].load(std::memory_order_relaxed) };
    }
    return {};
}

int FeedbackSuppressor::slotViews(SlotView* out, int maxCount) const noexcept
{
    int n = 0;
    for (int i = 0; i < kSnap && n < maxCount; ++i) {
        const size_t k = static_cast<size_t>(i);
        const int f = snapFixed_[k].load(std::memory_order_relaxed);
        if (f == 0) continue;
        out[n++] = { snapHz_[k].load(std::memory_order_relaxed), snapCut_[k].load(std::memory_order_relaxed), f == 2 };
    }
    return n;
}

void FeedbackSuppressor::setLearnedNotches(const Notch* notches, int count) noexcept
{
    const int n = std::clamp(count, 0, kMaxFixed);
    const unsigned s = loadSeq_.load(std::memory_order_relaxed);
    loadSeq_.store(s + 1, std::memory_order_release); // odd: being written
    for (int i = 0; i < n; ++i) {
        loadBuf_[static_cast<size_t>(i * 3)].store(notches[i].freq, std::memory_order_relaxed);
        loadBuf_[static_cast<size_t>(i * 3 + 1)].store(notches[i].cutDb, std::memory_order_relaxed);
        loadBuf_[static_cast<size_t>(i * 3 + 2)].store(notches[i].q, std::memory_order_relaxed);
    }
    loadCount_.store(n, std::memory_order_relaxed);
    loadSeq_.store(s + 2, std::memory_order_release);
}

void FeedbackSuppressor::applyPendingLoad() noexcept
{
    const unsigned s1 = loadSeq_.load(std::memory_order_acquire);
    if ((s1 & 1u) || s1 == appliedSeq_) return;
    std::array<Notch, kMaxFixed> local;
    const int n = loadCount_.load(std::memory_order_relaxed);
    for (int i = 0; i < n; ++i)
        local[static_cast<size_t>(i)] = { loadBuf_[static_cast<size_t>(i * 3)].load(std::memory_order_relaxed),
                                          loadBuf_[static_cast<size_t>(i * 3 + 1)].load(std::memory_order_relaxed),
                                          loadBuf_[static_cast<size_t>(i * 3 + 2)].load(std::memory_order_relaxed) };
    if (loadSeq_.load(std::memory_order_acquire) != s1) return; // the writer was at it again: try at the next hop
    appliedSeq_ = s1;
    for (auto& sl : slots_) if (sl.fixed) sl = Slot {};
    for (int i = 0; i < n; ++i) {
        Slot* free = nullptr;
        for (auto& sl : slots_) if (!sl.active) { free = &sl; break; }
        if (!free) break;
        const auto& nt = local[static_cast<size_t>(i)];
        *free = Slot {};
        free->active = free->fixed = true;
        free->freq = nt.freq;
        free->q = nt.q > 1.f ? nt.q : kNotchQ0;
        free->targetCut = free->curCut = std::clamp(nt.cutDb, 0.f, 40.f);
        free->lastDetect = frame_;
        free->bq.setNotch(fs_, free->freq, free->q, free->curCut);
    }
}

void FeedbackSuppressor::publishSnapshot() noexcept
{
    for (int i = 0; i < kSnap; ++i) {
        const size_t k = static_cast<size_t>(i);
        const auto& sl = slots_[k];
        snapFixed_[k].store(sl.active ? (sl.fixed ? 2 : 1) : 0, std::memory_order_relaxed);
        snapHz_[k].store(sl.freq, std::memory_order_relaxed);
        snapCut_[k].store(sl.curCut, std::memory_order_relaxed);
        snapQ_[k].store(sl.q, std::memory_order_relaxed);
    }
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
        ring_[static_cast<size_t>(wr_)] = in; // the analysis always sees the raw input
        hopSq_ += static_cast<double>(in) * in;
        wr_ = (wr_ + 1) & (kFft - 1);
        float y = in;
        for (auto& s : slots_)
            if (s.active) y = s.bq.process(y);
        guardLin_ += (guardTargetLin_ - guardLin_) * (guardTargetLin_ < guardLin_ ? guardAtt_ : guardRel_);
        x[i] = y * guardLin_;
        if (++hopCount_ == kHop) {
            hopCount_ = 0;
            analyze();
        }
    }
    if (anyActive_) avoidedSamples_.fetch_add(static_cast<std::uint64_t>(n), std::memory_order_relaxed);
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
    histPos_ = (histPos_ + 1) & 3;
    hist_[static_cast<size_t>(histPos_)] = lv_;
    const auto& lv3 = hist_[static_cast<size_t>((histPos_ + 1) & 3)]; // three frames (8 ms) ago

    const float s = std::clamp(sens_.load(std::memory_order_relaxed), 0.f, 1.f);
    thr_ = lerp(22.f, 12.f, s);
    strong_ = lerp(34.f, 24.f, s);
    growth_ = lerp(3.f, 1.5f, s);
    growFast_ = lerp(5.f, 2.5f, s); // growth over three frames that marks a howl taking off

    // Threshold shift in dB for a 1024-grid bin, from the neural voice hint (4 bins per hint bin).
    auto hintShift = [this](int k) {
        if (!hint_) return 0.f;
        const float h = hint_[std::min(k / 4, 128)];
        return h < 0.3f ? -4.f : h > 0.7f ? 3.f : 0.f;
    };

    float topLv = -200.f;
    int topBin = kLo_;
    for (int k = kLo_; k <= kHi_; ++k)
        if (lv_[static_cast<size_t>(k)] > topLv) { topLv = lv_[static_cast<size_t>(k)]; topBin = k; }

    std::fill(cand_.begin(), cand_.end(), 0);
    for (int k = kLo_; k <= kHi_; ++k) {
        const size_t i = static_cast<size_t>(k);
        if (lv_[i] < kMinLevelDb || !(lv_[i] > lv_[i - 1] && lv_[i] >= lv_[i + 1])) continue;
        float sum = 0;
        for (int d = 3; d <= 8; ++d) sum += pw_[static_cast<size_t>(k - d)] + pw_[static_cast<size_t>(k + d)];
        const float nb_db = 10.0f * std::log10(sum / 12.0f + 1e-12f);
        pnpr_[i] = lv_[i] - nb_db;
        if (pnpr_[i] >= kRelatedPeakDb)
            cand_[i] = (pnpr_[i] >= thr_ + hintShift(k) && lv_[i] >= topLv - kTopWindowDb) ? 2 : 1; // 1: weak peak (harmonic check only), 2: candidate
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
            const bool takingOff = persist_[i] >= kConfirmGrowing && frame_ > 4 && (lv_[i] - lv3[i]) >= growFast_;
            const bool strong = persist_[i] >= kConfirmStrong && pnpr_[i] >= strong_ + hintShift(k);
            const bool slow = persist_[i] >= persistFrames_ && refAge_[i] >= persistFrames_ / 2 && (lv_[i] - lvl0_[i]) >= growth_;
            if ((takingOff || strong || slow) && !looksHarmonic(k)) {
                detected(k);
                if (takingOff) raiseGuard(lv_[i] - lv3[i]); // a confirmed line that is still taking off: the notch alone may not be enough
            }
        } else if (persist_[i] > 0 && ++miss_[i] > 3) {
            persist_[i] = miss_[i] = refAge_[i] = 0;
        }
    }
    (void) topBin;
    trackExponentialGrowth(topLv);
    applyPendingLoad();
    updateSlots();
    publishSnapshot();
    runawayGuard();
}

// A howl is an isolated tone; voiced speech/singing has other peaks at integer multiples (or sub-multiples)
// of the same fundamental. A peak that towers 15 dB over all of its partners is a howl whose loudspeaker
// distortion added harmonics.
bool FeedbackSuppressor::looksHarmonic(int k) const noexcept
{
    int related = 0;
    float strongestPartner = -200.f;
    for (float ratio : { 2.f, 3.f, 0.5f, 1.f / 3.f }) {
        const int c = static_cast<int>(std::lround(static_cast<float>(k) * ratio));
        if (c - 1 < kLo_ || c + 1 > kHi_) continue;
        bool found = false;
        for (int d = -1; d <= 1; ++d) {
            const size_t j = static_cast<size_t>(c + d);
            if (cand_[j]) { found = true; strongestPartner = std::max(strongestPartner, lv_[j]); }
        }
        related += found ? 1 : 0;
    }
    return related >= 2 && (lv_[static_cast<size_t>(k)] - strongestPartner) < kDominantDb;
}

void FeedbackSuppressor::detected(int k) noexcept
{
    const size_t i = static_cast<size_t>(k);
    const float a = lv_[i - 1], b = lv_[i], c = lv_[i + 1];
    const float den = a - 2.f * b + c;
    const float d = std::fabs(den) > 1e-6f ? std::clamp(0.5f * (a - c) / den, -0.5f, 0.5f) : 0.f;
    const float f = (static_cast<float>(k) + d) * static_cast<float>(binHz_);
    const float maxCut = maxCut_.load(std::memory_order_relaxed);
    const float bin = static_cast<float>(binHz_);

    Slot* hit = nullptr;
    bool widen = false;
    for (auto& sl : slots_)
        if (sl.active && std::fabs(sl.freq - f) < 1.2f * bin) { hit = &sl; break; }
    if (!hit) // a howl hopping around inside a broad resonance: widen the nearby notch instead of piling up narrow ones
        for (auto& sl : slots_)
            if (sl.active && std::fabs(sl.freq - f) < 4.f * bin) { hit = &sl; widen = true; break; }

    const bool learn = learn_.load(std::memory_order_relaxed);
    if (!hit) {
        Slot* victim = nullptr;
        for (auto& sl : slots_)
            if (!sl.active) { victim = &sl; break; }
        if (!victim) {
            for (auto& sl : slots_) // the oldest live notch; a learned one is never evicted
                if (!sl.fixed && (!victim || sl.lastDetect < victim->lastDetect)) victim = &sl;
            if (!victim) return;
        }
        *victim = Slot {};
        victim->fixed = learn && fixedCount() < kMaxFixed;
        victim->active = true;
        victim->freq = f;
        victim->q = kNotchQ0;
        victim->targetCut = std::min(18.f, maxCut);
        victim->curCut = std::min(kFirstCutDb, victim->targetCut); // opens at once, deepens over the next frames
        victim->bq.setNotch(fs_, victim->freq, victim->q, victim->curCut);
        victim->lastRaise = frame_;
        hit = victim;
        events_.fetch_add(1, std::memory_order_relaxed);
        lastFreq_.store(f, std::memory_order_relaxed);
    } else {
        if (learn && !hit->fixed && fixedCount() < kMaxFixed) hit->fixed = true; // a live notch the howl found again while learning: keep it
        if (frame_ - hit->lastRaise >= 8) { // about one loop round trip between raises
            hit->targetCut = std::min(maxCut, hit->targetCut + 3.f);
            hit->lastRaise = frame_;
            if (widen) {
                hit->q = std::max(kNotchQMin, hit->q * 0.8f);
                hit->freq = 0.5f * (hit->freq + f);
                hit->bq.setNotch(fs_, hit->freq, hit->q, hit->curCut);
            }
        }
    }
    hit->lastDetect = frame_;
}

void FeedbackSuppressor::updateSlots() noexcept
{
    for (auto& sl : slots_) {
        if (!sl.active) continue;
        if (!sl.fixed && frame_ - sl.lastDetect > releaseFrames_) sl.targetCut = std::max(0.f, sl.targetCut - 0.1f);
        const float before = sl.curCut;
        if (sl.curCut < sl.targetCut) sl.curCut = std::min(sl.targetCut, sl.curCut + kRampDbPerHop);
        else sl.curCut = std::max(sl.targetCut, sl.curCut - 0.1f);
        if (sl.curCut != before) sl.bq.setNotch(fs_, sl.freq, sl.q, sl.curCut);
        if (!sl.fixed && sl.targetCut <= 0.f && sl.curCut <= 0.05f) sl = Slot {};
    }
}

// Closed-loop safety net, in two parts.
//  1. raiseGuard(): a spectral line that was confirmed as a howl (stable for 27 ms, away from voice-like
//     partners) and is still growing means the loop gain is above one by more than a notch can fix yet. The
//     growth per round trip is about the excess gain, so the whole signal is cut by that much plus a
//     margin at once. While the line lives the cut is raised again, at most once per 43 ms.
//     Growth of the level alone is NOT used: a word onset and the birth of a howl look alike for the first
//     30 ms (same dominant band, same steepness, same crest factor, measured on real speech).
//  2. hot input: the input stays above -6 dBFS rms for about 80 ms (several howls at once that never form a
//     clean line): the cut grows in 6 dB steps until the level comes back down. A voice does not sit there.
// Releasing is slow (3 dB/s) and never goes below what the loop has shown it needs; that floor falls as the
// notches that opened since then take their share.
void FeedbackSuppressor::raiseGuard(float growthDb) noexcept
{
    if (frame_ < refractoryUntil_) return;
    const long since = frame_ - lastRunaway_;
    if (since > guardHoldFrames_ && since < 70 * guardHoldFrames_) { // the howl came back after the guard had let go
        float sum = 0.f;
        for (const auto& sl : slots_) if (sl.active) sum += sl.curCut;
        guardFloorDb_ = std::min(kGuardMaxDb, guardTargetDb_ + 3.f);
        floorNotchSum_ = sum;
    }
    const float cut = std::clamp(growthDb * 1.3f + 4.f, 6.f, kGuardMaxDb);
    guardTargetDb_ = std::min(kGuardMaxDb + 10.f, since > guardHoldFrames_ ? std::max(guardTargetDb_, cut) : guardTargetDb_ + 0.6f * cut);
    if (guardFloorDb_ < 0.5f * guardTargetDb_) { // first alarm: do not let go all the way, the loop needs about this much
        guardFloorDb_ = 0.5f * guardTargetDb_;
        float sum = 0.f;
        for (const auto& sl : slots_) if (sl.active) sum += sl.curCut;
        floorNotchSum_ = sum;
    }
    lastRunaway_ = frame_;
    refractoryUntil_ = frame_ + 16;
}

// The signature of a feedback loop: a band of the spectrum grows by the same number of dB every frame, for
// several frames, in the same place. A word onset (even a steep one) does not hold one slope: it is smeared by
// the analysis window into a ramp whose slope falls. This catches a howl while it is still a broad band of
// amplified noise, tens of milliseconds before it collapses to a line a notch can see, and it follows several
// resonances growing at the same time, each on its own.
void FeedbackSuppressor::trackExponentialGrowth(float topLevelDb) noexcept
{
    constexpr int kSpan = 10; // frames of steady growth required (27 ms): a word onset smeared by the window ramps for 8
    const int nb = kFft / 2 + 1;
    bandPos_ = (bandPos_ + 1) & 15;
    auto& cur = bandHist_[static_cast<size_t>(bandPos_)];
    for (int k = 1; k < nb - 1; ++k)
        cur[static_cast<size_t>(k)] = 10.0f * std::log10(pw_[static_cast<size_t>(k - 1)] + pw_[static_cast<size_t>(k)] + pw_[static_cast<size_t>(k + 1)] + 1e-12f);
    if (frame_ < 24 || !fastPath_.load(std::memory_order_relaxed)) return; // never on in learn mode: someone is speaking then

    const float need = lerp(2.0f, 1.2f, std::clamp(sens_.load(std::memory_order_relaxed), 0.f, 1.f)); // dB per frame
    auto at = [this](int back) -> const std::vector<float>& { return bandHist_[static_cast<size_t>((bandPos_ - back) & 15)]; };

    // How much does the typical active bin grow over the same frames? A word onset enters the analysis window
    // everywhere at once, so the whole spectrum ramps together; a howl raises one band (or a few) and leaves the
    // rest where it was.
    growthScratch_.clear();
    const auto& past = at(kSpan);
    for (int k = kLo_; k <= kHi_; ++k)
        if (cur[static_cast<size_t>(k)] >= topLevelDb - 30.f) growthScratch_.push_back(cur[static_cast<size_t>(k)] - past[static_cast<size_t>(k)]);
    float typical = 0.f;
    if (growthScratch_.size() >= 12) {
        auto mid = growthScratch_.begin() + static_cast<std::ptrdiff_t>(growthScratch_.size() / 2);
        std::nth_element(growthScratch_.begin(), mid, growthScratch_.end());
        typical = std::max(0.f, *mid);
    }

    struct Hit { int bin; float level, slope; };
    std::array<Hit, 4> hits {};
    int nHits = 0;
    float strongestSlope = 0.f;
    for (int k = kLo_; k <= kHi_; ++k) {
        const size_t i = static_cast<size_t>(k);
        const float level = cur[i];
        if (level < topLevelDb - kTopWindowDb || level < kMinLevelDb || !(level >= cur[i - 1] && level >= cur[i + 1])) continue;
        float dMin = 1e9f, dMax = -1e9f;
        for (int f = 0; f < kSpan; ++f) {
            const float d = at(f)[i] - at(f + 1)[i];
            dMin = std::min(dMin, d);
            dMax = std::max(dMax, d);
        }
        const float total = level - past[i];
        if (dMin < need || dMax > 1.7f * dMin + 1.f || total < kSpan * need + 5.f || total - typical < 9.f || looksHarmonic(k)) continue;
        const Hit h { k, level, total / kSpan * 3.75f }; // slope in dB per ~10 ms
        if (nHits < 4) hits[static_cast<size_t>(nHits++)] = h;
        else {
            size_t weakest = 0;
            for (size_t n = 1; n < hits.size(); ++n) if (hits[n].level < hits[weakest].level) weakest = n;
            if (h.level > hits[weakest].level) hits[weakest] = h;
        }
    }
    for (int n = 0; n < nHits; ++n) {
        detected(hits[static_cast<size_t>(n)].bin);
        strongestSlope = std::max(strongestSlope, hits[static_cast<size_t>(n)].slope);
    }
    if (nHits > 0) raiseGuard(strongestSlope);
}

void FeedbackSuppressor::runawayGuard() noexcept
{
    // level in the time domain, averaged over the last 8 hops (21 ms): one hop is shorter than a low voice's pitch period
    tPos_ = (tPos_ + 1) & 31;
    tPow_[static_cast<size_t>(tPos_)] = static_cast<float>(hopSq_ / kHop);
    hopSq_ = 0;
    float sum = 0.f;
    for (int i = 0; i < 8; ++i) sum += tPow_[static_cast<size_t>((tPos_ - i) & 31)];
    const float rmsDb = 10.0f * std::log10(sum / 8.f + 1e-12f);
    hotCount_ = rmsDb > -6.f ? hotCount_ + 1 : 0;

    if (hotCount_ >= 30 && frame_ >= refractoryUntil_) { // about 80 ms
        guardTargetDb_ = std::min(kGuardMaxDb + 10.f, guardTargetDb_ + 6.f);
        lastRunaway_ = frame_;
        refractoryUntil_ = frame_ + 16;
    } else if (frame_ - lastRunaway_ > guardHoldFrames_) {
        float notchSum = 0.f;
        for (const auto& sl : slots_) if (sl.active) notchSum += sl.curCut;
        const float floorNow = std::max(0.f, guardFloorDb_ - std::max(0.f, notchSum - floorNotchSum_));
        guardTargetDb_ = std::max(floorNow, guardTargetDb_ - guardReleasePerHop_);
        guardFloorDb_ = std::max(0.f, guardFloorDb_ - guardFloorDecayPerHop_);
    }
    guardTargetLin_ = std::pow(10.0f, -guardTargetDb_ / 20.0f);

    const float guardNow = -20.0f * std::log10(std::max(guardLin_, 1e-6f));
    guardOut_.store(guardNow, std::memory_order_relaxed);
    float cuts = 0.f;
    bool any = guardNow > 0.5f;
    for (const auto& sl : slots_)
        if (sl.active) { any = true; cuts += sl.curCut; }
    cuts += guardNow;
    anyActive_ = any;
    cutSum_.store(cuts, std::memory_order_relaxed);
    if (cuts > peakCut_.load(std::memory_order_relaxed)) peakCut_.store(cuts, std::memory_order_relaxed);
}

} // namespace vc
