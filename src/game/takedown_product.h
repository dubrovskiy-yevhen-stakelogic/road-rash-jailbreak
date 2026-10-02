#pragma once
// The takedowns in the product (sim\takedown.h): these calls run PORTED on the session's arena:
//   * RASHCDG 0x800BF51C Takedown under RiderKnockOff (the rider layer's knock-off): a knock-off within 240
//     ticks of a player's blow credits the player - FightStat x2 and the dash flash 0x800D6224;
//   * SLUS 0x80018440 RiderOffSound(h, mode) - the knock-off's mode 7 and the recovery's mode 0 (the two
//     voices stopped) - on the sound runtime's world;
//   * SLUS 0x800273EC ObjectEffect (weapon.h, row weapon_object_effect) for the riding lean 0x800C3950 in
//     the presentation pass;
//   * SLUS 0x8001B3C8 BustedMusic under the police arrest (cop_race.cpp): the busted line of AUDTAUNT.STR.
// DEVELOPMENT switch, the negative control: RRJB_TAKEDOWN=off makes all four named seams (no effect).
#include <cstdint>
#include <functional>
#include <string>

#include "game/audio/sound_runtime.h"
#include "game/sim/road_query.h"

namespace rr::game {

using TakedownNote = std::function<void(const std::string&)>;

// False under RRJB_TAKEDOWN=off (read once).
bool TakedownOn();

// Each returns the original's v0 where it has one (0 otherwise); faults are cleared and named via `note`.
uint32_t ProductTakedown(rr::sim::GuestRam& g, uint32_t B, const TakedownNote& note);
uint32_t ProductRiderOffSound(SoundRuntime& s, uint32_t h, uint32_t mode, const TakedownNote& note);
uint32_t ProductLeanEffect(rr::sim::GuestRam& g, uint32_t e, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4,
                           const TakedownNote& note);
bool ProductBustedMusic(SoundRuntime& s, const TakedownNote& note);

// The run's totals (one line), for rrgame's seam report.
std::string TakedownTotals();

} // namespace rr::game
