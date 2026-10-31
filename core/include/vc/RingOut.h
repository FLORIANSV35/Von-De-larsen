#pragma once
#include <algorithm>
#include <cstdint>

namespace vc {

// Automatic ring-out for the room set-up, done WHILE SOMEONE SPEAKS. The output gain climbs slowly, but only
// while a voice is present, so that the loop gain creeps up under the real conditions of the show (the voice is
// what opens the gate and excites the room). Each time a howl is caught (the suppressor opened a notch) the climb
// pauses to let the notch settle, then goes on. It stops when the gain stands `margin` dB above the point where
// the last howl appeared, or at `maxDb`, after `maxSeconds`, or if the signal gets dangerously hot. Then the gain
// returns to zero over two seconds.
//
// The slow climb matters: a loop that crosses unity by a small amount grows slowly, and the notches have time
// to find the howl while it is still quiet.
class RingOut {
public:
    enum class State { Idle, Climbing, Settling, Finishing, Done };

    void prepare(double sampleRate) { fs_ = sampleRate; }
    void start(float marginDb = 6.f, float maxDb = 18.f, float dbPerSecond = 0.5f, float maxSeconds = 240.f)
    {
        margin_ = marginDb; max_ = maxDb; rate_ = dbPerSecond; maxSeconds_ = maxSeconds; elapsed_ = 0.f; sinceVoice_ = 0.f;
        gain_ = 0.f; lastEventGain_ = -1.f; settle_ = 0.f; held_ = 0.f; maxReached_ = 0.f; howls_ = 0; seenEvents_ = ~0u; abort_ = false; timedOut_ = false;
        state_ = State::Climbing;
    }
    void stop() { if (state_ != State::Idle && state_ != State::Done) state_ = State::Finishing; }

    // Call once per block. `events` is the suppressor's running count of howls caught, `inputDb` the rms level of
    // the input in dBFS, `outPeak` the largest output sample (linear), `voiceActive` whether someone is speaking.
    void update(int n, std::uint32_t events, float inputDb, float outPeak, bool voiceActive) noexcept
    {
        const float dt = static_cast<float>(n) / static_cast<float>(fs_);
        if (state_ == State::Idle || state_ == State::Done) return;
        elapsed_ += dt;
        sinceVoice_ = voiceActive ? 0.f : sinceVoice_ + dt;
        if (elapsed_ > maxSeconds_ && state_ != State::Finishing) { timedOut_ = true; state_ = State::Finishing; }
        if (seenEvents_ == ~0u) seenEvents_ = events;
        const bool newHowl = events > seenEvents_;
        seenEvents_ = events;

        if (state_ == State::Finishing) {
            gain_ -= dt * std::max(2.f, gain_ / 2.f); // back to zero in about two seconds
            if (gain_ <= 0.f) { gain_ = 0.f; state_ = State::Done; }
            return;
        }
        const bool tooHot = inputDb > -12.f || outPeak > 0.8f;
        if (inputDb > -3.f || outPeak > 0.97f) { gain_ = std::max(0.f, gain_ - 3.f); state_ = State::Finishing; abort_ = true; return; } // something is wrong
        if (newHowl) {
            lastEventGain_ = gain_;
            ++howls_;
            settle_ = 2.0f;
            state_ = State::Settling;
        }
        if (state_ == State::Settling) {
            settle_ -= dt;
            if (settle_ <= 0.f) state_ = State::Climbing;
            return;
        }
        if (tooHot || !voiceActive) return; // hold: the room is loud enough already, or nobody is speaking
        gain_ += rate_ * dt;
        maxReached_ = std::max(maxReached_, gain_);
        const bool margined = lastEventGain_ >= 0.f && gain_ >= lastEventGain_ + margin_;
        if (margined || gain_ >= max_) { gain_ = std::min(gain_, max_); state_ = State::Finishing; }
    }

    State state() const { return state_; }
    bool active() const { return state_ != State::Idle && state_ != State::Done; }
    bool finished() const { return state_ == State::Done; }
    bool aborted() const { return abort_; }
    bool timedOut() const { return timedOut_; }
    // the climb is waiting: nobody has been speaking for a moment
    bool waitingForVoice() const { return state_ == State::Climbing && sinceVoice_ > 1.5f; }
    float elapsedSeconds() const { return elapsed_; }
    // the highest gain the room was tested at: the learned notches are known to hold up to here
    float maxGainReachedDb() const { return maxReached_; }
    float gainDb() const { return gain_; }
    int howlsFound() const { return howls_; }
    float marginReachedDb() const { return lastEventGain_ >= 0.f ? maxReached_ - lastEventGain_ : maxReached_; }
    void reset() { state_ = State::Idle; gain_ = 0.f; }

private:
    double fs_ = 48000;
    State state_ = State::Idle;
    float margin_ = 6.f, max_ = 18.f, rate_ = 0.5f, gain_ = 0.f, lastEventGain_ = -1.f, settle_ = 0.f, held_ = 0.f, maxReached_ = 0.f;
    int howls_ = 0;
    std::uint32_t seenEvents_ = ~0u;
    bool abort_ = false, timedOut_ = false;
    float maxSeconds_ = 240.f, elapsed_ = 0.f, sinceVoice_ = 0.f;
};

} // namespace vc
