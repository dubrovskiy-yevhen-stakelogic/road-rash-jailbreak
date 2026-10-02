#include "game/sim/bike_react.h"

#include "game/sim/coll_util.h"
#include "game/sim/crash.h"
#include "game/sim/fixed.h"

namespace rr::sim {

using namespace cu;

namespace {

constexpr uint32_t kSkipResults = 0x8005B220; // s32: non-zero while the results screen owns the pads

// BikeBikeGate's four unported callees, sent through CollisionCallees::Unported at the gate's own sp.
struct GateSeam final : GateCallees {
    GuestRam& g;
    CollisionCallees& c;
    uint32_t sp; // the gate's frame: the stack pointer it calls at
    GateSeam(GuestRam& gr, CollisionCallees& cc, uint32_t s) : g(gr), c(cc), sp(s) {}
    bool ImpactTurn(uint32_t e, uint32_t partner, uint32_t n, int32_t mode) override {
        return Call(c, coll::kImpactTurn, {e, partner, n, U(mode)}, sp);
    }
    bool HitSpeed(uint32_t e, uint32_t dir, uint32_t pSpeed, uint32_t partner, const int16_t nrm[3], uint32_t nrmSlot,
                  int32_t k, int32_t ang, int32_t lim, int32_t out, int32_t a9, int32_t& v0) override {
        // the buffer lives in the ORIGINAL gate's frame, slot nrmSlot (crash.h)
        const uint32_t at = sp + react::kGateNrmSlot + 6u * nrmSlot;
        Write16x3(g, at, nrm);
        if (g.Faulted()) return false;
        uint32_t r = 0;
        if (!Call(c, react::kHitSpeed, {e, dir, pSpeed, partner, at, U(k), U(ang), U(lim), U(out), U(a9), 0u}, sp, &r))
            return false;
        v0 = S(r);
        return true;
    }
    bool TakePartnerHeading(uint32_t e, uint32_t dir, uint32_t pSpeed, uint32_t partnerDir, int32_t out,
                            int32_t& v0) override {
        uint32_t r = 0;
        if (!Call(c, react::kTakePartnerHeading, {e, dir, pSpeed, partnerDir, U(out)}, sp, &r)) return false;
        v0 = S(r);
        return true;
    }
    bool RiderSpeech(uint32_t h, int32_t crash) override {
        return Call(c, react::kRiderSpeech, {h, U(crash)}, sp);
    }
};

// The player / pad test in front of both rumbles (0x800AC37C.., 0x800AC4A8..).
bool RumbleWanted(GuestRam& g, uint32_t e) {
    if (!(g.U16(e + 172) < NumPlayers(g))) return false;
    if (!(g.U32(g.U32(e + 852) + 604) < 2u)) return false;
    return g.U32(kSkipResults) == 0;
}

} // namespace

uint32_t SideFromCos(int32_t c, uint32_t fl, uint32_t other) {
    if (0xDDB2 < c) return other < 4u ? (other ^ 2u) : 4u;
    if (c < S(0xFFFF224Eu)) return other;
    return (fl & 2u) ? 3u : 1u;
}

bool RememberHandle(GuestRam& g, uint32_t h, uint32_t set, uint32_t& v0) {
    uint32_t w = g.U32(set);
    uint32_t k = 0;
    if (w != 0) {
        const uint32_t t = (h & 0xFFFFu) + 1u;
        do {
            if ((w & 0x1Fu) == t) {
                v0 = 1;
                return !g.Faulted();
            }
            w >>= 5;
            k += 5;
        } while (w != 0);
    }
    if (static_cast<int32_t>(k) < 30) {
        const uint32_t add = (((h & 0xFFFFu) + 1u) & 0x1Fu) << (k & 31u);
        g.W32(set, g.U32(set) | add);
        v0 = 1;
    } else {
        v0 = 0;
    }
    return !g.Faulted();
}

bool ImpactSeverity(GuestRam& g, uint32_t e, int32_t mag, int32_t num, int32_t den, int32_t mode, uint32_t sp,
                    CollisionCallees& c, uint32_t& v0) {
    const uint32_t fr = sp - react::kImpactSeverityFrame;
    const uint32_t rider = g.U32(e + 852);
    const bool dmg = (g.U32(e + 568) & 0x22Cu) != 0;
    int32_t s3 = 21;
    uint32_t s7 = 0;
    if (dmg && den != 0x10000) {                                   // 0x800A946C..0x800A94A8
        if (num > 0) num = den > 0 ? FDiv(num, den) : Neg(FDiv(num, Neg(den)));
        else num = den <= 0 ? FDiv(Neg(num), Neg(den)) : Neg(FDiv(Neg(num), den));
    }
    const int32_t divisor = g.S32(g.U32(e + 556) + 224);
    const int32_t q = MipsDiv(Add(Shl(mag, 2), mag), divisor);    // 0x800A94BC
    const int32_t r4 = Sub(4, q);
    const int32_t sev = Add(q & ~(q >> 31), r4 & (r4 >> 31));      // clamp to 0..4
    if (mode == 0) s7 = 256;
    if (g.Faulted()) return false;
    if (g.U32(rider + 604) == 1) {
        const uint32_t st = g.U16(rider + 544);
        bool go = static_cast<uint16_t>(st - 26u) >= 12u && g.U16(0x800541D4u + 8u * st + 2u) != 3;
        if (go && g.U16(rider + 608) != 0) {
            const uint32_t s2 = g.U16(rider + 610);
            go = static_cast<uint16_t>(s2 - 26u) >= 12u && g.U16(0x800541D4u + (s2 << 3) + 2u) != 3;
        }
        if (g.Faulted()) return false;
        if (go) {
            s3 = mode == 1 ? 27 : (mode == 0 || mode == 2) ? 26 : mode == 3 ? 28 : 21;
            if (!Call(c, react::kStanceEvent, {U(s3), rider, s7 | 0xAu}, fr)) return false;   // 0x800A95C0
        }
    }
    if (dmg) {                                                     // 0x800A95D0
        const uint32_t r = g.U32(e + 1084);
        const uint32_t old = g.U8(r + 37);
        const int32_t nw = Sub(static_cast<int32_t>(old), MulLo(sev, num) >> 16);
        if (nw <= 0) {
            g.W8(r + 37, 0);
        } else {
            uint32_t fl = g.U8(r + 68);
            if (((old & 0x80u) && nw < 128) || (old >= 64u && nw < 64)) fl |= 0x40u;
            g.W8(r + 68, static_cast<uint8_t>(fl));
            g.W8(g.U32(e + 1084) + 37, static_cast<uint8_t>(nw));
        }
    }
    v0 = U(sev);
    return !g.Faulted();
}

bool BikeBikeReact(GuestRam& g, uint32_t a, uint32_t b, uint32_t codeA, uint32_t codeB, uint32_t sp,
                   const BikeTables& t, CollisionCallees& c) {
    const uint32_t fr = sp - react::kBikeBikeReactFrame;
    const uint32_t nAddr = fr + 24;
    uint32_t s7 = 8, s8 = 8, s4 = 6, s5 = 6;
    if (!(g.U32(a + 568) & 1u) && !(g.U32(b + 568) & 1u)) {       // 0x800AC178..0x800AC1B8
        if (g.U32(a + 828) == b + 172u) return !g.Faulted();
        if (g.U32(b + 828) == a + 172u) return !g.Faulted();
    }
    const bool fast = g.S32(a + 480) > 0x1017E || g.S32(b + 480) > 0x1017E;
    bool riders = false;                                           // 0x800AC1F0..0x800AC240
    if (g.U16(g.U32(a + 852) + 544) != 0 && !(g.U32(a + 568) & 0xEu) && g.U16(g.U32(b + 852) + 544) != 0)
        riders = (g.U32(b + 568) & 0xEu) == 0;
    if (g.Faulted()) return false;
    if (!riders) return true;
    if (codeB & 0x200u) {
        if (codeA & 0x200u) s4 = codeA & 0xFFu;
        else s7 = codeA & 0xFFu;
        s5 = codeB & 0xFFu;
        if (!Call(c, react::kFaceNormal, {b + 196u, b + 432u, s5, nAddr, 0u}, fr)) return false;   // 0x800AC27C
    } else {
        s4 = codeA & 0xFFu;
        if (!Call(c, react::kFaceNormal, {a + 196u, a + 432u, s4, nAddr, 0u}, fr)) return false;   // 0x800AC2A0
        s8 = codeB & 0xFFu;
        const uint16_t n0 = g.U16(nAddr), n2 = g.U16(nAddr + 4);
        g.W16(nAddr, static_cast<uint16_t>(0u - n0));
        const uint16_t n1 = g.U16(nAddr + 2);
        g.W16(nAddr + 4, static_cast<uint16_t>(0u - n2));
        g.W16(nAddr + 2, static_cast<uint16_t>(0u - n1));
    }
    if (g.Faulted()) return false;
    // ---- 0x800AC2E4: BikeBikeGate, natively, at sp = the frame
    const uint32_t gsp = fr - react::kGateFrame;
    const int32_t stale[4] = {g.S32(gsp + 108), g.S32(gsp + 116), g.S32(gsp + 120), g.S32(gsp + 124)};
    if (g.Faulted()) return false;
    GateSeam seam(g, c, gsp);
    bool ok = false;
    const int32_t hit = BikeBikeGate(g, a, b, codeA, codeB, nAddr, stale, t, seam, ok);
    if (!ok || g.Faulted()) return false;
    if (hit == 0) return true;
    const int32_t cs = Add(FixMul(g.S32(a + 296), g.S32(b + 296)), FixMul(g.S32(a + 300), g.S32(b + 300)));
    int32_t s0 = Iabs(Sub(g.S32(a + 480), FixMul(cs, g.S32(b + 480))));
    if (s4 == 6) s4 = SideFromCos(cs, s7, s5);
    uint32_t sev = 0;
    if (!ImpactSeverity(g, a, s0, g.S32(b + 316), g.S32(a + 316), S(s4), fr, c, sev)) return false;   // 0x800AC36C
    if (fast && RumbleWanted(g, a)) {
        const int32_t speed = (g.U32(a + 568) & 0x20Du) ? s0 : g.S32(a + 480);
        if (g.Faulted()) return false;
        if (!c.Rumble(a, 0, speed, 0x165A1C, 1, fr)) return false;                               // 0x800AC3EC
    }
    s0 = Iabs(Sub(g.S32(b + 480), FixMul(cs, g.S32(a + 480))));
    if (s5 == 6) s5 = SideFromCos(cs, s8, s4);
    if (!ImpactSeverity(g, b, s0, g.S32(a + 316), g.S32(b + 316), S(s5), fr, c, sev)) return false;   // 0x800AC448
    if (fast) {
        int32_t id;
        if (S(sev) >= 3) id = 48;
        else if (S(sev) >= 2) id = 18;
        else {
            uint32_t rc = 0;
            if (!Call(c, react::kGetRCnt, {0xF2000002u}, fr, &rc)) return false;                     // 0x800AC47C
            id = S((((rc & 0xFFu) * 5u) >> 8) + 50u);
        }
        if (!c.PlaySound3D(g.S32(b + 184), g.S32(b + 192), id, 0)) return false;                   // 0x800AC4A0
        if (RumbleWanted(g, b)) {
            const int32_t speed = (g.U32(b + 568) & 0x20Du) ? s0 : g.S32(b + 480);
            if (g.Faulted()) return false;
            if (!c.Rumble(b, 0, speed, 0x165A1C, 1, fr)) return false;                           // 0x800AC51C
        }
    }
    uint32_t v = 0;
    if (!RememberHandle(g, g.U16(b + 172), a + 912u, v)) return false;                            // 0x800AC528
    if (!RememberHandle(g, g.U16(a + 172), b + 912u, v)) return false;                            // 0x800AC534
    return !g.Faulted();
}

// ---------------------------------------------------------------------------- the traffic pair
bool TrafficSideShove(GuestRam& g, uint32_t code, uint32_t flags, uint32_t bike, uint32_t car, uint32_t imp,
                      uint32_t sp, uint32_t& v0) {
    const uint32_t q = sp - 64 + 16;
    v0 = flags;
    if (!(flags & 0x200u)) return !g.Faulted();
    if ((flags & 0xFFu) != 3 && (flags & 0xFFu) != 1) return !g.Faulted();
    const uint32_t cc = bike + 12u * (code & 0xFFu);
    g.W32(q, U(Add(g.S32(imp), g.S32(cc + 196))));
    g.W32(q + 4, U(Add(g.S32(imp + 4), g.S32(cc + 200))));
    g.W32(q + 8, U(Add(g.S32(imp + 8), g.S32(cc + 204))));
    const int32_t s4 = Sub(MulLo(g.S16(bike + 450), g.S16(car + 448)), MulLo(g.S16(bike + 454), g.S16(car + 444)));
    const int32_t p = GProject(g, q, car + 432, car + 184);                      // 0x800A9284
    const int32_t w = g.S32(car + 304);
    const int32_t w3 = Add(Shl(w, 1), w);
    const int32_t a1 = (w3 < 0 ? Add(w3, 3) : w3) >> 2;
    uint32_t nf;
    if (a1 < p && p < Add(w, (w < 0 ? Add(w, 7) : w) >> 3) && s4 > 0) {
        GScale(g, Sub(p, a1), car + 432, q);                                     // 0x800A92E8
        nf = 514;
    } else {
        if (!(p < Neg(a1))) return !g.Faulted();
        const int32_t w2 = g.S32(car + 304);
        if (!(Sub(Neg(w2), (w2 < 0 ? Add(w2, 7) : w2) >> 3) < p)) return !g.Faulted();
        if (s4 >= 0) return !g.Faulted();
        GScale(g, Add(p, a1), car + 432, q);                                     // 0x800A9338
        nf = 512;
    }
    const int32_t d = Iabs(GProject(g, bike + 468, car + 444, car + 468));      // 0x800A934C
    if (d < Sub(Add(g.S32(car + 308), g.S32(bike + 308)), 4096)) {
        g.W32(imp, g.U32(q));
        g.W32(imp + 4, g.U32(q + 4));
        g.W32(imp + 8, g.U32(q + 8));
    } else {
        g.W32(imp, g.U32(imp) + g.U32(q));
        g.W32(imp + 4, g.U32(imp + 4) + g.U32(q + 4));
        g.W32(imp + 8, g.U32(imp + 8) + g.U32(q + 8));
    }
    v0 = nf;
    return !g.Faulted();
}

bool BikeTrafficReact(GuestRam& g, uint32_t bike, uint32_t car, uint32_t code, uint32_t flags, uint32_t imp,
                      uint32_t sp, const BikeTables& t, CollisionCallees& c, const GuestRegs* caller) {
    if (t.sincos == nullptr || t.atan == nullptr || t.rsqrt == nullptr) return false;
    const uint32_t fr = sp - 80;
    const uint32_t nA = fr + 24;
    int32_t s3 = 6;
    // The registers the original holds at each call site (s0 the caller's until the magnitude is
    // computed, s1 bike, s2 car, s3 the class, s4 flags; s5..s8 the caller's), for a caller that
    // reproduces them; without one the calls are plain Unported ones.
    GuestRegs rr = caller != nullptr ? *caller : GuestRegs{};
    rr.s[1] = bike;
    rr.s[2] = car;
    rr.s[4] = flags;
    auto call = [&](uint32_t fn, std::initializer_list<uint32_t> args, uint32_t ra, uint32_t* v0) {
        uint32_t a[8] = {};
        int n = 0;
        for (uint32_t x : args) a[n++] = x;
        uint32_t r = 0;
        rr.s[3] = U(s3);
        rr.ra = ra;
        const bool ok = caller != nullptr ? c.UnportedAt(fn, a, n, fr, rr, r) : c.Unported(fn, a, n, fr, r);
        if (v0 != nullptr) *v0 = r;
        return ok;
    };
    uint32_t hit = 0;
    if (!call(react::kImpactGate, {bike, car + 172, code, flags, imp}, 0x800AC994u, &hit)) return false;   // 0x800AC98C
    if (hit != 0) {
        if (flags & 0x200u) {
            if (!call(react::kFaceNormal, {car + 196, car + 432, flags & 0xFFu, nA, 0u}, 0x800AC9B8u, nullptr)) return false;
            const int32_t d = GDot(g, bike + 450, nA);                            // 0x800AC9BC
            if (0xDDB2 < d) s3 = 1;
            else if (d < S(0xFFFF224Eu)) s3 = 3;
            else s3 = (MulLo(g.S16(bike + 450), g.S16(nA + 4)) < MulLo(g.S16(bike + 454), g.S16(nA)) ? 0 : 1) << 1;
        } else {
            uint32_t v = 0;
            if (!call(react::kXzDot, {car + 184, bike + 516, bike + 504}, 0x800ACA30u, &v)) return false;   // 0x800ACA28
            s3 = (0 < S(v) ? 1 : 0) << 1;
        }
        int32_t a0;
        if ((s3 & 5) == 0) {
            // the face normal's slots, UNWRITTEN on the !(flags & 0x200) path
            const int32_t m0 = FixMul(g.S32(bike + 296), Shl(g.S16(nA + 4), 4));
            const int32_t m1 = FixMul(g.S32(bike + 300), Shl(g.S16(nA), 4));
            a0 = Sub(Neg(m0), m1);
        } else {
            a0 = Add(FixMul(g.S32(bike + 296), g.S32(car + 296)), FixMul(g.S32(bike + 300), g.S32(car + 300)));
        }
        const int32_t s0 = Iabs(FixMul(a0, Sub(g.S32(bike + 480), g.S32(car + 480))));
        rr.s[0] = U(s0);
        uint32_t sev = 0;
        if (g.Faulted()) return false;
        if (!ImpactSeverity(g, bike, s0, 0x50000, 0x10000, s3, fr, c, sev)) return false;   // 0x800ACAC8
        if (g.S32(bike + 480) > 0x1017E || g.S32(car + 480) > 0x1017E) {
            int32_t id = 48;
            if (S(sev) < 3 && !(g.U32(bike + 564) & 0x8000u)) {
                if (S(sev) < 2) {
                    uint32_t rc = 0;
                    if (!call(react::kGetRCnt, {0xF2000002u}, 0x800ACB38u, &rc)) return false;   // 0x800ACB30
                    id = S(((rc & 0xFFu) >> 6) + 50u);
                } else {
                    id = 18;
                }
            }
            if (!c.PlaySound3D(g.S32(car + 184), g.S32(car + 192), id, 0)) return false;   // 0x800ACB4C
            if (RumbleWanted(g, bike)) {
                const int32_t speed = (g.U32(bike + 568) & 0x20Du) ? g.S32(bike + 480) : s0;
                if (g.Faulted()) return false;
                if (!c.Rumble(bike, 0, speed, 0x165A1C, 1, fr)) return false;              // 0x800ACBCC
            }
        }
        const uint32_t lo = flags & 0xFFu;
        if ((g.U32(bike + 568) & 0x20Eu) && (!(flags & 0x200u) || lo == 1 || lo == 3)) {
            g.W32(car + 484, 0);
            if ((g.U32(bike + 568) & 2u) && lo == 1) {
                const int32_t v = Sub(g.S32(car + 480), s0);
                g.W32(car + 480, U(v));
                g.W32(car + 480, U(v < 0 ? 0 : v));
            } else {
                const uint8_t b = g.U8(car + 509);
                g.W32(car + 480, 0);
                g.W8(car + 509, static_cast<uint8_t>(b | 0x10u));
            }
        }
    }
    // ---- 0x800ACC58: the bike riding on the car
    const uint32_t f = g.U32(bike + 564);
    if ((f & 0x28000u) == 0x8000u) {
        if (g.S32(bike + 616) == 0) rr.s[4] = 0x40000u;                              // 0x800ACC90 (delay slot)
        if (g.S32(bike + 616) == 0 && g.S32(bike + 772) != 0) {
            if (f & 0x40000u) {
                if (g.S32(bike + 488) != 0) {
                    const int32_t a = GDot(g, bike + 450, car + 432);
                    const int32_t b = GDot(g, bike + 450, car + 444);
                    g.W32(bike + 692, U(RatAtan2(a, b, t.atan)));                   // 0x800ACCD0
                    rr.s[0] = U(a);
                } else {
                    const uint32_t ang = g.U32(bike + 692) & 0xFFFu;
                    const int32_t cs = Shl(t.sincos[2 * ang + 1], 4), sn = Shl(t.sincos[2 * ang], 4);
                    int16_t va[3], vb[3], vo[3];
                    Read16x3(g, car + 444, va);
                    Read16x3(g, car + 432, vb);
                    Blend16(va, vb, vo, cs, sn);                                     // 0x800ACD24
                    Write16x3(g, bike + 528, vo);
                    int16_t dd[3], ir[3], out[3];
                    Read16x3(g, bike + 522, dd);
                    Read16x3(g, bike + 528, ir);
                    OuterProduct(dd, ir, out);                                       // cop2 0x178000C
                    Write16x3(g, bike + 516, out);
                    if (g.Faulted() || !GNormalize(g, bike + 516, t.rsqrt)) return false;   // 0x800ACD88
                    rr.s[0] = bike + 528;
                }
            } else {
                // 0x800ACD98: the first frame on the car
                const int32_t k = FixMul(g.S32(car + 480), GDot(g, car + 450, bike + 450));
                int32_t v = Sub(g.S32(bike + 576), k);
                g.W32(bike + 576, U(v));
                if (v < 0x23C36) v = 0x23C36;
                g.W32(bike + 576, U(v));
                g.W32(bike + 480, U(v));
                int32_t d[3];
                for (uint32_t i = 0; i < 3; ++i) {
                    d[i] = Sub(g.S32(bike + 504 + 4 * i), g.S32(car + 184 + 4 * i));
                    g.W32(fr + 32 + 4 * i, U(d[i]));
                }
                int32_t x = 0, z = 0;
                for (uint32_t i = 0; i < 3; ++i) x = Add(x, FixMul(d[i], Shl(g.S16(car + 432 + 2 * i), 4)));
                g.W32(bike + 776, U(x));
                for (uint32_t i = 0; i < 3; ++i) z = Add(z, FixMul(d[i], Shl(g.S16(car + 444 + 2 * i), 4)));
                g.W32(bike + 780, U(z));
                // the last product's lo / hi words are spilled to sp+48 / +52 (0x800ACE84, 0x800ACF18)
                const int64_t last = static_cast<int64_t>(d[2]) * static_cast<int64_t>(Shl(g.S16(car + 448), 4));
                g.W32(fr + 48, static_cast<uint32_t>(last));
                g.W32(fr + 52, static_cast<uint32_t>(static_cast<uint64_t>(last) >> 32));
                const int32_t a = GDot(g, bike + 450, car + 432);
                const int32_t b = GDot(g, bike + 450, car + 444);
                const uint32_t fb = g.U32(bike + 564);
                g.W32(bike + 692, U(RatAtan2(a, b, t.atan)));                       // 0x800ACF50
                g.W32(bike + 564, fb | 0x40000u);
                rr.s[0] = U(a);
            }
        } else if (g.U32(bike + 564) & 0x40000u) {
            // 0x800ACF6C
            const int32_t k = FixMul(g.S32(car + 480), GDot(g, car + 450, bike + 450));
            int32_t v = Add(g.S32(bike + 576), k);
            g.W32(bike + 576, U(v));
            if (v < 0x23C36) v = 0x23C36;
            g.W32(bike + 576, U(v));
            g.W32(bike + 480, U(v));
            GScale(g, v, bike + 450, bike + 456);                                    // 0x800ACFCC
            g.W32(bike + 564, g.U32(bike + 564) & 0xFFFBFFFFu);
            rr.s[0] = bike + 450;
        }
    }
    if (g.Faulted()) return false;
    // ---- 0x800ACFE8
    if (((g.U32(bike + 36) >> 25) & 3u) < 2u) {
        if (s3 == 0) {
            if (!call(react::kSurfaceFx, {bike, 4u}, 0x800AD02Cu, nullptr)) return false;
        } else if (s3 == 2) {
            if (!call(react::kSurfaceFx, {bike, 3u}, 0x800AD02Cu, nullptr)) return false;
        }
    }
    return !g.Faulted();
}

bool BikeVsTraffic(GuestRam& g, uint32_t bike, uint32_t car, uint32_t sp, const BikeTables& t,
                   CollisionCallees& c) {
    if (t.sincos == nullptr || t.atan == nullptr || t.sqrt == nullptr) return false;
    const uint32_t fr = sp - 96;
    const uint32_t IMP = fr + 24, PUSH = fr + 40, CODE = fr + 56, FLAGS = fr + 60, MAG = fr + 64;
    const uint32_t shape = car + 172;
    g.W32(MAG, 0);
    bool s3 = false;
    if (g.U32(bike + 832) == shape) s3 = (g.U8(bike + 565) >> 7) != 0;
    if (!(g.U32(bike + 568) & 1u) && g.U32(bike + 828) == shape && !s3) return !g.Faulted();
    int32_t stale = 0;
    if (g.Faulted() || !StaleHeading(g, bike, t, stale)) return false;           // 0x800AC628
    g.W32(IMP + 8, 0);
    g.W32(IMP + 4, 0);
    g.W32(IMP, 0);
    g.W32(FLAGS, 0);
    g.W32(CODE, 0);
    uint32_t s0 = U(stale);
    bool shove = false;
    if (InCameraBox(g, bike, 0x140000, 0x1C0000) != 0) {                          // 0x800AC650
        if (g.Faulted()) return false;
        if (!Call(c, react::kResponse, {bike, car, CODE, FLAGS, IMP, MAG}, fr, &s0)) return false;   // 0x800AC678
        const uint32_t a0 = g.U32(CODE);
        shove = ((a0 >> 8) & 1u) != 0;
        if (s0 != 0 && g.U32(bike + 856) != 0) {
            const uint32_t pas = g.U32(bike + 856);
            bool a3 = false;
            const uint32_t h = g.U16(s0 + 172);
            const bool eq = (a0 & 1u) == ((a0 & 2u) >> 1);
            if (h == g.U16(pas + 172) && !eq) a3 = true;
            else if (h == g.U16(bike + 172) && (a0 & 0xFFu) < 8u && eq) a3 = true;
            shove = shove && a3;
            if (s0 == g.U32(bike + 856)) s0 = bike;
        }
    } else {
        if (g.Faulted()) return false;
        if (s0 != 0) {                                                            // 0x800AC71C
            const int32_t h = RatAtan2(Shl(g.S16(bike + 450), 4), Shl(g.S16(bike + 454), 4), t.atan);
            g.W32(bike + 292, U(h));
            const int32_t cs = t.sincos[2 * (g.U32(bike + 292) & 0xFFFu) + 1];
            g.W32(bike + 296, U(Shl(cs, 4)));
            g.W32(bike + 300, U(Shl(t.sincos[2 * (g.U32(bike + 292) & 0xFFFu)], 4)));
        }
        if (!Call(c, react::kResponseFar, {bike, car, CODE, FLAGS, IMP}, fr, &s0)) return false;   // 0x800AC78C
        shove = false;
    }
    const uint32_t code = g.U32(CODE);
    if (code == 0 && !s3) return !g.Faulted();
    if (s3) {
        g.W32(IMP + 8, 0);
        g.W32(IMP + 4, 0);
        g.W32(IMP, 0);
    } else {
        if (shove) {
            uint32_t nf = 0;
            if (!TrafficSideShove(g, code, g.U32(FLAGS), bike, car, IMP, fr, nf)) return false;   // 0x800AC7E0
            g.W32(FLAGS, nf);
        }
        if (s0 == car) {
            const uint32_t i0 = g.U32(IMP), i2 = g.U32(IMP + 8);
            g.W32(IMP, 0u - i0);
            const uint32_t i1 = g.U32(IMP + 4);
            g.W32(IMP + 8, 0u - i2);
            g.W32(IMP + 4, 0u - i1);
        }
    }
    if (g.S32(MAG) > 0) {                                                         // 0x800AC820
        const int32_t k = FixMul(g.S32(car + 480), g.S32(MAG));
        GScale(g, k, car + 450, PUSH);
        ApplyImpulse(g, bike, PUSH, 1);
        ApplyImpulse(g, car, PUSH, 1);
        const uint32_t m = g.U32(MAG), cur = g.U32(bike + 552);
        g.W32(bike + 552, m < cur ? cur : m);
    }
    if (g.Faulted()) return false;
    const int32_t n = g.S32(kContactCount);
    if (s3 || !(g.U16(bike + 320) & 4u) || !(n < 8))
        return BikeTrafficReact(g, bike, car, g.U32(CODE), g.U32(FLAGS), IMP, fr, t, c);   // 0x800AC8CC
    const uint32_t r = kContactList + 36u * U(n);
    g.W16(r, g.U16(bike + 172));
    g.W16(r + 2, g.U16(car + 172));
    g.W32(r + 16, g.U32(IMP));
    const uint32_t i1 = g.U32(IMP + 4), fl = g.U32(FLAGS);
    g.W32(kContactCount, U(Add(n, 1)));
    g.W32(r + 20, i1);
    g.W32(r + 28, g.U32(CODE) | (fl << 16));
    g.W32(r + 24, g.U32(IMP + 8));
    return !g.Faulted();
}

namespace {

// (x * 25736) >> 8 by the original's shift-add chain (0x800BED8.., 0x8007BF44.., 0x8007BFE0..): 32-bit wrap.
int32_t Rad(int32_t a) { return MulLo(a, 25736) >> 8; }

// One live slot's body, [0x8007B8FC, 0x8007C20C).
bool PoolLoopLive(GuestRam& g, uint32_t e, int32_t dt, uint32_t sp, const BikeTables& t, PoolLoopCallees& c) {
    const bool s8 = (g.U32(e + 568) & 0x600u) != 0;
    const uint32_t rider = g.U32(e + 852);
    if (!(g.U32(rider + 552) & 0x80000u)) {
        // ---- 0x8007B924: the road edge
        int32_t s5 = 0, s6 = 0;
        g.W32(sp + 28, 0);
        g.W32(sp + 24, 0);
        g.W32(sp + 36, 0);
        g.W32(sp + 32, 0);
        const int32_t s2 = Shl((g.U32(e + 568) & 0x1FFu) != 0 ? 1 : 0, 17);
        const uint32_t h = g.U16(e + 172);
        if (h < NumPlayers(g)) {
            const uint32_t rec = 0x800D43C0u + 28u * h;
            if (g.S16(rec + 22) != 0 && g.U32(rec) == g.U32(e + 360)) {
                if (!s8) {
                    s5 = Neg(s2);
                    s6 = S(0xFFFB0000u);
                }
                g.W32(sp + 28, rec + 4);
                g.W32(sp + 36, rec + 16);
            }
        }
        const int32_t s7 = g.S32(e + 344);
        bool s4 = false;
        if (g.U8(e + 928) & 0x40u) g.W32(e + 36, g.U32(e + 36) | 0x200u);
        if (g.Faulted()) return false;
        if ((g.U32(e + 388) & 1u) && g.U32(e + 372) != 0 && !((g.U32(e + 36) >> 9) & 1u)) {
            const uint32_t slice = g.U32(e + 340);
            const uint32_t s0 = s7 > 0 ? g.U8(slice + 48) : g.U8(slice + 49);
            if (s0 != 0) {
                const int32_t dot = GDot(g, e + 450, slice + 2);                     // 0x8007BA48
                if (!s8) {
                    s5 = Shl(S(s0), 15);
                    s6 = Add(s5, S(0xFFFB0000u));
                    s5 = Sub(s5, s2);
                } else {
                    s6 = Shl(S(s0), 15);
                }
                const uint32_t sl = g.U32(e + 340);
                g.W32(sp + 160, U(Iabs(s7)));
                if (Shl(S(g.U8(sl + 48)), 15) < s7 && dot > 0) s4 = true;
                else if (s7 < Neg(Shl(S(g.U8(sl + 49)), 15)) && dot < 0) s4 = true;
            } else {
                uint32_t r = 0;
                if (g.Faulted() || !c.JunctionMargin(e, sp, r)) return false;       // 0x8007BAD0
                if (r != 0) {
                    if (!s8) {
                        s5 = Neg(s2);
                        s6 = S(0xFFFD0000u);
                    }
                    g.W32(sp + 24, r + 4);
                    g.W32(sp + 32, r + 16);
                }
            }
        }
        // ---- 0x8007BB08: the two edge planes
        for (uint32_t k = 0; k < 2; ++k) {
            const uint32_t pt = g.U32(sp + 24 + 4 * k);
            if (pt == 0 || s4) continue;
            const uint32_t n = g.U32(sp + 32 + 4 * k);
            if (GDot(g, e + 450, n) > 0) {                                          // 0x8007BB28
                g.W32(sp + 160, 0);
                continue;
            }
            int32_t d = FixMul(Shl(g.S16(n), 4), Sub(g.S32(pt), g.S32(e + 184)));
            d = Add(d, FixMul(Shl(g.S16(n + 4), 4), Sub(g.S32(pt + 8), g.S32(e + 192))));
            s4 = s6 < d;
            g.W32(sp + 160, U(d));
        }
        if (g.Faulted()) return false;
        if (s4 && !(s8 && (g.U32(e + 568) & 0xFu))) {
            // ---- 0x8007BBB4: the edge normal into +0x334
            if (g.U32(sp + 32) != 0) {
                g.W16(e + 820, g.U16(g.U32(sp + 32)));
                g.W16(e + 822, g.U16(g.U32(sp + 32) + 2));
                g.W16(e + 824, g.U16(g.U32(sp + 32) + 4));
            } else if (g.U32(sp + 36) != 0) {
                g.W16(e + 820, g.U16(g.U32(sp + 36)));
                g.W16(e + 822, g.U16(g.U32(sp + 36) + 2));
                g.W16(e + 824, g.U16(g.U32(sp + 36) + 4));
            } else {
                const uint32_t v0 = g.U32(e + 340), v1 = g.U32(e + 340);
                g.W16(e + 820, g.U16(v0 + 2));
                g.W16(e + 822, g.U16(v1 + 4));
                g.W16(e + 824, g.U16(v1 + 6));
                if (s7 > 0) {
                    const uint16_t a0 = g.U16(e + 820), a2 = g.U16(e + 824);
                    g.W16(e + 820, static_cast<uint16_t>(0u - a0));
                    const uint16_t a1 = g.U16(e + 822);
                    g.W16(e + 824, static_cast<uint16_t>(0u - a2));
                    g.W16(e + 822, static_cast<uint16_t>(0u - a1));
                }
            }
            if (g.Faulted()) return false;
            if (s8) {
                uint32_t r = 0;
                if (!c.ImpactTurn(e, e + 172, e + 820, 3, sp, r)) return false;      // 0x8007BC94
                if (r != 0) {
                    if (!c.PlaySound3D(g.S32(e + 184), g.S32(e + 192), g.S16(e + 322), 0)) return false;
                    if (g.U32(e + 568) & 0x1000u)
                        if (!c.RecoverEnd(e, dt, sp)) return false;                // 0x8007BCD0
                }
            } else {
                // ---- 0x8007BCE0: the off-road slow-down
                const int32_t d = g.S32(sp + 160);
                if (s5 < d) {
                    g.W32(e + 576, 0);
                    g.W32(e + 560, g.U32(e + 560) | 0x2000u);
                } else {
                    const int32_t f = SDiv(Sub(s5, d), Sub(s5, s6));
                    const int32_t sp240 = ClampLerpMin(g.S32(e + 576), g.S32(g.U32(e + 556) + 224) >> 1, f);
                    g.W32(e + 576, U(sp240));
                    g.W32(e + 676, U(FixMul(g.S32(e + 676), f)));
                    const int32_t w696 = g.S32(e + 696);
                    g.W32(e + 696, U(0xFFFF < w696 ? 0xF333 : w696));
                    const int32_t w700 = g.S32(e + 700);
                    g.W32(e + 700, U(0xFFFF < w700 ? 0xF333 : w700));
                }
                uint32_t fa = g.U32(e + 560);
                if ((fa & 0x08000000u) || g.S32(e + 480) == 0) {
                    if (!(fa & 0x2000u)) fa |= 0x6000u;
                    g.W32(e + 560, fa);
                }
            }
        }
    }
    // ---- 0x8007BDE0: the contact release
    const uint32_t fb = g.U32(e + 564);
    if ((fb & 0x18000u) == 0x8000u) {
        const bool air = ((g.U32(e + 568) >> 10) & 1u) != 0;
        if (g.U32(e + 832) != 0) {
            if ((fb & 0x20000u) && !air)
                if (!c.Launch(e, 1, sp)) return false;                              // 0x8007BE2C
            if (g.Faulted() || !c.ReleaseContact(e)) return false;                  // 0x8007BE34
            if (air) g.W32(e + 828, g.U32(e + 832));
            else g.W32(e + 832, 0);
        }
        const uint32_t b2 = g.U32(e + 564);
        g.W32(e + 744, 0);
        g.W32(e + 772, 0);
        g.W32(e + 616, 0);
        g.W32(e + 564, b2 & 0xFFD97FFFu);
    }
    if (g.Faulted()) return false;
    // ---- 0x8007BE74: re-seat on the road
    if (g.U32(e + 568) & 0x800000u) {
        if (!c.RoadRebind(e, S(((g.U32(e + 560) >> 27) ^ 1u) & 1u), sp)) return false;   // 0x8007BE98
        if (!c.GroundFrame(e, e + 184, sp)) return false;                                // 0x8007BEA4
        if (g.U32(e + 568) & 0x04000000u) {
            int32_t as = 0;
            if (!Asin(GDot(g, e + 522, e + 432), t.asin, as)) return false;              // 0x8007BEC8
            g.W32(e + 652, U(Rad(as)));
            g.W32(e + 676, 0);
            const uint16_t r0 = g.U16(e + 528), r1 = g.U16(e + 530), r2 = g.U16(e + 532);
            const uint16_t q0 = g.U16(e + 516), q1 = g.U16(e + 518), q2 = g.U16(e + 520);
            g.W16(e + 450, r0);
            g.W16(e + 452, r1);
            g.W16(e + 454, r2);
            g.W16(e + 814, q0);
            g.W16(e + 816, q1);
            g.W16(e + 818, q2);
            if (!Asin(GDot(g, e + 438, e + 528), t.asin, as)) return false;              // 0x8007BF3C
            g.W32(e + 616, U(Rad(as)));
            const int32_t x = MulLo(g.S16(e + 518), 157);
            g.W32(e + 732, U(x));
            const int32_t y = MulLo(g.S16(e + 524), 157);
            g.W32(e + 736, U(y));
            g.W32(e + 740, U(MulLo(g.S16(e + 530), 157)));
            g.W32(e + 668, U(Neg(Rad(RatAtan2(x, y, t.atan)))));                         // 0x8007BFD4
            if (g.Faulted() || !c.SteerLean(e, sp)) return false;                        // 0x8007C00C
            g.W32(e + 564, g.U32(e + 564) | 0x800u);
            const int32_t p = g.S32(e + 616);
            int32_t v1 = 0;
            if (p > 0) v1 = S(0xFFFC0000u);
            if (p < 0) v1 = Add(v1, 0x40000);
            g.W32(e + 620, U(v1));
            g.W32(e + 624, 0);
        }
        bool passenger = true;                                                           // reach 0x8007C1A8
        if (s8) {
            // ---- 0x8007C04C: the tumble centre 0.34 below the box centre along +0x1B6
            for (uint32_t k = 0; k < 3; ++k)
                g.W32(e + 784 + 4 * k, U(Add(FixMul(Shl(g.S16(e + 438 + 2 * k), 4), -22282), g.S32(e + 184 + 4 * k))));
            if (g.U32(e + 856) == 0) passenger = false;
            else if (g.U32(e + 1088) != 0) GMulAdd(g, e + 784, e + 432, Shl(g.S32(e + 304), 1), e + 784);   // 0x8007C1A0
        } else {
            if (!(g.U32(e + 568) & 0x100u)) {
                const int32_t p = g.S32(e + 616);
                if (p != 0) {
                    if (!c.TurnFacing(e, MulLo(p, 652) >> 16, sp)) return false;          // 0x8007C168
                } else {
                    const int32_t w0 = g.S32(e + 504), w1 = g.S32(e + 508), w2 = g.S32(e + 512);
                    g.W32(e + 184, U(w0));
                    g.W32(e + 188, U(w1));
                    g.W32(e + 192, U(w2));
                }
            }
            GMulAdd(g, e + 504, e + 528, g.S32(e + 308), e + 784);                       // 0x8007C1A0
        }
        if (passenger) {
            const uint32_t q = g.U32(e + 856);
            if (q != 0) GMulAdd(g, e + 184, e + 432, Add(g.S32(e + 304), g.S32(q + 304)), q + 184);   // 0x8007C1C8
        }
        if (g.S8(e + 8) < 2) {
            int32_t as = 0;
            if (!Asin(Shl(g.S16(e + 530), 4), t.asin, as)) return false;                 // 0x8007C1E8
            g.W16(e + 842, static_cast<uint16_t>(as));
        }
        g.W32(e + 568, g.U32(e + 568) & 0xFA7FFFFFu);
    }
    g.W32(e + 564, g.U32(e + 564) & 0xFFFEFFFFu);
    return !g.Faulted();
}

} // namespace

bool RiderPassPoolLoop(GuestRam& g, int32_t dt, uint32_t sp, const BikeTables& t, PoolLoopCallees& c) {
    if (t.asin == nullptr || t.atan == nullptr) return false;
    uint32_t e = g.U32(kPoolTableAddr);
    const int32_t last = g.S32(g.U32(kPoolTableAddr + 12));
    g.W32(sp + 164, U(last));                                                  // 0x8007B890
    if (g.Faulted()) return false;
    if (last < 0) return true;
    for (;;) {
        if (g.U32(e + 568) & 0x02000000u) {                                     // 0x8007B8A8: the latch
            const uint16_t h0 = g.U16(e + 864), h1 = g.U16(e + 866), h2 = g.U16(e + 868);
            const uint32_t w = g.U32(e + 860);
            g.W16(e + 450, h0);
            g.W16(e + 452, h1);
            g.W16(e + 454, h2);
            g.W32(e + 480, w);
        }
        if (g.U32(e + 568) & 0x08001800u)
            if (g.Faulted() || !c.ListMigrate(e, dt, sp)) return false;         // 0x8007B8E4
        if (g.S16(e + 320) != 0)
            if (!PoolLoopLive(g, e, dt, sp, t, c)) return false;
        const int32_t n = Sub(g.S32(sp + 164), 1);
        e += g.U32(kPoolTableAddr + 4);
        g.W32(sp + 164, U(n));
        if (g.Faulted()) return false;
        if (n < 0) break;
    }
    return true;
}

bool ServeBikeReact(GuestRam& g, const CollCall& call, const BikeTables& t, CollisionCallees& c, uint32_t& v0,
                    bool& ok) {
    const uint32_t* a = call.a;
    switch (call.fn) {
    case react::kSideFromCos:
        v0 = SideFromCos(S(a[0]), a[1], a[2]);
        ok = true;
        return true;
    case react::kRememberHandle:
        ok = RememberHandle(g, a[0], a[1], v0);
        return true;
    case react::kImpactSeverity:
        ok = ImpactSeverity(g, a[0], S(a[1]), S(a[2]), S(a[3]), S(a[4]), call.sp, c, v0);
        return true;
    case react::kBikeBikeReact:
        v0 = 0;
        ok = BikeBikeReact(g, a[0], a[1], a[2], a[3], call.sp, t, c);
        return true;
    case react::kTrafficSideShove:
        ok = TrafficSideShove(g, a[0], a[1], a[2], a[3], a[4], call.sp, v0);
        return true;
    case react::kBikeTrafficReact:
        v0 = 0;
        ok = BikeTrafficReact(g, a[0], a[1], a[2], a[3], a[4], call.sp, t, c);
        return true;
    case react::kBikeVsTraffic:
        v0 = 0;
        ok = BikeVsTraffic(g, a[0], a[1], call.sp, t, c);
        return true;
    default:
        return false;
    }
}

} // namespace rr::sim
