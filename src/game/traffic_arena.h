#pragma once
// The traffic's part of the race arena: what the race loader leaves in RAM for the car spawner, the
// model binder and the traffic pass. RASHCDI.BIN (SHA-1
// 9a8b79d8739713c12b0a2dc4d75ef8f0a2cc0b06, loaded at 0x8005B5E8) is not ported; the pieces below are
// TRANSCRIBED from our own listing of it and run on the player's own disc files:
//
//   * 0x8005BE40 / 0x8005D018: the model-family tables 0x800D4C38 (+16 kind: count, used, base id,
//     -> the s16 slot-by-id table, all -1 first), the per-kind class lists 0x800CE560 (+8 kind: s16
//     count 0, s16 first slot -1, -> the list, capacities from the signed bytes at 0x8006B49C), and
//     the model registry 0x800CE1B0 cleared (50 x 16);
//   * the level bundle's texture sections (RASHCDI 0x80061C24 dispatches type 8 to 0x8006270C and
//     type 9 to 0x8006275C, both walk the container with 0x8005C920): their CTKP chunks run
//     0x8005BDAC (the class lists: {s16 kind, _, u16 model id, u16 texture id}); the LECT / KNBP / TSLP
//     arms upload to VRAM and are NOT run (the product has no VRAM; named);
//   * the car file (RASHCDI 0x8005C630: "DATA\" "car" <game_state+0x40 as two digits> <"a" one player,
//     "b" two> ".GEO") loaded as 0x8005CA10 does: per RMD3 chunk the walker 0x8005C0C4 with the RMD3 /
//     DOD3 / DPD3 / BBD3 handlers 0x8005CB9C / 0x8005CC4C / 0x8005CD60 / 0x8005CE78 (registry slot,
//     LOD table, part-pointer arrays, the DOD3 region offsets made absolute, DPD3 +0x14 made absolute),
//     then 0x8005C054 (the registry's +7 texture key: the page table at *(0x8005B2E4) is empty here,
//     so -1), 0x8005BD80 (the kind's first slot) and SLUS 0x800303BC (slot by id, used + 1);
//   * 0x80068D54's pool-3 part: the 16 car slots cleared keeping each one's +4 part array, the control
//     {live 0, next 0, high -1}, the traffic block 0x800D8710 (+4/+12 per player 16 / 4, +0x14 16,
//     +0x18 4, +0x1C 180, +0x20 145, +0x24/+0x26/+0x28 100 / 75 / 50).
//
// OURS, named: where the file and the mallocs land (a bump region of the arena, not the console's
// heap), the 16 part arrays (32 bytes each, BuildGrid mallocs them), pool 3's entry in the pool table
// 0x800CE4D0 and the control's +0x0C capacity 16 (both from the pool allocator, not transcribed).
// Only the CAR file is loaded (the bikes, riders, pedestrians and props of the other registry slots
// are not), so the car models take slots 0.. instead of the captures' 8..; `--trafficarenacheck`
// compares by model id.
#include <cstdint>
#include <string>
#include <vector>

#include "game/sim/road_query.h"
#include "rrvfs/disc_image.h"

namespace rr::game {

struct TrafficArenaReport {
    bool ok = false;
    std::string error;
    std::string carFile;
    uint32_t fileAt = 0, end = 0;
    std::vector<uint32_t> models;  // registry order
    int classCars = 0;             // *(s16 *)0x800CE578
    int ctkpChunks = 0;
    int texArmsSkipped = 0;        // LECT / KNBP / TSLP chunks not run
};

// Builds the regions above in `g` for race `raceId` of a `players`-player race. `from` is the first
// free guest address the builder may use (advanced past what it used), `limit` one past the last.
TrafficArenaReport BuildTrafficArena(rr::sim::GuestRam& g, const rr::DiscImage& disc, int raceId, int players,
                                     uint32_t& from, uint32_t limit);

// The check: builds the arena for the race a captured 2 MiB image names (game_state +0x40, +0x30) on a
// blank image and compares, by model id, what the image holds. `mutate` leaves the DOD3 region
// relocation out (the negative control). Returns 0 when everything compared equal; prints the counts.
// RASHCDI 0x8005CA10's RMD3 loop over a .GEO file placed in the arena from `from`
// (DATA\HAZARD<n>.GEO, world_pop_product.h). False when there is no room or the transcription refused.
bool LoadGeoIntoArena(rr::sim::GuestRam& g, const std::vector<uint8_t>& geo, uint32_t& from, uint32_t limit,
                      std::vector<uint32_t>& models, uint32_t* fileAt = nullptr);

int CheckTrafficArena(const rr::DiscImage& disc, const std::string& ramPath, bool mutate);

} // namespace rr::game
