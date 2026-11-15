// Boot loading screen state: what `ksphmi --splash` shows while the Pi boots, as a pure function of a few probes.
//
// The splash (system unit ksphmi-splash.service) probes the system at about 2 Hz: seconds since power-on
// (CLOCK_BOOTTIME), multi-user.target active, the network interface and its address, a ksphmi process running and
// the ready file the HMI writes after its first presented frame. boot_view() turns them into the five rows of the
// BOOT SEQUENCE card, the target progress and the status message; drawing (splash.cpp) only reads the result.
#pragma once

#include <array>
#include <string>

namespace ksphmi {

/// What the splash knows about the boot, gathered by the probes (or made up for screenshots and tests).
struct BootInputs {
    double seconds = 0;       ///< since power-on (CLOCK_BOOTTIME)
    bool system_up = false;   ///< multi-user.target is active
    std::string iface;        ///< network interface ("wlan0"); empty when none is up yet
    std::string address;      ///< its IPv4 address; empty while waiting for DHCP
    bool hmi_running = false; ///< a ksphmi process (not a splash) exists
    bool ready = false;       ///< the HMI wrote its ready file (first frame presented)
    int width = 0, height = 0;
    std::string device = "/dev/fb0";
    std::string hostname;
};

enum class Step { Pending, Active, Done, Warn, Failed };

struct BootRow {
    std::string label, detail;
    Step step = Step::Pending;
};

struct BootView {
    std::array<BootRow, 5> rows;  ///< KERNEL, DISPLAY, SYSTEM, NETWORK, HMI
    double progress = 0;          ///< target fraction 0..1 (the screen eases toward it)
    bool failed = false;          ///< the HMI never became ready: percentage reads "HMI FAILED"
    bool done = false;            ///< the HMI is ready ("Orbit reached")
    std::string message;          ///< under the percentage; the ssh hint when failed
    std::string clock;            ///< "T+ 00:12.4"
    std::string footer;           ///< "<hostname>  ·  hmi-cpp  ·  SSH available while booting"
};

/// Seconds without an address before the NETWORK row reads "offline" (the boot carries on without it).
constexpr double NETWORK_GIVE_UP = 40;
/// Seconds after power-on when an HMI that has not drawn its first frame counts as failed.
constexpr double HMI_GIVE_UP = 45;

BootView boot_view(const BootInputs& in);

/// "T+ mm:ss.s" (minutes keep growing past 99).
std::string boot_clock(double seconds);

/// Inputs of a named boot stage for screenshots and tests: system, network, offline, hmi, ready, failed.
/// Throws std::invalid_argument for an unknown name.
BootInputs boot_demo_inputs(const std::string& stage, int width, int height);

// ---- probes (Linux; the splash calls them from a worker thread, never per frame)

/// Seconds since power-on, suspend included (CLOCK_BOOTTIME).
double boot_seconds();
/// $XDG_RUNTIME_DIR/ksphmi.ready, or /run/user/<uid>/ksphmi.ready when the variable is unset (system units).
std::string ready_file_path();
/// The HMI side of the hand-over: remove a stale ready file at start, write it after the first presented frame.
void remove_ready_file();
bool write_ready_file();
bool file_exists(const std::string& path);
/// `systemctl is-active --quiet UNIT`, capped at 2 s; false when systemctl cannot answer yet (early boot).
bool unit_active(const std::string& unit);
/// A process named ksphmi other than this one whose command line has no --splash.
bool hmi_process_running();
/// This machine's host name ("" when unknown).
std::string host_name();

}  // namespace ksphmi
