// Execute the actual numeric data-page renderer with fake matrix and GPIO peripherals.
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <math.h>
#include <stdexcept>
using byte = uint8_t;
using boolean = bool;
#include "firmware_packets.hpp"
#define HIGH 1
#define SCREENPIN 3
#define INPUT_PULLUP 2
#define AGSAS 0
unsigned long millis() { return 4000; }
int digitalRead(int) { return HIGH; }
void pinMode(int, int) {}
byte ControlStatus(byte) { return 0; }
bool Connected = false, everConnected = false;
byte warning = 0, forcedScreen = 255, mxCols[12]{};
unsigned long forcedUntil = 0;
VesselData VData{};
#include "../../arduino-sketch/KSPIODemo9/matrix.ino"

using Pixels = std::array<uint8_t, 96>;
Pixels render(uint8_t field, float value) {
    VData.AP = VData.PE = VData.inc = VData.Alt = VData.Vsurf = value;
    memset(fb, 0, sizeof fb);
    drawDataValue(field);
    Pixels pixels{};
    memcpy(pixels.data(), fb, sizeof fb);
    return pixels;
}
void check(bool ok, const char* text) { if (!ok) throw std::runtime_error(text); }
int main() {
    try {
        Pixels unavailable{};
        for (int start : {0, 4, 8})
            for (int x = start; x < start + 3; ++x) unavailable[3 * 12 + x] = 1;
        for (uint8_t field = 0; field < FLD_COUNT; ++field) {
            for (float invalid : {std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity(),
                                  std::numeric_limits<float>::quiet_NaN()})
                check(render(field, invalid) == unavailable, "non-finite data must show only three dashes");
            const float units = field == FLD_INC ? 1.f : 1000000.f;
            check(render(field, std::numeric_limits<float>::max()) == render(field, 999 * units),
                  "huge finite positive values must saturate to 999 before conversion");
            check(render(field, -std::numeric_limits<float>::max()) == render(field, -99 * units),
                  "huge finite negative values must saturate to -99 before conversion");
            for (float normal : {0.f, 1.2f, -1.2f, 12.f, 123.f})
                check(render(field, normal) != unavailable, "finite data must retain its numeric display");
        }
        auto decimal = render(FLD_AP, 1.2f);
        check(decimal[5 * 12 + 5] == 1 && decimal[7 * 12 + 1] == 1,
              "ordinary decimals and metre unit dots must be preserved");
        std::puts("PASS firmware matrix: unavailable data, finite saturation, ordinary numeric pages");
        return 0;
    } catch (const std::exception& e) { std::fprintf(stderr, "FAIL firmware matrix: %s\n", e.what()); return 1; }
}
