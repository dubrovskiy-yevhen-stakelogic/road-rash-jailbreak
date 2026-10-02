#pragma once
// The rider's stance event `RASHCDG 0x800C4550` - the gate `0x800C3E9C` and its commit
// (StanceLeave `0x800C4500`, StanceEnter `0x800C4454`, SetRiderState `0x800C2FF4`) plus the path
// lookup `StancePath 0x800C37B0` - on top of the animation machine's clock (anim.h). Transcribed
// from our own disassembly of the player's own images:
//
//   RASHCDG.BIN  SHA-1 cfe43a7786759f2cb9c57751cf99e84d1074782c, loaded at 0x8005B5E8
//   SLUS_010.53  SHA-1 67ed165a2c517d4e6106fb0dfa324d66dd9a76f1, text at 0x80010000
//
// Accepted only by `rrverify phys` rows (tools\rrverify\rows_anim.inc).
//
// NOT here, supplied by the caller through `StanceSeams` (the bench from the oracle):
//   * the pose side of the machine (AnimPoseSeam: TransitionCapture, ApplyFrame);
//   * the two combat children: `0x800BFD24` (leaving a category-3 stance: 0x80095AEC and FightEnd
//     0x800C1014, which can recurse into this very layer through 0x800BC8DC) and `0x800BFC5C`
//     (entering one: 0x800958F0 and the overlay clip 0x800C2F84).
//
// Memory model: anim.h's. The gate's `p` lives in its caller's frame in the original (StanceEvent
// passes the address of its own sp+16); `Gate` takes a guest address for it so that the function
// can be benched alone, and `Event` keeps it as a local, which is what the original's frame word is.
#include <cstdint>

#include "game/sim/anim.h"
#include "game/sim/road_query.h"

namespace rr::sim {

constexpr uint32_t kStanceEventFn   = 0x800C4550;
constexpr uint32_t kStanceGateFn    = 0x800C3E9C;
constexpr uint32_t kStanceLeaveFn   = 0x800C4500;
constexpr uint32_t kStanceEnterFn   = 0x800C4454;
constexpr uint32_t kSetRiderStateFn = 0x800C2FF4;
constexpr uint32_t kStancePathFn    = 0x800C37B0;
constexpr uint32_t kCombatLeaveFn   = 0x800BFD24; // NOT ported (seam)
constexpr uint32_t kCombatEnterFn   = 0x800BFC5C; // NOT ported (seam)

constexpr uint32_t kStanceTable     = 0x800541D4; // SLUS: 8 bytes per stance {w0: bank 0..3, clip 4..15; u16 cat; u32 mask}
constexpr uint32_t kStanceEventList = 0x8005B3E4; // -> u32[stance], the `ex` of every play call (0 = none)
constexpr uint32_t kStancePathKeys  = 0x800CCBAC; // StancePath's T: {u8 from, u8 offset} pairs
constexpr uint32_t kStancePathData  = 0x800CCBBC; // StancePath's U
constexpr uint32_t kStanceNone      = 224;

struct StanceSeams : AnimPoseSeam {
    // RASHCDG 0x800BFD24(r, cur, ev, p); returns its v0.
    virtual uint32_t CombatLeave(uint32_t r, uint32_t cur, uint32_t ev, uint32_t p) = 0;
    // RASHCDG 0x800BFC5C(r, cur, ev, p); returns its v0.
    virtual uint32_t CombatEnter(uint32_t r, uint32_t cur, uint32_t ev, uint32_t p) = 0;
};

class StanceLayer {
public:
    StanceLayer(GuestRam& g, StanceSeams& seams) : g_(g), seams_(seams), anim_(g, seams) {}

    AnimMachine& anim() { return anim_; }
    bool Failed() const { return refused_ || anim_.Failed(); }

    uint32_t Event(uint32_t ev, uint32_t r, uint32_t p);          // 0x800C4550
    uint32_t Gate(uint32_t ev, uint32_t r, uint32_t pp);          // 0x800C3E9C, `pp` a guest address
    uint32_t Leave(uint32_t ev, uint32_t r, uint32_t p);          // 0x800C4500
    uint32_t Enter(uint32_t ev, uint32_t r, uint32_t p);          // 0x800C4454
    uint32_t SetRiderState(uint32_t ev, uint32_t e, uint32_t p);  // 0x800C2FF4
    uint32_t Path(uint32_t rider, uint32_t ev);                   // 0x800C37B0

private:
    // The gate with `*pp` as a value: `p` in, and when the gate reaches its end the value(s) it
    // stores to `*pp` (first `p`, then `p | 0x20` after a redirect - both stores, in that order).
    struct GateOut {
        uint32_t v0 = 0;
        bool wrote = false;
        uint32_t first = 0;
        bool second = false;
        uint32_t secondValue = 0;
    };
    GateOut GateCore(uint32_t ev, uint32_t r, uint32_t p);

    GuestRam& g_;
    StanceSeams& seams_;
    AnimMachine anim_;
    bool refused_ = false;
};

// ============================================================================ the knock-off and the launch
// `RiderKnockOff 0x80090D84` (1764 B, frame 48) and `RiderLaunch 0x80091468` (4100 B, frame 80),
// on top of the stance layer. What they call that is NOT ported comes
// through `RiderSeams`; everything else - the stance event, FixMul, DotLcm, MulAdd, Scale, Asin,
// Normalize, the GTE OP, Rand, CopyHalfwords, AiClearCommands, AiPushCommand, the identity reset
// 0x8008CF74, QuatGet/QuatSet, CamTarget SLUS 0x800235B0, BuildObbAlt - is native.
constexpr uint32_t kRiderKnockOffFn = 0x80090D84;
constexpr uint32_t kRiderLaunchFn   = 0x80091468;
constexpr uint32_t kTakedownFn      = 0x800BF51C; // PORTED (takedown.h), 1 arg
constexpr uint32_t kRiderOffSoundFn = 0x80018440; // SLUS, PORTED (takedown.h), 2 args
constexpr uint32_t kArrestFn        = 0x80096F30; // NOT ported (seam), 3 args
constexpr uint32_t kPoseInitAFn     = 0x8008EE60; // stance 38      } the five pose initialisers:
constexpr uint32_t kPoseInitBFn     = 0x8008E818; // 39, 89         } NOT ported (seams); (R, 0),
constexpr uint32_t kPoseInitCFn     = 0x8008E50C; // 40, 88         } except C, which never reads
constexpr uint32_t kPoseInitDFn     = 0x8008EB88; // 41, 90         } its a1 (the first use of a1
constexpr uint32_t kPoseInitEFn     = 0x8008E044; // 42, 91         } at 0x8008E53C is a load)
constexpr uint32_t kRiderSyncFn     = 0x8008DF74; // NOT ported (seam), 1 arg
constexpr uint32_t kCrashEventFn    = 0x8001A760; // SLUS, NOT ported (seam), 2 args
constexpr uint32_t kLaunchLiftFn    = 0x8007E868; // NOT ported (seam), 4 args
constexpr uint32_t kSettleFn        = 0x8009246C; // NOT ported (seam), 1 arg

struct RiderSeams : StanceSeams {
    virtual uint32_t Takedown(uint32_t bike) = 0;
    virtual uint32_t RiderOffSound(uint32_t handle, uint32_t mode) = 0;
    virtual uint32_t Arrest(uint32_t cop, uint32_t bike, uint32_t why) = 0;
    // `fn` is one of the five kPoseInit*Fn; `a1` is 0 for all but C (see above).
    virtual uint32_t PoseInit(uint32_t fn, uint32_t r, uint32_t a1) = 0;
    virtual uint32_t RiderSync(uint32_t r) = 0;
    virtual uint32_t CrashEvent(uint32_t handle, uint32_t kind) = 0;
    virtual uint32_t LaunchLift(uint32_t dir, uint32_t speed, uint32_t c, uint32_t k) = 0;
    virtual uint32_t Settle(uint32_t r) = 0;
};

class RiderLayer {
public:
    // `ram` is the base of the same 2 MiB `g` views: the ported AI-stack helpers (ai.h) and
    // BuildObbAlt (bike.h) work on raw byte views of their records, as their own rows do.
    RiderLayer(GuestRam& g, uint8_t* ram, RiderSeams& seams)
        : g_(g), ram_(ram), seams_(seams), stance_(g, seams) {}

    StanceLayer& stance() { return stance_; }
    bool Failed() const { return refused_ || stance_.Failed(); }

    uint32_t KnockOff(uint32_t r); // 0x80090D84
    uint32_t Launch(uint32_t r);   // 0x80091468

private:
    uint8_t* Raw(uint32_t a) { return ram_ + (a & 0x1FFFFFu); }
    uint32_t Peek32(uint32_t a); // a load that never faults the view (0 off RAM): see Launch's tail
    void KnockOffBody(uint32_t r);
    void Police(uint32_t bike);
    void LoadDir(uint32_t r, uint32_t s5);
    void Rot357(uint32_t r);
    void LiftByAsin(uint32_t r);
    void Op(uint32_t dst, uint32_t d, uint32_t ir);
    bool Norm(uint32_t v, int32_t& n);
    void QuatRot(uint32_t r, int32_t ang, bool second);
    void ViewOr(uint32_t s5, uint32_t bits);

    GuestRam& g_;
    uint8_t* ram_;
    RiderSeams& seams_;
    StanceLayer stance_;
    bool refused_ = false;
};

} // namespace rr::sim
