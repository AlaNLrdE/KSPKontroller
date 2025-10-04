//pins for LEDs
#define GLED 7
#define YLED 6
#define RLED 5
#define SASLED 13
#define RCSLED 12
#define CG1LED 11

//pins for input
#define SASPIN 8
#define RCSPIN 9
#define CG1PIN 10
#define THROTTLEPIN 0

#define THROTTLEDB 4 //Throttle axis deadband

//Input enums
#define SAS 7
#define RCS 6
#define LIGHTS 5
#define GEAR 4
#define BRAKES 3
#define PRECISION 2
#define ABORT 1
#define STAGE 0

//Action group statuses
#define AGSAS      0
#define AGRCS      1       
#define AGLight    2 
#define AGGear     3
#define AGBrakes   4 
#define AGAbort    5 
#define AGCustom01 6
#define AGCustom02 7 
#define AGCustom03 8 
#define AGCustom04 9 
#define AGCustom05 10
#define AGCustom06 11 
#define AGCustom07 12 
#define AGCustom08 13 
#define AGCustom09 14 
#define AGCustom10 15

//macro 
#define details(name) (uint8_t*)&name,sizeof(name)

//if no message received from KSP for more than 2s, go idle
#define IDLETIMER 2000
#define CONTROLREFRESH 25

//warnings
#define GWARN 9                  //9G Warning
#define GCAUTION 5               //5G Caution
#define FUELCAUTION 50.0         //10% Fuel Caution
#define FUELWARN 5.0             //5% Fuel warning

unsigned long deadtime, deadtimeOld, controlTime, controlTimeOld;
unsigned long now;

boolean Connected = false;

byte caution = 0, warning = 0, id;

#pragma pack(push, 1)
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
    uint16_t ActionGroups;  // 46
    uint8_t SOINumber;      // 47
    uint8_t MaxOverHeat;    // 48
    float MachNumber;       // 49
    float IAS;              // 50
    uint8_t CurrentStage;   // 51
    uint8_t TotalStage;     // 52
    float TargetDist;       // 53
    float TargetV;          // 54
    uint8_t NavballSASMode; // 55
};
#pragma pack(pop)

static_assert(sizeof(VesselData) == 200, "VesselData struct size must be 200 bytes, the calculated size is %d");

struct HandShakePacket
{
  byte id;
  byte M1;
  byte M2;
  byte M3;
};

struct ControlPacket {
  byte id;
  byte MainControls;                  //SAS RCS Lights Gear Brakes Precision Abort Stage 
  byte Mode;                          //0 = stage, 1 = docking, 2 = map
  unsigned int ControlGroup;          //control groups 1-10 in 2 bytes
  byte AdditionalControlByte1;        //other stuff
  byte AdditionalControlByte2;
  int Pitch;                          //-1000 -> 1000
  int Roll;                           //-1000 -> 1000
  int Yaw;                            //-1000 -> 1000
  int TX;                             //-1000 -> 1000
  int TY;                             //-1000 -> 1000
  int TZ;                             //-1000 -> 1000
  int Throttle;                       //    0 -> 1000
};

HandShakePacket HPacket;
VesselData VData;
ControlPacket CPacket;

void setup(){
  Serial.begin(115200);

  initLEDS();
  InitTxPackets();
  controlsInit();

  LEDSAllOff();  
}

void loop()
{  
  input();
  output();
}


















