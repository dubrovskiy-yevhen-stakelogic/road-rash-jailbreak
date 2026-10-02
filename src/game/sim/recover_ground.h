#pragma once
// The rider on the ground (recover.h for the conventions): lying, tumbling, getting up and walking.
//
//   RASHCDG 0x80097BCC  RiderOnGround      the per-frame body of a rider off the bike: the wall /
//                                          junction bounce (flag 0x2, the normal into +0x26E), mount
//                                          +0x25C 3 (tumbling: GroundSlide, GroundGetUp) or not (the
//                                          heading turned with the GTE OP, GroundWalkControl when the
//                                          stance's kind is 8, GroundWalkRate), SetOp18 on the bike
//                                          when the get-up / walk says so, and the pose rebuilt
//   RASHCDG 0x8008F754  GroundSlide        the tumbling slide: the velocity +0x1C8 damped and
//                                          integrated, its length +0x1E0, the body frame rebuilt
//   RASHCDG 0x8008FD5C  GroundGetUp        the get-up decision over the lying stances 44..55 (a jump
//                                          table); v0 != 0: the rider is up
//   RASHCDG 0x8009926C  GroundWalkControl  a PLAYER's pad (0x800CE540 + 8p) steering the walking
//                                          rider: the bits 0x80/0x100/0x200/0x400 of +0x228, the
//                                          speed +0x1E0 and the turn rate +0x1E8
//   RASHCDG 0x800986D0  GroundWalkRate     the walk stance (69..73) chosen from the speed and the
//                                          walk cycle's clip rate (+0x18 of the animation object);
//                                          v0 != 0: the walk is over
//   RASHCDG 0x8009A038  SetOp18            the bike's top command set to {18, handle, 0, stamp}, the
//                                          rider's rows turned, the stance event 69/70, the view
//                                          flags of a player's bike
#include <cstdint>

#include "game/sim/recover.h"

namespace rr::sim {

constexpr uint32_t kRiderOnGroundFn     = 0x80097BCC;
constexpr uint32_t kGroundSlideFn       = 0x8008F754;
constexpr uint32_t kGroundGetUpFn       = 0x8008FD5C;
constexpr uint32_t kGroundWalkControlFn = 0x8009926C;
constexpr uint32_t kGroundWalkRateFn    = 0x800986D0;
constexpr uint32_t kSetOp18Fn           = 0x8009A038;

// The callees the ports below reach through RecoverCallees (the bench declares these).
constexpr uint32_t kGrJunctionMarginFn = 0x8003E338; // SLUS (R) -> a junction record or 0
constexpr uint32_t kGrEffectBurstFn    = 0x80027778; // SLUS EffectBurst(e, kind, life, tag)
constexpr uint32_t kGrClipDoneFn       = 0x8005BE58; // ClipDone(anim)                  anim.h
constexpr uint32_t kGrRangedStartFn    = 0x8005C018; // RangedStart(anim, clip, a2, a3, [3 on stack])
constexpr uint32_t kGrSetRiderStateFn  = 0x800C2FF4; // SetRiderState(ev, r, p)         stance.h
constexpr uint32_t kGrStanceEventFn    = 0x800C4550; // StanceEvent(ev, r, p)           stance.h
constexpr uint32_t kGrFollowStanceFn   = 0x800C45D8; // FollowStance(r, s32 *p)         present.h
constexpr uint32_t kGrCameraResetFn    = 0x8008AAB0; // CameraResetFlags(view)          camera.h

// Frames (`addiu sp,sp,-N` at each entry).
constexpr uint32_t kRiderOnGroundFrame     = 88;
constexpr uint32_t kGroundSlideFrame       = 96;
constexpr uint32_t kGroundGetUpFrame       = 56;
constexpr uint32_t kGroundWalkControlFrame = 64;
constexpr uint32_t kGroundWalkRateFrame    = 32;
constexpr uint32_t kSetOp18Frame           = 32;

// void 0x80097BCC(Rider *R, s32 dt)
bool RiderOnGround(GuestRam& g, uint32_t R, int32_t dt, uint32_t sp, const BikeTables& t, RecoverCallees& c);
// void 0x8008F754(Rider *R, s32 dt, s32 *timer) - the caller passes R+0x258.
bool GroundSlide(GuestRam& g, uint32_t R, int32_t dt, uint32_t timer, uint32_t sp, const BikeTables& t,
                 RecoverCallees& c);
// s32 0x8008FD5C(Rider *R)
bool GroundGetUp(GuestRam& g, uint32_t R, uint32_t sp, const BikeTables& t, RecoverCallees& c, uint32_t& v0);
// void 0x8009926C(Rider *R, s32 dt)
bool GroundWalkControl(GuestRam& g, uint32_t R, int32_t dt, uint32_t sp, const BikeTables& t, RecoverCallees& c);
// s32 0x800986D0(Rider *R) - one argument (a1..a3 are scratch in its body).
bool GroundWalkRate(GuestRam& g, uint32_t R, uint32_t sp, const BikeTables& t, RecoverCallees& c, uint32_t& v0);
// void 0x8009A038(Bike *B)
bool SetOp18(GuestRam& g, uint32_t B, uint32_t sp, const BikeTables& t, RecoverCallees& c);

} // namespace rr::sim
