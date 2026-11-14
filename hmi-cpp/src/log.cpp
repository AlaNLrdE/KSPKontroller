#include "log.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <mutex>

namespace ksphmi::log {

namespace {
std::mutex mutex;
std::atomic<bool> quiet{false};
}  // namespace

void set_quiet(bool q) { quiet = q; }

void write(Level level, const char* name, const std::string& text) {
    if (quiet && level != Level::Error) return;
    using namespace std::chrono;
    auto now = system_clock::now();
    std::time_t t = system_clock::to_time_t(now);
    int ms = static_cast<int>(duration_cast<milliseconds>(now.time_since_epoch()).count() % 1000);
    std::tm tm{};
    localtime_r(&t, &tm);
    char stamp[32];
    std::strftime(stamp, sizeof stamp, "%Y-%m-%d %H:%M:%S", &tm);
    static const char* names[] = {"DEBUG", "INFO", "WARNING", "ERROR"};
    std::lock_guard<std::mutex> lock(mutex);
    std::fprintf(stderr, "%s,%03d %s %s: %s\n", stamp, ms, names[static_cast<int>(level)], name, text.c_str());
    std::fflush(stderr);
}

}  // namespace ksphmi::log
