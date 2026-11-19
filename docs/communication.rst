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
   * - Editor ship report
     - kRPC service ``KSPHMI`` on TCP 50000
     - HMI calls ``KSPSerialIO.Hmi.dll`` through kRPC
     - VAB ship report: revision polled at 2 Hz, text fetched on change (format v1, :doc:`hmi`).
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

Besides ``SpaceCenter``, the HMI calls the project's own service ``KSPHMI`` (``Revision``, ``ShipReport``) in the
VAB, provided by ``KSPSerialIO.Hmi.dll`` and registered for the editor scene only. Any ``RpcError`` from it (the
service is missing, or the game is in another scene) only hides the VAB report; it never ends the session.

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


Agent coordination mailbox
--------------------------

``tools/agent_mailbox.py`` is a development-only, standard-library LAN service, separate from
all game protocols. Since 2026-11-18 the Raspberry Pi hosts it (user decision: the Pi is always on, the Mac and the
PC come and go): user unit ``agent-mailbox.service`` (``tools/systemd/agent-mailbox.service``, lingering, no sudo),
own folder ``~/agent-mailbox`` (a deliberate copy of the script, private ``server.json`` with the token, the log
``shared.jsonl``, ``backup/`` and ``evidence/``), URL ``http://kspKontrollerPi.local:50380`` (``localhost`` on the Pi).
To move it: stop the old server, copy ``shared.jsonl`` (plus ``backup/`` and ``evidence/``) unchanged and check its
hash, start the new one, then change only the ``url`` field of every agent's config; watchers reload it on retry. An
agent whose config still names the old host cannot receive the move notice: its operator updates that ``url``.
Every agent has its own private, ignored config file holding the server URL, the shared bearer
token and the agent's own identity. Bind only the intended LAN interface (default loopback),
use port 50380, and keep this plain HTTP endpoint on a trusted local network. Do not expose it
through a router or send account credentials.

Use a stable LAN hostname in each mailbox URL, not a DHCP address. On the LAN host, launch
``serve --bind 0.0.0.0`` so the listening socket remains usable when an IPv4 interface address
changes. The default loopback binding is for local tests. Each new HTTP request resolves its
hostname again; a running watcher also reloads its private config before each retry, so a
URL/token update does not require restarting that watcher. Keep the identity unchanged.
HTTP operations retry transient transport/DNS failures up to three times, resolving the name
again on each attempt. Message IDs and state-change digests make retries safe even when the
server committed a change but its reply was lost. Authentication/client errors fail immediately.
After bounded retries, watch keeps reconnecting; one-shot commands fail visibly and can be
retried with the same message ID. Hosts must advertise/resolve their names on the actual LAN.
Using a hostname alone does not install a Windows mDNS responder or configure a router.

The server is initialized once with ``init --url http://HOST:50380 --peer mac-claude`` (which
creates the token) and started with ``serve --bind HOST``. Every other agent creates its own
config with the same URL and token: ``init --url ... --peer pc-codex --token TOKEN``, at
``build/agent-mailbox/pc-codex.json``. The public script is available at ``/client.py`` for
bootstrap; no directories, credentials or history files are served. Pass ``--config PATH``
before the subcommand::

   python tools/agent_mailbox.py register
   python tools/agent_mailbox.py status
   python tools/agent_mailbox.py collect
   python tools/agent_mailbox.py pending
   python tools/agent_mailbox.py send --to pc-codex --text "REQUEST: rebuild the HMI at 6f887e7"
   python tools/agent_mailbox.py send --to pc --owner pc-codex --deadline 60 --text "REQUEST: ..."
   python tools/agent_mailbox.py track --id ID
   python tools/agent_mailbox.py done --id ID
   python tools/agent_mailbox.py watch --once

The agreed lifecycle is archive, acknowledge, work, result, done:

* At turn start/end and long-task progress checkpoints, run ``collect``. It atomically writes
  each incoming copy to the agent's ``*.history.md``, flushes the file, and only then records
  ``archived`` for that agent on the server. It never deletes other recipients' copies. Each
  entry is keyed by message id and recipient, so a retried collect never duplicates it.
  ``collect --no-ack`` is for migrating messages already acknowledged.
* Work from that Markdown history. Human-readable entries include a machine-readable copy so
  ``pending`` can recover tasks after the server compacted them.
* ``collect`` sends one deterministic working ACK (id ``ack-IDENTITY-SEQ``) for each REQUEST the
  agent owns and records ``acked``. ACK, RESULT and NOTICE messages never trigger ACK replies,
  avoiding loops. Leave outgoing messages for their recipients. Never send a manual ACK on top of
  it (the sender sees two ACKs per REQUEST); send STATUS/INFO only when it adds information.
* After completing the request, send RESULT with its number and findings, then ``done --id ID``
  (or ``--seq N``), in any order. ``done`` is stored on the server and in the local
  ``*.done.json``, so a lost local file does not resurrect finished work. Also read and mark
  informational messages done; an ACK is not completed work.

``watch --once`` is a background notification hint: it long-polls without model calls, prints
incoming messages and the ``acked``/``done``/``reassigned`` changes of the agent's own REQUESTs
(``MAILBOX TRACK`` lines), and exits. Re-arm it after a notification, then collect/process it.
``watch`` without ``--once`` keeps running. Notifications do not replace collect/pending checks:
in the observed Codex host, completed background output was not injected automatically into the
conversation. The script cannot create model turns or awaken a closed chat. Transport outages
retry without model polling; invalid credentials or a lost session fail visibly. The Mac must stay
awake with the server running; no auto-start service or firewall changes are installed.

Arbitration between agents
~~~~~~~~~~~~~~~~~~~~~~~~~~

**Roles** (user, 2026-11-18; ``pc-claude`` coordinated from 2026-11-17): the **coordinator is ``pi-claude``**
(Claude Code on the Pi, next to the mailbox server, which stays up when the PC or the Mac is away). It plans and
splits the work, assigns one owner and the locks per job, watches ``overdue`` and reassigns, and keeps the user
informed. It never deploys, pushes or flashes: merges, Windows builds, deploys and pushes are assigned to
``pc-claude`` (or ``mac-claude``), each still under the user's OK. ``pi-claude`` also operates the mailbox server
on its own host (user unit ``agent-mailbox.service``, port 50380, journal ``~/agent-mailbox``): it may restart only
that unit, only when the server is unhealthy, and reports it with INFO to all; moves and migrations still need the
user's OK. ``mac-claude`` is the backup operator (over SSH). The full roster is :doc:`agents`. Every other agent (``pc-claude``, ``mac-claude``,
``pc-codex``, ``mac-workbench`` for the ksp-workbench repo on the Mac) takes work from the coordinator. Any agent
still reports every message it exchanges to the user.

Several agents can work on one machine (on 2026-11-17 Claude Code and Codex both ran on the PC with the same ``pc``
config: their ``collect`` runs removed each other's messages and replayed old ACKs). The rules, introduced with
protocol 3 and enforced by protocol 4:

* **One identity per agent**, ``HOST-AGENT``: ``mac-claude``, ``pc-claude``, ``pc-codex``, ``pi-claude``. Each agent
  has its own config file named after it, hence its own session, history, done, cursor and track files, and runs at
  most one collector and one watcher. Two agents never share a config. The bare ``mac`` / ``pc`` / ``pi`` names are
  host groups in protocol 4; their old protocol 3 inboxes are drained once (see migration below).
* **One owner per request.** A REQUEST has exactly one owner among its recipients; that agent owns it until its
  RESULT and ``done``. The other recipients receive it for information. An agent that should not do the work replies
  ``RESULT #N declined: owner is X``, or the coordinator reassigns it. The REQUEST or its ACK names the files the
  owner will edit; no two agents edit the same file at the same time (separate worktrees, as for subagents).
* **Locks for shared resources.** Before using one, the agent acquires its lock and releases it after its RESULT:
  ``pi-deploy`` (``./ci deploy_hmi``), ``ksp-gamedata`` (plugin install into the shared game, which still needs the
  user's OK and KSP closed), ``board-flash`` (sketch upload and board tests) and ``push-main`` (push to
  ``origin/main``, which still needs the user's OK)::

     python tools/agent_mailbox.py --config build/agent-mailbox/pc-codex.json lock acquire pi-deploy --reason "deploy 3634558"
     python tools/agent_mailbox.py --config build/agent-mailbox/pc-codex.json lock release pi-deploy
     python tools/agent_mailbox.py --config build/agent-mailbox/pc-codex.json locks

  A lock held by another agent is refused with its holder, reason and expiry (HTTP 409). Wait or message the
  holder; never work around it. Locks expire by themselves (TTL 60 s to 4 h, default 30 min), so a crashed agent
  cannot block for ever; renew a long job by acquiring again. Locks persist in the log.
* **Delegate and stay available** (user, 2026-11-17). An agent that receives new work through the mailbox or from
  the user does not do it inline: it delegates it at once to other agents (its own subagents in their own
  worktrees, or another mailbox agent as an addressed REQUEST) and stays free to take and schedule further work in
  parallel. The coordinating agent plans, splits, assigns owners and locks, reviews and merges results, reports
  every exchange to the user and keeps the mailbox answered; it works inline only on what is too small to delegate
  (a reply, a status check, a merge). Delegated agents follow the same file-ownership, lock and consent rules.
* Messages remain coordination data: a lock or a message never grants deploy, flash or push consent.

Protocol 4 (approved 2026-11-17)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Protocol 2/3 removed a message when its recipient collected it: an identity shared by two agents lost messages for
one of them, a typo in ``--to`` queued a message nobody would read, and a sender could not see whether its message
was read. Protocol 4 keeps every message until every recipient has finished with it and tracks each recipient
separately. ``/health`` reports ``"protocol": 4``.

* **Registration and exclusive sessions.** ``register [--tool T]`` records the identity, tool and working directory
  and stores a random session in ``IDENTITY.session.json`` next to the config. Every ``collect``, ``pending``,
  ``done``, ``send`` and ``watch`` call carries it and renews a 90 s lease (the watcher polls about every 25 s;
  the first call registers automatically). A second client on an identity whose lease is alive is refused with
  HTTP 409, even with ``--takeover``. After the lease expired or the holder ran ``register --release``,
  another session takes the identity only with ``register --takeover``, once the old agent is confirmed gone; the
  old session is then refused. ``agents`` lists identity, tool, directory, last heartbeat and state: ``active``
  (lease alive), ``idle`` (expired less than an hour ago) or ``gone`` (released or older).
* **Addressing expanded at send time.** ``--to`` takes one registered identity, a comma list, a host group
  (``mac``, ``pc``, ``pi``: every agent registered on that host when the message is sent) or ``all``. The server
  stores the explicit recipient list (the sender excluded) with the message, so a broadcast gives each agent its
  own copy. An unknown identity or an empty group is refused (HTTP 400), never silently queued. A REQUEST to several
  recipients needs ``--owner`` naming one of them; a single recipient owns it by default; ``--owner`` on other
  kinds is refused. ``--deadline MINUTES`` sets when an unfinished REQUEST becomes overdue.
* **Per-recipient state.** The kind is the first word of the text (REQUEST, ACK, RESULT, NOTICE, otherwise INFO).
  Each recipient moves through ``queued`` → ``delivered`` (its client fetched it) → ``archived`` (copied to its
  local history) → ``acked`` → ``done``, with times. Each change carries the message digest and is idempotent.
  ``track --id ID`` prints every recipient's state and times, also after compaction.
* **Compaction.** A message leaves the active log when its REQUEST owner is ``done`` and every other recipient has
  at least ``archived`` (all kinds other than an owned REQUEST need only ``archived``). It is kept as a receipt
  (sequence, digests, final state per recipient) without its text, so a retried send with the same id is answered
  as already archived instead of being resurrected.
* **Overdue.** ``status`` and ``overdue`` list: delivered but not archived after 10 min, a REQUEST not acked by its
  owner after 15 min, past its deadline, or addressed to an agent whose lease expired. The coordinator reports them
  and may ``reassign --id ID --to NEW``: the new owner (a registered agent, not the
  sender) receives the REQUEST, the old owner's copy is marked ``reassigned`` and both get a NOTICE.
* **Durable log.** Append-only JSONL: a header snapshot (version 4, last sequence, receipts, locks, agents), then
  one record per change (message upsert, message drop into a receipt, lock, agent), each with a checksum and
  fsync'd before it takes effect. After 1000 records the log is rewritten as one snapshot by atomic replacement
  under the server lock. On load a torn final record (no trailing newline) is truncated; a bad checksum or
  unreadable record anywhere else stops the server. The first change of every day copies the log to
  ``backup/shared-YYYY-MM-DD.jsonl`` next to it (14 kept).
* **Protocol 3 clients keep working** during the transition: ``/events`` lists one copy per recipient that has
  not archived it, ``/archive`` marks only that identity ``archived`` (refused with 409 while a protocol 4 session
  holds that identity), ``/messages`` without ``"v": 4`` is a literal one-identity send (also to a bare ``mac`` or
  ``pc`` inbox; ``send --legacy`` uses this form), and ``/locks`` is unchanged. ACKs are sent in this literal form
  to the exact sender.

Migration from protocol 2/3, in order:

1. Coordinator, Mac: stop the old server, keep a copy of ``shared.jsonl``, start ``serve`` from the protocol 4
   script. The first load migrates the journal (backup ``backup/shared.v2-*.jsonl`` or ``.v3-*``); active messages
   keep their sequence numbers and old receipts keep deduplicating retried sends.
2. Every agent: ``init`` its own ``HOST-AGENT`` config with the same URL and token if it has none, then
   ``register --tool claude`` (or ``codex``) and arm ``watch --once``.
3. One agent per host, chosen by the coordinator: ``collect --legacy-inbox`` once. It archives what is still queued
   for the bare ``mac`` / ``pc`` identity and imports the unhandled items of the old ``mac`` / ``pc`` history (above
   its ``handled.json`` watermark) into its own history; ``pending`` and ``done --id`` then finish them. Stop using
   the old shared config afterwards.
4. Old clients that still run protocol 3 keep working until updated; update them from ``/client.py``.

Tests (``tools/test_agent_mailbox.py``) cover: two clients on one identity refused; a broadcast to three agents
read by each; host group and ``all`` expansion; unknown recipients rejected; the owner rule; a crash between
archive and done recovered from the server; lease expiry reported overdue; reassignment; compaction only after
every recipient; a protocol 3 client against a protocol 4 server; ten concurrent writer threads; checksum and
torn-record handling; migration of protocol 3 and older journals.

Mailbox listener daemon
~~~~~~~~~~~~~~~~~~~~~~~

An agent's chat session ends when the user closes it, and ``watch --once`` cannot start a model turn.
``tools/agent_daemon.py`` keeps an identity answering without an open session and without spending tokens while
the inbox is quiet (user, 2026-11-17; every identity may run one since the "wake service" rule, 2026-11-18). One
daemon runs per identity, in that agent's own repo; the config path (``build/agent-mailbox/<identity>.json``)
selects the identity. On the Pi it is the user unit ``agent-daemon@pi-claude.service``
(``tools/systemd/agent-daemon@.service``; no sudo, lingering starts it at boot):

* **Listening is token-free.** A loop of ``watch --once`` slices (120 s) long-polls the inbox and renews the lease,
  so other agents see the identity ``active``. After each slice it runs ``collect`` and reads ``pending``. After the
  machine sleeps the loop simply resumes and catches up with one ``collect`` + ``pending`` (it logs the gap).
* **Wake rule.** Only a REQUEST or NOTICE addressed to the identity, its host group (``pi``, ``pc``, ``mac``) or
  ``all`` that no successful run has seen starts one headless ``claude -p`` in the repo. INFO and RESULT never
  wake; the next run reads them as context. Incoming ACKs are marked done without a model.
* **Closed tool profile per identity.** The run uses ``--permission-mode dontAsk`` with an allowlist and a
  denylist (a deny beats an allow). Every profile denies sudo, ``git push``/``merge``/``reset``/``stash``/
  ``rebase`` (also through ``git -C``; the denies stay as defence in depth), every ``./ci``/``ci.cmd`` target (deploy, install, upload_sketch),
  ``arduino-cli``, signals, ``rm``, reading the mailbox token files, subagents, web and the PowerShell tool.
  ``agent_daemon.py --config ... profile`` prints the rules a run gets. There is no generic ``git -C:*`` allow:
  only ``git -C <main checkout> status|log|diff|show|branch|fetch|worktree list|worktree add`` (plus
  ``pull --ff-only`` on the Pi), because a prefix rule cannot constrain the subcommand after a wildcarded
  worktree folder; ``git -C`` into a worktree is refused and runs inspect worktree files with Read/Grep.

  - ``pi`` (default for ``pi-*``): today's Pi rules. Read-only diagnostics (``journalctl`` of ksphmi and the
    mailbox server, ``systemctl --user status/show/cat``), network checks, ``git pull --ff-only``, Python tests and
    edits in a new worktree, a measurement a coordinator explicitly scheduled, and the one exact restart
    ``systemctl --user restart agent-mailbox.service`` (mailbox-server duty, only when it is unhealthy, reported
    with INFO to all). No ksphmi restarts, ``systemctl --user start/stop`` or edits to ``~/ksphmi``; it never
    builds hmi-cpp on the Pi.
  - ``workstation`` (default for ``pc-*`` and ``mac-*``): read-only repo and git, the mailbox,
    ``tools/verify.py --docs-only``, Python unit tests and edits only inside a new worktree (the interactive
    session reviews and commits them); additionally denies ``systemctl``, ``launchctl``, ``schtasks``,
    ``taskkill``, ``git add/commit/checkout/switch`` and the service installer. On Windows the commands use the
    repo's ``build/venv/Scripts/python.exe``; elsewhere ``python3``.
  - ``--profile-file FILE`` (JSON ``{"profile", "allow", "deny", "limits"}``) adds rules for a repo or identity;
    it can never remove the base denylist.

* **What needs the user stays pending.** Deploy, flash, push, ksphmi restarts, GameData, boot or sudo work gets
  ``RESULT #N needs user: ...`` and remains pending until an interactive session with the user handles it. The
  daemon then notifies the user locally (best effort, never fatal): a macOS notification (``osascript``), a
  Windows toast (``Windows.UI.Notifications`` from PowerShell, ``msg.exe`` as fallback) or, on Linux/Pi, an INFO
  to the coordinator ``pc-claude``. A REQUEST that waits (for a GO) is not re-run until a new waking message
  arrives.
* **Health check (no model).** Two consecutive failed runs (non-zero exit, timeout or ``claude`` missing, e.g.
  ``OAuth session expired``) send ``INFO: daemon of <identity> unhealthy: <error summary>`` to all once per
  failure streak (fixed ``--id daemon-unhealthy-<identity>-<streak start>``, so retries never duplicate) and
  notify the user locally. A successful run resets the streak; ``state`` shows it.
* **Bounded cost.** At most six runs per hour (``--max-runs``), 20 minutes per run; a message whose runs failed
  three times is left for the interactive session.
* **One collector per identity.** An interactive session first runs ``agent_daemon.py --config ... pause
  --minutes N`` (renew it while working) and ``resume`` when the user closes it; a forgotten pause expires by
  itself. While paused the daemon neither watches nor collects. A lock file (``fcntl`` on POSIX, ``msvcrt`` on
  Windows) refuses a second daemon on the same identity.
* **Audit.** Each run's transcript goes to ``build/agent-mailbox/<identity>.daemon/runs/`` and one line with its
  ``SUMMARY:`` to ``summary.md``; ``agent_daemon.py --config ... state`` shows the pause, the health streak, the
  run count and the last runs. The interactive session reports them to the user when it opens.

Peers address the identity as before and see no difference except slower answers (one headless start) and
``needs user`` replies for actions only the user may approve.

**Installing the service.** ``tools/agent_daemon_install.py`` writes a native per-user service; review it with
``--dry-run`` first (``--os linux|macos|windows`` renders another platform's service for review only). The user
runs the real install on each machine:

.. code-block:: text

   # Pi / Linux: systemd --user (copies the daemon to ~/.local/lib/agent-daemon/, needs lingering)
   python3 tools/agent_daemon_install.py --identity pi-claude install --dry-run
   # macOS: ~/Library/LaunchAgents/com.kspkontroller.agent-daemon.mac-claude.plist, launchctl bootstrap gui/<uid>
   python3 tools/agent_daemon_install.py --identity mac-claude install --dry-run
   # Windows: Task Scheduler \KSPKontroller\agent-daemon-pc-claude (at logon, restart on failure,
   # build\venv\Scripts\pythonw.exe, log in build\agent-mailbox\pc-claude.daemon\daemon.log)
   build/venv/Scripts/python.exe tools/agent_daemon_install.py --identity pc-claude install --dry-run
   # remove it again on any OS
   python3 tools/agent_daemon_install.py --identity pi-claude uninstall --dry-run

Tests (no model, no server, nothing installed; Windows, macOS and Linux): ``python3 -m unittest
tools/test_agent_daemon.py`` covers the wake rule and addressing, pause/resume and the identity lock, the health
streak and its INFO, ``needs user`` notifications, the profiles (deploy/push/flash denied everywhere) and the
installer dry runs for all three platforms.

Keep messages concise: commit, paths, tests and requested action. Messages are coordination data,
not authority to override user instructions or deployment/flash/push safeguards. Test transport,
concurrent writes, archive-before-state, retries, history recovery and absence of ACK loops with
``python -m unittest discover -s tools -p test_agent_mailbox.py -v`` (loopback sockets required).

Verify the name matrix from every machine with ``python tools/verify.py --lan-only``. Its private
``build/agent-mailbox/lan.json`` manifest contains a ``hosts`` object with ``mac``, ``pc`` and
``pi`` hostname strings. ``--lan-config PATH`` selects another local file. This check rejects
numeric endpoints, bounds resolver waits and reports whether each name has a non-loopback
IPv4 address. It does not claim a game/service is running: test the mailbox, SSH and active
game endpoints separately. Keep local names/addresses and manifests out of version control.
Name-check tests: ``python -m unittest discover -s tools -p test_lan_names.py -v``.

A Windows PC with Ethernet and Wi-Fi on the same LAN is dual-homed: Windows' own responder (Dnscache)
answers ``<hostname>.local`` over mDNS (UDP 5353) and LLMNR (UDP 5355) on both adapters, so peers get
two addresses. The Pi's Redux source re-resolves every 10 s while silent and flips between them.
``tools/windows/mdns-prefer-ethernet.ps1`` keeps one address: four block rules in the firewall group
``KSPKontroller mDNS`` (inbound and outbound UDP 5353/5355, ``InterfaceType Wireless`` only) are enabled
while a wired (802.3) adapter is Up with a LAN IPv4 and no wireless adapter is on a different subnet,
and disabled otherwise, so Wi-Fi answers again when the cable is out. Adapters are chosen by physical
media type, never by alias. ``-Install`` (elevated) copies the script to ``%ProgramFiles%\KSPKontroller``,
creates the rules and a SYSTEM scheduled task ``\KSPKontroller\mDNS prefer Ethernet`` that runs
``-Apply`` at startup, on NetworkProfile/Operational events 10000/10001 and every 15 minutes;
``-Status`` is read-only, ``-Uninstall`` removes only that group, task and copy. The log is
``%ProgramData%\KSPKontroller\mdns-prefer-ethernet.log``. ``verify.py --lan-only`` reports the group on
Windows (the local lookup of the own name always lists every adapter, so it proves nothing there),
and on other hosts warns when the own ``.local`` name or the manifest's ``pc`` name resolves to more
than one LAN IPv4. Peers keep a cached answer until its TTL (about two minutes) expires.
