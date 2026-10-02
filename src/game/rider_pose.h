#pragma once
// The product side of the rider POSE: what the race session needs so that the
// ported pose (src\game\sim\pose.h) runs on its riders, and what the renderer reads back.
//
//   * BuildPoseArena - per rider and bike the two things Pose 0x8005D63C chases through the owner:
//     `+0` the model (DOD3 +0x0E type / class bits, +0x14 scale) and `+4` the part slots it writes
//     (24 bytes each: u32 DPD3, the 3x3). At rr-race's addresses: the player's bike / rider parts at
//     0x801BDF1C / 0x801BDF9C, the others' pairs from 0x801E7F3C every 0x220 bytes (exactly where the
//     race loader's heap put them in every capture). The model records are OURS (0x801EA360): the
//     fields of the disc's own DOD3 group 0 (BBLEVEL1.GEO model 150 / 100) that the pose reads - the
//     captures' models live inside the road-object region of the product's arena. `+9 |= 3` is the
//     instance binder's store (SLUS 0x8002FE88); the slots start at the binder's identity
//     (0x8002FF98..0x80030038).
//   * PoseLodPass - the animation half of 0x800667C4 (GameFrame step 4) at LOD 0, the only LOD the
//     product draws: object +0x6E0 := *(0x80052390), +0x24 bit 2 (interpolate) set, owner +9 bit 3
//     cleared. The LOD choice itself (the distance table) is not ported: OURS = 0.
//   * ReadRiderPose - the 17 posed slots and the root word triple, for the renderer.
//   * CheckRiderPose - `rrgame --posecheck <ram>`: the ported Pose run on a copy of a capture
//     against the matrices the reference emulator wrote, and the relation of the captured slot 0 to the
//     bike's.
#include <cstdint>
#include <string>
#include <vector>

#include "game/sim/road_query.h"
#include "rrformats/pose.h"
#include "rrvfs/disc_image.h"

namespace rr::game {

struct PoseBinding {
    uint32_t bike = 0;  // pool-0 entity
    uint32_t rider = 0; // pool-1 owner of the animation object (+0x21C)
};

// `bankEnd` is the first byte past the resident animation banks (they must end below the parts).
// Returns a line for the seam list.
// `pairsAt` non-zero places pairs 1.. (kPoseReserveBytes, up to 19 riders) and the model records there
// instead of at rr-race's addresses (a two-seat race: its short set with the passenger bank fills the bank area).
constexpr uint32_t kPoseReserveBytes = 18u * 0x220u + 0x40u;
// loader2: `originalBind` - the objects were bound by the PORTED ModelBind SLUS 0x8002FAD4 (RegistryBind: +0 the
// DOD3 of the last LOD, +4 a part array on heap 0 with the binder's identity): a bound pair keeps both (the original's);
// only an object not yet bound (a two-seat race's passenger, bound later by SpawnPassenger) gets the OURS record / slots.
std::string BuildPoseArena(rr::sim::GuestRam& g, const rr::DiscImage& disc, uint32_t bankEnd,
                           const std::vector<PoseBinding>& bikes, uint32_t pairsAt = 0, bool originalBind = false);

void PoseLodPass(rr::sim::GuestRam& g, const std::vector<PoseBinding>& bikes);

struct RiderPoseView {
    rr::PartMatrix local[17];
    int16_t root[3] = {0, 0, 0}; // owner +0x1C/+0x1E/+0x20, model units
    uint16_t stance = 0;
};
// False when the rider has no part slots in the arena.
bool ReadRiderPose(const uint8_t* ram, uint32_t rider, RiderPoseView& out);

// WHERE the original draws a rider (RASHCDG, SHA-1 cfe43a77...). ModelVisible 0x80067AC4 takes an object
// whose +0x48 is 1 as a SEATED child: it is drawn only through its parent, BikeParts 0x80066EC4 -> ChildPlace 0x80066B98
// hanging it on the bike's seat (the renderer's PosedRiderMatrix). Any other rider - thrown, lying, getting up,
// walking back (+0x48 = 3 and the bike's child link +0x38 cleared, measured on 1/11) - is an object of its own in the
// draw list: for +0x48 == 3 its position is +0x0C (the world, 1/64 unit: the point +0xB8 holds in 16.16) plus, with
// +0x09 bit 2, the root +0x1C turned by +0x68, and ModelDraw 0x80068468 turns part 0 by that +0x68 (the rows +0x1B0
// transposed: model axis c = row c); for another +0x48, +0xB8 alone and part 0 in world axes.
// RiderOwnFrame: false for a seated rider; otherwise the rider model's world frame - axis[c] = model axis c (unit,
// world), origin = the model origin in world units (the root included) - on which its part slots pose it as on the
// seat. RRJB_RIDER_PLACE=seat: always false (every rider drawn on the seat, the control).
bool RiderOwnFrame(const uint8_t* ram, uint32_t rider, float axis[3][3], float origin[3]);
bool RiderPlaceSeatOnly(); // RRJB_RIDER_PLACE=seat
// The re-seat's climb (ReSeat 0x8009277C: the bike's own animation object) sets the BIKE's +0x48
// to 3 for ~110 frames: ModelDraw then turns its part 0 by its slot 0 (the bike lifted off its side) after +0x68, and
// ChildPlace hangs the seated rider at bike + (+0x68 slot 0) (seat + slot 2 root). True in that state, with the bike's
// part slots 0 and 2 (4.12 as PartMatrix holds them); false while riding (+0x48 != 3: slot 0 is the rows the
// renderer's matrix already is).
bool MachineClimbSlots(const uint8_t* ram, uint32_t bike, rr::PartMatrix& slot0, rr::PartMatrix& slot2);

int CheckRiderPose(const std::string& ramPath, bool mutate);

// `rrview --state <dir> --ridercheck <report> --orig-prims <csv>`: the player's rider built three
// ways - the PORTED pose run on the capture, the capture's own slots, the rest pose - placed on
// rrview's bike (`bike`, a column-major 4x4 of model units -> world) at the attachment vertex plus the
// root triple, projected with the capture's camera (`project`) and matched primitive by primitive
// (same texel corners) against the ORIGINAL's rider packets (CLUT `clut`, psxgpu.py --prims).
struct RiderPacketCheck {
    const rr::DiscImage* disc = nullptr;
    const std::vector<uint8_t>* ram = nullptr;
    std::string primsCsv;
    float bike[16] = {};
    float attach[3] = {};
    uint16_t clut = 0x7D68;
    bool (*project)(void* ctx, const float p[3], double& sx, double& sy) = nullptr;
    void* ctx = nullptr;
};
std::string CheckRiderPackets(const RiderPacketCheck& in, bool& pass);

} // namespace rr::game
