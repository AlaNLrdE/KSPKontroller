#include "audioout.hpp"

#include <ctime>
#include <sys/stat.h>
#if defined(_WIN32)
#include <direct.h>
#endif

#include "fmt.hpp"
#include "log.hpp"

namespace ksphmi {

namespace {

const char* LOG = "ksphmi";  // lines read "ksphmi: audio: ..." like the rest of the main log

void make_dir(const std::string& dir) {
#if defined(_WIN32)
    _mkdir(dir.c_str());
#else
    mkdir(dir.c_str(), 0755);
#endif
}

std::string session_file(const std::string& dir) {
    char stamp[32] = "session";
    const std::time_t t = std::time(nullptr);
    if (const std::tm* tm = std::localtime(&t)) std::strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", tm);
    return dir + "/ksphmi-audio-" + stamp + ".wav";
}

}  // namespace

AudioOut::AudioOut(const AudioConfig& s) : mixer_(RATE), ring_(size_t(DUMP_SECONDS) * RATE * CHANNELS) {
    AudioGains g;
    g.volume = float(s.volume);
    g.category = {float(s.alerts), float(s.feedback), float(s.events), float(s.ambient)};
    mixer_.set_gains(g);
    if (!s.enabled) {
        log::info(LOG, "audio: off ([audio] enabled = false or --no-audio)");
        return;
    }
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        log::warning(LOG, std::string("audio: no audio subsystem (") + SDL_GetError() + "): silent");
        return;
    }
    subsystem_ = true;
    SDL_AudioSpec want{}, have{};
    want.freq = RATE;
    want.format = AUDIO_S16SYS;
    want.channels = CHANNELS;
    want.samples = BUFFER;
    want.callback = &AudioOut::callback;
    want.userdata = this;
    // allowed_changes 0: SDL converts to whatever the hardware wants, so the callback always gets this format
    dev_ = SDL_OpenAudioDevice(s.device.empty() ? nullptr : s.device.c_str(), 0, &want, &have, 0);
    if (!dev_) {
        log::warning(LOG, std::string("audio: no output device") + (s.device.empty() ? "" : " '" + s.device + "'") +
                              " (" + SDL_GetError() + "): silent");
        return;
    }
    device_ = s.device;
#if SDL_VERSION_ATLEAST(2, 24, 0)
    if (device_.empty()) {
        char* name = nullptr;
        SDL_AudioSpec spec{};
        if (SDL_GetDefaultAudioInfo(&name, &spec, 0) == 0 && name) device_ = name;
        SDL_free(name);
    }
#endif
    if (device_.empty()) device_ = "default";
    const char* driver = SDL_GetCurrentAudioDriver();
    log::info(LOG, fmt::str("audio: device \"%s\" (driver %s), %d Hz, %d ch, %d samples", device_.c_str(),
                            driver ? driver : "?", RATE, CHANNELS, BUFFER));
    if (!s.dump_dir.empty()) {
        make_dir(s.dump_dir);
        session_path_ = session_file(s.dump_dir);
        if (session_.open(session_path_, RATE, CHANNELS)) {
            log::info(LOG, "audio: recording the session to " + session_path_);
            recorder_ = std::thread([this] { record_loop(); });
        } else {
            log::warning(LOG, "audio: cannot write " + session_path_ + ": no session recording");
            session_path_.clear();
        }
    }
    SDL_PauseAudioDevice(dev_, 0);
}

AudioOut::~AudioOut() {
    if (dev_) SDL_CloseAudioDevice(dev_);  // waits for a running callback: nothing writes ring_ after this
    if (recorder_.joinable()) {
        {
            std::lock_guard<std::mutex> lock(record_mutex_);
            record_stop_ = true;
        }
        record_wake_.notify_all();
        recorder_.join();
    }
    if (subsystem_) SDL_QuitSubSystem(SDL_INIT_AUDIO);
}

void AudioOut::callback(void* self, Uint8* stream, int len) {
    auto* out = static_cast<AudioOut*>(self);
    auto* samples = reinterpret_cast<int16_t*>(stream);
    const int count = len / int(sizeof(int16_t));
    out->mixer_.mix(samples, count / CHANNELS, CHANNELS);
    out->ring_.write(samples, size_t(count));
}

void AudioOut::play(const AudioPlan& plan) {
    if (!dev_) return;
    for (const Cue& c : plan.cues) mixer_.play(c);
    mixer_.set_hum(plan.hum);
}

bool AudioOut::toggle_mute() {
    mixer_.set_muted(!mixer_.muted());
    return mixer_.muted();
}

bool AudioOut::dump(const std::string& path) {
    if (!dev_) return false;
    return write_wav(path, RATE, CHANNELS, ring_.snapshot());
}

void AudioOut::record_loop() {
    uint64_t from = ring_.total();
    bool warned = false;
    for (;;) {
        bool stop;
        {
            std::unique_lock<std::mutex> lock(record_mutex_);
            record_wake_.wait_for(lock, std::chrono::milliseconds(250), [&] { return record_stop_; });
            stop = record_stop_;
        }
        uint64_t lost = 0;
        const std::vector<int16_t> fresh = ring_.since(from, &lost);
        if (lost && !warned) log::warning(LOG, "audio: session recording fell behind, samples skipped"), warned = true;
        if (!fresh.empty() && !session_.append(fresh.data(), fresh.size()) && !warned)
            log::warning(LOG, "audio: writing " + session_path_ + " failed"), warned = true;
        if (stop) break;
    }
    session_.close();
}

}  // namespace ksphmi
