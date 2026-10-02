#include "game/sim/crash.h"

#include "game/sim/ai.h"
#include "game/sim/bike.h"
#include "game/sim/fixed.h"
#include "game/sim/vec.h"

#include <utility>

namespace rr::sim {
namespace {

// 32-bit wrapping arithmetic, as the R3000's addu/subu/sll/negu do it.
constexpr uint32_t U(int32_t v) { return static_cast<uint32_t>(v); }
constexpr int32_t S(uint32_t v) { return static_cast<int32_t>(v); }
inline int32_t Add(int32_t a, int32_t b) { return S(U(a) + U(b)); }
inline int32_t Sub(int32_t a, int32_t b) { return S(U(a) - U(b)); }
inline int32_t Neg(int32_t a) { return S(0u - U(a)); }
inline int32_t Shl(int32_t a, int s) { return S(U(a) << s); }
// `mult` / `multu`, low word.
inline int32_t MulLo(int32_t a, int32_t b) {
    return S(static_cast<uint32_t>(static_cast<int64_t>(a) * static_cast<int64_t>(b)));
}
// `multu`, high word.
inline uint32_t MulHiU(uint32_t a, uint32_t b) {
    return static_cast<uint32_t>((static_cast<uint64_t>(a) * static_cast<uint64_t>(b)) >> 32);
}
// The `sra/addu/xor` absolute value: INT32_MIN stays INT32_MIN.
inline int32_t MipsAbs(int32_t x) {
    const uint32_t s = U(x >> 31);
    return S((U(x) + s) ^ s);
}
inline int32_t Div(int32_t a, int32_t b) { return S(FixDiv(U(a), U(b))); }

void Read16x3(GuestRam& g, uint32_t a, int16_t v[3]) {
    for (uint32_t k = 0; k < 3; ++k) v[k] = g.S16(a + 2u * k);
}
void Write16x3(GuestRam& g, uint32_t a, const int16_t v[3]) {
    for (uint32_t k = 0; k < 3; ++k) g.W16(a + 2u * k, static_cast<uint16_t>(v[k]));
}
void Read32x3(GuestRam& g, uint32_t a, int32_t v[3]) {
    for (uint32_t k = 0; k < 3; ++k) v[k] = g.S32(a + 4u * k);
}
void Write32x3(GuestRam& g, uint32_t a, const int32_t v[3]) {
    for (uint32_t k = 0; k < 3; ++k) g.W32(a + 4u * k, U(v[k]));
}

// SLUS 0x8002E698 DotLcm(a, b) over guest addresses.
int32_t GDot(GuestRam& g, uint32_t a, uint32_t b) {
    int16_t x[3], y[3];
    Read16x3(g, a, x);
    Read16x3(g, b, y);
    return DotLcm(x, y);
}
// SLUS 0x8002EE50 Scale(t, dir, out).
void GScale(GuestRam& g, int32_t t, uint32_t dir, uint32_t out) {
    int16_t d[3];
    int32_t o[3];
    Read16x3(g, dir, d);
    Scale(t, d, o);
    Write32x3(g, out, o);
}
// SLUS 0x8002EAD8 MulAdd(base, dir, t, out). Every caller here either keeps `out` apart from `base`
// and `dir` or makes it EQUAL to `base`, and the original reads base[i] before it writes out[i].
void GMulAdd(GuestRam& g, uint32_t base, uint32_t dir, int32_t t, uint32_t out) {
    int32_t b[3], o[3];
    int16_t d[3];
    Read32x3(g, base, b);
    Read16x3(g, dir, d);
    MulAdd(b, d, t, o);
    Write32x3(g, out, o);
}
// SLUS 0x8002E468 Normalize(v) in place. False where the console raises the overflow exception of
// its trapping `add` (vec.h).
bool GNormalize(GuestRam& g, uint32_t a, const uint16_t* rsqrt) {
    int16_t v[3];
    Read16x3(g, a, v);
    if (!Normalize(v, rsqrt)) return false;
    Write16x3(g, a, v);
    return true;
}

// The inline "horizontal rescale" (0x80078F4C..0x80079044 and its copy 0x8007940C..0x80079504):
// keep the vertical component `y` (16.16) and scale the horizontal pair so that the vector is
// unit length. `x`/`z` are replaced by 0 / 655 when their squared length is below 6 (a 16.16 value).
// Returns SqrtGte((1 - y^2) / (x^2 + z^2)) << 2 with the original's four sign arms around the
// unsigned FixDiv, and FixMul(y, y) CALLED AGAIN for the numerator: the FixMul order is x*x, z*z,
// y*y, y*y.
int32_t HorizRescale(int32_t y, int32_t& x, int32_t& z, const int16_t* sqrtTable) {
    const int32_t xx = FixMul(x, x);
    int32_t sum = Add(xx, FixMul(z, z));
    if (sum < 6) {
        x = 0;
        z = 655;
        sum = 6;
    }
    int32_t r;
    if (Sub(0x10000, FixMul(y, y)) > 0) {
        if (sum > 0) r = Div(Sub(0x10000, FixMul(y, y)), sum);                 // 0x80079034
        else r = Neg(Div(Sub(0x10000, FixMul(y, y)), Neg(sum)));               // 0x80079010
    } else {
        if (sum > 0) r = Neg(Div(Sub(FixMul(y, y), 0x10000), sum));            // 0x80079010
        else r = Div(Sub(FixMul(y, y), 0x10000), Neg(sum));                    // 0x80079034
    }
    return Shl(SqrtGte(r, sqrtTable), 2);                                      // 0x8007903C
}

// The engine's sign split around the unsigned FixDiv in the form the roll ramp uses three times:
// n > 0 ? (d > 0 ? pos(n / d) : neg(n / -d)) : (d <= 0 ? pos(-n / -d) : neg(-n / d)), where pos /
// neg say whether the quotient is kept or negated on that arm (`keepSame` = the arm with both
// operands positive keeps it).
int32_t SplitDiv(int32_t n, int32_t d, bool negateWhenSameSign) {
    if (n > 0) {
        if (d > 0) {
            const int32_t q = Div(n, d);
            return negateWhenSameSign ? Neg(q) : q;
        }
        const int32_t q = Div(n, Neg(d));
        return negateWhenSameSign ? q : Neg(q);
    }
    if (d <= 0) {
        const int32_t q = Div(Neg(n), Neg(d));
        return negateWhenSameSign ? Neg(q) : q;
    }
    const int32_t q = Div(Neg(n), d);
    return negateWhenSameSign ? q : Neg(q);
}

// RASHCDG 0x80073D74 BikeSteerLean(e), the PORTED host function (bike.h), run on a host copy of
// the 1096-byte bike. It reads the stat block `*(e+0x22C)` at +0x04, +0xE8, +0x118 and +0x11C, and
// only past its own early-out (steer == 0 or speed <= 0.5), so the words are fetched only then.
bool GSteerLean(GuestRam& g, uint32_t e, const int16_t* sincos) {
    uint8_t bike[1096];
    g.ReadBlock(e, bike, sizeof(bike));
    uint8_t stats[0x120] = {};
    EntityView view(bike);
    const int32_t steer = S(view.U32(ent::kSteer));
    const int32_t speed = S(view.U32(ent::kSpeedCopy));
    if (!(steer == 0 || !(0x8000 < speed))) {
        const uint32_t sp = view.U32(ent::kStats);
        for (uint32_t off : {4u, 232u, 280u, 284u}) {
            const uint32_t w = g.U32(sp + off);
            for (uint32_t k = 0; k < 4; ++k) stats[off + k] = static_cast<uint8_t>(w >> (8u * k));
        }
    }
    if (g.Faulted()) return false;
    BikeSteerLean(view, stats, sincos);
    g.WriteBlock(e, bike, sizeof(bike));
    return true;
}

constexpr int32_t kFloorRequest = S(0xFFFF4D48); // -0.698 (-40 deg)
constexpr int32_t kFloorThrow = S(0xFFFF209A);   // -0.873 (-50 deg)
constexpr int32_t kRollLimit = 0x1921F;          // pi/2: lying on the side
constexpr int32_t kRollLimitNeg = S(0xFFFE6DE1);

// 0x80079218..0x8007967C, the throw. `R` is the rider (s7).
bool Throw(GuestRam& g, uint32_t e, uint32_t R, bool s6, const BikeTables& t) {
    bool tumble = true; // false: the class-4 airborne arm, which jumps straight to 0x80079610
    if (s6) {
        if (g.U32(e + 568) & 4u) {                                              // 0x8007922C
            int32_t a1 = g.S32(e + 480);
            if (a1 < 0x20000) a1 = 0x20000;
            const uint16_t r0 = g.U16(e + 516), r1 = g.U16(e + 518), r2 = g.U16(e + 520);
            g.W32(e + 488, U(a1));                                              // 0x80079258
            g.W16(e + 796, r0);                                                 // 0x8007925C
            g.W16(e + 798, r1);                                                 // 0x80079260
            g.W16(e + 800, r2);                                                 // 0x80079268
            tumble = false;
        }
    } else {
        GMulAdd(g, e + 504, e + 522, Neg(Shl(g.S32(e + 308), 1)), e + 184);     // 0x80079280
        GMulAdd(g, e + 184, e + 528, g.S32(e + 308), e + 184);                  // 0x80079294
    }
    if (tumble) {
        // ---- 0x8007929C: the speed, 0.9 x the RIDER's, at least 10.0
        int32_t v = FixMul(0xE666, g.S32(R + 480));
        g.W32(e + 480, U(v));                                                   // 0x800792B8
        if (v < 0xA0000) v = 0xA0000;
        g.W32(e + 480, U(v));                                                   // 0x800792C4
        // ---- Rand #1 (0x800792C0): the yaw kick +-(142..284)
        const uint32_t r1 = GuestRand(g);
        const uint32_t q1 = MulHiU(r1, 0xE6C2B449u) >> 8;
        const int32_t d = static_cast<int16_t>(static_cast<uint16_t>(r1 - 284u * q1 - 142u));
        const int32_t kick = static_cast<int16_t>(static_cast<uint16_t>(U(d > 0 ? d + 142 : d - 142)));
        const int32_t lo1 = MulLo(kick, g.S16(e + 812));                        // 0x80079318
        const int32_t lo2 = MulLo(kick, g.S16(e + 808));                        // 0x80079328
        g.W16(e + 452, g.U16(e + 810));                                         // 0x80079334
        const int32_t y0 = Shl(g.S16(e + 452), 4);
        g.W16(e + 450, static_cast<uint16_t>(g.U16(e + 808) + U(lo1 >> 12)));   // 0x80079348
        g.W16(e + 454, static_cast<uint16_t>(g.U16(e + 812) - U(lo2 >> 12)));   // 0x80079360
        int32_t a = 0;
        if (!Asin(y0, t.asin, a)) return false;                                 // 0x8007935C
        // ---- cos(110 deg - asin) out of the sine/cosine table, as a u16
        const uint32_t idx = U(1251 - a) & 0xFFFu;
        uint32_t s0 = static_cast<uint16_t>(t.sincos[2u * idx + 1u]);           // 0x80079390
        const int32_t sp = g.S32(e + 480);
        if (0x8DC28 < sp) {                                                     // 8.86
            const uint32_t q = (sp > 0) ? (0u - FixDiv(0x8DC28u, U(sp))) : FixDiv(0x8DC28u, U(Neg(sp)));
            uint32_t a0 = q >> 4;                                               // srl, not sra
            if (S(a0 << 16) < S(s0 << 16)) a0 = s0;                             // a signed 16-bit max
            s0 = a0;
        }
        const int32_t y16 = static_cast<int16_t>(static_cast<uint16_t>(s0));
        if (y16 != g.S16(e + 452)) {                                            // 0x800793F8 / 0x80079404
            int32_t x = Shl(g.S16(e + 450), 4);
            int32_t z = Shl(g.S16(e + 454), 4);
            g.W16(e + 452, static_cast<uint16_t>(s0));                          // 0x80079418
            const int32_t k = HorizRescale(Shl(y16, 4), x, z, t.sqrt);
            g.W16(e + 450, static_cast<uint16_t>(U(FixMul(k, x) >> 4)));        // 0x80079524
            g.W16(e + 454, static_cast<uint16_t>(U(FixMul(k, z) >> 4)));        // 0x8007952C
        }
        int32_t side = g.S32(e + 480);
        if (0xA0000 < side) side = 0xA0000;
        g.W32(e + 488, U(side));                                                // 0x8007954C
        // ---- Rand #2 (0x80079548) and #3 (0x800795AC): the tumble axis
        const uint32_t r2 = GuestRand(g);
        const uint32_t q2 = MulHiU(r2, 0x16F26017u) >> 6;
        const int32_t tilt = static_cast<int16_t>(static_cast<uint16_t>(r2 - 714u * q2 - 357u));
        const int32_t lo3 = MulLo(tilt, g.S16(e + 450));
        g.W16(e + 796, static_cast<uint16_t>(g.U16(e + 454) + U(lo3 >> 12)));   // 0x800795B0
        const uint32_t r3 = GuestRand(g);
        const uint32_t q3 = MulHiU(r3, 0x16F26017u) >> 6;
        const int32_t lo4 = MulLo(tilt, g.S16(e + 454));
        g.W32(e + 728, 0);                                                      // 0x800795C4
        g.W16(e + 798, static_cast<uint16_t>(r3 - 714u * q3 - 357u));           // 0x800795F8
        g.W16(e + 800, static_cast<uint16_t>(U(lo4 >> 12) - g.U16(e + 450)));   // 0x8007960C
        if (!GNormalize(g, e + 796, t.rsqrt)) return false;                     // 0x80079608
    }
    // ---- 0x80079610: bits 9, 11 (-> BikeCrashLaunch) and 18
    const uint32_t fc = (g.U32(e + 568) & 0xFFF00200u) | 0x00040A00u;
    const uint32_t partner = g.U32(e + 856);
    g.W32(e + 568, fc);                                                         // 0x80079634
    if (partner != 0 && g.U32(e + 1088) != 0)
        g.W32(partner + 568, g.U32(partner + 568) | 0x40000u);                  // 0x80079654
    int32_t pitch = g.S32(e + 616);
    if (pitch < kFloorThrow) pitch = kFloorThrow;
    g.W32(e + 616, U(pitch));                                                   // 0x8007967C
    return true;
}

// 0x80079680..0x80079AD4, everything that is not the request or the throw.
bool Fall(GuestRam& g, uint32_t e, uint32_t R, bool s6, bool s8, const BikeTables& t, ImpactSound& snd) {
    if (!s8) {
        if (!s6 && g.S32(e + 620) < 0 && g.S32(e + 616) <= 0) return true;     // 0x80079698 / A8
        if (!s6 && !(g.U32(R + 604) < 2u) && MipsAbs(g.S32(e + 652)) != kRollLimit) {
            // ---- the ROLL RAMP of a riderless bike (0x800796F0)
            if (g.U32(e + 660) != 0) return true;
            if (g.U32(e + 568) & 0x4000u) return true;
            int32_t s3 = 0x8000;
            const int32_t a1 = g.S32(e + 620);
            if (!(a1 < 66)) s3 = SplitDiv(g.S32(e + 624), a1, true);            // 0x80079754 / 6C
            if (s3 < 0x8000) s3 = 0x8000;
            const int32_t target = (g.S32(e + 652) > 0) ? kRollLimit : kRollLimitNeg;
            const uint32_t partner = g.U32(e + 856);
            g.W32(e + 660, U(target));                                          // 0x800797B4
            if (partner != 0 && g.U32(e + 1088) != 0) g.W32(e + 660, U(kRollLimitNeg));   // 0x800797CC
            const int32_t d = Shl(Sub(g.S32(e + 660), g.S32(e + 652)), 1);
            const int32_t rate = SplitDiv(d, s3, false);                        // 0x80079808 / 20
            const int32_t left = Sub(g.S32(e + 728), g.S32(e + 724));
            const uint32_t roll = g.U32(e + 652);
            g.W32(e + 660, U(rate));                                            // 0x80079840
            g.W32(e + 656, 0);
            g.W32(e + 576, 0);
            g.W32(e + 488, 0);
            g.W32(e + 728, U(left));                                            // 0x80079850
            g.W32(e + 664, roll);                                               // 0x80079858
            g.W32(e + 728, U(left < 0 ? 0 : left));                             // 0x8007986C
            g.W32(e + 724, 0);
            g.W32(e + 688, 0);                                                  // 0x80079878
            const int32_t dy = GDot(g, e + 528, e + 814);                       // 0x80079874
            const int32_t dx = GDot(g, e + 528, e + 450);                       // 0x80079884
            const int32_t ang = MulLo(RatAtan2(dy, dx, t.atan), 25736) >> 8;    // 0x80079890
            g.W32(e + 676, U(ang));                                             // 0x800798C4
            const int32_t step = SplitDiv(ang, s3, true);                       // 0x800798E4 / FC
            const uint32_t fc = g.U32(e + 568);
            g.W32(e + 680, U(step));                                            // 0x8007990C
            g.W32(e + 568, fc | 0x4000u);                                       // 0x80079918
            return true;
        }
    }
    // ---- 0x8007991C: the impact timer +0x2D4 / +0x2D8
    if (g.S32(e + 724) < g.S32(e + 728)) return true;
    if (!s8 && !s6 && g.S32(e + 616) < 0) return true;
    const uint32_t m = s8 ? 0xFFFFFFFFu : 0u;
    const uint32_t v26C = g.U32(e + 620);
    const uint32_t v268 = g.U32(e + 616);
    g.W32(e + 620, m & v26C);                                                   // 0x80079960
    const uint32_t v270 = g.U32(e + 624);
    g.W32(e + 688, 0);
    g.W32(e + 676, 0);
    g.W32(e + 624, m & v270);                                                   // 0x80079978
    g.W32(e + 616, m & v268);                                                   // 0x80079980
    if (s8) {
        if (!GSteerLean(g, e, t.sincos)) return false;                          // 0x80079984
        uint32_t v1 = 0;
        if (!(g.U32(R + 604) < 2u)) v1 = (g.U32(e + 856) == 0) ? 1u : 0u;
        g.W32(e + 568, g.U32(e + 568) | ((0u - v1) & 0x880u));                  // 0x800799C0
    } else {
        // The 1.78 arm is dead: the controls were cut to bits 27..31 at 0x80078ED8 this visit.
        const uint32_t speed = (g.U32(e + 560) & 0x300u) ? 0x1C9C4u : 0u;
        g.W32(e + 488, 0);                                                      // 0x800799D4
        const uint16_t h0 = g.U16(e + 528), h1 = g.U16(e + 530), h2 = g.U16(e + 532);
        const uint16_t r0 = g.U16(e + 516), r1 = g.U16(e + 518), r2 = g.U16(e + 520);
        g.W32(e + 576, speed);                                                  // 0x800799F8
        g.W16(e + 450, h0);
        g.W16(e + 452, h1);
        g.W16(e + 454, h2);
        g.W16(e + 814, r0);
        g.W16(e + 816, r1);
        g.W16(e + 818, r2);                                                     // 0x80079A10
        if (g.U32(R + 604) < 2u) {
            uint32_t fa = g.U32(e + 560);
            if ((fa & 0x08002000u) == 0x08000000u) fa |= 0x6000u;
            g.W32(e + 560, fa);                                                 // 0x80079A48
        } else {
            uint32_t rc = 0;
            if (g.Faulted() || !snd.GetRCnt(0xF2000002u, rc)) return false;     // 0x80079A50
            const int32_t id = S(((rc & 0xFFu) * 5u) >> 8) + 50;
            if (!snd.PlaySound3D(g.S32(e + 184), g.S32(e + 192), id, 0)) return false;   // 0x80079A74
            g.W32(e + 568, g.U32(e + 568) | 0x900u);                            // 0x80079A88
        }
    }
    // ---- 0x80079A8C
    const uint32_t mount = g.U32(R + 604);
    uint32_t fa = g.U32(e + 560);
    if (mount < 2u) fa |= 0x10080u;
    g.W32(e + 560, fa);                                                         // 0x80079AA8
    const uint32_t fc = g.U32(e + 568);
    uint32_t nf = fc & 0xFFFF9FF0u;
    g.W32(e + 568, nf);                                                         // 0x80079AC4
    if (!(fc & 0x600u)) nf |= 0x08000000u;
    g.W32(e + 568, nf);                                                         // 0x80079AD0
    g.W32(e + 828, 0);                                                          // 0x80079AD4
    return true;
}

} // namespace

bool ImpactStatePass(GuestRam& g, uint32_t head, const BikeTables& t, ImpactSound& snd) {
    if (t.sincos == nullptr || t.asin == nullptr || t.atan == nullptr || t.rsqrt == nullptr ||
        t.sqrt == nullptr)
        return false;
    uint32_t node = g.U32(head + 4u);                                           // 0x80078DE4
    int guard = 0;
    while (node != head) {
        if (g.Faulted() || ++guard > 4096) return false;
        const uint32_t e = node - 1088u;                                        // s2
        const int32_t turn = g.S32(e + 364);
        uint32_t fc = g.U32(e + 568);
        const uint32_t R = g.U32(e + 852);                                      // s7, read once
        if (!(MipsAbs(turn) < 3)) fc ^= 0x00400000u;                            // bit 22 TOGGLED
        g.W32(e + 568, fc);                                                     // 0x80078E34
        if (fc & 0x10u) {
            // ---- class 16: waits for TakePartnerHeading's turn +0x2A4 to reach 0
            if (g.U32(e + 676) == 0) {
                g.W32(e + 568, fc & ~0x10u);                                    // 0x80078E4C
                uint32_t v1;
                if (g.U32(R + 604) < 2u) v1 = 1;
                else if (g.U32(e + 856) == 0) v1 = 0;
                else v1 = (g.U32(e + 1088) != 0) ? 1u : 0u;
                g.W32(e + 568, g.U32(e + 568) | (((0u - v1) & 0x07FFF780u) + 2176u));   // 0x80078EA8
            }
        } else if (fc & 0xFu) {
            fc = g.U32(e + 568);
            const uint32_t fa = g.U32(e + 560);
            g.W32(e + 596, 0);                                                  // 0x80078EC4
            g.W32(e + 588, 0);                                                  // 0x80078EC8
            const bool s6 = (fc & 0x600u) != 0;
            g.W32(e + 560, fa & 0xF8000000u);                                   // 0x80078ED8
            uint32_t keep = 0;
            if (g.S32(e + 724) < g.S32(e + 728)) keep = g.U32(e + 488);
            g.W32(e + 488, keep);                                               // 0x80078EFC
            const bool s8 = (fc & 1u) != 0;
            const uint32_t fwd = e + (s6 ? 444u : 528u);
            if (s8) {
                bool take = (g.U32(e + 568) & 0x2000u) != 0;
                if (!take) take = GDot(g, e + 820, fwd) >= 0;                   // 0x80078F34
                if (take) {
                    if (s6) {
                        int32_t x = Shl(g.S16(fwd + 0), 4);
                        int32_t z = Shl(g.S16(fwd + 4), 4);
                        const int32_t y = Shl(g.S16(e + 452), 4);
                        const int32_t k = HorizRescale(y, x, z, t.sqrt);
                        g.W16(e + 450, static_cast<uint16_t>(U(FixMul(k, x) >> 4)));   // 0x80079064
                        g.W16(e + 454, static_cast<uint16_t>(U(FixMul(k, z) >> 4)));   // 0x8007907C
                        GScale(g, g.S32(e + 480), e + 450, e + 456);                    // 0x80079078
                    } else {
                        const uint16_t h0 = g.U16(fwd + 0);
                        const uint16_t r0 = g.U16(e + 516), r2 = g.U16(e + 520);
                        g.W16(e + 450, h0);                                     // 0x80079094
                        g.W16(e + 452, g.U16(fwd + 2));                         // 0x800790A0
                        const uint16_t h2 = g.U16(fwd + 4), r1 = g.U16(e + 518);
                        g.W16(e + 814, r0);
                        g.W16(e + 818, r2);
                        g.W16(e + 816, r1);
                        g.W16(e + 454, h2);                                     // 0x800790B8
                    }
                    g.W32(e + 568, g.U32(e + 568) | 0x2000u);                   // 0x800790C8
                }
            }
            fc = g.U32(e + 568);
            const bool on = g.U32(R + 604) < 2u;
            if ((fc & 8u) || ((fc & 4u) && on)) {
                if (g.U32(R + 604) < 2u) {
                    // ---- THE KNOCK-OFF REQUEST
                    int32_t v = g.S32(e + 616);
                    if (v < kFloorRequest) v = kFloorRequest;
                    g.W32(e + 616, U(v));                                       // 0x80079130
                    const bool request = (!s6 && v < 0) || g.S32(e + 728) < g.S32(e + 724);
                    if (request) {
                        const uint32_t mount = g.U32(R + 604);
                        uint32_t w = g.U32(R + 552);
                        if (mount < 2u) w |= 0x8000u;
                        g.W32(R + 552, w);                                      // 0x8007916C
                        if (g.U8(g.U32(e + 852) + 572) & 0x10u) {               // NO null test
                            const uint32_t R2 = g.U32(g.U32(e + 856) + 852);
                            if (g.U32(R2 + 604) < 2u) g.W32(R2 + 552, g.U32(R2 + 552) | 0x8000u);  // 0x800791BC
                        }
                    }
                }
                fc = g.U32(e + 568);
                bool go = (fc & 8u) != 0;
                if (!go) go = s6 && (fc & 4u);
                if (go) {
                    bool thr;
                    if (!s6 && g.S32(e + 616) < kFloorThrow) thr = true;
                    else thr = g.S32(e + 728) < g.S32(e + 724);
                    if (thr && !Throw(g, e, R, s6, t)) return false;
                }
            } else if (!Fall(g, e, R, s6, s8, t, snd)) {
                return false;
            }
        }
        if (g.Faulted()) return false;
        node = g.U32(node + 4u);                                                // 0x80079AE0
    }
    return !g.Faulted();
}

// ============================================================================ SLUS 0x8002EA20
uint32_t MulAdd16(GuestRam& g, uint32_t base, uint32_t dir, int32_t t, uint32_t out) {
    uint32_t v = 0;
    for (uint32_t k = 0; k < 3; ++k) {
        const int64_t p = static_cast<int64_t>(Shl(g.S16(dir + 2u * k), 4)) * static_cast<int64_t>(t);
        const int32_t b = Shl(g.S16(base + 2u * k), 4);
        const uint32_t lo = static_cast<uint32_t>(p), hi = static_cast<uint32_t>(static_cast<uint64_t>(p) >> 32);
        v = ((lo >> 16) | (hi << 16)) + U(b);
        g.W32(out + 4u * k, v);                                                 // 0x8002EA58 / 94 / D4
    }
    return v;
}

// ============================================================================ RASHCDG 0x800849D8
bool Spin(GuestRam& g, uint32_t e) {
    if (g.U32(e + 568) & 0x80000u) return !g.Faulted();                        // flagsC bit 19
    g.W32(e + 724, 0);                                                          // 0x80084A08
    g.W32(e + 728, 0x8000);                                                     // 0x80084A10
    const uint32_t r1 = GuestRand(g);                                           // 0x80084A0C
    const int32_t s1 = Sub(S(r1 - 45752u * (MulHiU(r1, 0x2DD65ECFu) >> 13)), 22876);
    const uint32_t r2 = GuestRand(g);                                           // 0x80084A54
    const int32_t s0 = Sub(S(r2 - 45752u * (MulHiU(r2, 0x2DD65ECFu) >> 13)), 22876);
    const uint32_t r3 = GuestRand(g);                                           // 0x80084A94
    const uint32_t f = r3 - 26215u * (MulHiU(r3, 0x4FFF8801u) >> 13);
    g.W32(e + 656, U(Shl(s1, 1)));                                              // 0x80084AB0
    g.W32(e + 640, U(Shl(s0, 1)));                                              // 0x80084AB4
    const int32_t yaw = FixMul(g.S32(e + 488), S(f + 0xCCCCu));
    const int32_t speed = g.S32(e + 480);
    g.W32(e + 488, U(yaw));                                                     // 0x80084B08
    int32_t lim;
    if (0x9FFFF < speed) {
        lim = 0xA0000;
    } else {
        const int32_t hi = S(static_cast<uint32_t>(
            static_cast<uint64_t>(static_cast<int64_t>(speed) * 0x55555556LL) >> 32));
        lim = Sub(hi, speed >> 31);                                             // speed / 3, truncated
    }
    const int32_t a1 = g.S32(e + 488);
    int32_t a2 = 0x18000;                                                       // 1.5
    if (a1 < 0) {
        a2 = S(0xFFFE8000u);
        lim = Neg(lim);
    }
    const int32_t c = Sub(MulLo(g.S16(e + 796), g.S16(e + 454)), MulLo(g.S16(e + 800), g.S16(e + 450)));
    const int32_t room = Sub(lim, a1);
    int32_t v = Add(a1, (Sub(a1, a2) >> 31) & Sub(a2, a1));                     // max(a1, a2)
    v = Add(v, (room >> 31) & room);                                            // + min(lim - a1, 0)
    g.W32(e + 488, U(v));                                                       // 0x80084BA0
    if ((c < 0 && v > 0) || (c > 0 && v < 0)) g.W32(e + 488, U(Neg(g.S32(e + 488))));   // 0x80084BC8
    return !g.Faulted();
}

// ============================================================================ RASHCDG 0x80084564
int32_t Bounce(GuestRam& g, uint32_t n, uint32_t d, uint32_t pSpeed, uint32_t vel, int32_t k,
               int32_t floor, int32_t flat, const BikeTables& t, bool& ok) {
    ok = false;
    if (t.sincos == nullptr || t.asin == nullptr || t.rsqrt == nullptr || t.sqrt == nullptr) return 0;
    int32_t s1 = GDot(g, d, n);                                                 // 0x800845AC
    if (s1 >= 0) {
        ok = !g.Faulted();
        return -1;                                                              // moving away
    }
    const int32_t s6 = (MipsAbs(g.S16(n + 2)) < 2048) ? 1 : 0;                  // |n.y| < 0.5
    int32_t s7;
    if (s6 && g.S16(d + 2) < 0) {
        // ---- 0x800845F8: a wall, heading down - mirror the horizontal part
        int32_t s2 = Neg(Shl(g.S16(n + 4), 4));
        int32_t s3 = Shl(g.S16(n + 0), 4);
        auto len = [&]() {
            const int32_t a = FixMul(s2, s2);
            return SqrtGte(Add(a, FixMul(s3, s3)), t.sqrt);
        };
        const int32_t l0 = len();                                               // 0x80084628
        const bool neg = Shl(l0, 2) < 0;
        const int32_t l1 = len();
        const int32_t l2 = len();
        auto divu = [](uint32_t a, uint32_t b) { return b == 0 ? 0xFFFFFFFFu : a / b; };
        uint32_t inv;
        if (neg) {                                                              // 0x80084684
            const int32_t x = Neg(Shl(l1, 2)) >> 1;
            const int32_t y = Sub(Neg(Shl(l2, 2)), 2) >> 31;
            inv = 0u - divu(0x80000000u, U(Add(x, y)));
        } else {                                                                // 0x80084700
            const int32_t x = Shl(l1, 2) >> 1;
            const int32_t y = Sub(Shl(l2, 2), 2) >> 31;
            inv = divu(0x80000000u, U(Add(x, y)));
        }
        s2 = FixMul(s2, S(inv));
        s3 = FixMul(s3, S(inv));
        const int32_t d0 = Shl(g.S16(d + 0), 4);
        const int32_t d2 = Shl(g.S16(d + 4), 4);
        const int32_t a = FixMul(s2, d0);
        const int32_t s0 = Shl(Add(a, FixMul(s3, d2)), 1);                      // 2 (t . d)
        g.W16(d + 0, static_cast<uint16_t>(U(Sub(FixMul(s0, s2), d0) >> 4)));  // 0x8008478C
        if (flat != 0) g.W16(d + 2, 0);                                         // 0x800847A4
        else g.W16(d + 2, static_cast<uint16_t>(U(Sub(0xB4FD, Shl(g.S16(d + 2), 4)) >> 5)));   // 0x800847BC
        g.W16(d + 4, static_cast<uint16_t>(U(Sub(FixMul(s0, s3), d2) >> 4)));  // 0x800847D4
        s7 = Add(floor, S(U(floor) >> 31)) >> 1;
    } else {
        // ---- 0x800847E8: the restitution about the normal
        int32_t s0 = k;
        if (floor == 0) {
            const int32_t v = g.S32(pSpeed);
            if (!(0x1FFFF < v)) s0 = 0;
            else if (!(0x9FFFF < v)) s0 = FixMul(FixMul(Sub(v, 0x20000), 8192), Shl(s0, 16)) >> 16;
        }
        s1 = Neg(s1);
        int32_t s2 = 0;
        if (!Asin(s1, t.asin, s2)) return 0;                                    // 0x8008483C
        if (s0 != 0) {
            const int32_t p = MulLo(s0, s2);
            const int32_t hi = S(static_cast<uint32_t>(
                static_cast<uint64_t>(static_cast<int64_t>(p) * 0x66666667LL) >> 32));
            const int32_t q = Sub(hi >> 2, p >> 31);                            // p / 10, truncated
            const int32_t lo = Add(q, (Sub(q, 56) >> 31) & Sub(56, q));         // max(q, 56)
            const int32_t hiCap = (Sub(512, q) >> 31) & Sub(512, q);            // min(512 - q, 0)
            const int32_t tn = RatTan(Add(lo, hiCap), t.sincos);                // 0x80084898
            const int32_t c = t.sincos[2u * (U(s2) & 0xFFFu) + 1u];
            s0 = Add(s1, FixMul(Shl(c, 4), tn));
        } else {
            s0 = s1;
        }
        if (0xFC28 < s1 && s0 < 0x11999) s0 = 0x11999;                         // at least 1.1
        MulAdd16(g, d, n, s0, vel);                                             // 0x80084908
        for (uint32_t i = 0; i < 3; ++i)
            g.W16(d + 2u * i, static_cast<uint16_t>(U(g.S32(vel + 4u * i) >> 4)));   // 0x80084924 / 34 / 44
        s7 = floor;
    }
    // ---- 0x80084948
    const int32_t keep = Add(S((0u - U(s6)) & 0xFFFF4CCDu), 0xCCCC);          // 0.1 or 0.8
    const int32_t v = FixMul(keep, g.S32(pSpeed));
    g.W32(pSpeed, U(v));                                                        // 0x80084978
    g.W32(pSpeed, U(s7 < v ? v : s7));                                          // 0x80084988
    if (!GNormalize(g, d, t.rsqrt)) return 0;                                   // 0x80084984
    GScale(g, g.S32(pSpeed), d, vel);                                           // 0x80084994
    ok = !g.Faulted();
    return s6;
}

// ============================================================================ RASHCDG 0x800903F4
bool Remount(GuestRam& g, uint32_t B, int32_t fromRoad, RemountCallees& c) {
    constexpr uint32_t kGameState = 0x8005B2F8;
    constexpr uint32_t kViews = 0x800CD898;
    constexpr uint32_t kKeep228 = 0xBFE67FC7u; // bits 3..5, 15, 16, 19, 20, 30 cleared
    auto fail = [&g]() { return false; };
    if (fromRoad != 0) {
        if (g.S16(B + 320) != 0) {                                              // a live bike
            const uint32_t sl = g.U32(B + 340);
            GMulAdd(g, sl + 20, sl + 14, g.S32(B + 348), B + 504);              // 0x80090430
            const uint32_t p0 = g.U32(B + 504), p1 = g.U32(B + 508), p2 = g.U32(B + 512);
            const uint32_t sl2 = g.U32(B + 340);
            g.W32(B + 184, p0);                                                 // 0x80090448
            g.W32(B + 188, p1);
            g.W32(B + 192, p2);
            g.W16(B + 450, g.U16(sl2 + 14));                                    // 0x8009045C
            g.W16(B + 452, g.U16(sl2 + 16));
            g.W16(B + 454, g.U16(sl2 + 18));                                    // 0x80090474
            if (g.U32(B + 568) & 0x00400000u) {
                const uint16_t h0 = g.U16(B + 450);
                g.W32(B + 364, 0xFFFFFFFEu);                                    // 0x80090490
                const uint16_t h2 = g.U16(B + 454);
                g.W16(B + 450, static_cast<uint16_t>(0u - h0));
                const uint16_t h1 = g.U16(B + 452);
                g.W16(B + 454, static_cast<uint16_t>(0u - h2));
                g.W16(B + 452, static_cast<uint16_t>(0u - h1));                 // 0x800904B4
            } else {
                g.W32(B + 364, 2);                                              // 0x800904BC
            }
            if (g.Faulted() || !c.RowsFromHeading(B)) return fail();            // 0x800904C0
        }
        g.W32(B + 344, 0);                                                      // 0x800904C8
    }
    if (g.Faulted() || !c.ResetBikeState(B)) return fail();                     // 0x800904CC
    const uint32_t R = g.U32(B + 852);
    g.W32(B + 568, g.U32(B + 568) | 0x08000000u);                               // 0x800904E8
    g.W32(R + 552, g.U32(R + 552) & kKeep228);                                  // 0x800904F8
    g.W16(g.U32(B + 852) + 320, g.U16(B + 320));                                // 0x80090508
    if (g.U32(g.U32(B + 852) + 556) != 0) {
        if (g.Faulted() || !c.ReleaseRiderObject(g.U32(B + 852))) return fail();   // 0x80090524
        g.W32(g.U32(B + 852) + 556, 0);                                         // 0x80090534
    }
    if (g.Faulted() || !c.Attach(B, g.U32(B + 852), 2, 0)) return fail();      // 0x80090544
    const uint32_t R1 = g.U32(B + 852);
    g.W8(B + 72, 0);                                                            // 0x80090558
    if (g.Faulted() || !c.Dismount(R1, 1)) return fail();                       // 0x80090554
    if (g.U8(g.U32(B + 852) + 572) & 0x10u) {
        if (g.U8(g.U32(kGameState) + 57) != 1) {
            // ---- a two-rider bike: the passenger too
            const uint32_t P = g.U32(g.U32(B + 856) + 852);
            const uint16_t live = g.U16(B + 320);
            const uint32_t w = g.U32(P + 552);
            g.W16(P + 320, live);                                               // 0x800905AC
            g.W32(P + 552, w & kKeep228);                                       // 0x800905B8
            if (g.Faulted() || !c.Attach(B, P, 2, 1)) return fail();            // 0x800905B4
            if (g.Faulted() || !c.Dismount(P, 1)) return fail();                // 0x800905C0
            g.W32(P + 604, 1);                                                  // 0x800905C8
            g.W32(g.U32(B + 856) + 720, 0);                                     // 0x800905D4
        }
    }
    if (g.Faulted() || !c.ClearCommands(B)) return fail();                      // 0x800905D8
    {
        const uint32_t rd = g.U32(B + 1084);
        bool two = g.U32(rd + 40) != 0;
        if (!two) two = !(g.U8(rd + 39) < 248u);
        const uint16_t op = static_cast<uint16_t>(((0u - (two ? 1u : 0u)) & 0xFFFFFFFEu) + 4u);
        if (g.Faulted() || !c.PushCommand(op, 224, 1, B)) return fail();       // 0x80090634
    }
    if (g.S16(B + 320) != 0) {
        if (g.Faulted() || !c.ReFace(B)) return fail();                         // 0x8009064C
        const uint16_t h0 = g.U16(B + 528), h1 = g.U16(B + 530), h2 = g.U16(B + 532);
        const uint16_t r0 = g.U16(B + 516), r1 = g.U16(B + 518), r2 = g.U16(B + 520);
        const uint32_t Rr = g.U32(B + 852);
        g.W16(B + 450, h0);                                                     // 0x80090670
        g.W16(B + 452, h1);
        g.W16(B + 454, h2);
        g.W16(B + 814, r0);
        g.W16(B + 816, r1);
        g.W16(B + 818, r2);                                                     // 0x80090684
        if ((g.U8(Rr + 572) & 0x10u) && g.U8(g.U32(kGameState) + 57) != 1) {
            const uint32_t Q = g.U32(B + 856);
            g.W32(Q + 344, g.U32(B + 344));                                     // 0x800906C0
            const uint32_t Q2 = g.U32(B + 856);
            const int32_t span = Add(g.S32(B + 304), g.S32(Q2 + 304));
            const int32_t lat = g.S32(Q2 + 344);
            g.W32(Q2 + 344, U(g.S32(B + 364) >= 0 ? Add(lat, span) : Sub(lat, span)));   // 0x80090708
            g.W32(g.U32(B + 856) + 364, g.U32(B + 364));                        // 0x80090718
        }
    }
    const uint32_t gs = g.U32(kGameState);
    const uint32_t h = g.U16(B + 172);
    if (h < g.U32(gs + 48)) {
        // ---- a player: the view record
        const uint32_t V = kViews + 1132u * h;
        g.W32(V + 548, g.U32(V + 548) & 0xF5FFFF7Fu);                           // 0x80090770
        if (g.U8(gs + 4) == 44 && g.U8(gs + 57) == 2) {
            const uint32_t v228 = g.U32(V + 552) | 0x10u;
            const int32_t v308 = g.S32(V + 776);
            g.W32(V + 552, v228);                                               // 0x800907A8
            if (v308 < 5) {
                g.W32(V + 776, 5);
                g.W32(V + 792, 0);
            }
            g.W8(g.U32(kGameState) + 10, 2);                                    // 0x800907C0
        } else {
            g.W32(V + 548, (g.U32(V + 548) | 6u) & 0xFF807FFFu);                // 0x800907D4
        }
        if (g.Faulted() || !c.ViewReset(V)) return fail();                      // 0x800907D8
        if (g.Faulted() || !c.PlayerVoice(g.U16(B + 172), 0)) return fail();    // 0x800907E4
        if (g.Faulted() || !c.PlayerBind(g.U16(B + 172), B)) return fail();     // 0x800907F0
    }
    return !g.Faulted();
}

// ============================================================================ SLUS 0x8002EED8
void ScaleTo16(GuestRam& g, int32_t t, uint32_t v, uint32_t out) {
    for (uint32_t k = 0; k < 3; ++k)
        g.W16(out + 2u * k, static_cast<uint16_t>(U(FixMul(t, g.S32(v + 4u * k)) >> 4)));   // 0x8002EF04 / 18 / 2C
}

// ============================================================================ RASHCDG 0x80083864
void MassExchangeValues(int32_t m1, int32_t m2, int32_t v1, int32_t v2, int32_t& o1raw, int32_t& o1,
                        int32_t& o2raw, int32_t& o2) {
    const int32_t p = FixMul(Sub(m1, m2), v1);
    o1raw = Add(p, FixMul(Shl(m2, 1), v2));
    o1 = o1raw < 0 ? 0 : o1raw;
    const int32_t q = FixMul(Shl(m1, 1), v1);
    o2raw = Add(q, FixMul(Sub(m2, m1), v2));
    o2 = o2raw < 0 ? 0 : o2raw;
}

void MassExchange(GuestRam& g, int32_t m1, int32_t m2, int32_t v1, int32_t v2, uint32_t o1, uint32_t o2) {
    int32_t r1 = 0, c1 = 0, r2 = 0, c2 = 0;
    MassExchangeValues(m1, m2, v1, v2, r1, c1, r2, c2);
    g.W32(o1, U(r1));                                                           // 0x800838C4
    g.W32(o1, U(c1));                                                           // 0x800838D8
    g.W32(o2, U(r2));                                                           // 0x800838F4
    g.W32(o2, U(c2));                                                           // 0x800838FC
}

// ============================================================================ RASHCDG 0x80081D7C
namespace {

// `mult x, y; (lo >> 16) | (hi << 16)`: the middle word of the 64-bit product.
inline int32_t MH(int32_t x, int32_t y) {
    return S(static_cast<uint32_t>(static_cast<uint64_t>(static_cast<int64_t>(x) * static_cast<int64_t>(y)) >> 16));
}
// neg3(p): lhu p0, lhu p2; sh -p0; lhu p1; sh -p2; sh -p1 - the store order 0, 2, 1 (halfwords).
void Neg3(GuestRam& g, uint32_t p) {
    const uint16_t x = g.U16(p + 0), z = g.U16(p + 4);
    g.W16(p + 0, static_cast<uint16_t>(0u - x));
    const uint16_t y = g.U16(p + 2);
    g.W16(p + 4, static_cast<uint16_t>(0u - z));
    g.W16(p + 2, static_cast<uint16_t>(0u - y));
}
// MH(rel, v << 4) summed over the three components of a guest s16[3] or a host one.
int32_t MHDot(const int32_t rel[3], const int16_t v[3]) {
    return Add(MH(rel[2], Shl(v[2], 4)), Add(MH(rel[1], Shl(v[1], 4)), MH(rel[0], Shl(v[0], 4))));
}
// The ANGLE of HitOutcome: pi/2 -+ (RatAtan2(DotLcm(e+0x32E, n), -DotLcm(dir, n)) * 25736 >> 8).
int32_t GateAngle(GuestRam& g, uint32_t dir, uint32_t e, const int16_t nrm[3], const int32_t* atan) {
    int16_t d[3], r[3];
    Read16x3(g, dir, d);
    const int32_t s0 = DotLcm(d, nrm);
    Read16x3(g, e + 814, r);
    const int32_t y = DotLcm(r, nrm);
    const int32_t v = MulLo(RatAtan2(y, Neg(s0), atan), 25736) >> 8;
    return Sub(v > 0 ? 0x1921F : S(0xFFFE6DE1u), v);
}
int32_t GateLimit(GuestRam& g, int32_t ang, uint32_t e) {
    return (S(U(ang) ^ g.U32(e + 488)) < 0) ? 0x1226C : 0xF5BE;               // 65 / 55 degrees
}
struct Faces {
    int32_t s = 0, x = 0, y = 0, z = 0;
};
Faces FacesOf(uint32_t code) {
    Faces f;
    const uint32_t lo = code & 0xFFu;
    if (code & 0x200u) {
        f.s = 1;
        f.x = lo == 3;
        f.y = lo == 1;
        f.z = lo == 2;
    } else {
        f.x = S((lo >> 1) & 1u);
        f.y = lo < 8u ? ((code & 2u) == 0) : 0;
        f.z = S((code & 1u) ^ ((code & 2u) >> 1));
    }
    return f;
}
inline uint32_t DivU(uint32_t a, uint32_t b) { return b == 0 ? 0xFFFFFFFFu : a / b; }

} // namespace

int32_t BikeBikeGate(GuestRam& g, uint32_t a, uint32_t b, uint32_t codeA, uint32_t codeB, uint32_t n,
                     const int32_t stale[4], const BikeTables& t, GateCallees& c, bool& ok) {
    ok = false;
    if (t.atan == nullptr || t.sqrt == nullptr || t.sincos == nullptr || t.asin == nullptr || t.rsqrt == nullptr)
        return 0;
    constexpr uint32_t kGameState = 0x8005B2F8;
    constexpr int32_t kAngledFaster = 0xD6944;                                  // 13.427
    const int32_t kCls32 = S(0xFFE9A5E4u), kCls4 = S(0xFFE52D78u), kCls8 = S(0xFFCED35Bu);
    uint32_t B[2] = {a, b}, D[2], P[2];                                         // sp+48, +160, +168
    int32_t ang[2] = {0, stale[0]}, lim[2] = {0, stale[1]};                     // sp+104, +112
    int32_t cl[2] = {stale[2], stale[3]};                                       // +120: REAR leaves them stale
    int32_t out[2] = {0, 0}, a9[2] = {0, 0}, cls[2] = {0, 0};                   // +128, +136, +184
    int16_t nrm[2][3] = {{0, 0, 0}, {0, 0, 0}};                                 // +144, +150
    int32_t rel[3] = {0, 0, 0};                                                 // +72
    int32_t xch = 0, knock = -1;                                                // +216, +224
    bool mainArm = true;                                                        // +220
    auto bad = [&g]() { return g.Faulted(); };

    // ---- 0x80081DE4: latch both
    for (uint32_t k = 0; k < 2; ++k) {
        const uint32_t e = B[k];
        g.W32(e + 564, g.U32(e + 564) & ~0x4000u);                              // 0x80081DF8
        if (!(g.U32(e + 568) & 0x02000000u)) {
            g.W16(e + 864, g.U16(e + 450));
            g.W16(e + 866, g.U16(e + 452));
            g.W16(e + 868, g.U16(e + 454));
            g.W32(e + 860, g.U32(e + 480));
            g.W32(e + 568, g.U32(e + 568) | 0x02000000u);                       // 0x80081E74
        }
        D[k] = e + 864;
        P[k] = e + 860;
    }
    const uint32_t gs = g.U32(kGameState);
    const uint32_t np = g.U32(gs + 48);
    bool bothPlayers = false;                                                   // s8
    if (g.U16(B[0] + 172) < np) bothPlayers = g.U16(B[1] + 172) < np;
    const uint32_t fcA = g.U32(B[0] + 568);
    auto knockAllowed = [&](uint32_t e) {                                       // (s8 || !player) && !bit 29
        if (!bothPlayers && g.U16(e + 172) < g.U32(g.U32(kGameState) + 48)) return false;
        return !(g.U32(e + 560) & 0x20000000u);
    };
    auto bounce = [&](uint32_t nn, uint32_t k2, int32_t kk) {
        bool bok = false;
        const int32_t floor = FixMul(0xCCCC, g.S32(P[k2]));
        const int32_t r = Bounce(g, nn, D[k2], P[k2], B[k2] + 456, kk, floor, 0, t, bok);
        return std::pair<bool, int32_t>(bok, r);
    };

    if ((fcA & 0x200u) || (g.U32(B[1] + 568) & 0x200u)) {
        // ---- A CRASHED BIKE (0x80081F18)
        mainArm = false;
        uint32_t C, O;
        if (fcA & 0x200u) {
            if (g.U32(B[1] + 568) & 0x200u) {
                // both crashed
                auto r = bounce(n, 0, 5);                                       // 0x80081F70
                if (!r.first) return 0;
                if (r.second >= 0 && !Spin(g, B[0])) return 0;                  // 0x80081F84
                Neg3(g, n);
                r = bounce(n, 1, 5);                                            // 0x80081FE0
                if (!r.first) return 0;
                if (r.second >= 0 && !Spin(g, B[1])) return 0;                  // 0x80081FF4
                goto done;
            }
            C = 0;
            O = 1;
        } else {
            C = 1;
            O = 0;
            Neg3(g, n);                                                         // 0x80082018..38
        }
        {
            auto r = bounce(n, C, 5);                                           // 0x80082088
            if (!r.first) return 0;
            if (r.second >= 0 && !Spin(g, B[C])) return 0;                      // 0x8008209C
        }
        const uint32_t oe = B[O], ce = B[C];
        const uint32_t ofc = g.U32(oe + 568);                                   // read AFTER Bounce / Spin
        if (ofc & 0x400u) {
            // the other bike is airborne
            int32_t d[3];
            for (uint32_t k = 0; k < 3; ++k) d[k] = Sub(g.S32(ce + 184 + 4u * k), g.S32(oe + 184 + 4u * k));
            int16_t h[3];
            Read16x3(g, ce + 450, h);
            const int32_t along = MHDot(d, h);
            if (along < 0 && !(g.U32(ce + 560) & 0x20000000u)) {
                // ---- O is ahead of the crashed bike: THROW it
                g.W32(oe + 568, g.U32(oe + 568) & ~0x400u);                     // 0x800821DC
                g.W32(oe + 568, g.U32(oe + 568) | 0xA00u);                      // 0x800821F4
                g.W16(oe + 450, g.U16(ce + 450));
                g.W16(oe + 452, g.U16(ce + 452));
                g.W16(oe + 454, g.U16(ce + 454));                               // 0x80082230
                int32_t v = FixMul(0xCCCC, g.S32(ce + 480));
                if (v < 0x190000) v = 0x190000;
                g.W32(oe + 480, U(v));                                          // 0x80082268
                g.W16(oe + 796, g.U16(oe + 454));
                g.W16(oe + 798, 0);
                g.W16(oe + 800, static_cast<uint16_t>(0u - g.U16(oe + 450)));   // 0x800822A0
                g.W32(oe + 488, 0x50000);
                g.W32(oe + 728, 0);                                             // 0x800822B8
            } else {
                Neg3(g, n);                                                     // 0x800822C4..E8
                if (bad() || !c.ImpactTurn(oe, ce + 172, n, 5)) return 0;       // 0x80082300
            }
        } else if (!(ofc & 0x7FFu) && knockAllowed(oe)) {
            g.W32(oe + 568, ofc | 0x200840u);                                   // 0x80082358: THE KNOCK-ON
            knock = S(O);
        }
    } else if ((fcA & 0x400u) || (g.U32(B[1] + 568) & 0x400u)) {
        // ---- AN AIRBORNE BIKE (0x80082374)
        int32_t d[3];
        for (uint32_t k = 0; k < 3; ++k) d[k] = Sub(g.S32(B[1] + 184 + 4u * k), g.S32(B[0] + 184 + 4u * k));
        int16_t r1[3];
        Read16x3(g, B[0] + 438, r1);
        const int32_t tt = MHDot(d, r1);
        const int32_t half = g.S32(B[0] + 312);
        if ((Add(half, S(U(half) >> 31)) >> 1) < MipsAbs(tt)) {
            mainArm = false;
            const uint32_t lo = tt > 0 ? 0u : 1u, up = 1u - lo;                 // s2, s3
            const uint32_t U2 = B[up], L2 = B[lo];
            const uint32_t ufc = g.U32(U2 + 568);
            if (ufc & 0x400u) {
                bool bok = false;
                Bounce(g, L2 + 522, D[up], P[up], U2 + 456, 0, FixMul(0xCCCC, g.S32(P[up])), 0, t, bok);   // 0x800824E4
                if (!bok) return 0;                                             // the result is dropped
            } else if (!(ufc & 0x7FFu) && knockAllowed(U2)) {
                knock = S(up);
                g.W32(U2 + 568, ufc | 0x200840u);                               // 0x80082544
                g.W32(L2 + 828, U2 + 172);                                      // 0x8008255C
            }
        }
    }
done:
    if (!mainArm) {
        if (knock >= 0 && (bad() || !c.RiderSpeech(g.U16(B[knock] + 172), 1))) return 0;   // 0x80083828
        ok = !bad();
        return 1;
    }

    // ================================================================ MAIN (0x80082570)
    a9[0] = a9[1] = 1143;                                                       // 1 degree
    {
        int16_t d[3];
        int32_t v[3];
        Read16x3(g, D[0], d);
        Scale(g.S32(P[0]), d, v);
        Write32x3(g, B[0] + 456, v);                                            // 0x8008258C
        Read16x3(g, D[1], d);
        Scale(g.S32(P[1]), d, v);
        Write32x3(g, B[1] + 456, v);                                            // 0x800825A4
    }
    for (uint32_t k = 0; k < 3; ++k) rel[k] = Sub(g.S32(B[0] + 456 + 4u * k), g.S32(B[1] + 456 + 4u * k));
    {
        int16_t nn[3];
        Read16x3(g, n, nn);
        ang[0] = GateAngle(g, D[0], B[0], nn, t.atan);
        lim[0] = GateLimit(g, ang[0], B[0]);
    }
    const Faces fa = FacesOf(codeA), fb = FacesOf(codeB);
    if (((fa.x | fa.y) & (fb.x | fb.y)) & ~(fb.y & fa.y)) {
        if ((fa.x & fb.y) || (fa.y & fb.x)) {
            // ---- REAR: a front against a rear
            if (fa.x & fb.y) {
                std::swap(B[0], B[1]);
                std::swap(D[0], D[1]);
                std::swap(P[0], P[1]);
            }
            if (g.S32(P[0]) < g.S32(P[1])) {
                int16_t d0[3], d1[3];
                Read16x3(g, D[0], d0);
                Read16x3(g, D[1], d1);
                for (int k = 0; k < 3; ++k) {
                    nrm[0][k] = static_cast<int16_t>(-d1[k]);
                    nrm[1][k] = static_cast<int16_t>(-d0[k]);
                }
                xch = 1;
                cls[0] = cls[1] = -1;
            }
        } else {
            // ---- HEAD: two fronts
            int16_t d0[3], d1[3];
            Read16x3(g, D[1], d1);
            cl[0] = MHDot(rel, d1);
            const int32_t pb = g.S32(P[1]), pa = g.S32(P[0]);
            Read16x3(g, D[0], d0);
            cl[1] = Neg(MHDot(rel, d0));
            const int32_t dv = Sub(pb, pa);
            int32_t t1 = 0;
            if (dv < S(0xFFF60000u)) {                                          // faster by more than 10.0
                t1 = 1;
                std::swap(B[0], B[1]);
                std::swap(D[0], D[1]);
                std::swap(P[0], P[1]);
                std::swap(cl[0], cl[1]);
            } else if (0xA0000 < dv) {
                t1 = 1;
            }
            Read16x3(g, D[0], nrm[1]);
            Read16x3(g, D[1], nrm[0]);
            int32_t k = 0;
            for (; k < t1; ++k)
                cls[k] = (cl[k] < kCls32 && !(g.U32(B[k] + 560) & 0x20000000u)) ? 32 : 16;
            for (; k < 2; ++k) {
                cls[k] = cl[k] < kCls8 ? 8 : cl[k] < kCls4 ? 4 : 2;
                if (g.U32(B[k] + 560) & 0x20000000u) cls[k] = 2;
            }
        }
    } else if (lim[0] < MipsAbs(ang[0])) {
        // ---- ANGLED: a T contact
        int16_t nn[3];
        Read16x3(g, n, nn);
        for (int k = 0; k < 3; ++k) {
            nrm[0][k] = nn[k];
            nrm[1][k] = static_cast<int16_t>(-nn[k]);
        }
        cl[0] = MHDot(rel, nn);
        cl[1] = Neg(cl[0]);
        int32_t t1, o;
        if (cl[0] > 0 || kAngledFaster < Sub(g.S32(P[1]), g.S32(P[0]))) {
            t1 = 1;
            o = 0;
        } else {
            t1 = 0;
            o = 1;
        }
        cls[o] = cl[t1] < kCls32 ? 32 : 16;
        if (g.U32(B[o] + 560) & 0x20000000u) cls[o] = 16;
        const int32_t Sa = ang[t1], La = lim[t1];                               // t1 == 1: the STALE words
        const bool beyond = (Sa < Neg(La)) || (La < Sa);
        cls[t1] = !beyond ? 1 : cl[t1] < kCls8 ? 8 : cl[t1] < kCls4 ? 4 : 0;
        if ((g.U32(B[t1] + 560) & 0x20000000u) && cls[t1] != 1) cls[t1] = 0;
        if (cls[t1] == 0) cls[t1] = -1;
    } else {
        // ---- SIDE
        if (g.U32(a + 828) == b + 172u || g.U32(b + 828) == a + 172u) goto join;   // the same partner
        if (0x10000 < g.S32(P[0]) || 0x10000 < g.S32(P[1])) {
            // the common momentum's direction x a's up row, on the GTE
            const int32_t q = SplitDiv(g.S32(B[1] + 316), g.S32(B[0] + 316), false);
            int32_t m[3], base[3], dir[3];
            Read32x3(g, B[0] + 456, base);
            Read32x3(g, B[1] + 456, dir);
            MulAdd32(base, dir, q, m);                                          // 0x80082F58
            for (;;) {
                if (0x5A8000 < MipsAbs(m[0]) || 0x5A8000 < MipsAbs(m[1]) || 0x5A8000 < MipsAbs(m[2])) {
                    for (int32_t& x : m) x >>= 1;
                    continue;
                }
                break;
            }
            const int32_t l0 = Length3(m, t.sqrt);                              // 0x8008301C
            uint32_t inv;
            if (l0 >= 0) {
                const int32_t l1 = Length3(m, t.sqrt), l2 = Length3(m, t.sqrt);
                inv = DivU(0x80000000u, U(Add(l1 >> 1, Sub(l2, 2) >> 31)));
            } else {
                const int32_t l1 = Length3(m, t.sqrt), l2 = Length3(m, t.sqrt);
                inv = 0u - DivU(0x80000000u, U(Add(Neg(l1) >> 1, Sub(Neg(l2), 2) >> 31)));
            }
            for (int k = 0; k < 3; ++k)
                nrm[0][k] = static_cast<int16_t>(static_cast<uint16_t>(U(FixMul(S(inv), m[k]) >> 4)));   // ScaleTo16
            int16_t up[3];
            Read16x3(g, B[0] + 522, up);
            OuterProduct(nrm[0], up, nrm[1]);                                   // 0x800830F0: GTE OP
            for (int k = 0; k < 3; ++k) nrm[0][k] = nrm[1][k];
            int16_t d0[3];
            Read16x3(g, D[0], d0);
            if (DotLcm(nrm[0], d0) > 0) {
                for (int16_t& x : nrm[0]) x = static_cast<int16_t>(-x);
            } else {
                for (int16_t& x : nrm[1]) x = static_cast<int16_t>(-x);
            }
        } else {
            Read16x3(g, B[0] + 814, nrm[0]);
            if (fa.z) for (int16_t& x : nrm[0]) x = static_cast<int16_t>(-x);
            Read16x3(g, B[1] + 814, nrm[1]);
            if (fb.z) for (int16_t& x : nrm[1]) x = static_cast<int16_t>(-x);
        }
        cl[0] = MHDot(rel, nrm[0]);
        cl[1] = Neg(MHDot(rel, nrm[1]));
        cls[0] = cls[1] = 1;
        if (fa.s && fb.s) a9[0] = a9[1] = 0;
    }
join:
    // ================================================================ JOIN (0x80083358)
    if (cls[0] == 0 && cls[1] == 0) {
        ok = !bad();
        return 0;
    }
    for (uint32_t k = 0; k < 2; ++k) {
        ang[k] = GateAngle(g, D[k], B[k], nrm[k], t.atan);
        lim[k] = GateLimit(g, ang[k], B[k]);
        if (g.U32(B[k] + 568) & 0x100u) cls[k] = 16;                            // carried along
    }
    uint32_t ij[2];
    if (cls[0] < 16 && cls[1] < 16 && xch == 0) {
        ij[0] = 0;
        ij[1] = 1;
        out[0] = g.S32(P[0]);
        out[1] = g.S32(P[1]);
    } else {
        ij[0] = (cls[1] < 16) ? 0u : 1u;
        ij[1] = ij[0] == 0 ? 1u : 0u;
        const int32_t M = Add(g.S32(B[0] + 316), g.S32(B[1] + 316));
        const uint32_t inv = DivU(0x80000000u, U(Add(M >> 1, Sub(M, 2) >> 31)));
        int16_t d0[3], d1[3];
        Read16x3(g, D[0], d0);
        Read16x3(g, D[1], d1);
        int32_t vi = FixMul(g.S32(P[ij[0]]), DotLcm(d0, d1));
        const int32_t mi = FixMul(g.S32(B[ij[0]] + 316), S(inv));
        const int32_t mj = FixMul(g.S32(B[ij[1]] + 316), S(inv));
        if (vi < 0) vi = 0;
        int32_t r1 = 0, r2 = 0;
        MassExchangeValues(mi, mj, vi, g.S32(P[ij[1]]), r1, out[ij[0]], r2, out[ij[1]]);   // 0x800835B4
    }
    for (uint32_t s = 0; s < 2; ++s) {
        const uint32_t k = ij[s], o = (k == 0) ? 1u : 0u;
        const uint32_t e = B[k];
        if (g.U32(e + 568) & 0x80u) {
            a9[k] = 11438;                                                      // 10 degrees
            g.W32(e + 560, g.U32(e + 560) | 0x04000000u);                       // 0x80083648
        }
        if (cls[k] == 0) continue;
        const uint32_t fc = g.U32(e + 568);
        if (fc & 0xFu) {
            g.W32(e + 568, fc & 0xFFF007F0u);                                   // 0x80083684
            g.W32(e + 828, 0);
            g.W32(e + 568, (fc & 0xFFF007F0u) | 0x08000000u);                   // 0x80083690
        }
        if (cls[k] == -1) g.W32(e + 564, g.U32(e + 564) | 0x4000u);             // 0x800836B4
        else g.W32(e + 568, g.U32(e + 568) | U(cls[k]));                        // 0x800836C4: THE CLASS
        int32_t v0 = 0;
        if (bad()) return 0;
        if (cls[k] < 16) {
            const int32_t kk = FixMul(cl[k], 2443);
            if (!c.HitSpeed(e, D[k], P[k], B[o] + 172, nrm[k], k, kk, ang[k], lim[k], out[k], a9[k], v0)) return 0;
        } else {
            if (!c.TakePartnerHeading(e, D[k], P[k], D[o], out[k], v0)) return 0;
        }
        cls[k] = v0;
        const uint32_t f2 = g.U32(e + 568);
        const uint32_t add = (f2 & 0x1Fu) ? 0x1000u : ((f2 & 0x220u) ? 0x800u : 0u);
        g.W32(e + 568, g.U32(e + 568) | add);                                   // 0x800837D8
    }
    ok = !bad();
    return cls[0] | cls[1];
}

} // namespace rr::sim
