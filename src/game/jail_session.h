#pragma once
// The Jailbreak mode (the career's venue 5, race type 0x2C) and the two-seat bike (the sidecar: race types
// with bit 3 - Jailbreak and Side Car 0x08 / 0x18) in the product (docs\formats\rules.md 16). Transcribed
// from our own disassembly of the disc's images:
//
//   RASHCDI.BIN  SHA-1 9a8b79d8739713c12b0a2dc4d75ef8f0a2cc0b06, loaded at 0x8005B5E8 (the race loader)
//   RASHCDG.BIN  SHA-1 cfe43a7786759f2cb9c57751cf99e84d1074782c, loaded at 0x8005B5E8 (the race)
//
// The loader parts are transcriptions (no bench: the loader runs once, before any capture); the race
// functions are PORTED in src\game\sim\jail.h with their bench rows. The RaceSession members of this
// domain (race_session.h: ModeEscapeScene, JailPlan, JailSpawn) are defined in jail_session.cpp.
#include <cstdint>
#include <string>
#include <vector>

#include "game/grid_loader.h"

namespace rr::game {

// BuildGrid RASHCDI 0x80067EDC..0x800680D8, the two-rider arm (race type bit 3, not type 17, not the Time
// Trial police grid): with `players` players, t3 = the racing entries - 10 - (type 44 ? 3 : 2) for one
// player (0 for two); t3 > 0 drops t3 entries - the racers from `players` on take the lateral / along of
// the next racing entry t3 further down while the count of racers stays below the same limit, then the
// police entries past the cut are copied down (the loop starts one past the cut, as the original's does).
// Returns a line for the seam list, empty when the arm does not run.
std::string TwoRiderGrid(const uint8_t* gs, int players, std::vector<StartEntry>& grid);

// The animation banks RASHCDI 0x80066414..0x80066594 loads, in its load order, for race type `type`:
// ANIMTBL2 (bank 1), ANIMTBL3 or in type 44 ANIMTBJ3 (2), then with bit 3 the SHORT set ANIMTBSB (4),
// ANIMTBS1 (0), ANIMTBSW (3) and the passenger bank ANIMTBLS or in type 44 ANIMTBLJ (6), else ANIMTBLB (4),
// ANIMTBL1 (0), ANIMTBLW (3); and ANIMTBLP (5) only while *(0x8005B254) = ((type & 0x18) == 0)
// (RASHCDI 0x80063BB4..0x80063BC4).
struct AnimBankName {
    const char* name;
    uint32_t stanceBank;
};
std::vector<AnimBankName> AnimBankSet(uint32_t type);

// Player p's bike index (game_state +0x48 + 4p) is a two-seat machine - a sidecar model 6..8 / 15..17
// (BuildGrid 0x80068344..0x80068370, the loader's own test on +0xB4).
bool SidecarBike(uint32_t bikeIndex);

// Player p's palette (rules.md 16.5): the TSLP handler RASHCDI 0x8005DBB8 copies, per player whose bike index
// gs+0x48 + 4p is below 18, three TSLP blocks from block 6u (a cruiser, index < 9) or 6u + 3 (a sport bike) -
// u = the player record's +0x0A (0x800D81D8 + 36p, `lb`), or p itself in a two-player race - into the next CLUT
// rows and records the first at 0x8006B898 + 4p + 3 (0x8005DD9C); SpawnBike 0x80065A94 gives the player's bike
// that row as its a1, the rider the next, SpawnPassenger 0x800670FC the passenger the third. Returns the TSLP
// block of the bike's palette, -1 for a bike index of 18 or more (no palette copied). `ram` is the arena.
int PlayerPaletteBlock(const uint8_t* ram, int p);

} // namespace rr::game
