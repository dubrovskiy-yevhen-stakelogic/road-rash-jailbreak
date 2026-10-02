// The PS1 MDEC's arithmetic, one implementation (mdec.h).
#include "rrformats/mdec.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace rr::mdec {

const uint8_t kZigZag[64] = {0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,  12, 19, 26, 33, 40, 48,
                             41, 34, 27, 20, 13, 6,  7,  14, 21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23,
                             30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};

namespace {

int32_t Signed10(uint32_t v) { return static_cast<int32_t>(static_cast<uint32_t>(v & 0x3FFu) << 22) >> 22; }
// The unit's 9-bit datapath: wrap to 9 bits, then saturate to a signed byte.
int32_t Wrap9Sat8(int32_t v) { return std::clamp(static_cast<int32_t>(static_cast<uint32_t>(v) << 23) >> 23, -128, 127); }

// ---------------------------------------------------------------------------- the console
int32_t HwCoef(int32_t v, int32_t scale, bool dc) {
    int32_t c;
    if (scale == 0) c = static_cast<int32_t>(static_cast<uint32_t>(v) << 5);
    else {
        const int32_t p = dc ? v * scale : (v * scale) >> 3;
        c = static_cast<int32_t>(static_cast<uint32_t>(p) << 4) + (v == 0 ? 0 : (v < 0 ? 8 : -8));
    }
    return std::clamp(c, -0x4000, 0x3FFF);
}

void HwIdct(int32_t blk[64], const std::array<int16_t, 64>& up) {
    int32_t m[64]; // m[x * 8 + u]: sample x of frequency u, the uploaded entry >> 3
    for (int u = 0; u < 8; ++u)
        for (int x = 0; x < 8; ++x) m[x * 8 + u] = static_cast<int32_t>(up[static_cast<size_t>(u * 8 + x)]) >> 3;
    int16_t tmp[64];
    // pass 1 along each row's horizontal frequencies, stored transposed; pass 2 the same on the result
    for (int r = 0; r < 8; ++r)
        for (int x = 0; x < 8; ++x) {
            int32_t s = 0;
            for (int u = 0; u < 8; ++u) s += blk[r * 8 + u] * m[x * 8 + u];
            tmp[x * 8 + r] = static_cast<int16_t>((s + 0x4000) >> 15);
        }
    for (int c = 0; c < 8; ++c)
        for (int y = 0; y < 8; ++y) {
            int32_t s = 0;
            for (int u = 0; u < 8; ++u) s += static_cast<int32_t>(tmp[c * 8 + u]) * m[y * 8 + u];
            blk[y * 8 + c] = Wrap9Sat8((s + 0x4000) >> 15);
        }
}

uint32_t HwColour(int32_t y, int32_t cb, int32_t cr, bool signedOut) {
    const int32_t add = signedOut ? 0 : 128;
    const int32_t r = Wrap9Sat8(y + ((359 * cr + 0x80) >> 8)) + add;
    const int32_t g = Wrap9Sat8(y + ((((-88 * cb) & ~0x1F) + ((-183 * cr) & ~0x07) + 0x80) >> 8)) + add;
    const int32_t b = Wrap9Sat8(y + ((454 * cb + 0x80) >> 8)) + add;
    return (static_cast<uint32_t>(r) & 0xFFu) | ((static_cast<uint32_t>(g) & 0xFFu) << 8) |
           ((static_cast<uint32_t>(b) & 0xFFu) << 16);
}

// ----------------------------------------------------------------- the legacy model (RRJB_MDEC=legacy)
void LegacyIdct(int32_t blk[64], const std::array<int16_t, 64>& m) { // 3 fractional bits out
    int32_t tmp[64];
    for (int col = 0; col < 8; ++col)
        for (int x = 0; x < 8; ++x) {
            int64_t s = 0;
            for (int u = 0; u < 8; ++u) s += static_cast<int64_t>(blk[u * 8 + col]) * m[static_cast<size_t>(u * 8 + x)];
            tmp[col * 8 + x] = static_cast<int32_t>((s + (1 << 12)) >> 13);
        }
    for (int row = 0; row < 8; ++row)
        for (int x = 0; x < 8; ++x) {
            int64_t s = 0;
            for (int u = 0; u < 8; ++u) s += static_cast<int64_t>(tmp[u * 8 + row]) * m[static_cast<size_t>(u * 8 + x)];
            blk[row * 8 + x] = static_cast<int32_t>((s + (1 << 15)) >> 16);
        }
}
uint32_t LegacyColour(int32_t y8, int32_t cb8, int32_t cr8) {
    const int64_t y = static_cast<int64_t>(y8) << 16;
    const auto c = [](int64_t v) { return static_cast<uint32_t>(std::clamp(static_cast<int>(v >> 19) + 128, 0, 255)); };
    return c(y + 91881LL * cr8) | (c(y - 22525LL * cb8 - 46812LL * cr8) << 8) | (c(y + 116130LL * cb8) << 16);
}

// ---------------------------------------------------------------------------- the capture emulator's default
void EmuOldIdct(int32_t blk[64], const std::array<int16_t, 64>& s) {
    int64_t t[64];
    for (int x = 0; x < 8; ++x)
        for (int y = 0; y < 8; ++y) {
            int64_t sum = 0;
            for (int u = 0; u < 8; ++u) sum += static_cast<int64_t>(blk[u * 8 + x]) * s[static_cast<size_t>(u * 8 + y)];
            t[x + y * 8] = sum;
        }
    for (int x = 0; x < 8; ++x)
        for (int y = 0; y < 8; ++y) {
            int64_t sum = 0;
            for (int u = 0; u < 8; ++u) sum += t[u + y * 8] * s[static_cast<size_t>(u * 8 + x)];
            blk[x + y * 8] = Wrap9Sat8(static_cast<int32_t>((sum >> 32) + ((sum >> 31) & 1)));
        }
}
uint32_t EmuOldColour(int32_t y, int32_t cb, int32_t cr, bool signedOut) {
    const int32_t add = signedOut ? 0 : 128;
    const int32_t rc = (22970 * cr) / 16384, bc = (29032 * cb) / 16384, gc = (-5631 * cb + -11703 * cr) / 16384;
    const auto c = [&](int32_t v) { return static_cast<uint32_t>(std::clamp(y + v, -128, 127) + add) & 0xFFu; };
    return c(rc) | (c(gc) << 8) | (c(bc) << 16);
}

} // namespace

Model ActiveModel() {
    static const Model m = [] {
        const char* v = std::getenv("RRJB_MDEC");
        if (v != nullptr && std::strcmp(v, "legacy") == 0) return Model::Legacy;
        if (v != nullptr && std::strcmp(v, "emuold") == 0) return Model::EmuOld;
        return Model::Hardware;
    }();
    return m;
}

const char* ModelName(Model m) {
    switch (m) {
    case Model::Hardware: return "hardware";
    case Model::Legacy: return "legacy (RRJB_MDEC=legacy)";
    case Model::EmuOld: return "the capture emulator's old routines (RRJB_MDEC=emuold)";
    }
    return "?";
}

bool ReadBlock(Input& in, const std::array<uint8_t, 64>& qt, int32_t coef[64], Model m) {
    std::fill(coef, coef + 64, 0);
    uint16_t n;
    do {
        if (in.Empty()) return false;
        n = in.Next();
    } while (n == 0xFE00u);
    const int32_t q = n >> 10;
    int32_t k = 0;
    for (;;) {
        const int32_t v = Signed10(n);
        if (m == Model::Hardware) {
            coef[kZigZag[k]] = HwCoef(v, k == 0 ? static_cast<int32_t>(qt[0]) : q * qt[static_cast<size_t>(k)], k == 0);
            if (k == 0 && q == 0) coef[0] = std::clamp(static_cast<int32_t>(static_cast<uint32_t>(v) << 5), -0x4000, 0x3FFF);
        } else {
            int32_t val;
            if (q == 0) val = v * 2;
            else if (k == 0) val = v * qt[0];
            else if (m == Model::Legacy) val = (v * qt[static_cast<size_t>(k)] * q + 4) >> 3;
            else val = (v * qt[static_cast<size_t>(k)] * q + 4) / 8;
            coef[q > 0 ? kZigZag[k] : k] = std::clamp(val, -0x400, 0x3FF);
        }
        if (m != Model::Legacy && k >= 63) return true; // the unit ends the block at index 63
        if (in.Empty()) return true;                     // a truncated stream: the block as far as it went
        n = in.Next();
        k += static_cast<int32_t>((n >> 10) & 0x3Fu) + 1;
        if (k > 63) return true;
    }
}

void Idct(int32_t blk[64], const std::array<int16_t, 64>& matrix, Model m) {
    switch (m) {
    case Model::Hardware: HwIdct(blk, matrix); return;
    case Model::Legacy: LegacyIdct(blk, matrix); return;
    case Model::EmuOld: EmuOldIdct(blk, matrix); return;
    }
}

uint32_t Colour(int32_t y, int32_t cb, int32_t cr, bool signedOut, Model m) {
    switch (m) {
    case Model::Hardware: return HwColour(y, cb, cr, signedOut);
    case Model::Legacy: return LegacyColour(y, cb, cr);
    case Model::EmuOld: return EmuOldColour(y, cb, cr, signedOut);
    }
    return 0;
}

uint8_t Mono(int32_t y, bool signedOut, Model m) {
    if (m == Model::Legacy) y = (y + 4) >> 3;
    return static_cast<uint8_t>(Wrap9Sat8(y) + (signedOut ? 0 : 128));
}

uint16_t To15(uint32_t rgb, bool stp, Model m) {
    const auto five = [m](uint32_t c) { return m == Model::EmuOld ? c >> 3 : std::min<uint32_t>(31u, (c + 4u) >> 3); };
    return static_cast<uint16_t>((stp ? 0x8000u : 0u) | (five((rgb >> 16) & 0xFFu) << 10) | (five((rgb >> 8) & 0xFFu) << 5) |
                                 five(rgb & 0xFFu));
}

bool DecodeColourMacroblock(Input& in, const Tables& t, bool signedOut, Model m, uint32_t rgb[256]) {
    int32_t b[6][64];
    for (int i = 0; i < 6; ++i) {
        if (!ReadBlock(in, i < 2 ? t.chroma : t.luma, b[i], m)) return false;
        Idct(b[i], t.idct, m);
    }
    for (int sy = 0; sy < 16; ++sy)
        for (int sx = 0; sx < 16; ++sx) {
            const int ci = (sy >> 1) * 8 + (sx >> 1);
            const int32_t y = b[2 + (sy >> 3) * 2 + (sx >> 3)][(sy & 7) * 8 + (sx & 7)];
            rgb[sy * 16 + sx] = Colour(y, b[1][ci], b[0][ci], signedOut, m);
        }
    return true;
}

bool DecodeMonoBlock(Input& in, const Tables& t, bool signedOut, Model m, uint8_t y8[64]) {
    int32_t b[64];
    if (!ReadBlock(in, t.luma, b, m)) return false;
    Idct(b, t.idct, m);
    for (int i = 0; i < 64; ++i) y8[i] = Mono(b[i], signedOut, m);
    return true;
}

size_t DecodeCommand(uint32_t cmd, Input& in, const Tables& t, Model m, uint32_t* out, size_t cap, size_t& outWords) {
    const uint32_t depth = (cmd >> 27) & 3u;
    const bool signedOut = ((cmd >> 26) & 1u) != 0, stp = ((cmd >> 25) & 1u) != 0;
    const auto push = [&](uint32_t w) {
        if (outWords < cap) out[outWords] = w;
        ++outWords;
    };
    size_t units = 0;
    if (depth >= 2u) {
        uint32_t rgb[256];
        while (DecodeColourMacroblock(in, t, signedOut, m, rgb)) {
            ++units;
            if (depth == 3u) {
                for (int i = 0; i < 256; i += 2) push(To15(rgb[i], stp, m) | (static_cast<uint32_t>(To15(rgb[i + 1], stp, m)) << 16));
            } else { // 24-bit: R G B bytes packed, 3 words per 4 pixels
                uint8_t bytes[768];
                for (int i = 0; i < 256; ++i) {
                    bytes[i * 3] = static_cast<uint8_t>(rgb[i]);
                    bytes[i * 3 + 1] = static_cast<uint8_t>(rgb[i] >> 8);
                    bytes[i * 3 + 2] = static_cast<uint8_t>(rgb[i] >> 16);
                }
                for (int i = 0; i < 768; i += 4)
                    push(bytes[i] | (bytes[i + 1] << 8) | (bytes[i + 2] << 16) | (static_cast<uint32_t>(bytes[i + 3]) << 24));
            }
        }
    } else {
        uint8_t y8[64];
        while (DecodeMonoBlock(in, t, signedOut, m, y8)) {
            ++units;
            if (depth == 1u)
                for (int i = 0; i < 64; i += 4) push(y8[i] | (y8[i + 1] << 8) | (y8[i + 2] << 16) | (static_cast<uint32_t>(y8[i + 3]) << 24));
            else
                for (int i = 0; i < 64; i += 8) {
                    uint32_t w = 0;
                    for (int j = 0; j < 8; ++j) w |= static_cast<uint32_t>(y8[i + j] >> 4) << (4 * j);
                    push(w);
                }
        }
    }
    return units;
}

} // namespace rr::mdec
