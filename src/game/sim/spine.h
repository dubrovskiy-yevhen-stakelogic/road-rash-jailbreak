#pragma once
// Callees of the race spine that its bench rows can also have the oracle execute, ported. Transcribed
// from our own disassembly of the player's own images:
//
//   SLUS_010.53  SHA-1 67ed165a2c517d4e6106fb0dfa324d66dd9a76f1, text at 0x80010000
//   RASHCDG.BIN  SHA-1 cfe43a7786759f2cb9c57751cf99e84d1074782c, loaded at 0x8005B5E8
//
// and accepted only by `rrverify phys` rows (tools\rrverify\rows_spine.inc, and the spine rows of
// verify_physics.cpp that now call these instead of the oracle).
//
// Memory model: road_query.h's `GuestRam`, a guest-address view of main RAM. Every function here
// either walks a guest pool (the effect records at 0x800D39B0, entity pool 0, the heap's free list)
// or writes through a record whose address it read one instruction earlier, so an address port
// reproduces the original where a caller-resolves-pointers port could only imitate it (the same
// argument as road_query.h). A load or store the console would not survive is not performed; the
// view records it and the caller fails the call. Nothing here guesses a value.
//
// Callees this file does NOT contain are asked for through the small interfaces below; the bench
// runs the PORTED functions for them (ai.h's command stack, stance.h's stance event) and the product
// must do the same.
#include <cstdint>

#include "game/sim/road_query.h"

namespace rr::sim {

// ---------------------------------------------------------------------------- the addresses
constexpr uint32_t kSpineGameStatePtr = 0x8005B2F8; // -> game_state
constexpr uint32_t kSpinePoolTable    = 0x800CE4D0; // pool 0: +0 base, +4 stride, +0x0C -> s32 high index
constexpr uint32_t kSpinePlayerBikes  = 0x8005B268; // Bike *[2]
constexpr uint32_t kEffectPool        = 0x800D39B0; // 20 x 112-byte effect records
constexpr uint32_t kEffectPoolWalkPtr = 0x800D8068; // the pointer 0x80027178 walks (20 x 112 from it)
constexpr uint32_t kEffectRecordBytes = 112;
constexpr uint32_t kEffectLastStamp   = 0x8005B360; // the race clock of the last burst/spray spawn
constexpr uint32_t kEffectOffsetTable = 0x80053670; // 12-byte local offsets, by effect kind
constexpr uint32_t kCrashFxTable      = 0x800537D8; // 6 bytes per crash class (crash_table_hit's)
constexpr uint32_t kCrashFxPartTable  = 0x800536F0; // 4 bytes per part set
constexpr uint32_t kRCntBasePtr       = 0x800549B8; // -> the root-counter register block
constexpr uint32_t kHeapHeads         = 0x800D6500; // 16 bytes per heap; +0 = the free-list head
constexpr uint32_t kSpineAsinTable    = 0x800527E0; // 61 x u16 (integrator.h Asin)
constexpr uint32_t kSpineAtanTable    = 0x8005285C; // 18 x s32 (fixed.h RatAtan2)
constexpr uint32_t kSpineStanceTable  = 0x800541D4; // 8-byte stance records; +2 = the category
constexpr uint32_t kSpineFightRecPtr  = 0x8005AD4C; // -> 12-byte fight records
constexpr uint32_t kSpinePool0Ptr     = 0x8005B3A0; // -> pool-0 slot 0, stride 1096
constexpr uint32_t kSpineTrafficPool  = 0x800CF660; // pool 3, 512-byte vehicle records
constexpr uint32_t kSpinePropPoolPtr  = 0x800CD6D4; // -> pool 4, 596-byte prop records

// The one peripheral the effect spawners read: root counter 2, through GetRCnt. The bench's machine
// answers every counter load with one constant (`Cpu::rootCounterValue`) and hands
// the same constant here; the product hands its own clock.
struct SpineIo {
    uint32_t rootCounter = 0; // the value a load of 0x1F801100/0x1F801110/0x1F801120 yields
};

// ---------------------------------------------------------------------------- SLUS 0x80043F00
// u32 GetRCnt(u32 spec), 14 instructions. `i = spec & 0xFFFF; if (i < 3) return *(u16*)(*(u32*)
// 0x800549B8 + 16*i); return 0;`. The load is a counter VALUE register on the console: it is answered
// from `io` when the address is one, and through the view (a fault outside RAM) when it is not.
uint32_t SpineGetRCnt(GuestRam& g, uint32_t spec, const SpineIo& io);

// ---------------------------------------------------------------------------- the effect records
// SLUS 0x80027178, 84 bytes: the index 0..19 of the first record whose `(w0 >> 6) & 15` is 0, walking
// from `*(0x800D8068)` in 112-byte steps, or -1. (Every caller then addresses the record from the
// FIXED base 0x800D39B0, not from that pointer.)
int32_t EffectFindFree(GuestRam& g);
// SLUS 0x800271CC, 24 instructions: append record `idx` to entity `e`'s effect chain. An empty chain
// (`(s8)e[+0x49] == -1`) takes `idx` as its head; otherwise the chain is followed through the SIGNED
// 6-bit link in bits 0..5 of each record's word 0 until a link of 63, and that link becomes `idx`.
void EffectLink(GuestRam& g, uint32_t e, uint32_t idx);
// SLUS 0x8002705C, 148 bytes: the jitter of a spray record. rec[+0x38] = (rc & 255) << 4 from one
// GetRCnt(0xF2000002); rec[+0x3A] = 50 when `speed == 0`, 120 when `speed < 18` (signed), else
// (s8)(((255 * (rc2 & 255)) >> 8) - 127) from a SECOND counter read.
void EffectJitterSpray(GuestRam& g, uint32_t rec, int32_t speed, const SpineIo& io);
// SLUS 0x800270F0, 136 bytes: the jitter of a burst record. Two counter reads: rec[+0x38] =
// (r1 & 255) << 4; v = (101 * (r2 & 255)) >> 8; rec[+0x3A] = (v >= 51) ? 50 - v : v.
void EffectJitterBurst(GuestRam& g, uint32_t rec, const SpineIo& io);
// SLUS 0x800289E8, 656 bytes, a leaf: a bike-local offset to world. x = FixMul(e[+0x130], v[0]),
// y = FixMul(e[+0x138], v[1]), z = FixMul(e[+0x134], v[2]) (the two scale words are CROSSED against
// the vector's order), then out[k] = (FixMul(m[k] << 4, x) + FixMul(m[k+3] << 4, y) +
// FixMul(m[k+6] << 4, z) + e[+0xB8 + 4k]) >> 10 with m the s16 matrix at e[+0x1B0]. Every input is
// read before the first output is stored.
void EffectLocalToWorld(GuestRam& g, uint32_t e, uint32_t v, uint32_t out);

// SLUS 0x80027540, 568 bytes: the crash emitter `CrashEmit(e, which, kind)` - players only, at most
// one pending (`(s8)e[+0x08] <= 0`) and at most two live (`e[+0x24] >> 30 < 2`); a record in state 4
// (bits 6..9), `which` in bits 14..21, `kind` at +0x6C, the part pair +0x4C/+0x4E out of the model's
// part list through the two crash tables. Its callees are the two record helpers above.
void CrashEmit(GuestRam& g, uint32_t e, uint32_t which, uint32_t kind);
// SLUS 0x80027778, 508 bytes: `EffectBurst(e, kind, life, tag)` - at most 20 (10 when
// game_state[+0x04] bit 4) per bike in `e[+0x24]` bits 19..22, at most one per race-clock value
// (*(0x8005B360)); state 7, the local offset of `kind` through `EffectLocalToWorld`, lifetime
// 300 >> (that bit), the burst jitter.
void EffectBurst(GuestRam& g, uint32_t e, uint32_t kind, uint32_t life, uint32_t tag, const SpineIo& io);
// SLUS 0x80027974, 524 bytes: `EffectSpray(e, unused, kind)` - players only, never while
// `e[+0x24]` bits 5..6 are set; kind 0 is capped at 4 by bits 19..22 and blocked by bits 23..24;
// state 2, the life clamp(150 - 2 * (s16)e[+0x1E2], 0, 0x960000) and the spray jitter. The second
// argument is never read.
void EffectSpray(GuestRam& g, uint32_t e, uint32_t kind, const SpineIo& io);

// ---------------------------------------------------------------------------- RASHCDG 0x80090270
// void RaceGo(void), 388 bytes, no arguments, no `jal`: the "GO" of the countdown. Pass 1 counts the
// pool-0 bikes that are asleep (`(s16)+0x140 == 0`), not a player and not police; pass 2 gives every
// non-police bike (and every police PLAYER) its first command-stack delay at `+0x3C0`: 1 for a
// sleeping bike, else `((place - count - 1) clamped at 0) >> 1` scaled by 4, or 1 when that is 0 -
// and copies it into the partner's top slot when the owner's `+0x23C` bit 4 says there is one.
void RaceGo(GuestRam& g);

// ---------------------------------------------------------------------------- SLUS 0x8001447C
// void *Malloc(u32 n, u32 heap): 0 for n == 0 or heap >= 2, else SLUS 0x800142B4(n, 0) - the heap
// argument is DROPPED (`move a1,zero` in the delay slot): every allocation comes from heap 0. The
// allocator is first fit over the free list at 0x800D6500: size = (n + 11) & ~7, an exact block is
// unlinked, a larger one is split (the remainder keeps the link), and the block's first word becomes
// its size; the caller gets block + 4. `refused` is set on the out-of-memory arm, which prints through
// the BIOS (SLUS 0x80044894) - the port does not print, it refuses.
uint32_t SpineMalloc(GuestRam& g, uint32_t n, uint32_t heap, bool& refused);

// ---------------------------------------------------------------------------- RASHCDG 0x8008A998
// void ViewEvent(View *v, u32 mode), 280 bytes, a leaf: the camera's reaction to an end-of-race or
// crash event.
void ViewEvent(GuestRam& g, uint32_t view, uint32_t mode);

// ---------------------------------------------------------------------------- SLUS 0x8002090C
// void ResetBikeState(Bike *e), 736 bytes: the bike state reset EndRace, Remount and the step's
// region J run. Releases the contact (SLUS 0x8002076C, inline below), zeroes 50 words of the drive
// state, masks the three flag words (+0x230 &= 0xF8000000, +0x234 &= 0xC0000000, +0x238 &=
// 0xF7B00000), derives +0x258 from the stat block (FixDiv of FixMul(stats[+0x0C], 9.8135) by
// stats[+0x10], signs by hand), +0x250 = stats[+0xBC], +0x34A = Asin((s16)e[+0x212] << 4),
// +0x2DC/+0x2E0/+0x2E4 = 157 x the s16 at +0x206/+0x20C/+0x212, +0x29C = -((25736 x RatAtan2(+0x2DC,
// +0x2E0)) >> 8), the partner's +0x1E8 zeroed when both +0x358 and +0x440 are set, and +0x28C =
// +0x29C + +0x27C. Returns false (with the view faulted or Asin refusing) instead of guessing.
bool ResetBikeState(GuestRam& g, uint32_t e);
// SLUS 0x8002076C, 104 instructions, as the address port the above needs (integrator.h has the same
// function over caller-resolved records; this one walks the handle itself, so a contact that names
// the bike's own slot aliases exactly as on the console).
void ReleaseContactAt(GuestRam& g, uint32_t e);

// ---------------------------------------------------------------------------- RASHCDG 0x800BC7CC
// void StampResult(Entity *e), 272 bytes: what a finished rider's AI stack becomes.
//
//   top = *(u16*)(e + 0x3BC + 8*((s8)e[+0x3B2] - 1));         // the opcode on top, read FIRST
//   if (e[+0x230] & 0x08000000) {
//       r = e[+0x354];
//       if (*(u16*)(0x800541D4 + 8*r[+0x220] + 2) == 3)      // a fight stance
//           StanceEvent(*(u16*)(*(0x8005AD4C) + 12*r[+0x239]), r, 2);   // RASHCDG 0x800C4550
//   }
//   AiClearCommands(e);                                        // RASHCDG 0x800BCD10
//   AiPushCommand({2, 224}, 1, e);                             // RASHCDG 0x800BCA68
//   if (top == 0 || top == 18) AiPushCommand({top, top == 18 ? e[+0xAC] : 224}, 0, e);
//   *(u16*)(e + 0x3B0) = 0;  *(u32*)(e + 0x38C) = 0;
//
// The command record lives in the original's own frame with only its first two halfwords written; a
// push overwrites the other two, and mode 1 compares only the first two, so the stale half never
// reaches memory (the same observation crash.h makes for Remount).
struct StampResultCallees {
    virtual ~StampResultCallees() = default;
    virtual bool StanceEvent(uint32_t ev, uint32_t rider, uint32_t p) = 0;                   // 0x800C4550
    virtual bool ClearCommands(uint32_t e) = 0;                                               // 0x800BCD10
    virtual bool PushCommand(uint16_t op, uint16_t target, int32_t mode, uint32_t e) = 0;    // 0x800BCA68
};
bool StampResult(GuestRam& g, uint32_t e, StampResultCallees& c);

} // namespace rr::sim
