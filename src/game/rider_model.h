#pragma once
// The rider's model class and collision box, as SpawnBike leaves them.
// Transcribed from our own disassembly of
//
//   RASHCDI.BIN  SHA-1 9a8b79d8739713c12b0a2dc4d75ef8f0a2cc0b06, loaded at 0x8005B5E8
//   SLUS_010.53  SHA-1 67ed165a2c517d4e6106fb0dfa324d66dd9a76f1, text at 0x80010000
//
// SpawnBike RASHCDI 0x80066350..0x800663FC binds the rider's model - ModelBind(rider, 1, cls, 1)
// (SLUS 0x8002FAD4, ported in traffic_bind.h) - stores its v0 at rider +0xB4 and then runs SLUS
// 0x80012FC8(rider, 0) (ported: traffic_leaves.h CarSetup), which gives a pool-1 entity the half
// extents of its bound model's LOD 0: +0x138 = 2 x0, +0x134 = y, +0x130 = z, each the BBD3 half extent
// of that LOD >> the DOD3 shift (u16 +14 >> 12) << 10 (SLUS 0x80012AEC ModelExtent).
//
// Without both, every rider box is 0 x 0 x 0, and a bike meeting a rider divides by the rider's
// length 0 in the contact response 0x800AAD30 (0x800AB1F0, DivArm) - an 8897-unit jump, which makes
// BikeVsRider 0x800AD04C unusable. rr-race holds +0x130/+0x134/+0x138 =
// 17408 / 10240 / 106496 (model 150) and 17408 / 11264 / 104448 (model 159) and +0xB4 = the class.
#include <cstdint>
#include <string>
#include <vector>

#include "game/sim/road_query.h"
#include "rrvfs/disc_image.h"

namespace rr::game {

// RASHCDI 0x80066350..0x800663EC on the seat just spawned (its +0x43C record's +1 as SpawnBike has
// it there; the 0x80065E18 "other class" flag of race types 33 / 44 recomputed): the model class
// cls = 9 s1 + 3 s4 + bank, stored at the rider's +0xB4 (ModelBind's v0, 0x800663FC). Returns cls.
uint32_t RiderModelClass(rr::sim::GuestRam& g, uint32_t bike);

// SLUS 0x80012FC8(rider, 0) for every rider after the grid: the model ModelBind binds for the class
// at +0xB4 - pool 1's base id 150 + the SLUS word gp+572 / gp+576 by `cls - 9 < 9` - read from the
// level bundle DATA\BBLEVEL<bank+1>.GEO. Returns a one-line note for the session's seam list.
std::string SetRiderBoxes(rr::sim::GuestRam& g, const rr::DiscImage& disc, int bank,
                          const std::vector<uint32_t>& riders);

} // namespace rr::game
