// Ship report format tests (KSPSerialIO.Hmi ShipReportFormat / ShipReportCache), run by tools/verify.py
// check_ship_report_contract. The expected text is hmi-cpp/tests/data/ship_report_v1.txt, which the Pi's parser
// (hmi-cpp/tests/test_core.cpp) and kspsim's formatter (tools/kspsim/test_kspsim.py) read too.
using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Threading;
using KSPSerialIO.Hmi;

internal static class Program
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
        var s = new StageFacts { Stage = n, DeltaV = dv, DeltaVVac = vac, Twr = twr, Burn = burn };
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

    private static int Main(string[] args)
    {
        string root = args.Length > 0 ? args[0] : Path.Combine("..", "..");
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

        Console.WriteLine(failures == 0 ? "ship report format: golden text, escaping, pending, revisions" : failures + " failures");
        return failures == 0 ? 0 : 1;
    }
}
