// Boot intro hand-over over mpv's JSON IPC (intro.hpp).
#include "intro.hpp"

#include "fmt.hpp"
#include "log.hpp"

#if defined(__linux__)
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <thread>
#endif

namespace ksphmi {

namespace {
const char* LOG = "ksphmi.intro";
}

#if !defined(__linux__)

IntroResult finish_boot_intro(const std::string&, const IntroTimeouts&, std::string* why) {
    if (why) why->clear();
    return IntroResult::NotRunning;
}

#else

namespace {

using Clock = std::chrono::steady_clock;

double since(Clock::time_point t0) { return std::chrono::duration<double>(Clock::now() - t0).count(); }

void nap(double seconds) {
    if (seconds > 0) std::this_thread::sleep_for(std::chrono::duration<double>(seconds));
}

bool exists(const std::string& path) {
    struct stat st{};
    return ::stat(path.c_str(), &st) == 0;
}

/// One AF_UNIX connection to mpv: newline-delimited JSON requests and replies (and mpv's own event lines).
class Ipc {
public:
    ~Ipc() {
        if (fd_ >= 0) ::close(fd_);
    }
    bool connect(const std::string& path) {
        sockaddr_un addr{};
        if (path.size() >= sizeof addr.sun_path) return false;
        addr.sun_family = AF_UNIX;
        std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);
        fd_ = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd_ < 0) return false;
        if (::connect(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof addr) == 0) return true;
        ::close(fd_);
        fd_ = -1;
        return false;
    }
    bool send(const std::string& line) {
        std::string data = line + "\n";
        size_t done = 0;
        while (done < data.size()) {
            ssize_t n = ::send(fd_, data.data() + done, data.size() - done, MSG_NOSIGNAL);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) return false;
            done += static_cast<size_t>(n);
        }
        return true;
    }
    /// Reads what arrives within `seconds`; false once mpv closed the connection (or it failed).
    bool read(double seconds) {
        pollfd p{fd_, POLLIN, 0};
        int r = ::poll(&p, 1, std::max(0, static_cast<int>(seconds * 1000)));
        if (r <= 0) return r == 0 || errno == EINTR;
        char buf[4096];
        ssize_t n = ::recv(fd_, buf, sizeof buf, 0);
        if (n < 0 && errno == EINTR) return true;
        if (n <= 0) return false;
        pending_.append(buf, static_cast<size_t>(n));
        return true;
    }
    /// The next complete line received, without spaces (mpv writes compact JSON; spaces only make matching harder).
    bool line(std::string& out) {
        size_t nl = pending_.find('\n');
        if (nl == std::string::npos) return false;
        out.clear();
        for (size_t i = 0; i < nl; ++i)
            if (pending_[i] != ' ' && pending_[i] != '\r' && pending_[i] != '\t') out += pending_[i];
        pending_.erase(0, nl + 1);
        return true;
    }

private:
    int fd_ = -1;
    std::string pending_;
};

IntroResult done(IntroResult r, const std::string& text, std::string* why) {
    if (why) *why = text;
    if (r == IntroResult::Finished) log::info(LOG, "boot intro finished, taking the display");
    else if (r != IntroResult::NotRunning) log::warning(LOG, "boot intro: " + text + "; taking the display");
    return r;
}

}  // namespace

IntroResult finish_boot_intro(const std::string& dir, const IntroTimeouts& t, std::string* why) {
    if (why) why->clear();
    if (!exists(dir)) return IntroResult::NotRunning;
    const std::string sock = dir + "/" + INTRO_SOCKET, starting = dir + "/" + INTRO_STARTING;

    // 1. mpv's socket: ksphmi-intro.sh keeps INTRO_STARTING while it waits for the monitor and starts mpv.
    const auto t0 = Clock::now();
    Ipc ipc;
    for (;;) {
        if (exists(sock) && ipc.connect(sock)) break;
        if (!exists(dir) || (!exists(sock) && !exists(starting)))
            return done(IntroResult::NotRunning, "not playing", why);
        if (since(t0) > t.start)
            return done(IntroResult::Failed, fmt::str("no mpv socket after %.0f s", t.start), why);
        nap(t.poll);
    }

    // 2. Poll eof-reached (true on the held last frame with --keep-open) until the end of the video.
    const auto t1 = Clock::now();
    bool eof = false, gone = false;
    int id = 0;
    std::string reply;
    while (!eof && !gone && since(t1) < t.eof) {
        const auto asked = Clock::now();
        const std::string tag = fmt::str("\"request_id\":%d", ++id);
        if (!ipc.send(fmt::str("{\"command\":[\"get_property\",\"eof-reached\"],\"request_id\":%d}", id))) {
            gone = true;
            break;
        }
        // Read until this request's reply or the poll interval ends; event lines are skipped.
        bool answered = false;
        while (!answered && !gone && since(asked) < t.poll) {
            if (!ipc.read(t.poll - since(asked))) gone = true;
            while (ipc.line(reply))
                if (reply.find(tag) != std::string::npos) {
                    answered = true;
                    eof = reply.find("\"data\":true") != std::string::npos;
                }
        }
        if (!eof && !gone) nap(t.poll - since(asked));
    }
    if (gone) {
        // mpv ended by itself (error or killed): its unit stops and the display is released.
        const auto t2 = Clock::now();
        while (exists(dir) && since(t2) < t.quit) nap(t.poll / 4);
        return exists(dir) ? done(IntroResult::Failed, "mpv closed its socket before the end of the video", why)
                           : done(IntroResult::Finished, "", why);
    }
    const bool cut = !eof;

    // 3. Quit: mpv drops DRM master when it exits; its unit then stops and removes the intro directory.
    ipc.send("{\"command\":[\"quit\"]}");
    const auto t3 = Clock::now();
    bool closed = false;
    while (since(t3) < t.quit) {
        if (!closed && !ipc.read(t.poll / 4)) closed = true;
        while (ipc.line(reply)) {
        }
        if (closed && (!exists(dir) || !exists(sock))) break;
        if (closed) nap(t.poll / 4);
    }
    if (!closed || (exists(dir) && exists(sock)))
        return done(IntroResult::Failed, fmt::str("mpv still running %.0f s after quit", t.quit), why);
    if (cut) return done(IntroResult::Cut, fmt::str("no end of video after %.0f s, mpv stopped", t.eof), why);
    return done(IntroResult::Finished, "", why);
}

#endif

}  // namespace ksphmi
