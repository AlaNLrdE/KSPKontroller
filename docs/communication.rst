Communication contracts
=======================

This page describes the implemented channel boundaries. :doc:`protocol` specifies the serial frame
and packet layouts; :doc:`vesseldata` defines its telemetry fields. The authoritative implementations
are ``KSPIO.cs``, the sketch's packet structs and ``SerialCOMS.ino``, ``hmi-cpp/src/krpc.cpp``,
``pb.cpp``, ``scenes.cpp`` and ``KSPSerialIO/KSPSerialIO/HmiContext.cs``.

Channel inventory
-----------------

.. list-table:: Production and development endpoints
   :header-rows: 1
   :widths: 18 20 30 32

   * - Channel
     - Transport / default
     - Initiator and receiver
     - Purpose
   * - Panel link
     - Serial, 115200 baud
     - Mod opens board serial port
     - Telemetry, physical controls and diagnostics.
   * - kRPC calls
     - TCP 50000
     - HMI connects to kRPC mod
     - Handshake, procedure calls and stream registration.
   * - kRPC streams
     - TCP 50001
     - HMI opens second connection
     - Server publishes registered stream results.
   * - HMI context
     - UDP 50002
     - HMI requests; KSPSerialIO replies
     - Scene/facility identity and host discovery.
   * - Browser desktop
     - HTTP / WebSocket 6080
     - Browser connects to local container
     - Development desktop pixels and input only.

The container uses TCP 50300/50301 internally for kspsim. These are test overrides, not a production
protocol change. Both applications still use UDP 50002 inside that container. No serial packets are
tunnelled through the browser connection.

Serial framing and semantics
----------------------------

A frame is ``BE EF length payload checksum``. Length includes the packet ID, and total frame length
is payload length plus four bytes. Checksum is the XOR of the length and every payload byte. It
detects some corruption but provides neither authentication nor protection against all error patterns.

For a probe payload ``00 01 02 03`` the complete frame is::

   BE EF 04 00 01 02 03 04

The reply payload ``00 03 01 04`` produces::

   BE EF 04 00 03 01 04 02

The first byte is always the ID: the three probe/reply magic bytes alone are not a complete frame.
Payload sizes are four bytes for handshake, 214 for telemetry, 25 for controls, two for the test-only
display command and 52 for board diagnostics. Fields follow the packed little-endian layout; angle
scaling and individual units are defined in :doc:`vesseldata`.

The handshake identifies a compatible controller. It does not negotiate a protocol version, authorize
a client or acknowledge each later command. The protocol has no sequence numbers or retransmission
queue. Controls are repeated state with special edge semantics; telemetry is replaceable current state.

.. uml:: diagrams/seq_runtime.puml
   :caption: Flight telemetry and controls, including the background sender.

SAS and RCS bits change on each physical press. The plugin compares them with the previous baseline
and toggles the game's action group. Other enabled switches represent levels. Stage uses a received
rising-edge latch and a freshness check before application. Resending a held bit is not a new press.

Firmware samples controls every 25 ms, sends on relevant changes and sends a keepalive every 200 ms.
Diagnostics are sent about once a second while connected. Telemetry refresh comes from the mod's
``refresh`` setting. These intervals differ from Unity's frame rate and physics callbacks.

Packet receivers assemble partial reads and validate sizes/checksums. Malformed input can be discarded
while scanning for a later frame. Keep unsupported IDs out of normal traffic: the firmware parser is
designed around its known inbound structures, rather than a general extensible message registry.

kRPC connection and messages
----------------------------

The C++ client speaks kRPC protocol version 1. It sends protobuf messages prefixed by an unsigned
protobuf varint byte length. This is independent of serial framing: there is no ``BE EF`` header or
XOR checksum on TCP. ``recv_exact`` handles messages split across socket reads; the implementation
rejects messages larger than 64 MiB and malformed size prefixes.

.. uml:: diagrams/seq_krpc.puml
   :caption: Pairing RPC and stream connections and publishing a display snapshot.

#. Open the RPC socket and send ``ConnectionRequest`` with client name and protocol version 1.
#. Check connection status and protocol version in ``ConnectionResponse``; retain its client identifier.
#. Open the stream socket and send a stream-type request containing that identifier.
#. Issue ``SpaceCenter`` calls using service/procedure names and position-indexed encoded arguments.
#. Register streams using ``KRPC.AddStream`` and set their rates where supported.
#. Decode ``ProcedureResult`` values and publish the resulting model. The stream reader holds only
   the latest result per stream ID.

``pb.hpp``/``pb.cpp`` implement varint, fixed32, fixed64 and length-delimited wire values plus kRPC
value codecs. Remote objects are IDs, not pointers usable by the client. A null remote vessel is a
valid non-flight condition and is distinct from a network failure.

``Client`` distinguishes these errors:

* ``RpcError``: the server answered with a procedure error. Its service, name and description can
  explain an unavailable target, invalid stage or scene transition without implying a broken socket.
* ``ConnectionError``: refused/closed socket, malformed response or other transport failure.
* ``TimeoutError``: a connection-error subtype for deadline expiry.

The default TCP timeout is five seconds. The entire two-connection handshake has a 20-second
deadline; accepting TCP does not prove that the server will answer RPC. The source retries after its
configured delay (default three seconds) and can discover a new host when enabled. Optional telemetry
streams retry individually so unavailable data on the pad does not restart the whole connection.

The implementation is an HMI-oriented subset of kRPC, not a replacement for every kRPC client feature.
``tools/kspsim/krpcserver.py`` is likewise a stand-in for the procedures the project exercises. Add
newly consumed procedures and regression tests together; do not assume the simulator implements all
services exposed by the real mod.

UDP scene context and discovery
-------------------------------

The exact ASCII request datagram is ``KSPHMI/1``, without a terminating newline or NUL. A normal reply
is ``KSPHMI/1 <scene>``. For example::

   request: KSPHMI/1
   reply:   KSPHMI/1 mission_control

The production plugin reads up to four pending requests per Unity frame from a non-blocking UDP
socket. It replies to the sender's endpoint. Scene state and facility event callbacks stay on the
Unity main thread. Leaving Space Center clears any remembered facility.

The eight recognized facility views are ``space_center``, ``vab``, ``sph``, ``mission_control``,
``astronaut_complex``, ``research``, ``administration`` and ``tracking_station``. Other game scenes
may still be returned by the responder; a reply does not automatically create an HMI facility view.
The C++ parser accepts only ASCII and recognized facility keys for its scene field.

The simulator can append ``skin=classic`` or ``skin=apollo``::

   KSPHMI/1 flight skin=apollo

The parser reads that optional field even when there is no recognized facility key. It accepts a
bounded plain token; the display independently validates supported skin names. A recognized context
skin wins over the configured skin. KSPSerialIO does not send skin overrides.

``read_context_reply`` uses a 150 ms receive timeout. ``KrpcSource`` refreshes context about once a
second on its telemetry thread, including in flight. This bounded wait never runs in the draw loop.
Context loss does not invalidate a valid vessel sample.

.. uml:: diagrams/seq_hmi_context.puml
   :caption: Facility context and optional simulator skin selection.

Discovery broadcasts the same request and uses an eligible responding address as a candidate server.
The implementation filters eligible IPv4 addresses; it does not authenticate a server or prove kRPC
is listening. Keep discovery disabled for isolated tests so a real game cannot redirect them.

Compatibility and change procedure
----------------------------------

For a serial field/layout change:

#. Update the C# and Arduino definitions, fixed widths, packing and expected sizes together.
#. Update ``tools/kspsim/arduino.py`` serialization and the field reference.
#. Run protocol consistency checks and simulator tests, then build both production projects.
#. Test the actual board/game pairing before treating the changed layout as deployed compatibility.

For an HMI value, keep the current kRPC transport and add the necessary procedure/stream to the
client, simulator stand-in and tests. For a facility, update the plugin mapping, simulator scene
set, HMI view and context tests. Change ``KSPHMI/1`` only as a coordinated protocol revision.

Network assumptions
-------------------

These links are designed for a local development/game network. The serial checksum, UDP context and
discovery provide no authentication. This client does not implement TLS for kRPC. Bind and firewall
the production server for the intended LAN; do not publish game-control endpoints to the Internet.
The Docker desktop publishes its unauthenticated browser session only on host loopback. Its internal
VNC listener stays on container loopback. Changing those bindings changes who can view and control
the test desktop.
