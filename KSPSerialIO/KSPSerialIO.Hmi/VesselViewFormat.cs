using System;
using System.Collections.Generic;
using System.Globalization;
using System.Text;

namespace KSPSerialIO.Hmi
{
    /// <summary>One part of the VESSEL side view: depth (mean z in the root part's frame, larger = farther) and its
    /// convex outline in metres (x right, y up along the stack).</summary>
    public sealed class VesselPartShape
    {
        public double Depth;
        public List<double[]> Hull = new List<double[]>();
    }

    /// <summary>
    /// The VESSEL panel's text format v1 (docs/hmi.rst "VESSEL panel"), the convex hull and the fuel rule. Plain C#
    /// without game types, so tools/plugin-shipreport-tests checks it against hmi-cpp/tests/data/vessel_shape_v1.txt
    /// and vessel_state_v1.txt, the fixtures the Pi parser and kspsim share.
    /// </summary>
    public static class VesselViewFormat
    {
        /// <summary>Outline points kept per part; a longer hull keeps evenly spaced vertices (still convex).</summary>
        public const int MaxHullPoints = 32;

        /// <summary>Resources that are not propellant for the FUEL view.</summary>
        private static readonly string[] NotFuel = { "ElectricCharge", "Ablator", "IntakeAir", "Ore" };

        public static bool IsFuel(string resourceName)
        {
            return Array.IndexOf(NotFuel, resourceName) < 0;
        }

        /// <summary>
        /// True for an engine plume or particle effect shader (Waterfall, stock and SmokeScreen particles, additive
        /// glows): those meshes hang tens of metres below engines and are not part of the vessel's outline.
        /// </summary>
        public static bool IsEffectShader(string shaderName)
        {
            if (string.IsNullOrEmpty(shaderName)) return false;
            return shaderName.StartsWith("Waterfall/", StringComparison.Ordinal) ||
                   shaderName.IndexOf("Particle", StringComparison.OrdinalIgnoreCase) >= 0 ||
                   shaderName.IndexOf("Additive", StringComparison.OrdinalIgnoreCase) >= 0;
        }

        /// <summary>Percent of capacity, rounded; -1 without capacity (no tanks).</summary>
        public static int FuelPercent(double amount, double capacity)
        {
            if (!(capacity > 1e-9)) return -1;
            return Clamp((int)Math.Round(100.0 * amount / capacity), 0, 100);
        }

        /// <summary>The hotter of internal and skin temperature, percent of its maximum.</summary>
        public static int HeatPercent(double temperature, double maxTemperature, double skin, double maxSkin)
        {
            double t = maxTemperature > 0 ? temperature / maxTemperature : 0;
            double s = maxSkin > 0 ? skin / maxSkin : 0;
            double h = Math.Max(t, s);
            if (double.IsNaN(h)) return 0;
            return Clamp((int)Math.Round(100.0 * h), 0, 100);
        }

        /// <summary>Andrew's monotone chain: counter-clockwise from the lowest-left point, no collinear points, at
        /// most MaxHullPoints. Fewer than three distinct points give what there is.</summary>
        public static List<double[]> ConvexHull(IList<double[]> points)
        {
            var p = new List<double[]>(points);
            p.Sort((a, b) => a[0] != b[0] ? a[0].CompareTo(b[0]) : a[1].CompareTo(b[1]));
            var unique = new List<double[]>();
            foreach (double[] q in p)
                if (unique.Count == 0 || unique[unique.Count - 1][0] != q[0] || unique[unique.Count - 1][1] != q[1])
                    unique.Add(q);
            if (unique.Count < 3) return unique;
            var hull = new List<double[]>();
            for (int pass = 0; pass < 2; ++pass)
            {
                int start = hull.Count;
                for (int k = 0; k < unique.Count; ++k)
                {
                    double[] q = unique[pass == 0 ? k : unique.Count - 1 - k];
                    while (hull.Count >= start + 2 && Cross(hull[hull.Count - 2], hull[hull.Count - 1], q) <= 0)
                        hull.RemoveAt(hull.Count - 1);
                    hull.Add(q);
                }
                hull.RemoveAt(hull.Count - 1);  // each chain's last point starts the other chain
            }
            if (hull.Count <= MaxHullPoints) return hull;
            var kept = new List<double[]>();
            for (int k = 0; k < MaxHullPoints; ++k) kept.Add(hull[k * hull.Count / MaxHullPoints]);
            return kept;
        }

        private static double Cross(double[] o, double[] a, double[] b)
        {
            return (a[0] - o[0]) * (b[1] - o[1]) - (a[1] - o[1]) * (b[0] - o[0]);
        }

        /// <summary>"v=1 rev=N parts=K" then "p=depth,x:y,..." per part, two decimals, invariant culture.</summary>
        public static string Shape(uint revision, IList<VesselPartShape> parts)
        {
            var sb = new StringBuilder();
            sb.Append("v=1 rev=").Append(revision.ToString(CultureInfo.InvariantCulture))
              .Append(" parts=").Append(parts.Count.ToString(CultureInfo.InvariantCulture)).Append('\n');
            foreach (VesselPartShape part in parts)
            {
                sb.Append("p=").Append(Num(part.Depth));
                foreach (double[] q in part.Hull) sb.Append(',').Append(Num(q[0])).Append(':').Append(Num(q[1]));
                sb.Append('\n');
            }
            return sb.ToString();
        }

        /// <summary>"v=1 rev=N" then "s=fuel:heat,..." in shape order (fuel -1 = no tanks).</summary>
        public static string State(uint revision, IList<int[]> fuelHeat)
        {
            var sb = new StringBuilder();
            sb.Append("v=1 rev=").Append(revision.ToString(CultureInfo.InvariantCulture)).Append("\ns=");
            for (int i = 0; i < fuelHeat.Count; ++i)
            {
                if (i > 0) sb.Append(',');
                sb.Append(fuelHeat[i][0].ToString(CultureInfo.InvariantCulture)).Append(':')
                  .Append(fuelHeat[i][1].ToString(CultureInfo.InvariantCulture));
            }
            return sb.Append('\n').ToString();
        }

        private static string Num(double v)
        {
            double r = Math.Round(v, 2);
            if (r == 0 || double.IsNaN(r) || double.IsInfinity(r)) r = 0;  // never "-0.00"
            return r.ToString("F2", CultureInfo.InvariantCulture);
        }

        private static int Clamp(int v, int lo, int hi)
        {
            return v < lo ? lo : v > hi ? hi : v;
        }
    }

    /// <summary>What KSPHMIFlight serves; written on Unity's main thread by VesselViewReport, read by kRPC (also the
    /// main thread). Empty vessel until the first report.</summary>
    public static class VesselViewCache
    {
        public static uint Revision;
        public static string Shape = "v=1 rev=0 parts=0\n";
        public static string State = "v=1 rev=0\ns=\n";
    }
}
