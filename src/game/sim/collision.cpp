#include "game/sim/collision.h"

#include "game/sim/ai.h"
#include "game/sim/bike_step.h"
#include "game/sim/crash.h"
#include "game/sim/fixed.h"
#include "game/sim/vec.h"

#include <initializer_list>
#include <utility>

namespace rr::sim {
namespace {

// 32-bit wrapping arithmetic, as the R3000's addu/subu/sll/negu do it.
constexpr uint32_t U(int32_t v) { return static_cast<uint32_t>(v); }
constexpr int32_t S(uint32_t v) { return static_cast<int32_t>(v); }
inline int32_t Add(int32_t a, int32_t b) { return S(U(a) + U(b)); }
inline int32_t Sub(int32_t a, int32_t b) { return S(U(a) - U(b)); }
inline int32_t Neg(int32_t a) { return S(0u - U(a)); }
inline int32_t Shl(int32_t a, int s) { return S(U(a) << s); }
// `mult`, low word.
inline int32_t MulLo(int32_t a, int32_t b) {
    return S(static_cast<uint32_t>(static_cast<int64_t>(a) * static_cast<int64_t>(b)));
}
// `mult` then `(lo >> 16) | (hi << 16)`: FixMul inlined.
inline int32_t Mid(int32_t a, int32_t b) { return FixMul(a, b); }
// The `sra/addu/xor` absolute value: INT32_MIN stays INT32_MIN.
inline int32_t Iabs(int32_t x) {
    const uint32_t s = U(x >> 31);
    return S((U(x) + s) ^ s);
}
// `srl t,x,31; addu; sra 1`: C's x / 2.
inline int32_t Half(int32_t x) { return S(U(x) + (U(x) >> 31)) >> 1; }
// `bgez; addiu 3; sra 2` and `bgez; addiu 15; sra 4`.
inline int32_t Div4(int32_t x) { return (x < 0 ? Add(x, 3) : x) >> 2; }
inline int32_t Div16(int32_t x) { return (x < 0 ? Add(x, 15) : x) >> 4; }
// The unsigned reciprocal FixDiv (SLUS 0x80010028).
inline int32_t FDiv(int32_t a, int32_t b) { return S(FixDiv(U(a), U(b))); }
// The sign-magnitude idiom around the unsigned FixDiv, branching on a > 0 and then b > 0.
inline int32_t SDiv(int32_t a, int32_t b) {
    if (a > 0) return b > 0 ? FDiv(a, b) : Neg(FDiv(a, Neg(b)));
    return b > 0 ? Neg(FDiv(Neg(a), b)) : FDiv(Neg(a), Neg(b));
}
// R3000 `div` (lo): every corner case defined.
inline int32_t MipsDiv(int32_t n, int32_t d) {
    if (d == 0) return n >= 0 ? -1 : 1;
    if (U(n) == 0x80000000u && d == -1) return n;
    return n / d;
}
// R3000 `divu` (lo).
inline uint32_t MipsDivU(uint32_t n, uint32_t d) { return d == 0 ? 0xFFFFFFFFu : n / d; }

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
    for (uint32_t k = 0; k < 3; ++k) g.W32(a + 4u * k, U(v[k]));
}
// RASHCDG 0x800B6AAC AiProject(p, axis, q) over guest addresses.
int32_t GProject(GuestRam& g, uint32_t p, uint32_t axis, uint32_t q) {
    int32_t a[3], o[3];
    int16_t n[3];
    Read32x3(g, p, a);
    Read16x3(g, axis, n);
    Read32x3(g, q, o);
    return AiProject(a, n, o);
}
// SLUS 0x8002E698 DotLcm(a, b).
int32_t GDot(GuestRam& g, uint32_t a, uint32_t b) {
    int16_t x[3], y[3];
    Read16x3(g, a, x);
    Read16x3(g, b, y);
    return DotLcm(x, y);
}
// SLUS 0x8002EE50 Scale(t, dir, out).
void GScale(GuestRam& g, int32_t t, uint32_t dir, uint32_t out) {
    int16_t d[3];
    int32_t o[3];
    Read16x3(g, dir, d);
    Scale(t, d, o);
    Write32x3(g, out, o);
}
// SLUS 0x8002EAD8 MulAdd(base, dir, t, out); `out` may equal `base` (component i reads base[i] before
// it writes out[i], and nothing else is read after a store).
void GMulAdd(GuestRam& g, uint32_t base, uint32_t dir, int32_t t, uint32_t out) {
    int32_t b[3], o[3];
    int16_t d[3];
    Read32x3(g, base, b);
    Read16x3(g, dir, d);
    MulAdd(b, d, t, o);
    Write32x3(g, out, o);
}
// SLUS 0x8002E468 Normalize(v) in place; false where the console raises its overflow exception.
bool GNormalize(GuestRam& g, uint32_t a, const uint16_t* rsqrt) {
    int16_t v[3];
    Read16x3(g, a, v);
    if (!Normalize(v, rsqrt)) return false;
    Write16x3(g, a, v);
    return true;
}
// SLUS 0x8002EED8 ScaleTo16(k, v, out): out[i] = FixMul(k, v[i]) >> 4, v[i] read after out[i-1].
void GScaleTo16(GuestRam& g, int32_t k, uint32_t v, uint32_t out) {
    for (uint32_t i = 0; i < 3; ++i) g.W16(out + 2u * i, static_cast<uint16_t>(FixMul(k, g.S32(v + 4u * i)) >> 4));
}
// The game state and its player count (game_state + 0x30).
uint32_t GameState(GuestRam& g) { return g.U32(0x8005B2F8u); }
// Whether a load at `a` lands in main RAM the way GuestRam maps it (KUSEG, KSEG0, KSEG1, the 8 MiB
// mirror); anything else (the BIOS ROM above all) the caller has to answer.
bool IsRam(uint32_t a) {
    const uint32_t seg = a >> 29;
    return (seg == 0u || seg == 4u || seg == 5u) && (a & 0x1FFFFFFFu) < 0x00800000u;
}
uint32_t NumPlayers(GuestRam& g) { return g.U32(GameState(g) + 48u); }

bool Call(CollisionCallees& c, uint32_t fn, std::initializer_list<uint32_t> args, uint32_t sp, uint32_t* v0 = nullptr) {
    uint32_t a[12] = {};
    int n = 0;
    for (uint32_t x : args) a[n++] = x;
    uint32_t r = 0;
    if (!c.Unported(fn, a, n, sp, r)) return false;
    if (v0 != nullptr) *v0 = r;
    return true;
}

} // namespace

// ============================================================================ RASHCDG 0x800A8DF0
void ApplyImpulse(GuestRam& g, uint32_t e, uint32_t v, int32_t flag) {
    g.W32(e + 184, U(Add(g.S32(e + 184), g.S32(v + 0))));                    // 0x800A8E04
    g.W32(e + 188, U(Add(g.S32(e + 188), g.S32(v + 4))));
    const uint32_t pool = g.U16(e + 172) >> 5;                                // 0x800A8E10
    g.W32(e + 192, U(Add(g.S32(e + 192), g.S32(v + 8))));
    bool both = false;
    if (pool == 0 && g.U32(e + 856) != 0) both = g.U32(e + 1088) != 0;       // sltu t4,zero,+0x440
    if (both) {
        for (uint32_t k = 0; k < 3; ++k) {
            const uint32_t p = g.U32(e + 856);                                // re-read per word
            g.W32(p + 184 + 4 * k, U(Add(g.S32(p + 184 + 4 * k), g.S32(v + 4 * k))));
        }
    }
    for (uint32_t c = 0; c < 8; ++c) {
        for (uint32_t k = 0; k < 3; ++k)
            g.W32(e + 196 + 12 * c + 4 * k, U(Add(g.S32(e + 196 + 12 * c + 4 * k), g.S32(v + 4 * k))));
        if (both)
            for (uint32_t k = 0; k < 3; ++k) {
                const uint32_t p = g.U32(e + 856) + 12 * c;
                g.W32(p + 196 + 4 * k, U(Add(g.S32(p + 196 + 4 * k), g.S32(v + 4 * k))));
            }
    }
    if (flag == 0) return;
    switch (pool) {                                                           // 0x800A8F58..
    case 0: g.W32(e + 568, g.U32(e + 568) | 0x01800000u); break;
    case 1: g.W32(e + 552, g.U32(e + 552) | 2u); break;
    case 2: g.W32(e + 568, g.U32(e + 568) | 2u); break;
    case 4: g.W32(e + 592, g.U32(e + 592) | 0x100u); break;
    default: break;
    }
}

// ============================================================================ RASHCDG 0x800A8FE8
bool StaleHeading(GuestRam& g, uint32_t e, const BikeTables& t, int32_t& v0) {
    v0 = 0;
    const uint32_t f = g.U32(e + 568);
    if (!(f & 0x01000000u)) return !g.Faulted();
    int32_t d[3];
    d[0] = Sub(g.S32(e + 184), g.S32(e + 468));
    g.W32(e + 568, f & 0xFEFFFFFFu);                                          // 0x800A9020
    d[1] = Sub(g.S32(e + 188), g.S32(e + 472));
    d[2] = Sub(g.S32(e + 192), g.S32(e + 476));
    const int32_t q = SumSquares(d);                                          // SLUS 0x8002F0F4
    if (q < 132) return !g.Faulted();
    if (!(g.U32(e + 568) & 0x02000000u)) {
        const uint16_t h0 = g.U16(e + 450), h1 = g.U16(e + 452), h2 = g.U16(e + 454);
        const uint32_t fc = g.U32(e + 568) | 0x02000000u;
        const uint32_t sp0 = g.U32(e + 480);
        g.W16(e + 864, h0);
        g.W16(e + 866, h1);
        g.W16(e + 868, h2);
        g.W32(e + 860, sp0);
        g.W32(e + 568, fc);                                                   // 0x800A90A4
    }
    const int32_t s0 = Shl(SqrtGte(q, t.sqrt), 2);
    const int32_t dt = g.S32(kCollDt);
    g.W32(e + 480, U(SDiv(s0, dt)));                                          // 0x800A9104
    int32_t k;
    if (s0 >= 0) {
        const uint32_t dv = U(Add(s0 >> 1, Sub(s0, 2) >> 31));
        k = S(MipsDivU(0x80000000u, dv));
    } else {
        const int32_t n = Neg(s0);
        const uint32_t dv = U(Add(n >> 1, Sub(n, 2) >> 31));
        k = Neg(S(MipsDivU(0x80000000u, dv)));
    }
    for (uint32_t i = 0; i < 3; ++i) g.W16(e + 450 + 2 * i, static_cast<uint16_t>(FixMul(k, d[i]) >> 4));
    v0 = 1;
    const uint32_t p = g.U32(e + 856);
    if (p == 0) return !g.Faulted();
    if (g.U32(e + 1088) != 0) {
        g.W32(e + 568, g.U32(e + 568) | 0x01000000u);
        int32_t r = 0;
        if (!StaleHeading(g, g.U32(e + 856), t, r)) return false;
    }
    return !g.Faulted();
}

// ============================================================================ RASHCDG 0x800A8C78
int32_t InCameraBox(GuestRam& g, uint32_t e, int32_t halfWidth, int32_t depth) {
    const uint16_t st = g.U16(e + 320);
    if (st & 8) return (st >> 2) & 1;
    g.W32(e + 48, 0x7FFFFFFFu);
    int32_t in = 0;
    uint32_t p = 0;
    do {
        const uint32_t o = 1132u * p;
        int32_t v = GProject(g, e + 184, 0x800CDA54u + o, 0x800CD950u + o);   // forward
        if (0x8000 < v && v < depth) {
            v = GProject(g, e + 184, 0x800CDA48u + o, 0x800CD950u + o);       // lateral: v reused
            if (Iabs(v) < halfWidth) in = 1;
        }
        g.W32(e + 44 + 4 * p, U(v >> 10));
        ++p;
    } while (p < NumPlayers(g));
    g.W16(e + 320, static_cast<uint16_t>(g.U16(e + 320) | 8u | (static_cast<uint32_t>(in) << 2)));
    return in;
}

// ============================================================================ RASHCDG 0x800B6F40
uint32_t CornerMin(GuestRam& g, uint32_t c, uint32_t n, uint32_t q, uint32_t outDepth, uint32_t outCount) {
    int32_t cnt = 0, mn = 0;
    uint32_t best = 8;
    for (uint32_t k = 0; k < 8; ++k) {
        const int32_t v = GProject(g, c + 12 * k, n, q);
        if (v < 0) {
            ++cnt;
            const uint32_t m = (v < mn) ? 0xFFFFFFFFu : 0u;                  // negu of slt
            mn = Add(mn, S(m & U(Sub(v, mn))));
            best = best + (m & (k - best));
        }
    }
    if (outCount != 0) g.W32(outCount, U(cnt));
    if (best == 8) return 8;
    if (outDepth != 0) g.W32(outDepth, U(Neg(mn)));
    return best;
}

// ============================================================================ RASHCDG 0x800B6BD0
int32_t RayPlane(GuestRam& g, uint32_t p, uint32_t dir, uint32_t n, uint32_t q) {
    const int32_t d = GDot(g, n, dir);
    if (-64 < d && d < 64) return 0x7FFF0000;
    int16_t nn[3];
    Read16x3(g, n, nn);
    const int32_t a0 = Mid(g.S32(p + 0), Shl(nn[0], 4)), a1 = Mid(g.S32(p + 4), Shl(nn[1], 4));
    const int32_t a2 = Mid(g.S32(p + 8), Shl(nn[2], 4));
    const int32_t b0 = Mid(g.S32(q + 0), Shl(nn[0], 4)), b1 = Mid(g.S32(q + 4), Shl(nn[1], 4));
    const int32_t b2 = Mid(g.S32(q + 8), Shl(nn[2], 4));
    const int32_t A = Add(a2, Add(a1, a0));
    const int32_t B = Add(b2, Add(b1, b0));
    const int32_t num = Sub(A, B);
    const int32_t nd = Neg(d);
    if (num > 0) return nd > 0 ? FDiv(num, nd) : Neg(FDiv(num, d));
    return nd <= 0 ? FDiv(Neg(num), d) : Neg(FDiv(Neg(num), nd));
}

// ============================================================================ RASHCDG 0x800B59F0
int32_t ContactMerge(GuestRam& g, uint32_t rec) {
    int32_t i = g.S32(kContactCount) - 1;
    uint32_t found = 0;
    while (i >= 0) {
        const uint32_t o = kContactList + 36u * U(i);
        found = 0;
        if (g.U16(o) == g.U16(rec)) found = (g.U16(o + 2) == g.U16(rec + 2)) ? 1u : 0u;
        if (found) break;
        --i;
    }
    if (!found) return 1;
    const uint32_t o = kContactList + 36u * U(i);
    const int32_t d = GDot(g, rec + 4, o + 4);
    if (0xE000 < d) {
        if (!(g.S32(rec + 32) < g.S32(o + 32)))
            for (uint32_t k = 0; k < 36; k += 4) g.W32(o + k, g.U32(rec + k));   // SLUS 0x8001E0B4
    } else if (S(0xFFFF4B03u) < d) {
        for (uint32_t k = 16; k < 28; k += 4) g.W32(o + k, U(Add(g.S32(o + k), g.S32(rec + k))));
    }
    return 0;
}

// ============================================================================ RASHCDG 0x800A451C
bool ViewTrack(GuestRam& g, uint32_t v, int32_t flag, const BikeTables& t) {
    const int32_t dt = g.S32(kCollDt);
    if (flag != 0) {
        int32_t d[3];
        for (uint32_t k = 0; k < 3; ++k) d[k] = Sub(g.S32(v + 184 + 4 * k), g.S32(v + 468 + 4 * k));
        const int32_t s = SumSquares(d);
        if (0x3FFEFFFF < s) return !g.Faulted();
        const int32_t r = Shl(SqrtGte(s, t.sqrt), 2);
        g.W32(v + 480, U(SDiv(r, dt)));                                       // 0x800A45E4
        if (r < 132) return !g.Faulted();
        int32_t k;
        if (r >= 0) {
            k = S(MipsDivU(0x80000000u, U(Add(r >> 1, Sub(r, 2) >> 31))));
        } else {
            const int32_t n = Neg(r);
            k = Neg(S(MipsDivU(0x80000000u, U(Add(n >> 1, Sub(n, 2) >> 31)))));
        }
        for (uint32_t i = 0; i < 3; ++i) g.W16(v + 450 + 2 * i, static_cast<uint16_t>(FixMul(k, d[i]) >> 4));
        return !g.Faulted();
    }
    if (g.U32(v + 548) & 0x40000u) return !g.Faulted();
    if (U(Sub(g.S32(v + 540), 13)) < 2u) return !g.Faulted();
    int32_t s0 = Iabs(g.S32(v + 632));
    const uint32_t o = g.U32(v + 568);
    if (o != 0 && (g.U16(o + 172) >> 5) == 0)
        s0 = Add(s0, g.U32(g.U32(o + 852) + 604) < 3u ? S(0xFFFE8000u) : -32768);
    const uint16_t h0 = g.U16(v + 600), h2 = g.U16(v + 604);
    g.W16(v + 450, static_cast<uint16_t>(0u - h0));
    const uint16_t h1 = g.U16(v + 602);
    g.W16(v + 454, static_cast<uint16_t>(0u - h2));
    g.W16(v + 452, static_cast<uint16_t>(0u - h1));
    g.W32(v + 480, U(SDiv(s0, dt)));                                          // 0x800A4748
    GMulAdd(g, v + 184, v + 450, Neg(s0), v + 468);
    return !g.Faulted();
}

// ============================================================================ RASHCDG 0x800B658C
bool PadRumble(GuestRam& g, uint32_t e, uint32_t other, int32_t speed, int32_t k, int32_t div, uint32_t sp,
               RumbleEnv& env) {
    const uint32_t fr = sp - 40;
    int32_t s1 = MipsDiv(Sub(Shl(speed, 8), speed), k);                      // speed * 255 / k
    const int32_t five = Add(Shl(speed, 2), speed);
    int32_t s0 = MipsDiv(Shl(Sub(Shl(five, 4), five), 1), k);                 // speed * 150 / k
    const int32_t v1 = MipsDiv(255, div);
    const int32_t a1 = MipsDiv(150, div);
    {
        const int32_t lo = S(~U(s1 >> 31) & U(s1));
        const int32_t dd = Sub(v1, s1);
        s1 = Add(lo, S(U(dd >> 31) & U(dd)));
    }
    {
        const int32_t lo = S(~U(s0 >> 31) & U(s0));
        const int32_t dd = Sub(a1, s0);
        s0 = Add(lo, S(U(dd >> 31) & U(dd)));
    }
    bool first = other == 0;
    if (!first) {
        const uint32_t h = g.U16(other + 172);
        first = (h >> 5) == 1 && S(h & 0x1F) < g.S32(GameState(g) + 48);
    }
    if (first && !env.PadMotor(g.U16(e + 172), s0, 150, s1, fr)) return false;
    if (other != 0 && !(g.U8(other + 572) & 0x20)) return !g.Faulted();
    // 0x800B6690: the passenger and ITS rider, with no null test
    const uint32_t pass = g.U32(e + 856);
    const uint32_t pr = g.U32(pass + 852);
    uint8_t prFlags = 0;
    if (IsRam(pr + 572)) prFlags = g.U8(pr + 572);
    else if (!env.ReadForeignByte(pr + 572, prFlags)) return false;
    if (prFlags & 0x40) return !g.Faulted();
    bool second = false;
    if (pass != 0 && (g.U8(g.U32(e + 852) + 572) & 0x10)) {
        uint32_t m = 0;
        if (IsRam(pr + 604)) m = g.U32(pr + 604);
        else {
            uint8_t b[4];
            for (uint32_t i = 0; i < 4; ++i)
                if (!env.ReadForeignByte(pr + 604 + i, b[i])) return false;
            m = b[0] | (b[1] << 8) | (b[2] << 16) | (static_cast<uint32_t>(b[3]) << 24);
        }
        second = m < 2u;
    }
    if (!second && other == 0) return !g.Faulted();
    const uint32_t who = other != 0 ? g.U32(g.U32(other + 596) + 856) : e;
    const uint32_t np = NumPlayers(g);
    const uint32_t pad = U(Add(Add(g.U16(who + 172), 1), (np < 2u) ? 0 : 1));
    if (!env.PadMotor(pad, s0, 150, s1, fr)) return false;
    return !g.Faulted();
}

// ============================================================================ RASHCDG 0x800B16F4
bool TouchDown(GuestRam& g, uint32_t e, uint32_t pSpeed, uint32_t sp, CollisionCallees& c) {
    const uint32_t fr = sp - 56;
    const uint32_t gs = GameState(g);
    const uint32_t h = g.U16(e + 172);
    if (h < g.U32(gs + 48)) {
        const uint32_t view = kCollViews + 1132u * h;
        const int32_t vy = g.S32(e + 460);
        const int32_t q = vy > 0 ? FDiv(vy, 0x80000) : Neg(FDiv(Neg(vy), 0x80000));  // signed vy / 8
        const int32_t up = Sub(0x30000, q);                                            // 3.0 - q
        const int32_t s3 = up >> 31, s1 = q >> 31;
        const int32_t s0 = Add(q, S(U(s1) & U(Neg(q))));                                // max(q, 0)
        g.W32(view + 740, U(Add(s0, S(U(s3) & U(up)))));                                // 0x800B1868
    }
    const int32_t d = GDot(g, e + 450, e + 444);
    g.W32(e + 576, U(FixMul(d >= 0 ? 0xE666 : 6553, g.S32(pSpeed))));                  // 0x800B1894
    g.W32(e + 568, g.U32(e + 568) & ~0x400u);                                           // 0x800B18A4: LANDED
    const uint32_t r = g.U32(e + 852);
    g.W32(r + 552, g.U32(r + 552) & ~0x2000u);
    if (0x30000 < g.S32(e + 768)) {
        if (!c.PlaySound3D(g.S32(e + 184), g.S32(e + 192), 54, 0)) return false;
        if (g.U16(e + 172) < g.U32(GameState(g) + 48) && g.U32(g.U32(e + 852) + 604) < 2u &&
            g.U32(0x8005B220u) == 0) {
            if (!c.Rumble(e, 0, g.S32(pSpeed), 0x165A1C, 1, fr)) return false;
        }
    }
    g.W32(e + 568, g.U32(e + 568) | 0x0C000000u);
    return !g.Faulted();
}

// ============================================================================ RASHCDG 0x800B1978
bool AirContact(GuestRam& g, uint32_t e, uint32_t sp, const BikeTables& t, CollisionCallees& c) {
    const uint32_t fr = sp - 96;
    const uint32_t N = fr + 48, DEPTH = fr + 56, DEPTH2 = fr + 60, IMP = fr + 32;
    g.W16(N + 0, static_cast<uint16_t>(0u - g.U16(e + 522)));                  // the ground plane's
    g.W16(N + 2, static_cast<uint16_t>(0u - g.U16(e + 524)));                  // normal: -(+0x20A)
    g.W16(N + 4, static_cast<uint16_t>(0u - g.U16(e + 526)));
    uint32_t s0 = CornerMin(g, e + 196, N, e + 504, DEPTH, 0);
    const uint32_t pass = g.U32(e + 856);
    if (pass != 0 && (s0 == 8 || (s0 & 1) != ((s0 & 2) >> 1))) {
        const uint32_t v1 = CornerMin(g, pass + 196, N, e + 504, DEPTH2, 0);
        if (v1 < 8) {
            s0 = v1;
            g.W32(DEPTH, g.U32(DEPTH2));
        }
    }
    if (!(s0 < 8)) return !g.Faulted();
    if ((g.U32(e + 568) & 0x400u) && !(g.S32(e + 708) < g.S32(DEPTH))) return !g.Faulted();
    bool stopDead = g.S8(e + 534) == 4;
    if (!stopDead) stopDead = (g.U32(e + 564) & 0x00100000u) != 0;
    if (stopDead) {                                                            // 0x800B1A98
        if (0x50000 < Iabs(g.S32(DEPTH))) {
            const uint32_t fb = g.U32(e + 564);
            g.W32(e + 464, 0);
            g.W32(e + 460, 0);
            g.W32(e + 456, 0);
            g.W32(e + 564, fb | 0x00100000u);
        }
        // 0x800B1AD4
        if (g.U16(e + 172) < g.U32(GameState(g) + 48)) {
            const uint32_t r = g.U32(e + 852);
            g.W32(r + 552, g.U32(r + 552) | 0x80000u);
        }
    } else {
        g.W32(e + 564, g.U32(e + 564) & 0xFFEFFFFFu);                         // 0x800B1B2C
        GScale(g, g.S32(DEPTH), N, IMP);
        ApplyImpulse(g, e, IMP, 1);
        if (!(g.U32(e + 568) & 0x02000000u)) {                                 // latch the pre-contact
            const uint16_t h0 = g.U16(e + 450), h1 = g.U16(e + 452), h2 = g.U16(e + 454);
            const uint32_t fc = g.U32(e + 568) | 0x02000000u;
            const uint32_t v = g.U32(e + 480);
            g.W16(e + 864, h0);
            g.W16(e + 866, h1);
            g.W16(e + 868, h2);
            g.W32(e + 860, v);
            g.W32(e + 568, fc);
        }
        if (g.U32(e + 564) & 0x8000u) goto rider_check;
        {
            const uint32_t fc = g.U32(e + 568);
            if (fc & 0x400u) {                                                 // AIRBORNE: the landing
                if (!TouchDown(g, e, e + 860, fr, c)) return false;
            } else if (fc & 0x200u) {                                          // crashed
                const bool bounce = !(32767 < g.S32(e + 756)) ? false : !(fc & 0x80000u);
                if (!bounce) {                                                 // 0x800B1BE4
                    int32_t d = GDot(g, e + 864, N);
                    if (d < S(0xFFFF199Au)) d = S(0xFFFF199Au);
                    g.W32(DEPTH, U(d));
                    const uint32_t v1 = g.U32(e + 568);
                    if (v1 & 0x00200000u) {                                   // a slide
                        const int32_t bx = g.S32(e + 184);
                        for (uint32_t o : {616u, 688u, 632u, 628u, 624u, 620u, 640u}) g.W32(e + o, 0);
                        const int32_t dx = Sub(bx, g.S32(e + 504));
                        const int32_t dy = Sub(g.S32(e + 188), g.S32(e + 508));
                        const int32_t dz = Sub(g.S32(e + 192), g.S32(e + 512));
                        const int32_t x = Mid(dx, Shl(g.S16(e + 522), 4));
                        const int32_t y = Mid(dy, Shl(g.S16(e + 524), 4));
                        const int32_t z = Mid(dz, Shl(g.S16(e + 526), 4));
                        int32_t a0 = Add(z, Add(y, x));
                        g.W32(e + 772, U(a0));                                  // 0x800B1D0C (delay slot)
                        if (a0 > 0) a0 = 0;
                        g.W32(e + 772, U(a0));
                        int16_t dd[3], ir[3], out[3];
                        Read16x3(g, e + 522, dd);
                        Read16x3(g, e + 864, ir);
                        OuterProduct(dd, ir, out);                              // cop2 0x178000C
                        Write16x3(g, e + 814, out);
                        g.W32(e + 488, 0);
                        if (g.U32(e + 856) != 0) g.W32(g.U32(e + 856) + 488, 0);
                        const int32_t aa = GDot(g, e + 528, e + 814);
                        const int32_t bb = GDot(g, e + 528, e + 450);
                        const int32_t ang = RatAtan2(aa, bb, t.atan);
                        g.W32(e + 676, U(MulLo(ang, 25736) >> 8));
                        const int32_t roll = g.S32(e + 708) > 0 ? 0x1921F : S(0xFFFE6DE1u);
                        const uint32_t f2 = g.U32(e + 568);
                        g.W32(e + 652, U(roll));
                        g.W32(e + 568, (f2 & 0xFFC7FDFFu) | 0x900u);
                        goto muladd;
                    }
                    if (!(v1 & 0x80000u)) {                                    // 0x800B1E14: the first hit
                        g.W32(e + 568, v1 & 0xFFCFFFFFu);
                        if (GDot(g, e + 808, e + 522) < 0) g.W32(e + 568, g.U32(e + 568) | 0x00100000u);
                        const int32_t r = Div4(Iabs(g.S32(e + 488)));
                        g.W32(e + 640, U(r));
                        int32_t a0 = r < 0x20000 ? 0x20000 : r;
                        const int32_t x = S(g.U32(e + 692) ^ g.U32(e + 488));
                        g.W32(e + 640, U(a0));
                        if (x >= 0) g.W32(e + 640, U(Neg(a0)));
                        int32_t yaw = g.S32(e + 488);
                        if (yaw > 0) { if (yaw < 0x50000) yaw = 0x50000; }
                        else if (S(0xFFFB0000u) < yaw) yaw = S(0xFFFB0000u);
                        g.W32(e + 488, U(yaw));
                        g.W32(e + 568, g.U32(e + 568) | 0x80000u);
                    }
                    g.W32(DEPTH, U(Half(g.S32(DEPTH))));                       // 0x800B1F00
                muladd:                                                        // 0x800B1F18
                    MulAdd16(g, e + 864, N, Neg(g.S32(DEPTH)), e + 456);
                    g.W16(e + 864, static_cast<uint16_t>(g.S32(e + 456) >> 4));
                    g.W16(e + 866, static_cast<uint16_t>(g.S32(e + 460) >> 4));
                    g.W16(e + 868, static_cast<uint16_t>(g.S32(e + 464) >> 4));
                    if (!GNormalize(g, e + 864, t.rsqrt)) return false;
                    GScale(g, g.S32(e + 860), e + 864, e + 456);
                } else {                                                       // 0x800B1F80: a bounce
                    if (!c.PlaySound3D(g.S32(e + 184), g.S32(e + 192), 17, 0)) return false;
                    const int32_t floor = (0x8F0D7 < g.S32(e + 860)) ? 0x8F0D8 : 0;
                    bool ok = false;
                    const int32_t v = Bounce(g, N, e + 864, e + 860, e + 456, 3, floor, 0, t, ok);
                    if (!ok) return false;
                    if (v >= 0 && !(0x1FFFF < g.S32(e + 756)))
                        if (!Spin(g, e)) return false;
                    g.W32(e + 756, 0);
                }
            }
        }
        // 0x800B2004
        if (!c.ReleaseContact(e)) return false;
        g.W32(e + 832, 0);
        g.W32(e + 828, 0);
    }
rider_check:                                                                   // 0x800B2014
    if (g.U32(g.U32(e + 852) + 552) & 0x80000u) {
        const uint32_t view = kCollViews + 1132u * g.U16(e + 172);
        if (g.S32(view + 788) < 0) g.W32(view + 788, 0);
    }
    return !g.Faulted();
}

// ============================================================================ RASHCDG 0x800B3AD0
namespace {

// The octagonal length of (dx, dz), every shift arithmetic (0x800B4528..0x800B45E4).
int32_t OctDist(int32_t dx, int32_t dz) {
    int32_t a1 = Iabs(dx), a0 = Iabs(dz);
    if (a1 < a0) std::swap(a1, a0);
    const int32_t a2 = Add(a0, a0 >> 1);
    int32_t v = Sub(a1, a1 >> 5);
    v = Sub(v, a1 >> 7);
    v = Add(v, a2 >> 2);
    return Add(v, a2 >> 6);
}

struct Wall {
    GuestRam& g;
    const BikeTables& t;
    CollisionCallees& c;
    uint32_t e = 0, fr = 0;
    int32_t force = 0;
    // frame slots the original keeps in its 296-byte frame; the ones a callee reads through a pointer
    // live in the guest stack at their offsets
    uint32_t NX = 0, NY = 0, NZ = 0, P0 = 0, P1 = 0, CP = 0, A = 0, I = 0, DEPTH = 0, COUNT = 0, HEAD = 0;
    int32_t mode = 1, cls = 0, lim2 = 2048, best = 0x7FFF0000, tmin = 0, travel = 0, segVal = 0;
    uint32_t ent2 = 0, bestSeg = 0, result = 0, L112 = 0, L116 = 0;
    uint32_t s5 = 0;
    int32_t hi12 = 0, isBike = 0, dirn = 1;
    bool isView = false;
    uint32_t view = 0;
    bool failed = false;

    Wall(GuestRam& gg, const BikeTables& tt, CollisionCallees& cc) : g(gg), t(tt), c(cc) {}

    void NegN() {                                          // lhu 24, lhu 28, sh -24, lhu 26, sh -28, sh -26
        const uint16_t x = g.U16(NX), z = g.U16(NZ);
        g.W16(NX, static_cast<uint16_t>(0u - x));
        const uint16_t y = g.U16(NY);
        g.W16(NZ, static_cast<uint16_t>(0u - z));
        g.W16(NY, static_cast<uint16_t>(0u - y));
    }
    bool Unported(uint32_t fn, std::initializer_list<uint32_t> a) {
        if (!Call(c, fn, a, fr)) { failed = true; return false; }
        return true;
    }

    enum class Next { Seg, Group, Return };
    // 0x800B4244..0x800B5864 for one segment.
    Next Segment(uint32_t cur, uint32_t s8, uint32_t nrm, uint32_t step, uint32_t other, uint16_t& prevF,
                 int32_t& segCount, int32_t groups, uint32_t& ret);
};

Wall::Next Wall::Segment(uint32_t cur, uint32_t s8, uint32_t nrm, uint32_t step, uint32_t other, uint16_t& prevF,
                         int32_t& segCount, int32_t groups, uint32_t& ret) {
    if (s8 == 0) s8 = cur;
    s5 &= 0xFFFFFDFFu;
    if (U(g.S16(s8 + 2)) != static_cast<uint32_t>(prevF)) s5 |= 0x200u;      // lh against the u16
    g.W16(NX, g.U16(s8 + 32));
    g.W16(NY, g.U16(s8 + 34));
    g.W16(NZ, g.U16(s8 + 36));
    for (uint32_t k = 0; k < 3; ++k) g.W32(P0 + 4 * k, g.U32(s8 + 8 + 4 * k));
    for (uint32_t k = 0; k < 3; ++k) g.W32(P1 + 4 * k, g.U32(s8 + 20 + 4 * k));
    const int32_t ny = g.S16(s8 + 34);
    prevF = g.U16(s8 + 2);
    segVal = g.S32(s8 + 4);
    if (Iabs(ny) >= 65) {
        g.W16(NY, 0);
        if (!GNormalize(g, NX, t.rsqrt)) { failed = true; return Next::Return; }
    }
    s5 &= 0xFFFFFFE0u;
    // ---- 0x800B4318: the step (type 5) segments
    enum { None, Gate, Up, Test } go = None;
    if ((s5 & 0x200u) && (prevF & 0xF) == 5) {
        if (force == 0 && !isView) {
            go = Gate;
        } else if ((hi12 & 0xF) == 5) {
            go = (isView || g.S16(e + 452) < 0) ? Up : Gate;
        } else if (isView) {
            go = Up;
        } else {
            uint32_t R = g.U32(e + 340);
            if (g.S32(e + 188) < Add(Sub(g.S32(R + 24), segVal), 0x10000)) {
                const uint16_t x = g.U16(R + 14);
                g.W16(NX, x);
                const uint16_t y = g.U16(g.U32(e + 340) + 16);
                g.W16(NY, y);
                const uint16_t z = g.U16(g.U32(e + 340) + 18);
                s5 |= 0x8u;
                g.W16(NZ, z);
                segCount = Add(segCount, 1);
                if (g.S32(e + 364) > 0) {
                    g.W16(NX, static_cast<uint16_t>(0u - x));
                    g.W16(NY, static_cast<uint16_t>(0u - y));
                    g.W16(NZ, static_cast<uint16_t>(0u - z));
                }
                if (dirn < 0)
                    for (uint32_t k = 0; k < 3; ++k) g.W32(P0 + 4 * k, g.U32(s8 + 20 + 4 * k));
                const uint32_t road = g.U32(e + 372);
                R = g.U32(e + 340);
                if (g.S32(e + 344) < 0) GMulAdd(g, P0, R + 2, Neg(g.S32(road + 16)), P1);
                else GMulAdd(g, P0, R + 2, Neg(g.S32(road + 144)), P1);
                go = Test;
            } else {
                go = g.S16(e + 452) < 0 ? Up : Gate;
            }
        }
        if (go == Up) {                                                        // 0x800B4498
            s5 |= 0x10u;
            g.W16(NX, 0);
            g.W16(NY, 4096);
            g.W16(NZ, 0);
            uint32_t R = g.U32(e + 340);
            segCount = Add(segCount, 1);
            GMulAdd(g, R + 20, R + 8, segVal, P0);
            uint32_t road = g.U32(e + 372);
            R = g.U32(e + 340);
            GMulAdd(g, P0, R + 2, g.S32(road + 144), P1);
            road = g.U32(e + 372);
            R = g.U32(e + 340);
            GMulAdd(g, P0, R + 2, g.S32(road + 16), P0);
        }
    }
    if ((s5 & 0x1D8u) == 0) return Next::Group;                                // 0x800B4510
    // ---- 0x800B4518: the distance gate
    {
        const int32_t bx = g.S32(e + 184), bz = g.S32(e + 192);
        const int32_t d0 = OctDist(Sub(bx, g.S32(P0)), Sub(bz, g.S32(P0 + 8)));
        const int32_t d1 = OctDist(Sub(bx, g.S32(P1)), Sub(bz, g.S32(P1 + 8)));
        if (d0 > 0xBF0000 || d1 > 0xBF0000) return Next::Seg;
    }
    if (prevF & 0x400) {
        if ((g.U16(e + 172) >> 5) == 1 && (prevF & 0xA00) == 0 && g.S32(e + 604) == 4) return Next::Seg;
        s5 |= U(GProject(g, e + 468, NX, P0)) >> 31;
        if ((prevF & 0xF) == 1 && (s5 & 1)) return Next::Seg;
    }
    travel = FixMul(g.S32(kCollDt), g.S32(e + 480));
    g.W32(fr + 188, U(travel));
    cls = S(U(cls) & 0xFFFFFFF2u);
    bool player = false;
    if (!isView && !(s5 & 0x18u) && !(prevF & 0x800)) {
        const uint32_t hh = g.U16(e + 172);
        player = (hh >> 5) < 2 && S(hh & 0x1F) < g.S32(GameState(g) + 48);
        bool near = true;
        if (!player) near = InCameraBox(g, e, 0x140000, 0x1C0000) != 0;
        if (near) {                                                            // 0x800B470C
            const int32_t s3 = Iabs(GDot(g, HEAD, NX));
            if ((hi12 & 0xF) == 0 || (travel >= 132 && 5701 < s3)) {
                s5 |= 4u;
                if (travel < 0x20000) {
                    travel = 0x20000;
                    g.W32(fr + 188, U(travel));
                }
                if (isBike) {
                    const int32_t p = MulLo(g.S16(e + 450), g.S16(NZ));
                    const int32_t q2 = MulLo(g.S16(e + 454), g.S16(NX));
                    uint32_t b = q2 >= p ? 1u : 0u;
                    b ^= s5 & 1u;
                    cls = S(U(cls) | (b << 2));
                    cls = S(U(cls) | (s3 <= 0x926D ? 1u : 0u));
                }
            }
        } else if ((hi12 & 0xF) == 0 || Iabs(travel) >= 131) {                 // 0x800B47E0
            const int32_t s3 = Iabs(GDot(g, HEAD, NX));
            if (s3 >= 11469) {
                s5 |= 2u;
                if (!(s5 & 0x20u)) {
                    GMulAdd(g, e + 184, HEAD, Neg(travel), A);
                    const int32_t v = FixMul(0xB4FD, Add(g.S32(e + 304), g.S32(e + 308)));
                    GMulAdd(g, e + 184, HEAD, v, I);
                    if (mode == 1 && GDot(g, HEAD, e + 444) < 0) mode = 0;
                    s5 |= 0x20u;
                }
            }
        }
    }
    // ---- 0x800B48C0: find the corner
    const int32_t s1c = 4096;
    uint32_t s7 = 8;
    if (s5 & 4u) {                                                             // swept corners
        s7 = 0;
        const uint32_t tab = kCornerOrder + 4u * U(cls);
        while (true) {
            const uint32_t s0 = isBike ? ((g.U32(tab) >> (4 * s7)) & 7u) : ((7u - s7) & 7u);
            const uint32_t crn = e + 196 + 12 * s0;
            GMulAdd(g, crn, HEAD, Neg(travel), A);
            const int32_t tt = RayPlane(g, A, HEAD, NX, P0);
            if (tt < -s1c) {
                ++s7;
            } else if (Add(travel, s1c) < tt) {
                ++s7;
            } else {
                s7 = s0;
                GMulAdd(g, A, HEAD, tt, CP);
                const int32_t v = GProject(g, crn, NX, P0);
                g.W32(DEPTH, U(Neg(v)));
                const int32_t dd = !(s1c < Neg(v)) ? s1c : Neg(v);
                g.W32(DEPTH, U(dd));
                if (isBike) {
                    const int32_t s2 = Neg(GDot(g, NX, HEAD));
                    if (s2 >= 132 && travel >= 132) {
                        const int32_t x = FixMul(g.S32(kCollDt), g.S32(DEPTH));
                        const int32_t y = FixMul(s2, travel);
                        tmin = SDiv(x, y);
                    }
                    const int32_t dt = g.S32(kCollDt);
                    tmin = tmin < dt ? tmin : dt;
                }
                break;
            }
            if (!(s7 < 8)) break;
        }
    } else if (s5 & 2u) {                                                      // 0x800B4AD0 crossing
        const int32_t x = GProject(g, A, NX, P0);
        const int32_t y = GProject(g, I, NX, P0);
        if ((x ^ y) < 0) {
            g.W32(DEPTH, U(Add(S((0u - (s5 & 1u)) & 0x80020000u), 0x3FFF0000)));
            for (uint32_t s0 = 0; s0 < 8; ++s0) {
                if (mode == 2 || S((s0 >> 1) & 1u) == mode) {
                    const int32_t tt = RayPlane(g, e + 196 + 12 * s0, HEAD, NX, P0);
                    bool take = (s5 & 1u) && g.S32(DEPTH) < tt;
                    if (!take) take = tt < g.S32(DEPTH);
                    if (take) {
                        s7 = s0;
                        g.W32(DEPTH, U(tt));
                    }
                }
            }
            if (s7 < 8) GMulAdd(g, e + 196 + 12 * s7, HEAD, g.S32(DEPTH), CP);
        }
    } else {                                                                   // 0x800B4BC4 static
        if (s5 & 1u) NegN();
        int32_t s3 = 0x80000;
        if (!isView) {
            const int32_t a = Add(Add(g.S32(e + 308), g.S32(e + 304)), Half(g.S32(e + 312)));
            s3 = Div4(MulLo(a, 3));
        }
        g.W32(fr + 16, COUNT);
        s7 = CornerMin(g, e + 196, NX, P0, DEPTH, COUNT);
        if (g.S32(COUNT) == 8) {
            bool x = false;
            if (prevF & 0x800) {
                const uint32_t hh = g.U16(e + 172);
                const uint32_t npl = g.U32(GameState(g) + 48);
                if (hh < npl && (g.U32(g.U32(e + 852) + 552) & 0x80000u)) x = true;
                else if ((hh >> 5) == 1 && S(hh & 0x1F) < S(npl)) x = (g.U32(e + 552) & 0x80000u) != 0;
            } else {
                x = true;
            }
            if (x) {
                if (s3 < g.S32(DEPTH)) s7 = 8;
                else if ((prevF & 0xF) == 2 && groups >= 2) s7 = 8;
            }
        }
        if (s7 < 8) GMulAdd(g, e + 196 + 12 * s7, NX, g.S32(DEPTH), CP);
    }
    // ---- 0x800B4D58
    g.W32(e + 388, (static_cast<uint32_t>(prevF) << 20) | (g.U32(e + 388) & 0x000FFFEFu));   // 0x800B4D84
    if (s5 & 1u) {
        const int32_t dx = Sub(g.S32(P0), g.S32(e + 184));
        const int32_t a = FixMul(dx, dx);
        const int32_t dz = Sub(g.S32(P0 + 8), g.S32(e + 192));
        const int32_t d = Add(a, FixMul(dz, dz));
        if (d < best) {
            bestSeg = s8;
            best = d;
        }
    }
    if (s7 == 8) return Next::Seg;
    // ---- 0x800B4DE0: the horizontal normal and the extent along the wall
    int32_t s1 = 2048;
    if (s5 & 0x10u) {
        L112 = U(Shl(g.S16(s8 + 32), 4));
        g.W32(fr + 112, L112);
        L116 = U(Shl(g.S16(s8 + 36), 4));
        g.W32(fr + 116, L116);
    } else {
        const uint32_t a = U(Shl(g.S16(NZ), 4));
        L112 = a;
        g.W32(fr + 112, a);
        L116 = U(Shl(Neg(g.S16(NX)), 4));
        g.W32(fr + 116, L116);
        if (Iabs(g.S16(NY)) >= 65) {
            const int32_t ss = Add(FixMul(S(a), S(a)), FixMul(S(L116), S(L116)));
            const int32_t r = SqrtGte(ss, t.sqrt);
            uint32_t k;
            if (Shl(r, 2) < 0) {
                const int32_t x = Neg(Shl(r, 2)) >> 1;
                const int32_t y = Sub(Neg(Shl(r, 2)), 2) >> 31;
                k = 0u - MipsDivU(0x80000000u, U(Add(x, y)));
            } else {
                const int32_t x = Shl(r, 2) >> 1;
                const int32_t y = Sub(Shl(r, 2), 2) >> 31;
                k = MipsDivU(0x80000000u, U(Add(x, y)));
            }
            L112 = U(FixMul(S(L112), S(k)));
            g.W32(fr + 112, L112);
            L116 = U(FixMul(S(L116), S(k)));
            g.W32(fr + 116, L116);
        }
    }
    int32_t s3 = Add(FixMul(S(L112), Sub(g.S32(CP), g.S32(P0))), FixMul(S(L116), Sub(g.S32(CP + 8), g.S32(P0 + 8))));
    int32_t s2 = Add(FixMul(S(L112), Sub(g.S32(P1), g.S32(CP))), FixMul(S(L116), Sub(g.S32(P1 + 8), g.S32(CP + 8))));
    if (prevF & 0x400) {
        s1 = Add(MulLo(Div16(g.S16(e + 482)), g.S32(e + 304)), 2048);
    } else if (isView) {
        if (s5 & 0x10000u) {
            int32_t v;
            if (s5 & 0x400u) v = GProject(g, P1, nrm, cur + 8);
            else v = GProject(g, P1, other + 32, other + 8);
            lim2 = v;
            const int32_t ext = Add(Half(MulLo(g.S32(e + 308), 3)), Shl(g.S32(e + 304), 1));
            if (0x8000 < Iabs(lim2)) {
                lim2 = s1;
                s1 = Add(s1, ext);
            } else {
                lim2 = Add(s1, ext);
            }
        }
    }
    if (!(s5 & 0x10000u)) lim2 = s1;
    if (s1 < Iabs(s3) && lim2 < Iabs(s2) && (s3 ^ s2) < 0) return Next::Seg;
    // ---- 0x800B50DC: the height test
    const uint32_t ty = prevF & 0xF;
    if (ty == 1 || (force != 0 && (prevF & 0x400))) {
        if ((g.U16(e + 172) >> 5) < 3) g.W32(P0 + 4, g.U32(e + 508));
        if (ty == 1 && g.U16(s8 + 38) != 0) {
            const int32_t a3v = Iabs(s3), a2v = Iabs(s2);
            s3 = SDiv(a3v, Add(a3v, a2v));
            segVal = s3;
            if (g.U16(s8 + 38) == 1) segVal = Sub(0x10000, s3);
        }
        const int32_t lvl = Sub(g.S32(P0 + 4), segVal);
        uint32_t s0 = 0;
        while (s0 < 8) {
            if (lvl < g.S32(e + 200 + 12 * s0)) break;
            ++s0;
        }
        if (s0 >= 8) {
            bool skip = true;
            const uint32_t hh = g.U16(e + 172);
            if ((hh >> 5) < 2 && S(hh & 0x1F) < g.S32(GameState(g) + 48) && (prevF & 0x200)) {
                g.W32(fr + 16, COUNT);
                const uint32_t r = CornerMin(g, e + 196, NX, P0, 0, COUNT);
                if (r != 8) {
                    const int32_t a = GDot(g, HEAD, NX);
                    if (Iabs(a) < 22413) {
                        skip = false;
                        if (a > 0) NegN();
                        s5 &= 0xFFFFFFFEu;
                    } else if (g.S32(COUNT) == 8) {
                        uint32_t who, vo;
                        if (isBike) {
                            who = g.U32(e + 852);
                            vo = kCollViews + 1132u * g.U16(e + 172);
                        } else {
                            who = e;
                            vo = kCollViews + 1132u * g.U16(g.U32(e + 596) + 172);
                        }
                        g.W32(vo + 788, 0xFFFF0000u);                          // 0x800B5354
                        g.W32(vo + 548, g.U32(vo + 548) | 0x40000u);
                        g.W32(who + 552, g.U32(who + 552) | 0x80000u);
                    }
                }
            }
            if (skip) return Next::Seg;
        }
    }
    // ---- 0x800B5380: the push I
    if (s5 & 2u) {
        const uint32_t s0 = s5 & 1u;
        const int32_t k = Add(S((0u - s0) & 0xFFFFC000u), 8192);
        g.W32(fr + 16, U(k));
        int16_t h[3], n[3];
        int32_t o[3];
        Read16x3(g, HEAD, h);
        Read16x3(g, NX, n);
        Blend16To32(h, n, o, Sub(Sub(g.S32(DEPTH), travel), 4096), k);        // SLUS 0x8002ECB8
        Write32x3(g, I, o);
        if (s0) NegN();
    } else {
        if ((s5 & 5u) == 5u) NegN();
        if ((g.U16(e + 172) >> 5) != 1 && !(s5 & 4u)) g.W32(DEPTH, U(Add(g.S32(DEPTH), 8192)));
        if ((prevF & 0x400) && !(s5 & 1u)) g.W32(DEPTH, U(Add(g.S32(DEPTH), Shl(Div16(g.S16(e + 482)), 13))));
        GScale(g, g.S32(DEPTH), NX, I);
    }
    // ---- 0x800B54A0: apply it
    if (isBike) {
        if (g.U32(ent2 + 1088) == 0) ent2 = g.U32(ent2 + 856);
        const uint32_t e2 = ent2;
        const uint32_t old = g.U32(e2 + 552);
        g.W32(e2 + 552, U(tmin) >= old ? U(tmin) : old);                      // 0x800B54FC, unsigned MAX
        const int32_t n = g.S32(kContactCount);
        if ((g.U16(e + 320) & 4) && n < 8) {
            const uint32_t rec = kContactList + 36u * U(n);
            g.W16(rec + 0, g.U16(e2 + 172));
            const uint16_t idv = g.U16(s8 + 0);
            g.W32(rec + 12, s8);
            g.W16(rec + 2, idv);
            g.W16(rec + 4, g.U16(NX));
            g.W16(rec + 6, g.U16(NY));
            g.W16(rec + 10, prevF);
            g.W16(rec + 8, g.U16(NZ));
            g.W32(rec + 16, g.U32(I));
            g.W32(rec + 20, g.U32(I + 4));
            g.W32(rec + 28, U(segVal));
            g.W32(rec + 32, g.U32(DEPTH));
            g.W32(rec + 24, g.U32(I + 8));
            if (isBike >= 2) {
                const int32_t v = ContactMerge(g, rec);
                g.W32(kContactCount, U(Add(g.S32(kContactCount), v)));
            } else {
                g.W32(kContactCount, U(Add(n, 1)));
            }
        } else {
            g.W32(fr + 16, s8);
            g.W32(fr + 20, I);
            if (!Unported(coll::kBikeWallHit, {e2, NX, prevF, U(segVal), s8, I})) return Next::Return;
        }
    } else if (isView) {
        if (s5 & 0x20000u) {
            const uint32_t pv = cur - step;
            const int32_t a = MulLo(g.S16(pv + 32), Shl(S(L112), 12) >> 16);
            const int32_t b = MulLo(g.S16(pv + 36), Shl(S(L116), 12) >> 16);
            cls = Add(Div4(a), Div4(b));
            int32_t s1v = s3;
            const int32_t x = FixMul(Sub(g.S32(P1), g.S32(P0)), S(L112));
            const int32_t y = FixMul(Sub(g.S32(P1 + 8), g.S32(P0 + 8)), S(L116));
            const int32_t s0 = Add(x, y);
            lim2 = s0;
            if ((cls ^ s0) >= 0) s1v = s2;
            s1v = Add(Iabs(s1v), 16384);
            if (cls < 0) s1v = Neg(s1v);
            g.W32(I, U(Add(g.S32(I), FixMul(S(L112), s1v))));
            g.W32(I + 8, U(Add(g.S32(I + 8), FixMul(S(L116), s1v))));
        }
        ApplyImpulse(g, e, I, 0);
        if (g.U32(view + 768) == 0) g.W32(view + 768, s8);                     // 0x800B574C
    } else {
        ApplyImpulse(g, e, I, 1);
        const uint32_t hh = g.U16(e + 172), pool = hh >> 5, slot = hh & 0x1F;
        if (pool == 2) {
            const uint32_t ped = g.U32(0x800D4B80u) + 572u * slot;
            g.W32(fr + 20, 0);
            g.W32(fr + 16, ped + 172);
            if (!Unported(coll::kPedHit, {ped, NX, 0, NX, ped + 172, 0})) return Next::Return;
        } else if (pool == 1) {
            if (!Unported(coll::kRiderWallHit, {g.U32(0x8005B3A4u) + 628u * slot, NX, prevF})) return Next::Return;
        } else if (pool == 4) {
            if (!Unported(coll::kPropTopple, {g.U32(0x800CD6D4u) + 596u * slot, 0, NX, 0xFFFFFFFFu}))
                return Next::Return;
        }
    }
    // ---- 0x800B5858
    s7 |= 0x100u;
    result = s7;
    if (!isView) {
        g.W32(e + 388, g.U32(e + 388) | ((s5 & 1u) << 4));                     // 0x800B5984
        ret = s7;
        return Next::Return;
    }
    return Next::Seg;
}

} // namespace

bool WallContact(GuestRam& g, uint32_t e, int32_t force, uint32_t sp, const BikeTables& t, CollisionCallees& c,
                 uint32_t& v0) {
    v0 = 0;
    Wall w(g, t, c);
    w.e = e;
    w.force = force;
    w.fr = sp - 296;
    const uint32_t fr = w.fr;
    w.NX = fr + 24; w.NY = fr + 26; w.NZ = fr + 28;
    w.P0 = fr + 32; w.P1 = fr + 48; w.CP = fr + 64; w.A = fr + 80; w.I = fr + 96;
    w.DEPTH = fr + 120; w.COUNT = fr + 124;
    w.HEAD = e + 450;
    g.W32(sp + 4, U(force));                                                   // 0x800B3B18: home spill
    const uint32_t f = g.U32(e + 388);
    w.hi12 = S(f) >> 20;
    g.W32(e + 388, f & 0x000FFFEFu);                                           // 0x800B3B5C
    const uint32_t h = g.U16(e + 172);
    if ((h >> 5) == 4 && (h & 0x1F) >= 30) {
        w.isView = true;
        w.view = e;
        g.W32(e + 768, 0);                                                     // 0x800B3B90
    }
    const uint32_t S0 = g.U32(e + 328);
    int32_t segCount = 0, groups = 0, first = 0;
    uint32_t groupNo = 0;
    uint32_t q = g.U32(e + 496);
    if (q != 0) {                                                              // 0x800B3BA8
        segCount = g.U8(q + 3);
        if (segCount == 0) return !g.Faulted();
        if ((g.U32(e + 388) & 1u) == 0) {
            if (g.U32(e + 372) == 0) return !g.Faulted();
            if (g.S16(e + 392) == 4) return !g.Faulted();
        }
        groups = 1;
        groupNo = 0;
        first = g.S16(g.U32(e + 496));
        w.s5 |= 0x100u;
    } else {
        q = g.U32(e + 492);
        if (q == 0) return !g.Faulted();
        if (g.U32(e + 372) == 0) return !g.Faulted();
        const int32_t lat = g.S32(e + 344);
        if (lat < 0) {
            groups = g.U8(q + 1);
            if (groups == 0) return !g.Faulted();
            const int32_t prod = MulLo(g.S32(e + 424), g.S16(e + 408));
            groupNo = g.U8(q + 0);
            if (Sub(prod, 32768) < Neg(lat)) w.s5 = 64;
        } else {
            groups = g.U8(q + 3);
            if (groups == 0) return !g.Faulted();
            const int32_t prod = MulLo(g.S32(e + 424), g.S16(e + 420));
            groupNo = g.U8(q + 2);
            if (Sub(prod, 32768) < lat) w.s5 = 128;
        }
        const uint32_t gr = g.U32(S0 + 100) + 4u * groupNo;
        segCount = g.U8(gr + 3);
        first = g.S16(gr);
        if (force == 0 && !w.isView && w.s5 == 0) return !g.Faulted();
    }
    // 0x800B3D18
    w.isBike = (g.U16(e + 172) >> 5) == 0 ? 1 : 0;
    if (w.isBike) {
        const uint32_t node = g.U32(e + 1088);
        w.ent2 = e;
        if (node == 0) ++w.isBike;
        if (node != 0) {
            int32_t r = 0;
            if (!StaleHeading(g, e, t, r)) return false;
        }
        if (Iabs(g.S32(e + 676)) >= 2130) w.mode = 2;
        if (g.U32(e + 856) != 0 && g.U32(e + 1088) != 0 && (g.U16(e + 320) & 8) == 0) {
            InCameraBox(g, e, 0x140000, 0x1C0000);
            g.W16(g.U32(e + 856) + 320, g.U16(e + 320));                       // 0x800B3DC8
        }
        if (g.S32(w.ent2 + 636) < 0) w.cls |= 2;
    }
    int32_t gi = 0;
    if (groups > 0) {
        g.W32(fr + 232, w.HEAD);
        g.W32(fr + 240, w.P0);
    }
    while (gi < groups) {                                                      // 0x800B3E14
        if (gi > 0) {
            const uint32_t gr = g.U32(S0 + 100) + 4u * groupNo;
            segCount = g.U8(gr + 3);
            first = g.S16(gr);
            if (w.isView && g.U32(w.view + 768) != 0) { v0 = 1; return !g.Faulted(); }
        }
        w.dirn = 1;
        const uint32_t base = g.U32(S0 + 96) + U(MulLo(40, first));
        const uint32_t endp = base + U(MulLo(40, segCount));
        uint32_t cur = base;
        uint32_t other = endp - 40;
        uint32_t R = g.U32(e + 340);
        int32_t s3 = GProject(g, base + 8, R + 2, R + 20);
        R = g.U32(e + 340);
        int32_t v = GProject(g, endp - 20, R + 2, R + 20);
        R = g.U32(e + 340);
        GMulAdd(g, endp - 20, R + 2, Sub(s3, v), w.P1);
        s3 = GProject(g, base + 8, w.HEAD, e + 184);
        v = GProject(g, w.P1, w.HEAD, e + 184);
        if (v < s3) {
            w.dirn = -1;
            cur = other;
        }
        w.s5 &= 0xFFFEF3FFu;
        uint16_t prevF = 0;
        other = 0;
        int32_t si = 0;
        if (segCount > 0) {
            const uint32_t step = U(MulLo(w.dirn, 40));
            uint32_t nrm = cur + 32;
            g.W32(fr + 252, nrm);
            while (true) {                                                     // 0x800B3F84
                const uint32_t ty = g.U16(cur + 2) & 0xF;
                if (ty == 0 || ty >= 9) g.W16(cur + 2, 1026);                  // 0x800B3FB0 (road data)
                uint32_t s8 = 0;
                bool skip = false;
                if (w.isView) {
                    uint32_t s5 = w.s5;
                    if ((g.U16(cur + 2) & 0x100) == 0) {
                        skip = true;
                    } else {
                        if (g.U32(w.view + 768) != 0) {
                            if ((s5 & 0x10800u) != 0x10000u) { v0 = 1; return !g.Faulted(); }
                            if (!ViewTrack(g, w.view, 1, t)) return false;
                            s5 |= 0x800u;
                        }
                        s5 &= 0xFFFDFFFFu;
                        if ((s5 & 0x18u) == 0) {
                            if ((s5 & 0x800u) == 0) s5 &= 0xFFFEFFFFu;
                            if (s5 & 0x400u) {
                                s8 = other - step;
                                s5 &= 0xFFFFFBFFu;
                            } else if (si < segCount - 1) {
                                const uint32_t nx = step + cur;
                                if (g.U16(nx + 2) & 0x100) {
                                    other = nx;
                                    for (uint32_t k = 0; k < 3; ++k)
                                        g.W32(w.P0 + 4 * k, U(Half(Add(g.S32(nx + 8 + 4 * k), g.S32(nx + 20 + 4 * k)))));
                                    g.W32(w.DEPTH, U(GProject(g, w.P0, nrm, cur + 20)));
                                    const int32_t d3 = GDot(g, nrm, w.HEAD);
                                    const int32_t d2 = GDot(g, other + 32, w.HEAD);
                                    if (0x8000 < g.S32(w.DEPTH)) {
                                        s5 |= Iabs(d3) < Iabs(d2) ? 0x10400u : 0x10000u;
                                    } else {
                                        const uint32_t b = (s5 & 0x10000u) ? 0u : (d3 < -46333 ? 1u : 0u);
                                        s5 |= b << 10;
                                    }
                                }
                            }
                            if ((s5 & 0x400u) == 0 && si > 0) {                // 0x800B41A8
                                const uint32_t pv = cur - step;
                                if (g.U16(pv + 2) & 0x100) {
                                    const int32_t d2 = GDot(g, nrm, pv + 32);
                                    const int32_t d3 = GDot(g, nrm, w.HEAD);
                                    const uint32_t b = (0xB4FC < d2) ? 0u : (0xDDB2 < d3 ? 1u : 0u);
                                    s5 |= b << 17;
                                    if (s5 & 0x20000u) s5 &= 0xFFFEFFFFu;
                                }
                            }
                            if (s5 & 0x400u) s8 = other;                       // 0x800B4240
                        }
                    }
                    w.s5 = s5;
                }
                if (!skip) {
                    uint32_t ret = 0;
                    const Wall::Next nx = w.Segment(cur, s8, nrm, step, other, prevF, segCount, groups, ret);
                    if (w.failed) return false;
                    if (nx == Wall::Next::Group) break;
                    if (nx == Wall::Next::Return) { v0 = ret; return !g.Faulted(); }
                }
                ++si;                                                          // 0x800B5868
                if ((w.s5 & 0x18u) == 0) {
                    nrm += step;
                    cur += step;
                    g.W32(fr + 252, nrm);
                }
                if (!(si < segCount)) break;
            }
        }
        ++groupNo;                                                             // 0x800B58B8
        ++gi;
    }
    // 0x800B58DC
    if (w.result != 0) { v0 = w.result; return !g.Faulted(); }
    const uint32_t bs = w.bestSeg;
    if (bs == 0) return !g.Faulted();
    uint32_t R = g.U32(e + 340);
    g.W32(e + 388, g.U32(e + 388) | 0x10u);                                    // 0x800B5914
    int32_t x = GProject(g, bs + 8, R + 2, R + 20);
    R = g.U32(e + 340);
    int32_t y = GProject(g, bs + 20, R + 2, R + 20);
    x = Iabs(x);
    y = Iabs(y);
    const int32_t d = y < x ? x : y;
    const int32_t h12 = Shl(d, 3) >> 16;
    g.W32(e + 388, (U(h12) << 8) | (g.U32(e + 388) & 0xFFF000FFu));           // 0x800B59B8
    return !g.Faulted();
}


// ============================================================================ RASHCDG 0x800A7AB4
namespace {
// The prologue's register spills, written where the original writes them: an unported
// callee called later at a deeper frame reads these words when it reads its own uninitialised stack.
void Spill(GuestRam& g, uint32_t fr, const uint32_t (&off)[10], const GuestRegs& r) {
    // off[k]: the frame offset of s0..s8 (k = 0..8) and ra (k = 9), or 0xFFFFFFFF when not saved
    for (uint32_t k = 0; k < 9; ++k)
        if (off[k] != 0xFFFFFFFFu) g.W32(fr + off[k], r.s[k]);
    if (off[9] != 0xFFFFFFFFu) g.W32(fr + off[9], r.ra);
}
constexpr uint32_t kNo = 0xFFFFFFFFu;
} // namespace

bool ChainReaction(GuestRam& g, uint32_t e, uint32_t vec, uint32_t skip, uint32_t sp, CollisionCallees& c,
                   const GuestRegs* caller) {
    const uint32_t fr = sp - 48;
    // sw s0,16 / s1,20 / s2,24 / s3,28 / s4,32 / s5,36 / s6,40 / ra,44 (0x800A7AC0..0x800A7AF0)
    static constexpr uint32_t kSpill[10] = {16, 20, 24, 28, 32, 36, 40, kNo, kNo, 44};
    if (caller != nullptr) Spill(g, fr, kSpill, *caller);
    if (g.S32(0x8005B1F8u) <= 0) return !g.Faulted();
    uint32_t s1 = 0;
    do {
        const uint32_t a2 = s1 & 0xFFFFu;
        if (a2 != g.U16(e + 172) && a2 != (skip & 0xFFFFu)) {
            const uint32_t mine = g.U32(kChainPrev + ((a2 >> 4) << 2)) >> ((s1 & 0xFu) << 1);
            const uint32_t eh = g.U16(e + 172);
            const uint32_t his = (g.U32(kChainPrev + ((eh >> 4) << 2)) >> ((eh & 0xFu) << 1)) & 3u;
            if (mine & his) {
                const uint32_t o = g.U32(0x8005B3A0u) + U(MulLo(1096, S(a2)));
                uint32_t r = 0;
                const uint32_t a[3] = {a2 < eh ? e : o, a2 < eh ? o : e, 1};
                bool ok;
                if (caller != nullptr) {
                    // the registers at 0x800A7B98: s0 = o, s1 = the counter, s2 = e, s3 = 0x800CCF70,
                    // s4 = skip, s5 = 0x80060000, s6 = vec, s7 / s8 the caller's; ra = 0x800A7BA0
                    GuestRegs rr = *caller;
                    rr.s[0] = o; rr.s[1] = s1; rr.s[2] = e; rr.s[3] = kChainPrev; rr.s[4] = skip;
                    rr.s[5] = 0x80060000u; rr.s[6] = vec; rr.ra = 0x800A7BA0u;
                    ok = c.UnportedAt(coll::kBikeVsBike, a, 3, fr, rr, r);
                } else {
                    ok = c.Unported(coll::kBikeVsBike, a, 3, fr, r);
                }
                if (!ok) return false;
                if (r != 0) ApplyImpulse(g, o, vec, 1);
            }
        }
        ++s1;
    } while (S(s1 & 0xFFFFu) < g.S32(0x8005B1F8u));
    return !g.Faulted();
}

// ============================================================================ RASHCDG 0x800A77B0
bool DeferredContacts(GuestRam& g, uint32_t sp, CollisionCallees& c, const GuestRegs* caller) {
    using namespace coll;
    const uint32_t fr = sp - 64;
    // sw s0,24 / s1,28 / s2,32 / s3,36 / s4,40 / s5,44 / s6,48 / s7,52 / ra,56 (0x800A77BC..0x800A77E8)
    static constexpr uint32_t kSpill[10] = {24, 28, 32, 36, 40, 44, 48, 52, kNo, 56};
    if (caller != nullptr) Spill(g, fr, kSpill, *caller);
    GuestRegs rr = caller != nullptr ? *caller : GuestRegs{};
    auto call = [&](uint32_t fn, const uint32_t* a, int n, uint32_t ra, uint32_t& v0) {
        if (caller == nullptr) return c.Unported(fn, a, n, fr, v0);
        rr.ra = ra;
        return c.UnportedAt(fn, a, n, fr, rr, v0);
    };
    uint32_t reacted = 0;                                                       // s4
    const int32_t count0 = g.S32(kContactCount);
    if (count0 - 1 >= 0) { rr.s[5] = 0x800CE598u; rr.s[6] = 0x800CD6A8u; rr.s[7] = 0x80060000u; }
    for (int32_t i = count0 - 1; i >= 0; --i) {                                 // BACKWARDS
        const uint32_t r = kContactList + 36u * U(i);
        rr.s[0] = r; rr.s[2] = U(i); rr.s[3] = 36u * U(i); rr.s[4] = reacted;
        const uint32_t hb = g.U16(r + 2);
        const uint32_t pool = hb >> 5;
        if (pool == 0) continue;
        const uint32_t e = g.U32(0x8005B3A0u) + U(MulLo(1096, S(g.U16(r + 0))));
        rr.s[1] = e;
        uint32_t v0 = 0;
        if (pool == 8) {
            const uint32_t a[6] = {e, r + 4, g.U16(r + 10) & 0xFFFu, g.U32(r + 28), g.U32(r + 12), r + 16};
            if (!call(kBikeWallHit, a, 6, 0x800A7884u, v0)) return false;
        } else if (pool == 3) {
            const uint32_t w = g.U32(r + 28);
            const uint32_t a[5] = {e, 0x800CF660u + ((hb & 0x1Fu) << 9), w & 0xFFFFu, U(S(w) >> 16), r + 16};
            if (!call(kBikeTrafficReact, a, 5, 0x800A78C4u, v0)) return false;
        } else {
            uint32_t shp;
            if (pool == 6) shp = g.U32(0x800CD6A8u + 28) + 280u * (hb & 0x1Fu);
            else shp = g.U32(0x800CE598u + 12) + 452u * (hb & 0x1Fu) + 172;
            if ((g.U16(shp) >> 5) == 6 && g.U32(shp + 8) == 1) {
                const uint32_t f = g.U16(r + 10);
                const uint32_t a[6] = {e, shp, r + 4, f & 0xFFEFu, f & 0x10u, r + 16};
                if (!call(kPoleReact, a, 6, 0x800A7954u, v0)) return false;
            } else {
                const uint32_t w = g.U32(r + 28);
                const uint32_t a[6] = {e, shp, w & 0xFFFFu, U(S(w) >> 16), r + 16, r + 4};
                if (!call(kBoxReact, a, 6, 0x800A797Cu, v0)) return false;
            }
        }
        const uint32_t h = g.U16(e + 172);
        reacted |= 1u << (h & 31u);                                              // sllv
        rr.s[4] = reacted;
        if ((g.U32(kChainPrev + ((h >> 4) << 2)) >> ((h & 0xFu) << 1)) & 3u) {
            GuestRegs cr = rr;
            cr.ra = 0x800A79C8u;
            if (!ChainReaction(g, e, r + 16, 31, fr, c, caller != nullptr ? &cr : nullptr)) return false;
        }
    }
    const uint32_t count1 = U(g.S32(kContactCount));
    if (S(count1) > 0) rr.s[3] = 0x80060000u;
    for (int32_t i = 0; i < g.S32(kContactCount); ++i) {                        // FORWARDS, count re-read
        const uint32_t r = kContactList + 36u * U(i);
        rr.s[0] = r; rr.s[2] = U(i);
        if ((g.U16(r + 2) >> 5) != 0) continue;
        const uint32_t base = g.U32(0x8005B3A0u);
        const uint32_t e = base + U(MulLo(1096, S(g.U16(r + 0))));
        rr.s[1] = e;
        if ((reacted >> (g.U16(e + 172) & 31u)) & 1u) continue;                  // srav
        const uint32_t w = g.U32(r + 28);
        const uint32_t a[4] = {e, base + U(MulLo(1096, S(g.U16(r + 2)))), w & 0xFFFFu, U(S(w) >> 16)};
        uint32_t v0 = 0;
        if (!call(kBikeBikeReact, a, 4, 0x800A7A70u, v0)) return false;
    }
    return !g.Faulted();
}

// ============================================================================ RASHCDG 0x800A4774
namespace {

struct Pass {
    GuestRam& g;
    const BikeTables& t;
    CollisionCallees& c;
    uint32_t fr = 0;       // the pass's own frame, entry sp - 112
    Pass(GuestRam& gg, const BikeTables& tt, CollisionCallees& cc) : g(gg), t(tt), c(cc) {}

    // 0x800A4A00..0x800A4AAC: the cell of (x, z), or -1 (the same code sits in front of every walk).
    int32_t CellOf(int32_t x, int32_t z) {
        uint32_t col = U(Add(Sub(x, g.S32(kCollGridOrgX)) >> 21, 11));
        uint32_t row = U(Add(Sub(z, g.S32(kCollGridOrgZ)) >> 21, 11));
        const uint32_t np = NumPlayers(g);
        if (np != 1 && !(col < 24u && row < 24u)) {
            col = U(Add(Sub(x, g.S32(kCollGridOrgX + 4)) >> 21, 11));
            const int32_t r = Add(Sub(z, g.S32(kCollGridOrgZ + 4)) >> 21, 11);
            row = U(Add(r, S(24u << ((r >> 31) & 31))));
        }
        if (!(col < 24u)) return -1;
        if (!(row < (24u << ((NumPlayers(g) - 1u) & 31u)))) return -1;
        return S(U(MulLo(S(row), 24)) + col);
    }
    uint8_t NodePrev(uint32_t n) { return g.U8(kCollNodes + 2 * n); }
    uint8_t NodeHandle(uint32_t n) { return g.U8(kCollNodes + 2 * n + 1); }
    // The node after mine in the cell's chain; the sentinel's `prev` when my handle is absent.
    uint32_t Cursor(int32_t cell, uint32_t myh) {
        if (cell < 0) return 128;
        uint32_t a0 = g.U8(kCollGrid + U(cell));
        if (a0 != 128) {
            while (NodeHandle(a0) != myh) {
                a0 = NodePrev(a0);
                if (a0 == 128) break;
            }
        }
        return NodePrev(a0);
    }
    void Gather(uint32_t e, uint32_t cur[4]) {
        const uint32_t myh = g.U16(e + 172);
        for (uint32_t k = 0; k < 4; ++k)
            cur[k] = Cursor(CellOf(g.S32(e + 196 + 12 * k), g.S32(e + 196 + 12 * k + 8)), myh);
    }
    uint32_t MinHandle(const uint32_t cur[4]) {
        const int32_t h0 = NodeHandle(cur[0]), h1 = NodeHandle(cur[1]);
        const int32_t h2 = NodeHandle(cur[2]), h3 = NodeHandle(cur[3]);
        int32_t a1 = h1 - h0;
        const int32_t a3 = h0 + (a1 & (a1 >> 31));
        a1 = h3 - h2;
        const int32_t a2 = h2 + (a1 & (a1 >> 31));
        a1 = a2 - a3;
        return U(a3 + (a1 & (a1 >> 31)));
    }
    // The partner and its point (pool 6: *(0x800CD6C4) + 280 * slot, point +0x0C; else +0xB8).
    void Resolve(uint32_t h, uint32_t& part, uint32_t& pt) {
        if ((h >> 5) == 6) {
            part = g.U32(0x800CD6C4u) + 280u * (h & 31u);
            pt = part + 12;
        } else {
            const uint32_t rec = kPoolTableAddr + ((h >> 1) & 0x7FF0u);
            part = g.U32(rec) + U(MulLo(g.S32(rec + 4), S(h & 31u)));
            pt = part + 184;
        }
    }
    int32_t Octagon(uint32_t pt, uint32_t e) {
        const int32_t dx = Iabs(Sub(g.S32(pt), g.S32(e + 184)));
        const int32_t dz = Iabs(Sub(g.S32(pt + 8), g.S32(e + 192)));
        const int32_t mn = dx < dz ? dx : dz;
        return Sub(Add(dx, dz), S(U(mn) + (U(mn) >> 31)) >> 1);
    }
    void Advance(uint32_t cur[4], uint32_t h) {
        for (uint32_t k = 0; k < 4; ++k)
            if (NodeHandle(cur[k]) == h) cur[k] = NodePrev(cur[k]);
    }
    bool MidWipeout(uint32_t e) {
        const uint32_t f = g.U32(e + 568);
        if (!(f & 0x1E0u)) return false;
        return (f & 0x200000u) != 0 || 0xB2B8 < Iabs(g.S32(e + 652));
    }
    bool U2(uint32_t fn, std::initializer_list<uint32_t> a, uint32_t* v0 = nullptr) { return Call(c, fn, a, fr, v0); }
    bool Dispatch(uint32_t kind, uint32_t e, uint32_t part, uint32_t pool);
    bool Tail(uint32_t kind, uint32_t e);
};

bool Pass::Dispatch(uint32_t kind, uint32_t e, uint32_t part, uint32_t pool) {
    using namespace coll;
    if (kind == 0) {                                                           // table 0x8005B910
        switch (pool) {
        case 1:
        case 2:
            if (U(Sub(g.U16(part + 544), 60)) < 4u) return true;
            return U2(kBikeVsRider, {e, part});
        case 3: return U2(kBikeVsTraffic, {e, part});
        case 4:
            if ((g.U16(g.U32(part) + 14) & 2) && !(g.U32(part + 592) & 0x1A06u)) return U2(kPoleResolve, {e, part + 172});
            return U2(kPointResolve, {e, part + 172});
        case 5:
            if (0x10000 < g.S32(part + 304) && 0x10000 < g.S32(part + 308)) return U2(kBoxResolve, {e, part + 172});
            return U2(kPointResolve, {e, part + 172});
        case 6:
            if (g.U32(part + 8) != 0) return U2(kPoleResolve, {e, part});
            if (0x10000 < g.S32(part + 132) && 0x10000 < g.S32(part + 136)) return U2(kBoxResolve, {e, part});
            return U2(kPointResolve, {e, part});
        default: return true;                                                  // pool 0: none; 7: sltiu 7
        }
    }
    if (kind == 1 || kind == 2) {                                              // table 0x8005B930, pool - 1
        switch (pool) {
        case 1:
        case 2: return U2(kRiderNone, {e, part});
        case 3: return U2(kRiderVsTraffic, {e, part});
        case 4:
        case 5: return U2(kRiderVsShape, {e, part + 172});
        case 6: return U2(kRiderVsShape, {e, part});
        default: return true;
        }
    }
    if (kind == 3) {
        if (pool == 3) return U2(kTrafficVsTraffic, {e, part});
        if (pool == 4) return U2(kTrafficVsProp, {e, part});
        return true;
    }
    if (pool == 5) return U2(kPropVsShape, {e, part + 172});
    if (pool == 6) return U2(kPropVsShape, {e, part});
    return true;
}

bool Pass::Tail(uint32_t kind, uint32_t e) {
    using namespace coll;
    uint32_t r = 0;
    if (kind == 0) {
        if (!WallContact(g, e, (g.U32(e + 568) & 0x600u) ? 1 : 0, fr, t, c, r)) return false;
        const uint32_t p = g.U32(e + 856);                                     // a0 keeps the passenger
        if (p != 0 && (((r & 1u) != ((r & 2u) >> 1)) || (r == 0 && ((S(g.U32(e + 388)) >> 20) & 0xF)))) {
            uint32_t r2 = 0;
            if (!WallContact(g, p, (g.U32(e + 568) & 0x600u) ? 1 : 0, fr, t, c, r2)) return false;
        }
        if ((g.U32(e + 568) & 0x600u) && !(g.U32(e + 564) & 0x800000u)) return AirContact(g, e, fr, t, c);
        if (!(g.U32(e + 564) & 0x400000u)) return true;
        if (g.U32(e + 568) & 0xFu) return true;
        uint32_t v = 0;
        if (!U2(kImpactTurn, {e, e + 172, e + 820, 4}, &v)) return false;
        if (v == 0) return true;
        if (!c.PlaySound3D(g.S32(e + 184), g.S32(e + 192), (g.U32(e + 564) & 0xEu) ? 17 : 3, 0)) return false;
        if (g.U16(e + 172) < NumPlayers(g) && g.U32(g.U32(e + 852) + 604) < 2u && g.U32(0x8005B220u) == 0)
            return c.Rumble(e, 0, g.S32(e + 480), 0x165A1C, 1, fr);
        return true;
    }
    if (kind == 1 || kind == 2) {
        if (!WallContact(g, e, S((g.U32(e + 552) >> 30) & 1u), fr, t, c, r)) return false;
        if ((g.U32(e + 552) & 0xC0000000u) == 0x40000000u && GDot(g, e + 522, e + 450) > 0)
            return U2(kRiderGetUp, {e});
        return true;
    }
    if (kind == 4) {
        if (!WallContact(g, e, S((g.U32(e + 592) >> 1) & 1u), fr, t, c, r)) return false;
        if ((g.U32(e + 592) & 3u) == 2u && GDot(g, e + 522, e + 450) > 0) {
            for (uint32_t k = 0; k < 3; ++k) g.W16(fr + 24 + 2 * k, static_cast<uint16_t>(0u - g.U16(e + 522 + 2 * k)));
            return U2(kPropTopple, {e, e + 504, fr + 24, 1});
        }
    }
    return true;
}

} // namespace

bool CollisionPass(GuestRam& g, int32_t dt, uint32_t sp, const BikeTables& t, CollisionCallees& c) {
    Pass p(g, t, c);
    p.fr = sp - kCollisionPassFrame;
    uint32_t n = 0;                                                            // sp+32
    g.W32(kCollDt, U(dt));                                                     // 0x800A47D8
    for (uint32_t i = 0; i < 48; ++i)
        for (uint32_t j = 0; j < 6; ++j) g.W32(kCollGrid + 24 * i + 4 * j, 0x80808080u);
    {
        const uint32_t b = g.U32(0x8005B38Cu);
        g.W32(kCollGridOrgX, U(Add(g.S32(b + 184), 0xF0000)));
        g.W32(kCollGridOrgZ, U(Add(g.S32(b + 192), 0xF0000)));
        if (!(NumPlayers(g) < 2u)) {
            const uint32_t b2 = g.U32(0x8005B21Cu);
            g.W32(kCollGridOrgX + 4, U(Add(g.S32(b2 + 184), 0xF0000)));
            g.W32(kCollGridOrgZ + 4, U(Add(g.S32(b2 + 192), 0xF0000)));
        }
    }
    bool full = false;
    for (int32_t kind = 6; kind >= 0 && !full; --kind) {                      // statics first, bikes last
        const uint32_t rec = kPoolTableAddr + 16u * U(kind);
        if (g.U32(g.U32(rec + 8)) == 0) continue;
        int32_t last = g.S32(g.U32(rec + 12));
        uint32_t e = g.U32(rec);
        for (; last >= 0 && !full; --last, e += g.U32(rec + 4)) {
            uint32_t pts = 0, npts = 0, h = 0;
            if (kind == 1 && g.U32(e + 604) < 2u) continue;
            if (kind == 6) {
                h = g.U16(e);
                if (h == 0 || g.S16(e + 148) == 0) continue;
                if (g.U32(e + 8) == 1) { pts = e + 12; npts = 1; } else { pts = e + 24; npts = 4; }
            } else {
                const int16_t st = g.S16(e + 320);
                h = g.U16(e + 172);
                if (st == 0) continue;
                if (kind != 0) {
                    if (h == 0) continue;
                } else {
                    g.W32(e + 568, g.U32(e + 568) & 0xFDFFFFFFu);              // 0x800A49A8
                    const uint32_t fl = (g.U32(e + 616) != 0 || g.U32(e + 676) != 0) ? 1u : 0u;
                    const uint32_t fc = g.U32(e + 568);
                    g.W32(e + 552, 0);                                         // 0x800A49C8
                    g.W32(e + 568, fc | (fl << 24));                           // 0x800A49D0
                }
                pts = e + 196;
                npts = 4;
            }
            for (uint32_t k = 0; k < npts; ++k) {
                const int32_t cell = p.CellOf(g.S32(pts + 12 * k), g.S32(pts + 12 * k + 8));
                if (cell < 0) continue;
                const uint32_t head = g.U8(kCollGrid + U(cell));
                if (head != 128 && p.NodeHandle(head) == h) continue;          // full u16 against the byte
                g.W8(kCollNodes + 2 * n, static_cast<uint8_t>(head));
                g.W8(kCollNodes + 2 * n + 1, static_cast<uint8_t>(h));
                g.W8(kCollGrid + U(cell), static_cast<uint8_t>(n));
                ++n;
                if (n == 128) { full = true; break; }
            }
        }
    }
    if (n == 0) return !g.Faulted();
    // 0x800A4B64: the contact globals
    g.W32(kContactCount, 0);
    for (uint32_t i = 0; i < 288; i += 4) g.W32(kContactList + i, 0);          // SLUS 0x8001E100
    {
        const uint32_t t0 = g.U32(kChainCur), t1 = g.U32(kChainCur + 4);
        g.W32(kChainCur, 1);
        g.W32(kChainMask, 0);
        g.W32(0x800CCF90u, 0);
        g.W32(kChainCur + 4, 0);
        g.W32(kChainPrev, t0);
        g.W32(kChainPrev + 4, t1);
        for (uint32_t i = 0; i < 8; i += 4) g.W32(0x800CCF88u + i, 0);
    }
    if (g.Faulted()) return false;
    // 0x800A4BCC: bike against bike
    if (g.U32(g.U32(kPoolTableAddr + 8)) != 0) {
        int32_t last = g.S32(g.U32(kPoolTableAddr + 12));
        uint32_t e = g.U32(kPoolTableAddr);
        for (; last >= 0; --last, e += g.U32(kPoolTableAddr + 4)) {
            if (g.S16(e + 320) == 0) continue;
            const int32_t rad = g.S32(kCollRadius);
            uint32_t cur[4];
            p.Gather(e, cur);
            while (cur[0] != 128 || cur[1] != 128 || cur[2] != 128 || cur[3] != 128) {
                const uint32_t ph = p.MinHandle(cur);
                uint32_t part = 0, pt = 0;
                p.Resolve(ph, part, pt);
                if (p.Octagon(pt, e) < rad && (ph >> 5) == 0) {
                    const bool sM = p.MidWipeout(e), sO = p.MidWipeout(part);
                    bool ok;
                    if ((!sO && sM) || g.U16(g.U32(e + 852) + 544) == 0) ok = p.U2(coll::kPointResolve, {part, e + 172});
                    else if (!sM && sO) ok = p.U2(coll::kPointResolve, {e, part + 172});
                    else if (g.U16(g.U32(part + 852) + 544) == 0) ok = p.U2(coll::kPointResolve, {e, part + 172});
                    else ok = p.U2(coll::kBikeVsBike, {e, part, 0});
                    if (!ok) return false;
                }
                p.Advance(cur, ph);
                if (g.Faulted()) return false;
            }
        }
    }
    // 0x800A54A8: the kind loop
    bool kindRan = false;
    uint32_t kindEnd = 0;
    for (uint32_t kind = 0; kind < 5; ++kind) {
        const uint32_t rec = kPoolTableAddr + 16u * kind;
        if (g.U32(g.U32(rec + 8)) == 0) continue;
        int32_t last = g.S32(g.U32(rec + 12));
        uint32_t e = g.U32(rec);
        kindRan = last >= 0;
        for (; last >= 0; --last, e += g.U32(rec + 4)) {
            if (g.S16(e + 320) == 0) continue;
            if (kind != 0 && g.U16(e + 172) == 0) continue;
            if (kind == 1 && g.U32(e + 604) < 2u) continue;
            const bool off = (g.U32(e + 388) & 1u) || (g.U32(e + 372) != 0 && g.S16(e + 392) != 4);
            const int32_t rad = Add(g.S32(kCollRadius + 4 * kind), off ? 0xA0000 : 0);
            if (kind == 4 && g.U32(e + 480) == 0) continue;
            uint32_t cur[4];
            p.Gather(e, cur);
            while (cur[0] != 128 || cur[1] != 128 || cur[2] != 128 || cur[3] != 128) {
                const uint32_t ph = p.MinHandle(cur);
                uint32_t part = 0, pt = 0;
                p.Resolve(ph, part, pt);
                if (p.Octagon(pt, e) < rad && (ph >> 5) < 7u)
                    if (!p.Dispatch(kind, e, part, ph >> 5)) return false;
                p.Advance(cur, ph);
                if (g.Faulted()) return false;
            }
            if (!p.Tail(kind, e)) return false;
            if (g.Faulted()) return false;
        }
        kindEnd = e;
        if (kind == 0 && g.U32(kContactCount) != 0)
            {
                // the pass's registers at 0x800A7760 (the kind loop's constants and the walk's end)
                GuestRegs pr;
                pr.s[0] = kindEnd; pr.s[1] = kindEnd + 196; pr.s[2] = 0x80060000u; pr.s[3] = 24;
                pr.s[4] = 0; pr.s[5] = 0xFFFFFFFFu; pr.s[6] = 1; pr.s[7] = kCollGrid; pr.s[8] = kCollGridOrgZ;
                pr.ra = 0x800A7768u;
                if (!DeferredContacts(g, p.fr, c, kindRan ? &pr : nullptr)) return false;   // 0x800A7760
            }
    }
    return !g.Faulted();
}


// ============================================================================ RASHCDG 0x8007EA64
namespace {
// Normalize as a caller that tests its v0 sees it (bike_step.cpp's rule): v0 is the sum of squares
// (GTE SQR, two TRAPPING adds); false where the console raises the overflow exception.
bool NormalizeN(GuestRam& g, uint32_t a, const uint16_t* rsqrt, int32_t& n) {
    int16_t v[3];
    Read16x3(g, a, v);
    const int64_t s1 = static_cast<int64_t>(v[0]) * v[0] + static_cast<int64_t>(v[1]) * v[1];
    if (s1 > INT32_MAX) return false;
    const int64_t s2 = s1 + static_cast<int64_t>(v[2]) * v[2];
    if (s2 > INT32_MAX) return false;
    if (!Normalize(v, rsqrt)) return false;
    Write16x3(g, a, v);
    n = static_cast<int32_t>(s2);
    return true;
}
// cop2 0x178000C with R11/R22/R33 = r[0..2] and IR1..3 = ir, stored to out.
void GOp(GuestRam& g, uint32_t r, uint32_t ir, uint32_t out) {
    int16_t d[3], i[3], o[3];
    Read16x3(g, r, d);
    Read16x3(g, ir, i);
    OuterProduct(d, i, o);
    Write16x3(g, out, o);
}
} // namespace

bool RowsFromUp(GuestRam& g, uint32_t e, const BikeTables& t) {
    const uint32_t up = e + 522, r204 = e + 516, r210 = e + 528;
    GOp(g, up, e + 444, r204);
    int32_t n = 0;
    if (!NormalizeN(g, r204, t.rsqrt, n)) return false;
    if (n != 0) {
        GOp(g, r204, up, r210);                                                // 0x8007EBBC
        return !g.Faulted();
    }
    GOp(g, e + 432, up, r210);
    if (!NormalizeN(g, r210, t.rsqrt, n)) return false;
    if (n == 0) BikeResetOrientation(g, e);                                    // 0x8008CF74
    GOp(g, up, r210, r204);
    return !g.Faulted();
}

// ============================================================================ RASHCDG [0x8007C23C, 0x8007C9A8)
namespace {
// The angle of a 16.16 radian value in the sine table's 4096-per-turn unit: bits 14..25 of 163 v
// (the compiler's 652 v >> 16, 0x8007C65C / 0x8007C694 / 0x8007C710).
uint32_t Angle(int32_t v) { return (U(MulLo(v, 163)) >> 14) & 0xFFFu; }
// The move-and-stop idiom of 0x8007C460 / 0x8007C5AC: value += FixMul(rate, dt); both are zeroed
// once the value has the rate's sign.
void RateStep(GuestRam& g, uint32_t value, uint32_t rate, int32_t dt, uint32_t alsoZero = 0) {
    const int32_t nv = Add(FixMul(g.S32(rate), dt), g.S32(value));
    if (alsoZero != 0) g.W32(alsoZero, 0);
    g.W32(value, U(nv));
    const uint32_t mask = (S(U(nv) ^ g.U32(rate)) < 0) ? 0xFFFFFFFFu : 0u;
    const uint32_t r = g.U32(rate);
    g.W32(value, mask & U(nv));
    g.W32(rate, mask & r);
}
} // namespace

bool RiderPassThrownWalk(GuestRam& g, int32_t dt, uint32_t sp, const BikeTables& t, ThrownWalkCallees& c) {
    constexpr uint32_t kHead = 0x8005B2D8;
    uint32_t node = g.U32(kHead + 4);
    for (int guard = 0; node != kHead; ++guard) {
        if (guard > 4096 || g.Faulted()) return false;
        const uint32_t e = node - 1088;
        const int32_t a1 = FixMul(g.S32(kAirDrag), dt);
        if (!(g.U32(e + 564) & 0x100000u)) {
            const int32_t x = g.S32(e + 456), y = g.S32(e + 460);
            const int32_t fy = Sub(FixMul(a1, y), FixMul(g.S32(e + 484), dt));  // drag - gravity
            const int32_t z = g.S32(e + 464);
            g.W32(e + 456, U(Sub(x, FixMul(a1, x))));
            g.W32(e + 460, U(Sub(y, fy)));
            g.W32(e + 464, U(Sub(z, FixMul(a1, z))));
        }
        // 0x8007C358: the speed and the heading from the velocity
        {
            const int32_t vx = g.S32(e + 456), vy = g.S32(e + 460), vz = g.S32(e + 464);
            const int32_t sq = Add(FixMul(vz, vz), Add(FixMul(vy, vy), FixMul(vx, vx)));
            g.W32(e + 580, U(sq));
            const int32_t r = Shl(SqrtGte(sq, t.sqrt), 2);
            g.W32(e + 480, U(r));
            g.W32(e + 576, U(r));
            if (r != 0) {
                int32_t k;
                if (r >= 0) {
                    k = S(MipsDivU(0x80000000u, U(Add(r >> 1, Sub(r, 2) >> 31))));
                } else {
                    const int32_t nr = Neg(r);
                    k = Neg(S(MipsDivU(0x80000000u, U(Add(nr >> 1, Sub(nr, 2) >> 31)))));
                }
                GScaleTo16(g, k, e + 456, e + 450);
            }
        }
        // 0x8007C450: the pitch rate +0x26C, from the stat block when +0x270 is 0
        if (g.S32(e + 624) != 0) RateStep(g, e + 620, e + 624, dt);
        else g.W32(e + 620, g.U32(g.U32(e + 556) + 424));
        {
            const int32_t v = Add(FixMul(g.S32(e + 620), dt), g.S32(e + 616));
            const int32_t lo = Add(v, S(U(Add(v, 0x138C3) >> 31) & U(Sub(S(0xFFFEC73Du), v))));
            const int32_t d = Sub(0x138C3, v);
            const uint32_t fc = g.U32(e + 568);
            g.W32(e + 616, U(Add(lo, S(U(d >> 31) & U(d)))));                    // clamp +-1.2217
            if ((fc & 0xFu) && !(fc & 0x4000u)) {
                int32_t step = dt;
                const int32_t el = Add(g.S32(e + 724), dt);
                g.W32(e + 724, U(el));
                const int32_t du = g.S32(e + 728);
                if (du < el) step = Add(dt, Sub(du, el));
                g.W32(e + 692, U(Add(FixMul(g.S32(e + 744), step), g.S32(e + 692))));
            }
        }
        // 0x8007C59C: the yaw rate
        if (g.S32(e + 640) != 0) RateStep(g, e + 636, e + 640, dt, e + 672);
        int32_t yaw;
        if (g.U32(e + 856) != 0) {
            const int32_t v1 = g.S32(e + 636), f28c = g.S32(e + 652);
            const uint32_t sgn = v1 < 0 ? (U(f28c) >> 31) : 0u;
            const int32_t mx = v1 < f28c ? f28c : v1;
            yaw = S((0u - sgn) & U(mx));
        } else {
            yaw = g.S32(e + 636);
        }
        // the rows +0x1B0 from the three angles (0x8007C680..0x8007C8E0)
        {
            const uint32_t ay = Angle(yaw), ar = Angle(g.S32(e + 616)), ap = Angle(g.S32(e + 692));
            const int32_t sy = Shl(t.sincos[2 * ay], 4), cy = Shl(t.sincos[2 * ay + 1], 4);
            const int32_t sr = Shl(t.sincos[2 * ar], 4), cr = Shl(t.sincos[2 * ar + 1], 4);
            const int32_t sq = Shl(t.sincos[2 * ap], 4), cq = Shl(t.sincos[2 * ap + 1], 4);
            const int32_t s5 = FixMul(sy, sr);
            const int32_t rc = FixMul(sr, cy);
            int32_t m[9];
            m[0] = Add(FixMul(cy, cq), FixMul(s5, sq));
            m[1] = FixMul(sy, cr);
            m[2] = Sub(FixMul(s5, cq), FixMul(cy, sq));
            m[3] = Sub(FixMul(rc, sq), FixMul(sy, cq));
            m[4] = FixMul(cy, cr);
            m[5] = Add(FixMul(sy, sq), FixMul(rc, cq));
            m[6] = FixMul(cr, sq);
            m[7] = Neg(sr);
            m[8] = FixMul(cr, cq);
            for (uint32_t i = 0; i < 6; ++i) g.W16(e + 432 + 2 * i, static_cast<uint16_t>(m[i] >> 4));
            g.W16(e + 444, static_cast<uint16_t>(m[6] >> 4));
            g.W16(e + 448, static_cast<uint16_t>(m[8] >> 4));
            g.W16(e + 446, static_cast<uint16_t>(m[7] >> 4));
        }
        if (!RowsFromUp(g, e, t)) return false;
        // 0x8007C8E4: a long drop
        {
            const bool drop = 0x30000 < g.S32(e + 768);
            uint32_t fa = g.U32(e + 560);
            if (drop) fa |= 0x800000u;
            g.W32(e + 560, fa);
            if (0x30000 < g.S32(e + 768)) {
                const uint32_t r = g.U32(e + 852);
                const uint32_t w = g.U32(r + 552);
                if (!(w & 0x2000u)) {
                    g.W32(r + 552, w | 0x2000u);
                    if (!c.StanceEvent(2, g.U32(e + 852), 10, sp)) return false;
                }
            }
        }
        if (!(g.U32(e + 560) & 0x08000000u)) {
            if (g.U32(e + 856) != 0 && g.U32(e + 1088) != 0)
                if (!c.PassengerLaunch(e, sp)) return false;
            if (!c.CrashTimer(e, dt, sp)) return false;
        }
        node = g.U32(node + 4);                                                // 0x8007C998
    }
    return !g.Faulted();
}

} // namespace rr::sim
