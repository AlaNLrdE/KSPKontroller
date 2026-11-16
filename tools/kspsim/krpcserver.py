"""A stand-in for the kRPC server mod, enough for the HMI on the Raspberry Pi.

Speaks the real kRPC wire protocol (protobuf messages with a varint size prefix, an RPC port and a stream
port, protocol version 1 as in the krpc 0.7 client), so the Pi runs exactly the code it runs against KSP.
Only the SpaceCenter members the HMI reads are implemented; anything else is answered with an error naming
it, which shows up in the HMI log instead of failing silently.
"""
from __future__ import annotations

import logging
import math
import os
import socket
import struct
import threading
import time
from dataclasses import dataclass, field

from krpc.schema import KRPC_pb2 as K

from . import physics

log = logging.getLogger("kspsim.krpc")

PROTOCOL_VERSION = 1
SITUATIONS = list(physics.SITUATIONS)
BODY_KERBIN, FRAME_KERBIN = 1, 2


# ---------------------------------------------------------------------------------------------- encoding
def varint(n: int) -> bytes:
    out = bytearray()
    while True:
        b = n & 0x7F
        n >>= 7
        if n:
            out.append(b | 0x80)
        else:
            out.append(b)
            return bytes(out)


def read_varint(data: bytes) -> int:
    n = shift = 0
    for b in data:
        n |= (b & 0x7F) << shift
        if not b & 0x80:
            return n
        shift += 7
    raise ValueError("truncated varint")


def read_string(data: bytes) -> str:
    """A protobuf string argument: varint length, then UTF-8 bytes."""
    n = shift = i = 0
    while True:
        if i >= len(data):
            raise ValueError("truncated string")
        b = data[i]
        i += 1
        n |= (b & 0x7F) << shift
        if not b & 0x80:
            break
        shift += 7
    return data[i:i + n].decode("utf-8")


def enc_double(v: float) -> bytes:
    return struct.pack("<d", v)


def enc_float(v: float) -> bytes:
    return struct.pack("<f", v)


def enc_bool(v: bool) -> bytes:
    return varint(1 if v else 0)


def enc_string(s: str) -> bytes:
    b = s.encode("utf-8")
    return varint(len(b)) + b


def enc_object(object_id: int) -> bytes:
    return varint(object_id)


def enc_tuple(values):
    return K.Tuple(items=[enc_double(v) for v in values]).SerializeToString()


def enc_list(values):
    return K.List(items=[enc_object(v) for v in values]).SerializeToString()


def enc_enum(v: int) -> bytes:
    return varint((v << 1) ^ (v >> 31))  # zigzag sint32


class RpcFailure(Exception):
    def __init__(self, description: str, service: str = "", name: str = "") -> None:
        super().__init__(description)
        self.description, self.service, self.name = description, service, name


def recv_exact(sock: socket.socket, n: int) -> bytes:
    data = bytearray()
    while len(data) < n:
        chunk = sock.recv(n - len(data))
        if not chunk:
            raise ConnectionError("connection closed")
        data += chunk
    return bytes(data)


def recv_message(sock: socket.socket, typ):
    prefix = bytearray()
    while True:
        b = recv_exact(sock, 1)
        prefix += b
        if not b[0] & 0x80:
            break
    msg = typ()
    msg.ParseFromString(recv_exact(sock, read_varint(prefix)))
    return msg


def send_message(sock: socket.socket, msg) -> None:
    data = msg.SerializeToString()
    sock.sendall(varint(len(data)) + data)


# The game scenes KSPSerialIO.Hmi's KSPHMI service is registered for (GameScene.Editor = VAB and SPH)
KSPHMI_SCENES = ("vab", "sph")


def services_message(ksphmi: bool = True) -> K.Services:
    services = K.Services()
    services.services.add(name="KRPC")
    if ksphmi:  # KSPSerialIO.Hmi.dll's service: no generated client stubs, so the procedures carry their types
        hmi = services.services.add(name="KSPHMI")
        for name, code in (("Revision", K.Type.UINT32), ("ShipReport", K.Type.STRING)):
            proc = hmi.procedures.add(name=name)
            proc.return_type.code = code
            proc.game_scenes.extend([K.Procedure.EDITOR_VAB, K.Procedure.EDITOR_SPH])
        press = hmi.procedures.add(name="Press")  # void Press(string button): the HMI keyboard's T / R
        press.parameters.add(name="button").type.code = K.Type.STRING
        press.game_scenes.extend([K.Procedure.EDITOR_VAB, K.Procedure.EDITOR_SPH])
    sc = services.services.add(name="SpaceCenter")
    for cls in ("Vessel", "Orbit", "CelestialBody", "Flight", "ReferenceFrame", "Control", "Node", "DockingPort",
                "Resources", "Stage", "Part", "Parts"):
        sc.classes.add(name=cls)
    situation = sc.enumerations.add(name="VesselSituation")
    for i, name in enumerate(SITUATIONS):
        situation.values.add(name="".join(p.capitalize() for p in name.split("_")), value=i)
    for enum, names in (('SpeedMode', ('Orbit', 'Surface', 'Target')),
                        ('SASMode', ('StabilityAssist', 'Maneuver', 'Prograde', 'Retrograde', 'Normal',
                                     'AntiNormal', 'Radial', 'AntiRadial', 'Target', 'AntiTarget'))):
        item = sc.enumerations.add(name=enum)
        for i, name in enumerate(names):
            item.values.add(name=name, value=i)
    return services


# ---------------------------------------------------------------------------------------------- server
@dataclass
class ClientConn:
    ident: bytes
    name: str
    address: str
    rpc: socket.socket
    stream: socket.socket | None = None
    streams: dict = field(default_factory=dict)  # id -> [ProcedureCall, rate]
    calls: int = 0
    connected_at: float = field(default_factory=time.monotonic)


class KrpcServer:
    """Listens on the kRPC ports while ``sim`` says the game is running; one thread per connection."""

    def __init__(self, sim, address: str = "0.0.0.0", rpc_port: int = 50000, stream_port: int = 50001,
                 max_stream_rate: float = 20.0, context_port: int = 50002) -> None:
        self.sim = sim
        self.address, self.rpc_port, self.stream_port = address, rpc_port, stream_port
        self.max_stream_rate = max_stream_rate
        self.clients: dict[bytes, ClientConn] = {}
        self._lock = threading.Lock()
        self._next_stream = 1
        self._listeners: list[socket.socket] = []
        self.error = ""
        self._context = None
        self.context_port = context_port

    # -- lifecycle ----------------------------------------------------------------------------------
    @property
    def listening(self) -> bool:
        return bool(self._listeners)

    def start(self) -> None:
        if self._listeners:
            return
        try:
            for port, handler in ((self.rpc_port, self._rpc_conn), (self.stream_port, self._stream_conn)):
                s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                if hasattr(socket, "SO_EXCLUSIVEADDRUSE"):  # Windows: refuse to share the port with a real kRPC
                    s.setsockopt(socket.SOL_SOCKET, socket.SO_EXCLUSIVEADDRUSE, 1)
                else:  # POSIX: rebind right after stop() although closed connections are still in TIME_WAIT
                    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
                s.bind((self.address, port))
                s.listen(8)
                self._listeners.append(s)
                threading.Thread(target=self._accept, args=(s, handler), daemon=True,
                                 name="krpc-accept-%d" % port).start()
                port = s.getsockname()[1]
                if handler == self._rpc_conn:
                    self.rpc_port = port
                else:
                    self.stream_port = port
            context = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            self._context = context
            context.bind((self.address, self.context_port))
            self.context_port = context.getsockname()[1]
            context.settimeout(0.2)
            threading.Thread(target=self._serve_context, args=(context,), daemon=True).start()
            self.error = ""
            log.info("kRPC stand-in listening on %s:%d/%d", self.address, self.rpc_port, self.stream_port)
        except OSError as e:
            self.error = "cannot listen on %s:%d-%d: %s" % (self.address, self.rpc_port, self.stream_port, e)
            log.error(self.error)
            self.stop()

    def context_reply(self) -> str:
        """``KSPHMI/1 <scene>[ skin=<name>]``: the optional skin field is the simulator's own (KSPSerialIO sends none)."""
        reply = "KSPHMI/1 " + self.sim.scene
        return reply + (" skin=" + self.sim.skin if self.sim.skin else "")

    def _serve_context(self, sock):
        while self._context is sock:
            try:
                data, peer = sock.recvfrom(256)
                if data == b"KSPHMI/1":
                    sock.sendto(self.context_reply().encode("ascii"), peer)
            except socket.timeout:
                continue
            except OSError:
                return

    def stop(self) -> None:
        """Close the ports and drop every client, as when KSP quits."""
        if self._context is not None:
            self._context.close()
            self._context = None
        for s in self._listeners:
            try:
                s.close()
            except OSError:
                pass
        self._listeners = []
        with self._lock:
            clients, self.clients = list(self.clients.values()), {}
        for c in clients:
            for s in (c.rpc, c.stream):
                if s is not None:
                    try:
                        s.shutdown(socket.SHUT_RDWR)
                        s.close()
                    except OSError:
                        pass

    def _accept(self, listener: socket.socket, handler) -> None:
        while True:
            try:
                sock, addr = listener.accept()
            except OSError:
                return  # listener closed
            sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            threading.Thread(target=handler, args=(sock, addr), daemon=True).start()

    # -- connections --------------------------------------------------------------------------------
    def _rpc_conn(self, sock: socket.socket, addr) -> None:
        client = None
        try:
            req = recv_message(sock, K.ConnectionRequest)
            if req.type != K.ConnectionRequest.RPC:
                send_message(sock, K.ConnectionResponse(status=K.ConnectionResponse.WRONG_TYPE,
                                                        message="this is the RPC port"))
                return
            client = ClientConn(os.urandom(16), req.client_name or "unnamed", addr[0], sock)
            send_message(sock, K.ConnectionResponse(status=K.ConnectionResponse.OK,
                                                    client_identifier=client.ident,
                                                    protocol_version=PROTOCOL_VERSION))
            with self._lock:
                self.clients[client.ident] = client
            log.info("client '%s' connected from %s", client.name, client.address)
            while True:
                request = recv_message(sock, K.Request)
                response = K.Response()
                for call in request.calls:
                    response.results.append(self._result(client, call))
                    client.calls += 1
                send_message(sock, response)
        except (ConnectionError, OSError) as e:
            if client:
                log.info("client '%s' disconnected (%s)", client.name, e)
        finally:
            if client:
                with self._lock:
                    self.clients.pop(client.ident, None)
                if client.stream:
                    try:
                        client.stream.close()
                    except OSError:
                        pass
            try:
                sock.close()
            except OSError:
                pass

    def _stream_conn(self, sock: socket.socket, addr) -> None:
        try:
            req = recv_message(sock, K.ConnectionRequest)
            with self._lock:
                client = self.clients.get(bytes(req.client_identifier))
            if req.type != K.ConnectionRequest.STREAM or client is None:
                send_message(sock, K.ConnectionResponse(status=K.ConnectionResponse.MALFORMED_MESSAGE,
                                                        message="unknown client identifier"))
                sock.close()
                return
            client.stream = sock
            send_message(sock, K.ConnectionResponse(status=K.ConnectionResponse.OK,
                                                    protocol_version=PROTOCOL_VERSION))
            self._stream_loop(client)
        except (ConnectionError, OSError):
            pass
        finally:
            try:
                sock.close()
            except OSError:
                pass

    def _stream_loop(self, client: ClientConn) -> None:
        while client.ident in self.clients:
            rates = [r for _, r in client.streams.values() if r > 0]
            rate = min(self.max_stream_rate, max(rates)) if rates else self.max_stream_rate
            update = K.StreamUpdate()
            for sid, (call, _) in list(client.streams.items()):
                update.results.add(id=sid).result.CopyFrom(self._result(client, call))
            if update.results:
                send_message(client.stream, update)
            time.sleep(1.0 / rate)

    # -- procedures ---------------------------------------------------------------------------------
    def _result(self, client: ClientConn, call) -> K.ProcedureResult:
        result = K.ProcedureResult()
        try:
            value = self._call(client, call.service, call.procedure, {a.position: a.value for a in call.arguments})
            if value is None:
                result.is_null = True
            else:
                result.value = value
        except RpcFailure as e:
            result.error.CopyFrom(K.Error(service=e.service, name=e.name, description=e.description))
        return result

    def _call(self, client: ClientConn, service: str, proc: str, args: dict) -> bytes | None:
        if service == "KRPC":
            return self._krpc(client, proc, args)
        if service == "SpaceCenter":
            with self.sim.lock:
                return self._space_center(proc, args)
        if service == "KSPHMI" and self.sim.ship:
            return self._ksphmi(proc, args)
        raise RpcFailure("kspsim does not implement the %s service" % service)

    def _ksphmi(self, proc: str, args: dict) -> bytes | None:
        """KSPSerialIO.Hmi's editor ship report. kRPC refuses an Editor-scene procedure in any other scene."""
        sim = self.sim
        if sim.scene not in KSPHMI_SCENES:
            raise RpcFailure("Procedure not available in game scene '%s'" % sim.scene, "KRPC", "InvalidOperationException")
        if proc == "Revision":
            return varint(sim.ship_revision)
        if proc == "ShipReport":
            return enc_string(sim.ship_report())
        if proc == "Press":
            button = read_string(args.get(0, b""))
            if button not in ("SAS", "RCS"):
                raise RpcFailure("button must be SAS or RCS, not '%s'" % button, "KRPC", "ArgumentException")
            sim.editor_press(button)
            return None
        raise RpcFailure("kspsim does not implement KSPHMI.%s" % proc)

    def _krpc(self, client: ClientConn, proc: str, args: dict) -> bytes | None:
        if proc == "GetServices":
            return services_message(bool(self.sim.ship)).SerializeToString()
        if proc == "GetStatus":
            return K.Status(version="kspsim", protocol_version=PROTOCOL_VERSION).SerializeToString()
        if proc == "GetClientName":
            return enc_string(client.name)
        if proc == "AddStream":
            call = K.ProcedureCall()
            call.ParseFromString(args.get(0, b""))
            first = self._result(client, call)  # a call that fails now fails at AddStream, as in kRPC
            if first.HasField("error"):
                raise RpcFailure(first.error.description, first.error.service, first.error.name)
            with self._lock:
                sid = self._next_stream
                self._next_stream += 1
            client.streams[sid] = [call, 0.0]
            return K.Stream(id=sid).SerializeToString()
        if proc == "RemoveStream":
            client.streams.pop(read_varint(args[0]), None)
            return None
        if proc == "SetStreamRate":
            sid = read_varint(args[0])
            if sid in client.streams:
                client.streams[sid][1] = struct.unpack("<f", args[1])[0]
            return None
        if proc == "StartStream":
            return None
        raise RpcFailure("kspsim does not implement KRPC.%s" % proc)

    def _vessel(self, args: dict, pos: int = 0) -> physics.Vessel:
        v = self.sim.vessel
        want = read_varint(args[pos]) if pos in args else 0
        if v is None or self.sim.scene != "flight" or want // 10 != self.sim.vessel_uid:
            raise RpcFailure("No such vessel %d" % want, "KRPC", "InvalidOperationException")
        return v

    def _node_orbit(self, proc: str, args: dict) -> bytes:
        """Node.orbit: the post-burn conic, as state vectors in Kerbin's non-rotating frame (any frame argument)."""
        sim = self.sim
        r = sim.reported()
        if not r['node_enabled']:
            raise RpcFailure('The maneuver node was removed', 'KRPC', 'InvalidOperationException')
        pos, vel = physics.maneuver_state(r)
        member = proc.partition('_')[2]
        if member in ('PositionAt', 'VelocityAt'):
            node_ut = sim.ut + r['node_time']
            pos, vel = physics.propagate3(pos, vel, struct.unpack('<d', args[1])[0] - node_ut)
            return enc_tuple(pos if member == 'PositionAt' else vel)
        ap, pe, e, a = physics.extremes(pos, vel)
        values = {'get_Periapsis': pe + physics.RADIUS, 'get_Apoapsis': ap + physics.RADIUS,
                  'get_PeriapsisAltitude': pe, 'get_ApoapsisAltitude': ap, 'get_Eccentricity': e,
                  'get_SemiMajorAxis': a}
        if member in values:
            return enc_double(values[member])
        if member == 'get_Body':
            return enc_object(BODY_KERBIN)
        raise RpcFailure("kspsim does not implement SpaceCenter.%s for a node orbit" % proc)

    def _space_center(self, proc: str, args: dict) -> bytes | None:
        sim = self.sim
        if proc == "get_ActiveVessel":  # real kRPC 0.7: null outside flight (seen at the Space Center), not an error
            return enc_object(sim.vessel_uid * 10) if sim.scene == "flight" else None
        if sim.scene != "flight":
            raise RpcFailure("Procedure not available in game scene '%s'" % "SpaceCenter")
        if proc == "get_UT":
            return enc_double(sim.ut)
        if proc in ('get_TargetVessel', 'get_TargetDockingPort'):
            return None
        if proc == 'get_TargetBody':
            return enc_object(BODY_KERBIN) if sim.reported()['target_enabled'] else None
        if proc == 'TransformDirection':
            r = sim.reported()
            frame = read_varint(args[1]) % 10
            mode = {6: 'orbital_speed', 7: 'surface_speed'}.get(frame)
            speed = r[mode] if mode else math.sqrt(sum(r['relative_' + a] ** 2 for a in ('up', 'north', 'east')))
            vertical = r['vertical_speed'] if mode else r['relative_up']
            pro = (vertical, 0, math.sqrt(max(0, speed * speed - vertical * vertical))) if mode else tuple(r['relative_' + a] for a in ('up', 'north', 'east'))
            length = math.sqrt(sum(x*x for x in pro)) or 1
            pro = tuple(x / length for x in pro)
            # A planar orbit has northward anti-normal; radial is perpendicular to velocity.
            horizontal = math.hypot(pro[1], pro[2])
            normal = (0, -pro[2] / horizontal, pro[1] / horizontal) if horizontal > 1e-8 else (0, -1, 0)
            radial = (pro[1]*normal[2] - pro[2]*normal[1], pro[2]*normal[0] - pro[0]*normal[2],
                      pro[0]*normal[1] - pro[1]*normal[0])
            value = K.Tuple()
            value.ParseFromString(args[0])
            x, y, z = (struct.unpack('<d', item)[0] for item in value.items)
            return enc_tuple(tuple(-x * radial[i] + y * pro[i] + z * normal[i] for i in range(3)))

        if proc.startswith(('Resources_', 'Stage_', 'Parts_')):
            ident = read_varint(args[0])
            if ident // 10000 != sim.vessel_uid:
                raise RpcFailure('Resource/stage object belongs to a previous vessel')
            r = sim.reported()
            if proc == 'Parts_get_All':
                count = max(0, min(100, int(r['stage_parts']))) * (max(0, int(r['stage_number'])) + 1)
                return enc_list([sim.vessel_uid * 10000 + 5000 + i for i in range(count)])
            if proc.startswith('Resources_'):
                data = args[1]
                length = read_varint(data)
                offset = len(varint(length))
                name = data[offset:offset + length].decode('utf-8')
                fields = {'ElectricCharge': ('electric_charge', 'electric_capacity'),
                          'LiquidFuel': ('liquid_fuel', 'liquid_capacity'), 'Oxidizer': ('oxidizer', 'oxidizer_capacity'),
                          'MonoPropellant': ('monopropellant', 'monoprop_capacity'),
                          'SolidFuel': ('solid_fuel', 'solid_capacity'), 'XenonGas': ('xenon_gas', 'xenon_capacity')}
                if proc in ('Resources_Amount', 'Resources_Max'):
                    field = fields.get(name)
                    return enc_float(r[field[0 if proc == 'Resources_Amount' else 1]]) if field else enc_float(0)
            else:
                number = ident % 10000 - 2000
                if proc == 'Stage_get_Number':
                    return enc_enum(number)
                if proc == 'Stage_get_DeltaV':
                    return enc_float(r['stage_delta_v'] if number == int(r['stage_number']) else physics.MAX_ACCEL * physics.BURN_TIME)
                if proc == 'Stage_get_Parts':
                    count = max(0, min(100, int(r['stage_parts'])))
                    return enc_list([sim.vessel_uid * 10000 + 3000 + number * 100 + i for i in range(count)])

        if proc.startswith('Orbit_') and args.get(0) is not None and read_varint(args[0]) == sim.vessel_uid * 10000 + 6000:
            return self._node_orbit(proc, args)

        cls, _, member = proc.partition("_")
        if cls in ("Vessel", "Orbit", "Flight", "Control", "Node"):
            self._vessel(args)  # a stale object id fails as in kRPC
            r = sim.reported()  # the flight model's values, or the ones typed in manual mode
        if cls == "Vessel":
            if member == "get_Name":
                return enc_string(r["name"])
            if member == "get_Orbit":
                return enc_object(sim.vessel_uid * 10 + 1)
            if member == "get_Situation":
                return enc_enum(SITUATIONS.index(r["situation"]))
            if member == "Flight":
                frame = read_varint(args.get(1, b'\x00'))
                offset = {5: 3, 6: 6, 7: 7, 8: 8}.get(frame % 10, 2)
                return enc_object(sim.vessel_uid * 10 + offset)
            if member == 'get_Control':
                return enc_object(sim.vessel_uid * 10 + 4)
            if member in ('Position', 'Velocity'):
                prefix = member.lower()
                return enc_tuple(tuple(r[prefix + '_' + axis] for axis in ('x', 'y', 'z')))
            if member == 'get_Resources':
                return enc_object(sim.vessel_uid * 10000 + 1000)
            if member == 'get_Parts':
                return enc_object(sim.vessel_uid * 10000 + 4000)
            if member == 'get_Stages':
                current = max(0, min(20, int(r['stage_number'])))
                return enc_list([sim.vessel_uid * 10000 + 2000 + number for number in range(current + 1)])
            frames = {'get_SurfaceReferenceFrame': 5, 'get_OrbitSpeedReferenceFrame': 6,
                      'get_SurfaceSpeedReferenceFrame': 7, 'get_TargetSpeedReferenceFrame': 8}
            if member in frames:
                return enc_object(sim.vessel_uid * 10 + frames[member])
            values = {'get_Mass': r['mass'], 'get_AvailableThrust': r['available_thrust'], 'get_SpecificImpulse': r['specific_impulse']}
            if member in values:
                return enc_float(values[member])
        elif cls == 'Control':
            if member == 'get_CurrentStage':
                return enc_enum(int(r['stage_number']))
            if member == 'get_Nodes':
                return enc_list([sim.vessel_uid * 10 + 9] if r['node_enabled'] else [])
            if member == 'get_SpeedMode':
                return enc_enum(int(r['speed_mode']))
            if member == 'get_SASMode':
                return enc_enum((0, 2, 3, 4, 5, 7, 6, 8, 9, 1)[sim.sas_mode - 1])
            if member in ('set_SAS', 'set_RCS'):  # the HMI keyboard's T / R in flight
                sim.set_game(**{member[4:].lower(): bool(read_varint(args.get(1, b'\x00')))})
                return None
            if member in ('get_SAS', 'get_RCS'):
                return enc_bool(bool(r['action_groups'] & (1 if member == 'get_SAS' else 2)))
            if member == 'get_Throttle':
                return enc_float(sim.vessel.controls.throttle)
        elif cls == 'Node':
            values = {'get_RemainingDeltaV': r['node_remaining'], 'get_DeltaV': r['node_dv'], 'get_TimeTo': r['node_time']}
            if member in values:
                return enc_double(values[member])
            if member == 'BurnVector':
                return enc_tuple(tuple(r['burn_' + a] for a in ('up', 'north', 'east')))
            # The node's own delta-v (prograde only: the model is planar) and the orbit it results in
            if member == 'get_UT':
                return enc_double(sim.ut + r['node_time'])
            if member in ('get_Prograde', 'get_Normal', 'get_Radial'):
                return enc_double(r['node_' + member[4:].lower()])
            if member == 'get_Orbit':
                if not r['node_enabled']:
                    raise RpcFailure('No such node', 'KRPC', 'InvalidOperationException')
                return enc_object(sim.vessel_uid * 10000 + 6000)
        elif cls == "Orbit":
            values = {
                "get_ApoapsisAltitude": r["apoapsis"], "get_PeriapsisAltitude": r["periapsis"],
                "get_TimeToApoapsis": r["time_to_apoapsis"], "get_TimeToPeriapsis": r["time_to_periapsis"],
                "get_Inclination": math.radians(r["inclination"]), "get_Eccentricity": r["eccentricity"],
                "get_Period": r["period"], "get_SemiMajorAxis": r["semi_major_axis"],
                "get_TrueAnomaly": r["true_anomaly"], "get_Speed": r["orbital_speed"],
                "get_Radius": r["altitude"] + physics.RADIUS,
            }
            if member in values:
                return enc_double(values[member])
            if member == "get_Body":
                return enc_object(BODY_KERBIN)
        elif cls == "Flight":
            if member in ('get_Pitch', 'get_Heading', 'get_Roll'):
                return enc_float(r[member[4:].lower()])
            if member == 'get_Rotation':
                return enc_tuple(physics.attitude_rotation(r['pitch'], r['heading'], r['roll']))
            values = {"get_MeanAltitude": r["altitude"], "get_SurfaceAltitude": max(r["altitude"], 0.0),
                      "get_Speed": r["surface_speed"], "get_VerticalSpeed": r["vertical_speed"],
                      "get_GForce": r["g_force"]}
            if member in values:
                if member == 'get_Speed':
                    frame = read_varint(args[0]) % 10
                    if frame == 6:
                        return enc_double(r['orbital_speed'])
                    if frame == 8:
                        return enc_double(math.sqrt(sum(r['relative_' + a] ** 2 for a in ('up', 'north', 'east'))))
                if member == 'get_GForce':
                    return enc_float(values[member])
                return enc_double(values[member])
        elif cls == "CelestialBody":
            if member == 'Position':
                r = sim.reported()
                return enc_tuple(tuple(r['target_' + a] for a in ('up', 'north', 'east')))
            values = {"get_Name": enc_string("Kerbin"), "get_EquatorialRadius": enc_double(physics.RADIUS),
                      "get_HasAtmosphere": enc_bool(True), "get_AtmosphereDepth": enc_double(physics.ATMOSPHERE),
                      "get_GravitationalParameter": enc_double(physics.MU),
                      "get_ReferenceFrame": enc_object(FRAME_KERBIN)}
            values['get_NonRotatingReferenceFrame'] = enc_object(3)
            values['get_SphereOfInfluence'] = enc_double(physics.SOI_RADIUS)
            if member in values:
                return values[member]
        raise RpcFailure("kspsim does not implement SpaceCenter.%s" % proc)
