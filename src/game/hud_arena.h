#pragma once
// The race HUD's arena: everything HudFrame (src\game\sim\hud.h) reads that the race LOADER builds -
// the item records, the art and texture tables, the flash timers and panel slides, the HUD font, the
// GAMESTRG string table, the message widths, the packet heap - built from the player's own disc by a
// transcription of the loader, plus the HUD's page of VRAM.
//
// Transcribed from our own disassembly of RASHCDI.BIN (SHA-1 9a8b79d8739713c12b0a2dc4d75ef8f0a2cc0b06,
// loaded at 0x8005B5E8):
//   0x8005ED94  HudAlloc     the item buffer, the tables' "unset" marks, the HUD globals
//   0x8005FA88  HudLayout    DASH?P.CSV (0x8005F1E4, tokenizer 0x8005F104), the texture and art
//                            fix-ups (0x8005F9EC, 0x8005EFFC), SetArtFull per item, the career shifts
//   0x80063C20  FontInit     the font slots and the VRAM origin of the font pages
//   0x80061528  FontLoad     GAMEFONT.PFN (with SLUS 0x8002CA64 for its CLUT, 0x80061430 for its page)
//   0x80063D94  StringsLoad  GAMESTRG.LOC, relocated, published at *(0x8005B544)
//   0x80060088  HudReset     per race: timers, slides, last-drawn values, the clock digits, the
//                            prelink (0x8005FE24), the bar tiles (0x80060028), 0x8005EC20
// and checked by `rrgame --hudarenacheck` against the captured RAM images and VRAM.
//
// THE PLACEMENT is the one thing that is ours. The item buffer, the font's header block and the LOC
// file are MALLOC'd by the original (0x8010449C, 0x801A5A34 and 0x801A5E7C in every race capture); the
// product's arena has its road objects and scene cells there, so the product passes addresses of its
// own, and the check passes the capture's.
#include "game/sim/road_query.h"
#include "rrvfs/disc_image.h"

#include <cstdint>
#include <string>
#include <vector>

namespace rr::game {

struct HudPlacement {
    uint32_t items = 0;      // 3960 bytes per player
    uint32_t fontBlock = 0;  // the PFN header + glyph table, bitmapOffset (0x440) bytes
    uint32_t fontBitmap = 0; // where the loader's bitmap buffer was (freed; the record keeps the pointer)
    uint32_t strings = 0;    // the LOC file
    uint32_t heapRecord = 0x800D6CB8;           // *(0x8005B470): +0x10C = the next free byte
    uint32_t heapBase = 0, heapEnd = 0;          // the packet heap HudFrame's text and icons take from
    uint32_t ot = 0x800D9CA0, ot2 = 0x800D9CA8;  // *(0x8005B590), *(0x8005B5A0) in every capture
    // Player 2's: *(0x8005B594) / *(0x8005B5A4), slots 7 and 9 of the same 12-slot table (RASHCDG
    // 0x800C89A0 points 0x8005B590.. at slots 8, 7, ... and 0x8005B5A0.. at 10, 9; the buffer rr-race shows).
    uint32_t otP2 = 0x800D9C9C, ot2P2 = 0x800D9CA4;
};

// The product's placement (OURS, in the RASHCDG bss gap below the road-object area 0x800E0000). Two
// players need two 3960-byte item buffers side by side (0x8005ED94 mallocs 3960 * players): the font,
// the strings and a smaller packet heap move up behind them.
HudPlacement ProductHudPlacement(int players = 1);

// The HUD page of VRAM the HUD's packets sample: x 960..1023 (tpage 15), rows 0..255.
struct HudVram {
    static constexpr int kX = 960, kWidth = 64, kRows = 256;
    std::vector<uint16_t> page = std::vector<uint16_t>(static_cast<size_t>(kWidth) * kRows, 0);
    uint16_t At(int x, int y) const { // VRAM coordinates; outside the page reads 0
        if (x < kX || x >= kX + kWidth || y < 0 || y >= kRows) return 0;
        return page[static_cast<size_t>(y) * kWidth + static_cast<size_t>(x - kX)];
    }
    int rowsWritten = 0; // how many rows the loaders define (the rest stays 0)
};

// Builds the HUD arena into `g` for game_state's player count and race type, as the loader does at
// race start, and the VRAM page into `vram`. `report` names what was built. False on a missing file
// or a faulting address.
bool BuildHudArena(rr::sim::GuestRam& g, const DiscImage& disc, const HudPlacement& at, HudVram& vram,
                   std::string& report, bool mutateNoArtFixup = false);

// HD media (docs\HD-MEDIA.md, tools\rrhd): the HUD page built without a race - the loader transcription on a scratch
// image of SLUS_010.53 + RASHCDG.BIN, as --hudarenacheck builds it - for `players` 1 or 2 (and, for two, the split view
// mode 0..2 the layout CSV depends on), and the regions its packets sample: every art rectangle with its CLUT, and the
// HUD font's bitmap with the font CLUT. u, v, w, h are 4-bit texels of the page (tpage 15, VRAM x 960).
struct HudRegion {
    int u = 0, v = 0, w = 0, h = 0;
    uint16_t clut = 0;
    bool font = false;
    std::string what;
};
bool BuildHudPageForHd(const DiscImage& disc, int players, int splitMode, HudVram& vram, std::vector<HudRegion>& regions,
                       std::string& report);

// Per frame, before HudFrame: the frame's ordering-table clear and packet-heap reset for the HUD's
// slots (OURS: the original's frame does it for its whole OT and double-buffered heap).
void HudBeginFrame(rr::sim::GuestRam& g, const HudPlacement& at);

// The oracle check of the HUD arena against a captured race state directory (ram.bin + vram.bin).
// `mutate` leaves out the art fix-up (0x8005EFFC): every image must then differ.
bool CheckHudArena(const DiscImage& disc, const std::string& stateDir, std::string& report, bool mutate);

} // namespace rr::game
