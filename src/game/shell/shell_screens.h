#pragma once
// The front end's per-screen input handlers that override the default
// 0x80069418, PORTED from RASHCDF.BIN (sha1 a3fec4b4e9292c358d0f6dc529843f5d8f25924a, base 0x8005B5E8)
// by reading our own objdump of it. Each is `int32_t H(Screen*)` in the
// original, called as h(screen) by the input pass 0x800667E4 through the table 0x8009C8C0 (or, for the
// jukebox chooser, through the object table 0x8009CC40[31]).
//
// Same conventions as shell_logic.h: guest memory through GuestRam at the original's addresses,
// unported callees through ShellCallees::Call at the point the original calls them. Every function
// returns false only when a seam call could not be made. Accepted only by the bench rows of
// tools\rrverify\rows_shell_screens.inc (`rrverify phys --only shell_scr_*`).
#include "game/shell/shell_logic.h"

#include <cstdint>

namespace rr::shell {

// ---------------------------------------------------------------------------- globals they keep
constexpr uint32_t kFeJukeRow = kFe + 0x06;       // s8 the jukebox screen's row cursor, 0..4
constexpr uint32_t kFeAbortYes = kFe + 0x19;      // s8 the abort modal's Yes/No (chooser 36, kind 0x24)
constexpr uint32_t kFeTrophyRace = kFe + 0x1F;    // u8 the trophy room's race (chooser 10, kind 10)
constexpr uint32_t kFeNewGame = kFe + 0x22;       // s8 "leaving the trophy room starts a new game"

constexpr uint32_t kRaceOptSaved = 0x8009C690;    // u8[3] session+0x09..0x0B on entry to 25/39
constexpr uint32_t kSoundSaved = 0x8009C694;      // s32 (s8) session+0x14 on entry to 48
constexpr uint32_t kPadCfgSaved = 0x8009C698;     // u8[6] player[i]+0x08 on entry to 49
constexpr uint32_t kPadVibSaved = 0x8009C6A0;     // u8[6] player[i]+0x17 on entry to 49
constexpr uint32_t kMultiSaved = 0x8009C6A6;      // u8[2] session+0x19, session+0x17 on entry to 51
constexpr uint32_t kJukeSaved = 0x8009C6B0;       // u8[18] session+0xC0.. on entry to 47
constexpr uint32_t kSoundTemplate = 0x80088BD0;   // 7 x 16-byte slider descriptors, value at +12
constexpr uint32_t kSoundPreviewDue = 0x80088CCC; // s32 "start the slider's preview next frame"

// The name / code keyboard of screens 52 and 56 (5 rows x 9 columns, row 4 the edit keys).
constexpr uint32_t kKeyCol = 0x8009CCD4;          // s32 0..8
constexpr uint32_t kKeyRow = 0x8009CCD8;          // s32 0..4
constexpr uint32_t kKeyPos = 0x80088DF0;          // s32 0..7, the edited character
constexpr uint32_t kKeyBuf = 0x80088C84;          // char[9] the edited text
constexpr uint32_t kKeyBlank = 0x8005BD5C;        // char[9] its initial "________"
constexpr uint32_t kKeyCopy = 0x80088C90;         // char[] the finished code, for the compare
constexpr uint32_t kKeyGrid = 0x80088C9C;         // char[5*9] the key caps, row*9 + col
constexpr uint32_t kCodeTable = 0x800540B8;       // SLUS: char*[8], code i sets 0x8005AE7C bit i
constexpr uint32_t kRecordHead = 0x800D9C40;      // 16 bytes copied over a table's header on a new #1

// Callees these reach that the port does not contain (sound).
constexpr uint32_t kVolumeSet = 0x8007F20C;       // (channel, value) RASHCDF
constexpr uint32_t kSoundPreview = 0x8007EB1C;    // (slider kind, value) RASHCDF
constexpr uint32_t kSoundPreviewStop = 0x8007ECFC; // () RASHCDF

// ---------------------------------------------------------------------------- the handlers
bool LogoInput(GuestRam& g, uint32_t screen, int32_t* result);                               // 0x8006AAF8  0
bool CreditsInput(GuestRam& g, ShellCallees& k, uint32_t screen, int32_t* result);           // 0x8006A838  21, 50
bool MessagePanelInput(GuestRam& g, ShellCallees& k, uint32_t screen, int32_t* result);      // 0x8006AD20  53
bool RaceOptionsInput(GuestRam& g, ShellCallees& k, uint32_t screen, int32_t* result);       // 0x80069440  25, 39
bool TrophyRoomInput(GuestRam& g, ShellCallees& k, uint32_t screen, int32_t* result);        // 0x80069560  26
bool OptionsInput(GuestRam& g, ShellCallees& k, uint32_t screen, int32_t* result);           // 0x8006AE6C  42
bool AbortModalInput(GuestRam& g, ShellCallees& k, uint32_t screen, int32_t* result);        // 0x8006AB58  54, 55
bool SoundOptionsInput(GuestRam& g, ShellCallees& k, uint32_t screen, int32_t* result);      // 0x80069FF0  48
bool ControllerOptionsInput(GuestRam& g, ShellCallees& k, uint32_t screen, int32_t* result); // 0x8006A390  49
bool MultiplayerOptionsInput(GuestRam& g, ShellCallees& k, uint32_t screen, int32_t* result); // 0x8006A518 51
bool JukeboxInput(GuestRam& g, ShellCallees& k, uint32_t screen, int32_t* result);           // 0x8006A610  47
bool NewRecordInput(GuestRam& g, ShellCallees& k, uint32_t screen, int32_t* result);         // 0x80069A8C  52
bool CodeEntryInput(GuestRam& g, ShellCallees& k, uint32_t screen, int32_t* result);         // 0x80069618  56
bool JukeboxChooserInput(GuestRam& g, ShellCallees& k, uint32_t screen, int32_t* result);    // 0x8006BE84  object 31
// RASHCDF 0x8006BF18(screen, chooser, padFirst, padEnd): Down/Up step the chooser (next/prev).
bool ChooserUpDown(GuestRam& g, ShellCallees& k, uint32_t chooser, int32_t padFirst, int32_t padEnd, int32_t* result);

// The one entry point for CallHandler: runs `handler` natively when it is one of the above and sets
// *handled; *handled = false (and nothing else touched) for any other address.
bool CallPortedScreenHandler(GuestRam& g, ShellCallees& k, uint32_t handler, uint32_t screen, int32_t* result,
                             bool* handled);

} // namespace rr::shell
