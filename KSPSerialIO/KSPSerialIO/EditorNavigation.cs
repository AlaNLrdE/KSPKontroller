using System;

namespace KSPSerialIO
{
    /// <summary>Receive-time editor navigation presses, consumed only on Unity's main thread.</summary>
    public static class EditorNavigation
    {
        public static event Action<int> StepRequested;

        public static void Consume(ButtonEvents.Press[] presses, int now)
        {
            foreach (ButtonEvents.Press press in presses)
            {
                int age = unchecked(now - press.Received);
                if (age < 0 || age > 1000 || (press.Bit != 6 && press.Bit != 7)) continue;
                Action<int> handler = StepRequested;
                if (handler != null) handler(press.Bit == 7 ? 1 : -1);
            }
        }
    }
}
