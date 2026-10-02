#pragma once
// The hazard objects: the keyframed things that move over and across the road (the flying traffic of
// ENV.EN's 40 object templates - paths up to 120 units above the road, 300 units wide) and the class-9
// placements of the cell records. Ported function by function from our own listings of
//
//   RASHCDI.BIN  SHA-1 9a8b79d8739713c12b0a2dc4d75ef8f0a2cc0b06, loaded at 0x8005B5E8 (the race loader)
//   RASHCDG.BIN  SHA-1 cfe43a7786759f2cb9c57751cf99e84d1074782c, loaded at 0x8005B5E8
//   SLUS_010.53  SHA-1 67ed165a2c517d4e6106fb0dfa324d66dd9a76f1, text at 0x80010000
//
// and accepted only by `rrverify phys` (tools\rrverify\rows_hazards.inc).
//
// THE DATA. The race's STARTDF<A|B>.BIN block (292 bytes, raceId - 1) carries at +220 the class table
// {s8 set, s8 slot, s16 window}[2] (copied to 0x8005B328 by BuildRace's RASHCDI 0x80068470), at +228
// the event count and at +232 three 16-byte event templates {road, along lo, along hi, s16 variant,
// s16 window}; the loader reads them into its own BSS (0x8006B8B4 / 0x8006EB10) and ENV.EN whole into
// 0x8006B8B8 (RASHCDI 0x800655D4): +204 the pair count, +205 the pairs {HAZARD set, 6}, +221 + set the
// variant count, +227 + 30 set + 3 variant the three object templates of a variant (-1 ends), +408
// forty 312-byte object templates. The race loader's BSS is RASHCDG's code during the race, so the
// ports that read it take HOST copies (HazardLoaderData) - the bench plants the same bytes at the
// original addresses and hands the port what it planted.
//
// THE RECORDS (280 bytes, *(0x8005B304) + 280 i, handle 0xBD + i at +0xAC): +0x04 its model record
// (*(0x8005B250) + 24 i), +0x08 the group ModelBind selected, +0xB0 its cell (-1: HazardDraw finds
// one), +0xB4 the release radius switch (0: 600 units, else 300), +0xB8 the position, +0xC4 the rows,
// +0xD6 the heading after the last key, +0xDC the speed, +0xE0 the origin, +0xEC / +0xF0 yaw / pitch
// (mode 0), +0xF4 the base rows, +0x106 / +0x107 the side bytes, +0x108 the key, +0x10C the key's
// start time, +0x110 the time, +0x114 -> its object.
// THE OBJECTS (312 bytes, *(0x8005B24C) + 312 k): +0 s8 (< 0: two random keys; > 0: hold the last
// key; 0: loop), +1 mode (0: rides with player 1's bike, else fixed at the spawn), +2 s8 (keep the
// heading level), +3 u8 key count, +0x04 s32 duration[k], +0x20 / +0x3C the time curve's tangents /
// keys, +0x58 / +0x74 / +0x90 x / y / z keys, +0xAC / +0xC8 / +0xE4 their tangents, +0x100 speed[k],
// +0x11C roll[k] (7 entries each).
//
// THE SEAMS (RecoverCallees): Malloc SLUS 0x8001447C (the set-up's three blocks; the product answers
// from its arena) and ModelVisible RASHCDG 0x80067AC4 (HazardDraw's last call; the product's model
// runtime / renderer draws). Everything else is called natively.
#include <cstddef>
#include <cstdint>

#include "game/sim/integrator.h"
#include "game/sim/recover.h"
#include "game/sim/road_query.h"

namespace rr::sim {

constexpr uint32_t kHzRecordsPtr  = 0x8005B304; // -> 3 x 280-byte records
constexpr uint32_t kHzObjectsPtr  = 0x8005B24C; // -> 6 x 312-byte objects
constexpr uint32_t kHzModelsPtr   = 0x8005B250; // -> 3 x 24-byte model records
constexpr uint32_t kHzEventsPtr   = 0x8005B35C; // -> 16-byte events {road, along, s8 cls, s8 obj[3], s8 armed, _, s16 window}
constexpr uint32_t kHzEventCount  = 0x8005B2AC;
constexpr uint32_t kHzObjectCount = 0x8005B260; // the listed objects (the class-9 arm scans from here)
constexpr uint32_t kHzOut         = 0x8005B314; // records live
constexpr uint32_t kHzTable       = 0x8005B328; // {s8 set / class, s8 slot, s16}[2]
constexpr uint32_t kHzBikePtr     = 0x8005B38C; // player 1's bike

constexpr uint32_t kHazardPickFn      = 0x8006AD4C; // RASHCDI
constexpr uint32_t kHazardSetupFn     = 0x8006AEF4; // RASHCDI
constexpr uint32_t kHazardSpawnFn     = 0x800A0A20;
constexpr uint32_t kHazardEventFn     = 0x800A0EC4;
constexpr uint32_t kHazardRandomizeFn = 0x800A0F9C;
constexpr uint32_t kHazardPlaceFn     = 0x800A1318;
constexpr uint32_t kHazardPassFn      = 0x800A13C4;
constexpr uint32_t kHazardDrawFn      = 0x800A2138;
constexpr uint32_t kHazardReleaseFn   = 0x80014000; // SLUS
constexpr uint32_t kHermiteFn         = 0x8002FA28; // SLUS
constexpr uint32_t kHermiteDFn        = 0x8002F994; // SLUS
constexpr uint32_t kHzMallocFn        = 0x8001447C; // SLUS, seam
constexpr uint32_t kHzModelVisibleFn  = 0x80067AC4; // seam

// The race loader's BSS, as host copies.
struct HazardLoaderData {
    const uint8_t* env = nullptr; // ENV.EN (0x8006B8B8), 12888 bytes
    size_t envSize = 0;
    int32_t sel = 0;              // *(0x8006EB40), HazardPick's pair
    int32_t eventCount = 0;       // *(0x8006B8B4)
    uint8_t events[48] = {};      // 0x8006EB10
};

// RASHCDI 0x8006AD4C HazardPick: sel = Rand() % env[204] (0 when env[204] is 0); when the class table
// 0x8005B328 names sets, the pairs after sel (cyclically) are searched for one holding every named set
// (sel itself is never tested: the search stops on coming back to it). `sel` gets *(0x8006EB40); returns clamp(pair[sel][0],
// 1, 5). False when an ENV.EN index leaves the buffer.
bool HazardPick(GuestRam& g, const uint8_t* env, size_t envSize, int32_t& sel, int32_t& v0);

// RASHCDI 0x8006AEF4 HazardSetup (sp = the stack pointer at entry; the object list is at sp-112+16).
bool HazardSetup(GuestRam& g, const HazardLoaderData& d, uint32_t sp, RecoverCallees& c);

// RASHCDG 0x800A0A20 HazardSpawn(cls, mode, pos[3], rows, [sp+16] obj): v0 the record or 0.
bool HazardSpawn(GuestRam& g, uint32_t cls, uint32_t mode, uint32_t pos, uint32_t rows, uint32_t obj, uint32_t sp,
                 const BikeTables& t, uint32_t& v0);
// RASHCDG 0x800A0EC4 HazardEvent(ev, pos, rows): the event's objects spawned, its armed byte cleared.
bool HazardEvent(GuestRam& g, uint32_t ev, uint32_t pos, uint32_t rows, uint32_t sp, const BikeTables& t);
// RASHCDG 0x800A0F9C HazardRandomize(obj): a random object's next leg.
void HazardRandomize(GuestRam& g, uint32_t obj, uint32_t sp);
// RASHCDG 0x800A1318 HazardPlace(rec, local[3], rows): +0xB8 = rows x local + origin, cell -1.
void HazardPlace(GuestRam& g, uint32_t rec, uint32_t local, uint32_t rows, uint32_t sp);
// RASHCDG 0x800A13C4 HazardPass(dt): the events near player 1 armed, every record moved on its keys.
bool HazardPass(GuestRam& g, int32_t dt, uint32_t sp, const BikeTables& t);
// RASHCDG 0x800A2138 HazardDraw(p): each live record's cell (the nearest), its model frame, ModelVisible.
bool HazardDraw(GuestRam& g, uint32_t p, uint32_t sp, RecoverCallees& c);
// SLUS 0x80014000 HazardRelease(rec).
void HazardRelease(GuestRam& g, uint32_t rec);
// SLUS 0x8002FA28 Hermite(t, h00, h01, h10, [sp+16] h11) and 0x8002F994 its derivative, over guest words.
void Hermite(GuestRam& g, int32_t t, uint32_t h00, uint32_t h01, uint32_t h10, uint32_t h11);
void HermiteD(GuestRam& g, int32_t t, uint32_t d00, uint32_t d01, uint32_t d10, uint32_t d11);

} // namespace rr::sim
