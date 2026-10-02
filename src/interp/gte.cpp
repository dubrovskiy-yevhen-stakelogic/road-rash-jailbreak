#include "interp/gte.h"

#include <algorithm>
#include <cstring>

namespace rr::interp {
namespace {

// The GTE's reciprocal seed table. Generated, not transcribed: the hardware table is exactly
//   unr[i] = max(0, (0x40000 / (i + 0x100) + 1) / 2 - 0x101)   for i = 0..256
// (psx-spx gives both the closed form and the 257 byte values; the closed form is used here so the
// constant is verifiable by inspection rather than by trusting a copied blob).
struct UnrTable {
    uint8_t v[257];
    constexpr UnrTable() : v{} {
        for (int i = 0; i <= 256; ++i) {
            const int x = (0x40000 / (i + 0x100) + 1) / 2 - 0x101;
            v[i] = static_cast<uint8_t>(x < 0 ? 0 : x);
        }
    }
};
constexpr UnrTable kUnr{};

constexpr int64_t kMacMax = (int64_t{1} << 43) - 1;
constexpr int64_t kMacMin = -(int64_t{1} << 43);

inline int32_t SignExtend16(uint32_t v) { return static_cast<int32_t>(static_cast<int16_t>(v & 0xFFFFu)); }

inline uint8_t CountLeadingZeros16(uint32_t v) {
    uint8_t n = 0;
    for (int i = 15; i >= 0; --i) {
        if (v & (1u << i)) break;
        ++n;
    }
    return n;
}

} // namespace

void Gte::Reset() {
    std::memset(dr, 0, sizeof(dr));
    std::memset(cr, 0, sizeof(cr));
    unimplemented = false;
    unimplementedDetail.clear();
}

// --------------------------------------------------------------------------------- accessors

int16_t Gte::Rt(int row, int col) const {
    const int i = row * 3 + col;
    const uint32_t w = cr[i >> 1];
    return static_cast<int16_t>((i & 1) ? (w >> 16) : (w & 0xFFFFu));
}
int16_t Gte::Llm(int row, int col) const {
    const int i = row * 3 + col;
    const uint32_t w = cr[8 + (i >> 1)];
    return static_cast<int16_t>((i & 1) ? (w >> 16) : (w & 0xFFFFu));
}
int16_t Gte::Lcm(int row, int col) const {
    const int i = row * 3 + col;
    const uint32_t w = cr[16 + (i >> 1)];
    return static_cast<int16_t>((i & 1) ? (w >> 16) : (w & 0xFFFFu));
}
int32_t Gte::Tr(int i) const { return static_cast<int32_t>(cr[5 + i]); }
int32_t Gte::Bk(int i) const { return static_cast<int32_t>(cr[13 + i]); }
int32_t Gte::Fc(int i) const { return static_cast<int32_t>(cr[21 + i]); }

int16_t Gte::V(int vec, int i) const {
    const uint32_t xy = dr[vec * 2];
    if (i == 0) return static_cast<int16_t>(xy & 0xFFFFu);
    if (i == 1) return static_cast<int16_t>(xy >> 16);
    return static_cast<int16_t>(dr[vec * 2 + 1] & 0xFFFFu);
}
int16_t Gte::Ir(int i) const { return static_cast<int16_t>(dr[8 + i] & 0xFFFFu); }

// --------------------------------------------------------------------------------- saturation

int64_t Gte::CheckMac(int n, int64_t value) {
    if (value > kMacMax) SetFlag(30 - (n - 1));
    else if (value < kMacMin) SetFlag(27 - (n - 1));
    // The accumulator is 44 bits wide; overflow wraps, it does not saturate.
    return (value << 20) >> 20;
}

int32_t Gte::CheckMac0(int64_t value) {
    if (value > int64_t{0x7FFFFFFF}) SetFlag(16);
    else if (value < -int64_t{0x80000000}) SetFlag(15);
    const int32_t truncated = static_cast<int32_t>(static_cast<uint32_t>(static_cast<uint64_t>(value) & 0xFFFFFFFFu));
    dr[24] = static_cast<uint32_t>(truncated);
    return truncated;
}

int32_t Gte::ClampIr(int n, int32_t value, bool lm) {
    const int32_t lo = lm ? 0 : -0x8000;
    if (value < lo) { SetFlag(24 - (n - 1)); return lo; }
    if (value > 0x7FFF) { SetFlag(24 - (n - 1)); return 0x7FFF; }
    return value;
}

void Gte::SetIr(int n, int32_t value, bool lm) {
    dr[8 + n] = static_cast<uint32_t>(ClampIr(n, value, lm));
}

void Gte::SetMacAndIr(int n, int64_t acc, bool lm) {
    const int32_t mac = static_cast<int32_t>(acc >> shift_);
    dr[24 + n] = static_cast<uint32_t>(mac);
    SetIr(n, mac, lm);
}

int32_t Gte::ClampSxy(int32_t value, int flagBit) {
    if (value < -0x400) { SetFlag(flagBit); return -0x400; }
    if (value > 0x3FF) { SetFlag(flagBit); return 0x3FF; }
    return value;
}

uint16_t Gte::ClampSz(int64_t value) {
    if (value < 0) { SetFlag(18); return 0; }
    if (value > 0xFFFF) { SetFlag(18); return 0xFFFF; }
    return static_cast<uint16_t>(value);
}

uint8_t Gte::ClampColor(int32_t value, int flagBit) {
    if (value < 0) { SetFlag(flagBit); return 0; }
    if (value > 0xFF) { SetFlag(flagBit); return 0xFF; }
    return static_cast<uint8_t>(value);
}

void Gte::PushSz(int64_t value) {
    dr[16] = dr[17];
    dr[17] = dr[18];
    dr[18] = dr[19];
    dr[19] = ClampSz(value);
}

void Gte::PushSxy(int32_t x, int32_t y) {
    dr[12] = dr[13];
    dr[13] = dr[14];
    dr[14] = (static_cast<uint32_t>(y & 0xFFFF) << 16) | static_cast<uint32_t>(x & 0xFFFF);
    dr[15] = dr[14];
}

void Gte::PushColor(int32_t r, int32_t g, int32_t b) {
    const uint32_t code = dr[6] & 0xFF000000u;
    dr[20] = dr[21];
    dr[21] = dr[22];
    const uint32_t cb = static_cast<uint32_t>(ClampColor(b, 19));
    const uint32_t cg = static_cast<uint32_t>(ClampColor(g, 20));
    const uint32_t crr = static_cast<uint32_t>(ClampColor(r, 21));
    dr[22] = code | (cb << 16) | (cg << 8) | crr;
}

void Gte::PushColorFromMac() {
    PushColor(static_cast<int32_t>(dr[25]) >> 4, static_cast<int32_t>(dr[26]) >> 4,
              static_cast<int32_t>(dr[27]) >> 4);
}

// --------------------------------------------------------------------------------- register file

uint32_t Gte::ReadData(uint32_t reg) const {
    switch (reg) {
    case 7:  return dr[7] & 0xFFFFu;                       // OTZ, unsigned 16
    case 8: case 9: case 10: case 11:
        return static_cast<uint32_t>(SignExtend16(dr[reg])); // IR0..IR3
    case 15: return dr[14];                                 // SXYP mirrors SXY2
    case 16: case 17: case 18: case 19:
        return dr[reg] & 0xFFFFu;                           // SZ0..SZ3, unsigned 16
    case 28:
    case 29: {                                              // IRGB / ORGB, both read the same way
        const int32_t r = std::clamp<int32_t>(SignExtend16(dr[9]) / 0x80, 0, 0x1F);
        const int32_t g = std::clamp<int32_t>(SignExtend16(dr[10]) / 0x80, 0, 0x1F);
        const int32_t b = std::clamp<int32_t>(SignExtend16(dr[11]) / 0x80, 0, 0x1F);
        return static_cast<uint32_t>(r | (g << 5) | (b << 10));
    }
    default: return dr[reg];
    }
}

void Gte::WriteData(uint32_t reg, uint32_t value) {
    switch (reg) {
    case 7:  dr[7] = value & 0xFFFFu; break;
    case 8: case 9: case 10: case 11:
        dr[reg] = static_cast<uint32_t>(SignExtend16(value));
        break;
    case 15: PushSxy(static_cast<int16_t>(value & 0xFFFFu), static_cast<int16_t>(value >> 16)); break;
    case 16: case 17: case 18: case 19:
        dr[reg] = value & 0xFFFFu;
        break;
    case 28:
        dr[28] = value & 0x7FFFu;
        dr[9] = static_cast<uint32_t>((value & 0x1Fu) * 0x80);
        dr[10] = static_cast<uint32_t>(((value >> 5) & 0x1Fu) * 0x80);
        dr[11] = static_cast<uint32_t>(((value >> 10) & 0x1Fu) * 0x80);
        break;
    case 29: break; // ORGB is read-only
    case 30: {
        dr[30] = value;
        // LZCR counts leading zeroes of a positive LZCS, leading ones of a negative one.
        uint32_t v = (value & 0x80000000u) ? ~value : value;
        uint32_t n = 0;
        while (n < 32 && (v & 0x80000000u) == 0) { ++n; v <<= 1; }
        dr[31] = n;
        break;
    }
    case 31: break; // LZCR is read-only
    default: dr[reg] = value; break;
    }
}

uint32_t Gte::ReadControl(uint32_t reg) const {
    switch (reg) {
    // The 16-bit control registers read back sign-extended, including H (which is used as unsigned).
    case 4: case 12: case 20: case 26: case 27: case 29: case 30:
        return static_cast<uint32_t>(SignExtend16(cr[reg]));
    default: return cr[reg];
    }
}

void Gte::WriteControl(uint32_t reg, uint32_t value) {
    if (reg == 31) {
        // Only bits 30..12 are writable; bit 31 is the recomputed error summary.
        const uint32_t f = value & 0x7FFFF000u;
        cr[31] = f | ((f & 0x7F87E000u) ? 0x80000000u : 0u);
        return;
    }
    cr[reg] = value;
}

// --------------------------------------------------------------------------------- division

uint32_t Gte::Divide(uint32_t h, uint32_t sz3) {
    if (h < sz3 * 2) {
        const uint8_t z = CountLeadingZeros16(sz3);
        const uint32_t n = h << z;
        uint32_t d = sz3 << z; // 0x8000..0xFFFF
        const uint32_t u = kUnr.v[(d - 0x7FC0) >> 7] + 0x101u;
        d = static_cast<uint32_t>((0x2000080u - d * u) >> 8);
        d = static_cast<uint32_t>((0x0000080u + d * u) >> 8);
        const uint64_t r = (static_cast<uint64_t>(n) * d + 0x8000u) >> 16;
        return static_cast<uint32_t>(std::min<uint64_t>(0x1FFFFu, r));
    }
    SetFlag(17);
    return 0x1FFFF;
}

// --------------------------------------------------------------------------------- commands

void Gte::MatrixVector(int matrix, int translation, const int16_t v[3], bool lm) {
    for (int n = 0; n < 3; ++n) {
        int16_t m0 = 0, m1 = 0, m2 = 0;
        switch (matrix) {
        case 0: m0 = Rt(n, 0);  m1 = Rt(n, 1);  m2 = Rt(n, 2);  break;
        case 1: m0 = Llm(n, 0); m1 = Llm(n, 1); m2 = Llm(n, 2); break;
        default: m0 = Lcm(n, 0); m1 = Lcm(n, 1); m2 = Lcm(n, 2); break;
        }
        int32_t t = 0;
        switch (translation) {
        case 0: t = Tr(n); break;
        case 1: t = Bk(n); break;
        case 2: t = Fc(n); break;
        default: t = 0; break;
        }

        if (translation == 2) {
            // Documented hardware bug: with the far-colour vector selected the first product is
            // evaluated (and its IR saturation is flagged) but then discarded, so only the last two
            // matrix terms reach the result.
            const int64_t discarded = CheckMac(n + 1, (static_cast<int64_t>(t) << 12) +
                                                          static_cast<int64_t>(m0) * v[0]);
            ClampIr(n + 1, static_cast<int32_t>(discarded >> shift_), false);
            int64_t acc = CheckMac(n + 1, static_cast<int64_t>(m1) * v[1]);
            acc = CheckMac(n + 1, acc + static_cast<int64_t>(m2) * v[2]);
            SetMacAndIr(n + 1, acc, lm);
        } else {
            int64_t acc = static_cast<int64_t>(t) << 12;
            acc = CheckMac(n + 1, acc + static_cast<int64_t>(m0) * v[0]);
            acc = CheckMac(n + 1, acc + static_cast<int64_t>(m1) * v[1]);
            acc = CheckMac(n + 1, acc + static_cast<int64_t>(m2) * v[2]);
            SetMacAndIr(n + 1, acc, lm);
        }
    }
}

void Gte::DoRtps(int vector, bool /*last*/) {
    const int16_t vx = V(vector, 0);
    const int16_t vy = V(vector, 1);
    const int16_t vz = V(vector, 2);

    int64_t a[3];
    for (int n = 0; n < 3; ++n) {
        int64_t acc = static_cast<int64_t>(Tr(n)) << 12;
        acc = CheckMac(n + 1, acc + static_cast<int64_t>(Rt(n, 0)) * vx);
        acc = CheckMac(n + 1, acc + static_cast<int64_t>(Rt(n, 1)) * vy);
        acc = CheckMac(n + 1, acc + static_cast<int64_t>(Rt(n, 2)) * vz);
        a[n] = acc;
        dr[25 + n] = static_cast<uint32_t>(static_cast<int32_t>(acc >> shift_));
    }
    SetIr(1, static_cast<int32_t>(a[0] >> shift_), lm_);
    SetIr(2, static_cast<int32_t>(a[1] >> shift_), lm_);

    // IR3 quirk: the saturation *flag* is decided from MAC3 shifted by 12 regardless of `sf`, while
    // the stored value is the ordinary clamp of MAC3.
    {
        const int32_t forFlag = static_cast<int32_t>(a[2] >> 12);
        if (forFlag < -0x8000 || forFlag > 0x7FFF) SetFlag(22);
        const int32_t mac3 = static_cast<int32_t>(a[2] >> shift_);
        const int32_t lo = lm_ ? 0 : -0x8000;
        dr[11] = static_cast<uint32_t>(std::clamp(mac3, lo, 0x7FFF));
    }

    PushSz(a[2] >> 12);

    const uint32_t d = Divide(cr[26] & 0xFFFFu, dr[19] & 0xFFFFu);

    const int64_t sx = static_cast<int64_t>(d) * Ir(1) + static_cast<int64_t>(static_cast<int32_t>(cr[24]));
    CheckMac0(sx);
    const int32_t x = ClampSxy(static_cast<int32_t>(sx >> 16), 14);

    const int64_t sy = static_cast<int64_t>(d) * Ir(2) + static_cast<int64_t>(static_cast<int32_t>(cr[25]));
    CheckMac0(sy);
    const int32_t y = ClampSxy(static_cast<int32_t>(sy >> 16), 13);

    PushSxy(x, y);

    const int64_t dq = static_cast<int64_t>(d) * SignExtend16(cr[27]) +
                       static_cast<int64_t>(static_cast<int32_t>(cr[28]));
    CheckMac0(dq);
    int32_t ir0 = static_cast<int32_t>(dq >> 12);
    if (ir0 < 0) { SetFlag(12); ir0 = 0; }
    else if (ir0 > 0x1000) { SetFlag(12); ir0 = 0x1000; }
    dr[8] = static_cast<uint32_t>(ir0);
}

void Gte::DoNclip() {
    const int32_t sx0 = static_cast<int16_t>(dr[12] & 0xFFFFu);
    const int32_t sy0 = static_cast<int16_t>(dr[12] >> 16);
    const int32_t sx1 = static_cast<int16_t>(dr[13] & 0xFFFFu);
    const int32_t sy1 = static_cast<int16_t>(dr[13] >> 16);
    const int32_t sx2 = static_cast<int16_t>(dr[14] & 0xFFFFu);
    const int32_t sy2 = static_cast<int16_t>(dr[14] >> 16);
    const int64_t v = static_cast<int64_t>(sx0) * sy1 + static_cast<int64_t>(sx1) * sy2 +
                      static_cast<int64_t>(sx2) * sy0 - static_cast<int64_t>(sx0) * sy2 -
                      static_cast<int64_t>(sx1) * sy0 - static_cast<int64_t>(sx2) * sy1;
    CheckMac0(v);
}

void Gte::DoAvsz(int count) {
    int64_t sum = static_cast<int64_t>(dr[17] & 0xFFFFu) + (dr[18] & 0xFFFFu) + (dr[19] & 0xFFFFu);
    int32_t zsf = SignExtend16(cr[29]);
    if (count == 4) {
        sum += (dr[16] & 0xFFFFu);
        zsf = SignExtend16(cr[30]);
    }
    const int64_t v = static_cast<int64_t>(zsf) * sum;
    CheckMac0(v);
    dr[7] = ClampSz(v >> 12);
}

void Gte::DoMvmva(uint32_t command) {
    const int mx = static_cast<int>((command >> 17) & 3);
    const int vsel = static_cast<int>((command >> 15) & 3);
    const int cv = static_cast<int>((command >> 13) & 3);
    if (mx == 3) {
        unimplemented = true;
        unimplementedDetail = "MVMVA with matrix select 3 (the undocumented 'garbage matrix') is not "
                              "implemented; its hardware behaviour is a bug and would have to be "
                              "measured, not guessed";
        return;
    }
    int16_t v[3];
    if (vsel == 3) {
        v[0] = Ir(1); v[1] = Ir(2); v[2] = Ir(3);
    } else {
        v[0] = V(vsel, 0); v[1] = V(vsel, 1); v[2] = V(vsel, 2);
    }
    MatrixVector(mx, cv, v, lm_);
}

void Gte::DoSquare() {
    for (int n = 1; n <= 3; ++n) {
        const int64_t acc = static_cast<int64_t>(Ir(n)) * Ir(n);
        SetMacAndIr(n, acc, lm_);
    }
}

void Gte::DoOuterProduct() {
    const int32_t d1 = Rt(0, 0), d2 = Rt(1, 1), d3 = Rt(2, 2);
    const int32_t ir1 = Ir(1), ir2 = Ir(2), ir3 = Ir(3);
    const int64_t a1 = static_cast<int64_t>(d2) * ir3 - static_cast<int64_t>(d3) * ir2;
    const int64_t a2 = static_cast<int64_t>(d3) * ir1 - static_cast<int64_t>(d1) * ir3;
    const int64_t a3 = static_cast<int64_t>(d1) * ir2 - static_cast<int64_t>(d2) * ir1;
    SetMacAndIr(1, CheckMac(1, a1), lm_);
    SetMacAndIr(2, CheckMac(2, a2), lm_);
    SetMacAndIr(3, CheckMac(3, a3), lm_);
}

void Gte::DoInterpolateTail(int64_t mac1, int64_t mac2, int64_t mac3) {
    const int64_t in[3] = {mac1, mac2, mac3};
    int64_t diff[3];
    for (int n = 0; n < 3; ++n) {
        diff[n] = CheckMac(n + 1, (static_cast<int64_t>(Fc(n)) << 12) - in[n]);
        // The intermediate IR clamp of the interpolation always behaves as lm = 0.
        SetIr(n + 1, static_cast<int32_t>(diff[n] >> shift_), false);
    }
    const int32_t ir0 = SignExtend16(dr[8]);
    for (int n = 0; n < 3; ++n) {
        const int64_t acc = CheckMac(n + 1, static_cast<int64_t>(Ir(n + 1)) * ir0 + in[n]);
        SetMacAndIr(n + 1, acc, lm_);
    }
    PushColorFromMac();
}

void Gte::DoDpcs(bool fromRgbFifo) {
    const uint32_t c = fromRgbFifo ? dr[20] : dr[6];
    DoInterpolateTail(static_cast<int64_t>(c & 0xFFu) << 16,
                      static_cast<int64_t>((c >> 8) & 0xFFu) << 16,
                      static_cast<int64_t>((c >> 16) & 0xFFu) << 16);
}

void Gte::DoIntpl() {
    DoInterpolateTail(static_cast<int64_t>(Ir(1)) << 12, static_cast<int64_t>(Ir(2)) << 12,
                      static_cast<int64_t>(Ir(3)) << 12);
}

void Gte::DoDcpl() {
    const uint32_t c = dr[6];
    DoInterpolateTail((static_cast<int64_t>(c & 0xFFu) * Ir(1)) << 4,
                      (static_cast<int64_t>((c >> 8) & 0xFFu) * Ir(2)) << 4,
                      (static_cast<int64_t>((c >> 16) & 0xFFu) * Ir(3)) << 4);
}

// `vector` < 0 means "skip the light-matrix step and start from IR" (the CC / CDP commands).
// `colourMul` multiplies the result by RGBC, `depthCue` runs the far-colour interpolation tail.
void Gte::DoNormalColour(int vector, bool depthCue, bool colourMul, bool /*useRgbc*/) {
    if (vector >= 0) {
        const int16_t v[3] = {V(vector, 0), V(vector, 1), V(vector, 2)};
        MatrixVector(1, 3, v, lm_); // MAC = IR = (LLM * V) >> sf*12
    }
    {
        const int16_t v[3] = {Ir(1), Ir(2), Ir(3)};
        MatrixVector(2, 1, v, lm_); // MAC = IR = (BK*0x1000 + LCM * IR) >> sf*12
    }
    if (!colourMul && !depthCue) {
        PushColorFromMac();
        return;
    }
    const uint32_t c = dr[6];
    const int64_t m1 = (static_cast<int64_t>(c & 0xFFu) * Ir(1)) << 4;
    const int64_t m2 = (static_cast<int64_t>((c >> 8) & 0xFFu) * Ir(2)) << 4;
    const int64_t m3 = (static_cast<int64_t>((c >> 16) & 0xFFu) * Ir(3)) << 4;
    if (depthCue) {
        DoInterpolateTail(m1, m2, m3);
    } else {
        SetMacAndIr(1, m1, lm_);
        SetMacAndIr(2, m2, lm_);
        SetMacAndIr(3, m3, lm_);
        PushColorFromMac();
    }
}

void Gte::DoCc() { DoNormalColour(-1, false, true, true); }
void Gte::DoCdp() { DoNormalColour(-1, true, true, true); }

void Gte::DoGpf(bool useIr0Only) {
    const int32_t ir0 = SignExtend16(dr[8]);
    int64_t base[3] = {0, 0, 0};
    if (!useIr0Only) {
        // GPL starts from the current MAC1..3 shifted left by sf*12.
        base[0] = static_cast<int64_t>(static_cast<int32_t>(dr[25])) << shift_;
        base[1] = static_cast<int64_t>(static_cast<int32_t>(dr[26])) << shift_;
        base[2] = static_cast<int64_t>(static_cast<int32_t>(dr[27])) << shift_;
    }
    for (int n = 0; n < 3; ++n) {
        const int64_t acc = CheckMac(n + 1, static_cast<int64_t>(Ir(n + 1)) * ir0 + base[n]);
        SetMacAndIr(n + 1, acc, lm_);
    }
    PushColorFromMac();
}

void Gte::Execute(uint32_t command) {
    const uint32_t op = command & 0x3F;
    sf_ = ((command >> 19) & 1) != 0;
    lm_ = ((command >> 10) & 1) != 0;
    shift_ = sf_ ? 12 : 0;
    cr[31] = 0;

    switch (op) {
    case 0x01: DoRtps(0, true); break;                       // RTPS
    case 0x06: DoNclip(); break;                             // NCLIP
    case 0x0C: DoOuterProduct(); break;                      // OP
    case 0x10: DoDpcs(false); break;                         // DPCS
    case 0x11: DoIntpl(); break;                             // INTPL
    case 0x12: DoMvmva(command); break;                      // MVMVA
    case 0x13: DoNormalColour(0, true, true, true); break;   // NCDS
    case 0x14: DoCdp(); break;                               // CDP
    case 0x16:                                               // NCDT
        for (int v = 0; v < 3; ++v) DoNormalColour(v, true, true, true);
        break;
    case 0x1B: DoNormalColour(0, false, true, true); break;  // NCCS
    case 0x1C: DoCc(); break;                                // CC
    case 0x1E: DoNormalColour(0, false, false, false); break; // NCS
    case 0x20:                                               // NCT
        for (int v = 0; v < 3; ++v) DoNormalColour(v, false, false, false);
        break;
    case 0x28: DoSquare(); break;                            // SQR
    case 0x29: DoDcpl(); break;                              // DCPL
    case 0x2A:                                               // DPCT
        for (int i = 0; i < 3; ++i) DoDpcs(true);
        break;
    case 0x2D: DoAvsz(3); break;                             // AVSZ3
    case 0x2E: DoAvsz(4); break;                             // AVSZ4
    case 0x30:                                               // RTPT
        DoRtps(0, false);
        DoRtps(1, false);
        DoRtps(2, true);
        break;
    case 0x3D: DoGpf(true); break;                           // GPF
    case 0x3E: DoGpf(false); break;                          // GPL
    case 0x3F:                                               // NCCT
        for (int v = 0; v < 3; ++v) DoNormalColour(v, false, true, true);
        break;
    default:
        unimplemented = true;
        unimplementedDetail = "unknown COP2 command opcode";
        break;
    }

    if (cr[31] & 0x7F87E000u) cr[31] |= 0x80000000u;
}

} // namespace rr::interp
