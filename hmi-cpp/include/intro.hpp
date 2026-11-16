// Boot intro hand-over: the Pi plays assets/intro/KSPKontroller_pi_Intro.mp4 with mpv on DRM at power-on
// (ksphmi-splash.service runs systemd/ksphmi-intro.sh) and keeps its last frame on the screen. mpv is DRM master
// while it shows that frame, so SDL's kmsdrm cannot open the display until mpv quits: before SDL video starts,
// the HMI waits for the end of the video over mpv's JSON IPC socket and asks mpv to quit.
//
// Linux only (the Pi); on other systems finish_boot_intro() returns NotRunning at once. Every failure lets the HMI
// proceed: at worst the video is cut short or the HMI waits for the bounded timeouts below.
#pragma once

#include <string>

namespace ksphmi {

/// systemd RuntimeDirectory=ksphmi-intro of ksphmi-splash.service: present while the intro unit runs.
constexpr const char* INTRO_DIR = "/run/ksphmi-intro";
/// mpv --input-ipc-server inside INTRO_DIR (ksphmi-intro.sh).
constexpr const char* INTRO_SOCKET = "mpv.sock";
/// Written by ksphmi-intro.sh while it waits for the monitor and mpv's socket, removed once mpv runs or gives up.
constexpr const char* INTRO_STARTING = "starting";

struct IntroTimeouts {
    double start = 40;  ///< for mpv's socket while INTRO_STARTING exists (the script waits 20 s for the monitor)
    double eof = 15;    ///< from connecting until mpv reports eof-reached (the video is ~10 s)
    double quit = 2;    ///< after "quit" until mpv's socket and the intro directory are gone
    double poll = 0.2;  ///< between eof-reached queries and file checks
};

enum class IntroResult {
    NotRunning,  ///< no intro directory or socket: nothing to wait for
    Finished,    ///< the video ended, mpv quit: the display is free
    Cut,         ///< eof timeout: mpv was asked to quit before the end
    Failed,      ///< socket error, mpv did not quit in time, or no socket appeared; `why` says which
};

/// Waits for the boot intro in `dir` (INTRO_DIR by default) to end, then makes mpv quit. Logs one INFO line
/// ("boot intro finished, taking the display") or a WARNING with the reason; NotRunning logs nothing.
IntroResult finish_boot_intro(const std::string& dir = INTRO_DIR, const IntroTimeouts& t = {},
                              std::string* why = nullptr);

}  // namespace ksphmi
