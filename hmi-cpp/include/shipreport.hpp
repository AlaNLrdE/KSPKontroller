// Editor ship report from the KSPHMI kRPC service (KSPSerialIO.Hmi.dll, docs/hmi.rst "VAB ship report").
//
// Text format version 1, one "key=value" group per line, groups separated by single spaces:
//   v=1 rev=57 facility=vab name=Kerbal%20X sit=kerbin_asl
//   mass=18.40 dry=6.10 cost=23400 funds=41200 parts=42 crew=1 seats=3 size=11.2,3.8 engines=5
//   st=2,1720,1960,1.63,48,engines:3,clamps:1        (stage, dv, dv vacuum, TWR, burn s, events fired)
//   dv=pending                                        (instead of st= lines while KSP computes delta-v)
// Text values are percent-encoded UTF-8; "nan" is an unknown number; funds is absent outside career games.
// Unknown keys and line kinds are ignored (phase 2 adds chk= and lim=); any other version is rejected.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ksphmi {

constexpr int SHIP_REPORT_VERSION = 1;

struct StageEvent {
    std::string kind;  ///< engines, decouplers, chutes, clamps, fairings (others are shown as sent)
    int count = 0;
};

struct StageReport {
    int stage = 0;
    double delta_v = 0, delta_v_vac = 0;  ///< m/s, in the report's situation and in vacuum (NaN = unknown)
    double twr = 0, burn = 0;             ///< in the report's situation; seconds
    std::vector<StageEvent> events;
};

struct ShipReport {
    uint32_t revision = 0;
    std::string facility, name;  ///< decoded
    std::string situation;       ///< "<body>_<asl|alt|vac>", decoded
    std::optional<double> mass, dry, cost, funds, height, width;  ///< t, t, funds, funds, m, m
    std::optional<int> parts, crew, seats, engines;
    bool delta_v_pending = false;
    std::vector<StageReport> stages;  ///< firing order (highest stage number first)

    /// Sum of the finite stage delta-v, in the situation and in vacuum (0 without stages).
    double total_delta_v() const;
    double total_delta_v_vac() const;
    /// "KERBIN · SEA LEVEL" (ALTITUDE, VACUUM); the raw text when it has no known suffix.
    std::string situation_label() const;
};

/// TWR thresholds of the VAB report ([vab] launch_twr_min, launch_twr_warn, upper_twr_warn in config.toml).
struct TwrLimits {
    double launch_bad = 1.0;   ///< launch stage: red below (cannot leave the pad)
    double launch_warn = 1.2;  ///< launch stage: amber below
    double upper_warn = 0.8;   ///< upper stages: amber below
};

/// None = not judged: a stage without delta-v (no thrust) or with an unknown TWR.
enum class TwrLevel { None, Ok, Warn, Bad };

/// Index in `stages` of the launch stage: the first one in firing order with delta-v; -1 without one.
int launch_stage_index(const ShipReport& report);
/// The level of stage `index` (launch thresholds for the launch stage, upper-stage threshold for later ones).
TwrLevel twr_level(const ShipReport& report, size_t index, const TwrLimits& limits);
/// Worst level of the launch stage, and of all upper stages (apollo LAUNCH TWR / UPPER TWR lamps).
TwrLevel launch_twr_level(const ShipReport& report, const TwrLimits& limits);
TwrLevel upper_twr_level(const ShipReport& report, const TwrLimits& limits);

/// Parse a report; nullopt (and `error`) for an empty text, a missing or unknown version, or a malformed known field.
std::optional<ShipReport> parse_ship_report(const std::string& text, std::string* error = nullptr);

/// %XX decoding of a text value; nullopt for a truncated or non-hex escape.
std::optional<std::string> percent_decode(const std::string& s);

/// "mun_landing" -> "MUN LANDING".
std::string destination_label(const std::string& key);

/// What a stage fires, short and upper case: "3 ENG  CLAMP" (a count only above one); "" for nothing.
std::string events_text(const StageReport& stage);

}  // namespace ksphmi
