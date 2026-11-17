// STAND-IN: replace with the Mac agent's MechJebFormat.cs (same public API). It exists only so MechJebBridge
// compiles and can be tested before the real format lands; it follows the agreed API and rules.
using System;
using System.Collections.Generic;
using System.Globalization;
using System.Text;

namespace KSPSerialIO.Hmi
{
    /// <summary>MechJeb report format v1 (docs/hmi.rst, "MechJeb scene"; fixture hmi-cpp/tests/data/mechjeb_report_v1.txt).
    /// Plain C#, no game types.</summary>
    public static class MechJebFormat
    {
        public enum Kind { Flag, Number, Integer, Text }

        public sealed class KeyInfo
        {
            public string Key;
            public Kind Kind;
            public bool ReadOnly;
        }

        private static KeyInfo K(string key, Kind kind, bool readOnly = false)
        {
            return new KeyInfo { Key = key, Kind = kind, ReadOnly = readOnly };
        }

        /// <summary>Every v1 key, in report (fixture) order.</summary>
        public static readonly IList<KeyInfo> Keys = new List<KeyInfo> {
            K("ascent.enabled", Kind.Flag, true),
            K("ascent.status", Kind.Text, true),
            K("ascent.t_minus", Kind.Number, true),
            K("ascent.path", Kind.Integer),
            K("ascent.altitude", Kind.Number),
            K("ascent.inclination", Kind.Number),
            K("ascent.lan", Kind.Number),
            K("ascent.launch_phase_angle", Kind.Number),
            K("ascent.launch_lan_difference", Kind.Number),
            K("ascent.warp_count_down", Kind.Integer),
            K("ascent.classic.auto", Kind.Flag),
            K("ascent.classic.auto_turn_start_altitude", Kind.Number, true),
            K("ascent.classic.turn_start_altitude", Kind.Number),
            K("ascent.classic.turn_start_velocity", Kind.Number),
            K("ascent.classic.turn_end_altitude", Kind.Number),
            K("ascent.classic.turn_end_angle", Kind.Number),
            K("ascent.classic.turn_shape_exponent", Kind.Number),
            K("ascent.pvg.desired_apoapsis", Kind.Number),
            K("ascent.pvg.desired_attach_alt", Kind.Number),
            K("ascent.pvg.desired_fpa", Kind.Number),
            K("ascent.pvg.pitch_start_velocity", Kind.Number),
            K("ascent.pvg.pitch_rate", Kind.Number),
            K("ascent.force_roll", Kind.Flag),
            K("ascent.vertical_roll", Kind.Number),
            K("ascent.turn_roll", Kind.Number),
            K("ascent.roll_altitude", Kind.Number),
            K("ascent.limit_aoa", Kind.Flag),
            K("ascent.max_aoa", Kind.Number),
            K("ascent.limit_qa", Kind.Flag),
            K("ascent.max_qa", Kind.Number),
            K("ascent.corrective_steering", Kind.Flag),
            K("ascent.corrective_steering_gain", Kind.Number),
            K("ascent.autostage", Kind.Flag),
            K("ascent.last_stage", Kind.Integer),
            K("ascent.autodeploy_solar_panels", Kind.Flag),
            K("ascent.autodeploy_antennas", Kind.Flag),
            K("ascent.skip_circularization", Kind.Flag),
            K("thrust.limit_q", Kind.Flag),
            K("thrust.max_q", Kind.Number),
            K("thrust.limit_overheat", Kind.Flag),
            K("thrust.limit_acceleration", Kind.Flag),
            K("thrust.max_acceleration", Kind.Number),
            K("thrust.limit_throttle", Kind.Flag),
            K("thrust.max_throttle", Kind.Number),
            K("thrust.limit_min_throttle", Kind.Flag),
            K("thrust.min_throttle", Kind.Number),
            K("thrust.smooth_throttle", Kind.Flag),
            K("thrust.prevent_flameout", Kind.Flag),
            K("thrust.flameout_safety_pct", Kind.Number),
            K("thrust.prevent_unstable_ignition", Kind.Flag),
            K("thrust.differential_throttle", Kind.Flag),
            K("staging.autostage_limit", Kind.Integer),
            K("staging.pre_delay", Kind.Number),
            K("staging.post_delay", Kind.Number),
            K("staging.clamp_thrust_pct", Kind.Number),
            K("staging.hot_staging", Kind.Flag),
            K("staging.hot_staging_lead_time", Kind.Number),
            K("staging.drop_solids", Kind.Flag),
            K("staging.drop_solids_lead_time", Kind.Number),
            K("staging.autostaging_once", Kind.Flag),
            K("staging.fairing_max_q", Kind.Number),
            K("staging.fairing_min_altitude", Kind.Number),
            K("staging.fairing_max_flux", Kind.Number),
        };

        /// <summary>MechJebAction names.</summary>
        public static readonly string[] Actions = {
            "ascent.engage", "ascent.disengage", "ascent.launch_rendezvous", "ascent.launch_target_plane",
            "ascent.abort_timed_launch", "all.disengage" };

        /// <summary>"v=1 rev=N mj=STATE[ version=X]", then one key=value line per entry, '\n' endings.</summary>
        public static string Report(uint revision, string state, string version, IList<KeyValuePair<string, string>> values)
        {
            var sb = new StringBuilder();
            sb.Append("v=1 rev=").Append(revision.ToString(CultureInfo.InvariantCulture)).Append(" mj=").Append(state);
            if (version != null) sb.Append(" version=").Append(version);
            sb.Append('\n');
            if (values != null)
                foreach (KeyValuePair<string, string> kv in values) sb.Append(kv.Key).Append('=').Append(kv.Value).Append('\n');
            return sb.ToString();
        }

        /// <summary>Invariant culture, at most 6 decimals, trailing zeros trimmed; non-finite is "nan".</summary>
        public static string Number(double v)
        {
            if (double.IsNaN(v) || double.IsInfinity(v)) return "nan";
            string s = Math.Round(v, 6).ToString("0.######", CultureInfo.InvariantCulture);
            return s == "-0" ? "0" : s;
        }

        public static string Flag(bool b) { return b ? "1" : "0"; }

        /// <summary>Percent-encode every UTF-8 byte outside A-Z a-z 0-9 . _ -.</summary>
        public static string Text(string s)
        {
            if (string.IsNullOrEmpty(s)) return "";
            var sb = new StringBuilder(s.Length);
            foreach (byte b in Encoding.UTF8.GetBytes(s))
            {
                bool plain = (b >= 'A' && b <= 'Z') || (b >= 'a' && b <= 'z') || (b >= '0' && b <= '9') ||
                             b == '.' || b == '_' || b == '-';
                if (plain) sb.Append((char)b);
                else sb.Append('%').Append(b.ToString("X2", CultureInfo.InvariantCulture));
            }
            return sb.ToString();
        }

        /// <summary>"ok" and the value as a number (flags 0/1), or "invalid value". ReadOnly is the caller's check.</summary>
        public static string Parse(KeyInfo key, string value, out double number)
        {
            number = 0;
            if (key == null || value == null) return "invalid value";
            string v = value.Trim();
            switch (key.Kind)
            {
                case Kind.Flag:
                    if (v == "1" || v.Equals("true", StringComparison.OrdinalIgnoreCase)) { number = 1; return "ok"; }
                    if (v == "0" || v.Equals("false", StringComparison.OrdinalIgnoreCase)) { number = 0; return "ok"; }
                    return "invalid value";
                case Kind.Integer:
                case Kind.Number:
                    double d;
                    if (!double.TryParse(v, NumberStyles.Float, CultureInfo.InvariantCulture, out d) ||
                        double.IsNaN(d) || double.IsInfinity(d)) return "invalid value";
                    if (key.Kind == Kind.Integer && (d != Math.Floor(d) || d < int.MinValue || d > int.MaxValue))
                        return "invalid value";
                    number = d;
                    return "ok";
                default:
                    return "invalid value";
            }
        }
    }
}
