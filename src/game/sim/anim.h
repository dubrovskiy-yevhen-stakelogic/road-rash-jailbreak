#pragma once
// The rider animation machine's CLOCK - `RASHCDG 0x8005BD74..0x8005E848`, plus the channel-header
// decoder `0x8005E2BC` - transcribed from our own disassembly of the player's own images:
//
//   RASHCDG.BIN  SHA-1 cfe43a7786759f2cb9c57751cf99e84d1074782c, loaded at 0x8005B5E8
//   SLUS_010.53  SHA-1 67ed165a2c517d4e6106fb0dfa324d66dd9a76f1, text at 0x80010000
//
// and accepted only by `rrverify phys` rows (tools\rrverify\rows_anim.inc).
//
// What is here: the op program (loop / play-once / jump / blend / skip / stop), the tick that
// advances `floor(acc / rate)` keys, the four starts that write a program and restart it, the two
// predicates gameplay reads back (ClipDone, ChannelFree), the per-frame pass, the bank switch and
// the seat release the stance gate calls, the quaternion get/set RiderLaunch uses, and the DMD3
// channel-header decode that every clip change runs.
//
// What is NOT here (the pose, pose.h): `TransitionCapture 0x8005CB70` and
// `ApplyFrame 0x8005D2A8`. The machine calls them through `AnimPoseSeam`; the bench supplies them
// from the oracle, and a product that wants only the clock supplies the three
// stores the clock depends on (see AnimPoseSeam).
//
// THE MEMORY MODEL is road_query.h's `GuestRam`: an animation object (0x83C bytes), its 5-op
// program (60 bytes), the bank slots and the DMD3 clip blocks are all read and written at their
// guest addresses, exactly where the original does. Every store is made in the original's order,
// re-reading what the original re-reads, so a program that aliases the object behaves as it does on
// the console. A function refuses (`Failed()`) when the view refuses an access or when the tick
// would spin past any instruction budget (a non-positive rate); it never guesses a value.
//
// QUIRKS carried on purpose:
//   * `ClipDone` is true from the pass where a play-once op's LAST key starts (`frame >= last`);
//     `Advance` ends that op only at `frame > last`, one key later;
//   * the loop count is loaded with `lh`: LoopStart's 0xFFFF becomes 0xFFFFFFFF, never the
//     0x0000FFFF that `Advance` compares with, so "forever" is 2^32 loops;
//   * a queued transition turns a running LOOP into play-once with (a, b) = (-1, 0);
//   * Restart keeps flag bits 0, 2, 5, 6, 7 (`andi 0xE5`);
//   * Seq's guard counts from the pc, not from 0;
//   * DecodeHeader returns 0x800D0000 (the delay-slot `lui`) when a coded clip has < 3 bytes left.
#include <cstdint>

#include "game/sim/road_query.h"

namespace rr::sim {

// ---------------------------------------------------------------------------- addresses
constexpr uint32_t kAnimRestartFn           = 0x8005BD74;
constexpr uint32_t kAnimStopFn              = 0x8005BE0C;
constexpr uint32_t kAnimStopIfPlayingFn     = 0x8005BE20;
constexpr uint32_t kAnimResumeFn            = 0x8005BE44;
constexpr uint32_t kAnimClipDoneFn          = 0x8005BE58;
constexpr uint32_t kAnimChannelFreeFn       = 0x8005BEF4;
constexpr uint32_t kAnimHardStartFn         = 0x8005BF6C;
constexpr uint32_t kAnimRangedStartFn       = 0x8005C018;
constexpr uint32_t kAnimLoopStartFn         = 0x8005C0B0;
constexpr uint32_t kAnimTransitionFn        = 0x8005C140;
constexpr uint32_t kAnimQuatGetFn           = 0x8005C338;
constexpr uint32_t kAnimQuatSetFn           = 0x8005C36C;
constexpr uint32_t kAnimClipSelectFn        = 0x8005C39C;
constexpr uint32_t kAnimSeqFn               = 0x8005C418;
constexpr uint32_t kAnimTickFn              = 0x8005C4EC;
constexpr uint32_t kAnimLoopRestartFn       = 0x8005C52C;
constexpr uint32_t kAnimAdvanceFn           = 0x8005C58C;
constexpr uint32_t kAnimPoseStartFn         = 0x8005C8F4;
constexpr uint32_t kAnimAdvanceFrameFn      = 0x8005CB04;
constexpr uint32_t kAnimTransitionCaptureFn = 0x8005CB70; // pose side, NOT ported (seam)
constexpr uint32_t kAnimApplyFrameFn        = 0x8005D2A8; // pose side, NOT ported (seam)
constexpr uint32_t kAnimPassFn              = 0x8005E1D8;
constexpr uint32_t kAnimDecodeHeaderFn      = 0x8005E2BC;
constexpr uint32_t kAnimBankSwitchFn        = 0x80012858; // SLUS
constexpr uint32_t kSeatReleaseFn           = 0x80068D20;

constexpr uint32_t kAnimDescriptor   = 0x800CE170; // {objects, programs, ?, count, slots, used, cap, 0, bank[16]}
constexpr uint32_t kAnimBankTable    = 0x800CE190; // u32[16], the descriptor's bank table
constexpr uint32_t kAnimWidthTable   = 0x800CC654; // u8 *[8]: first byte - 1 = a w-bit stream's escape
constexpr uint32_t kAnimGameStatePtr = 0x8005B2F8; // -> game_state; +0x1C the frame's dt (1/300 s)
constexpr uint32_t kAnimObjectBytes  = 0x83C;
constexpr uint32_t kAnimProgramBytes = 60;

// Object fields.
namespace animf {
constexpr uint32_t kOwner = 0x00, kProgram = 0x04, kLength = 0x08, kPc = 0x0C, kFrame = 0x10,
                   kLoops = 0x14, kRate = 0x18, kAcc = 0x1C, kSub = 0x20, kFlags = 0x24,
                   kBank = 0x28, kClip = 0x2C, kTrack = 0x30, kEx = 0x6DC, kMask = 0x6E0,
                   kBlend = 0x6E4;
}

// The pose side of the machine, which this file does not contain. Each returns the callee's v0.
// A product that keeps only the clock must reproduce what the clock later reads of their effects:
//   * TransitionCapture: `ClipSelect(a, op[+28])` (the NEW clip becomes current - Advance's op-3 end
//     reads its key count) and `B[+4] = 10` (PoseStart's op-3 rate);
//   * ApplyFrame: `a[+0x24] = lbu(a[+0x24]) | 0x10` (bits 8..31 cleared) and, when the op's play
//     flags have bit 4, owner byte +0x223 negated once and the flag bit cleared.
struct AnimPoseSeam {
    virtual ~AnimPoseSeam() = default;
    // RASHCDG 0x8005CB70 TransitionCapture(a, op, B = a + 0x6E4)
    virtual uint32_t TransitionCapture(uint32_t a, uint32_t op, uint32_t blend) = 0;
    // RASHCDG 0x8005D2A8 ApplyFrame(a)
    virtual uint32_t ApplyFrame(uint32_t a) = 0;
};

class AnimMachine {
public:
    AnimMachine(GuestRam& g, AnimPoseSeam& pose) : g_(g), pose_(pose) {}

    // True when the view refused an access or the tick refused to spin; nothing written since is
    // to be trusted.
    bool Failed() const { return refused_ || g_.Faulted(); }
    GuestRam& ram() { return g_; }
    AnimPoseSeam& pose() { return pose_; }

    // ---- A1: the leaves
    uint32_t Tick(uint32_t a, int32_t dt);            // 0x8005C4EC, returns 1
    uint32_t Seq(uint32_t a);                         // 0x8005C418, returns the new pc
    uint32_t ClipDone(uint32_t a);                    // 0x8005BE58
    uint32_t ChannelFree(uint32_t a);                 // 0x8005BEF4
    uint32_t Stop(uint32_t a);                        // 0x8005BE0C
    uint32_t StopIfPlaying(uint32_t a);               // 0x8005BE20
    uint32_t Resume(uint32_t a);                      // 0x8005BE44
    uint32_t QuatGet(uint32_t a, uint32_t out);       // 0x8005C338
    uint32_t QuatSet(uint32_t a, uint32_t in);        // 0x8005C36C
    uint32_t BankSwitch(uint32_t a, uint32_t bank);   // SLUS 0x80012858, returns 5
    uint32_t SeatRelease(uint32_t bike, uint32_t rider, uint32_t idx); // 0x80068D20

    // ---- A2: the clock and the starts
    uint32_t DecodeHeader(uint32_t a, uint32_t t);    // 0x8005E2BC (ported with the clock)
    uint32_t ClipSelect(uint32_t a, uint32_t clip);   // 0x8005C39C
    uint32_t LoopRestart(uint32_t a);                 // 0x8005C52C
    uint32_t Advance(uint32_t a, int32_t dt);         // 0x8005C58C, 1 = the op is over
    uint32_t PoseStart(uint32_t a);                   // 0x8005C8F4
    uint32_t AdvanceFrame(uint32_t a, int32_t dt);    // 0x8005CB04
    uint32_t Restart(uint32_t a);                     // 0x8005BD74
    // The o32 stack arguments of the starts are plain parameters here.
    uint32_t HardStart(uint32_t a, uint32_t clip, uint32_t flags, uint32_t rate, uint32_t ex);
    uint32_t RangedStart(uint32_t a, uint32_t clip, uint32_t flags, uint32_t first, uint32_t last,
                         uint32_t rate, uint32_t ex);
    uint32_t LoopStart(uint32_t a, uint32_t clip, uint32_t flags, uint32_t rate, uint32_t ex);
    uint32_t Transition(uint32_t a, uint32_t clip, uint32_t queued, uint32_t flags, uint32_t once,
                        uint32_t b7, uint32_t rate, uint32_t ex);

    // ---- A3: the per-frame pass over the descriptor (0x800CE170 in the game)
    uint32_t Pass(uint32_t desc);                     // 0x8005E1D8

private:
    GuestRam& g_;
    AnimPoseSeam& pose_;
    bool refused_ = false;
};

} // namespace rr::sim
