"""Read-only vessel resource totals and activation-stage telemetry."""
from dataclasses import dataclass
import math

RESOURCE_NAMES = ('ElectricCharge', 'LiquidFuel', 'Oxidizer', 'MonoPropellant', 'SolidFuel', 'XenonGas')
RESOURCE_LABELS = ('ELECTRICITY', 'LIQUID FUEL', 'OXIDIZER', 'MONOPROPELLANT', 'SOLID FUEL', 'XENON GAS')


@dataclass(frozen=True)
class Resource:
    name: str
    amount: float | None
    capacity: float | None

    @property
    def fraction(self):
        if self.amount is None or self.capacity is None or self.capacity <= 0:
            return None
        if not math.isfinite(self.amount) or not math.isfinite(self.capacity):
            return None
        return max(0, min(1, self.amount / self.capacity))


@dataclass(frozen=True)
class Stage:
    number: int
    parts: int | None
    delta_v: float | None


@dataclass(frozen=True)
class Systems:
    current_stage: int | None = None
    resources: tuple[Resource, ...] = ()
    stages: tuple[Stage, ...] = ()


class SystemsStreams:
    """Resources at 2 Hz, independent of attitude streams; staging rebuilds on queue changes."""
    def __init__(self, conn, vessel):
        self.conn, self.vessel = conn, vessel
        self.common, self.stage_streams, self.resource_streams, self.key = {}, {}, {}, None
        self.stage_numbers = []
        try:
            self.add(self.common, 'current', getattr, vessel.control, 'current_stage')
            self.add(self.common, 'stages', getattr, vessel, 'stages')
            self.add(self.common, 'parts', getattr, vessel.parts, 'all')
        except Exception:
            self.close()
            raise

    def add(self, group, name, func, *args):
        stream = self.conn.add_stream(func, *args)
        group[name] = stream
        stream.rate = 2

    @staticmethod
    def clear(group):
        for stream in group.values():
            try:
                stream.remove()
            except Exception:
                pass
        group.clear()

    def close(self):
        self.clear(self.common)
        self.clear(self.stage_streams)
        self.clear(self.resource_streams)

    @staticmethod
    def read(stream):
        if stream is None:
            return None
        try:
            return stream()
        except Exception:  # kRPC raises RuntimeError too, e.g. delta-v not available for a stage
            return None

    def sample(self):
        current = self.read(self.common['current'])
        objects = self.read(self.common['stages'])
        parts = self.read(self.common['parts'])
        key = (current, tuple(objects or ()), tuple(parts or ()))
        if key != self.key:
            self.clear(self.stage_streams)
            self.clear(self.resource_streams)
            self.stage_numbers = []
            self.key = key
            try:
                # Resource collections can retain a parts set. Re-resolve them
                # after staging, docking or other vessel topology changes.
                resources = self.vessel.resources
                for name in RESOURCE_NAMES:
                    self.add(self.resource_streams, (name, 'amount'), resources.amount, name)
                    self.add(self.resource_streams, (name, 'capacity'), resources.max, name)
                selected = sorted(((stage.number, stage) for stage in objects or ()), key=lambda row: -row[0])
                selected = [(number, stage) for number, stage in selected if current is None or number <= current][:4]
                for number, stage in selected:
                    self.add(self.stage_streams, (number, 'parts'), getattr, stage, 'parts')
                    try:
                        self.add(self.stage_streams, (number, 'delta_v'), getattr, stage, 'delta_v')
                    except Exception:  # "delta-v information is not available" for this stage
                        pass
                    self.stage_numbers.append(number)
            except Exception:  # docking or staging invalidated an object, or no delta-v for a stage
                self.key = None
        resources = tuple(Resource(name, self.read(self.resource_streams.get((name, 'amount'))),
                                   self.read(self.resource_streams.get((name, 'capacity')))) for name in RESOURCE_NAMES)
        stages = []
        for number in self.stage_numbers:
            parts = self.read(self.stage_streams[(number, 'parts')])
            # Unknown delta-v is normal (KSP has not computed it, e.g. on the pad): show it
            # as unknown without rebuilding every stream each sample. Missing parts means
            # the stage object went stale (staging, docking): rebuild.
            dv = self.read(self.stage_streams.get((number, 'delta_v')))
            if parts is None:
                self.key = None
            stages.append(Stage(number, len(parts) if parts is not None else None, dv))
        return Systems(current, resources, tuple(stages))
