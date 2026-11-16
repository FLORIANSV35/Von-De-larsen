#include "vc/SpectralDenoiser.h"

#include <algorithm>
#include <cmath>

namespace vc {

namespace {
constexpr float kNoiseRiseDbPerSec = 6.0f; // how fast the noise floor may follow an increase
constexpr float kNoiseBias = 2.5f;         // min-tracking underestimates the mean
constexpr float kSmooth = 0.8f;            // power smoothing for the noise tracker
constexpr float kDdAlpha = 0.92f;          // decision-directed weight
constexpr float kVoiceHiHz = 3900.0f;      // top of the band used to judge voice
constexpr float kHfProtectHz = 3000.0f;   // above this the noise reduction only goes half as deep (in dB): sibilants live here
constexpr float kNnTargetDb = -30.0f;      // voice-band level (amplitude-normalised dB) the model is happy with: speech peaking near -15 dBFS
constexpr float kNnMaxLiftDb = 40.0f;
constexpr float kNnDecayDbPerSec = 6.0f;   // how fast the tracked speech level falls in a pause
constexpr float kNnFloorAboveNoiseDb = 12.0f; // never lift noise up to the level of speech
constexpr float kGateHoldSec = 0.35f;
constexpr float kGateReleaseSec = 0.20f;
} // namespace

SpectralDenoiser::SpectralDenoiser()
    : fft_(kN), fftCep_(kCep), cep_(kCep)
    , winFull_(kN), featBuf_(kN)
    , hist_(kN, 0.f), win_(kN), acc_(kN, 0.f), outBlock_(kHop, 0.f)
    , ps_(kBins), pn_(kBins), prevClean_(kBins), feat_(kBins), nn_(kBins), gain_(kBins)
    , buf_(kN)
{
    prepare(48000);
}

void SpectralDenoiser::prepare(double fs)
{
    fs_ = fs;
    win_len_ = mode_ == Mode::Low ? kN / 2 : kN;
    // Zero mode keeps the normal hop: the estimators are tuned for it (measured: hops of 8..64 are worse, 128 is as good as normal mode)
    hop_ = mode_ == Mode::Low ? kN / 8 : kHop;
    hOld_.assign(static_cast<size_t>(kTaps), 0.f);
    hNew_.assign(static_cast<size_t>(kTaps), 0.f);
    nnEvery_ = kHop / hop_;
    staleLimit_ = kMaxStaleHops * nnEvery_;
    smooth_ = std::pow(kSmooth, static_cast<float>(hop_) / kHop); // the per-hop constants keep their time constants
    ddAlpha_ = std::pow(kDdAlpha, static_cast<float>(hop_) / kHop);
    outScale_ = 2.0f * hop_ / win_len_;
    win_.assign(static_cast<size_t>(kN), 0.f);
    for (int j = 0; j < win_len_; ++j) win_[static_cast<size_t>(j)] = static_cast<float>(std::sin(3.14159265358979323846 * j / win_len_));
    for (int j = 0; j < kN; ++j) winFull_[static_cast<size_t>(j)] = static_cast<float>(std::sin(3.14159265358979323846 * j / kN));
    rise_ = static_cast<float>(std::pow(10.0, (kNoiseRiseDbPerSec / 10.0) * hop_ / fs));
    hfBin_ = std::clamp(static_cast<int>(kHfProtectHz / (fs / kN)), 2, kBins - 1);
    voiceHi_ = std::clamp(static_cast<int>(kVoiceHiHz / (fs / kN)), 2, kBins - 2);
    holdHops_ = std::max(1, static_cast<int>(std::lround(kGateHoldSec * fs / hop_)));
    releaseHops_ = std::max(1, static_cast<int>(std::lround(kGateReleaseSec * fs / hop_)));
    reset();
}

void SpectralDenoiser::reset()
{
    std::fill(hist_.begin(), hist_.end(), 0.f);
    std::fill(acc_.begin(), acc_.end(), 0.f);
    std::fill(outBlock_.begin(), outBlock_.end(), 0.f);
    std::fill(prevClean_.begin(), prevClean_.end(), 0.f);
    wr_ = pos_ = 0;
    std::fill(hOld_.begin(), hOld_.end(), 0.f);
    std::fill(hNew_.begin(), hNew_.end(), 0.f);
    hOld_[0] = hNew_[0] = 1.f; // starts as a plain pass-through
    gatePrev_ = gateTgt_ = 1.f;
    nnPhase_ = nnEvery_ - 1; // the first frame feeds the model
    nnAge_ = 1 << 20;
    first_ = true;
    nnLevelDb_ = noiseBandDb_ = -100.f;
    nnLiftDb_ = 0.f;
    open_ = true;
    holdLeft_ = 0;
    gateDb_ = 0.f;
    prevGateLin_ = 1.f;
    voice_.store(true, std::memory_order_relaxed);
}

void SpectralDenoiser::setReductionDb(float db)
{
    gMin_.store(std::pow(10.0f, -std::clamp(db, 0.f, 40.f) / 20.0f), std::memory_order_relaxed);
}

void SpectralDenoiser::process(const float* in, float* out, int n) noexcept
{
    if (mode_ == Mode::Zero) {
        const float* ho = hOld_.data();
        const float* hn = hNew_.data();
        for (int i = 0; i < n; ++i) {
            hist_[static_cast<size_t>(wr_)] = in[i];
            wr_ = (wr_ + 1) & (kN - 1);
            float yo = 0.f, yn = 0.f;
            for (int k = 0; k < kTaps; ++k) {
                const float x = hist_[static_cast<size_t>((wr_ - 1 - k) & (kN - 1))];
                yo += ho[k] * x;
                yn += hn[k] * x;
            }
            const float a = static_cast<float>(pos_ + 1) / static_cast<float>(hop_); // new filter and new gate level fade in over the hop
            out[i] = (yo + (yn - yo) * a) * (gatePrev_ + (gateTgt_ - gatePrev_) * a);
            if (++pos_ == hop_) {
                pos_ = 0;
                runFrame();
                ho = hOld_.data();
                hn = hNew_.data();
            }
        }
        return;
    }
    for (int i = 0; i < n; ++i) {
        hist_[static_cast<size_t>(wr_)] = in[i];
        wr_ = (wr_ + 1) & (kN - 1);
        out[i] = outBlock_[static_cast<size_t>(pos_)];
        if (++pos_ == hop_) {
            pos_ = 0;
            runFrame();
        }
    }
}

void SpectralDenoiser::runFrame() noexcept
{
    // the last win_len_ samples, zero padded to the FFT size
    for (int j = 0; j < kN; ++j)
        buf_[static_cast<size_t>(j)] = j < win_len_
            ? Fft::cf(hist_[static_cast<size_t>((wr_ - win_len_ + j) & (kN - 1))] * win_[static_cast<size_t>(j)], 0.f)
            : Fft::cf(0.f, 0.f);
    fft_.forward(buf_.data());

    const float gMin = gMin_.load(std::memory_order_relaxed);
    const float gMinHf = std::sqrt(gMin);
    constexpr float amp = 3.14159265f / kN; // sum(win) = 2N/pi -> amplitude normalisation

    bool useNn = false;
    if (provider_) {
        if (++nnPhase_ >= nnEvery_) { // the model sees a full-length window at its own cadence
            nnPhase_ = 0;
            const Fft::cf* spec = buf_.data();
            if (win_len_ != kN) {
                for (int j = 0; j < kN; ++j)
                    featBuf_[static_cast<size_t>(j)] = Fft::cf(hist_[static_cast<size_t>((wr_ + j) & (kN - 1))] * winFull_[static_cast<size_t>(j)], 0.f);
                fft_.forward(featBuf_.data());
                spec = featBuf_.data();
            }
            float band = 0.f; // level of the voice band, to lift a quiet voice into the range the model knows
            for (int k = 1; k <= voiceHi_; ++k) band += std::norm(spec[k]) * amp * amp;
            const float frameDb = 10.0f * std::log10(band + 1e-12f);
            nnLevelDb_ = frameDb > nnLevelDb_ ? frameDb : nnLevelDb_ - kNnDecayDbPerSec * static_cast<float>(kHop) / static_cast<float>(fs_);
            nnLevelDb_ = std::max(nnLevelDb_, noiseBandDb_ + kNnFloorAboveNoiseDb);
            nnLiftDb_ = std::clamp(kNnTargetDb - nnLevelDb_, 0.f, kNnMaxLiftDb);
            for (int k = 0; k < kBins; ++k) {
                const float p = std::norm(spec[k]) * amp * amp;
                feat_[static_cast<size_t>(k)] = 10.0f * std::log10(p + 1e-10f) + nnLiftDb_;
            }
            provider_->submit(feat_.data(), kBins);
        }
        if (provider_->fetch(nn_.data(), kBins)) nnAge_ = 0;
        else if (nnAge_ < (1 << 20)) ++nnAge_;
        useNn = nnAge_ <= staleLimit_;
    }
    usingNn_.store(useNn, std::memory_order_relaxed);
    haveFresh_ = useNn;
    const float mix = useNn ? std::clamp(mix_.load(std::memory_order_relaxed), 0.f, 1.f) : 0.f;

    float bandSig = 0.f, bandNoise = 0.f, bandNn = 0.f;
    for (int k = 0; k < kBins; ++k) {
        const size_t i = static_cast<size_t>(k);
        const float p = std::norm(buf_[i]);
        if (first_) ps_[i] = pn_[i] = p;
        ps_[i] = smooth_ * ps_[i] + (1.f - smooth_) * p;
        pn_[i] = ps_[i] < pn_[i] ? ps_[i] : pn_[i] * rise_;
        const float noise = std::max(pn_[i] * kNoiseBias, 1e-12f);
        if (k >= 1 && k <= voiceHi_) {
            bandSig += p;
            bandNoise += noise;
            if (useNn) bandNn += p * nn_[i];
        }

        const float gamma = p / noise;
        const float xi = std::max(ddAlpha_ * prevClean_[i] / noise + (1.f - ddAlpha_) * std::max(gamma - 1.f, 0.f), 1e-3f);
        float g = xi / (1.f + xi); // classic Wiener gain, always computed: it keeps the noise tracker honest
        if (mix > 0.f) g = std::exp((1.f - mix) * std::log(g) + mix * std::log(std::max(nn_[i], 1e-4f)));
        g = std::clamp(g, static_cast<int>(i) >= hfBin_ ? gMinHf : gMin, 1.f);
        prevClean_[i] = g * g * p;
        gain_[i] = g;
    }
    first_ = false;

    noiseBandDb_ = 10.0f * std::log10(bandNoise * amp * amp + 1e-12f);

    // ---- voice gate: decide once per hop, applied to this hop's output block below
    const float snrDb = 10.0f * std::log10((bandSig + 1e-12f) / (bandNoise + 1e-12f));
    const float vClassic = std::clamp((snrDb - 1.5f) / 6.0f, 0.f, 1.f);
    const float vNeural = std::clamp((bandNn / (bandSig + 1e-12f) - 0.25f) / 0.35f, 0.f, 1.f);
    // The neural mask judges absolute levels: a faint steady noise looks like clean signal to it, so alone it says
    // "voice" in 86-99 % of the pauses. It only gets a small say, enough to keep an open gate open through weak
    // syllables (0.3 closes, 0.5 opens), never enough to open it on noise.
    const float v = std::min(1.f, vClassic + 0.25f * mix * vNeural);
    score_.store(v, std::memory_order_relaxed);
    const bool gateOn = gateOn_.load(std::memory_order_relaxed);
    if (gateOn) {
        if (v > (open_ ? 0.3f : 0.5f)) { open_ = true; holdLeft_ = holdHops_; }
        else if (holdLeft_ > 0) --holdLeft_;
        else open_ = false;
        const float depth = gateDepthDb_.load(std::memory_order_relaxed);
        if (open_) gateDb_ = 0.f;
        else gateDb_ = std::max(-depth, gateDb_ - depth / static_cast<float>(releaseHops_));
        gateDb_ = std::max(gateDb_, -depth);
    } else {
        open_ = true;
        holdLeft_ = 0;
        gateDb_ = 0.f;
    }
    voice_.store(open_, std::memory_order_relaxed);

    if (mode_ == Mode::Zero) {
        hOld_ = hNew_;
        buildMinPhase();
        gatePrev_ = gateTgt_;
        gateTgt_ = std::pow(10.0f, gateDb_ / 20.0f);
        return;
    }
    for (int k = 0; k < kBins; ++k) buf_[static_cast<size_t>(k)] *= gain_[static_cast<size_t>(k)];
    for (int k = 1; k < kBins - 1; ++k) buf_[static_cast<size_t>(kN - k)] = std::conj(buf_[static_cast<size_t>(k)]);
    fft_.inverse(buf_.data());

    for (int j = 0; j < win_len_; ++j) acc_[static_cast<size_t>(j)] += buf_[static_cast<size_t>(j)].real() * win_[static_cast<size_t>(j)] * outScale_;
    std::copy(acc_.begin(), acc_.begin() + hop_, outBlock_.begin());
    const float gateLin = std::pow(10.0f, gateDb_ / 20.0f);
    if (gateLin != 1.f || prevGateLin_ != 1.f) { // ramp across the block: no clicks when the gate moves
        for (int j = 0; j < hop_; ++j)
            outBlock_[static_cast<size_t>(j)] *= prevGateLin_ + (gateLin - prevGateLin_) * static_cast<float>(j + 1) / hop_;
        prevGateLin_ = gateLin;
    }
    std::copy(acc_.begin() + hop_, acc_.end(), acc_.begin());
    std::fill(acc_.end() - hop_, acc_.end(), 0.f);
}

// Gains (kBins magnitudes) -> minimum-phase impulse response: fold the real cepstrum of the log magnitude. The log
// gains are interpolated to a finer grid first (kCep points): on the kN grid the cepstrum of a jagged gain curve wraps
// around and the filter loses the high frequencies.
void SpectralDenoiser::buildMinPhase() noexcept
{
    constexpr int ratio = kCep / kN;
    constexpr int half = kCep / 2;
    for (int k = 0; k <= half; ++k) {
        const int i = k / ratio, r = k % ratio;
        const float a = std::log(std::max(gain_[static_cast<size_t>(i)], 1e-4f));
        const float b = i + 1 < kBins ? std::log(std::max(gain_[static_cast<size_t>(i + 1)], 1e-4f)) : a;
        cep_[static_cast<size_t>(k)] = Fft::cf(a + (b - a) * static_cast<float>(r) / ratio, 0.f);
    }
    for (int k = 1; k < half; ++k) cep_[static_cast<size_t>(kCep - k)] = cep_[static_cast<size_t>(k)];
    fftCep_.inverse(cep_.data()); // real cepstrum
    for (int n = 1; n < half; ++n) cep_[static_cast<size_t>(n)] *= 2.f;
    for (int n = half + 1; n < kCep; ++n) cep_[static_cast<size_t>(n)] = Fft::cf(0.f, 0.f);
    fftCep_.forward(cep_.data()); // log H of the minimum-phase response
    for (int k = 0; k < kCep; ++k) cep_[static_cast<size_t>(k)] = std::exp(cep_[static_cast<size_t>(k)].real()) * std::polar(1.f, cep_[static_cast<size_t>(k)].imag());
    fftCep_.inverse(cep_.data());
    constexpr int kTail = 48; // the response is cut at kTaps: fade the end out
    for (int n = 0; n < kTaps; ++n) {
        const float w = n < kTaps - kTail ? 1.f : 0.5f * (1.f + std::cos(3.14159265f * static_cast<float>(n - (kTaps - kTail)) / kTail));
        hNew_[static_cast<size_t>(n)] = cep_[static_cast<size_t>(n)].real() * w;
    }
}

} // namespace vc
