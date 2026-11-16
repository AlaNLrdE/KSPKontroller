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
                var kept = new List<Part>();
                var clouds = new List<List<Vector3>>();
                float minX = float.MaxValue, maxX = float.MinValue, minZ = float.MaxValue, maxZ = float.MinValue;
                foreach (Part p in active.parts)
                {
                    if (p.FindModuleImplementing<LaunchClamp>() != null) continue;  // ground equipment (clamps, towers)
                    List<Vector3> cloud = Points(p, root);
                    if (cloud.Count < 3) continue;
                    foreach (Vector3 v in cloud)
                    {
                        minX = Math.Min(minX, v.x); maxX = Math.Max(maxX, v.x);
                        minZ = Math.Min(minZ, v.z); maxZ = Math.Max(maxZ, v.z);
                    }
                    kept.Add(p);
                    clouds.Add(cloud);
                }
                // Look from the side where the vessel is widest, so side boosters stand beside the core instead of
                // in front of it: the root part's right axis, or its forward axis when that spread is larger.
                bool side = clouds.Count > 0 && maxZ - minZ > maxX - minX + 0.01f;
                for (int i = 0; i < kept.Count; ++i)
                {
                    var points = new List<double[]>(clouds[i].Count);
                    double depth = 0;
                    foreach (Vector3 v in clouds[i])
                    {
                        points.Add(side ? new double[] { v.z, v.y } : new double[] { v.x, v.y });
                        depth += side ? -v.x : v.z;
                    }
                    var shape = new VesselPartShape { Depth = depth / points.Count, Hull = VesselViewFormat.ConvexHull(points) };
                    if (shape.Hull.Count < 3) continue;
                    parts.Add(kept[i]);
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

        /// <summary>
        /// A part's enabled model mesh points in the root part's frame. Effect renderers are skipped, and so is a mesh
        /// reaching well outside the part's colliders (a live Bluedog RL79 carried a ~25 m mesh that is not a plume
        /// shader); each skipped mesh is logged once with its shader so the rule can be checked.
        /// </summary>
        private static List<Vector3> Points(Part p, Transform root)
        {
            var points = new List<Vector3>();
            Bounds? body = ColliderBox(p, root);
            Action<Renderer, Mesh, Transform> add = (r, mesh, t) =>
            {
                if (mesh == null) return;
                var cloud = new List<Vector3>();
                if (mesh.isReadable)
                {
                    Vector3[] vertices = mesh.vertices;
                    int stride = Math.Max(1, vertices.Length / VerticesPerMesh);
                    for (int i = 0; i < vertices.Length; i += stride) cloud.Add(root.InverseTransformPoint(t.TransformPoint(vertices[i])));
                }
                else
                {
                    Bounds b = mesh.bounds;  // unreadable mesh: its bounding box corners
                    for (int i = 0; i < 8; ++i)
                    {
                        Vector3 corner = new Vector3((i & 1) != 0 ? b.max.x : b.min.x, (i & 2) != 0 ? b.max.y : b.min.y,
                                                     (i & 4) != 0 ? b.max.z : b.min.z);
                        cloud.Add(root.InverseTransformPoint(t.TransformPoint(corner)));
                    }
                }
                if (body.HasValue && !Within(cloud, body.Value))
                {
                    Material m = r.sharedMaterial;
                    string key = p.partInfo.name + "/" + t.name;
                    if (Skipped.Add(key))
                        Debug.Log(Tag + "vessel view: skipped mesh " + key + " outside the part's colliders (shader " +
                                  (m != null && m.shader != null ? m.shader.name : "none") + ", queue " +
                                  (m != null ? m.renderQueue : -1) + ")");
                    return;
                }
                points.AddRange(cloud);
            };
            foreach (MeshFilter mf in p.FindModelComponents<MeshFilter>())
            {
                MeshRenderer r = mf.GetComponent<MeshRenderer>();
                if (r != null && r.enabled && mf.gameObject.activeInHierarchy && !IsEffect(r)) add(r, mf.sharedMesh, mf.transform);
            }
            foreach (SkinnedMeshRenderer smr in p.FindModelComponents<SkinnedMeshRenderer>())
                if (smr.enabled && smr.gameObject.activeInHierarchy && !IsEffect(smr)) add(smr, smr.sharedMesh, smr.transform);
            return points;
        }

        private static readonly HashSet<string> Skipped = new HashSet<string>();

        /// <summary>The part's solid colliders as one box in the root frame, null when it has none.</summary>
        private static Bounds? ColliderBox(Part p, Transform root)
        {
            Bounds? box = null;
            foreach (Collider c in p.GetComponentsInChildren<Collider>())
            {
                if (!c.enabled || c.isTrigger || !c.gameObject.activeInHierarchy) continue;
                Bounds w = c.bounds;
                for (int i = 0; i < 8; ++i)
                {
                    Vector3 local = root.InverseTransformPoint(new Vector3((i & 1) != 0 ? w.max.x : w.min.x,
                        (i & 2) != 0 ? w.max.y : w.min.y, (i & 4) != 0 ? w.max.z : w.min.z));
                    if (box.HasValue) { Bounds b = box.Value; b.Encapsulate(local); box = b; }
                    else box = new Bounds(local, Vector3.zero);
                }
            }
            return box;
        }

        /// <summary>True when every point lies inside the box grown by 0.5 m plus half its size on each axis.</summary>
        private static bool Within(List<Vector3> cloud, Bounds box)
        {
            Vector3 grow = box.size * 0.5f + Vector3.one * 0.5f;
            Vector3 lo = box.min - grow, hi = box.max + grow;
            foreach (Vector3 v in cloud)
                if (v.x < lo.x || v.x > hi.x || v.y < lo.y || v.y > hi.y || v.z < lo.z || v.z > hi.z) return false;
            return true;
        }

        /// <summary>
        /// A renderer whose materials all use effect shaders or the transparent render queue (engine plumes such as
        /// Waterfall's live under the part model, enabled even at zero throttle, and reached 60 m below a 55 m
        /// rocket's engines in a live test).
        /// </summary>
        private static bool IsEffect(Renderer r)
        {
            Material[] materials = r.sharedMaterials;
            if (materials == null || materials.Length == 0) return false;
            foreach (Material m in materials)
                if (m == null || m.shader == null ||
                    !(VesselViewFormat.IsEffectShader(m.shader.name) || m.renderQueue >= 3000)) return false;
            return true;
        }
    }
}
