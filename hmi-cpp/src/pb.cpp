#include "pb.hpp"

#include <cstring>

namespace ksphmi::pb {

void Writer::varint(uint64_t v) {
    while (v >= 0x80) {
        out += static_cast<char>((v & 0x7F) | 0x80);
        v >>= 7;
    }
    out += static_cast<char>(v);
}

std::string varint(uint64_t v) {
    Writer w;
    w.varint(v);
    return w.out;
}

uint64_t read_varint(std::string_view data, size_t& used) {
    uint64_t v = 0;
    for (size_t i = 0; i < data.size() && i < 10; ++i) {
        auto b = static_cast<uint8_t>(data[i]);
        v |= static_cast<uint64_t>(b & 0x7F) << (7 * i);
        if (!(b & 0x80)) {
            used = i + 1;
            return v;
        }
    }
    throw DecodeError("truncated varint");
}

uint64_t Reader::read_varint() {
    size_t used = 0;
    uint64_t v = pb::read_varint(d_.substr(pos_), used);
    pos_ += used;
    return v;
}

bool Reader::next() {
    if (pos_ >= d_.size()) return false;
    uint64_t key = read_varint();
    field_ = static_cast<int>(key >> 3);
    wire_ = static_cast<Wire>(key & 7);
    switch (wire_) {
        case Varint: varint_ = read_varint(); break;
        case Fixed64:
        case Fixed32: {
            size_t n = wire_ == Fixed64 ? 8 : 4;
            if (pos_ + n > d_.size()) throw DecodeError("truncated fixed field");
            bytes_ = d_.substr(pos_, n);
            pos_ += n;
            break;
        }
        case Bytes: {
            uint64_t n = read_varint();
            if (n > d_.size() - pos_) throw DecodeError("truncated length-delimited field");
            bytes_ = d_.substr(pos_, n);
            pos_ += n;
            break;
        }
        default: throw DecodeError("unsupported wire type");
    }
    return true;
}

std::string enc_double(double v) {
    std::string s(8, '\0');
    std::memcpy(&s[0], &v, 8);  // little endian on every target (x86-64, ARM)
    return s;
}

std::string enc_float(float v) {
    std::string s(4, '\0');
    std::memcpy(&s[0], &v, 4);
    return s;
}

std::string enc_bool(bool v) { return varint(v ? 1 : 0); }
std::string enc_string(std::string_view s) { return varint(s.size()) + std::string(s); }
std::string enc_object(uint64_t id) { return varint(id); }
std::string enc_sint32(int32_t v) {
    return varint(static_cast<uint32_t>((static_cast<uint32_t>(v) << 1) ^ static_cast<uint32_t>(v >> 31)));
}

std::string enc_tuple(const std::vector<double>& values) {
    Writer w;
    for (double v : values) w.field_bytes(1, enc_double(v));
    return w.out;
}

double dec_double(std::string_view v) {
    if (v.size() != 8) throw DecodeError("double value of " + std::to_string(v.size()) + " bytes");
    double d;
    std::memcpy(&d, v.data(), 8);
    return d;
}

float dec_float(std::string_view v) {
    if (v.size() != 4) throw DecodeError("float value of " + std::to_string(v.size()) + " bytes");
    float f;
    std::memcpy(&f, v.data(), 4);
    return f;
}

bool dec_bool(std::string_view v) {
    size_t used;
    return read_varint(v, used) != 0;
}

std::string dec_string(std::string_view v) {
    size_t used;
    uint64_t n = read_varint(v, used);
    if (n > v.size() - used) throw DecodeError("truncated string");
    return std::string(v.substr(used, n));
}

uint64_t dec_object(std::string_view v) {
    if (v.empty()) return 0;
    size_t used;
    return read_varint(v, used);
}

int32_t dec_sint32(std::string_view v) {
    size_t used;
    auto n = static_cast<uint32_t>(read_varint(v, used));
    return static_cast<int32_t>((n >> 1) ^ (~(n & 1) + 1));
}

std::vector<std::string> dec_items(std::string_view v) {
    std::vector<std::string> items;
    Reader r(v);
    while (r.next())
        if (r.field() == 1 && r.wire() == Bytes) items.emplace_back(r.bytes_value());
    return items;
}

std::vector<double> dec_tuple(std::string_view v) {
    std::vector<double> out;
    for (const auto& item : dec_items(v)) out.push_back(dec_double(item));
    return out;
}

}  // namespace ksphmi::pb
