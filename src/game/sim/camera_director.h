#pragma once
// The camera director's shot script reader and the spline under the director's tracks,
// transcribed from our own disassembly of
//
//   RASHCDG.BIN  SHA-1 cfe43a7786759f2cb9c57751cf99e84d1074782c, loaded at 0x8005B5E8
//   SLUS_010.53  SHA-1 67ed165a2c517d4e6106fb0dfa324d66dd9a76f1, text at 0x80010000
//
// and accepted by the rows shot_setup / knot_reverse / spline_slopes / spline_coef of
// tools\rrverify\rows_feel.inc.
//
//   RASHCDG 0x800853E4  ShotSetup(View *v, u8 *script)       1026 instructions, frame 24
//   RASHCDG 0x800852E8  KnotReverse(View *v)                 the pan's knots mirrored below the first
//   SLUS    0x8002F634  SplineSlopes(x[], y[], out[], n)     864 bytes, frame 192 (a tridiagonal solve)
//   SLUS    0x8002F4D8  SplineCoef(y0, y1, y2, h0, [h1, o0, o1, o2, o3])  frame 48
//
// ShotSetup reads a shot's alternative: `script[1]` the kind (0 pan, 1 track, 2 scripted-13,
// 3 scripted-14), then (type, index) byte pairs naming the director's points (the table at
// 0x800D83B0: s16 bases at +524 + 2*type, 6-byte points at +532; types 0/1 take two points - a
// position and a look offset), the pair (5, a) a knot angle, (4, t) the transition time +0x45C. It
// fills the knot arrays +0x33C / +0x354 / +0x36C (eye), +0x384 / +0x39C / +0x3B4 (its second triple),
// +0x3CC.. (look), the angles +0x324.., the knot count +0x320 and the mode +0x21C (11..14), and sets
// +0x224 |= 6 (clearing bit 2 when the first pair is not a position).
#include <cstdint>

#include "game/sim/road_query.h"

namespace rr::sim {

constexpr uint32_t kShotSetupFn = 0x800853E4;
constexpr uint32_t kKnotReverseFn = 0x800852E8;
constexpr uint32_t kSplineSlopesFn = 0x8002F634;
constexpr uint32_t kSplineCoefFn = 0x8002F4D8;
constexpr uint32_t kSplineSlopesFrame = 192;
constexpr uint32_t kSplineCoefFrame = 48;
constexpr uint32_t kShotTableAddr = 0x800D83B0;

// False: the view faulted.
bool KnotReverse(GuestRam& g, uint32_t v);
bool ShotSetup(GuestRam& g, uint32_t v, uint32_t script);
// `sp` at entry; the five stack arguments of SplineCoef are at sp+16.. (the caller's frame).
bool SplineCoef(GuestRam& g, int32_t y0, int32_t y1, int32_t y2, int32_t h0, uint32_t sp);
bool SplineSlopes(GuestRam& g, uint32_t x, uint32_t y, uint32_t out, int32_t n, uint32_t sp);

} // namespace rr::sim
