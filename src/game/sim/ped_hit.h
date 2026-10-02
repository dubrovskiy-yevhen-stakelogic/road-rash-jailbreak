#pragma once
// RASHCDG 0x800A9868 PedHit - the hit reaction of a rider (pool 1) or a pedestrian (pool 2) that
// meets a wall, a bike or a volume, transcribed from our own
// disassembly of RASHCDG.BIN (SHA-1 cfe43a7786759f2cb9c57751cf99e84d1074782c, loaded at 0x8005B5E8)
// and accepted by the row `ped_hit` of tools\rrverify\rows_feel.inc.
//
//   s32 PedHit(E *e, s16 n[3], s32 speed, s16 n2[3], [sp+16] u8 *other, [sp+20] s32 force)
//
// 566 instructions, frame 72. Three arms:
//   * GENTLE (not thrown +0x228 bit 30, speed <= 0x23C36, no force): for a pool-1 rider walking on
//     its own (+0x228 bit 29 clear): walking by hand (bit 11) -> +0x228 = & ~0x100100 | 0x80 and stop;
//     otherwise, heading into the surface (DotLcm(n, +0x1C2) < 0, not stance 69/70, not 0x100018) ->
//     +0x228 |= 0x100001, the turn rate +0x258 (0.5 over a half extent, or 0.5 for its own bike), the
//     turn axis +0x204 = +-n with the side bit 0x20000 / 0x40000, +0x1E8 = 0;
//   * THROWN / HARD (else, unless a fall stance 60..63 without force): a launched stance (kind 5)
//     runs RiderLaunch; a flying rider -> RiderAirHit (2: stance 43); on the ground, a stance event
//     by the hit side and clip (60..63, 67 / 68), or - above 0x8F0D8 - the LAUNCH: the body turned
//     by Rand() % 714 - 357, LaunchLift, the velocity, the rows by the GTE OP, +0x228 |= 0xC0000000,
//     stance 43 / 46 / 47 / 49 (Rand() % 3), a player's camera to the crash view;
//   * the tail: a stopped one loses its velocity, the rider keeps n at +0x26E; a hit rider (v0 = 1)
//     clears 0x100018, the bike's rider record +0x0F, and a player's pad rumbles (PadRumble).
// Callees through RecoverCallees (recover.h): RiderLaunch 0x80091468, RiderAirHit 0x800A966C, the
// stance event 0x800C4550, ClipDone 0x8005BE58, LaunchLift 0x8007E868, PadRumble 0x800B658C. Leaves
// native: DotLcm, MulAdd, FixMul, Rand, Asin, Scale, Normalize, FixDiv, the GTE OP.
#include <cstdint>

#include "game/sim/recover.h"

namespace rr::sim {

constexpr uint32_t kPedHitFn = 0x800A9868;
constexpr uint32_t kPedHitFrame = 72;

// `other` and `force` are the fifth and sixth o32 arguments. False: a callee refused or the view
// faulted (Normalize's overflow included); `v0` is the return value.
bool PedHit(GuestRam& g, uint32_t e, uint32_t n, int32_t speed, uint32_t n2, uint32_t other, uint32_t force,
            uint32_t sp, const BikeTables& t, RecoverCallees& c, uint32_t& v0);

} // namespace rr::sim
