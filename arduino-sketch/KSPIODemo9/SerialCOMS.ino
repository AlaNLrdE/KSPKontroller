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

/// @brief Abandons an incomplete frame without discarding unread serial bytes (which may contain a new handshake).
void KSPBoardResetReceiver() {
  rx_len = 0;
  rx_array_inx = 0;
  address = nullptr;
  structSize = 0;
  calc_CS = 0;
}

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

/**
 * @brief Sends a struct to KSP wrapped in the wire framing.
 * @param address Start of the packed struct (use the details() macro to pass address and size together).
 * @param len Size of the struct in bytes.
 */
void KSPBoardSendData(uint8_t * address, uint8_t len){
  // The frame is built in a local buffer and written with ONE call. _SerialUSB::write() of the renesas_uno core
  // loops without a timeout (USB interrupt on and off) until the transmit FIFO has room, so if the PC closes the port
  // while the board is inside it, the board freezes (until the link watchdog resets it). Writing byte by byte
  // meant about 30 calls and 30 one-byte USB transfers per packet and kept the board inside that loop most of
  // the time; one call per packet shrinks the exposure to a few microseconds.
  // Things that were tried and made the link worse on this core: calling `Serial` (operator bool) from loop(),
  // because it runs tud_task() in main context while the USB interrupt runs it too (random multi-second stalls),
  // writing straight to TinyUSB with tud_cdc_write() (the board then sent nothing), and gating on
  // Serial.availableForWrite() (it reads 0, so every frame was skipped).
  unsigned long sendStartUs = micros();
  uint8_t frame[260];
  uint8_t CS = len;
  frame[0] = 0xBE;
  frame[1] = 0xEF;
  frame[2] = len;
  for (int i = 0; i < len; i++) {
    CS ^= *(address + i);
    frame[3 + i] = *(address + i);
  }
  frame[3 + len] = CS;

  Serial.write(frame, (size_t)len + 4);
  // UART::write() of the renesas_uno core (1.6.0) returns on TX_DATA_EMPTY, while the LAST byte is still in the
  // shift register. A frame written right after it (ControlPacket and DebugPacket in the same loop()) then lost
  // that last byte, the checksum: about 1 frame in 180 arrived one byte short (kspsim log, 2026-11-14). Waiting
  // one byte time (10 bits at 115200 = 87 us) lets it leave first. Serial.flush() would do the same but spins on
  // a TX_COMPLETE flag with no timeout.
  delayMicroseconds(100);

  dbgTxPackets++;
  unsigned long sendUs = micros() - sendStartUs;
  if (sendUs > dbgTxMaxUs) dbgTxMaxUs = sendUs;
}
