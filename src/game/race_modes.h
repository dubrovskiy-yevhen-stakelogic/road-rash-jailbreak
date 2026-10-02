#pragma once
// The game modes in the product (docs\formats\rules.md 2 and 9): what the race
// loader does per race type, transcribed from our own disassembly of RASHCDI.BIN (SHA-1
// 9a8b79d8739713c12b0a2dc4d75ef8f0a2cc0b06), and the session's side of the PORTED mode functions
// (src\game\sim\modes.h; the RaceSession members are defined in race_modes.cpp).
//
// What each mode gets, by the byte game_state+0x04 the commit RASHCDF 0x8007F37C wrote:
//   bit 0 (Five-O = 1, the career's venue 3 = 33, 2P = 17)  the race time limit (0x80053084 / 0x80053090 by
//        bank, seconds x 300); player 1 on a police bike rides under AI control, its arrest word, quota,
//        designated suspect and start place (the player-cop chain of modes.h)
//   36   the time limit gs+0x08 seconds;  33 / 44  the time limit 0x8005ADE0 / 0x8005ADE4 seconds
//   44   (the Jailbreak venue) the milestone phase gs+0x39 = 0, gs+0x0A = 0, the police off, the route's
//        direction sign 0x8005B2E8 from milestone 0
//   bit 2 (Time Trial = 4, and 0x24 / 44)  the grid: gs+0x38 == 0 -> the player alone (two with bit 3),
//        == 2 -> the player and two police bikes (four with bit 3); otherwise the file's entries
#include <cstdint>
#include <string>
#include <vector>

#include "game/grid_loader.h"
#include "game/sim/integrator.h" // BikeTables
#include "game/sim/modes.h"
#include "game/sim/road_query.h"

namespace rr::game {

// BuildGrid RASHCDI 0x80067C1C..0x80067CA8 and 0x80067EB4..0x80067ED8: the entries the race uses, from
// the start block `grid` (StartEntry, file order) and game_state `gs`. Truncates / rewrites `grid` in
// place; returns a line for the seam list (empty when the race type leaves the file's grid alone).
std::string ModeGrid(const uint8_t* gs, std::vector<StartEntry>& grid);

// SetUpRace RASHCDI 0x80063670..0x800637A0 (the time limit, the clock triple) and BuildRace
// 0x80069834..0x80069888 (race type 44), then the start-position record 0x800CF578 SetUpRace fills through
// 0x8006ACD0 (its three words only; the two road lookups it then makes are not run). On the arena `g`.
std::string ModeSetUpRace(rr::sim::GuestRam& g);

// SpawnBike's per-race-type stores on a bike already seeded by the session (0x80066248: a player gets
// +0x230 bit 27 in a race type with bit 0; 0x80066858..0x800668E8 and 0x8006581C: player 1 on a police
// bike there - the arrest word |= 1, the start place PlayerCopPlace, the quota 0x8005AD44 = gs+0x07 and the
// designated suspect's bit of 0x800D8708; 0x80066BE8..0x80066C28: the first command {1 or 4, 224}).
// `tables` feeds AxisRotation (PlayerCopPlace's facing, 0x800656A0). Returns a line for the seam list.
std::string ModeSpawnBike(rr::sim::GuestRam& g, uint32_t e, bool player, const rr::sim::BikeTables& tables);

// RASHCDI 0x8006581C PlayerCopPlace(e), frame 40: the player cop's start - with a Rand bit the lateral is
// kept, 0x8005B2B0 = 0 and 0x800656A0 turns the bike ~9.7 degrees toward the road (answer 1: its first
// command is 1, it waits behind the field); else it is put at the road piece's edge, 0x8005B2B0 = 1
// (answer 0). Then the box centre from the slice, the quota and the suspect's bit.
uint32_t PlayerCopPlace(rr::sim::GuestRam& g, uint32_t e, const rr::sim::BikeTables& tables);

} // namespace rr::game
