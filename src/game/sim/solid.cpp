#include "game/sim/solid.h"

#include "game/sim/ai.h"          // SqrtGte SLUS 0x8004CF74
#include "game/sim/bike_step.h"   // RotateRowPair SLUS 0x8002ED94
#include "game/sim/coll_util.h"
#include "game/sim/crash.h"       // MulAdd16 SLUS 0x8002EA20, Bounce 0x80084564
#include "game/sim/pose.h"        // AnimPose::QuatToMatrix SLUS 0x8001005C
#include "game/sim/road_runtime.h" // RoadRebindBody 0x80037450
#include "game/sim/vec.h"         // Blend16 / Blend32 / Blend16To32
#include "game/sim/recover.h"     // rc::GteOp, rc::GNormalize (SLUS 0x8002E468)
#include "game/sim/recover_fall.h" // LaunchLift 0x8007E868
#include "game/sim/resolvers.h"   // FirstPointInsideBox 0x800B7030, FaceNormal 0x800B675C
#include "game/sim/world_pop.h"   // MatrixQuat 0x800716C0

namespace rr::sim {
namespace {

using namespace cu;

constexpr uint32_t kSinCos = 0x8005624C;   // SLUS {s16 sin, s16 cos} x 4096
constexpr uint32_t kSurfaceTable = 0x800525C0; // SLUS u8[52]
constexpr uint32_t kRandSeedAddr = 0x8005B4A8; // gp + 2076

uint32_t GRand(GuestRam& g) {
    uint32_t seed = g.U32(kRandSeedAddr);
    const uint32_t r = Rand(seed);
    g.W32(kRandSeedAddr, seed);
    return r;
}

// `x % 714 - 357` as the compiler writes it (multu 0x16F26017, >> 6).
int32_t Mod714(uint32_t r) { return S(r - 714u * static_cast<uint32_t>((static_cast<uint64_t>(r) * 0x16F26017u) >> 38)) - 357; }

// The DOD3 kind of the prop's model record: (u16 model[+0x0E] & 0xF80) >> 7.
uint32_t ModelKind(GuestRam& g, uint32_t p) { return (g.U16(g.U32(p) + 14) & 0xF80u) >> 7; }

bool PlaySound(CollisionCallees& c, GuestRam& g, uint32_t p, int32_t id) {
    return c.PlaySound3D(g.S32(p + 184), g.S32(p + 192), id, 0);
}

} // namespace

// ============================================================================ SLUS 0x8002E010
int32_t Len2(GuestRam& g, uint32_t v, const BikeTables& t) {
    const int32_t x = g.S32(v), y = g.S32(v + 4);
    return Shl(SqrtGte(Add(FixMul(x, x), FixMul(y, y)), t.sqrt), 2);
}

// ============================================================================ SLUS 0x8002DF14
bool Normalize2(GuestRam& g, uint32_t v, const BikeTables& t, int32_t& v0) {
    const int32_t len = Len2(g, v, t);
    v0 = 0;
    if (len == 0) return !g.Faulted();
    uint32_t inv;
    if (len >= 0) {
        const int32_t d = Add(len >> 1, Sub(len, 2) >> 31);
        inv = MipsDivU(0x80000000u, U(d));
    } else {
        const int32_t n = Neg(len);
        if (U(n) == 0x80000000u) return false;                              // addiu -2 traps
        const int32_t d = Add(n >> 1, Sub(n, 2) >> 31);
        inv = 0u - MipsDivU(0x80000000u, U(d));
    }
    const int32_t x = FixMul(S(inv), g.S32(v));
    const int32_t y = FixMul(S(inv), g.S32(v + 4));
    g.W32(v, U(x));
    g.W32(v + 4, U(y));
    v0 = 1;
    return !g.Faulted();
}

// ============================================================================ SLUS 0x8002DFC0
int32_t Dot2(GuestRam& g, uint32_t a, uint32_t b) {
    return Add(FixMul(g.S32(a), g.S32(b)), FixMul(g.S32(a + 4), g.S32(b + 4)));
}

// ============================================================================ RASHCDG 0x800ADC74
bool LeanPoleTest(GuestRam& g, uint32_t e, uint32_t s, int32_t radius, uint32_t push, uint32_t outLen, uint32_t sp,
                  const BikeTables& t, uint32_t& v0) {
    const uint32_t fr = sp - 280;
    g.W32(sp + 4, s);                                                          // a1..a3 into the home slots
    g.W32(sp + 8, U(radius));
    g.W32(sp + 12, push);
    int32_t s2 = 0;
    const uint32_t s8 = g.S32(e + 200) < g.S32(e + 236) ? 1u : 0u;            // corner 0 below corner 3
    const int32_t lean = g.S32(e + 676);
    const uint32_t yaw = g.U32(e + 636);
    const uint32_t m8 = 0u - s8;
    int32_t s3 = S(m8 & 1u) + 1;
    const int32_t i0 = S(m8 & 4u) + 3, i1 = S(m8 & 4u) + 2;
    int32_t s4 = MulLo(lean, 652) >> 16;                                       // 16.16 rad -> 4096 per turn
    const uint32_t s6 = (~yaw) >> 31;
    g.W8(fr + 192, static_cast<uint8_t>(i0));
    g.W8(fr + 193, static_cast<uint8_t>(i1));
    if (s6 != 0) s3 ^= 7;
    g.W8(fr + 195, static_cast<uint8_t>(i0 ^ 6));
    g.W8(fr + 196, static_cast<uint8_t>(i1 ^ 6));
    g.W8(fr + 194, static_cast<uint8_t>(s3));
    g.W8(fr + 197, static_cast<uint8_t>(s3 ^ 6));
    // the two side rows (x, z) << 4: the frame's own when its +0x20C is not near 1.0, else the rows
    if (Iabs(g.S16(e + 524)) < 4033) {
        g.W32(fr + 200, U(Shl(g.S16(e + 432), 4)));
        g.W32(fr + 204, U(Shl(g.S16(e + 436), 4)));
        g.W32(fr + 208, U(Shl(g.S16(e + 444), 4)));
        g.W32(fr + 212, U(Shl(g.S16(e + 448), 4)));
        int32_t r = 0;
        if (!Normalize2(g, fr + 200, t, r)) return false;
        if (!Normalize2(g, fr + 208, t, r)) return false;
    } else {
        g.W32(fr + 200, U(Shl(g.S16(e + 516), 4)));
        g.W32(fr + 204, U(Shl(g.S16(e + 520), 4)));
        g.W32(fr + 208, U(Shl(g.S16(e + 528), 4)));
        g.W32(fr + 212, U(Shl(g.S16(e + 532), 4)));
    }
    g.W32(fr + 216, U(Shl(g.S16(e + 438), 4)));
    g.W32(fr + 220, U(Shl(g.S16(e + 442), 4)));
    g.W32(fr + 224, U(Shl(g.S16(e + 450), 4)));
    g.W32(fr + 228, U(Shl(g.S16(e + 454), 4)));
    {
        int32_t r = 0;
        if (!Normalize2(g, fr + 224, t, r)) return false;
        if (!Normalize2(g, fr + 216, t, r)) return false;
        if (r != 0) {
            int32_t as = 0;
            if (!Asin(Dot2(g, fr + 200, fr + 216), t.asin, as)) return false;
            s2 = Sub(1024, as);
        }
    }
    s2 = Add(s2, Add(S(m8 & U(Sub(Sub(2048, s2), s2))), -1024));
    int32_t s7, f232;
    uint32_t s0;
    if (s8 == s6) {
        s7 = 7;
        if (s4 >= 0) {
            s4 = 1;
            f232 = 3;
            s0 = 306;
        } else {
            s7 = 1;
            f232 = 2;
            if (s4 < s2) {
                s4 = 3;
                s0 = 801;
            } else {
                s4 = 2;
                s0 = 561;
            }
        }
    } else {
        s7 = 0;
        if (s4 > 0) {
            f232 = 3;
            if (s4 < s2) {
                s4 = 2;
                s0 = 531;
            } else {
                s4 = 1;
                s0 = 291;
            }
        } else {
            s4 = 3;
            s7 = 2;
            f232 = 2;
            s0 = 786;
        }
    }
    g.W32(fr + 232, U(f232));
    // the octagon: the near corners as they are, the far ones swept back by dt x speed along the heading
    GScale(g, Neg(FixMul(g.S32(0x800CCE38u), g.S32(e + 480))), e + 450, fr + 144);
    const int32_t dx = g.S32(fr + 144), dz = g.S32(fr + 152);
    auto put = [&](int32_t slot, int32_t idxAt, bool swept) {
        const int32_t k = g.S8(fr + 192 + U(idxAt));
        const uint32_t cn = e + U(MulLo(k, 12));
        const uint32_t pt = fr + 16 + U(slot) * 8u;
        g.W32(pt, U(swept ? Add(g.S32(cn + 196), dx) : g.S32(cn + 196)));
        g.W32(pt + 4, U(swept ? Add(g.S32(cn + 204), dz) : g.S32(cn + 204)));
    };
    s3 = s4;
    put(s3, s3, false);
    for (int32_t j = 0; j < 3; ++j) put(s4 + 1 + j, s4 + j, true);
    s3 = (s4 + 3) % 6;
    put(s4 + 4, s3, true);
    put((s4 + 5) & 7, s3, false);
    s3 = (s3 + 1) % 6;
    put((s4 + 6) & 7, s3, false);
    s3 = (s3 + 1) % 6;
    put((s4 + 7) & 7, s3, false);
    // the edge normals (x, z)
    auto nrm = [&](uint32_t i, int32_t x, int32_t z) {
        g.W32(fr + 80 + 8 * i, U(x));
        g.W32(fr + 84 + 8 * i, U(z));
    };
    const int32_t f200 = g.S32(fr + 200), f204 = g.S32(fr + 204), f208 = g.S32(fr + 208), f212 = g.S32(fr + 212);
    const int32_t f216 = g.S32(fr + 216), f220 = g.S32(fr + 220), f224 = g.S32(fr + 224), f228 = g.S32(fr + 228);
    nrm(0, Neg(f208), Neg(f212));
    nrm(4, f208, f212);
    const uint32_t lo = s0 & 0xFu, mid = (s0 & 0xF0u) >> 4, hi = s0 >> 8;
    nrm(lo, Neg(f200), Neg(f204));
    nrm(lo + 4, f200, f204);
    if (s8 != 0) {
        nrm(mid, f220, Neg(f216));
        nrm(mid + 4, Neg(f220), f216);
    } else {
        nrm(mid, Neg(f220), f216);
        nrm(mid + 4, f220, Neg(f216));
    }
    nrm(hi, Neg(f228), f224);
    nrm(hi + 4, f228, Neg(f224));
    // the pole's centre against every edge: outside one by more than the radius -> no contact
    const uint32_t sh = g.U32(sp + 4);
    for (uint32_t k = 0; k < 8; ++k) {
        const int32_t d = Add(FixMul(g.S32(fr + 80 + 8 * k), Sub(g.S32(sh + 12), g.S32(fr + 16 + 8 * k))),
                              FixMul(g.S32(fr + 84 + 8 * k), Sub(g.S32(sh + 20), g.S32(fr + 20 + 8 * k))));
        g.W32(fr + 160 + 4 * k, U(d));
        if (d < Neg(g.S32(sp + 8))) {
            v0 = 6;
            return !g.Faulted();
        }
    }
    auto dAt = [&](int32_t i) { return g.S32(fr + 160 + 4 * U(i)); };
    int32_t s5 = 0;
    for (int32_t i = (s4 + 5) & 7; i != s4; i = (i + 1) & 7)
        if (dAt(i) < s5) {
            s5 = dAt(i);
            s3 = i;
        }
    int32_t s1;
    if (s5 < 0) {                                                              // inside the swept side
        s1 = Add(g.S32(sp + 8), s5);
        g.W32(push, U(FixMul(g.S32(fr + 80 + 8 * U(s3)), s1)));
        g.W32(push + 8, U(FixMul(g.S32(fr + 84 + 8 * U(s3)), s1)));
    } else {
        const int32_t s0i = (s7 + 2 - f232 + 8) & 7;
        {
            const int32_t v = Add(dAt(s0i), -9830);
            g.W32(fr + 160 + 4 * U(s0i), U(v < 0 ? 0 : v));
        }
        s5 = 0x140000;
        for (int32_t i = (s4 + 5) & 7; i != s4; i = (i + 1) & 7) {
            const int32_t tt = Dot2(g, fr + 80 + 8 * U(i), fr + 224);
            const int32_t a1 = Iabs(tt) < 132 ? 0x140000 : SDiv(dAt(i), Neg(tt));
            if (a1 < s5 && Neg(g.S32(sp + 8)) < a1) {
                s5 = a1;
                s3 = i;
            }
        }
        if (s5 == 0x140000) {
            v0 = 6;
            return !g.Faulted();
        }
        if (s3 == s0i) s5 = SDiv(Add(dAt(s3), 9830), Neg(Dot2(g, fr + 80 + 8 * U(s3), fr + 224)));
        s1 = Add(g.S32(sp + 8), s5);
        g.W32(push, U(FixMul(f224, Neg(s1))));
        g.W32(push + 8, U(FixMul(f228, Neg(s1))));
    }
    g.W32(push + 4, 0);
    g.W32(outLen, U(Iabs(s1)));
    if (s3 == s7 || s3 == ((s7 + 1) & 7)) v0 = U(f232);
    else v0 = f232 == 3 ? 0u : 3u;
    return !g.Faulted();
}

// ============================================================================ SLUS 0x80017B30
uint32_t GSurfaceSound(GuestRam& g, int32_t k) {
    const int32_t i = k < 0 ? 0 : (k > 51 ? 51 : k);
    return g.U8(kSurfaceTable + U(i));
}

// ============================================================================ RASHCDG 0x800A3CBC
bool PropUpRows(GuestRam& g, uint32_t p, const BikeTables& t) {
    int32_t sum = 0;
    rc::GteOp(g, p + 432, p + 522, p + 528);
    if (!rc::GNormalize(g, p + 528, t, sum)) return false;
    if (sum != 0) {
        rc::GteOp(g, p + 522, p + 528, p + 516);
        return !g.Faulted();
    }
    rc::GteOp(g, p + 522, p + 444, p + 516);
    if (!rc::GNormalize(g, p + 516, t, sum)) return false;
    if (sum == 0) {
        const uint32_t zero[] = {446, 444, 442, 438, 436, 434};
        for (uint32_t o : zero) g.W16(p + o, 0);
        g.W16(p + 448, 4096);
        g.W16(p + 440, 4096);
        g.W16(p + 432, 4096);
        const uint32_t zero2[] = {530, 528, 526, 522, 520, 518};
        for (uint32_t o : zero2) g.W16(p + o, 0);
        g.W16(p + 532, 4096);
        g.W16(p + 524, 4096);
        g.W16(p + 516, 4096);
    }
    rc::GteOp(g, p + 516, p + 522, p + 528);
    return !g.Faulted();
}

// ============================================================================ RASHCDG 0x800B3344
bool PropTopple(GuestRam& g, uint32_t p, uint32_t point, uint32_t n, uint32_t mode, uint32_t sp, const BikeTables& t,
                CollisionCallees& c, uint32_t& v0) {
    const uint32_t fr = sp - 80;
    v0 = 0;
    if (point != 0) {
        g.W32(fr + 16, 0);
        if (CornerMin(g, p + 196, n, point, fr + 48, 0) >= 8u) return !g.Faulted();
    }
    if (mode != 0) {
        GScale(g, g.S32(fr + 48), n, fr + 32);                               // the stale depth when no point
        ApplyImpulse(g, p, fr + 32, 1);
    }
    bool bounce = true;
    if (mode == 1) {
        if (32767 < g.S32(p + 484) && !(g.U32(p + 592) & 8u)) {
            g.W32(p + 484, 0);
        } else {
            int32_t k = GDot(g, p + 450, n);
            g.W32(fr + 48, U(k));
            if (k < S(0xFFFF199Au)) k = S(0xFFFF199Au);
            const uint32_t f = g.U32(p + 592);
            g.W32(fr + 48, U(k));
            if (f & 0x10u) {
                g.W32(p + 592, f & 0xFFFFFFF5u);
                if (!PropUpRows(g, p, t)) return false;
                g.W32(p + 484, 0x30000);
            } else {
                if (!(f & 8u)) {
                    if (GDot(g, p + 566, p + 522) < 0) g.W32(p + 592, g.U32(p + 592) | 0x40u);
                    if (g.U32(p + 540) == 0) {
                        g.W32(p + 544, 0);
                    } else {
                        int32_t v = Div4(Iabs(g.S32(p + 488)));
                        g.W32(p + 544, U(v));
                        if (v < 0x20000) v = 0x20000;
                        g.W32(p + 544, U(v));
                        if (g.S32(p + 540) > 0) g.W32(p + 544, U(Neg(v)));
                        int32_t w = g.S32(p + 488);
                        if (w > 0) {
                            if (w < 0x50000) w = 0x50000;
                        } else if (S(0xFFFB0000u) < w) {
                            w = S(0xFFFB0000u);
                        }
                        g.W32(p + 488, U(w));
                    }
                    g.W32(p + 592, g.U32(p + 592) | 8u);
                }
                g.W32(fr + 48, U(Half(g.S32(fr + 48))));
            }
            MulAdd16(g, p + 450, n, Neg(g.S32(fr + 48)), p + 456);
            g.W16(p + 450, static_cast<uint16_t>(g.S32(p + 456) >> 4));
            g.W16(p + 454, static_cast<uint16_t>(g.S32(p + 464) >> 4));
            g.W16(p + 452, static_cast<uint16_t>(g.S32(p + 460) >> 4));
            int32_t sum = 0;
            if (!rc::GNormalize(g, p + 450, t, sum)) return false;
            GScale(g, g.S32(p + 480), p + 450, p + 456);
            bounce = false;
        }
    }
    if (bounce) {
        g.W32(fr + 16, 5);
        g.W32(fr + 20, mode == 1 ? 0u : 0x60000u);
        g.W32(fr + 24, 0);
        bool ok = true;
        const int32_t b = Bounce(g, n, p + 450, p + 480, p + 456, 5, mode == 1 ? 0 : 0x60000, 0, t, ok);
        if (!ok) return false;
        if (b >= 0) {
            g.W16(p + 578, 0x8000);
            const uint32_t r1 = GRand(g);
            const int32_t s1 = S(r1 % 45752u) - 22876;
            const uint32_t r2 = GRand(g);
            const int32_t s0 = S(r2 % 45752u) - 22876;
            const uint32_t r3 = GRand(g);
            g.W32(p + 544, U(Shl(s1, 1)));
            g.W32(p + 552, U(Shl(s0, 1)));
            int32_t v = FixMul(g.S32(p + 488), S(r3 % 9831u) + 0xCCCC);
            g.W32(p + 488, U(v));
            if (0xA0000 < v) v = 0xA0000;
            g.W32(p + 488, U(v));
            const int32_t sp480 = g.S32(p + 480);
            int32_t a0 = 0x9FFFF < sp480 ? 0xA0000 : S(U(static_cast<int32_t>((static_cast<int64_t>(sp480) * 0x55555556LL) >> 32)) - U(sp480 >> 31));
            const int32_t a1 = g.S32(p + 488);
            int32_t a2 = 0x18000;
            if (a1 < 0) {
                a2 = S(0xFFFE8000u);
                a0 = Neg(a0);
            }
            const int32_t cy = Sub(MulLo(g.S16(p + 560), g.S16(p + 454)), MulLo(g.S16(p + 564), g.S16(p + 450)));
            a0 = Sub(a0, a1);
            int32_t w = Add(a1, S(U(Sub(a1, a2) >> 31) & U(Sub(a2, a1))));
            w = Add(w, S(U(a0 >> 31) & U(a0)));
            g.W32(p + 488, U(w));
            if ((cy < 0 && w > 0) || (cy > 0 && w < 0)) g.W32(p + 488, U(Neg(g.S32(p + 488))));
        }
        if (!PlaySound(c, g, p, S(GSurfaceSound(g, g.S32(p + 180)) + 14u))) return false;
    }
    v0 = 1;
    return !g.Faulted();
}

// ============================================================================ RASHCDG 0x800B2F94
bool PropKick(GuestRam& g, uint32_t p, uint32_t bike, uint32_t dir, uint32_t sp, const BikeTables& t,
              CollisionCallees& c, uint32_t& v0) {
    const uint32_t fr = sp - 32;
    if (g.U32(p + 592) & 2u) return PropTopple(g, p, 0, dir, 0, fr, t, c, v0);
    g.W32(p + 480, U(FixMul(0x11999, g.S32(bike + 480))));
    if (0x50000 < g.S32(bike + 480)) {
        const int32_t ang = static_cast<int16_t>(Mod714(GRand(g)));
        g.W16(p + 450, static_cast<uint16_t>(g.U16(bike + 450) + static_cast<uint32_t>(MulLo(ang, g.S16(bike + 454)) >> 12)));
        g.W16(p + 452, g.U16(bike + 452));
        const int32_t lo = MulLo(ang, g.S16(bike + 450)) >> 12;
        const int32_t y4 = Shl(g.S16(p + 452), 4);
        g.W16(p + 454, static_cast<uint16_t>(g.U16(bike + 454) - static_cast<uint32_t>(lo)));
        int32_t as = 0;
        if (!Asin(y4, t.asin, as)) return false;
        const uint32_t kind = ModelKind(g, p);
        const uint32_t up = (kind == 2u || kind == 5u) ? 1u : 0u;
        const uint32_t dn = (kind == 1u || kind == 4u) ? 1u : 0u;
        const int32_t ang2 = Sub(Add(Add(S((0u - dn) & U(-57)), S((0u - up) & 0x39u)), 1479), as);
        const int32_t cs = g.S16(kSinCos + (((U(ang2) & 0xFFFu) << 2) | 2u));
        const int32_t k = Add(S((0u - up) & 0x15439u), Add(S((0u - dn) & 0xFFFE3A5Eu), 0x633B6));
        if (!LaunchLift(g, p + 450, g.S32(p + 480), cs, k, fr, t)) return false;
        int32_t q = S(U(static_cast<int32_t>((static_cast<int64_t>(g.S32(bike + 480)) * 0x55555556LL) >> 32)) -
                      U(g.S32(bike + 480) >> 31));
        g.W32(p + 488, U(q));
        if (0xA0000 < q) q = 0xA0000;
        g.W32(p + 488, U(q));
        const int32_t s0 = static_cast<int16_t>(Mod714(GRand(g)));
        g.W16(p + 560, static_cast<uint16_t>(g.U16(p + 454) + static_cast<uint32_t>(MulLo(s0, g.S16(p + 450)) >> 12)));
        const uint32_t r = GRand(g);
        const int32_t z = MulLo(s0, g.S16(p + 454)) >> 12;
        g.W16(p + 578, 0);
        g.W16(p + 562, static_cast<uint16_t>(Mod714(r)));
        g.W16(p + 564, static_cast<uint16_t>(U(z) - g.U16(p + 450)));
        int32_t sum = 0;
        if (!rc::GNormalize(g, p + 560, t, sum)) return false;
        g.W32(p + 592, g.U32(p + 592) | 3u);
    } else {
        const int32_t spd = g.S32(p + 480);
        g.W16(p + 450, g.U16(bike + 450));
        g.W16(p + 452, g.U16(bike + 452));
        g.W16(p + 454, g.U16(bike + 454));
        GScale(g, spd, p + 450, p + 456);
    }
    if (!PlaySound(c, g, p, S(GSurfaceSound(g, g.S32(p + 180))))) return false;
    v0 = 1;
    return !g.Faulted();
}

// ============================================================================ RASHCDG 0x800B3838
bool PropKnock(GuestRam& g, uint32_t p, uint32_t dirF, uint32_t dirR, uint32_t dirU, int32_t speed, uint32_t sp,
               const BikeTables& t) {
    const uint32_t fr = sp - 64;
    int32_t s4 = 1024;
    auto copy = [&](uint32_t dst, uint32_t src, bool neg) {
        for (uint32_t k = 0; k < 3; ++k) {
            const uint16_t v = g.U16(src + 2 * k);
            g.W16(dst + 2 * k, neg ? static_cast<uint16_t>(0u - v) : v);
        }
    };
    if (GDot(g, p + 444, dirF) > 0) {
        copy(fr + 16, dirR, false);
        copy(fr + 22, dirU, false);
        copy(fr + 28, dirF, false);
        s4 = -1024;
    } else {
        copy(fr + 16, dirR, true);
        copy(fr + 28, dirF, true);
        copy(fr + 22, dirU, false);
    }
    const uint32_t r = GRand(g);
    const int32_t ang = S(r - 682u * static_cast<uint32_t>((static_cast<uint64_t>(r >> 1) * 0x300C0301u) >> 38)) - 341;
    RotateRowPair(g, fr + 16, fr + 28, ang, t.sincos);
    g.W32(p + 592, g.U32(p + 592) | 0x200u);
    RotateRowPair(g, fr + 22, fr + 28, Neg(s4), t.sincos);
    MatrixQuat(g, fr + 16, p + 572, t);
    const int32_t dq = Add(Add(Add(MulLo(g.S16(p + 572), g.S16(p + 560)), MulLo(g.S16(p + 574), g.S16(p + 562))),
                               MulLo(g.S16(p + 576), g.S16(p + 564))),
                           MulLo(g.S16(p + 578), g.S16(p + 566)));
    const uint32_t f = g.U32(p + 592) & ~0x40u;
    g.W32(p + 592, f);
    if (Shl(dq >> 14, 2) < 0) g.W32(p + 592, f | 0x40u);
    const int32_t v = FixMul(6443, Add(speed, S(0xFFFEFE82u)));
    g.W32(p + 484, U(v));
    g.W32(p + 488, U(Shl(v, 1)));
    const int32_t w = Add(v, g.S32(p + 556));
    g.W32(p + 484, U(w));
    if (0xC000 < w) g.W32(p + 484, 0x10000);
    return !g.Faulted();
}

// ============================================================================ RASHCDG 0x800B2D88
bool TrafficVsProp(GuestRam& g, uint32_t car, uint32_t p, uint32_t sp, const BikeTables& t, CollisionCallees& c) {
    const uint32_t fr = sp - 96;
    if (InCameraBox(g, p, 0x280000, 0x380000) == 0) return !g.Faulted();
    g.W32(fr + 16, fr + 64);
    g.W32(fr + 20, fr + 68);
    uint32_t corner = 0;
    if (!FirstPointInsideBox(g, p + 196, car + 432, car + 196, 16, fr + 64, fr + 68, fr, corner)) return false;
    if (corner == 8u) return !g.Faulted();
    g.W32(fr + 16, fr + 24);
    if (!FaceNormal(g, car + 196, car + 432, g.U32(fr + 64), fr + 40, fr + 24, fr)) return false;
    GScale(g, Add(g.S32(fr + 68), 8192), fr + 40, fr + 48);
    ApplyImpulse(g, p, fr + 48, 1);
    uint32_t r = 0;
    return PropKick(g, p, car, fr + 40, fr, t, c, r);
}

// ============================================================================ RASHCDG 0x80071A28
void QuatNormalize(GuestRam& g, uint32_t q, uint32_t sp, const BikeTables& t) {
    const uint32_t fr = sp - 56;
    int32_t v[4];
    for (uint32_t k = 0; k < 4; ++k) {
        v[k] = Shl(g.S16(q + 2 * k), 2);
        g.W32(fr + 16 + 4 * k, U(v[k]));
    }
    const int32_t s = Add(Add(Add(FixMul(v[0], v[0]), FixMul(v[1], v[1])), FixMul(v[2], v[2])), FixMul(v[3], v[3]));
    const int32_t inv = rc::Recip(Shl(SqrtGte(s, t.sqrt), 2));
    for (uint32_t k = 0; k < 4; ++k) g.W16(q + 2 * k, static_cast<uint16_t>(FixMul(v[k], inv) >> 2));
}

// ============================================================================ RASHCDG 0x8007198C
bool QuatMatrixT(GuestRam& g, uint32_t q, uint32_t out, uint32_t sp) {
    const uint32_t fr = sp - 56, m = fr + 16;
    const uint32_t fr2 = fr - 40;                                              // 0x800714A0, frame 40
    for (uint32_t k = 0; k < 4; ++k) g.W32(fr2 + 16 + 4 * k, U(Shl(g.S16(q + 2 * k), 2)));
    AnimPose pose(g);
    pose.QuatToMatrix(m, fr2 + 16);                                            // SLUS 0x8001005C
    if (pose.Failed()) return false;
    static const uint32_t kT[9] = {0, 3, 6, 1, 4, 7, 2, 5, 8};
    for (uint32_t k = 0; k < 9; ++k) g.W16(out + 2 * k, g.U16(m + 2 * kT[k]));
    return !g.Faulted();
}

// ============================================================================ RASHCDG 0x800A40D4
void PropFrameRows(GuestRam& g, uint32_t p, uint32_t sp, const BikeTables& t) {
    (void)sp;
    if (!(g.U32(p + 592) & 4u)) {
        rc::CopyHalfwords(g, 9, p + 516, p + 432);                            // SLUS 0x8003FA18
        return;
    }
    const bool flip = g.S16(p + 446) < 0;
    for (uint32_t k = 0; k < 3; ++k) g.W16(p + 432 + 2 * k, g.U16(p + 516 + 2 * k));
    for (uint32_t k = 0; k < 3; ++k) {
        const uint16_t up = g.U16(p + 522 + 2 * k), side = g.U16(p + 528 + 2 * k);
        g.W16(p + 444 + 2 * k, flip ? static_cast<uint16_t>(0u - up) : up);
        g.W16(p + 438 + 2 * k, flip ? side : static_cast<uint16_t>(0u - side));
    }
    if (g.S32(p + 180) == 2) RotateRowPair(g, p + 444, p + 438, flip ? 227 : -227, t.sincos);
}

// ============================================================================ RASHCDG 0x800A2A64
namespace {
void GBlend16(GuestRam& g, uint32_t a, uint32_t b, uint32_t out, int32_t wa, int32_t wb) {
    int16_t x[3], y[3], o[3];
    Read16x3(g, a, x);
    Read16x3(g, b, y);
    Blend16(x, y, o, wa, wb);
    Write16x3(g, out, o);
}
void Neg16x3(GuestRam& g, uint32_t a) {
    for (uint32_t k = 0; k < 3; ++k) g.W16(a + 2 * k, static_cast<uint16_t>(0u - g.U16(a + 2 * k)));
}
int32_t Mul25736(int32_t v) { return MulLo(v, 25736) >> 8; }
int32_t Ang652(int32_t v) { return MulLo(v, 652) >> 16; }
} // namespace

bool PropAnimPass(GuestRam& g, int32_t dt, uint32_t sp, const BikeTables& t, RecoverCallees& c) {
    const uint32_t F = sp - 136;
    const uint32_t tab = 0x800CE510u;                                          // the pool table's pool 4
    int32_t s7 = g.S32(g.U32(tab + 12));
    uint32_t e = g.U32(tab);
    RoadRuntimeNative road;
    for (; s7 >= 0; --s7, e += g.U32(tab + 4)) {
        if (g.U16(e + 172) == 0) continue;
        if (g.U32(e + 592) & 0x100u) {                                         // pushed: back onto the road
            GMulAdd(g, e + 184, e + 438, Neg(g.S32(e + 312) >> 1), e + 580);
            RoadRebindBody(g, e, F, road);                                     // 0x80037450
            if (!PropSettle(g, e, F, c)) return false;                         // 0x800A3A84
            const uint32_t v1 = g.U32(e + 592);
            if (!(v1 & 2u)) {
                if (v1 & 4u) {
                    GMulAdd(g, e + 504, e + 522, Neg(g.S32(e + 308)), e + 184);
                } else {
                    g.W32(e + 184, g.U32(e + 504));
                    g.W32(e + 188, g.U32(e + 508));
                    g.W32(e + 192, g.U32(e + 512));
                }
            }
            g.W32(e + 592, g.U32(e + 592) & ~0x100u);
        }
        if (g.U32(e + 592) & 1u) {                                             // 0x800A2B6C: knocked, first frame
            const int32_t s4 = GDot(g, e + 444, e + 560);
            int32_t sum = 0;
            if (0xF0A3 < Iabs(s4)) {
                for (uint32_t k = 0; k < 3; ++k) g.W16(e + 566 + 2 * k, g.U16(e + 438 + 2 * k));
            } else {
                rc::GteOp(g, e + 444, e + 560, e + 566);
                if (!rc::GNormalize(g, e + 566, t, sum)) return false;
            }
            rc::GteOp(g, e + 560, e + 566, e + 572);                           // 0x800A2C2C
            int32_t as = 0;
            if (!Asin(s4, t.asin, as)) return false;
            g.W32(e + 540, U(Mul25736(as)));
            g.W32(e + 556, 0);
            for (uint32_t k = 0; k < 3; ++k) g.W16(e + 528 + 2 * k, g.U16(e + 572 + 2 * k));
            for (uint32_t k = 0; k < 3; ++k) g.W16(e + 516 + 2 * k, g.U16(e + 566 + 2 * k));
            if (!Asin(GDot(g, e + 438, e + 566), t.asin, as)) return false;
            g.W32(e + 548, U(Mul25736(Sub(1024, as))));
            GScale(g, g.S32(e + 480), e + 450, e + 456);
            GMulAdd(g, e + 184, e + 438, Neg(g.S32(e + 312) >> 1), e + 580);
            g.W32(e + 592, (g.U32(e + 592) & 0xFFFFFFDEu) | 4u);
        }
        const uint32_t v1 = g.U32(e + 592);                                    // 0x800A2D80
        if (v1 & 2u) {                                                         // tumbling
            int32_t s5 = 0;
            if (v1 & 8u) {                                                     // 0x800A2D9C: fallen, rolling
                uint32_t s2 = 1;
                if (g.S32(e + 544) != 0) {
                    const int32_t old = g.S32(e + 540);
                    const int32_t nw = Add(old, FixMul(g.S32(e + 544), dt));
                    if ((nw > 0 && old > 0) || (nw < 0 && old < 0)) {
                        g.W32(e + 540, U(nw));
                    } else {
                        g.W32(e + 540, 0);
                        g.W32(e + 544, 0);
                    }
                }
                int32_t s4 = GDot(g, e + 566, e + 522);                        // 0x800A2DF4
                uint32_t f = g.U32(e + 592);
                bool skip = false;
                if (!(f & 0x20u)) {
                    if (f & 0x40u) {
                        if (s4 < 0) skip = true;
                    } else {
                        if (s4 > 0) skip = true;
                        f = g.U32(e + 592);
                    }
                    if (!skip && !(f & 0x20u)) {                               // 0x800A2E4C: lands on its side
                        g.W32(e + 592, f | 0x20u);
                        if (g.S32(e + 180) != 2) g.W32(e + 488, 0);
                        g.W32(e + 556, 0);
                    }
                }
                if (!skip) {                                                   // 0x800A2E70
                    s4 = GDot(g, e + 560, e + 522);
                    const uint32_t s5n = U(GDot(g, e + 572, e + 522)) >> 31;
                    if (!(Iabs(s4) < 6554)) {
                        MulAdd16(g, e + 560, e + 522, Neg(s4), F + 64);
                        g.W16(e + 560, static_cast<uint16_t>(g.S32(F + 64) >> 4));
                        g.W16(e + 562, static_cast<uint16_t>(g.S32(F + 68) >> 4));
                        s2 = 0;
                        g.W16(e + 564, static_cast<uint16_t>(g.S32(F + 72) >> 4));
                    } else {
                        rc::GteOp(g, e + 566, e + 522, e + 560);
                        if (s5n != 0) {
                            const uint16_t a0 = g.U16(e + 560), a2 = g.U16(e + 564);
                            g.W16(e + 560, static_cast<uint16_t>(0u - a0));
                            g.W16(e + 564, static_cast<uint16_t>(0u - a2));
                            g.W16(e + 562, static_cast<uint16_t>(0u - g.U16(e + 562)));
                        }
                    }
                    int32_t sum = 0;
                    if (!rc::GNormalize(g, e + 560, t, sum)) return false;         // 0x800A2F78
                    rc::GteOp(g, e + 522, e + 560, e + 566);
                    if (s2 == 0 && !rc::GNormalize(g, e + 566, t, sum)) return false;
                    rc::GteOp(g, e + 560, e + 566, e + 572);
                    if (s5n != 0) {
                        Neg16x3(g, e + 572);
                        Neg16x3(g, e + 566);
                    }
                }
                // 0x800A3094
                if (g.S32(e + 488) == 0 && g.S32(e + 544) == 0 && s2 != 0) g.W32(e + 592, g.U32(e + 592) | 0x10u);
            } else {                                                           // 0x800A30D0: still flying
                const uint32_t w = g.U16(e + 578);
                if (w != 0) {
                    int32_t s0;
                    if (dt < S(w)) {
                        g.W16(e + 578, static_cast<uint16_t>(w - U(dt)));
                        s0 = dt;
                    } else {
                        s0 = S(w);
                        g.W16(e + 578, 0);
                    }
                    if (g.S32(e + 544) != 0) g.W32(e + 540, U(Add(g.S32(e + 540), FixMul(g.S32(e + 544), s0))));
                    if (g.S32(e + 552) != 0) g.W32(e + 548, U(Add(g.S32(e + 548), FixMul(g.S32(e + 552), s0))));
                }
                g.W32(e + 488, U(FixMul(0xFEB8, g.S32(e + 488))));
                s5 = FixMul(g.S32(solid::kGravity), dt);
            }
            // 0x800A3174: the roll about the side
            if (g.S32(e + 488) != 0) {
                const int32_t a = Add(g.S32(e + 556), FixMul(g.S32(e + 488), dt));
                g.W32(e + 556, U(a));
                if (0x6487D < a) g.W32(e + 556, U(Add(a, S(0xFFF9B782u))));
                else if (!(S(0xFFF9B782u) < a)) g.W32(e + 556, U(Add(a, 0x6487E)));
                if (g.U32(e + 592) & 0x20u) {
                    if (!(g.S32(e + 556) < 22876)) {
                        g.W32(e + 556, 22876);
                        g.W32(e + 488, 0);
                    }
                } else {
                    const uint32_t ang = (U(MulLo(g.S32(e + 556), 163)) >> 14) & 0xFFFu;
                    const int32_t cs = Shl(g.S16(kSinCos + ((ang << 2) | 2u)), 4);
                    const int32_t sn = Shl(g.S16(kSinCos + (ang << 2)), 4);
                    g.W32(F + 16, U(sn));
                    GBlend16(g, e + 528, e + 516, e + 572, cs, sn);
                    g.W32(F + 16, U(cs));
                    GBlend16(g, e + 528, e + 516, e + 566, Neg(sn), cs);
                }
            }
            // 0x800A3284: the frame from the three angles and the tip frame
            int16_t ang[3];
            ang[0] = static_cast<int16_t>(-(((g.U32(e + 592) & 0x20u) && g.S32(e + 556) != 0) ? Ang652(g.S32(e + 556)) : 0));
            ang[1] = static_cast<int16_t>(-Ang652(g.S32(e + 540)));
            ang[2] = static_cast<int16_t>(-Ang652(g.S32(e + 548)));
            Write16x3(g, F + 80, ang);
            int16_t rt[9], b[9], o[9];
            RotMatrix(ang, rt, t.sincos);                                      // SLUS 0x8004D2A4
            for (uint32_t k = 0; k < 9; ++k) g.W16(F + 40 + 2 * k, static_cast<uint16_t>(rt[k]));
            for (uint32_t k = 0; k < 9; ++k) b[k] = g.S16(e + 560 + 2 * k);
            MulMatrix0(rt, b, o);                                              // three MVMVA
            for (uint32_t k = 0; k < 9; ++k) g.W16(e + 432 + 2 * k, static_cast<uint16_t>(o[k]));
            // gravity on the velocity, the speed and the heading from it
            {
                const int32_t vx = g.S32(e + 456);
                g.W32(e + 456, U(Sub(vx, FixMul(s5, vx))));
                const int32_t d1 = Sub(FixMul(s5, g.S32(e + 460)), FixMul(0x9D087, dt));
                g.W32(e + 460, U(Sub(g.S32(e + 460), d1)));
                g.W32(e + 464, U(Sub(g.S32(e + 464), FixMul(s5, g.S32(e + 464)))));
            }
            int32_t vel[3];
            Read32x3(g, e + 456, vel);
            const int32_t len = Length3(vel, t.sqrt);                          // SLUS 0x8002E548
            g.W32(e + 480, U(len));
            if (len != 0) GScaleTo16(g, rc::Recip(len), e + 456, e + 450);     // SLUS 0x8002EED8
            g.W32(e + 484, U(Add(g.S32(e + 484), dt)));
        } else if (v1 & 0x200u) {                                              // 0x800A3508: toppling
            if (g.S32(e + 488) != 0) {
                const int32_t a = Add(g.S32(e + 556), FixMul(g.S32(e + 488), dt));
                g.W32(e + 556, U(a));
                if (0xFFFF < a) {                                              // 0x800A36DC: there
                    for (uint32_t k = 0; k < 4; ++k) g.W16(F + 88 + 2 * k, g.U16(e + 572 + 2 * k));
                    const uint32_t f = g.U32(e + 592);
                    g.W32(e + 488, 0);
                    g.W32(e + 592, f | 0x1800u);
                } else {
                    const int32_t w1 = static_cast<int16_t>(a >> 2);
                    const bool other = (g.U32(e + 592) & 0x40u) != 0;
                    for (uint32_t k = 0; k < 4; ++k) {
                        const int32_t lo = MulLo(Sub(16384, w1), g.S16(e + 560 + 2 * k)) >> 14;
                        const int32_t w = other ? static_cast<int16_t>(-w1) : w1;
                        const int32_t hi = MulLo(w, g.S16(e + 572 + 2 * k)) >> 14;
                        g.W16(F + 88 + 2 * k, static_cast<uint16_t>(Add(lo, hi)));
                    }
                    QuatNormalize(g, F + 88, F, t);                            // 0x80071A28
                    if (!(g.S32(e + 556) < g.S32(e + 484))) {
                        const uint32_t f = g.U32(e + 592);
                        g.W32(e + 488, 0);
                        g.W32(e + 592, f | 0x1000u);
                    }
                }
                if (!QuatMatrixT(g, F + 88, e + 432, F)) return false;         // 0x8007198C
                if (!rc::Call(c, 0x8008BA18u, {e}, F)) return false;           // BuildObb
            }
            const uint32_t f = g.U32(e + 592);                                 // 0x800A3730
            if (f & 0x400u) g.W32(e + 592, f & ~0x400u);
            else if (g.S32(e + 488) == 0) g.W32(e + 592, f & ~0x200u);
        } else if (!(g.S32(e + 480) < 13108)) {                                // 0x800A3774: sliding
            int32_t r = Sub(g.S32(e + 484), dt);
            g.W32(e + 484, U(r));
            if (r < 0) r = 0;
            const uint32_t mask = g.S16(e + 524) < 3548 ? 0u : 0xFFFFFFFFu;
            g.W32(e + 484, U(r));
            g.W32(e + 484, U(Add(S(mask & U(Add(r, S(0xFFFF0000u)))), 0x10000)));
            const int32_t wa = FixMul(Shl(g.S16(e + 530), 4), 0x9D087);
            const int32_t wb = FixMul(Shl(g.S16(e + 518), 4), 0x9D087);
            {
                g.W32(F + 16, U(wb));
                int16_t x[3], y[3];
                int32_t o[3];
                Read16x3(g, e + 528, x);
                Read16x3(g, e + 516, y);
                Blend16To32(x, y, o, wa, wb);                                  // SLUS 0x8002ECB8
                Write32x3(g, F + 24, o);
            }
            const int32_t s0 = FixMul(g.S32(e + 484), dt);
            const int32_t w2 = FixMul(S(mask & 0xFFFF0000u), dt);
            {
                g.W32(F + 16, U(w2));
                int32_t x[3], y[3], o[3];
                Read32x3(g, F + 24, x);
                Read32x3(g, e + 456, y);
                Blend32(x, y, o, s0, w2);                                      // SLUS 0x8002E6F8
                Write32x3(g, F + 24, o);
            }
            for (uint32_t k = 0; k < 3; ++k) g.W32(e + 456 + 4 * k, U(Add(g.S32(e + 456 + 4 * k), g.S32(F + 24 + 4 * k))));
            int32_t vel[3];
            Read32x3(g, e + 456, vel);
            const int32_t len = Length3(vel, t.sqrt);
            g.W32(e + 480, U(len));
            const int32_t d = g.S32(e + 484);
            g.W32(e + 484, U(d > 0 ? Sub(d, dt) : 0));
            GScaleTo16(g, rc::Recip(g.S32(e + 480)), e + 456, e + 450);
            if (g.U32(e + 592) & 0x80u) {
                int32_t sum = 0;
                rc::GteOp(g, e + 516, e + 522, e + 528);
                if (!rc::GNormalize(g, e + 528, t, sum)) return false;
                rc::GteOp(g, e + 522, e + 528, e + 516);
                g.W32(e + 592, g.U32(e + 592) & ~0x80u);
            }
            PropFrameRows(g, e, F, t);                                         // 0x800A40D4
        } else {                                                               // 0x800A39D4: at rest
            g.W32(e + 488, 0);
            g.W32(e + 480, 0);
            g.W32(e + 464, 0);
            g.W32(e + 460, 0);
            g.W32(e + 456, 0);
        }
        // 0x800A39E8: the heading words
        const int32_t a = RatAtan2(Shl(g.S16(e + 450), 4), Shl(g.S16(e + 454), 4), t.atan);
        g.W32(e + 292, U(a));
        g.W32(e + 296, U(Shl(g.S16(kSinCos + (((U(a) & 0xFFFu) << 2) | 2u)), 4)));
        g.W32(e + 300, U(Shl(g.S16(kSinCos + ((g.U32(e + 292) & 0xFFFu) << 2)), 4)));
        if (g.Faulted()) return false;
    }
    return !g.Faulted();
}

// ============================================================================ the serve
bool ServeSolid(GuestRam& g, const CollCall& call, const BikeTables& t, CollisionCallees& c, uint32_t& v0, bool& ok) {
    auto need = [&](int k) { return call.n >= k; };
    const uint32_t* a = call.a;
    switch (call.fn) {
    case solid::kLeanPoleTest:
        v0 = 6;
        ok = need(5) && LeanPoleTest(g, a[0], a[1], S(a[2]), a[3], a[4], call.sp, t, v0);
        return true;
    case solid::kTrafficVsProp:
        v0 = 0;
        ok = need(2) && TrafficVsProp(g, a[0], a[1], call.sp, t, c);
        return true;
    case solid::kPropKick:
        v0 = 0;
        ok = need(3) && PropKick(g, a[0], a[1], a[2], call.sp, t, c, v0);
        return true;
    case solid::kPropTopple:
        v0 = 0;
        ok = need(4) && PropTopple(g, a[0], a[1], a[2], a[3], call.sp, t, c, v0);
        return true;
    case solid::kPropKnock:
        v0 = 0;
        ok = need(5) && PropKnock(g, a[0], a[1], a[2], a[3], S(a[4]), call.sp, t);
        return true;
    case solid::kPropUpRows:
        v0 = 0;
        ok = need(1) && PropUpRows(g, a[0], t);
        return true;
    case solid::kQuatNormalize:
        v0 = 0;
        if (need(1)) QuatNormalize(g, a[0], call.sp, t);
        ok = need(1) && !g.Faulted();
        return true;
    case solid::kQuatMatrixT:
        v0 = 0;
        ok = need(2) && QuatMatrixT(g, a[0], a[1], call.sp);
        return true;
    case solid::kPropFrameRows:
        v0 = 0;
        if (need(1)) PropFrameRows(g, a[0], call.sp, t);
        ok = need(1) && !g.Faulted();
        return true;
    case solid::kSurfaceSound:
        v0 = need(1) ? GSurfaceSound(g, S(a[0])) : 0u;
        ok = need(1) && !g.Faulted();
        return true;
    default:
        return false;
    }
}

} // namespace rr::sim
