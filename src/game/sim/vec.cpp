#include "game/sim/vec.h"

namespace rr::sim {
namespace {

// `mult rs,rt` followed by `v0 = (lo >> 16) | (hi << 16)`.
inline int32_t MultHiLo16(int32_t a, int32_t b) {
    const uint64_t p = static_cast<uint64_t>(static_cast<int64_t>(a) * static_cast<int64_t>(b));
    return static_cast<int32_t>(static_cast<uint32_t>(p >> 16));
}
inline int32_t Add32(int32_t a, int32_t b) {
    return static_cast<int32_t>(static_cast<uint32_t>(a) + static_cast<uint32_t>(b));
}
inline int32_t Sub32(int32_t a, int32_t b) {
    return static_cast<int32_t>(static_cast<uint32_t>(a) - static_cast<uint32_t>(b));
}
inline int32_t Shl4(int16_t v) {
    return static_cast<int32_t>(static_cast<uint32_t>(static_cast<int32_t>(v)) << 4);
}

} // namespace

int32_t DotLcm(const int16_t v[3], const int16_t m[3]) {
    // The 44-bit MAC accumulates term by term; with int16 operands no term and no partial sum can
    // reach 2^43, so the only truncation that is ever observable is the 32-bit one that `mfc2`
    // applies when MAC1 is read back.
    const int64_t acc = static_cast<int64_t>(m[0]) * v[0] + static_cast<int64_t>(m[1]) * v[1] +
                        static_cast<int64_t>(m[2]) * v[2];
    const int32_t mac1 = static_cast<int32_t>(static_cast<uint32_t>(static_cast<uint64_t>(acc) & 0xFFFFFFFFu));
    return mac1 >> 8;
}

void MulAdd(const int32_t base[3], const int16_t dir[3], int32_t t, int32_t out[3]) {
    for (int i = 0; i < 3; ++i) out[i] = Add32(MultHiLo16(Shl4(dir[i]), t), base[i]);
}

void MulAdd32(const int32_t base[3], const int32_t dir[3], int32_t t, int32_t out[3]) {
    for (int i = 0; i < 3; ++i) out[i] = Add32(MultHiLo16(dir[i], t), base[i]);
}

void Scale(int32_t t, const int16_t dir[3], int32_t out[3]) {
    for (int i = 0; i < 3; ++i) out[i] = MultHiLo16(t, Shl4(dir[i]));
}

void RoadProject(const int32_t p[3], const RoadSliceView& slice, int32_t* lateral, int32_t* along) {
    // 0x80036810 / 0x800368C4: each half is guarded by its own output pointer.
    if (lateral != nullptr) {
        int32_t acc = MultHiLo16(Shl4(slice.m[0]), Sub32(p[0], slice.pos[0]));
        acc = Add32(MultHiLo16(Shl4(slice.m[1]), Sub32(p[1], slice.pos[1])), acc);
        acc = Add32(MultHiLo16(Shl4(slice.m[2]), Sub32(p[2], slice.pos[2])), acc);
        *lateral = acc;
    }
    if (along != nullptr) {
        int32_t acc = MultHiLo16(Shl4(slice.m[6]), Sub32(p[0], slice.pos[0]));
        acc = Add32(MultHiLo16(Shl4(slice.m[7]), Sub32(p[1], slice.pos[1])), acc);
        acc = Add32(MultHiLo16(Shl4(slice.m[8]), Sub32(p[2], slice.pos[2])), acc);
        *along = acc;
    }
}

void Blend32(const int32_t a[3], const int32_t b[3], int32_t out[3], int32_t wa, int32_t wb) {
    for (int i = 0; i < 3; ++i) out[i] = Add32(MultHiLo16(wb, b[i]), MultHiLo16(wa, a[i]));
}

void Blend16(const int16_t a[3], const int16_t b[3], int16_t out[3], int32_t wa, int32_t wb) {
    for (int i = 0; i < 3; ++i) {
        const int32_t s = Add32(MultHiLo16(wb, Shl4(b[i])), MultHiLo16(wa, Shl4(a[i])));
        out[i] = static_cast<int16_t>(static_cast<uint16_t>(static_cast<uint32_t>(s >> 4) & 0xFFFFu));
    }
}

void Scale32(int32_t t, const int32_t v[3], int32_t out[3]) {
    for (int i = 0; i < 3; ++i) out[i] = MultHiLo16(t, v[i]);
}

void Blend16To32(const int16_t a[3], const int16_t b[3], int32_t out[3], int32_t wa, int32_t wb) {
    for (int i = 0; i < 3; ++i) out[i] = Add32(MultHiLo16(wa, Shl4(a[i])), MultHiLo16(wb, Shl4(b[i])));
}

bool Normalize(int16_t v[3], const uint16_t* rsqrtTable) {
    const int32_t x = v[0], y = v[1], z = v[2];
    // GTE SQR with sf = 0: MAC1..3 are the plain squares, each at most 2^30.
    const int64_t sq0 = static_cast<int64_t>(x) * x;
    const int64_t sq1 = static_cast<int64_t>(y) * y;
    const int64_t sq2 = static_cast<int64_t>(z) * z;
    // `add t0,t0,t1` then `add v0,t0,t2` - both trap on signed overflow.
    const int64_t partial = sq0 + sq1;
    if (partial > 0x7FFFFFFF) return false;
    const int64_t total = partial + sq2;
    if (total > 0x7FFFFFFF) return false;
    const uint32_t n = static_cast<uint32_t>(total);

    // LZCS / LZCR: the count of leading bits equal to bit 31; 32 for a zero input.
    uint32_t probe = (n & 0x80000000u) ? ~n : n;
    uint32_t lz = 0;
    while (lz < 32 && (probe & 0x80000000u) == 0) { ++lz; probe <<= 1; }

    int32_t sh = 22 - static_cast<int32_t>(lz & ~1u);
    if (sh <= 0) sh = 0;
    const int32_t index = static_cast<int32_t>(n) >> sh; // srav
    const int32_t half = sh >> 1;
    const uint32_t w = rsqrtTable[index];
    const uint32_t exponent = w & 0x1Fu;
    const int32_t mantissa = static_cast<int32_t>(w >> 5);
    int32_t f = static_cast<int32_t>(static_cast<uint32_t>(mantissa) << (exponent & 31u)); // sllv
    f = f >> (half & 31);                                                                  // srav
    const int32_t in[3] = {x, y, z}; // the original multiplies the values it loaded before the stores
    for (int i = 0; i < 3; ++i) {
        const int32_t product = static_cast<int32_t>(static_cast<uint32_t>(
            static_cast<uint64_t>(static_cast<int64_t>(in[i]) * f) & 0xFFFFFFFFu)); // mflo
        v[i] = static_cast<int16_t>(static_cast<uint16_t>(static_cast<uint32_t>(product >> 12) & 0xFFFFu));
    }
    return true;
}

} // namespace rr::sim
