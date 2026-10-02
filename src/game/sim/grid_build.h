#pragma once
// src\game\sim\grid_build - the race loader's starting grid,
// ported from our own disassembly of the player's images:
//
//   RASHCDI.BIN  SHA-1 9a8b79d8739713c12b0a2dc4d75ef8f0a2cc0b06, loaded at 0x8005B5E8
//   SLUS_010.53  SHA-1 67ed165a2c517d4e6106fb0dfa324d66dd9a76f1, text at 0x80010000
//
//   BuildGrid    RASHCDI 0x80067B00..0x80068470, frame 2360
//   SpawnBike    RASHCDI 0x80065A94..0x80066E1C, frame 248
//   GridLists    RASHCDI 0x80065468..0x800654BC, a leaf (the five bike-list heads)
//   StartRecord  RASHCDI 0x8006ACD0..0x8006AD24, frame 24 (the [START] record and its cursor)
//   and the leaves they run inline: AllocBike 0x80065974, AllocRider 0x80065A04, PlayerColumn 0x80065648,
//   StartCursor 0x8006AD24, BikeName 0x80064034, Appearance 0x8005DD9C, memset 0x8001E100 / 0x8001E0DC,
//   memcpy 0x8001E0B4, MulAdd 0x8002EAD8, RatAtan2 0x80020018, and the PORTED RiderRecordInit 0x80064C0C
//   and RiderNameId 0x80066E1C (rider_record.h).
//
// MEMORY MODEL (road_query.h's GuestRam): every function takes `sp`, the stack pointer at ITS entry, and
// keeps the locals whose address a callee receives at the original's frame offsets - BuildGrid's staged
// grid (sp-2360+40, 18 x {lateral, along, slot}), its LEVEL<n>.BI buffer (sp-2360+256) and its class
// mask (sp-2360+2304); SpawnBike's name buffer (sp-248+16), extension (+144), cursor copy (+152), walked
// road coordinate (+184) and the player-cop word (+200). Callee-saved spills are not written.
//
// RASHCDI is the loader overlay: its data (the bike names 0x8006B51C, the appearance bytes 0x8006B89B, the
// ".PH" extension 0x8005B8E4, the grid class table 0x8006B8A0) is read where the overlay puts it, so a
// caller whose arena holds RASHCDG there must lay RASHCDI over it for the call (as the console has it
// resident while the race loads) and put RASHCDG back afterwards.
//
// Every callee with a frame of its own (or a CD read, or a BIOS call) goes through the callee interfaces
// below, given the stack pointer at which the original makes the call. The bench (rows_grid.inc) answers
// them with the oracle; the product (grid_session.cpp) with the ported functions and its own loaders.
#include <cstdint>

#include "game/sim/road_query.h"

namespace rr::sim {

constexpr uint32_t kBuildGridFn = 0x80067B00, kBuildGridEnd = 0x80068470, kBuildGridFrame = 2360;
constexpr uint32_t kSpawnBikeFn = 0x80065A94, kSpawnBikeEnd = 0x80066E1C, kSpawnBikeFrame = 248;
constexpr uint32_t kGridListsFn = 0x80065468, kGridListsEnd = 0x800654BC;
constexpr uint32_t kStartRecordFn = 0x8006ACD0, kStartRecordEnd = 0x8006AD24, kStartRecordFrame = 24;
constexpr uint32_t kRashcdiLoad = 0x8005B5E8;   // the loader overlay's load address
constexpr uint32_t kGridStartRecord = 0x800CF578; // {u16 road, u16 kind; s32 dir; s32 along}
constexpr uint32_t kGridStartCursor = 0x800CF588; // the 32-byte cursor StartRecord seats there
constexpr uint32_t kGridEscapeFlag = 0x8005AD30;  // u8: BuildGrid did its work (non-zero: it returns at once)
constexpr uint32_t kGridStatLoaded = 0x8005AD40;  // bit k: stat block k is loaded; bits 24..: the block count
constexpr uint32_t kGridPoolBytes = 0x8005B2B8;   // s8 x 4: pool 0 capacity / used, pool 1 capacity / used
constexpr uint32_t kGridRacers = 0x8005B1FC;      // the players and the non-police bikes

// ---------------------------------------------------------------------------- SpawnBike's callees
class SpawnCallees {
public:
    virtual ~SpawnCallees() = default;
    // The BIOS string calls through the SLUS stubs (A(19h) strcpy etc.).
    virtual bool StrCpy(uint32_t dst, uint32_t src, uint32_t sp) = 0;                          // SLUS 0x800448E4
    virtual bool StrCat(uint32_t dst, uint32_t src, uint32_t sp) = 0;                          // SLUS 0x80044864
    virtual bool StrLen(uint32_t s, uint32_t sp, uint32_t& v0) = 0;                            // SLUS 0x800448F4
    virtual bool StrRChr(uint32_t s, uint32_t c, uint32_t sp, uint32_t& v0) = 0;               // SLUS 0x80044964
    virtual bool StrNCmp(uint32_t a, uint32_t b, uint32_t n, uint32_t sp, uint32_t& v0) = 0;   // SLUS 0x80044874
    // The CD: a bike's 448-byte stat block (0x80065558), an animation bank (0x8005BCDC), ANIMNOIZ.DAT.
    virtual bool LoadBikePh(uint32_t name, uint32_t dst, uint32_t sp) = 0;                     // RASHCDI 0x80065558
    virtual bool AnimBank(uint32_t desc, uint32_t name, uint32_t sp, uint32_t& v0) = 0;        // RASHCDI 0x8005BCDC
    virtual bool AnimNoise(uint32_t sp) = 0;                                                   // RASHCDI 0x80063158
    // The model and the box.
    virtual bool ModelBind(uint32_t obj, uint32_t pool, uint32_t cls, uint32_t alloc, uint32_t sp, uint32_t& v0) = 0; // SLUS 0x8002FAD4
    virtual bool BoxSetup(uint32_t e, uint32_t idx, uint32_t sp) = 0;                          // SLUS 0x80012FC8
    virtual bool Attach(uint32_t b, uint32_t r, uint32_t kind, uint32_t seat, uint32_t sp) = 0; // SLUS 0x80012838
    // The road.
    virtual bool RoadWalk(uint32_t from, uint32_t out, int32_t dist, uint32_t sp) = 0;         // SLUS 0x80012C1C
    virtual bool RoadGate(uint32_t h, uint32_t key, uint32_t sp, uint32_t& v0) = 0;            // SLUS 0x80039DFC
    virtual bool CursorSeat(uint32_t obj, uint32_t key, uint32_t cursor, uint32_t sp, uint32_t& v0) = 0; // SLUS 0x8003A700
    virtual bool RoadPosition(uint32_t heading, uint32_t cursor, uint32_t out, uint32_t sp) = 0; // SLUS 0x8003662C
    virtual bool RoadClass(uint32_t e, int32_t mode, uint32_t cursorOut, int32_t zone, uint32_t sp) = 0; // SLUS 0x8003DE28
    virtual bool RoadsideRun(uint32_t e, int32_t mode, int32_t zone, uint32_t sp) = 0;         // SLUS 0x8003DF54
    virtual bool RouteBind(uint32_t p, int32_t step, uint32_t x, uint32_t sp) = 0;             // SLUS 0x8003AF9C
    virtual bool Progress(uint32_t p, uint32_t sp, uint32_t& v0) = 0;                          // SLUS 0x8003B61C
    // The race-type arms.
    virtual bool PlayerCopPlace(uint32_t e, uint32_t sp, uint32_t& v0) = 0;                    // RASHCDI 0x8006581C
    virtual bool SaveCopRecord(uint32_t e, uint32_t sp) = 0;                                   // RASHCDI 0x800644C8
};

// RASHCDI 0x80065A94 SpawnBike(entry, bi, slot, mask, flags): allocates pool-0 bike and pool-1 rider (index
// flags >> 4 with flag 4, else the next), flags 1 / 8 make it player 1's / player 2's; the AI index and the
// rider record (0x800D5758 + 72 ai, RiderRecordInit from `bi`), the class mask bit at *mask, the class
// table 0x8006B8A0, the name id, the stat block +0x22C (a player's .PH read unless already loaded), the
// models and boxes of bike and rider, the seat, player 1's animation banks and weapon objects, the countdown
// bits, and the placement: [START] walked by the entry's along x 125/128, the resident object and cursor,
// the position lateral x 125/128 off the slice, the rows, heading, aim point, road position, class, roadside
// run, route binding and progress; the rider copied from the bike; a police bike saved and parked.
// `entry` is {lateral, along, slot-word} (both offsets are negated / clamped in place, as the original does).
// Returns the bike (v0), 0 when a pool is full. `ok` = false when a callee refused or the view faulted.
uint32_t SpawnBike(GuestRam& g, uint32_t entry, uint32_t bi, uint32_t slot, uint32_t mask, uint32_t flags,
                   uint32_t sp, SpawnCallees& c, bool& ok);

// ---------------------------------------------------------------------------- BuildGrid's callees
class GridCallees {
public:
    virtual ~GridCallees() = default;
    virtual bool Malloc(uint32_t bytes, uint32_t heap, uint32_t sp, uint32_t& v0) = 0;         // SLUS 0x8001447C
    virtual bool LoadLevelBi(uint32_t buf, uint32_t bytes, uint32_t sp) = 0;                  // RASHCDI 0x80064B44
    virtual bool LoadLevelPh(uint32_t dst, uint32_t sp) = 0;                                  // RASHCDI 0x800654BC
    // SpawnBike with its fifth argument `flags` at sp+16 (the caller's outgoing-argument slot).
    virtual bool SpawnBike(uint32_t entry, uint32_t bi, uint32_t slot, uint32_t mask, uint32_t flags, uint32_t sp,
                           uint32_t& v0) = 0;                                                  // RASHCDI 0x80065A94
    virtual bool ComputePlace(uint32_t e, int32_t mode, uint32_t sp, uint32_t& v0) = 0;        // SLUS 0x800138E8
    virtual bool RiderAdjust(uint32_t e, uint32_t sp) = 0;                                     // RASHCDI 0x800650A0
    virtual bool SpawnPassenger(uint32_t host, uint32_t bi, uint32_t rec, uint32_t count, uint32_t sp) = 0; // 0x800670FC
};

// RASHCDI 0x80067B00 BuildGrid(block, size): unless *(0x8005AD30) is set, clears the AI index maps and the
// player pointers, allocates the stat array (448 x 4, or x 5 with two players) and loads LEVEL<n>.PH into it,
// the five bike lists, LEVEL<n>.BI into its frame, the entry count (the block's n, at most 18; the race-type
// arms: type & 4 fixes 1..4 bikes, type & 8 adds the second player's and compacts the grid to 10 racers
// (+2 / +3 in two-player / race type 44), type 17 swaps the first two entries' offsets, type & 4 with
// gs+0x38 == 2 makes entries 1 and 2 police), the pools 0 / 1 (malloc'd, cleared, their capacity bytes and
// the pool table 0x800CE4D0 / 0x800CE4E0), one SpawnBike per entry in file order (flags 1 for entry 0, 8 for
// entry 1 with two players) into the riding or the dormant list, the racer count 0x8005B1FC, the initial
// places (ComputePlace(e, 0) -> riderDef+0x27) with GridRiderAdjust, one bike per player on an empty grid,
// the sidecar passengers (bike classes 6..8 / 15..17), 0x8005AD30 = 1 and 0x8005B244 = player 1's +0x144 >> 12.
// False when a callee refused or the view faulted.
bool BuildGrid(GuestRam& g, uint32_t block, int32_t size, uint32_t sp, GridCallees& c);

// RASHCDI 0x80065468: the five bike-list heads 0x8005B298 / 0x8005B350 / 0x8005B2D8 / 0x8005B378 /
// 0x8005B270 point at themselves (head, and head+4 = the tail).
void GridLists(GuestRam& g);

// RASHCDI 0x8006ACD0 StartRecord(road, along, dir): 0x800CF578 = {road & 0xFFFF, dir, along} and the cursor
// 0x800CF588 seated on it (RoadGate(0, rec) then CursorSeat, both PORTED, called at sp - 24). Returns
// CursorSeat's answer (1 seated, 0 not); the view's fault state tells a refusal.
int32_t StartRecord(GuestRam& g, uint32_t road, uint32_t along, uint32_t dir, uint32_t sp);

// PlayerColumn RASHCDI 0x80065648 (a leaf): a player's grid column from gs+0x3A and gs+0x44.
int32_t GridPlayerColumn(GuestRam& g);

} // namespace rr::sim
