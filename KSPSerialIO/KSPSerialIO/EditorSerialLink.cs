using System;
using UnityEngine;

namespace KSPSerialIO
{
    /// <summary>Owns the controller port in the editor. Sends a neutral heartbeat and routes only SAS/RCS press events.</summary>
    [KSPAddon(KSPAddon.Startup.EditorAny, false)]
    public class EditorSerialLink : KSPSerialPort
    {
        private float nextHeartbeat;

        protected override void Awake()
        {
            base.Awake();
            Buttons.Reset();
            KSPLogQueue.Log("KSPSerialIO[EDITOR] serial link started; SAS next, RCS previous");
        }

        protected override void Update()
        {
            base.Update();
            // The supervisor owns every open/reopen; Unity never races it for the port.
            try { EditorNavigation.Consume(Buttons.Take(), Environment.TickCount); }
            catch (Exception e) { KSPLogQueue.Log("KSPSerialIO[EDITOR] navigation rejected: " + e.Message); }
            ControlReceived = false;  // analog axes, stage and action-group levels have no editor meaning
            if (Time.unscaledTime < nextHeartbeat) return;
            nextHeartbeat = Time.unscaledTime + 0.2f;
            VData = new VesselData();
            VData.id = 1;
            TrySendPacket(VData);
        }

        private void OnDestroy()
        {
            PortLog("OnDestroy (editor scene unloading)");
            Buttons.Reset();
            PortCleanup();
            KSPLogQueue.Flush();
        }
    }
}
