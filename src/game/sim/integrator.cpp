#include "game/sim/integrator.h"

#include <initializer_list>

#include "game/sim/ai.h"
#include "game/sim/fixed.h"
#include "game/sim/vec.h"

namespace rr::sim {
namespace {

inline int32_t Add32(int32_t a, int32_t b) {
    return static_cast<int32_t>(static_cast<uint32_t>(a) + static_cast<uint32_t>(b));
}
inline int32_t Sub32(int32_t a, int32_t b) {
    return static_cast<int32_t>(static_cast<uint32_t>(a) - static_cast<uint32_t>(b));
}
inline int32_t Neg32(int32_t a) { return static_cast<int32_t>(0u - static_cast<uint32_t>(a)); }
inline int32_t Mul32(int32_t a, int32_t b) { // `mflo` of mult/multu: the low word
    return static_cast<int32_t>(static_cast<uint32_t>(a) * static_cast<uint32_t>(b));
}
// `mult a,b` followed by `(lo >> 16) | (hi << 16)`: bits 16..47 of the signed product.
inline int32_t MulHi16(int32_t a, int32_t b) {
    const uint64_t p = static_cast<uint64_t>(static_cast<int64_t>(a) * static_cast<int64_t>(b));
    return static_cast<int32_t>(static_cast<uint32_t>(p >> 16));
}
inline int32_t Shl(int32_t v, int n) { return static_cast<int32_t>(static_cast<uint32_t>(v) << (n & 31)); }
inline int32_t Shl4(int16_t v) { return Shl(v, 4); }
// The `sra 31 / addu / xor` absolute value: INT32_MIN stays INT32_MIN.
inline int32_t AbsIdiom(int32_t v) {
    const int32_t s = v >> 31;
    return static_cast<int32_t>((static_cast<uint32_t>(v) + static_cast<uint32_t>(s)) ^ static_cast<uint32_t>(s));
}
inline int16_t Lo16(int32_t v) { return static_cast<int16_t>(static_cast<uint16_t>(static_cast<uint32_t>(v) & 0xFFFFu)); }
// (v * 652) >> 16, with the compiler's shift-and-add product (32-bit wrap) - the steer/lean to
// angle conversion, 652 = 4 * 163.
inline int32_t Mul652Sra16(int32_t v) { return Mul32(v, 652) >> 16; }
// (v * 25736) >> 8 - the angle back to steer/lean units, 25736 / 256 = 65536 / 652.
inline int32_t Mul25736Sra8(int32_t v) { return Mul32(v, 25736) >> 8; }
// ((163 * v) >>> 14) & 0xFFF - the sine-table index of a 16.16 angle (LOGICAL shift).
inline uint32_t AngleIndex(int32_t v) { return (static_cast<uint32_t>(Mul32(v, 163)) >> 14) & 0xFFFu; }

inline int32_t S32(const uint8_t* p) {
    return static_cast<int32_t>(static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
                                (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24));
}
inline void PutS32(uint8_t* p, int32_t v) {
    const uint32_t u = static_cast<uint32_t>(v);
    p[0] = static_cast<uint8_t>(u);
    p[1] = static_cast<uint8_t>(u >> 8);
    p[2] = static_cast<uint8_t>(u >> 16);
    p[3] = static_cast<uint8_t>(u >> 24);
}
inline int16_t S16(const uint8_t* p) {
    return static_cast<int16_t>(static_cast<uint16_t>(p[0] | (static_cast<uint32_t>(p[1]) << 8)));
}
inline uint16_t U16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (static_cast<uint32_t>(p[1]) << 8)); }

// Entity helpers.
inline int32_t W(EntityView& e, uint32_t off) { return static_cast<int32_t>(e.U32(off)); }
inline void SetW(EntityView& e, uint32_t off, int32_t v) { e.SetU32(off, static_cast<uint32_t>(v)); }
inline void SetH(EntityView& e, uint32_t off, int32_t v) {
    e.SetU16(off, static_cast<uint16_t>(static_cast<uint32_t>(v) & 0xFFFFu));
}
inline void Vec16(EntityView& e, uint32_t off, int16_t v[3]) {
    for (uint32_t i = 0; i < 3; ++i) v[i] = e.S16(off + 2u * i);
}
inline void Vec32(EntityView& e, uint32_t off, int32_t v[3]) {
    for (uint32_t i = 0; i < 3; ++i) v[i] = W(e, off + 4u * i);
}
inline void SetVec32(EntityView& e, uint32_t off, const int32_t v[3]) {
    for (uint32_t i = 0; i < 3; ++i) SetW(e, off + 4u * i, v[i]);
}
inline void SetVec16(EntityView& e, uint32_t off, const int16_t v[3]) {
    for (uint32_t i = 0; i < 3; ++i) SetH(e, off + 2u * i, v[i]);
}

// LZCS/LZCR: the count of leading bits equal to bit 31; 32 for 0.
inline uint32_t Lzcr(int32_t value) {
    uint32_t v = static_cast<uint32_t>(value);
    if (v & 0x80000000u) v = ~v;
    uint32_t n = 0;
    while (n < 32 && (v & 0x80000000u) == 0) { ++n; v <<= 1; }
    return n;
}

inline int32_t Sat16(int64_t mac) {
    const int32_t m = static_cast<int32_t>(mac);
    if (m < -0x8000) return -0x8000;
    if (m > 0x7FFF) return 0x7FFF;
    return m;
}

// The signed divide idiom the compiler builds around the UNSIGNED FixDiv: split both signs off,
// divide the magnitudes, and negate the quotient when exactly one of them was non-positive. The
// arms test `> 0`, so a zero operand counts as "negative" - reproduced, not tidied.
inline int32_t SignedDiv(int32_t num, int32_t den) {
    if (num > 0) {
        if (den > 0) return static_cast<int32_t>(FixDiv(static_cast<uint32_t>(num), static_cast<uint32_t>(den)));
        return Neg32(static_cast<int32_t>(FixDiv(static_cast<uint32_t>(num), static_cast<uint32_t>(Neg32(den)))));
    }
    if (den > 0)
        return Neg32(static_cast<int32_t>(FixDiv(static_cast<uint32_t>(Neg32(num)), static_cast<uint32_t>(den))));
    return static_cast<int32_t>(FixDiv(static_cast<uint32_t>(Neg32(num)), static_cast<uint32_t>(Neg32(den))));
}

// `(a + b) / 2` of two halfwords, rounded toward zero (`srl 31 / addu / sra 1`), stored as s16.
inline int16_t HalfSum(int16_t a, int16_t b) {
    const int32_t s = static_cast<int32_t>(a) + static_cast<int32_t>(b);
    return Lo16((s + static_cast<int32_t>(static_cast<uint32_t>(s) >> 31)) >> 1);
}

} // namespace

// ============================================================================ the leaves

bool Asin(int32_t x, const uint16_t* table, int32_t& out) {
    int32_t t0 = x;
    bool negative = false;
    if (t0 < 0) { negative = true; t0 = Neg32(t0); }       // 0x8001FF40
    int32_t v0 = 1024;
    if (!(0xFFFF < t0)) {                                   // 0x8001FF54
        int32_t a1 = 0xFFF8, v1 = 0, a3 = 0;
        if (t0 < 0xFFF8) {                                  // 0x8001FF64
            // Only INT32_MIN gets here negative; its index would be half a megabyte below the
            // table. The console would read whatever lies there; the port refuses.
            if (t0 < 0) return false;
            a1 = 0x8000;
            a3 = 15;                                        // delay slot of 0x8001FF78
            if (!(t0 < 0x8000)) {
                do {                                        // 0x8001FF84..0x8001FF98
                    a3 -= 1;
                    a1 += 1 << a3;
                    v1 += 4;
                } while (!(t0 < a1));
            }
            a1 -= 1 << a3;                                  // 0x8001FF9C..0x8001FFA8
            a3 -= 2;
        } else {
            v1 = 52;                                        // 0x8001FFB0
            a3 = 0;
        }
        const int32_t a2 = (t0 - a1) >> a3;                 // 0x8001FFB8
        a1 += a2 << a3;
        v1 += a2;
        const uint32_t lo = table[v1];
        const uint32_t hi = table[v1 + 1];
        const int32_t d = static_cast<int32_t>(hi - lo);
        const int32_t prod = Mul32(t0 - a1, d);             // `mflo`
        v0 = ((prod >> a3) + 8 + static_cast<int32_t>(lo)) >> 4;
    }
    out = negative ? Neg32(v0) : v0;                        // 0x80020004
    return true;
}

namespace {
// One angle's sine and cosine as RotMatrix fetches them: a negative angle is folded to its
// magnitude (masked to 12 bits) and the sine negated.
void FetchSinCos(int16_t angle, const int16_t* sc, int32_t& s, int32_t& c) {
    int32_t a = angle;
    if (a >= 0) {
        const uint32_t i = static_cast<uint32_t>(a) & 0xFFFu;
        s = sc[2 * i];
        c = sc[2 * i + 1];
    } else {
        a = Neg32(a);
        const uint32_t i = static_cast<uint32_t>(a) & 0xFFFu;
        s = Neg32(sc[2 * i]);
        c = sc[2 * i + 1];
    }
}
inline int32_t P12(int32_t a, int32_t b) { return Mul32(a, b) >> 12; } // `multu`, `mflo`, `sra 12`
} // namespace

void RotMatrix(const int16_t angles[3], int16_t m[9], const int16_t* sc) {
    int32_t sx, cx, sy, cy, sz, cz;
    FetchSinCos(angles[0], sc, sx, cx);                     // t3, t0
    FetchSinCos(angles[1], sc, sy, cy);                     // t6, t1; t4 = -sy
    const int32_t nsy = Neg32(sy);
    FetchSinCos(angles[2], sc, sz, cz);                     // t5, t2
    m[2] = Lo16(sy);                                        // 0x8004D378
    m[5] = Lo16(Neg32(Mul32(cy, sx)) >> 12);                // 0x8004D38C
    m[8] = Lo16(P12(cy, cx));                               // 0x8004D3A0 / 0x8004D3E0
    m[0] = Lo16(P12(cz, cy));                               // 0x8004D418
    m[1] = Lo16(Neg32(Mul32(sz, cy)) >> 12);                // 0x8004D438
    int32_t t8 = P12(cz, nsy);
    m[3] = Lo16(Sub32(P12(sz, cx), P12(t8, sx)));           // 0x8004D480
    m[6] = Lo16(Add32(P12(sz, sx), P12(t8, cx)));           // 0x8004D4B0
    t8 = P12(sz, nsy);
    m[4] = Lo16(Add32(P12(cz, cx), P12(t8, sx)));           // 0x8004D4F8
    m[7] = Lo16(Sub32(P12(cz, sx), P12(t8, cx)));           // 0x8004D524
}

void MulMatrix0(const int16_t a[9], const int16_t b[9], int16_t out[9]) {
    const int16_t r[9] = {a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8]};
    for (int j = 0; j < 3; ++j) {
        const int16_t v[3] = {b[j], b[3 + j], b[6 + j]};
        int16_t col[3];
        for (int i = 0; i < 3; ++i) {
            const int64_t acc = static_cast<int64_t>(r[3 * i]) * v[0] +
                                static_cast<int64_t>(r[3 * i + 1]) * v[1] +
                                static_cast<int64_t>(r[3 * i + 2]) * v[2];
            col[i] = static_cast<int16_t>(Sat16(acc >> 12));
        }
        out[j] = col[0];
        out[3 + j] = col[1];
        out[6 + j] = col[2];
    }
}

void OuterProduct(const int16_t d[3], const int16_t ir[3], int16_t out[3]) {
    const int64_t a1 = static_cast<int64_t>(d[1]) * ir[2] - static_cast<int64_t>(d[2]) * ir[1];
    const int64_t a2 = static_cast<int64_t>(d[2]) * ir[0] - static_cast<int64_t>(d[0]) * ir[2];
    const int64_t a3 = static_cast<int64_t>(d[0]) * ir[1] - static_cast<int64_t>(d[1]) * ir[0];
    out[0] = static_cast<int16_t>(Sat16(a1 >> 12));
    out[1] = static_cast<int16_t>(Sat16(a2 >> 12));
    out[2] = static_cast<int16_t>(Sat16(a3 >> 12));
}

int32_t Normalize32(int32_t v[3], const uint16_t* rsqrt) {
    int32_t x = v[0], y = v[1], z = v[2];
    const int32_t ay = AbsIdiom(y), ax = AbsIdiom(x);
    int32_t m = (ay < ax) ? ax : ay;                        // 0x8002E184
    const int32_t az = AbsIdiom(z);
    if (az < m) {} else m = az;                             // 0x8002E1AC: `slt v0,v1,a0`
    const int32_t s = (m == 0) ? 0 : 31 - static_cast<int32_t>(Lzcr(m));
    if (!(s < 21)) {                                        // 0x8002E1F4
        const int sh = (s - 20) & 31;
        x >>= sh; y >>= sh; z >>= sh;
    } else {
        const int sh = (20 - s) & 31;
        x = Shl(x, sh); y = Shl(y, sh); z = Shl(z, sh);
    }
    const int32_t n = Add32(MulHi16(z, z), Add32(MulHi16(y, y), MulHi16(x, x)));
    if (n <= 0) return n;                                   // 0x8002E2A0: v is left untouched
    int32_t t2 = 22 - static_cast<int32_t>(Lzcr(n) & ~1u);
    if (!(t2 > 0)) t2 = 0;
    const int32_t index = n >> t2;
    const int32_t half = t2 >> 1;
    const uint32_t w = rsqrt[index];
    const int32_t f = Shl(static_cast<int32_t>(w >> 5), static_cast<int>(w & 0x1Fu));
    const int32_t g = f >> (half & 31);
    v[0] = MulHi16(x, g);
    v[1] = MulHi16(y, g);
    v[2] = MulHi16(z, g);
    return f;                                               // 0x8002E37C: `move v0,t4`
}

bool ReleaseContact(EntityView e, const ContactRefs& refs) {
    if (e.U32(0x340) == 0) return true;                     // 0x8002078C
    if (refs.contact == nullptr) return false;
    const uint32_t handle = U16(refs.contact);
    const uint32_t pool = handle >> 5;
    if (pool == 3) {                                        // 0x80020838
        if ((e.U32(ent::kFlagsB) & 0x60000u) != 0x40000u) return true;
        if (refs.traffic == nullptr) return false;
        const int16_t th[3] = {S16(refs.traffic + 450), S16(refs.traffic + 452), S16(refs.traffic + 454)};
        int16_t eh[3];
        Vec16(e, ent::kHeading, eh);
        const int32_t d = DotLcm(th, eh);
        const int32_t add = FixMul(S32(refs.traffic + 480), d);
        int32_t v1 = Add32(W(e, ent::kSpeedCopy), add);
        SetW(e, ent::kSpeedCopy, v1);
        // `slt a0,v1,0x23C36; beqz a0,...` at 0x8002088C: 0x23C36 is a FLOOR, not a ceiling -
        // a bike let go by a car leaves at no less than 2.24 units of speed.
        if (v1 < 0x23C36) v1 = 0x23C36;
        SetW(e, ent::kSpeedCopy, v1);
        SetW(e, ent::kSpeed, v1);
        int32_t vel[3];
        Scale(v1, eh, vel);
        SetVec32(e, 0x1C8, vel);
        return true;
    }
    if (pool < 4) {
        if (pool != 0) return true;                         // pools 1 and 2: nothing
        if (refs.bike == nullptr) return false;
        PutS32(refs.bike + 560, static_cast<int32_t>(static_cast<uint32_t>(S32(refs.bike + 560)) & 0xFDFFFFFFu));
        return true;
    }
    if (pool != 4) return true;
    if (refs.prop == nullptr) return false;
    const uint32_t kind = ((static_cast<uint32_t>(refs.propKindWord) & 0xF80u) >> 7) - 3u;
    if (!(kind < 3u)) return true;
    if ((S32(refs.prop + 592) & 0x200) == 0) return true;
    PutS32(refs.prop + 556, 0x10000);
    return true;
}

// ============================================================================ RASHCDG 0x80071D24

bool BikeCrashLaunch(EntityView e, const BikeLinks& L, const BikeTables& t) {
    if (L.owner == nullptr) return false;
    if (t.sincos == nullptr || t.asin == nullptr || t.atan == nullptr) return false;
    const int32_t speed = W(e, ent::kSpeed);                // 0x80071D44, before the stores
    for (uint32_t off : {0x290u, 0x304u, 0x26Cu, 0x270u, 0x280u, 0x2ACu, 0x248u, 0x2E8u, 0x300u,
                         0x260u, 0x2A8u})
        SetW(e, off, 0);
    int16_t heading[3];
    Vec16(e, ent::kHeading, heading);
    int32_t vel[3];
    Scale(speed, heading, vel);                             // 0x80071D74
    SetVec32(e, 0x1C8, vel);

    if ((e.U32(ent::kFlagsC) & 0x400u) != 0) {
        // ---- thrown: read the Euler angles back out of the orientation
        int32_t s2 = 0;
        // `negu a0,a0` then `sll a0,a0,4` in the delay slot of 0x80071D9C.
        if (!Asin(Shl(Neg32(e.S16(0x1BE)), 4), t.asin, s2)) return false;
        int32_t s0, a0;
        const uint16_t cosv = static_cast<uint16_t>(t.sincos[2u * (static_cast<uint32_t>(s2) & 0xFFFu) + 1u]);
        if (cosv != 0) {                                    // 0x80071DC8
            s0 = RatAtan2(Shl4(e.S16(0x1BC)), Shl4(e.S16(0x1C0)), t.atan);
            a0 = RatAtan2(Shl4(e.S16(0x1B2)), Shl4(e.S16(0x1B8)), t.atan);
        } else {
            s0 = RatAtan2(Shl4(e.S16(0x210)), Shl4(e.S16(0x214)), t.atan);
            a0 = Mul652Sra16(W(e, 0x27C));
        }
        if (!(AbsIdiom(a0) < 1025)) {                       // 0x80071E4C
            const int32_t base = (s2 >= 0) ? 2048 : -2048;
            s2 = Sub32(base, s2);
            s0 = (s0 > 0) ? s0 - 2048 : s0 + 2048;
            a0 = (a0 > 0) ? a0 - 2048 : a0 + 2048;
        }
        SetW(e, 0x268, Mul25736Sra8(s2));
        SetW(e, 0x2B4, Mul25736Sra8(s0));
        SetW(e, 0x27C, Mul25736Sra8(a0));
        const int32_t ah = AbsIdiom(Shl4(e.S16(0x1C4)));
        int32_t lift = 0x1E0000;                            // delay slot of 0x80071F34
        if (!(0xB4FC < ah)) lift = Add32(FixMul(ah, 0x279A9D), 0x20000);
        const int32_t vy = W(e, 0x1CC);
        const int32_t sq = FixMul(vy, vy);
        const int32_t v1 = SignedDiv(sq, lift);
        SetW(e, 0x1E4, v1);                                 // 0x80071FF8
        if (!(0x9D086 < v1)) SetW(e, 0x1E4, 0x9D087);
    } else {
        // ---- tumbling: rotate the orientation by the lean and the steer
        const int32_t lean = W(e, 0x268);
        SetW(e, 0x25C, 0);
        SetW(e, 0x2F4, 0);
        int16_t ang[3];
        ang[1] = 0;
        ang[0] = Lo16(Neg32(Mul652Sra16(lean)));
        ang[2] = Lo16(Neg32(Mul652Sra16(W(e, 0x28C))));
        int16_t rot[9], b[9], out[9];
        RotMatrix(ang, rot, t.sincos);                      // 0x80072074
        for (uint32_t i = 0; i < 9; ++i) b[i] = e.S16(0x204 + 2u * i);
        MulMatrix0(rot, b, out);                            // 0x80072084
        for (uint32_t i = 0; i < 9; ++i) SetH(e, 0x1B0 + 2u * i, out[i]);

        int16_t row2[3], w[3];
        Vec16(e, 0x1BC, row2);
        Vec16(e, 0x31C, w);
        const int32_t s2 = DotLcm(row2, w);                 // 0x80072098
        if (0xF0A3 < AbsIdiom(s2)) {
            SetH(e, 0x322, e.S16(0x1B6));
            SetH(e, 0x324, e.S16(0x1B8));
            SetH(e, 0x326, e.S16(0x1BA));
        } else {
            int16_t axis[3];
            OuterProduct(row2, w, axis);                    // 0x80072114
            // The original normalises with the TRAPPING `add`; an axis whose squares overflow it
            // is a console exception, not a value. The table is built at run time on the heap
            // (`*(0x8005B560)`), so a caller that does not have it passes null and is refused.
            if (t.rsqrt == nullptr || !Normalize(axis, t.rsqrt)) return false;
            SetVec16(e, 0x322, axis);
        }
        int16_t d[3], ir[3], o[3];
        Vec16(e, 0x31C, d);
        Vec16(e, 0x322, ir);
        OuterProduct(d, ir, o);                             // 0x8007217C
        SetVec16(e, 0x328, o);
        int32_t a = 0;
        if (!Asin(s2, t.asin, a)) return false;             // 0x8007219C
        SetW(e, 0x2B4, Mul25736Sra8(a));
        SetW(e, 0x268, 0);
        const uint16_t c0 = e.U16(0x328), c1 = e.U16(0x32A), c2 = e.U16(0x32C);
        const uint16_t c3 = e.U16(0x322), c4 = e.U16(0x324), c5 = e.U16(0x326);
        e.SetU16(0x26C, c0); e.SetU16(0x26E, c1); e.SetU16(0x270, c2);
        e.SetU16(0x274, c3); e.SetU16(0x276, c4); e.SetU16(0x278, c5);
        int16_t row1[3], ax2[3];
        Vec16(e, 0x1B6, row1);
        Vec16(e, 0x322, ax2);
        int32_t b2 = 0;
        if (!Asin(DotLcm(row1, ax2), t.asin, b2)) return false;
        SetW(e, 0x2C4, Mul25736Sra8(Sub32(1024, b2)));      // 0x8007224C
        auto markOwner = [](uint8_t* o) {
            uint32_t f = static_cast<uint32_t>(S32(o + 552));
            if (static_cast<uint32_t>(S32(o + 604)) < 2u) f |= 0x18000u;
            PutS32(o + 552, static_cast<int32_t>(f));
        };
        markOwner(L.owner);
        if ((L.owner[572] & 0x10u) != 0) {                  // 0x80072278
            // `lw a0,852(v0)` through the RIDER pointer - with no rider that is guest 0x354, the
            // kernel's area. The port does not follow it.
            if (e.U32(ent::kRider) == 0 || L.riderOwner == nullptr) return false;
            markOwner(L.riderOwner);
        }
        SetW(e, 0x2D4, 0);
    }

    // ---- 0x800722C0: the tumble centre, below the box centre along axis 1
    for (uint32_t i = 0; i < 3; ++i)
        SetW(e, 0x310 + 4u * i, Add32(MulHi16(Shl4(e.S16(0x1B6 + 2u * i)), -22282), W(e, 0xB8 + 4u * i)));
    if (e.U32(ent::kRider) != 0 && e.U32(0x440) != 0) {    // 0x80072368
        int32_t c[3], outc[3];
        int16_t axis0[3];
        Vec32(e, 0x310, c);
        Vec16(e, ent::kAxes, axis0);
        MulAdd(c, axis0, Shl(W(e, ent::kHalfX), 1), outc);
        SetVec32(e, 0x310, outc);
    }
    e.SetU32(ent::kFlagsC, e.U32(ent::kFlagsC) & 0xEFC7F7FFu);
    if (!ReleaseContact(e, L.contact)) return false;        // 0x800723B0
    e.SetU32(ent::kFlagsB, e.U32(ent::kFlagsB) & 0xE7CE0383u);
    PutS32(L.owner + 552, static_cast<int32_t>(static_cast<uint32_t>(S32(L.owner + 552)) & ~0x2000u));
    return true;
}

// ============================================================================ RASHCDG 0x800723FC

bool BikeWipeoutStart(EntityView e, const BikeLinks& L) {
    const uint32_t flagsC0 = e.U32(ent::kFlagsC);
    SetW(e, 0x3A4, 0);
    SetW(e, 0x2E8, 0);
    e.SetU32(ent::kFlagsB, e.U32(ent::kFlagsB) & 0xC0018200u);
    constexpr int32_t kHalfTurn = 0x1921F;                  // pi/2 in 16.16... as the game writes it
    constexpr int32_t kNegHalfTurn = static_cast<int32_t>(0xFFFE6DE1u);

    if ((flagsC0 & 0x40u) != 0) {
        // ---- a spin
        SetW(e, 0x2D4, 0);
        SetW(e, 0x2AC, 0);
        SetW(e, 0x298, W(e, 0x28C));
        int32_t s1;
        if ((e.U32(ent::kFlagsC) & 0x18000u) != 0) {        // 0x80072454
            s1 = 0x1AAAA;
            SetW(e, 0x248, 0);
            int32_t v1 = static_cast<int32_t>(static_cast<uint32_t>(L.wipeoutRateByte) << 10);
            SetW(e, 0x290, v1);
            if (v1 < 13107) v1 = 13107;
            SetW(e, 0x290, v1);
            const uint32_t bit15 = (e.U32(ent::kFlagsC) >> 15) & 1u;
            const int32_t e28c = W(e, 0x28C);
            const bool flip = bit15 ? (e28c > 0) : (e28c < 0);
            if (flip) {                                     // 0x800724CC
                int32_t r = W(e, 0x290);
                if (e28c < 0) r = Neg32(r);
                SetW(e, 0x290, r);
            } else {                                        // 0x800724E4
                const int32_t v0 = (e28c > 0) ? Neg32(W(e, 0x290)) : 0;
                const int32_t a2 = W(e, 0x28C);
                int32_t a0 = v0;
                if (a2 < 0) a0 = Add32(v0, W(e, 0x290));
                int32_t a1 = a0;
                if (a2 == 0) {                              // 0x80072518
                    const int32_t r = W(e, 0x290);
                    const int32_t mask = Neg32(static_cast<int32_t>(bit15));
                    const int32_t na = Neg32(r);
                    a1 = Add32(a1, Add32(na, mask & Sub32(r, na)));
                }
                SetW(e, 0x290, a1);
            }
            SetW(e, 0x294, (W(e, 0x290) >= 0) ? 0x10000 : static_cast<int32_t>(0xFFFF0000u));
            SetW(e, 0x2A8, 0);
            SetW(e, 0x2B0, (W(e, 0x28C) > 0) ? kHalfTurn : kNegHalfTurn);
        } else {
            s1 = 0x35555;
            const int32_t v1 = (W(e, 0x28C) > 0) ? kHalfTurn : kNegHalfTurn;
            SetW(e, 0x294, v1);
            SetW(e, 0x294, FixMul(Shl(Sub32(v1, W(e, 0x28C)), 1), 0xB1C71));
            SetW(e, 0x290, 0);
            if ((e.U32(ent::kFlagsC) & 0x20000u) != 0) {    // 0x800725C8
                const int32_t v = (W(e, 0x28C) > 0) ? kHalfTurn : kNegHalfTurn;
                SetW(e, 0x2B0, v);
                SetW(e, 0x2A8, FixMul(Sub32(v, W(e, 0x2A4)), 0x35555));
            }
        }
        SetW(e, 0x248, Neg32(FixMul(W(e, 0x1E8), s1)));     // 0x8007260C
        SetW(e, 0x26C, Neg32(FixMul(W(e, 0x268), s1)));
    } else if ((flagsC0 & 0x20u) != 0) {
        // ---- a high-side
        int32_t v1 = 0;
        if (e.U32(ent::kRider) != 0) v1 = (0 < W(e, 0x2A4)) ? 1 : 0;
        const int32_t sel = Add32(Neg32(v1) & Add32(W(e, 0x2A4), static_cast<int32_t>(0xFFFEC73Du)),
                                  kHalfTurn);
        int32_t a0 = Neg32(sel);
        const int32_t a3 = a0;
        SetW(e, 0x2B0, sel);
        if (W(e, 0x28C) > 0) a0 = sel;
        const int32_t a2 = W(e, 0x2A4);
        int32_t a1 = (a2 > 0) ? sel : a0;
        if (a2 < 0) a1 = Add32(a1, Sub32(a3, a0));
        const int32_t old2a8 = W(e, 0x2A8);
        SetW(e, 0x2B0, a1);
        int32_t r = ((old2a8 ^ a1) < 0) ? Neg32(old2a8) : old2a8;
        const int32_t t1 = (0 < W(e, 0x2B0)) ? 1 : 0;
        SetW(e, 0x2A8, r);
        if (r == 0) r = Shl(Sub32(Shl(t1, 18), 0x20000), 2);
        else r = Shl(r, 2);
        SetW(e, 0x2A8, r);
        // At least 2.0 in magnitude, keeping the sign (0x80072704..0x80072748).
        int32_t rate;
        if (r >= 0) rate = (r < 0x20000) ? 0x20000 : r;
        else rate = (-0x20000 < r) ? -0x20000 : r;
        const int32_t e2a4 = W(e, 0x2A4);
        const int32_t lo = Sub32(kNegHalfTurn, e2a4);
        int32_t c = Add32(e2a4, (Add32(e2a4, kHalfTurn) >> 31) & lo);
        const int32_t hi = Sub32(kHalfTurn, e2a4);
        c = Add32(c, (hi >> 31) & hi);
        SetW(e, 0x2A8, rate);
        SetW(e, 0x2A4, c);
        const int32_t t0 = (0x1921E < AbsIdiom(c)) ? 0 : rate;
        SetW(e, 0x2A8, t0);
        SetW(e, 0x2AC, 0);
        const int32_t steer = W(e, 0x27C);
        const bool force = (steer == 0) || (e.U32(ent::kRider) != 0 && steer > 0);
        if (force) SetW(e, 0x27C, t1 ? 65 : -65);
        e.SetU32(ent::kFlagsC, e.U32(ent::kFlagsC) & 0xFFFDFFFFu);
    } else if ((flagsC0 & 0x80u) != 0) {
        // ---- a slide
        if (L.owner == nullptr) return false;
        SetW(e, 0x2D4, (0x8000 < AbsIdiom(W(e, 0x27C))) ? 3276 : 0);
        SetW(e, 0x2AC, 0);
        SetW(e, 0x2B0, 0);
        SetW(e, 0x2A8, Neg32(W(e, 0x2A4)));
        if (static_cast<uint32_t>(S32(L.owner + 604)) < 2u) {
            const uint16_t h0 = e.U16(0x1C2), h1 = e.U16(0x1C4), h2 = e.U16(0x1C6);
            L.owner[456] = static_cast<uint8_t>(h0); L.owner[457] = static_cast<uint8_t>(h0 >> 8);
            L.owner[458] = static_cast<uint8_t>(h1); L.owner[459] = static_cast<uint8_t>(h1 >> 8);
            PutS32(L.owner + 552, static_cast<int32_t>(static_cast<uint32_t>(S32(L.owner + 552)) | 0x208000u));
            L.owner[460] = static_cast<uint8_t>(h2); L.owner[461] = static_cast<uint8_t>(h2 >> 8);
        }
    } else if ((flagsC0 & 0x100u) != 0) {
        // ---- a stop
        if (L.slice == nullptr) return false;
        SetW(e, 0x2AC, 0);
        int16_t hv[3];
        Vec16(e, 0x210, hv);
        const int16_t lat[3] = {S16(L.slice + 2), S16(L.slice + 4), S16(L.slice + 6)};
        const int32_t d = DotLcm(hv, lat);                  // 0x800728B4
        int32_t v1;
        if ((e.U32(ent::kFlagsC) & 0x400000u) != 0) v1 = (Neg32(d) > 0) ? -16384 : 16384;
        else v1 = (d > 0) ? -16384 : 16384;
        SetW(e, 0x2A8, v1);
        SetW(e, 0x268, 0);
        SetW(e, 0x26C, 0);
        SetW(e, 0x270, 0);
        SetW(e, 0x28C, (W(e, 0x28C) > 0) ? kHalfTurn : kNegHalfTurn);
        SetW(e, 0x290, 0);
        SetW(e, 0x294, 0);
        SetW(e, 0x280, 0);
        SetW(e, 0x1E8, 0);
        SetW(e, 0x248, 0);
        if (e.U32(ent::kRider) != 0) {
            if (L.rider == nullptr) return false;
            PutS32(L.rider + 0x1E8, 0);
        }
        SetW(e, 0x2C0, 0);
        SetW(e, 0x25C, 0);
        SetW(e, 0x2D4, 0x30000);
        int16_t heading[3];
        Vec16(e, ent::kHeading, heading);
        int32_t vel[3];
        Scale(W(e, ent::kSpeed), heading, vel);
        SetVec32(e, 0x1C8, vel);
        e.SetU32(ent::kFlagsC, e.U32(ent::kFlagsC) & 0xEFFFFFFFu);
    }
    e.SetU32(ent::kFlagsC, e.U32(ent::kFlagsC) & ~0x800u);  // 0x80072970
    return true;
}

// ============================================================================ RASHCDG 0x8007F0BC

bool BikeIntegrate(EntityView e, int32_t dt, uint32_t self, const BikeLinks& L, const BikeTables& t,
                   BikeActiveList& list) {
    if (t.sincos == nullptr) return false;
    const uint32_t fc = e.U32(ent::kFlagsC);
    const bool crashing = (fc & 0x600u) != 0;
    const int32_t moving = crashing ? W(e, ent::kSpeed) : W(e, ent::kSpeedCopy);
    if (moving == 0 && (e.U32(ent::kFlagsB) & 0x8000u) == 0 && (fc & 0xFu) == 0 && W(e, 0x1E8) == 0 &&
        W(e, 0x2A4) == 0)
        return true;                                        // 0x8007F13C: not even listed

    auto push = [&]() -> bool {                             // 0x8007FA14
        if (list.slot == nullptr) return false;
        PutS32(list.slot, static_cast<int32_t>(self));
        list.cursor += 4u;
        list.pushed = true;
        return true;
    };

    if (crashing) {
        if ((e.U32(ent::kFlagsC) & 0x800u) != 0) {          // 0x8007F154
            if (!BikeCrashLaunch(e, L, t)) return false;
            e.SetU32(ent::kFlagsC, e.U32(ent::kFlagsC) | 0x08000000u);
            e.SetU32(ent::kFlagsB, e.U32(ent::kFlagsB) | 0x00800000u);
        }
        int32_t c[3], v[3], o[3];
        Vec32(e, 0x310, c);
        Vec32(e, 0x1C8, v);
        MulAdd32(c, v, dt, o);                              // 0x8007F198
        SetVec32(e, 0x310, o);
        for (uint32_t i = 0; i < 3; ++i)
            SetW(e, 0xB8 + 4u * i, Add32(MulHi16(Shl4(e.S16(0x1B6 + 2u * i)), 22282), W(e, 0x310 + 4u * i)));
        if (e.U32(ent::kRider) != 0 && e.U32(0x440) != 0) {
            int32_t b[3], ob[3];
            int16_t axis0[3];
            Vec32(e, 0xB8, b);
            Vec16(e, ent::kAxes, axis0);
            MulAdd(b, axis0, Neg32(Shl(W(e, ent::kHalfX), 1)), ob);
            SetVec32(e, 0xB8, ob);
        }
        return push();
    }

    // ---- 0x8007F284: on the road
    const int32_t speedCopy = W(e, ent::kSpeedCopy);
    for (uint32_t i = 0; i < 3; ++i)
        SetW(e, 0x31C + 4u * i, Sub32(W(e, 0xB8 + 4u * i), W(e, 0x1F8 + 4u * i)));
    for (uint32_t i = 0; i < 3; ++i)
        SetW(e, 0x1C8 + 4u * i, MulHi16(speedCopy, Shl4(e.S16(ent::kHeading + 2u * i))));
    {
        const uint32_t f = e.U32(ent::kFlagsC);
        if ((f & 0x800u) != 0 && (f & 0x1E0u) != 0) {       // 0x8007F348
            if (!BikeWipeoutStart(e, L)) return false;
            e.SetU32(ent::kFlagsC, e.U32(ent::kFlagsC) | 0x08000000u);
        }
    }
    const uint32_t fb = e.U32(ent::kFlagsB);
    const bool riding = ((fb >> 18) & 1u) != 0;             // s3
    if ((e.U32(ent::kFlagsC) & 0xFu) != 0 || (fb & 8u) != 0) {
        int32_t c[3], v[3], o[3];
        Vec32(e, 0x310, c);
        Vec32(e, 0x1C8, v);
        MulAdd32(c, v, dt, o);                              // 0x8007F3A8
        SetVec32(e, 0x310, o);
        int16_t hv[3];
        Vec16(e, 0x210, hv);
        int32_t p[3];
        MulAdd(o, hv, Neg32(W(e, ent::kHalfY)), p);         // 0x8007F3C0
        SetVec32(e, 0x1F8, p);
    } else if (riding) {
        const uint8_t* ct = L.contact.contact;
        if (e.U32(0x340) == 0 || ct == nullptr) return false;
        auto along = [&](uint32_t off) {
            const int32_t a = MulHi16(W(e, 0x1C8), Shl4(S16(ct + off)));
            const int32_t b = MulHi16(W(e, 0x1CC), Shl4(S16(ct + off + 2)));
            const int32_t c = MulHi16(W(e, 0x1D0), Shl4(S16(ct + off + 4)));
            return Add32(c, Add32(b, a));
        };
        SetW(e, 0x308, Add32(W(e, 0x308), FixMul(along(260), dt)));
        SetW(e, 0x30C, Add32(W(e, 0x30C), FixMul(along(272), dt)));
    } else {
        for (uint32_t i = 0; i < 3; ++i)                    // 0x8007F520
            SetW(e, 0x1F8 + 4u * i, Add32(MulHi16(W(e, 0x1C8 + 4u * i), dt), W(e, 0x1F8 + 4u * i)));
    }

    // ---- 0x8007F5BC
    const uint32_t f2 = e.U32(ent::kFlagsC);
    const bool bit14 = ((f2 >> 14) & 1u) != 0;              // a3
    for (uint32_t i = 0; i < 3; ++i)
        SetW(e, 0xB8 + 4u * i, Add32(W(e, 0x1F8 + 4u * i), W(e, 0x31C + 4u * i)));
    bool lean = false;                                      // t0
    if (((f2 & 0x1Fu) != 0 || (e.U32(ent::kFlagsB) & 0x3400u) != 0) && !bit14) lean = true;
    bool turn = false;                                      // a2
    const uint32_t f3 = e.U32(ent::kFlagsC);
    if ((f3 & 0x600u) == 0 && W(e, ent::kSpeedCopy) != 0) turn = true;
    else if (bit14 || riding || W(e, 0x1E8) != 0 || W(e, 0x2A4) != 0) turn = true;
    int32_t step = dt;                                      // a0
    if ((e.U32(ent::kFlagsC) & 0xFu) != 0 && !bit14) {
        const int32_t tnew = Add32(W(e, 0x2D4), dt);
        const int32_t span = W(e, 0x2D8);
        SetW(e, 0x2D4, tnew);
        if (span < tnew) step = Add32(dt, Sub32(span, tnew));
    }
    if (lean) {
        int32_t rate = W(e, 0x2E8);
        bool go = true;
        if (rate == 0) {
            rate = W(e, 0x1E8);
            if (rate == 0) go = false;
        }
        if (go) {
            const uint32_t i = AngleIndex(MulHi16(rate, step));
            const int32_t cs = Shl4(t.sincos[2u * i + 1u]);
            const int32_t sn = Shl4(t.sincos[2u * i]);
            int16_t a[3], b[3], o[3];
            Vec16(e, 0x210, a);
            Vec16(e, 0x204, b);
            Blend16(a, b, o, cs, sn);                       // 0x8007F74C
            SetVec16(e, 0x210, o);
        }
    } else if (turn) {
        int32_t keep = 0;                                   // 0x8007F764
        if (W(e, 0x24C) > 0) keep = 1;
        else if (!(W(e, ent::kSpeedCopy) < 13108)) keep = 1;
        else if ((e.U32(ent::kFlagsA) & 0x300u) != 0) keep = 1;
        SetW(e, ent::kSpeedCopy, Neg32(keep) & W(e, ent::kSpeedCopy));
        if (W(e, 0x1E8) != 0 || W(e, 0x2A4) != 0 || riding) {
            const int32_t yaw = Add32(FixMul(Add32(W(e, 0x1E8), W(e, 0x2E8)), dt), W(e, 0x2A4));
            const uint32_t i = AngleIndex(yaw);
            const int32_t cs = Shl4(t.sincos[2u * i + 1u]);
            const int32_t sn = Shl4(t.sincos[2u * i]);
            int16_t o[3];
            for (uint32_t k = 0; k < 3; ++k) {
                const int32_t a = MulHi16(cs, Shl4(e.S16(ent::kHeading + 2u * k)));
                const int32_t b = MulHi16(sn, Shl4(e.S16(0x32E + 2u * k)));
                o[k] = Lo16(Add32(a, b) >> 4);
            }
            SetVec16(e, 0x210, o);                          // 0x8007F904 / 930 / 964
        }
    }
    // ---- 0x8007F968: the contact point hangs half a box below the heading
    const int32_t down = Neg32(W(e, ent::kHalfY));
    for (uint32_t i = 0; i < 3; ++i)
        SetW(e, 0x1F8 + 4u * i, Add32(MulHi16(Shl4(e.S16(0x210 + 2u * i)), down), W(e, 0x1F8 + 4u * i)));
    return push();
}

// ============================================================================ RASHCDG 0x80075628

bool BikeObstacleTest(EntityView e, const int16_t n[3], int32_t depth, const BikeLinks& L,
                      const BikeTables& t, int32_t& result) {
    result = 0;
    if (t.sincos == nullptr || t.asin == nullptr || t.sqrt == nullptr) return false;
    if ((e.U32(ent::kFlagsB) & 0x61D800u) != 0) return true;
    if ((e.U32(ent::kFlagsC) & 0x7FFu) != 0) return true;
    bool ground = (e.U32(0x184) & 1u) != 0;                 // s3
    int32_t hit = 0;                                        // s6
    if (0x8000 < depth && 0xB2D0E < W(e, ent::kSpeed)) hit = 1;
    else if (static_cast<int8_t>(e.bytes()[0x216]) == 4) hit = 1;
    if (hit != 0) {
        ground = true;                                      // 0x800756D0
    } else if (ground) {
        int16_t hd[3];
        Vec16(e, ent::kHeading, hd);
        const int32_t s0 = DotLcm(n, hd);                   // 0x800756E0
        if (!(s0 > 0)) {
            const int32_t v = MulHi16(s0, Shl4(n[1]));      // FixMul
            const int32_t vv = Shl(v, 12) >> 16;
            const int32_t lifted = Shl(Sub32(e.S16(0x1C4), vv), 4);
            int32_t ang[2];
            for (int k = 0; k < 2; ++k) {
                const int32_t arg = (k == 0) ? Shl4(e.S16(0x1C4)) : lifted;
                if (!Asin(arg, t.asin, ang[k])) return false; // inlined at 0x8007573C
            }
            if (L.stats == nullptr) return false;
            hit = (Sub32(ang[1], ang[0]) < S32(L.stats + 440)) ? 0 : 1;
        }
    } else {
        const uint32_t ob = e.U32(0x33C);
        if (ob != 0) {                                      // 0x80075834
            if (L.slice == nullptr || L.obstacle == nullptr) return false;
            const int16_t row2[3] = {S16(L.slice + 14), S16(L.slice + 16), S16(L.slice + 18)};
            int16_t hd[3];
            Vec16(e, ent::kHeading, hd);
            const int32_t s0 = DotLcm(row2, hd);
            const int32_t a1 = S16(L.obstacle + 50);
            if (!((a1 ^ W(e, 0x16C)) < 0)) {
                if (AbsIdiom(a1) < e.S16(0x1E2)) hit = (0xB4FD < AbsIdiom(s0)) ? 1 : 0;
            }
        }
    }
    result = hit;
    if (hit == 0) return true;                              // 0x8007589C
    e.SetU32(ent::kFlagsC, e.U32(ent::kFlagsC) | 0xC00u);
    if (ground) return true;

    // ---- 0x800758B8: ride up the slope instead of into it
    int32_t s0 = Shl4(e.S16(0x1C4));
    const int32_t sp = W(e, ent::kSpeed);
    if (!(0x35A510 < sp)) {
        int32_t s1 = 22;
        if (0x23C360 < sp) s1 = FixMul(0x13AF5, Sub32(0x35A511, sp)) >> 16;
        int32_t a = 0;
        if (!Asin(s0, t.asin, a)) return false;
        const uint32_t i = static_cast<uint32_t>(Sub32(Add32(s1, 1024), a)) & 0xFFFu;
        s0 = Shl4(t.sincos[2u * i + 1u]);
    }
    s0 = static_cast<int32_t>(static_cast<uint32_t>(s0) >> 4);   // 0x80075940: `srl`
    const int32_t sc = W(e, ent::kSpeedCopy);
    if (0x1BAFFFE < sc) {
        int32_t v;
        // Both arms as the original has them: 0x80075968 negates, 0x8007597C does not. Only the
        // first is reachable, because `sc` has just been tested above 0x1BAFFFE.
        if (sc > 0) v = Neg32(static_cast<int32_t>(FixDiv(0x1BAFFFEu, static_cast<uint32_t>(sc))));
        else v = static_cast<int32_t>(FixDiv(0x1BAFFFEu, static_cast<uint32_t>(Neg32(sc))));
        int32_t a0 = static_cast<int32_t>(static_cast<uint32_t>(v) >> 4);
        if (Shl(a0, 16) < Shl(s0, 16)) a0 = s0;
        s0 = a0;
    }
    const int32_t want = Shl(s0, 16) >> 16;                 // 0x800759A8
    const int32_t have = e.S16(0x1C4);
    if (!(have < want) && want == have) return true;
    const int32_t s1 = Shl(want, 4);
    SetH(e, 0x1C4, s0);
    int32_t s3 = Shl4(e.S16(0x1C2));
    int32_t s4 = Shl4(e.S16(0x1C6));
    int32_t flat = Add32(FixMul(s3, s3), FixMul(s4, s4));
    if (flat < 6) { s3 = 0; s4 = 655; flat = 6; }
    const int32_t rest = Sub32(0x10000, FixMul(s1, s1));
    const int32_t q = SignedDiv(rest, flat);
    const int32_t r = Shl(SqrtGte(q, t.sqrt), 2);           // 0x80075AA8
    SetH(e, 0x1C2, FixMul(r, s3) >> 4);
    SetH(e, 0x1C6, FixMul(r, s4) >> 4);
    return true;
}

// ============================================================================ RASHCDG 0x8007FA4C

bool BikeContactFrame(EntityView e, const BikeLinks& L, const BikeTables& t, BikeRoadRebind& rebind) {
    if (t.sincos == nullptr || t.asin == nullptr || t.atan == nullptr || t.rsqrt == nullptr ||
        t.sqrt == nullptr)
        return false;
    const bool crashing = (e.U32(ent::kFlagsC) & 0x600u) != 0;  // sp+84
    int32_t s0 = W(e, 0x104);
    e.SetU32(ent::kFlagsB, e.U32(ent::kFlagsB) & 0xFFBFFFFFu);

    auto impact = [&](uint32_t normalOff, int32_t depth, int32_t& v) -> bool {
        const bool ground = (e.U32(0x184) & 1u) != 0;
        int16_t nrm[3];
        Vec16(e, normalOff, nrm);
        if (ground && depth < 0) {
            v = SetImpactDirection(e, nrm, t.rsqrt);
            return true;
        }
        return BikeObstacleTest(e, nrm, depth, L, t, v);
    };

    bool toTail = crashing;                                 // 0x8007FAB4
    if (!toTail) {
        const bool clear = (e.U32(ent::kFlagsC) & 0x80000Fu) == 0;   // s1
        int32_t v = 0;
        if ((e.U32(0x184) & 0x40u) != 0 && clear)
            if (!impact(0x112, W(e, 0x104), v)) return false;
        s0 = 0;                                             // delay slot of 0x8007FB1C
        if (v != 0) toTail = true;
        if (!toTail) {
            int32_t d[3];
            for (uint32_t i = 0; i < 3; ++i) d[i] = Sub32(W(e, 0x118 + 4u * i), W(e, 0xF4 + 4u * i));
            const int32_t depth2 =
                Add32(MulHi16(d[2], Shl4(e.S16(0x110))),
                      Add32(MulHi16(d[1], Shl4(e.S16(0x10E))), MulHi16(d[0], Shl4(e.S16(0x10C)))));
            v = 0;
            if ((e.U32(ent::kFlagsB) & 0x2000000u) != 0 && clear)
                if (!impact(0x10C, depth2, v)) return false;
            if (v != 0) toTail = true;
            if (!toTail) {
                // ---- 0x8007FC48: drop the contact onto the averaged normal
                e.SetU16(0x20A, e.U16(0x112));
                e.SetU16(0x20C, e.U16(0x114));
                e.SetU16(0x20E, e.U16(0x116));
                int16_t avg[3];
                for (uint32_t i = 0; i < 3; ++i) avg[i] = HalfSum(e.S16(0x10C + 2u * i), e.S16(0x20A + 2u * i));
                int16_t n2[3];
                Vec16(e, 0x10C, n2);
                const int32_t along = SignedDiv(depth2, DotLcm(avg, n2));
                for (uint32_t i = 0; i < 3; ++i)
                    SetW(e, 0x310 + 4u * i, Add32(MulHi16(Shl4(avg[i]), along), W(e, 0xF4 + 4u * i)));
                int32_t h[3];
                for (uint32_t i = 0; i < 3; ++i) h[i] = Sub32(W(e, 0x310 + 4u * i), W(e, 0x1F8 + 4u * i));
                Normalize32(h, t.rsqrt);                    // inlined at 0x8007FDEC
                for (uint32_t i = 0; i < 3; ++i) SetH(e, 0x210 + 2u * i, h[i] >> 4);
                const int32_t half = W(e, ent::kHalfY);
                for (uint32_t i = 0; i < 3; ++i)
                    SetW(e, 0x1F8 + 4u * i,
                         Add32(MulHi16(Shl4(e.S16(0x210 + 2u * i)), half), W(e, 0x1F8 + 4u * i)));

                // ---- 0x800800C4: the orientation, through the rider's contact when there is one
                const int16_t* diag = nullptr;              // a0 at 0x800803D8
                int16_t diagStore[3];
                if (e.U32(ent::kRider) != 0 && e.U32(0x440) != 0) {
                    if (L.rider == nullptr) return false;
                    EntityView r(L.rider);
                    int16_t ra[3];
                    for (uint32_t i = 0; i < 3; ++i) ra[i] = HalfSum(r.S16(0x32E + 2u * i), e.S16(0x20A + 2u * i));
                    int16_t rn[3];
                    Vec16(r, 0x32E, rn);
                    const int32_t dd = DotLcm(ra, rn);
                    int32_t rd[3];
                    for (uint32_t i = 0; i < 3; ++i) rd[i] = Sub32(W(r, 0xB8 + 4u * i), W(r, 0x310 + 4u * i));
                    const int32_t num = Add32(MulHi16(rd[2], Shl4(rn[2])),
                                              Add32(MulHi16(rd[1], Shl4(rn[1])), MulHi16(rd[0], Shl4(rn[0]))));
                    const int32_t k = SignedDiv(num, dd);
                    int32_t rb[3], ro[3];
                    Vec32(r, 0xB8, rb);
                    MulAdd(rb, ra, Neg32(k), ro);           // 0x80080264
                    SetVec32(r, 0xB8, ro);
                    const int32_t s6 = Sub32(W(r, 0xB8), W(e, 0x1F8));
                    const int32_t s4 = Sub32(W(r, 0xBC), W(e, 0x1FC));
                    const int32_t s3 = Sub32(W(r, 0xC0), W(e, 0x200));
                    const int32_t s5 = Shl4(e.S16(0x212));
                    const int32_t s2 = Shl4(e.S16(0x210));
                    const int32_t s1 = Shl4(e.S16(0x214));
                    int32_t c[3];
                    c[0] = Sub32(FixMul(s5, s3), FixMul(s1, s4));
                    c[1] = Sub32(FixMul(s1, s6), FixMul(s2, s3));
                    c[2] = Sub32(FixMul(s2, s4), FixMul(s5, s6));
                    Normalize32(c, t.rsqrt);                // 0x8008032C
                    if (c[1] < 9841) {
                        Vec16(e, 0x20A, diagStore);         // a0 = e+522
                        diag = diagStore;
                    } else {
                        for (uint32_t i = 0; i < 3; ++i) SetH(e, 0x20A + 2u * i, c[i] >> 4);
                        int16_t dg[3], ir[3], o[3];
                        Vec16(e, 0x20A, dg);
                        Vec16(e, 0x210, ir);
                        OuterProduct(dg, ir, o);            // 0x800803B4
                        SetVec16(e, 0x204, o);
                        diag = nullptr;                     // a0 = 0
                    }
                } else {
                    for (uint32_t i = 0; i < 3; ++i) diagStore[i] = avg[i];  // a0 = sp+72
                    diag = diagStore;
                }
                if (diag != nullptr) {                      // 0x800803E0
                    int16_t ir[3], o[3];
                    Vec16(e, 0x210, ir);
                    OuterProduct(diag, ir, o);
                    // Normalize, inlined at 0x8008043C, with the same trapping `add`.
                    if (!Normalize(o, t.rsqrt)) return false;
                    SetVec16(e, 0x204, o);
                    int16_t d2[3], o2[3];
                    Vec16(e, 0x210, d2);
                    OuterProduct(d2, o, o2);                // 0x80080540
                    SetVec16(e, 0x20A, o2);
                }
                s0 = 0;                                     // 0x80080560
            }
        }
    }

    // ---- 0x80080564: the obstacle flag
    if ((e.U32(ent::kFlagsB) & 0x4000000u) != 0) {
        const uint32_t a0 = e.U32(ent::kFlagsC);
        if ((a0 & 0x400u) != 0) {
            if (e.U32(0x33C) != 0) {
                if (L.obstacle == nullptr) return false;
                const int32_t side = S16(L.obstacle + 50);
                if (((side ^ W(e, 0x16C)) < 0)) e.SetU32(ent::kFlagsC, a0 & 0xDFFFFFFFu);
                else e.SetU32(ent::kFlagsC, a0 | 0x20000000u);
            }
            SetW(e, 0x2C4, ((e.U32(ent::kFlagsC) & 0x20000000u) != 0) ? 6553 : 0);
        }
        SetW(e, 0x33C, 0);
        e.SetU32(ent::kFlagsB, e.U32(ent::kFlagsB) & 0xFBFFFFFFu);
    }

    if (crashing) {                                         // 0x80080604
        int32_t a2 = s0;
        const int32_t v1 = W(e, 0x300);
        if (a2 < v1) a2 = v1;
        SetW(e, 0x300, a2);
        const uint32_t fb = e.U32(ent::kFlagsB) & 0xFF7FFFFFu;
        e.SetU32(ent::kFlagsB, fb);
        int16_t a[3], b[3];
        Vec16(e, 0x20A, a);
        Vec16(e, ent::kHeading, b);
        e.SetU32(ent::kFlagsB, (DotLcm(a, b) < 0) ? (fb | 0x800000u) : fb);
        return true;
    }
    const uint32_t fb = e.U32(ent::kFlagsB);
    if ((fb & 0x400000u) != 0) {                            // 0x80080674: a latched impact
        const int32_t px = W(e, 0x1D4), py = W(e, 0x1D8), pz = W(e, 0x1DC);
        const int32_t flag = static_cast<int32_t>(((e.U32(ent::kFlagsA) >> 27) ^ 1u) & 1u);
        SetW(e, 0xB8, px);
        SetW(e, 0x1F8, Sub32(px, W(e, 0x31C)));
        SetW(e, 0xBC, py);
        SetW(e, 0xC0, pz);
        SetW(e, 0x1FC, Sub32(py, W(e, 0x320)));
        SetW(e, 0x200, Sub32(pz, W(e, 0x324)));
        rebind.Rebind(flag);                                // SLUS 0x800374D4 - the oracle's
        return true;
    }
    if ((fb & 0x40000u) != 0) {                             // 0x800806DC: riding a contact
        const uint8_t* ct = L.contact.contact;
        if (e.U32(0x340) == 0 || ct == nullptr) return false;
        const int16_t a[3] = {S16(ct + 260), S16(ct + 262), S16(ct + 264)};
        const int16_t b[3] = {S16(ct + 272), S16(ct + 274), S16(ct + 276)};
        int32_t o[3];
        Blend16To32(a, b, o, W(e, 0x308), W(e, 0x30C));     // 0x80080700
        SetVec32(e, 0x1F8, o);
        for (uint32_t i = 0; i < 3; ++i) SetW(e, 0x1F8 + 4u * i, Add32(W(e, 0x1F8 + 4u * i), S32(ct + 12 + 4u * i)));
    }
    if ((e.U32(ent::kFlagsC) & 0x600u) != 0) {              // 0x80080750: an obstacle test crashed it
        if (!BikeCrashLaunch(e, L, t)) return false;
        e.SetU32(ent::kFlagsC, e.U32(ent::kFlagsC) | 0x08000000u);
        e.SetU32(ent::kFlagsB, e.U32(ent::kFlagsB) | 0x00800000u);
        return true;
    }
    for (uint32_t i = 0; i < 3; ++i)                        // 0x80080790
        SetW(e, 0xB8 + 4u * i, Add32(W(e, 0x1F8 + 4u * i), W(e, 0x31C + 4u * i)));
    return true;
}

// ============================================================================ RASHCDG 0x8007EF60

int32_t BikeGripLimit(const uint8_t* stats, int32_t a, int32_t b, int32_t c, const int16_t* sqrt) {
    int32_t s2 = FixMul(b, S32(stats + 192));               // 0x8007EF88
    s2 = FixMul(s2, s2);
    int32_t v = FixMul(c, a);                               // 0x8007EFA8
    v = FixMul(v, v);
    const int32_t d = Sub32(v, s2);
    if (d < 0) return 0;                                    // 0x8007EFC0
    const int32_t r = Shl(SqrtGte(d, sqrt), 2);
    return SignedDiv(r, S32(stats + 200));
}

// ============================================================================ RASHCDG 0x80072C7C

bool BikePassengerSteer(EntityView e, int32_t dt, uint8_t* p, const uint8_t* stats, uint32_t numPlayers,
                        const uint8_t* handleTable, uint32_t handleTableBytes, int32_t& out) {
    out = 0;
    if (e.U32(ent::kRider) == 0 || p == nullptr || stats == nullptr) return false;
    if (S32(p + 720) != 0) return true;                     // 0x80072CA8
    uint32_t f = static_cast<uint32_t>(S32(p + 560));
    if (W(e, ent::kSpeedCopy) == 0) f &= 0xFFFFFCFFu;       // 0x80072CC4
    const bool bit20 = ((f >> 20) & 1u) != 0;               // t0
    PutS32(p + 560, static_cast<int32_t>(f));
    bool bounded = false;                                   // t1
    int32_t target = 0;                                     // a3
    if (bit20) {
        bounded = true;
        const uint32_t h = e.U16(ent::kHandle);
        const uint32_t idx = (numPlayers < 2u) ? h + 1u : h + 2u;   // 0x80072CFC
        const uint64_t off = static_cast<uint64_t>(idx) * 8u + 4u;
        if (handleTable == nullptr || off + 4u > handleTableBytes) return false;
        target = S32(handleTable + off);
        const int32_t cur = S32(p + 672);
        const uint32_t side = ((target < cur) ? 256u : 0u) + ((cur < target) ? 512u : 0u);
        uint32_t fresh = 0;
        if (side != 0) fresh = ((static_cast<uint32_t>(S32(p + 560)) & side) == 0) ? 1u : 0u;
        const uint32_t g = static_cast<uint32_t>(S32(p + 560)) | side | (fresh << 7);
        PutS32(p + 560, static_cast<int32_t>(g));
        uint32_t a2 = 768;
        if (side == 512) a2 = 256;
        const uint32_t keep = (side == 256) ? ~(a2 - 256u) : ~a2;
        PutS32(p + 560, static_cast<int32_t>(g & keep));   // 0x80072D98
    }
    const uint32_t fl = static_cast<uint32_t>(S32(p + 560));
    const int32_t rate = S32(stats + 428);
    int32_t v0;
    if ((fl & 0x100u) != 0) {
        v0 = ((fl & 0x200u) != 0) ? 0 : Neg32(rate);
    } else if ((fl & 0x200u) != 0) {
        v0 = rate;
    } else if (bit20) {
        v0 = 0;
    } else {
        bounded = true;                                     // `li t1,1` in a delay slot, 0x80072DFC
        const int32_t cur = S32(p + 672);
        if (cur > 0) v0 = Neg32(rate);
        else if (cur < 0) v0 = rate;
        else v0 = 0;
        v0 = Shl(v0, 1);                                    // 0x80072E1C: back to rest at twice the rate
    }
    const int32_t cur = S32(p + 672);
    const int32_t next = Add32(MulHi16(v0, dt), cur);
    int32_t r;
    if (!bounded) r = next;
    else if (target < cur) r = (target < next) ? next : target;
    else if (cur < target) r = (next < target) ? next : target;
    else r = target;
    // 0x80072E8C..0x80072EB4: the symmetric clamp to +-1.0.
    const int32_t lo = Sub32(static_cast<int32_t>(0xFFFF0000u), r);
    int32_t c = Add32(r, (Add32(r, 0x10000) >> 31) & lo);
    const int32_t hi = Sub32(0x10000, r);
    c = Add32(c, (hi >> 31) & hi);
    PutS32(p + 672, c);
    const int32_t sc = W(e, ent::kSpeedCopy);
    const int32_t mask = (0x8000 < sc) ? -1 : 0;
    out = SignedDiv(FixMul(c, S32(stats + 432)), sc) & mask;
    return true;
}

// ============================================================================ RASHCDG 0x80074C84

bool BikeSteerPass(BikeSteerPassNode* nodes, size_t count, int32_t dt, const BikeSteerPassEnv& env) {
    for (size_t i = 0; i < count; ++i) {
        BikeSteerPassNode& n = nodes[i];
        int32_t extra = 0;
        if ((n.ownerFlagByte & 0x10u) != 0) {               // 0x80074CCC
            if (!BikePassengerSteer(n.bike, dt, n.passenger, n.stats, env.numPlayers, env.handleTable,
                                    env.handleTableBytes, extra))
                return false;
        }
        if ((n.bike.U32(ent::kFlagsA) & 0x80000u) != 0) {   // 0x80074CF4
            const int32_t hasRider = (n.bike.U32(ent::kRider) != 0 && n.bike.U32(0x440) != 0) ? 1 : 0;
            if (n.bike.U32(ent::kRider) != 0 && n.passenger == nullptr) return false;
            EntityView rider(n.passenger);
            BikeApplySteering(n.bike, hasRider, n.passenger ? &rider : nullptr, extra, dt, n.stats,
                              env.atan, env.sincos, n.riderDefByte0);
        }
    }
    return true;
}

} // namespace rr::sim
