/**
 * @file KSPIODemo9.ino
 * @brief Arduino side of the KSP hardware controller: pin map, shared state and main loop.
 *
 * The sketch is split across several .ino files that the Arduino IDE concatenates:
 *  - KSPIODemo9.ino: pin definitions, packet structs, global state, setup() and loop()
 *  - SerialCOMS.ino: packet framing (receive and send)
 *  - HandShake.ino: answer to the plugin's handshake probe
 *  - Input.ino: receives telemetry from KSP (input() is called from loop())
 *  - output.ino: reads the physical controls and sends them to KSP (output() is called from loop())
 *  - utilities.ino: LED indicators and packet initialization
 *
 * @par Protocol
 * Both directions use the same frame:
 * @code
 * [0xBE][0xEF][size][payload ... size bytes][checksum]
 * @endcode
 * The first payload byte is the packet id (0 handshake, 1 VesselData, 101 ControlPacket) and the
 * checksum is @c size XOR-ed with every payload byte. The structs below must match the ones in
 * KSPIO.cs byte for byte, which is why they use fixed-width types and no padding.
 */

#include <WDT.h>   // watchdog timer of the UNO R4 (resets the board if loop() stalls)

//pins for LEDs
#define GLED 12     ///< Green LED: lit while telemetry is being received
#define YLED 6      ///< Yellow LED: caution (high G or low fuel)
#define RLED 5      ///< Red LED: warning (very high G or very low fuel)
#define SASLED 8    ///< Mirrors the SAS state reported by KSP
#define RCSLED 7    ///< Mirrors the RCS state reported by KSP
#define CG1LED 11   ///< Mirrors custom action group 1

//pins for input
#define STAGEPIN -1     ///< Stage push button: no pin assigned (set a pin number to enable; internal pull-up, wire to GND)
#define SASPIN 2        ///< SAS push button: each press toggles SAS on/off (internal pull-up, wire to GND). It used to be the stage button
#define RCSPIN 9        ///< RCS push button: each press toggles RCS on/off (internal pull-up, wire to GND)
#define SCREENPIN 3     ///< Screen button: cycles the LED matrix screens (internal pull-up, wire to GND)
#define CG1PIN 10       ///< Custom action group 1 switch
#define THROTTLEPIN -1  ///< Throttle potentiometer: none wired, so the throttle is sent as 0 (set the analog pin, e.g. 0 for A0, to enable)

#define LINKRESETMS 30000UL ///< After being connected, this long without any valid packet resets the board (a wedged USB stack)
#define WATCHDOGMS 2000 ///< The board resets itself if loop() does not run for this many milliseconds
#define STAGEHOLD 150 ///< Milliseconds the STAGE bit stays set after a press (long enough for the plugin to see it at low FPS)
#define RCSDEBOUNCE 150 ///< Milliseconds after an RCS button press during which further edges are ignored (contact bounce)
#define SASDEBOUNCE 150 ///< Milliseconds after a SAS button press during which further edges are ignored (contact bounce)
#define THROTTLEDB 4 ///< Throttle axis deadband, in ADC counts at each end of the range

//Input enums
// Bit positions inside ControlPacket::MainControls
#define SAS 7        ///< Bit of the SAS toggle
#define RCS 6        ///< Bit of the RCS toggle
#define LIGHTS 5     ///< Bit of the lights toggle
#define GEAR 4       ///< Bit of the landing gear toggle
#define BRAKES 3     ///< Bit of the brakes toggle
#define PRECISION 2  ///< Bit of the precision controls toggle
#define ABORT 1      ///< Bit of the abort button
#define STAGE 0      ///< Bit of the stage button

//Action group statuses
// Bit positions inside VesselData::ActionGroups (same order as enumAG in KSPIO.cs)
#define AGSAS      0   ///< SAS active
#define AGRCS      1   ///< RCS active
#define AGLight    2   ///< Lights on
#define AGGear     3   ///< Gear deployed
#define AGBrakes   4   ///< Brakes on
#define AGAbort    5   ///< Abort triggered
#define AGCustom01 6   ///< Custom action group 1
#define AGCustom02 7   ///< Custom action group 2
#define AGCustom03 8   ///< Custom action group 3
#define AGCustom04 9   ///< Custom action group 4
#define AGCustom05 10  ///< Custom action group 5
#define AGCustom06 11  ///< Custom action group 6
#define AGCustom07 12  ///< Custom action group 7
#define AGCustom08 13  ///< Custom action group 8
#define AGCustom09 14  ///< Custom action group 9
#define AGCustom10 15  ///< Custom action group 10

/// Shortcut that expands to the (address, size) argument pair KSPBoardSendData() expects.
#define details(name) (uint8_t*)&name,sizeof(name)

//if no message received from KSP for more than 2s, go idle
#define IDLETIMER 2000    ///< Milliseconds without a valid packet before the controller goes idle
#define CONTROLREFRESH 25 ///< Milliseconds between samples of the switches and buttons
#define CONTROLKEEPALIVE 200 ///< A ControlPacket is sent at least this often even if nothing changed
#define THROTTLEDELTA 6 ///< Smallest throttle change (0-1000) that is sent at once

//warnings
#define GWARN 9                  ///< G-force that raises the red warning
#define GCAUTION 5               ///< G-force that raises the yellow caution
#define FUELCAUTION 50.0         ///< Remaining stage fuel (percent) below which the caution is raised
#define FUELWARN 5.0             ///< Remaining stage fuel (percent) below which the warning is raised

unsigned long deadtime;      ///< Milliseconds since the last valid packet
unsigned long deadtimeOld;   ///< Timestamp of the last valid packet (or of the last idle reset)
unsigned long controlTime;   ///< Milliseconds since the last ControlPacket was sent
unsigned long controlTimeOld;///< Timestamp of the last ControlPacket transmission
unsigned long now;           ///< Current time, refreshed at the start of input() and output()

boolean Connected = false;   ///< True while KSP is talking to us; ControlPackets are only sent when connected

byte caution = 0;  ///< Number of active caution conditions (yellow LED when non-zero)
byte warning = 0;  ///< Number of active warning conditions (red LED when non-zero)
byte id;           ///< Id of the packet most recently received (see KSPBoardReceiveData())

#pragma pack(push, 1)

/**
 * @brief Telemetry received from KSP (packet id 1).
 *
 * Mirrors KSPSerialIO.VesselData in KSPIO.cs, which documents every field. The size is 214 bytes. The angle fields 56 to 62 are fixed point: degrees times 50.
 * Fields ending in @c Tot are resource capacities and the matching field without the suffix is the
 * current amount. Fields ending in @c S only count what the active engines can reach.
 */
struct VesselData {
    uint8_t id;             // 1
    float AP;               // 2
    float PE;               // 3
    float SemiMajorAxis;    // 4
    float SemiMinorAxis;    // 5
    float VVI;              // 6
    float e;                // 7
    float inc;              // 8
    float G;                // 9
    int32_t TAp;            // 10
    int32_t TPe;            // 11
    float TrueAnomaly;      // 12
    float Density;          // 13
    int32_t period;         // 14
    float RAlt;             // 15
    float Alt;              // 16
    float Vsurf;            // 17
    float Lat;              // 18
    float Lon;              // 19
    float LiquidFuelTot;    // 20
    float LiquidFuel;       // 21
    float OxidizerTot;      // 22
    float Oxidizer;         // 23
    float EChargeTot;       // 24
    float ECharge;          // 25
    float MonoPropTot;      // 26
    float MonoProp;         // 27
    float IntakeAirTot;     // 28
    float IntakeAir;        // 29
    float SolidFuelTot;     // 30
    float SolidFuel;        // 31
    float XenonGasTot;      // 32
    float XenonGas;         // 33
    float LiquidFuelTotS;   // 34
    float LiquidFuelS;      // 35
    float OxidizerTotS;     // 36
    float OxidizerS;        // 37
    uint32_t MissionTime;   // 38
    float deltaTime;        // 39
    float VOrbit;           // 40
    uint32_t MNTime;        // 41
    float MNDeltaV;         // 42
    float Pitch;            // 43
    float Roll;             // 44
    float Heading;          // 45
    uint16_t ActionGroups;  // 46  bit order: see the AG* defines
    uint8_t SOINumber;      // 47
    uint8_t MaxOverHeat;    // 48
    float MachNumber;       // 49
    float IAS;              // 50
    uint8_t CurrentStage;   // 51
    uint8_t TotalStage;     // 52
    float TargetDist;       // 53
    float TargetV;          // 54
    uint8_t NavballSASMode; // 55
    int16_t ProgradePitch;  // 56  degrees x 50, see the KSPIO.cs docs
    int16_t ProgradeHeading;// 57
    int16_t ManeuverPitch;  // 58  0 if there is no maneuver node
    int16_t ManeuverHeading;// 59
    int16_t TargetPitch;    // 60  0 if there is no target
    int16_t TargetHeading;  // 61
    int16_t NormalHeading;  // 62
};


static_assert(sizeof(VesselData) == 214, "VesselData struct size must be 214 bytes");

/**
 * @brief Handshake packet (id 0), exchanged in both directions.
 *
 * KSP probes with 1/2/3 and this sketch answers 3/1/4 (see Handshake()).
 */
struct HandShakePacket
{
  byte id;  ///< Packet id, always 0
  byte M1;  ///< First handshake byte
  byte M2;  ///< Second handshake byte
  byte M3;  ///< Third handshake byte
};

/**
 * @brief Controls sent to KSP (packet id 101).
 *
 * Mirrors KSPSerialIO.ControlPacket in KSPIO.cs. Fixed-width types are mandatory: on the UNO R4 a
 * plain @c int is 32 bits, which would break the 25-byte layout the plugin expects.
 */
struct ControlPacket {
  byte id;                            ///< Packet id, always 101
  byte MainControls;                  ///< SAS RCS Lights Gear Brakes Precision Abort Stage (one bit each, see the bit defines)
  byte Mode;                          ///< 0 = stage, 1 = docking, 2 = map
  uint16_t ControlGroup;              ///< Custom action groups 1-10; bit n is group n
  byte NavballSASMode;                ///< AutoPilot mode / navball mode
  byte AdditionalControlByte1;        ///< Reserved
  int16_t Pitch;                      ///< -1000 -> 1000
  int16_t Roll;                       ///< -1000 -> 1000
  int16_t Yaw;                        ///< -1000 -> 1000
  int16_t TX;                         ///< -1000 -> 1000
  int16_t TY;                         ///< -1000 -> 1000
  int16_t TZ;                         ///< -1000 -> 1000
  int16_t WheelSteer;                 ///< -1000 -> 1000
  int16_t Throttle;                   ///<     0 -> 1000
  int16_t WheelThrottle;              ///<     0 -> 1000
};

// Must match KSPSerialIO.ControlPacket (25 bytes); R4 int is 32-bit so fixed-width types are required
static_assert(sizeof(ControlPacket) == 25, "ControlPacket struct size must be 25 bytes");

/**
 * @brief Board diagnostics sent to KSP once per second (packet id 102).
 *
 * The plugin writes every one to KSP.log with the tag KSPSerialIO[BOARD], so the last line before a
 * freeze shows what the board was doing. Mirrors KSPSerialIO.DebugPacket (52 bytes: 40 of fields plus the 12-byte matrix frame).
 */
struct DebugPacket {
  byte id;                 ///< Packet id, always 102
  uint32_t uptimeMs;       ///< millis() since the board started; a drop means the board rebooted
  uint32_t loops;          ///< Completed loop() iterations
  uint16_t rxOk;           ///< Valid packets received from KSP
  uint16_t rxBadChecksum;  ///< Packets dropped for a wrong checksum
  uint16_t rxBadSize;      ///< Packets dropped because the announced size did not match the struct
  uint32_t txPackets;      ///< Packets sent to KSP (all kinds)
  uint32_t loopMaxUs;      ///< Longest loop() iteration since the previous report, microseconds
  uint32_t txMaxUs;        ///< Longest single packet send since the previous report, microseconds
  uint32_t sinceRxMs;      ///< Milliseconds since the last valid packet from KSP
  uint16_t resetCause;     ///< RA4M1 RSTSR0 (low byte) and RSTSR1 (high byte) read at boot: bit 0 power on, bit 8/9 watchdog, bit 10 software
  byte flags;              ///< bit0 Connected, bit2 SAS pin level now, bit3 watchdog started, bit4 ever connected
  int16_t rxAvail;         ///< Bytes waiting in the serial receive buffer
  uint16_t txSkipped;      ///< Frames not sent because the host had the port closed (cumulative since boot)
  uint16_t a0;             ///< Raw reading of the throttle analog pin
  uint8_t matrix[12];      ///< The LED matrix frame, one byte per column (bit r = row r); see matrix.ino
};
static_assert(sizeof(DebugPacket) == 52, "DebugPacket struct size must be 52 bytes");

/**
 * @brief Forces a LED matrix screen for a few seconds (packet id 2, KSP side never sends it).
 *
 * Only tools/matrix_preview.py uses it, to see every screen without flying. Screen numbers are the Screen
 * enum in matrix.ino; 255 returns to automatic selection.
 */
struct DisplayCommand {
  byte id;                 ///< Packet id, always 2
  byte screen;             ///< Screen to show, 255 = automatic
};
static_assert(sizeof(DisplayCommand) == 2, "DisplayCommand struct size must be 2 bytes");

#pragma pack(pop)

HandShakePacket HPacket; ///< Reused for both the received probe and our reply
VesselData VData;        ///< Latest telemetry received from KSP
ControlPacket CPacket;   ///< Controls being built and sent to KSP
DebugPacket DPacket;     ///< Diagnostics sent once per second (see debugReport())
DisplayCommand DCmd;     ///< Last DisplayCommand received (test tool only)
uint8_t mxCols[12];      ///< Last LED matrix frame, one byte per column (bit r = row r), sent in the DebugPacket
uint8_t forcedScreen = 255;     ///< LED matrix screen forced by a DisplayCommand (255 = none)
unsigned long forcedUntil = 0;  ///< The forced screen expires at this time

// Diagnostic counters, reported in DebugPacket
uint32_t dbgLoops = 0, dbgTxPackets = 0, dbgLoopMaxUs = 0, dbgTxMaxUs = 0;
uint16_t dbgRxOk = 0, dbgRxBadChecksum = 0, dbgRxBadSize = 0, dbgResetCause = 0;
unsigned long dbgLastRxMs = 0, dbgLastReportMs = 0;
uint16_t dbgTxSkipped = 0;   ///< Frames skipped because DTR was low (reported in DebugPacket.txSkipped)
bool everConnected = false;   ///< True once KSP has talked to us since power on
bool dbgWdtOk = false;   ///< True if WDT.begin() succeeded; reported in DebugPacket.flags bit 3

/// @brief Arduino entry point: opens the serial link and initializes LEDs, packets and inputs.
void setup(){
  Serial.begin(115200);
  dbgResetCause = (uint16_t)(R_SYSTEM->RSTSR0 | ((R_SYSTEM->RSTSR1 & 0xFF) << 8));   // why the board last reset: bit0 power on, bit8 IWDT, bit9 WDT, bit10 software
  // Watchdog: if loop() ever stops running (for example a blocked USB write) the board resets itself
  // after WATCHDOGMS instead of staying frozen until someone presses RESET.
  dbgWdtOk = WDT.begin(WATCHDOGMS);

  initLEDS();
  InitTxPackets();
  controlsInit();
  matrixInit();

  LEDSAllOff();
}

/// @brief Main loop: process incoming packets, then send our controls if it is time.
void loop()
{
  static unsigned long loopStartUs = 0;
  unsigned long nowUs = micros();
  if (loopStartUs != 0 && (nowUs - loopStartUs) > dbgLoopMaxUs) dbgLoopMaxUs = nowUs - loopStartUs;
  loopStartUs = nowUs;
  dbgLoops++;

  WDT.refresh();
  input();
  output();
  debugReport();
  matrixUpdate();
}
