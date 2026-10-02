#pragma once
// The widget DRAW handlers of the front end's handler tables 0x8009CF28 / 0x8009CF78 (frontend.md 4.2)
// that the menus the player moves through are made of - PORTED from RASHCDF.BIN (sha1
// a3fec4b4e9292c358d0f6dc529843f5d8f25924a, base 0x8005B5E8) by reading its Ghidra pseudo-C
// (work\ghidra\decomp_shell) and our objdump.
//
// Each writes the PlayStation packets the original writes (a sprite, a flat rectangle, a line box) into
// the display packet buffer *(0x8005B470)+0x10C, links them into the shell's ordering table, animates
// the state the original animates (the chooser arrows' frame, the bike page's stat bars), and calls the
// SLUS text functions / the shared sprite emitter 0x800700F0 through the seam. Each packet is also told
// to the host (ShellCallees::Drew), which is how the product's view draws it.
//
// Arguments as the original's: (screen, widget); v0 the handler's result.
#include "game/shell/shell_logic.h"

namespace rr::shell {

constexpr uint32_t kSpriteIds = 0x80088E0C;   // the 300 FourCCs the sprite functions search
constexpr uint32_t kSpriteRecs = 0x8009DDE0;  // their 36-byte records: +0 flags, +4 data, +0x10 VRAM rect,
                                              // +0x18 u, +0x1A v, +0x1C tpage, +0x1E clut
constexpr uint32_t kSpriteUpload = 0x80065768; // RASHCDF: LoadImage of a record's pixels (seam)
constexpr uint32_t kPacketWrap = 0x80021C98;  // SLUS: the packet buffer's wrap (seam)
constexpr uint32_t kPacketLimit = 0x8005B4D0; // SLUS: the packet buffer's end
constexpr uint32_t kStatTable = 0x80088CD0;   // the bike page's three stats: {s32 min, s32 max, s16 label} x 3
constexpr uint32_t kBikeStats = 0x80088CF4;   // s32[3] per bike: acceleration, durability, top speed
constexpr uint32_t kFeStatShown = kFe + 0x38; // s32[3] the stat bars as drawn so far (one step a frame)

bool SpriteWidget(GuestRam& g, ShellCallees& k, uint32_t s, uint32_t w, uint32_t* v0);  // 0x8006E894 type 7
bool PadHint(GuestRam& g, ShellCallees& k, uint32_t s, uint32_t w, uint32_t* v0);       // 0x8006E7A4 type 9
bool ButtonDraw(GuestRam& g, ShellCallees& k, uint32_t s, uint32_t w, uint32_t* v0);    // 0x8006F764 type 12
bool ChooserDraw(GuestRam& g, ShellCallees& k, uint32_t s, uint32_t w, uint32_t* v0);   // 0x8006EF30 type 13
bool SliderDraw(GuestRam& g, ShellCallees& k, uint32_t s, uint32_t w, uint32_t* v0);    // 0x8006F4A0 type 17
bool BikeStatsDraw(GuestRam& g, ShellCallees& k, uint32_t s, uint32_t w, uint32_t* v0); // 0x8006F0B8 type 18
bool PanelWidget(GuestRam& g, ShellCallees& k, uint32_t s, uint32_t w, uint32_t* v0);   // 0x8006ECA0 type 11
bool HeaderText(GuestRam& g, ShellCallees& k, uint32_t s, uint32_t w, uint32_t* v0);    // 0x8006F9A4 type 14
bool TextLine(GuestRam& g, ShellCallees& k, uint32_t s, uint32_t w, uint32_t* v0);      // 0x8006FA38 type 15
// 0x8006ECDC: the panel emitter (screen, panel record, ot) - the frame's four bands as flat quads of the
// record's colour (+0x02 bit 0 semi-transparent), and unless +0x20 bit 0 the inner quad of the record at
// +0x10 - through its quad writer 0x8006EE20.
bool PanelEmit(GuestRam& g, ShellCallees& k, uint32_t s, uint32_t rec, uint32_t ot, uint32_t* v0);
// 0x800705DC: an animated sprite record (FourCC, colour, x, y, frames, frame w/h, delay, frame, tick,
// u/v offset, flags) - the chooser's arrows - advanced by `mode` (0 run, 1 restart, 2 hold, 3 rewind
// and hold) and emitted into `ot`.
bool AnimSprite(GuestRam& g, ShellCallees& k, uint32_t rec, uint32_t ot, int32_t mode, uint32_t* v0);

} // namespace rr::shell
