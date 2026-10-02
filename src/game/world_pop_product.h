#pragma once
// The world around the road in the product:
// the arena the cell walker needs, its seams answered natively, the passes of the world pass, the draw
// loop's pools 2..5, and the live props for the renderer. Transcribed from our own listings of
//
//   RASHCDI.BIN  SHA-1 9a8b79d8739713c12b0a2dc4d75ef8f0a2cc0b06 (the race loader)
//   RASHCDG.BIN  SHA-1 cfe43a7786759f2cb9c57751cf99e84d1074782c
//   SLUS_010.53  SHA-1 67ed165a2c517d4e6106fb0dfa324d66dd9a76f1
//
// THE ARENA - RASHCDI 0x80068C3C.. (the pool table: pools 2, 4, 5, 6), 0x80068D54 (the population reset:
// pools 4 / 5 at 0x800D1818 / 0x800D36EC, the 8344-byte store, the pedestrian block), 0x80069000 (pool
// 6: 24 slots, 32 in two-player mode, malloc'd; the per-player volume lists 0x800D5CF8), and the prop
// model: DATA\HAZARD<n>.GEO through the loader's RMD3 walker 0x8005CA10 (traffic_arena.h
// LoadGeoIntoArena), which files model 200 as family 6 (0x800CE592). OURS, named: where the malloc'd
// blocks sit (the session's bump region).
//
// THE SEAMS of world_pop.h: BuildObb 0x8008BA18, SLUS 0x8002090C and 0x80093F94 through the session's
// population callees; GroundQuery 0x800A7BF8 PORTED (ground.h); the pedestrian spawner 0x800CB8C8 PORTED
// (peds.h: run through `pedSpawn`; without it answered v0 = 0, the record stays unspawned).
// The hazard-object spawner 0x800A0A20 (classes 0 / 9 of kind 4, 19 records on the disc) PORTED (hazard.h,
// hazard_product.h); under RRJB_HAZARDS=off not run.
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "game/sim/integrator.h"
#include "game/sim/population.h"
#include "game/sim/recover.h"
#include "game/sim/road_query.h"
#include "rrvfs/disc_image.h"

namespace rr::game {

// The HAZARD set the race loader picks (RASHCDI 0x8006383C -> 0x8005C7F0, 0x8006AD4C): 0 when
// (mode & 0x18) == 8, 1 in mode bit 4, else clamp(ENV.EN[0xCD + 2 (Rand() % ENV.EN[0xCC])], 1, 5) with
// the seed `seed` (not advanced here). The same rule as race_scene.h PickHazardSet; -1 without ENV.EN.
int HazardSetFor(const rr::DiscImage& disc, uint8_t mode, uint32_t seed);

// Pools 2 / 4 / 5 / 6, the store, the volume lists and the prop model. Returns a line for the seam list.
std::string BuildWorldArenaLine(rr::sim::GuestRam& g, const rr::DiscImage& disc, int hazardSet, uint32_t& from,
                                uint32_t limit);

// SLUS 0x800135E8, the slot fill's region-0 relocation (0x80032A20 -> 0x800135E8): the five array
// offsets +0x24..+0x34 of the body's region 0 made absolute, 0 for an empty array. Its tail, the
// two-player copy SLUS 0x8001339C, is not run (OURS: one buffer per cell for both players).
void RelocateRegionZero(rr::sim::GuestRam& g, uint32_t body);

// The seams, answered natively.
struct WorldProductCallees final : rr::sim::RecoverCallees {
    rr::sim::GuestRam& g;
    rr::sim::PopulationCallees& pop;
    const rr::sim::BikeTables& t;
    std::function<void(const std::string&)> note;
    // The pedestrians (peds_product.h): the PORTED spawner 0x800CB8C8(rec, 1, bike) and PedPass 0x800CB304(dt)
    // run by the session; unset = the seam (not run).
    std::function<bool(const uint32_t* a, uint32_t sp, uint32_t& v0)> pedSpawn;
    std::function<bool(int32_t dt, uint32_t sp)> pedPass;
    size_t peds = 0, hazards = 0, obbs = 0, grounds = 0, refused = 0;
    WorldProductCallees(rr::sim::GuestRam& gg, rr::sim::PopulationCallees& p, const rr::sim::BikeTables& tt)
        : g(gg), pop(p), t(tt) {}
    bool Call(uint32_t fn, const uint32_t* a, int n, uint32_t sp, uint32_t& v0) override;
};

// RASHCDG 0x8009C308, the cell walker (SpawnerPass's child), PORTED.
bool RunCellWalker(rr::sim::GuestRam& g, uint32_t sp, WorldProductCallees& c);
// The world pass's children after the rider pass: PropPass 0x800A2898, PedPass 0x800CB304 through `pedPass`
// while *(0x8005B254) (the pedestrians' switch), Pool5Pass 0x8009ACA4, VolumePass 0x8009AB60.
bool RunWorldPasses(rr::sim::GuestRam& g, int32_t dt, uint32_t sp, WorldProductCallees& c);
// One of those three by its guest address (the PORTED WorldBikePass 0x8008AC80 calls
// them one by one, PedPass between the first two). False: it refused or faulted.
bool RunWorldChild(rr::sim::GuestRam& g, uint32_t fn, int32_t dt, uint32_t sp, WorldProductCallees& c);

// RASHCDG 0x8008D56C's pools 2..5 for view p: an entity with a handle whose cell test 0x8008B99C
// (PORTED, cell_draw.h) finds a cell gets +0x0C/+0x10/+0x14 = its position >> 10 and its part-0 matrix
// (+0x68 when +0x48 == 3, else *(+4) + 4) = its rows +0x1B0 transposed. OURS, named: the loop's
// ModelVisible 0x80067AC4 is the model runtime's (model_runtime.h), not run here.
void DrawLoopPools(rr::sim::GuestRam& g, int players);

// A live roadside prop (pool 4 or 5) for the renderer: group `cls` of the prop model, at `pos` (16.16),
// turned by `rows` (row k = model axis k in the world), in cell `cell` (+0xB0).
struct LiveProp {
    uint32_t entity = 0;
    uint32_t cls = 0;
    int32_t pos[3] = {};
    int16_t rows[9] = {};
    int32_t cell = -1;
};
std::vector<LiveProp> LiveProps(const uint8_t* ram);

// `rrgame --worldcheck <ram>` (a capture's ram.bin or its state directory): the capture's live props
// (pools 4 / 5) and collision volumes (pool 6) are removed, the cell records that made them made fresh
// again (+4 / +6 = -1), and the PORTED cell walker is run once on that image with the capture's own
// camera; every entity the capture held must come back with the same record-derived fields (class,
// settled position, rows, quaternion, box, mass, cursor and road coordinate; a volume's footprint
// corners, frame and heading). `mutate` (the negative control) moves every kind-4 / kind-6 record by
// 1/64 unit first. Returns the process exit code.
int CheckWorldSpawn(const std::string& ramPath, bool mutate);

// SpawnBike RASHCDI 0x800662F0's bike class, ModelBind(bike, 2, s2, 1)'s v0 stored at the bike's +0xB4
// (0x80066328): a player's is game_state +0x48 / +0x4C (player 1 / 2, 0x80065FD8..0x80065FEC); another
// bike's s2 = 9 s1 + 3 s4 + bank (0x80066138..0x80066194) with s1 the record's +1 class nibble (2 for the
// other class in race types 33 / 44) and s4 = 1 when s1 < 2 and the column nibble is 2 or more. The
// police are class 2 -> 18..20, which CopJoin SLUS 0x80028034 needs for the lights. Returns the class.
uint32_t SpawnBikeClass(rr::sim::GuestRam& g, uint32_t bike, int player);

// The model ModelBind picks by class (SLUS 0x8002FAD4): a bike (pool 2) is id 100 + (cls - 3 for cls 3..5 /
// 12..14, else cls) (0x8002FB84); a rider (pool 1) 150 + the SLUS word gp+572 / gp+576 by cls - 9 < 9 (0 / 9:
// model 159 for classes 9..17, 0x8002FB60). `fallback` when that id has no registry slot in the arena.
uint32_t BikeClassModel(rr::sim::GuestRam& g, uint32_t cls, uint32_t fallback);
uint32_t RiderClassModel(rr::sim::GuestRam& g, uint32_t cls, uint32_t fallback);

// Run totals (the log).
struct WorldTotals {
    size_t walks = 0, walkRefused = 0, passes = 0, passRefused = 0;
    size_t maxProps = 0, maxVolumes = 0, propSpawns = 0, volumeSpawns = 0, peds = 0, hazards = 0;
};
WorldTotals& WorldRunTotals();
std::string WorldTotalsLine();

} // namespace rr::game
