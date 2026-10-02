#pragma once
// The front end's TEXT: the widget pass RASHCDF 0x8006D3E0, the text-block handler 0x8006FAC8 (widget
// type 16) with the arms of its 56-entry jump table 0x8005C13C, and the option/record text source
// 0x80063C7C - PORTED from RASHCDF.BIN (sha1 a3fec4b4e9292c358d0f6dc529843f5d8f25924a, base
// 0x8005B5E8) by reading its Ghidra pseudo-C (work\ghidra\decomp_shell) and our objdump.
//
// Same conventions as shell_logic.h. The text itself is PRESENTATION and stays a seam: every string
// the arms put on screen goes through the SLUS text calls below, called as the original calls them
// (same arguments, the rectangle and the sprintf buffer at the original's own stack addresses - `sp`
// is the original's stack pointer at the function's entry). The arms are nevertheless LOGIC too: the
// post-race panels count their numbers up frame by frame (fe+0x54.., fe+0x64..), tick the UI sound
// and write the display records the text source hands out - which is why they are ported and benched
// (tools\rrverify\rows_shell.inc, `shell_text_*`) instead of drawn by the view.
//
// In the bench the seams are the original's own functions run by the oracle; in the product
// (front_end.cpp ProductCallees) the text calls are turned into TextOps the view rasterises with the
// player's .PFN fonts, and sprintf is formatted natively into the arena.
#include "game/shell/shell_career.h"
#include "game/shell/shell_logic.h"

#include <cstdint>

namespace rr::shell {

// ---------------------------------------------------------------------------- the text seams (SLUS)
constexpr uint32_t kTextId = 0x8002CB08;      // DrawStringId(font, id, short rect[4], ot, rgb, just)
constexpr uint32_t kTextStr = 0x8002CC74;     // DrawString(font, char*, short rect[4], ot, rgb, just)
constexpr uint32_t kTextAt = 0x8002CD78;      // DrawStringIdAt(font, id, x, y, ot, rgb)
constexpr uint32_t kSprintf = 0x80043FD4;     // sprintf(buf, fmt, ...)
constexpr uint32_t kSpriteEmit = 0x800700F0;  // RASHCDF: the shared sprite emitter (screen, sprite rec*, ot)
constexpr uint32_t kPanelEmit = 0x8006ECDC;   // RASHCDF: the panel / frame emitter (screen, rec*, ot)

// ---------------------------------------------------------------------------- the globals they read
constexpr uint32_t kFontSlots = 0x800D8078;   // four 24-byte font slots: +0 loaded, +3 blend index,
                                              // +4 the .PFN, +8 its glyphs, +0xC its sheet, +0x14 max h
constexpr uint32_t kFontMain = 0x8009C5B8;    // s32 the slot of resource class 3 id 1 (BTN_FONT)
constexpr uint32_t kFontHdr = 0x8009C5BC;     // s32 ... id 3 (HDR_FONT)
constexpr uint32_t kFontMini = 0x8009C5C0;    // s32 ... id 2 (MINIFONT)
constexpr uint32_t kOtTable = 0x8009CFC8;     // u32* the shell's ordering table this frame
constexpr uint32_t kFeTicked = kFe + 0x08;    // s16 the screen ScreenTick is handling (DAT_8009c5d8)
constexpr uint32_t kFeLayer = kFe + 0x11;     // s8 its ordering-table layer (screen +0x0C / +0x0D)
constexpr uint32_t kStringTable = 0x8005B544; // const char** FESTRING.LOC's pointers (gp+0x8B8)
constexpr uint32_t kOverlayMask = 0x8005ACA8; // u32 the resident overlays (SLUS 0x800118A0 ORs them in)

// The post-race panels' counters (fe+0x54..): shown value, target value, tick timer.
constexpr uint32_t kFeShown = kFe + 0x54;     // s16[3] race / combat / total bonus as counted so far
constexpr uint32_t kFeTick = kFe + 0x62;      // u16 the roll-up's step timer
constexpr uint32_t kFeFiveShown = kFe + 0x64; // s16[3] the Five-O panel's counted values
constexpr uint32_t kFeFiveTick = kFe + 0x76;  // s8 its step timer
constexpr uint32_t kFeDuelShown = kFe + 0x44; // s16[6] the two-player panel's counted values (+0x44..+0x4E)
constexpr uint32_t kFeDuelTick = kFe + 0x52;  // s16 its step timer
constexpr uint32_t kFeBonusTarget = kFe + 0x70; // s16[3] + s16 +0x78 / +0x7A: what the dispatcher wrote (shell_career.h)

// The product's stack pointer for the ported text path (OURS: the console's is wherever the shell
// loop's frame happens to be; the port only needs a free window for the rectangles and buffers).
constexpr uint32_t kProductTextSp = 0x801FF000;

// ---------------------------------------------------------------------------- the ported functions
int32_t ActionGroup(GuestRam& g, uint32_t widget);                 // RASHCDF 0x80072100
int32_t ModeNameId(int32_t venue, uint32_t mode);                  // RASHCDF 0x80071F0C
int32_t BikeNameId(int32_t index);                                // RASHCDF 0x80071FDC
int32_t CourseNameId(GuestRam& g, int32_t race);                   // RASHCDF 0x800720B4
int32_t GangNameId(int32_t gang);                                  // RASHCDF 0x80072A60
int32_t RankNameId(int32_t gang, int32_t level);                   // RASHCDF 0x80072A80
// RASHCDF 0x80063C7C: the display record {s16 id; s8 just; ...; u32 rgb at +4} a text block of
// `kind` shows, or 0. Writes the static records it returns (0x8009C4A8, 0x800810F8..0x80081120).
uint32_t TextSource(GuestRam& g, uint32_t kind);

// RASHCDF 0x8006D3E0: every visible widget of `screen` handed to its type's handler from the update
// (screen+0x02 == 0) or draw table - through the seam, h(screen, widget).
bool WidgetPass(GuestRam& g, ShellCallees& k, uint32_t screen);
// RASHCDF 0x8006FAC8: widget type 16. `sp` is the original's stack pointer at entry. Returns 1.
bool TextBlock(GuestRam& g, ShellCallees& k, uint32_t screen, uint32_t widget, uint32_t sp);

} // namespace rr::shell
