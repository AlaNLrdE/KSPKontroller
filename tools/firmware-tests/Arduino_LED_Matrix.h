// Host-only stand-in: the production matrix code still writes its real framebuffer.
#pragma once
class ArduinoLEDMatrix {
public:
    void begin() {}
    template <size_t H, size_t W>
    void renderBitmap(uint8_t (&)[H][W], int, int) {}
};
