#pragma once
// The rider animation objects in the product: they are the
// ORIGINAL's - made by the race loader's RASHCDI 0x8005D1A0 with its count (0x80063844), handed out by SLUS
// 0x800119C0's loop through ViewSlot 0x80012884 (flags |= 5, in use), all PORTED (sim\anim_objects.h).
//
// The objects must be the original's: an object left at +0x24 = 0 ("free") is handed by ViewSlot to a bike (the
// re-seat 0x8009277C), whose Pose then writes the rider's 17-part clip through the bike's 5-slot part array.
//
// The measurement, every frame (AnimObjFrame): an owner whose animation word (+0x21C, a weapon's +0x22C)
// names an object that names ANOTHER owner - two owners on one object.
//
// DEVELOPMENT switch, the negative control: RRJB_ANIMOBJ=off restores the session's own wiring (OURS).
#include <cstdint>
#include <functional>
#include <string>

#include "game/sim/anim_objects.h"
#include "game/sim/road_query.h"

namespace rr::game {

// False under RRJB_ANIMOBJ=off (read once).
bool AnimObjOn();

// RASHCDI 0x8005D1A0 with the loader's count, its two mallocs placed from `at` by SLUS 0x8001447C's block
// rule (OURS: where), refused past `limit`. Returns the log line; `objects` / `programs` get the blocks.
// `count` < 0: the loader's count rule 0x80063844 here (anim_objects.h); else the count SetUpRace (the race loader) passed.
std::string AnimObjectsSetup(rr::sim::GuestRam& g, uint32_t at, uint32_t limit, uint32_t& objects, uint32_t& programs,
                             int32_t count = -1);

// SLUS 0x800119C0's per-bike loop (PORTED): the stance event through `stance`; the bike's live / dormant arm
// (EntityCell, BuildObb, Transition) is the session's at the end of its set-up (OURS order) and is only
// counted here. Returns the log line.
std::string AnimObjectsStart(rr::sim::GuestRam& g, uint32_t sp,
                             const std::function<bool(uint32_t ev, uint32_t r, uint32_t p)>& stance);

// The frame's measurement over pool 0 / pool 1 / pool 2 and the weapon objects.
void AnimObjFrame(rr::sim::GuestRam& g);

// The run's totals line.
std::string AnimObjTotals();

} // namespace rr::game
