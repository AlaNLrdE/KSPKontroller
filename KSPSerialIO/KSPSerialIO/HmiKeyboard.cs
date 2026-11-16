using System;

namespace KSPSerialIO
{
    /// <summary>
    /// The HMI keyboard's held flight keys (W/S, A/D, Q/E, J/L, I/K, H/N), received through the KSPHMIFlight kRPC
    /// service of KSPSerialIO.Hmi.dll and applied by KSPSerialIO.AxisInput, both on Unity's main thread. Values use
    /// FlightCtrlState signs, as KSP's own keys set them. The board's joystick wins: Merge applies a key only where
    /// the board leaves that axis neutral. Values older than Timeout read as zero, so a lost HMI never leaves an axis
    /// deflected. Plain C# without Unity types, so tools/button-events-tests can test it.
    /// </summary>
    public static class HmiKeyboard
    {
        public const int Pitch = 0, Yaw = 1, Roll = 2, X = 3, Y = 4, Z = 5;
        /// <summary>Seconds a SetAxes call stays valid; the HMI repeats it every telemetry tick while keys are held.</summary>
        public const float Timeout = 0.5f;

        private static readonly float[] axes = new float[6];
        private static float receivedAt = float.NegativeInfinity;

        /// <summary>Store the held keys (each clamped to -1..1) received at `now` (Time.unscaledTime).</summary>
        public static void SetAxes(float now, float pitch, float yaw, float roll, float x, float y, float z)
        {
            axes[Pitch] = Clamp(pitch);
            axes[Yaw] = Clamp(yaw);
            axes[Roll] = Clamp(roll);
            axes[X] = Clamp(x);
            axes[Y] = Clamp(y);
            axes[Z] = Clamp(z);
            receivedAt = now;
        }

        /// <summary>Forget the held keys (scene change, vessel switch).</summary>
        public static void Clear()
        {
            Array.Clear(axes, 0, axes.Length);
            receivedAt = float.NegativeInfinity;
        }

        /// <summary>One axis at `now`: zero when the last SetAxes is older than Timeout (or from the future).</summary>
        public static float Axis(int axis, float now)
        {
            float age = now - receivedAt;
            return age >= 0f && age <= Timeout ? axes[axis] : 0f;
        }

        /// <summary>The value an axis gets: unchanged when the board deflects it or no key is held, else the key's.</summary>
        public static float Merge(float current, bool boardActive, float key)
        {
            return boardActive || key == 0f ? current : key;
        }

        private static float Clamp(float v)
        {
            if (float.IsNaN(v)) return 0f;
            return v < -1f ? -1f : v > 1f ? 1f : v;
        }
    }
}
