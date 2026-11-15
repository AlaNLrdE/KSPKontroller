using System;
using System.Collections.Generic;
using System.Globalization;
using System.Text;

namespace KSPSerialIO.Hmi
{
    /// <summary>One staging step of the editor ship, as KSP's editor delta-v app shows it.</summary>
    public sealed class StageFacts
    {
        /// <summary>Stage number as in the staging list (the highest fires first).</summary>
        public int Stage;
        /// <summary>Delta-v in m/s in the situation of KSP's editor delta-v app (body and sea level / altitude /
        /// vacuum), and in vacuum.</summary>
        public double DeltaV, DeltaVVac;
        /// <summary>Thrust-to-weight ratio in the app's situation; burn time in seconds.</summary>
        public double Twr, Burn;
        /// <summary>What the stage fires: kind ("engines", "decouplers", "chutes", "clamps", "fairings") and count.</summary>
        public readonly List<KeyValuePair<string, int>> Events = new List<KeyValuePair<string, int>>();
        /// <summary>Delta-v in m/s at Kerbin sea level, whatever situation the app shows (the kasl= line).</summary>
        public double DeltaVKerbinAsl = double.NaN;
    }

    /// <summary>
    /// What each stage fires, counted from KSP's staging list (or the parts' inverse stage when there is none).
    /// Plain data: tested without KSP (tools/plugin-shipreport-tests).
    /// </summary>
    public sealed class StageTally
    {
        /// <summary>Report order of the kinds.</summary>
        public static readonly string[] Order = { "engines", "decouplers", "chutes", "clamps", "fairings" };
        private readonly Dictionary<int, Dictionary<string, int>> stages = new Dictionary<int, Dictionary<string, int>>();

        /// <summary>Highest stage that fires something, -1 for none.</summary>
        public int MaxStage = -1;

        /// <summary>Count one part of `kind` firing in `stage`; ignored for a null kind or a negative stage.</summary>
        public void Add(int stage, string kind)
        {
            if (kind == null || stage < 0) return;
            Dictionary<string, int> s;
            if (!stages.TryGetValue(stage, out s)) stages[stage] = s = new Dictionary<string, int>();
            int n;
            s.TryGetValue(kind, out n);
            s[kind] = n + 1;
            if (stage > MaxStage) MaxStage = stage;
        }

        /// <summary>True when `stage` fires anything.</summary>
        public bool Has(int stage) { return stages.ContainsKey(stage); }

        /// <summary>The stage's events in report order.</summary>
        public List<KeyValuePair<string, int>> Events(int stage)
        {
            var list = new List<KeyValuePair<string, int>>();
            Dictionary<string, int> s;
            if (stages.TryGetValue(stage, out s))
                foreach (string k in Order)
                    if (s.ContainsKey(k)) list.Add(new KeyValuePair<string, int>(k, s[k]));
            return list;
        }

        /// <summary>
        /// The kind from a part's staging icon name (Part.stagingIcon, KSP's DefaultIcons: PARACHUTES,
        /// DECOUPLER_VERT, LIQUID_ENGINE, SOLID_BOOSTER, ...), for parts whose modules did not tell; null otherwise.
        /// </summary>
        public static string KindFromIcon(string icon)
        {
            string i = (icon ?? "").ToUpperInvariant();
            if (i.Length == 0) return null;
            if (i.Contains("PARACHUTE")) return "chutes";
            if (i.Contains("DECOUPLER") || i.Contains("SEPARATOR")) return "decouplers";
            if (i.Contains("ENGINE") || i.Contains("BOOSTER")) return "engines";
            if (i.Contains("CLAMP")) return "clamps";
            if (i.Contains("FAIRING")) return "fairings";
            return null;
        }
    }

    /// <summary>Delta-v at Kerbin sea level from vacuum delta-v, and the editor crew count (no KSP types).</summary>
    public static class ShipMath
    {
        /// <summary>
        /// The value KSP's delta-v app shows for its situation code ("asl", "alt", "vac"), from a DeltaVStageInfo's
        /// sea-level, actual and vacuum fields, like its GetSituationDeltaV / GetSituationTWR (SeaLevel, Altitude,
        /// Vaccum). The "actual" field is the altitude value: it does not follow a vacuum selection.
        /// </summary>
        public static double Situation(string code, double asl, double actual, double vac)
        {
            if (code == "vac") return vac;
            if (code == "asl") return asl;
            return actual;
        }

        /// <summary>
        /// Effective specific impulse of engines burning together: KSP keeps each engine's propellant flow fixed
        /// (thrust / Isp, from the vacuum figures), so the mixture's Isp at a pressure is the flow-weighted mean of
        /// the engines' Isp at that pressure. Each engine is { vacuum thrust kN, vacuum Isp s, Isp s at the pressure }.
        /// NaN without a burning engine.
        /// </summary>
        public static double Isp(IList<double[]> engines, int column)
        {
            double flow = 0, weighted = 0;
            foreach (double[] e in engines)
            {
                if (e == null || e.Length < 3 || !(e[0] > 0) || !(e[1] > 0) || double.IsNaN(e[column])) continue;
                double f = e[0] / e[1];
                flow += f;
                weighted += f * e[column];
            }
            return flow > 0 ? weighted / flow : double.NaN;
        }

        /// <summary>
        /// Delta-v at the pressure of column 2 for a burn with `vacuumDeltaV` in vacuum: the same masses and flow,
        /// so delta-v scales with the effective Isp. 0 for a burn without delta-v; NaN without engine data.
        /// </summary>
        public static double AtPressure(double vacuumDeltaV, IList<double[]> engines)
        {
            if (!(vacuumDeltaV > 0)) return 0;
            double vac = Isp(engines, 1), asl = Isp(engines, 2);
            return vac > 0 && !double.IsNaN(asl) ? vacuumDeltaV * asl / vac : double.NaN;
        }

        /// <summary>
        /// Crew aboard the current ship: the crew manifest's (part id, kerbals) entries for parts of this ship
        /// (part id -> crew capacity), at most each part's capacity; 0 for a ship without seats. The editor keeps
        /// the previous ship's manifest after New, so entries for parts that are gone never count. If no entry
        /// matches any part (ids from another numbering), the manifest's crew capped at the seats.
        /// </summary>
        public static int CrewAboard(IEnumerable<KeyValuePair<uint, int>> manifest, IDictionary<uint, int> capacity)
        {
            int total = 0, unmatched = 0, seats = 0;
            bool matched = false;
            if (manifest == null || capacity == null) return 0;
            foreach (int c in capacity.Values) seats += Math.Max(0, c);
            if (seats == 0) return 0;
            var used = new Dictionary<uint, int>();
            foreach (KeyValuePair<uint, int> m in manifest)
            {
                int cap;
                if (m.Value <= 0) continue;
                if (!capacity.TryGetValue(m.Key, out cap))
                {
                    unmatched += m.Value;
                    continue;
                }
                matched = true;
                int already;
                used.TryGetValue(m.Key, out already);
                int add = Math.Min(m.Value, cap - already);
                if (add <= 0) continue;
                used[m.Key] = already + add;
                total += add;
            }
            return matched ? total : Math.Min(unmatched, seats);
        }
    }

    /// <summary>Plain data of one editor ship: filled from KSP on the main thread, formatted without KSP types.</summary>
    public sealed class ShipFacts
    {
        /// <summary>Editor building: "vab" or "sph" (EditorDriver.editorFacility).</summary>
        public string Facility = "vab";
        /// <summary>Ship name as typed in the editor.</summary>
        public string Name = "";
        /// <summary>Delta-v situation, "body_asl" / "body_alt" / "body_vac" (e.g. "kerbin_asl").</summary>
        public string Situation = "";
        /// <summary>Tonnes (wet and dry) and funds; NaN = unknown (Funds is NaN outside career games).</summary>
        public double Mass, Dry, Cost, Funds = double.NaN;
        /// <summary>Part count, kerbals aboard, crew seats and engines.</summary>
        public int Parts, Crew, Seats, Engines;
        /// <summary>Craft size in metres: height and the larger horizontal extent.</summary>
        public double Height, Width;
        /// <summary>True until KSP has finished its first delta-v calculation of this ship.</summary>
        public bool DeltaVPending;
        /// <summary>Stages in firing order (highest stage number first).</summary>
        public readonly List<StageFacts> Stages = new List<StageFacts>();
    }

    /// <summary>
    /// The text the KSPHMI kRPC service returns from ShipReport(): one "key=value" group per line, separated by
    /// single spaces, version 1. Line 1: v, rev, facility, name, sit. Line 2: the vehicle summary. Then one st= line
    /// per stage ("st=stage,dv,dv_vac,twr,burn[,kind:count...]") and one "kasl=stage:dv,..." line (Kerbin sea-level
    /// delta-v per stage, independent of the app's situation; added in 0.19.4.1, optional for readers), or a single
    /// "dv=pending" line. Numbers use the
    /// invariant culture; "nan" marks an unknown value. Text values (name, situation) are percent-encoded: every
    /// UTF-8 byte outside A-Z a-z 0-9 . _ ~ - becomes %XX (so spaces, '=', ',' and '%' never appear raw).
    /// Readers must reject any v other than 1 and ignore line kinds they do not know (phase 2 adds chk= and lim=).
    /// </summary>
    public static class ShipReportFormat
    {
        /// <summary>Text format version, the first field of every report ("v=1").</summary>
        public const int Version = 1;

        /// <summary>Percent-encode a text value (see the class summary).</summary>
        public static string Escape(string s)
        {
            if (string.IsNullOrEmpty(s)) return "";
            var sb = new StringBuilder(s.Length);
            foreach (byte b in Encoding.UTF8.GetBytes(s))
            {
                bool plain = (b >= 'A' && b <= 'Z') || (b >= 'a' && b <= 'z') || (b >= '0' && b <= '9') ||
                             b == '.' || b == '_' || b == '~' || b == '-';
                if (plain) sb.Append((char)b);
                else sb.Append('%').Append(b.ToString("X2", CultureInfo.InvariantCulture));
            }
            return sb.ToString();
        }

        /// <summary>A number with `decimals` decimals, or "nan" when it is not finite.</summary>
        public static string Number(double v, int decimals)
        {
            if (double.IsNaN(v) || double.IsInfinity(v)) return "nan";
            string s = v.ToString("F" + decimals, CultureInfo.InvariantCulture);
            return s.StartsWith("-") && double.Parse(s, CultureInfo.InvariantCulture) == 0 ? s.Substring(1) : s;
        }

        /// <summary>Everything after "v=1 rev=N ": compared between builds to decide whether the revision changes.</summary>
        public static string Body(ShipFacts f)
        {
            var sb = new StringBuilder();
            sb.Append("facility=").Append(Escape(f.Facility))
              .Append(" name=").Append(Escape(f.Name))
              .Append(" sit=").Append(Escape(f.Situation)).Append('\n');
            sb.Append("mass=").Append(Number(f.Mass, 2))
              .Append(" dry=").Append(Number(f.Dry, 2))
              .Append(" cost=").Append(Number(f.Cost, 0));
            if (!double.IsNaN(f.Funds)) sb.Append(" funds=").Append(Number(f.Funds, 0));
            sb.Append(" parts=").Append(f.Parts.ToString(CultureInfo.InvariantCulture))
              .Append(" crew=").Append(f.Crew.ToString(CultureInfo.InvariantCulture))
              .Append(" seats=").Append(f.Seats.ToString(CultureInfo.InvariantCulture))
              .Append(" size=").Append(Number(f.Height, 1)).Append(',').Append(Number(f.Width, 1))
              .Append(" engines=").Append(f.Engines.ToString(CultureInfo.InvariantCulture)).Append('\n');
            if (f.DeltaVPending)
            {
                sb.Append("dv=pending\n");
                return sb.ToString();
            }
            foreach (StageFacts st in f.Stages)
            {
                sb.Append("st=").Append(st.Stage.ToString(CultureInfo.InvariantCulture))
                  .Append(',').Append(Number(st.DeltaV, 0))
                  .Append(',').Append(Number(st.DeltaVVac, 0))
                  .Append(',').Append(Number(st.Twr, 2))
                  .Append(',').Append(Number(st.Burn, 0));
                foreach (KeyValuePair<string, int> e in st.Events)
                    sb.Append(',').Append(Escape(e.Key)).Append(':').Append(e.Value.ToString(CultureInfo.InvariantCulture));
                sb.Append('\n');
            }
            if (f.Stages.Count > 0)
            {
                sb.Append("kasl=");
                for (int i = 0; i < f.Stages.Count; ++i)
                {
                    if (i > 0) sb.Append(',');
                    sb.Append(f.Stages[i].Stage.ToString(CultureInfo.InvariantCulture)).Append(':')
                      .Append(Number(f.Stages[i].DeltaVKerbinAsl, 0));
                }
                sb.Append('\n');
            }
            return sb.ToString();
        }

        /// <summary>The complete report for one revision.</summary>
        public static string Text(uint revision, string body)
        {
            return "v=" + Version.ToString(CultureInfo.InvariantCulture) + " rev=" +
                   revision.ToString(CultureInfo.InvariantCulture) + " " + body;
        }
    }

    /// <summary>
    /// The last report, built on Unity's main thread (EditorShipReport) and read by the kRPC procedures. The revision
    /// only increases when the report's contents change, so the Pi can poll it cheaply and fetch the text on change.
    /// </summary>
    public static class ShipReportCache
    {
        private static readonly object Gate = new object();
        private static string body;
        private static string text = "";
        private static uint revision;

        /// <summary>Increases whenever the report's contents change and when the editor is left.</summary>
        public static uint Revision { get { lock (Gate) return revision; } }

        /// <summary>The current report, or "" when no editor ship has been reported (the Pi then keeps its cards).</summary>
        public static string Report { get { lock (Gate) return text; } }

        /// <summary>Store a new report; true (and a new revision) when it differs from the previous one.</summary>
        public static bool Publish(ShipFacts facts)
        {
            string next = ShipReportFormat.Body(facts);
            lock (Gate)
            {
                if (next == body) return false;
                body = next;
                ++revision;
                text = ShipReportFormat.Text(revision, body);
                return true;
            }
        }

        /// <summary>Forget the ship (leaving the editor). The revision keeps counting, so the next one is new.</summary>
        public static void Clear()
        {
            lock (Gate)
            {
                if (body == null) return;
                body = null;
                ++revision;
                text = "";
            }
        }
    }
}
