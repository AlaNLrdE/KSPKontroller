// Host regression tests execute the firmware parser, dispatcher and handshake with fake peripherals.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <stdexcept>
#include <vector>
using byte = uint8_t;
using boolean = bool;
#include "firmware_packets.hpp"

#define IDLETIMER 2000
#define LINKRESETMS 30000
#define GLED 12
#define HIGH 1
#define details(name) (uint8_t*)&name, sizeof(name)

struct FakeSerial {
    std::deque<byte> rx;
    std::vector<byte> tx;
    int available() { return static_cast<int>(rx.size()); }
    int read() { if (rx.empty()) return -1; byte b = rx.front(); rx.pop_front(); return b; }
    void write(const byte* data, size_t size) { tx.insert(tx.end(), data, data + size); }
    void feed(const std::vector<byte>& data) { rx.insert(rx.end(), data.begin(), data.end()); }
} Serial;
unsigned long clockMs = 0;
unsigned long millis() { return clockMs; }
unsigned long micros() { return clockMs * 1000; }
void delayMicroseconds(unsigned int) {}
void digitalWrite(int, int) {}
int lightsOff = 0;
void LEDSAllOff() { ++lightsOff; }
void Indicators() {}
void NVIC_SystemReset() { throw std::runtime_error("unexpected board reset"); }
unsigned long now = 0, deadtime = 0, deadtimeOld = 0, dbgLastRxMs = 0, forcedUntil = 0;
unsigned long dbgTxMaxUs = 0;
uint32_t dbgTxPackets = 0;
uint16_t dbgRxOk = 0, dbgRxBadChecksum = 0, dbgRxBadSize = 0;
bool Connected = false, everConnected = false;
byte id = 0, forcedScreen = 255;
HandShakePacket HPacket{};
VesselData VData{};
DisplayCommand DCmd{};
#include "../../arduino-sketch/KSPIODemo9/SerialCOMS.ino"
#include "../../arduino-sketch/KSPIODemo9/HandShake.ino"
#include "../../arduino-sketch/KSPIODemo9/Input.ino"

void check(bool ok, const char* text) { if (!ok) throw std::runtime_error(text); }
std::vector<byte> frame(const byte* payload, size_t size) {
    std::vector<byte> out{0xBE, 0xEF, static_cast<byte>(size)};
    byte checksum = static_cast<byte>(size);
    for (size_t i = 0; i < size; ++i) { out.push_back(payload[i]); checksum ^= payload[i]; }
    out.push_back(checksum);
    return out;
}
std::vector<byte> handshake() {
    HandShakePacket probe{0, 1, 2, 3};
    return frame(reinterpret_cast<byte*>(&probe), sizeof probe);
}
void partial_telemetry() {
    VesselData packet{};
    packet.id = 1;
    auto bytes = frame(reinterpret_cast<byte*>(&packet), sizeof packet);
    Serial.feed(std::vector<byte>(bytes.begin(), bytes.begin() + 12));
    check(input() == -1 && rx_len == sizeof(VesselData), "telemetry must remain partial");
}
void expect_handshake() {
    Serial.tx.clear();
    Serial.feed(handshake());
    check(input() == 0 && Connected, "handshake must reconnect immediately");
    HandShakePacket reply{0, 3, 1, 4};
    check(Serial.tx == frame(reinterpret_cast<byte*>(&reply), sizeof reply), "handshake response must be 3/1/4");
}
template <typename Packet>
void reject_unknown_after(const Packet& packet, Packet& destination) {
    Serial.feed(frame(reinterpret_cast<const byte*>(&packet), sizeof packet));
    check(input() == packet.id, "known packet must be accepted before unknown ID test");
    std::vector<byte> before(reinterpret_cast<byte*>(&destination), reinterpret_cast<byte*>(&destination) + sizeof packet);
    const uint16_t good = dbgRxOk, bad = dbgRxBadSize;
    const unsigned long received = dbgLastRxMs, deadline = deadtimeOld;
    std::vector<byte> unknown(sizeof packet, 0x55);
    unknown[0] = 99;
    clockMs += 10;
    Serial.feed(frame(unknown.data(), unknown.size()));
    check(input() == -1, "unknown ID must be rejected even with a matching length and valid checksum");
    check(memcmp(&destination, before.data(), sizeof packet) == 0, "unknown ID must not overwrite the previous destination");
    check(dbgRxOk == good && dbgRxBadSize == static_cast<uint16_t>(bad + 1), "unknown ID must count as invalid, not valid");
    check(dbgLastRxMs == received && deadtimeOld == deadline, "unknown ID must not renew link liveness");
    check(rx_len == 0 && address == nullptr, "unknown ID must clear the receive destination");
    expect_handshake();  // skip the rejected payload and accept the next valid frame
}

int main() {
    try {
        expect_handshake();
        clockMs = 100;
        partial_telemetry();
        clockMs += IDLETIMER + 1;
        check(input() == -1 && !Connected, "silence must disconnect");
        check(rx_len == 0, "idle timeout must discard partial telemetry");
        expect_handshake();
        clockMs += 100;
        partial_telemetry();
        clockMs += IDLETIMER + 1;
        expect_handshake();  // queued before the first loop iteration that observes expiry
        clockMs += 100;
        VesselData packet{};
        packet.id = 1;
        auto bytes = frame(reinterpret_cast<byte*>(&packet), sizeof packet);
        Serial.feed(std::vector<byte>(bytes.begin(), bytes.begin() + 12));
        check(input() == -1, "ordinary telemetry begins fragmented");
        clockMs += 100;
        Serial.feed(std::vector<byte>(bytes.begin() + 12, bytes.end()));
        check(input() == 1 && Connected, "partial telemetry below the timeout must survive");
        reject_unknown_after(VesselData{1}, VData);
        reject_unknown_after(HandShakePacket{0, 1, 2, 3}, HPacket);
        reject_unknown_after(DisplayCommand{2, 1}, DCmd);
        std::puts("PASS firmware receiver: timeouts, fragmentation, unknown IDs preserve all destinations and liveness");
        return 0;
    } catch (const std::exception& e) { std::fprintf(stderr, "FAIL firmware receiver: %s\n", e.what()); return 1; }
}
