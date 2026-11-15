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
    int emptyReads = 0;
    int read() { if (rx.empty()) { ++emptyReads; return -1; } byte b = rx.front(); rx.pop_front(); return b; }
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
int resetRequests = 0;
bool allowReset = false;
void NVIC_SystemReset() {
    ++resetRequests;
    if (!allowReset) throw std::runtime_error("unexpected board reset");
}
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

void fragmented_handshake(size_t split) {
    KSPBoardResetReceiver();
    Serial.rx.clear();
    Serial.tx.clear();
    auto bytes = handshake();
    Serial.feed({0, 0, 0});
    Serial.feed(std::vector<byte>(bytes.begin(), bytes.begin() + split));
    check(input() == -1, "fragmented handshake must wait for remaining bytes");
    Serial.feed(std::vector<byte>(bytes.begin() + split, bytes.end()));
    check(input() == 0, "fragmented header after garbage must survive until the remainder arrives");
    HandShakePacket reply{0, 3, 1, 4};
    check(Serial.tx == frame(reinterpret_cast<byte*>(&reply), sizeof reply), "fragmented handshake must reply exactly once");
}

template <typename Packet>
void receive_byte_by_byte(const Packet& packet) {
    auto bytes = frame(reinterpret_cast<const byte*>(&packet), sizeof packet);
    for (size_t i = 0; i < bytes.size(); ++i) {
        Serial.feed({bytes[i]});
        ++clockMs;
        check(input() == (i + 1 == bytes.size() ? packet.id : -1), "byte-wise delivery must accept only the complete frame");
    }
}

void test_incremental_receiver() {
    fragmented_handshake(2);  // reported sequence: 00 00 00 BE EF, then size/id/payload/checksum
    for (size_t split = 1; split < handshake().size(); ++split) fragmented_handshake(split);
    receive_byte_by_byte(HandShakePacket{0, 1, 2, 3});
    receive_byte_by_byte(VesselData{1});
    receive_byte_by_byte(DisplayCommand{2, 1});
    Serial.feed({0xBE});  // preserve the overlapping BE BE EF header
    expect_handshake();
    auto broken = handshake();
    broken.back() ^= 1;
    Serial.feed(broken);
    Serial.feed(handshake());
    check(input() == -1, "bad checksum must be rejected");
    check(input() == 0, "next valid frame must survive a rejected checksum");
    Serial.feed({0xBE, 0xEF, 0});
    Serial.feed(handshake());
    check(input() == -1, "zero payload size must be rejected");
    check(input() == 0, "next valid frame must survive a rejected length");
    Serial.feed({0xBE, 0xEF, 255, 0});
    Serial.feed(handshake());
    check(input() == -1, "oversized handshake length must be rejected before payload accumulation");
    check(input() == 0, "next valid frame must survive a mismatched length");
    auto probe = handshake();
    for (size_t split : {size_t(1), size_t(2), size_t(3)}) {
        Serial.feed(std::vector<byte>(probe.begin(), probe.begin() + split));
        check(input() == -1, "partial header must wait");
        clockMs += IDLETIMER + 1;
        expect_handshake();  // timeout must reset every header state as well as partial payload
    }
    check(Serial.emptyReads == 0, "parser must never read an empty serial buffer");
}

void reset_boot_state() {
    Serial.rx.clear();
    Serial.tx.clear();
    clockMs = now = deadtime = deadtimeOld = dbgLastRxMs = 0;
    Connected = everConnected = allowReset = false;
    resetRequests = 0;
    KSPBoardResetReceiver();
}

void test_early_connection_recovery() {
    for (byte packetId : {byte(0), byte(1), byte(2)}) {
        reset_boot_state();
        clockMs = 100;
        if (packetId == 0) Serial.feed(handshake());
        else if (packetId == 1) {
            VesselData packet{};
            packet.id = 1;
            Serial.feed(frame(reinterpret_cast<byte*>(&packet), sizeof packet));
        } else {
            DisplayCommand packet{2, 1};
            Serial.feed(frame(reinterpret_cast<byte*>(&packet), sizeof packet));
        }
        check(input() == packetId && everConnected, "first valid receive must register connection before any matrix draw");
        clockMs = 100 + IDLETIMER + 1;  // disconnect while the 2500 ms splash would still be showing
        check(input() == -1 && !Connected && everConnected, "idle must preserve connection history during splash");
        clockMs = 100 + LINKRESETMS;
        input();
        check(resetRequests == 0, "link recovery must not reset before its deadline");
        ++clockMs;
        allowReset = true;
        input();
        check(resetRequests == 1, "early connection followed by silence must request link reset");
    }
    reset_boot_state();
    clockMs = LINKRESETMS + 1000;
    input();
    check(!everConnected && resetRequests == 0, "a board that never connected must keep waiting without link reset");
    reset_boot_state();
    clockMs = 100;
    auto invalid = handshake();
    invalid.back() ^= 1;
    Serial.feed(invalid);
    check(input() == -1 && !everConnected, "an invalid packet must not register first connection");
    clockMs = LINKRESETMS + 1000;
    input();
    check(resetRequests == 0, "invalid traffic must not arm link recovery");
    reset_boot_state();
}

int main() {
    try {
        test_early_connection_recovery();
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
        test_incremental_receiver();
        std::puts("PASS firmware receiver: early connection recovery, timeouts, frame splits, byte-wise delivery, invalid frames and IDs");
        return 0;
    } catch (const std::exception& e) { std::fprintf(stderr, "FAIL firmware receiver: %s\n", e.what()); return 1; }
}
