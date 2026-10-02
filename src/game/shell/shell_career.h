#pragma once
// The front end's POST-RACE half, PORTED from the shell overlay RASHCDF.BIN (sha1
// a3fec4b4e9292c358d0f6dc529843f5d8f25924a, base 0x8005B5E8): the result dispatchers the resume path
// 0x8007FF4C selects by the session's mode word, the chooser sync that follows them, and the resume
// dispatch itself. Same style as shell_logic.h: every function works on the guest's own memory
// through GuestRam at the original's addresses, and a callee that is only presentation (the result
// panel's resource loader, the root-counter read that seeds the LCG) is reached through
// ShellCallees at the point the original calls it.
//
// Proven by tools\rrverify\rows_shell_career.inc (`rrverify phys --only shell_career_*` etc.).
#include "game/shell/shell_logic.h"

#include <cstdint>

namespace rr::shell {

// Callees these functions reach but do not contain.
constexpr uint32_t kResShow = 0x800782B0;     // res_show(record, mode)  RASHCDF, frontend.md 7.3
constexpr uint32_t kGetRCnt = 0x80043F00;     // GetRCnt(spec)           SLUS, reads a root counter

// Read out of the dispatchers' own lui/addiu pairs.
constexpr uint32_t kFeBonus = kFe + 0x70;     // s16[6] +0x70 race bonus, +0x72 combat bonus, +0x74 total,
                                              //        +0x78 / +0x7A the two progress notifications
constexpr uint32_t kFeNotify = kFe + 0x1C;    // s16 the result panel's string id
constexpr uint32_t kFeTagA = kFe + 0xAC;      // u32 FourCC of the panel's picture
constexpr uint32_t kFeTagB = kFe + 0xB0;      // u32 FourCC of the outcome (WNNR/LOSR/BSTD/WRKD)
constexpr uint32_t kRiders = 0x800D5758;      // the race's 0x48-byte rider records (identity at +0x2C)
constexpr uint32_t kPrizeTable = 0x80089F44;  // u16 prize[place][6 venues], rules.md 7.4

// SLUS 0x8001FC84: the LCG seed at gp+2076 := a0.
void SetSeed(GuestRam& g, uint32_t seed);

// RASHCDF 0x8007E7EC: 1 for an odd mission id below 18, else 0 (its 18-entry jump table).
int32_t OddMission(int32_t id);

// RASHCDF 0x8007BB34 - the career (mode 32) result dispatcher, INCLUDING its venue-progress step at
// 0x8007D4A0 (which is not a function: it is this function's tail, jumped into with the outcome in
// s3 and the return value in s7). Returns the hub screen id. `ok` is cleared when a seam call failed.
int32_t CareerResult(GuestRam& g, ShellCallees& k, bool* ok = nullptr);
// RASHCDF 0x8007E2C8 - the Five-O (mode 1) result dispatcher, mission bitmap session+0xFC.
int32_t FiveOResult(GuestRam& g, ShellCallees& k, bool* ok = nullptr);
// RASHCDF 0x8007DDA8 - the Side Car co-op (mode 8) result dispatcher.
int32_t SideCarResult(GuestRam& g, ShellCallees& k, bool* ok = nullptr);

// RASHCDF 0x8007FD20 - the attract path's short commit.
void ShortCommit(GuestRam& g);
// RASHCDF 0x80063908 - every chooser re-selected from the session/player fields it edits.
void ChooserSync(GuestRam& g);

// RASHCDF 0x8007FF74..0x80080180: what 0x8007FF4C does between its three init calls and its call of
// 0x80063908 - the attract path or the mode's result dispatcher. Returns the hub screen id (s0).
int32_t ResumeDispatch(GuestRam& g, ShellCallees& k, bool* ok = nullptr);
// RASHCDF 0x8008018C..0x800801EC: GotoScreen(hub) and ArmScreen over the hub's parent chain.
void ResumeShow(GuestRam& g, int32_t hub);

} // namespace rr::shell
