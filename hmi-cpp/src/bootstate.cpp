#include "bootstate.hpp"

#if defined(_WIN32)
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#endif

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <stdexcept>

namespace ksphmi {

namespace {
const char* DOT = "  \xC2\xB7  ";  // "  ·  "
const char* MID = " \xC2\xB7 ";    // " · "

bool finished(Step s) { return s == Step::Done || s == Step::Warn || s == Step::Failed; }
}  // namespace

std::string boot_clock(double seconds) {
    if (!(seconds >= 0)) seconds = 0;
    long tenths = static_cast<long>(std::floor(seconds * 10));
    long minutes = tenths / 600, rest = tenths % 600;
    char buf[48];
    std::snprintf(buf, sizeof buf, "T+ %02ld:%02ld.%ld", minutes, rest / 10, rest % 10);
    return buf;
}

BootView boot_view(const BootInputs& in) {
    BootView v;
    v.rows[0] = {"KERNEL", std::string("Linux") + MID + "quiet console", Step::Done};
    v.rows[1] = {"DISPLAY", "HDMI " + std::to_string(in.width) + "x" + std::to_string(in.height) + MID + in.device,
                 Step::Done};
    BootRow& system = v.rows[2];
    system.label = "SYSTEM";
    if (in.system_up) system.detail = "Filesystems and services up", system.step = Step::Done;
    else system.detail = "Mounting filesystems";
    BootRow& network = v.rows[3];
    network.label = "NETWORK";
    const std::string iface = in.iface.empty() ? "network" : in.iface;
    if (!in.address.empty()) network.detail = iface + " " + in.address, network.step = Step::Done;
    else if (in.seconds >= NETWORK_GIVE_UP) network.detail = iface + MID + "offline", network.step = Step::Warn;
    else network.detail = iface + MID + "waiting for address";
    BootRow& hmi = v.rows[4];
    hmi.label = "HMI";
    if (in.ready) hmi.detail = "First frame drawn", hmi.step = Step::Done;
    else if (in.seconds >= HMI_GIVE_UP) hmi.detail = "ksphmi did not start", hmi.step = Step::Failed;
    else if (in.hmi_running) hmi.detail = "Starting ksphmi";
    else hmi.detail = std::string("ksphmi.service") + MID + "waiting";
    // The first unfinished row is the one being worked on; the later unfinished ones wait their turn.
    bool seen = false;
    for (auto& r : v.rows)
        if (!finished(r.step)) r.step = seen ? Step::Pending : Step::Active, seen = true;

    v.done = in.ready;
    v.failed = !in.ready && hmi.step == Step::Failed;
    if (v.done) v.progress = 1.0, v.message = "Orbit reached";
    else if (!finished(system.step)) v.progress = 0.35, v.message = "Starting system services";
    else if (!finished(network.step)) v.progress = 0.62, v.message = "Connecting to the network";
    else v.progress = 0.86, v.message = "Launching the flight display";
    const std::string host = in.hostname.empty() ? "raspberrypi" : in.hostname;
    if (v.failed) v.message = "ssh " + host + "  \xE2\x86\x92  journalctl --user -u ksphmi";  // →
    v.clock = boot_clock(in.seconds);
    v.footer = host + DOT + "hmi-cpp" + DOT + "SSH available while booting";
    return v;
}

BootInputs boot_demo_inputs(const std::string& stage, int width, int height) {
    BootInputs in;
    in.width = width, in.height = height, in.hostname = "ksphmi", in.iface = "wlan0";
    if (stage == "system") {
        in.seconds = 7.8;
    } else if (stage == "network") {
        in.seconds = 13.6, in.system_up = true;
    } else if (stage == "offline") {
        in.seconds = 41.2, in.system_up = true, in.hmi_running = true;
    } else if (stage == "hmi") {
        in.seconds = 18.3, in.system_up = true, in.address = "192.168.1.20", in.hmi_running = true;
    } else if (stage == "ready") {
        in.seconds = 21.7, in.system_up = true, in.address = "192.168.1.20", in.hmi_running = true, in.ready = true;
    } else if (stage == "failed") {
        in.seconds = 52.4, in.system_up = true, in.address = "192.168.1.20";
    } else {
        throw std::invalid_argument("unknown splash state '" + stage +
                                    "' (system, network, offline, hmi, ready, failed)");
    }
    return in;
}

#if defined(_WIN32)

// The boot splash is the Pi's (ksphmi-splash.service, /dev/fb0): on Windows the probes only keep the HMI's
// ready-file hand-over harmless (written to %TEMP%) and report a system without systemd or a splash to wait for.
double boot_seconds() { return static_cast<double>(GetTickCount64()) / 1000.0; }

std::string ready_file_path() {
    const char* dir = std::getenv("XDG_RUNTIME_DIR");
    if (dir && *dir) return std::string(dir) + "/ksphmi.ready";
    char temp[MAX_PATH + 1] = {0};
    DWORD n = GetTempPathA(sizeof temp, temp);
    std::string base = n > 0 && n < sizeof temp ? std::string(temp, n) : std::string(".\\");
    return base + "ksphmi.ready";
}

void remove_ready_file() { std::remove(ready_file_path().c_str()); }

bool write_ready_file() {
    std::ofstream f(ready_file_path(), std::ios::trunc);
    if (!f) return false;
    f << GetCurrentProcessId() << "\n";
    return static_cast<bool>(f);
}

bool file_exists(const std::string& path) { return GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

bool unit_active(const std::string&) { return false; }

bool hmi_process_running() { return false; }

std::string host_name() {
    char name[MAX_COMPUTERNAME_LENGTH + 1] = {0};
    DWORD size = sizeof name;
    return GetComputerNameA(name, &size) ? std::string(name, size) : std::string();
}

#else

double boot_seconds() {
    timespec t{};
#ifdef CLOCK_BOOTTIME
    clock_gettime(CLOCK_BOOTTIME, &t);
#else
    clock_gettime(CLOCK_MONOTONIC, &t);
#endif
    return t.tv_sec + t.tv_nsec * 1e-9;
}

std::string ready_file_path() {
    const char* dir = std::getenv("XDG_RUNTIME_DIR");
    std::string base = dir && *dir ? dir : "/run/user/" + std::to_string(getuid());
    return base + "/ksphmi.ready";
}

void remove_ready_file() { unlink(ready_file_path().c_str()); }

bool write_ready_file() {
    std::ofstream f(ready_file_path(), std::ios::trunc);
    if (!f) return false;
    f << getpid() << "\n";
    return static_cast<bool>(f);
}

bool file_exists(const std::string& path) {
    struct stat st {};
    return stat(path.c_str(), &st) == 0;
}

bool unit_active(const std::string& unit) {
    // Unit names come from this program only; quote them anyway. `timeout` bounds a systemctl stuck on the bus.
    std::string cmd = "timeout 2 systemctl is-active --quiet '" + unit + "' >/dev/null 2>&1";
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) return false;
    int status = pclose(p);
    return status != -1 && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

bool hmi_process_running() {
    DIR* proc = opendir("/proc");
    if (!proc) return false;
    const std::string self = std::to_string(getpid());
    bool found = false;
    while (dirent* e = readdir(proc)) {
        const std::string pid = e->d_name;
        if (pid.empty() || pid.find_first_not_of("0123456789") != std::string::npos || pid == self) continue;
        std::string comm;
        {
            std::ifstream f("/proc/" + pid + "/comm");
            if (!std::getline(f, comm) || comm != "ksphmi") continue;
        }
        std::ifstream f("/proc/" + pid + "/cmdline", std::ios::binary);
        std::string arg;
        bool splash = false;
        while (std::getline(f, arg, '\0'))
            if (arg == "--splash") splash = true;
        if (!splash) {
            found = true;
            break;
        }
    }
    closedir(proc);
    return found;
}

std::string host_name() {
    char name[256] = {0};
    if (gethostname(name, sizeof name - 1) != 0) return "";
    return name;
}

#endif

}  // namespace ksphmi
