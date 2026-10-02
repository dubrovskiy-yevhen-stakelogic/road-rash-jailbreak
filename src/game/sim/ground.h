#pragma once
// The ground query - what an off-road bike stands on - ported from the RASHCDG overlay
// (SHA-1 cfe43a7786759f2cb9c57751cf99e84d1074782c, loaded at 0x8005B5E8), and the view distance
// that picks its level of detail. This file carries the addresses line by line.
//
//   F2  RASHCDG 0x800B6E08  PointInPoly   312 bytes   strictly inside a convex polygon
//   F3  RASHCDG 0x800B6844  TriNormal     616 bytes   the Newell normal, clamped and normalised
//   F4  RASHCDG 0x800A8498  CellLookup   1864 bytes   the resident cell and sub-area under a point
//   F5  RASHCDG 0x800A7BF8  GroundQuery  2208 bytes   the terrain triangle under a point
//   F8  RASHCDG 0x8008DBCC  ViewDistance  212 bytes   entity +0x2C / +0x30, the LOD distance
//
// F1 (SLUS 0x8002E14C Normalize32) is integrator.h's; F6/F7 (the off-road classifier that sets the
// gate, entity +0x184 bit 0) are road_runtime.h's.
//
// Each function is accepted by its own row of `rrverify phys` (tools\rrverify\rows_ground.inc):
// 0 mismatches over the whole guest RAM outside the stack window and the scratchpad, on
// dump-derived and randomised inputs.
//
// MEMORY MODEL. GroundQuery and CellLookup walk the resident-cell slot table at 0x800D87E8, the cell
// bodies it points at and their regions 2..7, so they are written over road_query.h's guest-address
// view (`GuestRam`), exactly as the road layer is. The two geometry leaves take host arrays: their
// operands are always the caller's own stack locals.
//
// STACK. The original keeps its polygons, list bounds, the local point and the cell origin in its
// own frames. None of that is observable outside the stack window, so the port keeps them as host
// locals - with one exception it has to model: CellLookup's 72-byte polygon buffer is REUSED from
// polygon to polygon inside one call, and its nearest-edge loop reads vertices in pairs, so an odd
// vertex count would read a slot the previous polygon (or older stack) left. The shipped data has
// only 4- and 6-vertex polygons (904 of 904 cells); the port models the reuse
// within a call and DECLINES a read of a slot no polygon of this call wrote, rather than inventing
// stack contents. It declines likewise on a polygon of more than 6 vertices (the original would
// overrun its buffer into its own local point) - `GroundResult::declined`.
#include <cstdint>

#include "game/sim/road_query.h"

namespace rr::sim {

// ---------------------------------------------------------------------------- the data
constexpr uint32_t kCellSlotTable = 0x800D87E8; // 24 x 112 bytes, scene_cell.md 1.1
constexpr uint32_t kCellSlotBytes = 112;
constexpr uint32_t kCellSlotCount = 24;
constexpr uint32_t kViewObjects = 0x800CD898;   // the per-player view objects, stride 1132
constexpr uint32_t kViewObjectBytes = 1132;
constexpr int32_t kGroundFineDistance = 1280;   // `slti v0,t5,1280` at 0x800A7CA4

// ---------------------------------------------------------------------------- 0x800B6E08
// s32 PointInPoly(const s32 p[3], const s32 v[][3], s32 n, s32 axis)
//
// 1 when `p` is STRICTLY inside the convex polygon `v` projected along `axis` (on-edge is outside),
// 0 otherwise; `n <= 0` returns 1. Both edge products are FixMul.
int32_t PointInPoly(const int32_t p[3], const int32_t (*v)[3], int32_t n, int32_t axis);

// ---------------------------------------------------------------------------- 0x800B6844
// void TriNormal(const s32 v[][3], s32 n, s16 out[3])
//
// The Newell sum over the n-gon (nine FixMul per edge), halved while any component exceeds
// 0x005A8000, normalised by SLUS 0x8002E14C (`rsqrt` is the table at *(gp+2260)) and stored as
// `s16(s >> 4)`, i.e. 4.12.
void TriNormal(const int32_t (*v)[3], int32_t n, int16_t out[3], const uint16_t* rsqrt);

// ---------------------------------------------------------------------------- 0x800A8498
// u32 CellLookup(const s32 p[3], const s16 *heading, u8 **prim, u8 **verts, s32 origin[3],
//                u32 *state)
//
// Six o32 arguments: `origin` at sp+16 and `state` at sp+20. The state word in and out is the
// query's. `*verts` and `origin` are written for EVERY non-empty slot visited,
// `*prim` only when a sub-area is found; the return is `tri | quad << 16` of the primitive group
// handed back, or 0 (with `*state = 0`) when nothing is found.
struct CellLookupResult {
    uint32_t counts = 0;       // v0
    uint32_t state = 0;        // *state on return
    bool primWritten = false;  // *prim was stored
    uint32_t prim = 0;
    bool slotVisited = false;  // *verts and origin were stored (for the last slot visited)
    uint32_t verts = 0;
    int32_t origin[3] = {0, 0, 0};
    bool declined = false;     // see STACK above; nothing of the result is meaningful then
};
CellLookupResult CellLookup(GuestRam& m, const int32_t p[3], uint32_t heading, uint32_t state);

// ---------------------------------------------------------------------------- 0x800A7BF8
// u32 GroundQuery(Entity *e, s32 *ref, s32 outPoint[3], s16 outNormal[3], u32 hint)
//
// Five o32 arguments, the fifth at sp+16. `ref == 0` means `e + 0xB8`. On a hit it writes the
// three words at `outPoint` (a vertex of the hit triangle, world 16.16), the three halfwords at
// `outNormal` (the NEGATED Newell normal, 4.12) and the byte `e + 0x216` (primitive +0x02 >> 12),
// and returns the packed hint of 16.7; otherwise it writes nothing and returns -1 or the
// "lists exhausted" word 0xFF000000 | off << 14 | low12.
struct GroundResult {
    uint32_t value = 0xFFFFFFFFu; // v0
    bool hit = false;             // the three outputs were written
    bool declined = false;        // CellLookup declined (see STACK); nothing was written
};
GroundResult GroundQuery(GuestRam& m, uint32_t e, uint32_t ref, uint32_t outPoint,
                         uint32_t outNormal, uint32_t hint, const uint16_t* rsqrt);

// ---------------------------------------------------------------------------- 0x8008DBCC
// void ViewDistance(Entity *e)
//
//   e[+0x30] = 0x7FFFFFFF;
//   p = 0; do {
//       v = ApproxLen3((e.B8 - view[p].B8) >> 10, (e.BC - view[p].BC) >> 10, (e.C0 - view[p].C0) >> 10);
//       e[+0x2C + 4p] = v < 0 ? 0x7FFFFFFF : v;
//   } while (++p < (u32)game_state[+0x30]);
//
// with `view[p] = 0x800CD898 + 1132 p`. Fine LOD in the query is `+0x2C < 1280`, i.e. within 20.0
// world units of a camera.
void ViewDistance(GuestRam& m, uint32_t e);

} // namespace rr::sim
