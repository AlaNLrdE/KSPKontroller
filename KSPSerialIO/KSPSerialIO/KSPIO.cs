/**
 * @file KSPIO.cs
 * @brief KSP plugin that bridges Kerbal Space Program and an external serial controller (Arduino).
 *
 * The plugin has three cooperating parts:
 *  - SettingsNStuff: loads config.xml once, when the main menu is shown.
 *  - KSPSerialPort: owns the serial port, the packet framing/parsing and the shared packet state.
 *  - KSPSerialIO: runs inside the flight scene, publishes vessel telemetry and applies the
 *    controls received from the hardware to the active vessel.
 *
 * @par Wire protocol
 * Every packet, in both directions, has the same framing:
 * @code
 * [0xBE][0xEF][size][payload ... size bytes][checksum]
 * @endcode
 * The first payload byte is the packet id (see the id constants in KSPSerialPort).
 * The checksum is @c size XOR-ed with every payload byte. All multi-byte values are
 * little-endian and the structs below are packed (no padding).
 *
 * @par Packet ids
 *  - 0   HandShakePacket (both directions)
 *  - 1   VesselData (KSP -> controller)
 *  - 101 ControlPacket (controller -> KSP)
 *
 * @par Threading
 * Bytes are read on a dedicated worker thread (KSPSerialPort::SerialWorker). Complete, checksum-valid
 * packets are copied to a shared buffer under a mutex and consumed on Unity's main thread
 * (KSPSerialPort::Update). The only exception is the handshake, which is processed directly on the
 * worker thread because it happens during scene load, before Update() ever runs.
 */


using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using System.Reflection;
using System.Threading;
using Microsoft.Win32;
using System.Runtime.InteropServices;

using Psimax.IO.Ports;
using UnityEngine;
using KSP.IO;
using KSP.UI.Screens;

namespace KSPSerialIO
{
    #region Structs

    /**
     * @brief Telemetry sent from KSP to the controller (packet id 1).
     *
     * The layout must match the @c VesselData struct in the Arduino sketch byte for byte
     * (214 bytes, packed).
     * Fields ending in @c Tot hold the maximum amount of a resource and the matching field
     * without the suffix holds the current amount. Fields ending in @c S only account for
     * the resources reachable by the currently active engines (the "stage" view).
     */
    [StructLayout(LayoutKind.Sequential, Pack = 1)]
    public struct VesselData
    {
        public byte id;             ///< Packet id, always 1
        public float AP;            ///< Apoapsis altitude (m)
        public float PE;            ///< Periapsis altitude (m)
        public float SemiMajorAxis; ///< Orbit semi-major axis (m)
        public float SemiMinorAxis; ///< Orbit semi-minor axis (m)
        public float VVI;           ///< Vertical speed (m/s)
        public float e;             ///< Orbit eccentricity
        public float inc;           ///< Orbit inclination (deg)
        public float G;             ///< G-force
        public int TAp;             ///< Time to apoapsis (s)
        public int TPe;             ///< Time to periapsis (s)
        public float TrueAnomaly;   ///< True anomaly (rad)
        public float Density;       ///< Atmospheric density at the vessel
        public int period;          ///< Orbital period (s)
        public float RAlt;          ///< Radar altitude: the lower of altitude above terrain and above sea level (m)
        public float Alt;           ///< Altitude above sea level (m)
        public float Vsurf;         ///< Surface speed (m/s)
        public float Lat;           ///< Latitude (deg)
        public float Lon;           ///< Longitude (deg)
        public float LiquidFuelTot; ///< Liquid fuel capacity, whole vessel
        public float LiquidFuel;    ///< Liquid fuel amount, whole vessel
        public float OxidizerTot;   ///< Oxidizer capacity, whole vessel
        public float Oxidizer;      ///< Oxidizer amount, whole vessel
        public float EChargeTot;    ///< Electric charge capacity
        public float ECharge;       ///< Electric charge amount
        public float MonoPropTot;   ///< Monopropellant capacity
        public float MonoProp;      ///< Monopropellant amount
        public float IntakeAirTot;  ///< Intake air capacity
        public float IntakeAir;     ///< Intake air amount
        public float SolidFuelTot;  ///< Solid fuel capacity
        public float SolidFuel;     ///< Solid fuel amount
        public float XenonGasTot;   ///< Xenon gas capacity
        public float XenonGas;      ///< Xenon gas amount
        public float LiquidFuelTotS;///< Liquid fuel capacity reachable by the active engines
        public float LiquidFuelS;   ///< Liquid fuel amount reachable by the active engines
        public float OxidizerTotS;  ///< Oxidizer capacity reachable by the active engines
        public float OxidizerS;     ///< Oxidizer amount reachable by the active engines
        public UInt32 MissionTime;  ///< Mission elapsed time (s)
        public float deltaTime;     ///< Mission time elapsed since the previous packet (s)
        public float VOrbit;        ///< Orbital speed (m/s)
        public UInt32 MNTime;       ///< Time until the next maneuver node (s), 0 if none
        public float MNDeltaV;      ///< Delta-v of the next maneuver node (m/s), 0 if none
        public float Pitch;         ///< Pitch (deg)
        public float Roll;          ///< Roll (deg)
        public float Heading;       ///< Heading (deg)
        /// Action group status bits. Bit order: SAS, RCS, Light, Gear, Brakes, Abort, Custom01 - 10 (see enumAG).
        public UInt16 ActionGroups;
        public byte SOINumber;      ///< Sphere of influence id, decimal form sun-planet-moon (e.g. 130 = Kerbin, 131 = Mun)
        public byte MaxOverHeat;    ///< Highest part overheat (percent)
        public float MachNumber;    ///< Mach number
        public float IAS;           ///< Indicated air speed (m/s)
        public byte CurrentStage;   ///< Current stage number
        public byte TotalStage;     ///< Total number of stages
        public float TargetDist;    ///< Distance to the targeted vessel (m), 0 if no target
        public float TargetV;       ///< Velocity relative to the targeted vessel (m/s)
        /**
         * Combined navball and SAS mode.
         *
         * The low 4 bits hold the autopilot mode plus one: 0 SAS is off, 1 stability assist,
         * 2 prograde, 3 retrograde, 4 normal, 5 anti-normal, 6 radial in, 7 radial out,
         * 8 target, 9 anti-target, 10 maneuver node. The high 4 bits hold the navball
         * speed display mode plus one: 0 ignore, 1 orbit, 2 surface, 3 target.
         */
        public byte NavballSASMode;
        /**
         * Navball vector directions (fields 56 to 62), added in upstream 0.19.1. Every angle is a
         * signed 16-bit fixed-point value: the angle in degrees times 50, so divide by 50 to get
         * degrees (pitch -90 to 90, heading 0 to 360). A direction that does not exist is 0.
         */
        public short ProgradePitch;   ///< Pitch of the prograde vector of the current navball mode (orbit, surface or target)
        public short ProgradeHeading; ///< Heading of the prograde vector of the current navball mode
        public short ManeuverPitch;   ///< Pitch of the maneuver node burn vector, 0 if there is no node
        public short ManeuverHeading; ///< Heading of the maneuver node burn vector, 0 if there is no node
        public short TargetPitch;     ///< Pitch towards the targeted vessel, 0 if there is no target
        public short TargetHeading;   ///< Heading towards the targeted vessel, 0 if there is no target
        public short NormalHeading;   ///< Heading of the orbit normal (its pitch is always 0)
    }

    /**
     * @brief Handshake packet (packet id 0), used in both directions.
     *
     * KSP sends it with M1/M2/M3 = 1/2/3 to probe a port. A compatible controller answers with
     * 3/1/4, which is how the plugin recognizes its display (see KSPSerialPort::InboundPacketHandler).
     */
    [StructLayout(LayoutKind.Sequential, Pack = 1)]
    public struct HandShakePacket
    {
        public byte id;  ///< Packet id, always 0
        public byte M1;  ///< First handshake magic byte
        public byte M2;  ///< Second handshake magic byte
        public byte M3;  ///< Third handshake magic byte
    }

    /**
     * @brief Controls sent from the controller to KSP (packet id 101).
     *
     * The layout must match the @c ControlPacket struct in the Arduino sketch (25 bytes, packed).
     * The Arduino side must use fixed-width types: on a UNO R4 a plain @c int is 32 bits wide.
     */
    [StructLayout(LayoutKind.Sequential, Pack = 1)]
    public struct ControlPacket
    {
        public byte id;                            ///< Packet id, always 101
        /// Main toggles, one per bit (bit 7 down to bit 0): SAS, RCS, Lights, Gear, Brakes, Precision, Abort, Stage.
        public byte MainControls;
        public byte Mode;                          ///< 0 = stage, 1 = docking, 2 = map
        public ushort ControlGroup;                ///< Custom action groups 1-10; bit n is group n
        /// AutoPilot mode in the low 4 bits and navball mode in the high 4 bits (see VesselData::NavballSASMode).
        /// Ignored when zero or out of range (> 10).
        public byte NavballSASMode;
        public byte AdditionalControlByte1;        ///< Reserved for future use
        public short Pitch;                        ///< Pitch axis, -1000 -> 1000
        public short Roll;                         ///< Roll axis, -1000 -> 1000
        public short Yaw;                          ///< Yaw axis, -1000 -> 1000
        public short TX;                           ///< Translation X (RCS), -1000 -> 1000
        public short TY;                           ///< Translation Y (RCS), -1000 -> 1000
        public short TZ;                           ///< Translation Z (RCS), -1000 -> 1000
        public short WheelSteer;                   ///< Wheel steering, -1000 -> 1000
        public short Throttle;                     ///< Main throttle, 0 -> 1000
        public short WheelThrottle;                ///< Wheel throttle, 0 -> 1000
    };

    /**
     * @brief Decoded, KSP-friendly version of the last received ControlPacket.
     *
     * Toggles are booleans and axes are scaled to the -1..1 (or 0..1) range KSP expects.
     * Filled by KSPSerialPort::VesselControls and consumed by KSPSerialIO::Update and KSPSerialIO::AxisInput.
     */
    public struct VesselControls
    {
        public Boolean SAS;
        public Boolean RCS;
        public Boolean Lights;
        public Boolean Gear;
        public Boolean Brakes;
        public Boolean Precision;
        public Boolean Abort;
        public Boolean Stage;
        public int Mode;
        public int SASMode;            ///< Requested autopilot mode plus one (0 = no change)
        public int SpeedMode;          ///< Requested navball speed mode plus one (0 = no change)
        public Boolean[] ControlGroup; ///< Custom action groups, indexed 1-10 (index 0 is unused)
        public float Pitch;            ///< -1 to 1
        public float Roll;             ///< -1 to 1
        public float Yaw;              ///< -1 to 1
        public float TX;               ///< -1 to 1
        public float TY;               ///< -1 to 1
        public float TZ;               ///< -1 to 1
        public float WheelSteer;       ///< -1 to 1
        public float Throttle;         ///< 0 to 1
        public float WheelThrottle;    ///< 0 to 1
    };

    /// @brief Current and maximum amount of a vessel resource.
    public struct IOResource
    {
        public float Max;
        public float Current;
    }

    /// @brief A direction relative to the local horizon, in degrees.
    public struct NavHeading
    {
        public float Pitch, Heading;
    }

    #endregion

    /// @brief Bit positions of the action groups inside VesselData::ActionGroups.
    enum enumAG : int
    {
        SAS,
        RCS,
        Light,
        Gear,
        Brakes,
        Abort,
        Custom01,
        Custom02,
        Custom03,
        Custom04,
        Custom05,
        Custom06,
        Custom07,
        Custom08,
        Custom09,
        Custom10,
    };

    /**
     * @brief Loads the plugin settings from config.xml.
     *
     * Runs once at the main menu. Every setting is exposed as a static field so the flight
     * scene components can read it. The axis settings (@c PitchEnable ... @c WheelThrottleEnable)
     * choose how the controller value is combined with KSP's own value:
     *  - 0: always use KSP's value (controller ignored)
     *  - 1: always use the controller's value
     *  - 2: use KSP's value unless it is zero, then use the controller's
     *  - 3: use the controller's value unless it is zero, then use KSP's
     */
    [KSPAddon(KSPAddon.Startup.MainMenu, false)]
    public class SettingsNStuff : MonoBehaviour
    {

        /// Settings file handle, bound to the plugin's PluginData folder.
        public static PluginConfiguration cfg = PluginConfiguration.CreateForType<SettingsNStuff>();
        public static string DefaultPort;     ///< Serial port tried first (e.g. COM3)
        public static double refreshrate;     ///< Minimum seconds between telemetry packets
        public static int HandshakeDelay;     ///< Milliseconds to wait after opening a port before talking (boards that reset on open)
        public static int HandshakeDisable;   ///< 1 = skip the handshake and trust DefaultPort
        public static int BaudRate;           ///< Serial baud rate; must match the firmware
        // Throttle and axis controls have the following settings:
        // 0: The internal value (supplied by KSP) is always used.
        // 1: The external value (read from serial packet) is always used.
        // 2: If the internal value is not zero use it, otherwise use the external value.
        // 3: If the external value is not zero use it, otherwise use the internal value.
        public static int PitchEnable;        ///< Axis mode for pitch (see class description)
        public static int RollEnable;         ///< Axis mode for roll
        public static int YawEnable;          ///< Axis mode for yaw
        public static int TXEnable;           ///< Axis mode for translation X
        public static int TYEnable;           ///< Axis mode for translation Y
        public static int TZEnable;           ///< Axis mode for translation Z
        public static int WheelSteerEnable;   ///< Axis mode for wheel steering
        public static int ThrottleEnable;     ///< Axis mode for the main throttle
        public static int WheelThrottleEnable;///< Axis mode for the wheel throttle
        public static int PerfLog;            ///< 1 = log how much time the plugin takes per frame every 10 s (default 0)
        public static double SASTol;          ///< Obsolete since 0.19.3: no longer used, still read so existing config.xml files load

        /// @brief Reads every setting from config.xml into the static fields.
        void Awake()
        {
            //cfg["refresh"] = 0.08;
            //cfg["DefaultPort"] = "COM1";
            //cfg["HandshakeDelay"] = 2500;
            print("KSPSerialIO: Loading settings...");

            cfg.load();
            DefaultPort = cfg.GetValue<string>("DefaultPort");
            print("KSPSerialIO: Default Port = " + DefaultPort);

            refreshrate = cfg.GetValue<double>("refresh");
            print("KSPSerialIO: Refreshrate = " + refreshrate.ToString());

            BaudRate = cfg.GetValue<int>("BaudRate");
            print("KSPSerialIO: BaudRate = " + BaudRate.ToString());

            HandshakeDelay = cfg.GetValue<int>("HandshakeDelay");
            print("KSPSerialIO: Handshake Delay = " + HandshakeDelay.ToString());

            HandshakeDisable = cfg.GetValue<int>("HandshakeDisable");
            print("KSPSerialIO: Handshake Disable = " + HandshakeDisable.ToString());

            PitchEnable = cfg.GetValue<int>("PitchEnable");
            print("KSPSerialIO: Pitch Enable = " + PitchEnable.ToString());

            RollEnable = cfg.GetValue<int>("RollEnable");
            print("KSPSerialIO: Roll Enable = " + RollEnable.ToString());

            YawEnable = cfg.GetValue<int>("YawEnable");
            print("KSPSerialIO: Yaw Enable = " + YawEnable.ToString());

            TXEnable = cfg.GetValue<int>("TXEnable");
            print("KSPSerialIO: Translate X Enable = " + TXEnable.ToString());

            TYEnable = cfg.GetValue<int>("TYEnable");
            print("KSPSerialIO: Translate Y Enable = " + TYEnable.ToString());

            TZEnable = cfg.GetValue<int>("TZEnable");
            print("KSPSerialIO: Translate Z Enable = " + TZEnable.ToString());

            WheelSteerEnable = cfg.GetValue<int>("WheelSteerEnable");
            print("KSPSerialIO: Wheel Steering Enable = " + WheelSteerEnable.ToString());

            ThrottleEnable = cfg.GetValue<int>("ThrottleEnable");
            print("KSPSerialIO: Throttle Enable = " + ThrottleEnable.ToString());

            WheelThrottleEnable = cfg.GetValue<int>("WheelThrottleEnable");
            print("KSPSerialIO: Wheel Throttle Enable = " + WheelThrottleEnable.ToString());

            PerfLog = cfg.GetValue<int>("PerfLog");
            print("KSPSerialIO: Perf Log = " + PerfLog.ToString());

            SASTol = cfg.GetValue<double>("SASTol");
            print("KSPSerialIO: SAS Tol = " + SASTol.ToString());
        }
    }

    /**
     * @brief Owns the serial port and the packet protocol.
     *
     * In the flight scene, Awake() either finds the controller (handshake on every candidate port)
     * or, when it was already found in an earlier flight, just recreates the port. The port is
     * then opened by KSPSerialIO::Start. A background thread reads bytes, a state machine
     * (ReceivedDataEvent) assembles packets, and Update() dispatches them on the main thread.
     */
    [KSPAddon(KSPAddon.Startup.Flight, false)]
    public class KSPSerialPort : MonoBehaviour
    {
        public static SerialPort Port;            ///< The serial port in use
        public static string PortNumber;          ///< Name of the port currently being probed or used
        public static Boolean DisplayFound = false;   ///< True once a controller answered the handshake
        public static Boolean ControlReceived = false;///< Set when a new ControlPacket was decoded, cleared by KSPSerialIO::Update

        public static VesselData VData;           ///< Telemetry being filled and sent to the controller
        public static ControlPacket CPacket;      ///< Last raw ControlPacket received
        private static HandShakePacket HPacket;   ///< Handshake packet, sent and received

        public static VesselControls VControls = new VesselControls();    ///< Decoded controls, latest values
        public static VesselControls VControlsOld = new VesselControls(); ///< Controls already applied to the vessel (change detection)

        private const int MaxPayloadSize = 255;   ///< Largest payload a packet can carry (size is one byte)

        /// @brief States of the packet parser, in the order a packet is received.
        enum ReceiveStates: byte {
            FIRSTHEADER, // Waiting for first header
            SECONDHEADER, // Waiting for second header
            SIZE, // Waiting for payload size
            PAYLOAD, // Waiting for rest of payload
            CS // Waiting for checksum
        }
        private static ReceiveStates CurrentState = ReceiveStates.FIRSTHEADER;
        private static byte CurrentPacketLength;  ///< Payload size announced by the packet being parsed
        private static byte CurrentBytesRead;     ///< Payload bytes stored so far for the packet being parsed
        // Guards access to data shared between threads
        private static Mutex SerialMutex = new Mutex();
        // Serial worker uses this buffer to read bytes
        private static byte[] PayloadBuffer = new byte[MaxPayloadSize];
        // Buffer for sharing packets from serial worker to main thrad
        private static volatile bool NewPacketFlag = false;
        private static volatile byte[] NewPacketBuffer = new byte[MaxPayloadSize];
        // Semaphore to indicate whether the serial worker should do work
        private static volatile bool doSerialRead = true;
        private static Thread SerialThread;       ///< Background thread running SerialWorker

        private const byte HSPid = 0, VDid = 1, Cid = 101; //hard coded values for packet IDS

        /**
         * @brief Dispatches the packet waiting in NewPacketBuffer according to its id.
         *
         * Called from Update() on the main thread, and from the worker thread while the handshake
         * is still pending. Takes SerialMutex and always releases it before running the handler.
         */
        public static void InboundPacketHandler()
        {
            SerialMutex.WaitOne();
            NewPacketFlag = false;
            switch (NewPacketBuffer[0])
            {
                case HSPid:
                    HPacket = (HandShakePacket)ByteArrayToStructure(NewPacketBuffer, HPacket);
                    SerialMutex.ReleaseMutex();
                    HandShake();
                    if ((HPacket.M1 == 3) && (HPacket.M2 == 1) && (HPacket.M3 == 4)) {
                        DisplayFound = true;
                    } else
                    {
                        DisplayFound = false;
                    }
                    break;
                case Cid:
                    CPacket = (ControlPacket)ByteArrayToStructure(NewPacketBuffer, CPacket);
                    SerialMutex.ReleaseMutex();
                    VesselControls();
                    break;
                default:
                    SerialMutex.ReleaseMutex();
                    Debug.Log("KSPSerialIO: Packet id unimplementd");
                    break;
            }

        }

        /**
         * @brief Serializes a struct, wraps it in the wire framing and writes it to the port.
         * @param anything A packed packet struct (VesselData or HandShakePacket).
         */
        public static void sendPacket(object anything)
        {
            byte[] Packet = BuildFrame(anything);
            Port.Write(Packet, 0, Packet.Length);
        }

        /// @brief Serializes a struct and wraps it in the wire framing, ready to be written to the port.
        private static byte[] BuildFrame(object anything)
        {
            byte[] Payload = StructureToByteArray(anything);
            byte header1 = 0xBE;
            byte header2 = 0xEF;
            byte size = (byte)Payload.Length;
            byte checksum = size;

            byte[] Packet = new byte[size + 4];

            //Packet = [header][size][payload][checksum];
            //Header = [Header1=0xBE][Header2=0xEF]
            //size = [payload.length (0-255)]

            for (int i = 0; i < size; i++)
            {
                checksum ^= Payload[i];
            }

            Payload.CopyTo(Packet, 3);
            Packet[0] = header1;
            Packet[1] = header2;
            Packet[2] = size;
            Packet[Packet.Length - 1] = checksum;

            return Packet;
        }

        // ---- Background writer ------------------------------------------------------------------
        // A serial write can block for tens of milliseconds (measured: about 20 ms per telemetry packet on
        // Windows), which on the main thread is a dropped frame five times a second. The main thread only
        // queues the frame; this thread writes it. Only the newest frame is kept: an older one is useless.
        private static byte[] pendingFrame;
        private static readonly object sendLock = new object();
        private static readonly AutoResetEvent sendEvent = new AutoResetEvent(false);
        private static Thread SenderThread;
        private static volatile bool senderRun = false;
        private static int WriteCount;                 // guarded by sendLock
        private static double WriteMsSum, WriteMsMax;  // guarded by sendLock

        /// True once sending failed repeatedly; telemetry stops until the next flight scene.
        public static volatile Boolean LinkFailed = false;
        private static int SendFailures = 0;
        private const int MaxSendFailures = 3;

        /**
         * @brief Queues a packet for the background writer and returns immediately; never throws.
         *
         * The frame is built on the calling thread (cheap) and replaces any frame still waiting to be
         * written. The writer closes the port after MaxSendFailures consecutive failed writes and sets
         * LinkFailed, so a dead port costs one log line per failure instead of blocking the game.
         */
        public static void TrySendPacket(object anything)
        {
            try
            {
                byte[] frame = BuildFrame(anything);
                lock (sendLock) { pendingFrame = frame; }
                EnsureSender();
                sendEvent.Set();
            }
            catch (Exception e)
            {
                Debug.Log("KSPSerialIO: could not queue the packet: " + e.Message);
            }
        }

        /// @brief Starts the writer thread if it is not running.
        private static void EnsureSender()
        {
            if (SenderThread != null && SenderThread.IsAlive && senderRun) return;
            senderRun = true;
            SenderThread = new Thread(SenderWorker);
            SenderThread.IsBackground = true;
            SenderThread.Start();
        }

        /// @brief Writer thread body: writes the newest queued frame, one at a time, until senderRun is cleared.
        private static void SenderWorker()
        {
            while (senderRun)
            {
                sendEvent.WaitOne(500);

                byte[] frame;
                lock (sendLock) { frame = pendingFrame; pendingFrame = null; }
                if (frame == null || LinkFailed || Port == null || !Port.IsOpen) continue;

                long t0 = System.Diagnostics.Stopwatch.GetTimestamp();
                try
                {
                    Port.Write(frame, 0, frame.Length);
                    SendFailures = 0;
                }
                catch (Exception e)
                {
                    SendFailures++;
                    Debug.Log("KSPSerialIO: send failed (" + SendFailures + "/" + MaxSendFailures + "): " + e.Message);
                    if (SendFailures >= MaxSendFailures)
                    {
                        LinkFailed = true;
                        Debug.Log("KSPSerialIO: too many send failures, closing " + PortNumber + " and stopping telemetry");
                        try { Port.Close(); } catch (Exception) { }
                    }
                }
                double ms = (System.Diagnostics.Stopwatch.GetTimestamp() - t0) * 1000.0 / System.Diagnostics.Stopwatch.Frequency;
                lock (sendLock)
                {
                    WriteCount++;
                    WriteMsSum += ms;
                    if (ms > WriteMsMax) WriteMsMax = ms;
                }
            }
        }

        /// @brief Stops the writer thread (waits briefly for a write in progress; writes are bounded by the write timeout).
        private static void StopSender()
        {
            senderRun = false;
            sendEvent.Set();
            try { if (SenderThread != null) SenderThread.Join(1000); } catch (Exception) { }
        }

        /// @brief Returns and resets the statistics of the background writes (used by the perf log).
        public static void TakeWriteStats(out int count, out double avgMs, out double maxMs)
        {
            lock (sendLock)
            {
                count = WriteCount;
                avgMs = WriteCount > 0 ? WriteMsSum / WriteCount : 0;
                maxMs = WriteMsMax;
                WriteCount = 0; WriteMsSum = 0; WriteMsMax = 0;
            }
        }

        /// @brief Creates the SerialPort object and starts the background reader thread.
        private void Begin()
        {
            Port = new SerialPort(PortNumber, SettingsNStuff.BaudRate, Parity.None, 8, StopBits.One);
            // Bound how long a stuck port can block the main thread
            Port.WriteTimeout = 250;
            // Native-USB boards such as the Arduino UNO R4 only transmit while the host asserts DTR
            Port.DtrEnable = true;
            SerialThread = new Thread(SerialWorker);
            SerialThread.Start();
            while (!SerialThread.IsAlive);
        }

        /// @brief Unity per-frame callback: processes a packet completed by the worker thread, if any.
        private void Update()
        {
            if (NewPacketFlag)
            {
                InboundPacketHandler();
            }
        }

        /**
         * @brief Background thread body: keeps reading from the port until doSerialRead is cleared.
         *
         * Reads are issued one at a time (EndRead blocks), so ReceivedDataEvent and the parser
         * state are only ever touched by this thread. If the port is closed the thread sleeps and retries.
         */
        private void SerialWorker()
        {
            byte[] buffer = new byte[MaxPayloadSize + 4];
            Action SerialRead = null;
            Debug.Log("KSPSerialIO: Serial Worker thread started");
            SerialRead = delegate {
                try
                {
                    // One read at a time: EndRead blocks until this read completes, so
                    // the shared buffer and the parser state are only touched by this thread.
                    IAsyncResult ar = Port.BaseStream.BeginRead(buffer, 0, buffer.Length, null, null);
                    int actualLength = Port.BaseStream.EndRead(ar);
                    if (actualLength > 0)
                    {
                        byte[] received = new byte[actualLength];
                        Buffer.BlockCopy(buffer, 0, received, 0, actualLength);
                        ReceivedDataEvent(received, actualLength);
                    }
                }
                catch (InvalidOperationException)
                {
                    Debug.Log("KSPSerialIO: Trying to read port that isn't open. Sleeping");
                    Thread.Sleep(500);
                }
                catch (Exception exc)
                {
                    // Never let the parser die silently: reset it and keep reading
                    // Closing the port aborts the read that is pending: expected, no stack trace needed
                    Debug.Log("KSPSerialIO: SerialWorker read ended: " + exc.GetType().Name + ": " + exc.Message);
                    CurrentState = ReceiveStates.FIRSTHEADER;
                    Thread.Sleep(100);
                }
            };

            doSerialRead = true;
            while (doSerialRead)
            {
                SerialRead();
            }
            Debug.Log("KSPSerialIO: Serial worker thread shutting down.");
        }

        /**
         * @brief Feeds received bytes through the packet state machine.
         *
         * Implements the framing @c [0xBE][0xEF][size][payload][checksum]. On a valid checksum the
         * payload is copied to NewPacketBuffer and NewPacketFlag is raised; on any framing error the
         * parser drops back to FIRSTHEADER and resynchronizes on the next 0xBE 0xEF.
         * @param ReadBuffer Bytes just read from the port.
         * @param BufferLength Number of valid bytes in @p ReadBuffer.
         */
        private void ReceivedDataEvent(byte[] ReadBuffer, int BufferLength)
        {
            for (int x=0; x<BufferLength; x++)
            {
                switch(CurrentState)
                {
                    case ReceiveStates.FIRSTHEADER:
                        if (ReadBuffer[x] == 0xBE)
                        {
                            CurrentState = ReceiveStates.SECONDHEADER;
                        }
                        break;
                    case ReceiveStates.SECONDHEADER:
                        if (ReadBuffer[x] == 0xEF)
                        {
                            CurrentState = ReceiveStates.SIZE;
                        } else
                        {
                            CurrentState = ReceiveStates.FIRSTHEADER;
                        }
                        break;
                    case ReceiveStates.SIZE:
                        CurrentPacketLength = ReadBuffer[x];
                        CurrentBytesRead = 0;
                        // A zero-length packet is invalid; resync instead of overrunning PayloadBuffer
                        CurrentState = (CurrentPacketLength == 0) ? ReceiveStates.FIRSTHEADER : ReceiveStates.PAYLOAD;
                        break;
                    case ReceiveStates.PAYLOAD:
                        PayloadBuffer[CurrentBytesRead] = ReadBuffer[x];
                        CurrentBytesRead++;
                        if (CurrentBytesRead >= CurrentPacketLength)
                        {
                            CurrentState = ReceiveStates.CS;
                        }
                        break;
                    case ReceiveStates.CS:
                        if (CompareChecksum(ReadBuffer[x]))
                        {
                            SerialMutex.WaitOne();
                            Buffer.BlockCopy(PayloadBuffer, 0, NewPacketBuffer, 0, CurrentBytesRead);
                            NewPacketFlag = true;
                            SerialMutex.ReleaseMutex();
                            // Seedy hack: Handshake happens during scene
                            // load before Update() is ever called
                            if (!DisplayFound)
                            {
                                InboundPacketHandler();
                            }
                        }
                        CurrentState = ReceiveStates.FIRSTHEADER;
                        break;
                }
            }
        }

        /**
         * @brief Verifies the checksum of the packet in PayloadBuffer.
         * @param readCS Checksum byte received from the wire.
         * @return True if it equals the packet size XOR-ed with every payload byte.
         */
        private static Boolean CompareChecksum(byte readCS)
        {
            byte calcCS = CurrentPacketLength;
            for (int i=0; i<CurrentPacketLength; i++)
            {
                calcCS ^= PayloadBuffer[i];
            }
            return (calcCS == readCS);
        }

        //these are copied from the intarwebs, converts struct to byte array
        /// @brief Copies a packed struct into a new byte array (marshalling).
        private static byte[] StructureToByteArray(object obj)
        {
            int len = Marshal.SizeOf(obj);
            byte[] arr = new byte[len];
            IntPtr ptr = Marshal.AllocHGlobal(len);
            Marshal.StructureToPtr(obj, ptr, true);
            Marshal.Copy(ptr, arr, 0, len);
            Marshal.FreeHGlobal(ptr);
            return arr;
        }

        /// @brief Rebuilds a packed struct of the same type as @p obj from the first bytes of @p bytearray.
        private static object ByteArrayToStructure(byte[] bytearray, object obj)
        {
            int len = Marshal.SizeOf(obj);

            IntPtr i = Marshal.AllocHGlobal(len);

            Marshal.Copy(bytearray, 0, i, len);

            obj = Marshal.PtrToStructure(i, obj.GetType());

            Marshal.FreeHGlobal(i);

            return obj;
        }
        /*
          private static T ReadUsingMarshalUnsafe<T>(byte[] data) where T : struct
          {
          unsafe
          {
          fixed (byte* p = &data[0])
          {
          return (T)Marshal.PtrToStructure(new IntPtr(p), typeof(T));
          }
          }
          }
        */
        /// @brief Creates the packet instances and pre-fills their ids and the handshake bytes (1, 2, 3).
        void initializeDataPackets()
        {
            VData = new VesselData();
            VData.id = VDid;

            HPacket = new HandShakePacket();
            HPacket.id = HSPid;
            HPacket.M1 = 1;
            HPacket.M2 = 2;
            HPacket.M3 = 3;

            CPacket = new ControlPacket();

            VControls.ControlGroup = new Boolean[11];
            VControlsOld.ControlGroup = new Boolean[11];
        }

        /**
         * @brief Unity Awake: locates the controller.
         *
         * Normally runs a handshake on the default port and then on every serial port found in the
         * registry, keeping the first one that answers. The port is closed again afterwards because
         * KSPSerialIO::Start reopens it. If the controller was already found in an earlier flight,
         * only the port object and reader thread are recreated.
         */
        void Awake()
        {
            // Static state survives scene changes: give every flight a clean link
            LinkFailed = false;
            SendFailures = 0;

            if (DisplayFound)
            {
                Debug.Log("KSPSerialIO: running...");
                Begin();
            }
            else
            {
                String version = System.Reflection.Assembly.GetExecutingAssembly()
                    .GetName().Version.ToString();
                Debug.Log(String.Format("KSPSerialIO: Version {0}", version));
                Debug.Log("KSPSerialIO: Getting serial ports...");
                Debug.Log(String.Format("KSPSerialIO: Output packet size: {0}/{1}",
                                        Marshal.SizeOf(VData).ToString(),
                                        MaxPayloadSize));
                initializeDataPackets();

                try
                {
                    //Use registry hack to get a list of serial ports until we get system.io.ports
                    RegistryKey SerialCOMSKey = Registry.LocalMachine.OpenSubKey(@"HARDWARE\\DEVICEMAP\\SERIALCOMM\\");

                    Begin();

                    //print("KSPSerialIO: receive threshold " + Port.ReceivedBytesThreshold.ToString());

                    String[] PortNames;
                    if (SerialCOMSKey == null)
                    {
                        Debug.Log("KSPSerialIO: Dude do you even win32 serial port??");
                        PortNames = new String[1];
                        PortNames[0] = SettingsNStuff.DefaultPort;
                    }
                    else
                    {
                        String[] realports = SerialCOMSKey.GetValueNames();  // get list of all serial devices
                        PortNames = new String[realports.Length + 1];   // make a new list with 1 extra, we put the default port first
                        realports.CopyTo(PortNames, 1);
                    }

                    Debug.Log(String.Format("KSPSerialIO: Found {0} serial ports",
                                            PortNames.Length));

                    //look through all found ports for our display
                    for (int j=0; j < PortNames.Length; j++)
                    {
                        if (j == 0)  // try default port first
                        {
                            PortNumber = SettingsNStuff.DefaultPort;
                            Debug.Log("KSPSerialIO: trying default port " + PortNumber);
                        }
                        else
                        {
                            PortNumber = (string)SerialCOMSKey.GetValue(PortNames[j]);
                            // Bluetooth modems open fine but never answer, and the default port was already tried
                            if (PortNames[j].IndexOf("Bth", StringComparison.OrdinalIgnoreCase) >= 0
                                || PortNumber == SettingsNStuff.DefaultPort)
                            {
                                Debug.Log("KSPSerialIO: skipping port " + PortNames[j] + " - " + PortNumber);
                                continue;
                            }
                            Debug.Log("KSPSerialIO: trying port " + PortNames[j] + " - " + PortNumber);
                        }
                        Port.PortName = PortNumber;

                        if (!Port.IsOpen)
                        {
                            try
                            {
                                Port.Open();
                            }
                            catch (Exception e)
                            {
                                Debug.Log("Error opening serial port " + Port.PortName + ": " + e.Message);
                            }

                            //secret handshake
                            if (Port.IsOpen && (SettingsNStuff.HandshakeDisable == 0))
                            {
                                // A port that fails here must not abort the scan or stay open
                                try
                                {
                                    Thread.Sleep(SettingsNStuff.HandshakeDelay);
                                    //Port.DiscardOutBuffer();
                                    //Port.DiscardInBuffer();

                                    sendPacket(HPacket);

                                    //wait for reply
                                    int k = 0;
                                    while (k < 15 && !DisplayFound)
                                    {
                                        Thread.Sleep(100);
                                        k++;
                                    }
                                }
                                catch (Exception e)
                                {
                                    Debug.Log("KSPSerialIO: handshake failed on " + Port.PortName + ": " + e.Message);
                                }
                                finally
                                {
                                    try { Port.Close(); } catch (Exception) { }
                                }

                                if (DisplayFound)
                                {
                                    Debug.Log("KSPSerialIO-hp: found KSP Display at " + Port.PortName);
                                    break;
                                }
                                else
                                {
                                    Debug.Log("KSPSerialIO-hp: KSP Display not found");
                                }
                            }
                            else if (Port.IsOpen && (SettingsNStuff.HandshakeDisable == 1))
                            {
                                DisplayFound = true;                                    
                                Debug.Log("KSPSerialIO: Handshake disabled, using " + Port.PortName);
                                break;
                            }
                        }
                        else
                        {
                            Debug.Log("KSPSerialIO: " + PortNumber + "is already being used.");
                        }
                    }
                }
                catch (Exception e)
                {
                    print(e.Message);
                }
            }
        }

        /// @brief Logs the handshake reply received from the controller.
        private static void HandShake()
        {
            Debug.Log("KSPSerialIO: Handshake received - " + HPacket.M1.ToString() + HPacket.M2.ToString() + HPacket.M3.ToString());
        }

        /// @brief Decodes the latest ControlPacket into VControls and flags ControlReceived.
        private static void VesselControls()
        {

            VControls.SAS = BitMathByte(CPacket.MainControls, 7);
            VControls.RCS = BitMathByte(CPacket.MainControls, 6);
            VControls.Lights = BitMathByte(CPacket.MainControls, 5);
            VControls.Gear = BitMathByte(CPacket.MainControls, 4);
            VControls.Brakes = BitMathByte(CPacket.MainControls, 3);
            VControls.Precision = BitMathByte(CPacket.MainControls, 2);
            VControls.Abort = BitMathByte(CPacket.MainControls, 1);
            VControls.Stage = BitMathByte(CPacket.MainControls, 0);
            VControls.Pitch = (float)CPacket.Pitch / 1000.0F;
            VControls.Roll = (float)CPacket.Roll / 1000.0F;
            VControls.Yaw = (float)CPacket.Yaw / 1000.0F;
            VControls.TX = (float)CPacket.TX / 1000.0F;
            VControls.TY = (float)CPacket.TY / 1000.0F;
            VControls.TZ = (float)CPacket.TZ / 1000.0F;
            VControls.WheelSteer = (float)CPacket.WheelSteer / 1000.0F;
            VControls.Throttle = (float)CPacket.Throttle / 1000.0F;
            VControls.WheelThrottle = (float)CPacket.WheelThrottle / 1000.0F;
            VControls.SASMode = (int)CPacket.NavballSASMode & 0x0F;
            VControls.SpeedMode = (int)(CPacket.NavballSASMode >> 4);

            for (int j = 1; j <= 10; j++)
            {
                VControls.ControlGroup[j] = BitMathUshort(CPacket.ControlGroup, j);
            }

            ControlReceived = true;
            //Debug.Log("KSPSerialIO: ControlPacket received");
        }

        /// @brief Returns bit @p n of @p x.
        private static Boolean BitMathByte(byte x, int n)
        {
            return ((x >> n) & 1) == 1;
        }

        /// @brief Returns bit @p n of a 16-bit value.
        private static Boolean BitMathUshort(ushort x, int n)
        {
            return ((x >> n) & 1) == 1;
        }

        /// @brief Logs that a packet type has no handler.
        private static void Unimplemented()
        {
            Debug.Log("KSPSerialIO: Packet id unimplemented");
        }

        /// @brief Debug helper: logs the number of bytes waiting in the port's input buffer.
        private static void debug()
        {
            Debug.Log(Port.BytesToRead.ToString() + "BTR");
        }


        /**
         * @brief Sets or clears bit @p n of VData.ActionGroups.
         * @param n Bit index (see enumAG).
         * @param s New state of the bit.
         */
        public static void ControlStatus(int n, Boolean s)
        {
            if (s)
                VData.ActionGroups |= (UInt16)(1 << n);       // forces nth bit of x to be 1.  all other bits left alone.
            else
                VData.ActionGroups &= (UInt16)~(1 << n);      // forces nth bit of x to be 0.  all other bits left alone.
        }

        /// @brief Stops the reader thread and closes and disposes the port.
        public static void PortCleanup()
        {
            StopSender();
            if (KSPSerialPort.Port.IsOpen)
            {
                doSerialRead = false;
                KSPSerialPort.Port.Close();
                KSPSerialPort.Port.Dispose();
                Debug.Log("KSPSerialIO: Port closed");
            }
        }
    }

    /**
     * @brief Flight-scene component that exchanges data with the controller.
     *
     * Every refresh interval it samples the active vessel into KSPSerialPort::VData and sends it.
     * Every frame it applies any newly received controls (toggles, action groups, SAS and navball
     * modes) to the vessel, and AxisInput is hooked into the autopilot to inject the analog axes.
     */
    [KSPAddon(KSPAddon.Startup.Flight, false)]
    public class KSPSerialIO : MonoBehaviour
    {
        private double lastUpdate = 0.0f;
        private double deltaT = 1.0f;
        private double missionTime = 0;
        private double missionTimeOld = 0;
        private double theTime = 0;

        public double refreshrate = 1.0f;
        public static Vessel ActiveVessel = new Vessel();

        public Guid VesselIDOld;

        IOResource TempR = new IOResource();


        private ScreenMessageStyle KSPIOScreenStyle = ScreenMessageStyle.UPPER_RIGHT;

        #region performance log
        // Accumulated over a 10 s window when PerfLog = 1, then written to the log and reset
        private static int perfFrames, perfSamples;
        private static double perfUpdateSum, perfUpdateMax, perfSampleSum, perfSampleMax, perfSendSum, perfSendMax;
        private static double perfWindowStart = -1;
        // Process-wide counters at the start of the window, to report deltas
        private static System.Diagnostics.Process perfProc;
        private static double perfCpuStartMs;
        private static int perfGc0, perfGc1, perfGc2;

        [StructLayout(LayoutKind.Sequential)]
        private struct ProcessMemoryCounters
        {
            public uint cb;
            public uint PageFaultCount;
            public UIntPtr PeakWorkingSetSize;
            public UIntPtr WorkingSetSize;
            public UIntPtr QuotaPeakPagedPoolUsage;
            public UIntPtr QuotaPagedPoolUsage;
            public UIntPtr QuotaPeakNonPagedPoolUsage;
            public UIntPtr QuotaNonPagedPoolUsage;
            public UIntPtr PagefileUsage;
            public UIntPtr PeakPagefileUsage;
            public UIntPtr PrivateUsage;
        }

        [DllImport("psapi.dll")]
        private static extern bool GetProcessMemoryInfo(IntPtr process, out ProcessMemoryCounters counters, uint size);

        [DllImport("kernel32.dll")]
        private static extern IntPtr GetCurrentProcess();

        /// @brief Working set and private memory of this process in MB, read with the Windows API (-1 if unavailable).
        private static void PerfProcessMemory(out double workingSetMb, out double privateMb)
        {
            workingSetMb = privateMb = -1;
            try
            {
                ProcessMemoryCounters c;
                if (GetProcessMemoryInfo(GetCurrentProcess(), out c, (uint)Marshal.SizeOf(typeof(ProcessMemoryCounters))))
                {
                    workingSetMb = c.WorkingSetSize.ToUInt64() / 1048576.0;
                    privateMb = c.PrivateUsage.ToUInt64() / 1048576.0;
                }
            }
            catch (Exception) { }
        }

        /// @brief Milliseconds elapsed since a Stopwatch timestamp.
        private static double PerfMs(long start)
        {
            return (System.Diagnostics.Stopwatch.GetTimestamp() - start) * 1000.0 / System.Diagnostics.Stopwatch.Frequency;
        }

        /// @brief Records one telemetry sample: the time spent reading the vessel and the time spent sending the packet.
        private static void PerfRecordSample(double sampleMs, double sendMs)
        {
            perfSamples++;
            perfSampleSum += sampleMs;
            if (sampleMs > perfSampleMax) perfSampleMax = sampleMs;
            perfSendSum += sendMs;
            if (sendMs > perfSendMax) perfSendMax = sendMs;
        }

        /// @brief Takes the process counters that the next summary is measured against.
        private static void PerfStartWindow(double now)
        {
            perfWindowStart = now;
            try
            {
                if (perfProc == null) perfProc = System.Diagnostics.Process.GetCurrentProcess();
                perfProc.Refresh();
                perfCpuStartMs = perfProc.TotalProcessorTime.TotalMilliseconds;
            }
            catch (Exception) { perfCpuStartMs = -1; }
            perfGc0 = GC.CollectionCount(0);
            perfGc1 = GC.CollectionCount(1);
            perfGc2 = GC.CollectionCount(2);
        }

        /**
         * @brief Records the cost of one Update() call and, every 10 s, logs one summary line.
         *
         * The line is a list of key=value pairs so it can be parsed by a script. It covers the whole game
         * (frame time, process memory, CPU, garbage collections) and the plugin (Update, sampling and
         * sending time). upd_share_pct is the plugin's Update time over the wall time of the window, so it
         * is the fraction of every frame the plugin takes; AxisInput and the reader thread are not included.
         */
        private static void PerfRecordFrame(long start, int parts)
        {
            double ms = PerfMs(start);
            perfFrames++;
            perfUpdateSum += ms;
            if (ms > perfUpdateMax) perfUpdateMax = ms;

            double now = Time.unscaledTime;
            if (perfWindowStart < 0) { PerfStartWindow(now); return; }
            double window = now - perfWindowStart;
            if (window < 10.0) return;

            double frameMs = window * 1000.0 / perfFrames;
            double ws, priv, cpuPct = -1;
            PerfProcessMemory(out ws, out priv);
            int writes; double writeAvg, writeMax;
            KSPSerialPort.TakeWriteStats(out writes, out writeAvg, out writeMax);
            try
            {
                perfProc.Refresh();
                if (perfCpuStartMs >= 0)
                    cpuPct = (perfProc.TotalProcessorTime.TotalMilliseconds - perfCpuStartMs) / (window * 1000.0) * 100.0;
            }
            catch (Exception) { }

            Debug.Log(String.Format(System.Globalization.CultureInfo.InvariantCulture,
                "KSPSerialIO: perf window_s={0:F1} parts={1} frames={2} frame_ms={3:F2} fps={4:F1} " +
                "upd_avg_ms={5:F3} upd_max_ms={6:F2} upd_share_pct={7:F3} " +
                "samples={8} sample_avg_ms={9:F3} sample_max_ms={10:F2} send_avg_ms={11:F3} send_max_ms={12:F2} " +
                "writes={20} write_avg_ms={21:F2} write_max_ms={22:F2} " +
                "ws_mb={13:F0} private_mb={14:F0} gc_heap_mb={15:F0} gc0={16} gc1={17} gc2={18} proc_cpu_pct={19:F0}",
                window, parts, perfFrames, frameMs, 1000.0 / frameMs,
                perfUpdateSum / perfFrames, perfUpdateMax, perfUpdateSum / (window * 1000.0) * 100.0,
                perfSamples, perfSamples > 0 ? perfSampleSum / perfSamples : 0, perfSampleMax,
                perfSamples > 0 ? perfSendSum / perfSamples : 0, perfSendMax,
                ws, priv, GC.GetTotalMemory(false) / 1048576.0,
                GC.CollectionCount(0) - perfGc0, GC.CollectionCount(1) - perfGc1, GC.CollectionCount(2) - perfGc2, cpuPct,
                writes, writeAvg, writeMax));

            perfFrames = perfSamples = 0;
            perfUpdateSum = perfUpdateMax = perfSampleSum = perfSampleMax = perfSendSum = perfSendMax = 0;
            PerfStartWindow(now);
        }
        #endregion

        /// @brief Unity Awake: shows a debug message and reads the telemetry refresh rate.
        void Awake()
        {
            ScreenMessages.PostScreenMessage("IO awake", 10f, KSPIOScreenStyle);
            refreshrate = SettingsNStuff.refreshrate;
        }

        /**
         * @brief Unity Start: opens the port, hooks AxisInput into the active vessel and syncs the initial control state.
         *
         * Does nothing but show a message if no controller was found during KSPSerialPort::Awake.
         */
        void Start()
        {
            if (KSPSerialPort.DisplayFound)
            {
                if (!KSPSerialPort.Port.IsOpen)
                {
                    ScreenMessages.PostScreenMessage("Starting serial port " + KSPSerialPort.Port.PortName, 10f, KSPIOScreenStyle);

                    try
                    {
                        KSPSerialPort.Port.Open();
                        Thread.Sleep(SettingsNStuff.HandshakeDelay);
                    }
                    catch (Exception e)
                    {
                        ScreenMessages.PostScreenMessage("Error opening serial port " + KSPSerialPort.Port.PortName, 10f, KSPIOScreenStyle);
                        ScreenMessages.PostScreenMessage(e.Message, 10f, KSPIOScreenStyle);
                    }
                }
                else
                {
                    ScreenMessages.PostScreenMessage("Using serial port " + KSPSerialPort.Port.PortName, 10f, KSPIOScreenStyle);

                    if (SettingsNStuff.HandshakeDisable == 1)
                        ScreenMessages.PostScreenMessage("Handshake disabled");
                }

                Thread.Sleep(200);

                ActiveVessel.OnPostAutopilotUpdate -= AxisInput;
                ActiveVessel = FlightGlobals.ActiveVessel;
                ActiveVessel.OnPostAutopilotUpdate += AxisInput;

                //sync inputs at start
                ActiveVessel.ActionGroups.SetGroup(KSPActionGroup.RCS, KSPSerialPort.VControls.RCS);
                ActiveVessel.ActionGroups.SetGroup(KSPActionGroup.SAS, KSPSerialPort.VControls.SAS);
                ActiveVessel.ActionGroups.SetGroup(KSPActionGroup.Light, KSPSerialPort.VControls.Lights);
                ActiveVessel.ActionGroups.SetGroup(KSPActionGroup.Gear, KSPSerialPort.VControls.Gear);
                ActiveVessel.ActionGroups.SetGroup(KSPActionGroup.Brakes, KSPSerialPort.VControls.Brakes);
                ActiveVessel.ActionGroups.SetGroup(KSPActionGroup.Abort, KSPSerialPort.VControls.Abort);
                ActiveVessel.ActionGroups.SetGroup(KSPActionGroup.Stage, KSPSerialPort.VControls.Stage);

                ActiveVessel.ActionGroups.SetGroup(KSPActionGroup.Custom01, KSPSerialPort.VControls.ControlGroup[1]);
                ActiveVessel.ActionGroups.SetGroup(KSPActionGroup.Custom02, KSPSerialPort.VControls.ControlGroup[2]);
                ActiveVessel.ActionGroups.SetGroup(KSPActionGroup.Custom03, KSPSerialPort.VControls.ControlGroup[3]);
                ActiveVessel.ActionGroups.SetGroup(KSPActionGroup.Custom04, KSPSerialPort.VControls.ControlGroup[4]);
                ActiveVessel.ActionGroups.SetGroup(KSPActionGroup.Custom05, KSPSerialPort.VControls.ControlGroup[5]);
                ActiveVessel.ActionGroups.SetGroup(KSPActionGroup.Custom06, KSPSerialPort.VControls.ControlGroup[6]);
                ActiveVessel.ActionGroups.SetGroup(KSPActionGroup.Custom07, KSPSerialPort.VControls.ControlGroup[7]);
                ActiveVessel.ActionGroups.SetGroup(KSPActionGroup.Custom08, KSPSerialPort.VControls.ControlGroup[8]);
                ActiveVessel.ActionGroups.SetGroup(KSPActionGroup.Custom09, KSPSerialPort.VControls.ControlGroup[9]);
                ActiveVessel.ActionGroups.SetGroup(KSPActionGroup.Custom10, KSPSerialPort.VControls.ControlGroup[10]);

            }
            else
            {
                ScreenMessages.PostScreenMessage("KerbalController not found", 10f, KSPIOScreenStyle);
            }
        }

        /**
         * @brief Unity per-frame callback: sends telemetry and applies received controls.
         *
         * Runs only while a vessel is active and the port is open. It follows vessel switches by
         * moving the AxisInput hook to the new vessel.
         */
        void Update()
        {
            long perfStart = SettingsNStuff.PerfLog == 1 ? System.Diagnostics.Stopwatch.GetTimestamp() : 0;

            // Without a controller (or after the link failed) there is nothing to send, even if a port was left open
            if (FlightGlobals.ActiveVessel != null && KSPSerialPort.DisplayFound && !KSPSerialPort.LinkFailed
                && KSPSerialPort.Port.IsOpen)
            {
                //Debug.Log("KSPSerialIO: 1");
                //If the current active vessel is not what we were using, we need to remove controls from the old 
                //vessel and attache it to the current one
                if (ActiveVessel != null && ActiveVessel.id != FlightGlobals.ActiveVessel.id)
                {
                    ActiveVessel.OnPostAutopilotUpdate -= AxisInput;
                    ActiveVessel = FlightGlobals.ActiveVessel;
                    ActiveVessel.OnPostAutopilotUpdate += AxisInput;
                    //sync some inputs on vessel switch
                    ActiveVessel.ActionGroups.SetGroup(KSPActionGroup.RCS, KSPSerialPort.VControls.RCS);
                    ActiveVessel.ActionGroups.SetGroup(KSPActionGroup.SAS, KSPSerialPort.VControls.SAS);
                    Debug.Log("KSPSerialIO: ActiveVessel changed");
                }
                else
                {
                    ActiveVessel = FlightGlobals.ActiveVessel;
                }

                #region outputs
                // Throttle outgoing telemetry to the configured refresh rate (real time, unaffected by time warp)
                theTime = Time.unscaledTime;
                if ((theTime - lastUpdate) > refreshrate)
                {
                    //Debug.Log("KSPSerialIO: 2");

                    lastUpdate = theTime;
                    long perfSampleStart = SettingsNStuff.PerfLog == 1 ? System.Diagnostics.Stopwatch.GetTimestamp() : 0;

                    // Engines currently burning; used for the per-stage ("S") resource totals
                    List<Part> ActiveEngines = new List<Part>();
                    ActiveEngines = GetListOfActivatedEngines(ActiveVessel);

                    KSPSerialPort.VData.AP = (float)ActiveVessel.orbit.ApA;
                    KSPSerialPort.VData.PE = (float)ActiveVessel.orbit.PeA;
                    KSPSerialPort.VData.SemiMajorAxis = (float)ActiveVessel.orbit.semiMajorAxis;
                    KSPSerialPort.VData.SemiMinorAxis = (float)ActiveVessel.orbit.semiMinorAxis;
                    KSPSerialPort.VData.e = (float)ActiveVessel.orbit.eccentricity;
                    KSPSerialPort.VData.inc = (float)ActiveVessel.orbit.inclination;
                    KSPSerialPort.VData.VVI = (float)ActiveVessel.verticalSpeed;
                    KSPSerialPort.VData.G = (float)ActiveVessel.geeForce;
                    KSPSerialPort.VData.TAp = (int)Math.Round(ActiveVessel.orbit.timeToAp);
                    KSPSerialPort.VData.TPe = (int)Math.Round(ActiveVessel.orbit.timeToPe);
                    KSPSerialPort.VData.Density = (float)ActiveVessel.atmDensity;
                    KSPSerialPort.VData.TrueAnomaly = (float)ActiveVessel.orbit.trueAnomaly;
                    KSPSerialPort.VData.period = (int)Math.Round(ActiveVessel.orbit.period);

                    //Debug.Log("KSPSerialIO: 3");
                    // Altitudes: ASL comes from the body, AGL is derived from terrain altitude; RAlt reports the lower one
                    double ASL = ActiveVessel.mainBody.GetAltitude(ActiveVessel.CoM);
                    double AGL = (ASL - ActiveVessel.terrainAltitude);

                    if (AGL < ASL)
                        KSPSerialPort.VData.RAlt = (float)AGL;
                    else
                        KSPSerialPort.VData.RAlt = (float)ASL;

                    KSPSerialPort.VData.Alt = (float)ASL;
                    KSPSerialPort.VData.Vsurf = (float)ActiveVessel.srfSpeed;
                    KSPSerialPort.VData.Lat = (float)ActiveVessel.latitude;
                    KSPSerialPort.VData.Lon = (float)ActiveVessel.longitude;

                    // Resource totals for the whole vessel (Tot = capacity) and for the active engines' feed network ("S")
                    TempR = GetResourceTotal(ActiveVessel, "LiquidFuel");
                    KSPSerialPort.VData.LiquidFuelTot = TempR.Max;
                    KSPSerialPort.VData.LiquidFuel = TempR.Current;

                    KSPSerialPort.VData.LiquidFuelTotS = (float)ProspectForResourceMax("LiquidFuel", ActiveEngines);
                    KSPSerialPort.VData.LiquidFuelS = (float)ProspectForResource("LiquidFuel", ActiveEngines);

                    TempR = GetResourceTotal(ActiveVessel, "Oxidizer");
                    KSPSerialPort.VData.OxidizerTot = TempR.Max;
                    KSPSerialPort.VData.Oxidizer = TempR.Current;

                    KSPSerialPort.VData.OxidizerTotS = (float)ProspectForResourceMax("Oxidizer", ActiveEngines);
                    KSPSerialPort.VData.OxidizerS = (float)ProspectForResource("Oxidizer", ActiveEngines);

                    TempR = GetResourceTotal(ActiveVessel, "ElectricCharge");
                    KSPSerialPort.VData.EChargeTot = TempR.Max;
                    KSPSerialPort.VData.ECharge = TempR.Current;
                    TempR = GetResourceTotal(ActiveVessel, "MonoPropellant");
                    KSPSerialPort.VData.MonoPropTot = TempR.Max;
                    KSPSerialPort.VData.MonoProp = TempR.Current;
                    TempR = GetResourceTotal(ActiveVessel, "IntakeAir");
                    KSPSerialPort.VData.IntakeAirTot = TempR.Max;
                    KSPSerialPort.VData.IntakeAir = TempR.Current;
                    TempR = GetResourceTotal(ActiveVessel, "SolidFuel");
                    KSPSerialPort.VData.SolidFuelTot = TempR.Max;
                    KSPSerialPort.VData.SolidFuel = TempR.Current;
                    TempR = GetResourceTotal(ActiveVessel, "XenonGas");
                    KSPSerialPort.VData.XenonGasTot = TempR.Max;
                    KSPSerialPort.VData.XenonGas = TempR.Current;

                    // Mission time and its delta since the previous packet
                    missionTime = ActiveVessel.missionTime;
                    deltaT = missionTime - missionTimeOld;
                    missionTimeOld = missionTime;

                    KSPSerialPort.VData.MissionTime = (UInt32)Math.Round(missionTime);
                    KSPSerialPort.VData.deltaTime = (float)deltaT;

                    KSPSerialPort.VData.VOrbit = (float)ActiveVessel.orbit.GetVel().magnitude;

                    //Debug.Log("KSPSerialIO: 4");

                    // Local frame for the navball vector headings: up, north and east at the vessel
                    Vector3d navCoM = ActiveVessel.CoM;
                    Vector3d navUp = (navCoM - ActiveVessel.mainBody.position).normalized;
                    Vector3d navNorth = Vector3d.Exclude(navUp, (ActiveVessel.mainBody.position + ActiveVessel.mainBody.transform.up * (float)ActiveVessel.mainBody.Radius) - navCoM).normalized;
                    Vector3d navEast = Vector3d.Cross(navUp, navNorth);
                    NavHeading navZero = new NavHeading();
                    NavHeading navManeuver = navZero;
                    NavHeading navTarget = navZero;

                    // Maneuver node data, left at 0 when there is no node
                    KSPSerialPort.VData.MNTime = 0;
                    KSPSerialPort.VData.MNDeltaV = 0;

                    if (ActiveVessel.patchedConicSolver != null)
                    {
                        if (ActiveVessel.patchedConicSolver.maneuverNodes != null)
                        {
                            if (ActiveVessel.patchedConicSolver.maneuverNodes.Count > 0)
                            {
                                KSPSerialPort.VData.MNTime = (UInt32)Math.Round(ActiveVessel.patchedConicSolver.maneuverNodes[0].UT - Planetarium.GetUniversalTime());
                                //ScreenMessages.PostScreenMessage(KSPSerialPort.VData.MNTime.ToString());
                                //KSPSerialPort.VData.MNDeltaV = (float)ActiveVessel.patchedConicSolver.maneuverNodes[0].DeltaV.magnitude;
                                KSPSerialPort.VData.MNDeltaV = (float)ActiveVessel.patchedConicSolver.maneuverNodes[0].GetBurnVector(ActiveVessel.patchedConicSolver.maneuverNodes[0].patch).magnitude; //Added JS

                                navManeuver = WorldVecToNavHeading(navUp, navNorth, navEast, ActiveVessel.patchedConicSolver.maneuverNodes[0].GetBurnVector(ActiveVessel.patchedConicSolver.maneuverNodes[0].patch));
                            }
                        }
                    }
                    KSPSerialPort.VData.ManeuverHeading = FloatAngleToFixed_16(navManeuver.Heading);
                    KSPSerialPort.VData.ManeuverPitch = FloatAngleToFixed_16(navManeuver.Pitch);

                    //Debug.Log("KSPSerialIO: 5");

                    // Attitude relative to the local surface, converted to the signed ranges the firmware expects
                    Quaternion attitude = updateHeadingPitchRollField(ActiveVessel);

                    KSPSerialPort.VData.Roll = (float)((attitude.eulerAngles.z > 180) ? (attitude.eulerAngles.z - 360.0) : attitude.eulerAngles.z);
                    KSPSerialPort.VData.Pitch = (float)((attitude.eulerAngles.x > 180) ? (360.0 - attitude.eulerAngles.x) : -attitude.eulerAngles.x);
                    KSPSerialPort.VData.Heading = (float)attitude.eulerAngles.y;

                    // Pack the action group states into VData.ActionGroups (bit order in enumAG)
                    KSPSerialPort.ControlStatus((int)enumAG.SAS, ActiveVessel.ActionGroups[KSPActionGroup.SAS]);
                    KSPSerialPort.ControlStatus((int)enumAG.RCS, ActiveVessel.ActionGroups[KSPActionGroup.RCS]);
                    KSPSerialPort.ControlStatus((int)enumAG.Light, ActiveVessel.ActionGroups[KSPActionGroup.Light]);
                    KSPSerialPort.ControlStatus((int)enumAG.Gear, ActiveVessel.ActionGroups[KSPActionGroup.Gear]);
                    KSPSerialPort.ControlStatus((int)enumAG.Brakes, ActiveVessel.ActionGroups[KSPActionGroup.Brakes]);
                    KSPSerialPort.ControlStatus((int)enumAG.Abort, ActiveVessel.ActionGroups[KSPActionGroup.Abort]);
                    KSPSerialPort.ControlStatus((int)enumAG.Custom01, ActiveVessel.ActionGroups[KSPActionGroup.Custom01]);
                    KSPSerialPort.ControlStatus((int)enumAG.Custom02, ActiveVessel.ActionGroups[KSPActionGroup.Custom02]);
                    KSPSerialPort.ControlStatus((int)enumAG.Custom03, ActiveVessel.ActionGroups[KSPActionGroup.Custom03]);
                    KSPSerialPort.ControlStatus((int)enumAG.Custom04, ActiveVessel.ActionGroups[KSPActionGroup.Custom04]);
                    KSPSerialPort.ControlStatus((int)enumAG.Custom05, ActiveVessel.ActionGroups[KSPActionGroup.Custom05]);
                    KSPSerialPort.ControlStatus((int)enumAG.Custom06, ActiveVessel.ActionGroups[KSPActionGroup.Custom06]);
                    KSPSerialPort.ControlStatus((int)enumAG.Custom07, ActiveVessel.ActionGroups[KSPActionGroup.Custom07]);
                    KSPSerialPort.ControlStatus((int)enumAG.Custom08, ActiveVessel.ActionGroups[KSPActionGroup.Custom08]);
                    KSPSerialPort.ControlStatus((int)enumAG.Custom09, ActiveVessel.ActionGroups[KSPActionGroup.Custom09]);
                    KSPSerialPort.ControlStatus((int)enumAG.Custom10, ActiveVessel.ActionGroups[KSPActionGroup.Custom10]);

                    if (ActiveVessel.orbit.referenceBody != null)
                    {
                        KSPSerialPort.VData.SOINumber = GetSOINumber(ActiveVessel.orbit.referenceBody.name);
                    }

                    KSPSerialPort.VData.MaxOverHeat = GetMaxOverHeat(ActiveVessel);
                    KSPSerialPort.VData.MachNumber = (float)ActiveVessel.mach;
                    KSPSerialPort.VData.IAS = (float)ActiveVessel.indicatedAirSpeed;

                    KSPSerialPort.VData.CurrentStage = (byte)StageManager.CurrentStage;
                    KSPSerialPort.VData.TotalStage = (byte)StageManager.StageCount;

                    //target distance and velocity stuff                    

                    KSPSerialPort.VData.TargetDist = 0;
                    KSPSerialPort.VData.TargetV = 0;

                    if (TargetExists())
                    {
                        KSPSerialPort.VData.TargetDist = (float)Vector3.Distance(FlightGlobals.fetch.VesselTarget.GetVessel().transform.position, ActiveVessel.transform.position);
                        KSPSerialPort.VData.TargetV = (float)FlightGlobals.ship_tgtVelocity.magnitude;
                        navTarget = WorldVecToNavHeading(navUp, navNorth, navEast, ActiveVessel.targetObject.GetTransform().position - ActiveVessel.transform.position);
                    }
                    KSPSerialPort.VData.TargetHeading = FloatAngleToFixed_16(navTarget.Heading);
                    KSPSerialPort.VData.TargetPitch = FloatAngleToFixed_16(navTarget.Pitch);

                    // Prograde of the current navball mode, and the orbit normal
                    Vector3d navPrograde = new Vector3d(0, 0, 0);
                    switch (FlightGlobals.speedDisplayMode)
                    {
                        case FlightGlobals.SpeedDisplayModes.Surface:
                            navPrograde = ActiveVessel.srf_velocity.normalized;
                            break;
                        case FlightGlobals.SpeedDisplayModes.Orbit:
                            navPrograde = ActiveVessel.obt_velocity.normalized;
                            break;
                        case FlightGlobals.SpeedDisplayModes.Target:
                            navPrograde = FlightGlobals.ship_tgtVelocity;
                            break;
                    }
                    NavHeading prograde = WorldVecToNavHeading(navUp, navNorth, navEast, navPrograde);
                    KSPSerialPort.VData.ProgradeHeading = FloatAngleToFixed_16(prograde.Heading);
                    KSPSerialPort.VData.ProgradePitch = FloatAngleToFixed_16(prograde.Pitch);
                    KSPSerialPort.VData.NormalHeading = FloatAngleToFixed_16(WorldVecToNavHeading(navUp, navNorth, navEast, Vector3d.Cross(ActiveVessel.obt_velocity.normalized, navUp)).Heading);


                    // Navball/SAS byte: high nibble = speed display mode + 1, low nibble = autopilot mode + 1 (only while SAS is on)
                    KSPSerialPort.VData.NavballSASMode = (byte)(((int)FlightGlobals.speedDisplayMode + 1) << 4); //get navball speed display mode
                    if (ActiveVessel.ActionGroups[KSPActionGroup.SAS])
                    {
                        KSPSerialPort.VData.NavballSASMode = (byte)(((int)FlightGlobals.ActiveVessel.Autopilot.Mode + 1) | KSPSerialPort.VData.NavballSASMode);
                    }

                    #region debugjunk

                    /*
                      Debug.Log("KSPSerialIO: Stage " + KSPSerialPort.VData.CurrentStage.ToString() + ' ' +
                      KSPSerialPort.VData.TotalStage.ToString()); 
                      Debug.Log("KSPSerialIO: Overheat " + KSPSerialPort.VData.MaxOverHeat.ToString());
                      Debug.Log("KSPSerialIO: Mach " + KSPSerialPort.VData.MachNumber.ToString());
                      Debug.Log("KSPSerialIO: IAS " + KSPSerialPort.VData.IAS.ToString());
                    
                      Debug.Log("KSPSerialIO: SOI " + ActiveVessel.orbit.referenceBody.name + KSPSerialPort.VData.SOINumber.ToString());
                    
                      ScreenMessages.PostScreenMessage(KSPSerialPort.VData.OxidizerS.ToString() + "/" + KSPSerialPort.VData.OxidizerTotS +
                      "   " + KSPSerialPort.VData.Oxidizer.ToString() + "/" + KSPSerialPort.VData.OxidizerTot);
                    */
                    //KSPSerialPort.VData.Roll = Mathf.Atan2(2 * (x * y + w * z), w * w + x * x - y * y - z * z) * 180 / Mathf.PI;
                    //KSPSerialPort.VData.Pitch = Mathf.Atan2(2 * (y * z + w * x), w * w - x * x - y * y + z * z) * 180 / Mathf.PI;
                    //KSPSerialPort.VData.Heading = Mathf.Asin(-2 * (x * z - w * y)) *180 / Mathf.PI;
                    //Debug.Log("KSPSerialIO: Roll    " + KSPSerialPort.VData.Roll.ToString());
                    //Debug.Log("KSPSerialIO: Pitch   " + KSPSerialPort.VData.Pitch.ToString());
                    //Debug.Log("KSPSerialIO: Heading " + KSPSerialPort.VData.Heading.ToString());
                    //Debug.Log("KSPSerialIO: VOrbit" + KSPSerialPort.VData.VOrbit.ToString());
                    //ScreenMessages.PostScreenMessage(ActiveVessel.ActionGroups[KSPActionGroup.RCS].ToString());
                    //Debug.Log("KSPSerialIO: MNTime" + KSPSerialPort.VData.MNTime.ToString() + " MNDeltaV" + KSPSerialPort.VData.MNDeltaV.ToString());
                    //Debug.Log("KSPSerialIO: Time" + KSPSerialPort.VData.MissionTime.ToString() + " Delta Time" + KSPSerialPort.VData.deltaTime.ToString());
                    //Debug.Log("KSPSerialIO: Throttle = " + KSPSerialPort.CPacket.Throttle.ToString());
                    //ScreenMessages.PostScreenMessage(KSPSerialPort.VData.Fuelp.ToString());
                    //ScreenMessages.PostScreenMessage(KSPSerialPort.VData.RAlt.ToString());
                    //KSPSerialPort.Port.WriteLine("Success!");
                    /*
                    ScreenMessages.PostScreenMessage(KSPSerialPort.VData.LiquidFuelS.ToString() + "/" + KSPSerialPort.VData.LiquidFuelTotS +
                        "   " + KSPSerialPort.VData.LiquidFuel.ToString() + "/" + KSPSerialPort.VData.LiquidFuelTot);
                    
                    ScreenMessages.PostScreenMessage("MNTime " + KSPSerialPort.VData.MNTime.ToString() + " MNDeltaV " + KSPSerialPort.VData.MNDeltaV.ToString());
                    ScreenMessages.PostScreenMessage("TargetDist " + KSPSerialPort.VData.TargetDist.ToString() + " TargetV " + KSPSerialPort.VData.TargetV.ToString());
                     */
                    #endregion
                    // Send the telemetry packet
                    long perfSendStart = SettingsNStuff.PerfLog == 1 ? System.Diagnostics.Stopwatch.GetTimestamp() : 0;
                    KSPSerialPort.TrySendPacket(KSPSerialPort.VData);
                    if (SettingsNStuff.PerfLog == 1) PerfRecordSample(PerfMs(perfSampleStart) - PerfMs(perfSendStart), PerfMs(perfSendStart));
                } //end refresh
                #endregion
                #region inputs
                // Apply the controls from the last ControlPacket. Only values that changed (VControls vs VControlsOld)
                // are pushed to the vessel, so KSP's own UI can still toggle things between packets.
                if (KSPSerialPort.ControlReceived)
                {
                    /*

                    ScreenMessages.PostScreenMessage("Nav Mode " + KSPSerialPort.CPacket.NavballSASMode.ToString());
                    
                     ScreenMessages.PostScreenMessage("SAS: " + KSPSerialPort.VControls.SAS.ToString() +
                     ", RCS: " + KSPSerialPort.VControls.RCS.ToString() +
                     ", Lights: " + KSPSerialPort.VControls.Lights.ToString() +
                     ", Gear: " + KSPSerialPort.VControls.Gear.ToString() +
                     ", Brakes: " + KSPSerialPort.VControls.Brakes.ToString() +
                     ", Precision: " + KSPSerialPort.VControls.Precision.ToString() +
                     ", Abort: " + KSPSerialPort.VControls.Abort.ToString() +
                     ", Stage: " + KSPSerialPort.VControls.Stage.ToString(), 10f, KSPIOScreenStyle);
                    
                     Debug.Log("KSPSerialIO: SAS: " + KSPSerialPort.VControls.SAS.ToString() +
                     ", RCS: " + KSPSerialPort.VControls.RCS.ToString() +
                     ", Lights: " + KSPSerialPort.VControls.Lights.ToString() +
                     ", Gear: " + KSPSerialPort.VControls.Gear.ToString() +
                     ", Brakes: " + KSPSerialPort.VControls.Brakes.ToString() +
                     ", Precision: " + KSPSerialPort.VControls.Precision.ToString() +
                     ", Abort: " + KSPSerialPort.VControls.Abort.ToString() +
                     ", Stage: " + KSPSerialPort.VControls.Stage.ToString());
                     */

                    //if (FlightInputHandler.RCSLock != KSPSerialPort.VControls.RCS)
                    if (KSPSerialPort.VControls.RCS != KSPSerialPort.VControlsOld.RCS)
                    {
                        ActiveVessel.ActionGroups.SetGroup(KSPActionGroup.RCS, KSPSerialPort.VControls.RCS);
                        KSPSerialPort.VControlsOld.RCS = KSPSerialPort.VControls.RCS;
                        //ScreenMessages.PostScreenMessage("RCS: " + KSPSerialPort.VControls.RCS.ToString(), 10f, KSPIOScreenStyle);
                    }

                    //if (ActiveVessel.ctrlState.killRot != KSPSerialPort.VControls.SAS)
                    if (KSPSerialPort.VControls.SAS != KSPSerialPort.VControlsOld.SAS)
                    {
                        ActiveVessel.ActionGroups.SetGroup(KSPActionGroup.SAS, KSPSerialPort.VControls.SAS);
                        KSPSerialPort.VControlsOld.SAS = KSPSerialPort.VControls.SAS;
                        //ScreenMessages.PostScreenMessage("SAS: " + KSPSerialPort.VControls.SAS.ToString(), 10f, KSPIOScreenStyle);
                    }

                    if (KSPSerialPort.VControls.Lights != KSPSerialPort.VControlsOld.Lights)
                    {
                        ActiveVessel.ActionGroups.SetGroup(KSPActionGroup.Light, KSPSerialPort.VControls.Lights);
                        KSPSerialPort.VControlsOld.Lights = KSPSerialPort.VControls.Lights;
                    }

                    if (KSPSerialPort.VControls.Gear != KSPSerialPort.VControlsOld.Gear)
                    {
                        ActiveVessel.ActionGroups.SetGroup(KSPActionGroup.Gear, KSPSerialPort.VControls.Gear);
                        KSPSerialPort.VControlsOld.Gear = KSPSerialPort.VControls.Gear;
                    }

                    if (KSPSerialPort.VControls.Brakes != KSPSerialPort.VControlsOld.Brakes)
                    {
                        ActiveVessel.ActionGroups.SetGroup(KSPActionGroup.Brakes, KSPSerialPort.VControls.Brakes);
                        KSPSerialPort.VControlsOld.Brakes = KSPSerialPort.VControls.Brakes;
                    }

                    if (KSPSerialPort.VControls.Abort != KSPSerialPort.VControlsOld.Abort)
                    {
                        ActiveVessel.ActionGroups.SetGroup(KSPActionGroup.Abort, KSPSerialPort.VControls.Abort);
                        KSPSerialPort.VControlsOld.Abort = KSPSerialPort.VControls.Abort;
                    }

                    if (KSPSerialPort.VControls.Stage != KSPSerialPort.VControlsOld.Stage)
                    {
                        if (KSPSerialPort.VControls.Stage)
                            StageManager.ActivateNextStage();

                        ActiveVessel.ActionGroups.SetGroup(KSPActionGroup.Stage, KSPSerialPort.VControls.Stage);
                        KSPSerialPort.VControlsOld.Stage = KSPSerialPort.VControls.Stage;
                    }

                    //================ control groups

                    if (KSPSerialPort.VControls.ControlGroup[1] != KSPSerialPort.VControlsOld.ControlGroup[1])
                    {
                        ActiveVessel.ActionGroups.SetGroup(KSPActionGroup.Custom01, KSPSerialPort.VControls.ControlGroup[1]);
                        KSPSerialPort.VControlsOld.ControlGroup[1] = KSPSerialPort.VControls.ControlGroup[1];
                    }

                    if (KSPSerialPort.VControls.ControlGroup[2] != KSPSerialPort.VControlsOld.ControlGroup[2])
                    {
                        ActiveVessel.ActionGroups.SetGroup(KSPActionGroup.Custom02, KSPSerialPort.VControls.ControlGroup[2]);
                        KSPSerialPort.VControlsOld.ControlGroup[2] = KSPSerialPort.VControls.ControlGroup[2];
                    }

                    if (KSPSerialPort.VControls.ControlGroup[3] != KSPSerialPort.VControlsOld.ControlGroup[3])
                    {
                        ActiveVessel.ActionGroups.SetGroup(KSPActionGroup.Custom03, KSPSerialPort.VControls.ControlGroup[3]);
                        KSPSerialPort.VControlsOld.ControlGroup[3] = KSPSerialPort.VControls.ControlGroup[3];
                    }

                    if (KSPSerialPort.VControls.ControlGroup[4] != KSPSerialPort.VControlsOld.ControlGroup[4])
                    {
                        ActiveVessel.ActionGroups.SetGroup(KSPActionGroup.Custom04, KSPSerialPort.VControls.ControlGroup[4]);
                        KSPSerialPort.VControlsOld.ControlGroup[4] = KSPSerialPort.VControls.ControlGroup[4];
                    }

                    if (KSPSerialPort.VControls.ControlGroup[5] != KSPSerialPort.VControlsOld.ControlGroup[5])
                    {
                        ActiveVessel.ActionGroups.SetGroup(KSPActionGroup.Custom05, KSPSerialPort.VControls.ControlGroup[5]);
                        KSPSerialPort.VControlsOld.ControlGroup[5] = KSPSerialPort.VControls.ControlGroup[5];
                    }

                    if (KSPSerialPort.VControls.ControlGroup[6] != KSPSerialPort.VControlsOld.ControlGroup[6])
                    {
                        ActiveVessel.ActionGroups.SetGroup(KSPActionGroup.Custom06, KSPSerialPort.VControls.ControlGroup[6]);
                        KSPSerialPort.VControlsOld.ControlGroup[6] = KSPSerialPort.VControls.ControlGroup[6];
                    }

                    if (KSPSerialPort.VControls.ControlGroup[7] != KSPSerialPort.VControlsOld.ControlGroup[7])
                    {
                        ActiveVessel.ActionGroups.SetGroup(KSPActionGroup.Custom07, KSPSerialPort.VControls.ControlGroup[7]);
                        KSPSerialPort.VControlsOld.ControlGroup[7] = KSPSerialPort.VControls.ControlGroup[7];
                    }

                    if (KSPSerialPort.VControls.ControlGroup[8] != KSPSerialPort.VControlsOld.ControlGroup[8])
                    {
                        ActiveVessel.ActionGroups.SetGroup(KSPActionGroup.Custom08, KSPSerialPort.VControls.ControlGroup[8]);
                        KSPSerialPort.VControlsOld.ControlGroup[8] = KSPSerialPort.VControls.ControlGroup[8];
                    }

                    if (KSPSerialPort.VControls.ControlGroup[9] != KSPSerialPort.VControlsOld.ControlGroup[9])
                    {
                        ActiveVessel.ActionGroups.SetGroup(KSPActionGroup.Custom09, KSPSerialPort.VControls.ControlGroup[9]);
                        KSPSerialPort.VControlsOld.ControlGroup[9] = KSPSerialPort.VControls.ControlGroup[9];
                    }

                    if (KSPSerialPort.VControls.ControlGroup[10] != KSPSerialPort.VControlsOld.ControlGroup[10])
                    {
                        ActiveVessel.ActionGroups.SetGroup(KSPActionGroup.Custom10, KSPSerialPort.VControls.ControlGroup[10]);
                        KSPSerialPort.VControlsOld.ControlGroup[10] = KSPSerialPort.VControls.ControlGroup[10];
                    }

                    //Set sas mode
                    if (KSPSerialPort.VControls.SASMode != KSPSerialPort.VControlsOld.SASMode)
                    {
                        if (KSPSerialPort.VControls.SASMode != 0 && KSPSerialPort.VControls.SASMode < 11)
                        {
                            if (!ActiveVessel.Autopilot.CanSetMode((VesselAutopilot.AutopilotMode)(KSPSerialPort.VControls.SASMode - 1)))
                            {
                                ScreenMessages.PostScreenMessage("KSPSerialIO: SAS mode " + KSPSerialPort.VControls.SASMode.ToString() + " not avalible");
                            }
                            else
                            {
                                ActiveVessel.Autopilot.SetMode((VesselAutopilot.AutopilotMode)KSPSerialPort.VControls.SASMode - 1);
                            }
                        }
                        KSPSerialPort.VControlsOld.SASMode = KSPSerialPort.VControls.SASMode;
                    }

                    //set navball mode
                    if (KSPSerialPort.VControls.SpeedMode != KSPSerialPort.VControlsOld.SpeedMode)
                    {
                        if (!((KSPSerialPort.VControls.SpeedMode == 0) || ((KSPSerialPort.VControls.SpeedMode == 3) && !TargetExists())))
                        {
                            FlightGlobals.SetSpeedMode((FlightGlobals.SpeedDisplayModes)(KSPSerialPort.VControls.SpeedMode - 1));
                        }
                        KSPSerialPort.VControlsOld.SpeedMode = KSPSerialPort.VControls.SpeedMode;
                    }


                    // Upstream 0.19.3b removed the old "disengage SAS while steering" block: it forced SAS back on
                    // every packet and fought with the axis input. SAS now only follows the SAS toggle above.

                    KSPSerialPort.ControlReceived = false;
                } //end ControlReceived
                #endregion


            }//end if null and same vessel
            else
            {
                //Debug.Log("KSPSerialIO: ActiveVessel not found");
            }

            if (SettingsNStuff.PerfLog == 1 && FlightGlobals.ActiveVessel != null)
                PerfRecordFrame(perfStart, FlightGlobals.ActiveVessel.parts.Count);

        }

        #region utilities

        /// @brief Converts an angle in degrees (-360 to 360) to the 16-bit fixed-point format of the navball fields: degrees times 50.
        private static short FloatAngleToFixed_16(float f)
        {
            return (short)Math.Round(f * 50.0f);
        }

        /**
         * @brief Direction of a world-space vector relative to the local horizon.
         * @param up Unit vector pointing away from the body.
         * @param north Unit vector pointing north.
         * @param east Unit vector pointing east.
         * @param v The vector to convert.
         * @return Pitch above the horizon and compass heading (0 to 360) in degrees.
         */
        private static NavHeading WorldVecToNavHeading(Vector3d up, Vector3d north, Vector3d east, Vector3d v)
        {
            NavHeading ret = new NavHeading();
            ret.Pitch = (float)-((Vector3d.Angle(up, v)) - 90.0f);
            Vector3d progradeFlat = Vector3d.Exclude(up, v);
            float NAngle = (float)Vector3d.Angle(north, progradeFlat);
            float EAngle = (float)Vector3d.Angle(east, progradeFlat);
            if (EAngle < 90)
                ret.Heading = NAngle;
            else
                ret.Heading = -NAngle + 360;
            return ret;
        }

        /// @brief True if a target vessel is selected.
        private Boolean TargetExists()
        {
            return (FlightGlobals.fetch.VesselTarget != null) && (FlightGlobals.fetch.VesselTarget.GetVessel() != null); //&& is short circuiting
        }

        /// @brief Highest overheat ratio of any part (internal or skin temperature) as a percentage.
        private byte GetMaxOverHeat(Vessel V)
        {
            byte percent = 0;
            double sPercent = 0, iPercent = 0;
            double percentD = 0, percentP = 0;

            foreach (Part p in ActiveVessel.parts)
            {
                //internal temperature
                iPercent = p.temperature / p.maxTemp;
                //skin temperature
                sPercent = p.skinTemperature / p.skinMaxTemp;

                if (iPercent > sPercent)
                    percentP = iPercent;
                else
                    percentP = sPercent;

                if (percentD < percentP)
                    percentD = percentP;
            }

            percent = (byte)Math.Round(percentD * 100);
            return percent;
        }


        /// @brief Sums the current and maximum amount of a resource over all parts of a vessel.
        private IOResource GetResourceTotal(Vessel V, string resourceName)
        {
            IOResource R = new IOResource();

            foreach (Part p in V.parts)
            {
                foreach (PartResource pr in p.Resources)
                {
                    if (pr.resourceName.Equals(resourceName))
                    {
                        R.Current += (float)pr.amount;
                        R.Max += (float)pr.maxAmount;

                        break;
                    }
                }
            }

            if (R.Max == 0)
                R.Current = 0;

            return R;
        }

        /**
         * @brief Autopilot callback that merges the controller axes into KSP's control state.
         *
         * Each axis is combined according to its SettingsNStuff mode (0-3, see SettingsNStuff).
         * @param s Control state about to be applied to the vessel; modified in place.
         */
        private void AxisInput(FlightCtrlState s)
        {
            switch (SettingsNStuff.ThrottleEnable)
            {
                case 1:
                    s.mainThrottle = KSPSerialPort.VControls.Throttle;
                    break;
                case 2:
                    if (s.mainThrottle == 0)
                    {
                        s.mainThrottle = KSPSerialPort.VControls.Throttle;
                    }
                    break;
                case 3:
                    if (KSPSerialPort.VControls.Throttle != 0)
                    {
                        s.mainThrottle = KSPSerialPort.VControls.Throttle;
                    }
                    break;
                default:
                    break;

            }

            switch (SettingsNStuff.PitchEnable)
            {
                case 1:
                    s.pitch = KSPSerialPort.VControls.Pitch;
                    break;
                case 2:
                    if (s.pitch == 0)
                        s.pitch = KSPSerialPort.VControls.Pitch;
                    break;
                case 3:
                    if (KSPSerialPort.VControls.Pitch != 0)
                        s.pitch = KSPSerialPort.VControls.Pitch;
                    break;
                default:
                    break;
            }

            switch (SettingsNStuff.RollEnable)
            {
                case 1:
                    s.roll = KSPSerialPort.VControls.Roll;
                    break;
                case 2:
                    if (s.roll == 0)
                        s.roll = KSPSerialPort.VControls.Roll;
                    break;
                case 3:
                    if (KSPSerialPort.VControls.Roll != 0)
                        s.roll = KSPSerialPort.VControls.Roll;
                    break;
                default:
                    break;
            }

            switch (SettingsNStuff.YawEnable)
            {
                case 1:
                    s.yaw = KSPSerialPort.VControls.Yaw;
                    break;
                case 2:
                    if (s.yaw == 0)
                        s.yaw = KSPSerialPort.VControls.Yaw;
                    break;
                case 3:
                    if (KSPSerialPort.VControls.Yaw != 0)
                        s.yaw = KSPSerialPort.VControls.Yaw;
                    break;
                default:
                    break;
            }

            switch (SettingsNStuff.TXEnable)
            {
                case 1:
                    s.X = KSPSerialPort.VControls.TX;
                    break;
                case 2:
                    if (s.X == 0)
                        s.X = KSPSerialPort.VControls.TX;
                    break;
                case 3:
                    if (KSPSerialPort.VControls.TX != 0)
                        s.X = KSPSerialPort.VControls.TX;
                    break;
                default:
                    break;
            }

            switch (SettingsNStuff.TYEnable)
            {
                case 1:
                    s.Y = KSPSerialPort.VControls.TY;
                    break;
                case 2:
                    if (s.Y == 0)
                        s.Y = KSPSerialPort.VControls.TY;
                    break;
                case 3:
                    if (KSPSerialPort.VControls.TY != 0)
                        s.Y = KSPSerialPort.VControls.TY;
                    break;
                default:
                    break;
            }

            switch (SettingsNStuff.TZEnable)
            {
                case 1:
                    s.Z = KSPSerialPort.VControls.TZ;
                    break;
                case 2:
                    if (s.Z == 0)
                        s.Z = KSPSerialPort.VControls.TZ;
                    break;
                case 3:
                    if (KSPSerialPort.VControls.TZ != 0)
                        s.Z = KSPSerialPort.VControls.TZ;
                    break;
                default:
                    break;
            }

            switch (SettingsNStuff.WheelSteerEnable)
            {
                case 1:
                    s.wheelSteer = KSPSerialPort.VControls.WheelSteer;
                    break;
                case 2:
                    if (s.wheelSteer == 0)
                    {
                        s.wheelSteer = KSPSerialPort.VControls.WheelSteer;
                    }
                    break;
                case 3:
                    if (KSPSerialPort.VControls.WheelSteer != 0)
                    {
                        s.wheelSteer = KSPSerialPort.VControls.WheelSteer;
                    }
                    break;
                default:
                    break;
            }

            switch (SettingsNStuff.WheelThrottleEnable)
            {
                case 1:
                    s.wheelThrottle = KSPSerialPort.VControls.WheelThrottle;
                    break;
                case 2:
                    if (s.wheelThrottle == 0)
                    {
                        s.wheelThrottle = KSPSerialPort.VControls.WheelThrottle;
                    }
                    break;
                case 3:
                    if (KSPSerialPort.VControls.WheelThrottle != 0)
                    {
                        s.wheelThrottle = KSPSerialPort.VControls.WheelThrottle;
                    }
                    break;
                default:
                    break;
            }
        }

        /// @brief Maps a celestial body name to the SOI id used in VesselData::SOINumber (0 if unknown).
        private byte GetSOINumber(string name)
        {
            byte SOI;

            switch (name.ToLower())
            {
                case "sun":
                    SOI = 100;
                    break;
                case "moho":
                    SOI = 110;
                    break;
                case "eve":
                    SOI = 120;
                    break;
                case "gilly":
                    SOI = 121;
                    break;
                case "kerbin":
                    SOI = 130;
                    break;
                case "mun":
                    SOI = 131;
                    break;
                case "minmus":
                    SOI = 132;
                    break;
                case "duna":
                    SOI = 140;
                    break;
                case "ike":
                    SOI = 141;
                    break;
                case "dres":
                    SOI = 150;
                    break;
                case "jool":
                    SOI = 160;
                    break;
                case "laythe":
                    SOI = 161;
                    break;
                case "vall":
                    SOI = 162;
                    break;
                case "tylo":
                    SOI = 163;
                    break;
                case "bop":
                    SOI = 164;
                    break;
                case "pol":
                    SOI = 165;
                    break;
                case "eeloo":
                    SOI = 170;
                    break;
                default:
                    SOI = 0;
                    break;
            }
            return SOI;
        }

        /// @brief Returns the parts that hold an engine module with an active ignition.
        // this recursive stage look up stuff stolen and modified from KOS and others
        public static List<Part> GetListOfActivatedEngines(Vessel vessel)
        {
            var retList = new List<Part>();

            foreach (var part in vessel.Parts)
            {
                foreach (PartModule module in part.Modules)
                {
                    var engineModule = module as ModuleEngines;
                    if (engineModule != null)
                    {
                        if (engineModule.getIgnitionState)
                        {
                            retList.Add(part);
                        }
                    }

                    var engineModuleFx = module as ModuleEnginesFX;
                    if (engineModuleFx != null)
                    {
                        if (engineModuleFx.getIgnitionState)
                        {
                            retList.Add(part);
                        }
                    }
                }
            }

            return retList;
        }

        /**
         * @brief Total amount of a resource available to a set of engines.
         *
         * Follows stack-attached parts that allow fuel cross-feed, counting every part only once.
         * @param resourceName Resource name (case insensitive).
         * @param engines Parts holding the active engines.
         */
        public static double ProspectForResource(String resourceName, List<Part> engines)
        {
            List<Part> visited = new List<Part>();
            double total = 0;

            foreach (var part in engines)
            {
                total += ProspectForResource(resourceName, part, ref visited);
            }

            return total;
        }

        /// @brief Amount of a resource reachable from a single engine part (see the list overload).
        public static double ProspectForResource(String resourceName, Part engine)
        {
            List<Part> visited = new List<Part>();

            return ProspectForResource(resourceName, engine, ref visited);
        }

        /// @brief Recursive worker: sums the resource in @p part and in the parts that can feed it; @p visited prevents counting a part twice.
        public static double ProspectForResource(String resourceName, Part part, ref List<Part> visited)
        {
            double ret = 0;

            if (visited.Contains(part))
            {
                return 0;
            }

            visited.Add(part);

            foreach (PartResource resource in part.Resources)
            {
                if (resource.resourceName.ToLower() == resourceName.ToLower())
                {
                    ret += resource.amount;
                }
            }

            foreach (AttachNode attachNode in part.attachNodes)
            {
                if (attachNode.attachedPart != null //if there is a part attached here
                    && attachNode.nodeType == AttachNode.NodeType.Stack //and the attached part is stacked (rather than surface mounted)
                    && (attachNode.attachedPart.fuelCrossFeed //and the attached part allows fuel flow
                        )
                    && !(part.NoCrossFeedNodeKey.Length > 0 //and this part does not forbid fuel flow
                         && attachNode.id.Contains(part.NoCrossFeedNodeKey))) // through this particular node
                {


                    ret += ProspectForResource(resourceName, attachNode.attachedPart, ref visited);
                }
            }

            return ret;
        }

        /// @brief Like ProspectForResource, but sums the maximum capacity instead of the current amount.
        public static double ProspectForResourceMax(String resourceName, List<Part> engines)
        {
            List<Part> visited = new List<Part>();
            double total = 0;

            foreach (var part in engines)
            {
                total += ProspectForResourceMax(resourceName, part, ref visited);
            }

            return total;
        }

        /// @brief Maximum capacity of a resource reachable from a single engine part.
        public static double ProspectForResourceMax(String resourceName, Part engine)
        {
            List<Part> visited = new List<Part>();

            return ProspectForResourceMax(resourceName, engine, ref visited);
        }

        /// @brief Recursive worker for ProspectForResourceMax.
        public static double ProspectForResourceMax(String resourceName, Part part, ref List<Part> visited)
        {
            double ret = 0;

            if (visited.Contains(part))
            {
                return 0;
            }

            visited.Add(part);

            foreach (PartResource resource in part.Resources)
            {
                if (resource.resourceName.ToLower() == resourceName.ToLower())
                {
                    ret += resource.maxAmount;
                }
            }

            foreach (AttachNode attachNode in part.attachNodes)
            {
                if (attachNode.attachedPart != null //if there is a part attached here
                    && attachNode.nodeType == AttachNode.NodeType.Stack //and the attached part is stacked (rather than surface mounted)
                    && (attachNode.attachedPart.fuelCrossFeed //and the attached part allows fuel flow
                        )
                    && !(part.NoCrossFeedNodeKey.Length > 0 //and this part does not forbid fuel flow
                         && attachNode.id.Contains(part.NoCrossFeedNodeKey))) // through this particular node
                {


                    ret += ProspectForResourceMax(resourceName, attachNode.attachedPart, ref visited);
                }
            }

            return ret;
        }

        /// @brief Computes the vessel's orientation relative to the local surface (north/up), used to derive heading, pitch and roll.
        //Borrowed from MechJeb2
        private Quaternion updateHeadingPitchRollField(Vessel v)
        {
            Vector3d CoM, north, up;
            Quaternion rotationSurface;
            CoM = v.CoM;
            up = (CoM - v.mainBody.position).normalized;
            north = Vector3d.Exclude(up, (v.mainBody.position + v.mainBody.transform.up * (float)v.mainBody.Radius) - CoM).normalized;
            rotationSurface = Quaternion.LookRotation(north, up);
            return Quaternion.Inverse(Quaternion.Euler(90, 0, 0) * Quaternion.Inverse(v.GetTransform().rotation) * rotationSurface);
        }

        #endregion

        /// @brief Unity FixedUpdate callback, intentionally empty.
        void FixedUpdate()
        {
        }

        /// @brief Unity OnDestroy: closes the serial port when the flight scene is unloaded.
        void OnDestroy()
        {
            if (KSPSerialPort.Port.IsOpen)
            {
                KSPSerialPort.PortCleanup();
                ScreenMessages.PostScreenMessage("Port closed", 10f, KSPIOScreenStyle);
            }
        }
    }
}
