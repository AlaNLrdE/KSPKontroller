import struct
import serial
from .config import SERIAL_PORT, BAUDRATE, VESSEL_DATA_SIZE

class VesselData:
    FORMAT = (
        '<Bfff'      # id, AP, PE, SemiMajorAxis
        'f' * 3      # SemiMinorAxis, VVI, e
        'f' * 2      # inc, G
        'ii'         # TAp, TPe
        'f' * 2      # TrueAnomaly, Density
        'i'          # period
        'f' * 2      # RAlt, Alt
        'f' * 2      # Vsurf, Lat
        'f' * 2      # Lon, LiquidFuelTot
        'f' * 2      # LiquidFuel, OxidizerTot
        'f' * 2      # Oxidizer, EChargeTot
        'f' * 2      # ECharge, MonoPropTot
        'f' * 2      # MonoProp, IntakeAirTot
        'f' * 2      # IntakeAir, SolidFuelTot
        'f' * 2      # SolidFuel, XenonGasTot
        'f' * 2      # XenonGas, LiquidFuelTotS
        'f' * 2      # LiquidFuelS, OxidizerTotS
        'f' * 2      # OxidizerS, MissionTime
        'f' * 2      # deltaTime, VOrbit
        'I'          # MNTime
        'f' * 2      # MNDeltaV, Pitch
        'f' * 2      # Roll, Heading
        'H'          # ActionGroups
        'B' * 2      # SOINumber, MaxOverHeat
        'f' * 2      # MachNumber, IAS
        'B' * 2      # CurrentStage, TotalStage
        'f' * 2      # TargetDist, TargetV
        'B'          # NavballSASMode
    )
    SIZE = VESSEL_DATA_SIZE

    @classmethod
    def unpack(cls, data):
        # For brevity, just return the raw tuple
        return struct.unpack(cls.FORMAT, data)

class SerialComs:
    def __init__(self):
        self.ser = serial.Serial(SERIAL_PORT, BAUDRATE, timeout=0.1)

    def read_vessel_data(self):
        data = self.ser.read(VesselData.SIZE)
        if len(data) == VesselData.SIZE:
            return VesselData.unpack(data)
        return None

    def write_packet(self, packet_bytes):
        self.ser.write(packet_bytes)