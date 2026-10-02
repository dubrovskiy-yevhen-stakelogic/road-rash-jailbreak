#pragma once
// The films in the menus' panels and the logos - widget types 2..6 and 8 of the front end's handler
// tables 0x8009CF28 / 0x8009CF78 (frontend.md 4.2) - PORTED from RASHCDF.BIN (sha1
// a3fec4b4e9292c358d0f6dc529843f5d8f25924a, base 0x8005B5E8) by reading its Ghidra pseudo-C
// (work\ghidra\decomp_shell) and our objdump.
//
// Types 2..6 (0x8006E4D8) pick a film record - the widget's own +0x10 (types 2, 3; type 3 names the
// options film or the mode logo film by the highlighted item's action group), the bound chooser's
// current option's kind-4 record (types 4, 6: the bike turntables, placed at the widget's +0x1C/+0x1E),
// or the mode's film (type 5, and type 6 without a kind-4 record) - and hand it to the gate 0x8006E6F4:
// with the screen live and the attract idle counter 0x8005ACAC at 151 or more the film is opened
// (0x8006FEF4) or stepped one picture (0x80070018); otherwise it is closed (0x8006FE6C). The film state
// is the movie screens' own: fe+0x0A bit 0 playing, bit 1 ended (reopen), bit 2 skip, fe+0xB4 the file
// handle, fe+0xB8 the sound channel.
//
// Type 8 (0x8006E8FC) is the logo: the six kinds +0x14 = 0x30..0x35 each choose a sprite record (the
// option's picture 0x80064154, the mode's logo, the controller diagram 0x80075BD0, the course kind's
// logo, the result panel's two pictures fe+0xAC / +0xB0) and emit it through 0x800700F0.
//
// Seams (ShellCallees): the splash fade SLUS 0x80022A78, sprintf 0x80043FD4, the file layer SLUS
// 0x8001458C / 0x8001460C, the film library RASHCDF 0x8005F36C (start) / 0x8005F484 (one picture) /
// 0x8005F7E0 (stop), and the sprite emitter 0x800700F0. The product answers the library with its own
// MDEC player (front_end.cpp PanelFilms, OURS); the bench runs the original's.
//
// `sp` is the original's stack pointer at the function's entry (FilmOpen formats "DATA\<name>" into its
// own frame at sp - 144 and hands that buffer to the file layer).
#include "game/shell/shell_logic.h"

#include <cstdint>

namespace rr::shell {

constexpr uint32_t kFilmHandle = kFe + 0xB4;   // s32 the film's file handle, -1 none (0x8009C684)
constexpr uint32_t kFilmChannel = kFe + 0xB8;  // s16 its sound channel (0x8009C688)
constexpr int32_t kFilmIdle = 151;             // the attract idle count a panel film waits for
constexpr uint32_t kFilmNames = 0x8008973C;    // the 42-name resource array (frontend.md 5.1)
constexpr uint32_t kFilmPathFmt = 0x8005BF84;  // "DATA\%s"
constexpr uint32_t kFileOpen = 0x8001458C;     // SLUS: open(path, 0) -> handle, < 0 missing
constexpr uint32_t kFileClose = 0x8001460C;    // SLUS: close(handle)
constexpr uint32_t kFilmStart = 0x8005F36C;    // RASHCDF library: (handle, mode, channel, x, y, 0x800, 0x40, 0x400, +0x0A)
constexpr uint32_t kFilmPicture = 0x8005F484;  // RASHCDF library: (x, y, channel, mode, count) -> 0 at the end
constexpr uint32_t kFilmStop = 0x8005F7E0;     // RASHCDF library: (channel)

// Frames of the functions (the original's stack use), for a caller that reaches them natively.
constexpr uint32_t kFilmWidgetFrame = 40, kFilmGateFrame = 24, kFilmOpenFrame = 184, kFilmStepFrame = 40,
                   kFilmCloseFrame = 24;

uint32_t OptionRecord(GuestRam& g, uint32_t kind);  // RASHCDF 0x80064154: the current option's entry (+2 == kind) + 4
uint32_t OptionFilm(GuestRam& g);                   // RASHCDF 0x800641D4: ... the entry of kind 4 (+0 == 4) + 4
bool FilmClose(GuestRam& g, ShellCallees& k, uint32_t* v0);                               // RASHCDF 0x8006FE6C
bool FilmOpen(GuestRam& g, ShellCallees& k, uint32_t rec, uint32_t sp, uint32_t* v0);     // RASHCDF 0x8006FEF4
bool FilmStep(GuestRam& g, ShellCallees& k, uint32_t rec, uint32_t sp, uint32_t* v0);     // RASHCDF 0x80070018
bool FilmGate(GuestRam& g, ShellCallees& k, uint32_t s, uint32_t rec, uint32_t sp, uint32_t* v0); // 0x8006E6F4
bool FilmWidget(GuestRam& g, ShellCallees& k, uint32_t s, uint32_t w, uint32_t sp, uint32_t* v0); // 0x8006E4D8
bool PadDiagram(GuestRam& g, ShellCallees& k, uint32_t s, uint32_t xy, uint32_t sp, uint32_t* v0); // 0x80075BD0
bool LogoWidget(GuestRam& g, ShellCallees& k, uint32_t s, uint32_t w, uint32_t sp, uint32_t* v0); // 0x8006E8FC

} // namespace rr::shell
