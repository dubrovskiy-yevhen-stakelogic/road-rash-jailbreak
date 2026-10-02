#include "game/sim/passes.h"

#include "game/sim/ai.h"
#include "game/sim/coll_util.h"
#include "game/sim/fixed.h"
#include "game/sim/vec.h"

namespace rr::sim {

using namespace cu;

// ============================================================================ RASHCDG 0x8008AC80
bool WorldBikePass(GuestRam& g, int32_t dt, uint32_t entrySp, WorldBikePassCallees& c) {
    const uint32_t sp = entrySp - kTopPassFrame;
    if (!c.TrafficPass(dt, sp)) return false;                                  // 0x8008AC8C
    if (!c.BikeStep(dt, sp)) return false;                                     // 0x8008AC94
    if (!c.RiderOffPass(dt, sp)) return false;                                 // 0x8008AC9C
    if (!c.PropPass(dt, sp)) return false;                                     // 0x8008ACA4
    if (g.U32(kPedSwitchWord) != 0u && !c.PedPass(dt, sp)) return false;       // 0x8008ACB0 / 0x8008ACC0
    if (!c.Pool5Pass(sp)) return false;                                        // 0x8008ACC8
    if (!c.VolumePass(sp)) return false;                                       // 0x8008ACD0
    return !g.Faulted();
}

// ============================================================================ RASHCDG 0x8008ACE8
bool RiderEnginePass(GuestRam& g, int32_t dt, uint32_t entrySp, RiderEnginePassCallees& c) {
    const uint32_t sp = entrySp - kTopPassFrame;
    if (!c.RiderPass(dt, sp)) return false;                                    // 0x8008ACF4
    if (!c.PropAnimPass(dt, sp)) return false;                                 // 0x8008ACFC
    if (g.U32(kPedSwitchWord) != 0u && !c.PedRelease(dt, sp)) return false;    // 0x8008AD04 / 0x8008AD18
    if (!c.HazardPass(dt, sp)) return false;                                   // 0x8008AD20
    return !g.Faulted();
}

// ============================================================================ RASHCDG 0x8007B840
bool RiderPass(GuestRam& g, int32_t dt, uint32_t entrySp, RiderPassCallees& c) {
    constexpr uint32_t kPoolTable = 0x800CE4D0;
    const uint32_t sp = entrySp - kRiderPassWholeFrame;
    g.W32(sp + 248u, U(dt));                                                   // 0x8007B878
    const int32_t high = g.S32(g.U32(kPoolTable + 12u));
    g.W32(sp + 164u, U(high));                                                 // 0x8007B890
    if (g.Faulted()) return false;
    if (high >= 0 && !c.PoolLoop(dt, sp)) return false;                        // 0x8007B88C
    if (!c.ThrownWalk(dt, sp)) return false;                                   // 0x8007C23C
    if (!c.Engine(0x8005B2D8u, dt, sp)) return false;                          // 0x8007C9B0
    if (!c.Engine(0x8005B298u, dt, sp)) return false;                          // 0x8007C9C0
    if (!c.Engine(0x8005B350u, dt, sp)) return false;                          // 0x8007C9D4
    if (!c.ClassWalk(dt, sp)) return false;                                    // 0x8007C9DC
    if (!c.ImpactState(0x8005B350u, sp)) return false;                         // 0x8007DCD8
    if (!c.ImpactState(0x8005B2D8u, sp)) return false;                         // 0x8007DCE4
    {                                                                          // 0x8007DCEC..0x8007DDD4
        int32_t n = g.S32(g.U32(kPoolTable + 12u));
        uint32_t e = g.U32(kPoolTable);
        for (int guard = 0; n >= 0; --n, ++guard) {
            if (guard > 4096 || g.Faulted()) return false;
            if ((g.U32(e + 568u) & 0x08001800u) != 0u && !c.ListMigrate(e, dt, sp)) return false;
            const uint32_t r = g.U32(e + 852u);
            if (!(g.U32(r + 604u) < 2u) && g.S16(r + 320u) != 0 && !c.RiderGround(r, dt, sp)) return false;
            if (g.U8(g.U32(e + 852u) + 572u) & 0x10u) {
                const uint32_t pr = g.U32(g.U32(e + 856u) + 852u);
                if (!(g.U32(pr + 604u) < 2u) && g.S16(pr + 320u) != 0 && !c.RiderGround(pr, dt, sp)) return false;
            }
            e += g.U32(kPoolTable + 4u);
        }
    }
    if (!c.Heading(0x8005B298u, dt, sp)) return false;                         // 0x8007DDDC
    if (!c.Heading(0x8005B350u, dt, sp)) return false;                         // 0x8007DDEC
    if (!c.DownWalk(dt, sp)) return false;                                     // 0x8007DDF4
    {                                                                          // 0x8007E804..0x8007E834
        constexpr uint32_t kDormant = 0x8005B270;
        uint32_t node = g.U32(kDormant + 4u);
        for (int guard = 0; node != kDormant; ++guard) {
            if (guard > 4096 || g.Faulted()) return false;
            if (!c.DormantDrive(node - 1088u, dt, sp)) return false;
            node = g.U32(node + 4u);
        }
    }
    return !g.Faulted();
}

// ============================================================================ RASHCDG [0x8007C9DC, 0x8007DCD4)
namespace {

// Normalize as a caller that tests its v0 sees it: v0 is the sum of squares (the GTE's SQR and two
// TRAPPING adds); false where the console raises the overflow exception (collision.cpp's rule).
bool NormalizeN(GuestRam& g, uint32_t a, const uint16_t* rsqrt, int32_t& n) {
    int16_t v[3];
    Read16x3(g, a, v);
    const int64_t s1 = static_cast<int64_t>(v[0]) * v[0] + static_cast<int64_t>(v[1]) * v[1];
    if (s1 > INT32_MAX) return false;
    const int64_t s2 = s1 + static_cast<int64_t>(v[2]) * v[2];
    if (s2 > INT32_MAX) return false;
    if (!Normalize(v, rsqrt)) return false;
    Write16x3(g, a, v);
    n = static_cast<int32_t>(s2);
    return true;
}

// cop2 0x178000C with R11/R22/R33 = r[0..2] and IR1..3 = ir, the result to `out`.
void GOp(GuestRam& g, uint32_t r, uint32_t ir, uint32_t out) {
    int16_t d[3], i[3], o[3];
    Read16x3(g, r, d);
    Read16x3(g, ir, i);
    OuterProduct(d, i, o);
    Write16x3(g, out, o);
}

// The side row +0x32E = +0x20A x +0x1C2, normalised; a zero one is the first row +0x1B0.
bool SideRow(GuestRam& g, uint32_t e, const BikeTables& t) {
    GOp(g, e + 522u, e + 450u, e + 814u);
    int32_t n = 0;
    if (!NormalizeN(g, e + 814u, t.rsqrt, n)) return false;
    if (n == 0) {
        const uint16_t a = g.U16(e + 432u), b = g.U16(e + 434u), d = g.U16(e + 436u);
        g.W16(e + 814u, a);
        g.W16(e + 816u, b);
        g.W16(e + 818u, d);
    }
    return true;
}

// SLUS 0x8002ECB8 Blend16To32 over guest addresses.
void GBlend16To32(GuestRam& g, uint32_t a, uint32_t b, uint32_t out, int32_t wa, int32_t wb) {
    int16_t x[3], y[3];
    int32_t o[3];
    Read16x3(g, a, x);
    Read16x3(g, b, y);
    Blend16To32(x, y, o, wa, wb);
    Write32x3(g, out, o);
}
// SLUS 0x8002E6F8 Blend32 over guest addresses (`out` may be `a`).
void GBlend32(GuestRam& g, uint32_t a, uint32_t b, uint32_t out, int32_t wa, int32_t wb) {
    int32_t x[3], y[3], o[3];
    Read32x3(g, a, x);
    Read32x3(g, b, y);
    Blend32(x, y, o, wa, wb);
    Write32x3(g, out, o);
}
// The inline `mult` / `(lo >> 16) | (hi << 16)` dot of the frame's blended vector sp+24 with the
// contact normal +0x334 (s16 << 4), the three terms added z + (y + x).
int32_t ContactDot(GuestRam& g, uint32_t sp, uint32_t e) {
    const int32_t x = FixMul(g.S32(sp + 24u), Shl(g.S16(e + 820u), 4));
    const int32_t y = FixMul(g.S32(sp + 28u), Shl(g.S16(e + 822u), 4));
    const int32_t z = FixMul(g.S32(sp + 32u), Shl(g.S16(e + 824u), 4));
    return Add(z, Add(y, x));
}
// `(v >> 1) + ((v - 2) >> 31)` and the unsigned 0x80000000 / it, negated for a negative v: the
// reciprocal ScaleTo16 is handed (0x8007CF38..0x8007CF7C, the thrown walk's idiom).
int32_t Recip(int32_t r) {
    if (r >= 0) return S(MipsDivU(0x80000000u, U(Add(r >> 1, Sub(r, 2) >> 31))));
    const int32_t nr = Neg(r);
    return Neg(S(MipsDivU(0x80000000u, U(Add(nr >> 1, Sub(nr, 2) >> 31)))));
}
// The rider's +0x228 bit 15 when it is on the bike (mount < 2), and the passenger's the same way.
// `always` = the 0x8007D804 form, which stores the word back either way; else 0x8007DC54's.
void RiderFlag(GuestRam& g, uint32_t sp, uint32_t e, uint32_t r, bool always) {
    auto one = [&](uint32_t x) {
        const bool on = g.U32(x + 604u) < 2u;
        if (always) {
            uint32_t v = g.U32(x + 552u);
            if (on) v |= 0x8000u;
            g.W32(x + 552u, v);
        } else if (on) {
            g.W32(x + 552u, g.U32(x + 552u) | 0x8000u);
        }
    };
    one(r);
    if (g.U32(sp + 180u) != 0u) one(g.U32(g.U32(e + 856u) + 852u));
}

} // namespace

bool RiderPassClassWalk(GuestRam& g, int32_t dt, uint32_t sp, const BikeTables& t, ClassWalkCallees& c) {
    uint32_t node = g.U32(kClassListHead + 4u);
    g.W32(sp + 176u, node);                                                    // 0x8007C9E8
    for (int guard = 0; node != kClassListHead; ++guard) {
        if (guard > 4096 || g.Faulted()) return false;
        const uint32_t e = node - 1088u;
        const uint32_t r = g.U32(e + 852u);                                    // s7
        g.W32(sp + 180u, (g.U8(r + 572u) >> 4) & 1u);
        const uint32_t fc = g.U32(e + 568u);
        bool s6 = false;
        if (fc & 0x100u) {
            // ---------------------------------------------------------------- the fall clip, 0x8007CA2C
            if (g.U16(r + 544u) == 0u) {
                uint32_t done = 0;
                if (!c.ClipDone(g.U32(r + 540u), sp, done)) return false;
                if (done == 0u) {
                    g.W32(e + 772u, 0);
                    g.W32(e + 576u, 0);
                    g.W8(e + 849u, 0);
                    const uint32_t a = g.U32(r + 540u);
                    const uint32_t b = g.U8(g.U32(a + 4u) + U(MulLo(g.S32(a + 12u), 12)));
                    const uint32_t clip = g.U32(g.U32(g.U32(a + 40u) + 4u) + 4u * b);
                    const int32_t last = static_cast<int16_t>(static_cast<uint16_t>(g.U16(clip + 16u) - 1u));
                    int32_t s0 = SDiv(Shl(g.S32(a + 16u), 16), Shl(last, 16));
                    s0 = Add(S(~U(s0 >> 31) & U(s0)), S(U(Sub(0x10000, s0) >> 31) & U(Sub(0x10000, s0))));
                    g.W32(e + 652u, U(FixMul(s0, g.S32(e + 668u))));
                    g.W32(e + 772u, U(FixMul(Sub(0x10000, s0), Neg(Half(g.S32(e + 304u))))));
                    g.W32(e + 568u, g.U32(e + 568u) & 0xFFFFDFFFu);
                    goto d080;
                }
            }
            // 0x8007CB58: the rider is off - the slide
            if (g.U32(r + 604u) < 2u) goto d080;
            {
                int32_t a3 = Neg(Half(g.S32(e + 304u)));
                const int32_t v1 = Add(g.S32(e + 772u), 6553);
                g.W32(e + 772u, U(v1));
                if (v1 < a3) a3 = v1;
                g.W32(e + 772u, U(a3));
                g.W32(sp + 16u, g.U32(e + 732u));
                const bool upright = g.S16(e + 524u) < 3547;                   // s0
                GBlend16To32(g, e + 528u, e + 516u, sp + 24u, g.S32(e + 740u), g.S32(e + 732u));
                if (g.U32(e + 576u) == 0u && upright && ContactDot(g, sp, e) > 0) g.W32(e + 576u, 0x10000u);
                const uint32_t t1 = g.U32(e + 568u);
                if (t1 & 0x2000u) {
                    if (ContactDot(g, sp, e) > 0) g.W32(e + 568u, t1 & 0xFFFFDFFFu);
                    if (g.U32(e + 568u) & 0x2000u) {
                        g.W32(e + 576u, 0);
                        goto d080;
                    }
                }
                // 0x8007CD30: the velocity along the heading, the push and |v|
                GScale(g, g.S32(e + 480u), e + 450u, e + 456u);
                int32_t w = Sub(g.S32(e + 724u), dt);
                g.W32(e + 724u, U(w));
                const bool four = g.U32(r + 604u) == 4u;
                if (w < 0) w = 0;
                g.W32(e + 724u, four ? U(w) : 0u);
                if (upright) {
                    g.W32(e + 724u, 0x10000u);
                    g.W32(e + 764u, 0);
                } else {
                    uint32_t f = 0;
                    if (four) f = (g.U16(e + 172u) < NumPlayers(g)) ? 0u : 1u;
                    g.W32(e + 764u, ((0u - f) & 0xFFFD8000u) + 0xFFFF8000u);
                }
                const int32_t wa = FixMul(g.S32(e + 724u), dt);
                const int32_t wb = FixMul(g.S32(e + 764u), dt);
                g.W32(sp + 16u, U(wb));
                GBlend32(g, sp + 24u, e + 456u, sp + 24u, wa, wb);
                const int32_t x = Add(g.S32(e + 456u), g.S32(sp + 24u));
                g.W32(e + 456u, U(x));
                const int32_t y = Add(g.S32(e + 460u), g.S32(sp + 28u));
                g.W32(e + 460u, U(y));
                const int32_t z = Add(g.S32(e + 464u), g.S32(sp + 32u));
                g.W32(e + 464u, U(z));
                const int32_t sq = Add(FixMul(z, z), Add(FixMul(y, y), FixMul(x, x)));
                const int32_t old = g.S32(e + 576u);
                g.W32(e + 580u, U(sq));
                const int32_t spd = Shl(SqrtGte(sq, t.sqrt), 2);
                g.W32(e + 480u, U(spd));
                g.W32(e + 576u, U(spd));
                g.W32(e + 484u, U(SDiv(Sub(spd, old), dt)));
                if (spd != 0) GScaleTo16(g, Recip(spd), e + 456u, e + 450u);
                if (!SideRow(g, e, t)) return false;
                const int32_t ang = RatAtan2(GDot(g, e + 528u, e + 814u), GDot(g, e + 528u, e + 450u), t.atan);
                const int32_t lean = MulLo(ang, 0x6488) >> 8;
                g.W32(e + 676u, U(lean));
                g.W32(e + 688u, U(Add(lean, 0x3243F)));
            }
        d080:
            if (g.U32(e + 576u) != 0u) goto next;
            g.W32(e + 680u, 0);
            {
                const uint32_t lean = g.U32(e + 676u);
                g.W32(e + 724u, 0);
                if (lean == 0u) goto next;
            }
            {
                const uint16_t a = g.U16(e + 528u), b = g.U16(e + 530u), d = g.U16(e + 532u);
                g.W16(e + 450u, a);
                g.W16(e + 452u, b);
                g.W16(e + 454u, d);
            }
            if (!SideRow(g, e, t)) return false;
            g.W32(e + 676u, 0);
            goto next;
        }
        // -------------------------------------------------------------------- 0x8007D148
        if ((fc & 0xFu) == 0u && g.S32(e + 576u) > 0) {
            const int32_t v = FixMul(g.S32(g.U32(e + 556u) + 324u), g.S32(e + 580u));
            const int32_t k = g.S32(kClassSurfaceTable + U(Shl(static_cast<int8_t>(g.U8(e + 534u)), 2)));
            const int32_t f = Neg(FixMul(k, v));
            g.W32(e + 752u, U(f));
            const uint32_t fc2 = g.U32(e + 568u);
            if (fc2 & 0x80u) {
                const int32_t a0 = g.S32(e + 724u);
                int32_t m;
                if (a0 < S(0xFFFD0000u)) {
                    m = 10;
                } else if (a0 >= 0) {
                    m = 6;
                } else {
                    const int32_t hi = static_cast<int32_t>((static_cast<int64_t>(a0) * 0x2AAAAAAB) >> 32);
                    m = Sub(6, Sub(hi >> 13, a0 >> 31));
                }
                g.W32(e + 752u, U(MulLo(g.S32(e + 752u), m)));
            } else if (fc2 & 0x10u) {
                const int32_t l = Iabs(g.S32(e + 676u));
                g.W32(e + 752u, U(FixMul(Add(MulLo(l, 100), 0x10000), f)));
            }
            const int32_t acc = Add(g.S32(e + 760u), g.S32(e + 752u));
            g.W32(e + 764u, U(acc));
            g.W32(e + 484u, U(acc));
            int32_t sp2 = Add(g.S32(e + 576u), FixMul(acc, dt));
            if (sp2 < 0x23C36) sp2 = 0x23C36;
            g.W32(e + 576u, U(sp2));
            if ((g.U32(e + 388u) & 1u) && g.S16(e + 452u) < -2048) {
                const int32_t tt = Add(FixMul(0x3C267, Shl(g.S16(e + 452u), 4)), 0x2E133);
                g.W32(e + 576u, U(ClampLerpMin(g.S32(e + 576u), g.S32(g.U32(e + 556u) + 224u) >> 1, tt)));
            }
        }
        // 0x8007D2E0
        g.W32(e + 564u, g.U32(e + 564u) & 0xFFFFCFFFu);
        if (g.U32(e + 568u) & 0x80u) {
            // ---------------------------------------------------------------- the wobble, 0x8007D2F8
            if (g.U32(e + 856u) != 0u && g.U32(e + 1088u) != 0u) goto next;
            bool end = (g.U32(e + 560u) & 0x04000000u) != 0u || !(0x4FFFF < g.S32(e + 576u));
            if (!end && g.S32(e + 724u) > 0 && Iabs(g.S32(e + 636u)) > 0xCCCC) end = true;
            if (end) {
                if (g.U32(r + 604u) < 2u) goto next;
                g.W32(e + 560u, g.U32(e + 560u) & 0xFBFFFFFFu);
                g.W32(e + 568u, (g.U32(e + 568u) & 0xFFFFFF7Fu) | 0x00200840u);
                goto next;
            }
            int32_t spd = g.S32(e + 576u);
            if (spd > 0x10FFFF) {
                g.W32(e + 724u, U(Sub(g.S32(e + 724u), dt)));
                goto next;
            }
            int32_t k = 0x10000;
            if (spd > 0x4FFFF) k = Add(FixMul(-3276, Add(spd, S(0xFFEF0000u))), 26214);
            const int32_t a2 = g.S32(e + 724u);
            if (a2 < 0) {
                g.W32(e + 724u, 0);
            } else {
                const uint32_t per = MipsDivU(0x80000000u, U(Add(k >> 1, Sub(k, 2) >> 31)));
                const int32_t v1 = Add(a2, dt);
                g.W32(e + 724u, U(v1));
                if (!(v1 < MulLo(S(per), 3))) g.W32(e + 724u, U(Sub(v1, S(per))));
            }
            const int32_t ph = FixMul(k, g.S32(e + 724u));
            const uint32_t off = (U(ph) >> 2) & 0x3FFCu;
            const int32_t s0 = FixMul(0x10000, Shl(t.sincos[off / 2u], 4));
            spd = g.S32(e + 576u);
            int32_t s1 = (spd > 0x4FFFF) ? Add(FixMul(-3822, Add(spd, S(0xFFEF0000u))), 6553) : 0xCCCC;
            s1 = FixMul(s1, s0);
            const int32_t w = g.S32(e + 724u);
            if (w > 0xFFFF) {
                g.W32(e + 488u, U(s0));
                g.W32(e + 636u, U(s1));
            } else {
                const int32_t n1 = Add(FixMul(Sub(0x10000, w), g.S32(e + 488u)), FixMul(g.S32(e + 724u), s0));
                g.W32(e + 488u, U(n1));
                const int32_t n2 = Add(FixMul(Sub(0x10000, g.S32(e + 724u)), g.S32(e + 636u)), FixMul(g.S32(e + 724u), s1));
                g.W32(e + 636u, U(n2));
            }
            goto next;
        }
        // -------------------------------------------------------------------- 0x8007D544
        if (g.S32(e + 660u) != 0) g.W32(e + 656u, U(Add(g.S32(e + 656u), FixMul(g.S32(e + 660u), dt))));
        if (g.S32(e + 656u) != 0) {
            int32_t v1;
            if (g.S32(e + 660u) != 0) {
                const int32_t el = Add(g.S32(e + 724u), dt);
                g.W32(e + 724u, U(el));
                const int32_t h = Half(FixMul(g.S32(e + 660u), el));
                v1 = Add(g.S32(e + 664u), FixMul(g.S32(e + 724u), Sub(g.S32(e + 656u), h)));
            } else {
                v1 = Add(g.S32(e + 652u), FixMul(g.S32(e + 656u), dt));
            }
            g.W32(e + 652u, U(v1));
            const int32_t a1 = g.S32(e + 652u);
            if (Iabs(a1) > 0x1921E) {
                g.W32(e + 652u, a1 > 0 ? 0x1921Fu : 0xFFFE6DE1u);
                g.W32(e + 660u, 0);
                g.W32(e + 656u, 0);
            }
            g.W32(e + 672u, 0);
        } else if (g.S32(e + 640u) != 0) {
            const int32_t old = g.S32(e + 636u);
            const int32_t tg = g.S32(e + 644u);
            const int32_t nw = Add(old, FixMul(g.S32(e + 640u), dt));
            const bool keep = (nw >= tg && tg < old) || (nw <= tg && old < tg);
            const uint32_t mask = keep ? 0xFFFFFFFFu : 0u;
            const int32_t tg2 = g.S32(e + 644u);
            const uint32_t rate = g.U32(e + 640u);
            g.W32(e + 672u, 0);
            g.W32(e + 636u, U(Add(tg2, S(mask & U(Sub(nw, tg2))))));
            g.W32(e + 640u, mask & rate);
        }
        // 0x8007D6B4: the roll rate +0x248 into +0x1E8, clamped by the stat block's +0xE8
        if (g.S32(e + 584u) != 0) {
            const int32_t v1 = g.S32(e + 488u);
            const int32_t s0 = Add(v1, FixMul(g.S32(e + 584u), dt));
            if ((s0 < 0 && v1 >= 0) || (s0 > 0 && v1 <= 0)) {
                g.W32(e + 584u, 0);
                g.W32(e + 488u, 0);
            } else {
                g.W32(e + 488u, U(s0));
            }
            const int32_t lim = g.S32(g.U32(e + 556u) + 232u);
            const int32_t a0 = g.S32(e + 488u);
            const int32_t nl = Neg(lim);
            int32_t v = Add(a0, S(U(Sub(a0, nl) >> 31) & U(Sub(nl, a0))));
            const int32_t d = Sub(lim, a0);
            v = Add(v, S(U(d >> 31) & U(d)));
            g.W32(e + 488u, U(v));
        }
        {
            const uint32_t fc3 = g.U32(e + 568u);
            if (fc3 & 0x40u) {
                // ------------------------------------------------------------ the tip, 0x8007D75C
                if ((fc3 & 0x18000u) != 0u && (g.S32(e + 656u) ^ g.S32(e + 652u)) >= 0) {
                    const uint32_t v1 = g.S32(e + 652u) >= 0 ? 0x28000u : 0xFFFD8000u;
                    if (g.U32(e + 684u) != v1) {
                        const uint32_t lean = g.U32(e + 676u);
                        g.W32(e + 724u, 0);
                        g.W32(e + 688u, lean);
                    }
                    g.W32(e + 684u, v1);
                    g.W32(e + 568u, g.U32(e + 568u) | 0x20000u);
                } else {
                    g.W32(e + 684u, 0);
                }
                const int32_t a1 = Iabs(g.S32(e + 652u));
                if (a1 > 0xB2B8 || (g.U32(e + 568u) & 0x200000u) != 0u) RiderFlag(g, sp, e, r, true);
                if (a1 == 0x1921F) {
                    g.W32(e + 568u, (g.U32(e + 568u) & 0xFFDC7FBFu) | 0x900u);
                    uint32_t rc = 0;
                    if (!c.GetRCnt(0xF2000002u, sp, rc)) return false;
                    const int32_t id = (S((rc & 0xFFu) * 5u) >> 8) + 50;
                    if (!c.PlaySound3D(g.S32(e + 184u), g.S32(e + 192u), id, 0, sp)) return false;
                }
                goto next;
            }
            if (!(fc3 & 0x20u)) goto next;
        }
        // -------------------------------------------------------------------- the lean-back, 0x8007D8C4
        if (g.U32(e + 856u) != 0u && g.U32(e + 1088u) != 0u) s6 = 0 < g.S32(e + 676u);
        if (s6) {
            const int32_t a0 = Sub(g.S32(e + 688u), g.S32(e + 676u));
            int32_t q;
            if (a0 > 0) q = Add(FDiv(a0, 22876), S(0xFFFF0000u));
            else q = Sub(S(0xFFFF0000u), FDiv(Sub(g.S32(e + 676u), g.S32(e + 688u)), 22876));
            g.W32(e + 636u, U(FixMul(q, 0x860A)));
            if (g.S32(e + 676u) != g.S32(e + 688u)) goto next;
        } else if (g.S32(e + 636u) != 0) {
            goto db8c;
        }
        // 0x8007D974
        {
            g.W32(e + 488u, g.S32(e + 676u) > 0 ? 0x80000u : 0xFFF80000u);
            const uint16_t h0 = g.U16(e + 528u), h1 = g.U16(e + 530u);
            const int32_t up = Shl(g.S16(e + 452u), 4);
            const uint16_t h2 = g.U16(e + 532u);
            g.W32(e + 728u, 0);
            g.W16(e + 796u, h0);
            g.W16(e + 798u, h1);
            g.W16(e + 800u, h2);
            int32_t as = 0;
            if (!Asin(up, t.asin, as)) return false;
            const uint32_t idx = U(Sub(1251, as)) & 0xFFFu;
            uint32_t s0 = static_cast<uint16_t>(t.sincos[2u * idx + 1u]);
            const int32_t spd = g.S32(e + 576u);
            if (0x787EF < spd) {
                const int32_t q = spd > 0 ? Neg(FDiv(0x787EF, spd)) : FDiv(0x787EF, Neg(spd));
                const uint32_t a = U(q) >> 4;
                s0 = (S(a << 16) < S(s0 << 16)) ? s0 : a;
            }
            const int32_t want = static_cast<int16_t>(static_cast<uint16_t>(s0));
            if (want != g.S16(e + 452u)) {
                const int32_t s1 = Shl(want, 4);
                int32_t s3 = Shl(g.S16(e + 450u), 4), s4 = Shl(g.S16(e + 454u), 4);
                g.W16(e + 452u, static_cast<uint16_t>(s0));
                int32_t ss = Add(FixMul(s3, s3), FixMul(s4, s4));
                if (ss < 6) {
                    s3 = 0;
                    s4 = 655;
                    ss = 6;
                }
                int32_t res;
                if (Sub(0x10000, FixMul(s1, s1)) > 0) {
                    res = ss > 0 ? FDiv(Sub(0x10000, FixMul(s1, s1)), ss)
                                 : Neg(FDiv(Sub(0x10000, FixMul(s1, s1)), Neg(ss)));
                } else {
                    res = ss > 0 ? Neg(FDiv(Sub(FixMul(s1, s1), 0x10000), ss))
                                 : FDiv(Sub(FixMul(s1, s1), 0x10000), Neg(ss));
                }
                const int32_t sq = Shl(SqrtGte(res, t.sqrt), 2);
                g.W16(e + 450u, static_cast<uint16_t>(FixMul(sq, s3) >> 4));
                g.W16(e + 454u, static_cast<uint16_t>(FixMul(sq, s4) >> 4));
            }
            g.W32(e + 568u, (g.U32(e + 568u) & 0xFFFFFFDFu) | 0x40A00u);
            goto dc0c;
        }
    db8c:
        if (s6) goto next;
        if (g.S32(e + 676u) == g.S32(e + 688u) || g.U32(e + 576u) == 0u) {
            if (!(g.U32(e + 568u) & 0x20000u)) {
                g.W32(e + 644u, 0);
                g.W32(e + 640u, g.S32(e + 636u) > 0 ? 0xFFFB8000u : 0x48000u);
                g.W32(e + 584u, g.S32(e + 488u) > 0 ? 0xFFFD0000u : 0x30000u);
                g.W32(e + 568u, g.U32(e + 568u) | 0x20000u);
            }
        }
    dc0c:
        if (s6) goto next;
        {
            const int32_t y = g.S32(e + 636u);
            const bool hit = (g.U32(e + 568u) & 0x200u) != 0u || !(0x8609u < U(y)) || (S(0xFFFF79F6u) < y && y < 0);
            if (hit) RiderFlag(g, sp, e, r, false);
        }
    next:
        node = g.U32(g.U32(sp + 176u) + 4u);                                   // 0x8007DCBC
        g.W32(sp + 176u, node);
    }
    return !g.Faulted();
}

// ============================================================================ SLUS [0x8001CD00, 0x8001CD7C)
uint32_t PadPollClock(GuestRam& g, uint32_t paused) {
    constexpr uint32_t kGs = 0x8005B2F8;
    uint32_t s0 = paused;
    uint32_t gs = g.U32(kGs);
    if (g.S8(gs + 3u) != 0) {                                                  // 0x8001CD04
        g.W8(gs, 1);                                                           // 0x8001CD14
        s0 = 1;
    }
    gs = g.U32(kGs);
    if (g.S8(gs) == 1) {                                                       // 0x8001CD28
        if (s0 == 0) {
            g.W32(gs + 0x18u, g.U32(gs + 0x0Cu) - g.U32(gs + 0x14u));          // 0x8001CD5C
            return s0;
        }
        if (g.S8(gs + 2u) != 0) {                                              // 0x8001CD40
            g.W32(gs + 0x1Cu, 15);
            g.W32(gs + 0x18u, 15);
            return s0;
        }
    }
    g.W32(gs + 0x1Cu, 0);                                                      // 0x8001CD74
    g.W32(gs + 0x18u, 0);
    return s0;
}

// ============================================================================ SLUS 0x8001C428
void FrameDelta(GuestRam& g) {
    constexpr uint32_t kGs = 0x8005B2F8;
    const uint32_t gs = g.U32(kGs);
    const uint32_t clock = g.U32(gs + 0x0Cu);
    const uint32_t n = g.U32(kFrameDeltaCount) + 1u;
    g.W32(kFrameDeltaCount, n);
    if (n == 60u) {
        g.W32(kFrameDeltaCount, 0);
        g.W32(gs + 0x64u, 0);
    }
    const uint32_t gs2 = g.U32(kGs);
    const uint32_t delta = (static_cast<uint32_t>(g.U8(gs2)) - 3u < 2u) ? 0u : clock - g.U32(kFrameDeltaPrev);
    g.W32(gs2 + 0x20u, delta);
    g.W32(kFrameDeltaPrev, clock);
}

// ============================================================================ SLUS 0x8003F708
namespace {
constexpr uint32_t kPoolTableR = 0x800CE4D0;
// 0x8003F9D8: is e's handle one of the table's 18 entries 0x800D5DA8 + 16 k (the word compared whole)?
bool InFinishTable(GuestRam& g, uint32_t e) {
    if (e == 0u) return false;
    const uint32_t h = g.U16(e + 0xACu);
    for (uint32_t k = 0; k < 18u; ++k)
        if (g.U32(kFinishOrderTable + 16u + 16u * k) == h) return true;
    return false;
}
// 0x8003F8D8: the unplaced rider furthest ahead, or 0.
uint32_t NextUnplaced(GuestRam& g) {
    uint32_t best = 0;
    int32_t bestProgress = 0;
    int32_t n = g.S32(g.U32(kPoolTableR + 12u));
    uint32_t e = g.U32(kPoolTableR);
    for (int guard = 0; n >= 0; --n, ++guard) {
        if (guard > 4096 || g.Faulted()) return 0;
        const uint32_t rd = g.U32(e + 0x43Cu);
        const bool racer = g.U16(e + 0xACu) < NumPlayers(g) || (g.U8(rd + 1u) & 0xFu) != 2u;
        if (racer && g.U8(g.U32(e + 0x43Cu) + 0x27u) < 248u && !InFinishTable(g, e) &&
            (bestProgress == 0 || g.S32(e + 0x144u) < bestProgress)) {
            bestProgress = g.S32(e + 0x144u);
            best = e;
        }
        e += g.U32(kPoolTableR + 4u);
    }
    return best;
}
} // namespace

bool ResultsPrepare(GuestRam& g, ResultsCallees& c) {
    uint32_t place = 1;
    int32_t clock = g.S32(g.U32(0x8005B2F8u) + 0x10u);
    uint32_t next = 1;                                                         // s1
    for (;;) {                                                                 // 0x8003F73C
        const uint32_t rec = kFinishOrderTable + 16u * place;
        next = place;
        if (g.U32(rec) == 0xFFFFFFFFu) break;
        clock = g.S32(rec + 12u);
        ++place;
        if (!(S(place) < 19)) {
            next = place;
            break;
        }
    }
    while (S(next) < 19) {                                                     // 0x8003F768
        const uint32_t e = NextUnplaced(g);
        if (e == 0u) break;
        g.W8(g.U32(e + 0x43Cu) + 0x27u, static_cast<uint8_t>(place));
        const int32_t rate = MulLo(g.S32(g.U32(e + 0x22Cu) + 0xE0u) >> 8, 0xE666) >> 16;
        const int32_t eta = MipsDiv(MulLo(g.S32(e + 0x144u) >> 4, 300), rate);
        g.W32(g.U32(e + 0x43Cu) + 0x28u, U(eta >= 0 ? Add(clock, eta) : clock));
        ++place;
        ++next;
        if (!c.RecordFinish(g.U16(e + 0xACu), 0)) return false;               // 0x8003F82C
        if (g.Faulted()) return false;
    }
    for (uint32_t k = 0; k < 18u; ++k) {                                       // 0x8003F840
        const uint32_t rec = kFinishOrderTable + 16u + 16u * k;
        if (g.U32(rec) == 0xFFFFFFFFu) continue;
        const uint32_t e = g.U32(0x8005B3A0u) + U(MulLo(g.U16(rec), 1096));
        if (e == 0u) continue;
        g.W8(g.U32(e + 0x43Cu) + 0x27u, g.U8(rec + 4u));
        g.W32(g.U32(e + 0x43Cu) + 0x28u, g.U32(rec + 12u));
    }
    return !g.Faulted();
}

} // namespace rr::sim
