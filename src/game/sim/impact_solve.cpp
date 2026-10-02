#include "game/sim/impact_solve.h"

#include "game/sim/coll_util.h"

namespace rr::sim {
namespace {

using namespace cu;
using namespace solve;

// `mult x, (lh << 4)` then `(lo >> 16) | (hi << 16)`: the middle word, as the three-term dots of the
// solver, FaceCrossing and PropHitFace write it out inline.
int32_t Mh(int32_t x, int16_t h) { return Mid(x, Shl(h, 4)); }
// sum Mh(d[i], axis[i]) in the original's addu order (wrapping)
int32_t Dot3(GuestRam& g, const int32_t d[3], uint32_t axis) {
    const int32_t x = Mh(d[0], g.S16(axis)), y = Mh(d[1], g.S16(axis + 2)), z = Mh(d[2], g.S16(axis + 4));
    return Add(z, Add(y, x));
}
// SLUS 0x8002E604 Dot32(a, b) - 148 B, a leaf with an 8-byte frame of scratch: sum Mid(a[i], b[i]).
int32_t Dot32(const int32_t a[3], const int32_t b[3]) {
    return Add(Mid(a[2], b[2]), Add(Mid(a[1], b[1]), Mid(a[0], b[0])));
}
// The six-halfword negated copy of 0x800B07EC / 0x800B08A0 / 0x800B0970 (out never aliases src here, but
// the store/reload order is the original's).
void NegCopy(GuestRam& g, uint32_t out, uint32_t src) {
    g.W16(out, g.U16(src));
    g.W16(out + 2, g.U16(src + 2));
    const uint16_t v0 = g.U16(out), v1 = g.U16(src + 4);
    g.W16(out, static_cast<uint16_t>(0u - v0));
    const uint16_t w = g.U16(out + 2);
    g.W16(out + 4, v1);
    g.W16(out + 4, static_cast<uint16_t>(0u - v1));
    g.W16(out + 2, static_cast<uint16_t>(0u - w));
}
void Copy16x3(GuestRam& g, uint32_t out, uint32_t src) {
    for (uint32_t k = 0; k < 3; ++k) g.W16(out + 2u * k, g.U16(src + 2u * k));
}
void NegRow(GuestRam& g, uint32_t out, uint32_t src) {
    for (uint32_t k = 0; k < 3; ++k) g.W16(out + 2u * k, static_cast<uint16_t>(0u - g.U16(src + 2u * k)));
}
// FaceCrossing's slab code: (v >>> 31) + (lim < v ? 2 : 0)
uint32_t Code(int32_t v, int32_t lim) { return (U(v) >> 31) + (lim < v ? 2u : 0u); }
// 0x800B63D0..: 0x80000000 /u ((|n| >> 1) + ((|n| - 2) >> 31)), negated for n < 0
int32_t Inv(int32_t n) {
    const int32_t a = n < 0 ? Neg(n) : n;
    const int32_t d = Add(a >> 1, Sub(a, 2) >> 31);
    const int32_t q = S(MipsDivU(0x80000000u, U(d)));
    return n < 0 ? Neg(q) : q;
}
int32_t PoolOf(GuestRam& g, uint32_t shape) { return static_cast<int32_t>(g.U16(shape) >> 5); }

} // namespace

// ============================================================================ RASHCDG 0x800B5EB4
bool FaceCrossing(GuestRam& g, uint32_t A, uint32_t B, uint32_t dirV, uint32_t n, const uint32_t (&st)[11],
                  uint32_t sp, uint32_t& v0) {
    v0 = 0;
    for (uint32_t k = 0; k < 11; ++k) g.W32(sp + 16u + 4u * k, st[k]);        // the caller's stores
    g.W32(sp + 0, A);                                                          // 0x800B5EC0: home spills
    g.W32(sp + 8, dirV);                                                       // 0x800B5EEC
    g.W32(sp + 12, n);                                                         // 0x800B5EF0
    const uint32_t P0 = st[1], ax1 = st[5], ax2 = st[6], pc = st[9], pt = st[10];
    const int32_t lim1 = S(st[7]), lim2 = S(st[8]);
    int32_t dA[3], dB[3];
    for (uint32_t i = 0; i < 3; ++i) dA[i] = Sub(g.S32(P0 + 4 * i), g.S32(A + 4 * i));
    const int32_t A1 = Dot3(g, dA, ax1), A2 = Dot3(g, dA, ax2);               // s7, s3
    for (uint32_t i = 0; i < 3; ++i) dB[i] = Sub(g.S32(P0 + 4 * i), g.S32(B + 4 * i));
    const int32_t B1 = Dot3(g, dB, ax1), B2 = Dot3(g, dB, ax2);               // s1, s6
    const uint32_t t1 = Code(A1, lim1), v1 = Code(A2, lim2), c1 = Code(B1, lim1), c0 = Code(B2, lim2);
    g.W32(pc, 0);                                                              // 0x800B6240
    g.W32(pt, 0);                                                              // 0x800B624C
    bool go = false;
    if (t1 == 0 && v1 == 0) go = true;
    else if (c1 == 0 && c0 == 0) go = true;
    else {
        if (t1 != 0) {
            if (v1 != 0) {
                if (t1 == c1) { v0 = U(-1); return !g.Faulted(); }
                if (v1 == c0) { v0 = U(-1); return !g.Faulted(); }
            } else if (t1 == c1) { v0 = U(-1); return !g.Faulted(); }
        } else if (v1 == c0) {
            v0 = U(-1);
            return !g.Faulted();
        }
        if (t1 == 0 && c1 == 0) go = true;
        else if (v1 == 0 && c0 == 0) go = true;
    }
    if (!go) {                                                                 // 0x800B62B8: the corners
        const int32_t p0 = GProject(g, P0, n, A);
        bool same = true;
        for (uint32_t k = 2; k <= 4 && same; ++k)
            if ((p0 ^ GProject(g, st[k], n, A)) < 0) same = false;
        if (same) { v0 = U(-1); return !g.Faulted(); }
    }
    int32_t s0 = GDot(g, dirV, ax1), s2 = GDot(g, dirV, ax2);                  // 0x800B6328
    int32_t s3 = A2, s6 = B2;
    if (Iabs(s0) < 655) {
        s3 = s2 > 0 ? s3 : Sub(lim2, s3);
        s6 = s2 > 0 ? Sub(lim2, s6) : s6;
    } else if (Iabs(s2) < 655) {
        s3 = s0 > 0 ? A1 : Sub(lim1, A1);
        s6 = s0 > 0 ? Sub(lim1, B1) : B1;
    } else {
        const int32_t i0 = Inv(s0), i2 = Inv(s2);
        int32_t s4;
        if (i0 > 0) { s4 = FixMul(A1, i0); s0 = FixMul(Sub(lim1, B1), i0); }
        else { s4 = FixMul(Sub(A1, lim1), i0); s0 = Neg(FixMul(B1, i0)); }
        if (i2 > 0) { s3 = FixMul(s3, i2); s6 = FixMul(Sub(lim2, B2), i2); }
        else { s3 = FixMul(Sub(s3, lim2), i2); s6 = Neg(FixMul(B2, i2)); }
        if (s4 < s3) s3 = s4;
        if (s0 < s6) s6 = s0;
    }
    g.W32(pc, s6 < s3 ? 1u : 0u);                                              // 0x800B6534
    g.W32(pt, U(Add(S(st[0]), s3 < s6 ? s3 : s6)));                            // 0x800B6554
    v0 = 1;
    return !g.Faulted();
}

// ============================================================================ RASHCDG 0x800AD9BC
bool LandOnTop(GuestRam& g, uint32_t e, int32_t h, uint32_t cross, uint32_t thr, int32_t k, uint32_t sp,
               const BikeTables& t, CollisionCallees& c, uint32_t& v0) {
    v0 = 0;
    const uint32_t fr = sp - 64;
    g.W32(sp + 16, U(k));                                                      // the caller's store
    const uint32_t f234 = g.U32(e + 564);
    g.W32(e + 768, U(Neg(h)));                                                 // 0x800ADA10
    g.W32(e + 564, f234 | 0x218000u | (U(Neg(S(thr))) & 0x20000u));           // 0x800ADA24
    const uint32_t flag = 0x7FFF < k ? 0u : 1u;
    g.W32(fr + 16, flag);
    int32_t s1 = SDiv(h, Shl(g.S32(e + 308), 1));                              // h / (2 * +0x134)
    int32_t s4 = h >> 1;
    if (0xFD70 < s1) {
        s1 = 0xFD70;
        s4 = FixMul(0xFD70, g.S32(e + 308));
    }
    int32_t as = 0;
    if (!Asin(s1, t.asin, as)) return false;
    s1 = MulLo(as, 25736) >> 8;                                                // 4096/turn -> radians 16.16
    int32_t s0 = 0;
    uint32_t s5 = cross;
    if (flag) {
        const int32_t tt = FixMul(k, 0x20000);
        s0 = FixMul(s1, tt);
        const uint32_t idx = (U(MulLo(s0, 163)) >> 12) & 0x3FFCu;             // a byte offset, sin
        const int32_t sn = t.sincos[idx >> 1];
        g.W32(e + 772, U(Neg(FixMul(g.S32(e + 308), Shl(sn, 4)))));           // 0x800ADB54
    } else {
        bool launch = false;
        if (k < 0x10000) {
            if (thr != 0 && s5 != 0) {
                g.W32(e + 772, U(Neg(h)));                                     // 0x800ADBC0
                launch = true;
            } else {
                s0 = FixMul(Sub(0x10000, k), 0x20000);
                g.W32(e + 772, U(Sub(FixMul(Sub(h, s4), s0), h)));             // 0x800ADB98
                s0 = thr == 0 ? FixMul(s1, s0) : s1;
            }
        } else {
            g.W32(e + 772, U(Neg(h)));                                         // 0x800ADBC0
            if (thr == 0) s0 = 0;
            else launch = true;
        }
        if (launch) {
            if (!Call(c, kLaunch, {e, 0}, fr)) return false;
            s0 = s1;
            s5 = 1;
            g.W32(e + 564, g.U32(e + 564) & 0xFFFEFFFFu);                       // 0x800ADBEC
        }
    }
    const int32_t n0 = Neg(s0);
    s0 = Add(n0, S(U(Neg(S(s5))) & U(Sub(s0, n0))));                           // s5 ? s0 : -s0
    uint32_t keep = 0;
    if (flag && s0 > 0) keep = s0 < g.S32(e + 616) ? 1u : 0u;
    const int32_t f = g.S32(e + 616);
    const int32_t nw = Add(s0, S(U(Neg(S(keep))) & U(Sub(f, s0))));
    g.W32(e + 616, U(nw));                                                     // 0x800ADC40
    v0 = U(nw);
    return !g.Faulted();
}

// ============================================================================ RASHCDG 0x800B0510
bool PropHitFace(GuestRam& g, uint32_t e, uint32_t prop, uint32_t out, uint32_t sp, uint32_t& v0) {
    (void)sp;
    v0 = 0;
    int32_t d[3], r[3];
    for (uint32_t i = 0; i < 3; ++i) d[i] = Sub(g.S32(prop + 184 + 4 * i), g.S32(e + 184 + 4 * i));
    GScale(g, g.S32(prop + 480), prop + 450, prop + 456);                      // the prop's velocity
    for (uint32_t i = 0; i < 3; ++i) r[i] = Sub(g.S32(prop + 456 + 4 * i), g.S32(e + 456 + 4 * i));
    if (Dot32(d, r) > 0) return !g.Faulted();                                  // separating
    if (Dot3(g, d, e + 444) >= 0) {
        Copy16x3(g, out, e + 444);
    } else if (Dot3(g, d, e + 432) >= 0) {
        Copy16x3(g, out, e + 432);
    } else {
        for (uint32_t i = 0; i < 3; ++i) d[i] = Sub(g.S32(prop + 184 + 4 * i), g.S32(e + 196 + 4 * i));   // corner 0
        if (Dot3(g, d, e + 432) <= 0) NegCopy(g, out, e + 432);
        else if (Dot3(g, d, e + 444) <= 0) NegCopy(g, out, e + 444);
        else if (Dot3(g, d, e + 438) >= 0) Copy16x3(g, out, e + 438);
        else NegCopy(g, out, e + 438);
    }
    v0 = 1;
    return !g.Faulted();
}

// ============================================================================ RASHCDG 0x800AF3B0
bool ImpactSolve(GuestRam& g, uint32_t e, uint32_t shape, uint32_t nrm, uint32_t face, uint32_t flags,
                 uint32_t imp, uint32_t sp, const BikeTables& t, CollisionCallees& c, uint32_t& v0) {
    v0 = 0;
    const uint32_t fr = sp - 256;
    const uint32_t A2 = sp + 8, IMP = sp + 20;
    // the frame slots a callee reads through a pointer
    const uint32_t OUT = fr + 64, TOP = fr + 72, BOT = fr + 88, ROW = fr + 120, CROSS = fr + 128, DEPTH = fr + 132;
    g.W32(sp + 16, flags);                                                     // the caller's stores
    g.W32(IMP, imp);
    g.W32(A2, nrm);                                                            // 0x800AF3E4
    g.W32(sp + 12, face);                                                      // 0x800AF3E8
    int32_t s8 = g.S32(e + 576);                                               // SPEED
    if (g.U32(e + 568) & 0x600u) s8 = g.S32(e + 480);
    int32_t s4 = 0;
    const int32_t pool = PoolOf(g, shape);
    bool p8 = false, s7 = false;
    uint32_t car = 0, prop = 0, partner = 0, code8 = 0;
    int32_t H = 0;
    if (pool == 8) {                                                           // a road wall segment
        p8 = true;
        code8 = U(g.S16(shape + 2));
        s7 = (code8 & 0xFu) == 1u;
        H = g.S32(shape + 4);
    } else {
        const uint32_t h = g.U16(shape);
        const uint32_t T = kPoolTableAddr + ((h >> 5) << 4);
        const int32_t stride = g.S32(T + 4);
        H = g.S32(shape + 140);
        partner = g.U32(T) + U(MulLo(stride, S(h & 0x1Fu)));                    // mult; mflo
        if (pool == 4) prop = partner;
        if (pool == 3) car = partner;
    }
    const bool riderped = U(pool - 1) < 2u;
    const bool onB = ((g.U32(e + 564) >> 15) & 1u) != 0;
    const bool is33C = g.U32(e + 828) == shape, is340 = g.U32(e + 832) == shape;
    const bool sticky = onB && is340;
    const bool crashing = (g.U32(e + 568) & 0xEu) != 0;
    bool slow = false;
    if (!(s8 > 0x1017D) && !sticky)
        if (car == 0 || g.U32(car + 480) == 0) slow = true;
    const bool air = ((g.U32(e + 568) >> 10) & 1u) != 0;
    int32_t knock = 0;
    bool pushProp = false, tail = false;
    uint32_t s0 = 0;
    int32_t s1 = 0;
    uint32_t s6 = 0;
    if (slow) {                                                                // (a) CRAWLING
        s0 = 0;
        if (!riderped && prop == 0 && !(g.U32(e + 560) & 0x300u)) g.W32(e + 576, 0);   // 0x800AF5DC
        pushProp = prop != 0;
        tail = true;
    } else {
        s1 = sticky ? 1 : 0;
        if (prop != 0) {                                                       // (b) KNOCK A PROP OVER
            const uint32_t cls = (g.U16(g.U32(prop) + 14) & 0xF80u) >> 7;
            if (cls - 3u < 3u) {
                const uint32_t f = g.U32(prop + 592);
                if (!(f & 4u) && !(s8 > 0xB2D0D)) {
                    if (f & 0x800u) {
                        s1 = 1;
                    } else {
                        knock = -1;
                        if (!(f & 0x200u)) {
                            if (!Call(c, kPropKnock, {prop, e + 450, e + 814, e + 522, U(s8)}, fr)) return false;
                            knock = 1;
                            uint32_t snd = 0;
                            if (!Call(c, kSurfaceSound, {g.U32(shape + 8)}, fr, &snd)) return false;
                            if (!c.PlaySound3D(g.S32(shape + 12), g.S32(shape + 20), S(snd), 0)) return false;
                        }
                        g.W32(prop + 592, g.U32(prop + 592) | 0x400u);           // 0x800AF6A8
                    }
                }
            }
        }
        if (knock != 0) {
            s0 = knock > 0 ? 1u : 0u;
            g.W32(IMP, 0);                                                     // 0x800AFF8C
            tail = true;
        }
    }
    if (!tail) {
        // ---- s1: does the bike get ON TOP?
        if (prop != 0) {                                                       // 0x800AF6C0
            const int32_t v = GDot(g, prop + 438, prop + 522);
            if (s1 != 0) s1 = 1;
            else if (onB) s1 = 0;
            else if (g.U32(e + 568) & 0x7FFu) s1 = 0;
            else {
                const uint32_t a1 = g.U32(prop + 592);
                if (a1 & 2u) s1 = 0;
                else if (g.U32(prop + 180) == 30u && Iabs(v) < 6553) s1 = 1;
                else {
                    const uint32_t cls = (g.U16(g.U32(prop) + 14) & 0xF80u) >> 7;
                    s1 = (cls - 3u < 3u && (a1 & 4u)) ? 1 : 0;
                }
            }
        } else if (s1 == 0 && !onB) {                                          // 0x800AF770
            if (air) {
                s1 = 0;
                if (U(pool - 5) < 2u && !is33C) s1 = H < g.S32(kEnvLandHigh) ? 1 : 0;
            } else {                                                           // 0x800AF7D4
                s1 = 0;
                if (g.U32(e + 568) & 0x7FFu) {
                } else if (pool == 0) {
                    s1 = 1;
                } else {
                    bool go = true;
                    if (p8) {
                        if (flags & 8u) { s1 = 1; go = false; }
                        else if (!s7) go = false;
                    }
                    if (go) {                                                  // 0x800AF824
                        int32_t lift = g.S32(e + 616);
                        if (lift < 0) lift = 0;
                        const int32_t v = FixMul(lift, Sub(g.S32(kEnvLandHigh), g.S32(kEnvLandLow)));
                        s1 = H < Add(g.S32(kEnvLandLow), v) ? 1 : 0;
                    }
                }
            }
        }
        // ---- s6: THROW (0x800AF86C)
        s6 = 0;
        if (g.S32(kEnvThrow) < s8 && !(g.U32(e + 568) & 0x7FFu) && !(g.U32(e + 564) & 0x4000u)) s6 = is340 ? 0u : 1u;
        if (sticky) {
            s6 |= (g.U32(e + 564) >> 17) & 1u;
        } else if (car != 0) {                                                 // 0x800AF8E8: the closing speed
            const int32_t d = GDot(g, car + 450, e + 450);
            s4 = Sub(s8, FixMul(g.S32(car + 480), d));
            s6 = Shl(g.S32(kEnvThrow), 1) < s4 ? 1u : 0u;
        }
        int land = 0;
        if (s1 != 0 && s7) {
            // 0x800AF934: sp+156 := 1, read by no later path
            s1 = 0;
        } else if (s1 != 0) {
            if (air) {                                                         // 0x800AF948
                if (!TouchDown(g, e, e + 576, fr, c)) return false;
                g.W32(e + 564, g.U32(e + 564) & 0xFFFDFFFFu);                   // 0x800AF964
                s6 = 0;
            }
            GScale(g, g.S32(e + 308), e + 528, BOT);                           // sp+88 = Scale(+0x134, +0x210)
            int32_t sc[3], top[3], bot[3];
            for (uint32_t k = 0; k < 3; ++k) sc[k] = g.S32(BOT + 4 * k);
            for (uint32_t k = 0; k < 3; ++k) top[k] = Add(g.S32(e + 504 + 4 * k), sc[k]);
            for (uint32_t k = 0; k < 3; ++k) g.W32(TOP + 4 * k, U(top[k]));
            for (uint32_t k = 0; k < 3; ++k) bot[k] = Sub(g.S32(e + 504 + 4 * k), sc[k]);
            for (uint32_t k = 0; k < 3; ++k) g.W32(BOT + 4 * k, U(bot[k]));
            g.W32(fr + 200, CROSS);
            if (PoolOf(g, shape) == 8) {                                       // 0x800AF9F8: the wall's plane
                const uint32_t np = g.U32(A2);
                int32_t dt[3], db[3];
                for (uint32_t k = 0; k < 3; ++k) {
                    dt[k] = Sub(top[k], g.S32(shape + 8 + 4 * k));
                    g.W32(fr + 104 + 4 * k, U(dt[k]));
                }
                const int32_t a = Dot3(g, dt, np);
                for (uint32_t k = 0; k < 3; ++k) {
                    db[k] = Sub(bot[k], g.S32(shape + 8 + 4 * k));
                    g.W32(fr + 104 + 4 * k, U(db[k]));
                }
                const int32_t b = Dot3(g, db, np);
                bool cross = false;
                if (a > 0) {
                    if (b < 0) cross = true;
                    else { s1 = -1; g.W32(DEPTH, 0); g.W32(CROSS, 1); }
                } else if (b > 0) {
                    if (a < 0) cross = true;
                    else { s1 = -1; g.W32(DEPTH, 0); g.W32(CROSS, 1); }
                } else {
                    s1 = -1;
                    g.W32(DEPTH, 0x10000);
                    g.W32(CROSS, 0);
                }
                if (cross) {                                                   // 0x800AFB98
                    const int32_t v = GDot(g, e + 528, g.U32(A2));
                    if (a > 0) { g.W32(DEPTH, U(FixMul(v, b))); g.W32(CROSS, 0); }
                    else { g.W32(DEPTH, U(FixMul(v, a))); g.W32(CROSS, 1); }
                    s1 = 1;
                    g.W32(DEPTH, U(Iabs(g.S32(DEPTH))));
                }
            } else {                                                           // 0x800AFBFC: the partner's top face
                const int32_t pl = PoolOf(g, shape);
                const bool rp = U(pl - 1) < 2u;
                uint32_t t0, t1, t2, a3, t3, t4;
                int32_t t5;
                if (pl == 4 || rp) {
                    t4 = shape + 266;
                    t3 = shape + 260;
                    const int32_t f88 = g.S32(shape + 136), f8c = g.S32(shape + 140);
                    t5 = rp ? f88 : f8c;
                    H = rp ? f8c : Shl(f88, 1);
                    if (g.S16(shape + 274) >= 0) {
                        t2 = shape + 36; t1 = shape + 24; t0 = shape + 72; a3 = shape + 84;
                    } else {
                        t2 = shape + 96; t1 = shape + 108; t0 = shape + 60; a3 = shape + 48;
                        NegRow(g, ROW, shape + 266);
                        t4 = ROW;
                    }
                } else {
                    t4 = shape + 272;
                    t5 = Shl(g.S32(shape + 136), 1);
                    if (pl != 0) {
                        t3 = shape + 260; t2 = shape + 48; t1 = shape + 60; t0 = shape + 24;
                        H = g.S32(shape + 140);
                        a3 = shape + 36;
                    } else {                                                   // a bike partner
                        H = g.S32(shape + 132);
                        t3 = shape + 266;
                        if (g.S16(shape + 262) > 0) {
                            t2 = shape + 96; t1 = shape + 48; t0 = shape + 36; a3 = shape + 84;
                            NegRow(g, ROW, shape + 266);
                            t3 = ROW;
                        } else {
                            t2 = shape + 60; t1 = shape + 108; t0 = shape + 72; a3 = shape + 24;
                        }
                    }
                }
                const uint32_t st[11] = {U(Shl(g.S32(e + 308), 1)), t2, t1, t0, a3, t3, t4,
                                         U(Shl(g.S32(shape + 132), 1)), U(t5), CROSS, DEPTH};
                uint32_t r = 0;
                if (!FaceCrossing(g, TOP, BOT, e + 528, e + 516, st, fr, r)) return false;
                s1 = S(r);
            }
            if (s1 < 0 && (s6 != 0 || (flags & 1u))) {                         // 0x800AFD88: forced "on"
                if (flags & 1u) { g.W32(CROSS, 1); g.W32(DEPTH, 0); }
                else { g.W32(CROSS, 0); g.W32(DEPTH, g.U32(shape + 136)); }
                s1 = 1;
            }
            land = s1 > 0 ? 1 : s1 < 0 ? -1 : 0;
        }
        if (land > 0) {                                                        // (c) ON TOP OF IT
            g.W32(e + 832, shape);                                             // 0x800AFDE4
            if (pool == 0) g.W32(partner + 560, g.U32(partner + 560) | 0x02000000u);   // 0x800AFE0C
            if (!onB && car != 0) {                                            // 0x800AFE30: landing on a car
                if (s4 < 0) s4 = 0;
                if (s6 != 0) {
                    const int32_t gth = g.S32(kEnvThrow);
                    s4 = Sub(s4, Shl(gth, 1));
                    s4 = SDiv(s4, Shl(gth, 2));
                    const int32_t lo = S(U(s4) & ~U(s4 >> 31));                 // max(s4, 0)
                    const int32_t hi = Sub(0x10000, s4);
                    const int32_t tt = Add(lo, S(U(hi >> 31) & U(hi)));         // + min(1.0 - s4, 0)
                    s8 = FixMul(Add(FixMul(tt, -9830), 0xE666), g.S32(e + 576));
                } else {
                    s8 = Sub(s8, s4 >> 1);
                }
                g.W32(e + 480, U(s8));                                         // 0x800AFEF0
                g.W32(e + 576, U(s8));                                         // 0x800AFEF4
            }
            const uint32_t depth = g.U32(DEPTH);
            g.W32(fr + 16, depth);
            uint32_t r = 0;
            if (!LandOnTop(g, e, H, g.U32(CROSS), s6, S(depth), fr, t, c, r)) return false;
            if (g.U32(e + 568) & 0x400u) g.W32(e + 828, g.U32(e + 832));       // 0x800AFF30
            g.W32(IMP, 0);                                                     // 0x800AFF34
            s0 = 0;
        } else if (land < 0) {
            g.W32(IMP, 0);                                                     // 0x800AFF8C
            s0 = 0;
        } else {                                                               // 0x800AFF48
            s0 = (pool == 0 && air && is33C) ? 0u : 1u;
            if (prop != 0) pushProp = true;
        }
    }
    // ---- the tail (0x800AFF90)
    if (pushProp) {                                                            // the PROP takes the reversed impulse
        const uint32_t p = g.U32(IMP);
        const uint32_t x = U(static_cast<int16_t>(0u - g.U16(p + 0)));
        const uint32_t y = U(static_cast<int16_t>(0u - g.U16(p + 4)));
        g.W32(p + 0, x);                                                       // 0x800AFFC4: 16 bits only
        const uint32_t z = U(static_cast<int16_t>(0u - g.U16(p + 8)));
        g.W32(p + 4, y);
        g.W32(p + 8, z);
        ApplyImpulse(g, prop, p, 1);
        g.W32(IMP, 0);                                                         // 0x800AFFF4
        s0 = 0;
        if (g.U32(prop + 480) == 0) {
            s0 = 1;
        } else {
            uint32_t r = 0;
            if (!PropHitFace(g, e, prop, OUT, fr, r)) return false;
            if (r != 0) s0 = 1;
        }
        if (s0 != 0) {                                                         // OUT may be unwritten here
            s0 = 0;
            uint32_t r = 0;
            if (!Call(c, kPropKick, {prop, e, OUT}, fr, &r)) return false;
            if (r != 0) s0 = slow ? 0u : 1u;
        }
    }
    bool replaced = false;
    if (air) {                                                                 // 0x800B0050
        const uint32_t np = g.U32(A2);
        if (Iabs(g.S16(np + 2)) < 2048 && (pool == 0 || !(H > 32767))) {
            g.W32(A2, OUT);                                                    // the normal := -e.up
            NegRow(g, OUT, e + 522);
            replaced = true;
        }
    }
    if (!replaced) {
        const uint32_t p = g.U32(IMP);
        if (p != 0) ApplyImpulse(g, e, p, 1);                                  // (d) THE PUSH (0x800B00F4)
    }
    uint32_t v1 = 0;
    if (s0 != 0 && !crashing) {                                                // THE HAND-OVER (0x800B0104)
        if (car != 0) {
            if (!Call(c, kHitOutcome, {e, car, g.U32(A2)}, fr, &v1)) return false;
        } else {
            const uint32_t fl = g.U32(sp + 16), fc = g.U32(sp + 12);
            const uint32_t B = (p8 && (code8 & 0x200u)) ? 1u : 0u;
            uint32_t kind;
            if (riderped || prop != 0) kind = 2;
            else if ((fl & 2u) && (fc & 5u) == 0) kind = fc != 2u ? 1u : 0u;
            else if (fl & 4u) kind = 3;
            else kind = 5u - B;
            if (!Call(c, kImpactTurn, {e, shape, g.U32(A2), kind}, fr, &v1)) return false;
        }
    } else {
        v1 = 0;
        if (!sticky && (g.U32(e + 564) & 0x8000u)) v1 = g.U32(e + 832) == shape ? 1u : 0u;
    }
    if (p8) { v0 = v1; return !g.Faulted(); }
    if (riderped || knock < 0) { v0 = 1; return !g.Faulted(); }
    if (v1 == 0) { v0 = 0; return !g.Faulted(); }
    if (car != 0 || slow) { v0 = 1; return !g.Faulted(); }
    if (!pushProp)
        if (!c.PlaySound3D(g.S32(shape + 12), g.S32(shape + 20), g.S16(shape + 150), 0)) return false;
    if (g.U16(e + 172) < NumPlayers(g) && g.U32(g.U32(e + 852) + 604) < 2u && g.U32(0x8005B220u) == 0) {
        const int32_t k = S((U(Neg(S(prop))) & 0xD6945u) + 0x165A1Cu);        // the NEGATED POINTER
        const int32_t div = prop != 0 ? 2 : 1;
        g.W32(fr + 16, U(div));
        if (!c.Rumble(e, 0, s8, k, div, fr)) return false;
    }
    v0 = 1;
    return !g.Faulted();
}

// ============================================================================ RASHCDG 0x800AF224
bool ImpactGate(GuestRam& g, uint32_t e, uint32_t shape, uint32_t code, uint32_t flags, uint32_t imp,
                uint32_t sp, const BikeTables& t, CollisionCallees& c, uint32_t& v0) {
    v0 = 0;
    const uint32_t fr = sp - 56, N = fr + 24;
    g.W32(sp + 16, imp);                                                       // the caller's store
    uint32_t s1 = 6, t0 = 0;
    if (code != 0) {
        if (code & 0x100u) {                                                   // the partner's face `flags & 0xFF`
            g.W32(fr + 16, 0);
            if (!Call(c, kFaceNormal, {shape + 24, shape + 260, flags & 0xFFu, N, 0}, fr)) return false;
            const int32_t d = GDot(g, e + 450, N);
            if (d < -0xDDB2) s1 = 3;
            else if (0xDDB2 < d) s1 = 1;
            else s1 = ((code & 1u) << 1) ^ (code & 2u);
        } else if (flags & 0x200u) {                                           // the partner's face, side = code
            s1 = code & 0xFFu;
            g.W32(fr + 16, 0);
            if (!Call(c, kFaceNormal, {shape + 24, shape + 260, flags & 0xFFu, N, 0}, fr)) return false;
        } else {                                                               // e's own face, negated
            s1 = code & 0xFFu;
            g.W32(fr + 16, 0);
            if (!Call(c, kFaceNormal, {e + 196, e + 516, s1, N, 0}, fr)) return false;
            g.W16(N + 0, static_cast<uint16_t>(0u - g.U16(N + 0)));
            g.W16(N + 4, static_cast<uint16_t>(0u - g.U16(N + 4)));
            g.W16(N + 2, static_cast<uint16_t>(0u - g.U16(N + 2)));
        }
        t0 = 1;
    } else {                                                                   // riding on this very shape only;
        if (!(g.U32(e + 564) & 0x8000u)) return !g.Faulted();                  // N is then left unwritten
        if (g.U32(e + 832) != shape) return !g.Faulted();
    }
    if (Shl(g.S32(e + 312), 1) < g.S32(shape + 140)) t0 |= 4u;                 // over twice the bike's height
    const uint32_t im = g.U32(sp + 16);
    g.W32(fr + 16, t0);
    g.W32(fr + 20, im);
    return ImpactSolve(g, e, shape, N, s1, t0, im, fr, t, c, v0);
}

// ============================================================================ RASHCDG 0x800AF0A0
bool PoleReact(GuestRam& g, uint32_t e, uint32_t shp, uint32_t dir, uint32_t flags, uint32_t bit4, uint32_t imp,
               uint32_t sp, const BikeTables& t, CollisionCallees& c, uint32_t& v0) {
    v0 = 0;
    const uint32_t fr = sp - 80;
    g.W32(sp + 16, bit4);                                                      // the caller's stores
    g.W32(sp + 20, imp);
    const bool s2 = (flags & 5u) == 1u;
    const int32_t f134 = g.S32(e + 308), f130 = g.S32(e + 304);
    const int32_t h = s2 ? f134 : f130;
    const int32_t tt = flags < 2u ? Neg(h) : h;
    GMulAdd(g, e + 184, e + (s2 ? 444u : 432u), tt, fr + 24);                  // never read
    uint32_t s1 = 0;
    if (g.U32(e + 832) == shp) s1 = g.U8(e + 565) >> 7;                        // riding on it BEFORE
    const int32_t s5 = g.S32(e + 480);                                         // the speed BEFORE the solver
    const uint32_t b4 = g.U32(sp + 16);
    const uint32_t st = b4 != 0 ? 1u : 3u;
    const uint32_t im = g.U32(sp + 20);
    g.W32(fr + 16, st);
    g.W32(fr + 20, im);
    uint32_t v = 0;
    if (!ImpactSolve(g, e, shp, dir, flags, st, im, fr, t, c, v)) return false;
    if (v == 0) return !g.Faulted();
    if (s1 != 0) s1 = 0;                                                       // JUST got on it
    else s1 = (g.U32(e + 564) & 0x8000u) ? (g.U32(e + 832) == shp ? 1u : 0u) : 0u;
    const uint32_t a2 = PoolOf(g, shp) == 6 ? 0x30000u : 0x8000u;
    g.W32(fr + 16, flags);
    const int32_t a1 = (s2 || s1 != 0) ? s5 : (s5 >> 2);
    return Call(c, kImpactSeverity, {e, U(a1), a2, 0x10000u, flags}, fr, &v0) && !g.Faulted();
}

// ============================================================================ RASHCDG 0x800B11B4
bool BoxReact(GuestRam& g, uint32_t e, uint32_t shp, uint32_t code, uint32_t flags, uint32_t imp, uint32_t dir,
              uint32_t sp, const BikeTables& t, CollisionCallees& c, uint32_t& v0) {
    v0 = 0;
    const uint32_t fr = sp - 40;
    g.W32(sp + 16, imp);                                                       // the caller's stores
    g.W32(sp + 20, dir);
    const uint32_t im = g.U32(sp + 16), dp = g.U32(sp + 20);
    g.W32(fr + 16, im);
    uint32_t v = 0;
    if (!ImpactGate(g, e, shp, code, flags, im, fr, t, c, v)) return false;
    if (v == 0) return !g.Faulted();
    const int32_t d = GDot(g, e + 450, dp);
    uint32_t side;
    if (0xDDB2 < d) side = 1;
    else if (d < -0xDDB2) side = 3;
    else side = (MulLo(g.S16(e + 450), g.S16(dp + 4)) < MulLo(g.S16(e + 454), g.S16(dp + 0)) ? 0u : 1u) << 1;
    const int32_t r = FixMul(Neg(d), g.S32(e + 480));
    g.W32(fr + 16, side);
    return Call(c, kImpactSeverity, {e, U(Iabs(r)), g.U32(shp + 144), g.U32(e + 316), side}, fr, &v0) &&
           !g.Faulted();
}

// ============================================================================ RASHCDG 0x800B12A0
bool BikeWallHit(GuestRam& g, uint32_t e, uint32_t n, uint32_t flags, int32_t segVal, uint32_t seg, uint32_t imp,
                 uint32_t sp, const BikeTables& t, CollisionCallees& c) {
    const uint32_t fr = sp - 64;
    g.W32(sp + 16, seg);                                                       // the caller's stores
    g.W32(sp + 20, imp);
    const uint32_t s6 = g.U32(sp + 16);
    if (g.U32(e + 832) == e + 172 && !(g.U32(e + 564) & 0x8000u)) return !g.Faulted();   // 0x800B12F8
    uint32_t fast = 0;
    if (!(g.U32(e + 560) & 0x08000000u) && !(flags & 0x200u) && (g.U32(e + 388) & 0x10u))
        fast = g.S32(kEnvThrow) < g.S32(e + 480) ? 1u : 0u;
    if (fast != 0 && (g.U32(e + 568) & 0x400u)) {                              // airborne into the wall
        const uint32_t f = g.U32(e + 568);
        if (!(f & 0x02000000u)) {                                              // latch once per pass
            const uint16_t h0 = g.U16(e + 450), h1 = g.U16(e + 452), h2 = g.U16(e + 454);
            const uint32_t spd = g.U32(e + 480);
            g.W16(e + 864, h0);
            g.W16(e + 866, h1);
            g.W16(e + 868, h2);
            g.W32(e + 860, spd);
            g.W32(e + 568, g.U32(e + 568) | 0x02000000u);
        }
        int32_t as = 0;
        if (!Asin(Shl(g.S16(e + 866), 4), t.asin, as)) return false;
        const int32_t cs = t.sincos[2u * (U(0x555 - as) & 0xFFFu) + 1u];
        if (!Call(c, kRaiseHeading, {e + 864, g.U32(e + 576), U(cs), 0x6A51Eu}, fr)) return false;
        GScale(g, g.S32(e + 860), e + 864, e + 456);
        return !g.Faulted();
    }
    uint32_t s2 = (fast << 3) | ((flags & 0x400u) ? 1u : 5u);
    const int32_t d = GDot(g, e + 450, n);
    uint32_t s3 = 3;
    if (!(d < -0xDDB2))
        s3 = (MulLo(g.S16(e + 450), g.S16(n + 4)) < MulLo(g.S16(e + 454), g.S16(n + 0)) ? 0u : 1u) << 1;
    const uint32_t saved = g.U32(s6 + 4);                                      // 0x800B1460
    g.W32(s6 + 4, U(segVal));                                                  // 0x800B1468
    if (segVal < 0) g.W32(s6 + 4, 0x640000u);                                  // 0x800B1470
    const uint32_t im = g.U32(sp + 20);                                        // 0x800B1480
    g.W32(fr + 16, s2);
    g.W32(fr + 20, im);
    uint32_t v = 0;
    if (!ImpactSolve(g, e, s6, n, s3, s2, im, fr, t, c, v)) return false;
    if (v != 0) {
        const int32_t mag = FixMul(Iabs(d), g.S32(e + 480));
        const uint32_t ty = flags & 0xFu;
        if (ty == 8u) s2 = 0;
        else s2 = (ty == 2u || (flags & 0x400u)) ? 4u : 6u;
        g.W32(fr + 16, s3);
        uint32_t sev = 0;
        if (!Call(c, kImpactSeverity, {e, U(mag), s2 << 16, 0x10000u, s3}, fr, &sev)) return false;
        if (0x1017E < g.S32(e + 480)) {                                        // the scrape sound
            int32_t id = 0;
            bool rcnt = false;
            switch (ty) {                                                      // jump table 0x8005B958
            case 2: id = 13; break;
            case 7: id = 4; break;
            case 6: id = 12; break;
            case 3:
            case 5:
                if (S(sev) < 3) rcnt = true;
                else id = 17;
                break;
            default: rcnt = true; break;
            }
            if (rcnt) {
                uint32_t r = 0;
                if (!Call(c, kGetRCnt, {0xF2000002u}, fr, &r)) return false;
                id = (S((r & 0xFFu) * 5u) >> 8) + 50;
            }
            switch ((flags & 0xF0u) >> 4) {
            case 1: id = 10; break;
            case 2: id = 11; break;
            case 3: id = 12; break;
            case 4: id = 13; break;
            default: break;
            }
            if (!c.PlaySound3D(g.S32(e + 184), g.S32(e + 192), id, 0)) return false;
            if (g.U16(e + 172) < NumPlayers(g) && g.U32(g.U32(e + 852) + 604) < 2u && g.U32(0x8005B220u) == 0) {
                g.W32(fr + 16, 1);
                if (!c.Rumble(e, 0, g.S32(e + 480), 0x165A1C, 1, fr)) return false;
            }
            if (!(S(sev) < 2))
                if (!c.PlaySound3D(g.S32(e + 184), g.S32(e + 192), 49, 0)) return false;
        }
        if (s3 == 0) {
            if (!Call(c, kWallStance, {e, 4}, fr)) return false;
        } else if (s3 == 2) {
            if (!Call(c, kWallStance, {e, 3}, fr)) return false;
        }
    }
    g.W32(s6 + 4, saved);                                                      // 0x800B16C0
    return !g.Faulted();
}

// ============================================================================ the product's serve
bool ServeImpactSolve(GuestRam& g, const CollCall& call, const BikeTables& t, CollisionCallees& c, uint32_t& v0,
                      bool& ok) {
    auto a = [&call](int k) { return k < call.n ? call.a[k] : 0u; };
    const uint32_t sp = call.sp;
    switch (call.fn) {
    case kFaceCrossing: {
        uint32_t st[11];
        for (int k = 0; k < 11; ++k) st[k] = a(4 + k);
        ok = FaceCrossing(g, a(0), a(1), a(2), a(3), st, sp, v0);
        return true;
    }
    case kLandOnTop: ok = LandOnTop(g, a(0), S(a(1)), a(2), a(3), S(a(4)), sp, t, c, v0); return true;
    case kPropHitFace: ok = PropHitFace(g, a(0), a(1), a(2), sp, v0); return true;
    case kImpactSolve: ok = ImpactSolve(g, a(0), a(1), a(2), a(3), a(4), a(5), sp, t, c, v0); return true;
    case kImpactGate: ok = ImpactGate(g, a(0), a(1), a(2), a(3), a(4), sp, t, c, v0); return true;
    case kPoleReact: ok = PoleReact(g, a(0), a(1), a(2), a(3), a(4), a(5), sp, t, c, v0); return true;
    case kBoxReact: ok = BoxReact(g, a(0), a(1), a(2), a(3), a(4), a(5), sp, t, c, v0); return true;
    case kBikeWallHit:
        v0 = 0;
        ok = BikeWallHit(g, a(0), a(1), a(2), S(a(3)), a(4), a(5), sp, t, c);
        return true;
    default: return false;
    }
}

} // namespace rr::sim
