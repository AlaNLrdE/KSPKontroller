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
  static boolean stagePressedBefore = false;  // button state at the previous sample (edge detection)
  static unsigned long stageHoldUntil = 0;    // the STAGE bit stays set until this time (ms)
  static int stageIdle = HIGH;                // idle level of the stage pin, learned while not connected
  static ControlPacket lastSent;
  static boolean haveLast = false;
  static unsigned long lastSendMs = 0;
  if (Connected) {

    // Stage button (same as the space bar). The plugin stages once on each rising edge of the STAGE bit and
    // only looks at the bit once per game frame, so a short tap must not be missed: the bit is raised on the
    // first sample that differs from the idle level and held for STAGEHOLD ms. Contact bounce falls inside
    // the hold time and is ignored. The idle level is learned while the link is down, so the button works
    // wired to GND (idle high, pull-up) or to +V with a pull-down (idle low), and a pin that is stuck at one
    // level never stages by itself.
    boolean stagePressed = (digitalRead(STAGEPIN) != stageIdle);
    if (stagePressed && !stagePressedBefore && now > stageHoldUntil) {
      stageHoldUntil = now + STAGEHOLD;
    }
    stagePressedBefore = stagePressed;
    MainControls(STAGE, now < stageHoldUntil);

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

    // Send only when something changed, plus a keep-alive. The UNO R4 USB serial is known to stall at random after
    // long, steady traffic, so the board no longer streams 40 identical ControlPackets per second. A change in a
    // switch, button or group is sent at once (sampled every CONTROLREFRESH ms); the throttle must move by at least
    // THROTTLEDELTA (the analog input jitters by a few counts); otherwise one packet every CONTROLKEEPALIVE ms tells
    // the plugin that the link is alive.
    int16_t throttleDiff = CPacket.Throttle - lastSent.Throttle;
    if (throttleDiff < 0) throttleDiff = -throttleDiff;
    boolean changed = !haveLast || CPacket.MainControls != lastSent.MainControls ||
                      CPacket.ControlGroup != lastSent.ControlGroup || throttleDiff >= THROTTLEDELTA;
    if (changed || (now - lastSendMs) >= CONTROLKEEPALIVE) {
      KSPBoardSendData(details(CPacket));
      lastSent = CPacket;
      haveLast = true;
      lastSendMs = now;
    }
  } else {
    haveLast = false;
    stageIdle = digitalRead(STAGEPIN);   // learn the idle level; keep the button released-looking until connected
    stagePressedBefore = false;
    stageHoldUntil = 0;
  }
}

/// @brief Configures the input pins (all switches use the internal pull-up).
void controlsInit() {
  pinMode(STAGEPIN, INPUT_PULLUP);
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
