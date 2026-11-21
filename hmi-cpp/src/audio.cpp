#include "audio.hpp"

#include <algorithm>
#include <cmath>

#include "scenes.hpp"

namespace ksphmi {

namespace {

constexpr double PI = 3.14159265358979323846;
constexpr double TAU = 2 * PI;

}  // namespace

// ------------------------------------------------------------------------------------------- shared clock

double waiting_sweep_angle(const std::string& skin, double now) {
    return (skin == "ksp1" || skin == "ksp2" ? 1.0 : 1.2) * now;
}

bool waiting_lamp_lit(double now) { return std::fmod(now, 1.0) < .6; }

bool shows_waiting_screen(const Snapshot& s) {
    if (s.state == State::Flight) return false;
    if (s.state == State::NoVessel && ((s.scene == "vab" && s.ship) || find_view(s.scene))) return false;
    return true;
}

const char* sound_name(Sound s) {
    switch (s) {
        case Sound::RadarPing: return "radar ping";
        case Sound::LampClick: return "lamp click";
        case Sound::LinkAcquired: return "link acquired";
        case Sound::LinkLost: return "link lost";
        case Sound::Count: break;
    }
    return "?";
}

Category sound_category(Sound s) {
    switch (s) {
        case Sound::LinkLost: return Category::Alerts;
        case Sound::LinkAcquired: return Category::Events;
        default: return Category::Feedback;
    }
}

// ------------------------------------------------------------------------------------------- director

void AudioDirector::emit(AudioPlan& plan, Sound s, float gain, double now) {
    double& last = last_[size_t(s)];
    if (now - last < RATE_LIMIT) return;
    last = now;
    plan.cues.push_back({s, gain});
}

AudioPlan AudioDirector::update(const AudioFrame& f) {
    AudioPlan plan;
    plan.hum = ambient ? 1.0f : 0.0f;
    const long long turn = static_cast<long long>(std::floor(waiting_sweep_angle(f.skin, f.now) / TAU));
    const long long second = static_cast<long long>(std::floor(f.now));
    if (started_) {
        const bool flight = f.state == State::Flight, was = state_ == State::Flight;
        if (flight && !was) emit(plan, Sound::LinkAcquired, 1, f.now);
        else if (!flight && was) emit(plan, Sound::LinkLost, 1, f.now);
        // Only while the same radar stays on screen: a new skin turns at another rate (a jump in turns) and a
        // screen that just appeared starts its sweep silently.
        if (f.waiting_screen && waiting_ && f.skin == skin_) {
            if (turn > sweep_turn_) emit(plan, Sound::RadarPing, f.state == State::NoVessel ? SOFT_PING : 1, f.now);
            if (f.state == State::Connecting && state_ == State::Connecting && second > lamp_second_)
                emit(plan, Sound::LampClick, 1, f.now);
        }
    }
    started_ = true;
    state_ = f.state;
    waiting_ = f.waiting_screen;
    skin_ = f.skin;
    sweep_turn_ = turn;
    lamp_second_ = second;
    return plan;
}

// ------------------------------------------------------------------------------------------- synthesis

std::vector<float> synthesize(Sound s, int rate) {
    const double dt = 1.0 / rate;
    auto make = [&](double seconds) { return std::vector<float>(static_cast<size_t>(seconds * rate), 0.0f); };
    std::vector<float> out;
    switch (s) {
        case Sound::RadarPing: {  // bright blip: G6 with its octave, fast attack, quick exponential decay
            out = make(0.14);
            for (size_t i = 0; i < out.size(); ++i) {
                const double t = i * dt, env = std::min(1.0, t / 0.003) * std::exp(-t / 0.035);
                out[i] = float(env * (0.42 * std::sin(TAU * 1568 * t) + 0.14 * std::sin(TAU * 3136 * t)));
            }
            break;
        }
        case Sound::LampClick: {  // soft relay click: a short noise burst over a 2.4 kHz tick
            out = make(0.015);
            uint32_t seed = 0x2545F491u;
            for (size_t i = 0; i < out.size(); ++i) {
                seed = seed * 1664525u + 1013904223u;
                const double noise = (seed >> 8) / double(1u << 24) * 2 - 1, t = i * dt;
                out[i] = float(0.22 * noise * std::exp(-t / 0.0015) + 0.18 * std::sin(TAU * 2400 * t) * std::exp(-t / 0.003));
            }
            break;
        }
        case Sound::LinkAcquired: {  // short rising major chord: C6, E6, G6, 60 ms apart
            out = make(0.5);
            const double notes[] = {1046.5, 1318.5, 1568.0};
            for (int n = 0; n < 3; ++n) {
                const double start = n * 0.06;
                for (size_t i = size_t(start * rate); i < out.size(); ++i) {
                    const double t = i * dt - start, env = std::min(1.0, t / 0.004) * std::exp(-t / 0.12);
                    out[i] += float(env * (0.2 * std::sin(TAU * notes[n] * t) + 0.05 * std::sin(TAU * 2 * notes[n] * t)));
                }
            }
            break;
        }
        case Sound::LinkLost: {  // descending tone, A5 to D4, with a little third harmonic
            out = make(0.45);
            double phase = 0;
            for (size_t i = 0; i < out.size(); ++i) {
                const double t = i * dt, f = 880 * std::pow(294.0 / 880.0, t / 0.45);
                phase += TAU * f * dt;
                const double env = std::min(1.0, t / 0.005) * (1 - t / 0.45);
                out[i] = float(env * (0.36 * std::sin(phase) + 0.08 * std::sin(3 * phase)));
            }
            break;
        }
        case Sound::Count: break;
    }
    const size_t ramp = std::min(out.size(), size_t(rate / 500));  // 2 ms to zero: no click at the end
    for (size_t i = 0; i < ramp; ++i) out[out.size() - 1 - i] *= float(i) / ramp;
    return out;
}

// ------------------------------------------------------------------------------------------- mixer

Mixer::Mixer(int rate) : rate_(rate) {
    for (size_t i = 0; i < pcm_.size(); ++i) pcm_[i] = synthesize(Sound(i), rate);
    set_gains(AudioGains{});
}

bool Mixer::play(const Cue& c) {
    if (c.sound >= Sound::Count) return false;
    const size_t head = head_.load(std::memory_order_relaxed);
    if (head - tail_.load(std::memory_order_acquire) >= QUEUE) return false;
    queue_[head % QUEUE] = c;
    head_.store(head + 1, std::memory_order_release);
    return true;
}

void Mixer::set_gains(const AudioGains& g) {
    volume_.store(g.volume);
    for (size_t i = 0; i < category_.size(); ++i) category_[i].store(g.category[i]);
}

void Mixer::mix(int16_t* out, int frames, int channels) {
    // new cues: a free voice, else the one nearest its end is replaced
    for (size_t tail = tail_.load(std::memory_order_relaxed); tail != head_.load(std::memory_order_acquire);) {
        const Cue c = queue_[tail % QUEUE];
        tail_.store(++tail, std::memory_order_release);
        Voice* v = &voices_[0];
        for (Voice& candidate : voices_) {
            if (!candidate.pcm) { v = &candidate; break; }
            if (v->pcm && candidate.pos * v->pcm->size() > v->pos * candidate.pcm->size()) v = &candidate;
        }
        *v = Voice{&pcm_[size_t(c.sound)], 0, c.gain, sound_category(c.sound)};
    }
    const float volume = volume_.load(), hum_target = hum_target_.load();
    const bool muted = muted_.load();
    std::array<float, size_t(Category::Count)> cat;
    for (size_t i = 0; i < cat.size(); ++i) cat[i] = category_[i].load();
    const float ambient = cat[size_t(Category::Ambient)];
    const float smooth = 1.0f / (0.05f * rate_);  // the hum fades in or out over ~50 ms
    const double step = TAU / rate_;
    int active = 0;
    for (int f = 0; f < frames; ++f) {
        float x = 0;
        for (Voice& v : voices_) {
            if (!v.pcm) continue;
            x += (*v.pcm)[v.pos] * v.gain * cat[size_t(v.category)];
            if (++v.pos >= v.pcm->size()) v.pcm = nullptr;
        }
        hum_level_ += (hum_target - hum_level_) * smooth;
        if (hum_level_ > 1e-5f) {  // low cockpit hum: 60 Hz and harmonics, audible on small speakers through 240 Hz
            const double p = hum_phase_;
            x += float(hum_level_ * ambient *
                       (0.05 * std::sin(60 * p) + 0.03 * std::sin(120 * p) + 0.015 * std::sin(180 * p) + 0.012 * std::sin(240 * p)));
        }
        hum_phase_ += step;
        if (hum_phase_ > TAU) hum_phase_ -= TAU;  // TAU / 60 Hz periods all fit a whole number of times in one second
        x = muted ? 0.0f : std::clamp(x * volume, -1.0f, 1.0f);
        const int16_t sample = static_cast<int16_t>(std::lround(x * 32767.0f));
        for (int ch = 0; ch < channels; ++ch) out[f * channels + ch] = sample;
    }
    for (const Voice& v : voices_) active += v.pcm != nullptr;
    active_.store(active);
}

// ------------------------------------------------------------------------------------------- WAV and dumps

std::vector<uint8_t> wav_header(int rate, int channels, uint32_t data_bytes) {
    std::vector<uint8_t> h;
    auto text = [&](const char* s) { h.insert(h.end(), s, s + 4); };
    auto u32 = [&](uint32_t v) { for (int i = 0; i < 4; ++i) h.push_back(uint8_t(v >> (8 * i))); };
    auto u16 = [&](uint16_t v) { h.push_back(uint8_t(v)), h.push_back(uint8_t(v >> 8)); };
    text("RIFF");
    u32(36 + data_bytes);
    text("WAVE");
    text("fmt ");
    u32(16);
    u16(1);  // PCM
    u16(uint16_t(channels));
    u32(uint32_t(rate));
    u32(uint32_t(rate * channels * 2));
    u16(uint16_t(channels * 2));
    u16(16);
    text("data");
    u32(data_bytes);
    return h;
}

bool write_wav(const std::string& path, int rate, int channels, const std::vector<int16_t>& samples) {
    WavWriter w;
    if (!w.open(path, rate, channels)) return false;
    const bool ok = samples.empty() || w.append(samples.data(), samples.size());
    w.close();
    return ok;
}

bool WavWriter::open(const std::string& path, int rate, int channels) {
    close();
    f_ = std::fopen(path.c_str(), "wb");
    if (!f_) return false;
    rate_ = rate, channels_ = channels, bytes_ = 0;
    const auto h = wav_header(rate, channels, 0);
    if (std::fwrite(h.data(), 1, h.size(), f_) != h.size()) return close(), false;
    return true;
}

bool WavWriter::append(const int16_t* samples, size_t count) {
    if (!f_) return false;
    if (std::fwrite(samples, sizeof(int16_t), count, f_) != count) return false;  // little-endian hosts only
    bytes_ += uint32_t(count * sizeof(int16_t));
    const auto h = wav_header(rate_, channels_, bytes_);
    if (std::fseek(f_, 0, SEEK_SET) != 0 || std::fwrite(h.data(), 1, h.size(), f_) != h.size() ||
        std::fseek(f_, 0, SEEK_END) != 0)
        return false;
    return std::fflush(f_) == 0;
}

void WavWriter::close() {
    if (f_) std::fclose(f_);
    f_ = nullptr;
}

SampleRing::SampleRing(size_t capacity) : buf_(std::max<size_t>(capacity, 1)) {}

void SampleRing::write(const int16_t* samples, size_t count) {
    std::lock_guard<std::mutex> lock(mutex_);
    const size_t cap = buf_.size();
    if (count > cap) samples += count - cap, total_ += count - cap, count = cap;
    size_t at = size_t(total_ % cap);
    const size_t first = std::min(count, cap - at);
    std::copy(samples, samples + first, buf_.begin() + at);
    std::copy(samples + first, samples + count, buf_.begin());
    total_ += count;
}

std::vector<int16_t> SampleRing::snapshot() const {
    uint64_t from = 0;
    return since(from);
}

std::vector<int16_t> SampleRing::since(uint64_t& from, uint64_t* lost) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const uint64_t cap = buf_.size();
    uint64_t skipped = 0;
    if (from > total_) from = total_;
    if (total_ - from > cap) skipped = total_ - from - cap, from = total_ - cap;
    if (lost) *lost = skipped;
    std::vector<int16_t> out(size_t(total_ - from));
    const size_t at = size_t(from % cap), first = std::min(out.size(), size_t(cap) - at);
    std::copy(buf_.begin() + at, buf_.begin() + at + first, out.begin());  // two memcpy-like copies under the lock
    std::copy(buf_.begin(), buf_.begin() + (out.size() - first), out.begin() + first);
    from = total_;
    return out;
}

uint64_t SampleRing::total() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return total_;
}

}  // namespace ksphmi
