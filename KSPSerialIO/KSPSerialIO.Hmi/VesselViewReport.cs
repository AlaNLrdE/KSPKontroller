using System;
using System.Collections.Generic;
using UnityEngine;

namespace KSPSerialIO.Hmi
{
    /// <summary>
    /// Builds the VESSEL panel's data on Unity's main thread in flight. The shape (one convex outline per part, from
    /// its model meshes projected on the root part's right/up plane, the way Vessel Viewer measures parts) is rebuilt
    /// only when the active vessel changes, its part count changes or KSP reports it modified (staging, docking,
    /// decoupling); each rebuild is a new revision. Fuel and heat are refreshed twice a second. Results go to
    /// VesselViewCache, which the KSPHMIFlight service only reads.
    /// </summary>
    [KSPAddon(KSPAddon.Startup.Flight, false)]
    public class VesselViewReport : MonoBehaviour
    {
        private const string Tag = "KSPSerialIO[HMI] ";
        /// <summary>Vertices read per mesh at most (evenly strided): the outline needs the extremes, not every vertex.</summary>
        private const int VerticesPerMesh = 256;

        private Vessel vessel;
        private readonly List<Part> parts = new List<Part>();
        private bool dirty = true;
        private float nextPoll;
        private string lastError = "";

        public void Start()
        {
            GameEvents.onVesselWasModified.Add(OnModified);
            GameEvents.onVesselChange.Add(OnModified);
        }

        public void OnDestroy()
        {
            GameEvents.onVesselWasModified.Remove(OnModified);
            GameEvents.onVesselChange.Remove(OnModified);
            Publish(new List<VesselPartShape>(), new List<int[]>(), true);
            parts.Clear();
        }

        private void OnModified(Vessel v) { dirty = true; }

        public void Update()
        {
            if (Time.unscaledTime < nextPoll) return;
            nextPoll = Time.unscaledTime + 0.5f;
            try
            {
                Vessel active = FlightGlobals.ActiveVessel;
                if (active != vessel || active == null || active.parts.Count != parts.Count) dirty = true;
                if (dirty) Rebuild(active);
                var state = new List<int[]>(parts.Count);
                foreach (Part p in parts)
                {
                    if (p == null || p.vessel != vessel) { dirty = true; return; }  // decoupled since: rebuild next time
                    double amount = 0, capacity = 0;
                    foreach (PartResource r in p.Resources)
                    {
                        if (!VesselViewFormat.IsFuel(r.resourceName)) continue;
                        amount += r.amount;
                        capacity += r.maxAmount;
                    }
                    state.Add(new[] {
                        VesselViewFormat.FuelPercent(amount, capacity),
                        VesselViewFormat.HeatPercent(p.temperature, p.maxTemp, p.skinTemperature, p.skinMaxTemp) });
                }
                VesselViewCache.State = VesselViewFormat.State(VesselViewCache.Revision, state);
                lastError = "";
            }
            catch (Exception e)
            {
                if (e.Message != lastError) Debug.LogWarning(Tag + "vessel view failed: " + e.Message);
                lastError = e.Message;
                dirty = true;
            }
        }

        private void Rebuild(Vessel active)
        {
            dirty = false;
            vessel = active;
            parts.Clear();
            var shapes = new List<VesselPartShape>();
            if (active != null && active.rootPart != null)
            {
                Transform root = active.rootPart.transform;
                foreach (Part p in active.parts)
                {
                    VesselPartShape shape = Outline(p, root);
                    if (shape == null) continue;
                    parts.Add(p);
                    shapes.Add(shape);
                }
            }
            Publish(shapes, null, false);
            Debug.Log(Tag + "vessel view rev " + VesselViewCache.Revision + ": " + shapes.Count + " parts");
        }

        private static void Publish(List<VesselPartShape> shapes, List<int[]> state, bool cleared)
        {
            unchecked { VesselViewCache.Revision++; }
            VesselViewCache.Shape = VesselViewFormat.Shape(VesselViewCache.Revision, shapes);
            if (cleared || state != null) VesselViewCache.State = VesselViewFormat.State(VesselViewCache.Revision, state ?? new List<int[]>());
        }

        /// <summary>A part's convex side-view outline from its enabled model meshes, null when it has none.</summary>
        private static VesselPartShape Outline(Part p, Transform root)
        {
            var points = new List<double[]>();
            double depth = 0;
            Action<Mesh, Transform> add = (mesh, t) =>
            {
                if (mesh == null) return;
                if (mesh.isReadable)
                {
                    Vector3[] vertices = mesh.vertices;
                    int stride = Math.Max(1, vertices.Length / VerticesPerMesh);
                    for (int i = 0; i < vertices.Length; i += stride) AddPoint(points, ref depth, root, t.TransformPoint(vertices[i]));
                }
                else
                {
                    Bounds b = mesh.bounds;  // unreadable mesh: its bounding box corners
                    for (int i = 0; i < 8; ++i)
                    {
                        Vector3 corner = new Vector3((i & 1) != 0 ? b.max.x : b.min.x, (i & 2) != 0 ? b.max.y : b.min.y,
                                                     (i & 4) != 0 ? b.max.z : b.min.z);
                        AddPoint(points, ref depth, root, t.TransformPoint(corner));
                    }
                }
            };
            foreach (MeshFilter mf in p.FindModelComponents<MeshFilter>())
            {
                MeshRenderer r = mf.GetComponent<MeshRenderer>();
                if (r != null && r.enabled && mf.gameObject.activeInHierarchy && !IsEffect(r)) add(mf.sharedMesh, mf.transform);
            }
            foreach (SkinnedMeshRenderer smr in p.FindModelComponents<SkinnedMeshRenderer>())
                if (smr.enabled && smr.gameObject.activeInHierarchy && !IsEffect(smr)) add(smr.sharedMesh, smr.transform);
            if (points.Count < 3) return null;
            var shape = new VesselPartShape { Depth = depth / points.Count, Hull = VesselViewFormat.ConvexHull(points) };
            return shape.Hull.Count >= 3 ? shape : null;
        }

        /// <summary>
        /// A renderer whose materials all use effect shaders (engine plumes such as Waterfall's live under the part
        /// model, enabled even at zero throttle, and reached 60 m below a 55 m rocket's engines in a live test).
        /// </summary>
        private static bool IsEffect(Renderer r)
        {
            Material[] materials = r.sharedMaterials;
            if (materials == null || materials.Length == 0) return false;
            foreach (Material m in materials)
                if (m == null || m.shader == null || !VesselViewFormat.IsEffectShader(m.shader.name)) return false;
            return true;
        }

        private static void AddPoint(List<double[]> points, ref double depth, Transform root, Vector3 world)
        {
            Vector3 local = root.InverseTransformPoint(world);
            points.Add(new double[] { local.x, local.y });
            depth += local.z;
        }
    }
}
