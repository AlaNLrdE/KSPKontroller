using System;
using System.Collections.Generic;

namespace KSPSerialIO
{
    /// <summary>Receive-time button edges, independent of Unity and the latest analog control packet.</summary>
    public sealed class ButtonEvents
    {
        public struct Press
        {
            public int Bit;
            public int Received;
        }

        private static readonly int[] Bits = new int[] { 6, 7, 0 };
        private readonly object gate = new object();
        private readonly Queue<Press> pending = new Queue<Press>();
        private bool seeded;
        private byte previous;

        public void Reset()
        {
            lock (gate)
            {
                seeded = false;
                pending.Clear();
            }
        }

        /// <summary>The first packet seeds held buttons; STAGE rises, SAS/RCS toggle on either edge.</summary>
        public void Capture(byte controls, int received)
        {
            lock (gate)
            {
                if (seeded)
                {
                    int changed = controls ^ previous;
                    foreach (int bit in Bits)
                    {
                        int mask = 1 << bit;
                        if ((changed & mask) != 0 && (bit != 0 || (controls & mask) != 0))
                            pending.Enqueue(new Press { Bit = bit, Received = received });
                    }
                }
                previous = controls;
                seeded = true;
            }
        }

        /// <summary>Atomically transfers all pending presses to the Unity thread.</summary>
        public Press[] Take()
        {
            lock (gate)
            {
                Press[] result = pending.ToArray();
                pending.Clear();
                return result;
            }
        }
    }
}
