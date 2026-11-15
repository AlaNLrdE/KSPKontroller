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
                ShipFacts facts = Collect(ship);
                if (ShipReportCache.Publish(facts))
                    Debug.Log(Tag + "ship report rev " + ShipReportCache.Revision + ": " + facts.Parts + " parts, " +
                              (facts.DeltaVPending ? "delta-v pending" : facts.Stages.Count + " stages") + ", " + facts.Situation);
                lastError = "";
            }
            catch (Exception e)
            {
                // Once per distinct error: a broken report must never spam the log or break the editor.
                if (e.Message != lastError) Debug.Log(Tag + "ship report failed: " + e);
                lastError = e.Message;
            }
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
            f.Crew = CrewCount();
            int maxStage = 0;
            var events = new Dictionary<int, Dictionary<string, int>>();
            foreach (Part p in ship.parts)
            {
                f.Seats += p.CrewCapacity;
                bool engine = p.FindModulesImplementing<ModuleEngines>().Count > 0;
                if (engine) ++f.Engines;
                string kind = engine ? "engines"
                    : p.FindModulesImplementing<LaunchClamp>().Count > 0 ? "clamps"
                    : p.FindModulesImplementing<ModuleDecouple>().Count > 0 ||
                      p.FindModulesImplementing<ModuleAnchoredDecoupler>().Count > 0 ? "decouplers"
                    : p.FindModulesImplementing<ModuleParachute>().Count > 0 ? "chutes"
                    : p.FindModulesImplementing<ModuleProceduralFairing>().Count > 0 ? "fairings"
                    : null;
                if (kind == null) continue;
                maxStage = Math.Max(maxStage, p.inverseStage);
                Dictionary<string, int> stage;
                if (!events.TryGetValue(p.inverseStage, out stage)) events[p.inverseStage] = stage = new Dictionary<string, int>();
                int n;
                stage.TryGetValue(kind, out n);
                stage[kind] = n + 1;
            }
            Vector3 size = ShipConstruction.CalculateCraftSize(ship);
            f.Height = size.y;
            f.Width = Math.Max(size.x, size.z);

            VesselDeltaV dv = ship.vesselDeltaV;
            f.DeltaVPending = dv == null || !dv.IsReady;
            DeltaVStageInfo reference = null;
            if (!f.DeltaVPending)
            {
                if (dv.OperatingStageInfo != null)
                    foreach (DeltaVStageInfo info in dv.OperatingStageInfo)
                        maxStage = Math.Max(maxStage, info.stage);
                string[] order = { "engines", "decouplers", "chutes", "clamps", "fairings" };
                for (int s = maxStage; s >= 0; --s)
                {
                    DeltaVStageInfo info = dv.GetStage(s);
                    Dictionary<string, int> fired;
                    events.TryGetValue(s, out fired);
                    if (info == null && fired == null) continue;
                    var st = new StageFacts { Stage = s };
                    if (info != null)
                    {
                        st.DeltaV = info.deltaVActual;
                        st.DeltaVVac = info.deltaVinVac;
                        st.Twr = info.TWRActual;
                        st.Burn = info.stageBurnTime;
                        if (reference == null && info.thrustActual > 0 && info.TWRActual > 0 && info.startMass > 0)
                            reference = info;
                    }
                    if (fired != null)
                        foreach (string k in order)
                            if (fired.ContainsKey(k)) st.Events.Add(new KeyValuePair<string, int>(k, fired[k]));
                    f.Stages.Add(st);
                }
            }
            f.Situation = BodyName(reference) + "_" + Option(reference);
            return f;
        }

        private static int CrewCount()
        {
            try
            {
                VesselCrewManifest manifest = ShipConstruction.ShipManifest;
                return manifest != null ? manifest.GetAllCrew(false).Count : 0;
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
