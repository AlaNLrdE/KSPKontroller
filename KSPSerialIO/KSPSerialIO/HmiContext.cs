using System;
using System.Net;
using System.Net.Sockets;
using System.Text;
using UnityEngine;

namespace KSPSerialIO
{
    // Read-only scene discovery. Unity state and socket IO stay on the main thread.
    [KSPAddon(KSPAddon.Startup.MainMenu, true)]
    public class HmiContext : MonoBehaviour
    {
        private UdpClient socket;
        private string facility = "";

        public void Awake()
        {
            DontDestroyOnLoad(this);
            GameEvents.onGUIAstronautComplexSpawn.Add(Astronauts);
            GameEvents.onGUIAstronautComplexDespawn.Add(CloseFacility);
            GameEvents.onGUIMissionControlSpawn.Add(Contracts);
            GameEvents.onGUIMissionControlDespawn.Add(CloseFacility);
            GameEvents.onGUIRnDComplexSpawn.Add(Research);
            GameEvents.onGUIRnDComplexDespawn.Add(CloseFacility);
            GameEvents.onGUIAdministrationFacilitySpawn.Add(Administration);
            GameEvents.onGUIAdministrationFacilityDespawn.Add(CloseFacility);
            try { socket = new UdpClient(50002); socket.Client.Blocking = false; }
            catch (SocketException e) { Debug.Log("KSPSerialIO[HMI] context unavailable: " + e.Message); }
        }

        private void Astronauts() { facility = "astronaut_complex"; }
        private void Contracts() { facility = "mission_control"; }
        private void Research() { facility = "research"; }
        private void Administration() { facility = "administration"; }
        private void CloseFacility() { facility = ""; }

        public void Update()
        {
            if (HighLogic.LoadedScene != GameScenes.SPACECENTER) facility = "";
            if (socket == null) return;
            // Bound work per frame, even if a peer sends too many requests.
            for (int i = 0; i < 4 && socket.Available > 0; ++i)
            {
                try
                {
                    IPEndPoint peer = new IPEndPoint(IPAddress.Any, 0);
                    byte[] request = socket.Receive(ref peer);
                    if (Encoding.ASCII.GetString(request) != "KSPHMI/1") continue;
                    string scene = HighLogic.LoadedScene.ToString().ToLowerInvariant();
                    if (HighLogic.LoadedSceneIsEditor)
                        scene = EditorDriver.editorFacility == EditorFacility.SPH ? "sph" : "vab";
                    else if (facility.Length > 0) scene = facility;
                    else if (scene == "spacecenter") scene = "space_center";
                    else if (scene == "trackstation") scene = "tracking_station";
                    byte[] response = Encoding.ASCII.GetBytes("KSPHMI/1 " + scene);
                    socket.Send(response, response.Length, peer);
                }
                catch (SocketException) { return; }
            }
        }

        public void OnDestroy()
        {
            GameEvents.onGUIAstronautComplexSpawn.Remove(Astronauts);
            GameEvents.onGUIAstronautComplexDespawn.Remove(CloseFacility);
            GameEvents.onGUIMissionControlSpawn.Remove(Contracts);
            GameEvents.onGUIMissionControlDespawn.Remove(CloseFacility);
            GameEvents.onGUIRnDComplexSpawn.Remove(Research);
            GameEvents.onGUIRnDComplexDespawn.Remove(CloseFacility);
            GameEvents.onGUIAdministrationFacilitySpawn.Remove(Administration);
            GameEvents.onGUIAdministrationFacilityDespawn.Remove(CloseFacility);
            if (socket != null) socket.Close();
        }
    }
}
