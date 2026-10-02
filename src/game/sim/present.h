#pragma once
// The rider PRESENTATION pass and what it drives, transcribed from our
// own disassembly of RASHCDG.BIN (SHA-1 cfe43a7786759f2cb9c57751cf99e84d1074782c, loaded at
// 0x8005B5E8) and accepted only by `rrverify phys` rows (tools\rrverify\rows_pose.inc):
//
//   0x80090814 PresentationPass(dt)  per bike of pool 0 (race tick): the flash timer, the passenger
//              copy, the knock-off request, then per rider: leaving the bike (mount 2) -> the fall
//              scrubbers / ClipDone -> RiderLaunch; riding -> the follow-up dispatcher
//   0x800C31CC FallScrubLean   0x800C341C FallScrubLift   0x800C3630 FallScrubPitch
//              the fall clips' clock driven by the bike's lean / lift / pitch (frozen, bit 3)
//   0x800C5078 FollowUp        the riding stance machine: the queue 0x800C47CC, the seat /
//              kick-start 0x800C4860, 0x800C4B30, the ride 0x800C4BA0 (-> the LEAN scrubber
//              0x800C3950, the brake / tuck / look-around stances), the near-rival 0x800C4E18, and
//              when the clip is done the next stance of 0x800C45D8
//   0x800C3950 RidingLean      the riding lean: the lean clip's key scrubbed from the bike's steer
//              input +0x2A0, slewed at most 2 keys a pass; its stance switch and LoopStart
//
// Callees on the seam (`PresentSeams`): the stance event 0x800C4550, RiderLaunch 0x80091468 and
// RiderKnockOff 0x80090D84 (all three ported: stance.h - the product passes the ported layers, the
// bench the oracle) and SLUS 0x800273EC (ObjectEffect, ported in weapon.h). Everything else is native:
// ClipDone / LoopStart / RangedStart (anim.h), SetRiderState (stance.h), Rand, FixMul, CopyHalfwords,
// Pick 0x8008B428 (fight.h), AiProject 0x800B6AAC (ai.h). Jump tables (0x8005BBA0, 0x8005BA98) are
// READ from the image and dispatched on their target words; an unknown target refuses.
#include <cstdint>

#include "game/sim/anim.h"
#include "game/sim/road_query.h"
#include "game/sim/stance.h"

namespace rr::sim {

constexpr uint32_t kPresentPassFn     = 0x80090814;
constexpr uint32_t kFallScrubLeanFn   = 0x800C31CC;
constexpr uint32_t kFallScrubLiftFn   = 0x800C341C;
constexpr uint32_t kFallScrubPitchFn  = 0x800C3630;
constexpr uint32_t kRidingLeanFn      = 0x800C3950;
constexpr uint32_t kFollowStanceFn    = 0x800C45D8;
constexpr uint32_t kFollowQueueFn     = 0x800C47CC;
constexpr uint32_t kFollowSeatFn      = 0x800C4860;
constexpr uint32_t kFollowGearFn      = 0x800C4B30;
constexpr uint32_t kFollowRideFn      = 0x800C4BA0;
constexpr uint32_t kFollowNearFn      = 0x800C4E18;
constexpr uint32_t kFollowUpFn        = 0x800C5078;
// kRiderLaunchFn 0x80091468 and kRiderKnockOffFn 0x80090D84: stance.h
constexpr uint32_t kObjectSoundFn     = 0x800273EC; // SLUS, PORTED (weapon.h ObjectEffect)

struct PresentSeams : StanceSeams {
    virtual uint32_t StanceEvent(uint32_t ev, uint32_t r, uint32_t p) = 0;       // 0x800C4550
    virtual uint32_t RiderLaunch(uint32_t r) = 0;                               // 0x80091468
    virtual uint32_t RiderKnockOff(uint32_t r) = 0;                             // 0x80090D84
    virtual uint32_t ObjectSound(uint32_t r, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4) = 0; // SLUS 0x800273EC
};

class PresentLayer {
public:
    PresentLayer(GuestRam& g, PresentSeams& seams) : g_(g), seams_(seams), anim_(g, seams) {}
    bool Failed() const { return refused_ || anim_.Failed() || g_.Faulted(); }

    uint32_t Pass(int32_t dt);                    // 0x80090814
    uint32_t FallScrubLean(uint32_t r);           // 0x800C31CC
    uint32_t FallScrubLift(uint32_t r);           // 0x800C341C
    uint32_t FallScrubPitch(uint32_t r);          // 0x800C3630
    uint32_t RidingLean(uint32_t r);              // 0x800C3950
    uint32_t FollowStance(uint32_t r, uint32_t out); // 0x800C45D8, `out` a guest s32
    uint32_t FollowQueue(uint32_t r);             // 0x800C47CC
    uint32_t FollowSeat(uint32_t r);              // 0x800C4860
    uint32_t FollowGear(uint32_t r);              // 0x800C4B30
    uint32_t FollowRide(uint32_t r);              // 0x800C4BA0
    uint32_t FollowNear(uint32_t r);              // 0x800C4E18
    uint32_t FollowUp(uint32_t r);                // 0x800C5078

private:
    int32_t LastKey(uint32_t a);                  // (s16)(keys - 1) of the clip the op at the pc names
    void Scrub(uint32_t a, int32_t t16);          // frame := t >> 16, sub := rate (t - frame << 16) >> 16
    uint32_t FollowStanceHost(uint32_t r, uint32_t& out);

    GuestRam& g_;
    PresentSeams& seams_;
    AnimMachine anim_;
    bool refused_ = false;
};

} // namespace rr::sim
