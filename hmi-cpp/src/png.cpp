#include "png.hpp"

#include <array>
#include <cstdint>
#include <fstream>
#include <vector>

#include "log.hpp"

namespace ksphmi {

namespace {

uint32_t crc32(const uint8_t* data, size_t n, uint32_t crc = 0) {
    static const std::array<uint32_t, 256> table = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    crc = ~crc;
    for (size_t i = 0; i < n; ++i) crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

void put32(std::vector<uint8_t>& v, uint32_t x) {
    for (int s = 24; s >= 0; s -= 8) v.push_back(static_cast<uint8_t>(x >> s));
}

void chunk(std::ofstream& out, const char* type, const std::vector<uint8_t>& data) {
    std::vector<uint8_t> head;
    put32(head, static_cast<uint32_t>(data.size()));
    out.write(reinterpret_cast<const char*>(head.data()), 4);
    std::vector<uint8_t> body(type, type + 4);
    body.insert(body.end(), data.begin(), data.end());
    out.write(reinterpret_cast<const char*>(body.data()), static_cast<std::streamsize>(body.size()));
    std::vector<uint8_t> crc;
    put32(crc, crc32(body.data(), body.size()));
    out.write(reinterpret_cast<const char*>(crc.data()), 4);
}

}  // namespace

bool save_png(const SDL_Surface* s, const std::string& path) {
    if (s->format->format != SDL_PIXELFORMAT_ARGB8888) {
        log::error("ksphmi", "screenshot: unexpected pixel format");
        return false;
    }
    std::vector<uint8_t> raw;
    raw.reserve(static_cast<size_t>(s->h) * (s->w * 3 + 1));
    for (int y = 0; y < s->h; ++y) {
        raw.push_back(0);  // filter: none
        const auto* row = reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(s->pixels) + y * s->pitch);
        for (int x = 0; x < s->w; ++x) {
            raw.push_back(static_cast<uint8_t>(row[x] >> 16));
            raw.push_back(static_cast<uint8_t>(row[x] >> 8));
            raw.push_back(static_cast<uint8_t>(row[x]));
        }
    }
    std::vector<uint8_t> z = {0x78, 0x01};
    uint32_t a = 1, b = 0;
    for (uint8_t byte : raw) a = (a + byte) % 65521, b = (b + a) % 65521;
    for (size_t pos = 0; pos < raw.size() || raw.empty();) {
        size_t n = std::min<size_t>(65535, raw.size() - pos);
        bool last = pos + n >= raw.size();
        z.push_back(last ? 1 : 0);
        z.push_back(static_cast<uint8_t>(n)), z.push_back(static_cast<uint8_t>(n >> 8));
        z.push_back(static_cast<uint8_t>(~n)), z.push_back(static_cast<uint8_t>(~n >> 8));
        z.insert(z.end(), raw.begin() + static_cast<long>(pos), raw.begin() + static_cast<long>(pos + n));
        pos += n;
        if (last) break;
    }
    put32(z, b << 16 | a);
    std::vector<uint8_t> ihdr;
    put32(ihdr, static_cast<uint32_t>(s->w));
    put32(ihdr, static_cast<uint32_t>(s->h));
    ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0});  // 8-bit RGB
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        log::error("ksphmi", "cannot write " + path);
        return false;
    }
    static const uint8_t signature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    out.write(reinterpret_cast<const char*>(signature), 8);
    chunk(out, "IHDR", ihdr);
    chunk(out, "IDAT", z);
    chunk(out, "IEND", {});
    return static_cast<bool>(out);
}

}  // namespace ksphmi
