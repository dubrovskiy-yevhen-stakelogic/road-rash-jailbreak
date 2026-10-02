// The two-player arena by the original's heap order.
//
// BuildGrid RASHCDI 0x80067B00 (RASHCDI SHA-1 9a8b79d8...) mallocs, in this order and nothing between:
// the stat array 448 * (players == 1 ? 4 : 5) (0x80067BD4), pool 0 (0x80067D7C), pool 1 (0x80067DAC) - through
// SLUS 0x8001447C / 0x800142B4 (SLUS_010.53 SHA-1 67ed165a...), a first-fit list whose block is (n + 0xB) & ~7
// bytes with the size word in front. rr-race's heap walk shows the three blocks back to back (0x801B5EC8 1800,
// 0x801B65D0 19736, 0x801BB2E8 11312) and no hole before them, so the stat array's block ENDS where pool 0's
// block starts in a one- and a two-player race alike: with five blocks it starts 0x1C0 lower. The product keeps
// pool 0 where rr-race has it, so the array's place is pool 0 minus its block - the route heap below it
// (route_product.cpp, the original's GRF / route / map / TOC blocks) ends there.
//
// RRJB_MPARENA=off (the negative control) puts the array in one of the session's spare 16 KiB blocks instead -
// the only one left, the one the sound world's heap takes - so a two-player race then has no attached sound world.
#pragma once

#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace rr::game {

constexpr uint32_t kMpArenaPool0 = 0x801B65D4u; // rr-race's pool 0 (race_session.cpp kArenaPool0)

// SLUS 0x800142B4: the block of a malloc(n) is (n + 0xB) & ~7 bytes, its user pointer 4 past its start.
constexpr uint32_t StatArrayBlockBytes(uint32_t players) { return (448u * (players == 1u ? 4u : 5u) + 0xBu) & ~7u; }
// the array's user pointer: pool 0's block start (kMpArenaPool0 - 4) minus the array's block, plus 4
constexpr uint32_t StatArrayAt(uint32_t players) { return kMpArenaPool0 - StatArrayBlockBytes(players); }
static_assert(StatArrayAt(1) == 0x801B5ECCu, "rr-race's stat array");

inline bool MpArenaOriginal() {
    const char* v = std::getenv("RRJB_MPARENA");
    return v == nullptr || std::strcmp(v, "off") != 0;
}

// RoadNote's effect spawners run on player 2's bike (the race's EffectSpawner; 0 in a one-player race)
struct MpSoundCounts {
    size_t p2Bursts = 0, p2Sprays = 0;
};
inline MpSoundCounts& MpSoundCounters() {
    static MpSoundCounts c;
    return c;
}

} // namespace rr::game
