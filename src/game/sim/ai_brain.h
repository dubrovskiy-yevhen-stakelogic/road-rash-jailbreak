#pragma once
// The AI's per-frame brain - the overtaking / avoidance planner, its spatial query and its target
// score - ported from our own disassembly of the player's own overlay:
//
//   RASHCDG.BIN  SHA-1 cfe43a7786759f2cb9c57751cf99e84d1074782c, loaded at 0x8005B5E8
//   SLUS_010.53  SHA-1 67ed165a2c517d4e6106fb0dfa324d66dd9a76f1 (the fixed-point leaves)
//
// Accepted only by `rrverify phys` rows ai_spatial_query, ai_brain_probe,
// ai_brain and ai_brain_pass (tools\rrverify\rows_ai_brain.inc).
//
// THE MEMORY MODEL is road_query.h's guest-address view (`GuestRam`), as in population.h: the brain
// chases the pool tables, the spatial grid and whichever object the query found, so it runs on the
// guest addresses themselves. `sp` is the stack pointer at the function's entry; the brain's frame
// (176 bytes) holds the hit list, the hit count, the 8-byte command record it hands to AiPushCommand,
// the aim target and scalar it hands to SetAimDelta, and the words it re-reads later - all written
// where the original writes them, because callees receive their addresses. ONE STALE READ is part of
// the original: with the other bike's top command 10, the brain compares the halfword at its frame's
// sp+98 (the target of the LAST command record it built, in this or an earlier call) with its own
// handle before writing it (0x800BDBAC). A host that wants the console's behaviour gives the brain a
// stack region nothing else writes between calls.
//
// Faults. A load or store the console would not survive is not performed; `GuestRam` records the
// first such address and every function here then returns false / the caller must FAIL the call.
#include <cstdint>

#include "game/sim/road_query.h"

namespace rr::sim {

// ---------------------------------------------------------------------------- globals read
constexpr uint32_t kAiBrainLiveCount  = 0x8005B1F8; // s32 live pool-0 bike count (the pass's bound)
constexpr uint32_t kAiBrainPool0Ptr   = 0x8005B3A0; // -> pool-0 slot 0, stride 1096
constexpr uint32_t kAiBrainPool1Ptr   = 0x8005B3A4; // -> pool-1 slot 0 (riders), stride 628
constexpr uint32_t kAiBrainGameState  = 0x8005B2F8; // -> game_state: +0x10 race clock, +0x1C, +0x30 players, +0x3C level
constexpr uint32_t kAiBrainPoolTable  = 0x800CE4D0; // 16 bytes per pool: +0 base, +4 stride
constexpr uint32_t kAiBrainHandleOff  = 0x800CCA68; // s32 per pool >= 2: offset of the handle in the object
constexpr uint32_t kAiBrainGridOrgX   = 0x800CCF98; // s32[2] the spatial grids' x origins (16.16)
constexpr uint32_t kAiBrainGridOrgZ   = 0x800CCFA0; // s32[2] their z origins
constexpr uint32_t kAiBrainGridLinks  = 0x800CCFA8; // 128 x {u8 next, u8 code}; 128 ends a chain
constexpr uint32_t kAiBrainGridCells  = 0x800CD0B0; // u8[48][24] chain heads (rows 24..47 = grid 1)
constexpr uint32_t kAiBrainPool6Ptr   = 0x800CD6C4; // -> pool-6 records, stride 280
constexpr uint32_t kAiBrainCarPool    = 0x800CF660; // pool-3 cars, stride 512 (the brain's own constant)
constexpr uint32_t kAiBrainWeights    = 0x800CCAC8; // u8 per pool: the probe's weight (pool 6: +4 / +6)
constexpr uint32_t kAiBrainSteerSin   = 0x8005624C; // s16 table the lean term indexes by e[+0x28C]
constexpr uint32_t kAiBrainGapLimit   = 0x80053018; // s32 per level (game_state +0x3C): the gap limit

constexpr uint32_t kAiBrainPassFrame  = 56;         // addiu sp,sp,-56 at 0x800BCD48
constexpr uint32_t kAiBrainFrame      = 176;        // addiu sp,sp,-176 at 0x800BD4D4

// ============================================================================ the callees
// The functions the brain reaches that are NOT in this file. `sp` is always the stack pointer the
// original makes the call at (the brain's entry sp - 176). Each returns false when the caller could
// not run it; the port then refuses and nothing it wrote is to be trusted.
struct AiBrainCallees {
    virtual ~AiBrainCallees() = default;
    // ---- PORTED elsewhere (ai.h); the caller runs them on the same memory
    // 0x800BCA68 AiPushCommand(cmd, mode, e); `cmd` is the guest address of the 8-byte record.
    virtual bool PushCommand(uint32_t cmd, int32_t mode, uint32_t e, uint32_t sp) = 0;
    // 0x800BC8DC AiPopCommand(e).
    virtual bool PopCommand(uint32_t e, uint32_t sp) = 0;
    // 0x80093CAC SetAimDelta(e, target, dir, scalar, mode): guest addresses, 0 for a null pointer;
    // the fifth argument is also stored at sp+16 of the brain's frame, as the original does.
    virtual bool SetAimDelta(uint32_t e, uint32_t target, uint32_t dir, uint32_t scalar, int32_t mode,
                             uint32_t sp) = 0;
    // ---- NOT ported (the bench runs the original code for them)
    // 0x800BB448(e, other, gap): the test that admits command 5 ("take the gap"); v0 != 0 admits.
    virtual bool GapTest(uint32_t e, uint32_t other, int32_t gap, uint32_t sp, uint32_t& v0) = 0;
    // 0x800BBBB8(e, other, gap): the test that admits command 7 ("close in"); v0 != 0 admits.
    virtual bool CloseTest(uint32_t e, uint32_t other, int32_t gap, uint32_t sp, uint32_t& v0) = 0;
};

// ============================================================================ the functions

// RASHCDG 0x8008AE94, 1428 bytes, frame 56, no callees: every object of the pools in `mask` (bit
// = pool; the brain asks 73 = pools 0, 3, 6) whose octagonal x/z distance from `pos` (s32[3], 16.16)
// is within `radius`, found through the 24x24 spatial grid of 32-unit cells (grid 1 = rows 24..47
// when there are two players and `pos` lies in it), `handle` (the low 16 bits; the original reads it
// from the stack at sp+20) excluded and each handle once. The handles are written as u16 at `list`,
// up to *countPtr of them; *countPtr is set to the number found - EXCEPT when the list fills, where
// the original returns without the store (the capacity already equals the count).
void AiSpatialQuery(GuestRam& g, uint32_t list, uint32_t countPtr, uint32_t pos, int32_t radius,
                    uint32_t mask, uint32_t handle);

// RASHCDG 0x800BEBA4, 1364 bytes, frame 48, callees only the ported leaves (AiProject, FixDiv):
// the brain's score of object `handle` for bike `e` - the time to reach it along the heading,
// weighted by the pool weight and a lateral penalty; 0x7FFF0000 = "not a candidate" (behind, too far
// sideways, too slow, too far ahead, or a pool-0 bike whose rider has +0x228 bit 15).
int32_t AiBrainProbe(GuestRam& g, uint32_t handle, uint32_t e);

// RASHCDG 0x800BD4D4, 4868 bytes, frame 176: AiBrain(e). `v0` is the original's return value (1 only
// when this call pushed a fresh command 3). Returns false on a refusal of a callee or a view fault.
bool AiBrain(GuestRam& g, uint32_t e, uint32_t sp, AiBrainCallees& c, int32_t& v0);

// RASHCDG 0x800BCD48, 420 bytes, frame 56: the per-frame brain pass over the live pool-0 bikes:
// AiBrain for each bike not in `skip`, awake, under AI control, not a
// "simplified" rider, top command 3..17; then the +0x234 bit 9 / +0x2C0 manoeuvre latch (`maskC`
// bit set: count down by `dt`, else reload 0xC000).
bool AiBrainPass(GuestRam& g, int32_t dt, uint32_t skip, uint32_t maskC, uint32_t sp, AiBrainCallees& c);

} // namespace rr::sim
