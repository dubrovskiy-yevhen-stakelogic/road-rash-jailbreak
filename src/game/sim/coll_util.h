#pragma once
// Shared inline helpers for the contact / impact ports of the collision family: the R3000
// arithmetic idioms and the guest-address wrappers of the ported SLUS leaves. Copied
// from collision.cpp's private helpers (that file keeps its own); nothing here is a port of a game
// function of its own - every wrapper forwards to the ported leaf named beside it.
#include <cstdint>
#include <initializer_list>

#include "game/sim/ai.h"
#include "game/sim/collision.h"
#include "game/sim/fixed.h"
#include "game/sim/road_query.h"
#include "game/sim/vec.h"

namespace rr::sim::cu {

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
// `mult` then `(lo >> 16) | (hi << 16)`: FixMul (SLUS 0x8001FC90) inlined or called.
inline int32_t Mid(int32_t a, int32_t b) { return FixMul(a, b); }
// The `sra/addu/xor` absolute value: INT32_MIN stays INT32_MIN.
inline int32_t Iabs(int32_t x) {
    const uint32_t s = U(x >> 31);
    return S((U(x) + s) ^ s);
}
// `srl t,x,31; addu; sra 1`: C's x / 2.
inline int32_t Half(int32_t x) { return S(U(x) + (U(x) >> 31)) >> 1; }
inline int32_t Div4(int32_t x) { return (x < 0 ? Add(x, 3) : x) >> 2; }
inline int32_t Div16(int32_t x) { return (x < 0 ? Add(x, 15) : x) >> 4; }
// The unsigned reciprocal FixDiv (SLUS 0x80010028).
inline int32_t FDiv(int32_t a, int32_t b) { return S(FixDiv(U(a), U(b))); }
// The sign-magnitude idiom around the unsigned FixDiv, branching on a > 0 and then b > 0.
inline int32_t SDiv(int32_t a, int32_t b) {
    if (a > 0) return b > 0 ? FDiv(a, b) : Neg(FDiv(a, Neg(b)));
    return b > 0 ? Neg(FDiv(Neg(a), b)) : FDiv(Neg(a), Neg(b));
}
// R3000 `div` (lo) and `divu` (lo): every corner case defined.
inline int32_t MipsDiv(int32_t n, int32_t d) {
    if (d == 0) return n >= 0 ? -1 : 1;
    if (U(n) == 0x80000000u && d == -1) return n;
    return n / d;
}
inline int32_t MipsRem(int32_t n, int32_t d) {
    if (d == 0) return n;
    if (U(n) == 0x80000000u && d == -1) return 0;
    return n % d;
}
inline uint32_t MipsDivU(uint32_t n, uint32_t d) { return d == 0 ? 0xFFFFFFFFu : n / d; }

inline void Read16x3(GuestRam& g, uint32_t a, int16_t v[3]) {
    for (uint32_t k = 0; k < 3; ++k) v[k] = g.S16(a + 2u * k);
}
inline void Write16x3(GuestRam& g, uint32_t a, const int16_t v[3]) {
    for (uint32_t k = 0; k < 3; ++k) g.W16(a + 2u * k, static_cast<uint16_t>(v[k]));
}
inline void Read32x3(GuestRam& g, uint32_t a, int32_t v[3]) {
    for (uint32_t k = 0; k < 3; ++k) v[k] = g.S32(a + 4u * k);
}
inline void Write32x3(GuestRam& g, uint32_t a, const int32_t v[3]) {
    for (uint32_t k = 0; k < 3; ++k) g.W32(a + 4u * k, U(v[k]));
}
// RASHCDG 0x800B6AAC AiProject(p, axis, q) over guest addresses (ported, ai.h).
inline int32_t GProject(GuestRam& g, uint32_t p, uint32_t axis, uint32_t q) {
    int32_t a[3], o[3];
    int16_t n[3];
    Read32x3(g, p, a);
    Read16x3(g, axis, n);
    Read32x3(g, q, o);
    return AiProject(a, n, o);
}
// SLUS 0x8002E698 DotLcm(a, b) (ported, vec.h).
inline int32_t GDot(GuestRam& g, uint32_t a, uint32_t b) {
    int16_t x[3], y[3];
    Read16x3(g, a, x);
    Read16x3(g, b, y);
    return DotLcm(x, y);
}
// SLUS 0x8002EE50 Scale(t, dir, out) (ported, vec.h).
inline void GScale(GuestRam& g, int32_t t, uint32_t dir, uint32_t out) {
    int16_t d[3];
    int32_t o[3];
    Read16x3(g, dir, d);
    Scale(t, d, o);
    Write32x3(g, out, o);
}
// SLUS 0x8002EAD8 MulAdd(base, dir, t, out) (ported, vec.h); `out` may equal `base`.
inline void GMulAdd(GuestRam& g, uint32_t base, uint32_t dir, int32_t t, uint32_t out) {
    int32_t b[3], o[3];
    int16_t d[3];
    Read32x3(g, base, b);
    Read16x3(g, dir, d);
    MulAdd(b, d, t, o);
    Write32x3(g, out, o);
}
// SLUS 0x8002E468 Normalize(v) in place (ported, vec.h); false where the console raises its overflow
// exception.
inline bool GNormalize(GuestRam& g, uint32_t a, const uint16_t* rsqrt) {
    int16_t v[3];
    Read16x3(g, a, v);
    if (!Normalize(v, rsqrt)) return false;
    Write16x3(g, a, v);
    return true;
}
// SLUS 0x8002EED8 ScaleTo16(k, v, out) (ported, crash.h): out[i] = FixMul(k, v[i]) >> 4.
inline void GScaleTo16(GuestRam& g, int32_t k, uint32_t v, uint32_t out) {
    for (uint32_t i = 0; i < 3; ++i) g.W16(out + 2u * i, static_cast<uint16_t>(FixMul(k, g.S32(v + 4u * i)) >> 4));
}
inline uint32_t GameState(GuestRam& g) { return g.U32(0x8005B2F8u); }
inline uint32_t NumPlayers(GuestRam& g) { return g.U32(GameState(g) + 48u); }

// A call through CollisionCallees::Unported: the o32 arguments in order (the first four in registers,
// the rest on the stack at sp + 16 ..), `sp` the stack pointer the original makes the call at.
inline bool Call(CollisionCallees& c, uint32_t fn, std::initializer_list<uint32_t> args, uint32_t sp,
                 uint32_t* v0 = nullptr) {
    uint32_t a[16] = {};
    int n = 0;
    for (uint32_t x : args) a[n++] = x;
    uint32_t r = 0;
    if (!c.Unported(fn, a, n, sp, r)) return false;
    if (v0 != nullptr) *v0 = r;
    return true;
}

} // namespace rr::sim::cu
