#include "game/sim/camera_director.h"

#include "game/sim/fixed.h"
#include "game/sim/recover.h"

namespace rr::sim {
namespace {

inline uint32_t U(int32_t v) { return static_cast<uint32_t>(v); }
inline int32_t S(uint32_t v) { return static_cast<int32_t>(v); }

constexpr uint32_t kSetA = 828; // +0x33C: the eye knots (x, y, z at +0, +24, +48; the second triple +72..+120)
constexpr uint32_t kSetB = 972; // +0x3CC: the look knots

// One (type, index) pair at `p` loaded into knot `slot` of set `set`: the point's first triple
// shifted by `sh`, the second (a type 0/1 point's) by 8, or zeros.
void Load(GuestRam& g, uint32_t v, uint32_t p, uint32_t slot, uint32_t set, int sh) {
    const int32_t type = g.S8(p), idx = g.S8(p + 1);
    const int32_t base = g.S16(kShotTableAddr + 524u + U(type) * 2u);
    const int32_t k = S(U(base) + (type < 2 ? U(idx) * 2u : U(idx)));
    const uint32_t a0 = kShotTableAddr + 532u + U(k) * 6u;
    const uint32_t w = v + 4u * slot + set;
    g.W32(w + 0u, U(g.S16(a0 + 0u)) << sh);
    g.W32(w + 24u, U(g.S16(a0 + 2u)) << sh);
    g.W32(w + 48u, U(g.S16(a0 + 4u)) << sh);
    if (g.U8(p) < 2u) {
        g.W32(w + 72u, U(g.S16(a0 + 6u)) << 8);
        g.W32(w + 96u, U(g.S16(a0 + 8u)) << 8);
        g.W32(w + 120u, U(g.S16(a0 + 10u)) << 8);
    } else {
        g.W32(w + 120u, 0);
        g.W32(w + 96u, 0);
        g.W32(w + 72u, 0);
    }
}
void ZeroSetB(GuestRam& g, uint32_t w) {
    for (uint32_t o : {1092u, 1068u, 1044u, 1020u, 996u, 972u}) g.W32(w + o, 0);
}
bool Pos(int32_t t) { return t == 0 || t == 2; }
bool Look(int32_t t) { return t == 1 || t == 3; }

} // namespace

// ============================================================================ RASHCDG 0x800852E8
bool KnotReverse(GuestRam& g, uint32_t v) {
    const int32_t n = g.S32(v + 800u);
    int32_t i = 0;
    if (n > 0) {
        const int32_t bound = n;
        for (; i < bound; ++i) {
            const int32_t w = g.S32(v + 828u + 4u * U(i));
            if (w != 0 && w >= 0) break;                                    // 0x80085308 / 0x80085310
        }
        if (i < g.S32(v + 800u)) return !g.Faulted();                       // a positive knot: nothing
    }
    const int32_t cnt = g.S32(v + 800u);
    const int32_t a1 = cnt - 1;
    const uint32_t last = v + 4u * U(a1);
    int32_t a2;
    if (g.S32(last + 828u) == 0) {
        a2 = cnt - 2;
        g.W32(last + 1020u, 0);
        g.W32(last + 996u, 0);
    } else {
        a2 = a1;
    }
    int32_t a3 = a1 + 1;
    if (a2 >= 0) {
        uint32_t dst = v + 4u * U(a3), src = v + 4u * U(a2);
        do {                                                                // 0x80085388
            g.W32(dst + 828u, 0u - g.U32(src + 828u));
            g.W32(dst + 852u, g.U32(src + 852u));
            g.W32(dst + 996u, 0u - g.U32(src + 996u));
            --a2;
            g.W32(dst + 876u, g.U32(src + 876u));
            ++a3;
            g.W32(dst + 1020u, 0u - g.U32(src + 1020u));
            src -= 4u;
            dst += 4u;
        } while (a2 >= 0);
    }
    g.W32(v + 800u, U(a3));
    return !g.Faulted();
}

// ============================================================================ RASHCDG 0x800853E4
bool ShotSetup(GuestRam& g, uint32_t v, uint32_t script) {
    const uint32_t a3 = g.U32(v + 548u) | 6u;
    g.W32(v + 548u, a3);
    const int32_t mode = g.S8(script + 1u);
    uint32_t p = script + 2u;
    uint32_t t0 = 0;
    if (mode == 0) {                                                        // 0x8008544C: a track
        if (Pos(g.S8(p))) { Load(g, v, p, 0, kSetA, 8); p += 2u; }
        else g.W32(v + 548u, a3 & ~4u);
        if (Look(g.S8(p))) { Load(g, v, p, 0, kSetB, 8); p += 2u; }
        else ZeroSetB(g, v);
        if (g.S8(p) == 5) { g.W32(v + 804u, U(g.S8(p + 1u)) << 16); p += 2u; }
        else g.W32(v + 804u, 0);
        while (Pos(g.S8(p))) {                                              // 0x80085674
            ++t0;
            Load(g, v, p, t0, kSetA, 8);
            p += 2u;
            if (g.S8(p) == 5) { g.W32(v + 4u * t0 + 804u, U(g.S8(p + 1u)) << 16); p += 2u; }
        }
        if (g.S8(p - 2u) == 5) {                                            // 0x80085788
            ++t0;
            if (Look(g.S8(p))) { Load(g, v, p, t0, kSetB, 8); p += 2u; }
            else ZeroSetB(g, v + 4u * t0);
            if (Pos(g.S8(p))) {
                Load(g, v, p, t0, kSetA, 8);
            } else {                                                        // 0x8008598C: the chase camera's
                const uint32_t w = v + 4u * t0;                             // own placement (CAMERA.CA)
                g.W32(w + 924u, 0);
                g.W32(w + 900u, 0);
                g.W32(w + 828u, 0);
                const uint32_t r = 0x800CD7B8u + 56u * g.U32(v + 544u);
                g.W32(w + 852u, g.U32(r + 12u));
                g.W32(w + 876u, g.U32(r + 4u));
                g.W32(w + 948u, g.U32(r + 20u));
            }
        }
        g.W32(v + 800u, t0 + 1u);
        g.W32(v + 540u, 11u);
    } else if (mode == 1) {                                                 // 0x800859F4: a pan
        if (Pos(g.S8(p))) {
            Load(g, v, p, 0, kSetA, 8);
            t0 = 1;
            p += 2u;
        } else {
            g.W32(v + 548u, a3 & ~4u);
            if (g.S8(script + 2u) == 4) {
                g.W32(v + 1116u, U(g.S8(script + 3u)) << 16);
                p = script + 4u;
            }
        }
        if (Look(g.S8(p))) { Load(g, v, p, t0, kSetB, 8); p += 2u; }
        else ZeroSetB(g, v);
        while (Pos(g.S8(p))) {                                              // 0x80085C20
            Load(g, v, p, t0, kSetA, 8);
            p += 2u;
            ++t0;
        }
        --t0;                                                               // 0x80085D18
        if (Look(g.S8(p))) {
            ++t0;
            Load(g, v, p, t0, kSetB, 8);
            p += 2u;
            if (Pos(g.S8(p))) Load(g, v, p, t0, kSetA, 8);
            else --t0;
        } else {
            ZeroSetB(g, v + 4u * t0);                                       // 0x80085EEC
            if (g.S8(p) == 4) g.W32(v + 1116u, U(g.S8(p + 1u)) << 16);
        }
        g.W32(v + 800u, t0 + 1u);
        if (!KnotReverse(g, v)) return false;                               // 0x80085F30
        g.W32(v + 540u, 12u);
    } else if (mode == 2) {                                                 // 0x80085F44: scripted (13)
        if (Pos(g.S8(p))) { Load(g, v, p, 0, kSetA, 16); p += 2u; }
        if (Look(g.S8(p))) { Load(g, v, p, 0, kSetB, 8); p += 2u; }
        else ZeroSetB(g, v);
        if (g.S8(p) == 5) { g.W32(v + 804u, U(g.S8(p + 1u)) << 16); p += 2u; }
        else g.W32(v + 804u, 0);
        while (Pos(g.S8(p))) {                                              // 0x8008615C
            ++t0;
            Load(g, v, p, t0, kSetA, 16);
            const uint32_t w = v + 4u * t0;
            g.W32(w + 828u, g.U32(w + 828u) - g.U32(v + 828u));
            g.W32(w + 852u, g.U32(w + 852u) - g.U32(v + 852u));
            g.W32(w + 876u, g.U32(w + 876u) - g.U32(v + 876u));
            p += 2u;
            if (g.S8(p) == 5) { g.W32(w + 804u, U(g.S8(p + 1u)) << 16); p += 2u; }
        }
        g.W32(v + 800u, t0 + 1u);
        g.W32(v + 4u * g.U32(v + 800u) + 828u, g.U32(v + 828u));
        g.W32(v + 4u * g.U32(v + 800u) + 852u, g.U32(v + 852u));
        g.W32(v + 4u * g.U32(v + 800u) + 876u, g.U32(v + 876u));
        g.W32(v + 876u, 0);
        g.W32(v + 852u, 0);
        g.W32(v + 828u, 0);
        g.W32(v + 540u, 13u);
    } else if (mode == 3) {                                                 // 0x80086304: scripted (14)
        g.W32(v + 540u, 14u);
        Load(g, v, p, 0, kSetA, 8);
        g.W32(v + 800u, 2u);
    }
    return !g.Faulted();
}

// ============================================================================ SLUS 0x8002F4D8
bool SplineCoef(GuestRam& g, int32_t y0, int32_t y1, int32_t y2, int32_t h0, uint32_t sp) {
    const int32_t h1 = g.S32(sp + 16u);
    int32_t q;                                                              // h1 / h0, signed
    if (h1 <= 0) {
        if (h0 <= 0) q = S(FixDiv(0u - U(h1), 0u - U(h0)));
        else q = S(0u - FixDiv(0u - U(h1), U(h0)));
    } else {
        if (h0 <= 0) q = S(0u - FixDiv(U(h1), 0u - U(h0)));
        else q = S(FixDiv(U(h1), U(h0)));
    }
    const int32_t r = rc::Recip(q);                                         // 0x8002F558
    const int32_t a = FixMul(y0, q);
    const int32_t b = FixMul(y1, S(U(r) - U(q)));
    const int32_t c = FixMul(y2, r);
    const int32_t s0 = S(U(a) + U(b) - U(c));
    g.W32(g.U32(sp + 20u), U(s0) * 3u);
    g.W32(g.U32(sp + 24u), U(h1));
    g.W32(g.U32(sp + 28u), (U(h0) + U(h1)) << 1);
    g.W32(g.U32(sp + 32u), U(h0));
    return !g.Faulted();
}

// ============================================================================ SLUS 0x8002F634
bool SplineSlopes(GuestRam& g, uint32_t x, uint32_t y, uint32_t out, int32_t n, uint32_t sp) {
    const uint32_t F = sp - kSplineSlopesFrame;
    g.W32(sp + 12u, U(n));                                                  // the o32 home slots
    g.W32(sp + 8u, out);
    g.W32(F + 44u, 0x10000u);                                               // A[1] = 1.0
    g.W32(F + 84u, 0);                                                      // B[1]
    g.W32(F + 80u, 0);                                                      // B[0]
    g.W32(F + 40u, 0);                                                      // A[0]
    int32_t s4 = 0;
    for (int32_t k = 1; k < n; ++k) {
        const uint32_t K = U(k);
        g.W32(F + 16u, g.U32(x + 4u * K));                                  // the stack arguments
        g.W32(F + 20u, F + 120u);
        g.W32(F + 24u, F + 124u);
        g.W32(F + 28u, F + 128u);
        g.W32(F + 32u, F + 132u);
        if (!SplineCoef(g, g.S32(y + 4u * K - 4u), g.S32(y + 4u * K), g.S32(y + 4u * K + 4u),
                        g.S32(x + 4u * K - 4u), F))
            return false;
        const int32_t r124 = g.S32(F + 124u), r128 = g.S32(F + 128u);
        const int32_t s1 = S(U(FixMul(r124, g.S32(F + 40u + 4u * (K - 1u)))) + U(FixMul(r128, g.S32(F + 40u + 4u * K))));
        const int32_t s0 = S(U(g.S32(F + 120u)) + U(FixMul(r124, g.S32(F + 80u + 4u * (K - 1u)))) +
                             U(FixMul(r128, g.S32(F + 80u + 4u * K))));
        if (k == n - 1) {                                                   // 0x8002F77C: the last knot
            const int32_t e = FixMul(g.S32(F + 132u), g.S32(out + 4u * K + 4u));
            const int32_t num = S(U(s0) + U(e));
            if (num > 0) {
                if (s1 > 0) s4 = S(0u - FixDiv(U(num), U(s1)));
                else s4 = S(FixDiv(U(num), 0u - U(s1)));
            } else {
                if (s1 > 0) s4 = S(FixDiv(0u - U(num), U(s1)));
                else s4 = S(0u - FixDiv(0u - U(num), 0u - U(s1)));
            }
        } else {                                                            // 0x8002F838
            const int32_t rc = rc::Recip(g.S32(F + 132u));
            g.W32(F + 132u, U(rc));
            g.W32(F + 40u + 4u * (K + 1u), 0u - U(FixMul(s1, rc)));
            g.W32(F + 80u + 4u * (K + 1u), 0u - U(FixMul(s0, g.S32(F + 132u))));
        }
    }
    g.W32(out + 4u, U(s4));                                                 // 0x8002F908
    for (int32_t k = 2; k < n; ++k)
        g.W32(out + 4u * U(k), U(FixMul(g.S32(F + 40u + 4u * U(k)), s4)) + g.U32(F + 80u + 4u * U(k)));
    return !g.Faulted();
}

} // namespace rr::sim
