#pragma once
// The memory-card save of the career, as a raw 128 KiB PlayStation card image (the format of the
// player's own `.srm` / `.mcr`) in a file next to rrgame.exe. frontend.md 8 is the format; every rule
// below carries its address there. What is PORTED: the block layout (header template RASHCDF
// 0x80080E8C, slot index, records table, ten 484-byte career records), SaveSlot 0x8006CDA4 /
// LoadSlot 0x8006CE60's memory copies, the per-record checksum 0x8005E9D0 and the directory frame
// 0x8005E858. What is OURS: the file (the original talks to BIOS libcard). The card screens themselves
// run PORTED (shell_memcard.h) over this file through its CardDevice; SaveCareer / LoadCareer remain for
// `rrshell cardcheck`.
//
// `rrshell cardcheck <card>` recomputes the checksums of a real card and compares every one.
#include "game/sim/road_query.h"

#include <cstdint>
#include <string>
#include <vector>

namespace rr::shell {

using rr::sim::GuestRam;

constexpr uint32_t kCardBlockPayload = 6948;          // WriteFile's a2 at 0x8005F1BC
constexpr uint32_t kCardRecordBytes = 484;            // 0x1E4
constexpr uint32_t kCardRecordsAt = 0x834;            // block offset of career slot 0
constexpr char kCardFileName[] = "BASLUS-01053ROADRASH"; // 0x8005B678

struct CardImage {
    std::vector<uint8_t> raw = std::vector<uint8_t>(128u * 1024u, 0); // 16 blocks of 8 KiB
    int block = -1; // the data block that holds the game's file, -1 = none
};

// A fresh formatted card (every directory frame free, frame XORs set) - the state of an empty
// `.mcr`.
CardImage FormatCard();
bool LoadCardFile(const std::string& path, CardImage& card, std::string& error);
bool SaveCardFile(const std::string& path, const CardImage& card, std::string& error);

// Finds the game's file on the card (directory frame state 0x51, name kCardFileName).
int FindGameBlock(const CardImage& card);

// The whole save as frontend.md 8.3 lays it out, created with the template header (from the arena's
// RASHCDF image) and all ten slots EMPTY (rec[0] = 1) and valid, if the card has none yet.
bool EnsureGameFile(CardImage& card, GuestRam& g, std::string& error);

// SaveSlot RASHCDF 0x8006CDA4: player records (216 bytes of 0x800D81D8) and session (256 bytes of
// 0x800D80D8) into career record `slot`, slot marked in use, block +0x200 = slot, checksums.
bool SaveCareer(CardImage& card, GuestRam& g, int slot, std::string& error);
// LoadSlot RASHCDF 0x8006CE60's copies back into guest memory (the mode re-dispatch that follows is
// the caller's). False when the slot is empty or its checksum does not hold.
bool LoadCareer(const CardImage& card, GuestRam& g, int slot, std::string& error);

// Every career record's checksum pair recomputed and compared with what the card stores.
std::string CheckCard(const CardImage& card, int* mismatches);

} // namespace rr::shell
