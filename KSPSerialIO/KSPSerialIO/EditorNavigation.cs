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
                if (age < 0 || age > 1000) continue;
                Press(press.Bit);
            }
        }

        /// <summary>One SAS (bit 7, next screen) or RCS (bit 6, previous) press on Unity's main thread: a board
        /// press from Consume, or the HMI keyboard through the KSPHMI.Press kRPC procedure. Other bits are ignored.</summary>
        public static void Press(int bit)
        {
            if (bit != 6 && bit != 7) return;
            Action<int> handler = StepRequested;
            if (handler != null) handler(bit == 7 ? 1 : -1);
        }
    }
}
