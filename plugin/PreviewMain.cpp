// Developer tool: renders the editor to a PNG after feeding the processor a simulated howl, so the
// flight-log numbers are alive. Usage: VonDeLarsenPreview out.png [help] [standalone] [bypass] [learn]   or   out.png icon
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <random>

#include "PluginProcessor.h"
#include "VdlArt.h"
#include "VdlEditor.h"

// The app icon: the rocket with its microphone windscreen, in the radar glass, on black. 1024 x 1024.
static void renderIcon(const juce::File& out)
{
    using namespace juce;
    using namespace vdl;
    const float S = 1024.f;
    Image img(Image::ARGB, 1024, 1024, true);
    Graphics g(img);
    // squircle-ish tile, with the margin macOS expects around an app icon
    const Rectangle<float> tile(100.f, 100.f, S - 200.f, S - 200.f);
    Path shape;
    shape.addRoundedRectangle(tile, 185.f);
    g.setColour(Colours::black.withAlpha(0.45f));
    g.fillPath(shape, AffineTransform::translation(0.f, 14.f));
    g.setGradientFill(ColourGradient(Colour(0xff2b0a10), S * 0.5f, S * 0.40f, kBlack, S * 0.5f, S * 1.05f, true));
    g.fillPath(shape);
    g.setColour(kRed.withAlpha(0.9f));
    g.strokePath(shape, PathStrokeType(7.f));

    Graphics::ScopedSaveState clip(g);
    g.reduceClipRegion(shape);
    const Point<float> c(S * 0.5f, S * 0.5f);
    // radar glass: rings, crosshair, LED ring
    const float R = 330.f;
    g.setColour(kBlack);
    g.fillEllipse(c.x - R - 26.f, c.y - R - 26.f, 2.f * (R + 26.f), 2.f * (R + 26.f));
    g.setColour(kRed);
    g.drawEllipse(c.x - R - 26.f, c.y - R - 26.f, 2.f * (R + 26.f), 2.f * (R + 26.f), 12.f);
    for (int i = 0; i < 41; ++i) {
        const float a = degreesToRadians(-135.f + 270.f * static_cast<float>(i) / 40.f);
        const bool major = i % 5 == 0;
        g.setColour(i <= 28 ? kHotRed : Colour(0xff35100f));
        g.drawLine(Line<float>(c.getPointOnCircumference(R * 0.80f, a), c.getPointOnCircumference(major ? R * 0.99f : R * 0.93f, a)), 15.f);
    }
    g.setGradientFill(ColourGradient(Colour(0xff2a1316), c.x - R * 0.3f, c.y - R * 0.4f, Colour(0xff050506), c.x + R * 0.7f, c.y + R * 0.7f, true));
    g.fillEllipse(c.x - R * 0.74f, c.y - R * 0.74f, R * 1.48f, R * 1.48f);
    g.setColour(kHotRed.withAlpha(0.22f));
    for (float f : { 0.25f, 0.5f, 0.72f }) g.drawEllipse(c.x - R * f, c.y - R * f, 2.f * R * f, 2.f * R * f, 3.f);
    // the rocket, tilted, with a long flame
    {
        Graphics::ScopedSaveState rot(g);
        g.addTransform(AffineTransform::rotation(degreesToRadians(32.f), c.x, c.y));
        drawRocket(g, c, 5.0f, 1.f);
    }
    // a few stars
    g.setColour(kCream.withAlpha(0.85f));
    for (auto p : { Point<float>(300.f, 270.f), { 770.f, 250.f }, { 800.f, 700.f }, { 250.f, 640.f }, { 640.f, 205.f } }) {
        Path star;
        star.addStar(p, 4, 3.f, 17.f, 0.f);
        g.fillPath(star);
    }
    out.deleteFile();
    FileOutputStream os(out);
    if (os.openedOk()) PNGImageFormat().writeImageToStream(img, os);
}

// Developer self-test of the whole processor with a simulated room around it: learn the room while someone speaks,
// then report what the processor learned. Usage: VonDeLarsenPreview x selftest
static int selfTest()
{
    VonDeLarsenProcessor p;
    constexpr double fs = 48000;
    constexpr int blk = 64, delay = 384;
    p.setActiveChannels(1);
    p.setPlayConfigDetails(1, 1, fs, blk);
    p.setRateAndBufferSizeDetails(fs, blk);
    p.prepareToPlay(fs, blk);

    struct Mode { double b0, b2, a1, a2, z1 = 0, z2 = 0, gain; Mode(double f, double q, double g) : gain(g) { const double w = 6.283185307179586 * f / fs, al = std::sin(w) / (2 * q), a0 = 1 + al; b0 = al / a0; b2 = -al / a0; a1 = -2 * std::cos(w) / a0; a2 = (1 - al) / a0; }
        float process(float x) { const double y = b0 * x + z1; z1 = -a1 * y + z2; z2 = b2 * x - a2 * y; return static_cast<float>(y * gain); } };
    std::vector<Mode> room;
    for (double f : { 900.0, 2400.0 }) room.emplace_back(f, 6.0, 0.55); // peak loop gain -5 dB: stable, the climb of the learning makes it ring
    std::vector<float> spk(static_cast<size_t>(fs * 400) + delay + blk, 0.f);
    std::mt19937 rng(5);
    std::normal_distribution<float> nd(0.f, 0.001f);
    juce::AudioBuffer<float> buf(1, blk);
    juce::MidiBuffer midi;
    // VDL_VOICE=file.wav: someone speaks that recording (looped) instead of the synthetic voice
    std::vector<float> speech;
    if (const char* vf = std::getenv("VDL_VOICE")) {
        std::ifstream f(vf, std::ios::binary); // 16-bit PCM WAV: walk the chunks to "data"
        std::vector<char> b((std::istreambuf_iterator<char>(f)), {});
        for (size_t i = 12; i + 8 < b.size();) {
            std::uint32_t sz;
            std::memcpy(&sz, &b[i + 4], 4);
            if (!std::memcmp(&b[i], "data", 4)) {
                std::vector<float> raw(sz / 2);
                float pk = 1e-6f;
                for (size_t k = 0; k < raw.size() && i + 8 + 2 * k + 1 < b.size(); ++k) {
                    std::int16_t v;
                    std::memcpy(&v, &b[i + 8 + 2 * k], 2);
                    raw[k] = v / 32768.f;
                    pk = std::max(pk, std::fabs(raw[k]));
                }
                for (float v : raw) speech.push_back(v * 0.15f / pk);
                break;
            }
            i += 8 + sz + (sz & 1);
        }
    }
    // VDL_LEVEL_DB=-20: the voice and the noise reach the input 20 dB lower (a quieter mic or a more distant speaker)
    const float micGain = std::getenv("VDL_LEVEL_DB") ? std::pow(10.f, static_cast<float>(std::atof(std::getenv("VDL_LEVEL_DB"))) / 20.f) : 1.f;
    p.startLearn();
    double ph = 0;
    long t = 0;
    const long maxT = static_cast<long>(fs * 300);
    while (t < maxT) {
        for (int i = 0; i < blk; ++i) {
            const double tt = static_cast<double>(t + i) / fs;
            float v = 0.f;
            if (!speech.empty()) {
                const size_t idx = static_cast<size_t>(t + i) % (speech.size() + static_cast<size_t>(fs * 0.7)); // then 0.7 s of silence
                if (tt > 1.0 && idx < speech.size()) v = speech[idx];
            } else if (tt > 1.0 && std::fmod(tt, 1.0) < 0.6) { // someone speaks
                ph += 6.283185307179586 * 160.0 * (1 + 0.02 * std::sin(6.283185307179586 * 5.5 * tt)) / fs;
                double s = 0;
                for (int h = 1; h <= 20; ++h) s += std::sin(h * ph) / h;
                v = static_cast<float>(0.04 * s);
            }
            float a = (nd(rng) + v) * micGain; // the mic level changes, the room (speaker to mic) does not
            for (auto& m : room) a += m.process(spk[static_cast<size_t>(t + i)]);
            buf.setSample(0, i, a);
        }
        p.processBlock(buf, midi);
        for (int i = 0; i < blk; ++i) spk[static_cast<size_t>(t + i + delay)] = std::clamp(buf.getSample(0, i), -1.f, 1.f);
        t += blk;
        const auto tm = p.telemetry();
        if (std::getenv("VDL_TRACE") && t % static_cast<long>(fs * 2) < blk)
            std::printf("  t=%5.1f s gain +%4.1f dB, phase %d, waiting %d, howls %d, guard %.1f dB, out peak %.2f\n", t / fs, tm.learnGainDb, tm.learnPhase, tm.learnWaiting, tm.learnHowls, tm.guardDb, std::fabs(buf.getMagnitude(0, 0, blk)));
        if (t > fs * 12 && tm.learnPhase == 0) break;
    }
    const auto tm = p.telemetry();
    std::printf("t=%5.1f s  learn finished: result %d, howls %d, notches kept %d, margin +%.0f dB, CPU %.1f %%\n",
                t / fs, tm.learnResult, tm.learnHowls, tm.learnedCount, tm.learnMarginDb, tm.cpuLoad * 100.f);
    // save and restore the project state: the learned room must come back
    juce::MemoryBlock state;
    p.getStateInformation(state);
    VonDeLarsenProcessor q;
    q.setActiveChannels(1);
    q.setPlayConfigDetails(1, 1, fs, blk);
    q.setRateAndBufferSizeDetails(fs, blk);
    q.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
    q.prepareToPlay(fs, blk);
    juce::AudioBuffer<float> quiet(1, blk);
    quiet.clear();
    for (int i = 0; i < 40; ++i) q.processBlock(quiet, midi);
    const auto tq = q.telemetry();
    std::printf("restored project: state %zu bytes, notches kept %d (before: %d)\n", state.getSize(), tq.learnedCount, tm.learnedCount);
    return 0;
}

// Developer self-test of High CPU: toggling it while audio runs must change the latency the host is told, and the
// bypassed sound must stay aligned with it, bit for bit. Usage: VonDeLarsenPreview x highcpu
static int highCpuTest()
{
    VonDeLarsenProcessor p;
    constexpr double fs = 48000;
    constexpr int blk = 64;
    p.setActiveChannels(1);
    p.setPlayConfigDetails(1, 1, fs, blk);
    p.setRateAndBufferSizeDetails(fs, blk);
    p.prepareToPlay(fs, blk);
    std::printf("start: latency %d samples, neural model loaded: %s\n", p.getLatencySamples(), p.neuralModelLoaded() ? "yes" : "NO");

    std::mt19937 rng(8);
    std::normal_distribution<float> nd(0.f, 0.1f);
    std::vector<float> in, out;
    juce::MidiBuffer midi;
    juce::AudioBuffer<float> buf(1, blk);
    auto run = [&](int blocks) {
        for (int b = 0; b < blocks; ++b) {
            for (int i = 0; i < blk; ++i) { const float v = nd(rng); buf.setSample(0, i, v); in.push_back(v); }
            p.processBlock(buf, midi);
            for (int i = 0; i < blk; ++i) out.push_back(buf.getSample(0, i));
        }
    };
    auto set = [&p](const char* id, float v) { auto* prm = p.apvts.getParameter(id); prm->setValueNotifyingHost(prm->convertTo0to1(v)); };
    auto aligned = [&](int latency, size_t from) {
        for (size_t i = from; i < out.size(); ++i)
            if (std::memcmp(&out[i], &in[i - static_cast<size_t>(latency)], sizeof(float)) != 0) return false;
        return true;
    };
    set("bypass", 1.f);
    run(200);
    const bool a256 = aligned(256, in.size() - 64 * 100);
    set("highCpu", 1.f);
    run(200);
    juce::MessageManager::getInstance()->runDispatchLoopUntil(100); // the latency is announced from the message thread
    const bool a128 = aligned(0, in.size() - 64 * 100);
    std::printf("normal: bypass aligned on 256 samples: %s\n", a256 ? "yes" : "NO");
    std::printf("high cpu: latency %d samples, bypass aligned on 0 samples: %s\n", p.getLatencySamples(), a128 ? "yes" : "NO");
    set("highCpu", 0.f);
    run(200);
    juce::MessageManager::getInstance()->runDispatchLoopUntil(100);
    std::printf("back to normal: latency %d samples, bypass aligned on 256 samples: %s\n", p.getLatencySamples(), aligned(256, in.size() - 64 * 100) ? "yes" : "NO");
    set("bypass", 0.f);
    set("highCpu", 1.f);
    run(400); // processed: just must not blow up
    float peak = 0.f;
    for (size_t i = out.size() - 64 * 100; i < out.size(); ++i) peak = std::max(peak, std::fabs(out[i]));
    std::printf("processed in high cpu: peak %.3f, latency %d\n", peak, p.getLatencySamples());
    return 0;
}

// Developer self-test of the learn button: click it in the real editor, feed audio, see whether the processor starts.
// Usage: VonDeLarsenPreview x uilearn
static juce::Component* findById(juce::Component* c, const juce::String& id)
{
    if (c->getComponentID() == id) return c;
    for (auto* ch : c->getChildren())
        if (auto* r = findById(ch, id)) return r;
    return nullptr;
}

static int uiLearnTest()
{
    VonDeLarsenProcessor p;
    constexpr double fs = 48000;
    constexpr int blk = 64;
    p.setActiveChannels(2);
    p.setPlayConfigDetails(2, 2, fs, blk);
    p.setRateAndBufferSizeDetails(fs, blk);
    p.prepareToPlay(fs, blk);
    std::unique_ptr<juce::AudioProcessorEditor> ed(p.createEditor());
    ed->setSize(720, 810);
    auto* learn = dynamic_cast<juce::Button*>(findById(ed.get(), "learn"));
    std::printf("learn button found: %s, showing: %s, enabled: %s\n", learn ? "yes" : "NO", learn && learn->isShowing() ? "yes" : "no", learn && learn->isEnabled() ? "yes" : "no");
    if (!learn) return 1;
    juce::AudioBuffer<float> buf(2, blk);
    juce::MidiBuffer midi;
    std::mt19937 rng(3);
    std::normal_distribution<float> nd(0.f, 0.003f);
    auto blocks = [&](int n) {
        for (int b = 0; b < n; ++b) {
            for (int c = 0; c < 2; ++c) for (int i = 0; i < blk; ++i) buf.setSample(c, i, nd(rng));
            p.processBlock(buf, midi);
        }
        juce::MessageManager::getInstance()->runDispatchLoopUntil(30);
    };
    blocks(50);
    learn->triggerClick();
    blocks(10);
    std::printf("after triggerClick: learning %d\n", p.telemetry().learning);
    if (learn->onClick) learn->onClick();
    blocks(50);
    const auto t = p.telemetry();
    std::printf("after click: learning %d, phase %d, waiting for voice %d, result %d\n", t.learning, t.learnPhase, t.learnWaiting, t.learnResult);
    return 0;
}

int main(int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI init;
    const juce::String out = argc > 1 ? argv[1] : "editor.png";
    if (argc > 2 && juce::String(argv[2]) == "icon") { renderIcon(juce::File(out)); return 0; }
    if (argc > 2 && juce::String(argv[2]) == "selftest") return selfTest();
    if (argc > 2 && juce::String(argv[2]) == "highcpu") return highCpuTest();
    if (argc > 2 && juce::String(argv[2]) == "uilearn") return uiLearnTest();
    bool help = false, standalone = false, bypass = false, learn = false;
    for (int i = 2; i < argc; ++i) {
        help = help || juce::String(argv[i]) == "help";
        standalone = standalone || juce::String(argv[i]) == "standalone";
        bypass = bypass || juce::String(argv[i]) == "bypass";
        learn = learn || juce::String(argv[i]) == "learn";
    }

    VonDeLarsenProcessor p;
    constexpr double fs = 48000;
    constexpr int blk = 64;
    p.setPlayConfigDetails(2, 2, fs, blk);
    if (standalone) { // pretend to be the standalone app, to see its extra controls
        p.setActiveChannels(4);
        p.hostHooks.setChannels = [&p](int n) { p.setActiveChannels(n); };
        p.hostHooks.openAudioSettings = [] {};
        p.hostHooks.deviceLatencyMs = [] { return 4.2; };
    }
    p.setRateAndBufferSizeDetails(fs, blk);
    p.prepareToPlay(fs, blk);
    if (auto* nr = p.apvts.getParameter("nrAmount")) nr->setValueNotifyingHost(nr->convertTo0to1(21.f));

    if (learn) p.startLearn();
    // three howls that ramp up over 1.5 s: 900 Hz at 2 s, 2400 Hz at 14 s, 5200 Hz at 18 s
    struct Howl { double f, start, end; };
    const Howl howls[] = { { 900, 2, 10 }, { 2400, 14, 24 }, { 5200, 18, 30 } };
    std::mt19937 rng(1);
    std::normal_distribution<float> noise(0.f, 0.001f);
    const int nCh = p.activeChannels() > 2 ? p.activeChannels() : 2;
    if (auto* a = p.apvts.getParameter("autoMixOn")) a->setValueNotifyingHost(1.f);
    juce::AudioBuffer<float> buf(nCh, blk);
    juce::MidiBuffer midi;
    const long total = static_cast<long>(fs * 29);
    for (long n = 0; n < total; n += blk) {
        for (int i = 0; i < blk; ++i) {
            const double t = static_cast<double>(n + i) / fs;
            float v = noise(rng);
            for (const auto& h : howls)
                if (t >= h.start && t < h.end) v += static_cast<float>(std::fmin(0.3, 0.002 * std::exp(3.0 * (t - h.start))) * std::sin(6.283185307179586 * h.f * t));
            if (t >= 27.0) // a little voice at the end, so the gate lamp shows VOIX
                for (int h = 1; h <= 12; ++h) v += static_cast<float>(0.12 * std::sin(6.283185307179586 * 140.0 * h * t) / h);
            buf.setSample(0, i, v);
            for (int c = 1; c < nCh; ++c) { // the other microphones: noise, a quieter talker on channel 3, noise
                float w = noise(rng);
                if (c == 2 && t >= 27.0)
                    for (int h = 1; h <= 12; ++h) w += static_cast<float>(0.05 * std::sin(6.283185307179586 * 220.0 * h * t) / h);
                buf.setSample(c, i, w);
            }
        }
        p.processBlock(buf, midi);
    }

    if (bypass)
        if (auto* b = p.apvts.getParameter("bypass")) b->setValueNotifyingHost(1.f);
    if (learn) { // pretend two notches were learned, to show the strip
        const vc::FeedbackSuppressor::Notch kept[] = { { 450.f, 18.f, 20.f }, { 2400.f, 21.f, 14.f } };
        p.setLearnedNotches(0, kept, 2);
        juce::AudioBuffer<float> silent(nCh, blk);
        silent.clear();
        for (int i = 0; i < 8; ++i) p.processBlock(silent, midi);
    }
    std::unique_ptr<juce::AudioProcessorEditor> ed(p.createEditor());
    ed->setSize(VdlEditor::kW, VdlEditor::kH);
    if (help) static_cast<VdlEditor*>(ed.get())->showHelp(true);
    const auto img = ed->createComponentSnapshot(ed->getLocalBounds(), true, 1.5f);
    juce::File f(out);
    f.deleteFile();
    juce::FileOutputStream os(f);
    if (!os.openedOk()) return 1;
    juce::PNGImageFormat png;
    png.writeImageToStream(img, os);
    return 0;
}
