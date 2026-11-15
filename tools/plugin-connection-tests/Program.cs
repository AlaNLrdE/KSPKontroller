// Exercise the built plugin's discovery/connection logic with a test-only serial assembly.
using System;
using System.Collections.Generic;
using System.IO;
using System.Reflection;
using System.Runtime.Loader;
using Psimax.IO.Ports;
class Program
{
    const BindingFlags Flags = BindingFlags.Static | BindingFlags.Public | BindingFlags.NonPublic;
    static Type port, settings;
    static void Set(Type t, string field, object value) { t.GetField(field, Flags).SetValue(null, value); }
    static object Get(string field) { return port.GetField(field, Flags).GetValue(null); }
    static object Call(string method, params object[] args) { return port.GetMethod(method, Flags).Invoke(null, args); }
    static void Check(bool ok, string message) { if (!ok) throw new Exception(message); }
    static void Reset(string configured, int disabled = 1) {
        Call("CloseCurrentLink", -1);
        Set(port, "Port", null);
        Set(port, "PortNumber", "TEST_LAST");
        Set(port, "DisplayFound", false);
        Set(port, "LinkUp", false);
        Set(port, "supGen", 7);
        Set(settings, "DefaultPort", configured);
        Set(settings, "HandshakeDisable", disabled);
        Set(settings, "BaudRate", 115200);
        SerialPort.Constructed.Clear(); SerialPort.Opened.Clear(); SerialPort.Writes.Clear();
        SerialPort.FailPorts.Clear(); SerialPort.ReplyPorts.Clear();
        SerialPort.Names = new[] { "TEST_OTHER", "TEST_LAST" };
        SerialPort.ListCalls = 0;
    }
    static bool Connect(int gen, params string[] candidates) {
        try { return (bool)Call("TryConnect", new List<string>(candidates), 0, 500, gen); }
        finally { Call("CloseCurrentLink", -1); }
    }
    static void Run() {
        foreach (int gen in new[] { -1, 7 }) {
            Reset("TEST_CONFIGURED");
            SerialPort.FailPorts.Add("TEST_CONFIGURED");
            bool connected = Connect(gen, "TEST_CONFIGURED", "TEST_OTHER");
            Check(!connected && !((bool)Get("DisplayFound")), "busy/absent configured port must not fall back without handshake");
            Check(string.Join(",", SerialPort.Constructed) == "TEST_CONFIGURED", "only configured port may be constructed");
        }
        Reset("TEST_CONFIGURED");
        Check(Connect(-1, "TEST_LAST", "TEST_CONFIGURED", "TEST_OTHER"), "configured port must still connect without handshake");
        Check(string.Join(",", SerialPort.Opened) == "TEST_CONFIGURED" && SerialPort.Writes.Count == 0,
              "skip previous/other ports before opening, and send no handshake");
        Reset("TEST_CONFIGURED");
        var listed = (List<string>)Call("BuildCandidates");
        Check(listed.Count == 1 && listed[0] == "TEST_CONFIGURED" && SerialPort.ListCalls == 0,
              "disabled handshake must retry configured port directly without enumeration, even when absent");
        foreach (string empty in new[] { "", "   ", null }) {
            Reset(empty);
            Check(((List<string>)Call("BuildCandidates")).Count == 0, "empty default must produce no candidates");
            Check(!Connect(-1, "TEST_OTHER") && SerialPort.Constructed.Count == 0, "empty default must never open other devices");
        }
        Reset("TEST_CONFIGURED", 0);
        SerialPort.ReplyPorts.Add("TEST_BOARD");
        Check(Connect(-1, "TEST_SILENT", "TEST_BOARD"), "normal discovery must continue until a candidate answers");
        Check(string.Join(",", SerialPort.Opened) == "TEST_SILENT,TEST_BOARD" && SerialPort.Writes.Count == 2,
              "enabled handshake must probe and verify both candidates");
        Reset("TEST_CONFIGURED");
        Check(!Connect(8, "TEST_CONFIGURED") && SerialPort.Constructed.Count == 0, "cancelled supervisor must not open ports");
    }
    static int Main(string[] args) {
        try {
            string root = Path.GetFullPath(args[0]);
            AssemblyLoadContext.Default.Resolving += (_, name) => {
                if (name.Name == "PsimaxSerial") return typeof(SerialPort).Assembly;
                if (name.Name == "Assembly-CSharp") return typeof(KSP.IO.PluginConfiguration).Assembly;
                string path = Path.Combine(root, "KSPSerialIO", "Includes", name.Name + ".dll");
                return File.Exists(path) ? AssemblyLoadContext.Default.LoadFromAssemblyPath(path) : null;
            };
            var assembly = AssemblyLoadContext.Default.LoadFromAssemblyPath(Path.Combine(root,
                "KSPSerialIO", "KSPSerialIO", "bin", "Release", "KSPSerialIO.dll"));
            port = assembly.GetType("KSPSerialIO.KSPSerialPort", true);
            settings = assembly.GetType("KSPSerialIO.SettingsNStuff", true);
            Check(port.GetField("Port", Flags).FieldType == typeof(SerialPort), "serial binding must use the test-only implementation");
            Check(settings.GetField("cfg", Flags).FieldType == typeof(KSP.IO.PluginConfiguration), "settings binding must use the test-only configuration");
            Run();
            Console.WriteLine("PASS plugin connection: configured-only trust, retries, empty configuration, verified discovery, cancellation");
            return 0;
        } catch (Exception e) { Console.Error.WriteLine(e); return 1; }
    }
}
