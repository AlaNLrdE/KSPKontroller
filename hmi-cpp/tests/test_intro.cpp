// Boot intro hand-over (intro.hpp) against a fake mpv JSON IPC server on an AF_UNIX socket. Linux only: on other
// systems finish_boot_intro() has nothing to wait for and the socket tests are skipped.
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

#include "check.hpp"
#include "intro.hpp"
#include "log.hpp"

using namespace ksphmi;

#if defined(__linux__)
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <atomic>
#include <cstring>

namespace {

IntroTimeouts quick() {
    IntroTimeouts t;
    t.start = 2;
    t.eof = 3;
    t.quit = 1;
    t.poll = 0.05;
    return t;
}

double seconds_since(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

void touch(const std::string& path) {
    if (FILE* f = std::fopen(path.c_str(), "w")) std::fclose(f);
}

/// Plays mpv's side: answers eof-reached false `false_replies` times (then true unless `never_eof`), sends event
/// lines in between, and on "quit" closes, removes its socket and the directory (like the systemd unit stopping).
struct FakeMpv {
    std::string dir, sock;
    int false_replies = 3;
    bool never_eof = false, ignore_quit = false;
    double delay = 0;  // before the socket appears
    std::atomic<int> queries{0};
    std::atomic<bool> quit{false};
    std::thread thread;

    explicit FakeMpv(const std::string& d) : dir(d), sock(d + "/" + INTRO_SOCKET) {}
    ~FakeMpv() {
        if (thread.joinable()) thread.join();
        unlink(sock.c_str());
        unlink((dir + "/" + INTRO_STARTING).c_str());
        rmdir(dir.c_str());
    }
    void start() { thread = std::thread([this] { run(); }); }
    void run() {
        std::this_thread::sleep_for(std::chrono::duration<double>(delay));
        int s = socket(AF_UNIX, SOCK_STREAM, 0);
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::strncpy(addr.sun_path, sock.c_str(), sizeof addr.sun_path - 1);
        if (bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0 || listen(s, 1) != 0) {
            close(s);
            return;
        }
        unlink((dir + "/" + INTRO_STARTING).c_str());  // ksphmi-intro.sh: mpv runs
        int c = accept(s, nullptr, nullptr);
        std::string buf;
        char tmp[512];
        bool open = c >= 0;
        while (open) {
            ssize_t n = recv(c, tmp, sizeof tmp, 0);
            if (n <= 0) break;
            buf.append(tmp, static_cast<size_t>(n));
            size_t nl;
            while ((nl = buf.find('\n')) != std::string::npos) {
                std::string line = buf.substr(0, nl);
                buf.erase(0, nl + 1);
                if (line.find("\"quit\"") != std::string::npos) {
                    quit = true;
                    if (!ignore_quit) open = false;
                    continue;
                }
                size_t at = line.find("\"request_id\":");
                if (at == std::string::npos) continue;
                int id = std::atoi(line.c_str() + at + 13);
                int q = ++queries;
                const bool eof = !never_eof && q > false_replies;
                std::string out = "{\"event\":\"playback-restart\"}\n";
                out += "{\"data\":" + std::string(eof ? "true" : "false") + ",\"request_id\":" + std::to_string(id) +
                       ",\"error\":\"success\"}\n";
                send(c, out.data(), out.size(), MSG_NOSIGNAL);
            }
        }
        if (ignore_quit) std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        if (c >= 0) close(c);
        close(s);
        unlink(sock.c_str());
        rmdir(dir.c_str());
    }
};

std::string new_dir() {
    std::string base = check::make_temp_dir("ksphmi-intro");
    return base.empty() ? "" : base + "/run";
}

}  // namespace

TEST(intro_absent_directory_is_not_running) {
    auto t0 = std::chrono::steady_clock::now();
    CHECK(finish_boot_intro("/nonexistent/ksphmi-intro", quick()) == IntroResult::NotRunning);
    CHECK(seconds_since(t0) < 0.5);
}

TEST(intro_directory_without_socket_or_starting_is_not_running) {
    std::string dir = new_dir();
    CHECK(!dir.empty());
    mkdir(dir.c_str(), 0700);
    CHECK(finish_boot_intro(dir, quick()) == IntroResult::NotRunning);
    rmdir(dir.c_str());
    check::remove_dir(dir.substr(0, dir.size() - 4));
}

TEST(intro_waits_for_the_socket_then_the_end_of_the_video) {
    std::string dir = new_dir();
    mkdir(dir.c_str(), 0700);
    touch(dir + "/" + INTRO_STARTING);
    {
        FakeMpv mpv(dir);
        mpv.delay = 0.3;
        mpv.start();
        std::string why;
        CHECK(finish_boot_intro(dir, quick(), &why) == IntroResult::Finished);
        CHECK(mpv.quit.load());
        CHECK(mpv.queries.load() >= 4);
    }
    check::remove_dir(dir.substr(0, dir.size() - 4));
}

TEST(intro_never_ending_video_is_cut_after_the_eof_timeout) {
    std::string dir = new_dir();
    mkdir(dir.c_str(), 0700);
    {
        FakeMpv mpv(dir);
        mpv.never_eof = true;
        mpv.start();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        IntroTimeouts t = quick();
        t.eof = 0.5;
        auto t0 = std::chrono::steady_clock::now();
        CHECK(finish_boot_intro(dir, t) == IntroResult::Cut);
        CHECK(seconds_since(t0) < 2);
        CHECK(mpv.quit.load());
    }
    check::remove_dir(dir.substr(0, dir.size() - 4));
}

TEST(intro_mpv_ignoring_quit_fails_within_the_quit_timeout) {
    std::string dir = new_dir();
    mkdir(dir.c_str(), 0700);
    {
        FakeMpv mpv(dir);
        mpv.false_replies = 0;
        mpv.ignore_quit = true;
        mpv.start();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        IntroTimeouts t = quick();
        t.quit = 0.4;
        std::string why;
        auto t0 = std::chrono::steady_clock::now();
        CHECK(finish_boot_intro(dir, t, &why) == IntroResult::Failed);
        CHECK(why.find("still running") != std::string::npos);
        CHECK(seconds_since(t0) < 1.5);
    }
    check::remove_dir(dir.substr(0, dir.size() - 4));
}

TEST(intro_no_socket_while_starting_fails_after_the_start_timeout) {
    std::string dir = new_dir();
    mkdir(dir.c_str(), 0700);
    touch(dir + "/" + INTRO_STARTING);
    IntroTimeouts t = quick();
    t.start = 0.3;
    CHECK(finish_boot_intro(dir, t) == IntroResult::Failed);
    unlink((dir + "/" + INTRO_STARTING).c_str());
    rmdir(dir.c_str());
    check::remove_dir(dir.substr(0, dir.size() - 4));
}

#else

TEST(intro_is_linux_only) { CHECK(finish_boot_intro("ignored") == IntroResult::NotRunning); }

#endif

int main(int argc, char** argv) {
    log::set_quiet(true);
    return RUN_TESTS(argc, argv);
}
