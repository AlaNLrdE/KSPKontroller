// MechJeb bridge tests without KSP or MechJeb: the key -> member map against MechJebFormat.Keys, the reflection
// accessors (Editable* through .Val, bool / int / enum members, read-only properties) on stand-in classes named like
// MechJeb 2.15.3's members, and the report built from them against hmi-cpp/tests/data/mechjeb_report_v1.txt.
using System;
using System.Collections.Generic;
using System.IO;
using KSPSerialIO.Hmi;

namespace FakeMuMech
{
    public class EditableDoubleMult
    {
        private double val;
        public EditableDoubleMult(double v) { val = v; }
        public double Val { get { return val; } set { val = value; } }
        public string Text { get { return (val / 1000).ToString(); } }  // the display multiplier: never reported
    }

    public class EditableDouble : EditableDoubleMult { public EditableDouble(double v) : base(v) { } }

    public class EditableInt
    {
        public EditableInt(int v) { Val = v; }
        public int Val { get; set; }
    }

    /// <summary>Like MechJeb's MovingAverage: only an implicit conversion to double.</summary>
    public class MovingAverage
    {
        private readonly double v;
        public MovingAverage(double v) { this.v = v; }
        public static implicit operator double(MovingAverage m) { return m.v; }
    }

    public enum AscentType { CLASSIC, GRAVITYTURN, PVG }

    public class ComputerModule
    {
        private bool enabled;
        public bool Enabled { get { return enabled; } set { enabled = value; } }
    }

    public class MechJebModuleAscentBaseAutopilot : ComputerModule
    {
        public string Status = "Awaiting liftoff";
        public bool TimedLaunch;  // set by StartCountdown: only then is TMinus a countdown
        public double tMinus = -42;
        public double TMinus { get { return tMinus; } }
    }

    public class MechJebModuleAscentSettings : ComputerModule
    {
        public EditableDouble PitchStartVelocity = new EditableDouble(50), PitchRate = new EditableDouble(0.5);
        public EditableDoubleMult DesiredApoapsis = new EditableDoubleMult(0), DesiredAttachAlt = new EditableDoubleMult(0),
            DesiredFPA = new EditableDoubleMult(0), TurnStartAltitude = new EditableDoubleMult(500),
            TurnStartVelocity = new EditableDoubleMult(50), DesiredOrbitAltitude = new EditableDoubleMult(80000),
            TurnEndAltitude = new EditableDoubleMult(45000), TurnShapeExponent = new EditableDoubleMult(0.4);
        public int AscentTypeInteger;
        public EditableDouble DesiredInclination = new EditableDouble(0), DesiredLan = new EditableDouble(0),
            CorrectiveSteeringGain = new EditableDouble(0.6), VerticalRoll = new EditableDouble(90),
            TurnRoll = new EditableDouble(90), RollAltitude = new EditableDouble(50), MaxAoA = new EditableDouble(5),
            LimitQa = new EditableDouble(2000), LaunchPhaseAngle = new EditableDouble(0),
            LaunchLANDifference = new EditableDouble(0), TurnEndAngle = new EditableDouble(0);
        public bool CorrectiveSteering, ForceRoll = true, AutodeploySolarPanels = true, AutoDeployAntennas = true,
            SkipCircularization, LimitAoA = true, LimitQaEnabled, AutoPath;
        public EditableInt WarpCountDown = new EditableInt(11), LastStage = new EditableInt(0);
        private bool autostage = true;
        public int DisabledModules;
        public AscentType AscentType
        {
            get { return (AscentType)AscentTypeInteger; }
            set { AscentTypeInteger = (int)value; ++DisabledModules; }
        }
        public bool Autostage { get { return autostage; } set { autostage = value; } }
        public double AutoTurnStartAltitude { get { return 1450; } }
        public MovingAverage Longitude = new MovingAverage(-74.5);
    }

    public class MechJebModuleThrustController : ComputerModule
    {
        public bool LimitDynamicPressure = true, LimitToPreventOverheats = true, SmoothThrottle, LimitToPreventFlameout,
            LimitToPreventUnstableIgnition, LimitAcceleration, LimitThrottle, LimiterMinThrottle, DifferentialThrottle;
        public EditableDouble MaxDynamicPressure = new EditableDouble(20000), FlameoutSafetyPct = new EditableDouble(5),  // percent
            MaxAcceleration = new EditableDouble(40);
        public EditableDoubleMult MaxThrottle = new EditableDoubleMult(1), MinThrottle = new EditableDoubleMult(0.05);
    }

    public class MechJebModuleStagingController : ComputerModule
    {
        public EditableDouble AutostagePreDelay = new EditableDouble(0.5), AutostagePostDelay = new EditableDouble(1),
            ClampAutoStageThrustPct = new EditableDouble(0.99), HotStagingLeadTime = new EditableDouble(1),
            DropSolidsLeadTime = new EditableDouble(1);
        public EditableInt AutostageLimit = new EditableInt(0);
        public EditableDoubleMult FairingMaxDynamicPressure = new EditableDoubleMult(50000),
            FairingMinAltitude = new EditableDoubleMult(50000), FairingMaxAerothermalFlux = new EditableDoubleMult(1135);
        public bool HotStaging, DropSolids, AutostagingOnce;
    }

    /// <summary>An older MechJeb without some members.</summary>
    public class OldAutopilot : ComputerModule { }

    /// <summary>TMinus without TimedLaunch: the countdown cannot be told apart from a stale value.</summary>
    public class NoTimedLaunchAutopilot : ComputerModule
    {
        public string Status = "";
        public double TMinus = 12;
    }
}

internal static partial class Program
{
    private static void MechJebBridge(string root)
    {
        // Map completeness: one entry per format key, no extra keys, only the four modules.
        var seen = new HashSet<string>();
        foreach (MechJebMap.Entry e in MechJebMap.Entries)
        {
            Check(seen.Add(e.Key), "mechjeb map: duplicate " + e.Key);
            Check(MechJebFormat.Find(e.Key) != null, "mechjeb map: " + e.Key + " is not a format key");
            Check(e.Module == MechJebMap.Ascent || e.Module == MechJebMap.Settings || e.Module == MechJebMap.Thrust ||
                  e.Module == MechJebMap.Staging, "mechjeb map: module " + e.Module);
            Check(e.Members.Length > 0, "mechjeb map: no member for " + e.Key);
        }
        foreach (MechJebFormat.KeyInfo k in MechJebFormat.Keys)
            Check(MechJebMap.Find(k.Key) != null, "mechjeb map: format key " + k.Key + " has no member");
        Check(MechJebMap.Find("nope") == null && MechJebFormat.Find(null) == null, "mechjeb map: unknown key");

        var ap = new FakeMuMech.MechJebModuleAscentBaseAutopilot { Enabled = true };
        var settings = new FakeMuMech.MechJebModuleAscentSettings();
        var thrust = new FakeMuMech.MechJebModuleThrustController();
        var staging = new FakeMuMech.MechJebModuleStagingController();
        var modules = new Dictionary<string, object> {
            { MechJebMap.Ascent, ap }, { MechJebMap.Settings, settings }, { MechJebMap.Thrust, thrust }, { MechJebMap.Staging, staging } };
        var missing = new List<string>();
        Func<string, object> lookup = m => { object o; return modules.TryGetValue(m, out o) ? o : null; };
        string report = MechJebFormat.Report(42, "ok", "2.15.3.0", MechJebValues.Collect(lookup, missing.Add));
        string golden = File.ReadAllText(Path.Combine(root, "hmi-cpp", "tests", "data", "mechjeb_report_v1.txt")).Replace("\r\n", "\n");
        Check(report == golden, "mechjeb report differs from mechjeb_report_v1.txt:\n" + report);
        Check(missing.Count == 0, "mechjeb: unexpected missing members: " + string.Join("; ", missing));

        // ascent.t_minus only while a timed launch is armed (TimedLaunch), right after ascent.status.
        Check(!report.Contains("ascent.t_minus"), "mechjeb: t_minus omitted without a timed launch");
        ap.TimedLaunch = true;
        ap.tMinus = 12.5;
        report = MechJebFormat.Report(43, "ok", "2.15.3.0", MechJebValues.Collect(lookup, missing.Add));
        Check(report.Contains("\nascent.status=Awaiting%20liftoff\nascent.t_minus=12.5\nascent.path="),
              "mechjeb: t_minus while armed:\n" + report);
        ap.TimedLaunch = false;
        report = MechJebFormat.Report(44, "ok", "2.15.3.0", MechJebValues.Collect(lookup, missing.Add));
        Check(report == golden.Replace("rev=42", "rev=44"), "mechjeb: t_minus gone after abort:\n" + report);
        Check(missing.Count == 0, "mechjeb: armed / disarmed members found");

        // thrust.flameout_safety_pct: MechJeb's percent as a fraction, and back to percent on writes.
        Check(golden.Contains("\nthrust.flameout_safety_pct=0.05\n"), "mechjeb: flameout safety fraction in the fixture");
        MechJebMap.Entry flameout = MechJebMap.Find("thrust.flameout_safety_pct");
        Check(flameout.Divisor == 100 && flameout.ToMember(0.05) == 5 && flameout.ToMember(0.125) == 12.5 &&
              flameout.ToReport(5) == 0.05, "mechjeb: flameout safety percent <-> fraction");
        Check(MechJebMap.Find("thrust.min_throttle").ToMember(0.05) == 0.05 &&
              MechJebMap.Find("thrust.min_throttle").ToReport(0.05) == 0.05, "mechjeb: other keys unscaled");
        thrust.FlameoutSafetyPct.Val = flameout.ToMember(0.1);
        report = MechJebFormat.Report(45, "ok", "2.15.3.0", MechJebValues.Collect(lookup, missing.Add));
        Check(thrust.FlameoutSafetyPct.Val == 10 && report.Contains("\nthrust.flameout_safety_pct=0.1\n"),
              "mechjeb: flameout safety round trip");
        thrust.FlameoutSafetyPct.Val = 5;

        // An autopilot without TimedLaunch: t_minus omitted, with the reason.
        modules[MechJebMap.Ascent] = new FakeMuMech.NoTimedLaunchAutopilot { Enabled = true };
        report = MechJebFormat.Report(46, "ok", null, MechJebValues.Collect(lookup, missing.Add));
        Check(!report.Contains("ascent.t_minus") && missing.Count == 1 && missing[0].Contains("TimedLaunch"),
              "mechjeb: no TimedLaunch member: " + string.Join("; ", missing));

        // Missing members and modules are omitted, never zero.
        modules[MechJebMap.Ascent] = new FakeMuMech.OldAutopilot { Enabled = true };
        modules.Remove(MechJebMap.Staging);
        missing.Clear();
        report = MechJebFormat.Report(1, "ok", null, MechJebValues.Collect(lookup, missing.Add));
        Check(report.Contains("\nascent.enabled=1\n") && !report.Contains("ascent.status") && !report.Contains("ascent.t_minus") &&
              !report.Contains("staging."), "mechjeb: missing members omitted:\n" + report);
        Check(missing.Count == 2 + 12, "mechjeb: missing reasons " + missing.Count);

        // Accessors: Editable* through .Val (SI value, not .Text), bool fields and properties, int wrappers, enums.
        MechJebMember alt = MechJebMember.Find(settings.GetType(), "DesiredOrbitAltitude");
        Check(alt != null && alt.CanWrite && alt.ValueType == typeof(double), "editable double member");
        alt.Write(settings, 120000);
        Check(settings.DesiredOrbitAltitude.Val == 120000, "editable write goes to .Val");
        Check(MechJebMember.Find(settings.GetType(), "DesiredOrbitAltitude") == alt, "members are cached");
        MechJebMember warp = MechJebMember.Find(settings.GetType(), "WarpCountDown");
        Check(warp.ValueType == typeof(int), "editable int member");
        warp.Write(settings, 5);
        Check(settings.WarpCountDown.Val == 5, "editable int write");
        MechJebMember.Find(thrust.GetType(), "LimitThrottle").Write(thrust, 1);
        Check(thrust.LimitThrottle, "bool field write");
        MechJebMember.Find(settings.GetType(), "Autostage").Write(settings, 0);
        Check(!settings.Autostage, "bool property write");
        MechJebMember path = MechJebMember.Find(settings.GetType(), "AscentType", "AscentTypeInteger");
        Check(path.ValueType == typeof(FakeMuMech.AscentType), "ascent path uses the AscentType property");
        path.Write(settings, 2);
        Check(settings.AscentTypeInteger == 2 && settings.DisabledModules == 1, "ascent path write through the setter");
        bool rejected = false;
        try { path.Write(settings, 7); } catch (ArgumentException) { rejected = true; }
        Check(rejected && settings.AscentTypeInteger == 2, "undefined ascent path rejected");
        Check(MechJebMember.Format(MechJebFormat.Kind.Integer, path.Read(settings)) == "2", "enum read as integer");
        Check(!MechJebMember.Find(settings.GetType(), "AutoTurnStartAltitude").CanWrite, "getter-only property is read only");
        Check(MechJebMember.Find(ap.GetType(), "Enabled").CanWrite, "inherited property found");
        Check(MechJebMember.Find(settings.GetType(), "NoSuchMember") == null, "missing member");
        Check(MechJebMember.ToDouble(settings.Longitude) == -74.5 && MechJebMember.ToDouble(3) == 3 &&
              MechJebMember.ToDouble(FakeMuMech.AscentType.PVG) == 2, "numbers from implicit conversions, ints and enums");
        Check(MechJebMember.Format(MechJebFormat.Kind.Text, "a b") == "a%20b" &&
              MechJebMember.Format(MechJebFormat.Kind.Text, null) == "" &&
              MechJebMember.Format(MechJebFormat.Kind.Number, null) == null, "text and null values");

        // The revision moves only when the text changes.
        uint r0 = MechJebCache.Revision;
        Check(MechJebCache.Publish("missing", null, null) && MechJebCache.Revision == r0 + 1, "mechjeb cache: first publish");
        Check(!MechJebCache.Publish("missing", null, null) && MechJebCache.Revision == r0 + 1, "mechjeb cache: same text keeps rev");
        Check(MechJebCache.Report == "v=1 rev=" + (r0 + 1) + " mj=missing\n", "mechjeb cache: report " + MechJebCache.Report);
        Check(MechJebCache.Publish("nocore", "2.15.3.0", null) && MechJebCache.Revision == r0 + 2, "mechjeb cache: change");
    }
}
