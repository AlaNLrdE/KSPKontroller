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
    }

    /// <summary>Plain data of one editor ship: filled from KSP on the main thread, formatted without KSP types.</summary>
    public sealed class ShipFacts
    {
        public string Facility = "vab";
        public string Name = "";
        /// <summary>Delta-v situation, "body_asl" / "body_alt" / "body_vac" (e.g. "kerbin_asl").</summary>
        public string Situation = "";
        /// <summary>Tonnes (wet and dry) and funds; NaN = unknown (Funds is NaN outside career games).</summary>
        public double Mass, Dry, Cost, Funds = double.NaN;
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
    /// per stage ("st=stage,dv,dv_vac,twr,burn[,kind:count...]"), or a single "dv=pending" line. Numbers use the
    /// invariant culture; "nan" marks an unknown value. Text values (name, situation) are percent-encoded: every
    /// UTF-8 byte outside A-Z a-z 0-9 . _ ~ - becomes %XX (so spaces, '=', ',' and '%' never appear raw).
    /// Readers must reject any v other than 1 and ignore line kinds they do not know (phase 2 adds chk= and lim=).
    /// </summary>
    public static class ShipReportFormat
    {
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
