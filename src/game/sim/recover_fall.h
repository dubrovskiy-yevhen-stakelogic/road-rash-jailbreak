#pragma once
// The rider leaving the bike and flying (see recover.h for the memory model and the call seam):
// the per-frame pass over the riders off their bikes, its tick, the five pose initialisers the
// tick and RiderLaunch place a thrown rider with, the rider's copy of its bike's road state, the
// settle against the ground and the launch lift.
//
// Every function returns false when a callee refused (the seam) or a load / store faulted; nothing it
// wrote is then to be trusted. `sp` is the stack pointer at the function's entry.
#include "game/sim/recover.h"

namespace rr::sim {

// ---------------------------------------------------------------------------- entry points
constexpr uint32_t kFallRiderOffPassFn = 0x8008F068; // frame 40
constexpr uint32_t kFallRiderOffTickFn = 0x8008F138; // frame 32, 716 B
constexpr uint32_t kFallPoseAFn        = 0x8008EE60; // frame 40, stance 38
constexpr uint32_t kFallPoseBFn        = 0x8008E818; // frame 56, stances 39 / 89
constexpr uint32_t kFallPoseCFn        = 0x8008E50C; // frame 56, stances 40 / 88
constexpr uint32_t kFallPoseDFn        = 0x8008EB88; // frame 56, everything else
constexpr uint32_t kFallPoseEFn        = 0x8008E044; // frame 80, stances 42 / 91
constexpr uint32_t kFallRiderSyncFn    = 0x8008DF74; // frame 32
constexpr uint32_t kFallRiderSettleFn  = 0x8009246C; // frame 96
constexpr uint32_t kFallLaunchLiftFn   = 0x8007E868; // frame 48, 508 B

// The callees these functions send through the seam.
constexpr uint32_t kFallRoadRebindFn   = 0x800374D4; // SLUS (R, 1)
constexpr uint32_t kFallBuildObbAltFn  = 0x8008BD2C; // (R)
constexpr uint32_t kFallGroundQueryFn  = 0x800A7BF8; // (R, 0, outPoint, outNormal, [sp+16] hint)
constexpr uint32_t kFallRouteBindFn    = 0x8003AF9C; // SLUS (R + 0xAC, 1, 0)

// The tick's pose table: 54 words, index stance - 38 (bounds `sltiu 54` at 0x8008F188).
constexpr uint32_t kFallPoseTable      = 0x8005B69C;

// 0x8008F068 RiderOffPass(dt): for every pool-0 slot 0..*(pool+12), 0x8008F138 on the slot's rider
// when its mount state +0x25C >=u 2, then (rider +0x23C bit 4) on the rider of the +0x358 bike.
bool RiderOffPass(GuestRam& g, int32_t dt, uint32_t sp, const BikeTables& t, RecoverCallees& c);

// 0x8008F138 RiderOffTick(R, dt): the per-frame tick of a rider off its bike. Gate
// lh(+0x140) != 0. Mount state 2: the pose initialiser through the table 0x8005B69C (dispatched on the
// table word; an unknown target refuses), the bike's +0x1F8 into +0x1F8, RiderSync. Any other mount
// state: the flight integration, the road re-bind and the settle. Both: BuildObbAlt, the +0x138
// restore, the heading angle +0x124 and its cos / sin +0x128 / +0x12C.
bool RiderOffTick(GuestRam& g, uint32_t R, int32_t dt, uint32_t sp, const BikeTables& t, RecoverCallees& c);

// The five pose initialisers (R, out): `out` 0 (RiderLaunch) or a word the half height +0x138 is
// saved into before it is lowered (B, D, E). A and C never read `out` (C's a1 is the tick's dt).
bool PoseA(GuestRam& g, uint32_t R, uint32_t out, uint32_t sp, const BikeTables& t);
bool PoseB(GuestRam& g, uint32_t R, uint32_t out, uint32_t sp, const BikeTables& t);
bool PoseC(GuestRam& g, uint32_t R, uint32_t out, uint32_t sp, const BikeTables& t);
bool PoseD(GuestRam& g, uint32_t R, uint32_t out, uint32_t sp, const BikeTables& t);
bool PoseE(GuestRam& g, uint32_t R, uint32_t out, uint32_t sp, const BikeTables& t);

// 0x8008DF74 RiderSync(R): the rider takes its bike's frame rows, road cursor, key, road block,
// +0x1EC / +0x1F0 / +0x1AC (the +0x358 bike's for a passenger, +0x23C bit 5), re-binds its route and
// takes +0x144 / +0x140.
bool RiderSync(GuestRam& g, uint32_t R, uint32_t sp, const BikeTables& t, RecoverCallees& c);

// 0x8009246C RiderSettle(R): the ground query (when +0x184 bit 0) or the road slice, the up vector
// +0x20A and the rider moved onto the ground plane along it (+0xB8, or +0x1F8 with +0x228 bit 30).
bool RiderSettle(GuestRam& g, uint32_t R, uint32_t sp, const BikeTables& t, RecoverCallees& c);

// 0x8007E868 LaunchLift(dir, speed, c, k): the launch direction `dir` (s16[3]) gets the vertical
// component c, or (k < speed) the larger, compared as s16, of c and the k / speed quotient; unless
// that equals dir[1] already, the horizontal part is rescaled to keep it a unit vector.
bool LaunchLift(GuestRam& g, uint32_t dir, int32_t speed, int32_t c, int32_t k, uint32_t sp, const BikeTables& t);

// ============================================================================ the rider pass's walk
// of the CRASHED-DOWN list - what stops a crashed bike sliding
//
// The region [0x8007DDF4, 0x8007E804) of the rider / engine pass RASHCDG 0x8007B840(dt) - single entry,
// nothing live at its exit but the pass's frame, no register carried in (every register it reads is
// written inside it first): the walk of the list 0x8005B378 (flagsC & 0x600 with bit 10 clear), the
// node kept at the pass's sp+184, the bike e = node - 1088, `next` (+4) re-read after the body.
//   flagsC bit 19 set (on the ground): the yaw rate +0x280 into the yaw +0x2C4 (clamped to +-1.5708,
//     +-2.7925 with a passenger and a positive yaw; the rate cleared at the bound); flagsC bit 20 kept
//     on the side of the ground frame's +0x328 / +0x322 against the up row +0x20A; with +0x1E8 = 0 (or
//     the side just decided) the ground frame +0x31C/+0x322/+0x328 rebuilt around the up row
//     (MulAdd16 / GTE OP / Normalize) and +0x1E8 := 0; flagsC bit 21 latched when at rest; no drag.
//   flagsC bit 19 clear (still falling): the impact timer +0x2D4/+0x2D8 steps +0x2B4 / +0x2C4 by
//     their rates +0x290 / +0x280, the roll rate +0x1E8 decays (x 0.9950), drag = FixMul(0x800D3964, dt).
//   Both: the roll +0x268 += FixMul(+0x1E8, dt) wrapped at +-2 pi turns the frame rows +0x322 / +0x328
//     (Blend16 of +0x26C / +0x274); the rows +0x1B0 = R(+0x2C4, +0x2B4) x the ground frame (three
//     MVMVA); unless flagsB bit 20 the drag and the gravity 0x9D087 on +0x1C8; the speed +0x1E0 /
//     +0x240 / |v|^2 +0x244 and the heading +0x1C2; the speed held at 8.94 when it drops through it;
//     +0x2F4 += dt; RowsFromUp.
// `sp` is the pass's own frame (entry sp - 248), the stack pointer the callee is called at; `dt` is
// the pass's argument (its home slot sp+248). `t` needs sincos, rsqrt and sqrt.
constexpr uint32_t kDownWalkEntry = 0x8007DDF4;
constexpr uint32_t kDownWalkExit = 0x8007E804;
constexpr uint32_t kDownListHead = 0x8005B378;
constexpr uint32_t kDownDragWord = 0x800D3964;     // s32, read at 0x8007E1F4
constexpr uint32_t kFallRowsFromUpFn = 0x8007EA64; // (e), through the seam
bool RiderPassDownWalk(GuestRam& g, int32_t dt, uint32_t sp, const BikeTables& t, RecoverCallees& c);

} // namespace rr::sim
