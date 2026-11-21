// The cockpit audio device: SDL2 core audio (no SDL2_mixer), 48 kHz, 16-bit stereo, 512-sample buffers. The
// frame loop only queues cues (play); the SDL audio callback mixes them (Mixer, audio.hpp) and keeps the last
// 60 s of the exact device samples for dumps. No device = silent, logged once, never a startup failure.
#pragma once

#include <SDL.h>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

#include "audio.hpp"
#include "config.hpp"

namespace ksphmi {

class AudioOut {
public:
    static constexpr int RATE = AUDIO_RATE, CHANNELS = 2, BUFFER = 512;
    static constexpr int DUMP_SECONDS = 60;

    /// Opens the device unless `settings.enabled` is false. Logs `audio: device ...` or why it stays silent.
    explicit AudioOut(const AudioConfig& settings);
    ~AudioOut();
    AudioOut(const AudioOut&) = delete;
    AudioOut& operator=(const AudioOut&) = delete;

    /// A device is open and playing.
    bool active() const { return dev_ != 0; }
    /// "" when not active.
    const std::string& device() const { return device_; }
    /// Queue one frame's cues and hum level (never blocks; a full queue drops cues).
    void play(const AudioPlan& plan);
    /// Ctrl+M; returns the new state.
    bool toggle_mute();
    bool muted() const { return mixer_.muted(); }
    /// Write the last DUMP_SECONDS of device samples to `path` (main thread; SIGUSR2). False when not active or on
    /// an I/O error.
    bool dump(const std::string& path);
    /// The session recording ([audio] dump_dir), "" when none.
    const std::string& session_path() const { return session_path_; }
    /// Samples the device has asked for so far (tests).
    uint64_t samples_played() const { return ring_.total(); }

private:
    static void callback(void* self, Uint8* stream, int len);
    void record_loop();

    Mixer mixer_;
    SampleRing ring_;
    SDL_AudioDeviceID dev_ = 0;
    bool subsystem_ = false;
    std::string device_, session_path_;
    // [audio] dump_dir: a thread appends what is new in ring_ to the session WAV every quarter second.
    std::thread recorder_;
    std::mutex record_mutex_;
    std::condition_variable record_wake_;
    bool record_stop_ = false;
    WavWriter session_;
};

}  // namespace ksphmi
