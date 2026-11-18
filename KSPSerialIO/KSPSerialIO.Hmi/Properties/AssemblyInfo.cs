using System.Reflection;
using System.Runtime.InteropServices;

// KSPSerialIO.Hmi: the editor ship report for the Raspberry Pi HMI, served by kRPC (service KSPHMI).
// A separate assembly so that KSPSerialIO.dll never depends on kRPC: without kRPC only this DLL fails to load.
// Its version always equals KSPSerialIO's (KSPSerialIO/KSPSerialIO/Properties/AssemblyInfo.cs; ./ci verify checks).
[assembly: AssemblyTitle("KSPSerialIO.Hmi")]
[assembly: AssemblyDescription("Editor ship report for the KSPKontroller Raspberry Pi HMI (kRPC service KSPHMI)")]
[assembly: AssemblyProduct("KSPSerialIO")]
[assembly: AssemblyCulture("")]
[assembly: ComVisible(false)]
[assembly: Guid("6a1c3e0f-5d2b-4c47-9a35-0e7f2b8d4c19")]
[assembly: AssemblyVersion("0.19.8.1")]
[assembly: AssemblyFileVersion("0.19.8.1")]
// KSPAssembly lets other mods depend on this one. There is deliberately no KSPAssemblyDependency on kRPC: kRPC 0.7
// declares no KSPAssembly attribute, so KSP could refuse to load this DLL even with kRPC installed.
[assembly: KSPAssembly("KSPSerialIO.Hmi", 0, 19)]
