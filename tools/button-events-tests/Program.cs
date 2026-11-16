using System;
using System.Threading;
using KSPSerialIO;

class Program
{
    static void Check(bool value, string name)
    {
        if (!value) throw new Exception(name);
    }

    static void Main()
    {
        var buttons = new ButtonEvents();
        buttons.Capture(193, 10); // held STAGE and retained SAS/RCS bits only seed the baseline
        Check(buttons.Take().Length == 0, "startup must not create presses");
        buttons.Capture(192, 20);
        buttons.Capture(193, 30);
        buttons.Capture(192, 180); // complete 150 ms pulse before the main thread consumes anything
        var presses = buttons.Take();
        Check(presses.Length == 1 && presses[0].Bit == 0 && presses[0].Received == 30,
            "stage pulse must survive release and keep its actual receipt timestamp");
        Check(buttons.Take().Length == 0, "a press is consumed once");
        buttons.Capture(0, 200);
        buttons.Capture(192, 400); // two presses of both toggle buttons return bits to their initial state
        presses = buttons.Take();
        Check(presses.Length == 4 && presses[0].Bit == 6 && presses[1].Bit == 7 &&
            presses[2].Bit == 6 && presses[3].Bit == 7, "both SAS/RCS transitions must survive");
        buttons.Capture(193, 500);
        buttons.Capture(193, 600);
        Check(buttons.Take().Length == 1, "held stage must not repeat");
        buttons.Capture(192, 700);
        buttons.Capture(193, 800);
        buttons.Reset();
        buttons.Capture(193, 900);
        Check(buttons.Take().Length == 0, "reconnect drops pending presses and reseeds held buttons");
        buttons.Capture(192, int.MaxValue - 10);
        buttons.Capture(193, int.MaxValue - 5);
        presses = buttons.Take();
        Check(unchecked((int.MinValue + 5) - presses[0].Received) == 11,
            "receipt timestamp must support TickCount wraparound");
        buttons.Reset();
        buttons.Capture(0, 0);
        const int count = 2000;
        var producer = new Thread(() => {
            for (int i = 1; i <= count; ++i) buttons.Capture((byte)((i % 2) * 128), i);
        });
        producer.Start();
        int consumed = 0;
        while (producer.IsAlive) consumed += buttons.Take().Length;
        producer.Join();
        consumed += buttons.Take().Length;
        Check(consumed == count, "concurrent consumption must preserve every edge");
        var steps = new System.Collections.Generic.List<int>();
        Action<int> navigate = value => steps.Add(value);
        EditorNavigation.StepRequested += navigate;
        EditorNavigation.Consume(new[] {
            new ButtonEvents.Press { Bit = 7, Received = 100 },
            new ButtonEvents.Press { Bit = 6, Received = 200 },
            new ButtonEvents.Press { Bit = 0, Received = 300 },
            new ButtonEvents.Press { Bit = 7, Received = -1000 }
        }, 500);
        Check(steps.Count == 2 && steps[0] == 1 && steps[1] == -1, "editor routes SAS/RCS only and drops stale presses");
        EditorNavigation.Consume(new[] { new ButtonEvents.Press { Bit = 7, Received = int.MaxValue - 5 } }, int.MinValue + 5);
        Check(steps.Count == 3, "editor navigation survives TickCount rollover");
        EditorNavigation.StepRequested -= navigate;
        // HMI keyboard axes: clamped, expire after the timeout, and never override a deflected board axis
        HmiKeyboard.SetAxes(100f, -1f, 0f, 2f, 0f, float.NaN, 0.5f);
        Check(HmiKeyboard.Axis(HmiKeyboard.Pitch, 100.2f) == -1f && HmiKeyboard.Axis(HmiKeyboard.Roll, 100.2f) == 1f &&
            HmiKeyboard.Axis(HmiKeyboard.Y, 100.2f) == 0f && HmiKeyboard.Axis(HmiKeyboard.Z, 100.2f) == 0.5f,
            "keyboard axes are clamped to -1..1 and NaN reads as zero");
        Check(HmiKeyboard.Axis(HmiKeyboard.Pitch, 100f + HmiKeyboard.Timeout + 0.01f) == 0f, "stale keyboard axes read as zero");
        Check(HmiKeyboard.Axis(HmiKeyboard.Pitch, 99f) == 0f, "keyboard axes from the future read as zero");
        Check(HmiKeyboard.Merge(0.3f, true, -1f) == 0.3f, "a deflected board axis wins over a held key");
        Check(HmiKeyboard.Merge(0.3f, false, -1f) == -1f, "a held key sets an axis the board leaves neutral");
        Check(HmiKeyboard.Merge(0.3f, false, 0f) == 0.3f, "no key leaves the axis as it was");
        HmiKeyboard.Clear();
        Check(HmiKeyboard.Axis(HmiKeyboard.Pitch, 100.1f) == 0f, "Clear releases every axis");
        Console.WriteLine("PASS button events: stage pulse, SAS/RCS transitions, baseline, timestamps, reset, concurrency, HMI keyboard axes");
    }
}
