#pragma once
// The race frame's two top-level passes and the rider / engine pass, WHOLE: RaceTick's third and fifth
// children and the 12328-byte function the fifth one starts with. RASHCDG
// SHA-1 cfe43a7786759f2cb9c57751cf99e84d1074782c, read out of our own disassembly.
//
//   WorldBikePass   0x8008AC80(dt), 104 bytes, frame 24: TrafficPass 0x8009A298, the per-bike step
//                   0x80075EE0, the rider-off pass 0x8008F068, PropPass 0x800A2898, PedPass 0x800CB304 while
//                   *(0x8005B254) != 0 (read after PropPass), Pool5Pass 0x8009ACA4, VolumePass 0x8009AB60.
//                   The first two and the three after the step get dt in a0 (the traffic pass's a0 is the
//                   pass's own a0, never reloaded); the last two take nothing.
//   RiderEnginePass 0x8008ACE8(dt), 80 bytes, frame 24: the rider pass 0x8007B840, PropAnimPass 0x800A2A64,
//                   PedRelease 0x800CB4F8 while *(0x8005B254) != 0, HazardPass 0x800A13C4 - all four (dt).
//   RiderPass       0x8007B840(dt), frame 248, dt kept at its home slot sp+248 and the pool-0 high index at
//                   sp+164 (the prologue's two stores the first region reads):
//                     [0x8007B894, 0x8007C23C) the first pool loop (skipped when the high index is < 0)
//                     [0x8007C23C, 0x8007C9A8) the walk of the thrown list 0x8005B2D8
//                     the engine 0x80079B20 on 0x8005B2D8, 0x8005B298, 0x8005B350
//                     [0x8007C9DC, 0x8007DCD4) the walk of the class list 0x8005B350 (RiderPassClassWalk)
//                     ImpactStatePass 0x80078DB4 on 0x8005B350 and 0x8005B2D8
//                     [0x8007DCEC, 0x8007DDD4) per pool-0 slot: 0x80071BCC(e, dt) on flagsC & 0x08001800, then
//                       0x8008F404(R, dt) on the rider when it is off (mount >= 2) and live, and the same on a
//                       two-rider bike's passenger (count read once, the stride re-read per slot)
//                     the heading writer 0x8007AC04 on 0x8005B298 and 0x8005B350
//                     [0x8007DDF4, 0x8007E804) the walk of the crashed-down list 0x8005B378
//                     [0x8007E804, 0x8007E838) DormantDrive 0x80095724(e, dt) over 0x8005B270, next read after
//
// Every `sp` below is the stack pointer the callee is called at (the pass's own frame).
#include <cstdint>

#include "game/sim/integrator.h"
#include "game/sim/road_query.h"

namespace rr::sim {

constexpr uint32_t kWorldBikePassFn = 0x8008AC80;
constexpr uint32_t kRiderEnginePassFn = 0x8008ACE8;
constexpr uint32_t kTopPassFrame = 24;              // `addiu sp,sp,-24` at both
constexpr uint32_t kPedSwitchWord = 0x8005B254;     // EnterRace's ((game_state+4 & 0x18) == 0)
namespace passes { // the two passes' children
constexpr uint32_t kTrafficPassFn = 0x8009A298, kBikeStepPassFn = 0x80075EE0, kRiderOffPassFn = 0x8008F068,
                   kPropPassFn = 0x800A2898, kPedPassFn = 0x800CB304, kPool5PassFn = 0x8009ACA4,
                   kVolumePassFn = 0x8009AB60;
constexpr uint32_t kRiderPassWholeFn = 0x8007B840, kPropAnimPassFn = 0x800A2A64, kPedReleaseFn = 0x800CB4F8,
                   kHazardPassFn = 0x800A13C4;
} // namespace passes

struct WorldBikePassCallees {
    virtual ~WorldBikePassCallees() = default;
    virtual bool TrafficPass(int32_t dt, uint32_t sp) = 0;   // RASHCDG 0x8009A298
    virtual bool BikeStep(int32_t dt, uint32_t sp) = 0;      // RASHCDG 0x80075EE0
    virtual bool RiderOffPass(int32_t dt, uint32_t sp) = 0;  // RASHCDG 0x8008F068
    virtual bool PropPass(int32_t dt, uint32_t sp) = 0;      // RASHCDG 0x800A2898
    virtual bool PedPass(int32_t dt, uint32_t sp) = 0;       // RASHCDG 0x800CB304
    virtual bool Pool5Pass(uint32_t sp) = 0;                 // RASHCDG 0x8009ACA4
    virtual bool VolumePass(uint32_t sp) = 0;                // RASHCDG 0x8009AB60
};
// `entrySp` is the stack pointer RaceTick calls the pass at. False: a callee returned false.
bool WorldBikePass(GuestRam& g, int32_t dt, uint32_t entrySp, WorldBikePassCallees& c);

struct RiderEnginePassCallees {
    virtual ~RiderEnginePassCallees() = default;
    virtual bool RiderPass(int32_t dt, uint32_t sp) = 0;     // RASHCDG 0x8007B840
    virtual bool PropAnimPass(int32_t dt, uint32_t sp) = 0;  // RASHCDG 0x800A2A64
    virtual bool PedRelease(int32_t dt, uint32_t sp) = 0;    // RASHCDG 0x800CB4F8
    virtual bool HazardPass(int32_t dt, uint32_t sp) = 0;    // RASHCDG 0x800A13C4
};
bool RiderEnginePass(GuestRam& g, int32_t dt, uint32_t entrySp, RiderEnginePassCallees& c);

// ---- the rider pass 0x8007B840. The four regions are run through the callees (each is a ported
// region function of its own: bike_react.h RiderPassPoolLoop, collision.h RiderPassThrownWalk,
// RiderPassClassWalk below, recover_fall.h RiderPassDownWalk) so that a caller supplies each one's own
// callees; the glue between them - the prologue's two stores, the order, the rider loop and the dormant
// loop - is here.
struct RiderPassCallees {
    virtual ~RiderPassCallees() = default;
    virtual bool PoolLoop(int32_t dt, uint32_t sp) = 0;                  // [0x8007B894, 0x8007C23C)
    virtual bool ThrownWalk(int32_t dt, uint32_t sp) = 0;                // [0x8007C23C, 0x8007C9A8)
    virtual bool Engine(uint32_t list, int32_t dt, uint32_t sp) = 0;     // 0x80079B20
    virtual bool ClassWalk(int32_t dt, uint32_t sp) = 0;                 // [0x8007C9DC, 0x8007DCD4)
    virtual bool ImpactState(uint32_t list, uint32_t sp) = 0;            // 0x80078DB4
    virtual bool ListMigrate(uint32_t e, int32_t dt, uint32_t sp) = 0;   // 0x80071BCC
    virtual bool RiderGround(uint32_t r, int32_t dt, uint32_t sp) = 0;   // 0x8008F404
    virtual bool Heading(uint32_t list, int32_t dt, uint32_t sp) = 0;    // 0x8007AC04
    virtual bool DownWalk(int32_t dt, uint32_t sp) = 0;                  // [0x8007DDF4, 0x8007E804)
    virtual bool DormantDrive(uint32_t e, int32_t dt, uint32_t sp) = 0;  // 0x80095724
};
constexpr uint32_t kRiderPassWholeFrame = 248;
bool RiderPass(GuestRam& g, int32_t dt, uint32_t entrySp, RiderPassCallees& c);

// ---- [0x8007C9DC, 0x8007DCD4): the rider pass's walk of the CLASS list 0x8005B350 (flagsC & 0x1FF: a bike
// knocked by a hit class), after the engine, before ImpactStatePass. Single entry, no
// register carried out (every register the code after it reads is written there first); the node is kept
// at the pass's sp+176 and next (+4) re-read after the body, sp+180 = the rider's +0x23C bit 4 (a
// passenger rides). Per bike e (R = e+0x354):
//   flagsC bit 8 (the fall clip): while R's clip +0x21C is not done / R+0x220 set, the clip's progress sets
//     the lean +0x28C and +0x304 and zeroes the speed and gear; once R is off (mount >= 2) the bike slides:
//     +0x304 relaxes, the ground-contact tests of the blended frame (sp+24) against +0x334, the velocity
//     +0x1C8 from +0x1E0 along +0x1C2 plus the blended push (+0x2D4 / +0x2FC), |v| into +0x244 / +0x1E0 /
//     +0x240 and the deceleration +0x1E4, the heading +0x1C2, the side row +0x32E (GTE OP, Normalize) and
//     the lean +0x2A4 / +0x2B0 from its angle (RatAtan2);
//   flagsC bit 8 clear: the knock's push +0x2F0 (stat +0x144 x +0x244 x the surface table 0x800D38E0), the
//     speed floor 2.2352 and the uphill clamp (0x80074FB4); then flagsB &= ~0x3000 and
//     bit 7: the wobble (+0x2D4 timer, +0x1E8 / +0x27C sine blend) or its end (flagsC |= 0x200840);
//     else the lean step (+0x294 / +0x290 / +0x28C clamped to +-1.5708), the yaw step to +0x284, the roll
//       rate +0x248 into +0x1E8 (clamped by stat +0xE8), then bit 6 (the tip: a sound at 90 degrees - the
//       root counter and PlaySound3D) or bit 5 (the lean-back, the heading lift from Asin, flagsC 0x40A00);
//     the rider's (and passenger's) +0x228 bit 15 when the lean passes its limits.
// `t` needs sincos, asin, atan, rsqrt and sqrt.
struct ClassWalkCallees {
    virtual ~ClassWalkCallees() = default;
    virtual bool ClipDone(uint32_t anim, uint32_t sp, uint32_t& v0) = 0;                   // RASHCDG 0x8005BE58
    virtual bool GetRCnt(uint32_t id, uint32_t sp, uint32_t& v0) = 0;                      // SLUS 0x80043F00
    virtual bool PlaySound3D(int32_t x, int32_t z, int32_t id, int32_t bank, uint32_t sp) = 0; // SLUS 0x80017BA0
};
constexpr uint32_t kClassWalkEntry = 0x8007C9DC;
constexpr uint32_t kClassWalkExit = 0x8007DCD4;
constexpr uint32_t kClassListHead = 0x8005B350;
constexpr uint32_t kClassSurfaceTable = 0x800D38E0; // s32 per surface, indexed by the signed byte +0x216
bool RiderPassClassWalk(GuestRam& g, int32_t dt, uint32_t sp, const BikeTables& t, ClassWalkCallees& c);

// ============================================================================ the frame's clock (SLUS)
// SLUS_010.53 SHA-1 67ed165a2c517d4e6106fb0dfa324d66dd9a76f1. game_state (*(0x8005B2F8)) +0x0C is the tick
// clock (1/300 s) the VSync callback SLUS 0x8001B700 advances by 5 per vertical blank; +0x14 its value at
// the last pad poll, +0x18 the frame's ticks RaceStep reads, +0x1C the clamped copy RaceStep writes, +0x20
// the ticks since the last FrameDelta.
//
// The region [0x8001CD00, 0x8001CD7C) of the pad poll SLUS 0x8001CB3C (PollPads, 29 instructions, after
// its pause test; single entry - both paths into it reach 0x8001CD00 with game_state in v1 - single exit
// 0x8001CD7C, live out s0): game_state+0x03 (EnterRace's "first race frame" byte) set -> game_state+0x00 =
// 1 (racing; the store at 0x8001CD14) and s0 = 1; then, in state 1, +0x18 = +0x0C - +0x14 when s0 is 0,
// else 15 / 15 into +0x18 / +0x1C when game_state+0x02 is set, and 0 / 0 in every other case.
// `paused` is s0 on entry (the pause test's verdict); the return value is s0 on exit.
constexpr uint32_t kPadPollFn = 0x8001CB3C;
constexpr uint32_t kPadClockEntry = 0x8001CD00, kPadClockExit = 0x8001CD7C;
uint32_t PadPollClock(GuestRam& g, uint32_t paused);

// SLUS 0x8001C428 FrameDelta(), 28 instructions: the main loop's per-frame bookkeeping (every frame but
// the first race frame): gp+256 (0x8005AD8C) counts frames and at 60 wraps to 0 and zeroes game_state
// +0x64; game_state+0x20 = +0x0C - gp+1996 (0x8005B458) - 0 while the state byte is 3 or 4 - and
// gp+1996 = +0x0C.
constexpr uint32_t kFrameDeltaFn = 0x8001C428;
constexpr uint32_t kFrameDeltaCount = 0x8005AD8C, kFrameDeltaPrev = 0x8005B458;
void FrameDelta(GuestRam& g);

// SLUS 0x8003F708 ResultsPrepare(), 116 instructions, with its two callees ported into it: 0x8003F8D8
// (the unplaced rider furthest ahead: the smallest progress +0x144 - 0 counts as none - among the bikes
// that are a player or not police, have no result code (+0x27 < 248) and are not in the finishing-order
// table) and 0x8003F9D8 (is the bike's handle in the table 0x800D5DA8, 18 x 16 bytes). RaceDirector calls
// it when every player is done: the next free table place after the last recorded one (whose stamp +12
// is the clock the rest start from; the race clock game_state+0x10 when the table is empty) is given to
// each remaining rider in progress order - place into +0x27, the stamp +0x28 = that clock + (+0x144 >> 4)
// * 300 / ((stat +0xE0 >> 8) * 0xE666 >> 16) (R3000 `div`, no zero check), unless the quotient is
// negative - and recorded with RecordFinish SLUS 0x8003F680(handle, 0); then every table entry's place
// and stamp are written back to its rider.
constexpr uint32_t kResultsPrepareFn = 0x8003F708, kRecordFinishFn = 0x8003F680;
constexpr uint32_t kFinishOrderTable = 0x800D5D98;
struct ResultsCallees {
    virtual ~ResultsCallees() = default;
    virtual bool RecordFinish(uint32_t handle, int32_t flag) = 0; // SLUS 0x8003F680, PORTED (race.h)
};
bool ResultsPrepare(GuestRam& g, ResultsCallees& c);

} // namespace rr::sim
