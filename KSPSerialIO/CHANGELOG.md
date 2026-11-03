# Changelog

* **0.19.3.2**: 20261103
  Optional performance log (`PerfLog` in config.xml): every 10 s it writes how long the plugin takes per
  frame, how much of that is vessel sampling and packet sending, and the game's own frame time. The
  harmless read error when a port is closed no longer dumps a stack trace.
* **0.19.3.1**: 20261103
  The plugin now asserts DTR on its serial port. Native-USB boards such as the Arduino UNO R4 only
  transmit while DTR is high, so without it the handshake never got a reply and no LED lit. The default
  `BaudRate` in config.xml is now 115200 to match the sketch.
* **0.19.3.0**: 20261103
  Merged the functional changes of upstream zitron-git/KSPSerialIO 0.19.0 to 0.19.3b into this
  cross-platform (PsimaxSerial) fork:
  * Seven navball vector fields in `VesselData` (prograde, maneuver, target and normal headings;
    upstream 0.19.1). The packet grows from 200 to 214 bytes, so the Arduino struct must be updated.
  * Those angles are 16-bit fixed point, degrees times 50 (upstream 0.19.2).
  * Removed the "disengage SAS while steering" block, which fought with the axis input (upstream
    0.19.3b). `SASTol` in config.xml is now unused.
  * Not ported: the `\.\COMn` port name workaround (PsimaxSerial already adds that prefix), the
    OpenNETCF serial code and the always-on checksum debug logging.
  Fork fixes since 0.18.7: single-reader serial thread and parser hardening, 25-byte ControlPacket,
  per-port isolation during discovery, Bluetooth ports skipped and bounded send failures.

* **0.20.0.0**: 20180105
  This is my first edit, removing stuff I don't need and testing the build process.
* **0.18.6.1**: 20170304
  This release just updates to the upstream 0.18.6 release.
* **0.18.2.2**: 20160809
  I'm still getting the hang of making the new serial handling code work
  properly, sorry. This release is a minor change to the serial port
  initialisation and teardown, ensuring the port is created properly every
  time the flight scene loads, and is torn down properly without memory leaks
  when the flight scene exits. This should make it more stable across
  multiple scene changes.
* **0.18.2.1**: 20160721
  * Move inbound packet processing out of worker back in to main thread.
  * Serial worker thread uses while loop instead of nested calls,
    fixing segfaults on Linux.
* **0.18.2**: 20160719
  * Initial port of upstream 0.18.2.
  * Replace original serialport library with PsiMaxSerial.
  * Replace serial event listener with threaded Stream BeginRead/EndRead loop.
