#pragma once
// The population runtime - which entities around the players are simulated ("live") and which are
// only a road coordinate ("dormant"), the transitions between the two, the downed riders' own pass,
// the dormant racers' route walk, and the traffic spawner - ported from our own disassembly of the
// player's own images:
//
//   SLUS_010.53  SHA-1 67ed165a2c517d4e6106fb0dfa324d66dd9a76f1, text at 0x80010000
//   RASHCDG.BIN  SHA-1 cfe43a7786759f2cb9c57751cf99e84d1074782c, loaded at 0x8005B5E8
//
// The executable models of tools\scout\spawn.py are the specification; this file carries the
// addresses line by line. Accepted only by `rrverify phys` rows (tools\rrverify\rows_population.inc).
//
// THE MEMORY MODEL is road_query.h's guest-address view (`GuestRam`): the window test chases the
// cursor, the BTT_ records, the resident piece list, the view records and the pool-0 slots; the
// passes walk the pools; placement writes a bike and its rider. Which pointer is chased next depends
// on what the last one held, so the functions run on the guest addresses themselves.
//
// STACK. As in road_runtime.h: every function that has a frame takes `sp`, the stack pointer at its
// entry, and hands its callees the `sp` the original makes each call at, so that a caller that
// supplies an unported callee from the original code (the bench) runs it on the same stack bytes.
// Locals whose address the original hands out, and the words it leaves in its own frame that a later
// read of the same function uses (the window test's per-player distance array), are written where
// the original writes them; callee-saved register spills are not.
//
// Faults. A load or store the console would not survive is not performed; `GuestRam` records the
// first such address and the caller must FAIL the call. Nothing here guesses a value.
#include <cstdint>

#include "game/sim/road_query.h"
#include "game/sim/road_runtime.h"

namespace rr::sim {

// ---------------------------------------------------------------------------- globals
constexpr uint32_t kPopViewArray      = 0x800CD898; // View[2], stride 1132 (+0xB8/+0xC0 the camera x/z)
constexpr uint32_t kPopViewStride     = 1132;
constexpr uint32_t kPopVolumeRadiusGp = 204;        // gp+204 = 0x8005AD58: the collision-volume radius
constexpr uint32_t kPopPieceList      = 0x800D4B10; // resident road pieces, 16 bytes, first word the id
constexpr uint32_t kPopPieceLast      = 0x8005B31C; // s32: the list's last index (-1 = empty)
constexpr uint32_t kPopPool0Ptr       = 0x8005B3A0; // -> pool-0 slot 0, stride 1096
constexpr uint32_t kPopGameStatePtr   = 0x8005B2F8; // -> game_state; +0x30 the player count
constexpr uint32_t kPopPlayerBikes    = 0x8005B268; // player p's bike at +4p
constexpr uint32_t kPopPoolTable      = 0x800CE4D0; // pool 0 control: +0 base, +4 stride, +12 -> high index

// ============================================================================ the window test and its
// road helpers (SLUS)

// SLUS 0x80013110, 61 instructions, a leaf: 1 when the integer x/z of `pos` lies within the window of
// player p's view record (octagonal distance; bikes and riders 300 / 350 sticky, collision volumes
// (kind 6) (gp+204 + extra) >> 16, everything else 200 / 230). `sticky` is the original's fifth
// argument (sp+16), read as a whole word.
int32_t WindowPred(GuestRam& g, uint32_t kind, uint32_t pos, uint32_t p, uint32_t extra, uint32_t sticky);

// SLUS 0x80039CFC, 64 instructions, frame 24: 1 when the object of BTT_ record `btt` holds road
// coordinate `key` ({u32 road | kind << 16, s32 dir, s32 along}; the along's integer half is read):
// a road piece by its id and along range (+0x16..+0x1A), a junction by its own node key (then
// BTT_ +0x1C == 0) or by the arm FindRoadPiece finds on the key's road (GRPT +0x1A..+0x1E).
int32_t PieceContains(GuestRam& g, uint32_t btt, uint32_t key);

// SLUS 0x80039DFC, 91 instructions, frame 48: the resident object that holds `key`, or 0. `h` is an
// entity's handle address (e + 0xAC) or 0. A live entity's own cursor object is tried first (a
// player's bike or rider takes it without a test); then the resident piece list 0x800D4B10 is scanned
// (entries 0..*(0x8005B31C), -1 = empty, the live entity's own object skipped); a hit returns the
// object - or, for a live entity, ITS OWN object.
uint32_t RoadGate(GuestRam& g, uint32_t h, uint32_t key);

// SLUS 0x80037524, 142 instructions, a leaf: seats `cursor` (object, piece, sub-object set, slice
// the sub-object's first) at `along` from the piece start through the object's DIST table (+0x44):
// the slice and cursor +0x14 (along in slice). Before the piece's range: the first slice at 0; past
// it: the last slice at its chord. Returns the slice it stopped on (0 on the refusals).
uint32_t SeatAlong(GuestRam& g, uint32_t cursor, int32_t along);

// SLUS 0x8003A700, 163 instructions, frame 48: builds the 32-byte cursor on `obj` for road
// coordinate `key` (road arm: the piece, its first sub-object, SeatAlong; node arm: the object must be
// the node's own core, the sub-object comes from the route record's turn (TurnSubObject) and the
// junction / turn words from the IPT_ record). Returns 1, or 0 when it cannot.
int32_t CursorSeat(GuestRam& g, uint32_t obj, uint32_t key, uint32_t cursor, uint32_t sp);

// SLUS 0x8003A468, 99 instructions, frame 40: re-seats `cursor` from a road coordinate alone
// (RoadGate(0, key), the piece on the key's road, SeatAlong clamped to the piece). Returns 1 or 0.
int32_t CursorReseat(GuestRam& g, uint32_t key, uint32_t cursor);

// SLUS 0x80039F68, 261 instructions, frame 56: the road-window test of the entity whose handle is
// at `h` (= e + 0xAC). Returns the new live value (0 / 1; on a player's own bike its live word as it
// is). Re-seats a dormant bike or rider on its road coordinate, and sets a far cop's +0x3A0 bit 5.
uint32_t RoadWindow(GuestRam& g, uint32_t h, uint32_t sp);

// ============================================================================ three small SLUS leaves
// the placement and the retirement reach (ported with them)

// SLUS 0x80012838, 8 instructions: seat rider `r` on bike `b` in seat `seat`: r[+0x48] = 1,
// b[+0x38 + 8 seat] = r, b[+0x3C + 8 seat] = kind, r[+0x34] = b.
void Attach(GuestRam& g, uint32_t b, uint32_t r, int32_t kind, int32_t seat);
// SLUS 0x80012884, 66 instructions: the first free animation object of descriptor `desc` (0x800CE170)
// for rider `r` (object +0x24 == 0; none when the used count +8 equals the capacity +12): owner, the
// "-1" at +0x6E0 (or the SLUS 0x80052390 word for a rider kind 1 / 4), flags | 5, its program's byte
// +1 = 5, the used count + 1. Returns the object, or 0.
uint32_t ViewSlot(GuestRam& g, uint32_t desc, uint32_t r);
// SLUS 0x8002820C, 12 instructions: e[+0x24] &= ~(bits 27, 28, 29).
void CopDrop(GuestRam& g, uint32_t e);

// ============================================================================ the RASHCDG callees
// The functions the passes below reach that are NOT written in this file. The caller supplies each
// one; `sp` is always the stack pointer the original makes the call at. Each returns false when the
// caller could not run it; the port then refuses (returns false) and nothing it wrote is to be trusted.
struct PopulationCallees {
    virtual ~PopulationCallees() = default;
    // ---- NOT ported (the bench runs the original code for them)
    virtual bool ResetBike(uint32_t e, uint32_t sp) = 0;                                  // SLUS 0x8002090C (1)
    virtual bool CopLeave(uint32_t e, uint32_t sp) = 0;                                   // SLUS 0x8002847C (1)
    virtual bool CopJoin(uint32_t e, uint32_t sp) = 0;                                    // SLUS 0x80028034 (1)
    virtual bool AxisRotation(uint32_t axis, int32_t ang, uint32_t out, uint32_t sp) = 0; // SLUS 0x8003FB34 (3)
    virtual bool RowsFromHeading(uint32_t e, uint32_t sp) = 0;                            // 0x8007EC30 (1)
    virtual bool RiderDismount(uint32_t r, int32_t how, uint32_t sp) = 0;                 // 0x800C3104 (2)
    // ---- PORTED elsewhere; the caller runs them on the same memory
    virtual bool Remount(uint32_t b, int32_t fromRoad, uint32_t sp) = 0;                  // 0x800903F4 (crash.h)
    virtual bool StanceEvent(uint32_t ev, uint32_t r, uint32_t p, uint32_t sp) = 0;       // 0x800C4550 (stance.h)
    virtual bool BankSwitch(uint32_t a, uint32_t bank, uint32_t sp) = 0;                  // SLUS 0x80012858 (anim.h)
    virtual bool SeatRelease(uint32_t b, uint32_t r, uint32_t idx, uint32_t sp) = 0;      // 0x80068D20 (anim.h)
    virtual bool BuildObb(uint32_t e) = 0;                                                // 0x8008BA18 (bike.h)
    virtual bool ClearCommands(uint32_t e, uint32_t sp) = 0;                              // 0x800BCD10 (ai.h)
    // 0x800BCA68 (ai.h); `cmd` is the guest address of the 8-byte command record.
    virtual bool PushCommand(uint32_t cmd, int32_t mode, uint32_t e, uint32_t sp) = 0;
    virtual bool TargetSpeed(uint32_t e, int32_t dt, uint32_t sp, int32_t& v0) = 0;       // 0x80095BF8 (ai.h)
    // ---- the spawner's and the traffic's, NOT ported
    virtual bool CellWalker(uint32_t sp) = 0;                                             // 0x8009C308 (0)
    virtual bool PoliceSched(int32_t acc, uint32_t sp) = 0;                               // 0x8009E89C (1)
    virtual bool Budget(int32_t kind, uint32_t sp, uint32_t& v0) = 0;                     // 0x8008CDF4 (1)
    virtual bool ShareFlags(uint32_t flags, uint32_t sp) = 0;                             // 0x8009FF24 (1)
    virtual bool RoadWalk(uint32_t from, uint32_t out, int32_t dist, uint32_t sp) = 0;    // SLUS 0x80012C1C (3)
    virtual bool Spacing(uint32_t rec, uint32_t sp, uint32_t& v0) = 0;                    // 0x8009F578 (1)
    virtual bool Release(uint32_t h, int32_t pool, uint32_t sp) = 0;                      // 0x8008C000 (2)
    virtual bool ModelBind(uint32_t car, int32_t pool, uint32_t cls, uint32_t sp, uint32_t& v0) = 0;  // SLUS 0x8002FAD4 (4)
    virtual bool CarSetup(uint32_t car, uint32_t sp) = 0;                                 // SLUS 0x80012FC8 (car, 0)
    virtual bool NodeLanes(uint32_t obj, int32_t road, uint32_t dirOut, uint32_t sp, uint32_t& v0) = 0;  // SLUS 0x8003E1E8 (3)
    // The six under the road re-bind (road_runtime.h), for RoadClass / RoadsideRun / RoadRebindBody /
    // RouteBind: `RoadRuntimeNative` wherever the ports are wanted.
    virtual RoadRuntimeCallees& Road() = 0;
};

// ============================================================================ bikes: the activation
// pass (RASHCDG)

// 0x8009DAF8, 24 instructions, a leaf: *(0x800D86F0) (cops out) += 1 or -= 1 by the sign of `d`,
// then clamped into [0, *(0x800D86F4)] - three stores to the one word. Returns the clamped count.
int32_t CopCount(GuestRam& g, int32_t d);
constexpr uint32_t kPopCopsOut = 0x800D86F0;

// 0x80093FE4, 104 instructions, frame 24: a bike leaves the window (e[+0x140] == 0).
bool Retire(GuestRam& g, uint32_t e, uint32_t sp, PopulationCallees& c);
// 0x8009432C, 879 instructions, frame 88: a bike enters the window - put on the road at its road
// coordinate, faced along it, its rider seated; the cop tail.
bool Placement(GuestRam& g, uint32_t e, uint32_t sp, PopulationCallees& c);
// 0x80093F94, 20 instructions, frame 24: placement or retirement when bit 0 of the live word changed
// (dispatched on the WHOLE live word).
bool Transition(GuestRam& g, uint32_t e, uint32_t old, uint32_t sp, PopulationCallees& c);
// 0x80093ED4, 48 instructions, frame 32: one bike's live word from the window test (0 when `force`).
bool Activate(GuestRam& g, uint32_t e, uint32_t force, uint32_t sp, PopulationCallees& c);
// 0x80093E6C, 26 instructions, frame 32: Activate(e, 0) for every pool-0 slot.
bool ActivationPass(GuestRam& g, uint32_t sp, PopulationCallees& c);

// ============================================================================ riders on the ground


// 0x80099D48, 188 instructions, frame 64: a dormant downed rider's road coordinate walks toward its
// bike's by 5.0 a call (or jumps onto the bike's road). Race graph through GraphRoad / GraphNode.
void RiderChase(GuestRam& g, uint32_t r);
// 0x800952AC, 89 instructions, frame 40: a downed rider enters (re-seated, stance event 73) or
// leaves the window (its bike dismounts it and is told to fetch it, or is remounted).
bool RiderTransition(GuestRam& g, uint32_t r, uint32_t old, uint32_t sp, PopulationCallees& c);
// 0x800951B8, 61 instructions, frame 32: a downed rider's own window test.
bool Downed(GuestRam& g, uint32_t r, uint32_t off, uint32_t sp, PopulationCallees& c);
// 0x800950E8, 52 instructions, frame 32: Downed(r, 0) for every pool-0 bike's rider in mount state 3
// or 4, and a passenger's.
bool DownedRiderPass(GuestRam& g, uint32_t sp, PopulationCallees& c);

// ============================================================================ the dormant racers


// SLUS 0x8003A3CC, 39 instructions, frame 32: the road a route leaves node `node` by: the route
// record's (RouteLegFor(e[+0x1AC], node)) only exit, or one of several by `Rand() % count`; -1 with no
// route or no record.
int32_t RouteChoice(GuestRam& g, uint32_t e, uint32_t node);
// 0x80097518, 107 instructions, frame 40: a dormant bike moved `dist` along its route - across
// nodes by the route leg (RouteBind step 1 first on every stretch).
bool RouteWalk(GuestRam& g, uint32_t e, int32_t dist, uint32_t sp, PopulationCallees& c);
// 0x80095724, 73 instructions, frame 32: the rider pass's per-bike driver: a live bike raises
// *(0x800CCA80); a dormant one (not a parked cop, not frozen, not finished, rider seated, a route
// loaded) gets +0x39C = +0x1E0 = AiTargetSpeed, walks FixMul(dt, speed) and +0x144 = ProgressScalar.
bool DormantDrive(GuestRam& g, uint32_t e, int32_t dt, uint32_t sp, PopulationCallees& c);
constexpr uint32_t kPopLiveSeen = 0x800CCA80;

// ============================================================================ the spawner and the
// traffic. Every Rand draw is made where the original makes it, in its
// order: the shared stream is part of what the bench compares.
constexpr uint32_t kPopSpawnAcc    = 0x8005B318; // SpawnerPass's accumulator of dt
constexpr uint32_t kPopTrafficAcc  = 0x8005B210; // the traffic scheduler's accumulator
constexpr uint32_t kPopTrafficOn   = 0x8005ACC4; // the traffic switch
constexpr uint32_t kPopTrafficBlk  = 0x800D8710; // +4+4p cap, +12+4p interval, +0x1C/+0x20 distances,
                                                 // +0x24/+0x26/+0x28 the "ahead" percentages
constexpr uint32_t kPopCarGate     = 0x800CE578; // s16, the car spawner's own gate
constexpr uint32_t kPopPool3Ctrl   = 0x800CF650; // live, next, high; 16 x 512-byte cars from +16
constexpr uint32_t kPopLastLane    = 0x8005B358; // s8, the lane the last random-lane car drew
constexpr uint32_t kPopDensityTab  = 0x800524F0; // SLUS: 37 x (cap, interval) bytes by road id
constexpr uint32_t kPopClockTab    = 0x80052FAC; // SLUS: 9 words by bank and race type

// 0x8009F44C, 75 instructions: player p's traffic cap and interval from the per-road table.
void Density(GuestRam& g, uint32_t p);
// 0x8008DBA8, 9 instructions: pool 3's live count below player p's cap.
int32_t CapTest(GuestRam& g, uint32_t p);
// SLUS 0x8003A98C, 19 instructions: a slow player (+0x1E0 < 131) gets -dir; else Rand() & 1 flips it.
int32_t DirCoin(GuestRam& g, int32_t dir, uint32_t player);
// 0x8009CF1C, 54 instructions: the lateral centre of lane `sel` (s8) of `lanes` lanes `spacing` apart;
// |sel| == 2 on 3 or more lanes draws Rand.
int32_t LaneOffset(GuestRam& g, int32_t spacing, int32_t lanes, uint32_t sel);
// 0x8009E768, 77 instructions, frame 40: |the road's own offset| + |LaneOffset(width, lanes, car
// lane)|, or +-1.8 without road data. A car at a node asks SLUS 0x8003E1E8 (dir to its sp+16).
bool LaneLateral(GuestRam& g, uint32_t car, uint32_t sp, PopulationCallees& c, int32_t& v0);
// 0x8009FE90, 37 instructions, frame 24: 1 when a live car's cursor object is still resident; a
// dormant or stranded car is released (0x8008C000(car + 0xAC, 3)) and 0 returned.
bool CarCheck(GuestRam& g, uint32_t car, uint32_t sp, PopulationCallees& c, int32_t& v0);
// 0x8009AD48, 459 instructions, frame 112: a pool-3 car from a 68-byte placement record (`rec`) near
// player bike `player`; returns it (v0), or 0.
bool CarSpawn(GuestRam& g, uint32_t rec, uint32_t player, uint32_t sp, PopulationCallees& c, uint32_t& v0);
// 0x8009CFF4, 412 instructions, frame 160: the traffic scheduler, once per SpawnerPass round.
bool TrafficSched(GuestRam& g, int32_t acc, uint32_t sp, PopulationCallees& c);
// 0x8009B474, 19 instructions, frame 24: kind 3 -> TrafficSched(acc), kind 0 -> the police.
bool SchedDispatch(GuestRam& g, int32_t kind, int32_t acc, uint32_t sp, PopulationCallees& c);
// 0x8008CD88, 27 instructions, frame 24: SpawnerPass(dt).
bool SpawnerPass(GuestRam& g, int32_t dt, uint32_t sp, PopulationCallees& c);

} // namespace rr::sim
