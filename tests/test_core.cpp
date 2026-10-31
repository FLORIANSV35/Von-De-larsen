// Portable DSP checks: perfect reconstruction + latency, noise reduction,
// howl suppression in a simulated PA loop, no false positives on voice,
// and the async mask mailbox.
#include <algorithm>
#include <functional>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <random>
#include <thread>
#include <vector>

#include "vc/AsyncMaskProvider.h"
#include "vc/AutoMixer.h"
#include "vc/BypassMixer.h"
#include "vc/FeedbackCanceller.h"
#include "vc/FeedbackSuppressor.h"
#include "vc/RingOut.h"
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

struct LoopResult { double tailDb; int notches; double freq; std::vector<double> freqs; vc::FeedbackSuppressor::Stats stats; };

LoopResult runLoop(bool suppress, std::vector<double> freqs = { 1500 })
{
    const int blk = 64, delay = 480, total = static_cast<int>(kFs * 8);
    vc::FeedbackSuppressor fb;
    fb.prepare(kFs);
    std::vector<Room> rooms;
    for (double f : freqs) rooms.emplace_back(f, 8, 1.4);
    std::mt19937 rng(3);
    std::normal_distribution<float> nd(0.f, 0.001f); // -60 dBFS noise floor seeds the howl
    std::vector<float> spk(static_cast<size_t>(total + delay), 0.f), mic(blk), outTail;
    double e = 0;
    int cnt = 0;
    for (int b = 0; b < total; b += blk) {
        for (int i = 0; i < blk; ++i) mic[static_cast<size_t>(i)] = nd(rng) + [&] { float a = 0; for (auto& r : rooms) a += r.process(spk[static_cast<size_t>(b + i)]); return a; }();
        if (suppress) fb.process(mic.data(), blk);
        for (int i = 0; i < blk; ++i) {
            const float o = std::clamp(mic[static_cast<size_t>(i)], -1.f, 1.f);
            spk[static_cast<size_t>(b + i + delay)] = o;
            if (b + i >= total - static_cast<int>(kFs)) { e += static_cast<double>(o) * o; ++cnt; }
        }
    }
    double f = 0;
    std::vector<double> all;
    for (int s = 0; s < vc::FeedbackSuppressor::kMaxNotches; ++s)
        if (fb.notchCutDb(s) > 0) { f = fb.notchFrequency(s); all.push_back(f); }
    return { db(e / cnt), fb.activeNotches(), f, all, fb.stats() };
}

void testFeedback()
{
    const auto off = runLoop(false);
    const auto on = runLoop(true);
    check(off.tailDb > -20, "loop without suppressor howls (tail dBFS)", off.tailDb);
    check(on.tailDb < -40, "loop with suppressor stays quiet (tail dBFS)", on.tailDb);
    check(on.notches >= 1 && on.notches <= 3, "few notches used (count)", on.notches);
    check(std::fabs(on.freq - 1500) < 400, "notch lands near the resonance (Hz)", on.freq);
    check(on.stats.events >= 1 && on.stats.events <= 4, "stats: howls caught (count)", on.stats.events);
    check(on.stats.avoidedSeconds > 3 && on.stats.avoidedSeconds <= 8.01, "stats: time with a notch holding (s)", on.stats.avoidedSeconds);
    check(on.stats.cutDb > 3, "stats: current total cut (dB)", on.stats.cutDb);
    check(on.stats.peakCutDb >= on.stats.cutDb, "stats: peak cut is at least the current cut (dB)", on.stats.peakCutDb);
    check(std::fabs(on.stats.lastFreqHz - 1500) < 400, "stats: last caught frequency (Hz)", on.stats.lastFreqHz);
}

void testStatsReset()
{
    vc::FeedbackSuppressor fb;
    fb.prepare(kFs);
    check(fb.stats().events == 0 && fb.stats().avoidedSeconds == 0, "stats start at zero");
    std::vector<float> quiet(512, 0.f);
    for (int i = 0; i < 50; ++i) fb.process(quiet.data(), 512);
    check(fb.stats().events == 0 && fb.stats().avoidedSeconds == 0, "silence catches nothing and counts no time");
    fb.resetStats();
    check(fb.stats().peakCutDb == 0, "resetStats clears the record");
}

void testSimultaneousNotches()
{
    const std::vector<double> res = { 900, 2400, 5200 };
    const auto off = runLoop(false, res);
    const auto on = runLoop(true, res);
    check(off.tailDb > -20, "3 resonances: howls without suppressor (tail dBFS)", off.tailDb);
    check(on.tailDb < -40, "3 resonances: quiet with suppressor (tail dBFS)", on.tailDb);
    check(on.notches >= 3 && on.notches <= 5, "3 resonances: notches active at the same time (count)", on.notches);
    check(on.stats.events >= 3, "3 resonances: stats count at least 3 catches", on.stats.events);
    for (double r : res) {
        bool found = false;
        for (double f : on.freqs) found = found || std::fabs(f - r) < 150;
        char msg[64];
        std::snprintf(msg, sizeof msg, "a notch sits on the %.0f Hz resonance", r);
        check(found, msg);
    }
}

// Constant-mask provider whose value is chosen by the test.
class ConstProvider : public vc::AsyncMaskProvider {
public:
    explicit ConstProvider(float v) : AsyncMaskProvider(vc::SpectralDenoiser::kBins), v_(v) { start(); }
    ~ConstProvider() override { stop(); }
protected:
    bool infer(const float*, float* m) override
    {
        for (int i = 0; i < vc::SpectralDenoiser::kBins; ++i) m[i] = v_;
        return true;
    }
private:
    float v_;
};

double noiseOnlyRmsDb(float nnValue, float mix)
{
    ConstProvider p(nnValue);
    vc::SpectralDenoiser dn;
    dn.prepare(kFs);
    dn.setReductionDb(30);
    dn.setNeuralMix(mix);
    if (mix >= 0) dn.setMaskProvider(&p);
    std::mt19937 rng(5);
    std::normal_distribution<float> nd(0.f, 0.05f);
    std::vector<float> x(static_cast<size_t>(kFs * 2)), y(x.size());
    for (auto& v : x) v = nd(rng);
    double e = 0;
    int cnt = 0;
    for (size_t i = 0; i + 128 <= x.size(); i += 128) {
        dn.process(&x[i], &y[i], 128);
        std::this_thread::sleep_for(std::chrono::microseconds(300));
        if (i > x.size() / 2) for (size_t j = i; j < i + 128; ++j) { e += y[j] * y[j]; ++cnt; }
    }
    return db(e / cnt);
}

void testHybridMix()
{
    // Noise only. The classic Wiener gain removes most of it; a network that says "voice" (mask = 1) keeps it.
    const double inputDb = db(0.05 * 0.05);
    const double classic = noiseOnlyRmsDb(1.0f, 0.0f);
    const double half = noiseOnlyRmsDb(1.0f, 0.5f);
    const double full = noiseOnlyRmsDb(1.0f, 1.0f);
    check(classic < inputDb - 6.0, "hybrid: classic path removes noise (dB below input)", classic - inputDb);
    check(classic < half && half < full, "hybrid: output level rises monotonically with neural share (dB)", half - classic);
    check(std::fabs(full - inputDb) < 1.5, "hybrid: mix 1 follows the neural mask (dB vs input)", full - inputDb);
    const double ignored = noiseOnlyRmsDb(0.1f, 0.0f);
    check(std::fabs(ignored - classic) < 0.5, "hybrid: mix 0 ignores the neural mask (dB diff)", ignored - classic);
}

void testHintCannotBlockHowl()
{
    const std::vector<double> res = { 1500 };
    for (float h : { 0.0f, 0.5f, 1.0f }) {
        // run the loop with a fixed voice hint on the suppressor
        const int blk = 64, delay = 480, total = static_cast<int>(kFs * 8);
        vc::FeedbackSuppressor fb;
        fb.prepare(kFs);
        std::vector<float> hint(129, h);
        fb.setVoiceHint(hint.data());
        Room room(1500, 8, 1.4);
        std::mt19937 rng(3);
        std::normal_distribution<float> nd(0.f, 0.001f);
        std::vector<float> spk(static_cast<size_t>(total + delay), 0.f), mic(blk);
        double e = 0;
        int cnt = 0;
        for (int b = 0; b < total; b += blk) {
            for (int i = 0; i < blk; ++i) mic[static_cast<size_t>(i)] = nd(rng) + room.process(spk[static_cast<size_t>(b + i)]);
            fb.process(mic.data(), blk);
            for (int i = 0; i < blk; ++i) {
                const float o = std::clamp(mic[static_cast<size_t>(i)], -1.f, 1.f);
                spk[static_cast<size_t>(b + i + delay)] = o;
                if (b + i >= total - static_cast<int>(kFs)) { e += static_cast<double>(o) * o; ++cnt; }
            }
        }
        char msg[96];
        std::snprintf(msg, sizeof msg, "howl still suppressed with voice hint = %.1f (tail dBFS)", h);
        check(db(e / cnt) < -40, msg, db(e / cnt));
    }
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

// Energy of the denoiser output that belongs to input time [t0, t1) (the output lags by the latency).
double outEnergy(const std::vector<float>& o, double t0, double t1)
{
    double e = 0;
    for (long i = static_cast<long>(t0 * kFs); i < static_cast<long>(t1 * kFs); ++i) e += static_cast<double>(o[static_cast<size_t>(i + 256)]) * o[static_cast<size_t>(i + 256)];
    return e;
}

void testLowLatency()
{
    // High CPU: half-length window, a quarter of the hop. The latency halves and the filtering still works.
    vc::SpectralDenoiser dn;
    dn.setLowLatency(true);
    dn.prepare(kFs);
    dn.setReductionDb(0);
    std::mt19937 rng(1);
    std::normal_distribution<float> nd(0.f, 0.1f);
    const int n = 20000;
    std::vector<float> x(n), y(n);
    for (auto& s : x) s = nd(rng);
    for (int i = 0; i < n; i += 100) dn.process(&x[static_cast<size_t>(i)], &y[static_cast<size_t>(i)], 100);
    double err = 0, sig = 0;
    for (int i = 2 * vc::SpectralDenoiser::kN; i < n; ++i) {
        const double d = y[static_cast<size_t>(i)] - x[static_cast<size_t>(i - dn.latencySamples())];
        err += d * d;
        sig += x[static_cast<size_t>(i - dn.latencySamples())] * x[static_cast<size_t>(i - dn.latencySamples())];
    }
    check(dn.latencySamples() == vc::SpectralDenoiser::kN / 2, "high cpu: latency is half a window (samples)", dn.latencySamples());
    check(db(sig / err) > 60, "high cpu: unity gain = pure delay (SNR dB)", db(sig / err));

    const int m = static_cast<int>(kFs * 6);
    auto clean = voice(m, 140, 0.3f, true);
    std::mt19937 rng2(2);
    std::normal_distribution<float> nd2(0.f, 0.06f);
    std::vector<float> noisy(clean), out(static_cast<size_t>(m));
    for (auto& s : noisy) s += nd2(rng2);
    dn.setReductionDb(18);
    dn.process(noisy.data(), out.data(), m);
    const int L = dn.latencySamples(), skip = static_cast<int>(kFs);
    double eIn = 0, eOut = 0, gapIn = 0, gapOut = 0;
    for (int i = skip; i < m; ++i) {
        const double c = clean[static_cast<size_t>(i - L)];
        const double dIn = noisy[static_cast<size_t>(i - L)] - c, dOut = out[static_cast<size_t>(i)] - c;
        eIn += dIn * dIn;
        eOut += dOut * dOut;
        if (c == 0.0) { gapIn += noisy[static_cast<size_t>(i - L)] * noisy[static_cast<size_t>(i - L)]; gapOut += out[static_cast<size_t>(i)] * out[static_cast<size_t>(i)]; }
    }
    check(db(eIn / eOut) >= 3.0, "high cpu: denoiser improves SNR by >= 3 dB (dB)", db(eIn / eOut));
    check(db(gapIn / gapOut) >= 10.0, "high cpu: noise-only gaps attenuated >= 10 dB (dB)", db(gapIn / gapOut));

    dn.setLowLatency(false); // switching back restores the normal latency
    check(dn.latencySamples() == vc::SpectralDenoiser::kN, "high cpu: switching back restores the latency (samples)", dn.latencySamples());
}

void testGateKeepsWeakSyllables()
{
    // A weak voice over a noticeable floor, dipping to a third of its level between syllables: the classic
    // detector must not decide "no voice" in mid-phrase and drop the level by the gate depth.
    const int n = static_cast<int>(kFs * 10);
    auto v = voice(n, 140, 0.03f, false);
    std::mt19937 rng(11);
    std::normal_distribution<float> nd(0.f, 0.01f);
    std::vector<float> x(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) x[static_cast<size_t>(i)] = v[static_cast<size_t>(i)] * (std::fmod(i / kFs, 0.6) < 0.35 ? 1.f : 0.3f) + nd(rng);
    auto run = [&](bool gate) {
        vc::SpectralDenoiser dn;
        dn.prepare(kFs);
        dn.setReductionDb(15);
        dn.setNeuralMix(0.f);
        dn.setGate(gate, 20);
        std::vector<float> o(static_cast<size_t>(n) + 512, 0.f);
        for (int i = 0; i + 128 <= n; i += 128) dn.process(&x[static_cast<size_t>(i)], &o[static_cast<size_t>(i)], 128);
        return o;
    };
    const auto off = run(false), on = run(true);
    int windows = 0, dipped = 0;
    for (double t = 3.0; t < 9.0; t += 0.02, ++windows)
        if (db(outEnergy(on, t, t + 0.02) / outEnergy(off, t, t + 0.02)) < -3.0) ++dipped;
    check(dipped * 100 < windows, "gate: a weak voice is not cut in mid-phrase (% of 20 ms windows more than 3 dB down)", 100.0 * dipped / windows);
}

void testVoiceGate()
{
    const int n = static_cast<int>(kFs * 12);
    auto v = voice(n, 140, 0.3f, false);
    std::mt19937 rng(9);
    std::normal_distribution<float> nd(0.f, 0.01f); // -40 dBFS floor
    std::vector<float> x(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) x[static_cast<size_t>(i)] = v[static_cast<size_t>(i)] * (std::fmod(i / kFs, 2.0) < 0.5 ? 1.f : 0.f) + nd(rng); // 0.5 s of voice every 2 s

    auto run = [&](bool gate, std::vector<char>* flags) {
        vc::SpectralDenoiser dn;
        dn.prepare(kFs);
        dn.setReductionDb(15);
        dn.setGate(gate, 20);
        std::vector<float> o(static_cast<size_t>(n) + 512, 0.f);
        for (int i = 0; i + 128 <= n; i += 128) {
            dn.process(&x[static_cast<size_t>(i)], &o[static_cast<size_t>(i)], 128);
            if (flags) flags->push_back(dn.voiceDetected());
        }
        return o;
    };
    std::vector<char> fl;
    const auto off = run(false, nullptr);
    const auto on = run(true, &fl);

    double gap = 0, voiceDb = 0, onset = 0;
    int bursts = 0;
    bool flagsOk = true;
    for (double b : { 4.0, 6.0, 8.0, 10.0 }) {
        gap += db(outEnergy(on, b + 1.0, b + 1.9) / outEnergy(off, b + 1.0, b + 1.9));
        voiceDb += db(outEnergy(on, b + 0.1, b + 0.45) / outEnergy(off, b + 0.1, b + 0.45));
        onset += db(outEnergy(on, b, b + 0.04) / outEnergy(off, b, b + 0.04));
        flagsOk = flagsOk && fl[static_cast<size_t>((b + 0.3) * kFs / 128)] && !fl[static_cast<size_t>((b + 1.5) * kFs / 128)];
        ++bursts;
    }
    check(gap / bursts < -12, "gate: silence between bursts is attenuated further (dB vs gate off)", gap / bursts);
    check(std::fabs(voiceDb / bursts) < 1.5, "gate: voice itself is untouched (dB vs gate off)", voiceDb / bursts);
    check(onset / bursts > -3, "gate: word onsets are not clipped (dB vs gate off, first 40 ms)", onset / bursts);
    check(flagsOk, "gate: voiceDetected() true during voice, false in a long silence");

    // gate off must be a no-op
    const auto off2 = run(false, nullptr);
    bool same = true;
    for (size_t i = 0; i < off.size(); ++i) same = same && off[i] == off2[i];
    check(same, "gate off is deterministic and adds nothing");
}

void testVoiceGateNeural()
{
    // Noise only. A neural mask that says "voice" keeps the gate open, one that says "noise" closes it.
    auto run = [&](float nnValue, bool gate, bool* voiceFlag) {
        ConstProvider p(nnValue);
        vc::SpectralDenoiser dn;
        dn.prepare(kFs);
        dn.setReductionDb(0);
        dn.setNeuralMix(1.f);
        dn.setMaskProvider(&p);
        dn.setGate(gate, 20);
        std::mt19937 rng(4);
        std::normal_distribution<float> nd(0.f, 0.02f);
        std::vector<float> x(static_cast<size_t>(kFs * 5)), y(x.size());
        for (auto& s : x) s = nd(rng);
        double e = 0;
        for (size_t i = 0; i + 128 <= x.size(); i += 128) {
            dn.process(&x[i], &y[i], 128);
            std::this_thread::sleep_for(std::chrono::microseconds(300));
            if (i > x.size() * 4 / 5) for (size_t j = i; j < i + 128; ++j) e += static_cast<double>(y[j]) * y[j];
        }
        if (voiceFlag) *voiceFlag = dn.voiceDetected();
        return e;
    };
    bool vOpen = false, vClosed = true;
    const double eOpen = run(0.9f, true, &vOpen), eOpenOff = run(0.9f, false, nullptr);
    const double eClosed = run(0.02f, true, &vClosed), eClosedOff = run(0.02f, false, nullptr);
    check(vOpen, "gate: a 'voice' neural mask keeps the gate open");
    check(!vClosed, "gate: a 'noise' neural mask closes the gate");
    check(std::fabs(db(eOpen / eOpenOff)) < 1.0, "gate: open gate changes nothing (dB)", db(eOpen / eOpenOff));
    check(db(eClosed / eClosedOff) < -17, "gate: closed gate drops the level by about its depth (dB)", db(eClosed / eClosedOff));
}

// ---------------------------------------------------------------- automixer (Dugan style gain sharing)
struct MixRun {
    std::vector<std::array<float, 4>> trace; // gains in dB at every block
    std::vector<std::vector<float>> out;
    double at(double t, int c) const { return trace[static_cast<size_t>(t * kFs / 64)][static_cast<size_t>(c)]; }
};

MixRun runMixer(const std::vector<std::vector<float>>& in, const std::function<std::array<float, 4>(double)>& scores, bool enabled, float depth = 20.f)
{
    vc::AutoMixer m;
    m.prepare(kFs);
    m.setDepthDb(depth);
    MixRun r;
    r.out = in;
    const int n = static_cast<int>(in[0].size());
    for (int i = 0; i + 64 <= n; i += 64) {
        float* ch[4];
        for (int c = 0; c < 4; ++c) ch[c] = &r.out[static_cast<size_t>(c)][static_cast<size_t>(i)];
        const auto sc = scores(i / kFs);
        m.process(ch, sc.data(), 4, 64, enabled);
        r.trace.push_back({ m.gainDb(0), m.gainDb(1), m.gainDb(2), m.gainDb(3) });
    }
    return r;
}

void testAutoMixer()
{
    const int n = static_cast<int>(kFs * 4);
    std::mt19937 rng(21);
    std::normal_distribution<float> nd(0.f, 0.01f);
    auto noise = [&] { std::vector<float> v(static_cast<size_t>(n)); for (auto& x : v) x = nd(rng); return v; };
    auto talk = [&](double f0, double from, double to) { // voice between from and to seconds, over the noise floor
        auto v = voice(n, f0, 0.3f, false);
        auto b = noise();
        for (int i = 0; i < n; ++i) { const double t = i / kFs; b[static_cast<size_t>(i)] += (t >= from && t < to) ? v[static_cast<size_t>(i)] : 0.f; }
        return b;
    };
    using Sc = std::array<float, 4>;

    // 1. one talker: unity for them, the rest fall to the floor
    {
        const auto r = runMixer({ talk(140, 0, 4), noise(), noise(), noise() }, [](double) { return Sc { 0.9f, 0.02f, 0.02f, 0.02f }; }, true);
        check(r.at(3.5, 0) > -1.0, "automix: the only talker keeps about unity gain (dB)", r.at(3.5, 0));
        check(r.at(3.5, 1) < -18 && r.at(3.5, 2) < -18 && r.at(3.5, 3) < -18, "automix: silent microphones fall to the floor (dB)", r.at(3.5, 1));
    }
    // 2. two equal talkers share the gain: -3 dB each, power adds up to one
    {
        const auto r = runMixer({ talk(140, 0, 4), talk(190, 0, 4), noise(), noise() }, [](double) { return Sc { 0.9f, 0.9f, 0.02f, 0.02f }; }, true);
        const double g0 = r.at(3.5, 0), g1 = r.at(3.5, 1);
        check(std::fabs(g0 + 3.0) < 1.5 && std::fabs(g1 + 3.0) < 1.5, "automix: two equal talkers get about -3 dB each (dB)", g0);
        check(std::fabs(std::pow(10, g0 / 10) + std::pow(10, g1 / 10) - 1.0) < 0.2, "automix: the squared gains of the talkers add up to one", std::pow(10, g0 / 10) + std::pow(10, g1 / 10));
    }
    // 3. nobody talks: gains stay shared, the number of open microphones does not add noise
    {
        const auto r = runMixer({ noise(), noise(), noise(), noise() }, [](double) { return Sc { 0.02f, 0.02f, 0.02f, 0.02f }; }, true);
        bool even = true;
        for (int c = 0; c < 4; ++c) even = even && std::fabs(r.at(3.5, c) + 6.0) < 1.5;
        check(even, "automix: four idle microphones share the gain evenly (-6 dB each)", r.at(3.5, 0));
    }
    // 4. hand-over from one talker to the next
    {
        const auto r = runMixer({ talk(140, 0, 1), talk(190, 1, 4), noise(), noise() },
                                [](double t) { return t < 1.0 ? Sc { 0.9f, 0.02f, 0.02f, 0.02f } : Sc { 0.02f, 0.9f, 0.02f, 0.02f }; }, true);
        check(r.at(0.9, 0) > -1.0, "automix: first talker has the gain before the hand-over (dB)", r.at(0.9, 0));
        check(r.at(1.1, 1) > -3.0, "automix: second talker gets the gain within 100 ms (dB)", r.at(1.1, 1));
        check(r.at(2.5, 0) < -15 && r.at(2.5, 1) > -1.0, "automix: first talker has faded out, second has unity (dB)", r.at(2.5, 0));
    }
    // 5. disabled: untouched
    {
        const auto in = std::vector<std::vector<float>> { talk(140, 0, 4), noise(), noise(), noise() };
        const auto r = runMixer(in, [](double) { return Sc { 0.9f, 0.02f, 0.02f, 0.02f }; }, false);
        bool same = true;
        for (int c = 0; c < 4; ++c) for (size_t i = 0; i < in[0].size(); ++i) same = same && r.out[static_cast<size_t>(c)][i] == in[static_cast<size_t>(c)][i];
        check(same, "automix: disabled is bit-exact");
    }
    // 6. digital silence holds the gains
    {
        auto a = talk(140, 0, 2);
        for (int i = static_cast<int>(2 * kFs); i < n; ++i) a[static_cast<size_t>(i)] = 0.f;
        std::vector<float> z(static_cast<size_t>(n), 0.f);
        for (int i = 0; i < static_cast<int>(2 * kFs); ++i) z[static_cast<size_t>(i)] = 0.f;
        auto bch = noise();
        for (int i = static_cast<int>(2 * kFs); i < n; ++i) bch[static_cast<size_t>(i)] = 0.f;
        auto c2 = bch, c3 = bch;
        const auto r = runMixer({ a, bch, c2, c3 }, [](double) { return Sc { 0.9f, 0.02f, 0.02f, 0.02f }; }, true);
        check(std::fabs(r.at(3.9, 0) - r.at(1.9, 0)) < 0.5 && std::fabs(r.at(3.9, 1) - r.at(1.9, 1)) < 0.5, "automix: gains are held during digital silence (dB change)", r.at(3.9, 1) - r.at(1.9, 1));
    }
}

// ---------------------------------------------------------------- bypass
void testBypass()
{
    constexpr int L = 256, blk = 64, n = 48000;
    std::vector<float> sine(n);
    for (int i = 0; i < n; ++i) sine[static_cast<size_t>(i)] = static_cast<float>(0.5 * std::sin(2 * kPi * 440.0 * i / kFs));

    // `gain` stands for the processing: it delays by L like the real chain and scales by `gain`
    auto run = [&](float gain, const std::function<bool(int)>& bypassAt) {
        vc::BypassMixer bm;
        bm.prepare(kFs, L);
        std::vector<float> out(n, 0.f);
        for (int i = 0; i + blk <= n; i += blk) {
            std::vector<float> blkIn(sine.begin() + i, sine.begin() + i + blk), blkOut(blk);
            const float* in[1] = { blkIn.data() };
            bm.push(in, 1, blk);
            for (int k = 0; k < blk; ++k) blkOut[static_cast<size_t>(k)] = (i + k - L >= 0 ? sine[static_cast<size_t>(i + k - L)] : 0.f) * gain;
            float* o[1] = { blkOut.data() };
            bm.mix(o, 1, blk, bypassAt(i));
            std::copy(blkOut.begin(), blkOut.end(), out.begin() + i);
        }
        return out;
    };
    // bypass off: bit-exact with the processed signal
    {
        const auto o = run(0.5f, [](int) { return false; });
        bool same = true;
        for (int i = L; i < n; ++i) same = same && o[static_cast<size_t>(i)] == 0.5f * sine[static_cast<size_t>(i - L)];
        check(same, "bypass off: output is the processed signal, bit for bit");
    }
    // bypass on (after the fade): the input delayed by the latency, exactly
    {
        const auto o = run(0.5f, [](int) { return true; });
        bool same = true;
        for (int i = 2000; i < n; ++i) same = same && o[static_cast<size_t>(i)] == sine[static_cast<size_t>(i - L)];
        check(same, "bypass on: output is the input delayed by the latency, bit for bit");
    }
    // switching on at 0.5 s and off at 0.75 s: the fade is smooth and never jumps
    {
        const auto o = run(0.1f, [](int i) { return i >= 24000 && i < 36000; });
        double maxJump = 0, ref = 0;
        for (int i = L + 1; i < n; ++i) maxJump = std::max(maxJump, static_cast<double>(std::fabs(o[static_cast<size_t>(i)] - o[static_cast<size_t>(i - 1)])));
        for (int i = 1; i < n; ++i) ref = std::max(ref, static_cast<double>(std::fabs(sine[static_cast<size_t>(i)] - sine[static_cast<size_t>(i - 1)])));
        check(maxJump < ref * 1.1, "bypass: no click when switching (largest sample step vs the clean sine's)", maxJump / ref);
    }
    // the fade lasts about 10 ms: a constant input makes the ramp easy to measure
    {
        vc::BypassMixer bm;
        bm.prepare(kFs, L);
        const int total = 9600;
        std::vector<float> out(static_cast<size_t>(total));
        for (int i = 0; i < total; i += blk) {
            std::vector<float> in(blk, 1.0f), blkOut(blk, 0.0f); // dry = 1, processed = 0 once the history is full
            const float* pin[1] = { in.data() };
            bm.push(pin, 1, blk);
            float* po[1] = { blkOut.data() };
            bm.mix(po, 1, blk, i >= 4800);
            std::copy(blkOut.begin(), blkOut.end(), out.begin() + i);
        }
        int first = 4800;
        while (first < total && out[static_cast<size_t>(first)] == 0.f) ++first;
        int last = first;
        while (last < total && out[static_cast<size_t>(last)] < 1.f) ++last;
        check(std::fabs((last - first) / kFs * 1000.0 - 10.0) < 1.5, "bypass: the cross-fade lasts about 10 ms (ms)", (last - first) / kFs * 1000.0);
    }
    // oversized block: passed through processed, no crash
    {
        vc::BypassMixer bm;
        bm.prepare(kFs, L);
        std::vector<float> big(vc::BypassMixer::kMaxBlock + 10, 0.25f);
        const float* in[1] = { big.data() };
        float* o[1] = { big.data() };
        bm.push(in, 1, static_cast<int>(big.size()));
        bm.mix(o, 1, static_cast<int>(big.size()), true);
        check(big[0] == 0.25f && big.back() == 0.25f, "bypass: an oversized block is left untouched");
    }
}

// ---------------------------------------------------------------- hard feedback loops (fast reaction)
struct HarshResult { double firstMs, audibleMs, tailDb; unsigned events; float peakGuard; };

// Several room resonances (Q 6), optional loudspeaker saturation, optional loud voice, loop gain that jumps at
// t0 = 1 s from 0.2 to `peakDb` above unity. Level is measured on the signal sent to the loudspeaker.
HarshResult runHarsh(const std::vector<double>& freqs, double peakDb, double drive, bool withVoice, int delayMs)
{
    const int blk = 64, delay = delayMs * 48, total = static_cast<int>(kFs * 6);
    const double t0 = 1.0, lin = freqs.empty() ? 0.0 : std::pow(10.0, peakDb / 20.0);
    vc::FeedbackSuppressor fb;
    fb.prepare(kFs);
    std::vector<Room> rooms;
    for (double f : freqs) rooms.emplace_back(f, 6.0, 0.2);
    std::mt19937 rng(7);
    std::normal_distribution<float> nd(0.f, 0.001f);
    std::vector<float> spk(static_cast<size_t>(total + delay), 0.f), mic(blk);
    HarshResult r { -1, 0, 0, 0, 0 };
    double tail = 0, ph = 0;
    int tailN = 0;
    for (int b = 0; b < total; b += blk) {
        const double t = b / kFs;
        if (t >= t0) for (auto& m : rooms) m.gain = lin;
        for (int i = 0; i < blk; ++i) {
            const double tt = (b + i) / kFs;
            float v = 0.f;
            if (withVoice && tt > 0.2) {
                ph += 2 * kPi * 160.0 * (1 + 0.02 * std::sin(2 * kPi * 5.5 * tt)) / kFs;
                double sum = 0;
                for (int h = 1; h <= 20; ++h) sum += std::sin(h * ph) / h;
                v = static_cast<float>(0.25 * sum * (std::fmod(tt, 1.0) < 0.4 ? 1.0 : 0.0));
            }
            float a = nd(rng) + v;
            for (auto& m : rooms) a += m.process(spk[static_cast<size_t>(b + i)]);
            mic[static_cast<size_t>(i)] = a;
        }
        fb.process(mic.data(), blk);
        double e = 0;
        for (int i = 0; i < blk; ++i) {
            float o = std::clamp(mic[static_cast<size_t>(i)], -1.f, 1.f);
            if (drive > 0) o = static_cast<float>(std::tanh(drive * o) / std::tanh(drive));
            spk[static_cast<size_t>(b + i + delay)] = o;
            e += static_cast<double>(mic[static_cast<size_t>(i)]) * mic[static_cast<size_t>(i)];
        }
        const double level = db(e / blk);
        r.peakGuard = std::max(r.peakGuard, fb.stats().guardDb);
        if (t >= t0) {
            if (level > -20) r.audibleMs += 1000.0 * blk / kFs;
            if (t > 5.0) { tail += e / blk; ++tailN; }
            if (r.firstMs < 0 && fb.stats().events >= 1) r.firstMs = (t + blk / kFs - t0) * 1000.0;
        }
    }
    r.tailDb = db(tail / std::max(1, tailN));
    r.events = fb.stats().events;
    return r;
}

void testFastHowl()
{
    // The detector only acts on evidence that cannot be a voice, so it needs some tens to a few hundred
    // milliseconds on a hard loop. These checks pin down what it guarantees, not what it would like to do.
    {
        const auto r = runHarsh({ 1800 }, 12, 0, false, 8);
        check(r.firstMs > 0 && r.firstMs < 350, "howl +12 dB: first notch within 350 ms (ms)", r.firstMs);
        check(r.audibleMs < 400, "howl +12 dB: audible (> -20 dBFS) for less than 400 ms (ms)", r.audibleMs);
        check(r.tailDb < -50, "howl +12 dB: quiet at the end (tail dBFS)", r.tailDb);
    }
    {
        const auto r = runHarsh({ 1800 }, 20, 2.0, false, 8);
        check(r.audibleMs < 400, "howl +20 dB saturated: audible for less than 400 ms (ms)", r.audibleMs);
        check(r.tailDb < -50, "howl +20 dB saturated: quiet at the end (tail dBFS)", r.tailDb);
        check(r.peakGuard > 10, "howl +20 dB saturated: the guard did what the notches cannot (guard dB)", r.peakGuard);
    }
    {
        const auto r = runHarsh({ 700, 1800, 3100, 4600 }, 12, 2.0, false, 8);
        check(r.tailDb < -50, "four broad resonances at +12 dB: quiet at the end (tail dBFS)", r.tailDb);
        check(r.audibleMs < 400, "four broad resonances at +12 dB: audible for less than 400 ms (ms)", r.audibleMs);
    }
    {
        const auto r = runHarsh({ 1800 }, 12, 0, false, 20);
        check(r.audibleMs < 400, "howl with a 20 ms loop delay: audible for less than 400 ms (ms)", r.audibleMs);
        check(r.tailDb < -50, "howl with a 20 ms loop delay: quiet at the end (tail dBFS)", r.tailDb);
    }
    {
        const auto r = runHarsh({}, 0, 2.0, true, 8); // loud voice, no loop at all
        check(r.events == 0 && r.peakGuard < 0.5f, "a loud voice alone opens no notch and triggers no guard (events)", r.events);
    }
}


// ---------------------------------------------------------------- learn mode and ring-out (someone is speaking)
struct RingRun { double tailDb, audibleMs; float finalGain; int howls; bool finished; double seconds; };

// A room with one resonance (nominal peak loop gain `roomDb`), 8 ms loop delay, and a person speaking into the
// microphone 60% of the time. `ring` (optional) climbs the output gain, only while the person speaks; otherwise
// `fixedGainDb` is held. `learnMode` turns the suppressor's learn mode on.
RingRun runRoom(vc::FeedbackSuppressor& fb, bool learnMode, vc::RingOut* ring, double fixedGainDb, double maxSeconds, double roomDb = -10.0)
{
    const int blk = 64, delay = 384;
    const int total = static_cast<int>(kFs * maxSeconds);
    Room room(1800, 6.0, std::pow(10.0, roomDb / 20.0));
    std::mt19937 rng(11);
    std::normal_distribution<float> nd(0.f, 0.001f);
    std::vector<float> spk(static_cast<size_t>(total + delay), 0.f), mic(blk);
    fb.setLearn(learnMode);
    if (ring) { ring->prepare(kFs); ring->start(6.f, 18.f, 0.5f, 240.f); }
    RingRun r { 0, 0, 0, 0, false, maxSeconds };
    double tail = 0, ph = 0;
    int tailN = 0;
    for (int b = 0; b < total; b += blk) {
        const double gainDb = ring ? ring->gainDb() : fixedGainDb;
        const float lin = static_cast<float>(std::pow(10.0, gainDb / 20.0));
        const bool speaking = std::fmod(b / kFs, 1.0) < 0.6;
        for (int i = 0; i < blk; ++i) {
            const double tt = (b + i) / kFs;
            float v = 0.f;
            if (speaking) {
                ph += 2 * kPi * 160.0 * (1 + 0.02 * std::sin(2 * kPi * 5.5 * tt)) / kFs;
                double sum = 0;
                for (int h = 1; h <= 20; ++h) sum += std::sin(h * ph) / h;
                v = static_cast<float>(0.04 * sum);
            }
            mic[static_cast<size_t>(i)] = nd(rng) + v + room.process(spk[static_cast<size_t>(b + i)]);
        }
        fb.process(mic.data(), blk);
        double e = 0;
        float pk = 0.f;
        for (int i = 0; i < blk; ++i) {
            const float o = std::clamp(mic[static_cast<size_t>(i)] * lin, -1.f, 1.f);
            spk[static_cast<size_t>(b + i + delay)] = o;
            e += static_cast<double>(o) * o;
            pk = std::max(pk, std::fabs(o));
        }
        const double level = db(e / blk);
        if (ring) {
            ring->update(blk, fb.stats().events, static_cast<float>(level), pk, speaking);
            if (ring->finished()) { r.finished = true; r.seconds = b / kFs; break; }
        } else if (b > kFs * 1.0 && !speaking) { // judge the howl only when nobody speaks: the amplified voice is not a howl
            if (level > -20) r.audibleMs += 1000.0 * blk / kFs;
            if (b > total - static_cast<int>(kFs * 3)) { tail += e / blk; ++tailN; }
        }
    }
    if (tailN) r.tailDb = db(tail / tailN);
    r.finalGain = ring ? static_cast<float>(ring->gainDb()) : static_cast<float>(fixedGainDb);
    r.howls = ring ? ring->howlsFound() : 0;
    return r;
}

void testLearnAndRingOut()
{
    vc::FeedbackSuppressor learner;
    learner.prepare(kFs);
    learner.setMaxCutDb(24);
    vc::RingOut ring;
    const auto learned = runRoom(learner, true, &ring, 0, 300);
    check(learned.finished, "learn: the ring-out finishes by itself (s)", learned.seconds);
    check(learned.howls >= 1, "learn: with a voice in the room, the howl is provoked and caught (howls)", learned.howls);
    check(learner.learnedCount() >= 1, "learn: a notch is kept (count)", learner.learnedCount());
    const auto n0 = learner.learnedNotch(0);
    check(std::fabs(n0.freq - 1800) < 200, "learn: the kept notch sits on the room resonance (Hz)", n0.freq);
    check(n0.cutDb >= 6, "learn: the kept notch is deep enough to matter (dB)", n0.cutDb);
    check(std::fabs(learned.finalGain) < 0.1f, "learn: the gain is back to zero when it ends (dB)", learned.finalGain);
    check(!ring.aborted() && !ring.timedOut(), "learn: no safety abort and no time-out in a normal room");

    // hold the gain at the top of what the learning tested (about 6 dB past the first howl): a suppressor that has
    // not learned howls there, one that has does not
    const double hold = ring.maxGainReachedDb();
    check(hold > 10.0, "learn: the ring-out climbed past the point where the room rings (dB)", hold);
    vc::FeedbackSuppressor naive;
    naive.prepare(kFs);
    naive.setMaxCutDb(24);
    const auto cold = runRoom(naive, false, nullptr, hold, 10);

    vc::FeedbackSuppressor trained;
    trained.prepare(kFs);
    trained.setMaxCutDb(24);
    std::array<vc::FeedbackSuppressor::Notch, vc::FeedbackSuppressor::kMaxFixed> profile;
    const int cnt = learner.learnedCount();
    for (int i = 0; i < cnt; ++i) profile[static_cast<size_t>(i)] = learner.learnedNotch(i);
    trained.setLearnedNotches(profile.data(), cnt);
    const auto warm = runRoom(trained, false, nullptr, hold, 10);
    check(warm.audibleMs <= cold.audibleMs, "learn: what was learned is at least as good as live detection alone (audible ms, warm vs cold)", warm.audibleMs - cold.audibleMs);
    check(warm.audibleMs < 20, "learn: with the learned notch the same loop never howls (audible ms)", warm.audibleMs);
    check(warm.tailDb < -35, "learn: with the learned notch the level stays down (tail dBFS)", warm.tailDb);
    check(trained.learnedCount() == cnt, "learn: learned notches are applied when handed over (count)", trained.learnedCount());

    // learned notches are permanent: no release after a long quiet time, and they survive a prepare()
    std::vector<float> quiet(512, 0.f);
    for (int i = 0; i < 48000 * 14 / 512; ++i) trained.process(quiet.data(), 512);
    check(trained.learnedCount() == cnt, "learn: a learned notch is not released after 14 s of silence (count)", trained.learnedCount());
    trained.prepare(44100);
    for (int i = 0; i < 8; ++i) trained.process(quiet.data(), 512);
    check(trained.learnedCount() == cnt, "learn: a learned notch survives a change of sample rate (count)", trained.learnedCount());
    trained.clearLearned();
    for (int i = 0; i < 8; ++i) trained.process(quiet.data(), 512);
    check(trained.learnedCount() == 0, "learn: clearLearned removes them (count)", trained.learnedCount());

    // a quiet room (loop gain far below unity): the voice alone teaches nothing, and the ring-out runs to its limit
    vc::FeedbackSuppressor quietRoom;
    quietRoom.prepare(kFs);
    quietRoom.setMaxCutDb(24);
    vc::RingOut ring2;
    const auto calm = runRoom(quietRoom, true, &ring2, 0, 300, -45.0);
    check(calm.finished && calm.howls == 0, "learn: a calm room is learned as 'no howl up to +18 dB' (howls)", calm.howls);
    check(quietRoom.learnedCount() == 0, "learn: speech alone does not leave a permanent notch (count)", quietRoom.learnedCount());

    // the climb waits for the voice: with nobody speaking it does not move
    vc::RingOut idle;
    idle.prepare(kFs);
    idle.start(6.f, 18.f, 0.5f, 240.f);
    for (int i = 0; i < 48000 * 10 / 64; ++i) idle.update(64, 0, -70.f, 0.f, false);
    check(idle.gainDb() == 0.f && idle.waitingForVoice(), "learn: with nobody speaking the gain does not climb (dB)", idle.gainDb());
}


// ---------------------------------------------------------------- adaptive feedback canceller (high CPU mode)
namespace {
// The true response of the test room (two resonances at 900 and 2400 Hz, 384 samples of delay), as the canceller sees it.
std::vector<double> roomTaps(double peak, int dmin)
{
    std::vector<Room> m;
    for (double f : { 900.0, 2400.0 }) m.emplace_back(f, 6.0, peak);
    std::vector<double> truth(vc::FeedbackCanceller::kTaps, 0.0);
    for (int i = 0; i < vc::FeedbackCanceller::kTaps + dmin; ++i) {
        const float in = i == 0 ? 1.f : 0.f;
        double o = 0;
        for (auto& r : m) o += r.process(in);
        const int at = i + 384 - dmin;
        if (at >= 0 && at < vc::FeedbackCanceller::kTaps) truth[static_cast<size_t>(at)] = o;
    }
    return truth;
}

double tapsErrorDb(const vc::FeedbackCanceller& c, double peak)
{
    std::vector<float> taps(vc::FeedbackCanceller::kTaps);
    c.exportTaps(taps.data());
    const auto truth = roomTaps(peak, c.minLoopDelaySamples());
    double num = 0, den = 0;
    for (size_t i = 0; i < truth.size(); ++i) { num += (taps[i] - truth[i]) * (taps[i] - truth[i]); den += truth[i] * truth[i]; }
    return db(num / den);
}

// A closed loop: microphone = noise + the room's answer to the loudspeaker feed; the feed is the cleaned microphone
// times `gain`, plus an independent probe. Returns the time the feed spent above -20 dBFS and the final level.
struct CancelRun { double audibleMs, tailDb, peakDb; };
CancelRun runCancelLoop(vc::FeedbackCanceller* afc, bool detector, double peak, double gainDb, double probeDb, double seconds, bool learnPhase)
{
    const int blk = 64, delay = 384, total = static_cast<int>(kFs * seconds);
    std::vector<Room> room;
    for (double f : { 900.0, 2400.0 }) room.emplace_back(f, 6.0, peak);
    vc::FeedbackSuppressor fb;
    fb.prepare(kFs);
    std::mt19937 rng(21), pr(5);
    std::normal_distribution<float> nd(0.f, 1.f);
    std::vector<float> spk(static_cast<size_t>(total + delay), 0.f), mic(blk), u(blk);
    const float lin = static_cast<float>(std::pow(10.0, gainDb / 20.0)), probe = static_cast<float>(std::pow(10.0, probeDb / 20.0));
    CancelRun r { 0, 0, -200 };
    double tail = 0;
    int tailN = 0;
    if (afc) { afc->setLearning(learnPhase); afc->setAdapt(learnPhase); }
    for (int b = 0; b < total; b += blk) {
        for (int i = 0; i < blk; ++i) {
            float a = 0.001f * nd(rng);
            for (auto& m : room) a += m.process(spk[static_cast<size_t>(b + i)]);
            mic[static_cast<size_t>(i)] = a;
        }
        if (afc) afc->cancel(mic.data(), blk);
        if (detector) fb.process(mic.data(), blk);
        double e = 0;
        for (int i = 0; i < blk; ++i) {
            const float o = std::clamp(mic[static_cast<size_t>(i)] * lin + probe * nd(pr), -1.f, 1.f);
            u[static_cast<size_t>(i)] = o;
            spk[static_cast<size_t>(b + i + delay)] = o;
            e += static_cast<double>(o) * o;
        }
        if (afc) afc->update(u.data(), blk);
        const double level = db(e / blk);
        if (b > kFs) {
            r.peakDb = std::max(r.peakDb, level);
            if (level > -20) r.audibleMs += 1000.0 * blk / kFs;
            if (b > total - static_cast<int>(kFs)) { tail += e / blk; ++tailN; }
        }
    }
    r.tailDb = db(tail / std::max(1, tailN));
    return r;
}
} // namespace

void testFeedbackCanceller()
{
    // 1. open loop: an independent white-noise reference, the room answers; the filter must find the room
    {
        vc::FeedbackCanceller c;
        c.prepare(kFs, 64);
        c.setLearning(true);
        std::vector<Room> room;
        for (double f : { 900.0, 2400.0 }) room.emplace_back(f, 6.0, 0.316);
        std::mt19937 rng(3);
        std::normal_distribution<float> nd(0.f, 1.f);
        std::vector<float> ref(static_cast<size_t>(kFs * 10) + 512, 0.f);
        double ex = 0, ee = 0;
        for (int b = 0; b < kFs * 10; b += 64) {
            float mic[64], u[64];
            for (int i = 0; i < 64; ++i) { u[i] = 0.1f * nd(rng); ref[static_cast<size_t>(b + i + 384)] = u[i]; }
            for (int i = 0; i < 64; ++i) { float a = 1e-4f * nd(rng); for (auto& m : room) a += m.process(ref[static_cast<size_t>(b + i)]); mic[i] = a; }
            float before[64]; std::copy(mic, mic + 64, before);
            c.cancel(mic, 64);
            c.update(u, 64);
            if (b > kFs * 8) for (int i = 0; i < 64; ++i) { ex += static_cast<double>(before[i]) * before[i]; ee += static_cast<double>(mic[i]) * mic[i]; }
        }
        check(tapsErrorDb(c, 0.316) < -35, "canceller: finds the room from an independent reference (filter error dB)", tapsErrorDb(c, 0.316));
        check(db(ex / ee) > 25, "canceller: removes the room's answer from the microphone (dB)", db(ex / ee));
        const float loop = c.peakLoopGainDb();
        check(loop > -13 && loop < -7, "canceller: reads the room's loop gain from what it learned (peak dB, true -10)", loop);
    }
    // 2. closed loop, measured with noise at -20 dBFS and then frozen: the filter is close to the room, and a hard loop does not howl
    vc::FeedbackCanceller trained;
    trained.prepare(kFs, 64);
    runCancelLoop(&trained, false, 0.316, 0.0, -20.0, 10.0, true);
    check(tapsErrorDb(trained, 0.316) < -22, "canceller: closed-loop measurement with a -20 dBFS noise (filter error dB)", tapsErrorDb(trained, 0.316));
    // loop gain +20 dB: the amplified microphone noise alone sits near -30 dBFS, a howl is far above it
    const auto bare = runCancelLoop(nullptr, true, 0.316, 30.0, -200.0, 6.0, false);   // detector only
    const auto with = runCancelLoop(&trained, true, 0.316, 30.0, -200.0, 6.0, false);  // same, with the frozen filter
    check(bare.peakDb > -20, "canceller: without it, the +20 dB loop howls (peak dBFS)", bare.peakDb);
    check(with.peakDb < -20 && with.audibleMs < 10, "canceller: with the frozen filter the same loop does not howl (peak dBFS)", with.peakDb);
    check(with.peakDb < bare.peakDb - 10, "canceller: its peak is far below the detector's alone (dB)", with.peakDb - bare.peakDb);
    check(with.tailDb < -24, "canceller: the level settles at the amplified noise floor (tail dBFS)", with.tailDb);

    // 3. the guard: even a wrong filter cannot make the cancelled signal louder than the input
    {
        vc::FeedbackCanceller bad;
        bad.prepare(kFs, 64);
        std::vector<float> junk(vc::FeedbackCanceller::kTaps);
        std::mt19937 rng(8);
        std::normal_distribution<float> nd(0.f, 0.05f);
        for (auto& v : junk) v = nd(rng);
        bad.importTaps(junk.data(), bad.minLoopDelaySamples());
        std::normal_distribution<float> sig(0.f, 0.1f);
        double ex = 0, ee = 0;
        for (int b = 0; b < kFs * 3; b += 64) {
            float x[64], before[64], u[64];
            for (int i = 0; i < 64; ++i) { x[i] = before[i] = sig(rng); u[i] = sig(rng); }
            bad.cancel(x, 64);
            bad.update(u, 64);
            for (int i = 0; i < 64; ++i) { ex += static_cast<double>(before[i]) * before[i]; ee += static_cast<double>(x[i]) * x[i]; }
        }
        check(ee <= ex * 1.12, "canceller: a wrong filter cannot add energy to the input (ratio)", ee / ex);
        check(bad.cappedBlocks() > 0, "canceller: the guard stepped in (blocks)", bad.cappedBlocks());
    }
    // 4. export then import keeps the filter
    {
        vc::FeedbackCanceller copy;
        copy.prepare(kFs, 64);
        std::vector<float> taps(vc::FeedbackCanceller::kTaps);
        trained.exportTaps(taps.data());
        copy.importTaps(taps.data(), trained.minLoopDelaySamples());
        check(std::fabs(tapsErrorDb(copy, 0.316) - tapsErrorDb(trained, 0.316)) < 0.5, "canceller: export then import keeps the filter (dB difference)", tapsErrorDb(copy, 0.316) - tapsErrorDb(trained, 0.316));
        vc::FeedbackCanceller other;
        other.prepare(kFs, 256); // another host block size: the first tap moves, the room does not
        other.importTaps(taps.data(), trained.minLoopDelaySamples());
        std::vector<float> back(vc::FeedbackCanceller::kTaps);
        other.exportTaps(back.data());
        const int shift = other.minLoopDelaySamples() - trained.minLoopDelaySamples();
        double num = 0, den = 0;
        for (int i = 0; i + shift < vc::FeedbackCanceller::kTaps; ++i) { num += (back[static_cast<size_t>(i)] - taps[static_cast<size_t>(i + shift)]) * (back[static_cast<size_t>(i)] - taps[static_cast<size_t>(i + shift)]); den += taps[static_cast<size_t>(i + shift)] * taps[static_cast<size_t>(i + shift)]; }
        check(db(num / den) < -40, "canceller: a different block size keeps the room at the same absolute delays (dB)", db(num / den));
    }
    // 5. a frozen filter measured in a room with no feedback at all leaves the voice alone
    {
        vc::FeedbackCanceller quiet;
        quiet.prepare(kFs, 64);
        runCancelLoop(&quiet, false, 0.0, 0.0, -20.0, 10.0, true); // measured: no room answer
        auto v = voice(static_cast<int>(kFs * 4), 160, 0.25f, false);
        quiet.setLearning(false);
        quiet.setAdapt(false);
        double ex = 0, ee = 0;
        for (size_t i = 0; i + 64 <= v.size(); i += 64) {
            float x[64];
            for (int k = 0; k < 64; ++k) x[k] = v[i + static_cast<size_t>(k)];
            float u[64];
            std::copy(x, x + 64, u);
            quiet.cancel(x, 64);
            quiet.update(u, 64);
            for (int k = 0; k < 64; ++k) { ex += static_cast<double>(v[i + static_cast<size_t>(k)]) * v[i + static_cast<size_t>(k)]; ee += static_cast<double>(x[k]) * x[k]; }
        }
        check(std::fabs(db(ee / ex)) < 0.3, "canceller: with no feedback to cancel the voice is left alone (dB)", db(ee / ex));
    }
}

} // namespace

int main()
{
    testReconstructionAndLatency();
    testDenoise();
    testLowLatency();
    testGateKeepsWeakSyllables();
    testFeedback();
    testSimultaneousNotches();
    testStatsReset();
    testHybridMix();
    testHintCannotBlockHowl();
    testVoiceNotTouched();
    testAsyncMask();
    testVoiceGate();
    testVoiceGateNeural();
    testAutoMixer();
    testBypass();
    testFastHowl();
    testLearnAndRingOut();
    testFeedbackCanceller();
    std::printf(failures ? "\n%d FAILED\n" : "\nall passed\n", failures);
    return failures ? 1 : 0;
}
