#pragma once
// The hazard objects in the product (src\game\sim\hazard.h): the race loader's
// part (BuildRace's STARTDF block copy RASHCDI 0x80068470 transcribed, HazardPick 0x8006AD4C and
// HazardSetup 0x8006AEF4 PORTED), the cell walker's class-9 seam closed by the PORTED spawner 0x800A0A20,
// the rider/engine pass's last child 0x800A13C4 and GameFrame's per-view 0x800A2138 run PORTED, and the
// live records for the renderer.
//
// OURS, named: the loader's BSS (ENV.EN at 0x8006B8B8, the event templates 0x8006B8B4 / 0x8006EB10, sel
// 0x8006EB40) is RASHCDG's code during the race, so it is kept host-side (HazardLoaderData); the three
// mallocs come from the session's bump region; HazardDraw's last call ModelVisible 0x80067AC4 runs where the
// product runs it for every drawn model, in the render's model pass (model_runtime.h: the live records are
// appended to its entity list by AppendHazardRecords), after the frame - its +0x09 view bit is what the next
// pass's release test reads; the GL renderer draws a record whose view bit is set as group +0x08 of the prop
// model at +0xB8 turned by +0xC4.
//
// DEVELOPMENT switch, the negative control: RRJB_HAZARDS=off leaves all of it out (no set-up, the seam
// answered: not run, the pass and the draw not run).
#include <cstdint>
#include <string>
#include <vector>

#include "game/sim/hazard.h"
#include "rrvfs/disc_image.h"

namespace rr::game {

bool HazardsOn(); // false under RRJB_HAZARDS=off (read once)

struct HazardRace {
    std::vector<uint8_t> env; // ENV.EN
    rr::sim::HazardLoaderData data;
    int32_t pick = -1;        // HazardPick's v0 (the HAZARD set before the mode rule)
};

// BuildRace's RASHCDI 0x80068470 (the class table into 0x8005B328; the event count and templates into
// `h`) for race `raceId` of road set `set` (1: STARTDFA.BIN, 2: STARTDFB.BIN), ENV.EN into `h`, then the
// PORTED HazardPick 0x8006AD4C unless the mode has bit 4 (0x80063814) and the loader's set rule
// (0x8005C7F0: set 0 in mode (mode & 0x18) == 8, set 1 in mode bit 4). Returns the HAZARD<n> number, -1
// without ENV.EN; `line` gets a line for the seam list.
int HazardSetForRace(rr::sim::GuestRam& g, const rr::DiscImage& disc, int set, int raceId, uint8_t mode,
                     HazardRace& h, std::string& line);

// RASHCDI 0x800639A0: the PORTED HazardSetup 0x8006AEF4 unless the mode has bit 4, its mallocs from
// [from, limit). Returns a line for the seam list.
std::string BuildHazardArenaLine(rr::sim::GuestRam& g, const HazardRace& h, uint8_t mode, uint32_t& from,
                                 uint32_t limit);

// The cell walker's seam 0x800A0A20(cls, 1, pos, 0, [obj]) served by the PORTED spawner.
bool HazardSpawnSeam(rr::sim::GuestRam& g, const uint32_t* a, int n, uint32_t sp, const rr::sim::BikeTables& t,
                     uint32_t& v0);
// RASHCDG 0x8008ACE8's last child, HazardPass 0x800A13C4, PORTED.
bool RunHazardPass(rr::sim::GuestRam& g, int32_t dt, uint32_t sp, const rr::sim::BikeTables& t);
// GameFrame's per-view 0x800A2138 (SLUS 0x80012010: only while *(0x8005B314)), PORTED, for views 0..players-1.
bool RunHazardDraw(rr::sim::GuestRam& g, int players, uint32_t sp);

// A live hazard record for the renderer: group `group` of the prop model at `pos` (16.16), turned by
// `rows` (row k = model axis k in the world, as the props' +0x1B0), filed in cell `cell` (+0xB0).
struct LiveHazard {
    uint32_t record = 0;
    uint32_t group = 0;
    int32_t pos[3] = {};
    int16_t rows[9] = {};
    int32_t cell = -1;
    uint8_t viewBits = 0; // +0x09: ModelVisible's bit per view (set: visible in that view's last model pass)
};
std::vector<LiveHazard> LiveHazards(const uint8_t* ram);
// The live records, appended to the render's model-pass entity list (their ModelVisible and ModelDraw).
void AppendHazardRecords(const uint8_t* ram, std::vector<uint32_t>& entities);
// The renderer's count of frames with a hazard drawn (for the log).
void NoteHazardDrawn(size_t n);

std::string HazardTotals();

} // namespace rr::game
