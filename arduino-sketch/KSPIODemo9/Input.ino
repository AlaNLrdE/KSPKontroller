/**
 * @file Input.ino
 * @brief Handling of the packets received from KSP and of the idle timeout.
 */

bool dtrNow = false;                 ///< The host has the port open (DTR asserted)
unsigned long dtrLowSince = 0;       ///< When DTR was first seen low (0 = it is high)

/**
 * @brief Goes disconnected at once when the host closes the port.
 *
 * On the UNO R4 `Serial` is true only while the host asserts DTR (the plugin opens with DtrEnable = true).
 * DTR low for DTRDEBOUNCEMS while connected means nobody is listening: LEDs off, no more packets sent.
 */
void hostMonitor() {
  dtrNow = Serial;
  if (dtrNow) { dtrLowSince = 0; return; }
  if (dtrLowSince == 0) dtrLowSince = now | 1;   // never 0
  if (Connected && (now - dtrLowSince) >= DTRDEBOUNCEMS) {
    Connected = false;
    deadtimeOld = now;
    LEDSAllOff();
    rxReset();
  }
}

/// @brief A handshake arrived: start a fresh host session.
void newSession() {
  rxReset();
  stageReset();
  dtrLowSince = 0;
  deadtimeOld = now;
  Connected = true;
  dbgForceReport = true;
}

/**
 * @brief Processes at most one incoming packet and tracks the link state.
 *
 * A handshake probe is answered immediately, a telemetry packet updates the indicators. If nothing
 * valid arrives for IDLETIMER milliseconds the board is marked disconnected and all LEDs go off,
 * which also stops ControlPacket transmission (see controls()).
 *
 * @return The id of the packet that was processed, or -1 if none was received.
 */
int input() {
  int returnValue = -1;
  now = millis();
  hostMonitor();

  if (KSPBoardReceiveData()){
    deadtimeOld = now;
    returnValue = id;
    switch(id) {
    case 0: //Handshake packet: a new host session, whatever happened before
      newSession();
      Handshake();
      break;
    case 1: // Telemetry packet
      Indicators();
      break;
    case 2: // DisplayCommand (test tool): force a LED matrix screen for a few seconds
      forcedScreen = DCmd.screen;
      forcedUntil = now + 4000;
      break;
    }

    //We got some data, turn the green led on
    digitalWrite(GLED,HIGH);
    Connected = true;
  }
  else
  { //if no message received for a while, go idle
    // The loop is alive but the USB link has been dead for a long time after working: a wedged USB stack
    // is not recoverable from inside the sketch, so reset the whole board (the watchdog cannot help, loop() keeps running).
    // (Serial.end()/begin() cannot be used as a softer first step: on this core end() is an empty TODO and begin() returns early.)
    if (everConnected && (now - dbgLastRxMs) > LINKRESETMS)
      NVIC_SystemReset();

    deadtime = now - deadtimeOld;
    if (deadtime > IDLETIMER)
    {
      deadtimeOld = now;
      Connected = false;
      LEDSAllOff();
    }
  }

  return returnValue;
}

/**
 * @brief Reads one bit of the action group status reported by KSP.
 * @param n Bit index, one of the AG* defines.
 * @return 1 if the action group is active, 0 otherwise.
 */
byte ControlStatus(byte n)
{
  return ((VData.ActionGroups >> n) & 1) == 1;
}
