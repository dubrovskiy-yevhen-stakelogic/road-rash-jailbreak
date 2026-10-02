#pragma once
// The vector primitives of the per-frame bike step, ported from the original MIPS code.
// Addresses in the resident executable `SLUS_010.53`, SHA-1
// 67ed165a2c517d4e6106fb0dfa324d66dd9a76f1, text base 0x80010000.
//
// Units, established in docs\formats\road_chunk.md and confirmed here by the code itself:
//   * a direction / matrix row is `int16_t` with 4096 = 1.0;
//   * a world position is `int32_t` with 65536 = 1.0 (16.16);
//   * a direction is promoted to 16.16 by `<< 4`, which is exactly 65536/4096.
#include <cstdint>

namespace rr::sim {

// ---------------------------------------------------------------------------- 0x8002E698
// s32 DotLcm(const s16 v[3], const s16 m[3])
//
// The original computes this on the GTE: it loads `v` into IR1..IR3, `m` into the first row of the
// colour matrix, issues MVMVA (mx = 2 colour matrix, v = 3 IR vector, cv = 3 no translation,
// sf = 0, lm = 0) and returns MAC1 >> 8.
//
// With sf = 0 the accumulator is not shifted, so MAC1 is the plain sum of the three products,
// truncated to 32 bits (the 44-bit accumulator cannot overflow with int16 terms, but the 32-bit
// register it is read back through can, and the game relies on that wrap).
int32_t DotLcm(const int16_t v[3], const int16_t m[3]);

// ---------------------------------------------------------------------------- 0x8002EAD8
// void MulAdd(const s32 base[3], const s16 dir[3], s32 t, s32 out[3])
//
//   out[i] = base[i] + (((s64)((s32)dir[i] << 4) * t) >> 16)
//
// "step from `base` along `dir` by `t`", the form every integrator in the bike step uses.
void MulAdd(const int32_t base[3], const int16_t dir[3], int32_t t, int32_t out[3]);

// ---------------------------------------------------------------------------- 0x8002E570
// void MulAdd32(const s32 base[3], const s32 dir[3], s32 t, s32 out[3])
//
//   out[i] = base[i] + (((s64)dir[i] * t) >> 16)
//
// The same step with a 16.16 direction instead of a 4096 = 1.0 one - 35 instructions, no callees,
// the three components written out longhand. `RASHCDG 0x8007F0BC` (the per-bike step's velocity
// integrator) calls it twice.
void MulAdd32(const int32_t base[3], const int32_t dir[3], int32_t t, int32_t out[3]);

// ---------------------------------------------------------------------------- 0x8002EE50
// void Scale(s32 t, const s16 dir[3], s32 out[3])
//
//   out[i] = ((s64)t * ((s32)dir[i] << 4)) >> 16
void Scale(int32_t t, const int16_t dir[3], int32_t out[3]);

// ---------------------------------------------------------------------------- 0x80036800
// The road-contact query: where is a world point on a road slice?
//
// A `SLCT` slice (docs\formats\road_chunk.md 3) begins
//     u16 index; i16 m[9]; i32 pos[3];
// with `m` row 0 the lateral axis, row 1 the surface normal and row 2 the unit tangent, all
// 4096 = 1.0, and `pos` the slice origin in 16.16 world units.
//
// `RoadProject` reproduces `0x80036800(const s32 p[3], const Slice *s, s32 *lateral, s32 *along)`
// exactly, including the pointer-may-be-null arms: each output is computed only if its pointer is
// non-null, and neither is touched otherwise.
//
//   *lateral = dot(m_row0 << 4, p - pos) >> 16
//   *along   = dot(m_row2 << 4, p - pos) >> 16
//
// Each term is a separate 64-bit product whose top 32 bits are dropped, and the three terms are
// then added as 32-bit words - so the wrap behaviour of a point far off the slice is reproduced,
// not approximated.
struct RoadSliceView {
    const int16_t* m;    // 9 entries, row-major
    const int32_t* pos;  // 3 entries, 16.16
};

void RoadProject(const int32_t p[3], const RoadSliceView& slice, int32_t* lateral, int32_t* along);

// ---------------------------------------------------------------------------- 0x8002E6F8
// void Blend32(const s32 a[3], const s32 b[3], s32 out[3], s32 wa, s32 wb)
//
//   out[i] = FixMul(wb, b[i]) + FixMul(wa, a[i])
//
// `wb` is the fifth argument and therefore arrives on the stack at sp+16 - which is exactly how the
// bench has to call it.
void Blend32(const int32_t a[3], const int32_t b[3], int32_t out[3], int32_t wa, int32_t wb);

// ---------------------------------------------------------------------------- 0x8002EB78
// void Blend16(const s16 a[3], const s16 b[3], s16 out[3], s32 wa, s32 wb)
//
//   out[i] = (s16)((FixMul(wb, b[i] << 4) + FixMul(wa, a[i] << 4)) >> 4)
//
// The direction-vector form of the same blend: promote to 16.16, blend, demote back to 4096 = 1.0.
// This is the interpolation the lean and steering axes go through every frame.
void Blend16(const int16_t a[3], const int16_t b[3], int16_t out[3], int32_t wa, int32_t wb);

// ---------------------------------------------------------------------------- 0x8002E810
// void Scale32(s32 t, const s32 v[3], s32 out[3])
//
//   out[i] = FixMul(t, v[i])
//
// The 16.16-in, 16.16-out scale, as opposed to `Scale` above which promotes a 4096 = 1.0 direction.
void Scale32(int32_t t, const int32_t v[3], int32_t out[3]);

// ---------------------------------------------------------------------------- 0x8002ECB8
// void Blend16To32(const s16 a[3], const s16 b[3], s32 out[3], s32 wa, s32 wb)
//
//   out[i] = FixMul(wa, a[i] << 4) + FixMul(wb, b[i] << 4)
//
// The same blend as Blend16 but left in 16.16 instead of being shifted back down to 4096 = 1.0.
// `wb` is again the fifth argument, on the stack at sp+16.
void Blend16To32(const int16_t a[3], const int16_t b[3], int32_t out[3], int32_t wa, int32_t wb);

// ---------------------------------------------------------------------------- 0x8002E468
// void Normalize(s16 v[3], const u16 *rsqrtTable)
//
// In-place normalisation to 4096 = 1.0. The original squares the three components on the GTE
// (`SQR`, sf = 0, lm = 1), adds the three MAC registers with the TRAPPING `add` instruction, and
// then looks the reciprocal square root up in a packed table:
//
//   n     = x*x + y*y + z*z                      (a signed overflow here is a CPU exception)
//   lz    = leading sign bits of n               (GTE LZCS/LZCR)
//   sh    = max(22 - (lz & ~1), 0)
//   w     = rsqrtTable[n >> sh]                  (u16: mantissa in bits 5..15, exponent in bits 0..4)
//   f     = ((w >> 5) << (w & 0x1F)) >> (sh >> 1)
//   v[i]  = (s16)((v[i] * f) >> 12)
//
// `rsqrtTable` is the table the game keeps behind `*(gp + 2260)`. It is game data and is therefore
// never reproduced here: the caller passes a pointer into the player's own image.
//
// Returns false, without touching `v`, when the sum of squares would overflow a signed 32-bit add -
// i.e. exactly the inputs on which the original raises an arithmetic-overflow exception. The engine
// only ever passes vectors of 4096 = 1.0 scale, so this never fires in the game; it is reported
// rather than hidden because a stub that quietly produced *something* would be worthless.
bool Normalize(int16_t v[3], const uint16_t* rsqrtTable);

} // namespace rr::sim
