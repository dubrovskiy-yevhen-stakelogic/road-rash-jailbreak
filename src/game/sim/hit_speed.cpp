#include "game/sim/hit_speed.h"

#include "game/sim/coll_util.h"
#include "game/sim/crash.h"
#include "game/sim/fixed.h"
#include "game/sim/vec.h"

namespace rr::sim {
namespace {

using cu::Add;
using cu::Half;
using cu::Iabs;
using cu::MulLo;
using cu::Neg;
using cu::S;
using cu::SDiv;
using cu::Shl;
using cu::Sub;
using cu::U;

constexpr uint32_t kBikesPtr = 0x8005B3A0; // `lui 0x8006; lw -19552`: the pool-0 base
constexpr int32_t kPi2 = 0x1921F;          // pi / 2 in 16.16

// The engine angle (4096 per turn) in 16.16 radians: `x 25736` (the sll/addu chain), low word, `sra 8`.
int32_t Rad(int32_t ang) { return MulLo(ang, 25736) >> 8; }

// cop2 0x178000C (OP, sf = 1) with R11/R22/R33 = r[0..2] and IR1..3 = ir, the IRs stored to out.
void GOp(GuestRam& g, uint32_t r, uint32_t ir, uint32_t out) {
    int16_t d[3], i[3], o[3];
    cu::Read16x3(g, r, d);
    cu::Read16x3(g, ir, i);
    OuterProduct(d, i, o);
    cu::Write16x3(g, out, o);
}

// Normalize as a caller that tests its v0 sees it: v0 is the sum of squares (GTE SQR and two TRAPPING adds);
// false where the console raises the overflow exception.
bool NormalizeN(GuestRam& g, uint32_t a, const uint16_t* rsqrt, int32_t& n) {
    int16_t v[3];
    cu::Read16x3(g, a, v);
    const int64_t s1 = static_cast<int64_t>(v[0]) * v[0] + static_cast<int64_t>(v[1]) * v[1];
    if (s1 > INT32_MAX) return false;
    const int64_t s2 = s1 + static_cast<int64_t>(v[2]) * v[2];
    if (s2 > INT32_MAX) return false;
    if (!Normalize(v, rsqrt)) return false;
    cu::Write16x3(g, a, v);
    n = static_cast<int32_t>(s2);
    return true;
}

// The OP pair every re-aim of this family makes: side = up x dir into +0x32E, Normalize (a zero side takes
// the lateral row +0x1B0), dir = side x up.
bool Reproject(GuestRam& g, uint32_t e, uint32_t dir, const BikeTables& t) {
    GOp(g, e + 522, dir, e + 814);
    int32_t n = 0;
    if (!NormalizeN(g, e + 814, t.rsqrt, n)) return false;
    if (n == 0) {
        const uint16_t x = g.U16(e + 432), y = g.U16(e + 434), z = g.U16(e + 436);
        g.W16(e + 814, x);
        g.W16(e + 816, y);
        g.W16(e + 818, z);
    }
    GOp(g, e + 814, e + 522, dir);
    return true;
}

// +0x2A4 = the angle of the side row +0x32E against the heading +0x1C2, both seen along +0x210.
void SideAngle(GuestRam& g, uint32_t e, const BikeTables& t) {
    const int32_t d1 = cu::GDot(g, e + 528, e + 814);
    const int32_t d2 = cu::GDot(g, e + 528, e + 450);
    g.W32(e + 676, U(Rad(RatAtan2(d1, d2, t.atan))));
}

// flagsC bit 22 := +0x16C < 0 (the opening of HitSpeed and TakePartnerHeading).
void BackwardBit(GuestRam& g, uint32_t e) {
    const int32_t dirn = g.S32(e + 364);
    uint32_t f = g.U32(e + 568);
    f = dirn < 0 ? (f | 0x00400000u) : (f & 0xFFBFFFFFu);
    g.W32(e + 568, f);
}

// 0x80081218 / 0x80081288: tell a mounted rider (+0x25C <u 2) the new speed and heading.
void RiderHit(GuestRam& g, uint32_t r, int32_t s3, uint32_t dir) {
    g.W32(r + 480, U(s3));
    g.W16(r + 456, g.U16(dir));
    g.W16(r + 458, g.U16(dir + 2));
    const uint32_t f = g.U32(r + 552);
    const uint16_t z = g.U16(dir + 4);
    g.W32(r + 552, f | 0x00200000u);
    g.W16(r + 460, z);
}

// The latch at the head of HitOutcome and ImpactTurn: +0x234 bit 14 cleared; once per pass (flagsC bit 25) the
// pre-contact heading +0x360 and speed +0x35C.
void Latch(GuestRam& g, uint32_t e) {
    g.W32(e + 564, g.U32(e + 564) & 0xFFFFBFFFu);
    if (!(g.U32(e + 568) & 0x02000000u)) {
        const uint16_t h0 = g.U16(e + 450), h1 = g.U16(e + 452), h2 = g.U16(e + 454);
        const uint32_t f = g.U32(e + 568);
        const uint32_t spd = g.U32(e + 480);
        g.W16(e + 864, h0);
        g.W16(e + 866, h1);
        g.W16(e + 868, h2);
        g.W32(e + 860, spd);
        g.W32(e + 568, f | 0x02000000u);
    }
}

// sum of FixMul(v[i], n[i] << 4) over the three components, the `mult; (lo >> 16) | (hi << 16)` idiom, and the
// raw 64-bit product of the third term (which the original stores to its frame at `lohi`).
int32_t DotMH(GuestRam& g, const int32_t v[3], uint32_t n, uint32_t lohi) {
    int32_t s = 0;
    for (uint32_t k = 0; k < 3; ++k) {
        const int32_t h = Shl(g.S16(n + 2u * k), 4);
        const int64_t p = static_cast<int64_t>(v[k]) * static_cast<int64_t>(h);
        if (k == 2) {
            g.W32(lohi, static_cast<uint32_t>(p));
            g.W32(lohi + 4, static_cast<uint32_t>(static_cast<uint64_t>(p) >> 32));
        }
        s = Add(s, S(static_cast<uint32_t>(p >> 16)));
    }
    return s;
}

// The sign-armed FixDiv idiom that yields -(a / b) (0x80081A70, 0x80081B80, 0x80081BEC).
int32_t NegDiv(int32_t a, int32_t b) { return Neg(SDiv(a, b)); }

} // namespace

// ============================================================================ RASHCDG 0x80080D1C
bool HitSpeed(GuestRam& g, uint32_t e, uint32_t dir, uint32_t pSpeed, uint32_t shape, uint32_t nrm, int32_t a5,
              int32_t a6, int32_t a7, int32_t a8, int32_t a9, int32_t a10, uint32_t sp, const BikeTables& t,
              uint32_t& v0) {
    const uint32_t fr = sp - 80u;
    g.W32(sp + 12, shape);                                                     // 0x80080D20 home spills
    g.W32(sp + 8, pSpeed);                                                     // 0x80080D58
    BackwardBit(g, e);                                                         // 0x80080D8C
    const uint32_t pool = static_cast<uint32_t>(g.U16(shape)) >> 5;
    const uint32_t a0 = g.U32(e + 568);

    int32_t s3 = 0;
    int32_t s0 = 1;
    bool s8 = false;
    auto tail = [&](int32_t ret) {                                             // 0x80081CF8
        if (s8) {
            g.W32(pSpeed, U(s3));                                              // 0x80081D10
            cu::GScale(g, s3, dir, e + 456);
        } else {
            g.W32(e + 576, U(s3));                                             // 0x80081D1C
        }
        s0 = ret;
    };
    auto latch = [&]() {                                                       // 0x80081D20
        for (uint32_t k = 0; k < 3; ++k) g.W16(e + 820 + 2u * k, g.U16(nrm + 2u * k));
        v0 = U(s0);
        return !g.Faulted();
    };

    if (a0 & 0x100u) {
        // ---- AIR: reflect and halve
        if (!(0x4786B < g.S32(e + 576))) {
            g.W32(e + 576, 0);                                                 // 0x80080DC8
        } else {
            const int32_t v = cu::GDot(g, dir, nrm);
            MulAdd16(g, dir, nrm, Neg(Shl(v, 1)), e + 456);
            for (uint32_t k = 0; k < 3; ++k) g.W16(dir + 2u * k, static_cast<uint16_t>(g.S32(e + 456 + 4u * k) >> 4));
            if (!Reproject(g, e, dir, t)) return false;
            const int32_t h = FixMul(0x8000, g.S32(e + 576));
            g.W32(pSpeed, U(h));                                               // 0x80080F0C
            g.W32(e + 576, U(h));                                              // 0x80080F10
            cu::GScale(g, g.S32(pSpeed), dir, e + 456);
            SideAngle(g, e, t);                                                // 0x80080F74
        }
        g.W32(e + 568, g.U32(e + 568) & 0xFFFFFFF0u);                          // 0x80080F84
        const uint32_t b = (pool == 3 || pool == 5 || pool == 6) ? 1u : 0u;
        g.W32(e + 568, g.U32(e + 568) | (b << 13));                            // 0x80080FC0
        s0 = 1;
        return latch();
    }

    // ---- 0x80080FC4
    s8 = (a0 & 0x600u) != 0;
    const uint32_t a1f = a0 | ((static_cast<uint32_t>(!s8) & ((a0 >> 4) & 1u)) << 27);
    const uint32_t a2f = a1f & 0xFFFFEFEFu;
    {
        const int32_t old = g.S32(e + 576);
        const int32_t pv = g.S32(pSpeed);
        g.W32(e + 568, a2f);                                                   // 0x80081000
        s3 = s8 ? pv : old;
    }
    enum class Path { Slow, Exch, Main } path = Path::Exch;
    if (!s8 && !(0x4786B < s3)) {
        if (a1f & 2u) path = Path::Slow;
        else if (!(g.U32(e + 564) & 0x4000u)) path = Path::Main;
        else if (a8 < g.S32(pSpeed)) path = Path::Slow;
    }

    if (path == Path::Slow) {
        // ---- 0x80081058: the bike stops
        const uint32_t f230 = g.U32(e + 560);
        g.W32(e + 568, a2f & 0xFFFFFFFCu);                                     // 0x80081060
        g.W32(e + 728, 0);                                                     // 0x8008106C
        if (!(f230 & 0x300u)) s3 = 0;
        uint32_t a1 = 0;
        g.W32(e + 564, (g.U32(e + 564) & 0xFFFFBFFFu) | 0x1000u);              // 0x8008108C
        bool test = true;
        if (pool == 0) {
            const uint32_t other = g.U32(kBikesPtr) + 1096u * g.U16(shape);
            if (g.U32(other + 560) & 0x2000u) test = false;
        }
        if (test && (g.U32(e + 560) & 0x08002000u) == 0x08000000u) a1 = 1;
        g.W32(e + 560, g.U32(e + 560) | ((0u - a1) & 0x6000u));                // 0x80081100
        tail(0);
        return latch();
    }

    if (path == Path::Exch && (g.U32(e + 564) & 0x4000u)) {
        // ---- 0x80081118: THE EXCHANGE SPEED, at least 1.0
        s3 = a8 < 0x10000 ? 0x10000 : a8;
        if (!s8 && g.U32(e + 616) != 0) {
            tail(1);
            return latch();
        }
        int32_t k;
        if (0xA0000 < s3) {
            k = 0x38000;
        } else {
            const int32_t q = s3 > 0 ? cu::FDiv(s3, 0xA0000) : Neg(cu::FDiv(Neg(s3), 0xA0000));
            k = Add(FixMul(S(0xFFFD8000u), q), 0x60000);
        }
        const int32_t v = FixMul(0x40000, k);
        const uint32_t f268 = g.U32(e + 616);
        g.W32(e + 624, U(v));                                                  // 0x800811A8
        g.W32(e + 632, 0);                                                     // 0x800811B0
        g.W32(e + 620, 0xFFFE0000u);                                           // 0x800811B4
        g.W32(e + 628, f268);                                                  // 0x800811B8
        if (g.S32(pSpeed) < a8) {
            const int32_t x = g.S32(e + 624), y = g.S32(e + 620);
            g.W32(e + 624, U(Neg(x)));                                         // 0x800811E8
            g.W32(e + 620, U(Neg(y)));                                         // 0x800811F0
        }
        tail(1);
        return latch();
    }

    // ---- 0x800811F4: MAIN
    const int32_t s4 = Iabs(a6);
    {
        const uint32_t r = g.U32(e + 852);
        if (g.U32(r + 604) < 2u) RiderHit(g, r, s3, dir);                      // 0x80081218..44
        if (g.U8(g.U32(e + 852) + 572) & 0x10u) {
            const uint32_t p = g.U32(g.U32(e + 856) + 852);                    // 0x80081264: no null test
            if (g.U32(p + 604) < 2u) RiderHit(g, p, s3, dir);                  // 0x80081288..B4
        }
    }
    uint32_t fl;
    {
        const uint16_t d0 = g.U16(dir);
        fl = g.U32(e + 568);
        g.W16(e + 808, d0);                                                    // 0x800812C0
        g.W16(e + 810, g.U16(dir + 2));
        g.W16(e + 812, g.U16(dir + 4));
    }
    const uint32_t t1 = fl & 1u;
    g.W32(fr + 32, t1);
    bool crash = false;
    if (!s8 && a10 == 0 && (fl & 0xCu) == 4u) {
        if (g.S32(e + 616) >= 17158) crash = true;
        else if (57189 < a6 && g.S32(e + 676) < -34314) crash = true;
        else if (!(-57190 < a6) && 34314 < g.S32(e + 676)) crash = true;
    }
    if (crash) {
        // ---- 0x80081360: CRASH (flagsC 0x200)
        const uint32_t f = g.U32(e + 568);
        g.W32(e + 488, 0xFFF80000u);                                           // 0x80081378
        const uint16_t hx = g.U16(e + 450);
        s3 = 0x50000;
        g.W16(e + 798, 0);                                                     // 0x80081384
        g.W32(e + 728, 0);                                                     // 0x80081388
        const uint16_t hz = g.U16(e + 454);
        g.W32(e + 568, (f & 0xFFF00600u) | 0x200u);                            // 0x8008139C
        g.W16(e + 800, static_cast<uint16_t>(0u - hx));                        // 0x800813A0
        g.W16(e + 796, hz);                                                    // 0x800813A8
        const int32_t v = cu::GDot(g, dir, nrm);
        MulAdd16(g, dir, nrm, Neg(Shl(v, 1)), fr + 16);
        g.W32(pSpeed, 0x50000u);                                               // 0x800813D0
    } else {
        if (s8 && (g.U32(e + 568) & 4u)) {
            // ---- 0x800813F0: straight back
            for (uint32_t k = 0; k < 3; ++k) g.W32(fr + 16 + 4u * k, U(Neg(Shl(g.S16(dir + 2u * k), 4))));
        } else {
            // ---- 0x80081434: PLANE, the horizontal tangent of nrm on the heading's side
            const int32_t d0 = g.S16(dir);
            const int32_t n2 = g.S16(nrm + 4);
            const int32_t n0 = g.S16(nrm);
            const int32_t d2 = g.S16(dir + 4);
            if (Sub(MulLo(n2, d0), MulLo(n0, d2)) > 0) {
                g.W32(fr + 16, U(Shl(n2, 4)));
                g.W32(fr + 24, U(Neg(Shl(g.S16(nrm), 4))));
            } else {
                g.W32(fr + 16, U(Neg(Shl(n2, 4))));
                g.W32(fr + 24, U(Shl(g.S16(nrm), 4)));
            }
            int32_t y = 0;
            if (s8 && 0 < g.S16(dir + 2)) y = Shl(g.S16(dir + 2), 4);
            g.W32(fr + 20, U(y));
        }
        const uint32_t f = g.U32(e + 568);
        g.W32(e + 568, a6 < 0 ? (f | 0x00100000u) : (f & 0xFFEFFFFFu));        // 0x800814F0
    }
    // ---- 0x800814F4
    for (uint32_t k = 0; k < 3; ++k) g.W16(dir + 2u * k, static_cast<uint16_t>(g.S32(fr + 16 + 4u * k) >> 4));
    if (!s8 && !Reproject(g, e, dir, t)) return false;
    fl = g.U32(e + 568);
    g.W32(e + 828, shape);                                                     // 0x80081610
    if (fl & 0x200u) {
        tail(1);
        return latch();
    }

    if (t1) {
        // ---- 0x80081624: BUMP
        const int32_t q = SDiv(s4, a7);
        const int32_t v = FixMul(-1311, q);
        s3 = FixMul(Add(v, 0xFEB8), s3);
        const int32_t lo = Add(s3, (Sub(s3, 0x20000) >> 31) & Sub(0x20000, s3));
        const int32_t dd = Sub(0xA0000, s3);
        const int32_t sc = Add(lo, (dd >> 31) & dd);                           // clamp(s3, 2.0, 10.0)
        uint32_t rr;
        if (sc >= 0) {
            const int32_t dv = Add(sc >> 1, Sub(sc, 2) >> 31);
            rr = cu::MipsDivU(0x80000000u, U(dv));
        } else {
            const int32_t nn = Neg(sc);
            const int32_t dv = Add(nn >> 1, Sub(nn, 2) >> 31);
            rr = 0u - cu::MipsDivU(0x80000000u, U(dv));
        }
        g.W32(e + 728, rr);                                                    // 0x80081708
        const int32_t av = a6 > 0 ? Add(a6, a9) : Add(a6, Neg(a9));
        const int32_t yaw = FixMul(av, sc);
        g.W32(e + 488, U(yaw));                                                // 0x80081748
        bool big = pool == 8;
        if (!big && pool == 6)
            big = 0x10000 < g.S32(shape + 132) && 0x10000 < g.S32(shape + 136) && 0x10000 < g.S32(shape + 140);
        if (big) {
            // ---- 0x8008179C: a pool-8 or big pool-6 partner turns the steering back toward 0
            if ((yaw ^ g.S32(e + 636)) >= 0) {
                g.W32(e + 640, 0);                                             // 0x800817B4
                tail(1);
                return latch();
            }
            const int32_t f27c = g.S32(e + 636);
            g.W32(e + 644, 0);                                                 // 0x800817C0
            int32_t r = FixMul(Iabs(f27c), sc);
            g.W32(e + 640, U(r));                                              // 0x800817E4
            if (r < 0x10000) r = 0x10000;
            const int32_t f27c2 = g.S32(e + 636);
            g.W32(e + 640, U(r));                                              // 0x800817F8
            g.W32(e + 640, U(f27c2 >= 0 ? Neg(r) : r));                        // 0x80081804
            tail(1);
            return latch();
        }
        // ---- 0x80081808: the steering target +0x284 and its rate +0x280
        uint32_t a3 = 0;
        if (pool == 0) {
            const uint32_t other = g.U32(kBikesPtr) + 1096u * g.U16(shape);
            if (g.U32(other + 568) & 1u) {
                const int32_t f27c = g.S32(e + 636);
                if ((f27c ^ g.S32(e + 488)) < 0) a3 = (f27c ^ g.S32(other + 636)) < 0 ? 1u : 0u;
            }
        }
        int32_t k = s4 < 17157 ? 5719 : 11438;
        if (34314 < s4) k += 5719;
        const int32_t f1e8 = g.S32(e + 488);
        const int32_t f27c = g.S32(e + 636);
        const int32_t tgt = f1e8 < 0 ? Sub(f27c, k) : Add(f27c, k);
        g.W32(e + 644, U(tgt));                                                // 0x800818B8
        uint32_t keep = 1;
        if (a3 != 0) keep = (tgt ^ g.S32(e + 636)) < 0 ? 1u : 0u;
        const int32_t kept = S((0u - keep) & g.U32(e + 644));
        const uint32_t stat = g.U32(e + 556);
        g.W32(e + 644, U(kept));                                               // 0x800818EC
        const int32_t lim = g.S32(stat + 228);
        const int32_t nl = Neg(lim);
        int32_t cl = Add(kept, (Sub(kept, nl) >> 31) & Sub(nl, kept));
        const int32_t tt = Sub(lim, kept);
        cl = Add(cl, (tt >> 31) & tt);                                         // clamp(kept, -lim, lim)
        const int32_t f27c3 = g.S32(e + 636);
        g.W32(e + 644, U(cl));                                                 // 0x80081930
        const int32_t r = FixMul(Sub(Iabs(cl), Iabs(f27c3)), sc);
        int32_t ar = Iabs(r);
        g.W32(e + 640, U(r));                                                  // 0x80081954
        if (ar < 0x10000) ar = 0x10000;
        const int32_t f27c4 = g.S32(e + 636), f284 = g.S32(e + 644);
        g.W32(e + 640, U(ar));                                                 // 0x80081980
        g.W32(e + 640, U(f27c4 < f284 ? ar : Neg(ar)));                        // 0x8008198C
        tail(1);
        return latch();
    }

    // ---- 0x80081990: GLANCE
    {
        const int32_t q = SDiv(Sub(kPi2, s4), Sub(kPi2, a7));
        s3 = FixMul(FixMul(6553, q), s3);
    }
    if (s8 && s3 < 0x30000) s3 = 0x30000;
    {
        const int32_t x = g.S32(e + 616);
        g.W32(e + 616, U(x > 0 ? 0 : x));                                      // 0x80081A20
    }
    const int32_t nh = Half(Neg(g.S32(pSpeed)));
    const uint32_t fl2 = g.U32(e + 568);
    g.W32(e + 620, U(nh));                                                     // 0x80081A50
    if (fl2 & 8u) {
        const int32_t aa = Add(Iabs(g.S32(e + 616)), 0xDF66);
        g.W32(e + 728, U(NegDiv(aa, nh)));                                     // 0x80081A88 / A4 / BC
        g.W32(e + 624, 0);                                                     // 0x80081AC4
    } else {
        int32_t v1 = Add(nh, (Add(nh, 0x140000) >> 31) & Sub(S(0xFFEC0000u), nh));
        const int32_t tt = Sub(S(0xFFFC0000u), nh);
        v1 = Add(v1, (tt >> 31) & tt);                                         // clamp(-speed / 2, -20.0, -4.0)
        const int32_t f268 = g.S32(e + 616);
        g.W32(e + 620, U(v1));                                                 // 0x80081AFC
        const int32_t a1v = Add(Iabs(f268), 0xB2B8);
        const uint32_t fl3 = g.U32(e + 568);
        g.W32(e + 624, U(a1v));                                                // 0x80081B20
        if (!(fl3 & 4u)) {
            int32_t c = Add(a5, (Add(a5, 0x10000) >> 31) & Sub(S(0xFFFF0000u), a5));
            const int32_t na = Neg(a5);
            c = Add(c, (na >> 31) & na);                                       // clamp(a5, -1.0, 0)
            g.W32(e + 624, U(Neg(FixMul(c, a1v))));                            // 0x80081B5C
        }
        int32_t a0v = g.S32(e + 624);
        if (a0v < 11438) a0v = 11438;
        g.W32(e + 624, U(a0v));                                                // 0x80081B78
        a0v = Shl(a0v, 1);
        const int32_t f26c = g.S32(e + 620);
        const int32_t v = NegDiv(a0v, f26c);
        const int32_t f26c2 = g.S32(e + 620);
        g.W32(e + 728, U(v));                                                  // 0x80081BE8
        const int32_t w = NegDiv(f26c2, v);
        const uint32_t f268b = g.U32(e + 616);
        g.W32(e + 624, U(w));                                                  // 0x80081C30
        const uint32_t fl4 = g.U32(e + 568);
        g.W32(e + 632, 0);                                                     // 0x80081C38
        g.W32(e + 628, f268b);                                                 // 0x80081C3C
        g.W32(e + 728, U(Shl(g.S32(e + 728), 1)));                             // 0x80081C50
        if (fl4 & 2u) {
            if (pool == 3) {
                if (g.U32(shape + 480) == 0) s3 = 0;                           // 0x80081C80: car + 0x28C
            } else if (pool == 5 || pool == 6) {
                s3 = 0;
            }
        }
    }
    // ---- 0x80081C94: YAW
    {
        const int32_t f2d8 = g.S32(e + 728);
        const int32_t yv = f2d8 > 0 ? cu::FDiv(1143, f2d8) : Neg(cu::FDiv(1143, Neg(f2d8)));
        g.W32(e + 488, U(yv));                                                 // 0x80081CB0 / C0
        int32_t y1 = a6 >= 0 ? yv : Neg(yv);
        const uint32_t fl5 = g.U32(e + 568);
        g.W32(e + 488, U(y1));                                                 // 0x80081CE8
        if (fl5 & 0xCu) y1 = Shl(y1, 2);
        g.W32(e + 488, U(y1));                                                 // 0x80081CF0
    }
    tail(1);
    return latch();
}

// ============================================================================ RASHCDG 0x80080B10
bool TakePartnerHeading(GuestRam& g, uint32_t e, uint32_t dir, uint32_t pSpeed, uint32_t src, int32_t speed,
                        uint32_t sp, const BikeTables& t, uint32_t& v0) {
    (void)sp; // frame 32 holds only the saved registers
    BackwardBit(g, e);                                                         // 0x80080B54
    if (!(0x1FFFF < speed)) {
        g.W32(e + 568, (g.U32(e + 568) & 0xFFFFFFCFu) | 0x08000000u);          // 0x80080B84
        v0 = 0;
        return !g.Faulted();
    }
    for (uint32_t k = 0; k < 3; ++k) g.W16(dir + 2u * k, g.U16(src + 2u * k));  // 0x80080B90..A8
    if (g.U32(e + 568) & 0x600u) {
        g.W32(pSpeed, U(speed));                                               // 0x80080BC0
        cu::GScale(g, speed, dir, e + 456);
        v0 = 1;
        return !g.Faulted();
    }
    GOp(g, e + 522, dir, e + 814);                                             // 0x80080C24..2C
    int32_t n = 0;
    if (!NormalizeN(g, e + 814, t.rsqrt, n)) return false;
    if (n == 0) {
        const uint16_t x = g.U16(e + 432), y = g.U16(e + 434), z = g.U16(e + 436);
        g.W16(e + 814, x);
        g.W16(e + 816, y);
        g.W16(e + 818, z);
    }
    const uint32_t p = g.U32(e + 856);
    g.W32(e + 576, U(speed));                                                  // 0x80080C5C
    g.W32(e + 488, 0);                                                         // 0x80080C64
    if (p != 0) g.W32(p + 488, 0);                                             // 0x80080C68
    SideAngle(g, e, t);                                                        // 0x80080CC4
    const uint32_t fl = g.U32(e + 568);
    g.W32(e + 680, 0);                                                         // 0x80080CD0
    if (fl & 0x100u) {
        const uint32_t v = g.S32(e + 636) > 0 ? 0x20000u : 0xFFFE0000u;
        const uint32_t f = g.U32(e + 568);
        g.W32(e + 656, v);                                                     // 0x80080CEC
        g.W32(e + 568, f & 0xFFFFFFCFu);                                       // 0x80080CF8
    }
    v0 = 1;
    return !g.Faulted();
}

// ============================================================================ RASHCDG 0x80083928
bool HitOutcome(GuestRam& g, uint32_t e, uint32_t o, uint32_t n, uint32_t sp, const BikeTables& t, uint32_t& v0) {
    const uint32_t fr = sp - 152u;
    cu::GScale(g, g.S32(o + 480), o + 450, fr + 48);                           // the partner's velocity
    Latch(g, e);                                                               // 0x8008397C .. 0x800839BC
    const uint32_t pdir = e + 864, pspd = e + 860;
    g.W32(fr + 92, pdir);
    const uint32_t f = g.U32(e + 568);
    g.W32(fr + 96, pspd);
    const uint32_t b9 = f & 0x200u;
    bool bounce = b9 != 0;
    if (!bounce && (f & 0x400u)) bounce = !(Iabs(g.S16(n + 2)) < 2048);
    if (bounce) {
        // ---- 0x80083A0C: crashed, or airborne onto a floor
        const int32_t k5 = b9 ? 5 : 0;
        g.W32(fr + 16, U(k5));
        g.W32(fr + 20, 0x30000u);
        g.W32(fr + 24, 0);
        bool ok = false;
        const int32_t r = Bounce(g, n, g.U32(fr + 92), g.U32(fr + 96), e + 456, k5, 0x30000, 0, t, ok);
        if (!ok) return false;
        v0 = U(r);
        if (r < 0) return !g.Faulted();
        if (!Spin(g, e)) return false;
        return !g.Faulted();
    }
    // ---- 0x80083A5C
    cu::GScale(g, g.S32(e + 860), pdir, e + 456);
    int32_t rel[3];
    for (uint32_t k = 0; k < 3; ++k) {
        rel[k] = Sub(g.S32(e + 456 + 4u * k), g.S32(fr + 48 + 4u * k));
        g.W32(fr + 64 + 4u * k, U(rel[k]));
    }
    const int32_t s5 = DotMH(g, rel, n, fr + 104);                             // the closing speed along n
    const int32_t d1 = cu::GDot(g, g.U32(fr + 92), n);
    const int32_t d2 = cu::GDot(g, e + 814, n);
    int32_t s4 = Rad(RatAtan2(d2, Neg(d1), t.atan));
    s4 = Sub(s4 > 0 ? kPi2 : -kPi2, s4);                                       // the hit angle: pi/2 - t
    g.W32(fr + 88, 0x1226Cu);
    if ((s4 ^ g.S32(e + 488)) >= 0) g.W32(fr + 88, 0xF5BEu);
    const int32_t s6 = cu::GDot(g, g.U32(fr + 92), o + 450);
    if (!(s5 < 1)) {                                                           // separating
        v0 = 0;
        return !g.Faulted();
    }
    int32_t s2;
    if (g.S32(fr + 88) < Iabs(s4)) {
        // head-on / rear-end
        const bool a2 = s5 < S(0xFFEE1E50u), v1 = s5 < S(0xFFDC3C9Fu);
        s2 = 0xC417 < s6 ? 0 : 2;
        if (a2 && !v1) s2 = 4;
        if (v1) s2 = 8;
        if (s2 == 0) s2 = -1;                                                  // the EXCHANGE
        else if (g.U32(e + 560) & 0x20000000u) s2 = 2;
    } else {
        // side-on
        const bool a2 = 0xB333 < cu::GDot(g, o + 450, n);
        const bool v1 = s5 < S(0xFFF94B5Eu), a3 = s5 < S(0xFFF4D2F2u);
        s2 = ((a2 && v1 && !a3) ? 15 : 0) + ((a2 && a3) ? 32 : 1);
        if (s2 == 32 && (g.U32(e + 560) & 0x20000000u)) s2 = 16;
        else if (s2 == 1 && g.U32(e + 828) == o + 172u) s2 = 0;                // the same partner: no new bump
    }
    if (s2 == 0) {
        v0 = 0;
        return !g.Faulted();
    }
    {
        const uint32_t fc = g.U32(e + 568);
        if (fc & 0xFu) {
            const uint32_t m = fc & 0xFFF007F0u;
            g.W32(e + 568, m);                                                 // 0x80083D48
            g.W32(e + 828, 0);                                                 // 0x80083D50
            g.W32(e + 568, m | 0x08000000u);                                   // 0x80083D54
        }
    }
    if (s2 >= 16 || s2 == -1) {
        // ---- 0x80083D6C: the momentum exchange
        const int32_t me = g.S32(e + 316);
        const int32_t tot = Add(me, g.S32(o + 316));
        const int32_t dv = Add(tot >> 1, Sub(tot, 2) >> 31);
        const int32_t inv = S(cu::MipsDivU(0x80000000u, U(dv)));
        const int32_t s1 = FixMul(me, inv);
        const int32_t s0 = FixMul(g.S32(o + 316), inv);
        const int32_t v2 = FixMul(g.S32(o + 480), Iabs(s6));
        g.W32(fr + 16, fr + 80);
        g.W32(fr + 20, fr + 84);
        MassExchange(g, s1, s0, g.S32(e + 480), v2, fr + 80, fr + 84);
    } else {
        g.W32(fr + 80, g.U32(g.U32(fr + 96)));
    }
    if (s2 == -1) g.W32(e + 564, g.U32(e + 564) | 0x4000u);                   // 0x80083E20
    else g.W32(e + 568, g.U32(e + 568) | U(s2));                               // 0x80083E30 THE CLASS
    uint32_t r = 0;
    if (s2 < 16) {
        const int32_t k = FixMul(s5, 3664);
        const uint32_t lim = g.U32(fr + 88), a8 = g.U32(fr + 80);
        const uint32_t st[7] = {n, U(k), U(s4), lim, a8, 1143u, 0u};
        for (uint32_t i = 0; i < 7; ++i) g.W32(fr + 16 + 4u * i, st[i]);
        if (!HitSpeed(g, e, g.U32(fr + 92), g.U32(fr + 96), o + 172, n, k, s4, S(lim), S(a8), 1143, 0, fr, t, r))
            return false;
    } else {
        const uint32_t a8 = g.U32(fr + 80);
        g.W32(fr + 16, a8);
        if (!TakePartnerHeading(g, e, g.U32(fr + 92), g.U32(fr + 96), o + 450, S(a8), fr, t, r)) return false;
    }
    {
        const uint32_t fc = g.U32(e + 568);
        const uint32_t b = (fc & 0x1Fu) ? 0x1000u : ((fc & 0x220u) ? 0x800u : 0u);
        const uint32_t f2 = g.U32(e + 568) | b;
        g.W32(e + 568, f2);                                                    // 0x80083EDC
        if (f2 & 0x130u) g.W8(o + 509, static_cast<uint8_t>(g.U8(o + 509) | 0x10u));   // 0x80083EF8
    }
    v0 = r;
    return !g.Faulted();
}

// ============================================================================ RASHCDG 0x80083F30
bool ImpactTurn(GuestRam& g, uint32_t e, uint32_t shape, uint32_t n, int32_t mode, uint32_t sp, const BikeTables& t,
                uint32_t& v0) {
    const uint32_t fr = sp - 120u;
    g.W32(sp + 4, shape);                                                      // 0x80083F64: the caller's home slot
    Latch(g, e);                                                               // 0x80083F74 .. 0x80083FB4
    const uint32_t s7 = e + 864, s8 = e + 860;
    const uint32_t f = g.U32(e + 568);
    const uint32_t b9 = f & 0x200u;
    bool bounce = b9 != 0;
    if (!bounce && ((f >> 10) & 1u)) bounce = !(Iabs(g.S16(n + 2)) < 2048);
    if (bounce) {
        // ---- 0x80084004
        const int32_t k5 = b9 ? 5 : 0;
        g.W32(fr + 16, U(k5));
        g.W32(fr + 20, 0x30000u);
        g.W32(fr + 24, 0);
        bool ok = false;
        const int32_t r = Bounce(g, n, s7, s8, e + 456, k5, 0x30000, 0, t, ok);
        if (!ok) return false;
        v0 = U(r);
        if (r < 0) return !g.Faulted();
        if (!Spin(g, e)) return false;
        return !g.Faulted();
    }
    // ---- 0x80084054
    int32_t s4 = 0, s0 = 0, s1 = 0;
    if (mode != 2) {
        int32_t vel[3];
        cu::Read32x3(g, e + 456, vel);
        s4 = DotMH(g, vel, n, fr + 72);                                        // the velocity into the normal
        if (mode < 2) {
            s0 = mode == 1 ? 11438 : -11438;                                   // +-10 degrees
        } else {
            int32_t v;
            if (g.U32(e + 568) & 0x600u) {
                // ---- 0x80084124: the flattened heading
                g.W16(fr + 50, 0);
                g.W16(fr + 48, g.U16(e + 450));
                g.W16(fr + 52, g.U16(e + 454));
                if (!cu::GNormalize(g, fr + 48, t.rsqrt)) return false;
                const uint16_t uz = g.U16(fr + 52), ux = g.U16(fr + 48);
                g.W16(fr + 58, 0);
                g.W16(fr + 56, uz);
                g.W16(fr + 60, static_cast<uint16_t>(0u - ux));
                s1 = cu::GDot(g, fr + 48, n);
                v = cu::GDot(g, fr + 56, n);
            } else {
                // ---- 0x800841B4
                s1 = cu::GDot(g, s7, n);
                v = cu::GDot(g, e + 814, n);
            }
            s0 = Rad(RatAtan2(v, Neg(s1), t.atan));
            s0 = Sub(s0 > 0 ? kPi2 : -kPi2, s0);
            const int32_t lo = Add(s0, (Add(s0, kPi2) >> 31) & Sub(-kPi2, s0));
            const int32_t hi = Sub(kPi2, s0);
            s0 = Add(lo, (hi >> 31) & hi);                                     // clamp(s0, -pi/2, pi/2)
        }
    }
    // ---- 0x80084258: only when heading into the normal
    if (!(s1 < 1)) {
        v0 = 0;
        return !g.Faulted();
    }
    int32_t s3;
    if (mode == 2) {
        // ---- 0x8008426C: a rider, pedestrian or prop
        s3 = 0;
        s1 = -1;
        g.W32(fr + 64, U(FixMul(655, g.S32(s8))));
    } else {
        // ---- 0x80084288
        g.W32(fr + 64, 0);
        s3 = (s0 ^ g.S32(e + 488)) < 0 ? 0x1226C : 0xF5BE;
        if (mode < 2) {
            // ---- 0x800842B0: the heading turned 10 degrees about the normal - REWRITES n
            const int32_t a1 = MulLo(s0, 652) >> 16;
            const uint32_t a2 = static_cast<uint32_t>(a1) & 0xFFFu;
            const int32_t c16 = Shl(t.sincos[2u * a2 + 1u], 4);
            const int32_t wb = a1 < 0 ? Neg(c16) : c16;
            const int32_t s16 = Shl(t.sincos[2u * a2], 4);
            const int32_t wa = a1 > 0 ? Neg(s16) : s16;
            g.W32(fr + 16, U(wb));
            int16_t a[3], b[3], out[3];
            cu::Read16x3(g, s7, a);
            cu::Read16x3(g, e + 814, b);
            Blend16(a, b, out, wa, wb);
            cu::Write16x3(g, n, out);
        }
        // ---- 0x80084330
        if (s4 > 0) s4 = 0;
        if (s3 < Iabs(s0)) {
            // ---- 0x8008434C
            const bool a2 = s4 < S(0xFFEE1E50u) && !(g.U32(e + 560) & 0x20000000u);
            const bool v1 = mode == 5 && s4 < S(0xFFDC3C9Fu);
            s1 = (a2 && v1) ? 8 : a2 ? 4 : 2;
        } else {
            // ---- 0x800843B8
            s1 = g.U32(e + 828) == g.U32(sp + 4) ? 0 : 1;
        }
    }
    // ---- 0x800843DC
    if (s1 == 0) {
        v0 = 0;
        return !g.Faulted();
    }
    {
        const uint32_t fc = g.U32(e + 568);
        if (fc & 0xFu) {
            const uint32_t m = fc & 0xFFF007F0u;
            g.W32(e + 568, m);                                                 // 0x80084404
            g.W32(e + 828, 0);                                                 // 0x8008440C
            g.W32(e + 568, m | 0x08000000u);                                   // 0x80084410
        }
    }
    if (s1 == -1) g.W32(e + 564, g.U32(e + 564) | 0x4000u);                   // 0x8008442C
    else g.W32(e + 568, g.U32(e + 568) | U(s1));                               // 0x80084440 THE CLASS
    // ---- 0x80084444
    const int32_t k = FixMul(s4, 3664);
    const uint32_t home = g.U32(sp + 4);
    const int32_t a8 = Sub(g.S32(s8), g.S32(fr + 64));
    const uint32_t st[7] = {n, U(k), U(s0), U(s3), U(a8), 1143u, 1u};
    for (uint32_t i = 0; i < 7; ++i) g.W32(fr + 16 + 4u * i, st[i]);
    uint32_t r = 0;
    if (!HitSpeed(g, e, s7, s8, home, n, k, s0, s3, a8, 1143, 1, fr, t, r)) return false;
    const uint32_t fc = g.U32(e + 568);
    uint32_t a1 = 0;
    if (fc & 0x1Fu) {
        a1 = 0x1000u;
        if (mode == 3 && (fc & 0xCu)) {
            const uint32_t rd = g.U32(e + 852);
            g.W32(rd + 552, g.U32(rd + 552) | 0x10000u);                       // 0x800844D0
            if (g.U8(g.U32(e + 852) + 572) & 0x10u) {
                const uint32_t r2 = g.U32(g.U32(e + 856) + 852);
                g.W32(r2 + 552, g.U32(r2 + 552) | 0x10000u);                   // 0x80084510
            }
        }
    } else {
        a1 = (fc & 0x220u) ? 0x800u : 0u;
    }
    g.W32(e + 568, g.U32(e + 568) | a1);                                       // 0x8008452C
    v0 = r;
    return !g.Faulted();
}

// ============================================================================ the product's dispatcher
bool ServeHitSpeed(GuestRam& g, const CollCall& call, const BikeTables& t, CollisionCallees& c, uint32_t& v0,
                   bool& ok) {
    (void)c;
    const uint32_t* a = call.a;
    auto arg = [&](int i) { return i < call.n ? a[i] : 0u; };
    switch (call.fn) {
    case 0x80080D1C:
        ok = HitSpeed(g, arg(0), arg(1), arg(2), arg(3), arg(4), S(arg(5)), S(arg(6)), S(arg(7)), S(arg(8)),
                      S(arg(9)), S(arg(10)), call.sp, t, v0);
        return true;
    case 0x80080B10:
        ok = TakePartnerHeading(g, arg(0), arg(1), arg(2), arg(3), S(arg(4)), call.sp, t, v0);
        return true;
    case 0x80083928:
        ok = HitOutcome(g, arg(0), arg(1), arg(2), call.sp, t, v0);
        return true;
    case 0x80083F30:
        ok = ImpactTurn(g, arg(0), arg(1), arg(2), S(arg(3)), call.sp, t, v0);
        return true;
    default:
        return false;
    }
}

} // namespace rr::sim
