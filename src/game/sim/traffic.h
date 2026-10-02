#pragma once
// The two functions that kept the population transitions of the product refused:
// the rider dismount RASHCDG 0x800C3104, which the remount under every retirement reaches, and
// RowsFromHeading RASHCDG 0x8007EC30, which every wake reaches. Ported from our own disassembly of
// the player's own image:
//
//   RASHCDG.BIN  SHA-1 cfe43a7786759f2cb9c57751cf99e84d1074782c, loaded at 0x8005B5E8
//
// Accepted only by `rrverify phys` rows
// (tools\rrverify\rows_traffic.inc: `rider_dismount`, `rows_from_heading`). Memory model:
// road_query.h's guest-address view, as population.h.
#include <cstdint>

#include "game/sim/road_query.h"

namespace rr::sim {

constexpr uint32_t kRiderDismountFn   = 0x800C3104;
constexpr uint32_t kRowsFromHeadingFn = 0x8007EC30;
constexpr uint32_t kAnimStopFnAddr    = 0x8005BE0C; // anim.h AnimMachine::Stop

// RASHCDG 0x8007EC30, 76 instructions, a leaf: the bike's orientation rebuilt from its heading
// +0x1C2..+0x1C6 (forward, into +0x210) and the slice's up vector (+0x154 -> slice +8..+12, negated
// into +0x20A); the right row +0x204 = up x forward (GTE OP, sf = 1, lm = 0); +0x2A4, +0x268,
// +0x27C, +0x2A0, +0x1E8 cleared; the rows copied into +0x1B0..+0x1C0 and the right row into
// +0x32E; the second bike's (+0x358) +0x1E8 cleared. Returns the original's v0 (the u16 at +0x204).
uint32_t RowsFromHeading(GuestRam& g, uint32_t e);

// The dismount's three callees, all PORTED (stance.h, anim.h); the caller runs them on the same memory.
struct DismountCallees {
    virtual ~DismountCallees() = default;
    virtual bool StanceEvent(uint32_t ev, uint32_t r, uint32_t p, uint32_t sp) = 0; // 0x800C4550
    virtual bool StanceLeave(uint32_t ev, uint32_t r, uint32_t p, uint32_t sp) = 0; // 0x800C4500
    virtual bool AnimStop(uint32_t a, uint32_t sp) = 0;                             // 0x8005BE0C
};

// RASHCDG 0x800C3104, 50 instructions, frame 32: rider `r` is taken off its bike. +0x25C = 0,
// +0x260 = 0; a DORMANT rider (+0x140 == 0) leaves its stance (StanceLeave(224, r, 0)), its
// animation object +0x21C is stopped, and +0x220 = 224 (how != 0) or 73 with +0x25C = 4 (how == 0);
// a live one gets the stance event 6 (77 when +0x23C bit 5) with p = 16, unless it already is in
// stance 6 or 77. Returns false when a callee refused.
bool RiderDismount(GuestRam& g, uint32_t r, int32_t how, uint32_t sp, DismountCallees& c);

} // namespace rr::sim
