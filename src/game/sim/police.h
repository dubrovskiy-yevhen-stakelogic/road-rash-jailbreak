#pragma once
// The police scheduler - once per SpawnerPass round (SchedDispatch 0x8009B474 kind 0) it decides, per
// player, whether a dormant cop bike is released onto the road near him, and releases it - ported from
// our own disassembly of the player's own images:
//
//   SLUS_010.53  SHA-1 67ed165a2c517d4e6106fb0dfa324d66dd9a76f1, text at 0x80010000
//   RASHCDG.BIN  SHA-1 cfe43a7786759f2cb9c57751cf99e84d1074782c, loaded at 0x8005B5E8
//
// docs\formats\rules.md 9.2 has the police spawner's rules. Accepted only by `rrverify phys` rows
// (tools\rrverify\rows_police.inc).
//
// The memory model, the stack convention and the fault rule are population.h's: functions over
// road_query.h's guest-address view, `sp` at entry for a function with a frame, locals whose address
// the original hands out at the original's frame offsets, false on a refusal or a fault.
//
// RAND. The shared LCG (SLUS 0x8001FC58, seed at gp+2076) is drawn where the original draws it:
// the police roll (0x8009EB10, one `% 100` per player whose accumulator A is past its threshold), the
// road walk's node choices (on the seam, SLUS 0x80012C1C) and CopSetup's `& 7` (0x800942D4, redrawn
// until it differs from the old value, at most 8 times) - in that order within a player.
#include <cstdint>

#include "game/sim/population.h"
#include "game/sim/road_query.h"

namespace rr::sim {

// ---------------------------------------------------------------------------- globals
constexpr uint32_t kPoliceOn       = 0x8005ACC0; // the police switch
constexpr uint32_t kPoliceAvail    = 0x800D86F4; // s32: cops available (CopCount's cap)
constexpr uint32_t kPoliceRecord   = 0x800D86F0; // the saved cop record (+8 stat word, +12..+22 riderDef
                                                 // bytes, +19 valid); its +0 is the cops-out count
constexpr uint32_t kPoliceAccA     = 0x8005B2A0; // s32 per player: accumulator A (the roll)
constexpr uint32_t kPoliceAccB     = 0x8005B368; // s32 per player: accumulator B (the release timer)
constexpr uint32_t kPoliceField    = 0x8005B1F8; // s32: the place past which a player is "near the back"
                                                 //      once the available cops are taken off
constexpr uint32_t kPoliceThrTab   = 0x800530FC; // s32 x 6: accumulator thresholds, row bank (+3 with a cop out)
constexpr uint32_t kPoliceDistTab  = 0x80053114; // s32 by bank: the release distance (120.0)
constexpr uint32_t kPolicePctTab   = 0x80053120; // u32 x 6: the roll's percentage, row as the thresholds
constexpr uint32_t kPoliceSpeedTab = 0x80053138; // s32 by bank: a released cop's speed (18.0 / 29.0 / 33.0)
constexpr uint32_t kPoliceP2Bike   = 0x8005B21C; // player 2's bike (PoliceShare)
constexpr uint32_t kPoliceP1Bike   = 0x8005B38C; // player 1's bike (PoliceShare)

// ---------------------------------------------------------------------------- the callees
// The two the scheduler reaches that other passes port; the caller supplies them (the bench runs the
// original code). `sp` is the stack pointer the original makes the call at. Each
// returns false when the caller could not run it; the port then refuses.
struct PoliceCallees {
    virtual ~PoliceCallees() = default;
    // SLUS 0x80012C1C RoadWalk(from, out, dist): the road coordinate `dist` along from `from` into
    // `out` (12 bytes), across nodes by the route (Rand at SLUS 0x80012D9C / 0x80012EB0).
    virtual bool RoadWalk(uint32_t from, uint32_t out, int32_t dist, uint32_t sp) = 0;
    // SLUS 0x8003C520(cursor): the length (16.16) of the cursor's road (1 argument).
    virtual bool RoadLength(uint32_t cursor, uint32_t sp, uint32_t& v0) = 0;
};

// 0x80095848, 42 instructions, a leaf: the first pool-0 slot (from *(0x800CE4D0), stride
// *(0x800CE4D4), *(*(0x800CE4DC)) + 1 slots) that is not a player (its handle +0xAC not below the
// player count), is class 2 (riderDef +1 low nibble), dormant (+0x140 == 0) and not in play (+0x3A0
// bit 4 clear); 0 when there is none. A null slot pointer is skipped (the stride still added).
uint32_t FindFreeCop(GuestRam& g);

// 0x80094184, 106 instructions, frame 40: for a class-2 bike and a valid saved cop record
// (0x800D86F0 + 19 non-zero), the record's rider bytes into riderDef, stats[+0xE0] from 0x800D86F8,
// and riderDef[+0x26] = 66 + 8 * clamp(bank, 0, 2) + (Rand() & 7), redrawn while it equals the old
// value (at most 8 draws).
void CopSetup(GuestRam& g, uint32_t e);

// 0x800A01CC, 185 instructions, frame 56: the two-player share (race type bit 4, both players
// eligible, their bikes within half the release distance + 100 of each other): a player whose bike is
// in front of the OTHER player's camera loses his cop when only one of them is; when both or neither
// are, both are dropped if the two cameras face apart (DotLcm of the views' +0x1BC rows < 0). `flags`
// is the scheduler's s32[2] (its sp+24).
void PoliceShare(GuestRam& g, uint32_t flags, uint32_t sp);

// 0x8009E89C, 444 instructions, frame 80: the police scheduler, `acc` the SpawnerPass round's time.
// Accumulator A rolls `Rand() % 100` against the percentage and forces a release when the player is
// far enough from the end of his road; accumulator B releases a cop when it passes its threshold (half
// of it for a "wanted" rider, riderDef[0] bit 7, or a player off his route). The release: FindFreeCop,
// the cop placed 60.0 behind the player by the road walk (the forced arm of race type 44 / phase 3:
// 120.0 ahead on the player's own road, clamped by SLUS 0x8003C520), aborted when the walk ends on a
// node; then ResetBike, +0x39C = +0x1E0 = the bank's speed, CopSetup, live, the rider seated, the
// placement (Transition(cop, 0)), AI stack [{4, 224}], riderDef[+0x3E] = clock >> 8.
// `pop` supplies the population's callees (ResetBike, the placement's, ClearCommands / PushCommand).
bool PoliceSched(GuestRam& g, int32_t acc, uint32_t sp, PoliceCallees& pc, PopulationCallees& pop);

} // namespace rr::sim
