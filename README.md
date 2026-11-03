# KSPKontroller

KSPKontroller is a physical controller system for [Kerbal Space Program (KSP)](https://kerbalspaceprogram.com/), enabling real-time interaction with the game using custom hardware. This project is based on and extends the work from [hugopeeters/KSPSerialIO](https://github.com/hugopeeters/KSPSerialIO).

## Documentation

Full documentation (architecture, serial protocol, UML diagrams and the generated API reference) is published at **<https://alanlrde.github.io/KSPKontroller/>**. It is built with Sphinx and PlantUML from the [docs/](docs/) folder; see [Building with `./ci`](#building-with-ci-cicmd-in-powershellcmd) to build it locally with `./ci sphinx`.

## Project Structure

- **arduino-sketch/**  
  Contains Arduino code for interfacing with physical hardware (buttons, switches, LEDs, etc.) and communicating with KSP via serial.

- **KSPSerialIO/**  
  Contains a KSP plugin (C#) that communicates with the Arduino hardware over a serial port, sending telemetry and receiving control inputs.

---

## How It Works

1. **KSPSerialIO Plugin**  
   Runs inside KSP, collects vessel telemetry, and sends it to the Arduino. It also receives control inputs (e.g., button presses, throttle) from the Arduino and applies them to the active vessel in-game.

2. **Arduino Sketch**  
   Reads physical controls (switches, buttons, analog axes), updates LEDs and indicators based on telemetry from KSP, and sends control commands back to the game.

---

## Getting Started

### 1. Hardware Setup

- Build your controller using an Arduino-compatible board.
- Connect buttons, switches, potentiometers (for axes), and LEDs as desired.
- Wire the pins according to the definitions in [arduino-sketch/KSPIODemo9.ino](arduino-sketch/KSPIODemo9.ino).

### 2. Arduino Firmware

- Open the `arduino-sketch` folder in the Arduino IDE.
- Upload the combined sketch (all `.ino` files are used together) to your Arduino board.
- The main entry point is in [arduino-sketch/KSPIODemo9.ino](arduino-sketch/KSPIODemo9.ino), which includes setup and loop functions.

### 3. KSP Plugin Installation

- Build the KSPSerialIO plugin using Visual Studio or Mono (`make` or `xbuild`).
- Copy the resulting `KSPSerialIO.dll` and `PsimaxSerial.dll` from `KSPSerialIO/bin/Release/` to your KSP installation's `GameData/KSPSerialIO/` directory.
- Copy `config.xml` to `GameData/KSPSerialIO/PluginData/KSPSerialIO/`.
- See [KSPSerialIO/Makefile](KSPSerialIO/Makefile) for build and install commands.

### 4. Configuration

- Edit [KSPSerialIO/config.xml](KSPSerialIO/config.xml) to set your serial port, baud rate, and control preferences.
  - Example:  
    `<string name="DefaultPort">/dev/cu.usbmodem30</string>`
- Ensure the baud rate matches between Arduino and KSPSerialIO (`115200` in Arduino, `BaudRate` in config.xml).

### 5. Running

- Start Kerbal Space Program.
- The plugin will attempt to connect to the Arduino via the specified serial port.
- On successful handshake, telemetry and controls will sync between KSP and your hardware.

---

## Features

- **Telemetry Output:**  
  Real-time vessel data (altitude, speed, fuel, etc.) sent from KSP to Arduino for display on LEDs or other indicators.

- **Control Input:**  
  Physical switches and buttons mapped to KSP action groups (SAS, RCS, Lights, Gear, Brakes, Staging, Custom Groups).

- **Analog Axes:**  
  Throttle and other axes supported via potentiometers.

- **Status LEDs:**  
  Visual feedback for warnings, action group status, and custom indicators.

---

## File Reference

- **[arduino-sketch/KSPIODemo9.ino](arduino-sketch/KSPIODemo9.ino):**  
  Main Arduino sketch, pin definitions, and struct layouts.

- **[arduino-sketch/output.ino](arduino-sketch/output.ino):**  
  Handles sending control data from Arduino to KSP.

- **[arduino-sketch/Input.ino](arduino-sketch/Input.ino):**  
  Handles receiving telemetry and updating indicators.

- **[arduino-sketch/SerialCOMS.ino](arduino-sketch/SerialCOMS.ino):**  
  Serial communication protocol implementation.

- **[arduino-sketch/utilities.ino](arduino-sketch/utilities.ino):**  
  Utility functions for LEDs and packet initialization.

- **[KSPSerialIO/KSPSerialIO/KSPIO.cs](KSPSerialIO/KSPSerialIO/KSPIO.cs):**  
  Main KSP plugin code, serial communication, and vessel data handling.

---

## Troubleshooting

- **Serial Port Not Found:**  
  - Check your Arduino is connected and the correct port is set in `config.xml`.
  - On Windows, use `COMx` (e.g., `COM3`). On Mac/Linux, use `/dev/tty...` or `/dev/cu...`.

- **No Telemetry/Controls:**  
  - Ensure both Arduino and KSPSerialIO are using the same baud rate.
  - Check for handshake messages in the KSP debug log.

- **LEDs/Buttons Not Working:**  
  - Verify wiring matches the pin assignments in the Arduino sketch.

---

## Credits

- Original KSPSerialIO by [zitronen](http://forum.kerbalspaceprogram.com/index.php?/topic/60281-hardware-plugin-arduino-based-physical-display-serial-port-io-tutorial-22-april/)
- Fork and MacOS port by [hugopeeters](https://github.com/hugopeeters/KSPSerialIO)
- This fork and hardware adaptation by Alan Ocegueda

---

## License

This project is licensed under the MIT License. See [LICENSE](LICENSE)
---

## Building with `./ci` (`./ci.cmd` in PowerShell/cmd)

```
./ci build_sketch        # compile the Arduino sketch (downloads arduino-cli + UNO R4 core if missing)
./ci build_KSPSerialIO   # compile the KSP plugin (msbuild, xbuild or dotnet)
./ci build_all           # both
./ci deploy               # build the plugin and install it into a local KSP (lists installs and asks which)
./ci ksp_list             # list the KSP installs found on this machine
./ci deploy_all [PORT]    # build everything, install the plugin into a local KSP and flash the board (then test it; NO_TEST=1 skips)
./ci disable_mod          # move the plugin out of GameData (and enable_mod back) to compare performance
./ci save_log [LABEL]      # keep the KSP.log of a run in build/perf/LABEL/ (KSP overwrites it)
./ci upload_sketch [PORT]   # upload the compiled sketch to the board
./ci test_arduino [PORT]   # compile + upload + test the board over USB serial (NO_UPLOAD=1 skips the upload)
./ci clean
```

Set `FQBN` to target another board (default `arduino:renesas_uno:unor4wifi`, use `arduino:renesas_uno:minima` for the Minima) and `CONFIG=Debug` for a debug plugin build.
