import struct

class HandShakePacket:
    FORMAT = '<BBBB'
    SIZE = struct.calcsize(FORMAT)

    def __init__(self, id=0, M1=0, M2=0, M3=0):
        self.id = id
        self.M1 = M1
        self.M2 = M2
        self.M3 = M3

    def pack(self):
        return struct.pack(self.FORMAT, self.id, self.M1, self.M2, self.M3)

    @classmethod
    def unpack(cls, data):
        return cls(*struct.unpack(cls.FORMAT, data))