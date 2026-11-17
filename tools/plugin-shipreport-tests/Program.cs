// Ship report format tests (KSPSerialIO.Hmi ShipReportFormat / ShipReportCache), run by tools/verify.py
// check_ship_report_contract. The expected text is hmi-cpp/tests/data/ship_report_v1.txt, which the Pi's parser
// (hmi-cpp/tests/test_core.cpp) and kspsim's formatter (tools/kspsim/test_kspsim.py) read too.
using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Threading;
using KSPSerialIO.Hmi;

internal static partial class Program
{
    private static int failures;

    private static void Check(bool ok, string what)
    {
        if (ok) return;
        ++failures;
        Console.WriteLine("FAIL " + what);
    }

    private static StageFacts Stage(int n, double dv, double vac, double twr, double burn, params object[] events)
    {
        var s = new StageFacts { Stage = n, DeltaV = dv, DeltaVVac = vac, Twr = twr, Burn = burn, DeltaVKerbinAsl = dv };
        for (int i = 0; i + 1 < events.Length; i += 2)
            s.Events.Add(new KeyValuePair<string, int>((string)events[i], (int)events[i + 1]));
        return s;
    }

    public static ShipFacts Sample()
    {
        var f = new ShipFacts
        {
            Facility = "vab", Name = "Kerbal X", Situation = "kerbin_asl", Mass = 18.4, Dry = 6.1, Cost = 23400,
            Funds = 41200, Parts = 42, Crew = 1, Seats = 3, Height = 11.2, Width = 3.8, Engines = 5
        };
        f.Stages.Add(Stage(2, 1720, 1960, 1.63, 48, "engines", 3, "clamps", 1));
        f.Stages.Add(Stage(1, 1890, 2010, 1.41, 71, "engines", 1, "decouplers", 1));
        f.Stages.Add(Stage(0, 1210, 1340, 0.62, 52, "engines", 1, "decouplers", 1, "chutes", 1));
        return f;
    }

    // The stock Kerbal X's last stage fires only its parachute: a stage without delta-v must still list it.
    private static void StageEvents()
    {
        var t = new StageTally();
        for (int i = 0; i < 7; ++i) t.Add(7, "engines");
        for (int i = 0; i < 3; ++i) t.Add(7, "clamps");
        t.Add(1, "decouplers");
        t.Add(0, "chutes");
        t.Add(-1, "decouplers");  // a heat shield's decoupler with staging off
        t.Add(2, null);
        Check(t.MaxStage == 7 && t.Has(0) && !t.Has(2), "tally stages");
        List<KeyValuePair<string, int>> seven = t.Events(7);
        Check(seven.Count == 2 && seven[0].Key == "engines" && seven[0].Value == 7 && seven[1].Key == "clamps",
              "engines before clamps");
        var f = new ShipFacts { Name = "Kerbal X", Situation = "kerbin_asl" };
        var chute = new StageFacts { Stage = 0, DeltaVKerbinAsl = 0 };
        chute.Events.AddRange(t.Events(0));
        f.Stages.Add(chute);
        string body = ShipReportFormat.Body(f);
        Check(body.Contains("\nst=0,0,0,0.00,0,chutes:1\n") && body.EndsWith("kasl=0:0\n"), "chute-only stage:\n" + body);
        Check(StageTally.KindFromIcon("PARACHUTES") == "chutes" && StageTally.KindFromIcon("DECOUPLER_VERT") == "decouplers" &&
              StageTally.KindFromIcon("LIQUID_ENGINE") == "engines" && StageTally.KindFromIcon("SOLID_BOOSTER") == "engines" &&
              StageTally.KindFromIcon("") == null && StageTally.KindFromIcon(null) == null &&
              StageTally.KindFromIcon("FUEL_TANK") == null, "staging icon kinds");
    }

    // Duna vacuum in KSP's app: the situation value is the vacuum one (the app showed 2452 m/s, TWR 5.42 for the
    // Kerbal X's Poodle stage; deltaVActual read 2328, TWRActual 5.15).
    private static void Situations()
    {
        Check(VesselViewFormat.IsEffectShader("Waterfall/Additive (Dynamic)") &&
              VesselViewFormat.IsEffectShader("KSP/Particles/Alpha Blended") &&
              VesselViewFormat.IsEffectShader("Legacy Shaders/Particles/Additive") &&
              !VesselViewFormat.IsEffectShader("KSP/Bumped Specular") && !VesselViewFormat.IsEffectShader("TU/Metallic") &&
              !VesselViewFormat.IsEffectShader(null), "vessel view effect shaders");
        Check(ShipMath.Situation("vac", 1000, 2328, 2452) == 2452, "vacuum situation uses the vacuum value");
        Check(ShipMath.Situation("vac", 4.9, 5.15, 5.42) == 5.42, "vacuum situation TWR");
        Check(ShipMath.Situation("asl", 631, 700, 2452) == 631, "sea level");
        Check(ShipMath.Situation("alt", 631, 700, 2452) == 700, "altitude is the actual value");
    }

    // Kerbin sea level (1 atm) from vacuum delta-v for the stock Kerbal X: KSP's app showed 257, 304, 372, 2764 and 631.
    private static void KerbinSeaLevel()
    {
        double[] mainsail = { 1500, 310, 285 }, swivel = { 215, 320, 250 }, poodle = { 250, 350, 90 };
        var launch = new List<double[]> { mainsail };
        for (int i = 0; i < 6; ++i) launch.Add(swivel);
        var core = new List<double[]> { mainsail };
        Check(Math.Abs(ShipMath.AtPressure(301, launch) - 257) <= 1, "stage 7 Kerbin ASL " + ShipMath.AtPressure(301, launch));
        Check(Math.Abs(ShipMath.AtPressure(3006, core) - 2764) <= 1, "stage 4 Kerbin ASL " + ShipMath.AtPressure(3006, core));
        Check(ShipReportFormat.Number(ShipMath.AtPressure(2452, new List<double[]> { poodle }), 0) == "631", "stage 2 Kerbin ASL");
        Check(ShipMath.AtPressure(0, core) == 0, "no delta-v");
        Check(double.IsNaN(ShipMath.AtPressure(100, new List<double[]>())), "no engine data");
    }

    // After New the editor keeps the previous crew manifest: crew must come from this ship's parts and seats.
    private static void Crew()
    {
        var none = new Dictionary<uint, int>();
        var stale = new List<KeyValuePair<uint, int>> { new KeyValuePair<uint, int>(4294320818u, 3) };
        Check(ShipMath.CrewAboard(stale, none) == 0, "empty ship has no crew");
        var pod = new Dictionary<uint, int> { { 100u, 3 } };
        Check(ShipMath.CrewAboard(new List<KeyValuePair<uint, int>> { new KeyValuePair<uint, int>(100u, 2) }, pod) == 2, "crew in the pod");
        Check(ShipMath.CrewAboard(new List<KeyValuePair<uint, int>> { new KeyValuePair<uint, int>(100u, 5) }, pod) == 3, "at most the seats");
        var mixed = new List<KeyValuePair<uint, int>> { new KeyValuePair<uint, int>(100u, 1), new KeyValuePair<uint, int>(7u, 3) };
        Check(ShipMath.CrewAboard(mixed, pod) == 1, "parts that are gone do not count");
        Check(ShipMath.CrewAboard(stale, new Dictionary<uint, int> { { 1u, 1 } }) == 1, "unmatched ids: capped at the seats");
    }

    private static void EditorContract(string root)
    {
        var f = Sample();
        f.EditorMode = "actions";
        f.SelectedGroup = "custom1";
        f.Actions.Add(new ActionFacts { Group = "custom1", Part = "Communotron 16", Name = "Toggle antenna" });
        for (int i = 0; i < 3; ++i)
            f.Manifest.Add(new SeatFacts { PartId = "1", Part = "Mk1-3 Pod", Seat = i,
                Name = i == 0 ? "Valentina Kerman" : "", Role = i == 0 ? "Pilot" : "", Level = i == 0 ? 1 : 0 });
        f.Inventories.Add(new InventoryFacts { Key = "1:0", Part = "Mk1-3 Pod", Slots = 3,
            Mass = .022, MassCapacity = .05, Volume = 84, VolumeCapacity = 120 });
        f.Cargo.Add(new CargoFacts { Inventory = "1:0", Slot = 0, Name = "Repair kit", Quantity = 2, Mass = .010, Volume = 20 });
        f.Cargo.Add(new CargoFacts { Inventory = "1:0", Slot = 1, Name = "EVA cylinder", Quantity = 1, Mass = .012, Volume = 64 });
        var names = new Dictionary<string,string> { {"engines","Engine"}, {"clamps","Launch clamp"}, {"decouplers","Decoupler"}, {"chutes","Parachute"} };
        foreach (StageFacts st in f.Stages)
            foreach (var e in st.Events)
                st.Items.Add(new StagePartFacts { Stage = st.Stage, Kind = e.Key, Name = names[e.Key], Count = e.Value });
        string golden = File.ReadAllText(Path.Combine(root, "hmi-cpp", "tests", "data", "editor_report_v1.txt")).Replace("\r\n", "\n");
        Check(ShipReportFormat.Text(57, ShipReportFormat.Body(f)) == golden, "extended editor fixture");
        var tally = new StageTally();
        tally.Add(0, "chutes", "Chute, A=B%");
        tally.Add(0, "chutes", "Chute, A=B%");
        Check(tally.Items(0).Count == 1 && tally.Items(0)[0].Count == 2, "staged names aggregate quantities");
        f.DeltaVPending = true;
        string pending = ShipReportFormat.Body(f);
        Check(pending.Contains("editor=actions,custom1") && pending.Contains("seat=") && !pending.Contains("\nst="),
              "editor screens keep data while delta-v is pending");
        ShipReportCache.Clear();
        ShipReportCache.Publish(f);
        uint previous = ShipReportCache.Revision;
        f.EditorMode = "crew";
        Check(ShipReportCache.Publish(f) && ShipReportCache.Revision == previous + 1, "mode change advances revision");
        previous = ShipReportCache.Revision;
        f.SelectedGroup = "sas";
        Check(ShipReportCache.Publish(f) && ShipReportCache.Revision == previous + 1, "group change advances revision");
        ShipReportCache.Clear();
    }

    /// <summary>The VESSEL panel format against the fixtures the Pi parser and kspsim share.</summary>
    private static void VesselView(string root)
    {
        string data = Path.Combine(root, "hmi-cpp", "tests", "data");
        // Point clouds with interior points and duplicates, in any order: their hulls are the fixture's outlines.
        var nose = new List<double[]> { new[] { 0.0, 20.0 }, new[] { 0.0, 19.0 }, new[] { 1.3, 18.6 }, new[] { -1.3, 18.6 },
                                        new[] { 1.3, 18.6 }, new[] { 0.2, 18.9 } };
        var tank = new List<double[]> { new[] { 1.8, 13.3 }, new[] { -1.8, 9.5 }, new[] { 0.0, 11.0 }, new[] { 1.8, 9.5 },
                                        new[] { -1.8, 13.3 }, new[] { 1.8, 11.0 } };  // (1.8, 11) is collinear: dropped
        var bell = new List<double[]> { new[] { -0.84, 1.8 }, new[] { 0.84, 1.8 }, new[] { 1.6, 0.2 }, new[] { -1.6, 0.2 },
                                        new[] { 0.0, 1.0 } };
        var parts = new List<VesselPartShape> {
            new VesselPartShape { Depth = 0.001, Hull = VesselViewFormat.ConvexHull(nose) },  // rounds to 0.00, not -0
            new VesselPartShape { Depth = 0.25, Hull = VesselViewFormat.ConvexHull(tank) },
            new VesselPartShape { Depth = -0.4, Hull = VesselViewFormat.ConvexHull(bell) } };
        string shape = File.ReadAllText(Path.Combine(data, "vessel_shape_v1.txt")).Replace("\r\n", "\n");
        string state = File.ReadAllText(Path.Combine(data, "vessel_state_v1.txt")).Replace("\r\n", "\n");
        string text = VesselViewFormat.Shape(12, parts);
        Check(text == shape, "vessel shape differs from vessel_shape_v1.txt:\n" + text);
        text = VesselViewFormat.State(12, new List<int[]> { new[] { -1, 30 }, new[] { 48, 20 }, new[] { -1, 93 } });
        Check(text == state, "vessel state differs from vessel_state_v1.txt:\n" + text);
        Check(VesselViewFormat.Shape(3, new List<VesselPartShape>()) == "v=1 rev=3 parts=0\n", "empty vessel shape");
        Check(VesselViewFormat.State(3, new List<int[]>()) == "v=1 rev=3\ns=\n", "empty vessel state");
        // Fuel and heat rules
        Check(VesselViewFormat.IsFuel("LiquidFuel") && VesselViewFormat.IsFuel("XenonGas") &&
              !VesselViewFormat.IsFuel("ElectricCharge") && !VesselViewFormat.IsFuel("Ablator"), "propellants only");
        Check(VesselViewFormat.FuelPercent(0, 0) == -1 && VesselViewFormat.FuelPercent(45, 90) == 50 &&
              VesselViewFormat.FuelPercent(91, 90) == 100, "fuel percent: no tanks, half, clamped");
        Check(VesselViewFormat.HeatPercent(600, 1200, 1800, 2000) == 90 && VesselViewFormat.HeatPercent(5000, 1200, 0, 0) == 100 &&
              VesselViewFormat.HeatPercent(300, 0, 0, 0) == 0, "heat: hotter of internal and skin, clamped, no max");
        // A dense circle keeps at most MaxHullPoints vertices; fewer than three distinct points stay as they are.
        var circle = new List<double[]>();
        for (int i = 0; i < 360; ++i) circle.Add(new[] { Math.Cos(i * Math.PI / 180), Math.Sin(i * Math.PI / 180) });
        Check(VesselViewFormat.ConvexHull(circle).Count == VesselViewFormat.MaxHullPoints, "long hulls are thinned");
        Check(VesselViewFormat.ConvexHull(new List<double[]> { new[] { 1.0, 1.0 }, new[] { 1.0, 1.0 } }).Count == 1,
              "degenerate input");
    }

    /// <summary>The MechJeb report v1 against the fixture the Pi parser and kspsim share.</summary>
    private static void MechJeb(string root)
    {
        string fixture = File.ReadAllText(Path.Combine(root, "hmi-cpp", "tests", "data", "mechjeb_report_v1.txt")).Replace("\r\n", "\n");
        string[] lines = fixture.TrimEnd('\n').Split('\n');
        var values = new List<KeyValuePair<string, string>>();
        for (int i = 1; i < lines.Length; ++i)
        {
            int eq = lines[i].IndexOf('=');
            values.Add(new KeyValuePair<string, string>(lines[i].Substring(0, eq), lines[i].Substring(eq + 1)));
        }
        bool sameKeys = values.Count == MechJebFormat.Keys.Count;
        for (int i = 0; sameKeys && i < values.Count; ++i) sameKeys = values[i].Key == MechJebFormat.Keys[i].Key;
        Check(sameKeys, "MechJeb keys differ from mechjeb_report_v1.txt (names or order)");
        var readOnly = new List<string>();
        foreach (MechJebFormat.KeyInfo k in MechJebFormat.Keys) if (k.ReadOnly) readOnly.Add(k.Key);
        Check(string.Join(",", readOnly.ToArray()) ==
              "ascent.enabled,ascent.status,ascent.t_minus,ascent.classic.auto_turn_start_altitude", "MechJeb read-only keys");
        Check(MechJebFormat.Find("ascent.status").Kind == MechJebFormat.Kind.Text && MechJebFormat.Find("nope") == null,
              "MechJeb find");

        // Every fixture value is what the formatter writes for it (flags, integers and numbers re-formatted).
        bool reformats = true;
        foreach (KeyValuePair<string, string> kv in values)
        {
            MechJebFormat.KeyInfo k = MechJebFormat.Find(kv.Key);
            if (k == null) { reformats = false; continue; }
            double n;
            if (MechJebFormat.Parse(k, kv.Value, out n) != "ok") { reformats = false; Console.WriteLine("  parse " + kv.Key); continue; }
            string again = k.Kind == MechJebFormat.Kind.Text ? kv.Value
                         : k.Kind == MechJebFormat.Kind.Flag ? MechJebFormat.Flag(n != 0) : MechJebFormat.Number(n);
            if (again != kv.Value) { reformats = false; Console.WriteLine("  reformat " + kv.Key + "=" + again); }
        }
        Check(reformats, "MechJeb fixture values round trip through Parse and the formatters");
        values[1] = new KeyValuePair<string, string>("ascent.status", MechJebFormat.Text("Awaiting liftoff"));
        string text = MechJebFormat.Report(42, "ok", "2.15.3.0", values);
        Check(text == fixture, "MechJeb report differs from mechjeb_report_v1.txt:\n" + text);

        Check(MechJebFormat.Report(7, "missing", null, new List<KeyValuePair<string, string>>()) == "v=1 rev=7 mj=missing\n",
              "MechJeb missing report");
        Check(MechJebFormat.Report(8, "error:Bad thing=1", "2.15.3.0", new List<KeyValuePair<string, string>>()) ==
              "v=1 rev=8 mj=error:Bad%20thing%3D1 version=2.15.3.0\n", "MechJeb error text is encoded");

        Check(MechJebFormat.Number(0) == "0" && MechJebFormat.Number(-0.0) == "0", "MechJeb zero and negative zero");
        Check(MechJebFormat.Number(1e-7) == "0" && MechJebFormat.Number(-1e-7) == "0", "MechJeb below 6 decimals");
        Check(MechJebFormat.Number(80000) == "80000" && MechJebFormat.Number(0.4) == "0.4" &&
              MechJebFormat.Number(1.0) == "1" && MechJebFormat.Number(-2.5) == "-2.5", "MechJeb trimmed numbers");
        Check(MechJebFormat.Number(1.0 / 3) == "0.333333" && MechJebFormat.Number(2.0 / 3) == "0.666667", "MechJeb 6 decimals");
        Check(MechJebFormat.Number(double.NaN) == "nan" && MechJebFormat.Number(double.PositiveInfinity) == "nan" &&
              MechJebFormat.Number(double.NegativeInfinity) == "nan", "MechJeb non-finite numbers");
        Check(MechJebFormat.Flag(true) == "1" && MechJebFormat.Flag(false) == "0", "MechJeb flags");
        Check(MechJebFormat.Text("Awaiting liftoff") == "Awaiting%20liftoff" && MechJebFormat.Text("a=b,c%d") == "a%3Db%2Cc%25d" &&
              MechJebFormat.Text("Señal") == "Se%C3%B1al" && MechJebFormat.Text("PVG_1.5-x") == "PVG_1.5-x" &&
              MechJebFormat.Text(null) == "" && MechJebFormat.Text("a\nb") == "a%0Ab", "MechJeb text encoding");

        var flag = MechJebFormat.Find("ascent.autostage");
        var integer = MechJebFormat.Find("ascent.path");
        var number = MechJebFormat.Find("ascent.altitude");
        var status = MechJebFormat.Find("ascent.status");
        double v;
        Check(MechJebFormat.Parse(flag, "1", out v) == "ok" && v == 1 && MechJebFormat.Parse(flag, "true", out v) == "ok" && v == 1 &&
              MechJebFormat.Parse(flag, "False", out v) == "ok" && v == 0 && MechJebFormat.Parse(flag, "0", out v) == "ok" && v == 0,
              "MechJeb flag values");
        Check(MechJebFormat.Parse(flag, "2", out v) == "invalid value" && MechJebFormat.Parse(flag, "", out v) == "invalid value" &&
              MechJebFormat.Parse(flag, "yes", out v) == "invalid value", "MechJeb invalid flags");
        Check(MechJebFormat.Parse(integer, "2", out v) == "ok" && v == 2 && MechJebFormat.Parse(integer, "-3", out v) == "ok" && v == -3 &&
              MechJebFormat.Parse(integer, "2147483647", out v) == "ok", "MechJeb integers");
        Check(MechJebFormat.Parse(integer, "1.5", out v) == "invalid value" &&
              MechJebFormat.Parse(integer, "2147483648", out v) == "invalid value" &&
              MechJebFormat.Parse(integer, "-2147483649", out v) == "invalid value" &&
              MechJebFormat.Parse(integer, "nan", out v) == "invalid value" && MechJebFormat.Parse(integer, "x", out v) == "invalid value",
              "MechJeb invalid integers");
        Check(MechJebFormat.Parse(number, "80000", out v) == "ok" && v == 80000 && MechJebFormat.Parse(number, "-0.5", out v) == "ok" &&
              v == -0.5 && MechJebFormat.Parse(number, "1e3", out v) == "ok" && v == 1000, "MechJeb numbers");
        Check(MechJebFormat.Parse(number, "NaN", out v) == "invalid value" && MechJebFormat.Parse(number, "Infinity", out v) == "invalid value" &&
              MechJebFormat.Parse(number, "1e999", out v) == "invalid value" && MechJebFormat.Parse(number, "1,5", out v) == "invalid value" &&
              MechJebFormat.Parse(number, "", out v) == "invalid value" && MechJebFormat.Parse(number, null, out v) == "invalid value",
              "MechJeb invalid numbers");
        Check(MechJebFormat.Parse(status, "anything", out v) == "ok" && v == 0, "MechJeb text parse");
        Check(MechJebFormat.Actions.Length == 6 && MechJebFormat.Actions[0] == "ascent.engage" &&
              MechJebFormat.Actions[5] == "all.disengage", "MechJeb actions");
    }

    private static int Main(string[] args)
    {
        string root = args.Length > 0 ? args[0] : Path.Combine("..", "..");
        EditorContract(root);
        VesselView(root);
        MechJebBridge(root);
        // A comma decimal separator must not leak into the report.
        Thread.CurrentThread.CurrentCulture = new CultureInfo("es-MX");
        string golden = File.ReadAllText(Path.Combine(root, "hmi-cpp", "tests", "data", "ship_report_v1.txt")).Replace("\r\n", "\n");
        string text = ShipReportFormat.Text(57, ShipReportFormat.Body(Sample()));
        Check(text == golden, "sample report differs from ship_report_v1.txt:\n" + text);

        Check(ShipReportFormat.Escape("Kerbal X") == "Kerbal%20X", "space escaped");
        Check(ShipReportFormat.Escape("a=b,c%d") == "a%3Db%2Cc%25d", "separators escaped");
        Check(ShipReportFormat.Escape("Señal") == "Se%C3%B1al", "UTF-8 bytes escaped");
        Check(ShipReportFormat.Number(double.NaN, 2) == "nan" && ShipReportFormat.Number(double.PositiveInfinity, 0) == "nan",
              "non-finite numbers are nan");
        Check(ShipReportFormat.Number(-0.001, 2) == "0.00", "no negative zero");

        var pending = Sample();
        pending.DeltaVPending = true;
        pending.Funds = double.NaN;  // sandbox / science: no funds field
        string body = ShipReportFormat.Body(pending);
        Check(body.EndsWith("engines=5\ndv=pending\n") && !body.Contains("\nst=") && !body.Contains("funds="),
              "pending report has dv=pending, no stages and no funds:\n" + body);

        // The revision only moves when the contents change, and when the editor is left.
        uint r0 = ShipReportCache.Revision;
        Check(ShipReportCache.Publish(Sample()) && ShipReportCache.Revision == r0 + 1, "first publish");
        Check(!ShipReportCache.Publish(Sample()) && ShipReportCache.Revision == r0 + 1, "identical publish keeps rev");
        Check(ShipReportCache.Report.StartsWith("v=1 rev=" + (r0 + 1) + " facility=vab"), "report carries rev");
        Check(ShipReportCache.Publish(pending) && ShipReportCache.Revision == r0 + 2, "changed publish");
        ShipReportCache.Clear();
        Check(ShipReportCache.Report == "" && ShipReportCache.Revision == r0 + 3, "clear empties and bumps rev");
        Check(ShipReportCache.Publish(Sample()) && ShipReportCache.Revision == r0 + 4, "publish after clear");

        StageEvents();
        Situations();
        KerbinSeaLevel();
        Crew();
        MechJeb(root);

        Console.WriteLine(failures == 0
            ? "ship report format: golden text, escaping, pending, revisions, stage events, situations, Kerbin ASL, crew, vessel view, MechJeb, MechJeb bridge"
            : failures + " failures");
        return failures == 0 ? 0 : 1;
    }
}
