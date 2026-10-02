#pragma once
// The pedestrians in the product: what the race
// loader gives pool 2 besides the block the world arena allocates, the run counters for the log, and the
// renderer's view of a live pedestrian. Transcribed from our own listing of
//
//   RASHCDI.BIN  SHA-1 9a8b79d8739713c12b0a2dc4d75ef8f0a2cc0b06 (the race loader)
//
// THE SWITCH - EnterRace RASHCDI 0x80063BB8: *(0x8005B254) = ((game_state+4 & 0x18) == 0), written before
// the population reset (world_pop_product.h BuildWorldArenaLine allocates the 2288-byte block only then).
// `RRJB_PEDS=off` (the negative control) leaves it 0.
//
// THE ARENA - RASHCDI 0x800689D0's first two calls: 0x80068500 (with the switch on: the clip-id table
// 0x800D5F40 = 0x5000 + i for ids 224..236, the state table 0x800D5730) and 0x80069394's pool-2 part (each
// of the four slots gets its 17 part slots, +4 = malloc(408), +0 / +0x60 / +0x21C cleared, +12 of the
// block counted up to 7); and RASHCDI 0x8005C630's first load, DATA\PED01A.GEO through the RMD3 walker
// 0x8005CA10 (models 400 / 430, family 4) when (game_state+4 & 0x18) == 0. OURS, named: the models are
// loaded after the car file (the original loads them first - only the registry slot numbers differ), the
// part slots are zeroed, and where the mallocs sit (the session's bump region).
#include <cstdint>
#include <string>
#include <vector>

#include "game/sim/road_query.h"
#include "rrvfs/disc_image.h"

namespace rr::game {

// False with RRJB_PEDS=off.
bool PedsEnabled();

// EnterRace's switch (see above): the value written to 0x8005B254.
uint32_t PedSwitchFor(rr::sim::GuestRam& g);

// After BuildWorldArenaLine. Returns a line for the seam list.
// `raceId`: the race's level bundle (GAMEBIN1.DAT, rr::LevelBundleIndexForRace), as traffic_arena.h takes it.
std::string BuildPedArena(rr::sim::GuestRam& g, const rr::DiscImage& disc, int raceId, uint32_t& from, uint32_t limit);

// Run totals (the log): spawns the cell walker asked for and made, the most live at once, the passes run
// and refused, pedestrians knocked down (+0x228 bit 29 rising; `byPlayer` when the player's bike is
// within 6 units then).
struct PedTotals {
    size_t asked = 0, spawned = 0, maxLive = 0, passes = 0, passRefused = 0, releases = 0, releaseRefused = 0,
           knocked = 0, byPlayer = 0, spawnRefused = 0, drawn = 0, drawFrames = 0;
};
PedTotals& PedRunTotals();
// Once a frame after the release pass: the live count and the knock-downs.
void PedsFrame(rr::sim::GuestRam& g);
std::string PedTotalsLine();

// A live pedestrian for the renderer: its model id (400 / 430), LOD +0x08, position (16.16), rows +0x1B0
// (row k = model axis k in the world), its 17 part slots' 3x3 (the pose, rider_pose.h's layout) and the
// root triple +0x1C.
struct LivePed {
    uint32_t entity = 0, model = 0, lod = 0;
    uint32_t sheet = 0; // the LECT id of its sheet record (ModelKeySet's +0x4A index into *(0x8005B2E4)), 0 none
    int32_t pos[3] = {};
    int16_t rows[9] = {};
    int16_t parts[17][9] = {};
    int16_t root[3] = {};
    int32_t cell = -1;
};
std::vector<LivePed> LivePeds(const uint8_t* ram);

} // namespace rr::game
