#pragma once
// Fixed-point primitives of Road Rash: Jailbreak, ported function by function from the original
// MIPS code and accepted only by `rrverify phys` (0 mismatches over the whole guest RAM outside the
// guest stack, plus the scratchpad, across dump-derived and randomised inputs).
//
// These are the *leaves* of the per-frame bike physics call tree:
// every integrator, every steering term and every road query in the game is built out of them, so
// they are the first thing that has to be bit-exact. Nothing here uses floating point; the
// original's fixed point IS the specification.
//
// All addresses below are in the resident executable `SLUS_010.53`,
// SHA-1 67ed165a2c517d4e6106fb0dfa324d66dd9a76f1, text base 0x80010000 (file offset 0x800).
#include <cstdint>

namespace rr::sim {

// ---------------------------------------------------------------------------- 0x8001FC90
// s32 FixMul(s32 a, s32 b) - the 16.16 multiply.
//
//   mult a0,a1 ; v0 = (lo >> 16) | (hi << 16)
//
// i.e. bits 16..47 of the 64-bit signed product, truncated to 32 bits. The truncation is part of
// the specification: the original overflows silently and the game relies on it.
int32_t FixMul(int32_t a, int32_t b);

// ---------------------------------------------------------------------------- 0x80010028
// u32 FixDiv(u32 a, u32 b) - the 16.16 divide, done as a reciprocal multiply.
//
//   a1 >>= 1 (logical) ; v0 = 0x80000000u / a1 ; multu a0,v0 ; v0 = (lo >> 16) | (hi << 16)
//
// Both the divide and the multiply are UNSIGNED. `b == 0` (or `b == 1`, which also shifts to 0) is
// not special-cased: an R3000 `divu` by zero yields a quotient of 0xFFFFFFFF, and that value then
// flows into the multiply. This function reproduces that, because the original does.
uint32_t FixDiv(uint32_t a, uint32_t b);

// ---------------------------------------------------------------------------- 0x8001FCB0
// s32 ApproxLen3(s32 x, s32 y, s32 z) - the cheap 3D length the engine uses instead of a square
// root: with m = max(|x|,|y|,|z|) and s = the sum of the other two,
//
//   m - (m >> 4) + (s >> 2) + (s >> 3)
//
// The shifts are arithmetic and the absolute values are the MIPS `sra/addu/xor` idiom, so
// ApproxLen3(INT32_MIN, ...) wraps exactly as the original does.
int32_t ApproxLen3(int32_t x, int32_t y, int32_t z);

// ---------------------------------------------------------------------------- 0x8001FC58
// u32 Rand(u32 &seed) - the linear congruential generator, seed at gp+2076 = 0x8005B4A8.
//
//   seed = seed * 0x19660D + 0x3C6EF35F;  return seed;
uint32_t Rand(uint32_t& seed);

// ---------------------------------------------------------------------------- 0x80020018
// s32 RatAtan2(s32 y, s32 x, const int32_t *table) - the arctangent, in the engine's angle unit of
// 4096 per full turn (1024 = 90 degrees, the same convention the camera uses).
//
// `table` is the 18-word interpolation table the original keeps at 0x8005285C. It is game data, so
// it is NOT reproduced in this repository: the caller passes a pointer to the table inside the
// player's own `SLUS_010.53` image (the bench reads it straight out of guest RAM). Passing nullptr is
// a programming error, not a fallback.
int32_t RatAtan2(int32_t y, int32_t x, const int32_t* table);

// ---------------------------------------------------------------------------- 0x8001FEB4
// s32 RatTan(s32 angle, const int16_t *sincos)
//
// The tangent, from the engine's own sine/cosine table: 4096 entries of `{s16 sin; s16 cos}` at
// 0x8005624C, the same table `rsin`/`rcos` (0x8001FD24) use. The angle unit is 4096 = one turn.
//
//   i = angle & 0xFFF;  s = sincos[2*i] << 4;  c = sincos[2*i + 1] << 4;
//   both are then made positive, divided with the UNSIGNED FixDiv, and the quotient is negated
//   when exactly one of them was non-positive.
//
// `sincos` is game data, so the caller passes a pointer into the player's own image.
int32_t RatTan(int32_t angle, const int16_t* sincos);

// ---------------------------------------------------------------------------- 0x80074FB4 (RASHCDG)
// s32 ClampLerpMin(s32 a, s32 b, s32 t)
//
// The blend-with-a-floor the bike step uses on its scalar responses. `t` is first clamped into
// [0, 1.0] by the engine's standard `max(t,0) + min(0x10000 - t, 0)` idiom, and then
//
//   s = FixMul(b, t);
//   if (s >= a) return a;
//   r = FixMul(a, t);
//   return (s < r) ? r : s;
//
// Overlay `RASHCDG.BIN`, SHA-1 cfe43a7786759f2cb9c57751cf99e84d1074782c, loaded at 0x8005B5E8
// (file offset 0 = that address).
int32_t ClampLerpMin(int32_t a, int32_t b, int32_t t);

// The clamp above on its own: `max(t, 0) + min(0x10000 - t, 0)`, computed with 32-bit wrap.
int32_t ClampUnitFrac(int32_t t);

} // namespace rr::sim
