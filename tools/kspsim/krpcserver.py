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


def services_message() -> K.Services:
    services = K.Services()
    services.services.add(name="KRPC")
    sc = services.services.add(name="SpaceCenter")
    for cls in ("Vessel", "Orbit", "CelestialBody", "Flight", "ReferenceFrame"):
        sc.classes.add(name=cls)
    situation = sc.enumerations.add(name="VesselSituation")
    for i, name in enumerate(SITUATIONS):
        situation.values.add(name="".join(p.capitalize() for p in name.split("_")), value=i)
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

    def _serve_context(self, sock):
        while self._context is sock:
            try:
                data, peer = sock.recvfrom(256)
                if data == b"KSPHMI/1":
                    sock.sendto(("KSPHMI/1 " + self.sim.scene).encode("ascii"), peer)
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
        raise RpcFailure("kspsim does not implement the %s service" % service)

    def _krpc(self, client: ClientConn, proc: str, args: dict) -> bytes | None:
        if proc == "GetServices":
            return services_message().SerializeToString()
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

    def _space_center(self, proc: str, args: dict) -> bytes | None:
        sim = self.sim
        if proc == "get_ActiveVessel":  # real kRPC 0.7: null outside flight (seen at the Space Center), not an error
            return enc_object(sim.vessel_uid * 10) if sim.scene == "flight" else None
        if sim.scene != "flight":
            raise RpcFailure("Procedure not available in game scene '%s'" % "SpaceCenter")
        if proc == "get_UT":
            return enc_double(sim.ut)

        cls, _, member = proc.partition("_")
        if cls in ("Vessel", "Orbit", "Flight"):
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
                return enc_object(sim.vessel_uid * 10 + 2)
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
            values = {"get_MeanAltitude": r["altitude"], "get_SurfaceAltitude": max(r["altitude"], 0.0),
                      "get_Speed": r["surface_speed"], "get_VerticalSpeed": r["vertical_speed"],
                      "get_GForce": r["g_force"]}
            if member in values:
                return enc_double(values[member])
        elif cls == "CelestialBody":
            values = {"get_Name": enc_string("Kerbin"), "get_EquatorialRadius": enc_double(physics.RADIUS),
                      "get_HasAtmosphere": enc_bool(True), "get_AtmosphereDepth": enc_double(physics.ATMOSPHERE),
                      "get_GravitationalParameter": enc_double(physics.MU),
                      "get_ReferenceFrame": enc_object(FRAME_KERBIN)}
            if member in values:
                return values[member]
        raise RpcFailure("kspsim does not implement SpaceCenter.%s" % proc)
