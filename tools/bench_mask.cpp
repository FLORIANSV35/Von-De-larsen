// Measures how a mask provider keeps up with the audio hop rate.
//   bench_mask                 -> dummy provider (sanity check of the mailbox)
//   bench_mask model.mlmodelc  -> CoreML / Neural Engine model (macOS)
// Submits one frame every hop (2.67 ms at 48 kHz) for 10 s, then reports how
// many frames the model could consume and its inference time.
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

#include "vc/CoreMlMaskProvider.h"
#include "vc/SpectralDenoiser.h"

namespace {
class Dummy : public vc::AsyncMaskProvider {
public:
    Dummy() : AsyncMaskProvider(vc::SpectralDenoiser::kBins) { start(); }
    ~Dummy() override { stop(); }
protected:
    bool infer(const float*, float* m) override
    {
        for (int i = 0; i < vc::SpectralDenoiser::kBins; ++i) m[i] = 1.f;
        return true;
    }
};
} // namespace

int main(int argc, char** argv)
{
    std::unique_ptr<vc::AsyncMaskProvider> p;
    if (argc > 1) {
        std::string err;
        p = vc::makeCoreMlMaskProvider(argv[1], vc::SpectralDenoiser::kBins, err);
        if (!p) { std::fprintf(stderr, "cannot load model: %s\n", err.c_str()); return 1; }
    } else {
        p = std::make_unique<Dummy>();
    }

    const auto hop = std::chrono::microseconds(static_cast<long>(1e6 * vc::SpectralDenoiser::kHop / 48000.0));
    std::vector<float> f(vc::SpectralDenoiser::kBins, -60.f), m(f.size());
    std::this_thread::sleep_for(std::chrono::seconds(1)); // ANE warm-up
    const uint32_t base = p->inferCount();
    int submitted = 0, fresh = 0;
    auto next = std::chrono::steady_clock::now();
    for (int i = 0; i < 3750; ++i) { // 10 s of hops
        p->submit(f.data(), static_cast<int>(f.size()));
        ++submitted;
        if (p->fetch(m.data(), static_cast<int>(m.size()))) ++fresh;
        next += hop;
        std::this_thread::sleep_until(next);
    }
    const uint32_t done = p->inferCount() - base;
    std::printf("hops submitted : %d\n", submitted);
    std::printf("inferences     : %u (%.0f%% of hops)\n", done, 100.0 * done / submitted);
    std::printf("fresh masks    : %d\n", fresh);
    std::printf("last / max inference: %u / %u us  (hop = %lld us)\n", p->lastInferUs(), p->maxInferUs(), static_cast<long long>(hop.count()));
    std::printf(done >= 0.95 * submitted ? "OK: model keeps up with the hop rate\n"
                                         : "WARNING: model is slower than the hop rate, masks will be stale\n");
    return 0;
}
