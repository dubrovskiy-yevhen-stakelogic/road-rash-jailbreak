#include "game/sim/recover_ground.h"

#include "game/sim/ai.h"
#include "game/sim/bike_step.h"
#include "game/sim/coll_util.h"
#include "game/sim/crash.h"
#include "game/sim/fixed.h"
#include "game/sim/vec.h"

namespace rr::sim {
namespace {

using cu::Add;
using cu::Iabs;
using cu::MulLo;
using cu::Neg;
using cu::Read16x3;
using cu::Read32x3;
using cu::S;
using cu::Sub;
using cu::U;
using cu::Write32x3;

constexpr uint32_t kGetUpJumpTable = 0x8005B774; // RASHCDG data: 12 words, stances 44..55 (0x8008FDBC)
constexpr uint32_t kPlayerRecords  = 0x800D43C0; // 28 bytes per player handle (0x80097C5C)
constexpr uint32_t kPadAxes        = 0x800CE540; // SLUS: {s32 throttle; s32 steer} per player (input.h)
constexpr uint32_t kPadRecords     = 0x800D7128; // SLUS: 192 bytes per player, +16 manual control
constexpr uint32_t kGetUpClipWord  = 0x80054354; // SLUS word: bits 4..15 the get-up clip (0x8008FDEC)
constexpr uint32_t kGetUpObjPtr    = 0x8005B3E4; // RASHCDG word: 0 or a record whose +192 is passed on

uint32_t GrKind(GuestRam& g, uint32_t st) { return g.U16(kRcStanceTable + 8u * st + 2u); }
uint32_t GrGs(GuestRam& g) { return g.U32(kRcGameStatePtr); }

// A guest s16 row, read and written back as the halfwords the original moves with lhu/sh.
void GrCopyRow(GuestRam& g, uint32_t src, uint32_t dst) {
    const uint16_t a = g.U16(src), b = g.U16(src + 2u), d = g.U16(src + 4u);
    g.W16(dst, a);
    g.W16(dst + 2u, b);
    g.W16(dst + 4u, d);
}
void GrNegRow(GuestRam& g, uint32_t a) {
    for (uint32_t k = 0; k < 3; ++k) g.W16(a + 2u * k, static_cast<uint16_t>(0u - g.U16(a + 2u * k)));
}
// Normalize in place, the identity reset when its v0 (the sum of squares) is 0.
bool GrNormReset(GuestRam& g, uint32_t v, uint32_t R, const BikeTables& t) {
    int32_t sum = 0;
    if (!rc::GNormalize(g, v, t, sum)) return false;
    if (sum == 0) BikeResetOrientation(g, R);
    return true;
}
// (x << 4) of a guest s16.
int32_t GrH4(GuestRam& g, uint32_t a) { return S(U(g.S16(a)) << 4); }

} // namespace

// ============================================================================ RASHCDG 0x8009A038
bool SetOp18(GuestRam& g, uint32_t B, uint32_t sp, const BikeTables& /*t*/, RecoverCallees& c) {
    const uint32_t F = sp - kSetOp18Frame;
    // The top of the command stack (+0x3B2 the depth, 8-byte commands from +0x3B4).
    const uint32_t cmd = B + 956u + 8u * U(g.S8(B + 946u)) - 8u;
    g.W16(cmd, 18);                                   // 0x8009A060
    const uint16_t h = g.U16(B + 172u);
    g.W16(cmd + 6u, 0);                               // 0x8009A074
    g.W16(cmd + 2u, h);                               // 0x8009A078
    const uint32_t clock = g.U32(GrGs(g) + 16u);
    g.W16(cmd + 6u, static_cast<uint16_t>(((clock * 2180u + 0x8000u) >> 16) | 0xC000u)); // 0x8009A0A8
    const uint32_t R = g.U32(B + 852u);
    if (g.U16(R + 544u) == 44u) {
        const uint16_t o432 = g.U16(R + 432u), o434 = g.U16(R + 434u), o436 = g.U16(R + 436u);
        const uint16_t o444 = g.U16(R + 444u), o446 = g.U16(R + 446u), o448 = g.U16(R + 448u);
        auto neg = [](uint16_t v) { return static_cast<uint16_t>(0u - v); };
        if (g.U32(R + 552u) & 0x08000000u) {
            g.W16(R + 444u, o432);                    // 0x8009A0F8
            g.W16(R + 432u, neg(o444));
            g.W16(R + 446u, o434);
            g.W16(R + 434u, neg(o446));
            g.W16(R + 448u, o436);
            g.W16(R + 436u, neg(o448));               // 0x8009A114
        } else {
            g.W16(R + 432u, o444);                    // 0x8009A124
            g.W16(R + 446u, neg(o434));
            g.W16(R + 444u, neg(o432));
            g.W16(R + 434u, o446);
            g.W16(R + 448u, neg(o436));
            g.W16(R + 436u, o448);                    // 0x8009A150
        }
    }
    GrCopyRow(g, R + 444u, R + 450u);                   // 0x8009A16C
    rc::CopyHalfwords(g, 9, R + 432u, R + 516u);      // 0x8009A174
    const uint32_t st = g.U16(R + 544u);
    const bool b70 = st == 51u || st == 45u || st == 61u || st == 62u || st == 68u || st == 57u || st == 53u;
    const uint32_t ev = b70 ? 70u : 69u;
    const uint32_t p = ((st - 60u) < 4u || st == 44u) ? 0x1000u : 0u;
    if (!rc::Call(c, kGrStanceEventFn, {ev, R, p}, F)) return false; // 0x8009A1F4
    const uint32_t gs = GrGs(g);
    g.W32(R + 488u, 0);                               // 0x8009A204
    g.W32(R + 480u, 0);
    const uint32_t hb = g.U16(B + 172u);
    if (hb < g.U32(gs + 48u)) {
        const uint32_t view = kRcViewArray + kRcViewStride * hb;
        if (!rc::Call(c, kGrCameraResetFn, {view}, F)) return false; // 0x8009A248
        g.W32(view + 548u, (g.U32(view + 548u) | 0x100000u) & 0xF5FFFFFFu); // 0x8009A268
    }
    g.W32(R + 552u, (g.U32(R + 552u) & 0xFEFFF07Fu) | 0x80u); // 0x8009A280
    return !g.Faulted();
}

// ============================================================================ RASHCDG 0x800986D0
bool GroundWalkRate(GuestRam& g, uint32_t R, uint32_t sp, const BikeTables& /*t*/, RecoverCallees& c,
                    uint32_t& v0) {
    const uint32_t F = sp - kGroundWalkRateFrame;
    const uint32_t st = g.U16(R + 544u);
    const bool walk = (st - 69u) < 2u;                // s2
    const bool kind8 = GrKind(g, st) == 8u;       // s0
    uint32_t res = 0;
    if (!walk && !kind8) {
        if (!rc::Call(c, kGrClipDoneFn, {g.U32(R + 540u)}, F, &res)) return false; // 0x80098924
    } else {
        uint32_t done = 0;
        if (!rc::Call(c, kGrClipDoneFn, {g.U32(R + 540u)}, F, &done)) return false; // 0x80098724
        if (done != 0u || kind8) {
            bool fast = false;                        // a2
            const uint32_t def = g.U32(g.U32(R + 596u) + 1084u);
            if (g.U8(def + 39u) != 254u) {
                if (walk && (g.U32(R + 552u) & 0x800u) == 0u) fast = true;
                else if (g.S32(R + 480u) > 0) fast = true;
                else fast = (g.U32(R + 552u) & 0x10u) != 0u;
            }
            const uint32_t cur = g.U16(R + 544u);
            const bool upright = walk || (cur - 72u) < 2u;
            const uint32_t base = upright ? 71u : cur;
            const uint32_t run = (g.S32(R + 480u) > 0x27FFF) ? 73u : 72u;
            const uint32_t next = (fast ? run : base) & 0xFFFFu;
            if (next != g.U16(R + 544u)) {
                const uint32_t p = ((0u - (walk ? 1u : 0u)) & 0x1000u) | 0x83u;
                if (!rc::Call(c, kGrStanceEventFn, {next, R, p}, F)) return false; // 0x80098854
                if (rc::IsPlayerRider(g, g.U16(R + 172u)) && walk) {
                    const uint32_t view = kRcViewArray + kRcViewStride * g.U16(g.U32(R + 596u) + 172u);
                    if (g.U32(view + 772u) == 0u) {
                        const uint32_t w548 = g.U32(view + 548u);
                        const uint32_t a = g.U32(view + 572u), b = g.U32(view + 576u), d = g.U32(view + 580u);
                        g.W32(view + 548u, w548 | 0x600000u); // 0x800988F4
                        g.W32(view + 804u, a);
                        g.W32(view + 808u, b);
                        g.W32(view + 812u, d);
                    }
                    g.W32(view + 548u, g.U32(view + 548u) & 0xFFEFFFFFu); // 0x80098914
                }
            }
        }
    }
    // 0x80098930: the walk cycle's clip rate.
    if ((g.U16(R + 544u) - 72u) < 2u) {
        const uint32_t prog = g.U32(g.U32(R + 540u) + 4u);
        int32_t rate;
        if (g.U8(prog + 1u) == 3u || g.U8(prog + 13u) == 3u) {
            rate = 15;
        } else {
            const int32_t v = g.S32(R + 480u);
            if (v > 0x7FFFF) {
                rate = 22;
            } else if (v > 0x27FFF) {
                const int32_t m = FixMul(11915, Add(v, S(0xFFF80000u)));
                rate = Add(S(U(m) * (0u - 11u)) >> 16, 33);
            } else if (v > 0xFFFF) {
                const int32_t m = FixMul(0xAAAA, Add(v, S(0xFFFD8000u)));
                rate = Add(S(0u - U(m) * 10u) >> 16, 50);
            } else {
                rate = 50;
            }
        }
        g.W32(g.U32(R + 540u) + 24u, rate == 0 ? 10u : U(rate)); // 0x80098A24 / 0x80098A30
    }
    v0 = res;
    return !g.Faulted();
}

// ============================================================================ RASHCDG 0x8009926C
bool GroundWalkControl(GuestRam& g, uint32_t R, int32_t dt, uint32_t /*sp*/, const BikeTables& /*t*/,
                       RecoverCallees& /*c*/) {
    int32_t s2 = 0x2AAAA; // the turn rate
    int32_t s1 = 0x80000; // the speed cap
    const bool s8 = ((g.U32(R + 552u) >> 11) & 1u) != 0u;
    const uint32_t two = (g.U8(R + 572u) >> 5) & 1u;
    if (s8 && (rc::IsPlayerRider(g, g.U16(R + 172u)) || two != 0u)) {
        const uint32_t bike = two != 0u ? g.U32(g.U32(R + 596u) + 856u) : g.U32(R + 596u);
        const uint32_t players = g.U32(GrGs(g) + 48u);
        const uint32_t hv = g.U16(bike + 172u);
        const uint32_t p = players < 2u ? hv + two : hv + 2u * two;
        if (g.U32(kPadRecords + 192u * p + 16u) != 0u) {
            const uint32_t pad = kPadAxes + 8u * p;
            int32_t x = g.S32(pad + 4u);
            if (x != 0) {
                const uint32_t f = g.U32(R + 552u) & 0xFFFFF9FFu;
                g.W32(R + 552u, f);                   // 0x80099398
                if (x > 0) {
                    g.W32(R + 552u, f | 0x400u);      // 0x800993A4
                } else {
                    g.W32(R + 552u, f | 0x200u);      // 0x800993AC
                    x = Neg(x);
                }
                s2 = FixMul(0x2AAAA, x);
            }
            const uint32_t a0 = g.U32(R + 552u);
            if ((a0 & 0x80u) == 0u) {
                const int32_t y = g.S32(pad);
                if (y != 0) {
                    const uint32_t f = a0 & 0xFFFFFE7Fu;
                    g.W32(R + 552u, f);               // 0x800993F8
                    if (y >= 0) {
                        g.W32(R + 552u, f | 0x80u);   // 0x80099420
                    } else {
                        g.W32(R + 552u, f | 0x100u);  // 0x8009940C
                        s1 = FixMul(0x80000, Neg(y));
                        g.W32(R + 480u, U(s1));       // 0x80099418
                    }
                }
            }
        }
    }
    // 0x80099424
    const uint32_t f = g.U32(R + 552u);
    const uint32_t s5 = (f >> 9) & 1u, s6 = (f >> 10) & 1u;
    const uint32_t s3 = (s5 | s6) == 0u ? 1u : 0u;
    const bool pos = g.S32(R + 488u) > 0;
    const uint32_t l16 = pos ? s3 : 0u;
    const uint32_t l20 = pos ? 0u : s3;
    const uint32_t s7 = s3 | (pos ? s5 : s6);
    if (s7 != 0u) s2 = S(U(s2) * 3u);
    if (!s8) {
        int32_t s0 = cu::Div4(g.S32(R + 484u));
        if (s0 > 0x30000) s0 = 0x30000;
        if (s0 > 0x17FFF) {
            const int32_t v1 = cu::SDiv(s1, s0);
            if (s2 < v1) s1 = FixMul(s2, s0);
            else s2 = v1;
        } else {
            const int32_t v1 = FixMul(s2, s0);
            if (s1 < v1) {
                s2 = cu::SDiv(s1, s0);
            } else {
                s1 = v1;
                if (s1 < 0x40000) s1 = 0x40000;
            }
        }
    }
    if (g.U32(R + 552u) & 0x80u) s1 = 0;
    int32_t a2 = FixMul(0x80000, dt);
    if ((g.U32(R + 552u) & 0x100u) == 0u && (!s8 || s3 != 0u)) a2 = Neg(S(U(a2) * 3u));
    int32_t k = S(((0u - (s5 | l16)) & 0xFFFAAAABu) + ((0u - (s6 | l20)) & 0x55555u));
    k = S(U(k) + ((0u - s7) & (U(k) << 1)));
    {
        const int32_t v1 = Add(g.S32(R + 480u), a2);
        const int32_t d = Sub(s1, v1);
        const int32_t sp480 = S((~U(v1 >> 31) & U(v1)) + (U(d >> 31) & U(d)));
        g.W32(R + 480u, U(sp480));                    // 0x80099680
    }
    const int32_t a0 = Add(g.S32(R + 488u), FixMul(k, dt));
    g.W32(R + 488u, U(a0));                           // 0x80099690
    const int32_t lo = l16 != 0u ? 0 : Neg(s2);
    if (l20 != 0u) s2 = 0;
    int32_t v1 = Sub(a0, lo);
    v1 = S(U(v1 >> 31) & U(Sub(lo, a0)));
    v1 = Add(a0, v1);
    const int32_t d = Sub(s2, a0);
    v1 = Add(v1, S(U(d >> 31) & U(d)));
    g.W32(R + 488u, U(v1));                           // 0x800996DC
    return !g.Faulted();
}

// ============================================================================ RASHCDG 0x8008FD5C
bool GroundGetUp(GuestRam& g, uint32_t R, uint32_t sp, const BikeTables& /*t*/, RecoverCallees& c, uint32_t& v0) {
    const uint32_t F = sp - kGroundGetUpFrame;
    const int32_t s0 = g.S32(R + 480u);
    uint32_t s2 = 0;
    if (s0 == 0) {
        const uint32_t st = g.U16(R + 544u);
        if ((st - 60u) >= 4u) s2 = (st - 67u) < 2u ? 0u : 1u;
    }
    v0 = s2;
    auto anim = [&g, R]() { return g.U32(R + 540u); };
    auto clipDone = [&](uint32_t& r) { return rc::Call(c, kGrClipDoneFn, {anim()}, F, &r); };
    auto event = [&](uint32_t ev, uint32_t p) { return rc::Call(c, kGrStanceEventFn, {ev, R, p}, F); };
    // The clip rate from the speed: `hi` past 0x1908B, `base` below.
    auto rate = [&](bool twenty) {
        int32_t v1;
        if (s0 > 0x165A1C) {
            v1 = 4;
        } else if (s0 > 0x1908B) {
            const int32_t m = FixMul(3152, Add(s0, S(0xFFFE6F74u)));
            v1 = twenty ? Add(S(0u - (U(m) << 4)) >> 16, 20) : Add(S(0u - U(m) * 26u) >> 16, 30);
        } else {
            v1 = twenty ? 20 : 30;
        }
        g.W32(anim() + 24u, v1 == 0 ? 10u : U(v1));
    };
    const uint32_t idx = g.U16(R + 544u) - 44u;
    const uint32_t target = idx < 12u ? g.U32(kGetUpJumpTable + 4u * idx) : 0x8009012Cu;
    switch (target) {
    case 0x8008FDD8u: { // 55
        uint32_t r = 0;
        if (!clipDone(r)) return false;
        if (r == 0u) break;
        const uint32_t clipWord = g.U32(kGetUpClipWord);
        const uint32_t obj = g.U32(kGetUpObjPtr);
        const uint32_t l24 = obj != 0u ? g.U32(obj + 192u) : 0u;
        if (!rc::Call(c, kGrRangedStartFn, {anim(), (clipWord >> 4) & 0xFFFu, 0u, 4u, 15u, 0u, l24}, F)) return false;
        if (!rc::Call(c, kGrSetRiderStateFn, {48u, R, 0u}, F)) return false; // 0x8008FE38
        break;
    }
    case 0x8008FE48u: // 50, 51
        rate(false);
        s2 = s0 > 0x15752 ? 0u : 1u;
        break;
    case 0x8008FEDCu: { // 48
        rate(true);
        const uint32_t a = anim();
        const uint32_t rec = g.U32(a + 4u) + 12u * g.U32(a + 12u);
        const uint32_t clip = g.U32(g.U32(g.U32(a + 40u) + 4u) + 4u * g.U8(rec));
        const uint32_t last = g.U16(clip + 16u) - 1u;
        const int32_t frame = g.S32(a + 16u);
        bool up = s2 != 0u;
        if (!up && s0 <= 0x1C9C3) up = cu::Div4(static_cast<int16_t>(last) * 3) < frame;
        if (up) {
            const uint32_t p = (g.U32(R + 552u) & 0x08000000u) ? 2u : 258u;
            if (!event(44u, p)) return false; // 0x8008FFF0
            s2 = 0;
            break;
        }
        uint32_t r = 0;
        if (!clipDone(r)) return false; // 0x80090004
        if (r != 0u && !event(48u, 1u)) return false;
        break;
    }
    case 0x80090020u: { // 49
        rate(true);
        uint32_t r = 0;
        if (!clipDone(r)) return false; // 0x80090094
        if (r == 0u) break;
        const uint32_t n = g.U8(R + 535u) + 1u;
        g.W8(R + 535u, static_cast<uint8_t>(n)); // 0x800900B0
        if (S(n << 24) > 0) {
            const uint32_t rnd = GuestRand(g);
            const uint32_t m = rnd % 3u;
            const uint32_t ev = (m == 0u ? 57u : 55u) + (m == 1u ? 1u : 0u);
            if (!event(ev, 2u)) return false;
        } else {
            if (!event(49u, 0u)) return false;
        }
        break;
    }
    case 0x80090118u: { // 44
        uint32_t r = 0;
        if (!clipDone(r)) return false;
        s2 = r;
        break;
    }
    case 0x8009012Cu: { // the rest
        uint32_t r = 0;
        if (!clipDone(r)) return false; // 0x80090130
        if (r == 0u) break;
        uint32_t next = 0;
        if (!rc::Call(c, kGrFollowStanceFn, {R, F + 32u}, F, &next)) return false; // 0x80090140
        s2 = (s2 != 0u || ((next - 69u) & 0xFFFFu) < 2u) ? 1u : 0u;
        if (s2 == 0u && !event(next & 0xFFFFu, g.U32(F + 32u))) return false; // 0x80090184
        break;
    }
    default:
        return false; // a table word this port does not know
    }
    v0 = s2;
    return !g.Faulted();
}

// ============================================================================ RASHCDG 0x8008F754
bool GroundSlide(GuestRam& g, uint32_t R, int32_t dt, uint32_t timer, uint32_t /*sp*/, const BikeTables& t,
                 RecoverCallees& /*c*/) {
    const bool s5 = g.S16(R + 524u) < 3547;
    const uint32_t st = g.U16(R + 544u);
    bool s4 = true;
    if (st == 45u || st == 50u || st == 51u) {
        const uint16_t a2 = g.U16(R + 450u), a1 = g.U16(R + 454u);
        const uint32_t v = Iabs(static_cast<int16_t>(a1)) < Iabs(static_cast<int16_t>(a2))
                               ? (a2 ^ g.U16(R + 444u))
                               : (a1 ^ g.U16(R + 448u));
        s4 = (v & 0x8000u) == 0u;
    }
    int32_t vel[3];
    {
        const int32_t wa = FixMul(GrH4(g, R + 446u), 0x9D087);
        const int32_t wb = FixMul(GrH4(g, R + 434u), 0x9D087);
        int16_t a[3], b[3];
        Read16x3(g, R + 444u, a);
        Read16x3(g, R + 432u, b);
        Blend16To32(a, b, vel, wa, wb);               // 0x8008F848
    }
    if (g.S32(R + 480u) == 0) {
        if (!s5 || (g.U16(R + 172u) >> 5) != 1u) return !g.Faulted();
        const int32_t sum = Add(Add(FixMul(vel[0], GrH4(g, R + 622u)), FixMul(vel[1], GrH4(g, R + 624u))),
                                FixMul(vel[2], GrH4(g, R + 626u)));
        if (sum > 0) g.W32(R + 480u, 0x10000u);       // 0x8008F90C
        if (g.S32(R + 480u) == 0) return !g.Faulted();
    }
    // 0x8008F920
    const bool s3 = st == 45u || st == 55u || st == 56u || st == 57u || st == 52u || st == 53u || st == 58u ||
                    st == 59u || st == 54u || st == 49u;
    int32_t tv = Sub(g.S32(timer), dt);
    g.W32(timer, U(tv));                              // 0x8008F994
    if (tv < 0) tv = 0;
    g.W32(timer, U(tv));                              // 0x8008F99C
    {
        const int32_t v1 = tv < 0x10000 ? 0x10000 : tv;
        tv = S(U(tv) + (U(Sub(v1, tv)) & (0u - (s5 ? 1u : 0u))));
    }
    g.W32(timer, U(tv));                              // 0x8008F9C4
    const uint32_t drag = (st - 50u) < 2u ? 0xFFFF0000u : 0xFFFF8000u;
    const int32_t wa = FixMul(tv, dt);
    const uint32_t on = s5 ? 0u : (s3 ? 0u : 1u);
    const int32_t wb = FixMul(S((0u - on) & drag), dt);
    {
        int32_t v[3], o[3];
        Read32x3(g, R + 456u, v);
        Blend32(vel, v, o, wa, wb);                   // 0x8008FA20
        for (int k = 0; k < 3; ++k) vel[k] = o[k];
        for (uint32_t k = 0; k < 3; ++k) g.W32(R + 456u + 4u * k, U(Add(g.S32(R + 456u + 4u * k), vel[k])));
    }
    const int32_t oldLen = g.S32(R + 480u);
    {
        int32_t v[3];
        Read32x3(g, R + 456u, v);
        g.W32(R + 480u, U(Length3(v, t.sqrt)));       // 0x8008FA68
    }
    {
        const int32_t v1 = g.S32(timer);
        g.W32(timer, v1 > 0 ? U(Sub(v1, dt)) : 0u);   // 0x8008FA80
    }
    int16_t prev[3];
    Read16x3(g, R + 450u, prev);
    if (g.S32(R + 480u) != 0) ScaleTo16(g, rc::Recip(g.S32(R + 480u)), R + 456u, R + 450u); // 0x8008FB04
    {
        int16_t now[3];
        Read16x3(g, R + 450u, now);
        if (DotLcm(now, prev) < 0) {
            const uint32_t a = g.U32(R + 540u);
            const uint32_t rec = g.U32(a + 4u) + 12u * g.U32(a + 12u);
            const uint8_t b = g.U8(rec + 2u);
            if (b & 1u) {
                g.W8(rec + 2u, static_cast<uint8_t>(b & 0xFEu)); // 0x8008FB60
                g.W32(R + 552u, g.U32(R + 552u) & 0xF7FFFFFFu);
            } else {
                g.W8(rec + 2u, static_cast<uint8_t>(b | 1u));    // 0x8008FB78
                g.W32(R + 552u, g.U32(R + 552u) | 0x08000000u);
            }
        }
    }
    {
        const int32_t v1 = g.S32(R + 480u);
        if (v1 < oldLen && (s3 || (s5 && v1 <= 0x15752))) {
            g.W32(R + 480u, U(oldLen));               // 0x8008FBC0
            int16_t d[3];
            int32_t o[3];
            Read16x3(g, R + 450u, d);
            Scale(oldLen, d, o);
            Write32x3(g, R + 456u, o);                // 0x8008FBCC
        }
    }
    GrCopyRow(g, R + 450u, R + 444u);                   // 0x8008FBE0
    if (!s4) GrNegRow(g, R + 444u);
    GrCopyRow(g, R + 522u, R + 438u);                   // 0x8008FC20
    rc::GteOp(g, R + 438u, R + 444u, R + 432u);       // 0x8008FC68
    if (!GrNormReset(g, R + 432u, R, t)) return false;
    rc::GteOp(g, R + 432u, R + 438u, R + 444u);       // 0x8008FCD8
    GrCopyRow(g, R + 444u, R + 450u);                   // 0x8008FD00
    if (!s4) GrNegRow(g, R + 450u);
    return !g.Faulted();
}

// ============================================================================ RASHCDG 0x80097BCC
bool RiderOnGround(GuestRam& g, uint32_t R, int32_t dt, uint32_t sp, const BikeTables& t, RecoverCallees& c) {
    const uint32_t F = sp - kRiderOnGroundFrame;
    // ---- the wall / junction bounce (0x80097BF0 .. 0x80098008)
    if ((g.U32(R + 552u) & 0x18u) == 0u) {
        bool s4 = false, s3 = false;
        uint32_t s2 = 0; // -> the s16 normal the bounce takes
        // Is the rider on the far side of a record's plane: (n.x * dx + n.z * dz) > 0, mult lo.
        auto behind = [&](uint32_t rec) {
            const int32_t dx = Sub(g.S32(rec + 4u), g.S32(R + 184u)) >> 16;
            const int32_t dz = Sub(g.S32(rec + 12u), g.S32(R + 192u)) >> 16;
            return Add(MulLo(g.S16(rec + 16u), dx), MulLo(g.S16(rec + 20u), dz)) > 0;
        };
        if (rc::IsPlayerRider(g, g.U16(R + 172u))) {
            const uint32_t rec = kPlayerRecords + 28u * g.U16(g.U32(R + 596u) + 172u);
            if (g.S16(rec + 22u) != 0 && g.U32(rec) == g.U32(R + 360u)) {
                int32_t a2 = Iabs(Sub(g.S32(rec + 4u), g.S32(R + 184u)) >> 16);
                int32_t a1 = Iabs(Sub(g.S32(rec + 12u), g.S32(R + 192u)) >> 16);
                if (a2 < a1) {
                    const int32_t x = a2;
                    a2 = a1;
                    a1 = x;
                }
                const int32_t a0 = a1 + (a1 >> 1);
                const int32_t d = a2 - (a2 >> 5) - (a2 >> 7) + (a0 >> 2) + (a0 >> 6);
                if (d < 50) {
                    if (cu::GDot(g, R + 450u, rec + 16u) <= 0) s4 = behind(rec);
                    s2 = rec + 16u;
                }
            }
        }
        if ((g.U32(R + 388u) & 1u) && g.U32(R + 372u) != 0u && ((g.U32(R + 36u) >> 9) & 1u) == 0u) {
            const uint32_t sl = g.U32(R + 340u);
            const int32_t lat = g.S32(R + 344u);
            const bool margins = lat > 0 ? g.U8(sl + 48u) != 0u : g.U8(sl + 49u) != 0u;
            if (margins) {
                const int32_t t0 = cu::GDot(g, R + 450u, g.U32(R + 340u) + 2u); // 0x80097DDC
                const uint32_t a0 = g.U32(R + 340u);
                if (S(U(g.U8(a0 + 48u)) << 15) < lat && t0 > 0) s3 = true;
                else if (lat < Neg(S(U(g.U8(a0 + 49u)) << 15)) && t0 < 0) s3 = true;
                if (s3) {
                    const uint32_t sl2 = g.U32(R + 340u);
                    s2 = sl2 + 2u;
                    if (t0 > 0) {
                        g.W16(F + 40u, static_cast<uint16_t>(0u - g.U16(sl2 + 2u))); // 0x80097E54
                        g.W16(F + 42u, static_cast<uint16_t>(0u - g.U16(s2 + 2u)));
                        g.W16(F + 44u, static_cast<uint16_t>(0u - g.U16(s2 + 4u)));
                        s2 = F + 40u;
                    }
                }
            } else {
                uint32_t jr = 0;
                if (!rc::Call(c, kGrJunctionMarginFn, {R}, F, &jr)) return false; // 0x80097E7C
                if (jr != 0u) {
                    if (cu::GDot(g, R + 450u, jr + 16u) <= 0) s3 = behind(jr);
                    if (s3) s2 = jr + 16u;
                }
            }
        }
        if (s3 || s4) {
            // 0x80097F04
            g.W32(R + 552u, g.U32(R + 552u) | 2u);
            const uint32_t v1 = g.U32(R + 552u);
            g.W16(R + 622u, g.U16(s2));               // 0x80097F1C
            g.W16(R + 624u, g.U16(s2 + 2u));
            const uint16_t n2 = g.U16(s2 + 4u);
            g.W32(R + 480u, 0);
            g.W32(R + 464u, 0);
            g.W32(R + 460u, 0);
            g.W32(R + 456u, 0);
            g.W16(R + 626u, n2);                      // 0x80097F40
            if (v1 & 0x800u) {
                g.W32(R + 552u, (v1 & 0xFFEFFEFFu) | 0x80u); // 0x80097F60
            } else if ((v1 & 0x100000u) == 0u) {
                g.W32(R + 552u, v1 | 0x100001u);      // 0x80097F88
                g.W32(R + 600u, FixDiv(0x40000u, 0x80000u)); // 0x80097F98
                const int32_t d = cu::GDot(g, s2, R + 516u);
                g.W16(R + 516u, g.U16(s2));           // 0x80097FA4
                g.W16(R + 518u, g.U16(s2 + 2u));
                const uint16_t z = g.U16(s2 + 4u);
                const uint32_t f = g.U32(R + 552u) & 0xFFF9FFFFu;
                g.W32(R + 552u, f);                   // 0x80097FC0
                g.W16(R + 520u, z);
                if (d < 0) {
                    g.W16(R + 516u, static_cast<uint16_t>(0u - g.U16(R + 516u))); // 0x80097FD8
                    g.W16(R + 520u, static_cast<uint16_t>(0u - z));
                    g.W16(R + 518u, static_cast<uint16_t>(0u - g.U16(R + 518u)));
                    g.W32(R + 552u, g.U32(R + 552u) | 0x40000u); // 0x80098004
                } else {
                    g.W32(R + 552u, f | 0x20000u);
                }
                g.W32(R + 488u, 0);                   // 0x80098008
            }
        }
    }
    // ---- 0x8009800C
    uint32_t up = 0; // s0
    if (g.S32(R + 604u) == 3) {
        // tumbling
        if (rc::IsPlayerRider(g, g.U16(R + 172u)) || (g.U8(R + 572u) & 0x20u) != 0u) {
            if (g.S32(R + 480u) > 0x30000) {
                if (!rc::Call(c, kGrEffectBurstFn, {R, 3u, 300u, 1u}, F)) return false; // 0x80098078
            }
        }
        if (g.U32(R + 552u) & 1u) {
            int16_t d[3];
            int32_t o[3];
            Read16x3(g, R + 450u, d);
            Scale(g.S32(R + 480u), d, o);
            Write32x3(g, R + 456u, o);                // 0x80098098
        }
        if (!rc::Call(c, kGroundSlideFn, {R, U(dt), R + 600u}, F)) return false; // 0x800980A8
        rc::CopyHalfwords(g, 9, R + 432u, R + 516u);  // 0x800980B8
        if (!rc::Call(c, kGroundGetUpFn, {R}, F, &up)) return false; // 0x800980C0
        if (g.U32(R + 552u) & 0x80000u) up = 0;
    } else {
        // up: the heading turned by +0x1E8
        if (g.S32(R + 488u) != 0) {
            const int32_t m = FixMul(g.S32(R + 488u), dt);
            const uint32_t idx = ((U(m) * 163u) >> 14) & 0xFFFu;
            const int32_t sn = g.S16(kRcSinCos + 4u * idx);
            const int32_t cs = g.S16(kRcSinCos + 4u * idx + 2u);
            int16_t a[3], b[3], o[3];
            Read16x3(g, R + 528u, a);
            Read16x3(g, R + 516u, b);
            Blend16(a, b, o, Neg(S(U(sn) << 4)), S(U(cs) << 4));
            cu::Write16x3(g, R + 516u, o);            // 0x8009815C
        }
        if (g.U32(R + 552u) & 1u) {
            rc::GteOp(g, R + 516u, R + 522u, R + 528u); // 0x800981B4
            if (!GrNormReset(g, R + 528u, R, t)) return false;
            rc::GteOp(g, R + 522u, R + 528u, R + 516u); // 0x80098224
            const int32_t s0 = Add(FixMul(GrH4(g, R + 450u), GrH4(g, R + 522u)), FixMul(GrH4(g, R + 454u), GrH4(g, R + 526u)));
            const int32_t h = GrH4(g, R + 524u);
            uint32_t v;
            if (s0 > 0) {
                v = h > 0 ? (0u - FixDiv(U(s0), U(h))) >> 4 : FixDiv(U(s0), U(Neg(h))) >> 4;
            } else {
                v = h > 0 ? FixDiv(U(Neg(s0)), U(h)) >> 4 : (0u - FixDiv(U(Neg(s0)), U(Neg(h)))) >> 4;
            }
            g.W16(R + 452u, static_cast<uint16_t>(v)); // 0x800983BC
            if (!GrNormReset(g, R + 450u, R, t)) return false;
        } else if (g.S32(R + 488u) != 0) {
            if (!GrNormReset(g, R + 516u, R, t)) return false; // 0x800983F0
            rc::GteOp(g, R + 516u, R + 522u, R + 528u); // 0x80098444
        }
        const uint32_t f = g.U32(R + 552u);
        bool rateOnly = false;
        if (f & 0x10u) {
            cu::GMulAdd(g, R + 456u, R + 516u, g.S32(R + 484u), R + 184u); // 0x80098480
            rateOnly = true;
        } else if (f & 0x60000u) {
            const uint32_t f1 = f & 0xFFFFF9FFu;
            g.W32(R + 552u, f1);                      // 0x800984B0
            g.W32(R + 552u, (f1 & 0x20000u) ? (f1 | 0x400u) : (f1 | 0x200u)); // 0x800984C0
            const uint32_t B = g.U32(R + 596u);
            const int32_t d0 = Sub(g.S32(B + 184u), g.S32(R + 184u));
            const int32_t d1 = Sub(g.S32(B + 188u), g.S32(R + 188u));
            const int32_t d2 = Sub(g.S32(B + 192u), g.S32(R + 192u));
            const uint32_t t1 = g.U32(R + 552u);
            bool clear = (t1 & 0x800u) != 0u;
            if (!clear) {
                const int32_t sum = Add(Add(FixMul(d0, GrH4(g, R + 528u)), FixMul(d1, GrH4(g, R + 530u))),
                                        FixMul(d2, GrH4(g, R + 532u)));
                clear = sum > 0;
            }
            if (clear) g.W32(R + 552u, t1 & 0xFFF9FFFFu); // 0x800985C4
        }
        if (!rateOnly && GrKind(g, g.U16(R + 544u)) == 8u) {
            if (!rc::Call(c, kGroundWalkControlFn, {R, U(dt)}, F)) return false; // 0x800985EC
        }
        if (!rc::Call(c, kGroundWalkRateFn, {R}, F, &up)) return false; // 0x800985F4
    }
    // ---- 0x80098600
    if (up != 0u) {
        if (!rc::Call(c, kSetOp18Fn, {g.U32(R + 596u)}, F)) return false; // 0x8009860C
    }
    if (g.S32(R + 604u) == 4 || up != 0u) {
        const uint16_t v1 = g.U16(R + 528u), a1 = g.U16(R + 530u), a2 = g.U16(R + 532u);
        g.W16(R + 434u, 0);                           // 0x8009863C
        g.W16(R + 450u, v1);
        g.W16(R + 452u, a1);
        g.W16(R + 454u, a2);
        g.W16(R + 432u, a2);
        g.W16(R + 436u, static_cast<uint16_t>(0u - v1)); // 0x80098660
        int32_t sum = 0;
        if (!rc::GNormalize(g, R + 432u, t, sum)) return false;
        if (sum == 0) {
            g.W16(R + 432u, 4096);                    // 0x8009866C
            g.W16(R + 436u, 0);
        }
        g.W16(R + 440u, 4096);                        // 0x80098678
        const uint16_t z = g.U16(R + 436u), x = g.U16(R + 432u);
        g.W16(R + 446u, 0);
        g.W16(R + 438u, 0);
        g.W16(R + 442u, 0);
        g.W16(R + 444u, static_cast<uint16_t>(0u - z));
        g.W16(R + 448u, x);                           // 0x80098698
    }
    g.W32(R + 552u, g.U32(R + 552u) & 0xFFFFFFFEu); // 0x800986A8
    return !g.Faulted();
}

} // namespace rr::sim
