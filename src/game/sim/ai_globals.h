#pragma once
// DATA\GLOBALS.BI - the AI and difficulty file - put where the race loader puts it.
//
// RASHCDI 0x80064610 (1332 bytes; RASHCDI.BIN SHA-1 9a8b79d8739713c12b0a2dc4d75ef8f0a2cc0b06, loaded at
// 0x8005B5E8) reads exactly 2732 bytes of the file onto its stack and scatters them with 46 calls of
// SLUS 0x8001E0B4 (memcpy by words): 41 contiguous copies of file 0x000..0x2DB into resident globals
// (0x8005ADC0.., SLUS 0x80052EE4..0x80053187), ONE of the 32 60-byte difficulty profiles at
// 0x2DC + 60 * row into SLUS 0x800531AC (the selector 0x800649CC..0x80064A9C), and five copies of the
// tail 0xA5C..0xAAB (0x8005ADCC 8, 0x8005ADD4 8, 0x8005ADDC 4, 0x800531E8 40, 0x80053210 20). The copy
// list is transcribed from our own listing of the loader (the `lui a0 / addiu a0 / addiu a1,sp,X / li
// a2,N` quadruples, file offset = X - 40).
//
// This is a DATA loader: the product runs it once at the race set-up on its guest arena. It is checked
// byte for byte against every captured RAM image (`CheckAiGlobals`, rrgame --aiglobalscheck), with a
// negative control that must fail.
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "game/sim/road_query.h"

namespace rr::sim {

constexpr uint32_t kAiGlobalsBytes = 2732;         // the loader's read length (li a2,2732 at 0x8006466C)
constexpr uint32_t kAiProfileDst   = 0x800531AC;   // the chosen difficulty profile
constexpr uint32_t kAiProfileBase  = 0x2DC;        // 32 x 60 bytes from here
constexpr uint32_t kAiProfileBytes = 60;
constexpr uint32_t kAiTwoPlayerSel = 0x8005B220;   // the selector's first test (lw -19936(0x8006))

struct AiGlobalsCopy {
    uint32_t dst;
    uint32_t offset;
    uint32_t bytes;
};

// The 46 copies in the loader's order, the profile's `offset` being that of row 0 (the selector adds
// 60 * row). `list.size() == 46`.
const std::vector<AiGlobalsCopy>& AiGlobalsCopies();

// The selector RASHCDI 0x800649CC: the profile row from game_state (*(0x8005B2F8)) +0x04 (flags),
// +0x30 (players), (s16) +0x3A (difficulty) and *(0x8005B220). Reads guest memory only.
int32_t AiProfileRow(GuestRam& g);

// The whole loader on guest memory. Returns false (writing nothing) when the file is shorter than 2732
// bytes or the profile row falls outside the file - the original would print its warning and copy
// whatever its stack held; the port refuses instead of guessing.
bool LoadAiGlobals(GuestRam& g, const std::vector<uint8_t>& file, std::string& why);

// The check: every destination of a captured RAM image equals the file bytes the loader would copy
// there under that image's own game_state. `mutate` checks the profile one row off (the control: it
// must FAIL on every image). Returns the number of differing bytes (0 = pass); `report` gets a line.
size_t CheckAiGlobals(uint8_t* ram, uint32_t gp, const std::vector<uint8_t>& file, bool mutate,
                      std::string& report);

} // namespace rr::sim
