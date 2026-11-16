// Black-box probe: loads any VST3 and measures what it does to audio (latency, level, closed-loop larsen behaviour).
// It only feeds audio in and reads audio out: nothing of the plugin is inspected. See README.md in this folder.
#include <juce_audio_processors/juce_audio_processors.h>
#include <fstream>
#include <algorithm>
#include <random>
#include <cmath>
using namespace juce;
static constexpr double fs = 48000;
static constexpr int blk = 64;

static std::vector<float> readWav(const char* p)
{
    std::ifstream f(p, std::ios::binary); std::vector<char> b((std::istreambuf_iterator<char>(f)), {});
    for (size_t i = 12; i + 8 < b.size();) { uint32_t sz; memcpy(&sz, &b[i + 4], 4);
        if (!memcmp(&b[i], "data", 4)) { std::vector<float> o(sz / 2); for (size_t k = 0; k < o.size(); ++k) { int16_t s; memcpy(&s, &b[i + 8 + 2 * k], 2); o[k] = s / 32768.f; } return o; }
        i += 8 + sz + (sz & 1); }
    return {};
}
struct Room { double b0, b2, a1, a2, z1 = 0, z2 = 0, g;
    Room(double f, double q, double gain) : g(gain) { double w = 2 * M_PI * f / fs, al = std::sin(w) / (2 * q), a0 = 1 + al; b0 = al / a0; b2 = -al / a0; a1 = -2 * std::cos(w) / a0; a2 = (1 - al) / a0; }
    float process(float x) { double y = b0 * x + z1; z1 = -a1 * y + z2; z2 = b2 * x - a2 * y; return (float)(y * g); } };

static std::unique_ptr<AudioPluginInstance> load(const String& path, String& err)
{
    AudioPluginFormatManager fm; fm.addFormat(new VST3PluginFormat());
    OwnedArray<PluginDescription> d; for (auto* f : fm.getFormats()) f->findAllTypesForFile(d, path);
    if (d.isEmpty()) { err = "no plugin found in " + path; return nullptr; }
    auto p = fm.createPluginInstance(*d[0], fs, blk, err);
    if (!p) return nullptr;
    p->setPlayConfigDetails(1, 1, fs, blk); p->setRateAndBufferSizeDetails(fs, blk);
    AudioProcessor::BusesLayout l; l.inputBuses.add(AudioChannelSet::mono()); l.outputBuses.add(AudioChannelSet::mono());
    if (!p->setBusesLayout(l)) { AudioProcessor::BusesLayout s; s.inputBuses.add(AudioChannelSet::stereo()); s.outputBuses.add(AudioChannelSet::stereo()); p->setBusesLayout(s); }
    p->prepareToPlay(fs, blk);
    return p;
}
// process one mono block through the plugin (extra channels get a copy)
static void run(AudioPluginInstance& p, float* x, int n)
{
    static AudioBuffer<float> b; static MidiBuffer m;
    int ch = jmax(1, p.getTotalNumInputChannels()); b.setSize(ch, n, false, false, true);
    for (int c = 0; c < ch; ++c) b.copyFrom(c, 0, x, n);
    p.processBlock(b, m);
    memcpy(x, b.getReadPointer(0), sizeof(float) * (size_t)n);
}
static void listParams(AudioPluginInstance& p)
{
    printf("name: %s | latency %d samples | tail %.2f s | in %d out %d ch\n", p.getName().toRawUTF8(), p.getLatencySamples(), p.getTailLengthSeconds(), p.getTotalNumInputChannels(), p.getTotalNumOutputChannels());
    for (auto* prm : p.getParameters()) {
        auto* ranged = dynamic_cast<RangedAudioParameter*>(prm);
        String line = String(prm->getName(40)) + " | default " + prm->getText(prm->getDefaultValue(), 20) + " [min " + prm->getText(0.f, 20) + " .. max " + prm->getText(1.f, 20) + "]";
        if (prm->isDiscrete()) line += " discrete";
        (void)ranged; printf("  - %s\n", line.toRawUTF8());
    }
}
static double db(double x) { return 10.0 * std::log10(x + 1e-20); }

// closed loop: mic = source + room(delayed speaker). Prints the output rms every `step` s.
static void loopTest(AudioPluginInstance& p, std::vector<double> freqs, double q, double gain, double seconds, const std::vector<float>* voice, double voiceDb, int delaySmp = 480)
{
    std::vector<Room> rooms; for (double f : freqs) rooms.emplace_back(f, q, gain);
    std::mt19937 rng(3); std::normal_distribution<float> nd(0.f, 0.0003f);
    const int total = (int)(fs * seconds); std::vector<float> spk((size_t)total + delaySmp + blk, 0.f), out((size_t)total, 0.f);
    float vs = 0; if (voice) { float pk = 0; for (float v : *voice) pk = std::max(pk, std::fabs(v)); vs = (float)std::pow(10.0, voiceDb / 20) / pk; }
    std::vector<float> blkbuf(blk);
    for (int t = 0; t + blk <= total; t += blk) {
        for (int i = 0; i < blk; ++i) { float a = nd(rng); if (voice) { size_t k = (size_t)(t + i) % (voice->size() + 24000); if (k < voice->size()) a += (*voice)[k] * vs; }
            for (auto& r : rooms) a += r.process(spk[(size_t)(t + i)]); blkbuf[(size_t)i] = a; }
        run(p, blkbuf.data(), blk);
        for (int i = 0; i < blk; ++i) { out[(size_t)(t + i)] = blkbuf[(size_t)i]; spk[(size_t)(t + i + delaySmp)] = jlimit(-1.f, 1.f, blkbuf[(size_t)i]); }
    }
    // report: rms per 0.25 s for the first 3 s, then per second
    if (getenv("FROM")) { double from0 = atof(getenv("FROM")); std::vector<double> lv; for (int i = (int)(fs * from0); i + 480 < total; i += 480) { double e = 0; for (int k = 0; k < 480; ++k) e += out[(size_t)(i + k)] * out[(size_t)(i + k)]; lv.push_back(db(e / 480)); }
        std::sort(lv.begin(), lv.end()); auto pc = [&](double q) { return lv[(size_t)(q * (lv.size() - 1))]; }; printf("   level of 10 ms windows (dBFS rms): p50 %.1f | p90 %.1f | p99 %.1f | max %.1f\n", pc(.5), pc(.9), pc(.99), lv.back());
        if (getenv("TRACE")) { printf("   trace every 0.1 s from %.1f s: ", from0); for (double t = from0; t < from0 + 4; t += 0.1) { double e = 0; int n0 = (int)(t * fs); for (int k = 0; k < 4800; ++k) e += out[(size_t)(n0 + k)] * out[(size_t)(n0 + k)]; printf("%.0f ", db(e / 4800)); } printf("\n"); } }
    printf("   t(s):dBFS rms ");
    for (double t = 0; t < seconds; t += (t < 3 ? 0.25 : 1.0)) { double e = 0; int n0 = (int)(t * fs), n1 = std::min(total, n0 + (int)((t < 3 ? 0.25 : 1.0) * fs)); for (int i = n0; i < n1; ++i) e += out[(size_t)i] * out[(size_t)i]; printf("%.2f:%.0f ", t, db(e / std::max(1, n1 - n0))); }
    double pkTail = 0; for (int i = total - (int)fs; i < total; ++i) pkTail = std::max(pkTail, (double)std::fabs(out[(size_t)i]));
    double firstLoud = -1, lastLoud = -1; for (int i = (int)(fs * (getenv("FROM") ? atof(getenv("FROM")) : 0)); i + 480 < total; i += 480) { double e = 0; for (int k = 0; k < 480; ++k) e += out[(size_t)(i + k)] * out[(size_t)(i + k)]; if (db(e / 480) > -30) { if (firstLoud < 0) firstLoud = i / fs; lastLoud = i / fs; } }
    { int loud = 0, win = 0; double sumDb = 0; const int from = (int)(fs * (getenv("FROM") ? atof(getenv("FROM")) : 0)); for (int i = from; i + 480 < total; i += 480) { double pk = 0, e = 0; for (int k = 0; k < 480; ++k) { pk = std::max(pk, (double)std::fabs(out[(size_t)(i + k)])); e += out[(size_t)(i + k)] * out[(size_t)(i + k)]; } ++win; sumDb += db(e / 480); if (db(pk * pk) > -6) ++loud; }
      printf("\n   -> time with output peaks above -6 dBFS (howl/clipping): %.2f s of %.0f s | mean level %.1f dBFS", loud * 0.01, seconds - (getenv("FROM") ? atof(getenv("FROM")) : 0), sumDb / std::max(1, win)); }
    printf("\n   -> final-second peak %.1f dBFS | first/last 10 ms window above -30 dBFS: %.2f s / %.2f s\n", db(pkTail * pkTail), firstLoud, lastLoud);
}

int main(int argc, char** argv)
{
    ScopedJuceInitialiser_GUI init;
    String err; auto p = load(argv[1], err);
    if (!p) { printf("load failed: %s\n", err.toRawUTF8()); return 1; }
    String mode = argc > 2 ? argv[2] : "info";
    if (mode == "info") { listParams(*p); return 0; }
    // parameter overrides: name=value (0..1 normalised) after the mode
    for (int i = 3; i < argc; ++i) { String a(argv[i]); int eq = a.indexOfChar('='); if (eq < 0) continue; for (auto* prm : p->getParameters()) if (prm->getName(40).containsIgnoreCase(a.substring(0, eq))) { prm->setValue(a.substring(eq + 1).getFloatValue()); printf("  set %s = %s\n", prm->getName(40).toRawUTF8(), prm->getText(prm->getValue(), 20).toRawUTF8()); } }
    for (int i = 0; i < 400; ++i) { std::vector<float> z(blk, 0.f); run(*p, z.data(), blk); } // let the host-side latency change settle
    if (mode == "latency") { std::vector<float> x(blk * 40, 0.f); x[10 * blk] = 0.5f; for (int i = 0; i < 40; ++i) run(*p, &x[(size_t)i * blk], blk);
        int pk = 0; float m = 0; for (size_t i = 0; i < x.size(); ++i) if (std::fabs(x[i]) > m) { m = std::fabs(x[i]); pk = (int)i; }
        printf("impulse: peak %.3f at +%d samples (%.2f ms), reported latency %d\n", m, pk - 10 * blk, (pk - 10 * blk) * 1000.0 / fs, p->getLatencySamples()); return 0; }
    for (int i = 0; i < 100; ++i) { std::vector<float> z(blk, 0.f); run(*p, z.data(), blk); } // settle
    if (mode == "stream") { // steady inputs, output level per second: noise at -30 dBFS, then a 1 kHz tone, then speech
        std::mt19937 rng(5); std::normal_distribution<float> nd(0.f, 1.f); auto v = readWav(getenv("WAV")); float pk = 0; for (float s : v) pk = std::max(pk, std::fabs(s)); float vs = std::pow(10.f, -20.f / 20) / pk;
        for (int kind = 0; kind < 3; ++kind) { printf("  %s: ", kind == 0 ? "noise -30 dBFS" : kind == 1 ? "tone 1 kHz -20 dBFS" : "speech -20 dBFS peak");
            for (int sec = 0; sec < 8; ++sec) { double ei = 0, eo = 0; for (int t = 0; t < (int)fs; t += blk) { std::vector<float> b(blk); for (int i = 0; i < blk; ++i) { size_t n = (size_t)(sec * fs + t + i);
                        b[(size_t)i] = kind == 0 ? nd(rng) * 0.0316f * 0.8f : kind == 1 ? 0.1f * std::sin(2 * M_PI * 1000 * n / fs) : v[n % v.size()] * vs; ei += b[(size_t)i] * b[(size_t)i]; }
                    run(*p, b.data(), blk); for (float x : b) eo += x * x; }
                printf("%.0f:%.1f ", sec + 0.0, db(eo / ei)); }
            printf(" (output level vs input, dB, per second)\n"); } }
    if (mode == "mutemap") { // when does the output go silent? speech looped at -20 dBFS peak, 0.25 s cells, M = silent
        auto v = readWav(getenv("WAV")); float pk = 0; for (float s : v) pk = std::max(pk, std::fabs(s)); float vs = std::pow(10.f, -20.f / 20) / pk; const double secs = getenv("SECS") ? atof(getenv("SECS")) : 60;
        std::string line; int cell = (int)(fs * 0.25); double eo = 0, ei = 0; int n = 0; std::vector<int> muted;
        for (long t = 0; t + blk <= (long)(fs * secs); t += blk) { std::vector<float> b(blk); for (int i = 0; i < blk; ++i) { b[(size_t)i] = v[(size_t)(t + i) % v.size()] * vs; ei += b[(size_t)i] * b[(size_t)i]; }
            run(*p, b.data(), blk); for (float x : b) eo += x * x; n += blk; if (n >= cell) { bool m = db(eo / n) < -90 && db(ei / n) > -60; line += m ? 'M' : '.'; muted.push_back(m); eo = ei = 0; n = 0; } }
        printf("%s\n(each char = 0.25 s; M = output silent while input is present)\n", line.c_str()); int runs = 0, cur = 0; std::vector<int> lens; for (int m : muted) { if (m) ++cur; else { if (cur) lens.push_back(cur); cur = 0; } } if (cur) lens.push_back(cur);
        printf("muted runs (in 0.25 s cells):"); for (int l : lens) printf(" %d", l); printf("\n"); (void)runs; }
    if (mode == "speechgain") { // gain the plugin applies to the speech itself, with room noise: LEVEL_DB (speech peak), NOISE_DB (pink-ish)
        auto v = readWav(getenv("WAV")); float pk = 0; for (float s : v) pk = std::max(pk, std::fabs(s));
        const float lvl = std::pow(10.f, (getenv("LEVEL_DB") ? (float)atof(getenv("LEVEL_DB")) : -20.f) / 20) / pk, nz = std::pow(10.f, (getenv("NOISE_DB") ? (float)atof(getenv("NOISE_DB")) : -60.f) / 20);
        std::mt19937 rng(7); std::normal_distribution<float> nd(0.f, 1.f); float lp = 0;
        const long warm = (long)(fs * 8.0), meas = (long)(fs * 18.0); std::vector<float> clean, o;
        for (long t = 0; t + blk <= warm + meas; t += blk) { std::vector<float> b(blk); for (int i = 0; i < blk; ++i) { long n = t + i; float c = v[(size_t)n % v.size()] * lvl; clean.push_back(c); lp = 0.9f * lp + 0.1f * nd(rng); b[(size_t)i] = c + lp * 3.f * nz; }
            run(*p, b.data(), blk); o.insert(o.end(), b.begin(), b.end()); }
        double best = -1e30; int bl = 0; for (int L = 0; L < 600; ++L) { double c = 0; for (long i = warm + 8000; i < warm + 40000; ++i) c += (double)o[(size_t)(i + L)] * clean[(size_t)i]; if (c > best) { best = c; bl = L; } }
        double cc = 0, co = 0, ec = 0; for (long i = warm; i + bl < (long)o.size(); ++i) { cc += (double)clean[(size_t)i] * clean[(size_t)i]; co += (double)o[(size_t)(i + bl)] * clean[(size_t)i]; }
        // loudest 200 ms stretches (what a listener hears as "level"): gain of the speech component there
        double alpha = co / cc; printf("speech peak %.0f dBFS, noise %.0f dBFS: delay %d, gain applied to the speech: %+.2f dB", 20 * std::log10(lvl * pk), 20 * std::log10(nz), bl, 20 * std::log10(std::fabs(alpha) + 1e-9)); (void)ec; printf("\n"); }
    if (mode == "speech2" || mode == "tone2") {
        auto v = readWav(getenv("WAV")); float pk = 0; for (float s : v) pk = std::max(pk, std::fabs(s)); float vs = std::pow(10.f, -20.f / 20) / pk;
        const long warm = (long)(fs * 8.0), meas = (long)(fs * 18.0); std::vector<float> in, o;
        for (long t = 0; t + blk <= warm + meas; t += blk) { std::vector<float> b(blk); for (int i = 0; i < blk; ++i) { long n = t + i; b[(size_t)i] = (mode == "tone2" && n >= warm) ? 0.1f * std::sin(2 * M_PI * 1000 * n / fs) : v[(size_t)n % v.size()] * vs; }
            in.insert(in.end(), b.begin(), b.end()); run(*p, b.data(), blk); o.insert(o.end(), b.begin(), b.end()); }
        if (mode == "speech2") { double bestC = -1; int bestL = 0; for (int L = 0; L < 1200; ++L) { double c = 0; for (long i = warm + 8000; i < warm + 40000; ++i) c += (double)o[(size_t)(i + L)] * in[(size_t)i]; if (c > bestC) { bestC = c; bestL = L; } }
            double ei = 0, eo = 0; for (long i = warm; i + bestL < (long)o.size(); ++i) { ei += (double)in[(size_t)i] * in[(size_t)i]; eo += (double)o[(size_t)(i + bestL)] * o[(size_t)(i + bestL)]; }
            printf("speech through (8 s warm-up, 18 s measured): delay %d samples, level change %.2f dB\n", bestL, db(eo / ei)); }
        else { printf("tone 1 kHz -20 dBFS after 8 s of speech, output vs input (dB) every 0.25 s: "); for (int c = 0; c < 40; ++c) { double ei = 0, eo = 0; long n0 = warm + c * (long)(fs * 0.25); for (long i = n0; i < n0 + (long)(fs * 0.25); ++i) { ei += (double)in[(size_t)i] * in[(size_t)i]; eo += (double)o[(size_t)i] * o[(size_t)i]; } printf("%.0f ", db(eo / ei)); } printf("\n"); } }
    if (mode == "howl1") loopTest(*p, { 1500 }, 8, 1.4, 8, nullptr, 0);
    if (mode == "howl3") loopTest(*p, { 800, 1500, 3100 }, 8, 1.4, 8, nullptr, 0);
    if (mode == "howlhard") loopTest(*p, { 1500 }, 8, 3.0, 8, nullptr, 0);
    if (mode == "voiceloop") { auto v = readWav(getenv("WAV")); double g = getenv("G") ? atof(getenv("G")) : 1.4; loopTest(*p, { 1500 }, 8, g, getenv("SECS") ? atof(getenv("SECS")) : 12, &v, -20); }
    if (mode == "voiceloop3") { auto v = readWav(getenv("WAV")); double g = getenv("G") ? atof(getenv("G")) : 1.4; loopTest(*p, { 800, 1500, 3100 }, 8, g, getenv("SECS") ? atof(getenv("SECS")) : 12, &v, -20); }
    if (mode == "speech") { // passthrough: speech at -20 dBFS peak, no loop. level change vs input, aligned by cross-correlation search
        auto v = readWav(getenv("WAV")); float pk = 0; for (float s : v) pk = std::max(pk, std::fabs(s)); float sc = std::pow(10.f, -20.f / 20) / pk;
        std::vector<float> in(v.size()), o; for (size_t i = 0; i < v.size(); ++i) in[i] = v[i] * sc; o = in;
        for (size_t i = 0; i + blk <= o.size(); i += blk) run(*p, &o[i], blk);
        double bestC = -1; int bestL = 0; for (int L = 0; L < 1200; ++L) { double c = 0; for (size_t i = 20000; i < 60000; ++i) c += (double)o[i + (size_t)L] * in[i]; if (c > bestC) { bestC = c; bestL = L; } }
        double ei = 0, eo = 0; for (size_t i = 20000; i + bestL < o.size() && i < in.size(); ++i) { ei += (double)in[i] * in[i]; eo += (double)o[i + (size_t)bestL] * o[i + (size_t)bestL]; }
        printf("speech through, no loop: delay %d samples, level change %.2f dB\n", bestL, db(eo / ei)); }
    return 0;
}
