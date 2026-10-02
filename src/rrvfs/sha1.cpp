#include "rrvfs/sha1.h"

#include <cstring>

namespace rr {

namespace {
uint32_t Rol(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }
} // namespace

Sha1::Sha1() : h_{0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u}, buffer_{} {}

void Sha1::Block(const uint8_t* p) {
    uint32_t w[80];
    for (int i = 0; i < 16; ++i)
        w[i] = (uint32_t(p[i * 4]) << 24) | (uint32_t(p[i * 4 + 1]) << 16) | (uint32_t(p[i * 4 + 2]) << 8) |
               uint32_t(p[i * 4 + 3]);
    for (int i = 16; i < 80; ++i) w[i] = Rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4];
    for (int i = 0; i < 80; ++i) {
        uint32_t f, k;
        if (i < 20) {
            f = (b & c) | (~b & d);
            k = 0x5A827999u;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1u;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDCu;
        } else {
            f = b ^ c ^ d;
            k = 0xCA62C1D6u;
        }
        const uint32_t t = Rol(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = Rol(b, 30);
        b = a;
        a = t;
    }
    h_[0] += a;
    h_[1] += b;
    h_[2] += c;
    h_[3] += d;
    h_[4] += e;
}

void Sha1::Update(const void* data, size_t size) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    totalBytes_ += size;
    while (size > 0) {
        const size_t take = (64 - buffered_) < size ? (64 - buffered_) : size;
        std::memcpy(buffer_ + buffered_, p, take);
        buffered_ += take;
        p += take;
        size -= take;
        if (buffered_ == 64) {
            Block(buffer_);
            buffered_ = 0;
        }
    }
}

std::string Sha1::HexDigest() {
    const uint64_t bits = totalBytes_ * 8;
    const uint8_t pad = 0x80;
    Update(&pad, 1);
    const uint8_t zero = 0;
    while (buffered_ != 56) Update(&zero, 1);
    uint8_t length[8];
    for (int i = 0; i < 8; ++i) length[i] = static_cast<uint8_t>(bits >> (56 - 8 * i));
    Update(length, 8);
    static const char kHex[] = "0123456789abcdef";
    std::string out;
    for (uint32_t v : h_)
        for (int shift = 28; shift >= 0; shift -= 4) out.push_back(kHex[(v >> shift) & 0xF]);
    return out;
}

std::string Sha1Hex(const void* data, size_t size) {
    Sha1 s;
    s.Update(data, size);
    return s.HexDigest();
}

} // namespace rr
