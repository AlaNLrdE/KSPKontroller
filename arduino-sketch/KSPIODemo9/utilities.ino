/**
 * @file utilities.ino
 * @brief LED indicators and packet initialization.
 */

/**
 * @brief Updates the indicator LEDs from the latest telemetry in VData.
 *
 * Yellow: G-force above GCAUTION or stage fuel below FUELCAUTION percent.
 * Red: G-force above GWARN or stage fuel below FUELWARN percent.
 * SAS, RCS and custom group 1 LEDs mirror the action group state reported by KSP.
 */
void Indicators() {
  caution = 0;
  warning = 0;

  caution += VData.G > GCAUTION;
  warning += VData.G > GWARN;
  caution += VData.LiquidFuelS/VData.LiquidFuelTotS*100 < FUELCAUTION;
  warning += VData.LiquidFuelS/VData.LiquidFuelTotS*100 < FUELWARN;

  if (caution != 0)
    digitalWrite(YLED,HIGH);
  else
    digitalWrite(YLED,LOW);

  if (warning != 0)
    digitalWrite(RLED,HIGH);
  else
    digitalWrite(RLED,LOW);

  digitalWrite(SASLED,ControlStatus(AGSAS));
  digitalWrite(RCSLED,ControlStatus(AGRCS));
  digitalWrite(CG1LED,ControlStatus(AGCustom01));
}

/// @brief Configures every LED pin as an output and lights it (a quick power-on lamp test).
void initLEDS() {
  pinMode(GLED,OUTPUT);
  digitalWrite(GLED,HIGH);

  pinMode(YLED,OUTPUT);
  digitalWrite(YLED,HIGH);

  pinMode(RLED,OUTPUT);
  digitalWrite(RLED,HIGH);

  pinMode(SASLED,OUTPUT);
  digitalWrite(SASLED,HIGH);

  pinMode(RCSLED,OUTPUT);
  digitalWrite(RCSLED,HIGH);

  pinMode(CG1LED,OUTPUT);
  digitalWrite(CG1LED,HIGH);
}

/// @brief Turns every LED off.
void LEDSAllOff() {
  digitalWrite(GLED,LOW);
  digitalWrite(YLED,LOW);
  digitalWrite(RLED,LOW);
  digitalWrite(SASLED,LOW);
  digitalWrite(RCSLED,LOW);
  digitalWrite(CG1LED,LOW);
}


/// @brief Sets the fixed packet ids: 0 for the handshake and 101 for the control packet.
void InitTxPackets() {
  HPacket.id = 0;
  CPacket.id = 101;
}
