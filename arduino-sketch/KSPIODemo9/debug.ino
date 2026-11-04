/**
 * @file debug.ino
 * @brief Once-per-second diagnostics sent to the plugin, which writes them to KSP.log.
 */

/**
 * @brief Sends a DebugPacket every second while connected, then resets the "max" counters.
 *
 * Called from loop(). Nothing is sent when KSP is not talking to us, so it never fills the transmit
 * buffer of a port nobody reads.
 */
void debugReport() {
  unsigned long ms = millis();
  if (!Connected || (ms - dbgLastReportMs) < 1000) return;
  dbgLastReportMs = ms;

  DPacket.id = 102;
  DPacket.uptimeMs = ms;
  DPacket.loops = dbgLoops;
  DPacket.rxOk = dbgRxOk;
  DPacket.rxBadChecksum = dbgRxBadChecksum;
  DPacket.rxBadSize = dbgRxBadSize;
  DPacket.txPackets = dbgTxPackets;
  DPacket.loopMaxUs = dbgLoopMaxUs;
  DPacket.txMaxUs = dbgTxMaxUs;
  DPacket.sinceRxMs = ms - dbgLastRxMs;
  DPacket.resetCause = dbgResetCause;
  DPacket.flags = (Connected ? 1 : 0) | (digitalRead(STAGEPIN) == HIGH ? 4 : 0);
  DPacket.rxAvail = Serial.available();
  DPacket.txFree = Serial.availableForWrite();
  DPacket.a0 = analogRead(THROTTLEPIN);

  dbgLoopMaxUs = 0;
  dbgTxMaxUs = 0;
  KSPBoardSendData(details(DPacket));
}
