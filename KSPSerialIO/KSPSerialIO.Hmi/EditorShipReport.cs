using System;
using System.Collections.Generic;
using KSP.UI.Screens;
using UnityEngine;

namespace KSPSerialIO.Hmi
{
    /// <summary>
    /// Builds the editor ship report on Unity's main thread whenever the ship changes (onEditorShipModified, a craft
    /// loaded, a new craft) and whenever KSP finishes a delta-v calculation (onDeltaVCalcsCompleted) or the editor
    /// delta-v app changes its situation, and publishes it to ShipReportCache. Nothing here references kRPC: the
    /// kRPC service (HmiService) only reads the cache, so RPCs never touch KSP objects.
    /// </summary>
    [KSPAddon(KSPAddon.Startup.EditorAny, false)]
    public class EditorShipReport : MonoBehaviour
    {
        private const string Tag = "KSPSerialIO[HMI] ";
        private const double G0 = 9.80665;
        /// <summary>Situation chosen in the editor delta-v app ("asl", "alt", "vac"), "" until it reports one.</summary>
        private string situationOption = "";
        private string lastError = "";
        /// <summary>Last diagnostic line (staging list vs part stage, Kerbin ASL check): logged once per change.</summary>
        private string lastDiagnostic = "";
        private readonly List<string> diagnostics = new List<string>();

        /// <summary>Unity: subscribe to the editor and delta-v events and build the first report.</summary>
        public void Start()
        {
            GameEvents.onEditorShipModified.Add(OnShipModified);
            GameEvents.onEditorLoad.Add(OnEditorLoad);
            GameEvents.onEditorStarted.Add(Rebuild);
            GameEvents.onEditorRestart.Add(Rebuild);
            GameEvents.onDeltaVCalcsCompleted.Add(Rebuild);
            GameEvents.onDeltaVAppAtmosphereChanged.Add(OnSituation);
            Rebuild();
        }

        /// <summary>Unity: unsubscribe and forget the ship (leaving the editor).</summary>
        public void OnDestroy()
        {
            GameEvents.onEditorShipModified.Remove(OnShipModified);
            GameEvents.onEditorLoad.Remove(OnEditorLoad);
            GameEvents.onEditorStarted.Remove(Rebuild);
            GameEvents.onEditorRestart.Remove(Rebuild);
            GameEvents.onDeltaVCalcsCompleted.Remove(Rebuild);
            GameEvents.onDeltaVAppAtmosphereChanged.Remove(OnSituation);
            ShipReportCache.Clear();
        }

        private void OnShipModified(ShipConstruct ship) { Rebuild(); }

        private void OnEditorLoad(ShipConstruct ship, CraftBrowserDialog.LoadType type) { Rebuild(); }

        // The enum's member names are not part of any documented contract: match on their text.
        private void OnSituation(DeltaVSituationOptions option)
        {
            situationOption = SituationCode(option.ToString());
            Rebuild();
        }

        /// <summary>"asl", "alt" or "vac" from a DeltaVSituationOptions name, "" if unknown.</summary>
        public static string SituationCode(string option)
        {
            string o = (option ?? "").ToLowerInvariant();
            if (o.Contains("sea")) return "asl";
            if (o.Contains("vac")) return "vac";
            if (o.Contains("alt")) return "alt";
            return "";
        }

        private void Rebuild()
        {
            try
            {
                ShipConstruct ship = EditorLogic.fetch != null ? EditorLogic.fetch.ship : null;
                if (ship == null) return;
                diagnostics.Clear();
                ShipFacts facts = Collect(ship);
                if (ShipReportCache.Publish(facts))
                    Debug.Log(Tag + "ship report rev " + ShipReportCache.Revision + ": " + facts.Parts + " parts, " +
                              (facts.DeltaVPending ? "delta-v pending" : facts.Stages.Count + " stages") + ", " + facts.Situation);
                string diagnostic = string.Join("; ", diagnostics.ToArray());
                if (diagnostic.Length > 0 && diagnostic != lastDiagnostic) Debug.Log(Tag + "ship report check: " + diagnostic);
                lastDiagnostic = diagnostic;
                lastError = "";
            }
            catch (Exception e)
            {
                // Once per distinct error: a broken report must never spam the log or break the editor.
                if (e.Message != lastError) Debug.Log(Tag + "ship report failed: " + e);
                lastError = e.Message;
            }
        }

        // What a part fires when staged, from its modules; its staging icon when the modules do not tell.
        private static string Kind(Part p)
        {
            if (p.FindModulesImplementing<ModuleEngines>().Count > 0) return "engines";
            if (p.FindModulesImplementing<LaunchClamp>().Count > 0) return "clamps";
            if (p.FindModulesImplementing<ModuleDecouple>().Count > 0 ||
                p.FindModulesImplementing<ModuleAnchoredDecoupler>().Count > 0) return "decouplers";
            if (p.FindModulesImplementing<ModuleParachute>().Count > 0) return "chutes";
            if (p.FindModulesImplementing<ModuleProceduralFairing>().Count > 0) return "fairings";
            return StageTally.KindFromIcon(p.stagingIcon);
        }

        // The stages as KSP's staging list shows them (StageManager: one group per stage, one icon per staged part or
        // per symmetry group). Parts that are not in the list do not fire anything. Without a staging list (it is
        // built a frame after a change) every part counts in its inverseStage, as before.
        private StageTally Tally(ShipConstruct ship)
        {
            var tally = new StageTally();
            StageManager manager = StageManager.Instance;
            var stageOf = new Dictionary<Part, int>();
            if (manager != null && manager.Stages != null)
            {
                foreach (StageGroup group in manager.Stages)
                    if (group != null && group.Icons != null)
                        foreach (StageIcon icon in group.Icons)
                            if (icon != null && icon.Part != null && !stageOf.ContainsKey(icon.Part))
                                stageOf[icon.Part] = group.inverseStageIndex;
                // grouped symmetry icons stand for their counterparts too
                foreach (StageGroup group in manager.Stages)
                    if (group != null && group.Icons != null)
                        foreach (StageIcon icon in group.Icons)
                            if (icon != null && icon.Part != null && icon.Part.symmetryCounterparts != null)
                                foreach (Part cp in icon.Part.symmetryCounterparts)
                                    if (cp != null && !stageOf.ContainsKey(cp)) stageOf[cp] = group.inverseStageIndex;
            }
            var shipParts = new HashSet<Part>(ship.parts);
            bool fromList = false;
            foreach (KeyValuePair<Part, int> kv in stageOf)
                if (shipParts.Contains(kv.Key)) { fromList = true; break; }
            foreach (Part p in ship.parts)
            {
                string kind = Kind(p);
                if (kind == null) continue;
                if (!fromList)
                {
                    tally.Add(p.inverseStage, kind);
                    continue;
                }
                int stage;
                if (!stageOf.TryGetValue(p, out stage)) continue;  // not staged (e.g. a heat shield's decoupler)
                tally.Add(stage, kind);
                if (stage != p.inverseStage)
                    diagnostics.Add(p.partInfo.name + " (" + kind + ") staged in " + stage + ", inverseStage " + p.inverseStage);
            }
            return tally;
        }

        private ShipFacts Collect(ShipConstruct ship)
        {
            var f = new ShipFacts();
            f.Facility = EditorDriver.editorFacility == EditorFacility.SPH ? "sph" : "vab";
            f.Name = ship.shipName ?? "";
            float dry, fuel, dryCost, fuelCost;
            ship.GetShipMass(out dry, out fuel);
            ship.GetShipCosts(out dryCost, out fuelCost);
            f.Mass = dry + fuel;
            f.Dry = dry;
            f.Cost = dryCost + fuelCost;
            if (Funding.Instance != null) f.Funds = Funding.Instance.Funds;
            f.Parts = ship.parts.Count;
            var capacity = new Dictionary<uint, int>();
            foreach (Part p in ship.parts)
            {
                f.Seats += p.CrewCapacity;
                if (p.FindModulesImplementing<ModuleEngines>().Count > 0) ++f.Engines;
                if (p.CrewCapacity > 0) capacity[p.craftID] = p.CrewCapacity;
            }
            f.Crew = CrewCount(capacity);
            StageTally tally = Tally(ship);
            Vector3 size = ShipConstruction.CalculateCraftSize(ship);
            f.Height = size.y;
            f.Width = Math.Max(size.x, size.z);

            // The situation KSP's delta-v app shows: its own values (DeltaVGlobals), else the last event / inference.
            DeltaVAppValues app = DeltaVGlobals.fetch != null ? DeltaVGlobals.fetch.deltaVAppValues : null;
            VesselDeltaV dv = ship.vesselDeltaV;
            f.DeltaVPending = dv == null || !dv.IsReady;
            DeltaVStageInfo reference = null;
            CelestialBody home = FlightGlobals.GetHomeBody();
            if (!f.DeltaVPending)
            {
                int maxStage = tally.MaxStage;
                if (dv.OperatingStageInfo != null)
                    foreach (DeltaVStageInfo info in dv.OperatingStageInfo)
                        maxStage = Math.Max(maxStage, info.stage);
                for (int s = maxStage; s >= 0; --s)
                {
                    DeltaVStageInfo info = dv.GetStage(s);
                    if (info == null && !tally.Has(s)) continue;
                    var st = new StageFacts { Stage = s };
                    if (info != null)
                    {
                        // what the app displays for its situation, as its GetSituationDeltaV / GetSituationTWR pick
                        // them (deltaVActual / TWRActual do not follow a vacuum selection: with Duna vacuum they read
                        // 2328 m/s and TWR 5.15 for the Poodle stage the app showed as 2452 and 5.42)
                        string code = app != null ? SituationCode(app.situation.ToString()) : situationOption.Length > 0 ? situationOption : "alt";
                        st.DeltaV = ShipMath.Situation(code, info.deltaVatASL, info.deltaVActual, info.deltaVinVac);
                        st.Twr = ShipMath.Situation(code, info.TWRASL, info.TWRActual, info.TWRVac);
                        st.DeltaVVac = info.deltaVinVac;
                        st.Burn = info.stageBurnTime;
                        st.DeltaVKerbinAsl = KerbinAsl(info);
                        // KSP's own sea-level figure is Kerbin's when the app's body is the home body: check against it
                        if (app != null && app.body != null && app.body == home && info.deltaVinVac > 0 &&
                            Math.Abs(info.deltaVatASL - st.DeltaVKerbinAsl) > 2)
                            diagnostics.Add("stage " + s + " Kerbin ASL " + st.DeltaVKerbinAsl.ToString("F0") +
                                            " vs KSP " + info.deltaVatASL.ToString("F0"));
                        if (reference == null && info.thrustActual > 0 && info.TWRActual > 0 && info.startMass > 0)
                            reference = info;
                    }
                    st.Events.AddRange(tally.Events(s));
                    f.Stages.Add(st);
                }
            }
            if (app != null && app.body != null)
                f.Situation = app.body.bodyName.ToLowerInvariant().Replace(' ', '_') + "_" + SituationCode(app.situation.ToString());
            else
                f.Situation = BodyName(reference) + "_" + Option(reference);
            return f;
        }

        // Kerbin sea-level delta-v of a stage, independent of the app's body: each burn segment's vacuum delta-v
        // scaled by its engines' flow-weighted Isp at 1 atm (Kerbin's sea-level pressure) over their vacuum Isp.
        private static double KerbinAsl(DeltaVStageInfo info)
        {
            if (!(info.deltaVinVac > 0)) return 0;
            double total = 0;
            bool any = false;
            if (info.deltaVCalcs != null)
                foreach (DeltaVCalc calc in info.deltaVCalcs)
                {
                    if (calc == null || !(calc.dVinVac > 0)) continue;
                    double v = ShipMath.AtPressure(calc.dVinVac, EngineIsp(calc.activeEngines));
                    if (double.IsNaN(v)) { any = false; break; }
                    total += v;
                    any = true;
                }
            if (any) return total;
            return ShipMath.AtPressure(info.deltaVinVac, EngineIsp(info.enginesActiveInStage));
        }

        private static List<double[]> EngineIsp(List<DeltaVEngineInfo> engines)
        {
            var list = new List<double[]>();
            if (engines == null) return list;
            foreach (DeltaVEngineInfo e in engines)
            {
                if (e == null || e.engine == null || e.engine.atmosphereCurve == null) continue;
                list.Add(new double[] { e.thrustVac, e.ispVac, e.engine.atmosphereCurve.Evaluate(1f) });
            }
            return list;
        }

        // Crew of the current ship only: KSP keeps the previous ship's manifest after New (crew=3 with no parts).
        private static int CrewCount(Dictionary<uint, int> capacity)
        {
            try
            {
                VesselCrewManifest manifest = ShipConstruction.ShipManifest;
                if (manifest == null || capacity.Count == 0) return 0;
                var seats = new List<KeyValuePair<uint, int>>();
                foreach (PartCrewManifest part in manifest.PartManifests)
                {
                    if (part == null) continue;
                    int n = 0;
                    foreach (ProtoCrewMember c in part.GetPartCrew())
                        if (c != null) ++n;
                    seats.Add(new KeyValuePair<uint, int>(part.PartID, n));
                }
                return ShipMath.CrewAboard(seats, capacity);
            }
            catch (Exception) { return 0; }  // the crew dialog may not have built a manifest yet
        }

        // The app's situation as reported by its event, otherwise inferred from the first stage with thrust:
        // the "actual" thrust equals the vacuum or the sea-level thrust, or lies between them (altitude).
        private string Option(DeltaVStageInfo s)
        {
            if (situationOption.Length > 0) return situationOption;
            if (s == null) return "asl";
            double vac = s.thrustVac, asl = s.thrustASL, act = s.thrustActual;
            double tol = 0.002 * Math.Max(vac, 1e-6);
            if (Math.Abs(vac - asl) <= tol) return "asl";  // engines without pressure dependence: cannot tell
            if (Math.Abs(act - vac) <= tol) return "vac";
            if (Math.Abs(act - asl) <= tol) return "asl";
            return "alt";
        }

        // KSP's editor app computes TWR against the selected body's gravity: thrust (kN) / (mass (t) * g) gives g,
        // which names the body (surface gravity, nearest on a log scale). The home body when there is no thrust.
        private static string BodyName(DeltaVStageInfo s)
        {
            CelestialBody home = FlightGlobals.GetHomeBody();
            CelestialBody best = home;
            if (s != null && FlightGlobals.Bodies != null)
            {
                double g = s.thrustActual / (s.startMass * s.TWRActual);
                double bestError = 0.15;  // |ln(ratio)|: altitude situations read a little less than surface gravity
                foreach (CelestialBody b in FlightGlobals.Bodies)
                {
                    double surface = b.GeeASL * G0;
                    if (!(surface > 0) || !(g > 0)) continue;
                    double error = Math.Abs(Math.Log(g / surface));
                    if (error < bestError) { bestError = error; best = b; }
                }
            }
            return best != null ? best.bodyName.ToLowerInvariant().Replace(' ', '_') : "kerbin";
        }
    }
}
