/**
 * @file HandShake.ino
 * @brief Reply to the KSPSerialIO plugin's handshake probe.
 */

// Handshake

/**
 * @brief Answers the plugin's probe so it recognizes this board as its display.
 *
 * The plugin sends 1/2/3; we reply with 3/1/4, the sequence it waits for. Also lights the green LED.
 */
void Handshake(){
  digitalWrite(GLED,HIGH);

  HPacket.id = 0;
  HPacket.M1 = 3;
  HPacket.M2 = 1;
  HPacket.M3 = 4;

  KSPBoardSendData(details(HPacket));
  //Serial.println(F("KSP;0"));
  //delay(1000);
}
