#include "vc/AsyncMaskProvider.h"

#include <algorithm>
#include <chrono>
#include <vector>

namespace vc {

AsyncMaskProvider::AsyncMaskProvider(int nBins)
    : nBins_(nBins)
    , in_(new std::atomic<float>[static_cast<size_t>(nBins)])
    , out_(new std::atomic<float>[static_cast<size_t>(nBins)])
{
    for (int i = 0; i < nBins; ++i) {
        in_[i].store(0.0f, std::memory_order_relaxed);
        out_[i].store(1.0f, std::memory_order_relaxed);
    }
}

AsyncMaskProvider::~AsyncMaskProvider() { stop(); }

void AsyncMaskProvider::start()
{
    if (running_.exchange(true)) return;
    th_ = std::thread([this] { run(); });
}

void AsyncMaskProvider::stop()
{
    if (!running_.exchange(false)) return;
    if (th_.joinable()) th_.join();
}

void AsyncMaskProvider::submit(const float* f, int n) noexcept
{
    if (n != nBins_) return;
    const uint32_t s = inSeq_.load(std::memory_order_relaxed);
    inSeq_.store(s + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    for (int i = 0; i < n; ++i) in_[i].store(f[i], std::memory_order_relaxed);
    inSeq_.store(s + 2, std::memory_order_release);
}

bool AsyncMaskProvider::fetch(float* m, int n) noexcept
{
    if (n != nBins_) return false;
    const uint32_t s1 = outSeq_.load(std::memory_order_acquire);
    if ((s1 & 1u) || s1 == lastFetched_) return false;
    for (int i = 0; i < n; ++i) m[i] = out_[i].load(std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_acquire);
    if (outSeq_.load(std::memory_order_relaxed) != s1) return false; // torn, try next hop
    lastFetched_ = s1;
    return true;
}

void AsyncMaskProvider::run()
{
    onWorkerStart();
    std::vector<float> feat(static_cast<size_t>(nBins_)), mask(static_cast<size_t>(nBins_));
    uint32_t last = 0;
    while (running_.load(std::memory_order_relaxed)) {
        const uint32_t s1 = inSeq_.load(std::memory_order_acquire);
        if ((s1 & 1u) || s1 == last) {
            std::this_thread::sleep_for(std::chrono::microseconds(100));
            continue;
        }
        for (int i = 0; i < nBins_; ++i) feat[static_cast<size_t>(i)] = in_[i].load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (inSeq_.load(std::memory_order_relaxed) != s1) continue; // torn
        last = s1;

        const auto t0 = std::chrono::steady_clock::now();
        const bool ok = infer(feat.data(), mask.data());
        const auto us = static_cast<uint32_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count());
        lastUs_.store(us, std::memory_order_relaxed);
        if (us > maxUs_.load(std::memory_order_relaxed)) maxUs_.store(us, std::memory_order_relaxed);
        count_.fetch_add(1, std::memory_order_relaxed);
        if (!ok) continue;

        const uint32_t o = outSeq_.load(std::memory_order_relaxed);
        outSeq_.store(o + 1, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        for (int i = 0; i < nBins_; ++i) out_[i].store(std::clamp(mask[static_cast<size_t>(i)], 0.0f, 1.0f), std::memory_order_relaxed);
        outSeq_.store(o + 2, std::memory_order_release);
    }
}

} // namespace vc
