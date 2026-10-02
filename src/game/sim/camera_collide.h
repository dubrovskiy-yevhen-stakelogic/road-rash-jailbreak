#pragma once
// The camera's collision step - what RaceStep runs after ViewUpdate when view +0x224 & 0x100 - and
// the volume test under it. Transcribed from our own disassembly of
//
//   RASHCDG.BIN  SHA-1 cfe43a7786759f2cb9c57751cf99e84d1074782c, loaded at 0x8005B5E8
//
// and accepted only by `rrverify phys` rows (tools\rrverify\rows_feel.inc).
//
//   RASHCDG 0x800A421C  CameraCollide  768 bytes, frame 104
//   RASHCDG 0x800B2E64  PropVsShape    304 bytes, frame 88
//
// Memory model and stack rule: collision.h (`sp` is the stack pointer AT ENTRY; the locals a callee
// reads or writes through a pointer live in the guest stack at the original's frame addresses).
// Callees run natively: BuildObb 0x8008BA18 (bike.h), ViewTrack 0x800A451C, WallContact 0x800B3AD0,
// CornerMin 0x800B6F40, ApplyImpulse 0x800A8DF0 (collision.h), FirstPointInsideBox 0x800B7030 and
// FaceNormal 0x800B675C (resolvers.h), GroundQuery 0x800A7BF8 (ground.h), Scale / MulAdd (vec.h).
// PropTopple 0x800B3344 (unported; reached only for a pool-4 prop below slot 30 with +0x250 bit 1,
// never for a view object) goes through CollisionCallees::Unported.
#include <cstdint>

#include "game/sim/collision.h"
#include "game/sim/integrator.h"
#include "game/sim/road_query.h"

namespace rr::sim {

// RASHCDG 0x800B2E64 PropVsShape(e, shape): e's eight box corners +0xC4 against the static volume
// `shape` (pool 6, 280 bytes: box +0x18, rows +0x104). The first corner inside (face mask 16) ->
// the face normal, e pushed out by Scale(depth + 8192, n) through ApplyImpulse (flag: not a view
// object, i.e. not pool 4 slot >= 30); a view object returns 1 there; any other entity is stopped
// (+0x1E0 = 0) or, with +0x250 bit 1, handed to PropTopple(e, 0, n, -1). v0 = 0 when no corner is in.
bool PropVsShape(GuestRam& g, uint32_t e, uint32_t shape, uint32_t sp, CollisionCallees& c, uint32_t& v0);
constexpr uint32_t kPropVsShapeFn = 0x800B2E64;
constexpr uint32_t kPropVsShapeFrame = 88;

// RASHCDG 0x800A421C CameraCollide(v): +0x224 &= ~0xE00; BuildObb(v); ViewTrack(v, 1); save +0x1D4,
// the heading +0x1C2 and the speed +0x1E0; ViewTrack(v, 0); PropVsShape against each volume listed at
// 0x800CCF88 (count 0x800CCF90, pool 6 base *(0x800CD6C4)) - the first contact re-runs ViewTrack(v, 1)
// and ends the scan; WallContact(v, 0); restore; the ground under the eye (GroundQuery off the road,
// the road slice on it) and, with fewer than 8 box corners on its far side, the eye pushed out along
// the ground normal (MulAdd) and v0 = 1; otherwise v0 = WallContact's answer. RaceStep re-runs
// CameraOrient when v0 != 0. `t` needs rsqrt and sqrt.
bool CameraCollide(GuestRam& g, uint32_t v, uint32_t sp, const BikeTables& t, CollisionCallees& c, uint32_t& v0);
constexpr uint32_t kCameraCollideFn = 0x800A421C;
constexpr uint32_t kCameraCollideFrame = 104;

} // namespace rr::sim
