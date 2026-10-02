#pragma once
// The traffic's small leaves - the road walk that finds a spawn spot, the spawner budget, the car
// spacing test, the two-player sharing, a junction's lane record, the car's model extent and the
// cursor's road end - ported from our own disassembly of the player's own images:
//
//   SLUS_010.53  SHA-1 67ed165a2c517d4e6106fb0dfa324d66dd9a76f1, text at 0x80010000
//   RASHCDG.BIN  SHA-1 cfe43a7786759f2cb9c57751cf99e84d1074782c, loaded at 0x8005B5E8
//
// This file carries the addresses line by line. Accepted only by `rrverify phys` rows
// (tools\rrverify\rows_traffic_leaves.inc).
//
// Memory model and stack as population.h: functions over road_query.h's guest-address view; a
// function whose frame holds a local whose address it hands out (or a word it writes into its
// caller's argument area) takes `sp`, the stack pointer at its entry, and writes those words at the
// original's offsets; callee-saved register spills are not written. Every callee is ported, so none
// of these needs a callee interface. A load or store the console would not survive is not performed;
// `GuestRam` records it and the caller must FAIL the call.
#include <cstdint>

#include "game/sim/road_query.h"

namespace rr::sim {

// SLUS 0x80012C1C, 235 instructions, frame 56: copies the 12-byte road coordinate `from` to `out`
// and walks it |dist| (16.16) along the race graph - forward along the road's own direction for
// dist > 0, against it for dist < 0 (out's direction is negated first). Past a road end the walk
// stands on the end node; at a node of degree >= 2 it leaves by exit `Rand() % degree` (the route
// preferences in between call SLUS 0x8003F408 / 0x8003B4B0 with a NULL record and never match), onto
// the exit's road at along 0 (exit direction > 0) or at the road's length. At the end the direction
// is 1, or `from`'s own when the walk ended on `from`'s road key. Writes `from` to sp+0 (the a0 home
// slot in the caller's frame, 0x80012C38).
void RoadWalk(GuestRam& g, uint32_t from, uint32_t out, int32_t dist, uint32_t sp);

// RASHCDG 0x8008CDF4, 96 instructions, a leaf: 1 when the spawner may create one more entity of
// `kind` (0..6, a jump table at 0x8005B680; anything else: 0).
uint32_t Budget(GuestRam& g, uint32_t kind);

// RASHCDG 0x8009F578, 96 instructions, a leaf: the new car record `rec` against pool 3. 1 when a car
// on the same road key is within 10.0 of rec+0x24 (or the nearest same-direction car of flags
// +0x1FD & 0x11 has no cross-section record); 2 after rewriting the record's lane rec+0x40 away from
// that car's lane; 0 when there is no such car or its lane count (+0x1A4 / +0x198 by its direction)
// is 1.
uint32_t Spacing(GuestRam& g, uint32_t rec);

// RASHCDG 0x8009FF24, 170 instructions, frame 32: two-player sharing of the traffic flags
// `flags` (s32 per player). Only with race type bit 0x10 and the two players' bikes within 260
// (octagonal, integer x/z): each player's bike is tested against the OTHER view's forward axis
// (+0x1BC, s16 << 4) and the flags rewritten to 2 / 3. The per-player results live at sp-32+0/+4;
// with fewer than two players the second word is the stale stack word the original reads.
void ShareFlags(GuestRam& g, uint32_t flags, uint32_t sp);

// SLUS 0x8003E1E8, 84 instructions, frame 40 (spills only): the 264-byte lane record of junction
// object `obj` (a core: +0x10 == 1, +0x0C == 0) where road `road` meets it, and the arm's direction
// (s16) to `dirOut`; 0 when there is none.
uint32_t NodeLanes(GuestRam& g, uint32_t obj, int32_t road, uint32_t dirOut);

// SLUS 0x80012AEC, 76 instructions, a leaf: entry `idx` (12 bytes) of the model table
// *(*(e+0x60)+8): its bounding half-extents +0x18/+0x1A/+0x1C (s16), shifted right by the top nibble
// of *(entry+0)+0x0E and left by 10, to out[0..2]. Returns 0, or -1 (nothing written) when the
// entry's +8 is 0.
int32_t ModelExtent(GuestRam& g, uint32_t e, int32_t idx, uint32_t out);

// SLUS 0x80012FC8, 82 instructions, frame 40: an entity's collision half-extents +0x130/+0x134/
// +0x138 from ModelExtent(e, idx, sp-40+16) (the result is not checked: on -1 the three stack words
// are read as they are); pools 1 and 2 swap the axes, a pool-0 bike's +0x138 is scaled (by 11/8 or
// 9/8 below class 18, then minus 1/16 unit for a player; by 11/16 from class 18).
void CarSetup(GuestRam& g, uint32_t e, int32_t idx, uint32_t sp);

// SLUS 0x8003C520, 28 instructions, frame 24 (spills only): the along (16.16) of the end of the road
// the 32-byte cursor `cursor` is on - a junction piece's (+2 == 1) sub-object +0x0C, else the race
// graph road's length (GraphRoad(piece+0x0C) +4 >> 6, << 16). The police release and 0x800A05D4
// clamp an along with it.
int32_t CursorRoadEnd(GuestRam& g, uint32_t cursor);

} // namespace rr::sim
