using System;
using System.Collections.Generic;
using System.Reflection;
using UnityEngine;
using KSP.UI.Screens;

namespace KSPSerialIO.Hmi
{
    /// <summary>Editor-only facts and navigation, always called on Unity's main thread.</summary>
    public sealed class EditorDetails
    {
        private EditorScreen screen = EditorScreen.Parts;
        private readonly Queue<int> pending = new Queue<int>();
        private bool switching;
        private EditorScreen requested;
        private float requestDeadline;
        private static readonly FieldInfo MassUsed = typeof(ModuleInventoryPart).GetField("massOccupied", BindingFlags.Instance | BindingFlags.NonPublic);
        private static readonly FieldInfo VolumeUsed = typeof(ModuleInventoryPart).GetField("volumeOccupied", BindingFlags.Instance | BindingFlags.NonPublic);
        private static readonly KSPActionGroup[] Groups = {
            KSPActionGroup.Stage, KSPActionGroup.Gear, KSPActionGroup.Light, KSPActionGroup.Brakes,
            KSPActionGroup.Abort, KSPActionGroup.SAS, KSPActionGroup.RCS,
            KSPActionGroup.Custom01, KSPActionGroup.Custom02, KSPActionGroup.Custom03, KSPActionGroup.Custom04,
            KSPActionGroup.Custom05, KSPActionGroup.Custom06, KSPActionGroup.Custom07, KSPActionGroup.Custom08,
            KSPActionGroup.Custom09, KSPActionGroup.Custom10
        };
        private static readonly string[] Codes = { "stage", "gear", "lights", "brakes", "abort", "sas", "rcs",
            "custom1", "custom2", "custom3", "custom4", "custom5", "custom6", "custom7", "custom8", "custom9", "custom10" };

        /// <summary>Accept the authoritative KSP screen-change event.</summary>
        public void OnScreen(EditorScreen value)
        {
            screen = value;
            if (!switching || value != requested) pending.Clear();
            switching = false;
        }

        /// <summary>Request a cyclic editor screen change through the stock UI callbacks.</summary>
        public void Step(int direction)
        {
            if (direction != 1 && direction != -1) return;
            if (EditorDriver.editorFacility != EditorFacility.VAB) return;
            if (pending.Count < 32) pending.Enqueue(direction);
        }

        /// <summary>Serialize queued presses across the stock UI transition; only its screen event acknowledges a change.</summary>
        public void Pump()
        {
            if (switching)
            {
                if (Time.unscaledTime < requestDeadline) return;
                switching = false;
                pending.Clear();
                Debug.Log("KSPSerialIO[EDITOR] screen request not acknowledged; keeping the reported screen");
            }
            if (pending.Count == 0) return;
            EditorLogic editor = EditorLogic.fetch;
            if (editor == null) return;
            int direction = pending.Dequeue();
            int current = screen == EditorScreen.Actions ? 1 : screen == EditorScreen.Crew ? 2 : screen == EditorScreen.Cargo ? 3 : 0;
            int next = (current + direction + 4) % 4;
            requested = next == 0 ? EditorScreen.Parts : next == 1 ? EditorScreen.Actions : next == 2 ? EditorScreen.Crew : EditorScreen.Cargo;
            switching = true;
            requestDeadline = Time.unscaledTime + 2f;
            if (next == 0) editor.SelectPanelParts();
            else if (next == 1) editor.SelectPanelActions();
            else if (next == 2) editor.SelectPanelCrew();
            else editor.SelectPanelCargo();
            // OnEditorScreenChange is authoritative: a locked/modal editor may reject a request.
            Debug.Log("KSPSerialIO[EDITOR] requested screen " + next + " (" + direction + ")");
        }

        /// <summary>Collect current action assignments, seats and inventories.</summary>
        public void Collect(ShipConstruct ship, ShipFacts facts)
        {
            facts.EditorMode = screen == EditorScreen.Actions ? "actions" : screen == EditorScreen.Crew ? "crew" : screen == EditorScreen.Cargo ? "cargo" : "construction";
            facts.SelectedGroup = "";
            if (EditorActionGroups.Instance != null)
                for (int i = 0; i < Groups.Length; ++i)
                    if (EditorActionGroups.Instance.SelectedGroup == Groups[i]) facts.SelectedGroup = Codes[i];
            foreach (Part part in ship.parts)
            {
                string title = Title(part);
                foreach (BaseAction action in part.Actions) AddAction(facts, title, action);
                foreach (PartModule module in part.Modules)
                    foreach (BaseAction action in module.Actions) AddAction(facts, title, action);
                CollectSeats(part, facts);
                int index = 0;
                foreach (ModuleInventoryPart inventory in part.FindModulesImplementing<ModuleInventoryPart>())
                {
                    string key = part.craftID + ":" + index++;
                    var store = new InventoryFacts { Key = key, Part = title, Slots = inventory.InventorySlots,
                        Mass = Read(MassUsed, inventory), Volume = Read(VolumeUsed, inventory),
                        MassCapacity = inventory.HasMassLimit ? inventory.massLimit : -1,
                        VolumeCapacity = inventory.HasPackedVolumeLimit ? inventory.packedVolumeLimit : -1 };
                    facts.Inventories.Add(store);
                    foreach (StoredPart item in inventory.storedParts.Values)
                    {
                        if (item == null || item.quantity <= 0 || item.IsEmpty) continue;
                        AvailablePart info = PartLoader.getPartInfoByName(item.partName);
                        ModuleCargoPart cargo = info != null && info.partPrefab != null ? info.partPrefab.FindModuleImplementing<ModuleCargoPart>() : null;
                        facts.Cargo.Add(new CargoFacts { Inventory = key, Slot = item.slotIndex,
                            Name = info != null ? info.title : item.partName, Quantity = item.quantity,
                            Mass = item.snapshot != null ? item.snapshot.mass * item.quantity : double.NaN,
                            Volume = cargo != null && cargo.packedVolume >= 0 ? cargo.packedVolume * item.quantity : double.NaN });
                    }
                }
            }
        }

        private static double Read(FieldInfo field, object owner) { return field != null ? Convert.ToDouble(field.GetValue(owner)) : double.NaN; }
        /// <summary>Display title for a current ship part.</summary>
        public static string Title(Part part) { return part.partInfo != null ? part.partInfo.title : part.name; }

        private static void AddAction(ShipFacts facts, string part, BaseAction action)
        {
            if (action == null) return;
            for (int i = 0; i < Groups.Length; ++i)
                if ((action.actionGroup & Groups[i]) != 0)
                    facts.Actions.Add(new ActionFacts { Group = Codes[i], Part = part, Name = action.guiName });
        }

        private static void CollectSeats(Part part, ShipFacts facts)
        {
            if (part.CrewCapacity <= 0) return;
            ProtoCrewMember[] crew = null;
            if (ShipConstruction.ShipManifest != null)
                foreach (PartCrewManifest manifest in ShipConstruction.ShipManifest.PartManifests)
                    if (manifest != null && manifest.PartID == part.craftID)
                    {
                        crew = new ProtoCrewMember[part.CrewCapacity];
                        foreach (ProtoCrewMember member in manifest.GetPartCrew())
                        {
                            if (member == null) continue;
                            int seat = manifest.GetCrewSeat(member);
                            if (seat >= 0 && seat < crew.Length) crew[seat] = member;
                        }
                        break;
                    }
            for (int i = 0; i < part.CrewCapacity; ++i)
            {
                ProtoCrewMember member = crew != null && i < crew.Length ? crew[i] : null;
                facts.Manifest.Add(new SeatFacts { Part = Title(part), PartId = part.craftID.ToString(), Seat = i,
                    Name = member != null ? member.name : "", Role = member != null ? member.trait : "",
                    Level = member != null ? member.experienceLevel : 0 });
            }
        }
    }
}
