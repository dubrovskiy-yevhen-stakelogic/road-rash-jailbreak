#include "game/sim/contact.h"

#include "game/sim/coll_util.h"
#include "game/sim/crash.h"

namespace rr::sim {

using namespace cu;

namespace {

constexpr uint32_t kGsPtr = 0x8005B2F8;     // -> game state (+48 player count)
constexpr uint32_t kPlayer2Bike = 0x8005B21C;
constexpr uint32_t kSinCos = 0x8005624C;    // SLUS {s16 sin, s16 cos} x 4096, read in guest RAM

// The heading re-derivation BikeVsBike does for a stale bike out of the camera band
// (0x800ABA20..0x800ABAEC): +0x124 = RatAtan2(+0x1C2 << 4, +0x1C6 << 4), +0x128 = cos << 4 (table +2),
// +0x12C = sin << 4 (table +0), the angle re-read between the two.
void RebuildHeading(GuestRam& g, uint32_t e, const BikeTables& t) {
    const int32_t a = RatAtan2(Shl(g.S16(e + 450), 4), Shl(g.S16(e + 454), 4), t.atan);
    g.W32(e + 292, U(a));
    g.W32(e + 296, U(Shl(g.S16(kSinCos + ((U(a) & 0xFFFu) << 2 | 2u)), 4)));
    g.W32(e + 300, U(Shl(g.S16(kSinCos + ((g.U32(e + 292) & 0xFFFu) << 2)), 4)));
}

// 0x800AA140's quadrant of an angle: ((x + 8192) & 0xFFF) + 256, then / 1024 (always >= 0).
int32_t Quad(int32_t x) { return (Add(S(U(Add(x, 8192)) & 0xFFFu), 256) + 512) >> 10; }

// 0x800AA474's reciprocal length (0x800AA788..0x800AA814): 0x80000000 /u ((4r) / 2), negated with 4r.
int32_t InvLen(int32_t s3, const BikeTables& t) {
    const int32_t r4 = Shl(SqrtGte(s3, t.sqrt), 2);
    if (r4 < 0) {
        const int32_t n = Neg(r4);
        const int32_t s0 = n >> 1;
        const int32_t adj = Sub(n, 2) >> 31;
        return Neg(S(MipsDivU(0x80000000u, U(Add(s0, adj)))));
    }
    const int32_t s0 = r4 >> 1;
    const int32_t adj = Sub(r4, 2) >> 31;
    return S(MipsDivU(0x80000000u, U(Add(s0, adj))));
}

// The four-way sign idiom around the unsigned FixDiv, as 0x800AAD30 and 0x800AA474 branch on it.
int32_t DivArm(int32_t num, int32_t den) { return SDiv(num, den); }

} // namespace

// ============================================================================ RASHCDG 0x800ABE78
int32_t BikePairClassify(GuestRam& g, uint32_t a, uint32_t b, uint32_t pSide, uint32_t pAngle, uint32_t sp) {
    const uint32_t pAlong = g.U32(sp + 16), pAcross = g.U32(sp + 20);
    int32_t code = Iabs(Sub(g.S32(a + 188), g.S32(b + 188))) < g.S32(a + 312) ? 4 : 0;
    if (code != 0) {
        const int32_t d = S(U(Add(Sub(g.S32(a + 292), g.S32(b + 292)), 4096)) & 0xFFFu);
        const int32_t fold = Add(d, S(U(Sub(4096, Shl(d, 1))) & (Sub(2048, d) < 0 ? 0xFFFFFFFFu : 0u)));
        if (fold < 384) code += 8;
        g.W32(pAngle, U(d));
    }
    if (code == 12) {
        const int32_t x = GProject(g, b + 504, a + 516, a + 504);
        g.W32(pAcross, U(x));
        const uint32_t f1 = g.U32(a + 856) != 0 ? (0 < x ? 1u : 0u) : 0u;
        const uint32_t f2 = g.U32(b + 856) != 0 ? (U(x) >> 31) : 0u;
        const uint32_t side = f1 | (f2 << 1);
        g.W32(pSide, side);
        const int32_t w0 = g.S32(a + 304);
        int32_t reach = Add(Shl(g.S32(a + 308), 1), Shl(w0, 2));
        int32_t half = Add(w0, w0 < 0 ? 1 : 0) >> 1;
        const int32_t ax = Iabs(g.S32(pAcross));
        if (side & 1u) {
            const int32_t q = g.S32(g.U32(a + 856) + 304);
            reach = Add(reach, Shl(q, 1));
            half = Add(Add(half, q), w0);
        } else if (side & 2u) {
            const int32_t q = g.S32(g.U32(b + 856) + 304);
            reach = Add(reach, Shl(q, 1));
            half = Add(Add(half, g.S32(b + 304)), q);
        }
        if (!(ax < reach)) return -1;
        if (half < ax) code += 1;
    }
    if (code == 13) {
        const int32_t y = GProject(g, b + 504, a + 528, a + 504);
        g.W32(pAlong, U(y));
        const uint32_t side = g.U32(pSide);
        int32_t thr;
        if (side & 3u) {
            const uint32_t q = (side & 1u) ? g.U32(a + 856) : g.U32(b + 856);
            const int32_t v = MulLo(7, Add(g.S32(q + 308), g.S32(b + 308)));
            thr = v < 0 ? Add(v, 7) >> 3 : v >> 3;
        } else {
            const int32_t v = MulLo(9, Add(g.S32(a + 308), g.S32(b + 308)));
            thr = v < 0 ? Add(v, 15) >> 4 : v >> 4;
        }
        if (Iabs(g.S32(pAlong)) < thr) code += 2;
    }
    if (code == 15 && !(0xEFFFF < Iabs(Sub(g.S32(a + 480), g.S32(b + 480))))) code = 31;
    return code;
}

// ============================================================================ RASHCDG 0x800AA34C
void ViewExtent(GuestRam& g, uint32_t corners, uint32_t rec, uint32_t out) {
    g.W32(out + 4, 0x3FFF0000u);
    g.W32(out + 0, 0x3FFF0000u);
    g.W32(out + 12, 0xC0010000u);
    g.W32(out + 8, 0xC0010000u);
    uint32_t order = 0x7520u; // corners 0, 2, 1, 3
    for (int k = 0; k < 4; ++k) {
        const uint32_t p = corners + 12u * (order & 3u);
        const int32_t u = GProject(g, p, rec + 2, rec + 20);
        const int32_t v = GProject(g, p, rec + 14, rec + 20);
        int32_t a0 = g.S32(out + 0);
        if (u < a0) a0 = u;
        g.W32(out + 0, U(a0));
        const int32_t a1 = g.S32(out + 8);
        g.W32(out + 8, U(a1 < u ? u : a1));
        int32_t b0 = g.S32(out + 4);
        if (v < b0) b0 = v;
        g.W32(out + 4, U(b0));
        const int32_t b1 = g.S32(out + 12);
        g.W32(out + 12, U(b1 < v ? v : b1));
        order >>= 4;
    }
}

// ============================================================================ RASHCDG 0x800AA140
bool ResponseFar(GuestRam& g, uint32_t A, uint32_t B, uint32_t outA, uint32_t outB, uint32_t esp, const BikeTables& t,
                 uint32_t& v0) {
    const uint32_t sp = esp - 80u;
    const uint32_t rec = g.U32(A + 340);
    const uint32_t vout = g.U32(esp + 16);
    ViewExtent(g, A + 196, rec, sp + 16);
    ViewExtent(g, B + 196, rec, sp + 32);
    int32_t bA[4], bB[4];
    for (uint32_t k = 0; k < 4; ++k) {
        bA[k] = g.S32(sp + 16 + 4 * k);
        bB[k] = g.S32(sp + 32 + 4 * k);
    }
    const int32_t t0 = Sub(bB[2], bA[0]);
    const int32_t a3 = Sub(bA[2], bB[0]);
    bool sep = t0 < 0 || a3 < 0;
    int32_t a1 = 0, vv = 0;
    if (!sep) {
        a1 = Sub(bB[3], bA[1]);
        vv = Sub(bA[3], bB[1]);
        if (a1 < 0 || vv < 0) sep = true;
    }
    if (sep) {
        g.W32(outB, 0);
        g.W32(outA, 0);
        v0 = 0;
        return !g.Faulted();
    }
    const bool fa = t0 < a3;
    const int32_t ox = fa ? t0 : a3;
    const bool fb = a1 < vv;
    const int32_t oy = fb ? a1 : vv;
    int32_t k, s0;
    uint32_t axis;
    if (ox < oy) {
        k = fa ? Neg(ox) : ox;
        s0 = fa ? 2 : 0;
        axis = rec + 2;
    } else {
        k = fb ? Neg(oy) : oy;
        s0 = fb ? 3 : 1;
        axis = rec + 14;
    }
    GScale(g, k, axis, vout);
    int32_t s1 = s0 ^ 2;
    const int32_t ang = RatAtan2(Shl(g.S16(rec + 14), 4), Shl(g.S16(rec + 18), 4), t.atan);
    s0 = (s0 + Quad(Sub(g.S32(B + 292), ang))) & 3;
    g.W32(outB, U(s0) | 0x200u);
    s1 = (s1 + Quad(Sub(g.S32(A + 292), ang))) & 3;
    g.W32(outA, U(s1) | 0x200u);
    v0 = B;
    return !g.Faulted();
}

// ============================================================================ RASHCDG 0x800B5B48
void PairPushDir(GuestRam& g, uint32_t X, uint32_t Y, uint32_t out, const BikeTables& t) {
    const int32_t k = S(FixDiv(g.U32(Y + 316), g.U32(X + 316)));
    const int32_t t0 = Mid(g.S32(Y + 480), k);
    int32_t v[3];
    for (uint32_t i = 0; i < 3; ++i) {
        const int32_t a = Mid(g.S32(X + 480), Shl(g.S16(X + 450 + 2 * i), 4));
        const int32_t b = Mid(t0, Shl(g.S16(Y + 450 + 2 * i), 4));
        v[i] = Add(b, a);
    }
    constexpr int32_t T = 0x5A8000;
    while (Iabs(v[0]) > T || Iabs(v[1]) > T || Iabs(v[2]) > T)
        for (int32_t& x : v) x >>= 1;
    const int32_t ss = Add(Mid(v[2], v[2]), Add(Mid(v[1], v[1]), Mid(v[0], v[0])));
    const int32_t r4 = Shl(SqrtGte(ss, t.sqrt), 2);
    const int32_t adj = Sub(r4, 2) < 0 ? -1 : 0;
    const uint32_t d = U(Add(r4 >> 1, adj));
    const int32_t q = S(MipsDivU(0x80000000u, d));
    for (uint32_t i = 0; i < 3; ++i) g.W16(out + 2 * i, static_cast<uint16_t>(Mid(q, v[i]) >> 4));
}

// ============================================================================ RASHCDG 0x800AA474
bool HeavyResponse(GuestRam& g, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t esp, const BikeTables& t,
                   uint32_t& v0) {
    const uint32_t sp = esp - 264u;
    auto L = [&](uint32_t o) { return g.S32(sp + o); };
    auto W = [&](uint32_t o, uint32_t v) { g.W32(sp + o, v); };
    auto done = [&](uint32_t r) {
        v0 = r;
        return !g.Faulted();
    };
    W(200, 131);
    W(204, 131);
    W(208, 1);
    int32_t s8 = 0;
    const int32_t t0 = L(280);
    if (a2 & 2u) a1 = g.U32(a1 + 856);
    else if (a2 & 1u) a0 = g.U32(a0 + 856);
    int32_t s4 = L(284) < 1 ? 1 : 0;
    if (!(S(a3) < 2049)) s4 ^= 1;
    const bool noswap = t0 > 0 ? (s4 == 1) : (s4 == 0);
    if (noswap) {
        W(168, a0);
        W(172, a1);
        W(192, g.U32(sp + 288));
        W(196, g.U32(sp + 292));
    } else {
        W(192, g.U32(sp + 292));
        W(168, a1);
        W(172, a0);
        W(196, g.U32(sp + 288));
    }
    const uint32_t P = g.U32(sp + 168), Q = g.U32(sp + 172);
    s4 = Neg(s4);
    int32_t s5 = s4 & -3;
    const int32_t w = g.S32(P + 304), h = g.S32(P + 312);
    W(20, 0);
    W(16, U(Neg(w)));
    W(28, 0);
    W(24, U(w));
    W(32, U(w));
    W(36, U(Neg(h)));
    W(212, sp + 80);
    W(40, U(Neg(w)));
    W(216, sp + 48);
    W(44, U(Neg(h)));
    {
        const int32_t idx[4] = {Add(s5, 3), Add(s4, 2), Add(s4, 6), Add(s5, 7)};
        for (uint32_t k = 0; k < 4; ++k) {
            const uint32_t c = Q + 196u + U(MulLo(12, idx[k]));
            W(48 + 8 * k, U(GProject(g, c, P + 432, P + 184)));
            W(52 + 8 * k, U(GProject(g, c, P + 438, P + 184)));
        }
    }
    int32_t s7 = 0;
    for (;;) { // 0x800AA6EC: two passes, s8 = 0 then 1 (| 0x10)
        s7 = 0;
        if (s8 <= 0) {
            W(160, P);
            W(176, sp + 16);
            W(184, g.U32(sp + 192));
            W(164, Q);
            W(180, sp + 48);
            W(80, 0x10000);
            W(84, 0);
            W(88, 0);
            W(92, 0xFFFF0000u);
            W(96, 0xFFFF0000u);
            W(100, 0);
            W(104, 0);
            W(108, 0x10000);
            W(188, g.U32(sp + 196));
        } else {
            uint32_t s2 = g.U32(sp + 216), s1 = g.U32(sp + 212);
            s5 = L(72);
            int32_t s6 = L(76);
            W(160, Q);
            W(176, sp + 48);
            W(184, g.U32(sp + 196));
            W(164, P);
            W(180, sp + 16);
            W(188, g.U32(sp + 192));
            for (s7 = 0; s7 < 4; ++s7) {
                const int32_t nx = Sub(g.S32(s2 + 4), s6);
                g.W32(s1, U(nx));
                const int32_t ny = Sub(s5, g.S32(s2));
                const int32_t q0 = Mid(nx, nx);
                g.W32(s1 + 4, U(ny));
                const int32_t q1 = Mid(g.S32(s1 + 4), g.S32(s1 + 4));
                const int32_t s3 = Add(q0, q1);
                if (s3 < 17) {
                    g.W32(g.U32(sp + 292), 0);
                    g.W32(g.U32(sp + 288), 0);
                    return done(0);
                }
                const int32_t k = InvLen(s3, t);
                g.W32(s1, U(Mid(k, g.S32(s1))));
                g.W32(s1 + 4, U(Mid(k, g.S32(s1 + 4))));
                s5 = g.S32(s2);
                s6 = g.S32(s2 + 4);
                s2 += 8;
                s1 += 8;
            }
            if (L(80) < 1024) s8 |= 0x10;
        }
        // 0x800AA8F8: the points at sp+180 against the polygon at sp+176 with the normals at sp+212
        const uint32_t pts = g.U32(sp + 180), poly = g.U32(sp + 176), nrm = g.U32(sp + 212);
        int found = -1;
        for (int k = 0; k < 4 && found < 0; ++k) {
            const uint32_t p2 = pts + 8u * static_cast<uint32_t>(k);
            bool inside = true;
            for (uint32_t j = 0; j < 4; ++j) {
                const uint32_t p1 = poly + 8 * j, n = nrm + 8 * j;
                const int32_t x = Mid(Sub(g.S32(p2), g.S32(p1)), g.S32(n));
                const int32_t y = Mid(Sub(g.S32(p2 + 4), g.S32(p1 + 4)), g.S32(n + 4));
                const int32_t d = Add(x, y);
                W(144 + 4 * j, U(d));
                if (Add(d, 1024) < 0) {
                    inside = false;
                    break;
                }
            }
            if (inside) found = k;
        }
        const uint32_t s3bits = found < 0 ? 0u : (1u << found);
        if (s3bits != 0) {
            const uint32_t other = g.U32(sp + 164), mine = g.U32(sp + 160);
            const int32_t v = GProject(g, other + 468, mine + 516, mine + 468);
            int32_t s6, vx, off;
            if (v < 0) {
                s7 = (s3bits & 2u) ? 1 : 2;
                const uint32_t v1 = (s8 & 0x10) ? 0xFFFFFFFFu : 0u;
                s4 = S(L(84) < 0 ? (v1 & 5u) : (v1 & 4u));
                const uint32_t pt = g.U32(sp + 180) + 8u * U(s7);
                s6 = g.S32(pt + 4);
                vx = g.S32(pt);
                off = S(0xFFF80000u);
            } else {
                s7 = S((U(S(s3bits & 1u) - 1)) & 3u);
                const uint32_t v1 = (s8 & 0x10) ? 0xFFFFFFFFu : 0u;
                s4 = Add(S(L(84) < 0 ? (v1 & 2u) : (v1 & 3u)), 2);
                const uint32_t pt = g.U32(sp + 180) + 8u * U(s7);
                s6 = g.S32(pt + 4);
                vx = g.S32(pt);
                off = 0x80000;
            }
            s5 = Add(vx, off);
            // s4 may be 4 or 5 here: the edge loads then run past both arrays,
            // reading the frame words the original reads - kept in the guest stack for that reason.
            const uint32_t s1 = g.U32(sp + 176) + 8u * U(s4);
            const uint32_t n = g.U32(sp + 212) + 8u * U(s4);
            const int32_t x = Mid(Sub(s5, g.S32(s1)), g.S32(n));
            const int32_t y = Mid(Sub(s6, g.S32(s1 + 4)), g.S32(n + 4));
            const int32_t a1v = Add(x, y);
            const int32_t e = g.S32(sp + 144 + 4u * U(s4));
            if (Sub(e, a1v) < 131 && a1v < 131) {
                W(208, 0);
            } else {
                int32_t s3;
                const int32_t a0v = Neg(a1v);
                if (a0v > 0) {
                    const int32_t d = Sub(e, a1v);
                    s3 = d <= 0 ? Neg(FDiv(a0v, Sub(a1v, e))) : FDiv(a0v, d);
                } else {
                    const int32_t d = Sub(e, a1v);
                    s3 = d <= 0 ? FDiv(a1v, Sub(a1v, e)) : Neg(FDiv(a1v, d));
                }
                if (!(s3 < -15)) {
                    const int32_t s2 = Sub(0x10000, s3);
                    int32_t s0 = Mid(s2, s5);
                    const uint32_t q = g.U32(sp + 180) + 8u * U(s7);
                    W(208, 0);
                    int32_t vv = Mid(s3, g.S32(q));
                    W(200, U(Add(s0, vv)));
                    s0 = Mid(s2, s6);
                    vv = Mid(s3, g.S32(q + 4));
                    W(204, U(Add(s0, vv)));
                }
            }
        }
        // 0x800AAB94
        const uint32_t t1 = g.U32(sp + 208);
        s8 = Add(s8, 1);
        if (t1 == 0) break;
        if (s8 < 2) continue;
        g.W32(g.U32(sp + 292), 0);
        g.W32(g.U32(sp + 288), 0);
        return done(0);
    }
    // 0x800AABC8: found
    if (s4 == 1) s4 = 4;
    else if (s4 == 3) s4 = 5;
    g.W32(g.U32(sp + 188), U(s7) | 0x100u);
    g.W32(g.U32(sp + 184), U(s4) | 0x200u);
    {
        const uint32_t m = (s4 == 0 || s4 == 2) ? 0xFFFFFFFFu : 0u;
        const uint32_t p = g.U32(sp + 188);
        const uint32_t v1 = g.U32(p);
        const uint32_t d = ((U(s4) ^ 2u) | 0x200u) - v1;
        g.W32(p, v1 + (m & d));
    }
    const uint32_t q = g.U32(sp + 180) + 8u * U(s7);
    int32_t v = MulLo(Sub(L(200), g.S32(q)), 5);
    if (v < 0) v = Add(v, 3);
    s5 = v >> 2;
    v = MulLo(Sub(L(204), g.S32(q + 4)), 5);
    if (v < 0) v = Add(v, 3);
    const int32_t s6 = v >> 2;
    const uint32_t P2 = g.U32(sp + 168);
    GScale(g, s5, P2 + 432, sp + 112);
    GScale(g, s6, P2 + 438, sp + 128);
    const uint32_t out = g.U32(sp + 296);
    g.W32(out, g.U32(sp + 112) + g.U32(sp + 128));
    g.W32(out + 4, g.U32(sp + 116) + g.U32(sp + 132));
    g.W32(out + 8, g.U32(sp + 120) + g.U32(sp + 136));
    return done(g.U32(sp + 164));
}

// ============================================================================ RASHCDG 0x800AAD30
bool Response(GuestRam& g, uint32_t A, uint32_t B, uint32_t outA, uint32_t outB, uint32_t esp, const BikeTables& t,
              CollisionCallees& c, uint32_t& v0) {
    const uint32_t sp = esp - 200u;
    auto L = [&](uint32_t o) { return g.S32(sp + o); };
    auto W = [&](uint32_t o, uint32_t v) { g.W32(sp + o, v); };
    // the home-area spills of a0..a3 (the caller's frame)
    W(200, A);
    W(204, B);
    W(208, outA);
    W(212, outB);
    const uint32_t vout = g.U32(sp + 216);
    auto tail = [&](uint32_t s4) { // 0x800AB710: the optional remaining-time out-parameter
        const uint32_t p = g.U32(sp + 220);
        if (p != 0) {
            const int32_t k = L(144);
            if (k < Sub(L(148), 1)) g.W32(p, U(Sub(g.S32(kCollDt), MulLo(Add(k, 1), L(132)))));
            else g.W32(p, 0);
        }
        v0 = s4;
        return !g.Faulted();
    };
    g.W32(outB, 0);
    g.W32(outA, 0);
    if (0x140000 < Iabs(Sub(g.S32(A + 188), g.S32(B + 188)))) {
        v0 = 0;
        return !g.Faulted();
    }
    int32_t s8 = 1;
    W(104, 8);
    W(108, 6);
    W(140, 0);
    W(136, 0);
    W(64, A);
    W(68, B);
    W(76, B);
    W(72, g.U32(A + 856));
    uint32_t s4 = 0;
    {
        int v1 = 0;
        if (g.U32(A + 856) != 0) v1 = g.U32(A + 1088) != 0 ? 1 : 0;
        W(152, 2u << v1);
    }
    {
        const int32_t d = GDot(g, A + 450, B + 450);
        const int32_t pr = Mid(d, g.S32(B + 480));
        const int32_t sA = g.S32(A + 480), sB = g.S32(B + 480);
        const int32_t rel = Sub(sA, pr);
        W(124, U(rel));
        const bool take = sB < sA ? rel < sA : rel < sB;
        const int32_t s2 = take ? (sB < sA ? sA : sB) : rel;
        int32_t v = Mid(g.S32(kCollDt), s2);
        v = Add(v, 0x8000) >> 16;
        v = MulLo(v, 3);
        if (v < 0) v = Add(v, 3);
        const int32_t n = v >> 2;
        int32_t a0 = Add(n, (Sub(n, 1) >> 31) & Sub(1, n));
        const int32_t tt = Sub(4, n);
        a0 = Add(a0, (tt >> 31) & tt);
        W(148, U(a0));
        const int32_t dt = g.S32(kCollDt);
        W(132, U(dt));
        if (!(a0 < 2)) {
            const int32_t step = MipsDiv(dt, a0);
            W(132, U(step));
            GScale(g, Mid(g.S32(A + 480), Sub(step, dt)), A + 450, vout);
            ApplyImpulse(g, A, vout, 0);
            const int32_t dt2 = g.S32(kCollDt);
            GScale(g, Mid(g.S32(B + 480), Sub(L(132), dt2)), B + 450, vout);
            ApplyImpulse(g, B, vout, 0);
        }
    }
    W(112, U(Mid(L(132), L(124))));
    {
        const int32_t d = GDot(g, B + 450, A + 450);
        W(144, 0);
        const int32_t pr = Mid(d, g.S32(A + 480));
        const int32_t v1 = Sub(g.S32(B + 480), pr);
        W(128, U(v1));
        W(116, U(Mid(L(132), v1)));
    }
    uint32_t s5 = 0;
    for (;;) { // 0x800AB02C: the substep loop
        int32_t s7 = 0;
        for (;;) { // 0x800AB030: the pairings
            s4 = g.U32(sp + 64 + 4u * U(s7));
            const uint32_t s3 = g.U32(sp + 64 + 4u * U((s7 + 3) & 3));
            int32_t s2, a2;
            if (s7 & 1) {
                s2 = L(116);
                a2 = L(128);
            } else {
                s2 = L(112);
                a2 = L(124);
            }
            const uint32_t p3 = g.U16(s3 + 172) >> 5;
            s5 = 0;
            if (p3 == 3) {
                s5 = 16;
                if ((g.U16(s4 + 172) >> 5) == 0 && (g.U32(s4 + 568) & 0x600u) == 0) s5 = 48;
            }
            const bool big = p3 == 0 || p3 == 3;
            const int32_t hw = g.S32(s3 + 304), hl = g.S32(s3 + 308);
            const int32_t mn = hw < hl ? hw : hl;
            const int32_t thr = big ? MulLo(mn, 20) : 0;
            W(84, U(thr));
            int32_t s1;
            uint32_t r = 0;
            if (Iabs(a2) < thr) {
                const int32_t x0 = Mid(g.S32(s4 + 300), Sub(g.S32(s3 + 468), g.S32(s4 + 468)));
                const int32_t x1 = Mid(g.S32(s4 + 296), Sub(g.S32(s3 + 476), g.S32(s4 + 476)));
                s1 = Add(x0, x1);
                W(16, s3 + 432);
                W(20, s5);
                W(24, sp + 80);
                W(28, sp + 84);
                if (!Call(c, pair::kFacePoint, {s4 + 196, s4 + 450, U(s1), s3 + 196, s3 + 432, s5, sp + 80, sp + 84},
                          sp, &r))
                    return false;
            } else {
                s1 = s2;
                W(16, s3 + 432);
                W(20, s5);
                W(24, sp + 80);
                W(28, sp + 84);
                if (!Call(c, pair::kFaceDeepest, {s4 + 196, s4 + 450, U(s2), s3 + 196, s3 + 432, s5, sp + 80, sp + 84},
                          sp, &r))
                    return false;
            }
            s5 = r;
            if (s5 < 8u) { // 0x800AB1B0
                s8 = 0;
                W(16, 0);
                if (!Call(c, pair::kFaceNormal, {s3 + 196, s3 + 432, g.U32(sp + 80), sp + 56, 0u}, sp)) return false;
                const int32_t dv = GDot(g, sp + 56, s4 + 450);
                const int32_t s0 = s1 < 0 ? Neg(dv) : dv;
                const int32_t a1 = g.S32(s4 + 308);
                s1 = a1;
                if (!(a1 < s2)) s1 = DivArm(s2, a1);
                if (s0 < -3275) {
                    const int32_t a0 = Add(Add(L(84), 8192), s1);
                    W(84, U(a0));
                    GScale(g, a0, sp + 56, vout);
                } else {
                    const int32_t vv = Add(L(84), s1);
                    W(84, U(vv));
                    W(16, 8192);
                    int16_t ha[3], hb[3];
                    int32_t o[3];
                    Read16x3(g, s4 + 450, ha);
                    Read16x3(g, sp + 56, hb);
                    Blend16To32(ha, hb, o, Neg(vv), 8192); // SLUS 0x8002ECB8
                    Write32x3(g, vout, o);
                }
                const uint32_t pB = g.U16(B + 172) >> 5;
                if (pB == 0 || pB == 3) {
                    const uint32_t f2 = s5 & 2u;
                    const int32_t sp80 = L(80);
                    if ((f2 && sp80 == 3) || (!f2 && sp80 == 1)) {
                        if (GDot(g, A + 450, B + 450) > 0) {
                            const int32_t pv = GProject(g, s4 + 468, s3 + 432, s3 + 468);
                            W(84, U(pv));
                            int32_t a0;
                            if (pv < 0) {
                                W(80, 0);
                                s5 = (s5 & 2u) ? 2u : 1u;
                                a0 = Sub(Neg(g.S32(s3 + 304)), g.S32(s4 + 304));
                            } else {
                                W(80, 2);
                                s5 = (s5 & 2u) ? 3u : 0u;
                                a0 = Add(g.S32(s3 + 304), g.S32(s4 + 304));
                            }
                            GScale(g, a0, s3 + 432, vout);
                            W(84, U(Add(g.S32(s3 + 304), g.S32(s4 + 304))));
                        }
                    }
                }
                // 0x800AB3A8
                bool saved = false;
                if (s7 + 2 < L(152)) {
                    if ((s3 == A && L(80) == 2) || (s4 == A && (s5 & 1u) != ((s5 & 2u) >> 1))) {
                        W(104, s5);
                        W(136, s4);
                        W(108, U(L(80)));
                        W(40, g.U32(vout));
                        W(140, s3);
                        W(44, g.U32(vout + 4));
                        W(48, g.U32(vout + 8));
                        s7 = 1;
                        s8 = 1;
                        W(120, U(L(84)));
                        saved = true;
                    }
                }
                if (!saved && g.U32(sp + 104) < 8u) {
                    bool add;
                    if (s3 == g.U32(sp + 140)) {
                        if (L(108) != L(80)) {
                            add = true;
                        } else {
                            add = false;
                            if (L(84) < L(120)) {
                                g.W32(vout, g.U32(sp + 40));
                                g.W32(vout + 4, g.U32(sp + 44));
                                g.W32(vout + 8, g.U32(sp + 48));
                            }
                        }
                    } else {
                        W(40, U(Neg(L(40))));
                        W(48, U(Neg(L(48))));
                        W(44, U(Neg(L(44))));
                        add = true;
                    }
                    if (add) {
                        g.W32(vout, g.U32(vout) + g.U32(sp + 40));
                        g.W32(vout + 4, g.U32(vout + 4) + g.U32(sp + 44));
                        g.W32(vout + 8, g.U32(vout + 8) + g.U32(sp + 48));
                    }
                }
            }
            // 0x800AB510
            if (s8 != 0) {
                ++s7;
                if (s7 < L(152)) continue;
            }
            break;
        }
        // 0x800AB52C
        if (g.U32(sp + 104) < 8u) s8 = 0;
        if (s8 == 0) break;
        s4 = 0;
        if (L(144) < Sub(L(148), 1)) {
            GScale(g, Mid(g.S32(A + 480), L(132)), A + 450, vout);
            ApplyImpulse(g, A, vout, 0);
            GScale(g, Mid(g.S32(B + 480), L(132)), B + 450, vout);
            ApplyImpulse(g, B, vout, 0);
            W(144, U(Add(L(144), 1)));
            if ((g.U16(B + 172) >> 5) != 0 || L(104) != 8) continue;
            W(16, sp + 96);
            W(20, sp + 100);
            const int32_t r = BikePairClassify(g, A, B, sp + 88, sp + 92, sp);
            if (!(r < 15)) {
                W(16, g.U32(sp + 96));
                W(24, outA);
                W(28, outB);
                W(32, vout);
                W(20, g.U32(sp + 100));
                uint32_t hv = 0;
                if (!HeavyResponse(g, A, B, g.U32(sp + 88), g.U32(sp + 92), sp, t, hv)) return false;
                s4 = hv;
            }
            if (s4 != 0) return tail(s4);
            continue;
        }
        s8 = 0;
        break;
    }
    // 0x800AB684
    if (g.U32(sp + 104) < 8u && s5 == 8u) {
        W(80, g.U32(sp + 108));
        g.W32(vout, g.U32(sp + 40));
        s5 = g.U32(sp + 104);
        g.W32(vout + 4, g.U32(sp + 44));
        s4 = g.U32(sp + 136);
        g.W32(vout + 8, g.U32(sp + 48));
    }
    if (!(s5 < 8u)) {
        v0 = s4;
        return !g.Faulted();
    }
    uint32_t a2p;
    if (s4 == B) {
        g.W32(outB, s5 | 0x100u);
        a2p = outA;
    } else {
        g.W32(outA, s5 | 0x100u);
        a2p = outB;
    }
    g.W32(a2p, g.U32(sp + 80) | 0x200u);
    return tail(s4);
}

// ============================================================================ RASHCDG 0x800AB7A0
bool BikeVsBike(GuestRam& g, uint32_t me, uint32_t other, int32_t mode, uint32_t esp, const BikeTables& t,
                CollisionCallees& c, uint32_t& v0) {
    const uint32_t sp = esp - 152u;
    auto ret = [&](uint32_t r) {
        v0 = r;
        return !g.Faulted();
    };
    g.W32(sp + 80, 0);
    g.W32(sp + 108, 0);
    int32_t st0 = 0, st1 = 0;
    if (!StaleHeading(g, me, t, st0) || !StaleHeading(g, other, t, st1)) return false;
    const uint32_t stale = U(st0) | (U(st1) << 1);
    int arm = 0;
    if (InCameraBox(g, me, 0x140000, 0x1C0000) != 0) {
        g.W16(other + 320, static_cast<uint16_t>(g.U16(other + 320) | 0xCu));
        const uint32_t oh = g.U16(other + 172);
        if (oh < g.U32(g.U32(kGsPtr) + 48u)) {
            const uint32_t t0 = (oh == 1 ? 1u : 0u) & ((g.U32(kChainCur) >> 2) & 1u);
            const uint32_t mh = g.U16(me + 172);
            const uint32_t wa = kChainCur + 4u * (mh >> 4);
            g.W32(wa, g.U32(wa) | ((oh - (t0 - 1u)) << ((mh & 0xFu) * 2u)));
            const uint32_t t1 = other == g.U32(kPlayer2Bike) ? (t0 == 0 ? 1u : 0u) : 0u;
            g.W32(kChainCur, g.U32(kChainCur) | (t1 << 3));
        }
        for (uint32_t ent : {me, other}) {
            const uint32_t h = g.U16(ent + 172);
            const uint32_t hit = (g.U32(kChainPrev + 4u * (h >> 4)) >> ((h & 0xFu) * 2u)) & 3u;
            g.W32(kChainMask, g.U32(kChainMask) | (hit != 0 ? (1u << (h & 31u)) : 0u));
        }
        if (mode == 0) {
            const int32_t n = g.S32(kContactCount);
            const uint32_t mh = g.U16(me + 172), oh2 = g.U16(other + 172);
            for (int32_t i = 0; i < n; ++i) {
                const uint32_t r = kContactList + 36u * U(i);
                const uint32_t ha = g.U16(r), hb = g.U16(r + 2);
                if ((ha == mh && hb == oh2) || (ha == oh2 && hb == mh)) return ret(0);
            }
        }
        g.W32(sp + 16, sp + 88);
        g.W32(sp + 20, sp + 92);
        const int32_t code = BikePairClassify(g, me, other, sp + 80, sp + 84, sp);
        if (code == -1) return ret(0);
        arm = code == 0x1F ? 2 : 1;
    } else {
        if (stale & 1u) RebuildHeading(g, me, t);
        if (stale & 2u) RebuildHeading(g, other, t);
    }
    g.W32(sp + 104, 0);
    uint32_t s1 = 0;
    if (arm == 2) {
        g.W32(sp + 16, g.U32(sp + 88));
        g.W32(sp + 24, sp + 96);
        g.W32(sp + 28, sp + 100);
        g.W32(sp + 32, sp + 40);
        g.W32(sp + 20, g.U32(sp + 92));
        if (!HeavyResponse(g, me, other, g.U32(sp + 80), g.U32(sp + 84), sp, t, s1)) return false;
    } else if (arm == 1) {
        g.W32(sp + 16, sp + 40);
        g.W32(sp + 20, sp + 104);
        if (!Response(g, other, me, sp + 100, sp + 96, sp, t, c, s1)) return false;
        if (g.U32(me + 856) != 0 && s1 == 0) {
            g.W32(sp + 16, sp + 40);
            g.W32(sp + 20, sp + 104);
            if (!Response(g, me, other, sp + 96, sp + 100, sp, t, c, s1)) return false;
        }
    } else {
        g.W32(sp + 16, sp + 40);
        if (!ResponseFar(g, other, me, sp + 100, sp + 96, sp, t, s1)) return false;
    }
    if (g.U32(sp + 96) == 0 && g.U32(sp + 100) == 0) return ret(g.U32(sp + 108));
    if (g.U32(s1 + 1088) == 0) s1 = g.U32(s1 + 856);
    uint32_t s2, s3, s4;
    if (s1 == me) {
        s2 = other;
        s3 = g.U32(sp + 100);
        s4 = g.U32(sp + 96);
    } else {
        s2 = me;
        s4 = g.U32(sp + 100);
        s3 = g.U32(sp + 96);
    }
    if (mode == 0) ApplyImpulse(g, s1, sp + 40, 1);
    if (arm <= 0) {
        if (!Call(c, pair::kBikeBikeReact, {s1, s2, s4, s3}, sp)) return false;
        return ret(g.U32(sp + 108));
    }
    g.W32(sp + 108, 1);
    if (g.S32(sp + 104) > 0) {
        PairPushDir(g, s2, s1, sp + 72, t);
        const int32_t k = FixMul(g.S32(s2 + 576), g.S32(sp + 104));
        GScale(g, k, sp + 72, sp + 56);
        ApplyImpulse(g, me, sp + 56, 1);
        ApplyImpulse(g, other, sp + 56, 1);
        const uint32_t v = g.U32(sp + 104);
        for (uint32_t ent : {me, other}) {
            const uint32_t w = g.U32(ent + 552);
            g.W32(ent + 552, v < w ? w : v);
        }
    }
    const int32_t n = g.S32(kContactCount);
    int32_t i = 0;
    const uint32_t h1 = g.U16(s1 + 172), h2 = g.U16(s2 + 172);
    for (; i < n; ++i) {
        const uint32_t r = kContactList + 36u * U(i);
        const uint32_t ha = g.U16(r), hb = g.U16(r + 2);
        if ((ha == h1 && hb == h2) || (ha == h2 && hb == h1)) break;
    }
    if (!(i < 8)) {
        if (!Call(c, pair::kBikeBikeReact, {s1, s2, s4, s3}, sp)) return false;
        return ret(1);
    }
    if (mode != 0 && i < g.S32(kContactCount)) return ret(1);
    const uint32_t r = kContactList + 36u * U(i);
    g.W16(r, static_cast<uint16_t>(g.U16(s1 + 172)));
    g.W32(r + 16, g.U32(sp + 40));
    g.W32(r + 20, g.U32(sp + 44));
    g.W32(kContactCount, g.U32(kContactCount) + 1u);
    g.W32(r + 24, g.U32(sp + 48));
    g.W32(r + 28, s4 | (s3 << 16));
    g.W16(r + 2, static_cast<uint16_t>(g.U16(s2 + 172)));
    if (g.U32(kChainMask) & (1u << (g.U16(s1 + 172) & 31u))) {
        // ChainReaction (ported) calls BikeVsBike back (mode 1): the recursion runs natively here.
        struct Recurse final : CollisionCallees {
            GuestRam& g;
            const BikeTables& t;
            CollisionCallees& in;
            Recurse(GuestRam& gg, const BikeTables& tt, CollisionCallees& i) : g(gg), t(tt), in(i) {}
            bool PlaySound3D(int32_t x, int32_t z, int32_t id, int32_t bank) override {
                return in.PlaySound3D(x, z, id, bank);
            }
            bool ReleaseContact(uint32_t e) override { return in.ReleaseContact(e); }
            bool Rumble(uint32_t e, uint32_t o, int32_t s, int32_t k, int32_t d, uint32_t spx) override {
                return in.Rumble(e, o, s, k, d, spx);
            }
            bool Unported(uint32_t fn, const uint32_t* a, int n, uint32_t spx, uint32_t& r) override {
                if (fn == pair::kBikeVsBike) return BikeVsBike(g, a[0], a[1], S(a[2]), spx, t, in, r);
                return in.Unported(fn, a, n, spx, r);
            }
            bool UnportedAt(uint32_t fn, const uint32_t* a, int n, uint32_t spx, const GuestRegs& regs,
                            uint32_t& r) override {
                if (fn == pair::kBikeVsBike) return BikeVsBike(g, a[0], a[1], S(a[2]), spx, t, in, r);
                return in.UnportedAt(fn, a, n, spx, regs, r);
            }
        } rec(g, t, c);
        if (!ChainReaction(g, s1, sp + 40, g.U16(s2 + 172), sp, rec)) return false;
    }
    return ret(g.U32(sp + 108));
}

// ============================================================================ the product's serve
bool ServeContactPair(GuestRam& g, const CollCall& call, const BikeTables& t, CollisionCallees& c, uint32_t& v0,
                      bool& ok) {
    const uint32_t* a = call.a;
    auto stackArgs = [&](int from) {
        for (int k = from; k < call.n; ++k) g.W32(call.sp + 16u + 4u * U(k - 4), a[k]);
    };
    switch (call.fn) {
    case pair::kBikeVsBike: ok = BikeVsBike(g, a[0], a[1], S(a[2]), call.sp, t, c, v0); return true;
    case pair::kClassify:
        stackArgs(4);
        v0 = U(BikePairClassify(g, a[0], a[1], a[2], a[3], call.sp));
        ok = !g.Faulted();
        return true;
    case pair::kResponseFar: stackArgs(4); ok = ResponseFar(g, a[0], a[1], a[2], a[3], call.sp, t, v0); return true;
    case pair::kViewExtent:
        ViewExtent(g, a[0], a[1], a[2]);
        v0 = 0;
        ok = !g.Faulted();
        return true;
    case pair::kResponse: stackArgs(4); ok = Response(g, a[0], a[1], a[2], a[3], call.sp, t, c, v0); return true;
    case pair::kHeavyResponse:
        stackArgs(4);
        ok = HeavyResponse(g, a[0], a[1], a[2], a[3], call.sp, t, v0);
        return true;
    case pair::kPairPushDir:
        PairPushDir(g, a[0], a[1], a[2], t);
        v0 = 0;
        ok = !g.Faulted();
        return true;
    default: return false;
    }
}

} // namespace rr::sim
