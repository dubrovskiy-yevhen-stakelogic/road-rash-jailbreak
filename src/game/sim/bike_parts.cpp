#include "game/sim/bike_parts.h"

#include "game/sim/fixed.h"
#include "game/sim/integrator.h"

namespace rr::sim {
namespace {

inline uint32_t U(int32_t v) { return static_cast<uint32_t>(v); }
inline int32_t S(uint32_t v) { return static_cast<int32_t>(v); }
inline int32_t Mul(int32_t a, int32_t b) { return S(U(a) * U(b)); }     // `mult`, lo
inline int32_t Hi(int32_t a, int32_t b) {                                // `mult`, hi
    return static_cast<int32_t>((static_cast<int64_t>(a) * static_cast<int64_t>(b)) >> 32);
}
constexpr uint32_t kSinCos = 0x8005624C;

} // namespace

// ============================================================================ SLUS 0x8001FD24
void RotMatrix(GuestRam& g, int32_t a, int32_t b, int32_t c, uint32_t out) {
    const uint32_t ia = kSinCos + 4u * (U(a) & 0xFFFu), ib = kSinCos + 4u * (U(b) & 0xFFFu);
    const uint32_t ic = kSinCos + 4u * (U(c) & 0xFFFu);
    const int32_t sA = g.S16(ia), cA = g.S16(ia + 2u), sB = g.S16(ib), cB = g.S16(ib + 2u);
    const int32_t sC = g.S16(ic), cC = g.S16(ic + 2u);
    const int32_t sAsB = Mul(sA, sB) >> 12, cCcA = Mul(cC, cA) >> 12, sCcA = Mul(sC, cA) >> 12;
    auto h = [&](uint32_t k, int32_t v) { g.W16(out + 2u * k, static_cast<uint16_t>(U(v))); };
    h(6, S(0u - U(sB)));
    h(7, Mul(sA, cB) >> 12);
    h(8, Mul(cA, cB) >> 12);
    h(1, S(U(Mul(sAsB, cC) >> 12) - U(sCcA)));
    h(0, Mul(cC, cB) >> 12);
    h(4, S(U(cCcA) + U(Mul(sAsB, sC) >> 12)));
    h(3, Mul(cB, sC) >> 12);
    h(2, S(U(Mul(sA, sC) >> 12) + U(Mul(cCcA, sB) >> 12)));
    h(5, S(U(Mul(sCcA, sB) >> 12) - U(Mul(sA, cC) >> 12)));
}

// ============================================================================ RASHCDG 0x80085224
void RiderInstanceStores(GuestRam& g, uint32_t r) {
    g.W32(r + 12u, U(g.S32(r + 184u) >> 10));                          // 0x80085238
    g.W32(r + 20u, U(g.S32(r + 192u) >> 10));                          // 0x80085244
    g.W32(r + 16u, U(g.S32(r + 188u) >> 10));                          // 0x80085250
    const uint32_t m = (g.S8(r + 72u) == 3) ? r + 104u : g.U32(r + 4u) + 4u; // 0x80085258
    g.W16(m + 0u, g.U16(r + 432u));
    g.W16(m + 6u, g.U16(r + 434u));
    g.W16(m + 12u, g.U16(r + 436u));
    g.W16(m + 2u, g.U16(r + 438u));
    g.W16(m + 8u, g.U16(r + 440u));
    g.W16(m + 14u, g.U16(r + 442u));
    g.W16(m + 4u, g.U16(r + 444u));
    g.W16(m + 10u, g.U16(r + 446u));
    g.W16(m + 16u, g.U16(r + 448u));                                   // 0x800852D4, the jal's delay slot
}

// ============================================================================ RASHCDG 0x80084E10
bool BikeInstance(GuestRam& g, uint32_t e, uint32_t a1, uint32_t sp, const uint16_t* asinTable, BikePartCallees& c) {
    const uint32_t F = sp - kBikeInstanceFrame;
    const uint32_t s1 = g.U32(e + 556u);
    g.W32(e + 12u, U(g.S32(e + 184u) >> 10));
    g.W32(e + 20u, U(g.S32(e + 192u) >> 10));
    g.W32(e + 16u, U(g.S32(e + 188u) >> 10));
    const uint32_t m = (g.S8(e + 72u) == 3) ? e + 104u : g.U32(e + 4u) + 4u;
    g.W16(m + 0u, g.U16(e + 432u));
    g.W16(m + 6u, g.U16(e + 434u));
    g.W16(m + 12u, g.U16(e + 436u));
    g.W16(m + 2u, g.U16(e + 438u));
    g.W16(m + 8u, g.U16(e + 440u));
    g.W16(m + 14u, g.U16(e + 442u));
    g.W16(m + 4u, g.U16(e + 444u));
    g.W16(m + 10u, g.U16(e + 446u));
    g.W16(m + 16u, g.U16(e + 448u));
    if (g.S8(g.U32(0x8005B2F8u)) != 3 && g.S8(e + 8u) < 2) {
        RotMatrix(g, 0, g.S16(e + 826u), 0, g.U32(e + 4u) + 28u);          // 0x80084F18: the fork
        int32_t a2 = 0;
        if (!(g.U32(e + 568u) & 0x600u)) {
            if (g.U32(e + 612u) != 0) {                                     // 0x80084F44
                const int32_t v1 = g.S32(e + 616u);
                const int32_t v0 = g.S16(e + 846u);
                if (v1 > 0 || v0 > 0) a2 = S(g.U16(s1 + 438u));
                else if (v1 < 0 || v0 < 0) a2 = S(g.U16(s1 + 436u));
                else a2 = 0;
            } else {                                                        // 0x80084F8C
                int32_t as = 0;
                if (!Asin(S(U(static_cast<int32_t>(g.S16(e + 530u))) << 4), asinTable, as)) return false;
                const int32_t a3 = S(g.U16(s1 + 436u));
                int32_t ha = S(g.U16(s1 + 438u));
                int32_t s0 = S((U(as) - U(static_cast<int32_t>(g.S16(e + 842u)))) << 1);
                const int32_t na = S(0u - U(a3));
                int32_t v1 = S(U(S(U(s0) - U(na)) >> 31) & (U(na) - U(s0)));
                v1 = S(U(s0) + U(v1));
                const int32_t d = S(U(ha) - U(s0));
                const int32_t v0 = S(U(d >> 31) & U(d));
                s0 = S(U(v1) + U(v0));
                const uint32_t fb = g.U32(e + 564u);
                int32_t a0;
                if (fb & 0x20u) {
                    a0 = ha;
                } else if (fb & 0x40u) {
                    a0 = na;
                } else {
                    const int32_t r = g.S32(e + 608u);
                    if (r > 0) a0 = FixMul(r, S(U(ha) << 16)) >> 16;
                    else if (r >= 0) a0 = 0;
                    else a0 = FixMul(r, S(U(a3) << 16)) >> 16;
                }
                if ((s0 ^ a0) < 0) a2 = a0;                                 // 0x80085020
                else if (a0 > 0) a2 = (a0 < s0) ? s0 : a0;
                else a2 = (s0 < a0) ? s0 : a0;
            }
        }
        // 0x80085064: the pitch spring
        const int32_t t = FixMul(0x3C000, S(U(a2 - g.S16(e + 846u)) << 16));
        const int32_t damp = FixMul(22937, S(U(static_cast<int32_t>(g.S16(e + 844u))) << 16));
        const int32_t step = S(U(t) - U(damp)) >> 16;
        const uint16_t v844 = static_cast<uint16_t>(g.U16(e + 844u) + U(step));
        g.W16(e + 844u, v844);
        const int32_t sv = static_cast<int16_t>(v844);
        const int32_t q = S(U(Hi(sv, 0x66666667) >> 3) - U(sv >> 31));
        g.W16(e + 846u, static_cast<uint16_t>(g.U16(e + 846u) + U(q)));
        int32_t roll = 0;
        const uint32_t pass = g.U32(e + 856u);
        if (pass != 0) {                                                    // 0x800850D8: the passenger
            int32_t v0 = 0;
            if (!(g.U32(e + 568u) & 0x700u)) {
                if (g.S32(e + 636u) > 0) {
                    const int32_t x = g.S32(e + 488u);
                    v0 = (x < 0 ? S(U(x) + 3u) : x) >> 2;
                } else {
                    const int32_t x = S(0u - g.U32(e + 636u));
                    v0 = S(U(Hi(x, 0x55555556)) - U(x >> 31));
                }
            }
            const uint32_t p = g.U32(e + 856u);
            const int32_t s0 = FixMul(0x3C000, S(U(v0) - g.U32(p + 656u)));
            const int32_t d2 = FixMul(16384, g.S32(p + 660u));
            const int32_t v660 = S(g.U32(p + 660u) + (U(s0) - U(d2)));
            g.W32(p + 660u, U(v660));
            const int32_t q2 = S(U(Hi(v660, 0x66666667) >> 3) - U(v660 >> 31));
            const int32_t a = S(g.U32(p + 656u) + U(q2));
            int32_t a0 = S(U(a) + (U(S(U(a) + 13725u) >> 31) & (U(-13725) - U(a))));
            const int32_t hi = S(13725u - U(a));
            a0 = S(U(a0) + (U(hi >> 31) & U(hi)));
            roll = S(0u - U(S(U(a0) * 652u) >> 16));
            g.W32(p + 656u, U(a0));
        }
        RotMatrix(g, g.S16(e + 846u), 0, roll, g.U32(e + 4u) + 52u);       // 0x800851F4
    }
    if (!c.Call(kBikeDrawFn, e, a1, F)) return false;                        // 0x80085200
    return !g.Faulted();
}

// ============================================================================ RASHCDG 0x80066EC4
bool WheelSlots(GuestRam& g, uint32_t e) {
    const uint32_t parts = g.U16(g.U32(e) + 24u);
    if (parts < 5u) return false;
    const bool six = !(parts < 6u);
    RotMatrix(g, g.S16(e + 836u), 0, 0, g.U32(e + 4u) + 76u);                // the wheels
    RotMatrix(g, g.S16(e + 838u), 0, 0, g.U32(e + 4u) + 100u);
    if (six) RotMatrix(g, g.S16(g.U32(e + 856u) + 836u), 0, 0, g.U32(e + 4u) + 124u);
    return six;
}

bool BikeWheels(GuestRam& g, uint32_t e, uint32_t sp, BikePartCallees& c) {
    const uint32_t F = sp - kBikeWheelsFrame;
    if (g.U16(e + 172u) < g.U32(g.U32(0x8005B2F8u) + 48u)) {                  // a player's: the stack top
        const uint32_t top = g.U32(0x1F8002BCu);
        for (uint32_t k = 0; k < 8; ++k) g.W32(0x800CCD80u + 4u * k, g.U32(top + 4u * k));
    }
    const uint32_t parts = g.U16(g.U32(e) + 24u);
    if (parts < 5u) {
        if (parts != 1u)
            if (!c.Call(kPartProgramFn, 5u, e, F)) return false;
    } else {
        const uint32_t six = WheelSlots(g, e) ? 1u : 0u;                    // 0x80066F6C..0x80066FB8
        if (!c.Call(kPartProgramFn, 4u - six, e, F)) return false;
    }
    for (uint32_t i = 0; i < 2; ++i)
        if (g.U32(e + 56u + 8u * i) != 0)
            if (!c.Call(kAttachChildFn, e, i, F)) return false;
    if (g.U16(g.U32(e) + 24u) != 1u) g.W32(0x1F8002BCu, g.U32(0x1F8002BCu) - 32u);
    g.W32(0x1F8002BCu, g.U32(0x1F8002BCu) - 32u);
    return !g.Faulted();
}

// ============================================================================ RASHCDG 0x800667C4
bool LodChoice(GuestRam& g, uint32_t e, int32_t players) {
    const uint32_t t3 = (g.U16(g.U32(e) + 14u) & 0x78u) >> 3;
    int32_t p = 0;
    do {                                                                    // at least once (0x800668BC)
        const int32_t d = g.S32(e + 44u + 4u * U(p));
        const uint32_t tbl = g.U32(e + 100u);
        if (tbl == 0) { ++p; continue; }
        const int32_t cnt = S(g.U8(g.U32(e + 96u) + 4u));
        int32_t i = g.S8(e + 10u + U(p));
        if (i < cnt - 1 && g.S32(tbl + 8u * U(i)) < d) {                     // 0x80066814: walk up
            uint32_t v1 = tbl + 8u * U(i);
            int32_t v0;
            do {
                v1 += 8u;
                v0 = g.S32(v1);
                ++i;
            } while (v0 < d);
        } else if (i != 0) {                                                // 0x80066850: walk down
            uint32_t v1 = g.U32(e + 100u) + 8u * U(i);
            if (d < g.S32(v1 + 4u)) {
                int32_t v0;
                do {
                    v1 -= 8u;
                    v0 = g.S32(v1 + 4u);
                    --i;
                } while (d < v0);
            }
        }
        const int32_t mx = S(g.U8(g.U32(e + 96u) + 4u)) - 1;
        g.W8(e + 10u + U(p), static_cast<uint8_t>(U(i < mx ? i : mx)));
        if (g.Faulted()) return false;
        ++p;
    } while (p < players);
    int32_t lod;
    if (players < 2) {
        lod = g.S8(e + 10u);
    } else {
        const uint8_t b = (g.S8(e + 10u) < g.S8(e + 11u)) ? g.U8(e + 10u) : g.U8(e + 11u);
        lod = static_cast<int8_t>(b);
    }
    if (t3 == 1u || t3 == 4u) {
        uint32_t interp = 0;
        if (lod == 0) {
            interp = 1;
        } else if (lod == 1 && t3 == 1u) {
            if (S(g.U16(e + 172u) & 31u) < g.S32(g.U32(0x8005B2F8u) + 48u)) interp = 1;
            else if (g.U8(e + 572u) & 0x20u) interp = 1;
        }
        const uint32_t v1 = (g.U16(g.U32(e) + 14u) & 0x78u) >> 3;
        const uint32_t a = g.U32(e + 540u);
        if (v1 == 1u || v1 == 4u) {
            const uint32_t idx = (v1 == 4u) ? U(lod + 1) : U(lod);
            g.W32(a + 1760u, g.U32(0x80052390u + 4u * idx));
            g.W32(a + 36u, (g.U32(a + 36u) & ~4u) | (interp << 2));
        }
    }
    g.W8(e + 9u, static_cast<uint8_t>(g.U8(e + 9u) & 0xF7u));
    return !g.Faulted();
}

} // namespace rr::sim
