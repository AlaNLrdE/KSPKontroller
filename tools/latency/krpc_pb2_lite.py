"""A pure-Python stand-in for ``krpc.schema.KRPC_pb2`` (test fixture only).

The simulator's kRPC stand-in (tools/kspsim/krpcserver.py) imports the generated protobuf module of the
``krpc`` package, which needs ``protobuf``. The Raspberry Pi has neither (stdlib only, no pip), so the latency
tool's end-to-end test registers this module under that name before importing kspsim. It implements only
the protobuf API surface kspsim uses (keyword constructors, nested enums, repeated ``add``/``extend``,
``CopyFrom``, ``HasField``, ``SerializeToString``/``ParseFromString``) with the field numbers of kRPC 0.7's
KRPC.proto. It is independent of the latency client's hand-written encoder (udp_vs_krpc.py, which mirrors
hmi-cpp/src/krpc.cpp), so the test checks one implementation against another.
"""
from __future__ import annotations

import struct
import sys
import types

VARINT, I64, LEN, I32 = 0, 1, 2, 5
SCALAR_DEFAULTS = {"uint": 0, "int": 0, "enum": 0, "bool": False, "bytes": b"", "string": "", "double": 0.0}


def _varint(n: int) -> bytes:
    if n < 0:
        n += 1 << 64
    out = bytearray()
    while True:
        b = n & 0x7F
        n >>= 7
        if n:
            out.append(b | 0x80)
        else:
            out.append(b)
            return bytes(out)


def _read_varint(data: bytes, i: int):
    n = shift = 0
    while True:
        if i >= len(data):
            raise ValueError("truncated varint")
        b = data[i]
        i += 1
        n |= (b & 0x7F) << shift
        if not b & 0x80:
            return n, i
        shift += 7


class Repeated(list):
    def __init__(self, kind) -> None:
        super().__init__()
        self.kind = kind

    def add(self, **kw):
        item = self.kind(**kw)
        self.append(item)
        return item


class Message:
    """Fields: ``{name: (number, kind, repeated)}``; kind is a scalar name or a Message subclass."""
    FIELDS: dict = {}

    def __init__(self, **kw) -> None:
        for name, value in kw.items():
            if self.FIELDS[name][2]:
                getattr(self, name).extend(value)
            else:
                setattr(self, name, value)

    def __getattr__(self, name):
        spec = type(self).FIELDS.get(name)
        if spec is None:
            raise AttributeError(name)
        _, kind, repeated = spec
        if repeated:
            value = Repeated(kind)
        elif isinstance(kind, type):
            value = kind()
        else:
            return SCALAR_DEFAULTS[kind]
        object.__setattr__(self, name, value)  # sub-messages become present once touched (enough for kspsim)
        return value

    def HasField(self, name: str) -> bool:
        return name in self.__dict__

    def CopyFrom(self, other: "Message") -> None:
        self.__dict__.clear()
        self.ParseFromString(other.SerializeToString())

    def SerializeToString(self) -> bytes:
        out = bytearray()
        for name, (num, kind, repeated) in sorted(type(self).FIELDS.items(), key=lambda f: f[1][0]):
            if name not in self.__dict__:
                continue
            value = self.__dict__[name]
            for v in (value if repeated else [value]):
                if isinstance(kind, type):
                    body = v.SerializeToString()
                    out += _varint(num << 3 | LEN) + _varint(len(body)) + body
                elif kind in ("bytes", "string"):
                    b = v.encode("utf-8") if kind == "string" else bytes(v)
                    if b or repeated:
                        out += _varint(num << 3 | LEN) + _varint(len(b)) + b
                elif kind == "double":
                    if v or repeated:
                        out += _varint(num << 3 | I64) + struct.pack("<d", v)
                elif v or repeated:
                    out += _varint(num << 3 | VARINT) + _varint(int(v))
        return bytes(out)

    def ParseFromString(self, data: bytes) -> None:
        data = bytes(data)
        by_number = {spec[0]: (name, spec[1], spec[2]) for name, spec in type(self).FIELDS.items()}
        i = 0
        while i < len(data):
            tag, i = _read_varint(data, i)
            num, wire = tag >> 3, tag & 7
            if wire == VARINT:
                raw, i = _read_varint(data, i)
            elif wire == I64:
                raw, i = data[i:i + 8], i + 8
            elif wire == I32:
                raw, i = data[i:i + 4], i + 4
            elif wire == LEN:
                n, i = _read_varint(data, i)
                raw, i = data[i:i + n], i + n
            else:
                raise ValueError("unsupported wire type %d" % wire)
            if num not in by_number:
                continue
            name, kind, repeated = by_number[num]
            if isinstance(kind, type):
                values = [kind()]
                values[0].ParseFromString(raw)
            elif kind == "string":
                values = [raw.decode("utf-8")]
            elif kind == "bytes":
                values = [raw]
            elif kind == "double":
                values = [struct.unpack("<d", raw)[0]]
            elif wire == LEN:  # packed repeated varints
                values, j = [], 0
                while j < len(raw):
                    v, j = _read_varint(raw, j)
                    values.append(v)
            else:
                values = [bool(raw) if kind == "bool" else raw]
            if repeated:
                getattr(self, name).extend(values)
            else:
                object.__setattr__(self, name, values[-1])


def _message(name: str, fields: dict, **enums) -> type:
    return type(name, (Message,), dict(FIELDS=fields, **enums))


Error = _message("Error", {"service": (1, "string", False), "name": (2, "string", False),
                           "description": (3, "string", False), "stack_trace": (4, "string", False)})
ConnectionRequest = _message("ConnectionRequest", {"type": (1, "enum", False), "client_name": (2, "string", False),
                                                   "client_identifier": (3, "bytes", False),
                                                   "protocol_version": (4, "uint", False)}, RPC=0, STREAM=1)
ConnectionResponse = _message("ConnectionResponse", {"status": (1, "enum", False), "message": (2, "string", False),
                                                     "client_identifier": (3, "bytes", False),
                                                     "protocol_version": (4, "uint", False)},
                              OK=0, MALFORMED_MESSAGE=1, TIMEOUT=2, WRONG_TYPE=3)
Argument = _message("Argument", {"position": (1, "uint", False), "value": (2, "bytes", False)})
ProcedureCall = _message("ProcedureCall", {"service": (1, "string", False), "procedure": (2, "string", False),
                                           "arguments": (3, Argument, True), "service_id": (4, "uint", False),
                                           "procedure_id": (5, "uint", False)})
Request = _message("Request", {"calls": (1, ProcedureCall, True)})
ProcedureResult = _message("ProcedureResult", {"error": (1, Error, False), "value": (2, "bytes", False),
                                               "is_null": (3, "bool", False)})
Response = _message("Response", {"error": (1, Error, False), "results": (2, ProcedureResult, True)})
StreamResult = _message("StreamResult", {"id": (1, "uint", False), "result": (2, ProcedureResult, False)})
StreamUpdate = _message("StreamUpdate", {"results": (1, StreamResult, True)})
Stream = _message("Stream", {"id": (1, "uint", False)})
Status = _message("Status", {"version": (1, "string", False), "protocol_version": (22, "uint", False)})
Tuple = _message("Tuple", {"items": (1, "bytes", True)})
List = _message("List", {"items": (1, "bytes", True)})

Type = _message("Type", {"code": (1, "enum", False), "service": (2, "string", False), "name": (3, "string", False)},
                NONE=0, DOUBLE=1, FLOAT=2, SINT32=3, SINT64=4, UINT32=5, UINT64=6, BOOL=7, STRING=8, BYTES=9,
                CLASS=100, ENUMERATION=101)
Type.FIELDS["types"] = (4, Type, True)
Parameter = _message("Parameter", {"name": (1, "string", False), "type": (2, Type, False),
                                   "default_value": (3, "bytes", False), "nullable": (4, "bool", False)})
Procedure = _message("Procedure", {"name": (1, "string", False), "parameters": (2, Parameter, True),
                                   "return_type": (3, Type, False), "return_is_nullable": (4, "bool", False),
                                   "documentation": (5, "string", False), "game_scenes": (6, "enum", True)},
                     SPACE_CENTER=0, FLIGHT=1, TRACKING_STATION=2, EDITOR_VAB=3, EDITOR_SPH=4, MISSION_BUILDER=5)
Class = _message("Class", {"name": (1, "string", False), "documentation": (2, "string", False)})
EnumerationValue = _message("EnumerationValue", {"name": (1, "string", False), "value": (2, "int", False),
                                                 "documentation": (3, "string", False)})
Enumeration = _message("Enumeration", {"name": (1, "string", False), "values": (2, EnumerationValue, True),
                                       "documentation": (3, "string", False)})
Service = _message("Service", {"name": (1, "string", False), "procedures": (2, Procedure, True),
                               "classes": (3, Class, True), "enumerations": (4, Enumeration, True),
                               "documentation": (6, "string", False)})
Services = _message("Services", {"services": (1, Service, True)})


def install() -> None:
    """Register this module as ``krpc.schema.KRPC_pb2`` unless the real krpc package is importable."""
    try:
        import krpc.schema.KRPC_pb2  # noqa: F401  (real package: nothing to do)
        return
    except ImportError:
        pass
    krpc = sys.modules.setdefault("krpc", types.ModuleType("krpc"))
    schema = sys.modules.setdefault("krpc.schema", types.ModuleType("krpc.schema"))
    krpc.schema = schema
    schema.KRPC_pb2 = sys.modules[__name__]
    sys.modules["krpc.schema.KRPC_pb2"] = sys.modules[__name__]
