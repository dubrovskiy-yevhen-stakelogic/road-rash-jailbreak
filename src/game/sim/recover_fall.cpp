#include "game/sim/recover_fall.h"

#include "game/sim/ai.h"
#include "game/sim/coll_util.h"
#include "game/sim/crash.h"
#include "game/sim/fixed.h"
#include "game/sim/vec.h"

namespace rr::sim {

namespace rcfall {
using cu::Add;
using cu::Half;
using cu::MipsDiv;
using cu::Neg;
using cu::S;
using cu::Shl;
using cu::Sub;
using cu::U;

// A row element `lh(a) << 4`, the form every pose reads its rows in.
inline int32_t Row16(GuestRam& g, uint32_t a) { return Shl(g.S16(a), 4); }

// `sh (0 - lhu(a))`.
inline void NegH(GuestRam& g, uint32_t a) { g.W16(a, static_cast<uint16_t>(0u - g.U16(a))); }

// The clip phase every initialiser opens with, A = R+0x21C:
//   t = (lw(A+16) << 16) + div(lw(A+32) << 16, lw(A+24))
//   n = (s16)(lhu(clip + 16) - 1), clip = *(lw(lw(A+40)+4) + 4 * lbu(lw(A+4) + 12 * lw(A+12)))
void ClipPhase(GuestRam& g, uint32_t R, int32_t& t, int32_t& n) {
    const uint32_t A = g.U32(R + 540u);
    const int32_t q = MipsDiv(S(g.U32(A + 32u) << 16), g.S32(A + 24u));
    const uint32_t slot = g.U32(A + 12u);
    const uint32_t idx = g.U8(g.U32(A + 4u) + slot * 12u);
    const uint32_t clip = g.U32(g.U32(g.U32(A + 40u) + 4u) + idx * 4u);
    n = static_cast<int16_t>(static_cast<uint16_t>(g.U16(clip + 16u) - 1u));
    t = S((g.U32(A + 16u) << 16) + U(q));
}

// R.B8+4i := base[i] + FixMul(a, row1[i]) + FixMul(b, row2[i]) + FixMul(c, row3[i]), component by
// component (each base word read after the previous component was stored).
void Place(GuestRam& g, uint32_t R, uint32_t base, uint32_t r1, uint32_t r2, uint32_t r3, int32_t a, int32_t b,
           int32_t c) {
    for (uint32_t i = 0; i < 3; ++i) {
        const int32_t m1 = FixMul(a, Row16(g, r1 + 2u * i));
        const int32_t m2 = FixMul(b, Row16(g, r2 + 2u * i));
        const int32_t m3 = FixMul(c, Row16(g, r3 + 2u * i));
        g.W32(R + 184u + 4u * i, g.U32(base + 4u * i) + U(m1) + U(m2) + U(m3));
    }
}

// R+0x1E0 := B+0x1E0 > 0x4FFFF ? B+0x1E0 : 0x50000 (slt).
void SpeedFloor(GuestRam& g, uint32_t R, uint32_t B) {
    const int32_t v = g.S32(B + 480u);
    g.W32(R + 480u, v > 0x4FFFF ? U(v) : 0x50000u);
}

// *out := R+0x138; R+0x138 /= 2 (reloaded after the store).
void SaveHalfHeight(GuestRam& g, uint32_t R, uint32_t out) {
    g.W32(out, g.U32(R + 312u));
    g.W32(R + 312u, U(Half(g.S32(R + 312u))));
}

// The rider's bike: +0x254, or its +0x358 bike for a passenger (+0x23C bit 5).
uint32_t PoseBike(GuestRam& g, uint32_t R, bool passenger) {
    return passenger ? g.U32(g.U32(R + 596u) + 856u) : g.U32(R + 596u);
}

// SLUS 0x8002E570 MulAdd32(base, dir, t, out) over guest words.
void GMulAdd32(GuestRam& g, uint32_t base, uint32_t dir, int32_t t, uint32_t out) {
    int32_t b[3], d[3], o[3];
    cu::Read32x3(g, base, b);
    cu::Read32x3(g, dir, d);
    MulAdd32(b, d, t, o);
    cu::Write32x3(g, out, o);
}
} // namespace rcfall

// ============================================================================ 0x8008EE60 PoseA
bool PoseA(GuestRam& g, uint32_t R, uint32_t /*out*/, uint32_t /*sp*/, const BikeTables& /*t*/) {
    using namespace rcfall;
    int32_t t = 0, n = 0;
    ClipPhase(g, R, t, n);
    const uint32_t B = g.U32(R + 596u);
    const int32_t c = MipsDiv(FixMul(S(0xFFFEE7FDu), t), n);                     // 0x8008EEF8
    Place(g, R, B + 184u, B + 432u, B + 438u, B + 444u, 0x8000, S(0xFFFF13FFu), c); // 0x8008EF50 ..
    rc::CopyHalfwords(g, 9, B + 432u, R + 432u);                                  // 0x8008F000
    for (uint32_t k = 0; k < 3; ++k) g.W16(R + 450u + 2u * k, g.U16(B + 450u + 2u * k)); // 0x8008F010 ..
    SpeedFloor(g, R, B);                                                          // 0x8008F044
    return !g.Faulted();
}

// ============================================================================ 0x8008E818 PoseB
bool PoseB(GuestRam& g, uint32_t R, uint32_t out, uint32_t sp, const BikeTables& /*t*/) {
    using namespace rcfall;
    const uint32_t F = sp - 56u;
    g.W32(F + 60u, out); // 0x8008E848: a1 into the caller's argument slot
    int32_t t = 0, n = 0;
    ClipPhase(g, R, t, n);
    const bool pass = (g.U8(R + 572u) & 0x20u) != 0;
    int32_t s4, s6, s7, s0, s1;
    if (pass) {
        s4 = 23429;
        s6 = S(0xFFFE9CFBu);
        s7 = S(0xFFFF373Fu);
        s0 = 0x8EB8;
        s1 = S(0xFFFF0000u);
    } else {
        s4 = -11396;
        s6 = S(0xFFFEEA3Eu);
        s7 = -18435;
        s0 = 0;
        s1 = S(0xFFFF4000u);
    }
    const uint32_t B = PoseBike(g, R, pass);
    int32_t a = Add(s0, MipsDiv(FixMul(Sub(s4, s0), t), n)); // 0x8008E930
    const int32_t b = Add(s1, MipsDiv(FixMul(Sub(s6, s1), t), n));
    if ((g.U32(R + 552u) & 0x08000000u) != 0) a = Neg(a);
    Place(g, R, B + 184u, B + 432u, B + 438u, B + 444u, a, b, s7); // 0x8008E9A8 ..
    // Rows 1B6 and 1BC swapped (0x8008EA54 ..).
    for (uint32_t k = 0; k < 3; ++k) g.W16(R + 444u + 2u * k, g.U16(B + 438u + 2u * k));
    for (uint32_t k = 0; k < 3; ++k) g.W16(R + 432u + 2u * k, g.U16(B + 432u + 2u * k));
    for (uint32_t k = 0; k < 3; ++k) g.W16(R + 438u + 2u * k, g.U16(B + 444u + 2u * k));
    // Heading = -row 1B6 (0x8008EAE4 ..).
    for (uint32_t k = 0; k < 3; ++k)
        g.W16(R + 450u + 2u * k, static_cast<uint16_t>(0u - g.U16(R + 438u + 2u * k)));
    const uint32_t o = g.U32(F + 60u);
    if (o != 0) SaveHalfHeight(g, R, o); // 0x8008EB18
    SpeedFloor(g, R, B);                 // 0x8008EB54
    return !g.Faulted();
}

// ============================================================================ 0x8008E50C PoseC
bool PoseC(GuestRam& g, uint32_t R, uint32_t /*out*/, uint32_t /*sp*/, const BikeTables& /*t*/) {
    using namespace rcfall;
    int32_t t = 0, n = 0;
    ClipPhase(g, R, t, n);
    const bool pass = (g.U8(R + 572u) & 0x20u) != 0;
    int32_t s8, s5, s7;
    const int32_t s0 = S(0xFFFF599Au);
    if (pass) {
        s8 = 0x9041;
        s5 = S(0xFFFDF08Au);
        s7 = -21725;
    } else {
        s8 = -65;
        s5 = S(0xFFFE23CAu);
        s7 = 4384;
    }
    const uint32_t B = PoseBike(g, R, pass);
    uint32_t rows = B + 516u, base;
    if ((g.U32(R + 552u) & 0x10000u) != 0) { // the launch: from the bike, on its +0x204 rows
        s7 = 0;
        s5 = S(0xFFFD6667u);
        base = (g.U32(B + 568u) & 0x600u) != 0 ? B + 184u : B + 504u;
        rc::CopyHalfwords(g, 9, B + 432u, R + 432u); // 0x8008E640
    } else {                                         // from B+0x1F8 + R+0x244.., on R's own rows
        g.W32(R + 184u, g.U32(B + 504u) + g.U32(R + 580u)); // 0x8008E660
        g.W32(R + 188u, g.U32(B + 508u) + g.U32(R + 584u)); // 0x8008E674
        g.W32(R + 192u, g.U32(B + 512u) + g.U32(R + 588u)); // 0x8008E68C
        rows = R + 432u;
        base = R + 184u;
        s5 = Add(s0, MipsDiv(FixMul(Sub(s5, s0), t), n)); // 0x8008E69C
    }
    Place(g, R, base, rows, rows + 6u, rows + 12u, s8, s5, s7); // 0x8008E6EC ..
    for (uint32_t k = 0; k < 3; ++k)                             // heading = -R.row1B6 (0x8008E7C0 ..)
        g.W16(R + 450u + 2u * k, static_cast<uint16_t>(0u - g.U16(R + 438u + 2u * k)));
    SpeedFloor(g, R, B); // 0x8008E7E4
    return !g.Faulted();
}

// ============================================================================ 0x8008EB88 PoseD
bool PoseD(GuestRam& g, uint32_t R, uint32_t out, uint32_t /*sp*/, const BikeTables& /*t*/) {
    using namespace rcfall;
    int32_t t = 0, n = 0;
    ClipPhase(g, R, t, n);
    const bool pass = (g.U8(R + 572u) & 0x20u) != 0;
    int32_t s6, s4, s5, s0;
    if (pass) {
        s6 = 0x8F83;
        s4 = S(0xFFFEB2BEu);
        s5 = S(0xFFFE577Au);
        s0 = S(0xFFFEE5F1u);
    } else {
        s6 = 1218;
        s4 = S(0xFFFF013Bu);
        s5 = S(0xFFFEF77Au);
        s0 = S(0xFFFF346Eu);
    }
    const uint32_t B = PoseBike(g, R, pass);
    const int32_t c = MipsDiv(FixMul(s5, t), n);                     // 0x8008EC8C
    const int32_t b = Add(s0, MipsDiv(FixMul(Sub(s4, s0), t), n));   // 0x8008ECB8
    Place(g, R, B + 184u, B + 432u, B + 438u, B + 444u, s6, b, c);   // 0x8008ECF8 ..
    rc::CopyHalfwords(g, 9, B + 432u, R + 432u);                     // 0x8008EDA0
    for (uint32_t k = 0; k < 3; ++k) g.W16(R + 450u + 2u * k, g.U16(R + 444u + 2u * k)); // heading := row1BC
    const int32_t v1 = g.S32(B + 480u);
    int32_t speed;
    if (0x50000 < v1) {
        speed = Sub(v1, 0x50000);
    } else { // below 5.0: the heading reversed (0x8008EDE0 ..)
        NegH(g, R + 450u);
        NegH(g, R + 454u);
        NegH(g, R + 452u);
        speed = Sub(0x50000, g.S32(B + 480u));
    }
    g.W32(R + 480u, U(speed)); // 0x8008EE08
    if (out != 0) SaveHalfHeight(g, R, out); // 0x8008EE14
    return !g.Faulted();
}

// ============================================================================ 0x8008E044 PoseE
bool PoseE(GuestRam& g, uint32_t R, uint32_t out, uint32_t sp, const BikeTables& t) {
    using namespace rcfall;
    const uint32_t F = sp - 80u;
    g.W32(F + 84u, out); // 0x8008E074
    int32_t tp = 0, n = 0;
    ClipPhase(g, R, tp, n);
    const uint32_t s7 = (g.U32(R + 552u) >> 27) & 1u;
    g.W32(F + 32u, U(tp)); // 0x8008E0F8
    const bool pass = (g.U8(R + 572u) & 0x20u) != 0;
    int32_t a0, s6, s5;
    uint32_t B;
    if (pass) {
        a0 = S(((0u - s7) << 16) + 0xFFFED0E6u);
        s6 = S(0xFFFF165Au);
        s5 = S(0xFFFF44FEu);
        B = g.U32(g.U32(R + 596u) + 856u);
    } else {
        s6 = S(0xFFFEECFBu);
        B = g.U32(R + 596u);
        s5 = -18821;
        const int32_t p = S(g.U32(B + 652u) * 652u);
        const int32_t sg = p >> 31;
        const uint32_t ang = (U(p >> 16) + U(sg)) ^ U(sg);
        const int32_t cs = g.S16(kRcSinCos + ((ang & 0xFFFu) << 2) + 2u); // cos
        const int32_t v = FixMul(Sub(0x10000, Shl(cs, 4)), 0x1CCCC);      // 0x8008E19C
        a0 = Sub(S(0xFFFEE362u), v);
    }
    const uint32_t s8 = 0u - s7;
    const int32_t d = MipsDiv(FixMul(a0, g.S32(F + 32u)), n); // 0x8008E1BC
    const int32_t c = S(U(d) + ((0u - U(d) - U(d)) & s8));    // s7 ? -d : d
    Place(g, R, B + 184u, B + 516u, B + 522u, B + 444u, c, s6, s5); // 0x8008E224 ..
    // Rows from the bike's +0x20A / +0x204 / +0x210 (0x8008E2D0 ..), negated unless bit 27.
    for (uint32_t k = 0; k < 3; ++k) g.W16(R + 444u + 2u * k, g.U16(B + 522u + 2u * k));
    for (uint32_t k = 0; k < 3; ++k) g.W16(R + 438u + 2u * k, g.U16(B + 516u + 2u * k));
    for (uint32_t k = 0; k < 3; ++k) g.W16(R + 432u + 2u * k, g.U16(B + 528u + 2u * k));
    if (s7 == 0)
        for (uint32_t off : {438u, 440u, 442u, 432u, 434u, 436u}) NegH(g, R + off);
    const uint32_t k = pass ? (s8 & 0x180000u) + 0xFFF40000u : (s8 & 0x140000u) + 0xFFF60000u;
    cu::GScale(g, S(k), B + 516u, F + 16u); // 0x8008E3B0
    for (uint32_t i = 0; i < 3; ++i) g.W32(F + 16u + 4u * i, g.U32(F + 16u + 4u * i) + g.U32(B + 456u + 4u * i));
    int32_t v[3];
    cu::Read32x3(g, F + 16u, v);
    const int32_t len = Length3(v, t.sqrt); // 0x8008E3F0
    g.W32(R + 480u, U(len));
    ScaleTo16(g, rc::Recip(len), F + 16u, R + 450u); // 0x8008E448
    const uint32_t o = g.U32(F + 84u);
    if (o != 0) {
        const int32_t ph = g.S32(F + 32u);
        if (ph < 19660) { // below phase 0.3: /4
            g.W32(o, g.U32(R + 312u));
            g.W32(R + 312u, U(cu::Div4(g.S32(R + 312u))));
        } else if (!(0x34CCB < ph)) { // up to 3.3: /3
            g.W32(o, g.U32(R + 312u));
            const int32_t h = g.S32(R + 312u);
            const int32_t hi = static_cast<int32_t>((static_cast<int64_t>(h) * 0x55555556LL) >> 32);
            g.W32(R + 312u, U(hi) - U(h >> 31));
        }
    }
    return !g.Faulted();
}

// ============================================================================ 0x8008DF74 RiderSync
bool RiderSync(GuestRam& g, uint32_t R, uint32_t sp, const BikeTables& /*t*/, RecoverCallees& c) {
    const uint32_t F = sp - 32u;
    const uint32_t B = rcfall::PoseBike(g, R, (g.U8(R + 572u) & 0x20u) != 0);
    rc::CopyHalfwords(g, 9, R + 432u, R + 516u); // 0x8008DFB8
    GuestCopyWords(g, R + 328u, B + 328u, 32u);  // the road cursor
    GuestCopyWords(g, R + 360u, B + 360u, 12u);  // the key
    g.W32(R + 492u, g.U32(B + 492u));
    g.W32(R + 496u, g.U32(B + 496u));
    GuestCopyWords(g, R + 372u, B + 372u, 56u);
    g.W32(R + 428u, g.U32(B + 428u)); // 0x8008E014
    if (g.Faulted()) return false;
    if (!rc::Call(c, kFallRouteBindFn, {R + 172u, 1u, 0u}, F)) return false;
    g.W32(R + 324u, g.U32(B + 324u)); // 0x8008E020
    g.W16(R + 320u, g.U16(B + 320u)); // 0x8008E02C
    return !g.Faulted();
}

// ============================================================================ 0x8009246C RiderSettle
bool RiderSettle(GuestRam& g, uint32_t R, uint32_t sp, const BikeTables& /*t*/, RecoverCallees& c) {
    using namespace rcfall;
    const uint32_t F = sp - 96u;
    const uint32_t s3 = (g.U32(R + 552u) >> 30) & 1u;
    uint32_t pt = 0, nrm = 0;
    int32_t v1;
    if ((g.U32(R + 388u) & 1u) != 0) {
        pt = F + 40u;
        nrm = F + 56u;
        const uint32_t hint = g.U32(R + 536u);
        if (g.Faulted()) return false;
        uint32_t r = 0;
        if (!rc::Call(c, kFallGroundQueryFn, {R, 0u, F + 40u, F + 56u, hint}, F, &r)) return false;
        v1 = static_cast<int32_t>(r);
        g.W32(R + 536u, r); // 0x800924D4
    } else {
        v1 = 0;
        g.W8(R + 534u, g.U32(R + 372u) != 0 ? g.U8(R + 394u) : static_cast<uint8_t>(1)); // 0x800924F0 / F8
        g.W32(R + 536u, 0);                                                            // 0x800924FC
    }
    bool toGround = false; // 0x800925B4: the up vector from the found normal
    bool skipTest = false; // v1 < 0: straight to 0x80092564
    if (v1 <= 0) {
        const uint32_t sl = g.U32(R + 340u);
        pt = sl + 20u;
        nrm = sl + 8u;
        if (v1 < 0) {
            if (g.S32(R + 480u) == 0) {
                g.W32(R + 480u, 0x1C9C4u);                  // 0x80092530
                cu::GScale(g, 0x1C9C4, R + 450u, R + 456u); // 0x80092540
            }
            skipTest = true;
        }
    }
    if (!skipTest && g.S16(nrm + 2u) < -614) toGround = true; // 0x80092558
    if (toGround) {
        for (uint32_t k = 0; k < 3; ++k) // 0x800925C0 ..
            g.W16(R + 522u + 2u * k, static_cast<uint16_t>(0u - g.U16(nrm + 2u * k)));
    } else { // 0x80092564
        if (s3 == 0) {
            cu::GMulAdd(g, R + 184u, R + 522u, g.S32(R + 508u), F + 40u); // 0x80092578
            pt = F + 40u;
        }
        for (uint32_t k = 0; k < 3; ++k) // 0x8009258C ..
            g.W16(F + 56u + 2u * k, static_cast<uint16_t>(0u - g.U16(R + 522u + 2u * k)));
    }
    // 0x800925E4: the offset to the ground point, projected on the up vector.
    int32_t dv[3];
    for (uint32_t k = 0; k < 3; ++k) {
        dv[k] = S(g.U32(pt + 4u * k) - g.U32(R + 184u + 4u * k));
        g.W32(F + 24u + 4u * k, U(dv[k]));
    }
    int32_t m[3];
    for (uint32_t k = 0; k < 3; ++k) m[k] = FixMul(dv[k], rcfall::Row16(g, R + 522u + 2u * k));
    {
        const int64_t p = static_cast<int64_t>(dv[2]) * rcfall::Row16(g, R + 522u + 4u);
        g.W32(F + 64u, static_cast<uint32_t>(static_cast<uint64_t>(p)));       // 0x8009269C / 0x80092734
        g.W32(F + 68u, static_cast<uint32_t>(static_cast<uint64_t>(p) >> 32)); // 0x800926A0 / 0x80092738
    }
    const int32_t k = S(U(m[2]) + (U(m[1]) + U(m[0])));
    if (s3 != 0)
        cu::GMulAdd(g, R + 184u, R + 522u, k, R + 504u); // 0x80092758
    else
        cu::GMulAdd(g, R + 184u, R + 522u, S(U(k) - g.U32(R + 508u)), R + 184u);
    return !g.Faulted();
}

// ============================================================================ 0x8008F138 RiderOffTick
bool RiderOffTick(GuestRam& g, uint32_t R, int32_t dt, uint32_t sp, const BikeTables& t, RecoverCallees& c) {
    using namespace rcfall;
    const uint32_t F = sp - 32u;
    const int16_t live = g.S16(R + 320u);
    g.W32(F + 16u, 0); // 0x8008F154 (the delay slot: before the gate)
    if (live == 0) return !g.Faulted();
    g.W32(R + 472u, g.U32(R + 188u)); // 0x8008F164
    g.W32(R + 468u, g.U32(R + 184u));
    g.W32(R + 476u, g.U32(R + 192u));
    const uint32_t mount = g.U32(R + 604u);
    if (mount == 2u) {
        const uint32_t idx = static_cast<uint32_t>(g.U16(R + 544u)) - 38u;
        const uint32_t target = idx < 54u ? g.U32(kFallPoseTable + 4u * idx) : 0x8008F1FCu;
        if (g.Faulted()) return false;
        bool ok;
        switch (target) {
        case 0x8008F1B0u: ok = rc::Call(c, kFallPoseBFn, {R, F + 16u}, F); break;
        case 0x8008F1C4u: ok = rc::Call(c, kFallPoseCFn, {R, U(dt)}, F); break; // a1 still the dt
        case 0x8008F1D4u: ok = rc::Call(c, kFallPoseAFn, {R, F + 16u}, F); break;
        case 0x8008F1E8u: ok = rc::Call(c, kFallPoseEFn, {R, F + 16u}, F); break;
        case 0x8008F1FCu: ok = rc::Call(c, kFallPoseDFn, {R, F + 16u}, F); break;
        default: return false; // a table word that is not one of the five call sites
        }
        if (!ok) return false;
        const uint32_t B = g.U32(R + 596u);
        g.W32(R + 504u, g.U32(B + 504u)); // 0x8008F21C
        g.W32(R + 508u, g.U32(B + 508u)); // 0x8008F228
        const uint32_t f = g.U32(R + 552u);
        const uint32_t w512 = g.U32(B + 512u);
        g.W32(R + 252u, 0);
        g.W32(R + 248u, 0);
        g.W32(R + 244u, 0);
        g.W32(R + 552u, f | 0xC0000000u); // 0x8008F248
        g.W8(R + 547u, 1);                // 0x8008F250
        g.W32(R + 512u, w512);            // 0x8008F258
        rc::CopyHalfwords(g, 9, R + 432u, R + 256u);
        if (g.Faulted()) return false;
        if (!rc::Call(c, kFallRiderSyncFn, {R}, F)) return false; // 0x8008F25C
    } else {
        const uint32_t f = g.U32(R + 552u);
        if ((f & 0x40000000u) != 0) {
            GMulAdd32(g, R + 184u, R + 456u, dt, R + 184u); // 0x8008F284
        } else if ((f & 0x18u) == 0) {
            const int32_t v = FixMul(dt, g.S32(R + 480u));
            cu::GMulAdd(g, R + 184u, R + 450u, v, R + 184u); // 0x8008F2B8
            int32_t a2 = g.S32(R + 508u);
            if (a2 > 0) {
                const int32_t r = Sub(a2, 6553);
                g.W32(R + 508u, U(r)); // 0x8008F2D4
                if (r >= 0)
                    a2 = 6553;
                else
                    g.W32(R + 508u, 0); // 0x8008F2DC
                cu::GMulAdd(g, R + 184u, R + 522u, a2, R + 184u); // 0x8008F2EC
            }
        }
        if (g.Faulted()) return false;
        uint32_t rb = 0;
        if (!rc::Call(c, kFallRoadRebindFn, {R, 1u}, F, &rb)) return false; // 0x8008F2F8
        bool settle = true;
        if (rb != 1u && (g.U32(R + 552u) & 0x40000000u) == 0) {
            const uint32_t B = g.U32(R + 596u);
            const int32_t i = g.S8(B + 946u);
            settle = (g.U16(B + (U(i) << 3) + 954u) & 0x8000u) != 0; // 0x8008F338
        }
        if (settle) {
            if (g.Faulted()) return false;
            if (!rc::Call(c, kFallRiderSettleFn, {R}, F)) return false; // 0x8008F35C
            const uint32_t f2 = g.U32(R + 552u);
            if ((f2 & 0x40000000u) == 0) g.W32(R + 552u, f2 | 1u); // 0x8008F378
        }
    }
    if (g.Faulted()) return false;
    if (!rc::Call(c, kFallBuildObbAltFn, {R}, F)) return false; // 0x8008F37C
    const uint32_t saved = g.U32(F + 16u);
    if (saved != 0) g.W32(R + 312u, saved); // 0x8008F394: the half height restored
    const int32_t ang = RatAtan2(Row16(g, R + 450u), Row16(g, R + 454u), t.atan); // 0x8008F3A4
    g.W32(R + 292u, U(ang));
    const int32_t cs = g.S16(kRcSinCos + ((U(ang) & 0xFFFu) << 2) + 2u);
    g.W32(R + 296u, U(Shl(cs, 4))); // 0x8008F3E0
    const int32_t sn = g.S16(kRcSinCos + ((g.U32(R + 292u) & 0xFFFu) << 2));
    g.W32(R + 300u, U(Shl(sn, 4))); // 0x8008F3F0
    return !g.Faulted();
}

// ============================================================================ 0x8008F068 RiderOffPass
bool RiderOffPass(GuestRam& g, int32_t dt, uint32_t sp, const BikeTables& /*t*/, RecoverCallees& c) {
    const uint32_t F = sp - 40u;
    const int32_t hi = g.S32(g.U32(kRcPoolTable + 12u));
    uint32_t e = g.U32(kRcPoolTable);
    if (g.Faulted()) return false;
    for (int32_t s1 = hi; s1 >= 0; --s1) {
        const uint32_t R = g.U32(e + 852u);
        const uint32_t m = g.U32(R + 604u);
        if (g.Faulted()) return false;
        if (m >= 2u && !rc::Call(c, kFallRiderOffTickFn, {R, rcfall::U(dt)}, F)) return false; // 0x8008F0BC
        if ((g.U8(g.U32(e + 852u) + 572u) & 0x10u) != 0) {
            const uint32_t R2 = g.U32(g.U32(e + 856u) + 852u);
            const uint32_t m2 = g.U32(R2 + 604u);
            if (g.Faulted()) return false;
            if (m2 >= 2u && !rc::Call(c, kFallRiderOffTickFn, {R2, rcfall::U(dt)}, F)) return false; // 0x8008F104
        }
        e += g.U32(kRcPoolTable + 4u);
    }
    return !g.Faulted();
}

// ============================================================================ 0x8007E868 LaunchLift
bool LaunchLift(GuestRam& g, uint32_t dir, int32_t speed, int32_t c, int32_t k, uint32_t /*sp*/,
                const BikeTables& t) {
    using namespace rcfall;
    int32_t s0 = c;
    if (k < speed) {
        uint32_t q;
        if (k > 0) {
            if (speed > 0)
                q = (0u - FixDiv(U(k), U(speed))) >> 4; // 0x8007E8DC
            else
                q = FixDiv(U(k), 0u - U(speed)) >> 4; // 0x8007E8AC
        } else {
            if (speed > 0)
                q = FixDiv(0u - U(k), U(speed)) >> 4; // 0x8007E8C4
            else
                q = (0u - FixDiv(0u - U(k), 0u - U(speed))) >> 4; // 0x8007E8D4
        }
        uint32_t a0 = q;
        if (S(a0 << 16) < S(U(s0) << 16)) a0 = U(s0); // 0x8007E8F8
        s0 = S(a0);
    }
    const int32_t a0 = static_cast<int16_t>(static_cast<uint16_t>(s0));
    const int32_t v1 = g.S16(dir + 2u);
    if (a0 == v1) return !g.Faulted(); // 0x8007E924
    int32_t s3 = Shl(g.S16(dir), 4);
    int32_t s4 = Shl(g.S16(dir + 4u), 4);
    const int32_t s1 = Shl(a0, 4);
    g.W16(dir + 2u, static_cast<uint16_t>(s0)); // 0x8007E938
    int32_t h = Add(FixMul(s3, s3), FixMul(s4, s4));
    if (h < 6) { // 0x8007E964
        s3 = 0;
        s4 = 655;
        h = 6;
    }
    const int32_t m = FixMul(s1, s1);
    uint32_t q;
    if (Sub(0x10000, m) > 0) {
        const uint32_t a = 0x10000u - U(m);
        q = h > 0 ? FixDiv(a, U(h)) : 0u - FixDiv(a, 0u - U(h));
    } else {
        const uint32_t a = U(m) - 0x10000u;
        q = h > 0 ? 0u - FixDiv(a, U(h)) : FixDiv(a, 0u - U(h));
    }
    const int32_t r = Shl(SqrtGte(S(q), t.sqrt), 2); // 0x8007EA0C
    g.W16(dir, static_cast<uint16_t>(FixMul(r, s3) >> 4));      // 0x8007EA34
    g.W16(dir + 4u, static_cast<uint16_t>(FixMul(r, s4) >> 4)); // 0x8007EA3C
    return !g.Faulted();
}

// ============================================================================ RASHCDG [0x8007DDF4, 0x8007E804)
namespace rcfall {
// SLUS 0x8002EB78 Blend16(a, b, out, wa, wb) over guest halfwords.
void GBlend16(GuestRam& g, uint32_t a, uint32_t b, uint32_t out, int32_t wa, int32_t wb) {
    int16_t x[3], y[3], o[3];
    cu::Read16x3(g, a, x);
    cu::Read16x3(g, b, y);
    Blend16(x, y, o, wa, wb);
    cu::Write16x3(g, out, o);
}
// The sine table's index of a 16.16 radian value: bits 14..25 of 163 v (0x8007E298 / 0x8007E31C).
uint32_t DownAngle(int32_t v) { return (U(cu::MulLo(v, 163)) >> 14) & 0xFFFu; }
int32_t Sin16(GuestRam& g, uint32_t i) { return Shl(g.S16(kRcSinCos + 4u * i), 4); }
int32_t Cos16(GuestRam& g, uint32_t i) { return Shl(g.S16(kRcSinCos + 4u * i + 2u), 4); }
} // namespace rcfall

bool RiderPassDownWalk(GuestRam& g, int32_t dt, uint32_t sp, const BikeTables& t, RecoverCallees& c) {
    using namespace rcfall;
    uint32_t node = g.U32(kDownListHead + 4u);
    g.W32(sp + 184u, node); // 0x8007E800 (the delay slot: also when the list is empty)
    for (int guard = 0; node != kDownListHead; ++guard) {
        if (guard > 4096 || g.Faulted()) return false;
        const uint32_t e = node - 1088u;
        if ((g.U32(e + 568u) & 0x80000u) != 0) {
            // ---- on the ground (0x8007DE20)
            bool s2 = true;
            const int32_t rate = g.S32(e + 640u);
            if (rate != 0) {
                const int32_t v1 = Add(g.S32(e + 708u), FixMul(rate, dt));
                g.W32(e + 708u, U(v1)); // 0x8007DE54
                int32_t a1 = 0x1921F;
                if (g.U32(e + 856u) != 0 && v1 > 0) a1 = 0x2CAE2;
                const int32_t w = g.S32(e + 708u);
                const uint32_t lo = U(Add(w, a1) >> 31) & (0u - U(a1) - U(w));
                const int32_t d = Sub(a1, w);
                const int32_t v = S(U(w) + lo + (U(d >> 31) & U(d)));
                g.W32(e + 708u, U(v)); // 0x8007DE98
                if (cu::Iabs(v) == a1) g.W32(e + 640u, 0); // 0x8007DEAC
            }
            const int32_t s3 = cu::GDot(g, e + 808u, e + 522u); // 0x8007DEB4
            bool s0 = (g.U32(e + 568u) & 0x100000u) != 0 ? s3 >= 0 : s3 <= 0;
            if (s0 && cu::GDot(g, e + 802u, e + 522u) < 0) { // 0x8007DEF8
                const uint32_t f = g.U32(e + 568u);
                g.W32(e + 568u, (f & 0x100000u) != 0 ? (f & 0xFFEFFFFFu) : (f | 0x100000u)); // 0x8007DF30
                s0 = false;
            }
            if (g.S32(e + 488u) == 0 || s0) {
                g.W32(e + 488u, 0); // 0x8007DF50
                const int32_t k = cu::GDot(g, e + 796u, e + 522u);
                if (!(cu::Iabs(k) < 6554)) {
                    MulAdd16(g, e + 796u, e + 522u, Neg(k), sp + 104u); // 0x8007DF8C
                    for (uint32_t i = 0; i < 3; ++i)
                        g.W16(e + 796u + 2u * i, static_cast<uint16_t>(g.S32(sp + 104u + 4u * i) >> 4));
                    s2 = false;
                } else {
                    rc::GteOp(g, e + 522u, e + 808u, e + 796u); // 0x8007E004
                }
                int32_t sum = 0;
                if (!rc::GNormalize(g, e + 796u, t, sum)) return false; // 0x8007E024
                rc::GteOp(g, e + 796u, e + 522u, e + 808u);           // 0x8007E068
                if (!s2) {
                    if (!rc::GNormalize(g, e + 808u, t, sum)) return false; // 0x8007E090
                    rc::GteOp(g, e + 808u, e + 796u, e + 802u);           // 0x8007E0D0
                } else {
                    for (uint32_t i = 0; i < 3; ++i) g.W16(e + 802u + 2u * i, g.U16(e + 522u + 2u * i)); // 0x8007E104
                }
            }
            if (g.S32(e + 488u) == 0 && g.S32(e + 640u) == 0 && s2)
                g.W32(e + 568u, g.U32(e + 568u) | 0x200000u); // 0x8007E144
            g.W32(sp + 188u, 0);                               // 0x8007E14C: no drag
        } else {
            // ---- still falling (0x8007E150)
            if (g.S32(e + 728u) != 0) {
                const int32_t v1 = Add(g.S32(e + 724u), dt);
                const int32_t du = g.S32(e + 728u);
                g.W32(e + 724u, U(v1)); // 0x8007E178
                int32_t step = dt;
                if (du < v1) {
                    step = Sub(dt, Sub(v1, du));
                    g.W32(e + 728u, 0); // 0x8007E188
                }
                if (g.S32(e + 656u) != 0)
                    g.W32(e + 692u, U(Add(g.S32(e + 692u), FixMul(g.S32(e + 656u), step)))); // 0x8007E1B4
                if (g.S32(e + 640u) != 0)
                    g.W32(e + 708u, U(Add(g.S32(e + 708u), FixMul(g.S32(e + 640u), step)))); // 0x8007E1DC
            }
            g.W32(e + 488u, U(FixMul(0xFEB8, g.S32(e + 488u))));      // 0x8007E1EC
            g.W32(sp + 188u, U(FixMul(g.S32(kDownDragWord), dt)));    // 0x8007E204
        }
        // ---- the roll +0x268 turns the frame rows +0x322 / +0x328 (0x8007E208)
        const int32_t roll = g.S32(e + 488u);
        if (roll != 0) {
            const int32_t v = Add(g.S32(e + 616u), FixMul(roll, dt));
            g.W32(e + 616u, U(v)); // 0x8007E23C
            if (0x6487D < v)
                g.W32(e + 616u, U(Add(v, S(0xFFF9B782u)))); // 0x8007E26C
            else if (!(S(0xFFF9B782u) < v))
                g.W32(e + 616u, U(Add(v, 0x6487E)));
            const uint32_t ang = DownAngle(g.S32(e + 616u));
            const int32_t cs = Cos16(g, ang), sn = Sin16(g, ang);
            g.W32(sp + 16u, U(sn));
            GBlend16(g, e + 620u, e + 628u, e + 808u, cs, sn); // 0x8007E2E0
            g.W32(sp + 16u, U(cs));
            GBlend16(g, e + 620u, e + 628u, e + 802u, Neg(sn), cs); // 0x8007E2F8
        }
        // ---- the rows +0x1B0 = R x the ground frame (0x8007E304 .. 0x8007E61C)
        {
            const uint32_t aA = DownAngle(g.S32(e + 708u)), aB = DownAngle(g.S32(e + 692u));
            const int32_t sC = Sin16(g, 0), cC = Cos16(g, 0);
            const int32_t sA = Sin16(g, aA), cA = Cos16(g, aA);
            const int32_t sB = Sin16(g, aB), cB = Cos16(g, aB);
            const int32_t s5 = FixMul(sA, sC);
            const int32_t p = FixMul(sC, cA);
            g.W32(sp + 192u, U(p));
            int32_t m[9];
            m[0] = Add(FixMul(cA, cB), FixMul(s5, sB));
            m[1] = FixMul(sA, cC);
            m[2] = Sub(FixMul(s5, cB), FixMul(cA, sB));
            m[3] = Sub(FixMul(p, sB), FixMul(sA, cB));
            m[4] = FixMul(cA, cC);
            m[5] = Add(FixMul(sA, sB), FixMul(p, cB));
            m[6] = FixMul(cC, sB);
            m[7] = Neg(sC);
            m[8] = FixMul(cC, cB);
            for (uint32_t i = 0; i < 9; ++i) g.W32(sp + 120u + 4u * i, U(m[i])); // 0x8007E414 .. 0x8007E4BC
            int16_t rt[9], fr[9], out[9];
            for (uint32_t i = 0; i < 9; ++i) {
                rt[i] = static_cast<int16_t>(m[i] >> 4);
                g.W16(sp + 80u + 2u * i, static_cast<uint16_t>(rt[i])); // 0x8007E4C8 ..
            }
            for (uint32_t i = 0; i < 9; ++i) fr[i] = g.S16(e + 796u + 2u * i);
            MulMatrix0(rt, fr, out); // three MVMVA (cop2 0x49E012), 0x8007E57C / 5BC / 600
            for (uint32_t i = 0; i < 9; ++i) g.W16(e + 432u + 2u * i, static_cast<uint16_t>(out[i]));
        }
        // ---- drag and gravity (0x8007E620)
        if ((g.U32(e + 564u) & 0x100000u) == 0) {
            const int32_t k = g.S32(sp + 188u);
            const int32_t vx = g.S32(e + 456u);
            g.W32(e + 456u, U(Sub(vx, FixMul(k, vx))));
            const int32_t fy = Sub(FixMul(k, g.S32(e + 460u)), FixMul(0x9D087, dt));
            g.W32(e + 460u, U(Sub(g.S32(e + 460u), fy)));
            g.W32(e + 464u, U(Sub(g.S32(e + 464u), FixMul(k, g.S32(e + 464u)))));
        }
        // ---- the speed and the heading (0x8007E69C)
        {
            const int32_t vx = g.S32(e + 456u), vy = g.S32(e + 460u), vz = g.S32(e + 464u);
            const int64_t pz = static_cast<int64_t>(vz) * vz;
            g.W32(sp + 200u, static_cast<uint32_t>(static_cast<uint64_t>(pz)));
            g.W32(sp + 204u, static_cast<uint32_t>(static_cast<uint64_t>(pz) >> 32));
            const int32_t sq = Add(FixMul(vz, vz), Add(FixMul(vy, vy), FixMul(vx, vx)));
            const int32_t before = g.S32(e + 480u);
            g.W32(e + 580u, U(sq)); // 0x8007E724
            const int32_t r = Shl(SqrtGte(sq, t.sqrt), 2);
            g.W32(e + 480u, U(r));
            g.W32(e + 576u, U(r));
            if (r != 0) ScaleTo16(g, rc::Recip(r), e + 456u, e + 450u); // 0x8007E788
            if (0x8F0D7 < before && !(0x8F0D7 < g.S32(e + 480u))) {
                g.W32(e + 480u, 0x8F0D8u);                   // 0x8007E7BC
                cu::GScale(g, 0x8F0D8, e + 450u, e + 456u); // 0x8007E7CC
            }
        }
        g.W32(e + 756u, g.U32(e + 756u) + U(dt)); // 0x8007E7E8
        if (g.Faulted()) return false;
        if (!rc::Call(c, kFallRowsFromUpFn, {e}, sp)) return false; // 0x8007E7E4
        node = g.U32(g.U32(sp + 184u) + 4u);
        g.W32(sp + 184u, node); // 0x8007E800
    }
    return !g.Faulted();
}

} // namespace rr::sim
