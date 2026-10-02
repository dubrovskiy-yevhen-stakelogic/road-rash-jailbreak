#include "platform/png.h"

#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace rr {
namespace {

uint32_t Crc32(const uint8_t* data, size_t size, uint32_t crc = 0xFFFFFFFFu) {
    static uint32_t table[256];
    static bool ready = false;
    if (!ready) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        ready = true;
    }
    for (size_t i = 0; i < size; ++i) crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return crc;
}

void PushBigEndian32(std::vector<uint8_t>& out, uint32_t value) {
    out.push_back(static_cast<uint8_t>(value >> 24));
    out.push_back(static_cast<uint8_t>(value >> 16));
    out.push_back(static_cast<uint8_t>(value >> 8));
    out.push_back(static_cast<uint8_t>(value));
}

void PushChunk(std::vector<uint8_t>& out, const char* type, const std::vector<uint8_t>& payload) {
    PushBigEndian32(out, static_cast<uint32_t>(payload.size()));
    const size_t crcStart = out.size();
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), payload.begin(), payload.end());
    const uint32_t crc = Crc32(out.data() + crcStart, out.size() - crcStart) ^ 0xFFFFFFFFu;
    PushBigEndian32(out, crc);
}

// Deflate with stored blocks only - correct zlib output, no compression, no dependency.
std::vector<uint8_t> StoredDeflate(const std::vector<uint8_t>& raw) {
    std::vector<uint8_t> out;
    out.push_back(0x78); // CM = deflate, CINFO = 7
    out.push_back(0x01); // FCHECK so that (0x78 << 8 | 0x01) % 31 == 0, no preset dictionary
    size_t at = 0;
    while (at < raw.size() || raw.empty()) {
        const size_t take = raw.size() - at > 65535 ? 65535 : raw.size() - at;
        const bool last = at + take >= raw.size();
        out.push_back(last ? 1 : 0);
        out.push_back(static_cast<uint8_t>(take & 0xFF));
        out.push_back(static_cast<uint8_t>(take >> 8));
        out.push_back(static_cast<uint8_t>(~take & 0xFF));
        out.push_back(static_cast<uint8_t>((~take >> 8) & 0xFF));
        out.insert(out.end(), raw.begin() + static_cast<ptrdiff_t>(at),
                   raw.begin() + static_cast<ptrdiff_t>(at + take));
        at += take;
        if (last) break;
    }
    uint32_t a = 1, b = 0;
    for (uint8_t byte : raw) {
        a = (a + byte) % 65521;
        b = (b + a) % 65521;
    }
    PushBigEndian32(out, (b << 16) | a);
    return out;
}

} // namespace

void WritePng(const std::string& path, int width, int height, const std::vector<uint8_t>& rgba) {
    if (width <= 0 || height <= 0) throw std::runtime_error("PNG: empty image");
    if (rgba.size() != static_cast<size_t>(width) * static_cast<size_t>(height) * 4)
        throw std::runtime_error("PNG: pixel buffer size does not match the dimensions");

    std::vector<uint8_t> raw;
    raw.reserve(rgba.size() + static_cast<size_t>(height));
    for (int y = 0; y < height; ++y) {
        raw.push_back(0); // filter type 0 (none)
        const size_t row = static_cast<size_t>(y) * static_cast<size_t>(width) * 4;
        raw.insert(raw.end(), rgba.begin() + static_cast<ptrdiff_t>(row),
                   rgba.begin() + static_cast<ptrdiff_t>(row + static_cast<size_t>(width) * 4));
    }

    std::vector<uint8_t> file = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    std::vector<uint8_t> ihdr;
    PushBigEndian32(ihdr, static_cast<uint32_t>(width));
    PushBigEndian32(ihdr, static_cast<uint32_t>(height));
    ihdr.push_back(8); // bit depth
    ihdr.push_back(6); // colour type RGBA
    ihdr.push_back(0); // deflate
    ihdr.push_back(0); // adaptive filtering
    ihdr.push_back(0); // no interlace
    PushChunk(file, "IHDR", ihdr);
    PushChunk(file, "IDAT", StoredDeflate(raw));
    PushChunk(file, "IEND", {});

    std::FILE* out = std::fopen(path.c_str(), "wb");
    if (!out) throw std::runtime_error("PNG: cannot write " + path);
    const size_t written = std::fwrite(file.data(), 1, file.size(), out);
    std::fclose(out);
    if (written != file.size()) throw std::runtime_error("PNG: short write to " + path);
}

} // namespace rr
