#pragma once
// Which level bundle DATA\<stem>.GEO / .TEX a race loads (docs\formats\rules.md 16.5), transcribed from
// our own disassembly of RASHCDI.BIN (SHA-1 9a8b79d8739713c12b0a2dc4d75ef8f0a2cc0b06, loaded at 0x8005B5E8):
//
//   0x800635D0 calls LoadBikeBank 0x8005C45C(game_state+0x3C). There: a0 = min(a0, 2); race type
//   game_state+4 == 0x2C (Jailbreak): a0 = 4, and 3 when player 1's bike index game_state+0x48 < 9 (a
//   cruiser); the name is the pointer table 0x8006B4A4[a0] = "bblevel1", "bblevel2", "bblevel3",
//   "bblevJBD", "bblevJBK", formatted "%s%s%s" with "DATA\" and ".GEO" / ".TEX" (0x8005CA10 loads both).
#include <cstdint>
#include <cstdlib>
#include <string>

namespace rr {

// The index 0..4 into the name table 0x8006B4A4 for a race of bank `bank` (game_state+0x3C), race type
// `type` (game_state+4) and player 1's bike index `p1Bike` (game_state+0x48).
inline int LevelBankIndex(int32_t bank, uint32_t type, int32_t p1Bike) {
    int a0 = bank > 2 ? 2 : bank;                                     // 0x8005C488
    if (a0 < 0) a0 = 0;                                               // OURS: a negative bank never occurs
    const char* off = std::getenv("RRJB_JBBANK");                     // negative control: "off" = BBLEVEL<n>
    if (type == 0x2Cu && !(off != nullptr && off[0] == 'o')) a0 = p1Bike < 9 ? 3 : 4;   // 0x8005C4A0..0x8005C4C4
    return a0;
}

// "DATA/BBLEVEL1.GEO" .. "DATA/BBLEVJBK.TEX" (the disc's directory spells the names upper case).
inline std::string LevelBankFile(int index, const char* ext) {
    static const char* const kStem[5] = {"BBLEVEL1", "BBLEVEL2", "BBLEVEL3", "BBLEVJBD", "BBLEVJBK"};
    const int i = index < 0 ? 0 : index > 4 ? 4 : index;
    return std::string("DATA/") + kStem[i] + ext;
}

// The same from a game_state image (at least 0x4C bytes).
inline int LevelBankIndexOf(const uint8_t* gs) {
    auto s32 = [gs](int o) {
        return static_cast<int32_t>(static_cast<uint32_t>(gs[o]) | (static_cast<uint32_t>(gs[o + 1]) << 8) |
                                    (static_cast<uint32_t>(gs[o + 2]) << 16) | (static_cast<uint32_t>(gs[o + 3]) << 24));
    };
    return LevelBankIndex(s32(0x3C), gs[4], s32(0x48));
}

} // namespace rr
