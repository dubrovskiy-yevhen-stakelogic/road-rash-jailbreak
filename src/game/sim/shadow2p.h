#pragma once
// The model shadow of a TWO-player race, transcribed from our own disassembly of
// the player's own image:
//
//   SLUS_010.53  SHA-1 67ed165a2c517d4e6106fb0dfa324d66dd9a76f1, text at 0x80010000
//
// and accepted only by the `mp2_shadow` row of `rrverify phys` (tools\rrverify\rows_mp2.inc).
//
//   SLUS 0x80026960 Shadow2p(obj, view, ground, normal) - the emitter's tail 0x800251E4 (0x80025E74..0x80025EA8)
//   calls it in place of the one-player form 0x80025EE0 when game_state+0x30 != 1. Not a hull: ONE textured
//   sprite. The level light 0x80052364 (bent to 0.7071 (n + unit((n x L) x n)) when n.L, DotLcm 0x8002E698, is at
//   most 0xB503: the OP twice, Normalize 0x8002E468, Blend16 0x8002EB78, then n.L again), r = 2^31 / (n.L / 2)
//   (the reciprocal idiom, signed); the object's four points +0xC4 + 12 k (s32[3] 16.16, taken k = 3, 2, 1, 0)
//   are each moved along L onto the ground plane (t = mid((g - p) . (n << 4)) * r, MulAdd 0x8002EAD8), >> 10
//   minus the render camera's eye *(0x8005AEC0 + 4 view) + 0x1C.. as halfwords, then RotTransPers4 0x8004D1E4 with
//   RT = the camera's rows +0x5C and TR = 0 (RTPT over points 0, 1, 3, RTPS over 2, AVSZ4). The packet: 40 bytes
//   from the frame heap *(0x8005B470) + 0x10C (checked against *(0x8005B4D0); its arm 0x80021C98 is not
//   ported: the port refuses), GP0 0x2F (textured, semi-transparent, raw) with sprite descriptor 0 of the effect
//   sheet 0x800D4270 {u, v, w, h, tpage, clut}: corners (u, v) (u+w-1, v) (u, v+h-1) (u+w-1, v+h-1) on the
//   points 0, 1, 3, 2; linked at the OTZ the scratchpad's depth ranges map OTZ * 4 to (the one-player form's
//   mapping), the OT slot's word stored whole.
#include <cstdint>

#include "game/sim/road_query.h"
#include "game/sim/shadow.h"

namespace rr::sim::shadow {

constexpr uint32_t kSprites = 0x800D4270; // the effect sheet's sprite descriptors (28 bytes each)

// Told of the packet linked: its address and OTZ * 4 (the value the depth mapping took).
struct Sink2p {
    virtual ~Sink2p() = default;
    virtual void Packet(uint32_t address, int32_t otz4) = 0;
};

// SLUS 0x80026960. False when it refused: the packet heap would overflow, Normalize's sum of squares overflows
// (the console's exception), or a guest access faulted.
bool Shadow2p(GuestRam& g, const Gte& gte, uint32_t obj, uint32_t view, uint32_t ground, uint32_t normal,
              Sink2p* sink = nullptr);

// The emitter's tail (as EmitterShadow, shadow.h) for game_state+0x30 != 1: the ground point / normal choice
// 0x80025E18..0x80025E70, then Shadow2p. Nothing unless +0x24 bit 7 (`w24`) and two players; `ran` tells whether
// the two-player form was called.
bool EmitterShadow2p(GuestRam& g, const Gte& gte, uint32_t obj, uint32_t view, uint32_t w24, Sink2p* sink, bool& ran);

} // namespace rr::sim::shadow
