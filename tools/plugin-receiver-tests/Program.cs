// Run the real plugin assembly's receive path without a Unity scene or serial hardware.
using System;
using System.IO;
using System.Reflection;
using System.Runtime.Loader;

class Program
{
    static Type port;
    const BindingFlags Fields = BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Static;
    static object Get(string name) { return port.GetField(name, Fields).GetValue(null); }
    static void Set(string name, object value) { port.GetField(name, Fields).SetValue(null, value); }
    static object Call(string name, params object[] args) { return port.GetMethod(name, Fields).Invoke(null, args); }
    static void Check(bool value, string text) { if (!value) throw new Exception(text); }
    static byte[] Frame(byte[] payload)
    {
        var frame = new byte[payload.Length + 4];
        frame[0] = 0xBE; frame[1] = 0xEF; frame[2] = (byte)payload.Length;
        byte checksum = (byte)payload.Length;
        for (int i = 0; i < payload.Length; ++i) { frame[i + 3] = payload[i]; checksum ^= payload[i]; }
        frame[frame.Length - 1] = checksum;
        return frame;
    }
    static void Feed(byte[] payload)
    {
        var bytes = Frame(payload);
        Call("ReceivedDataEvent", bytes, bytes.Length);
    }
    static void Dispatch() { if ((bool)Get("NewPacketFlag")) Call("InboundPacketHandler"); }
    static bool Abort()
    {
        object controls = Get("VControls");
        return (bool)controls.GetType().GetField("Abort").GetValue(controls);
    }
    static int PendingButtons()
    {
        object buttons = Get("Buttons");
        return ((Array)buttons.GetType().GetMethod("Take").Invoke(buttons, null)).Length;
    }
    static void Main(string[] args)
    {
        string root = Path.GetFullPath(args[0]);
        string includes = Path.Combine(root, "KSPSerialIO", "Includes");
        AssemblyLoadContext.Default.Resolving += (_, name) => {
            string path = Path.Combine(includes, name.Name + ".dll");
            return File.Exists(path) ? AssemblyLoadContext.Default.LoadFromAssemblyPath(path) : null;
        };
        var assembly = AssemblyLoadContext.Default.LoadFromAssemblyPath(Path.Combine(root,
            "KSPSerialIO", "KSPSerialIO", "bin", "Release", "KSPSerialIO.dll"));
        port = assembly.GetType("KSPSerialIO.KSPSerialPort", true);
        object controls = Get("VControls");
        controls.GetType().GetField("ControlGroup").SetValue(controls, new bool[11]);
        Set("VControls", controls);
        Feed(new byte[] { 0, 3, 1, 4 });
        Check((bool)Get("DisplayFound"), "valid handshake must establish the connection");
        Feed(new byte[] { 101 });
        Dispatch();
        Check(!Abort(), "short controls must not reuse handshake bytes to activate Abort");
        Check(!((bool)Get("ControlReceived")), "short controls must not publish controls");
        // A malformed handshake must not be accepted using the previous valid reply's residual magic.
        Set("DisplayFound", false);
        foreach (int size in new int[] { 1, 3, 5, 255 }) {
            var payload = new byte[size];
            if (size > 1) payload[1] = 3;
            if (size > 2) payload[2] = 1;
            if (size > 3) payload[3] = 4;
            Feed(payload);
            Check(!((bool)Get("DisplayFound")) && !((bool)Get("NewPacketFlag")),
                "wrong-size handshake must not establish connection or publish a packet");
        }
        Feed(new byte[] { 0, 3, 1, 4 });
        Check((bool)Get("DisplayFound"), "valid handshake must still work after rejected sizes");
        var good = new byte[25];
        good[0] = 101;
        Feed(good);
        Dispatch();
        Set("ControlReceived", false);
        Check(PendingButtons() == 0, "first valid controls must seed buttons");
        foreach (int size in new int[] { 1, 24, 26, 255 }) {
            var invalid = new byte[size];
            invalid[0] = 101;
            if (size > 1) invalid[1] = 0xC3; // SAS/RCS/STAGE plus Abort must not escape length validation
            var before = (byte[])((byte[])Get("NewPacketBuffer")).Clone();
            Feed(invalid);
            Check(!((bool)Get("NewPacketFlag")), "wrong-size controls must not publish a pending packet");
            Dispatch();
            Check(!Abort() && !((bool)Get("ControlReceived")) && PendingButtons() == 0,
                "wrong-size controls must not change controls or capture presses");
            Check(Convert.ToHexString(before) == Convert.ToHexString((byte[])Get("NewPacketBuffer")),
                "wrong-size controls must leave the shared buffer untouched");
        }
        good[1] = 0xC1; // one valid stage, SAS and RCS press
        Feed(good);
        Check((bool)Get("NewPacketFlag"), "valid controls must publish after invalid packets");
        var pending = (byte[])((byte[])Get("NewPacketBuffer")).Clone();
        Feed(new byte[] { 101 });
        Feed(new byte[] { 0, 3, 1, 4, 99 });
        Check((bool)Get("NewPacketFlag") && Convert.ToHexString(pending) == Convert.ToHexString((byte[])Get("NewPacketBuffer")),
            "malformed packets must not overwrite a pending valid packet");
        Dispatch();
        Check((bool)Get("ControlReceived") && !Abort() && PendingButtons() == 3,
            "valid controls and their presses must survive malformed following packets");
        Check((bool)Get("DisplayFound"), "malformed handshake must not disconnect an established session");
        Console.WriteLine("PASS plugin receiver: real assembly rejects short/long controls and handshakes before publication");
    }
}
