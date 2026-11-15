// Minimal protobuf wire format, enough for the kRPC messages (krpc.schema.KRPC: ConnectionRequest/Response,
// Request, ProcedureCall, Argument, Response, ProcedureResult, Error, StreamUpdate, StreamResult, Stream, List,
// Tuple) and kRPC value encoding. Hand-written instead of libprotobuf + protoc: about 200 lines, no generated code,
// no library version coupling on the Pi.
#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace ksphmi::pb {

struct DecodeError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

enum Wire : int { Varint = 0, Fixed64 = 1, Bytes = 2, Fixed32 = 5 };

class Writer {
public:
    void varint(uint64_t v);
    void tag(int field, Wire wire) { varint(static_cast<uint64_t>(field) << 3 | wire); }
    void field_varint(int field, uint64_t v) { tag(field, Varint), varint(v); }
    void field_bytes(int field, std::string_view data) {
        tag(field, Bytes), varint(data.size()), out.append(data.data(), data.size());
    }
    std::string out;
};

/// Iterates the fields of one message.
class Reader {
public:
    explicit Reader(std::string_view data) : d_(data) {}
    /// Next field; false at the end. Throws DecodeError on truncated input.
    bool next();
    int field() const { return field_; }
    Wire wire() const { return wire_; }
    uint64_t varint_value() const { return varint_; }
    std::string_view bytes_value() const { return bytes_; }

private:
    uint64_t read_varint();
    std::string_view d_;
    size_t pos_ = 0;
    int field_ = 0;
    Wire wire_ = Varint;
    uint64_t varint_ = 0;
    std::string_view bytes_;
};

/// Varint at the start of data; `used` receives its length. Throws DecodeError.
uint64_t read_varint(std::string_view data, size_t& used);
std::string varint(uint64_t v);

// ---- kRPC value encoding (one value, no tag): see krpc/encoder.py
std::string enc_double(double v);
std::string enc_float(float v);
std::string enc_bool(bool v);
std::string enc_string(std::string_view s);
std::string enc_object(uint64_t id);
std::string enc_sint32(int32_t v);
std::string enc_tuple(const std::vector<double>& values);  // Tuple of doubles

double dec_double(std::string_view v);
float dec_float(std::string_view v);
bool dec_bool(std::string_view v);
std::string dec_string(std::string_view v);
uint64_t dec_object(std::string_view v);  // 0 = null
int32_t dec_sint32(std::string_view v);
/// Items of a List or Tuple message, each still encoded.
std::vector<std::string> dec_items(std::string_view v);
std::vector<double> dec_tuple(std::string_view v);  // Tuple of doubles

}  // namespace ksphmi::pb
