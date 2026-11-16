using KRPC.Service;
using KRPC.Service.Attributes;
using UnityEngine;

namespace KSPSerialIO.Hmi
{
    /// <summary>
    /// kRPC service for the HMI keyboard in flight (GameScene.Flight). kRPC runs procedures on Unity's main thread.
    /// The held attitude/translation keys go to HmiKeyboard, which KSPSerialIO.AxisInput applies only where the
    /// board's joystick is neutral (kRPC's own Control axes are combined in kRPC's fly-by-wire callback, where the
    /// board's priority cannot be enforced). The throttle keys change FlightInputHandler.state once, exactly like
    /// KSP's Z / X / Shift / Ctrl: kRPC's Control.Throttle would hold the throttle while the client stays connected.
    /// </summary>
    [KRPCService(Name = "KSPHMIFlight", GameScene = GameScene.Flight)]
    public static class HmiFlightService
    {
        /// <summary>The held keys, FlightCtrlState signs (-1..1). Repeat while held: values expire after
        /// HmiKeyboard.Timeout, and a final call with zeros releases them at once.</summary>
        [KRPCProcedure]
        public static void SetAxes(float pitch, float yaw, float roll, float x, float y, float z)
        {
            HmiKeyboard.SetAxes(Time.unscaledTime, pitch, yaw, roll, x, y, z);
        }

        /// <summary>Set the main throttle (0..1) once, like Z (1) and X (0).</summary>
        [KRPCProcedure]
        public static void SetThrottle(float value)
        {
            FlightInputHandler.state.mainThrottle = Mathf.Clamp01(value);
        }

        /// <summary>The VESSEL panel's side view (format v1): one convex outline per part. Fetch it when
        /// VesselState's revision differs from the shape held. Reads VesselViewCache only.</summary>
        [KRPCProcedure]
        public static string VesselShape()
        {
            return VesselViewCache.Shape;
        }

        /// <summary>Fuel and heat per part (format v1, shape order), refreshed twice a second.</summary>
        [KRPCProcedure]
        public static string VesselState()
        {
            return VesselViewCache.State;
        }

        /// <summary>Move the main throttle by `delta` (clamped to 0..1), like a frame of held Shift / Ctrl.</summary>
        [KRPCProcedure]
        public static void NudgeThrottle(float delta)
        {
            FlightInputHandler.state.mainThrottle = Mathf.Clamp01(FlightInputHandler.state.mainThrottle + delta);
        }
    }
}
