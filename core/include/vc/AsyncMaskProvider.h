#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>

namespace vc {

// Contract between the audio thread and a neural mask estimator.
// Both calls are made from the audio thread and must be wait-free.
class IMaskProvider {
public:
    virtual ~IMaskProvider() = default;
    // Per-bin features: power in dBFS of the amplitude-normalised spectrum.
    virtual void submit(const float* features, int nBins) noexcept = 0;
    // Copies the newest completed mask (gains in 0..1) and returns true only if
    // it is newer than the one returned by the previous call.
    virtual bool fetch(float* mask, int nBins) noexcept = 0;
};

// Runs infer() on a worker thread. The audio thread only touches seqlocked
// single-slot mailboxes, so a slow or stalled model can never block audio:
// the mailbox is overwritten and fetch() simply reports "nothing new".
class AsyncMaskProvider : public IMaskProvider {
public:
    explicit AsyncMaskProvider(int nBins);
    ~AsyncMaskProvider() override;

    void start();
    void stop(); // idempotent; derived classes must call it from their own destructor

    void submit(const float* features, int nBins) noexcept override;
    bool fetch(float* mask, int nBins) noexcept override;

    uint32_t lastInferUs() const { return lastUs_.load(std::memory_order_relaxed); }
    uint32_t maxInferUs() const { return maxUs_.load(std::memory_order_relaxed); }
    uint32_t inferCount() const { return count_.load(std::memory_order_relaxed); }

protected:
    // Worker thread. Return false to drop the result.
    virtual bool infer(const float* features, float* mask) = 0;
    virtual void onWorkerStart() {}

private:
    void run();

    int nBins_;
    std::unique_ptr<std::atomic<float>[]> in_, out_;
    std::atomic<uint32_t> inSeq_ { 0 }, outSeq_ { 0 };
    uint32_t lastFetched_ = 0; // audio thread only
    std::atomic<bool> running_ { false };
    std::thread th_;
    std::atomic<uint32_t> lastUs_ { 0 }, maxUs_ { 0 }, count_ { 0 };
};

} // namespace vc
