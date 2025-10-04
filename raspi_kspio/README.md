# KSPKontroller Raspberry Pi Python Port

This project is a multi-file Python 3 port of the original Arduino-based KSPKontroller, designed to run on a Raspberry Pi 4.  
It allows you to interface physical controls (buttons, LEDs, and eventually analog throttle) with Kerbal Space Program via serial communication.

---

## Project Structure

```
raspi_kspio/
│
├── __init__.py
├── main.py                # Main loop and integration
├── handshake.py           # Handshake packet structure
├── input_handler.py       # Button and throttle input handling
├── output_handler.py      # LED output handling
├── serialcoms.py          # Serial communication and VesselData struct
├── utilities.py           # Utility functions (e.g., millis)
└── config.py              # Pin and constant definitions
```

---

## How the Code Works

- **`config.py`**  
  Centralizes all pin assignments, serial port settings, and constants for easy configuration.

- **`handshake.py`**  
  Defines the handshake packet structure and provides methods for packing/unpacking handshake data.

- **`serialcoms.py`**  
  Handles serial communication with KSP, including reading and unpacking the `VesselData` struct.

- **`input_handler.py`**  
  Reads button states from GPIO pins. Includes (commented out) code for reading throttle input from an external ADC (such as MCP3008).

- **`output_handler.py`**  
  Controls the state of LEDs connected to the Raspberry Pi GPIO pins.

- **`utilities.py`**  
  Provides utility functions, such as `millis()` for millisecond-precision timing.

- **`main.py`**  
  Initializes all subsystems, enters the main loop, reads inputs, processes serial data, and updates outputs accordingly.

---

## How to proceed

- Place each code block in its respective file.
- Install dependencies:  
  ```sh
  pip3 install pyserial RPi.GPIO
  # For ADC support (when ready): pip3 install spidev
  ```
- Run with:  
  ```sh
  python3 -m raspi_kspio.main
  ```

---

**Note:**  
- The analog throttle input code is included but commented out.  
  When you add an external ADC (like MCP3008), you can enable this functionality by uncommenting the relevant lines in `input_handler.py`.
- Update the GPIO pin numbers and serial port in `config.py` to match your hardware setup.

---

## Hardware Wiring Guide

### Buttons (Inputs)
- **SAS Button:** Connect one side to GPIO pin 8 (BCM numbering), the other side to GND.
- **RCS Button:** Connect one side to GPIO pin 9, the other side to GND.
- **Custom Group 1 Button:** Connect one side to GPIO pin 10, the other side to GND.

> The code uses internal pull-up resistors, so pressing a button connects the pin to GND (logic LOW).

---

### LEDs (Outputs)
- **Green LED (GLED):** Connect the anode (long leg) to GPIO pin 7 via a 220Ω resistor, cathode (short leg) to GND.
- **Yellow LED (YLED):** GPIO pin 6 → 220Ω resistor → LED anode, cathode to GND.
- **Red LED (RLED):** GPIO pin 5 → 220Ω resistor → LED anode, cathode to GND.
- **SAS LED:** GPIO pin 13 → 220Ω resistor → LED anode, cathode to GND.
- **RCS LED:** GPIO pin 12 → 220Ω resistor → LED anode, cathode to GND.
- **Custom Group 1 LED:** GPIO pin 11 → 220Ω resistor → LED anode, cathode to GND.

---

### Throttle (Analog Input, Optional)
- **When using an MCP3008 ADC:**
  - Connect the throttle potentiometer's wiper to CH0 of the MCP3008.
  - Connect MCP3008 VDD and VREF to 3.3V, AGND and DGND to GND.
  - Connect MCP3008 DIN to Raspberry Pi MOSI (GPIO 10), DOUT to MISO (GPIO 9), CLK to SCLK (GPIO 11), and CS/SHDN to CE0 (GPIO 8).
  - Potentiometer ends go to 3.3V and GND.

---

### General Notes
- All GPIO numbers refer to **BCM numbering** (not physical pin numbers).
- Always use resistors (220Ω–470Ω) in series with LEDs to prevent damage.
- Double-check voltage compatibility: Raspberry Pi GPIOs are **3.3V** logic.
- For longer wires or more buttons/LEDs, consider using a breadboard or PCB for neatness and reliability.

---

**Refer to the pinout diagram for your Raspberry Pi model to locate the correct GPIO pins.**