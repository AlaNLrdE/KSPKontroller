using System;
using System.Collections.Generic;
using System.Globalization;
using System.Reflection;

namespace KSPSerialIO.Hmi
{
    /// <summary>
    /// Report key -> MechJeb 2.15.3 member (docs/hmi.rst, "MechJeb scene"). Module is the MechJebCore member that holds
    /// the computer module: "Ascent" (property, MechJebModuleAscentBaseAutopilot), "AscentSettings", "Thrust" and
    /// "Staging" (fields). Members lists the names tried in order; the first one the installed MechJeb has is used.
    /// Plain C#: MechJeb is never referenced at compile time.
    /// </summary>
    public static class MechJebMap
    {
        public sealed class Entry
        {
            public string Key;
            public string Module;
            public string[] Members;
        }

        public const string Ascent = "Ascent", Settings = "AscentSettings", Thrust = "Thrust", Staging = "Staging";

        private static Entry E(string key, string module, params string[] members)
        {
            return new Entry { Key = key, Module = module, Members = members };
        }

        public static readonly IList<Entry> Entries = new List<Entry> {
            E("ascent.enabled", Ascent, "Enabled"),
            E("ascent.status", Ascent, "Status"),
            E("ascent.t_minus", Ascent, "TMinus"),
            // The AscentType property setter also disables the other path's modules (DisableAscentModules), the way
            // MechJeb's own path selector does; AscentTypeInteger is its backing field.
            E("ascent.path", Settings, "AscentType", "AscentTypeInteger"),
            E("ascent.altitude", Settings, "DesiredOrbitAltitude"),
            E("ascent.inclination", Settings, "DesiredInclination"),
            E("ascent.lan", Settings, "DesiredLan"),
            E("ascent.launch_phase_angle", Settings, "LaunchPhaseAngle"),
            E("ascent.launch_lan_difference", Settings, "LaunchLANDifference"),
            E("ascent.warp_count_down", Settings, "WarpCountDown"),
            E("ascent.classic.auto", Settings, "AutoPath"),
            E("ascent.classic.auto_turn_start_altitude", Settings, "AutoTurnStartAltitude"),
            E("ascent.classic.turn_start_altitude", Settings, "TurnStartAltitude"),
            E("ascent.classic.turn_start_velocity", Settings, "TurnStartVelocity"),
            E("ascent.classic.turn_end_altitude", Settings, "TurnEndAltitude"),
            E("ascent.classic.turn_end_angle", Settings, "TurnEndAngle"),
            E("ascent.classic.turn_shape_exponent", Settings, "TurnShapeExponent"),
            E("ascent.pvg.desired_apoapsis", Settings, "DesiredApoapsis"),
            E("ascent.pvg.desired_attach_alt", Settings, "DesiredAttachAlt"),
            E("ascent.pvg.desired_fpa", Settings, "DesiredFPA"),
            E("ascent.pvg.pitch_start_velocity", Settings, "PitchStartVelocity"),
            E("ascent.pvg.pitch_rate", Settings, "PitchRate"),
            E("ascent.force_roll", Settings, "ForceRoll"),
            E("ascent.vertical_roll", Settings, "VerticalRoll"),
            E("ascent.turn_roll", Settings, "TurnRoll"),
            E("ascent.roll_altitude", Settings, "RollAltitude"),
            E("ascent.limit_aoa", Settings, "LimitAoA"),
            E("ascent.max_aoa", Settings, "MaxAoA"),
            E("ascent.limit_qa", Settings, "LimitQaEnabled"),
            E("ascent.max_qa", Settings, "LimitQa"),
            E("ascent.corrective_steering", Settings, "CorrectiveSteering"),
            E("ascent.corrective_steering_gain", Settings, "CorrectiveSteeringGain"),
            E("ascent.autostage", Settings, "Autostage"),
            E("ascent.last_stage", Settings, "LastStage"),
            E("ascent.autodeploy_solar_panels", Settings, "AutodeploySolarPanels"),
            E("ascent.autodeploy_antennas", Settings, "AutoDeployAntennas"),
            E("ascent.skip_circularization", Settings, "SkipCircularization"),
            E("thrust.limit_q", Thrust, "LimitDynamicPressure"),
            E("thrust.max_q", Thrust, "MaxDynamicPressure"),
            E("thrust.limit_overheat", Thrust, "LimitToPreventOverheats"),
            E("thrust.limit_acceleration", Thrust, "LimitAcceleration"),
            E("thrust.max_acceleration", Thrust, "MaxAcceleration"),
            E("thrust.limit_throttle", Thrust, "LimitThrottle"),
            E("thrust.max_throttle", Thrust, "MaxThrottle"),
            E("thrust.limit_min_throttle", Thrust, "LimiterMinThrottle"),
            E("thrust.min_throttle", Thrust, "MinThrottle"),
            E("thrust.smooth_throttle", Thrust, "SmoothThrottle"),
            E("thrust.prevent_flameout", Thrust, "LimitToPreventFlameout"),
            E("thrust.flameout_safety_pct", Thrust, "FlameoutSafetyPct"),
            E("thrust.prevent_unstable_ignition", Thrust, "LimitToPreventUnstableIgnition"),
            E("thrust.differential_throttle", Thrust, "DifferentialThrottle"),
            E("staging.autostage_limit", Staging, "AutostageLimit"),
            E("staging.pre_delay", Staging, "AutostagePreDelay"),
            E("staging.post_delay", Staging, "AutostagePostDelay"),
            E("staging.clamp_thrust_pct", Staging, "ClampAutoStageThrustPct"),
            E("staging.hot_staging", Staging, "HotStaging"),
            E("staging.hot_staging_lead_time", Staging, "HotStagingLeadTime"),
            E("staging.drop_solids", Staging, "DropSolids"),
            E("staging.drop_solids_lead_time", Staging, "DropSolidsLeadTime"),
            E("staging.autostaging_once", Staging, "AutostagingOnce"),
            E("staging.fairing_max_q", Staging, "FairingMaxDynamicPressure"),
            E("staging.fairing_min_altitude", Staging, "FairingMinAltitude"),
            E("staging.fairing_max_flux", Staging, "FairingMaxAerothermalFlux"),
        };

        private static Dictionary<string, Entry> byKey;
        private static Dictionary<string, MechJebFormat.KeyInfo> infoByKey;

        public static Entry Find(string key)
        {
            if (byKey == null)
            {
                var d = new Dictionary<string, Entry>();
                foreach (Entry e in Entries) d[e.Key] = e;
                byKey = d;
            }
            Entry found;
            return key != null && byKey.TryGetValue(key, out found) ? found : null;
        }

        public static MechJebFormat.KeyInfo Info(string key)
        {
            if (infoByKey == null)
            {
                var d = new Dictionary<string, MechJebFormat.KeyInfo>();
                foreach (MechJebFormat.KeyInfo k in MechJebFormat.Keys) d[k.Key] = k;
                infoByKey = d;
            }
            MechJebFormat.KeyInfo found;
            return key != null && infoByKey.TryGetValue(key, out found) ? found : null;
        }
    }

    /// <summary>
    /// One MechJeb field or property found by reflection, cached per (type, names). Editable* wrappers (EditableDouble,
    /// EditableDoubleMult, EditableInt: any class with a readable "Val" property) are read and written through .Val,
    /// which holds the SI value (the display multiplier only scales .Text).
    /// </summary>
    public sealed class MechJebMember
    {
        private const BindingFlags Flags = BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic;
        private static readonly Dictionary<string, MechJebMember> Cache = new Dictionary<string, MechJebMember>();

        private FieldInfo field;
        private PropertyInfo property;
        private PropertyInfo val;  // the wrapper's Val, null for plain members

        /// <summary>Declaring type and name, for logs.</summary>
        public string Name { get; private set; }

        /// <summary>The value type read and written: bool, int, double, an enum, string...</summary>
        public Type ValueType { get; private set; }

        public bool CanWrite
        {
            get
            {
                if (val != null) return val.CanWrite && val.GetSetMethod(true) != null;
                if (field != null) return !field.IsInitOnly && !field.IsLiteral;
                return property.CanWrite && property.GetSetMethod(true) != null;
            }
        }

        /// <summary>The first of `names` that `type` has, or null when it has none (the key is then omitted).</summary>
        public static MechJebMember Find(Type type, params string[] names)
        {
            if (type == null || names == null || names.Length == 0) return null;
            string cacheKey = type.AssemblyQualifiedName + "|" + string.Join("|", names);
            lock (Cache)
            {
                MechJebMember m;
                if (Cache.TryGetValue(cacheKey, out m)) return m;
                m = Resolve(type, names);
                Cache[cacheKey] = m;
                return m;
            }
        }

        private static MechJebMember Resolve(Type type, string[] names)
        {
            foreach (string name in names)
            {
                var m = new MechJebMember();
                for (Type t = type; t != null && m.field == null && m.property == null; t = t.BaseType)
                {
                    m.field = t.GetField(name, Flags | BindingFlags.DeclaredOnly);
                    if (m.field == null)
                    {
                        PropertyInfo p = t.GetProperty(name, Flags | BindingFlags.DeclaredOnly);
                        if (p != null && p.CanRead && p.GetIndexParameters().Length == 0) m.property = p;
                    }
                }
                if (m.field == null && m.property == null) continue;
                Type memberType = m.field != null ? m.field.FieldType : m.property.PropertyType;
                m.Name = type.Name + "." + name;
                m.ValueType = memberType;
                if (!memberType.IsPrimitive && !memberType.IsEnum && memberType != typeof(string) && memberType.IsClass)
                {
                    PropertyInfo v = memberType.GetProperty("Val", BindingFlags.Instance | BindingFlags.Public);
                    if (v != null && v.CanRead && v.GetIndexParameters().Length == 0)
                    {
                        m.val = v;
                        m.ValueType = v.PropertyType;
                    }
                }
                return m;
            }
            return null;
        }

        private object Raw(object target)
        {
            return field != null ? field.GetValue(target) : property.GetValue(target, null);
        }

        /// <summary>The member's value, unwrapped from Editable* (null when the wrapper itself is null).</summary>
        public object Read(object target)
        {
            object raw = Raw(target);
            if (val == null || raw == null) return raw;
            return val.GetValue(raw, null);
        }

        /// <summary>Write a parsed report number (flags 0/1) converted to the member's type. Throws when the member is
        /// read only, the Editable* wrapper is null or the value does not fit the type (an undefined enum value).</summary>
        public void Write(object target, double number)
        {
            if (!CanWrite) throw new InvalidOperationException(Name + " is read only");
            object value = Convert(number, ValueType);
            if (val != null)
            {
                object raw = Raw(target);
                if (raw == null) throw new InvalidOperationException(Name + " is null");
                val.SetValue(raw, value, null);
            }
            else if (field != null) field.SetValue(target, value);
            else property.SetValue(target, value, null);
        }

        /// <summary>A report number as `type`: bool (non-zero), an enum (only defined values), int, float, double...</summary>
        public static object Convert(double number, Type type)
        {
            if (type == typeof(bool)) return number != 0;
            if (type.IsEnum)
            {
                object e = Enum.ToObject(type, (int)number);
                if (number != Math.Floor(number) || !Enum.IsDefined(type, e))
                    throw new ArgumentException(number.ToString(CultureInfo.InvariantCulture) + " is not a " + type.Name);
                return e;
            }
            if (type == typeof(int)) return checked((int)number);
            if (type == typeof(double)) return number;
            if (type == typeof(float)) return (float)number;
            return System.Convert.ChangeType(number, type, CultureInfo.InvariantCulture);
        }

        /// <summary>A read value as report text for `kind`, or null when it cannot be expressed (the key is omitted).</summary>
        public static string Format(MechJebFormat.Kind kind, object value)
        {
            if (value == null) return kind == MechJebFormat.Kind.Text ? "" : null;
            switch (kind)
            {
                case MechJebFormat.Kind.Text:
                    return MechJebFormat.Text(System.Convert.ToString(value, CultureInfo.InvariantCulture));
                case MechJebFormat.Kind.Flag:
                    if (value is bool) return MechJebFormat.Flag((bool)value);
                    return MechJebFormat.Flag(ToDouble(value) != 0);
                default:
                    return MechJebFormat.Number(ToDouble(value));
            }
        }

        /// <summary>A number from a primitive, an enum, or any type with an implicit conversion to double (MechJeb's
        /// MovingAverage, EditableDoubleMult). Throws InvalidCastException otherwise.</summary>
        public static double ToDouble(object value)
        {
            if (value == null) throw new InvalidCastException("null is not a number");
            if (value is double) return (double)value;
            if (value is bool) return (bool)value ? 1 : 0;
            Type t = value.GetType();
            if (t.IsEnum) return System.Convert.ToInt64(value, CultureInfo.InvariantCulture);
            if (t.IsPrimitive) return System.Convert.ToDouble(value, CultureInfo.InvariantCulture);
            foreach (MethodInfo m in t.GetMethods(BindingFlags.Static | BindingFlags.Public))
            {
                if (m.Name != "op_Implicit" || m.ReturnType != typeof(double)) continue;
                ParameterInfo[] p = m.GetParameters();
                if (p.Length == 1 && p[0].ParameterType.IsAssignableFrom(t)) return (double)m.Invoke(null, new[] { value });
            }
            throw new InvalidCastException(t.Name + " is not a number");
        }
    }

    /// <summary>Builds the report values from MechJeb's computer modules (looked up by MechJebMap module name).</summary>
    public static class MechJebValues
    {
        /// <summary>One key=value pair per MechJebFormat key, in format order; keys whose module or member is missing,
        /// or whose value cannot be expressed, are omitted (never zero). `missing` (may be null) is told why each
        /// key without a module or member is omitted, on every call.</summary>
        public static List<KeyValuePair<string, string>> Collect(Func<string, object> module, Action<string> missing)
        {
            var values = new List<KeyValuePair<string, string>>(MechJebFormat.Keys.Count);
            foreach (MechJebFormat.KeyInfo info in MechJebFormat.Keys)
            {
                MechJebMap.Entry entry = MechJebMap.Find(info.Key);
                if (entry == null) { if (missing != null) missing(info.Key + ": no MechJeb member mapped"); continue; }
                object target = module(entry.Module);
                if (target == null) { if (missing != null) missing(info.Key + ": no module " + entry.Module); continue; }
                MechJebMember member = MechJebMember.Find(target.GetType(), entry.Members);
                if (member == null)
                {
                    if (missing != null) missing(info.Key + ": " + target.GetType().Name + " has no " + string.Join("/", entry.Members));
                    continue;
                }
                string text = MechJebMember.Format(info.Kind, member.Read(target));
                if (text != null) values.Add(new KeyValuePair<string, string>(info.Key, text));
            }
            return values;
        }
    }

    /// <summary>The MechJeb report the KSPHMIFlight service returns. The revision moves only when the text (without
    /// its revision) changes. Written by MechJebBridge on Unity's main thread.</summary>
    public static class MechJebCache
    {
        private static readonly object Gate = new object();
        private static uint revision;
        private static string text = "";
        private static string last;

        /// <summary>Set after a write or an action, so the bridge refreshes the report on its next frame.</summary>
        public static volatile bool Dirty;

        public static uint Revision { get { lock (Gate) return revision; } }
        public static string Report { get { lock (Gate) return text; } }

        /// <summary>Publish a report; true when its contents changed (and the revision advanced).</summary>
        public static bool Publish(string state, string version, IList<KeyValuePair<string, string>> values)
        {
            string body = MechJebFormat.Report(0, state, version, values);
            lock (Gate)
            {
                if (body == last) return false;
                last = body;
                unchecked { ++revision; }
                text = MechJebFormat.Report(revision, state, version, values);
                return true;
            }
        }
    }
}
