// Log lines on stderr in the Python logging format: "2026-11-14 10:00:00,123 INFO ksphmi.telemetry: text".
// systemd puts them in the journal (./ci hmi_logs).
#pragma once

#include <string>

namespace ksphmi::log {

enum class Level { Debug, Info, Warning, Error };

void write(Level level, const char* name, const std::string& text);
inline void info(const char* name, const std::string& text) { write(Level::Info, name, text); }
inline void warning(const char* name, const std::string& text) { write(Level::Warning, name, text); }
inline void error(const char* name, const std::string& text) { write(Level::Error, name, text); }
/// Tests silence the log.
void set_quiet(bool quiet);

}  // namespace ksphmi::log
