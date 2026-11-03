/**
 * @file output.ino
 * @brief Reads the physical controls and sends them to KSP.
 */

/**
 * @brief Rate limiter: calls controls() at most once every CONTROLREFRESH milliseconds.
 */
void output() {
  now = millis();
  controlTime = now - controlTimeOld;
  if (controlTime > CONTROLREFRESH){
    controlTimeOld = now;
    controls();
  }
}

/**
 * @brief Samples every control, fills CPacket and sends it. Does nothing until KSP is connected.
 *
 * Use this function as the template for adding more switches, buttons and axes.
 */
void controls() {
  if (Connected) {

    if (digitalRead(SASPIN))  //--------- This is how you do main controls
      MainControls(SAS, HIGH);
    else
      MainControls(SAS, LOW);

    if (digitalRead(RCSPIN))
      MainControls(RCS, HIGH);
    else
      MainControls(RCS, LOW);

    if (digitalRead(CG1PIN))   //--------- This is how you do control groups
      ControlGroups(1, HIGH);
    else
      ControlGroups(1, LOW);

    //This is an example of reading analog inputs to an axis, with deadband and limits
    CPacket.Throttle = constrain(map(analogRead(THROTTLEPIN),THROTTLEDB,1024-THROTTLEDB,0,1000),0, 1000);

    KSPBoardSendData(details(CPacket));
  }
}

/// @brief Configures the input pins (all switches use the internal pull-up).
void controlsInit() {
  pinMode(SASPIN, INPUT_PULLUP);
  pinMode(RCSPIN, INPUT_PULLUP);
  pinMode(CG1PIN, INPUT_PULLUP);
}

/**
 * @brief Sets or clears one bit of CPacket.MainControls.
 * @param n Bit position (SAS, RCS, LIGHTS, GEAR, BRAKES, PRECISION, ABORT or STAGE).
 * @param s Desired state of the bit.
 */
void MainControls(byte n, boolean s) {
  if (s)
    CPacket.MainControls |= (1 << n);       // forces nth bit of x to be 1.  all other bits left alone.
  else
    CPacket.MainControls &= ~(1 << n);      // forces nth bit of x to be 0.  all other bits left alone.
}

/**
 * @brief Sets or clears one custom action group bit of CPacket.ControlGroup.
 * @param n Action group number, 1 to 10.
 * @param s Desired state of the group.
 */
void ControlGroups(byte n, boolean s) {
  if (s)
    CPacket.ControlGroup |= (1 << n);       // forces nth bit of x to be 1.  all other bits left alone.
  else
    CPacket.ControlGroup &= ~(1 << n);      // forces nth bit of x to be 0.  all other bits left alone.
}
