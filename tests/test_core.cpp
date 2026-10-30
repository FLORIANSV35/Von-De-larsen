// Portable DSP checks: perfect reconstruction + latency, noise reduction,
// howl suppression in a simulated PA loop, no false positives on voice,
// and the async mask mailbox.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <random>
#include <thread>
#include <vector>

#include "vc/AsyncMaskProvider.h"
#include "vc/FeedbackSuppressor.h"
#include "vc/SpectralDenoiser.h"

namespace {
int failures = 0;
void check(bool ok, const char* what, double value = 0)
{
    std::printf("[%s] %s (%.2f)\n", ok ? " ok " : "FAIL", what, value);
    if (!ok) ++failures;
}
double db(double x) { return 10.0 * std::log10(x + 1e-20); }
constexpr double kPi = 3.14159265358979323846;
constexpr double kFs = 48000;

// Syllable-gated harmonic signal, a crude voice stand-in.
std::vector<float> voice(int n, double f0, float amp, bool gated)
{
    std::vector<float> v(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        const double t = i / kFs;
        const double f = f0 * (1.0 + 0.01 * std::sin(2 * kPi * 5.5 * t)); // vibrato
        static double ph = 0;
        ph += 2 * kPi * f / kFs;
        double s = 0;
        for (int h = 1; h <= 20; ++h) s += std::sin(h * ph) / h;
        const double env = gated ? (std::fmod(t, 0.5) < 0.3 ? 1.0 : 0.0) : 1.0;
        v[static_cast<size_t>(i)] = static_cast<float>(amp * env * s * 0.5);
    }
    return v;
}

void testReconstructionAndLatency()
{
    vc::SpectralDenoiser dn;
    dn.prepare(kFs);
    dn.setReductionDb(0); // unity gain: must be a pure delay
    std::mt19937 rng(1);
    std::normal_distribution<float> nd(0.f, 0.1f);
    const int n = 20000;
    std::vector<float> x(n), y(n);
    for (auto& s : x) s = nd(rng);
    for (int i = 0; i < n; i += 100) dn.process(&x[static_cast<size_t>(i)], &y[static_cast<size_t>(i)], 100); // odd host block size
    double err = 0, sig = 0;
    for (int i = vc::SpectralDenoiser::kN; i < n; ++i) {
        const double d = y[static_cast<size_t>(i)] - x[static_cast<size_t>(i - dn.latencySamples())];
        err += d * d;
        sig += x[static_cast<size_t>(i - dn.latencySamples())] * x[static_cast<size_t>(i - dn.latencySamples())];
    }
    check(db(sig / err) > 60, "unity gain = pure delay of latencySamples() (SNR dB)", db(sig / err));
    check(dn.latencySamples() / kFs * 1000 <= 10.0, "latency <= 10 ms (ms)", dn.latencySamples() / kFs * 1000);
}

void testDenoise()
{
    const int n = static_cast<int>(kFs * 6);
    auto clean = voice(n, 140, 0.3f, true);
    std::mt19937 rng(2);
    std::normal_distribution<float> nd(0.f, 0.06f);
    std::vector<float> noisy(clean), out(static_cast<size_t>(n));
    for (auto& s : noisy) s += nd(rng);

    vc::SpectralDenoiser dn;
    dn.prepare(kFs);
    dn.setReductionDb(18);
    dn.process(noisy.data(), out.data(), n);

    const int L = dn.latencySamples(), skip = static_cast<int>(kFs); // let the tracker settle
    double eIn = 0, eOut = 0, sig = 0, gapIn = 0, gapOut = 0;
    for (int i = skip; i < n; ++i) {
        const double c = clean[static_cast<size_t>(i - L)];
        const double dIn = noisy[static_cast<size_t>(i - L)] - c, dOut = out[static_cast<size_t>(i)] - c;
        eIn += dIn * dIn;
        eOut += dOut * dOut;
        sig += c * c;
        if (c == 0.0) { gapIn += noisy[static_cast<size_t>(i - L)] * noisy[static_cast<size_t>(i - L)]; gapOut += out[static_cast<size_t>(i)] * out[static_cast<size_t>(i)]; }
    }
    check(db(eIn / eOut) >= 3.0, "denoiser improves SNR by >= 3 dB (dB)", db(eIn / eOut));
    check(db(gapIn / gapOut) >= 10.0, "noise-only gaps attenuated >= 10 dB (dB)", db(gapIn / gapOut));
}

// Resonant room path: band-pass at 1.5 kHz with +3 dB peak gain, 10 ms delay.
struct Room {
    double b0, b2, a1, a2, z1 = 0, z2 = 0, gain;
    Room(double f, double q, double g) : gain(g)
    {
        const double w = 2 * kPi * f / kFs, al = std::sin(w) / (2 * q), a0 = 1 + al;
        b0 = al / a0; b2 = -al / a0; a1 = -2 * std::cos(w) / a0; a2 = (1 - al) / a0;
    }
    float process(float x)
    {
        const double y = b0 * x + z1;
        z1 = -a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return static_cast<float>(y * gain);
    }
};

struct LoopResult { double tailDb; int notches; double freq; };

LoopResult runLoop(bool suppress)
{
    const int blk = 64, delay = 480, total = static_cast<int>(kFs * 8);
    vc::FeedbackSuppressor fb;
    fb.prepare(kFs);
    Room room(1500, 8, 1.4);
    std::mt19937 rng(3);
    std::normal_distribution<float> nd(0.f, 0.001f); // -60 dBFS noise floor seeds the howl
    std::vector<float> spk(static_cast<size_t>(total + delay), 0.f), mic(blk), outTail;
    double e = 0;
    int cnt = 0;
    for (int b = 0; b < total; b += blk) {
        for (int i = 0; i < blk; ++i) mic[static_cast<size_t>(i)] = nd(rng) + room.process(spk[static_cast<size_t>(b + i)]);
        if (suppress) fb.process(mic.data(), blk);
        for (int i = 0; i < blk; ++i) {
            const float o = std::clamp(mic[static_cast<size_t>(i)], -1.f, 1.f);
            spk[static_cast<size_t>(b + i + delay)] = o;
            if (b + i >= total - static_cast<int>(kFs)) { e += static_cast<double>(o) * o; ++cnt; }
        }
    }
    double f = 0;
    for (int s = 0; s < vc::FeedbackSuppressor::kMaxNotches; ++s)
        if (fb.notchCutDb(s) > 0) f = fb.notchFrequency(s);
    return { db(e / cnt), fb.activeNotches(), f };
}

void testFeedback()
{
    const auto off = runLoop(false);
    const auto on = runLoop(true);
    check(off.tailDb > -20, "loop without suppressor howls (tail dBFS)", off.tailDb);
    check(on.tailDb < -40, "loop with suppressor stays quiet (tail dBFS)", on.tailDb);
    check(on.notches >= 1 && on.notches <= 3, "few notches used (count)", on.notches);
    check(std::fabs(on.freq - 1500) < 400, "notch lands near the resonance (Hz)", on.freq);
}

void testVoiceNotTouched()
{
    for (double f0 : { 110.0, 220.0, 440.0 }) {
        const int n = static_cast<int>(kFs * 6);
        auto x = voice(n, f0, 0.3f, false);
        auto y = x;
        vc::FeedbackSuppressor fb;
        fb.prepare(kFs);
        fb.process(y.data(), n);
        char msg[96];
        std::snprintf(msg, sizeof msg, "sustained voice f0=%.0f Hz triggers no notch (count)", f0);
        check(fb.activeNotches() == 0, msg, fb.activeNotches());
    }
}

class DummyProvider : public vc::AsyncMaskProvider {
public:
    DummyProvider() : AsyncMaskProvider(vc::SpectralDenoiser::kBins) { start(); }
    ~DummyProvider() override { stop(); }
protected:
    bool infer(const float*, float* m) override
    {
        for (int i = 0; i < vc::SpectralDenoiser::kBins; ++i) m[i] = 0.5f;
        return true;
    }
};

void testAsyncMask()
{
    DummyProvider p;
    vc::SpectralDenoiser dn;
    dn.prepare(kFs);
    dn.setReductionDb(30);
    dn.setMaskProvider(&p);
    std::vector<float> x(static_cast<size_t>(kFs), 0.1f), y(x.size());
    bool used = false;
    for (size_t i = 0; i + 128 <= x.size(); i += 128) {
        dn.process(&x[i], &y[i], 128);
        std::this_thread::sleep_for(std::chrono::microseconds(500)); // let the worker run
        used = used || dn.usingNeuralMask();
    }
    check(used, "denoiser picks up masks from the async worker");
    check(p.inferCount() > 10, "worker ran inferences (count)", p.inferCount());
}
} // namespace

int main()
{
    testReconstructionAndLatency();
    testDenoise();
    testFeedback();
    testVoiceNotTouched();
    testAsyncMask();
    std::printf(failures ? "\n%d FAILED\n" : "\nall passed\n", failures);
    return failures ? 1 : 0;
}
