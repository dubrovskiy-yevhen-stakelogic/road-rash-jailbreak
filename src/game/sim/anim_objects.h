#pragma once
// The rider animation OBJECTS - who makes them, who hands them out at the start of a race - ported from
// our own disassembly of the player's own images:
//
//   RASHCDI.BIN  SHA-1 9a8b79d8739713c12b0a2dc4d75ef8f0a2cc0b06, loaded at 0x8005B5E8 (the race loader)
//   SLUS_010.53  SHA-1 67ed165a2c517d4e6106fb0dfa324d66dd9a76f1, text at 0x80010000
//
//   RASHCDI 0x8005D130 AnimDescInit     the descriptor 0x800CE170: capacity 10, no objects, slots 0x800CF5D8
//   RASHCDI 0x8005D1A0 AnimObjectsInit  n objects (0x83C) and n programs (60) from Malloc SLUS 0x8001447C,
//                                       each object free (+0x24 = 0) and wired to its program
//   RASHCDI 0x80063670 (its count)      n = *(0x8005B218) + 4 * *(0x8005B254) + 1, +1 more for race type 8,
//                                       +4 more for race type 44 (transcribed, 0x80063774..0x800637B0)
//   SLUS    0x800119C0 RaceStart        the per-bike loop: every pool-0 bike's rider takes an object through
//                                       ViewSlot SLUS 0x80012884 (flags |= 5 - IN USE), BankSwitch on bank 0,
//                                       the stance event 6 / 4; the bike's live / dormant arm; a sidecar
//                                       bike's passenger rider the same on bank 6 (stance 77); then the tail
//
// Accepted only by `rrverify phys` rows (tools\rrverify\rows_animobj.inc).
//
// Why it matters: an object the loader made but ViewSlot never handed out has +0x24 = 0, which is exactly
// what ViewSlot looks for. A host that wires each rider to an object itself and leaves +0x24 at 0 lets a
// later ViewSlot (the re-seat 0x8009277C, a bike's climb object) hand a rider's LIVE object to a bike, and
// the two owners then drive one clip through two part arrays.
//
// What the original does on free and reuse, read here (no reset anywhere): every free site stores
// +0x24 = 0 and decrements the used count +8 (RASHCDG 0x80092AD4 / 0x80093FE4 / 0x80095AEC / 0x800CC0B0,
// RASHCDI 0x800677EC / 0x800694B0 at the race's end); ViewSlot writes only the owner, the part mask +0x6E0,
// flags |= 5 and the program's op byte +1 = 5 - the clip +0x2C and the bank +0x28 stay; a clip is chosen
// again only by a start (HardStart 0x8005BF6C -> Restart 0x8005BD74 -> PoseStart), and AnimationPass
// 0x8005E1D8 poses an object only while +0x24 bit 1 (playing) is set, which ViewSlot does not set.
#include <cstdint>

#include "game/sim/road_query.h"

namespace rr::sim {

constexpr uint32_t kAnimDescInitFn    = 0x8005D130; // RASHCDI
constexpr uint32_t kAnimObjectsInitFn = 0x8005D1A0; // RASHCDI
constexpr uint32_t kRaceStartFn       = 0x800119C0; // SLUS
constexpr uint32_t kRaceStartFrame    = 48;         // `addiu sp,sp,-48` at 0x800119C0
constexpr uint32_t kAnimObjectsFrame  = 40;         // `addiu sp,sp,-40` at RASHCDI 0x8005D1A0
constexpr uint32_t kAoMallocFn        = 0x8001447C; // SLUS Malloc(bytes, heap)
constexpr uint32_t kAoSlotRecords     = 0x800CF5D8; // the descriptor's 10 bank-slot records (12 bytes)
constexpr uint32_t kAoPool1Count      = 0x8005B218; // the riders in pool 1
constexpr uint32_t kAoPedSwitch       = 0x8005B254; // the pedestrians' switch (4 objects each)

// RASHCDI 0x8005D130, a leaf.
void AnimDescInit(GuestRam& g, uint32_t desc);

struct AnimObjectsCallees {
    virtual ~AnimObjectsCallees() = default;
    virtual bool Malloc(uint32_t bytes, uint32_t heap, uint32_t sp, uint32_t& v0) = 0; // SLUS 0x8001447C
};

// RASHCDI 0x8005D1A0(desc, n): `sp` is the stack pointer at its entry. False on a view fault or a
// callee that refused.
bool AnimObjectsInit(GuestRam& g, uint32_t desc, int32_t n, uint32_t sp, AnimObjectsCallees& c);

// RASHCDI 0x80063670's count argument of 0x8005D1A0 (0x80063774..0x800637B0).
int32_t AnimObjectCount(GuestRam& g);

// SLUS 0x800119C0's callees other than ViewSlot / BankSwitch (both PORTED, called natively).
struct RaceStartCallees {
    virtual ~RaceStartCallees() = default;
    virtual bool StanceEvent(uint32_t ev, uint32_t r, uint32_t p, uint32_t sp) = 0; // RASHCDG 0x800C4550
    virtual bool EntityCell(uint32_t e, uint32_t sp) = 0;                          // RASHCDG 0x8008B99C
    virtual bool BuildObb(uint32_t e, uint32_t sp) = 0;                            // RASHCDG 0x8008BA18
    virtual bool Transition(uint32_t e, uint32_t to, uint32_t sp) = 0;             // RASHCDG 0x80093F94
    // The tail (0x80011B18..0x80011C24), each by its address with its arguments.
    virtual bool Tail(uint32_t fn, int n, uint32_t a0, uint32_t sp) = 0;
};

struct RaceStartCounts {
    uint32_t riders = 0, passengers = 0, noObject = 0, switched = 0, live = 0, dormant = 0;
};

// SLUS 0x800119C0's per-bike loop [0x800119EC, 0x80011B18): `sp` is the stack pointer at the FUNCTION's
// entry (the callees are called at sp - 48).
bool RaceStartBikes(GuestRam& g, uint32_t sp, RaceStartCallees& c, RaceStartCounts* counts = nullptr);

// SLUS 0x800119C0 whole: the loop, then the tail through `c.Tail`.
bool RaceStart(GuestRam& g, uint32_t sp, RaceStartCallees& c);

} // namespace rr::sim
