#pragma once
// The bike's own part slots and the rider's level of detail,
// transcribed from our own disassembly of
//
//   RASHCDG.BIN  SHA-1 cfe43a7786759f2cb9c57751cf99e84d1074782c, loaded at 0x8005B5E8
//   SLUS_010.53  SHA-1 67ed165a2c517d4e6106fb0dfa324d66dd9a76f1, text at 0x80010000
//
// and accepted by the rows bike_instance / bike_wheels / lod_choice / rot_matrix of
// tools\rrverify\rows_feel.inc.
//
//   SLUS    0x8001FD24  RotMatrix(a, b, c, s16 out[9])  the Euler matrix of the sine table 0x8005624C
//   RASHCDG 0x80084E10  BikeInstance(e, a1)   the draw loop's per-bike set-up (0x8008D5FC): the
//                       position >> 10 at +0x0C, the object matrix (the rows +0x1B0 transposed) into
//                       slot 0 (or +0x68 for a +0x48 == 3 entity), and - riding, not in the pause - the
//                       FORK slot 1 = RotMatrix(0, +0x33A steer, 0), the pitch spring +0x34C / +0x34E
//                       toward a target from the lean / the wheelie / the brake and throttle bits, the
//                       passenger's lean spring +0x290 / +0x294, and slot 2 = RotMatrix(+0x34E, 0, roll);
//                       then the draw 0x80067AC4 (a callee).
//   RASHCDG 0x80066EC4  BikeWheels(e)  under the draw: a player's matrix-stack top copied to
//                       0x800CCD80; the WHEELS - slot 3 = RotMatrix(+0x344), slot 4 = RotMatrix(+0x346),
//                       with six or more parts slot 5 = RotMatrix(the passenger's +0x344); the part
//                       program 0x80067064 and the attachments 0x80066B98 (callees); the stack popped.
//   RASHCDG 0x800667C4  LodChoice(inst, players)  per player the level of detail from the distance
//                       +0x2C + 4p against the table +0x64 (hysteresis: the byte +0x0A + p walks up or
//                       down), the nearest one's part mask 0x80052390[lod (+1 for a type-4 model)] into
//                       the animation object's +0x6E0 and its interpolation bit (+0x24 bit 2: LOD 0,
//                       or LOD 1 for a player's / a passenger's rider); +9 bit 3 cleared.
//
// A part slot is 24 bytes at the entity's +4: u32, then the 3x3 s16 matrix at +4.
#include <cstdint>
#include <functional>

#include "game/sim/road_query.h"

namespace rr::sim {

constexpr uint32_t kRotMatrixFn = 0x8001FD24;
constexpr uint32_t kBikeInstanceFn = 0x80084E10;
constexpr uint32_t kBikeInstanceFrame = 40;
constexpr uint32_t kBikeDrawFn = 0x80067AC4;      // (e, a1) - the draw, a callee
constexpr uint32_t kBikeWheelsFn = 0x80066EC4;
constexpr uint32_t kBikeWheelsFrame = 32;
constexpr uint32_t kPartProgramFn = 0x80067064;   // (n, e) - the part program's transform + draw
constexpr uint32_t kAttachChildFn = 0x80066B98;   // (e, i)
constexpr uint32_t kLodChoiceFn = 0x800667C4;

// The callees that draw. False: not run (the port refuses).
struct BikePartCallees {
    virtual ~BikePartCallees() = default;
    virtual bool Call(uint32_t fn, uint32_t a0, uint32_t a1, uint32_t sp) = 0;
};

void RotMatrix(GuestRam& g, int32_t a, int32_t b, int32_t c, uint32_t out);
bool BikeInstance(GuestRam& g, uint32_t e, uint32_t a1, uint32_t sp, const uint16_t* asinTable, BikePartCallees& c);
// RASHCDG 0x80085224 RiderInstance(r, a1), 46 instructions: the draw loop's set-up of a rider OFF its bike
// (0x8008D644 / 0x8008D6D8 / 0x8008D74C, rider +0x25C >= 3): +0x0C/+0x14/+0x10 = the box +0xB8/+0xC0/+0xBC
// >> 10 (sra), then the rows +0x1B0..+0x1C0 transposed into +0x68 (a +0x48 == 3 entity) or part slot 0
// (*(+4) + 4). Its tail, the draw 0x80067AC4 (0x800852D0), is NOT here: the product's model pass runs it.
// ModelDraw 0x80068468 turns +0x0C back into the alternate box offset +0xF4 = (+0x0C << 10) - box, which
// BuildObbAlt 0x8008BD2C consumes on the next frame - so a stale +0x0C throws the rider's box far away.
void RiderInstanceStores(GuestRam& g, uint32_t r);
bool BikeWheels(GuestRam& g, uint32_t e, uint32_t sp, BikePartCallees& c);
// BikeWheels' slot writes alone (0x80066F6C..0x80066FB8, a model of five or more parts): what the product
// runs, since its renderer draws the parts itself. Returns "six or more parts" (the passenger wheel).
bool WheelSlots(GuestRam& g, uint32_t e);
bool LodChoice(GuestRam& g, uint32_t inst, int32_t players);

} // namespace rr::sim
