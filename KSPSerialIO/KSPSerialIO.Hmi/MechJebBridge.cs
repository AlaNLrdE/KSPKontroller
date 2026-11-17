using System;
using System.Collections.Generic;
using System.Reflection;
using UnityEngine;

namespace KSPSerialIO.Hmi
{
    /// <summary>
    /// The HMI's MechJeb scene (docs/hmi.rst, "MechJeb scene"): reads and changes the active vessel's MechJeb by
    /// reflection, so MechJeb stays an optional mod (no compile-time reference). The type MuMech.MechJebCore is found
    /// in AssemblyLoader.loadedAssemblies; the vessel's master core comes from MuMech.VesselExtensions.GetMasterMechJeb
    /// (the same core MechJeb's own windows drive), else the first MechJebCore part module and its MasterMechJeb.
    /// Twice a second (and on the frame after a write or an action) the report is rebuilt into MechJebCache on Unity's
    /// main thread; KSPHMIFlight.MechJebSet / MechJebAction run on that thread too (kRPC calls procedures there).
    /// </summary>
    [KSPAddon(KSPAddon.Startup.Flight, false)]
    public class MechJebBridge : MonoBehaviour
    {
        private const string Tag = "KSPSerialIO[HMI] ";
        private const BindingFlags Any = BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Instance | BindingFlags.Static;

        private float nextPoll;
        private string lastState = "";
        private static readonly HashSet<string> Logged = new HashSet<string>();

        // Resolved once per game session: MechJeb's assemblies are loaded before the first flight scene.
        private static bool resolved;
        private static Type coreType;
        private static MethodInfo getMaster;
        private static Type astroType;
        private static string version;
        private static readonly Dictionary<string, MethodInfo> Methods = new Dictionary<string, MethodInfo>();

        public void Update()
        {
            if (Time.unscaledTime < nextPoll && !MechJebCache.Dirty) return;
            nextPoll = Time.unscaledTime + 0.5f;
            MechJebCache.Dirty = false;
            string state;
            try
            {
                object core;
                state = Availability(out core);
                if (core == null) MechJebCache.Publish(state, version, null);
                else MechJebCache.Publish(state, version, MechJebValues.Collect(m => Module(core, m), LogOnce));
            }
            catch (Exception e)
            {
                Exception inner = e is TargetInvocationException && e.InnerException != null ? e.InnerException : e;
                state = "error:" + inner.Message;  // MechJebFormat.Report percent-encodes the text
                MechJebCache.Publish(state, version, null);
                LogOnce("report failed: " + inner);
            }
            if (state != lastState)
            {
                Debug.Log(Tag + "mechjeb: " + state + (version != null ? " (MechJeb " + version + ")" : "") +
                          ", report rev " + MechJebCache.Revision);
                lastState = state;
            }
        }

        private static void LogOnce(string what)
        {
            if (Logged.Add(what)) Debug.Log(Tag + "mechjeb: " + what);
        }

        private static void Resolve()
        {
            if (resolved) return;
            resolved = true;
            foreach (AssemblyLoader.LoadedAssembly a in AssemblyLoader.loadedAssemblies)
            {
                if (a.assembly == null) continue;
                Type t = a.assembly.GetType("MuMech.MechJebCore", false);
                if (t != null && coreType == null)
                {
                    coreType = t;
                    version = a.assembly.GetName().Version.ToString();
                    Type ext = a.assembly.GetType("MuMech.VesselExtensions", false);
                    if (ext != null)
                        getMaster = ext.GetMethod("GetMasterMechJeb", BindingFlags.Public | BindingFlags.Static, null,
                                                  new[] { typeof(Vessel) }, null);
                }
                Type astro = a.assembly.GetType("MechJebLib.Functions.Astro", false);
                if (astro != null && astroType == null) astroType = astro;
            }
            Debug.Log(Tag + "mechjeb: " + (coreType == null ? "not installed" :
                      "MechJeb " + version + (getMaster == null ? ", no GetMasterMechJeb (part module fallback)" : "")));
        }

        /// <summary>"ok" with the active vessel's master core, else "missing" / "nocore" and null.</summary>
        private static string Availability(out object core)
        {
            Resolve();
            core = null;
            if (coreType == null) return "missing";
            core = Core(FlightGlobals.ActiveVessel);
            return core != null ? "ok" : "nocore";
        }

        private static object Core(Vessel v)
        {
            if (v == null) return null;
            if (getMaster != null)
            {
                object master = getMaster.Invoke(null, new object[] { v });
                if (master != null) return master;
            }
            foreach (Part p in v.parts)
                foreach (PartModule m in p.Modules)
                    if (coreType.IsInstanceOfType(m))
                    {
                        object master = Get(m, "MasterMechJeb", false);
                        return master ?? m;
                    }
            return null;
        }

        /// <summary>A computer module by MechJebMap module name ("Ascent", "AscentSettings", "Thrust", "Staging").</summary>
        private static object Module(object core, string name)
        {
            return Get(core, name, false);
        }

        /// <summary>A field or property (Editable* unwrapped to .Val). Missing: null, or MissingMemberException.</summary>
        private static object Get(object target, string name, bool required = true)
        {
            MechJebMember m = target != null ? MechJebMember.Find(target.GetType(), name) : null;
            if (m == null)
            {
                if (required) throw new MissingMemberException(target == null ? "null" : target.GetType().Name, name);
                return null;
            }
            return m.Read(target);
        }

        private static void Set(object target, string name, double number)
        {
            MechJebMember m = MechJebMember.Find(target.GetType(), name);
            if (m == null) throw new MissingMemberException(target.GetType().Name, name);
            m.Write(target, number);
        }

        /// <summary>Invoke a method found by name and argument count (cached per type).</summary>
        private static object Call(object target, Type type, string name, params object[] args)
        {
            string key = type.AssemblyQualifiedName + "." + name + "/" + args.Length;
            MethodInfo method;
            if (!Methods.TryGetValue(key, out method))
            {
                for (Type t = type; t != null && method == null; t = t.BaseType)
                    foreach (MethodInfo mi in t.GetMethods(Any | BindingFlags.DeclaredOnly))
                        if (mi.Name == name && mi.GetParameters().Length == args.Length) { method = mi; break; }
                Methods[key] = method;
            }
            if (method == null) throw new MissingMethodException(type.Name, name);
            return method.Invoke(method.IsStatic ? null : target, args);
        }

        private static object Call(object target, string name, params object[] args)
        {
            return Call(target, target.GetType(), name, args);
        }

        /// <summary>KSPHMIFlight.MechJebSet: ok, unknown key, read only, invalid value or unavailable.</summary>
        public static string SetValue(string key, string value)
        {
            MechJebFormat.KeyInfo info = MechJebFormat.Find(key);
            MechJebMap.Entry entry = MechJebMap.Find(key);
            if (info == null || entry == null) return "unknown key";
            if (info.ReadOnly) return "read only";
            double number;
            string parsed = MechJebFormat.Parse(info, value, out number);
            if (parsed != "ok") return parsed;
            try
            {
                object core;
                if (Availability(out core) != "ok") return "unavailable";
                object module = Module(core, entry.Module);
                MechJebMember member = module != null ? MechJebMember.Find(module.GetType(), entry.Members) : null;
                if (member == null) return "unavailable";
                if (!member.CanWrite) return "read only";
                try { MechJebMember.Convert(number, member.ValueType); }
                catch (ArgumentException) { return "invalid value"; }
                catch (OverflowException) { return "invalid value"; }
                member.Write(module, number);
                MechJebCache.Dirty = true;
                return "ok";
            }
            catch (Exception e)
            {
                LogOnce("set " + key + " failed: " + (e.InnerException ?? e).Message);
                return "unavailable";
            }
        }

        /// <summary>KSPHMIFlight.MechJebAction, mirroring MechJeb 2.15.3's Ascent Guidance window
        /// (MechJebModuleAscentMenu): Engage / Disengage add or remove that window as a user of the ascent autopilot,
        /// and the launch buttons set AscentSettings.LaunchingTo* and call StartCountdown with the same times.</summary>
        public static string Act(string action)
        {
            if (action == null || Array.IndexOf(MechJebFormat.Actions, action) < 0) return "unknown key";
            try
            {
                object core;
                if (Availability(out core) != "ok") return "unavailable";
                object ap = Module(core, MechJebMap.Ascent);
                object settings = Module(core, MechJebMap.Settings);
                object menu = Call(core, "GetComputerModule", "MechJebModuleAscentMenu");
                if (ap == null || settings == null) return "unavailable";
                string result;
                switch (action)
                {
                    case "ascent.engage":
                        if (menu == null) return "unavailable";
                        if (!(bool)Get(ap, "Enabled")) Call(Get(ap, "Users"), "Add", menu);
                        result = "ok";
                        break;
                    case "ascent.disengage":
                    case "all.disengage":  // the ascent autopilot is the only one this bridge drives (v1)
                        Disengage(ap, menu);
                        result = "ok";
                        break;
                    case "ascent.launch_rendezvous":
                        result = LaunchRendezvous(core, ap, settings, menu);
                        break;
                    case "ascent.launch_target_plane":
                        result = LaunchTargetPlane(core, ap, settings);
                        break;
                    default:  // ascent.abort_timed_launch
                        foreach (string flag in new[] { "LaunchingToLan", "LaunchingToMatchLan", "LaunchingToRendezvous", "LaunchingToPlane" })
                            Set(settings, flag, 0);
                        Set(ap, "TimedLaunch", 0);
                        result = "ok";
                        break;
                }
                MechJebCache.Dirty = true;
                return result;
            }
            catch (Exception e)
            {
                LogOnce("action " + action + " failed: " + (e.InnerException ?? e).Message);
                return "unavailable";
            }
        }

        /// <summary>Remove the Ascent Guidance window as a user; if another user keeps the autopilot on (another mod),
        /// clear its users so it really disengages.</summary>
        private static void Disengage(object ap, object menu)
        {
            object users = Get(ap, "Users");
            if (menu != null) Call(users, "Remove", menu);
            if ((bool)Get(ap, "Enabled")) Call(users, "Clear");
        }

        /// <summary>Timed launches are offered only on the ground with a target, as in MechJeb's window.</summary>
        private static bool CanTimeLaunch(object core)
        {
            Vessel v = FlightGlobals.ActiveVessel;
            object target = Get(core, "Target");
            return v != null && v.LandedOrSplashed && target != null && (bool)Get(target, "NormalTargetExists");
        }

        private static string LaunchRendezvous(object core, object ap, object settings, object menu)
        {
            // MechJeb hides "Launch to rendezvous" for the GravityTurn path (AscentType 1).
            if (menu == null || !CanTimeLaunch(core) || MechJebMember.ToDouble(Get(settings, "AscentType")) == 1)
                return "unavailable";
            object state = Get(ap, "VesselState");
            object orbit = Get(Get(core, "Target"), "TargetOrbit");
            double wait = (double)Call(menu, "TimeToPhaseAngle", MechJebMember.ToDouble(Get(settings, "LaunchPhaseAngle")),
                                       Get(ap, "MainBody"), MechJebMember.ToDouble(Get(state, "longitude")), orbit);
            Set(settings, "LaunchingToRendezvous", 1);
            Call(ap, "StartCountdown", MechJebMember.ToDouble(Get(state, "time")) + wait);
            return "ok";
        }

        private static string LaunchTargetPlane(object core, object ap, object settings)
        {
            if (astroType == null || !CanTimeLaunch(core)) return "unavailable";
            object state = Get(ap, "VesselState");
            var body = (CelestialBody)Get(ap, "MainBody");
            var orbit = (Orbit)Get(Get(core, "Target"), "TargetOrbit");
            object tuple = Call(null, astroType, "MinimumTimeToPlane", body.rotationPeriod,
                                MechJebMember.ToDouble(Get(state, "latitude")),
                                MechJebMember.ToDouble(Get(state, "celestialLongitude")),
                                orbit.LAN - MechJebMember.ToDouble(Get(settings, "LaunchLANDifference")), orbit.inclination);
            double wait = MechJebMember.ToDouble(tuple.GetType().GetField("Item1").GetValue(tuple));
            double inclination = MechJebMember.ToDouble(tuple.GetType().GetField("Item2").GetValue(tuple));
            Set(settings, "LaunchingToPlane", 1);
            Call(ap, "StartCountdown", MechJebMember.ToDouble(Get(state, "time")) + wait);
            Set(settings, "DesiredInclination", inclination);
            return "ok";
        }
    }
}
