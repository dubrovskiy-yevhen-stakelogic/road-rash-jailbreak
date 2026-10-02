#pragma once
// The police in the race: the planner's cop tail, the op-17 intercept, the parked cop's op 1, the
// arrest and the cop-mode arrest scan - ported from our own disassembly of the player's own images:
//
//   SLUS_010.53  SHA-1 67ed165a2c517d4e6106fb0dfa324d66dd9a76f1, text at 0x80010000
//   RASHCDG.BIN  SHA-1 cfe43a7786759f2cb9c57751cf99e84d1074782c, loaded at 0x8005B5E8
//
// docs\formats\rules.md 9.3 has the mode's rules. Accepted only by `rrverify phys`
// rows (tools\rrverify\rows_cops.inc, one row per function).
//
// The memory model is road_query.h's guest-address view (`GuestRam`), as population.h / police.h: every
// function takes guest addresses, `sp` is the stack pointer at entry, locals whose address the original
// hands out sit at the original's frame offsets; false = a callee could not run or an address faulted.
//
// THE CHAIN (what these functions do together, one planner pass at a time, ~2 Hz):
//   release (PoliceSched, police.h)   cop stack [{4, 224}], live, +0x3A0 bit 4
//   CopTail 0x8009DC90                target = CopPickTarget (the player); if the cop's stopping distance
//                                     v^2 / (2 * +0x258) is within 5.0 of its distance to the target
//                                     (CopStopTest) -> clear, push {2, 224}, riderDef[+0x28] = clock (the
//                                     op-2 arm brakes it to a stop); else, on op 4: a slow or seated-on-
//                                     another-road target within 120.0 -> push {17, target} (intercept);
//                                     on the same road -> back off {6} (AiBackOffOk) or fight {16}
//                                     (AiStrikeReach + AiTaunt); on op 2: both slow, within 5.0 -> CopArrest
//   CopIntercept 0x800BBEBC (op 17)   the aim point 2.0 beside the target, +0x394 its distance; out of
//                                     range (CopInRange) -> pop
//   CopArrest 0x8009E528              the target stopped, place byte 254, the busted music, its camera
//                                     eye put at it, EndRace(target, 9) - the HUD's BUSTED
#include <cstdint>

#include "game/sim/road_query.h"

namespace rr::sim {

// ---------------------------------------------------------------------------- guest addresses
constexpr uint32_t kCopTailFn       = 0x8009DC90;
constexpr uint32_t kCopTargetOkFn   = 0x8009DB58;
constexpr uint32_t kCopStopTestFn   = 0x8009DBA0;
constexpr uint32_t kCopPickTargetFn = 0x8009E178;
constexpr uint32_t kCopAheadFn      = 0x8009E3F0;
constexpr uint32_t kCopDistFn       = 0x8009E444;
constexpr uint32_t kCopArrestFn     = 0x8009E528;
constexpr uint32_t kCopWantedBitsFn = 0x8009FC80;
constexpr uint32_t kCopInterceptFn  = 0x800BBEBC;
constexpr uint32_t kCopInRangeFn    = 0x800BC120;
constexpr uint32_t kCopIdleFn       = 0x800BAA2C;
constexpr uint32_t kCopArrestScanFn = 0x80097388;
constexpr uint32_t kCopArrestTestFn = 0x80097470;
// the callees that are not in this file
constexpr uint32_t kCopBustedMusicFn = 0x8001B3C8; // SLUS: the busted music (0x80023148), PORTED (takedown.h)
constexpr uint32_t kCopEndRaceFn     = 0x80092C7C; // EndRace, PORTED (spine.h)
constexpr uint32_t kCopJailTestFn    = 0x8009DA4C; // the cop-mode quota test, not ported
constexpr uint32_t kCopJailReleaseFn = 0x800A0708; // the cop-mode release, not ported
constexpr uint32_t kCopKnockArrestFn = 0x80096F30; // Arrest(e, other, how) of the cop-mode scan, not ported
// data
constexpr uint32_t kCopWantedMask   = 0x800D8708; // u32: bit (handle - 1) for every handle a cop was hit by
constexpr uint32_t kCopNearTab      = 0x800530A8; // s32 [bank + 3 * (race type & 1)]: 5.0 / 8.0 (arrest reach)
constexpr uint32_t kCopJailAcc      = 0x8005B290; // s32: the cop-mode quota accumulator
constexpr uint32_t kCopSlowSpeed    = 0x8F0D8;    // 8.94 units/s: "stopped" for an arrest

// The callees not written in this file. Each returns false when the caller could not run it.
struct CopCallees {
    virtual ~CopCallees() = default;
    // ---- PORTED elsewhere (ai.h); the caller runs them on the same memory
    virtual bool PushCommand(uint32_t cmd, int32_t mode, uint32_t e, uint32_t sp) = 0; // 0x800BCA68
    virtual bool ClearCommands(uint32_t e, uint32_t sp) = 0;                           // 0x800BCD10
    virtual bool PopCommand(uint32_t e, uint32_t sp) = 0;                              // 0x800BC8DC
    virtual bool EndRace(uint32_t e, int32_t how, uint32_t sp) = 0;                    // 0x80092C7C (spine.h)
    virtual bool ComputePlace(uint32_t e, int32_t mode, uint32_t sp, int32_t& v0) = 0; // SLUS 0x800138E8
    // ---- NOT ported
    virtual bool BustedMusic(uint32_t handle, uint32_t copHandle, uint32_t sp) = 0;    // SLUS 0x8001B3C8
    virtual bool JailTest(uint32_t e, uint32_t out, uint32_t sp, int32_t& v0) = 0;     // 0x8009DA4C
    virtual bool JailRelease(uint32_t e, uint32_t sp) = 0;                             // 0x800A0708
    virtual bool KnockArrest(uint32_t e, uint32_t other, int32_t how, uint32_t sp) = 0; // 0x80096F30
};

// ---- the leaves ---------------------------------------------------------------------------------
// 0x8009E444, frame 16: |octagonal x/z distance| << 16 between bike `a` and `b` (b's rider when it is
// off the bike, mount state 3 / 4); 0 when either is null.
int32_t CopDist(GuestRam& g, uint32_t a, uint32_t b, uint32_t sp);
// 0x8009DB58: may cop `e` go after `t`? t not finished, the cop's rider seated, t's place below 247.
int32_t CopTargetOk(GuestRam& g, uint32_t e, uint32_t t);
// 0x8009DBA0: has the cop reached the point to stop at? |dist - stop| within the arrest reach, the cop
// not finished, and the cop slow (or t's rider off the bike in stance 72/73 and not slower than it).
int32_t CopStopTest(GuestRam& g, uint32_t e, uint32_t t, int32_t dist, int32_t stop);
// 0x8009E3F0: is `t` (its rider when off the bike) behind `e` along the road, in t's direction?
int32_t CopAhead(GuestRam& g, uint32_t e, uint32_t t);
// 0x8009FC80: the 5-bit handle list `list` into the wanted mask 0x800D8708 (handles below the count).
void CopWantedBits(GuestRam& g, uint32_t list);
// 0x8009E178, frame 16: the cop's target handle - its top command's, the race's target (race type
// bit 0), the nearer unfinished player (bit 4), else player 1 unless finished. Reads its own frame
// words as the original does (a one-player bit-4 race reads a stale word, as on the console).
uint32_t CopPickTarget(GuestRam& g, uint32_t e, uint32_t sp);
// 0x800BC120: is `t` within the cop's intercept range (|dist| <= 2 * tune[+0x0C])? A seated fast target
// never is; a fast cop always is within that; a slow one within the arrest reach.
int32_t CopInRange(GuestRam& g, uint32_t e, uint32_t t, int32_t dist);

// ---- the functions with callees -----------------------------------------------------------------
// 0x8009E528, frame 32: cop `e` arrests `t`.
bool CopArrest(GuestRam& g, uint32_t e, uint32_t t, uint32_t sp, CopCallees& c);
// 0x8009DC90, frame 56: the planner's tail for a cop bike (AiPlan 0x800B8894). v0: 1 when it began to
// stop (op 2 pushed), else 0.
bool CopTail(GuestRam& g, uint32_t e, uint32_t sp, CopCallees& c, uint32_t& v0);
// 0x800BBEBC, frame 48: op 17's handler. `sqrtTable` = SLUS 0x800560CC (Length3's).
bool CopIntercept(GuestRam& g, uint32_t e, uint32_t target, uint32_t sp, CopCallees& c,
                  const int16_t* sqrtTable);
// 0x800BAA2C, frame 48: op 1's handler (a parked cop, the cop-mode player): when its release time
// (the command's +4, x 0x6D00 race ticks) has come, the command at `cmd` (or slot 1) becomes {4}.
bool CopIdle(GuestRam& g, uint32_t e, uint32_t cmd, int32_t dt, uint32_t sp, CopCallees& c);
// 0x80097470, frame 40: may cop-mode player `e` arrest bike `t`?
bool CopArrestTest(GuestRam& g, uint32_t e, uint32_t t, uint32_t sp, CopCallees& c, uint32_t& v0);
// 0x80097388, frame 40: the cop-mode player's arrest scan over the whole pool table.
bool CopArrestScan(GuestRam& g, uint32_t e, uint32_t sp, CopCallees& c);

} // namespace rr::sim
