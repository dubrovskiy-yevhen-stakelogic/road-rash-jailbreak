#pragma once
// The race loader's grid rules this product transcribes (RASHCDI.BIN SHA-1
// 9a8b79d8739713c12b0a2dc4d75ef8f0a2cc0b06, loaded at 0x8005B5E8): the start block, SpawnBike's AI-index
// rule, and the police bikes the grid
// file lists (every entry with slot >= 17), which the loader spawns DORMANT.
#include <cstdint>
#include <string>
#include <vector>

#include "game/sim/road_query.h"
#include "rrvfs/disc_image.h"

namespace rr::game {

// DATA\STARTDF<A|B>.BIN block raceId - 1 (292 bytes): u32 n, then n x {s32 slot, s32 lateral, s32 along}
// BuildRace RASHCDI 0x80069944..0x80069974 copies it onto its stack.
struct StartEntry {
    int32_t slot = 0, lateral = 0, along = 0;
};
bool ReadStartBlock(const DiscImage& disc, int set, int32_t raceId, std::vector<StartEntry>& out, std::string& why);

// SpawnBike RASHCDI 0x80065A94, the AI index and the LEVEL<n>.BI record of one grid entry
// (0x80065B50..0x80065DF0, and for a player 0x80065FD4..0x800660B0). `mask` is BuildGrid's word at its
// sp+2304 (0 before the first entry), updated as the original updates it.
//   a player (`player` = p >= 0):  ai = p; record 26 for class 2, else 2 (P[p]+10 - 1) + class + 22 when
//          the player record's byte +10 (0x800D81D8 + 36p) is positive, else the class (= game_state's
//          bike word +0x48 + 4p / 9); riderDef+1 = class | PlayerColumn 0x80065648 << 4, the column 8
//          below 2 (a second player of the same class moves one column), and mask bit 10*class + column;
//   a police slot (>= 17):        ai = record = slot + 1, no mask bit;
//   otherwise:                    v = slot + 1: column v >> 1, class v & 1, the column stepped past taken
//          bits (and the slot past 9 columns), mask bit 10*class + column, ai = column + 8*class + 1; the
//          record is the same but for race type 33 (player 1's model < 9 picks the class), an odd race
//          type with *(0x8005B1F8) >= 3 and the Jailbreak-class types 4 (not 44) (player 1's class).
// `g` views the arena: game_state (0x8005B2F8), the player records 0x800D81D8, player 1's bike 0x8005B38C.
struct SpawnIndex {
    uint32_t ai = 0, record = 0;
    int32_t riderDef1 = -1;   // a player's riderDef+1 (class | column << 4), else -1
    int32_t columnClass = -1; // a racer's grid class bit (SpawnBike stores it at 0x8006B8A0[handle], 0x80065C94)
};
SpawnIndex SpawnAiIndex(rr::sim::GuestRam& g, int32_t slot, int player, uint32_t& mask);

// ---- the rider records: what BuildGrid RASHCDI 0x80067B08 leaves in the twenty 72-byte records
// 0x800D5758 + 72 * AI index and in the AI index maps, for the bikes it spawns in grid order.
// 0x80067B40 / 0x80067B94 fill the maps with 31; per seat SpawnBike 0x80065A94's rider-record part, in its
// order: the AI index and record (SpawnAiIndex), the grid class byte 0x8006B8A0[handle] (a racer), the maps
// and the bike's +0x43C = 0x800D5758 + 72 ai (0x80065DC4..0x80065DF0), the PORTED RiderRecordInit
// 0x80064C0C on the LEVEL<n>.BI buffer, the appearance of record 18 for a rider of the other class in race
// types 33 / 44 (0x80065E18..0x80065EA4), the PORTED RiderNameId 0x80066E1C into +0x26, the race-type-8
// weapon strip (0x80065F5C..0x80065FC4: mask &= 0xFECD, weapons 1 / 4 / 5 / 8 back to fists), a player's
// +0x01 (0x80066004..0x80066020), a player's countdown bits +0x00 |= 0x60 outside race types & 1
// (0x80066634), a type-33 player's class 2 (0x80066678), a police rider's weapon 2 when its mask holds
// bit 2 outside type 33 (0x80066D5C..0x80066D90); then per bike the PORTED GridRiderAdjust 0x800650A0
// (0x80068240). NOT here: a player's appearance 0x8005DD9C (0x80065EF0), the model init 0x8002FAD4
// (which replaces bike +0x4C with 1.0), the initial place 0x800138E8 (0x8006822C) - the caller's.
// `bi` is the file (0x80064B44 reads exactly 1728 bytes); the loader reads it from guest RAM, so it is
// copied to `biAt` (BuildGrid's stack, sp+256, in the original) and cleared again afterwards; the bytes
// RASHCDI's table 0x8006B8A0 overwrites are restored (on the console RASHCDG is loaded over it).
constexpr uint32_t kLevelBiBytes = 1728;
struct RiderSeat {
    uint32_t entity = 0;   // the pool-0 bike, its handle +0xAC set
    int32_t slot = 0;      // its grid entry's slot
    int player = -1;       // the player index for a player's bike, else -1
    uint32_t ai = 0, record = 0, riderDef = 0; // out
};
bool LoadRiderRecords(rr::sim::GuestRam& g, const std::vector<uint8_t>& bi, uint32_t biAt,
                      std::vector<RiderSeat>& seats, std::string& note);
// `rrgame --ridercheck <ram|dir>`: for every race RAM image, the loader above re-run on the image's own
// grid (the STARTDF block of its race, its pool in spawn order) over a copy whose rider records, maps,
// +0x43C and appearance words were wiped, compared byte for byte with the image where the loader's value
// survives the race; the shell image (no race) is refused. `mutate`: the file one record out of step.
bool CheckRiderRecords(const DiscImage& disc, const std::string& path, bool mutate, std::string& report);

// ---- the police bikes (a non-player entry whose LEVEL<n>.BI record has class 2: slots 17 / 18 -> records
// 18 / 19, class nibbles 0x12 / 0x22 on the USA disc), as the loader leaves them.
// SpawnBike's stores on a police bike, on a pool-0 bike the session seeded like a racer (before the rider
// records): the command stack {1, 224} at depth 1 (0x80066C10..28, the loader's command for every bike,
// which only the police keep), +0xB0 = -1 on bike and rider (0x80066D44/48), +0x366 = 0 (0x80066DE4), the
// rider's +0x220 = 224, +0x239 = 41, +0x23A = 0, +0x23B = 0xFF, +0x23C = 0, +0x22C = 0, +0x25C = 0 (race
// type bit 0 clear), then the tail 0x80066DD4..E0: the live word +0x140 = 0 and +0x3A0 |= 0x30.
void SpawnCopBike(rr::sim::GuestRam& g, uint32_t e);
// SaveCopRecord RASHCDI 0x800644C8 (after the stat blocks): the record 0x800D86F0 +8 stats[+0xE0], +12
// .. +18, +20, +22 from the riderDef, +19 = 1, +24 = 0 - a non-player class-2 bike only.
void SaveCopRecord(rr::sim::GuestRam& g, uint32_t e);
// BuildRace's police census RASHCDI 0x80068684 (0x80068614(2)): 0x800D86F4 = the class-2 bikes of pool 0,
// 0x800D86F0 = those with a live word.
void PoliceCensus(rr::sim::GuestRam& g);

} // namespace rr::game
