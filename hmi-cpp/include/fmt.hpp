// Number formatting for the screens: short strings that do not jump around.
#pragma once

#include <string>

namespace ksphmi::fmt {

std::string str(const char* format, ...) __attribute__((format(printf, 1, 2)));

/// Metres as m / km / Mm / Gm: "950 m", "80.0 km", "12.40 Mm". "--" when not finite.
std::string distance(double m);
std::string speed(double ms);
/// Seconds as "45s", "12m 03s", "2h 05m", "3d 4h" (KSP days are 6 h unless day_hours says 24).
std::string duration(double s, int day_hours = 6);
/// Universal time as the KSP calendar date "Y1 D001 00:00:00".
std::string clock(double ut, int day_hours = 6, int year_days = 426);
std::string degrees(double v);
/// kRPC VesselSituation name to a label: "sub_orbital" -> "Sub-orbital".
std::string situation(const std::string& name);
/// Python's round(): halves to even.
long long round_half_even(double v);

}  // namespace ksphmi::fmt
