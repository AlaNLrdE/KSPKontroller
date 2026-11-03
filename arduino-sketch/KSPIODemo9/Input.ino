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
    }

    //We got some data, turn the green led on
    digitalWrite(GLED,HIGH);
    Connected = true;
  }
  else
  { //if no message received for a while, go idle
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
