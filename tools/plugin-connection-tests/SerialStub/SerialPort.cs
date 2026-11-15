// Test-only serial implementation: never opens operating-system ports.
using System;
using System.Collections.Generic;
using System.IO;
using System.Threading;
namespace Psimax.IO.Ports
{
    public enum Parity { None = 0 }
    public enum StopBits { One = 1 }
    public sealed class SerialPort : System.ComponentModel.Component
    {
        public static string[] Names = Array.Empty<string>();
        public static readonly List<string> Constructed = new List<string>();
        public static readonly List<string> Opened = new List<string>();
        public static readonly List<string> Writes = new List<string>();
        public static readonly HashSet<string> FailPorts = new HashSet<string>();
        public static readonly HashSet<string> ReplyPorts = new HashSet<string>();
        public static int ListCalls;
        readonly FakeStream stream = new FakeStream();
        public SerialPort(string name, int baud, Parity parity, int bits, StopBits stop) {
            PortName = name;
            Constructed.Add(name);
        }
        public string PortName { get; }
        public bool IsOpen { get; private set; }
        public bool DtrEnable { get; set; }
        public int WriteTimeout { get; set; }
        public int BytesToRead { get { return 0; } }
        public Stream BaseStream { get { return stream; } }
        public static string[] GetPortNames() { ++ListCalls; return Names; }
        public void Open() {
            Opened.Add(PortName);
            if (FailPorts.Contains(PortName)) throw new IOException("simulated busy or absent port");
            IsOpen = true;
        }
        public void Write(byte[] data, int offset, int count) {
            Writes.Add(PortName);
            if (ReplyPorts.Contains(PortName)) stream.Feed(new byte[] { 0xBE, 0xEF, 4, 0, 3, 1, 4, 2 });
        }
        public void Close() { IsOpen = false; stream.Close(); }
        protected override void Dispose(bool disposing) { Close(); base.Dispose(disposing); }
        sealed class FakeStream : Stream
        {
            readonly Queue<byte> data = new Queue<byte>();
            bool closed;
            public void Feed(byte[] bytes) { lock (data) { foreach (byte b in bytes) data.Enqueue(b); Monitor.PulseAll(data); } }
            public override int Read(byte[] buffer, int offset, int count) {
                lock (data) {
                    while (!closed && data.Count == 0) Monitor.Wait(data);
                    if (closed) return 0;
                    int size = Math.Min(count, data.Count);
                    for (int i = 0; i < size; ++i) buffer[offset + i] = data.Dequeue();
                    return size;
                }
            }
            protected override void Dispose(bool disposing) { lock (data) { closed = true; Monitor.PulseAll(data); } base.Dispose(disposing); }
            public override bool CanRead { get { return true; } }
            public override bool CanWrite { get { return false; } }
            public override bool CanSeek { get { return false; } }
            public override long Length { get { throw new NotSupportedException(); } }
            public override long Position { get { throw new NotSupportedException(); } set { throw new NotSupportedException(); } }
            public override void Flush() {}
            public override long Seek(long offset, SeekOrigin origin) { throw new NotSupportedException(); }
            public override void SetLength(long value) { throw new NotSupportedException(); }
            public override void Write(byte[] buffer, int offset, int count) { throw new NotSupportedException(); }
        }
    }
}
