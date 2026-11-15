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

/// Parse a report; nullopt (and `error`) for an empty text, a missing or unknown version, or a malformed known field.
std::optional<ShipReport> parse_ship_report(const std::string& text, std::string* error = nullptr);

/// %XX decoding of a text value; nullopt for a truncated or non-hex escape.
std::optional<std::string> percent_decode(const std::string& s);

/// "mun_landing" -> "MUN LANDING".
std::string destination_label(const std::string& key);

/// What a stage fires, short and upper case: "3 ENG  CLAMP" (a count only above one); "" for nothing.
std::string events_text(const StageReport& stage);

}  // namespace ksphmi
