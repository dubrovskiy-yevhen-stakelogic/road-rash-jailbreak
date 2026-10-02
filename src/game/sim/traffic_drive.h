#pragma once
// The per-frame traffic pass - every pool-3 car: the window test, the release when it leaves the
// window, dead-ends or gives up behind a blocker, and otherwise the drive along its road in its lane
// (speed, road step, rows and heading, the road re-classification, the lane lateral, the blocker
// search, the horn, the brake and the lane change) - ported from our own disassembly of the player's
// own images:
//
//   SLUS_010.53  SHA-1 67ed165a2c517d4e6106fb0dfa324d66dd9a76f1, text at 0x80010000
//   RASHCDG.BIN  SHA-1 cfe43a7786759f2cb9c57751cf99e84d1074782c, loaded at 0x8005B5E8
//
// This file carries the addresses line by line. Accepted only by `rrverify phys` rows
// (tools\rrverify\rows_traffic_drive.inc), one per function.
//
// The memory model, the stack convention and the fault rule are population.h's: functions over
// road_query.h's guest-address view (`GuestRam`); a function with a frame takes `sp`, the stack pointer
// at its entry, and hands its callees the `sp` the original makes each call at; locals whose address
// the original hands out (the road step's candidate array and walk direction, the blocker's handle
// list and count, the lane-change node word) and the words the original leaves in its own frame that a
// later read can see (the per-player arrays of 0x8009F6F8 / 0x800A04B0, the 64-bit product spills,
// the stack arguments) are written at the original's offsets; callee-saved register spills are not.
// A load or store the console would not survive is not performed; `GuestRam` records the first one and
// the caller must FAIL the call.
//
// RAND. Drawn where the original draws it, in its order:
// per car, LaneLateral three times (0x8009A844/850/85C, a draw each when the car's lane is +-2 on a
// road of three or more lanes), the horn's `& 7` (0x8009FAA0, a car stopped behind a bike), the lane
// change's `& 0x64` (0x8009B9BC - the original's mask, not `% 100`).
#include <cstdint>

#include "game/sim/population.h"
#include "game/sim/road_query.h"

namespace rr::sim {

// ---------------------------------------------------------------------------- globals
constexpr uint32_t kTdPool3Table   = 0x800CE500; // pool 3's control in the pool table: +0 base, +4 stride,
                                                 // +12 -> the high index
constexpr uint32_t kTdPoolTable    = 0x800CE4D0; // 16 bytes per pool: +0 base, +4 stride
constexpr uint32_t kTdPlayer1Bike  = 0x8005B38C;
constexpr uint32_t kTdPlayer2Bike  = 0x8005B21C;
constexpr uint32_t kTdLaneTimer    = 0x8005B2E0; // s32: the lane-change accumulator of dt (all cars)
constexpr uint32_t kTdFinishAlong  = 0x8005317A; // u16 (SLUS data): the finish along's integer
constexpr uint32_t kTdFinishDir    = 0x8005B2E8; // s32: the direction the finish is approached in
constexpr uint32_t kTdCatchupTab   = 0x800530F0; // s32 by bank (SLUS data): the catch-up speed
constexpr uint32_t kTdSinCos       = 0x8005624C; // {s16 sin, s16 cos} x 4096 (SLUS data)
constexpr uint32_t kTdPool6Ptr     = 0x800CD6C4; // -> pool 6's 280-byte records
constexpr uint32_t kTdGridOriginX  = 0x800CCF98; // s32[2] the two proximity grids' x origins
constexpr uint32_t kTdGridOriginZ  = 0x800CCFA0; // s32[2] their z origins
constexpr uint32_t kTdGridLinks    = 0x800CCFA8; // {u8 next, u8 handle} per grid entry, 128 ends
constexpr uint32_t kTdGridCells    = 0x800CD0B0; // u8 heads, 2 grids x 24 x 24
constexpr uint32_t kTdHandleOffTab = 0x800CCA68; // s32 by pool - 2: the handle's offset in the record
constexpr uint32_t kTdWarnClockGp  = 1936;       // gp+1936: the clock of the last pass warning
constexpr uint32_t kTdWarnPosGp    = 1920;       // gp+1920: -> 72-byte per-player listener records

// ---------------------------------------------------------------------------- the callees
// What the pass reaches that is NOT written in this file. The caller supplies each one; `sp` is the
// stack pointer the original makes the call at. Each returns false when the caller could not run it;
// the port then refuses (returns false) and nothing it wrote is to be trusted.
struct TrafficDriveCallees {
    virtual ~TrafficDriveCallees() = default;
    // The population's: Release 0x8008C000 (the car release, traffic_bind.h PoolRelease), BuildObb
    // 0x8008BA18 (bike.h), NodeLanes SLUS 0x8003E1E8 (under LaneLateral for a car at a node) and the
    // road layer's six under RoadClass / RoadsideRun / RouteBind (`Road()`).
    virtual PopulationCallees& Population() = 0;
    // SLUS 0x8003C520 (traffic_leaves.h CursorRoadEnd): the along of the end of the cursor's road.
    virtual bool RoadLength(uint32_t cursor, uint32_t sp, uint32_t& v0) = 0;
    // SLUS 0x80017BA0 PlaySound3D(x, z, soundIndex, bank) (sound.h, ported).
    virtual bool PlaySound3D(int32_t x, int32_t z, int32_t id, int32_t bank, uint32_t sp) = 0;
};

// ============================================================================ the road step (SLUS)

// SLUS 0x8003775C, 181 instructions, frame 176: walks the 32-byte cursor `cursor` |dist| along the
// road in the walk direction (p[+0xC0] = e[+0x16C] for an entity handle `p`, else the sign of dist):
// within the slice (cursor +0x14), slice by slice (+-52), and across a sub-object end by the
// neighbour walkers and the pick (road_query.h; candidates at sp-176+24, directions at +120, the
// walk direction at +136, the first candidate's +0x1C cleared first). A walk that meets a road end
// whose next object is not resident stops there. `v0` = the slice it ends on (0 for no cursor).
// False only when the walk ran past a named bound (the rows file) or the view faulted.
bool RoadStep(GuestRam& g, uint32_t p, uint32_t cursor, int32_t dist, uint32_t sp, uint32_t& v0);

// ============================================================================ the proximity grid

// RASHCDG 0x8008AE94, 357 instructions, frame 56: the handles (u16, up to *countPtr of them, no
// duplicates) of the entities of the pools whose bits are set in `mask` whose box centre x/z lies
// within `radius` of `pos` (|dx| + |dz| - min / 2), from the two 24 x 24 proximity grids (2.0-unit
// cells; the second grid for a two-player game's second player). The fifth and sixth arguments are
// the caller's: `mask` at sp+16 (read on every entry) and the handle to skip at sp+20 (u16). Writes
// the count to *countPtr unless the list filled up (then it already equals it). False when the
// named chain bound was hit or the view faulted.
bool GridQuery(GuestRam& g, uint32_t out, uint32_t countPtr, uint32_t pos, int32_t radius, uint32_t sp);

// ============================================================================ the car's callees (RASHCDG)

// 0x8009BA5C, 59 instructions, a leaf: the nearest live car ahead of `car` on its road key in its
// direction (along greater for dir > 0, smaller for dir < 0), or 0.
uint32_t CarAhead(GuestRam& g, uint32_t car);

// 0x800A04B0, 73 instructions, frame 8: the player the car is judged against: 0 unless race type bit
// 0x10; with both players on one road, 1 when the car's along is nearer player 2's (the per-player
// distances at sp-8+0/+4 - with one player the second is the stale stack word the original reads);
// else the first player on the car's road, or 0.
uint32_t LanePlayer(GuestRam& g, uint32_t car, uint32_t sp);

// 0x800A05D4, 77 instructions, frame 24: 1 when bike (or downed rider) `car[+0x1FF]` is on the car's
// road key close enough to stop it: at a node within 3.0; on a road approaching head-on within 12.0,
// or with the car within 3.0 of either end of its road (SLUS 0x8003C520).
bool PlayerClose(GuestRam& g, uint32_t car, uint32_t sp, TrafficDriveCallees& c, int32_t& v0);

// 0x8009B4C0, 154 instructions, frame 40: the brake. A car at a node clears bits 0, 1, 6 of +0x1FD;
// a stopped car (+0x1E0 < 131) counts +0x1F8 up by dt and pulls away at 2.0 (2.4 acceleration, bit 6);
// a car near its road's end node (SLUS 0x8003A5F4, < 15.0) at speed brakes with -v^2 / 2d (bits 0, 7).
void CarBrake(GuestRam& g, uint32_t car, int32_t dt, uint32_t changed, uint32_t sp);

// 0x8009B728, 205 instructions, frame 64: the lane change. Every 2.0 of the accumulator 0x8005B2E0 (a
// car not already changing, stopped or braking), a car in front of the judged player's camera, faster
// than 131, on his road 2.5..5.0 ahead of him, with >= 2 lanes, > 100.0 from its road's end node and no
// car ahead within 25.0: `(Rand() & 0x64) < 40` changes the lane (2 lanes: 1 <-> 2; 3+: to or from
// the middle) and sets +0x1FE bits 0, 1.
void LaneChange(GuestRam& g, uint32_t car, int32_t dt, uint32_t sp);

// 0x8009F6F8, 189 instructions, frame 80: 1 when the blocked car gives up: per player, its box is
// behind the player's camera (+0x1BC row) and the blocker's is too, or it is 160 or more (octagonal)
// from the camera and the blocker is behind it / is a pool-3 car. Race type bit 0x10 needs both
// players (the per-player words at sp-80+0/+4, stale when one player).
int32_t BlockGiveUp(GuestRam& g, uint32_t car, uint32_t h, uint32_t sp);

// 0x8009F9EC, 59 instructions, frame 32: the horn - a stopped car (+0x1E0 < 131, blocked) behind a
// bike or rider (not a finished one) sounds 99 - (+0xB4 & 1) at its box when `(Rand() & 7) == 0`.
bool Horn(GuestRam& g, uint32_t car, uint32_t h, uint32_t sp, TrafficDriveCallees& c);

// SLUS 0x80017CF0, 44 instructions, frame 24: the pass-warning sound, at most once per 76 clock ticks
// (gp+1936), drawn a quarter of the way from player `idx`'s listener (gp+1920) to (x, z): 98 + bit.
bool WarnSound(GuestRam& g, uint32_t idx, uint32_t bit, int32_t x, int32_t z, uint32_t sp, TrafficDriveCallees& c);

// 0x8009FCDC, 109 instructions, frame 40: 1 when a seated player faster than 40.0 on the car's road
// key, heading the other way, has passed it within its half-width (+0x130) laterally; the warning
// sound for each such player.
bool PassWarn(GuestRam& g, uint32_t car, uint32_t sp, TrafficDriveCallees& c, int32_t& v0);

// 0x800BF0F8, 203 instructions, frame 40: the gap from `car` to the entity of handle `h` along the
// car's heading, as time (distance / closing speed) when closing faster than 132; 0x7FFF0000 when it
// is not in the car's path.
int32_t BlockerGap(GuestRam& g, uint32_t h, uint32_t car, uint32_t sp);

// 0x800BE7D8, 150 instructions, frame 88: the blocker - the grid's entities within 60.35 (pools 0, 1,
// 3, 4, 5, not the car itself) with the smallest gap below 10.0. None: the car resumes (2.4 when it was
// blocked) and bits 4, 5 of +0x1FD clear; else the car brakes to stop at the gap (or stops below
// 0.05) and sets bit 4 (bit 7 on a new block). `v0` = the blocker's handle address (pool 6: its
// record), or 0. False when the grid query hit its named bound or the view faulted.
bool Blocker(GuestRam& g, uint32_t car, uint32_t sp, uint32_t& v0);

// ============================================================================ the pass

// 0x8009A298, 562 instructions, frame 72: TrafficPass(dt), from WorldBikePass 0x8008AC80. Per pool-3
// slot with a handle and CarCheck: the window test into +0x140 (0 -> release 0x8008C000(&car[+0xAC], 3));
// the previous box +0x1D4; the speed +0x1E0 integrated from +0x1E4 and capped (a class-0 car in an odd
// race type by the catch-up table and the player's speed); the road step; on a new slice the rows
// +0x1B0..+0x1C6 and heading +0x124/+0x128/+0x12C from the slice (negated against the heading);
// a dead end (the same slice run past its end) -> release; the road position / class / roadside /
// zones / route / progress; the box on the slice at the lateral +0x158, steered toward the lane's
// (LaneLateral x 3); the pass warning, the blocker (giving up -> release), the horn, the brake,
// BuildObb, the lane change.
bool TrafficPass(GuestRam& g, int32_t dt, uint32_t sp, TrafficDriveCallees& c);

} // namespace rr::sim
