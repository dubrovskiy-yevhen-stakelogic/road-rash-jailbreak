#include "game/sim/ped_hit.h"

#include "game/sim/fixed.h"
#include "game/sim/vec.h"

namespace rr::sim {
namespace {

using rc::S;
using rc::U;

int32_t GDot(GuestRam& g, uint32_t a, uint32_t b) {
    int16_t x[3], y[3];
    for (uint32_t k = 0; k < 3; ++k) {
        x[k] = g.S16(a + 2u * k);
        y[k] = g.S16(b + 2u * k);
    }
    return DotLcm(x, y);
}
inline int32_t Iabs(int32_t v) {
    const uint32_t s = U(v >> 31);
    return S((U(v) + s) ^ s);
}
// `(s16)x` of the low halfword, as `sll 16; sra 16` leaves it.
inline int32_t Sx16(uint32_t v) { return static_cast<int16_t>(static_cast<uint16_t>(v)); }

} // namespace

bool PedHit(GuestRam& g, uint32_t e, uint32_t n, int32_t speed, uint32_t n2, uint32_t other, uint32_t force,
            uint32_t sp, const BikeTables& t, RecoverCallees& c, uint32_t& v0) {
    const uint32_t F = sp - kPedHitFrame;
    uint32_t s3 = other;
    const uint32_t s1 = ((g.U16(e + 172u) >> 5) == 1u) ? e : 0u;           // 0x800A98B8
    bool s7 = false, s5 = false;
    uint32_t s8 = 0;
    const uint32_t v1in = g.U32(e + 552u);
    const int32_t oldSpeed = g.S32(e + 480u);                                // F+24
    bool gentle = !(v1in & 0x40000000u) && !(0x23C36 < speed) && force == 0;
    if (gentle) {
        // 0x800A9EB0
        bool stop = false;
        if (s1 == 0 || (v1in & 0x20000000u)) {
            stop = true;
        } else {
            const uint32_t v1 = g.U32(s1 + 552u);
            if (v1 & 0x800u) {
                g.W32(s1 + 552u, (v1 & 0xFFEFFEFFu) | 0x80u);
                stop = true;
            } else if (!(U(g.U16(s1 + 544u)) - 69u < 2u) && !(v1 & 0x100018u) && GDot(g, n, s1 + 450u) < 0) {
                g.W32(s1 + 552u, g.U32(s1 + 552u) | 0x100001u);
                int32_t rate;
                if (s3 == s1 + 172u) {
                    rate = S(FixDiv(0x40000u, 0x80000u));                     // 0x800A9F68
                } else {
                    const int32_t sum = S(U(g.S32(e + 304u)) + U(g.S32(e + 308u)));
                    const int32_t a0 = S(U(sum) + (U(sum) >> 31)) >> 1;
                    if (a0 > 0) rate = S(FixDiv(U(a0), 0x80000u));
                    else rate = S(0u - FixDiv(U(0 - a0), 0x80000u));
                }
                g.W32(s1 + 600u, U(rate));
                const int32_t d = GDot(g, n, s1 + 516u);                     // 0x800A9F90
                g.W16(s1 + 516u, g.U16(n + 0u));
                g.W16(s1 + 518u, g.U16(n + 2u));
                const uint16_t nz = g.U16(n + 4u);
                uint32_t f = g.U32(s1 + 552u) & 0xFFF9FFFFu;
                g.W32(s1 + 552u, f);
                g.W16(s1 + 520u, nz);
                if (d < 0) {
                    g.W16(s1 + 516u, static_cast<uint16_t>(0u - g.U16(s1 + 516u)));
                    g.W16(s1 + 520u, static_cast<uint16_t>(0u - nz));
                    g.W16(s1 + 518u, static_cast<uint16_t>(0u - g.U16(s1 + 518u)));
                    g.W32(s1 + 552u, g.U32(s1 + 552u) | 0x40000u);
                } else {
                    g.W32(s1 + 552u, f | 0x20000u);
                }
                g.W32(s1 + 488u, 0);
            }
        }
        if (stop) s5 = true;                                                 // 0x800AA00C
    } else if (!((U(g.U16(e + 544u)) - 60u < 4u) && force == 0)) {
        // 0x800A9928
        if (g.U16(0x800541D4u + 8u * g.U16(e + 544u) + 2u) == 5u)
            if (!rc::Call(c, 0x80091468u, {s1}, F)) return false;           // RiderLaunch
        s8 = 1;
        if (g.U32(e + 552u) & 0x40000000u) {                                 // flying
            uint32_t r = 0;
            if (!rc::Call(c, 0x800A966Cu, {e, n, 0x80000u}, F, &r)) return false;
            if (r == 2u) {
                if (g.U16(e + 544u) != 43u)
                    if (!rc::Call(c, 0x800C4550u, {43u, e, 3u}, F)) return false;
            } else if (r == 0u) {
                s8 = 0;
            }
        } else {
            // 0x800A99BC: the running clip
            const uint32_t a1 = g.U32(e + 540u);
            const uint32_t op = g.U32(a1 + 12u);
            const uint32_t bank = g.U32(a1 + 40u);
            const uint32_t clip = g.U8(g.U32(a1 + 4u) + 12u * op);
            const uint32_t frames = g.U16(g.U32(g.U32(bank + 4u) + 4u * clip) + 16u);
            s3 = g.U32(a1 + 16u);
            const uint32_t fm1 = frames - 1u;
            int path = 0;                                                    // 1: 0x800A9A4C, 2: 0x800A9AD4
            const uint32_t st = g.U16(e + 544u);
            if (force != 0 || st == 67u || st == 68u) {
                path = 1;
            } else if (st - 69u < 2u) {
                const uint32_t v = fm1 << 16;
                const int32_t half = S(U(S(v) >> 16) + (v >> 31)) >> 1;
                path = (S(s3) < half) ? 1 : 2;
            } else {
                path = 2;
            }
            if (path == 1) {
                bool stance = true;
                if (U(g.U16(e + 544u)) - 67u < 2u) {                        // 0x800A9A54
                    uint32_t done = 0;
                    if (!rc::Call(c, 0x8005BE58u, {g.U32(e + 540u)}, F, &done)) return false;
                    if (done == 0) stance = false;
                }
                if (stance) {
                    const uint32_t s = g.U16(e + 544u);
                    const uint32_t a0 = (s == 70u || s == 51u || s == 45u || s == 61u || s == 62u || s == 68u ||
                                         s == 57u || s == 53u) ? 1u : 0u;
                    if (!rc::Call(c, 0x800C4550u, {a0 + 67u, e, 2u}, F)) return false;
                }
                s5 = true;                                                   // 0x800A9B58
            } else {
                if (0x8F0D8 < speed) {                                       // 0x800A9ADC
                    s7 = true;
                } else {
                    s5 = true;                                               // the delay slot's li s5,1
                    if (!(g.U32(e + 552u) & 0x20000000u)) {
                        const int32_t d = GDot(g, n2, e + 444u);
                        const int32_t ad = Iabs(d);
                        uint32_t a0;
                        if (d > 0) a0 = (ad < 16384) ? 61u : 60u;
                        else a0 = (ad < 16384) ? 63u : 62u;
                        if (!rc::Call(c, 0x800C4550u, {a0, e, 2u}, F)) return false;
                    }
                }
            }
        }
        if (s7) {                                                            // 0x800A9B64: the launch
            const bool fwd = 0 < GDot(g, n2, e + 444u);
            if (!(g.U32(e + 552u) & 0x20000000u)) {
                int32_t b[3], o[3];
                int16_t d[3];
                for (uint32_t k = 0; k < 3; ++k) {
                    b[k] = g.S32(e + 184u + 4u * k);
                    d[k] = g.S16(e + 438u + 2u * k);
                }
                MulAdd(b, d, S(0xFFFF0000u), o);                             // 0x800A9B90
                for (uint32_t k = 0; k < 3; ++k) g.W32(e + 184u + 4u * k, U(o[k]));
            }
            g.W32(e + 480u, U(FixMul(0x11999, speed)));                       // 0x800A9BA0
            const uint32_t r = GuestRand(g);                                 // 0x800A9BA8
            const uint32_t q = static_cast<uint32_t>((static_cast<uint64_t>(r) * 0x16F26017u) >> 32) >> 6;
            const int32_t j = Sx16(r - 714u * q - 357u);
            const int32_t lo1 = S(U(j) * U(static_cast<int32_t>(g.S16(n2 + 4u))));
            g.W16(e + 450u, static_cast<uint16_t>(g.U16(n2 + 0u) + U(lo1 >> 12)));
            g.W16(e + 452u, g.U16(n2 + 2u));
            const int32_t lo2 = S(U(j) * U(static_cast<int32_t>(g.S16(n2 + 0u))));
            const int32_t ay = S(U(static_cast<int32_t>(g.S16(e + 452u))) << 4);
            g.W16(e + 454u, static_cast<uint16_t>(g.U16(n2 + 4u) - U(lo2 >> 12)));
            int32_t as = 0;
            if (!Asin(ay, t.asin, as)) return false;                         // 0x800A9C40
            const uint32_t idx = (1365u - U(as)) & 0xFFFu;
            const int32_t cosv = g.S16(kRcSinCos + 4u * idx + 2u);
            if (!rc::Call(c, 0x8007E868u, {e + 450u, g.U32(e + 480u), U(cosv), 0x8DC28u}, F)) return false;
            {
                int16_t d[3];
                int32_t o[3];
                for (uint32_t k = 0; k < 3; ++k) d[k] = g.S16(e + 450u + 2u * k);
                Scale(g.S32(e + 480u), d, o);                                // 0x800A9C8C
                for (uint32_t k = 0; k < 3; ++k) g.W32(e + 456u + 4u * k, U(o[k]));
            }
            // 0x800A9C94: the rows - +0x1BC = +-heading, +0x1B0 = (+-hz, 0, -row2.x), Normalize, +0x1B6 = OP
            const uint32_t mask = fwd ? 0xFFFFFFFFu : 0u;
            auto sel = [&](uint16_t h) {
                const uint32_t a1 = 0u - U(static_cast<int32_t>(static_cast<int16_t>(h)));
                const uint32_t v = U(static_cast<int32_t>(static_cast<int16_t>(h))) - a1;
                return static_cast<uint16_t>(a1 + (mask & v));
            };
            const uint16_t hx = g.U16(e + 450u), hy = g.U16(e + 452u);
            g.W16(e + 434u, 0);
            g.W16(e + 444u, sel(hx));
            g.W16(e + 446u, sel(hy));
            const uint16_t r2x = g.U16(e + 444u);
            const uint16_t hz = sel(g.U16(e + 454u));
            g.W16(e + 448u, hz);
            g.W16(e + 432u, hz);
            g.W16(e + 436u, static_cast<uint16_t>(0u - r2x));
            int32_t sum = 0;
            if (!rc::GNormalize(g, e + 432u, t, sum)) return false;           // 0x800A9CF8
            rc::GteOp(g, e + 444u, e + 432u, e + 438u);                       // 0x800A9D38
            g.W32(e + 552u, g.U32(e + 552u) | 0xC0000000u);
            uint32_t ev = 43u;
            if (fwd) {
                const uint32_t r3 = GuestRand(g);                             // 0x800A9D6C
                const uint32_t q3 = static_cast<uint32_t>((static_cast<uint64_t>(r3) * 0xAAAAAAABu) >> 32) >> 1;
                const uint32_t m = r3 - 3u * q3;
                ev = (m != 0 ? 46u : 49u) + (m == 1u ? 1u : 0u);
            }
            if ((ev & 0xFFFFu) != g.U16(e + 544u))
                if (!rc::Call(c, 0x800C4550u, {ev, e, 3u}, F)) return false;
            if (s1 != 0) {                                                   // 0x800A9DD4
                g.W32(s1 + 552u, g.U32(s1 + 552u) | 0x20u);
                g.W32(s1 + 580u, g.U32(0x800D3964u));
                if (rc::IsPlayerRider(g, g.U16(s1 + 172u))) {
                    const uint32_t v = kRcViewArray + kRcViewStride * g.U16(g.U32(s1 + 596u) + 172u);
                    if (g.U32(v + 772u) == 0) {
                        const uint32_t a3 = g.U32(v + 548u);
                        if (!(a3 & 0x8000u)) {
                            const uint32_t w0 = g.U32(v + 572u), w1 = g.U32(v + 576u), w2 = g.U32(v + 580u);
                            g.W32(v + 548u, a3 | 0x600000u);
                            g.W32(v + 804u, w0);
                            g.W32(v + 808u, w1);
                            g.W32(v + 812u, w2);
                        }
                    }
                    g.W32(v + 548u, g.U32(v + 548u) & 0xFFE1FFFFu);
                }
            }
        }
    }
    // 0x800AA010
    if (s5) {
        g.W32(e + 464u, 0);
        g.W32(e + 460u, 0);
        g.W32(e + 456u, 0);
        g.W32(e + 480u, 0);
        if (s1 != 0) {
            g.W16(s1 + 622u, g.U16(n + 0u));
            g.W16(s1 + 624u, g.U16(n + 2u));
            g.W16(s1 + 626u, g.U16(n + 4u));
        }
    }
    if (s8 != 0 && s1 != 0) {                                                // 0x800AA050
        g.W32(s1 + 552u, g.U32(s1 + 552u) & 0xFFEFFFE7u);
        const uint32_t bike = g.U32(s1 + 596u);
        g.W8(g.U32(bike + 1084u) + 15u, 0);
        g.W32(s1 + 488u, 0);
        const bool player = rc::IsPlayerRider(g, g.U16(s1 + 172u));
        if ((player || (g.U32(s1 + 572u) & 0x60u) == 0x20u) && g.U32(0x8005B220u) == 0) {
            const int32_t a2 = (speed < oldSpeed) ? oldSpeed : speed;
            g.W32(F + 16u, 2u);
            if (!rc::Call(c, 0x800B658Cu, {g.U32(s1 + 596u), s1, U(a2), 0x165A1Cu, 2u}, F)) return false;
        }
    }
    v0 = s8;
    return !g.Faulted();
}

} // namespace rr::sim
