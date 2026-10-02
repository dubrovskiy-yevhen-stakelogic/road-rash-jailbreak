#include "game/sim/bike_step.h"

#include "game/sim/fixed.h"
#include "game/sim/vec.h"

#include <utility>

namespace rr::sim {
namespace {

// 32-bit wrapping arithmetic, as the R3000's addu/subu/sll do it.
inline int32_t Add(int32_t a, int32_t b) {
    return static_cast<int32_t>(static_cast<uint32_t>(a) + static_cast<uint32_t>(b));
}
inline int32_t Sub(int32_t a, int32_t b) {
    return static_cast<int32_t>(static_cast<uint32_t>(a) - static_cast<uint32_t>(b));
}
inline int32_t Neg(int32_t a) { return static_cast<int32_t>(0u - static_cast<uint32_t>(a)); }
inline int32_t MulW(int32_t a, uint32_t k) {
    return static_cast<int32_t>(static_cast<uint32_t>(a) * k);
}
inline int32_t Shl(int32_t a, int s) { return static_cast<int32_t>(static_cast<uint32_t>(a) << s); }
inline int32_t Sra(int32_t a, int s) { return a >> s; } // arithmetic on every compiler we build with
// The `sra/addu/xor` absolute value: INT32_MIN stays INT32_MIN.
inline int32_t MipsAbs(int32_t x) {
    const uint32_t s = static_cast<uint32_t>(x >> 31);
    return static_cast<int32_t>((static_cast<uint32_t>(x) + s) ^ s);
}
// The hand-written sign split around the UNSIGNED FixDiv that the whole step uses (e.g.
// 0x8007B138..0x8007B194): `n` and `d` made positive by negation, the quotient negated when exactly
// one of them was not positive.
inline int32_t SplitDiv(int32_t n, int32_t d) {
    if (n > 0) {
        if (d > 0) return static_cast<int32_t>(FixDiv(static_cast<uint32_t>(n), static_cast<uint32_t>(d)));
        return Neg(static_cast<int32_t>(FixDiv(static_cast<uint32_t>(n), static_cast<uint32_t>(Neg(d)))));
    }
    if (d > 0) return Neg(static_cast<int32_t>(FixDiv(static_cast<uint32_t>(Neg(n)), static_cast<uint32_t>(d))));
    return static_cast<int32_t>(FixDiv(static_cast<uint32_t>(Neg(n)), static_cast<uint32_t>(Neg(d))));
}
// Signed divide by 4 / by 2 rounding toward zero, the compiler's `bgez; addiu 3; sra 2` and
// `srl 31; addu; sra 1` idioms.
inline int32_t Div4(int32_t v) { return Sra(v < 0 ? Add(v, 3) : v, 2); }
inline int32_t Div2(int32_t v) {
    return Sra(Add(v, static_cast<int32_t>(static_cast<uint32_t>(v) >> 31)), 1);
}

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
    for (uint32_t k = 0; k < 3; ++k) g.W32(a + 4u * k, static_cast<uint32_t>(v[k]));
}

// `SLUS 0x8002E468 Normalize(v)` as a caller that tests its v0 sees it: v0 is the sum of squares
// `x*x + y*y + z*z` (GTE SQR, then two TRAPPING `add`s at 0x8002E4A0/0x8002E4A4). Returns false
// where the console raises the overflow exception; `n` is the returned sum otherwise.
bool NormalizeN(int16_t v[3], const uint16_t* rsqrt, int32_t& n) {
    const int64_t x2 = static_cast<int64_t>(v[0]) * v[0];
    const int64_t y2 = static_cast<int64_t>(v[1]) * v[1];
    const int64_t z2 = static_cast<int64_t>(v[2]) * v[2];
    const int64_t s1 = x2 + y2;
    if (s1 > INT32_MAX) return false;
    const int64_t s2 = s1 + z2;
    if (s2 > INT32_MAX) return false;
    if (rsqrt == nullptr || !Normalize(v, rsqrt)) return false;
    n = static_cast<int32_t>(s2);
    return true;
}

} // namespace

void BikeResetOrientation(GuestRam& g, uint32_t e) {
    g.W16(e + 444, 0);                      // 0x8008CF74
    const uint16_t v1 = g.U16(e + 444);
    g.W16(e + 446, 0);
    const uint16_t a1 = g.U16(e + 446);
    g.W16(e + 448, 4096);
    g.W16(e + 442, 0);
    g.W16(e + 438, 0);
    g.W16(e + 436, 0);
    g.W16(e + 434, 0);
    g.W16(e + 440, 4096);
    g.W16(e + 432, 4096);
    g.W16(e + 530, 0);
    g.W16(e + 528, 0);
    g.W16(e + 526, 0);
    g.W16(e + 522, 0);
    g.W16(e + 520, 0);
    g.W16(e + 518, 0);
    g.W16(e + 532, 4096);
    g.W16(e + 524, 4096);
    g.W16(e + 516, 4096);
    g.W16(e + 454, 4096);                   // sh a2,454 (a2 = 4096)
    g.W16(e + 450, v1);
    g.W16(e + 452, a1);                     // 0x8008CFD8, the delay slot of `jr ra`
}

bool BikeHeadingPass(GuestRam& g, uint32_t list, const BikeTables& t) {
    if (t.sincos == nullptr || t.atan == nullptr || t.rsqrt == nullptr) return false;
    const int16_t* sc = t.sincos;
    uint32_t node = g.U32(list + 4u);                                   // 0x8007AC2C
    int guard = 0;
    while (node != list) {                                              // 0x8007AC34 / 0x8007B810
        if (g.Faulted() || ++guard > 4096) return false;
        const uint32_t e = node - 1088u;                                // s2

        // ---- 0x8007AC44: a wipeout's two lean fields
        if (g.U32(e + 564) & 0x3000u) {
            g.W32(e + 676, 0);
            g.W32(e + 828, 0);
        }
        // ---- 0x8007AC60: the lean's sine and cosine, 16.16 (s3 = sin << 4, s1 = cos << 4)
        int32_t sinL = 0, cosL = 0x10000;
        {
            const int32_t lean = g.S32(e + 676);
            if (lean != 0) {
                const uint32_t idx = (static_cast<uint32_t>(MulW(lean, 163u)) >> 14) & 0xFFFu; // srl
                sinL = Shl(sc[2u * idx], 4);
                cosL = Shl(sc[2u * idx + 1u], 4);
            }
        }
        // ---- 0x8007ACBC..0x8007AD74: whether the heading is re-derived this frame
        const bool a = (g.U32(e + 568) & 0x1Fu) != 0 || (g.U32(e + 564) & 0x3400u) != 0;
        bool moving = false;
        if (!(g.U32(e + 568) & 0x600u) && g.S32(e + 576) != 0) moving = true;          // 0x8007ACFC
        else if (g.U32(e + 564) & 0x40000u) moving = true;                            // 0x8007AD0C
        else if (g.S32(e + 488) != 0) moving = true;                                  // 0x8007AD20
        else if (g.S32(e + 676) != 0) moving = true;                                  // 0x8007AD30
        bool heading;
        if (moving && !a) heading = true;                                             // 0x8007AD40
        else if (a && (g.U32(e + 568) & 0xFu) == 0) heading = true;                   // 0x8007AD48
        else heading = (g.U32(e + 568) & 0x4000u) != 0;                               // 0x8007AD64

        if (heading) {
            if (sinL != 0 || cosL < 0) {
                // 0x8007AD88: the heading is the ground facing turned toward +0x204 by the lean
                int16_t fa[3], fb[3], h[3];
                Read16x3(g, e + 528, fa);
                Read16x3(g, e + 516, fb);
                Blend16(fa, fb, h, cosL, Neg(sinL));                  // 0x8002EB78 at 0x8007AD9C
                Write16x3(g, e + 450, h);
                int32_t n = 0;
                Read16x3(g, e + 450, h);
                if (!NormalizeN(h, t.rsqrt, n)) return false;         // 0x8002E468 at 0x8007ADA4
                Write16x3(g, e + 450, h);
                if (n == 0) BikeResetOrientation(g, e);               // 0x8008CF74 at 0x8007ADB4
                int16_t d[3], ir[3], out[3];
                Read16x3(g, e + 522, d);                              // ctc2 R11, R22, R33
                Read16x3(g, e + 450, ir);                             // IR1..IR3
                OuterProduct(d, ir, out);                             // cop2 0x178000C
                Write16x3(g, e + 814, out);
            } else {
                // 0x8007AE20: a plain copy
                for (uint32_t k = 0; k < 3; ++k) g.W16(e + 450 + 2u * k, g.U16(e + 528 + 2u * k));
                for (uint32_t k = 0; k < 3; ++k) g.W16(e + 814 + 2u * k, g.U16(e + 516 + 2u * k));
                g.W32(e + 564, g.U32(e + 564) & 0xFFFFFBFFu);
            }
        }

        // ---- 0x8007AE60: gravity along the orientation's three axes, 157 = 9.81 * 16
        g.W32(e + 732, static_cast<uint32_t>(MulW(g.S16(e + 518), 157u)));
        {
            const int32_t e0 = MulW(g.S16(e + 524), 157u);
            const int32_t e4 = MulW(g.S16(e + 530), 157u);
            const int32_t lean = g.S32(e + 676);                      // 0x8007AEA0
            g.W32(e + 736, static_cast<uint32_t>(e0));
            g.W32(e + 740, static_cast<uint32_t>(e4));
            int32_t along = e4;
            if (lean != 0) along = Sub(FixMul(e4, cosL), FixMul(g.S32(e + 732), sinL)); // 0x8007AEC8
            g.W32(e + 760, static_cast<uint32_t>(along));             // 0x8007AF14
        }
        // ---- 0x8007AF18..0x8007B0A4: RatAtan2 inlined (the same octant table, 0x8005B648 in
        // RASHCDG, and the same interpolation over 0x8005285C), then 0x8007B0A8: to 16.16 radians
        const int32_t angle = RatAtan2(g.S32(e + 732), g.S32(e + 736), t.atan);
        const int32_t slope = Neg(Sra(MulW(angle, 25736u), 8));
        g.W32(e + 668, static_cast<uint32_t>(slope));                // 0x8007B0E0

        // ---- 0x8007B0D8: the steering pair
        if (g.U32(e + 568) & 0x14Cu) {
            int32_t v1;
            const int32_t lat = g.S32(e + 488);
            if (lat != 0 && (lat ^ g.S32(e + 652)) >= 0) {            // 0x8007B0EC..0x8007B100
                const int32_t mag = MipsAbs(g.S32(e + 652));
                const uint32_t st = g.U32(e + 556);
                const int32_t lo = g.S32(st + 296), hi = g.S32(st + 300);
                if (mag < lo) g.W32(e + 672, 0);
                else if (mag < hi) g.W32(e + 672, static_cast<uint32_t>(SplitDiv(Sub(mag, lo), Sub(hi, lo))));
                else g.W32(e + 672, 0x10000u);
                const int32_t w = FixMul(g.S32(e + 672), g.S32(g.U32(e + 556) + 236)); // 0x8007B198
                const int32_t a0 = g.S32(e + 652);
                v1 = a0 > 0 ? Add(a0, w) : Sub(a0, w);                // 0x8007B1CC
            } else {
                v1 = g.S32(e + 652);                                  // 0x8007B1DC
                g.W32(e + 672, 0);
            }
            g.W32(e + 636, static_cast<uint32_t>(Sub(v1, g.S32(e + 668)))); // 0x8007B1E4
        } else {
            const int32_t s0 = Add(g.S32(e + 636), slope);            // 0x8007B208
            const int32_t lat = g.S32(e + 488);
            int32_t w = 0;
            if (lat != 0 && (lat ^ s0) >= 0) {                        // 0x8007B204..0x8007B210
                const int32_t mag = MipsAbs(s0);
                const uint32_t st = g.U32(e + 556);
                const int32_t hi = g.S32(st + 300), ec = g.S32(st + 236), lo = g.S32(st + 296);
                const int32_t top = Add(hi, ec);
                if (mag < lo) g.W32(e + 672, 0);
                else if (mag < top) g.W32(e + 672, static_cast<uint32_t>(SplitDiv(Sub(mag, lo), Sub(top, lo))));
                else g.W32(e + 672, 0x10000u);
                w = FixMul(g.S32(e + 672), g.S32(g.U32(e + 556) + 236)); // 0x8007B2B4
            } else {
                g.W32(e + 672, 0);                                    // 0x8007B2E8
            }
            g.W32(e + 652, static_cast<uint32_t>(s0 > 0 ? Sub(s0, w) : Add(s0, w))); // 0x8007B2EC
        }

        // ---- 0x8007B304: the pitch in 4096-per-turn, and the roll the display leans by
        const int32_t pitch = Sra(MulW(g.S32(e + 616), 652u), 16);   // s0
        int32_t roll;                                                 // a2
        if (g.U32(e + 856) == 0) {
            roll = g.S32(e + 652);                                    // 0x8007B3AC
        } else {
            const uint32_t fc = g.U32(e + 568);
            if (fc & 0xECu) roll = g.S32(e + 652);
            else if (fc & 0x100u) roll = (g.S32(e + 652) <= 0) ? static_cast<int32_t>(0xFFFE6DE1u) : 0x2CAE2;
            else {                                                    // 0x8007B370
                int32_t v1 = g.S32(e + 636);
                const int32_t mask = (v1 < 0) ? Neg(static_cast<int32_t>(g.U32(e + 652) >> 31)) : 0;
                const int32_t a0 = g.S32(e + 652);
                if (v1 < a0) v1 = a0;
                roll = mask & v1;
            }
        }
        {
            const uint32_t gs = g.U32(0x8005B2F8u);                   // 0x8007B3B0
            // `sltu; beqz` at 0x8007B3C4: a bike that is NOT a player's always smooths; a player's
            // only while the word at 0x8005B220 is set.
            const bool player = static_cast<uint32_t>(g.U16(e + 172)) < g.U32(gs + 48);
            if (!player || g.U32(0x8005B220u) != 0) {
                roll = Sub(roll, g.S32(e + 668));                     // 0x8007B3F4
                int32_t v;
                if (g.U32(e + 568) & 0x7FFu) v = Div4(Add(g.S32(e + 648), MulW(roll, 3u)));
                else if (g.S32(e + 660) != 0 || g.S16(e + 944) == 0) v = Div2(Add(g.S32(e + 648), roll));
                else v = Div4(Add(MulW(g.S32(e + 648), 3u), roll));
                g.W32(e + 648, static_cast<uint32_t>(v));             // 0x8007B46C
                roll = Add(g.S32(e + 648), g.S32(e + 668));
            }
        }
        int16_t rot[9];
        {
            const int16_t angles[3] = {static_cast<int16_t>(Neg(pitch)), 0,
                                       static_cast<int16_t>(Neg(Sra(MulW(roll, 652u), 16)))};
            RotMatrix(angles, rot, sc);                               // 0x8004D2A4 at 0x8007B4B8
        }

        // ---- 0x8007B4C0: the box centre from the contact point
        if ((g.U32(e + 564) & 0x200000u) || ((g.U32(e + 568) & 0x100u) && g.U32(e + 772) != 0)) {
            int32_t base[3], out[3];
            int16_t dir[3];
            Read32x3(g, e + 504, base);
            Read16x3(g, e + 522, dir);
            MulAdd(base, dir, g.S32(e + 772), out);                   // 0x8002EAD8 at 0x8007B500
            Write32x3(g, e + 184, out);
        } else if (pitch == 0) {
            int32_t p[3];                                             // 0x8007B6D0
            Read32x3(g, e + 504, p);
            Write32x3(g, e + 184, p);
        } else {
            int32_t h = g.S32(e + 308);                               // 0x8007B518
            if (pitch > 0) h = Neg(h);
            const int32_t s3 = Div4(MulW(h, 3u));
            const uint32_t idx = static_cast<uint32_t>(pitch) & 0xFFFu;
            const int32_t k1 = FixMul(s3, Shl(sc[2u * idx], 4));     // 0x8007B558
            for (uint32_t k = 0; k < 3; ++k) {
                const int32_t r = Shl(g.S16(e + 438 + 2u * k), 4);   // the OLD +0x1B0 row 1
                g.W32(e + 184 + 4u * k, static_cast<uint32_t>(Add(FixMul(r, k1), g.S32(e + 504 + 4u * k))));
            }
            const int32_t k2 = FixMul(s3, Sub(0x10000, Shl(sc[2u * idx + 1u], 4))); // 0x8007B624
            for (uint32_t k = 0; k < 3; ++k) {
                const int32_t r = Shl(g.S16(e + 528 + 2u * k), 4);
                g.W32(e + 184 + 4u * k, static_cast<uint32_t>(Add(FixMul(r, k2), g.S32(e + 184 + 4u * k))));
            }
        }
        // ---- 0x8007B6E8: +0x244 = +0x240^2, then +0x1B0 = rot x (+0x204) on the GTE (MVMVA per column)
        {
            const int32_t sp = g.S32(e + 576);
            g.W32(e + 580, static_cast<uint32_t>(FixMul(sp, sp)));
            int16_t m[9], out[9];
            for (uint32_t k = 0; k < 9; ++k) m[k] = g.S16(e + 516 + 2u * k);
            MulMatrix0(rot, m, out);
            for (uint32_t k = 0; k < 9; ++k) g.W16(e + 432 + 2u * k, static_cast<uint16_t>(out[k]));
        }
        node = g.U32(node + 4u);                                      // 0x8007B808
    }
    return !g.Faulted();
}

// ============================================================================ region C
bool BikeStepRegionC(GuestRam& g) {
    const uint32_t head = kRideListHead;                              // s1
    uint32_t node = g.U32(head + 4u);                                 // 0x80076208: t2
    int guard = 0;
    while (node != head) {                                            // 0x80076210 / 0x800767FC
        if (g.Faulted() || ++guard > 4096) return false;
        const uint32_t e = node - 1088u;                              // a3
        const uint32_t pas = g.U32(e + 856);
        const int32_t lean = g.S32(e + 676);                          // a1
        const uint32_t st = g.U32(e + 556);                           // t0
        const bool t3 = pas != 0u;                                    // sltu t3,t1(=0),v0
        const int32_t alean = MipsAbs(lean);                          // a0
        uint32_t t1 = 0;
        if (g.U32(e + 564) & 4u) t1 = g.U32(e + 716) >> 31;            // 0x80076258
        // 0x80076264..0x8007629C: a2 = |lean| is NOT below the loop-out limit stats[+0xCC], halved
        // (toward zero) with a passenger while leaning positive
        bool below;
        if (t3 && lean > 0) below = alean < Div2(g.S32(st + 204));
        else below = alean < g.S32(st + 204);
        const bool a2 = !below;
        // 0x800762A0..0x800762D4: flagsA bit 4 for an AI past stats[+0xD0]; stored unconditionally
        uint32_t fa = g.U32(e + 560);
        uint32_t bit = 0;
        if (fa & 0x08000000u) bit = (g.S32(st + 208) < alean) ? 1u : 0u;
        fa |= bit << 4;
        g.W32(e + 560, fa);
        // ---- the loop-out trigger, 0x800762D8..0x800763FC
        if (!(g.U32(e + 564) & 0x400u) && (fa & 0x300u) && (a2 || t1 != 0u) &&
            !(fa & 0x20000000u)) {
            uint32_t fc = g.U32(e + 568);                             // 0x80076300
            if (g.S32(e + 364) >= 0) fc &= 0xFFBFFFFFu;
            else fc |= 0x00400000u;
            g.W32(e + 568, fc);
            g.W32(e + 716, 0);                                        // 0x80076334, delay slot
            uint32_t nfc;
            if (t3 && g.S32(e + 676) > 0) {
                nfc = g.U32(e + 568) | 0x820u;                        // 0x80076348
            } else {
                const uint32_t w = g.U32(e + 568) | 0x800u;           // 0x80076358 / 0x8007636C
                bool set = false;
                if (g.U32(e + 564) & 0x400u) set = true;              // never true: tested clear above
                else if (t1 != 0u && g.U32(e + 596) == 0u) set = true; // 0x80076378
                else if (a2) {                                        // 0x80076388
                    const int32_t l2 = g.S32(e + 676);
                    const uint32_t fa2 = g.U32(e + 560);
                    if (l2 < 0) set = (fa2 & 0x100u) != 0u;           // 0x800763A0
                    else if (l2 > 0) set = (fa2 & 0x200u) != 0u;      // 0x800763BC
                }
                nfc = w | (((set ? 1u : 0u) << 5) + 32u);             // 0x800763D8
            }
            g.W32(e + 568, nfc);                                      // 0x800763E0
            int32_t s = g.S32(e + 576);
            if (s < 0x10000) s = 0x10000;
            g.W32(e + 576, static_cast<uint32_t>(s));                 // 0x800763FC
        }
        // ---- the rider-off trigger, 0x80076400..0x80076460
        if (g.S32(e + 616) == 0 && g.S32(e + 620) == 0 && g.S32(e + 624) == 0) {
            const uint32_t fc = g.U32(e + 568);
            if ((fc & 0x7FFu) == 0u) {
                const uint32_t rider = g.U32(e + 852);
                if (g.U32(rider + 604) >= 2u) g.W32(e + 568, fc | 0x880u); // `sltiu ... 2`: unsigned
            }
        }
        // ---- the passive resistance, 0x80076464..0x80076624
        {
            const int32_t surf = g.S8(e + 534);                       // lb: an UNBOUNDED signed index
            const int32_t r = g.S32(kEnvTable + static_cast<uint32_t>(Shl(surf, 2)));
            g.W32(e + 756, 0);
            g.W32(e + 752, 0);                                        // 0x80076488, delay slot
            if (!(g.U32(e + 564) & 0x400u)) {
                const int32_t thr = (g.U32(e + 560) & 0x08000000u) ? 0 : 0x1C9C4;
                const int32_t fm = FixMul(r, g.S32(e + 580));
                const int32_t k2 = (thr < g.S32(e + 576)) ? Neg(fm) : 0;   // a2
                const uint32_t gs = g.U32(kGameStatePtr);
                const uint32_t h = g.U16(e + 172);
                int32_t k = g.S32(st + 320);                          // a1
                if (h < g.U32(gs + 48) && g.S8(kOptionByte) != 0) {  // 0x800764F8, 0x80076504
                    const uint32_t other = g.U32(kPlayerBikes + 4u * (h < 1u ? 1u : 0u));
                    if (Add(g.S32(other + 324), 0xC000) < g.S32(e + 324)) k = Sub(k, g.S32(st + 332));
                }
                g.W32(e + 752, static_cast<uint32_t>(FixMul(k2, k)));
                g.W32(e + 756, static_cast<uint32_t>(FixMul(k2, g.S32(st + 324))));
                const int32_t l = g.S32(e + 676);
                if (l != 0) {
                    const int32_t s = Add(FixMul(g.S32(st + 328), MipsAbs(l)), 0x10000);
                    const int32_t p0 = FixMul(g.S32(e + 752), s);
                    const int32_t p1 = FixMul(g.S32(e + 756), s);
                    g.W32(e + 752, static_cast<uint32_t>(p0));
                    g.W32(e + 756, static_cast<uint32_t>(p1));
                }
            }
        }
        // ---- 0x80076628: the passive term and +0x2EC
        g.W32(e + 764, static_cast<uint32_t>(Add(g.S32(e + 760), g.S32(e + 752))));
        g.W32(e + 748, static_cast<uint32_t>(FixMul(g.S32(st + 12), g.S32(e + 736))));
        // ---- the hard-braking latch, flagsB bit 7, 0x8007666C..0x80076758
        {
            const uint32_t fa3 = g.U32(e + 560);
            uint32_t fb = g.U32(e + 564);
            if (fa3 & 0x20u) {
                const int32_t y = g.S16(e + 530);
                int32_t thr;
                if (y < 0) thr = 0x23C361;
                else if (y >= 3851) thr = 0x1AD288;
                else {
                    // (y << 12) / 3850 by the compiler's magic multiply (0x800766A8..0x800766CC)
                    const int32_t v = Shl(y, 12);
                    const int32_t hi = static_cast<int32_t>(
                        (static_cast<int64_t>(v) * static_cast<int32_t>(0x882DBF5Fu)) >> 32);
                    const int32_t q = Sub(Sra(Add(hi, v), 11), Sra(v, 31));
                    thr = Add(FixMul(Shl(q, 4), static_cast<int32_t>(0xFFF70F27u)), 0x23C361);
                }
                if (thr < g.S32(e + 576) && !t3) fb |= 0x80u;
                else fb &= ~0x80u;
            } else if (!(fa3 & 0x40u)) {
                fb &= ~0x80u;
            }
            g.W32(e + 564, fb);
        }
        // ---- the brake capacity +0x258, 0x8007675C..0x800767F0
        {
            const int32_t x = g.S32(st + 16);
            const uint32_t d = static_cast<uint32_t>(Add(Sra(x, 1), Sra(Add(x, -2), 31)));
            const uint32_t q = (d == 0u) ? 0xFFFFFFFFu : 0x80000000u / d;   // divu; R3000 by zero
            const int32_t cap = FixMul(g.S32(e + 748), static_cast<int32_t>(q));
            g.W32(e + 600, static_cast<uint32_t>(cap));
            uint32_t a1 = 0;
            if (g.U32(e + 564) & 0x80u) a1 = (g.S32(e + 576) < g.S32(st + 304)) ? 1u : 0u;
            const int32_t adj = Add(static_cast<int32_t>((0u - a1) & 0x3332u), -6553);
            int32_t v = Add(cap, adj);
            g.W32(e + 600, static_cast<uint32_t>(v));
            if (v < 0) v = 0;
            g.W32(e + 600, static_cast<uint32_t>(v));
        }
        node = g.U32(node + 4u);                                      // 0x800767F4
    }
    return !g.Faulted();
}

// ============================================================================ region A
bool BikeStepRegionA(GuestRam& g, int32_t dt, const BikeTables& t, BikeStepCallees& calls) {
    constexpr uint32_t kPoolTable = 0x800CE4D0; // s3: base, +4 stride, +0xC -> the high slot index
    int32_t left = g.S32(g.U32(kPoolTable + 12u));                    // 0x80075F18: s4
    uint32_t e = g.U32(kPoolTable);                                   // s1
    if (left < 0) return !g.Faulted();                               // 0x80075F28
    if (left > 4096) return false;
    for (; left >= 0; --left) {
        if (g.Faulted()) return false;
        // ---- A1, 0x80075F34: the AI's flag hygiene
        if (g.U32(e + 560) & 0x08000000u) {
            const uint32_t fb = g.U32(e + 564);
            g.W32(e + 564, fb & 0xF7FFFFFFu);
            if (fb & 0x200u) g.W32(e + 924, static_cast<uint32_t>(Shl(g.S32(g.U32(e + 556) + 224), 1)));
        }
        // ---- A2, 0x80075F78: the aim-point glide
        if (g.S16(e + 944) != 0) {
            const int32_t s0 = FixMul(dt, g.S32(e + 904));             // 0x80075F90
            int32_t a0 = g.S32(e + 908);
            a0 = (g.S16(e + 944) >= 0) ? Add(a0, s0) : Sub(a0, s0);
            const int32_t w = ClampUnitFrac(a0);                      // 0x80075FB8..0x80075FD8
            g.W32(e + 908, static_cast<uint32_t>(w));
            for (uint32_t k = 0; k < 3; ++k)
                g.W32(e + 880 + 4u * k,
                      static_cast<uint32_t>(Add(FixMul(g.S32(e + 892 + 4u * k), w), g.S32(e + 880 + 4u * k))));
            const int32_t step = Sra(Add(Shl(dt, 8), 0x8000), 16);    // 0x80076054
            const int32_t a1 = g.S16(e + 944);
            const uint16_t u1 = g.U16(e + 944);
            uint16_t store = 0;
            bool doStore = true;
            if (a1 < 0) {
                const int32_t v = Add(a1, step);                      // 0x8007608C
                store = static_cast<uint16_t>(v <= 0 ? v : 0);
            } else if (Sub(a1, step) > 0) {
                store = static_cast<uint16_t>(static_cast<uint32_t>(u1) - static_cast<uint32_t>(step));
            } else {
                // 0x800760B0: at zero from above the top AI command decides
                const int32_t depth = g.S8(e + 946);
                const uint32_t op = g.U16(e + 956u + static_cast<uint32_t>(Shl(Sub(depth, 1), 3)));
                if (op == 3u || (op == 4u && a1 == 1) || (op - 5u) < 13u) {
                    store = 1;
                } else {
                    int32_t v[3];
                    for (uint32_t k = 0; k < 3; ++k) v[k] = g.S32(e + 892 + 4u * k);
                    if (t.sqrt == nullptr) return false;
                    const int32_t len = Length3(v, t.sqrt);           // 0x8002E548 at 0x800760FC
                    if (len < 16) {
                        g.W32(e + 908, 0);
                        g.W16(e + 944, 0);
                        doStore = false;
                    } else {
                        if (len > 0) g.W32(e + 904, FixDiv(0x28000u, static_cast<uint32_t>(len)));
                        else g.W32(e + 904, static_cast<uint32_t>(Neg(static_cast<int32_t>(
                                                   FixDiv(0x28000u, static_cast<uint32_t>(Neg(len)))))));
                        int32_t tv;
                        if (len > 0) tv = Add(Shl(static_cast<int32_t>(FixDiv(static_cast<uint32_t>(len), 0x28000u)), 8), 0x8000);
                        else tv = Sub(0x8000, Shl(static_cast<int32_t>(FixDiv(static_cast<uint32_t>(Neg(len)), 0x28000u)), 8));
                        store = static_cast<uint16_t>(Sra(Neg(tv), 16));
                    }
                }
            }
            if (doStore) g.W16(e + 944, store);                        // 0x80076190
        }
        // ---- A3, 0x80076198: the list migration
        if (g.U32(e + 568) & 0x08001800u) {
            if (!calls.ListMigrate(e, dt)) return false;               // 0x800761B0
        }
        e = e + g.U32(kPoolTable + 4u);                               // 0x800761B8..0x800761C4
    }
    return !g.Faulted();
}

// ============================================================================ region B
bool BikeStepRegionB(int32_t dt, BikeStepCallees& calls) {
    return calls.SteerDriver(kRideListHead, dt) && calls.SteerDriver(kThrownListHead, dt) &&
           calls.SteerPass(kRideListHead, dt) && calls.SteerPass(kThrownListHead, dt);
}

// ============================================================================ region D
namespace {

// The throttle / brake amount slew of D4 (0x800771DC..0x800773F4): `amount` rises by
// FixMul(FixMul(rateUp, cap), dt) while `held`, else falls by the `rateDown` rate, floored at 0 on
// the way down only; an AI's amount is kept from crossing its command `cmd` in either direction.
int32_t SlewAmount(int32_t amount, bool held, bool ai, int32_t cmd, int32_t rateUp, int32_t rateDown,
                   int32_t cap, int32_t dt, bool& storeTwice, int32_t& first) {
    bool a2 = false;
    storeTwice = false;
    if (held) {
        const int32_t a0 = Add(FixMul(FixMul(rateUp, cap), dt), amount);
        if (ai && !(cmd < amount)) a2 = cmd < a0;
        return Add(a0, Neg(a2 ? 1 : 0) & Sub(cmd, a0));
    }
    const int32_t a0 = Sub(amount, FixMul(FixMul(rateDown, cap), dt));
    if (ai && a0 < cmd) a2 = !(amount < cmd);
    int32_t v = Add(a0, Neg(a2 ? 1 : 0) & Sub(cmd, a0));
    storeTwice = true;
    first = v;
    if (v < 0) v = 0;
    return v;
}

// The 16.16 projection of `d` on the s16 heading `h` (x16 into 16.16), three inline FixMul.
int32_t ProjectOnHeading(const int32_t d[3], const int16_t h[3]) {
    const int32_t a = FixMul(d[0], Shl(h[0], 4));
    const int32_t b = FixMul(d[1], Shl(h[1], 4));
    const int32_t c = FixMul(d[2], Shl(h[2], 4));
    return Add(c, Add(b, a));
}

} // namespace

bool BikeStepRegionD(GuestRam& g, int32_t dt, const BikeTables& t, BikeStepCallees& calls) {
    const uint32_t head = kRideListHead;
    uint32_t node = g.U32(head + 4u);                                 // 0x80076810: sp+48
    int guard = 0;
    while (node != head) {                                            // 0x80076818 / 0x800774C0
        if (g.Faulted() || ++guard > 4096) return false;
        const uint32_t e = node - 1088u;                              // s1
        // ---- D0, 0x8007682C: the steering-ramp hold
        {
            const int32_t hold = g.S32(e + 720);
            if (hold != 0) {
                if (hold > 0) {
                    int32_t v = Sub(hold, dt);
                    g.W32(e + 720, static_cast<uint32_t>(v));
                    if (v < 0) v = 0;
                    g.W32(e + 720, static_cast<uint32_t>(v));
                }
                g.W32(e + 596, 0);
                g.W32(e + 588, 0);
                node = g.U32(node + 4u);
                continue;
            }
        }
        // ---- D1, 0x8007686C: the longitudinal command a0
        const uint32_t fa0 = g.U32(e + 560);
        const bool ai = ((fa0 >> 27) & 1u) != 0u;                     // s6
        const bool analog = ((fa0 >> 20) & 1u) != 0u;                 // sp+52
        const uint32_t st = g.U32(e + 556);                           // s7
        int32_t a0;
        if (ai) {
            if (g.U32(e + 564) & 0x80000u) {
                a0 = Neg(g.S32(e + 600));                             // 0x800768B0
            } else {
                const int32_t s0 = g.S32(e + 916);                   // +0x394
                int32_t v1;
                if (s0 <= 0) v1 = 0x7FFF0000;
                else v1 = Add(s0, SplitDiv(Sub(g.S32(e + 920), g.S32(e + 580)),
                                           Shl(Sub(g.S32(e + 600), g.S32(e + 760)), 1)));
                if (0x20000 < v1) {                                  // 0x80076954: chase +0x39C
                    const int32_t pass = g.S32(e + 764);
                    const int32_t x = Sub(SplitDiv(Sub(g.S32(e + 924), g.S32(e + 576)), dt), pass);
                    if (x < 0) {
                        const int32_t v = Add(x, g.S32(e + 752));
                        const int32_t lo = Neg(g.S32(e + 600));
                        a0 = (lo < v) ? v : lo;
                    } else {
                        const int32_t hi = g.S32(st + 188);
                        a0 = (x < hi) ? x : hi;
                    }
                } else if (v1 > 0) {
                    a0 = 0;
                } else {
                    a0 = Neg(g.S32(e + 600));                         // 0x80076A24: latch a full brake
                    g.W32(e + 564, g.U32(e + 564) | 0x80000u);
                }
            }
        } else if (analog) {
            const uint32_t h = g.U16(e + 172);                        // 0x80076A54
            const int32_t axis = g.S32(0x800CE540u + 8u * h);
            const int32_t k = (axis >= 0) ? g.S32(e + 600) : g.S32(e + 592);
            a0 = Neg(FixMul(k, axis));
        } else {
            a0 = 0;
        }
        // ---- D2, 0x80076AA4: the command in the pad's encoding
        int32_t s3 = 0, s4 = 0;
        if (ai) {
            if (a0 < 0) {
                s4 = Neg(a0);
                const int32_t brk = g.S32(e + 596);
                g.W32(e + 588, 0);
                uint32_t f;
                if (s4 < brk) {
                    f = g.U32(e + 560) & ~0x40u;
                } else {
                    const uint32_t v1 = g.U32(e + 560);
                    f = (v1 & 0x40u) ? (v1 | 0x40u) : (v1 | 0x60u);
                }
                g.W32(e + 560, f);
                g.W32(e + 560, g.U32(e + 560) & ~2u);                 // 0x80076AF8..0x80076C14
            } else if (a0 > 0) {
                s3 = a0;
                const uint32_t v1 = g.U32(e + 560);                   // 0x80076B10
                g.W32(e + 596, 0);
                g.W32(e + 560, v1 & ~0x40u);
                if (v1 & 0x10u) {
                    uint8_t stats[0x1C0];
                    g.ReadBlock(st, stats, sizeof(stats));
                    const int32_t surf = g.S8(e + 534);
                    const int32_t c = g.S32(kEnvTable + 0x40u + static_cast<uint32_t>(Shl(surf, 2)));
                    if (t.sqrt == nullptr) return false;
                    const int32_t lim = BikeGripLimit(stats, g.S32(e + 736), g.S32(e + 652), c, t.sqrt);
                    s3 = (s3 < lim) ? s3 : lim;                       // 0x80076B54
                    g.W32(e + 560, g.U32(e + 560) & ~0x10u);
                }
                if (s3 < g.S32(e + 588)) {
                    g.W32(e + 560, g.U32(e + 560) & ~2u);
                } else {
                    const uint32_t f = g.U32(e + 560);
                    g.W32(e + 560, (f & 2u) ? (f | 2u) : (f | 3u));
                }
            } else {
                g.W32(e + 560, g.U32(e + 560) & ~0x42u);             // 0x80076C08
            }
        } else if (analog) {
            if (a0 < 0) {
                s4 = Neg(a0);                                         // 0x80076BCC
                const uint32_t f = g.U32(e + 560);
                g.W32(e + 588, 0);
                g.W32(e + 560, (f | 0x40u) & ~2u);
            } else if (a0 > 0) {
                s3 = a0;                                              // 0x80076BE8
                const uint32_t f = g.U32(e + 560);
                g.W32(e + 596, 0);
                g.W32(e + 560, (f & ~0x40u) | 2u);
            } else {
                g.W32(e + 560, g.U32(e + 560) & ~0x42u);
            }
        }
        // 0x80076C18: throttle and brake held
        const uint32_t fa = g.U32(e + 560);
        const bool s5 = ((fa >> 1) & 1u) != 0u;
        const bool s8 = ((fa >> 6) & 1u) != 0u;
        // ---- D3, 0x80076C34: a human's
        if (!ai) {
            if (g.U32(e + 856) != 0u && g.U32(e + 1088) != 0u)
                if (!calls.PassengerLaunch(e)) return false;          // 0x80076C58
            if (!calls.CrashTimer(e, dt)) return false;               // 0x80076C64
            if (g.U32(0x8005B21Cu) != 0u) {                          // a second player: the tow
                const uint32_t h = g.U16(e + 172);
                const uint32_t o = g.U32(kPlayerBikes + 4u * (h < 1u ? 1u : 0u));
                if (!(g.U32(o + 568) & 0x10000000u)) {
                    // 0x80076CB0: the planar distance, max + 3/8 min in the engine's octagon form
                    const int32_t dx = Sub(g.S32(e + 184), g.S32(o + 184));
                    const int32_t dz = Sub(g.S32(e + 192), g.S32(o + 192));
                    int32_t big = MipsAbs(Sra(dx, 16)), small = MipsAbs(Sra(dz, 16));
                    if (big < small) std::swap(big, small);
                    const int32_t m = Add(small, Sra(small, 1));
                    const int32_t s2 = Add(Add(Sub(Sub(big, Sra(big, 5)), Sra(big, 7)), Sra(m, 2)), Sra(m, 6));
                    const int32_t opt = g.S8(kOptionByte);            // 0x80076D38
                    const int32_t bc = g.S32(kEnvTable + 0xBCu), c0 = g.S32(kEnvTable + 0xC0u);
                    const int32_t lim = Add(c0, Neg(opt) & Sub(bc, c0));
                    if (g.U32(e + 568) & 0x10000000u) {
                        bool clear = false;                           // 0x80076D64: the converse
                        if (lim < s2 || !s5 || s8) clear = true;
                        else if (g.S32(e + 480) < FixMul(g.S32(kEnvTable + 0xB8u), g.S32(st + 224))) clear = true;
                        else if (g.S32(o + 480) < FixMul(g.S32(kEnvTable + 0xB4u), g.S32(g.U32(o + 556) + 224))) clear = true;
                        else if (g.U32(o + 568) & 0x7ECu) clear = true;
                        else if (s2 < 5 && (g.U32(o + 560) & 0x40u)) clear = true;
                        if (clear) g.W32(e + 568, g.U32(e + 568) & 0xEFFFFFFFu);
                    } else {
                        bool ahead = false;                           // 0x80076E1C
                        if (s2 < lim) {
                            int16_t h1[3], h2[3];
                            Read16x3(g, e + 450, h1);
                            Read16x3(g, o + 450, h2);
                            if (0xB333 < DotLcm(h1, h2)) {
                                int32_t d[3];
                                for (uint32_t k = 0; k < 3; ++k) d[k] = Sub(g.S32(o + 504 + 4u * k), g.S32(e + 504 + 4u * k));
                                ahead = 0 < ProjectOnHeading(d, h1);
                            }
                        }
                        if (ahead && s5 && !s8 && !(g.U32(e + 388) & 1u) &&
                            g.U16(0x800CCAC0u + 2u * h) == 0u &&
                            g.S32(e + 480) < Add(g.S32(o + 480), 0x8F0D8) &&
                            FixMul(g.S32(kEnvTable + 0xB8u), g.S32(st + 224)) < g.S32(e + 480) &&
                            FixMul(g.S32(kEnvTable + 0xB4u), g.S32(g.U32(o + 556) + 224)) < g.S32(o + 480))
                            g.W32(e + 568, g.U32(e + 568) | 0x10000000u);    // 0x80076FD4
                    }
                    // 0x80076FE8: both players far enough from the end
                    if (!(0x12BFFF < g.S32(e + 324)) || !(0x12BFFF < g.S32(o + 324)))
                        g.W32(e + 568, g.U32(e + 568) & 0xEFFFFFFFu);
                }
                // 0x80077024: while the tow holds, +0x39C closes the gap along the heading
                if (g.U32(e + 568) & 0x10000000u) {
                    int32_t d[3];
                    int16_t h1[3];
                    for (uint32_t k = 0; k < 3; ++k) d[k] = Sub(g.S32(o + 504 + 4u * k), g.S32(e + 504 + 4u * k));
                    Read16x3(g, e + 450, h1);
                    int32_t s0 = FixMul(g.S32(kEnvTable + 0xC4u), ProjectOnHeading(d, h1));
                    s0 = Sub(s0, FixMul(g.S32(kEnvTable + 0xC8u), Sub(g.S32(e + 576), g.S32(o + 576))));
                    const int32_t nc = Neg(g.S32(e + 600));
                    const int32_t drv = g.S32(e + 592);
                    const int32_t lo = Add(s0, Sra(Sub(s0, nc), 31) & Sub(nc, s0));
                    const int32_t arg = Add(lo, Sra(Sub(drv, s0), 31) & Sub(drv, s0));
                    const int32_t x = Add(g.S32(e + 576), FixMul(arg, dt));
                    g.W32(e + 924, static_cast<uint32_t>(x));
                    const int32_t top = g.S32(g.U32(e + 556) + 224);
                    g.W32(e + 560, g.U32(e + 560) | 0x01000000u);
                    int32_t v1 = static_cast<int32_t>(~static_cast<uint32_t>(Sra(x, 31)) & static_cast<uint32_t>(x));
                    const int32_t over = Sub(top, Add(x, static_cast<int32_t>(0xFFF70F28u)));
                    v1 = Add(v1, Sra(over, 31) & over);
                    g.W32(e + 924, static_cast<uint32_t>(v1));
                }
            }
        }
        // ---- D4, 0x800771C0: the throttle and brake amounts
        if (analog) {
            g.W32(e + 588, static_cast<uint32_t>(s3));
            g.W32(e + 596, static_cast<uint32_t>(s4));
        } else {
            bool twice = false;
            int32_t first = 0;
            const int32_t thr = SlewAmount(g.S32(e + 588), s5, ai, s3, g.S32(st + 340), g.S32(st + 344),
                                           g.S32(e + 592), dt, twice, first);
            if (twice) g.W32(e + 588, static_cast<uint32_t>(first));
            g.W32(e + 588, static_cast<uint32_t>(thr));
            const int32_t brk = SlewAmount(g.S32(e + 596), s8, ai, s4, g.S32(st + 348), g.S32(st + 352),
                                           g.S32(e + 600), dt, twice, first);
            if (twice) g.W32(e + 596, static_cast<uint32_t>(first));
            g.W32(e + 596, static_cast<uint32_t>(brk));
        }
        // 0x800773F8: the friction-ratio seeds, -1.0 while moving and braking / on throttle
        g.W32(e + 696, (g.S32(e + 480) != 0 && s8) ? 0xFFFF0000u : 0u);
        g.W32(e + 700, (g.S32(e + 480) != 0 && s5) ? 0xFFFF0000u : 0u);
        if (!s5 && (!ai || g.S32(e + 588) == 0)) {                    // 0x80077464: coasting
            const int32_t d = Sub(g.S32(e + 756), g.S32(e + 752));
            g.W32(e + 752, g.U32(e + 756));
            g.W32(e + 764, static_cast<uint32_t>(Add(g.S32(e + 764), d)));
        }
        if (Neg(g.S32(e + 752)) < g.S32(e + 596)) {                  // 0x80077484: the brake wins
            g.W32(e + 752, 0);
            g.W32(e + 764, g.U32(e + 760));
        } else {
            g.W32(e + 596, 0);
        }
        node = g.U32(node + 4u);                                      // 0x800774B0
    }
    return !g.Faulted();
}

// ============================================================================ region E
namespace {
// The inline reciprocal: `0x80000000 / ((x >> 1) + ((x - 2) >> 31))` by `divu`,
// arithmetic shifts, a zero divisor giving the R3000's 0xFFFFFFFF.
int32_t InlineRecip(int32_t x) {
    const uint32_t d = static_cast<uint32_t>(Add(Sra(x, 1), Sra(Add(x, -2), 31)));
    return static_cast<int32_t>(d == 0u ? 0xFFFFFFFFu : 0x80000000u / d);
}
} // namespace

bool BikeStepRegionE(GuestRam& g, int32_t dt, const BikeTables& t, BikeStepCallees& calls) {
    if (t.sincos == nullptr || t.atan == nullptr) return false;
    const uint32_t head = kRideListHead;
    uint32_t node = g.U32(head + 4u);                                 // 0x800774D4: sp+56
    int guard = 0;
    while (node != head) {                                            // 0x800774DC / 0x80078470
        if (g.Faulted() || ++guard > 4096) return false;
        const uint32_t e = node - 1088u;                              // s1
        const uint32_t st = g.U32(e + 556);                           // s4
        const uint32_t rider = g.U32(e + 852);                        // sp+60
        // ---- E0, 0x800774F0: drive allowed unless off the road model heading away from the road
        int32_t sp64 = 0;
        if (!(g.U32(e + 388) & 1u)) {
            sp64 = 1;
        } else {
            int16_t h[3], lat[3];
            Read16x3(g, e + 450, h);
            Read16x3(g, g.U32(e + 340) + 2u, lat);
            if ((DotLcm(h, lat) ^ g.S32(e + 344)) < 0) sp64 = 1;
        }
        {
            const int32_t thr = g.S32(e + 588), cap = g.S32(e + 592);   // 0x80077538
            const int32_t brk = g.S32(e + 596), bcap = g.S32(e + 600);
            g.W32(e + 588, static_cast<uint32_t>(thr < cap ? thr : cap));
            g.W32(e + 596, static_cast<uint32_t>(brk < bcap ? brk : bcap));
        }
        const int32_t surf = g.S8(e + 534);
        const int32_t s3 = g.S32(kEnvTable + 0x40u + static_cast<uint32_t>(Shl(surf, 2))); // grip
        if (g.S32(e + 696) != 0 || g.S32(e + 700) != 0) {
            // 0x800775A8: a friction-circle ratio, (L^2 + (amount x stat)^2) / g^2
            int32_t g2 = FixMul(s3, g.S32(e + 736));
            g2 = FixMul(g2, g2);
            const int32_t r = (g2 < 0) ? Neg(InlineRecip(Neg(g2))) : InlineRecip(g2);
            int32_t l2 = FixMul(g.S32(e + 652), g.S32(st + 192));
            l2 = FixMul(l2, l2);
            if (g.S32(e + 700) != 0) {
                int32_t v = FixMul(g.S32(e + 588), g.S32(st + 200));
                v = FixMul(v, v);
                g.W32(e + 700, static_cast<uint32_t>(FixMul(Add(l2, v), r)));
            } else {
                int32_t v = FixMul(g.S32(e + 596), g.S32(st + 196));
                v = FixMul(v, v);
                g.W32(e + 696, static_cast<uint32_t>(FixMul(Add(l2, v), r)));
            }
        }
        // ---- E1, 0x800776AC: the lateral margin s2 and the longitudinal load s5
        const int32_t s7 = MipsAbs(g.S32(e + 652));
        int32_t s2 = Sub(SplitDiv(FixMul(s3, g.S32(e + 736)), g.S32(st + 192)), s7);
        int32_t s5;
        {
            const int32_t pitch = g.S32(e + 616);
            if (!(pitch < 11439)) s5 = 0x10000;
            else if (pitch < -11438) s5 = static_cast<int32_t>(0xFFFF0000u);
            else if (g.S32(e + 576) == 0) s5 = 0;
            else {
                const uint32_t hi = ((static_cast<uint32_t>(MulW(g.S32(e + 636), 163u)) >> 13) & 0x1FFEu) + 1u;
                const int32_t k = FixMul(Shl(t.sincos[hi], 4), g.S32(st + 16));  // the steer's cosine
                const int32_t t0 = FixMul(Add(Sub(g.S32(e + 588), g.S32(e + 596)), g.S32(e + 752)), k);
                s5 = t0;
                if (s5 > 0) {
                    const int32_t den = FixMul(g.S32(st + 8), g.S32(e + 736));
                    s5 = den > 0 ? static_cast<int32_t>(FixDiv(static_cast<uint32_t>(t0), static_cast<uint32_t>(den)))
                                 : Neg(static_cast<int32_t>(FixDiv(static_cast<uint32_t>(t0), static_cast<uint32_t>(Neg(den)))));
                    if (!(0xFFFF < s5)) g.W32(e + 564, g.U32(e + 564) & ~0x100u);   // 0x800778A0
                } else if (s5 < 0) {
                    const int32_t den = g.S32(e + 748);
                    s5 = den > 0 ? Neg(static_cast<int32_t>(FixDiv(static_cast<uint32_t>(Neg(s5)), static_cast<uint32_t>(den))))
                                 : static_cast<int32_t>(FixDiv(static_cast<uint32_t>(Neg(s5)), static_cast<uint32_t>(Neg(den))));
                }
            }
        }
        // 0x800778E0: the drive s6, the brake sp+68, and the small-steer flag s0
        const bool driveOn = g.S32(e + 616) >= 0 || (g.U32(e + 564) & 0x200000u) != 0u;
        int32_t sp72 = 0;
        int32_t sp68 = g.S32(e + 596);
        int32_t s6 = Neg(driveOn ? 1 : 0) & g.S32(e + 588);
        const bool s0 = MipsAbs(g.S32(e + 636)) < 22876;
        int32_t s8 = 0;
        bool toE3 = false;
        if (s0 && !(s2 > 0)) {
            // ---- the GRIP-LOSS FALL, 0x80077944
            const int32_t ang = RatAtan2(Neg(g.S32(e + 732)), g.S32(e + 740), t.atan);
            int32_t s4 = Sra(MulW(ang, 25736u), 8);
            {
                const int32_t d = Sub(s4, g.S32(e + 676));
                if (0x3243F < d) s4 = Add(s4, static_cast<int32_t>(0xFFF9B782u));      // -2 pi
                else if (d < static_cast<int32_t>(0xFFFCDBC1u)) s4 = Add(s4, 0x6487E); // +2 pi
            }
            const bool up = !(s4 < g.S32(e + 676));                   // s7 here
            int32_t tgt = Add(s4, up ? static_cast<int32_t>(0xFFFE9A8Fu) : 0x16571);   // -+80 degrees
            const int32_t speed = g.S32(e + 576);
            if (0x50000 < speed) {                                    // 0x800779FC: past 5.0
                const int32_t over = Sub(speed, 0x50000);
                tgt = SplitDiv(Add(FixMul(tgt, over), s4), Add(over, 1));
            }
            g.W32(e + 708, static_cast<uint32_t>(MipsAbs(Sub(s4, tgt))));  // +0x2C4
            g.W32(e + 688, static_cast<uint32_t>(tgt));                     // +0x2B0
            g.W32(e + 684, 0);
            const int32_t lean = g.S32(e + 676);
            if ((up && lean < tgt) || (!up && tgt < lean)) {
                const int32_t a0 = FixMul(Sub(0x9D087, g.S32(e + 736)), 0x10000);  // 0x80077AEC
                g.W32(e + 680, static_cast<uint32_t>(up ? a0 : Neg(a0)));
            } else {
                g.W32(e + 680, 0);
            }
            if (g.S32(e + 576) != 0 && 0x16571 < MipsAbs(s4))          // 0x80077B28
                g.W32(e + 744, static_cast<uint32_t>(FixMul(Sub(0x9D087, g.S32(e + 736)), 0x8000)));
            else
                g.W32(e + 744, 0);
            {
                const int32_t a1 = g.S32(e + 744);                    // 0x80077B8C
                const int32_t mask = (MipsAbs(g.S32(e + 488)) < a1) ? -1 : 0;
                g.W32(e + 744, static_cast<uint32_t>(mask & (g.S32(e + 688) >= 0 ? Neg(a1) : a1)));
            }
            g.W32(e + 700, 0x10000u);
            g.W32(e + 696, 0x10000u);
            g.W32(e + 564, (g.U32(e + 564) & ~0x41Cu) | 0x400u);      // 0x80077BF4 -> 0x8007816C
        } else if (g.U32(e + 564) & 0x400u) {
            // ---- E2, latched: continue, or settle
            g.W32(e + 744, 0);                                        // 0x80077C1C
            bool settle = true;
            if (g.S32(e + 680) != 0 && g.S32(e + 576) != 0 && !(MipsAbs(g.S32(e + 676)) < g.S32(e + 708)))
                settle = false;                                       // 0x80077C48: on to E4
            if (settle) {
                const int32_t v = FixMul(s2, 0xA0000);                // 0x80077C54
                const uint16_t f0 = g.U16(e + 528), f1 = g.U16(e + 530), f2 = g.U16(e + 532);
                const uint16_t r0 = g.U16(e + 516), r1 = g.U16(e + 518), r2 = g.U16(e + 520);
                g.W32(e + 680, 0);
                g.W32(e + 676, 0);
                g.W32(e + 700, 0);
                g.W32(e + 696, 0);
                g.W16(e + 450, f0);
                g.W16(e + 452, f1);
                g.W16(e + 454, f2);
                g.W16(e + 814, r0);
                g.W16(e + 816, r1);
                g.W16(e + 818, r2);
                g.W32(e + 764, static_cast<uint32_t>(Neg(v)));
            }
        } else {
            // ---- E2, not latched: wheelspin, front lock, or neither
            g.W32(e + 744, 0);                                        // 0x80077CC8
            if (g.S32(e + 588) > 0 && g.S32(e + 596) == 0) {
                if (0x10000 < g.S32(e + 700)) {
                    const uint32_t v1 = g.U32(e + 564);               // 0x80077CF0
                    const uint32_t v0 = v1 & ~4u;
                    g.W32(e + 564, v0);
                    if (!(v1 & 8u)) {                                 // the edge
                        g.W32(e + 564, v0 | 0x10u);
                        int32_t base[3], out[3];
                        int16_t dir[3];
                        Read32x3(g, e + 504, base);
                        Read16x3(g, e + 528, dir);
                        MulAdd(base, dir, g.S32(e + 308), out);       // 0x8002EAD8 at 0x80077D1C
                        Write32x3(g, e + 784, out);
                    }
                    g.W32(e + 564, g.U32(e + 564) | 8u);
                    s6 = SplitDiv(g.S32(e + 588), g.S32(e + 700));    // 0x80077D30..0x80077D84
                    int32_t m = 0;
                    if (g.S32(e + 764) < Neg(s6) && sp64 != 0) m = s0 ? -1 : 0;
                    s6 = Add(s6, m & Sub(Sub(0x8000, s6), g.S32(e + 764)));
                } else {
                    g.W32(e + 564, g.U32(e + 564) & ~0xCu);           // 0x80077E88
                }
            } else if (0x10000 < g.S32(e + 696)) {
                const uint32_t v1 = g.U32(e + 564);                   // 0x80077DE4: front lock
                const uint32_t v0 = v1 & ~8u;
                g.W32(e + 564, v0);
                g.W32(e + 564, (v1 & 4u) ? (v0 | 4u) : (v0 | 0x14u));
                if (!(g.U32(e + 560) & 0x08000000u))
                    sp68 = SplitDiv(g.S32(e + 596), g.S32(e + 696)); // 0x80077E20
            } else {
                g.W32(e + 564, g.U32(e + 564) & ~0xCu);
            }
            toE3 = true;
        }
        // ---- E3, 0x80077E98: the pitch moves
        if (toE3) {
            const uint32_t fa = g.U32(e + 560);
            const int32_t pitch = g.S32(e + 616);
            const bool a2 = ((fa >> 11) & 1u) != 0u;
            bool positive = false, negTest = false;
            if (pitch >= 0) {
                if (0xFFFF < s5 && pitch == 0 && !(g.U32(e + 564) & 0x920u)) positive = true;
                else if (a2 && (g.U32(e + 560) & 0x1000u) && 0x23C36 < g.S32(e + 576)) positive = true;
                else if (!(g.U32(e + 564) & 0x20u)) negTest = true;              // -> 0x80078078
                else {
                    const uint32_t f = g.U32(e + 560);
                    if (!(f & 1u)) negTest = true;                                // -> 0x80078074
                    else if (f & 0x08000000u) negTest = true;                     // -> 0x80078078
                    else positive = true;
                }
            } else {
                negTest = true;
            }
            if (positive) {
                // 0x80077F40: the target s8
                s8 = a2 ? g.S32(st + 268) : g.S32(st + 264);
                const int32_t y = g.S16(e + 530);
                if (!(y < 3548)) s8 = 0;
                else if (!(y < -2047)) s8 = FixMul(s8, FixMul(Sub(0xDDB2, Shl(y, 4)), 0xBB68));
                if (s8 != 0) {
                    sp72 = 1;                                          // 0x80077FC4
                    g.W32(e + 564, g.U32(e + 564) | 0x100u);
                    if (!a2) s6 = SplitDiv(s6, s5);
                    sp68 = 0;
                    g.W32(e + 564, (g.U32(e + 564) & ~0x800u) | 0x20u);
                }
                if (g.S32(e + 616) == 0 && s8 > 0)                    // 0x8007803C
                    if (!calls.StanceEvent(22, rider, 8)) return false;
                g.W32(e + 560, g.U32(e + 560) & ~0x1001u);             // 0x80078060
            } else if (negTest) {
                // 0x80078078: a negative move
                if (!(static_cast<int32_t>(0xFFFF0000u) < s5) && g.S32(e + 616) == 0 &&
                    !(g.U32(e + 564) & 0x40u) && s7 < 17157 && g.S32(e + 576) < g.S32(st + 304)) {
                    const int32_t y = g.S16(e + 530);
                    if (!(y < 3548)) s8 = Neg(g.S32(st + 272));
                    else if (y < -2047) s8 = 0;
                    else {
                        const int32_t k = FixMul(Sub(0xDDB2, Shl(y, 4)), 0xBB68);
                        s8 = Sub(Neg(FixMul(Sub(3431, g.S32(st + 272)), k)), g.S32(st + 272));
                    }
                    if (s8 != 0) {
                        sp72 = 1;                                      // 0x80078158
                        s6 = 0;
                        g.W32(e + 564, g.U32(e + 564) | 0x40u);
                    }
                }
            }
        }
        // ---- E4, 0x80078180: the load, the net acceleration, the speed
        {
            const int32_t lo = Add(s5, Sra(Add(s5, 0x10000), 31) & Sub(static_cast<int32_t>(0xFFFF0000u), s5));
            const int32_t over = Sub(0x10000, s5);
            g.W32(e + 608, static_cast<uint32_t>(Add(lo, Sra(over, 31) & over)));   // +0x260
            const bool keep = g.S32(e + 576) > 0 || s6 > 0 || (g.U32(e + 564) & 0x400u) != 0u;
            const int32_t acc = Neg(keep ? 1 : 0) & Add(Sub(s6, sp68), g.S32(e + 764));
            g.W32(e + 484, static_cast<uint32_t>(acc));               // +0x1E4
            if ((g.U32(e + 560) & 0x42u) == 0x42u && static_cast<int32_t>(0xFFF90000u) < acc)
                g.W32(e + 484, 0xFFF90000u);                           // both held: -7.0
            const uint32_t fb = g.U32(e + 564);
            const int32_t speed0 = g.S32(e + 576);                    // s3
            const bool latched = ((fb >> 10) & 1u) != 0u;             // s2
            const bool stopBit = ((fb >> 12) & 1u) != 0u;             // s0
            if (g.U32(e + 560) & 0x01000000u) {
                g.W32(e + 576, g.U32(e + 924));                       // 0x80078234: the tow
                g.W32(e + 560, g.U32(e + 560) & 0xFEFFFFFFu);
            } else {
                int32_t v = Add(FixMul(g.S32(e + 484), dt), speed0);  // 0x80078254
                g.W32(e + 576, static_cast<uint32_t>(v));
                if (v < 0) v = 0;
                g.W32(e + 576, static_cast<uint32_t>(v));
                if (v == 0 || stopBit) {
                    const uint32_t fa = g.U32(e + 560);               // 0x8007829C
                    if ((fa & 0x300u) && !(fa & 0x40u)) {
                        g.W32(e + 576, static_cast<uint32_t>(speed0));
                    } else if (stopBit && !latched) {
                        g.W32(e + 484, 0);
                        g.W32(e + 576, 0);
                        g.W8(e + 849, 0);                             // the gear
                    }
                }
            }
            g.W32(e + 564, g.U32(e + 564) & ~0x3000u);                // 0x800782E0
            if (g.U32(e + 388) & 1u) {
                const int32_t vy = g.S16(e + 452);                    // 0x800782F8: downhill
                if (vy < -2048) {
                    const int32_t k = Add(FixMul(0x3C267, Shl(vy, 4)), 0x2E133);
                    const int32_t top = Sra(g.S32(g.U32(e + 556) + 224), 1);
                    g.W32(e + 576, static_cast<uint32_t>(ClampLerpMin(g.S32(e + 576), top, k)));
                }
            }
            uint32_t stopped = 0;                                     // 0x8007833C
            if (!latched && !stopBit && speed0 > 0) stopped = (g.U32(e + 576) == 0u) ? 1u : 0u;
            const uint32_t nfb = g.U32(e + 564) | (stopped << 13);
            g.W32(e + 564, nfb);
            if (!(nfb & 0x200000u) && sp72 != 0) {
                // 0x80078390: the pitch move's second-order set-up toward s8
                const int32_t a1 = Sub(g.S32(e + 616), s8);
                if ((a1 >= 0 && a1 < 656) || (a1 < 0 && !(a1 < -655))) {
                    g.W32(e + 616, static_cast<uint32_t>(s8));        // 0x80078454
                    g.W32(e + 624, 0);
                    g.W32(e + 620, 0);
                } else {
                    g.W32(e + 624, static_cast<uint32_t>(SplitDiv(0x20000, a1)));
                    const int32_t p = g.S32(e + 616);
                    g.W32(e + 632, 0);
                    g.W32(e + 620, a1 < 0 ? 0x20000u : 0xFFFE0000u);
                    g.W32(e + 628, static_cast<uint32_t>(p));
                    g.W32(e + 616, static_cast<uint32_t>(p != 0 ? p : (a1 < 0 ? 16 : -16)));
                }
            }
        }
        node = g.U32(node + 4u);                                      // 0x80078460
    }
    return !g.Faulted();
}

// ============================================================================ region F
bool BikeStepRegionF(GuestRam& g, int32_t dt) {
    const uint32_t head = kRideListHead;                              // s5
    uint32_t node = g.U32(head + 4u);                                 // 0x80078480: s4
    int guard = 0;
    while (node != head) {                                            // 0x80078488 / 0x80078AA0
        if (g.Faulted() || ++guard > 4096) return false;
        const uint32_t e = node - 1088u;                              // s0
        const uint32_t st = g.U32(e + 556);                           // s2
        bool latchedOrStopped = false;
        if (g.S32(e + 576) <= 0) latchedOrStopped = true;             // 0x8007849C
        else if (g.U32(e + 564) & 0x400u) latchedOrStopped = true;    // 0x800784B0
        if (latchedOrStopped) {
            g.W32(e + 564, g.U32(e + 564) & 0xFFF7FFFFu);             // 0x800788C4..0x800788D4
        } else {
            // ---- 0x800784B4: the lean's response
            bool spin = false;
            if (g.U32(e + 564) & 0x8u) {
                const int32_t lean = g.S32(e + 676);                  // a0
                if (lean >= 0) {
                    if (g.S32(st + 244) < g.S32(e + 636)) spin = true;        // 0x800784DC
                    else if (lean > 0) spin = false;                         // 0x800784E8
                    else spin = g.S32(e + 636) < Neg(g.S32(st + 244));        // 0x800784F0
                } else {
                    spin = g.S32(e + 636) < Neg(g.S32(st + 244));
                }
            }
            if (spin) {
                // 0x80078508: the damped second-order response, limits halved with a passenger
                // while +0x28C > 0
                int32_t s1, s3;
                if (g.U32(e + 856) != 0u && g.S32(e + 652) > 0) {
                    s1 = Div2(g.S32(st + 212));
                    s3 = Div2(g.S32(st + 204));
                } else {
                    s1 = g.S32(st + 212);
                    s3 = g.S32(st + 204);
                }
                if (g.U32(e + 564) & 0x10u) {                         // 0x80078554: the skid edge
                    const int32_t k = FixMul(s1, FixMul(g.S32(e + 576), g.S32(e + 652)));
                    const int32_t lean = g.S32(e + 676);
                    g.W32(e + 684, static_cast<uint32_t>(k));
                    g.W32(e + 680, 0);
                    g.W32(e + 676, static_cast<uint32_t>((lean ^ k) >= 0 ? lean : 0));
                }
                // 0x800785A4: reverse the rate when past half the limit and still moving outward
                const int32_t half = Sra(s3, 1);
                const int32_t v1 = g.S32(e + 676);
                bool m;
                if (!(Neg(half) < v1) && g.S32(e + 684) < 0) m = true;
                else if (v1 < half) m = false;
                else m = g.S32(e + 684) > 0;
                int32_t rate = g.S32(e + 684);
                if (m) rate = Add(rate, Sub(Neg(rate), rate));
                g.W32(e + 684, static_cast<uint32_t>(rate));
                const int32_t vel = Add(FixMul(rate, dt), g.S32(e + 680));
                const int32_t a1 = g.S32(e + 676);
                g.W32(e + 680, static_cast<uint32_t>(vel));           // 0x80078638
                bool clampIt = false;
                if (a1 > 0) clampIt = vel < 0;                        // 0x8007863C
                else if (a1 < 0) clampIt = vel > 0;                   // 0x80078644..0x8007864C
                if (clampIt) {
                    const int32_t l = g.S32(e + 676);                 // 0x80078654
                    g.W32(e + 680, 0);
                    g.W32(e + 676, static_cast<uint32_t>(l > 0 ? s3 : Neg(s3)));
                }
                g.W32(e + 676, static_cast<uint32_t>(Add(FixMul(g.S32(e + 680), dt), g.S32(e + 676))));
            } else {
                // 0x800786A4: decay at 5.0/s, snap to 0 under 655
                const int32_t lean = g.S32(e + 676);
                const int32_t step = FixMul(FixMul(lean, 0x50000), dt);
                g.W32(e + 684, 0);
                const int32_t nl = Sub(lean, step);
                g.W32(e + 680, 0);
                g.W32(e + 676, static_cast<uint32_t>(MipsAbs(nl) < 655 ? 0 : nl));
                // 0x80078718: a human with only the front locked
                const uint32_t fa = g.U32(e + 560);
                const uint32_t fb = g.U32(e + 564);
                if (!(fa & 0x08000000u) && (fb & 0xCu) == 4u) {
                    bool countDown = false;
                    if (!(fb & 0x10u) && (fa & 0x40u) && (fa & 0x300u) && !(fb & 0x2u)) countDown = true;
                    if (countDown) {
                        g.W32(e + 716, static_cast<uint32_t>(Sub(g.S32(e + 716), dt)));     // 0x800787EC
                    } else {
                        const int32_t sp = g.S32(e + 576);            // 0x80078760
                        const int32_t lo = g.S32(st + 356);
                        if (sp < lo) g.W32(e + 716, g.U32(st + 400));
                        else if (g.S32(st + 360) < sp) g.W32(e + 716, g.U32(st + 404));
                        else g.W32(e + 716, static_cast<uint32_t>(
                                                Add(g.S32(st + 400), FixMul(Sub(sp, lo), g.S32(st + 408)))));
                        if (g.U32(e + 388) & 1u)                     // 0x800787C0: off the road model
                            g.W32(e + 716, static_cast<uint32_t>(FixMul(g.S32(st + 412), g.S32(e + 716))));
                    }
                }
            }
            // ---- 0x80078800: the lateral force +0x2E8 = +0x1E8 x the gain ramped over stats[+0xDC]
            const int32_t al = MipsAbs(g.S32(e + 676));
            const int32_t full = g.S32(st + 220);
            int32_t gain;
            if (al < full) gain = FixMul(g.S32(st + 216), SplitDiv(al, full));
            else gain = g.S32(st + 216);
            g.W32(e + 744, static_cast<uint32_t>(FixMul(g.S32(e + 488), gain)));
        }
        // ---- 0x800788D8: every bike
        uint32_t fb = g.U32(e + 564) & 0xFFFFFFEFu;
        g.W32(e + 564, fb);
        if (fb & 0x01000000u) {                                       // the steering ramp is armed
            const int32_t dur = g.S32(e + 720);
            if (dur < 65) {
                g.W32(e + 656, 0);
                g.W32(e + 660, 0);
                g.W32(e + 720, 0);
            } else {
                const int32_t rate = Neg(SplitDiv(g.S32(e + 656), dur));     // 0x80078968: negated
                const int32_t r290 = g.S32(e + 656);
                const int32_t r2d0 = g.S32(e + 720);
                const int32_t steer = g.S32(e + 636);
                g.W32(e + 660, static_cast<uint32_t>(rate));
                g.W32(e + 724, 0);
                g.W32(e + 664, static_cast<uint32_t>(steer));
                const int32_t half = Sra(FixMul(r290, r2d0), 1);
                g.W32(e + 644, static_cast<uint32_t>(Add(g.S32(e + 636), half)));
            }
            g.W32(e + 564, g.U32(e + 564) & 0xFEFFFFFFu);                    // 0x80078998
        }
        if (g.S32(e + 660) != 0) {                                    // 0x800789AC: the ramp runs
            const int32_t r294 = g.S32(e + 660);
            bool end = (g.S32(e + 656) ^ r294) >= 0;                  // 0x800789C4
            if (!end) end = !(MipsAbs(g.S32(e + 636)) < g.S32(st + 228));
            if (end) {
                const int32_t to = g.S32(e + 644);                   // 0x800789F0
                g.W32(e + 720, 0);
                g.W32(e + 660, 0);
                g.W32(e + 656, 0);
                g.W32(e + 636, static_cast<uint32_t>(to));
            } else {
                const int32_t d = FixMul(r294, dt);                  // 0x80078A0C
                const int32_t t = Add(g.S32(e + 724), dt);
                g.W32(e + 656, static_cast<uint32_t>(Add(g.S32(e + 656), d)));
                g.W32(e + 724, static_cast<uint32_t>(t));
                const int32_t h = Div2(FixMul(g.S32(e + 660), t));    // 0x80078A30
                const int32_t v = FixMul(g.S32(e + 724), Sub(g.S32(e + 656), h));
                g.W32(e + 636, static_cast<uint32_t>(Add(g.S32(e + 664), v)));
            }
            // 0x80078A64: +0x27C into [-stats[+0xE4], +stats[+0xE4]], branch-free
            const int32_t lim = g.S32(st + 228);
            const int32_t a0 = g.S32(e + 636);
            const int32_t nlim = Neg(lim);
            int32_t v1 = Add(a0, Sra(Sub(a0, nlim), 31) & Sub(nlim, a0));
            const int32_t over = Sub(lim, a0);
            v1 = Add(v1, Sra(over, 31) & over);
            g.W32(e + 636, static_cast<uint32_t>(v1));
        }
        node = g.U32(node + 4u);                                      // 0x80078A98
    }
    return !g.Faulted();
}

// ============================================================================ the regions' callees
// RASHCDG cfe43a77..., SLUS 67ed165a...

void BikeRideArmReset(GuestRam& g, uint32_t e) {
    const uint32_t fa = g.U32(e + 560);                               // 0x8007F094
    const uint32_t pas = g.U32(e + 856);                              // 0x8007F098
    g.W32(e + 560, fa & 0xFF7FFFFFu);                                 // 0x8007F0A4
    if (pas != 0u) g.W32(pas + 488, g.U32(e + 488));                  // 0x8007F0A8..0x8007F0B0
}

void RotateRowPair(GuestRam& g, uint32_t a, uint32_t b, int32_t ang, const int16_t* sincos) {
    const uint32_t idx = static_cast<uint32_t>(ang) & 0xFFFu;         // 0x8002EDB0
    const int32_t c16 = Shl(sincos[2u * idx + 1u], 4);                // 0x8002EDD8 lh +2, sll 4: s1
    const int32_t s16 = Shl(sincos[2u * idx], 4);                     // 0x8002EDDC lh +0, sll 4: s0
    int16_t va[3], vb[3], tmp[3], nb[3];
    Read16x3(g, a, va);
    Read16x3(g, b, vb);
    Blend16(va, vb, tmp, c16, Neg(s16));                              // 0x8002EDF0: into sp+24
    Read16x3(g, a, va);                                               // the second Blend16 reads
    Read16x3(g, b, vb);                                               // its inputs afresh
    Blend16(va, vb, nb, s16, c16);                                    // 0x8002EE08: b in place
    Write16x3(g, b, nb);
    Write16x3(g, a, tmp);                                             // 0x8002EE10..0x8002EE30
}

bool BikeRecoverEnd(GuestRam& g, uint32_t e, uint32_t sp, const BikeTables& t,
                    BikeRiderLayerCallees& calls) {
    const uint32_t csp = sp - 32u;                                    // `addiu sp,sp,-32` at 0x80072994
    const uint32_t fb = g.U32(e + 564);                               // 0x800729B0
    const uint32_t owner = g.U32(e + 852);                            // 0x800729B4
    g.W32(e + 932, 0);                                                // 0x800729BC
    g.W32(e + 564, fb & 0xC0018200u);                                 // 0x800729C4
    // One rider's arm, 0x800729EC..0x80072A60 (and its twin 0x80072A6C..0x80072AE0).
    auto riderArm = [&](uint32_t r) -> bool {
        bool knocked = false;                                         // s2
        if (g.U32(r + 552) & 0x8000u) {
            knocked = true;
            if (!calls.RiderKnockOff(r, csp)) return false;           // 0x80072A00 / 0x80072A80
        }
        const uint32_t st = g.U16(r + 544);
        if (st == 89u || st == 39u || st == 88u || st == 40u || st == 38u) {
            if (knocked) g.W32(r + 552, g.U32(r + 552) | 0x10000u);   // 0x80072A58 / 0x80072AD8
            if (!calls.RiderLaunch(r, csp)) return false;             // 0x80072A5C / 0x80072ADC
        }
        return true;
    };
    if (g.U8(owner + 572) & 0x10u) {                                  // 0x800729C8
        const uint32_t pr = g.U32(g.U32(e + 856) + 852);              // 0x800729DC..0x800729E4
        if (!riderArm(pr)) return false;
    }
    const uint32_t s1 = g.U32(e + 852);                               // 0x80072A64: the own rider
    if (!riderArm(s1)) return false;
    const uint32_t fc = g.U32(e + 568) & 0xFFFC7F1Fu;                 // 0x80072AE8..0x80072AF4
    g.W32(e + 724, 0);                                                // 0x80072AF0
    g.W32(e + 568, fc);                                               // 0x80072B00
    if (fc & 0x10u) {
        const int32_t lean = g.S32(e + 676);                          // 0x80072B04
        g.W32(e + 688, 0);                                            // 0x80072B0C
        g.W32(e + 680, 0);                                            // 0x80072B14
        const uint32_t a1 = (lean > 0) ? 0xFFFD0000u : 0x30000u;      // -+3.0
        const uint32_t lean2 = g.U32(e + 676);                        // 0x80072B1C
        const int32_t steer = g.S32(e + 636);                         // 0x80072B20
        g.W32(e + 684, a1);                                           // 0x80072B28
        g.W32(e + 644, 0);                                            // 0x80072B2C
        g.W32(e + 688, lean2);                                        // 0x80072B34
        const uint32_t a0 = (steer > 0) ? 0xFFFF0000u : 0x10000u;     // -+1.0
        uint32_t v1 = g.U32(e + 568);                                 // 0x80072B3C
        g.W32(e + 640, a0);                                           // 0x80072B4C
        if (v1 & 0x600u) v1 &= 0xFFFFFFEFu;                           // 0x80072B54
        g.W32(e + 568, v1);                                           // 0x80072B5C
    } else if (fc & 0xFu) {
        if ((fc & 2u) && !(g.U32(s1 + 604) < 2u))                     // 0x80072B6C..0x80072B80
            g.W32(e + 568, (fc & 0xFFFFFFFDu) | 4u);                  // 0x80072B90
        if (g.U32(e + 568) & 0x600u) {                                // 0x80072B94
            g.W32(e + 744, g.U32(e + 488));                           // 0x80072BA8..0x80072BB0
        } else {
            MulAddView(g, e + 504, e + 528, g.S32(e + 308), e + 784); // 0x80072BC0
            const int32_t a1 = g.S32(e + 552);                        // 0x80072BC8
            if (a1 != 0) {
                const int32_t a0 = g.S32(e + 488);                    // 0x80072BD8
                if (a0 != 0) {
                    const int32_t v = FixMul(a0, a1);                 // 0x80072BE8
                    // 0x80072BF8..0x80072C1C: 652 * v by shift-and-add (wrapping), sra 16, negu
                    const int32_t ang = Neg(Sra(MulW(v, 652u), 16));
                    if (t.sincos == nullptr) return false;
                    RotateRowPair(g, e + 528, e + 516, ang, t.sincos); // 0x80072C18
                }
            }
            if ((g.U32(e + 568) & 0xCu) == 0u) g.W32(e + 724, g.U32(e + 552));   // 0x80072C20..0x80072C3C
        }
        g.W32(e + 656, 0);                                            // 0x80072C40..0x80072C50
        g.W32(e + 660, 0);
        g.W32(e + 680, 0);
        g.W32(e + 684, 0);
        g.W32(e + 584, 0);
    }
    g.W32(e + 568, g.U32(e + 568) & 0xFFFFEFFFu);                     // 0x80072C54..0x80072C60
    return !g.Faulted();
}

bool BikeListMigrate(GuestRam& g, uint32_t e, uint32_t sp, const BikeTables& t,
                     BikeListMigrateCallees& calls) {
    const uint32_t fsp = sp - 40u;                                    // `addiu sp,sp,-40` at 0x80071BCC
    uint32_t head;
    if (g.S16(e + 320) == 0) {                                        // 0x80071BE0
        head = 0x8005B270u;
    } else {
        uint32_t fc = g.U32(e + 568);                                 // 0x80071BFC: v1
        if (fc & 0x600u) {
            if (fc & 0x800u) {
                if (!calls.CrashLaunch(e)) return false;              // 0x80071C18
                fc = g.U32(e + 568);                                  // 0x80071C20
            }
            if ((fc & 0x1FFu) && (fc & 0x1000u))                      // 0x80071C2C / 0x80071C34
                if (!BikeRecoverEnd(g, e, fsp, t, calls)) return false; // 0x80071C3C
            head = (g.U32(e + 568) & 0x400u) ? 0x8005B2D8u : 0x8005B378u;   // 0x80071C44..0x80071C68
        } else if (fc & 0x1FFu) {                                     // 0x80071C6C
            if (fc & 0x800u)
                if (!calls.WipeoutStart(e)) return false;             // 0x80071C80
            if (g.U32(e + 568) & 0x1000u)                             // 0x80071C88
                if (!BikeRecoverEnd(g, e, fsp, t, calls)) return false; // 0x80071C9C
            head = 0x8005B350u;
        } else {
            BikeRideArmReset(g, e);                                   // 0x80071CB0
            head = 0x8005B298u;
        }
    }
    if (g.Faulted()) return false;
    // 0x80071CC0..0x80071CDC: unlink; every word loaded afresh
    {
        const uint32_t prev = g.U32(e + 1088), next = g.U32(e + 1092);
        g.W32(prev + 4u, next);
    }
    {
        const uint32_t next = g.U32(e + 1092), prev = g.U32(e + 1088);
        g.W32(next, prev);
    }
    // 0x80071CE0..0x80071D0C: insert right after the head
    const uint32_t n = e + 1088u;
    g.W32(g.U32(head + 4u), n);
    g.W32(e + 1092, g.U32(head + 4u));
    g.W32(head + 4u, n);
    const uint32_t fc = g.U32(e + 568);
    g.W32(e + 1088, head);
    g.W32(e + 568, fc & 0xF7FFFFFFu);
    return !g.Faulted();
}

bool BikePassengerLaunch(GuestRam& g, uint32_t e, uint32_t sp, BikeRiderLayerCallees& calls) {
    const uint32_t csp = sp - 32u;                                    // `addiu sp,sp,-32` at 0x80074D58
    const uint32_t fa = g.U32(e + 560);                               // 0x80074D6C
    const uint32_t p = g.U32(e + 856);                                // 0x80074D70: s0, the passenger
    if (!(fa & 0x400u)) return !g.Faulted();                          // 0x80074D78
    if (!(g.U32(g.U32(p + 852) + 604) < 2u)) return !g.Faulted();     // 0x80074D80..0x80074D94
    {
        const uint32_t fc = g.U32(p + 568);                           // 0x80074D9C
        const uint32_t r = g.U32(p + 852);                            // 0x80074DA0, before the store
        g.W32(p + 568, fc | 8u);                                      // 0x80074DA8
        g.W16(r + 456, g.U16(e + 450));                               // 0x80074DB4
    }
    g.W16(g.U32(p + 852) + 458, g.U16(e + 452));                      // 0x80074DC4
    g.W16(g.U32(p + 852) + 460, g.U16(e + 454));                      // 0x80074DD4
    {
        const uint32_t r = g.U32(p + 852);                            // 0x80074DD8
        g.W32(r + 552, g.U32(r + 552) | 0x200000u);                   // 0x80074DEC
    }
    {
        const uint32_t v = g.U32(e + 480);                            // 0x80074DF0
        g.W32(g.U32(p + 852) + 480, v + 0xD6944u);                    // 0x80074DFC
    }
    if (g.Faulted()) return false;
    if (!calls.RiderKnockOff(g.U32(p + 852), csp)) return false;      // 0x80074E04
    g.W32(p + 568, g.U32(p + 568) & 0xFFFFFFF7u);                     // 0x80074E0C..0x80074E18
    if (g.S32(e + 616) == 0) {                                        // 0x80074E1C
        const uint32_t v1 = g.U32(e + 616);                           // 0x80074E2C
        g.W32(e + 624, 0x2DD62Du);
        g.W32(e + 632, 0);
        g.W32(e + 620, 0xFFF80000u);
        g.W32(e + 628, v1);
    }
    g.W32(e + 560, g.U32(e + 560) & 0xFFFFFBFFu);                     // 0x80074E48..0x80074E54
    return !g.Faulted();
}

// ============================================================================ regions G, H, I, J
namespace {
constexpr uint32_t kPoolTableG = 0x800CE4D0; // pool 0: base, +4 stride, +0xC -> the high slot index
} // namespace

bool BikeStepRegionG(GuestRam& g, int32_t dt, BikeStepTailCallees& calls) {
    const uint32_t hp = g.U32(kPoolTableG + 12u);                     // 0x80078AB4
    g.W32(kRoadListHead, kRoadListFirst);                             // 0x80078AC0
    int32_t left = g.S32(hp);                                         // 0x80078AC4: s1
    uint32_t e = g.U32(kPoolTableG);                                  // 0x80078AC8: s0
    if (left < 0) return !g.Faulted();                                // 0x80078ACC
    if (left > 4096) return false;
    for (; left >= 0; --left) {
        if (g.Faulted()) return false;
        if (g.S16(e + 320) != 0) {                                    // 0x80078AD4
            const uint32_t x = g.U32(e + 184), y = g.U32(e + 188), z = g.U32(e + 192);
            const uint32_t pas = g.U32(e + 856);                      // 0x80078AF0, before the stores
            g.W32(e + 468, x);                                        // 0x80078AF4..0x80078B00
            g.W32(e + 472, y);
            g.W32(e + 476, z);
            if (pas != 0u) {
                g.W32(pas + 468, g.U32(pas + 184));                   // 0x80078B04..0x80078B0C
                const uint32_t p2 = g.U32(e + 856);                   // re-read per word
                g.W32(p2 + 472, g.U32(p2 + 188));
                const uint32_t p3 = g.U32(e + 856);
                g.W32(p3 + 476, g.U32(p3 + 192));
            }
            if (g.Faulted()) return false;
            if (!calls.Integrate(e, dt)) return false;                // 0x80078B3C
        }
        e = e + g.U32(kPoolTableG + 4u);                              // 0x80078B44..0x80078B50
    }
    return !g.Faulted();
}

bool BikeStepRegionH(GuestRam& g, uint32_t sp, BikeStepTailCallees& calls) {
    RoadRuntimeCallees& road = calls.RoadSeams();
    RoadTrackPass(g, sp, road);                                       // 0x80078B54
    if (g.Faulted()) return false;
    RoadClassPass(g, sp, road);                                       // 0x80078B5C
    if (g.Faulted()) return false;
    RouteCheckPass(g, sp, road);                                      // 0x80078B64
    if (g.Faulted()) return false;
    ProgressPass(g, sp);                                              // 0x80078B6C
    if (g.Faulted()) return false;
    // 0x80078B74..0x80078BC4: the ground frame of every listed entity
    {
        uint32_t cur = kRoadListFirst;                                // s0
        uint32_t end = g.U32(kRoadListHead);
        uint32_t e = g.U32(kRoadListFirst);
        int guard = 0;
        while (cur < end) {                                           // sltu
            if (g.Faulted() || ++guard > 4096) return false;
            const uint32_t ref = (g.U32(e + 568) & 0x600u) ? e + 184u : e + 504u;   // 0x80078B90..0x80078BA4
            cur += 4u;                                                // 0x80078BAC (delay slot)
            if (!calls.GroundFrame(e, ref, sp)) return false;         // 0x80078BA8
            end = g.U32(kRoadListHead);
            e = g.U32(cur);
        }
    }
    // 0x80078BC8..0x80078C0C: the contact frame of every listed entity (its `dt` in a1 is dead)
    {
        uint32_t cur = kRoadListFirst;
        uint32_t end = g.U32(kRoadListHead);
        uint32_t e = g.U32(kRoadListFirst);
        int guard = 0;
        while (cur < end) {
            if (g.Faulted() || ++guard > 4096) return false;
            cur += 4u;                                                // 0x80078BF4 (delay slot)
            if (!calls.ContactFrame(e, sp)) return false;             // 0x80078BF0
            end = g.U32(kRoadListHead);
            e = g.U32(cur);
        }
    }
    return !g.Faulted();
}

bool BikeStepRegionI(GuestRam& g, int32_t dt, BikeStepTailCallees& calls) {
    int32_t left = g.S32(g.U32(kPoolTableG + 12u));                   // 0x80078C14..0x80078C1C: s1
    uint32_t e = g.U32(kPoolTableG);                                  // 0x80078C20: s0
    if (left < 0) return !g.Faulted();                                // 0x80078C24
    if (left > 4096) return false;
    for (; left >= 0; --left) {
        if (g.Faulted()) return false;
        if (g.S16(e + 320) != 0)                                      // 0x80078C2C
            if (!calls.RiderPose(e, dt)) return false;                // 0x80078C40
        e = e + g.U32(kPoolTableG + 4u);                              // 0x80078C48..0x80078C54
    }
    return !g.Faulted();
}

bool BikeStepRegionJ(GuestRam& g, int32_t dt, uint32_t sp, BikeStepTailCallees& calls) {
    if (!calls.ActivationPass(sp)) return false;                      // 0x80078C58
    if (!calls.DownedRiderPass(sp)) return false;                     // 0x80078C60
    if (g.U32(g.U32(kGameStatePtr) + 48u) == 0u) return !g.Faulted(); // 0x80078C68..0x80078C7C
    uint32_t view = 0x800CD898u;                                      // s0
    uint32_t odo = 0x8005B380u;                                       // s2
    uint32_t pb = kPlayerBikes;                                       // s6
    uint32_t p = 0;                                                   // s4
    for (;;) {
        if (g.Faulted() || p > 4096u) return false;
        const uint32_t b = g.U32(pb);                                 // 0x80078C98: s1
        const bool seated = g.U32(g.U32(b + 852) + 604) < 3u;         // 0x80078CA0..0x80078CB0: s3
        if (seated) {
            const int32_t d = FixMul(g.S32(b + 480), dt);             // 0x80078CC4
            g.W32(odo, static_cast<uint32_t>(Add(g.S32(odo), Sra(d, 8))));   // 0x80078CCC..0x80078CD8
        }
        if ((g.U32(g.U32(b + 852) + 552) & 0x80000u) && g.U32(view + 540) != 6u) {   // 0x80078CDC..0x80078D00
            bool end = 0x30000 < g.S32(view + 788);                   // 0x80078D08..0x80078D14
            if (!end && seated && !(g.S32(b + 44u + 4u * p) < 4801))  // 0x80078D1C..0x80078D30
                end = true;
            if (end) {
                if (!calls.EndRace(b, 10, sp)) return false;          // 0x80078D38
            } else {
                g.W32(view + 788, static_cast<uint32_t>(Add(g.S32(view + 788), dt)));   // 0x80078D48..0x80078D58
            }
        }
        view += 1132u;                                                // 0x80078D5C / 0x80078D44
        odo += 4u;                                                    // 0x80078D60
        ++p;                                                          // 0x80078D74
        const uint32_t n = g.U32(g.U32(kGameStatePtr) + 48u);         // 0x80078D64..0x80078D70
        pb += 4u;                                                     // 0x80078D80
        if (!(p < n)) break;                                          // 0x80078D78 sltu
    }
    return !g.Faulted();
}

bool BikeStep(GuestRam& g, int32_t dt, uint32_t sp, const BikeTables& t, BikeStepCallees& calls,
              BikeStepTailCallees& tail) {
    const uint32_t ssp = sp - kBikeStepFrame;                         // 0x80075EE0
    return BikeStepRegionA(g, dt, t, calls) &&                        // [0x80075F18, 0x800761C8)
           BikeStepRegionB(dt, calls) &&                              // [0x800761C8, 0x80076208)
           BikeStepRegionC(g) &&                                      // [0x80076208, 0x80076804)
           BikeStepRegionD(g, dt, t, calls) &&                        // [0x80076804, 0x800774C8)
           BikeStepRegionE(g, dt, t, calls) &&                        // [0x800774C8, 0x80078478)
           BikeStepRegionF(g, dt) &&                                  // [0x80078478, 0x80078AAC)
           BikeStepRegionG(g, dt, tail) &&                            // [0x80078AAC, 0x80078B54)
           BikeStepRegionH(g, ssp, tail) &&                           // [0x80078B54, 0x80078C10)
           BikeStepRegionI(g, dt, tail) &&                            // [0x80078C10, 0x80078C58)
           BikeStepRegionJ(g, dt, ssp, tail);                         // [0x80078C58, 0x80078D84)
}

} // namespace rr::sim
