# KSPIODemo9 – Arduino UNO R4 KSP Controller

This project implements the Arduino side of a custom controller for [Kerbal Space Program (KSP)](https://kerbalspaceprogram.com/), using the KSPSerialIO protocol. It enables real-time, bidirectional communication between your physical controls (buttons, switches, LEDs, potentiometers, etc.) and the game.

## Features

- **Telemetry Reception:** Receives real-time vessel data (altitude, speed, fuel, etc.) from KSP for display or indicator updates.
- **Control Transmission:** Sends button, switch, and analog axis states (e.g., throttle) to KSP for direct in-game control.
- **Status LEDs:** Visual feedback for action groups, warnings, and custom indicators.
- **Robust Serial Protocol:** Implements handshake and packet-based communication for reliable operation.

---

## Folder Structure

```
arduino-sketch/KSPIODemo9/
├── HandShake.ino      # Handshake protocol with KSPSerialIO
├── Input.ino          # Handles incoming telemetry and updates outputs
├── KSPIODemo9.ino     # Main sketch: setup, loop, pin/struct definitions
├── output.ino         # Reads controls and sends data to KSP
├── SerialCOMS.ino     # Serial communication protocol implementation
├── utilities.ino      # Helper functions (LEDs, packet init, etc.)
```

---

## File Overview

- **KSPIODemo9.ino**  
  Main entry point. Sets up hardware, initializes serial, and runs the main loop. Defines pin assignments and data structures for communication.

- **HandShake.ino**  
  Implements the handshake routine required by KSPSerialIO. Ensures both Arduino and the KSP plugin are synchronized before exchanging data.

- **Input.ino**  
  Processes incoming telemetry packets from KSP. Updates LEDs, displays, or other indicators based on vessel status.

- **output.ino**  
  Reads the state of physical controls (buttons, switches, potentiometers) and sends this data to KSP as control packets.

- **SerialCOMS.ino**  
  Handles low-level serial communication, including packet parsing, sending, and error checking.

- **utilities.ino**  
  Contains utility functions for LED control, packet initialization, and other helper routines.

---

## Hardware Setup

- **Board:** Arduino UNO R4 (or compatible)
- **Controls:** Connect buttons, switches, and potentiometers to digital/analog pins as defined in `KSPIODemo9.ino`.
- **Indicators:** Connect LEDs or other output devices to the appropriate pins.
- **Serial:** Connect via USB to the PC running KSP.

**Tip:** Review the pin assignments in `KSPIODemo9.ino` and adjust wiring as needed for your custom controller layout.

---

## Usage

1. **Open the Project:**  
   Open the `arduino-sketch/KSPIODemo9` folder in the Arduino IDE.

2. **Configure Pins:**  
   Edit `KSPIODemo9.ino` to match your hardware wiring if necessary.

3. **Upload Firmware:**  
   Select your Arduino UNO R4 board and upload the sketch.

4. **Connect to KSP:**  
   - Ensure the KSPSerialIO plugin is installed and configured in KSP.
   - Set the correct serial port and baud rate in the plugin’s `config.xml` (must match the Arduino sketch, typically `115200`).

5. **Start KSP:**  
   The Arduino will perform a handshake with the plugin. On success, telemetry and controls will sync.

---

## Hints & Best Practices

- **Debugging:**  
  Use `Serial.println()` (uncomment as needed) for debugging communication or hardware issues.

- **Baud Rate:**  
  Ensure the baud rate in both Arduino and KSPSerialIO config match exactly.

- **Custom Controls:**  
  You can expand the sketch to support more buttons, axes, or indicators by modifying the pin definitions and packet structures.

- **Safety:**  
  Always disconnect power before rewiring your controller.

---

## Extending the Project

- Add displays (e.g., 7-segment, LCD) for more detailed telemetry.
- Implement rotary encoders for fine control.
- Expand the protocol to support custom data or feedback.

---

## References

- [KSPSerialIO Forum Thread](http://forum.kerbalspaceprogram.com/index.php?/topic/60281-hardware-plugin-arduino-based-physical-display-serial-port-io-tutorial-22-april/)
- [hugopeeters/KSPSerialIO](https://github.com/hugopeeters/KSPSerialIO)

---

## License

This project is licensed under the MIT License. See [LICENSE](../../LICENSE) for details.