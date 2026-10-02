#include "game/sim/fixed.h"

namespace rr::sim {
namespace {

// The MIPS `sra rd,rt,31 / addu / xor` absolute-value idiom, in unsigned arithmetic so that
// abs(INT32_MIN) wraps to INT32_MIN exactly as it does on the hardware instead of being UB.
inline int32_t MipsAbs(int32_t v) {
    const uint32_t u = static_cast<uint32_t>(v);
    const uint32_t mask = static_cast<uint32_t>(v >> 31);
    return static_cast<int32_t>((mask + u) ^ mask);
}

inline int32_t Sra(int32_t v, int n) { return v >> n; } // MSVC and gcc both define this as arithmetic

// `mult rs,rt` followed by `v0 = (lo >> 16) | (hi << 16)`: bits 16..47 of the 64-bit signed product.
inline int32_t MultHiLo16(int32_t a, int32_t b) {
    const uint64_t p = static_cast<uint64_t>(static_cast<int64_t>(a) * static_cast<int64_t>(b));
    return static_cast<int32_t>(static_cast<uint32_t>(p >> 16));
}

} // namespace

int32_t FixMul(int32_t a, int32_t b) { return MultHiLo16(a, b); }

uint32_t FixDiv(uint32_t a, uint32_t b) {
    const uint32_t d = b >> 1;
    // R3000A `divu` with a zero divisor sets lo = 0xFFFFFFFF (and hi = the dividend). No exception.
    const uint32_t q = (d == 0) ? 0xFFFFFFFFu : (0x80000000u / d);
    const uint64_t p = static_cast<uint64_t>(a) * static_cast<uint64_t>(q); // multu: unsigned
    return static_cast<uint32_t>(p >> 16);
}

int32_t ApproxLen3(int32_t x, int32_t y, int32_t z) {
    int32_t a0 = MipsAbs(x);
    int32_t a1 = MipsAbs(y);
    int32_t a2 = MipsAbs(z);
    if (a0 < a1) { const int32_t t = a0; a0 = a1; a1 = t; }
    if (a0 < a2) { const int32_t t = a0; a0 = a2; a2 = t; }
    const int32_t sum = static_cast<int32_t>(static_cast<uint32_t>(a1) + static_cast<uint32_t>(a2));
    int32_t r = static_cast<int32_t>(static_cast<uint32_t>(a0) - static_cast<uint32_t>(Sra(a0, 4)));
    r = static_cast<int32_t>(static_cast<uint32_t>(r) + static_cast<uint32_t>(Sra(sum, 2)));
    r = static_cast<int32_t>(static_cast<uint32_t>(r) + static_cast<uint32_t>(Sra(sum, 3)));
    return r;
}

uint32_t Rand(uint32_t& seed) {
    seed = seed * 0x0019660Du + 0x3C6EF35Fu;
    return seed;
}

int32_t RatAtan2(int32_t y, int32_t x, const int32_t* table) {
    // 0x80020024: the only early-out. Note that `x == 0, y != 0` does NOT return here.
    if (x == 0 && y == 0) return 0;

    // 0x80020034..0x80020054: the octant code, then both arguments are made positive.
    uint32_t oct = static_cast<uint32_t>(x >> 31) & 2u;
    if (y < 0) oct |= 4u;
    int32_t ax = MipsAbs(x);
    int32_t ay = MipsAbs(y);

    int32_t num, den;
    bool negate; // the two `negu a1,v0` tails at 0x800200C8
    if (ax < ay) {
        oct |= 1u;                                   // 0x80020068, in the delay slot: always taken
        if (ax > 0) {
            num = ax;                                // 0x80020070, delay slot
            if (ay > 0) { den = ay;  negate = false; }
            else        { den = -ay; negate = true;  }
        } else {
            num = -ax;                               // 0x80020088, delay slot
            if (ay > 0) { den = ay;  negate = true;  }
            else        { den = -ay; negate = false; }
        }
    } else {
        if (ay > 0) {
            num = ay;                                // 0x800200A8, delay slot
            if (ax > 0) { den = ax;  negate = false; }
            else        { den = -ax; negate = true;  }
        } else {
            num = -ay;                               // 0x800200B8, delay slot
            if (ax > 0) { den = ax;  negate = true;  }
            else        { den = -ax; negate = false; }
        }
    }
    int32_t ratio = static_cast<int32_t>(FixDiv(static_cast<uint32_t>(num), static_cast<uint32_t>(den)));
    if (negate) ratio = static_cast<int32_t>(0u - static_cast<uint32_t>(ratio));

    // 0x800200DC..0x80020134: linear interpolation in the 0x8005285C table, 4096 steps per entry.
    const int32_t index = Sra(ratio, 12);
    const int32_t lo = table[index];
    const int32_t hi = table[index + 1];
    const int32_t frac = static_cast<int32_t>(static_cast<uint32_t>(ratio) & 0xFFFu);
    int32_t slope = static_cast<int32_t>(static_cast<uint32_t>(hi) - static_cast<uint32_t>(lo));
    slope = static_cast<int32_t>(static_cast<uint32_t>(slope) << 4);
    slope = static_cast<int32_t>(static_cast<uint32_t>(slope) + 8u);
    int32_t v = MultHiLo16(frac, slope);
    v = static_cast<int32_t>(static_cast<uint32_t>(v) + static_cast<uint32_t>(lo));
    const int32_t angle = Sra(v, 20);

    // 0x80020138: `sltiu v0,s0,8` - an octant code of 8 or more returns 0 rather than indexing the
    // jump table. Unreachable with the code above, but it is what the original does.
    if (oct >= 8u) return 0;
    switch (oct) {
        case 0: return angle;
        case 1: return static_cast<int32_t>(1024u - static_cast<uint32_t>(angle));
        case 2: return static_cast<int32_t>(2048u - static_cast<uint32_t>(angle));
        case 3: return static_cast<int32_t>(static_cast<uint32_t>(angle) + 1024u);
        case 4: return static_cast<int32_t>(0u - static_cast<uint32_t>(angle));
        case 5: return static_cast<int32_t>(static_cast<uint32_t>(angle) - 1024u);
        case 6: return static_cast<int32_t>(static_cast<uint32_t>(angle) - 2048u);
        default: return static_cast<int32_t>(static_cast<uint32_t>(-1024) - static_cast<uint32_t>(angle));
    }
}

int32_t RatTan(int32_t angle, const int16_t* sincos) {
    const uint32_t i = static_cast<uint32_t>(angle) & 0xFFFu;
    int32_t s = static_cast<int32_t>(static_cast<uint32_t>(static_cast<int32_t>(sincos[2 * i])) << 4);
    int32_t c = static_cast<int32_t>(static_cast<uint32_t>(static_cast<int32_t>(sincos[2 * i + 1])) << 4);
    // 0x8001FEE8..0x8001FF28: the four sign cases, transcribed rather than reduced.
    bool negate;
    if (s > 0) {
        if (c > 0) { negate = false; }
        else { c = static_cast<int32_t>(0u - static_cast<uint32_t>(c)); negate = true; }
    } else {
        if (c > 0) { s = static_cast<int32_t>(0u - static_cast<uint32_t>(s)); negate = true; }
        else {
            s = static_cast<int32_t>(0u - static_cast<uint32_t>(s));
            c = static_cast<int32_t>(0u - static_cast<uint32_t>(c));
            negate = false;
        }
    }
    const uint32_t q = FixDiv(static_cast<uint32_t>(s), static_cast<uint32_t>(c));
    return static_cast<int32_t>(negate ? (0u - q) : q);
}

int32_t ClampUnitFrac(int32_t t) {
    // 0x80074FC8..0x80074FE4 (and the identical sequence at 0x80075FB8 in the per-bike step):
    //   a0 = ~(t >> 31) & t          -> max(t, 0)
    //   v1 = 0x10000 - t ; v0 = (v1 >> 31) & v1   -> min(0x10000 - t, 0)
    //   result = a0 + v0
    const int32_t lowClamped = static_cast<int32_t>(~static_cast<uint32_t>(t >> 31) & static_cast<uint32_t>(t));
    const int32_t over = static_cast<int32_t>(0x10000u - static_cast<uint32_t>(t));
    const int32_t highTerm = static_cast<int32_t>(static_cast<uint32_t>(over >> 31) & static_cast<uint32_t>(over));
    return static_cast<int32_t>(static_cast<uint32_t>(lowClamped) + static_cast<uint32_t>(highTerm));
}

int32_t ClampLerpMin(int32_t a, int32_t b, int32_t t) {
    const int32_t frac = ClampUnitFrac(t);
    const int32_t s = FixMul(b, frac);
    if (!(s < a)) return a;            // 0x80075004, `beqz` on `slt s1,s2`
    const int32_t r = FixMul(a, frac); // 0x8007500C
    return (s < r) ? r : s;            // 0x8007501C
}

} // namespace rr::sim
