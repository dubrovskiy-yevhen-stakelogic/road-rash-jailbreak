#pragma once
// src\game\sim\traffic_bind - the model binder the car spawner calls, the pool release, the cop
// effect join / leave and the rider-object release (the traffic domain). Transcribed from our own
// disassembly of the player's own images:
//
//   SLUS_010.53  SHA-1 67ed165a2c517d4e6106fb0dfa324d66dd9a76f1, text at 0x80010000
//   RASHCDG.BIN  SHA-1 cfe43a7786759f2cb9c57751cf99e84d1074782c, loaded at 0x8005B5E8
//
// and accepted only by `rrverify phys` rows (tools\rrverify\rows_traffic_bind.inc).
//
// Memory model: road_query.h's `GuestRam`. Every function here walks guest records whose addresses
// it read one instruction earlier (the model registry, a model's LOD table, the effect records, the
// pools' control blocks), so they run on the guest addresses themselves. A load or store the console
// would not survive is not performed; the view records it and the caller fails the call.
//
// No `sp`: every callee these functions reach is ported (here, or spine.h's Malloc / effect helpers,
// population.h's CopDrop, anim.h's SeatRelease, the LCG), and none of them keeps a word in its own
// frame that a later read uses. Every Rand draw is made where the original makes it.
#include <cstdint>

#include "game/sim/road_query.h"

namespace rr::sim {

// ---------------------------------------------------------------------------- the addresses
constexpr uint32_t kModelRegistry    = 0x800CE1B0; // 50 x 16: +0 id (0 = free), +4 u8 LOD count,
                                                   // +7 s8 default table key, +8 -> LOD table (12 bytes:
                                                   // +0 DOD3, +4 -> part pointers)
constexpr uint32_t kModelClassLists  = 0x800CE560; // + 8 pool: s16 count, +4 -> {s16 model, s16 key}[]
constexpr uint32_t kModelPoolTables  = 0x800D4C38; // + 16 pool: +8 s32 base id, +12 -> s16 slot by id - base
constexpr uint32_t kModelBindFlags   = 0x80054144; // u32 by pool: the binder's "key 0 lookup" flag
constexpr uint32_t kModelKeyPool1    = 0x80054114; // u8 by class: pool 1's table key
constexpr uint32_t kModelKeyPool2    = 0x8005412C; // u8 by class: pool 2's table key
constexpr uint32_t kModelKeyTablePtr = 0x8005B2E4; // -> 34 x 12-byte records, +0 u8 key, +10 u16
constexpr uint32_t kSeatBitsWord     = 0x8005AD50; // the seat bits SeatFree / ReleaseRiderObject clear
constexpr uint32_t kSeatRecords      = 0x800CF018; // 172-byte records, by the object's s8 seat index
constexpr uint32_t kAnimUsedCount    = 0x800CE178; // 0x800CE170 +8: the animation objects in use
constexpr uint32_t kPoolFreeScan     = 0x800D1814; // the word every release loop reloads and stores
constexpr uint32_t kPoolLowFree      = 0x800D1660; // s32: the lowest free index of pools 4 / 5's store
constexpr uint32_t kPoolStoreBase    = 0x800D1664; // 24-byte store records (index = (p - base) / 24)

// ============================================================================ the model binder (SLUS)

// SLUS 0x8001298C, 88 instructions, a leaf: LodSelect(obj, lod). Returns the old (s8)obj[+8]. When
// `lod` differs and is below the registry record's LOD count (signed compare), the LOD becomes
// current: obj[+8] = lod, obj[+0] = its DOD3, obj[+40] = DOD3 +16, the part pointers into the part
// array (24-byte stride), and a kind-5 model leaving LOD 0 or 4 gets part 0's rotation identity.
int32_t LodSelect(GuestRam& g, uint32_t obj, uint32_t lod);

// SLUS 0x800302C4, 62 instructions, a leaf: ModelKeySet(obj, key). obj[+74] = the registry record's
// s8 +7 (key 0) or the index of the first of the 34 records at *(0x8005B2E4) whose byte +0 is `key`
// (0xFFFF when none). Returns -1 when obj[+74] is -1; otherwise 0, re-deriving obj[+36] bits 12..17
// from the record's u16 +10 when obj[+36] & 0x3F400 is 0x3F400.
int32_t ModelKeySet(GuestRam& g, uint32_t obj, uint32_t key);

// SLUS 0x8002FDEC, 310 instructions, frame 40: RegistryBind(obj, slot, alloc, keyFlag). obj[+96] =
// the registry record of `slot` (slot < 50, id word non-zero) or 0 (-> -1). LOD = count - 1 (+8, +10,
// +11), obj[+0] = its DOD3, `alloc` != 0 mallocs the part array (the first LOD's part count x 24,
// heap 0) into obj[+4] (0 there -> -1); the parts and the per-object state are reset; the DOD3 kind
// ((u16 +14 & 0x78) >> 3) picks obj[+100] and obj[+36]'s bits (kind 3 draws Rand % 15 for every
// model but 300, 315 and 309). obj[+74] = -1, and `keyFlag` != 0 runs ModelKeySet(obj, 0). Returns
// 0 or -1 in `v0`; false when the view faulted or the allocator ran out of memory (the original
// prints through the BIOS there).
bool RegistryBind(GuestRam& g, uint32_t obj, uint32_t slot, uint32_t alloc, uint32_t keyFlag, int32_t& v0);

// SLUS 0x8002FAD4, 198 instructions, frame 56: ModelBind(obj, pool, cls, alloc). The registry slot of
// the model class `cls` of `pool` (1..6): pool 1 / 2 by the class, pool 3 from its class list (0xFFFF
// draws Rand % (count - (race type & 1)), or count - 2 in race type 44), pool 4 always by a Rand draw
// from its list, pools 5 / 6 the table's first. RegistryBind(obj, slot, alloc, *(0x80054144 + 4 pool)),
// then pool 1 / 2 / 4 set the table key, pools 5 / 6 select LOD `cls`. `v0` = the class (pool 3 / 4:
// the list entry's model id - the pool's base id), or 0xFFFF on a refusal. False on a fault / OOM.
bool ModelBind(GuestRam& g, uint32_t obj, int32_t pool, uint32_t cls, uint32_t alloc, uint32_t& v0);

// ============================================================================ the cop effects (SLUS)

// SLUS 0x8002A738, 107 instructions, frame 40: EffectUnlink(e, prevRec, rec, link). Undoes the count
// that record `rec`'s state ((w0 >> 6) & 15) holds in e[+0x24] (states 1 / 3: bits 25..26; 2: bits
// 23..24 when (w0 >> 14) & 255 is 2, else 19..22; 4: bits 30..31; 7: bits 19..22; 5: CopDrop(e)),
// frees the record (byte +60 = 0, state and bits 10..13 cleared, link 63) and puts `link` where the
// record was: in `prevRec`'s low 6 bits, or e[+0x49] when `prevRec` is 0.
void EffectUnlink(GuestRam& g, uint32_t e, uint32_t prevRec, uint32_t rec, uint32_t link);

// SLUS 0x8002847C, 46 instructions, frame 40: CopLeave(e). Every record of e's effect chain (head
// (s8)e[+0x49], 112-byte records at 0x800D39B0, signed 6-bit links, -1 ends) is unlinked in turn;
// the link is read before the record is freed.
void CopLeave(GuestRam& g, uint32_t e);

// SLUS 0x80028034, 118 instructions, frame 48: CopJoin(e). Unless e[+0x24] bit 27 is set: 4 effect
// records (DOD3 kind 2: 6, from e[+180] >= 18 clamped to 18..20) in state 5 with the entity's
// offsets from the SLUS byte pairs at 0x800536E8 (kind 2: 0x800536DC), each linked to e's chain;
// then e[+0x24] |= bits 27..29. Kind 2 with e[+180] < 18 (unsigned) does nothing.
void CopJoin(GuestRam& g, uint32_t e);

// ============================================================================ the release (RASHCDG)

// RASHCDG 0x800CB84C, 31 instructions, frame 24: SeatFree(o). When (s8)o[+0x237] is not -1 and its
// bit is set in *(0x8005AD50): the bit is cleared and SeatRelease(o, 0x800CF018 + 172 idx, 0).
bool SeatFree(GuestRam& g, uint32_t o);

// RASHCDG 0x800CC0B0, 23 instructions, frame 24: ObjectFree(o). o[+0x21C]'s animation object is
// released (its +0x24 = 0, the used count 0x800CE178 - 1, o[+0x21C] = 0); byte o[+0x236] == 0 then
// runs SeatFree(o).
bool ObjectFree(GuestRam& g, uint32_t o);

// RASHCDG 0x8008C000, 279 instructions, frame 40: PoolRelease(h, pool). The entity whose handle is
// at `h` (slot = *h & 31) leaves pool 2..6: pool 2 frees its object (ObjectFree), pool 3 a cop car
// (handle +8 == 0 and +0x24 bit 27) leaves its effects (CopLeave, CopDrop); the pool's live count
// drops (not below 0), its high index walks down past empty slots, its lowest-free index takes the
// slot; pools 4 / 5 also free their store record (and 0x800D1660 its index). The handle becomes 0.
// Any other pool does nothing (the handle stays).
bool PoolRelease(GuestRam& g, uint32_t h, int32_t pool);

// RASHCDG 0x80095AEC, 67 instructions, frame 32: ReleaseRiderObject(r). When (s8)r[+0x23B]'s bit is
// set in *(0x8005AD50): cleared, SeatRelease(r, 0x800CF018 + 172 idx, 0), r[+0x22C]'s animation
// object released, a set bit 7 of r[+0x23C] makes the seat record leave its effects (CopLeave) and is
// cleared; r[+0x22C] = 0, r[+0x23B] = -1.
bool ReleaseRiderObject(GuestRam& g, uint32_t r);

} // namespace rr::sim
