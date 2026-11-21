// Cockpit audio, pure logic (no SDL): the waiting-screen animation clock shared with the painters, the
// AudioDirector that turns each frame's state into sound cues, the synthesized sounds, the voice mixer and the
// WAV writer. The SDL device lives in audioout.hpp (ksphmi_ui); docs/hmi.rst "Audio".
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

#include "model.hpp"

namespace ksphmi {

/// Angle in radians of the waiting screen's scan dot (the LINK SEARCH radar) for `skin` at monotonic `now`:
/// one rad/s on ksp1/ksp2, 1.2 rad/s on classic/apollo/steampunk. The painters and the AudioDirector both call
/// it, so the radar ping cannot drift from the picture.
double waiting_sweep_angle(const std::string& skin, double now);
/// The 1 Hz waiting annunciator: lit for the first 0.6 s of every second.
bool waiting_lamp_lit(double now);
/// True when `s` is drawn as the waiting / link-search screen (not flight, a facility or the VAB report).
bool shows_waiting_screen(const Snapshot& s);

/// The sounds of phase 1.
enum class Sound : uint8_t { RadarPing, LampClick, LinkAcquired, LinkLost, Count };
/// Gain groups ([audio] alerts / feedback / events / ambient). The hum is the only ambient sound.
enum class Category : uint8_t { Alerts, Feedback, Events, Ambient, Count };
const char* sound_name(Sound s);
Category sound_category(Sound s);

/// One sound to start now, at `gain` (1 = full) before the category and master gains.
struct Cue {
    Sound sound = Sound::RadarPing;
    float gain = 1;
    bool operator==(const Cue& o) const { return sound == o.sound && gain == o.gain; }
};

/// What the frame on screen looks like, as far as sound goes.
struct AudioFrame {
    State state = State::Connecting;
    std::string skin;            ///< Display::skin() of the frame just drawn
    bool waiting_screen = false; ///< shows_waiting_screen(snapshot)
    double now = 0;              ///< the monotonic time the frame was drawn with
};

/// Cues and hum level for one frame.
struct AudioPlan {
    std::vector<Cue> cues;
    float hum = 0;  ///< target level of the continuous ambient hum (0..1)
};

/// Turns frames into cues; called once per frame by the main loop, which only queues the result.
class AudioDirector {
public:
    /// Identical cues closer together than this are dropped.
    static constexpr double RATE_LIMIT = 0.150;
    /// The NoVessel radar ping is this much softer than the Connecting one.
    static constexpr float SOFT_PING = 0.45f;

    AudioPlan update(const AudioFrame& f);
    /// The ambient hum category (on by default).
    bool ambient = true;

private:
    bool started_ = false;
    State state_ = State::Connecting;
    bool waiting_ = false;
    std::string skin_;
    long long sweep_turn_ = 0, lamp_second_ = 0;
    std::array<double, size_t(Sound::Count)> last_ = {-1e9, -1e9, -1e9, -1e9};
    void emit(AudioPlan& plan, Sound s, float gain, double now);
};

// ------------------------------------------------------------------------------------------- synthesis

constexpr int AUDIO_RATE = 48000;
/// Each sound as mono float samples at `rate`, KSP1 Stock character: a bright blip, a soft click, a short major
/// chord, a descending tone. Deterministic (the click's noise uses a fixed seed).
std::vector<float> synthesize(Sound s, int rate = AUDIO_RATE);

/// Gains read by the mixer on the audio thread; written from the main thread.
struct AudioGains {
    float volume = 0.6f;
    std::array<float, size_t(Category::Count)> category = {1.0f, 0.8f, 0.7f, 0.5f};
};

/// Up to VOICES sounds at once plus the hum. One producer (the main thread: play, set_*) and one consumer (the
/// audio callback: mix): the cue queue is a lock-free single-producer / single-consumer ring and every setting
/// is an atomic, so the callback never waits for the frame loop.
class Mixer {
public:
    static constexpr int VOICES = 8;
    static constexpr size_t QUEUE = 64;

    explicit Mixer(int rate = AUDIO_RATE);
    int rate() const { return rate_; }

    // producer side
    /// False when the queue is full (the cue is dropped).
    bool play(const Cue& c);
    void set_gains(const AudioGains& g);
    void set_hum(float level) { hum_target_.store(level); }
    void set_muted(bool m) { muted_.store(m); }
    bool muted() const { return muted_.load(); }

    // consumer side
    /// Fill `frames` interleaved int16 frames of `channels` channels (the same mono mix in each).
    void mix(int16_t* out, int frames, int channels);
    /// Voices playing after the last mix (tests).
    int active_voices() const { return active_.load(); }

private:
    struct Voice {
        const std::vector<float>* pcm = nullptr;
        size_t pos = 0;
        float gain = 0;
        Category category = Category::Feedback;
    };
    int rate_;
    std::array<std::vector<float>, size_t(Sound::Count)> pcm_;
    std::array<Cue, QUEUE> queue_{};
    std::atomic<size_t> head_{0}, tail_{0};
    std::array<Voice, VOICES> voices_{};
    std::atomic<float> volume_{0.6f};
    std::array<std::atomic<float>, size_t(Category::Count)> category_{};
    std::atomic<float> hum_target_{0};
    std::atomic<bool> muted_{false};
    std::atomic<int> active_{0};
    float hum_level_ = 0;
    double hum_phase_ = 0;
};

// ------------------------------------------------------------------------------------------- dumps

/// The 44-byte RIFF/WAVE header of 16-bit PCM.
std::vector<uint8_t> wav_header(int rate, int channels, uint32_t data_bytes);
/// Write a whole 16-bit PCM WAV file; false on any I/O error.
bool write_wav(const std::string& path, int rate, int channels, const std::vector<int16_t>& samples);

/// A WAV file that grows: append() writes samples and rewrites the header sizes, so the file stays valid if the
/// process ends without close().
class WavWriter {
public:
    ~WavWriter() { close(); }
    bool open(const std::string& path, int rate, int channels);
    bool append(const int16_t* samples, size_t count);
    void close();
    bool is_open() const { return f_ != nullptr; }
    uint32_t data_bytes() const { return bytes_; }

private:
    std::FILE* f_ = nullptr;
    int rate_ = 0, channels_ = 0;
    uint32_t bytes_ = 0;
};

/// The last `capacity` int16 samples written (the exact device stream), plus a running total so a reader can
/// take what is new since its last read. Guarded by a mutex held only for copies.
class SampleRing {
public:
    explicit SampleRing(size_t capacity);
    void write(const int16_t* samples, size_t count);
    /// Everything kept, oldest first.
    std::vector<int16_t> snapshot() const;
    /// Samples written after `from` (a total() value), oldest first; `from` moves to the new total. Samples that
    /// were overwritten before this read are skipped (`lost` counts them).
    std::vector<int16_t> since(uint64_t& from, uint64_t* lost = nullptr) const;
    uint64_t total() const;
    size_t capacity() const { return buf_.size(); }

private:
    mutable std::mutex mutex_;
    std::vector<int16_t> buf_;
    uint64_t total_ = 0;
};

}  // namespace ksphmi
