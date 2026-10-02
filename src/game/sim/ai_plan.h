#pragma once
// The AI planner - `AiPlan RASHCDG 0x800B8018` and the functions under it that were not ported
// before - transcribed from our own disassembly of the player's own
// images:
//
//   SLUS_010.53  SHA-1 67ed165a2c517d4e6106fb0dfa324d66dd9a76f1, text at 0x80010000
//   RASHCDG.BIN  SHA-1 cfe43a7786759f2cb9c57751cf99e84d1074782c, loaded at 0x8005B5E8
//
// Accepted only by `rrverify phys` rows (tools\rrverify\rows_ai_plan.inc).
//
// THE MEMORY MODEL is road_query.h's guest-address view (`GuestRam`), as in population.h: the
// planner walks the pool, follows the partner chain at e+0x358, indexes grudge bytes by a table
// read out of guest memory and hands entity addresses to its callees, so it runs on guest addresses.
// The ported ai.h helpers (AiHandleInList, AiProject, AiNibbleAdd, AiNibbleDecay) are called
// directly on values / one-byte copies read out of the view.
//
// STACK. Every function with a frame takes `sp`, the stack pointer at its entry, and hands each
// callee the `sp` the original makes the call at. The two 8-byte command records the original builds
// in its own frame (AiPlan sp+16, AiChooseCommand sp+16) are written there, and only the halfwords
// the original writes: AiPushCommand reads all 8 bytes and its mode-2 rotation stores the record's
// spare halfword (+4) into the entity's stack, so the rest is whatever the guest stack holds.
// AiPlan's head-to-head sign halfword is kept at its own sp+40 and READ BACK from there, because the
// original reads it on a one-player pass too (then it is a stale frame word). Callee-saved register
// spills are not written.
//
// Faults. A load or store the console would not survive is not performed; `GuestRam` records the
// first such address and every function here returns false (refuses) when the view faulted.
// Nothing here guesses a value.
//
// WHAT THE PLANNER NEEDS SET (all read out of the functions' own lui/lw pairs):
//   0x8005B2F8 -> game_state (+0x04 mode byte, bit 0 = cop mode; +0x0A+p player p's state byte;
//                             +0x30 the player count)
//   0x800CE4D0 pool-0 control (+0 base, +8 -> the slot count word), 0x8005B3A0 pool-0 base (targets)
//   0x8005B268 player bikes (4 bytes each), 0x8005B38C / 0x8005B21C player 1 / 2 bike
//   0x8005B1F8 live bike count, 0x8005B2A8 the pass parity word (bit 0 clear = recompute places),
//   0x8005AD48 race flags (& 0x1F), 0x800CD540 (u16) vs 0x8005ADC0 (u16 by player count - 1) the
//   full-AI cap, 0x8005ADC4 the taunt weights, 0x8005ADCC / 0x8005ADD4 the grudge / mood nibble
//   tables (2 words each), 0x8005ADDC the own-event nibble word, 0x800D38B0 handle -> AI index,
//   0x800D38C8 AI index -> handle (31 = empty), 0x800CCAC0 per-player claim masks (u16), 0x800CCAC4
//   the head-to-head word, 0x800CD530 the per-bike drama amounts (bytes, from slot 2), the class
//   table SLUS 0x80052EE4 (36 bytes per class), 0x80052F70 the "close" gap, SLUS 0x800541D4 and
//   0x8005AD4C for the stance event. e+0x352 is the proximity score the planner ranks by; it is
//   written by RASHCDG 0x8008CFDC, not here.
#include <cstdint>

#include "game/sim/road_query.h"

namespace rr::sim {

constexpr uint32_t kAiPlanFn          = 0x800B8018;
constexpr uint32_t kAiChooseCommandFn = 0x800B8FB0;
constexpr uint32_t kAiTauntFn         = 0x800B92C0;
constexpr uint32_t kAiWeaponPickFn    = 0x800B9340;
constexpr uint32_t kAiRelationFn      = 0x800BCF6C;
constexpr uint32_t kAiOwnEventFn      = 0x800BD2A0;
constexpr uint32_t kAiReachableFn     = 0x800BEA30;
constexpr uint32_t kAiReachAheadFn    = 0x800205C0; // SLUS
constexpr uint32_t kAiReachBehindFn   = 0x8002064C; // SLUS
constexpr uint32_t kAiReachSideFn     = 0x800206DC; // SLUS
// Callees kept on the interface below.
constexpr uint32_t kAiCanEngageFn     = 0x800BC1EC; // 784 B, NOT ported here
constexpr uint32_t kAiChaseTestFn     = 0x800BBD44; // NOT ported here
constexpr uint32_t kAiCopTailFn       = 0x8009DC90; // 1256 B, the cop's planner tail, NOT ported
constexpr uint32_t kAiCopArrestScanFn = 0x80097388; // NOT ported (-> 0x80097470, Arrest 0x80096F30(.., 9))
constexpr uint32_t kAiRiderVoiceFn    = 0x8001A760; // SLUS, NOT ported
constexpr uint32_t kAiComputePlaceFn  = 0x800138E8; // SLUS, ported in race.h (host view)
constexpr uint32_t kAiPushCommandFn   = 0x800BCA68; // ported in ai.h (host view)
constexpr uint32_t kAiStanceEventFn   = 0x800C4550; // ported in stance.h

// The callees the planner reaches that are NOT written in this file. `sp` is always the stack
// pointer the original makes the call at. Each returns false when the caller could not run it; the
// port then refuses (returns false) and nothing it wrote is to be trusted.
struct AiPlanCallees {
    virtual ~AiPlanCallees() = default;
    // ---- NOT ported (the bench runs the original code for them)
    // 0x800BC1EC(me, target, lateral) - the engage feasibility test; v0 != 0 = feasible.
    virtual bool CanEngage(uint32_t e, uint32_t t, int32_t lateral, uint32_t sp, uint32_t& v0) = 0;
    // 0x800BBD44(me, target, lateral) - the chase (command 8) test; v0 != 0 = go.
    virtual bool ChaseTest(uint32_t e, uint32_t t, int32_t lateral, uint32_t sp, uint32_t& v0) = 0;
    // 0x8009DC90(e) - a cop bike's planner tail (it pushes its own commands).
    virtual bool CopTail(uint32_t e, uint32_t sp) = 0;
    // 0x80097388(e) - a player bike in cop mode: every pool entity close enough is checked
    // (0x80097470) and busted (0x80096F30(e, other, 9)).
    virtual bool CopArrestScan(uint32_t e, uint32_t sp) = 0;
    // SLUS 0x8001A760(handle, 0) - the rider's voice line.
    virtual bool RiderVoice(uint32_t handle, int32_t a1, uint32_t sp) = 0;
    // ---- PORTED elsewhere; the caller runs them on the same memory
    // SLUS 0x800138E8 ComputePlace(e, mode) (race.h).
    virtual bool ComputePlace(uint32_t e, int32_t mode, uint32_t sp, int32_t& place) = 0;
    // 0x800BCA68 AiPushCommand(cmd, mode, e) (ai.h); `cmd` is the guest address of the 8-byte record
    // in the caller's frame, read before and written back after.
    virtual bool PushCommand(uint32_t cmd, int32_t mode, uint32_t e, uint32_t sp, int32_t& v0) = 0;
    // 0x800C4550 the stance event (stance.h).
    virtual bool StanceEvent(uint32_t ev, uint32_t r, uint32_t p, uint32_t sp) = 0;
};

// ============================================================================ SLUS leaves
// The three reach limits of a bike of class `cls` (SLUS 0x80052EE4 + 36 cls: +24, +28, +32) at speed
// `speed` (e+0x1E0, 16.16). Frames 0 / 32 / 32, callees FixMul only.
// 0x800205C0: min(max(FixMul(FixMul(speed, 0xCCC), T24), > 0x300000), T24).
int32_t AiReachAhead(GuestRam& g, int32_t speed, uint32_t cls);
// 0x8002064C: d = T28 - FixMul(FixMul(0x666, speed), T28); min(d > 0x400000 ? d : 0x400000, T28).
int32_t AiReachBehind(GuestRam& g, int32_t speed, uint32_t cls);
// 0x800206DC: the same with 0x8F5, T32 and 0x190000.
int32_t AiReachSide(GuestRam& g, int32_t speed, uint32_t cls);

// ============================================================================ RASHCDG leaves
// 0x800BD2A0, 43 instructions: the own-event classifier - 2 when a rider placed 4th or worse
// (and not past the live count) has gained on `oldPlace`, 4 when one placed 5th or better has lost
// against it; 0 for a cop bike that is not a player, and when bit 0 of 0x8005B2A8 is set.
uint32_t AiOwnEvent(GuestRam& g, uint32_t e, uint32_t oldPlace);
// 0x800BCF6C, 205 instructions: the 13-bit stimulus mask of `other` toward `me`.
uint32_t AiRelation(GuestRam& g, uint32_t me, uint32_t other);
// 0x800BEA30, 92 instructions: 1 when the bike whose handle is at `h` (= t + 0xAC) lies inside `e`'s
// reach box (AiReachAhead / AiReachBehind along, AiReachSide across).
int32_t AiReachable(GuestRam& g, uint32_t h, uint32_t e);
// 0x800B9340, 48 instructions: the weapon choice of rider record `rd` from its combat byte +0x3C
// (+0x2E weapon, +0x2F its level). False when the class-table scan runs away (the console would
// read on forever); the original never returns in that case.
bool AiWeaponPick(GuestRam& g, uint32_t rd);
// 0x800B92C0, 32 instructions, frame 32: Rand() & 0x7F against the cumulative weights at
// 0x8005ADC4 picks rd+0x3C = rd[0x34 + k], then AiWeaponPick. One Rand draw.
bool AiTaunt(GuestRam& g, uint32_t rd);

// ============================================================================ the planner
// 0x800B8FB0, 196 instructions, frame 48: which command `e` issues at target `t` with grudge
// `aggr` (a signed byte), and its push (mode 2) or the in-place replace of the top slot. `v0` is the
// original's return (0, 1, or AiPushCommand's).
bool AiChooseCommand(GuestRam& g, uint32_t e, int32_t aggr, uint32_t t, uint32_t sp, AiPlanCallees& c,
                     uint32_t& v0);
// 0x800B8018, 998 instructions, frame 96: the planner pass. `acc` is the argument
// RaceTick passes (the accumulated time, or 0 on the first crossing).
bool AiPlan(GuestRam& g, int32_t acc, uint32_t sp, AiPlanCallees& c);

} // namespace rr::sim
