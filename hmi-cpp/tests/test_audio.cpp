// Cockpit audio: the AudioDirector on a fake clock, the mixer, WAV dumps, [audio] settings and the SDL device
// (SDL_AUDIODRIVER=dummy: no sound card needed, and a missing device must stay silent).
#include <SDL.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <thread>

#include "audio.hpp"
#include "audioout.hpp"
#include "check.hpp"
#include "config.hpp"
#include "log.hpp"

using namespace ksphmi;

namespace {

constexpr double TURN = 2 * 3.14159265358979323846;  // one sweep turn, radians
constexpr double FRAME = 1.0 / 30;

AudioFrame frame(State state, double now, const std::string& skin = "ksp1", bool waiting = true) {
    return {state, skin, waiting && state != State::Flight, now};
}

int count(const std::vector<Cue>& cues, Sound s) {
    int n = 0;
    for (const Cue& c : cues) n += c.sound == s;
    return n;
}

std::vector<uint8_t> read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

uint32_t le32(const std::vector<uint8_t>& b, size_t at) {
    return uint32_t(b[at]) | uint32_t(b[at + 1]) << 8 | uint32_t(b[at + 2]) << 16 | uint32_t(b[at + 3]) << 24;
}

}  // namespace

// ------------------------------------------------------------------------------------------- shared clock

TEST(sweep_angle_is_the_painters_rate_per_skin) {
    CHECK_NEAR(waiting_sweep_angle("ksp1", 10), 10, 1e-12);
    CHECK_NEAR(waiting_sweep_angle("ksp2", 10), 10, 1e-12);
    for (const char* skin : {"classic", "apollo", "steampunk"}) CHECK_NEAR(waiting_sweep_angle(skin, 10), 12, 1e-12);
    CHECK(waiting_lamp_lit(100.0) && waiting_lamp_lit(100.59) && !waiting_lamp_lit(100.61) && !waiting_lamp_lit(100.99));
    Snapshot s;
    CHECK(shows_waiting_screen(s));
    s.state = State::Flight;
    CHECK(!shows_waiting_screen(s));
}

// ------------------------------------------------------------------------------------------- director

TEST(director_is_silent_at_startup) {
    for (State st : {State::Connecting, State::NoVessel, State::Flight}) {
        AudioDirector d;
        // the first frame lands exactly on a sweep turn and a whole second: still nothing
        CHECK(d.update(frame(st, TURN * 50)).cues.empty());
    }
    AudioDirector d;
    CHECK(d.update(frame(State::Flight, 10)).cues.empty());
    CHECK(d.update(frame(State::Flight, 10 + FRAME)).cues.empty());
}

TEST(radar_ping_lands_within_one_frame_of_the_sweep_crossing) {
    for (const char* skin : {"ksp1", "apollo"}) {
        const double rate = waiting_sweep_angle(skin, 1);
        AudioDirector d;
        std::vector<double> pings;
        const double start = 1000.3;
        for (double t = start; t < start + 40; t += FRAME)
            for (const Cue& c : d.update(frame(State::Connecting, t, skin)).cues)
                if (c.sound == Sound::RadarPing) {
                    pings.push_back(t);
                    CHECK_EQ(c.gain, 1.0f);
                }
        // every crossing of a whole turn after the first frame, once, at most one frame late
        long long first = (long long)std::floor(start * rate / (TURN)) + 1;
        size_t expected = 0;
        for (long long k = first; TURN * k / rate < start + 40 - FRAME; ++k, ++expected) {
            const double crossing = TURN * k / rate;
            CHECK(expected < pings.size());
            if (expected >= pings.size()) break;
            CHECK(pings[expected] >= crossing - 1e-9);
            CHECK(pings[expected] - crossing < FRAME + 1e-9);
        }
        CHECK_EQ(pings.size(), expected);
    }
}

TEST(lamp_click_follows_the_blink_only_while_connecting) {
    AudioDirector d;
    int clicks = 0;
    for (double t = 50.5; t < 60.5; t += FRAME) {
        const auto cues = d.update(frame(State::Connecting, t, "apollo")).cues;
        if (count(cues, Sound::LampClick)) {
            ++clicks;
            CHECK(waiting_lamp_lit(t));     // the lamp just lit
            CHECK(std::fmod(t, 1.0) < FRAME + 1e-9);
        }
    }
    CHECK_EQ(clicks, 10);
    AudioDirector nv;
    int soft = 0;
    for (double t = 50.5; t < 70.5; t += FRAME) {
        const auto cues = nv.update(frame(State::NoVessel, t, "apollo")).cues;
        CHECK_EQ(count(cues, Sound::LampClick), 0);
        for (const Cue& c : cues)
            if (c.sound == Sound::RadarPing) {
                ++soft;
                CHECK_EQ(c.gain, AudioDirector::SOFT_PING);
            }
    }
    CHECK(soft >= 2);
    CHECK(AudioDirector::SOFT_PING < 1.0f);
}

TEST(link_cues_fire_once_per_flight_edge) {
    AudioDirector d;
    double t = 10;
    auto run = [&](State st, int frames) {
        std::vector<Cue> all;
        for (int i = 0; i < frames; ++i, t += FRAME) {
            auto cues = d.update(frame(st, t)).cues;
            all.insert(all.end(), cues.begin(), cues.end());
        }
        return all;
    };
    auto connecting = run(State::Connecting, 30);
    CHECK_EQ(count(connecting, Sound::LinkAcquired) + count(connecting, Sound::LinkLost), 0);
    auto novessel = run(State::NoVessel, 30);  // Connecting -> NoVessel is no flight edge
    CHECK_EQ(count(novessel, Sound::LinkAcquired) + count(novessel, Sound::LinkLost), 0);
    auto flight = run(State::Flight, 60);
    CHECK_EQ(count(flight, Sound::LinkAcquired), 1);
    CHECK_EQ(count(flight, Sound::LinkLost), 0);
    CHECK_EQ(count(flight, Sound::RadarPing), 0);
    auto lost = run(State::Connecting, 60);
    CHECK_EQ(count(lost, Sound::LinkLost), 1);
    CHECK_EQ(count(lost, Sound::LinkAcquired), 0);
    auto again = run(State::Flight, 5);
    CHECK_EQ(count(again, Sound::LinkAcquired), 1);
    auto to_nv = run(State::NoVessel, 5);
    CHECK_EQ(count(to_nv, Sound::LinkLost), 1);
}

TEST(identical_cues_are_rate_limited) {
    AudioDirector d;
    double t = 5;
    d.update(frame(State::NoVessel, t));
    int acquired = 0, lost = 0;
    double last_acquired = -1;
    for (int i = 0; i < 40; ++i) {  // a flapping link: a state change every 50 ms
        t += 0.05;
        for (const Cue& c : d.update(frame(i % 2 ? State::NoVessel : State::Flight, t)).cues) {
            if (c.sound == Sound::LinkAcquired) {
                if (last_acquired >= 0) CHECK(t - last_acquired >= AudioDirector::RATE_LIMIT - 1e-9);
                last_acquired = t, ++acquired;
            }
            lost += c.sound == Sound::LinkLost;
        }
    }
    CHECK(acquired >= 5 && acquired <= 10);  // 20 edges in 2 s, at most one per 150 ms
    CHECK(lost >= 5 && lost <= 10);
}

TEST(skin_switch_and_hidden_radar_never_ping) {
    AudioDirector d;
    double t = 300;
    d.update(frame(State::Connecting, t, "ksp1"));
    // ksp1 -> apollo at t = 300: the turn count jumps from 47 to 57; no burst, no ping
    t += FRAME;
    CHECK_EQ(count(d.update(frame(State::Connecting, t, "apollo")).cues, Sound::RadarPing), 0);
    AudioDirector f;  // a facility screen (no radar) over many turns: silent
    for (double u = 0; u < 30; u += FRAME) CHECK(f.update(frame(State::NoVessel, u, "ksp1", false)).cues.empty());
    AudioDirector h;
    h.ambient = false;
    CHECK_EQ(h.update(frame(State::Connecting, 1)).hum, 0.0f);
    CHECK_EQ(AudioDirector().update(frame(State::Connecting, 1)).hum, 1.0f);  // ambient on by default
}

// ------------------------------------------------------------------------------------------- mixer

TEST(synthesized_sounds_are_short_bounded_and_end_at_zero) {
    for (int i = 0; i < int(Sound::Count); ++i) {
        const auto pcm = synthesize(Sound(i));
        CHECK(!pcm.empty());
        CHECK(pcm.size() <= size_t(AUDIO_RATE));  // under a second
        float peak = 0;
        for (float x : pcm) peak = std::max(peak, std::fabs(x));
        CHECK(peak > 0.05f && peak < 0.9f);
        CHECK_NEAR(pcm.back(), 0, 1e-6);
        CHECK(synthesize(Sound(i)) == pcm);  // deterministic
    }
}

TEST(mixer_plays_cues_mutes_and_limits_voices) {
    Mixer m;
    std::vector<int16_t> buf(512 * 2);
    m.mix(buf.data(), 512, 2);
    CHECK(std::all_of(buf.begin(), buf.end(), [](int16_t s) { return s == 0; }));  // no cue, no hum
    CHECK(m.play({Sound::RadarPing, 1}));
    m.mix(buf.data(), 512, 2);
    int peak = 0;
    for (size_t i = 0; i < buf.size(); i += 2) {
        peak = std::max(peak, std::abs(int(buf[i])));
        CHECK_EQ(buf[i], buf[i + 1]);
    }
    CHECK(peak > 1000);
    CHECK_EQ(m.active_voices(), 1);
    m.set_muted(true);
    m.mix(buf.data(), 512, 2);
    CHECK(std::all_of(buf.begin(), buf.end(), [](int16_t s) { return s == 0; }));
    m.set_muted(false);
    for (int i = 0; i < 12; ++i) CHECK(m.play({Sound::LinkAcquired, 1}));
    m.mix(buf.data(), 64, 2);
    CHECK_EQ(m.active_voices(), Mixer::VOICES);
    // the queue holds QUEUE cues between two callbacks; more are dropped, never blocked on
    int queued = 0;
    for (size_t i = 0; i < Mixer::QUEUE + 10; ++i) queued += m.play({Sound::LampClick, 1});
    CHECK_EQ(queued, int(Mixer::QUEUE));
    // a zero category gain silences its sounds; the hum alone is quiet but present
    Mixer q;
    AudioGains g;
    g.category[size_t(Category::Feedback)] = 0;
    q.set_gains(g);
    q.play({Sound::RadarPing, 1});
    q.mix(buf.data(), 512, 2);
    CHECK(std::all_of(buf.begin(), buf.end(), [](int16_t s) { return s == 0; }));
    q.set_hum(1);
    std::vector<int16_t> hum(AUDIO_RATE / 2);
    q.mix(hum.data(), int(hum.size()), 1);
    int hum_peak = 0;
    for (size_t i = hum.size() / 2; i < hum.size(); ++i) hum_peak = std::max(hum_peak, std::abs(int(hum[i])));
    CHECK(hum_peak > 300 && hum_peak < 6000);
}

// ------------------------------------------------------------------------------------------- WAV dumps

TEST(wav_files_have_valid_headers_and_grow) {
    const auto h = wav_header(48000, 2, 400);
    CHECK_EQ(h.size(), size_t(44));
    CHECK(std::string(h.begin(), h.begin() + 4) == "RIFF" && std::string(h.begin() + 8, h.begin() + 16) == "WAVEfmt ");
    CHECK_EQ(le32(h, 4), 436u);
    CHECK_EQ(le32(h, 24), 48000u);
    CHECK_EQ(le32(h, 28), 192000u);
    CHECK_EQ(le32(h, 40), 400u);
    const std::string dir = check::make_temp_dir("ksphmi-audio");
    CHECK(!dir.empty());
    const std::string path = dir + "/a.wav";
    CHECK(write_wav(path, 48000, 2, {1, -1, 2, -2}));
    auto b = read_file(path);
    CHECK_EQ(b.size(), size_t(52));
    CHECK_EQ(le32(b, 40), 8u);
    CHECK_EQ(int16_t(b[46] | b[47] << 8), int16_t(-1));
    const std::string grow = dir + "/b.wav";
    {
        WavWriter w;
        CHECK(w.open(grow, 48000, 1));
        const int16_t s[3] = {7, 8, 9};
        CHECK(w.append(s, 3));
        CHECK_EQ(le32(read_file(grow), 40), 6u);  // valid before close
        CHECK(w.append(s, 3));
    }
    b = read_file(grow);
    CHECK_EQ(b.size(), size_t(56));
    CHECK_EQ(le32(b, 4), 48u);
    CHECK(!write_wav(dir + "/missing/c.wav", 48000, 1, {1}));
    std::remove(path.c_str());
    std::remove(grow.c_str());
    check::remove_dir(dir);
}

TEST(sample_ring_keeps_the_newest_samples) {
    SampleRing r(5);
    const int16_t a[3] = {1, 2, 3}, b[4] = {4, 5, 6, 7};
    r.write(a, 3);
    CHECK((r.snapshot() == std::vector<int16_t>{1, 2, 3}));
    uint64_t from = 0, lost = 0;
    CHECK((r.since(from) == std::vector<int16_t>{1, 2, 3}));
    r.write(b, 4);
    CHECK((r.snapshot() == std::vector<int16_t>{3, 4, 5, 6, 7}));
    CHECK((r.since(from, &lost) == std::vector<int16_t>{4, 5, 6, 7}));
    CHECK_EQ(lost, 0u);
    r.write(a, 3), r.write(b, 4);  // 7 new samples, 5 kept
    CHECK((r.since(from, &lost) == std::vector<int16_t>{3, 4, 5, 6, 7}));
    CHECK_EQ(lost, 2u);
    CHECK_EQ(r.total(), 14u);
    const int16_t big[7] = {1, 2, 3, 4, 5, 6, 7};
    r.write(big, 7);
    CHECK((r.snapshot() == std::vector<int16_t>{3, 4, 5, 6, 7}));
}

// ------------------------------------------------------------------------------------------- settings

TEST(audio_settings_parse_and_validate) {
    Config d;
    CHECK(d.audio.enabled);
    CHECK_NEAR(d.audio.volume, 0.6, 1e-12);
    CHECK_NEAR(d.audio.alerts, 1.0, 1e-12);
    CHECK_NEAR(d.audio.feedback, 0.8, 1e-12);
    CHECK_NEAR(d.audio.events, 0.7, 1e-12);
    CHECK_NEAR(d.audio.ambient, 0.5, 1e-12);
    CHECK(d.audio.device.empty() && d.audio.dump_dir.empty());
    // the shipped config.toml spells out the same defaults
    std::ifstream f(std::string(KSPHMI_SOURCE_DIR) + "/config.toml");
    std::stringstream text;
    text << f.rdbuf();
    Config shipped;
    apply_toml(shipped, parse_toml(text.str()));
    CHECK(shipped.audio.enabled);
    CHECK_EQ(shipped.audio.volume, d.audio.volume);
    CHECK_EQ(shipped.audio.ambient, d.audio.ambient);
    CHECK(shipped.audio.device.empty() && shipped.audio.dump_dir.empty());
    Config c;
    apply_toml(c, parse_toml("[audio]\nenabled = false\nvolume = 0\nevents = 1\ndevice = \"USB Audio\"\n"
                             "dump_dir = '/tmp/x'\n"));
    CHECK(!c.audio.enabled);
    CHECK_EQ(c.audio.volume, 0.0);
    CHECK_EQ(c.audio.events, 1.0);
    CHECK_EQ(c.audio.device, "USB Audio");
    CHECK_EQ(c.audio.dump_dir, "/tmp/x");
    for (const char* key : {"volume", "alerts", "feedback", "events", "ambient"})
        for (const char* value : {"1.01", "-0.1", "nan", "inf", "-inf", "\"loud\""}) {
            bool threw = false;
            Config bad;
            try {
                apply_toml(bad, parse_toml(std::string("[audio]\n") + key + " = " + value + "\n"));
            } catch (const std::runtime_error&) {
                threw = true;
            }
            if (!threw) std::fprintf(stderr, "accepted audio.%s = %s\n", key, value);
            CHECK(threw);
        }
    bool threw = false;
    try {
        Config bad;
        apply_toml(bad, parse_toml("[audio]\nenabled = 1\n"));
    } catch (const std::runtime_error&) {
        threw = true;
    }
    CHECK(threw);
}

// ------------------------------------------------------------------------------------------- SDL device

TEST(audio_out_disabled_or_without_device_stays_silent) {
    AudioConfig off;
    off.enabled = false;
    {
        AudioOut a(off);
        CHECK(!a.active());
        a.play({{{Sound::RadarPing, 1}}, 1});  // harmless
        CHECK(!a.dump("/nonexistent/never.wav"));
    }
    check::set_env("SDL_AUDIODRIVER", "no-such-driver");
    {
        AudioOut a(AudioConfig{});  // no device: one log line, no exception
        CHECK(!a.active());
        CHECK(a.device().empty());
    }
    check::unset_env("SDL_AUDIODRIVER");
}

TEST(audio_out_dummy_device_mixes_and_dumps) {
    check::set_env("SDL_AUDIODRIVER", "dummy");
    const std::string dir = check::make_temp_dir("ksphmi-audio-out");
    CHECK(!dir.empty());
    AudioConfig cfg;
    cfg.dump_dir = dir;
    std::string session;
    {
        AudioOut a(cfg);
        if (!a.active()) {
            std::printf("     (SDL dummy audio driver unavailable: device checks skipped)\n");
        } else {
            CHECK(!a.device().empty());
            a.play({{{Sound::LinkAcquired, 1}}, 1});
            CHECK(a.toggle_mute() && a.muted());
            CHECK(!a.toggle_mute());
            // the dummy driver runs the callback at the device rate on its own thread
            for (int i = 0; i < 100 && a.samples_played() < 4096; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
            const std::string path = dir + "/dump.wav";
            CHECK(a.dump(path));
            const auto b = read_file(path);
            CHECK(b.size() >= 44);
            if (b.size() >= 44) CHECK_EQ(le32(b, 40), uint32_t(b.size() - 44));
            if (a.samples_played() == 0) std::printf("     (dummy driver never called back: dump is empty)\n");
            std::remove(path.c_str());
            session = a.session_path();
            CHECK(!session.empty());
        }
    }
    if (!session.empty()) {
        const auto b = read_file(session);
        CHECK(b.size() >= 44);
        if (b.size() >= 44) CHECK_EQ(le32(b, 40), uint32_t(b.size() - 44));
        std::remove(session.c_str());
    }
    check::remove_dir(dir);
    check::unset_env("SDL_AUDIODRIVER");
}

int main(int argc, char** argv) {
    log::set_quiet(true);
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    if (SDL_Init(0) != 0) return 2;
    const int rc = RUN_TESTS(argc, argv);
    SDL_Quit();
    return rc;
}
