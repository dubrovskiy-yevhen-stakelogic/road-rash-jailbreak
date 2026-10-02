#pragma once
// The walk back to the bike and the re-seat (recover.h for the conventions):
//
//   RASHCDG 0x80092E04  RiderRecover   AI op 18: the downed rider walks back, or the bike is put back
//                                      on the rider's road and re-seated
//   RASHCDG 0x80098A50  WalkTarget     the point beside the bike the rider walks to; close enough ->
//                                      the re-seat
//   RASHCDG 0x80098F2C  WalkPivot      the pivot of a turn round the bike (the +0x250/+0x258 frame)
//   RASHCDG 0x80099710  WalkTurn       the turn round the pivot; lined up -> the re-seat
//   RASHCDG 0x8009989C  WalkStep       the walk: speed, heading turned toward the target
//   RASHCDG 0x80093BE0  AngleBetween   the angle of v in the (a, b) plane, 4096 a turn
//   RASHCDG 0x8009277C  ReSeat         the animated climb: the bike's animation object, the climb
//                                      stance, the command {1, 224} with the rider's bit 6 (0x40)
//   RASHCDG 0x80092AD4  ClimbDone      op 1 with bit 6: the climb's stance done -> pop, reset, aim
//   RASHCDG 0x80096564  ReFace         the bike re-faced along its road leg (Remount's and op 18's)
//   RASHCDG 0x8008CC94  FreeFarRider   no free animation object: release the farthest pool-2 object
//   SLUS    0x8003FB34  AxisRotation   the rotation matrix of an angle about a unit axis
//   SLUS    0x8002EFF4  VecMat         v x M (row vector times matrix), each product >> 12
#include <cstdint>

#include "game/sim/recover.h"

namespace rr::sim {

constexpr uint32_t kRiderRecoverFn = 0x80092E04;
constexpr uint32_t kWalkTargetFn   = 0x80098A50;
constexpr uint32_t kWalkPivotFn    = 0x80098F2C;
constexpr uint32_t kWalkTurnFn     = 0x80099710;
constexpr uint32_t kWalkStepFn     = 0x8009989C;
constexpr uint32_t kAngleBetweenFn = 0x80093BE0;
constexpr uint32_t kReSeatFn       = 0x8009277C;
constexpr uint32_t kClimbDoneFn    = 0x80092AD4;
constexpr uint32_t kReFaceFn       = 0x80096564;
constexpr uint32_t kFreeFarRiderFn = 0x8008CC94;
constexpr uint32_t kAxisRotationFn = 0x8003FB34; // SLUS
constexpr uint32_t kVecMatFn       = 0x8002EFF4; // SLUS

// The callees the ports below reach through RecoverCallees (the bench declares these).
constexpr uint32_t kRcRemountFn      = 0x800903F4; // Remount(B, fromRoad)            crash.h
constexpr uint32_t kRcEndRaceFn      = 0x80092C7C; // EndRace(e, how)                 race.h
constexpr uint32_t kRcSpringResetFn  = 0x80086AF8; // CameraSpringReset(view)          camera.h
constexpr uint32_t kRcJailbreakFn    = 0x800CA05C; // (a0) - Jailbreak phase 2 only, NOT ported
constexpr uint32_t kRcRouteBindFn    = 0x8003AF9C; // SLUS RouteBind(p, step, x)       road_runtime.h
constexpr uint32_t kRcRoadPositionFn = 0x8003662C; // SLUS RoadPosition(heading, cursor, out)
constexpr uint32_t kRcZonesFn        = 0x8003A9D8; // SLUS RoadsideZones(e)
constexpr uint32_t kRcResetBikeFn    = 0x8002090C; // SLUS ResetBikeState(e)           spine.h
constexpr uint32_t kRcReleaseObjFn   = 0x80095AEC; // ReleaseRiderObject(r)           traffic_bind.h
constexpr uint32_t kRcViewSlotFn     = 0x80012884; // SLUS ViewSlot(desc, r)          population.h
constexpr uint32_t kRcAttachFn       = 0x80012838; // SLUS Attach(b, r, kind, seat)   population.h
constexpr uint32_t kRcBankSwitchFn   = 0x80012858; // SLUS BankSwitch(a, bank)        anim.h
constexpr uint32_t kRcStanceEventFn  = 0x800C4550; // StanceEvent(ev, r, p)           stance.h
constexpr uint32_t kRcVoiceFn        = 0x80018440; // SLUS the rider-off voice (h, mode), PORTED (takedown.h)
constexpr uint32_t kRcCamTargetFn    = 0x800235B0; // SLUS CamTarget(h, b)
constexpr uint32_t kRcPopCommandFn   = 0x800BC8DC; // AiPopCommand(e)                 ai.h
constexpr uint32_t kRcPoolReleaseFn  = 0x8008C000; // PoolRelease(h, pool)            traffic_bind.h

// 0x80093BE0, frame 32: x = DotLcm(a, v), y = DotLcm(b, v); |x| < |y| ? +-(1024 - Asin(x)) (the sign
// of y) : from Asin(y) by the sign of x. False only where Asin refuses.
bool AngleBetween(GuestRam& g, uint32_t v, uint32_t a, uint32_t b, const BikeTables& t, int32_t& v0);
// SLUS 0x8003FB34, frame 112: out (s16[9]) = the rotation by `ang` (4096 a turn) about the unit axis
// `axis` (s16[3]); the 16.16 matrix is kept in the frame at F+16..F+48 and stored >> 4.
void AxisRotation(GuestRam& g, uint32_t axis, int32_t ang, uint32_t out, uint32_t sp, const BikeTables& t);
// SLUS 0x8002EFF4, a leaf: out[j] = sum_i (v[i] * m[3i + j]) >> 12, each term shifted, out[j] stored
// before out[j+1] is read (the original's order, for an `out` that aliases).
void VecMat(GuestRam& g, uint32_t v, uint32_t m, uint32_t out);

// 0x80098A50, frame 104: v0 = 0 when the re-seat was started (and ReSeat ran), else 1.
bool WalkTarget(GuestRam& g, uint32_t R, uint32_t B, uint32_t dirA, uint32_t dirB, uint32_t sp,
                const BikeTables& t, RecoverCallees& c, uint32_t& v0);
// 0x80098F2C, frame 72; `flag` is the fifth argument (sp+16).
bool WalkPivot(GuestRam& g, uint32_t R, uint32_t dirA, uint32_t dirB, int32_t sq, int32_t flag, uint32_t sp,
               const BikeTables& t);
// 0x80099710, frame 40.
bool WalkTurn(GuestRam& g, uint32_t R, uint32_t dirA, uint32_t dirB, int32_t dt, uint32_t sp,
              const BikeTables& t, RecoverCallees& c);
// 0x8009989C, frame 80; `d` (s32[3], halved in place) is the caller's buffer, `flag` and `dt` the fifth
// and sixth arguments (sp+16, sp+20).
bool WalkStep(GuestRam& g, uint32_t R, uint32_t B, uint32_t d, int32_t sq, int32_t flag, int32_t dt, uint32_t sp,
              const BikeTables& t);
// 0x80092E04, frame 104: the op-18 handler on bike `e` (a passenger's: its partner's bike).
bool RiderRecover(GuestRam& g, uint32_t e, int32_t dt, uint32_t sp, const BikeTables& t, RecoverCallees& c);
// 0x8009277C, frame 40.
bool ReSeat(GuestRam& g, uint32_t B, uint32_t sp, RecoverCallees& c);
// 0x80092AD4, frame 32.
bool ClimbDone(GuestRam& g, uint32_t B, uint32_t sp, const BikeTables& t, RecoverCallees& c);
// 0x80096564, frame 56.
bool ReFace(GuestRam& g, uint32_t B, uint32_t sp, const BikeTables& t, RecoverCallees& c);
// 0x8008CC94, frame 24: v0 = the released entity's +0x21C, or 0.
bool FreeFarRider(GuestRam& g, uint32_t sp, RecoverCallees& c, uint32_t& v0);

} // namespace rr::sim
