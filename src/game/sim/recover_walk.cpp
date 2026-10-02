#include "game/sim/recover_walk.h"

#include <utility>

#include "game/sim/ai.h"
#include "game/sim/bike_step.h"
#include "game/sim/coll_util.h"
#include "game/sim/crash.h"
#include "game/sim/fixed.h"
#include "game/sim/road_runtime.h"
#include "game/sim/vec.h"

namespace rr::sim {

namespace {

using rc::S;
using rc::U;
using cu::Iabs;

int32_t Dot3(GuestRam& g, uint32_t a, uint32_t b) {
    int16_t x[3], y[3];
    cu::Read16x3(g, a, x);
    cu::Read16x3(g, b, y);
    return DotLcm(x, y);
}
void GMulAdd(GuestRam& g, uint32_t base, uint32_t dir, int32_t t, uint32_t out) { cu::GMulAdd(g, base, dir, t, out); }
void GScale(GuestRam& g, int32_t t, uint32_t dir, uint32_t out) { cu::GScale(g, t, dir, out); }
int32_t GLength3(GuestRam& g, uint32_t v, const BikeTables& t) {
    int32_t x[3];
    cu::Read32x3(g, v, x);
    return Length3(x, t.sqrt);
}
int32_t GSumSquares(GuestRam& g, uint32_t v) {
    int32_t x[3];
    cu::Read32x3(g, v, x);
    return SumSquares(x);
}
// `mult d, (lh r) << 4` summed over three, the lo/hi of the third product left at F+off / F+off+4.
int32_t Proj3(GuestRam& g, const int32_t d[3], uint32_t row, uint32_t spill) {
    int32_t s = 0;
    int64_t last = 0;
    for (uint32_t k = 0; k < 3; ++k) {
        const int32_t r = S(U(static_cast<int32_t>(g.S16(row + 2u * k))) << 4);
        last = static_cast<int64_t>(d[k]) * r;
        s = S(U(s) + U(FixMul(d[k], r)));
    }
    if (spill != 0) {
        g.W32(spill, static_cast<uint32_t>(static_cast<uint64_t>(last)));
        g.W32(spill + 4u, static_cast<uint32_t>(static_cast<uint64_t>(last) >> 32));
    }
    return s;
}
void Neg16x3(GuestRam& g, uint32_t a) {
    for (uint32_t k = 0; k < 3; ++k) g.W16(a + 2u * k, static_cast<uint16_t>(0u - g.U16(a + 2u * k)));
}
void Copy16x3(GuestRam& g, uint32_t src, uint32_t dst) {
    for (uint32_t k = 0; k < 3; ++k) g.W16(dst + 2u * k, g.U16(src + 2u * k));
}
bool Big(int32_t x) { return Iabs(x) > 0x5A8000; }
// The octagonal distance of the x/z integer parts: max - max/32 - max/128 + (1.5 min)/4 + (1.5 min)/64.
int32_t Octagonal(int32_t dx, int32_t dz) {
    int32_t a = Iabs(dx >> 16), b = Iabs(dz >> 16);
    if (a < b) std::swap(a, b);
    const int32_t m = b + (b >> 1);
    return S(U(a) - U(a >> 5) - U(a >> 7) + U(m >> 2) + U(m >> 6));
}
// SLUS 0x8003B8F4 RouteBindingValid(p = e + 0xAC), a leaf over 0x8003B4B0.
int32_t RouteBindingValidAt(GuestRam& g, uint32_t p) {
    if (p == 0) return 0;
    const uint32_t o = g.U32(p + 256u);
    if (o == 0) return 0;
    const uint32_t w = g.U32(p + 188u);
    if ((w >> 16) == 1u) return (w & 0xFFFFu) == g.U32(o) ? 1 : 0;
    return RouteFindLegView(g, o, w & 0xFFFFu) != 0 ? 1 : 0;
}
uint32_t View(GuestRam&, uint32_t h) { return kRcViewArray + kRcViewStride * h; }

} // namespace

// ============================================================================ the leaves

bool AngleBetween(GuestRam& g, uint32_t v, uint32_t a, uint32_t b, const BikeTables& t, int32_t& v0) {
    const int32_t x = Dot3(g, a, v);                                      // 0x80093BFC
    const int32_t y = Dot3(g, b, v);                                      // 0x80093C0C
    int32_t as = 0;
    if (Iabs(x) < Iabs(y)) {
        if (!Asin(x, t.asin, as)) return false;
        const int32_t r = S(1024u - U(as));
        v0 = y < 0 ? S(0u - U(r)) : r;
        return true;
    }
    if (!Asin(y, t.asin, as)) return false;
    const int32_t v1 = S(1024u - U(as));
    if (x > 0) v0 = S(1024u - U(v1));
    else v0 = v1 < 1024 ? S(U(v1) + 1024u) : S(U(v1) - 3072u);
    return true;
}

void AxisRotation(GuestRam& g, uint32_t axis, int32_t ang, uint32_t out, uint32_t sp, const BikeTables& t) {
    const uint32_t F = sp - 112u;                                         // `addiu sp,sp,-112`
    const uint32_t i = U(ang) & 0xFFFu;
    for (uint32_t k = 0; k < 3; ++k) g.W32(F + 56u + 4u * k, U(static_cast<int32_t>(g.S16(axis + 2u * k))) << 4);
    const int32_t sn = S(U(static_cast<int32_t>(t.sincos[2u * i])) << 4);      // s2
    const int32_t cs = S(U(static_cast<int32_t>(t.sincos[2u * i + 1u])) << 4); // s3
    const int32_t omc = S(0x10000u - U(cs));                              // s5
    auto axisW = [&](uint32_t k) { return S(U(static_cast<int32_t>(g.S16(axis + 2u * k))) << 4); };
    auto cross = [&](uint32_t p, uint32_t q) {                            // ((lo(p*q) << 4) >> 16) << 4
        const uint32_t lo = static_cast<uint32_t>(static_cast<int32_t>(g.S16(axis + 2u * p)) *
                                                  static_cast<int32_t>(g.S16(axis + 2u * q)));
        return S(U(S(lo << 4) >> 16) << 4);
    };
    int32_t m[9];
    // x: 0x8003FBB8..0x8003FC38
    {
        const int32_t s8 = FixMul(axisW(0), axisW(0));
        const int32_t s7 = FixMul(axisW(0), sn);
        const int32_t s6 = FixMul(cross(1, 2), omc);
        const int32_t v0 = FixMul(cs, S(0x10000u - U(s8)));
        m[0] = S(U(s8) + U(v0));
        m[7] = S(U(s6) - U(s7));
        m[5] = S(U(s6) + U(s7));
    }
    // y: 0x8003FC3C..0x8003FCB4
    {
        const int32_t s8 = FixMul(axisW(1), axisW(1));
        const int32_t s7 = FixMul(axisW(1), sn);
        const int32_t s6 = FixMul(cross(2, 0), omc);
        const int32_t v0 = FixMul(cs, S(0x10000u - U(s8)));
        m[4] = S(U(s8) + U(v0));
        m[6] = S(U(s6) + U(s7));
        m[2] = S(U(s6) - U(s7));
    }
    // z: 0x8003FCB8..0x8003FD34
    {
        const int32_t s8 = FixMul(axisW(2), axisW(2));
        const int32_t s7 = FixMul(axisW(2), sn);
        const int32_t s6 = FixMul(cross(0, 1), omc);
        const int32_t v0 = FixMul(cs, S(0x10000u - U(s8)));
        m[8] = S(U(s8) + U(v0));
        m[1] = S(U(s6) + U(s7));
        m[3] = S(U(s6) - U(s7));
    }
    for (uint32_t k = 0; k < 9; ++k) g.W32(F + 16u + 4u * k, U(m[k]));
    for (uint32_t k = 0; k < 9; ++k) g.W16(out + 2u * k, static_cast<uint16_t>(m[k] >> 4)); // 0x8003FD38..
}

void VecMat(GuestRam& g, uint32_t v, uint32_t m, uint32_t out) {
    for (uint32_t j = 0; j < 3; ++j) {
        uint32_t s = 0;
        for (uint32_t i = 0; i < 3; ++i) {
            const int32_t p = static_cast<int32_t>(g.S16(v + 2u * i)) * static_cast<int32_t>(g.S16(m + 2u * (3u * i + j)));
            s += U(p >> 12);
        }
        g.W16(out + 2u * j, static_cast<uint16_t>(s));
    }
}

// ============================================================================ the walk

bool WalkTarget(GuestRam& g, uint32_t R, uint32_t B, uint32_t dirA, uint32_t dirB, uint32_t sp,
                const BikeTables& t, RecoverCallees& c, uint32_t& v0) {
    const uint32_t F = sp - 104u;                                         // `addiu sp,sp,-104`
    v0 = 1;
    if (Iabs(g.S32(B + 652u)) == 0x1921F) {                               // 0x80098A98: lying on its side
        Copy16x3(g, B + 438u, dirA);
        Copy16x3(g, B + 444u, dirB);
    } else {
        Copy16x3(g, B + 516u, dirA);                                      // 0x80098AE8
        if (g.S32(B + 652u) > 0) Neg16x3(g, dirA);
        Copy16x3(g, B + 528u, dirB);
    }
    GMulAdd(g, B + 504u, dirA, S(0xFFFEE000u), R + 580u);                 // 0x80098B78
    GMulAdd(g, R + 580u, dirB, -2051, R + 580u);                          // 0x80098B8C
    if (g.S32(B + 652u) < 0) Neg16x3(g, dirB);
    if (g.U32(R + 552u) & 0x18u) return true;                             // 0x80098BD4
    int32_t s5 = 0x10000;
    if (g.S32(R + 480u) == 0) {
        int32_t d[3];
        for (uint32_t k = 0; k < 3; ++k) d[k] = S(g.U32(R + 184u + 4u * k) - g.U32(R + 580u + 4u * k));
        cu::Write32x3(g, F + 40u, d);
        if (Octagonal(d[0], d[2]) < 6) {                                  // 0x80098C7C
            s5 = GLength3(g, F + 40u, t) >> 1;
            if (0x10000 < s5) s5 = 0x10000;
            if (s5 < 6553) {
                if (!rc::Call(c, kReSeatFn, {B}, F)) return false;        // 0x80098CB4
                v0 = 0;
                return true;
            }
        }
    }
    if (g.U32(R + 552u) & 0x800u) return true;                            // 0x80098CD0
    const int32_t s0 = FixMul(S(U(static_cast<int32_t>(g.S16(dirA + 4u))) << 4), S(g.U32(R + 184u) - g.U32(R + 580u)));
    const int32_t q = FixMul(S(U(static_cast<int32_t>(g.S16(dirA))) << 4), S(g.U32(R + 192u) - g.U32(R + 588u)));
    const int32_t s6 = S(U(s0) - U(q));
    GMulAdd(g, R + 580u, dirB, s6 > 0 ? s5 : S(0u - U(s5)), R + 456u);  // 0x80098D24
    int32_t s3 = 1;
    int32_t d[3];
    for (uint32_t k = 0; k < 3; ++k) d[k] = S(g.U32(R + 184u + 4u * k) - g.U32(R + 456u + 4u * k));
    cu::Write32x3(g, F + 40u, d);
    if (Big(d[0]) || Big(d[1]) || Big(d[2])) {                            // 0x80098D9C: halve to fit
        s3 = 0;
        do {
            for (int32_t& x : d) x >>= 1;
            cu::Write32x3(g, F + 40u, d);
        } while (Big(d[0]) || Big(d[1]) || Big(d[2]));
    }
    int32_t len = GLength3(g, F + 40u, t);                                // 0x80098E04
    if (len < 655) len = 655;
    const int32_t recip = rc::Recip(len);
    ScaleTo16(g, recip, F + 40u, F + 56u);                                // 0x80098E78
    uint32_t src = F + 56u;
    if (s3 != 0) {
        int32_t x = FixMul(s5, recip);                                    // 0x80098E88
        if (0x10000 < x) x = 0x10000;
        int32_t as = 0;
        if (!Asin(x, t.asin, as)) return false;
        const int32_t a1 = S(1024u - U(as));
        AxisRotation(g, B + 522u, s6 < 0 ? S(0u - U(a1)) : a1, F + 16u, F, t);   // 0x80098EC8: the BIKE's up
        VecMat(g, F + 56u, F + 16u, F + 64u);                             // 0x80098EDC
        src = F + 64u;
    }
    GMulAdd(g, R + 456u, src, s5, R + 580u);                              // 0x80098EF8
    v0 = 1;
    return !g.Faulted();
}

bool WalkPivot(GuestRam& g, uint32_t R, uint32_t dirA, uint32_t dirB, int32_t sq, int32_t flag, uint32_t sp,
               const BikeTables& t) {
    const uint32_t F = sp - 72u;                                          // `addiu sp,sp,-72`
    int32_t s0 = Dot3(g, R + 516u, dirA);                                 // 0x80098F60
    int32_t d[3];
    for (uint32_t k = 0; k < 3; ++k) d[k] = S(g.U32(R + 184u + 4u * k) - g.U32(R + 580u + 4u * k));
    cu::Write32x3(g, F + 16u, d);
    if (s0 < 655) {                                                       // 0x80098FA0
        if (s0 > 0) s0 = 655;
        else if (!(s0 < -654)) s0 = -655;
    }
    const int32_t recip = rc::Recip(s0);
    const int32_t a = FixMul(Proj3(g, d, dirA, F + 32u), recip);          // 0x80099094
    g.W32(R + 592u, U(a));
    const int32_t b = FixMul(Proj3(g, d, R + 528u, F + 32u), recip);      // 0x80099128
    g.W32(R + 600u, U(b));
    if (S(g.U32(R + 592u) ^ U(b)) >= 0) {                                 // 0x8009913C
        GMulAdd(g, R + 580u, dirB, S(0u - U(b)), R + 456u);
        GMulAdd(g, R + 456u, R + 516u, g.S32(R + 592u), F + 16u);
        for (uint32_t k = 0; k < 3; ++k) g.W32(F + 16u + 4u * k, g.U32(F + 16u + 4u * k) - g.U32(R + 184u + 4u * k));
        GMulAdd(g, R + 456u, dirB, g.S32(R + 600u), F + 16u);
        for (uint32_t k = 0; k < 3; ++k) g.W32(F + 16u + 4u * k, g.U32(F + 16u + 4u * k) - g.U32(R + 580u + 4u * k));
        g.W32(R + 480u, 0);                                               // 0x800991FC
        g.W32(R + 552u, (g.U32(R + 552u) | 0x10u) & ~8u);
        return !g.Faulted();
    }
    g.W32(R + 552u, g.U32(R + 552u) | (recip >= 0 ? 0x500u : 0x300u));  // 0x80099210
    g.W32(R + 484u, flag != 0 ? 0x7FFF0000u : U(S(U(SqrtGte(sq, t.sqrt)) << 2)));
    return !g.Faulted();
}

bool WalkTurn(GuestRam& g, uint32_t R, uint32_t dirA, uint32_t dirB, int32_t dt, uint32_t sp,
              const BikeTables& t, RecoverCallees& c) {
    const uint32_t F = sp - 40u;                                          // `addiu sp,sp,-40`
    int32_t ang = 0;
    if (!AngleBetween(g, R + 528u, dirA, dirB, t, ang)) return false;     // 0x80099730
    const int32_t s2 = S(U(ang) * 25736u);
    int32_t s1 = s2 >> 8;
    const int32_t v1 = FixMul(0x40000, dt);                               // 0x80099764
    const int32_t lim = v1 < 3431 ? 3431 : v1;
    const uint32_t sg = U(s2 >> 31);
    if (S((U(s1) + sg) ^ sg) < lim) {                                     // 0x80099790
        return rc::Call(c, kReSeatFn, {g.U32(R + 596u)}, F);              // 0x800997A0
    }
    uint32_t spin;
    if (g.S32(R + 600u) > 0) {                                            // 0x800997B8
        if (s1 < 0) s1 = S(U(s1) + 0x6487Eu);
        spin = 0xFFFC0000u;
    } else {
        s1 = s1 < 0 ? S(0u - U(s1)) : S(0x6487Eu - U(s1));
        spin = 0x40000u;
    }
    g.W32(R + 488u, spin);                                                // 0x800997F4
    if (g.S32(R + 480u) == 0) {
        const int32_t r250 = g.S32(R + 592u), r258 = g.S32(R + 600u);
        const int32_t a0 = S(U(r250) - U(r258));
        int32_t v;
        if (a0 > 0) {
            v = s1 > 0 ? cu::FDiv(a0, s1) : S(0u - U(cu::FDiv(a0, S(0u - U(s1)))));
        } else {
            const int32_t b0 = S(U(r258) - U(r250));
            v = s1 > 0 ? S(0u - U(cu::FDiv(b0, s1))) : cu::FDiv(b0, S(0u - U(s1)));
        }
        g.W32(R + 592u, U(v));                                            // 0x80099858
        g.W32(R + 480u, 0x80000u);
    }
    g.W32(R + 484u, U(FixMul(g.S32(R + 592u), s1)) + g.U32(R + 600u));   // 0x8009987C
    return !g.Faulted();
}

bool WalkStep(GuestRam& g, uint32_t R, uint32_t B, uint32_t d, int32_t sq, int32_t flag, int32_t dt, uint32_t sp,
              const BikeTables& t) {
    const uint32_t F = sp - 80u;                                          // `addiu sp,sp,-80`
    int32_t s4, a0;
    if (flag == 0) {
        s4 = S(U(SqrtGte(sq, t.sqrt)) << 2);                              // 0x800999B4
        a0 = s4;
    } else {
        int32_t v[3];
        cu::Read32x3(g, d, v);
        while (Big(v[0]) || Big(v[1]) || Big(v[2])) {                     // 0x80099934
            for (int32_t& x : v) x >>= 1;
            cu::Write32x3(g, d, v);
        }
        a0 = GLength3(g, d, t);                                           // 0x800999A0
        s4 = 0x7FFF0000;
    }
    g.W32(R + 484u, U(s4));                                               // 0x800999C4
    const bool s6 = s4 < 655 || (g.S32(R + 480u) != 0 && !(32767 < s4));
    bool toTarget = false;
    if (!s6) {
        ScaleTo16(g, rc::Recip(a0), d, F + 16u);                          // 0x80099A48
        rc::GteOp(g, R + 522u, F + 16u, F + 24u);                         // 0x80099A90
        int32_t sum = 0;
        if (!rc::GNormalize(g, F + 24u, t, sum)) return false;            // 0x80099AB0
        if (sum == 0) Copy16x3(g, R + 528u, F + 24u);
        rc::GteOp(g, F + 24u, R + 522u, F + 16u);                         // 0x80099B1C
        int32_t s0 = 0;
        if (!AngleBetween(g, F + 16u, R + 528u, R + 516u, t, s0)) return false; // 0x80099B40
        const int32_t turn = Iabs(FixMul(g.S32(R + 488u), dt));           // 0x80099B50
        const int32_t lim = turn < 3431 ? 3431 : turn;
        if (S(U(Iabs(s0)) * 25736u) >> 8 < lim) {                         // 0x80099BB8
            Copy16x3(g, F + 16u, R + 528u);
            Copy16x3(g, F + 24u, R + 516u);
        } else {
            g.W32(R + 552u, g.U32(R + 552u) | (s0 < 0 ? 0x300u : 0x500u)); // 0x80099D00
            return !g.Faulted();
        }
    }
    g.W32(R + 488u, 0);                                                   // 0x80099C14
    if (s4 < 13107) {
        toTarget = true;
    } else {
        int32_t v[3];
        cu::Read32x3(g, d, v);
        toTarget = Proj3(g, v, R + 528u, F + 32u) < 0;                    // 0x80099CA0
    }
    if (toTarget) {                                                       // 0x80099CA8: arrived
        for (uint32_t k = 0; k < 3; ++k) g.W32(R + 184u + 4u * k, g.U32(R + 580u + 4u * k));
        Copy16x3(g, R + 528u, R + 450u);
        g.W32(R + 552u, g.U32(R + 552u) | 8u);
        g.W32(B + 576u, 0);
        g.W32(B + 480u, 0);
    }
    g.W32(R + 552u, (g.U32(R + 552u) & ~0x180u) | 0x100u);               // 0x80099CEC
    return !g.Faulted();
}

// ============================================================================ op 18

bool RiderRecover(GuestRam& g, uint32_t e, int32_t dt, uint32_t sp, const BikeTables& t, RecoverCallees& c) {
    (void)t;
    const uint32_t F = sp - 104u;                                         // `addiu sp,sp,-104`
    const uint32_t gs = g.U32(kRcGameStatePtr);
    const uint32_t R = g.U32(e + 852u);                                   // s2
    const bool player = g.U16(e + 172u) < g.U32(gs + 48u);                // s5 (the ORIGINAL bike)
    const bool passenger = (g.U8(R + 572u) >> 5) & 1u;                    // s6
    if (passenger) e = g.U32(g.U32(R + 596u) + 856u);                     // s1: the partner's bike
    if (g.U8(gs + 57u) == 2u) {                                           // 0x80092E74: Jailbreak phase 2
        if (player) {
            const uint32_t rd = g.U32(e + 1084u);
            if (g.U32(rd + 40u) != 0u || !(g.U8(rd + 39u) < 248u)) return true;
            return rc::Call(c, kRcJailbreakFn, {1}, F);                   // 0x80092EB0
        }
        const uint32_t slot = e + 948u + 8u * U(static_cast<int32_t>(g.S8(e + 946u)));
        const uint32_t timer = g.U16(slot + 4u);
        if (timer == 0u) {
            const uint32_t p1 = g.U32(kRcPlayer1Bike);                    // 0x80092F18
            const uint32_t rd = g.U32(p1 + 1084u);
            if (g.U32(rd + 40u) != 0u || !(g.U8(rd + 39u) < 248u)) {
                const uint32_t pr = g.U32(p1 + 852u);
                const int32_t dx = S(g.U32(R + 184u) - g.U32(pr + 184u));
                const int32_t dz = S(g.U32(R + 192u) - g.U32(pr + 192u));
                if (Octagonal(dx, dz) < 25) g.W32(0x8005B228u, 0x20000u); // 0x80092FD8
            }
        } else {
            const int32_t left = S(timer - U(S(U(dt) * 300u) >> 16));     // 0x80092EFC
            if (left > 0) {
                g.W16(slot + 4u, static_cast<uint16_t>(left));
                return true;
            }
            g.W16(slot + 4u, 0);
        }
    }
    if (g.S16(R + 320u) == 0) return true;                                // 0x80092FDC
    bool s0 = false;                                                      // put the bike back on the road
    if ((g.U32(R + 552u) & 0x1000u) || (g.U32(e + 856u) != 0u && g.U32(e + 1088u) != 0u && !passenger) ||
        (player && g.U32(0x8005B220u) != 0u))
        s0 = true;
    if (!s0 && player && g.S16(e + 320u) != 0 && (g.U32(e + 388u) & 1u) && (g.U32(e + 568u) & 0x100u)) {
        const uint32_t sl = g.U32(e + 340u);                              // 0x8009308C: off the road
        int32_t d[3];
        for (uint32_t k = 0; k < 3; ++k) d[k] = S(g.U32(e + 184u + 4u * k) - g.U32(sl + 20u + 4u * k));
        cu::Write32x3(g, F + 40u, d);
        const int32_t lat = Proj3(g, d, g.U32(e + 340u) + 8u, F + 56u);
        s0 = 0x10000 < Iabs(lat);
    }
    const int32_t dist = ApproxLen3(S(g.U32(R + 184u) - g.U32(e + 184u)) >> 16,   // 0x800931A4 (s3)
                                    S(g.U32(R + 188u) - g.U32(e + 188u)) >> 16,
                                    S(g.U32(R + 192u) - g.U32(e + 192u)) >> 16);
    const bool far = player && !(dist < 201);                             // s4
    const bool bVar5 = s0 || (far && (g.U32(e + 560u) & 0x08000000u));
    const uint32_t st = g.U16(R + 544u);
    if (g.U16(kRcStanceTable + 8u * st + 2u) != 8u) {                     // 0x80093204
        if (!((st - 69u) < 2u)) return true;
        uint32_t done = 0;
        if (!rc::Call(c, 0x8005BE58u, {g.U32(R + 540u)}, F, &done)) return false;   // ClipDone
        if (done == 0u || !bVar5) return true;
    }
    g.W32(R + 552u, g.U32(R + 552u) & ~0x1000u);                          // 0x80093244
    if (bVar5) {
        // ---- the bike put back on the rider's road slice, re-seated (0x80093248..0x80093848)
        g.W16(e + 320u, 1);
        g.W32(R + 348u, 0);
        g.W32(R + 344u, 0);
        g.W32(e + 348u, 0);
        g.W32(e + 344u, 0);
        for (uint32_t k = 0; k < 3; ++k) g.W32(e + 504u + 4u * k, g.U32(g.U32(R + 340u) + 20u + 4u * k));
        g.W32(e + 188u, g.U32(e + 508u));
        g.W32(e + 184u, g.U32(e + 504u));
        g.W32(e + 192u, g.U32(e + 512u));
        for (uint32_t k = 0; k < 3; ++k) g.W16(e + 528u + 2u * k, g.U16(g.U32(R + 340u) + 14u + 2u * k));
        g.W16(e + 522u, g.U16(g.U32(R + 340u) + 8u));
        g.W16(e + 524u, g.U16(g.U32(R + 340u) + 10u));
        {
            const uint16_t w = g.U16(g.U32(R + 340u) + 12u);
            g.W16(e + 522u, static_cast<uint16_t>(0u - g.U16(e + 522u)));
            g.W16(e + 526u, w);
            g.W16(e + 524u, static_cast<uint16_t>(0u - g.U16(e + 524u)));
            g.W16(e + 526u, static_cast<uint16_t>(0u - w));
        }
        for (uint32_t k = 0; k < 3; ++k) g.W16(e + 516u + 2u * k, g.U16(g.U32(R + 340u) + 2u + 2u * k));
        GuestCopyWords(g, e + 328u, R + 328u, 32);                        // 0x8009337C
        g.W32(e + 428u, g.U32(R + 428u));
        if (!rc::Call(c, kRcRouteBindFn, {e + 172u, 1, 0}, F)) return false;   // 0x80093394
        if (!rc::Call(c, kReFaceFn, {e}, F)) return false;                // 0x8009339C
        const int16_t t3 = g.S16(e + 434u);
        const uint32_t sgn = U(t3) >> 31;                                 // s3
        {
            const uint16_t r210[3] = {g.U16(e + 528u), g.U16(e + 530u), g.U16(e + 532u)};
            const uint16_t r20A[3] = {g.U16(e + 522u), g.U16(e + 524u), g.U16(e + 526u)};
            const uint16_t r204[3] = {g.U16(e + 516u), g.U16(e + 518u), g.U16(e + 520u)};
            for (uint32_t k = 0; k < 3; ++k) {
                g.W16(e + 444u + 2u * k, r210[k]);
                g.W16(e + 432u + 2u * k, r20A[k]);
                g.W16(e + 438u + 2u * k, r204[k]);
            }
            if (sgn) {
                for (uint32_t k = 0; k < 3; ++k) g.W16(e + 432u + 2u * k, static_cast<uint16_t>(0u - r20A[k]));
            } else {
                Neg16x3(g, e + 438u);
            }
        }
        Copy16x3(g, e + 528u, e + 450u);
        if (!rc::Call(c, kRcRoadPositionFn, {e + 450u, e + 328u, e + 360u}, F)) return false;   // 0x80093464
        const uint32_t keep100 = (g.U32(e + 568u) >> 8) & 1u;
        g.W32(e + 464u, 0);
        g.W32(e + 460u, 0);
        g.W32(e + 456u, 0);
        Copy16x3(g, e + 516u, e + 814u);
        if (!rc::Call(c, kRcResetBikeFn, {e}, F)) return false;           // 0x8009349C
        g.W32(e + 568u, g.U32(e + 568u) | (keep100 ? 0x100u : 0x900u));
        g.W32(e + 652u, ((0u - sgn) & 0xFFFCDBC2u) + 0x1921Fu);           // 0x800934E4
        g.W32(e + 492u, g.U32(R + 492u));
        g.W32(e + 496u, g.U32(R + 496u));
        GuestCopyWords(g, e + 372u, R + 372u, 56);                        // 0x800934FC
        g.W32(e + 324u, g.U32(R + 324u));
        g.W32(e + 344u, 0);
        g.W32(e + 348u, 0);
        g.W16(e + 392u, 4);
        g.W16(e + 394u, 1);
        g.W32(e + 388u, g.U32(e + 388u) & ~1u);
        if (!rc::Call(c, kRcZonesFn, {e}, F)) return false;               // 0x80093534
        const uint32_t P = g.U32(e + 856u);
        if (P != 0u && g.U32(e + 1088u) != 0u) {                          // the partner rides along
            GuestCopyWords(g, P + 328u, e + 328u, 32);
            g.W32(P + 428u, g.U32(e + 428u));
            if (!rc::Call(c, kRcRouteBindFn, {P + 172u, 1, 0}, F)) return false;   // 0x80093578
            g.W32(P + 360u, g.U32(e + 360u));
            g.W32(P + 368u, g.U32(e + 368u));
            g.W32(P + 364u, g.U32(e + 364u));
            g.W32(P + 492u, g.U32(e + 492u));
            g.W32(P + 496u, g.U32(e + 496u));
            GuestCopyWords(g, P + 372u, e + 372u, 56);
            const uint32_t prog = g.U32(R + 324u);
            const uint32_t f184 = g.U32(P + 388u);
            const int32_t gap = S(g.U32(e + 304u) + g.U32(P + 304u));
            g.W32(P + 344u, 0);
            g.W32(P + 348u, 0);
            g.W16(P + 392u, 4);
            g.W16(P + 394u, 1);
            g.W16(P + 320u, 1);
            g.W32(P + 324u, prog);
            g.W32(P + 388u, f184 & ~1u);
            GMulAdd(g, e + 184u, e + 432u, gap, P + 184u);                // 0x800935FC
        }
        const uint32_t h = g.U16(e + 172u);
        if (h < g.U32(gs + 48u)) {                                        // 0x8009361C: a player
            const uint32_t rd = g.U32(e + 1084u);
            if (g.U8(rd + 37u) == 0u || g.U8(rd + 14u) == 0u) {
                // ---- the player's race is over: the rider stands on the road beside the bike
                if (!rc::Call(c, kRcEndRaceFn, {e, 8}, F)) return false; // 0x80093650
                g.W32(e + 480u, 0);
                const uint32_t sl = g.U32(R + 340u);
                g.W32(R + 480u, 0);
                g.W32(R + 488u, 0);
                g.W32(R + 348u, 0);
                const int32_t off = S(g.U32(R + 304u) + (g.U32(e + 304u) << 1));
                g.W32(R + 344u, U(off));
                GMulAdd(g, sl + 20u, sl + 2u, off, R + 184u);             // 0x80093688
                const uint32_t s1 = g.U32(R + 340u);
                g.W32(R + 508u, 0);
                g.W16(R + 522u, g.U16(s1 + 8u));
                g.W16(R + 524u, g.U16(s1 + 10u));
                const uint16_t a0 = g.U16(s1 + 12u);
                g.W16(R + 522u, static_cast<uint16_t>(0u - g.U16(R + 522u)));
                g.W16(R + 526u, a0);
                g.W16(R + 524u, static_cast<uint16_t>(0u - g.U16(R + 524u)));
                g.W16(R + 526u, static_cast<uint16_t>(0u - a0));
                for (uint32_t k = 0; k < 3; ++k) g.W16(R + 516u + 2u * k, g.U16(s1 + 14u + 2u * k));
                for (uint32_t k = 0; k < 3; ++k) g.W16(R + 528u + 2u * k, g.U16(s1 + 2u + 2u * k));
                const uint16_t n0 = static_cast<uint16_t>(0u - g.U16(R + 528u));
                const uint16_t n2 = static_cast<uint16_t>(0u - g.U16(R + 532u));
                g.W16(R + 532u, n2);
                g.W16(R + 528u, n0);
                const uint16_t n1 = static_cast<uint16_t>(0u - g.U16(R + 530u));
                g.W16(R + 530u, n1);
                g.W32(R + 464u, 0);
                g.W32(R + 460u, 0);
                g.W32(R + 456u, 0);
                g.W16(R + 434u, g.U16(R + 524u));
                g.W16(R + 436u, g.U16(R + 526u));
                g.W16(R + 438u, g.U16(R + 516u));
                g.W16(R + 440u, g.U16(R + 518u));
                g.W16(R + 442u, g.U16(R + 520u));
                g.W16(R + 444u, n0);
                g.W16(R + 446u, n1);
                g.W16(R + 448u, n2);
                g.W16(R + 450u, n0);
                g.W16(R + 452u, n1);
                g.W16(R + 454u, n2);
                g.W16(R + 432u, g.U16(R + 522u));
                if (!rc::Call(c, kRcRouteBindFn, {R + 172u, 1, 0}, F)) return false;   // 0x800937B8
                if (!rc::Call(c, kRcRoadPositionFn, {R + 450u, R + 328u, R + 360u}, F)) return false;
                g.W16(R + 392u, 4);
                g.W16(R + 394u, 1);
                g.W32(R + 388u, g.U32(R + 388u) & ~1u);
                return rc::Call(c, kRcZonesFn, {R}, F);                   // 0x800937F0
            }
            const uint32_t v = View(g, h);                                // 0x80093800
            g.W32(v + 548u, (g.U32(v + 548u) | 6u) & 0xFF807FFFu);
            if (!rc::Call(c, kRcSpringResetFn, {v}, F)) return false;     // 0x80093838
        }
        return rc::Call(c, kReSeatFn, {e}, F);                            // 0x80093840
    }
    // ---- the walk (0x80093850..)
    if (!(g.U8(g.U32(e + 1084u) + 39u) < 247u)) {                         // 0x80093864: finished
        g.W32(R + 552u, g.U32(R + 552u) & 0xFFEFFFE7u);
        return true;
    }
    g.W32(R + 484u, 0x7FFF0000u);                                         // 0x80093870
    if (far || ((g.U32(e + 560u) & 0x02000000u) && dist < 3)) {
        g.W32(R + 552u, g.U32(R + 552u) | 0x01000800u);                   // 0x80093890
    } else {
        bool walk = true;
        if ((g.U32(R + 552u) >> 20) & 1u) {                               // 0x800938B8: a pause
            const int32_t left = S(g.U32(R + 600u) - U(dt));
            g.W32(R + 600u, U(left));
            if (left > 0) {
                g.W32(R + 552u, g.U32(R + 552u) | 0x100u);
                walk = false;
            } else {
                g.W32(R + 552u, g.U32(R + 552u) & 0xFFEFFFFFu);
            }
        }
        if (walk) {
            uint32_t go = 0;
            if (!rc::Call(c, kWalkTargetFn, {R, e, F + 24u, F + 32u}, F, &go)) return false;   // 0x8009390C
            if (go != 0u) {
                int32_t d[3];
                for (uint32_t k = 0; k < 3; ++k) d[k] = S(g.U32(R + 580u + 4u * k) - g.U32(R + 184u + 4u * k));
                cu::Write32x3(g, F + 40u, d);
                int32_t s3 = 0, sq;
                if (Big(d[0]) || Big(d[1]) || Big(d[2])) {
                    s3 = 1;
                    sq = 0x7FFF0000;
                } else {
                    sq = GSumSquares(g, F + 40u);                         // 0x800939A8
                }
                if (passenger && !(0x3FFFF < sq)) {                       // 0x800939B4
                    if (g.S32(e + 480u) == 0)
                        if (!rc::Call(c, kRcRemountFn, {e, 0}, F)) return false;   // 0x800939DC
                } else {
                    const uint32_t f = g.U32(R + 552u);
                    const bool moving = (f & 0x18u) != 0u;                // a0
                    if (!((f & 0x800u) && !moving && 0x3FFFF < sq)) {     // 0x800939EC
                        uint32_t v1 = f & 0xFFFFF07Fu;
                        g.W32(R + 552u, v1);
                        bool turning = moving;
                        if (turning && (g.S32(e + 480u) != 0 || g.S32(e + 488u) != 0)) {
                            g.W32(R + 552u, v1 & ~0x18u);                 // 0x80093A50
                            turning = false;
                        }
                        if (turning) {
                            if (g.U32(R + 552u) & 8u) {
                                if (player) {
                                    const uint32_t rd = g.U32(e + 1084u);
                                    if (g.U8(rd + 37u) == 0u || g.U8(rd + 14u) == 0u)
                                        if (!rc::Call(c, kRcEndRaceFn, {e, 8}, F)) return false;   // 0x80093AA8
                                }
                                if (!rc::Call(c, kWalkPivotFn, {R, F + 24u, F + 32u, U(sq), U(s3)}, F)) return false;
                            }
                            if (g.U32(R + 552u) & 0x10u)
                                if (!rc::Call(c, kWalkTurnFn, {R, F + 24u, F + 32u, U(dt)}, F)) return false;
                        } else {
                            if (!rc::Call(c, kWalkStepFn, {R, e, F + 40u, U(sq), U(s3), U(dt)}, F)) return false;
                        }
                    }
                }
            }
        }
    }
    // 0x80093B10
    const uint32_t f = g.U32(R + 552u);
    if (f & 0x800u) {
        g.W32(R + 552u, f & 0xFFEFFFE7u);
        if (rc::IsPlayerRider(g, g.U16(R + 172u))) {
            const uint32_t v = View(g, g.U16(g.U32(R + 596u) + 172u));
            g.W32(v + 548u, g.U32(v + 548u) & ~0x80u);
        }
    }
    return !g.Faulted();
}

// ============================================================================ the re-seat

bool ReSeat(GuestRam& g, uint32_t B, uint32_t sp, RecoverCallees& c) {
    const uint32_t F = sp - 40u;                                          // `addiu sp,sp,-40`
    const uint32_t gs = g.U32(kRcGameStatePtr);
    if (!(g.U8(gs + 57u) < 2u)) {
        const uint32_t R = g.U32(B + 852u);
        if (g.S8(R + 571u) != -1)
            if (!rc::Call(c, kRcReleaseObjFn, {R}, F)) return false;     // 0x800927CC
    }
    if (g.U32(0x8005B254u) != 0u &&
        (g.U16(B + 172u) < g.U32(g.U32(kRcGameStatePtr) + 48u) || (g.U16(B + 320u) & 4u)) &&
        g.U32(kRcAnimDesc + 8u) == g.U32(kRcAnimDesc + 12u)) {
        if (!rc::Call(c, kFreeFarRiderFn, {}, F)) return false;           // 0x80092830
    }
    if (g.U32(B + 540u) == 0u) {
        uint32_t obj = 0;
        if (!rc::Call(c, kRcViewSlotFn, {kRcAnimDesc, B}, F, &obj)) return false;   // 0x80092850
        g.W32(B + 540u, obj);
        if (obj == 0u) return rc::Call(c, kRcRemountFn, {B, 0}, F);       // 0x80092864: no slot
    }
    const uint32_t R = g.U32(B + 852u);                                   // s1
    const uint32_t t0 = B + 948u + 8u * U(static_cast<int32_t>(g.S8(B + 946u)));
    g.W16(t0 + 2u, 224);                                                  // {1, 224, -, stamp}
    g.W16(t0, 1);
    g.W16(t0 + 6u, 0);
    g.W16(t0 + 6u, static_cast<uint16_t>(((U(g.S32(g.U32(kRcGameStatePtr) + 16u)) * 2180u + 0x8000u) >> 16) | 0xC000u));
    g.W32(R + 552u, (g.U32(R + 552u) | 0x40u) & 0xBFE67FC7u);            // 0x800928FC
    g.W32(B + 720u, 0);
    if (!rc::Call(c, kRcAttachFn, {B, R, 2, 0}, F)) return false;         // 0x80092900
    g.W8(B + 72u, 3);
    if (g.U32(B + 856u) != 0u && g.U32(B + 1088u) != 0u) {                // a two-rider bike
        if (!rc::Call(c, kRcStanceEventFn, {4, R, 16}, F)) return false;  // 0x8009292C
        if ((g.U8(g.U32(B + 852u) + 572u) & 16u) && g.U8(g.U32(kRcGameStatePtr) + 57u) != 1u) {
            const uint32_t P = g.U32(g.U32(B + 856u) + 852u);
            g.W16(P + 320u, g.U16(B + 320u));
            if (!rc::Call(c, kRcAttachFn, {B, P, 2, 1}, F)) return false;   // 0x80092980
            g.W8(B + 72u, 3);
            g.W32(P + 552u, (g.U32(P + 552u) | 0x40u) & 0xBFE67FE7u);
            if (!rc::Call(c, kRcStanceEventFn, {77, P, 16}, F)) return false;   // 0x800929AC
        }
        g.W32(g.U32(B + 856u) + 720u, 0);
        if (!rc::Call(c, kClimbDoneFn, {B}, F)) return false;             // 0x800929BC
    } else {
        if (!rc::Call(c, kRcBankSwitchFn, {g.U32(B + 540u), g.U32(0x800CE1A0u)}, F)) return false;   // 0x800929D8
        uint32_t p = 16;
        if (g.S16(B + 320u) != 0 && g.S32(B + 652u) < 0) p |= 0x100u;
        if (Iabs(g.S32(B + 652u)) != 0x1921F) p |= 0x8000u;
        if (!rc::Call(c, kRcStanceEventFn, {0, R, p}, F)) return false;   // 0x80092A28
    }
    rc::CopyHalfwords(g, 9, B + 516u, B + 432u);                          // 0x80092A38
    const uint32_t h = g.U16(B + 172u);
    if (h < g.U32(g.U32(kRcGameStatePtr) + 48u)) {
        if (!rc::Call(c, kRcVoiceFn, {h, 0}, F)) return false;            // 0x80092A60
        const uint32_t v = View(g, g.U16(B + 172u));
        g.W32(v + 548u, g.U32(v + 548u) & 0xF5FFFF7Fu);
        if (!rc::Call(c, kRcCamTargetFn, {g.U16(B + 172u), B}, F)) return false;   // 0x80092AAC
    }
    return !g.Faulted();
}

bool ClimbDone(GuestRam& g, uint32_t B, uint32_t sp, const BikeTables& t, RecoverCallees& c) {
    (void)t;
    const uint32_t F = sp - 32u;                                          // `addiu sp,sp,-32`
    const uint32_t R = g.U32(B + 852u);
    const uint32_t st = g.U16(R + 544u);
    if (g.U16(kRcStanceTable + 8u * st + 2u) != 0u) return true;          // the climb still plays
    if (st == 0u) return true;
    g.W8(B + 72u, 0);
    if (!rc::Call(c, kRcPopCommandFn, {B}, F)) return false;              // 0x80092B1C
    g.W32(R + 552u, g.U32(R + 552u) & 0xFFFE7FBFu);
    if (!rc::Call(c, kRcResetBikeFn, {B}, F)) return false;               // 0x80092B38
    g.W32(B + 568u, g.U32(B + 568u) | 0x08000000u);
    if (!(g.U16(B + 172u) < g.U32(g.U32(kRcGameStatePtr) + 48u))) {       // 0x80092B68: not a player
        const int32_t dx = Iabs(S(g.U32(B + 184u) - g.U32(0x800CD950u)));
        const int32_t dz = Iabs(S(g.U32(B + 192u) - g.U32(0x800CD958u)));
        const int32_t mn = dx < dz ? dx : dz;
        const int32_t v = S(U(S(U(dx) + U(dz))) - U(cu::Half(mn)));
        if (0x780000 < v) {                                               // far from view 0's eye: re-face
            if (!rc::Call(c, kReFaceFn, {B}, F)) return false;            // 0x80092BDC
            rc::CopyHalfwords(g, 9, B + 516u, B + 432u);
            const uint16_t w[6] = {g.U16(B + 528u), g.U16(B + 530u), g.U16(B + 532u),
                                   g.U16(B + 516u), g.U16(B + 518u), g.U16(B + 520u)};
            g.W16(B + 450u, w[0]);
            g.W16(B + 452u, w[1]);
            g.W16(B + 454u, w[2]);
            g.W16(B + 814u, w[3]);
            g.W16(B + 816u, w[4]);
            g.W16(B + 818u, w[5]);
        }
    }
    GMulAdd(g, g.U32(B + 340u) + 20u, B + 444u, 0x1E0000, B + 880u);      // 0x80092C34: the aim, 30 ahead
    const uint32_t a = g.U32(B + 540u);
    if (a != 0u) {
        g.W32(a + 36u, 0);                                                // the object freed
        g.W32(kRcAnimDesc + 8u, g.U32(kRcAnimDesc + 8u) - 1u);
    }
    g.W32(B + 540u, 0);
    return !g.Faulted();
}

bool ReFace(GuestRam& g, uint32_t B, uint32_t sp, const BikeTables& t, RecoverCallees& c) {
    const uint32_t F = sp - 56u;                                          // `addiu sp,sp,-56`
    bool flip = false;                                                    // s4
    if (RouteBindingValidAt(g, B + 172u) == 0) {                          // 0x80096588
        flip = (g.U32(B + 568u) & 0x00400000u) != 0u;
    } else {
        const uint32_t R = g.U32(B + 852u);
        const uint32_t s0 = g.U32(R + 604u) < 3u ? B : R;
        const uint32_t key = g.U32(s0 + 360u);
        if ((key >> 16) == 0u) {                                          // on a road
            const int32_t dot = Dot3(g, g.U32(s0 + 340u) + 14u, B + 528u);
            const uint32_t leg = RouteFindLegView(g, g.U32(s0 + 428u), g.U16(s0 + 360u));
            if (leg != 0u && g.S32(leg + 4u) < 0 && dot > 0) flip = true;
        } else {                                                          // in a junction
            const uint32_t rr = RouteLegOfRoad(g, S(key & 0xFFFFu));      // 0x80096610
            const uint32_t node = RoadNodeRecord(g, g.S32(g.U32(s0 + 328u)));
            if (rr != 0u && node != 0u) {
                const uint32_t arm = RoadNodeArm(g, node, g.S32(rr + 84u));   // 0x80096640
                if (arm != 0u) {
                    int32_t d[3];
                    for (uint32_t k = 0; k < 3; ++k) d[k] = S(g.U32(arm + 12u + 4u * k) - g.U32(s0 + 184u + 4u * k));
                    cu::Write32x3(g, F + 16u, d);
                    Normalize32(d, t.rsqrt);                              // 0x8009668C
                    cu::Write32x3(g, F + 16u, d);
                    for (uint32_t k = 0; k < 3; ++k) g.W16(B + 528u + 2u * k, static_cast<uint16_t>(g.S32(F + 16u + 4u * k) >> 4));
                    rc::GteOp(g, B + 522u, B + 528u, B + 516u);           // 0x80096700
                    int32_t sum = 0;
                    if (!rc::GNormalize(g, B + 516u, t, sum)) return false;   // 0x80096720
                    rc::GteOp(g, B + 516u, B + 522u, B + 528u);           // 0x80096760
                }
            }
        }
    }
    if (flip) {                                                           // 0x800967A4
        Neg16x3(g, B + 528u);
        Neg16x3(g, B + 516u);
    }
    return rc::Call(c, kRcRoadPositionFn, {B + 528u, B + 328u, B + 360u}, F) && !g.Faulted();   // 0x800967F0
}

bool FreeFarRider(GuestRam& g, uint32_t sp, RecoverCallees& c, uint32_t& v0) {
    const uint32_t F = sp - 24u;                                          // `addiu sp,sp,-24`
    const uint32_t pool = 0x800CE4F0u;                                    // pool 2's control record
    int32_t n = g.S32(g.U32(pool + 12u));
    uint32_t a0 = g.U32(pool);
    uint32_t best = 0, other = 0;
    int32_t far = 0;
    v0 = 0;
    if (n >= 0) {
        const uint32_t p1 = g.U32(kRcPlayer1Bike);
        do {
            if (g.U16(a0 + 172u) != 0u) {
                const int32_t d = Iabs(S(g.U32(p1 + 324u) - g.U32(a0 + 324u)));
                if (far < d) {
                    far = d;
                    best = a0;
                }
                if (!(g.U8(a0 + 9u) & 3u) && other != 0u) other = a0;     // (never taken: other starts 0)
            }
            a0 += g.U32(pool + 4u);
            --n;
        } while (n >= 0);
    }
    if (best == 0u && other == 0u) return !g.Faulted();
    const uint32_t e = best != 0u ? best : other;
    const uint32_t s0 = g.U32(e + 540u);
    if (!rc::Call(c, kRcPoolReleaseFn, {e + 172u, 2}, F)) return false;   // 0x8008CD6C
    v0 = s0;
    return !g.Faulted();
}

} // namespace rr::sim
