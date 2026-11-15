/**
 * @file Input.ino
 * @brief Handling of the packets received from KSP and of the idle timeout.
 */

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

  // Expire the old frame before parsing: a fresh handshake may already be queued after the silence.
  deadtime = now - deadtimeOld;
  if (deadtime > IDLETIMER) {
    deadtimeOld = now;
    Connected = false;
    KSPBoardResetReceiver();
    LEDSAllOff();
  }

  if (KSPBoardReceiveData()){
    deadtimeOld = now;
    returnValue = id;
    switch(id) {
    case 0: //Handshake packet
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
  { // No valid packet this iteration: check the longer link recovery deadline.
    // The loop is alive but the USB link has been dead for a long time after working: a wedged USB stack
    // is not recoverable from inside the sketch, so reset the whole board (the watchdog cannot help, loop() keeps running).
    if (everConnected && (now - dbgLastRxMs) > LINKRESETMS)
      NVIC_SystemReset();

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
