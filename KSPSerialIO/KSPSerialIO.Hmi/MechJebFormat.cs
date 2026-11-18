using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.Globalization;
using System.Text;

namespace KSPSerialIO.Hmi
{
    /// <summary>
    /// The MechJeb report format v1 (docs/hmi.rst "MechJeb scene"): the key list with each value's kind, the report
    /// text and the parsing of MechJebSet values. Plain C# without game or MechJeb types, so
    /// tools/plugin-shipreport-tests checks it against hmi-cpp/tests/data/mechjeb_report_v1.txt, the fixture the Pi
    /// parser and kspsim share. The reflection bridge formats each value with Number / Flag / Text and passes them
    /// to Report in Keys order, omitting keys the installed MechJeb lacks.
    /// </summary>
    public static class MechJebFormat
    {
        /// <summary>Text format version, the first field of every report ("v=1").</summary>
        public const int Version = 1;

        public enum Kind { Flag, Number, Integer, Text }

        public sealed class KeyInfo
        {
            public string Key;
            public Kind Kind;
            public bool ReadOnly;
            /// <summary>Present only while its condition holds (ascent.t_minus: while a timed launch is armed), so
            /// the fixture, which shows an idle vessel, leaves it out.</summary>
            public bool Conditional;
        }

        private static KeyInfo K(string key, Kind kind)
        {
            return new KeyInfo { Key = key, Kind = kind, ReadOnly = false };
        }

        private static KeyInfo RO(string key, Kind kind)
        {
            return new KeyInfo { Key = key, Kind = kind, ReadOnly = true };
        }

        private static readonly ReadOnlyCollection<KeyInfo> keys = new ReadOnlyCollection<KeyInfo>(new List<KeyInfo> {
            RO("ascent.enabled", Kind.Flag),
            RO("ascent.status", Kind.Text),
            new KeyInfo { Key = "ascent.t_minus", Kind = Kind.Number, ReadOnly = true, Conditional = true },
            K("ascent.path", Kind.Integer),
            K("ascent.altitude", Kind.Number),
            K("ascent.inclination", Kind.Number),
            K("ascent.lan", Kind.Number),
            K("ascent.launch_phase_angle", Kind.Number),
            K("ascent.launch_lan_difference", Kind.Number),
            K("ascent.warp_count_down", Kind.Integer),
            K("ascent.classic.auto", Kind.Flag),
            RO("ascent.classic.auto_turn_start_altitude", Kind.Number),
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
        });

        /// <summary>Every key of the contract, in fixture order.</summary>
        public static IList<KeyInfo> Keys { get { return keys; } }

        /// <summary>The procedures MechJebAction accepts.</summary>
        public static readonly string[] Actions = {
            "ascent.engage", "ascent.disengage", "ascent.launch_rendezvous", "ascent.launch_target_plane",
            "ascent.abort_timed_launch", "all.disengage" };

        /// <summary>The key's description, or null when the contract does not have it.</summary>
        public static KeyInfo Find(string key)
        {
            foreach (KeyInfo k in keys)
                if (k.Key == key) return k;
            return null;
        }

        /// <summary>
        /// The report: "v=1 rev=N mj=STATE[ version=X]" and one "key=value" line per entry, in the given order.
        /// STATE is ok, missing, nocore or error:TEXT (TEXT percent-encoded here); version is null when unknown.
        /// </summary>
        public static string Report(uint revision, string state, string version, IList<KeyValuePair<string, string>> values)
        {
            var sb = new StringBuilder();
            sb.Append("v=").Append(Version).Append(" rev=").Append(revision.ToString(CultureInfo.InvariantCulture)).Append(" mj=");
            const string error = "error:";
            if (state != null && state.StartsWith(error, StringComparison.Ordinal))
                sb.Append(error).Append(Text(state.Substring(error.Length)));
            else
                sb.Append(state);
            if (version != null) sb.Append(" version=").Append(Text(version));
            sb.Append('\n');
            if (values != null)
                foreach (KeyValuePair<string, string> kv in values)
                    sb.Append(kv.Key).Append('=').Append(kv.Value).Append('\n');
            return sb.ToString();
        }

        /// <summary>Invariant culture, at most 6 decimals, trailing zeros trimmed, no "-0"; non-finite is "nan".</summary>
        public static string Number(double v)
        {
            if (double.IsNaN(v) || double.IsInfinity(v)) return "nan";
            string s = v.ToString("F6", CultureInfo.InvariantCulture);
            if (s.IndexOf('.') >= 0) s = s.TrimEnd('0').TrimEnd('.');
            return s == "-0" ? "0" : s;
        }

        public static string Flag(bool b)
        {
            return b ? "1" : "0";
        }

        /// <summary>Percent-encoded UTF-8 text, the ship report's rule (ShipReportFormat.Escape).</summary>
        public static string Text(string s)
        {
            return ShipReportFormat.Escape(s);
        }

        /// <summary>
        /// Parse a MechJebSet value for the key's kind: "ok" or "invalid value". Flags take 0/1/true/false (number
        /// 0 or 1), integers must be integral and fit an int, numbers must be finite; text gives number 0. The
        /// caller checks ReadOnly.
        /// </summary>
        public static string Parse(KeyInfo key, string value, out double number)
        {
            number = 0;
            const string invalid = "invalid value";
            if (key == null || value == null) return invalid;
            switch (key.Kind)
            {
                case Kind.Text:
                    return "ok";
                case Kind.Flag:
                    if (value == "1" || string.Equals(value, "true", StringComparison.OrdinalIgnoreCase)) { number = 1; return "ok"; }
                    if (value == "0" || string.Equals(value, "false", StringComparison.OrdinalIgnoreCase)) { number = 0; return "ok"; }
                    return invalid;
                default:
                    double v;
                    if (!double.TryParse(value, NumberStyles.Float, CultureInfo.InvariantCulture, out v) ||
                        double.IsNaN(v) || double.IsInfinity(v))
                        return invalid;
                    if (key.Kind == Kind.Integer && (Math.Floor(v) != v || v < int.MinValue || v > int.MaxValue))
                        return invalid;
                    number = v == 0 ? 0 : v;
                    return "ok";
            }
        }
    }
}
