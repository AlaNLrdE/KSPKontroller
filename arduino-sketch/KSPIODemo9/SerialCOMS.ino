/**
 * @file SerialCOMS.ino
 * @brief Packet framing over the serial port: receive state machine and transmit.
 *
 * Frame format: @c [0xBE][0xEF][size][payload][checksum], where the first payload byte is the
 * packet id and the checksum is @c size XOR-ed with every payload byte.
 * Contains code borrowed from the EasyTransfer library.
 */

uint8_t rx_len;        ///< Payload size announced by the packet being received (0 = no packet in progress)
uint8_t * address;     ///< Destination struct for the packet being received
byte buffer[256];      ///< Temporary storage and parsing buffer; buffer[0] holds the packet id
uint8_t structSize;    ///< Size of the struct that matches the packet id being received
uint8_t rx_array_inx;  ///< Index for the RX parsing buffer
uint8_t calc_CS;       ///< Calculated checksum

/**
 * @brief Reads pending serial bytes and assembles one packet.
 *
 * Non-blocking: call it repeatedly. It first looks for the 0xBE 0xEF header and reads the size and
 * id, then accumulates payload bytes across calls until the whole frame has arrived, verifies the
 * checksum and copies the payload into HPacket or VData (selected by the id).
 * A frame whose size does not match the expected struct, or whose checksum fails, is dropped.
 *
 * @return true when a complete, valid packet was stored; the packet id is left in the global @c id.
 */
boolean KSPBoardReceiveData() {
  if ((rx_len == 0)&&(Serial.available()>3)){
    // Skip bytes until the first header byte
    while(Serial.read()!= 0xBE) {
      if (Serial.available() == 0)
        return false;
    }
    if (Serial.read() == 0xEF){
      rx_len = Serial.read();
      id = Serial.read();
      rx_array_inx = 1;

      // Select the destination struct from the packet id
      switch(id) {
      case 0:
        structSize = sizeof(HPacket);
        address = (byte*)&HPacket;
        break;
      case 1:
        structSize = sizeof(VData);
        address = (byte*)&VData;
        break;
      case 2:
        structSize = sizeof(DCmd);
        address = (byte*)&DCmd;
        break;
      }
    }

    //make sure the binary structs on both Arduinos are the same size.
    if(rx_len != structSize){
      rx_len = 0;
      dbgRxBadSize++;
      return false;
    }
  }

  if(rx_len != 0){
    // Accumulate payload bytes (the last one received is the checksum)
    while(Serial.available() && rx_array_inx <= rx_len){
      buffer[rx_array_inx++] = Serial.read();
    }
    buffer[0] = id;

    if(rx_len == (rx_array_inx-1)){
      //seem to have got whole message
      //last uint8_t is CS
      calc_CS = rx_len;
      for (int i = 0; i<rx_len; i++){
        calc_CS^=buffer[i];
      }

      if(calc_CS == buffer[rx_array_inx-1]){//CS good
        memcpy(address,buffer,structSize);
        rx_len = 0;
        rx_array_inx = 1;
        dbgRxOk++;
        dbgLastRxMs = millis();
        return true;
      }
      else{
        //failed checksum, need to clear this out anyway
        dbgRxBadChecksum++;
        rx_len = 0;
        rx_array_inx = 1;
        return false;
      }
    }
  }

  return false;
}

/// @brief Forgets any half-received frame and discards unread input (a new host session starts).
void rxReset() {
  rx_len = 0;
  rx_array_inx = 1;
  // read() returns -1 without consuming while DTR is low, so stop on the first failure (no infinite loop)
  while (Serial.available() > 0 && Serial.read() >= 0) {}
}

/**
 * @brief Sends a struct to KSP wrapped in the wire framing. Never blocks.
 *
 * The frame is skipped (and counted in dbgTxSkipped) when the host has not asserted DTR, because nobody
 * has the port open, or when the USB transmit buffer has less room than the frame, because the core's
 * write() would then spin until the host drains it and starve the watchdog.
 * @param address Start of the packed struct (use the details() macro to pass address and size together).
 * @param len Size of the struct in bytes.
 * @return true if the frame was handed to the USB stack.
 */
bool KSPBoardSendData(uint8_t * address, uint8_t len){
  // _SerialUSB::write() of the renesas_uno core spins forever, with the USB interrupt disabled, when the host
  // does not drain the 256-byte transmit FIFO (the host stopped reading, or closed the port abruptly while DTR
  // still looked asserted). That froze the board for good, and the watchdog never saw it as a normal stall.
  // So wait for room in the FIFO for the whole frame, but only up to SENDWAITUS, and skip the frame if there is none.
  if (!Serial) {
    dbgTxSkipped++;
    return false;
  }
  unsigned long waitStartUs = micros();
  while (Serial.availableForWrite() < (int)len + 4) {
    if (micros() - waitStartUs > SENDWAITUS) {
      dbgTxSkipped++;
      return false;
    }
  }
  unsigned long sendStartUs = micros();
  uint8_t CS = len;
  Serial.write(0xBE);
  Serial.write(0xEF);
  Serial.write(len);

  for(int i = 0; i<len; i++){
    CS^=*(address+i);
    Serial.write(*(address+i));
  }

  Serial.write(CS);

  dbgTxPackets++;
  unsigned long sendUs = micros() - sendStartUs;
  if (sendUs > dbgTxMaxUs) dbgTxMaxUs = sendUs;
  return true;
}
