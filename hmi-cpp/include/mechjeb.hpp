// The MechJeb report of KSPHMIFlight.MechJebReport() (KSPSerialIO.Hmi.dll, docs/hmi.rst "MechJeb scene"), format
// version 1 (shared fixture hmi-cpp/tests/data/mechjeb_report_v1.txt):
//
//   v=1 rev=42 mj=ok version=2.15.3.0          (mj: ok, missing, nocore or error:TEXT)
//   ascent.altitude=80000                       (one key=value per line: SI numbers, 0/1 flags or
//   ascent.status=Awaiting%20liftoff             percent-encoded text)
//
// A key the installed MechJeb lacks is omitted: its control stays unknown, never zero. Unknown keys are kept but
// never shown, so later modules add lines without a new version. Any other version is rejected.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>

namespace ksphmi {

constexpr int MECHJEB_REPORT_VERSION = 1;

struct MechJebReport {
    enum class Status { Ok, Missing, NoCore, Error };
    Status status = Status::Ok;
    uint32_t rev = 0;
    std::string version;  ///< MechJeb's own version ("" when not installed)
    std::string error;    ///< mj=error:TEXT, decoded
    std::map<std::string, std::string> values;  ///< decoded values by key

    bool ok() const { return status == Status::Ok; }
    bool has(const std::string& key) const { return values.count(key) != 0; }
    /// A finite number, or nothing (absent key, text, nan).
    std::optional<double> number(const std::string& key) const;
    /// 0 / 1 flags; anything else is unknown.
    std::optional<bool> flag(const std::string& key) const;
    std::optional<std::string> text(const std::string& key) const;
};

/// %XX sequences to bytes; false in `ok` for a broken sequence (the text is then kept as it was).
std::string percent_decode(const std::string& s, bool* ok = nullptr);

/// Nothing (and why, in `error`) when the first line is not "v=1 rev=N mj=STATE" or a line is not key=value.
std::optional<MechJebReport> parse_mechjeb_report(const std::string& text, std::string* error = nullptr);

/// The demo source's report: the same values as the shared fixture (MechJeb installed, Ascent Guidance engaged).
std::shared_ptr<const MechJebReport> demo_mechjeb_report();
/// Its text, in report format v1.
const char* demo_mechjeb_text();

}  // namespace ksphmi
