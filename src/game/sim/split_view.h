#pragma once
// The two-player split screen's layout, transcribed from our own
// disassembly of SLUS_010.53 (SHA-1 67ed165a2c517d4e6106fb0dfa324d66dd9a76f1, text 0x80010000):
//
//   0x8001B5EC  ViewsLoad    the first 0x4C bytes of DATA\VIEWS.VI to 0x800D6C68 (a file read: the
//                            product's own disc door, not a port)
//   0x8001B670  ViewModeWrite(mode)  *(0x800D6C68) = mode
//   0x8001B67C  ViewModeSet(mode)    *(0x800D6C68) = mode; *(gp+0x7E8 = 0x8005B474) = 0x800D6C6C + 24 mode
//   0x8001BFA8  ViewsInit    the boot's call (SLUS 0x8001180C): keep the mode, ViewsLoad, ViewModeWrite(it),
//                            the pointer as ViewModeSet leaves it
//   0x8001C498  ViewMode()   return *(0x800D6C68) - the HUD loader's split layout (RASHCDI 0x8005EBDC)
//
// VIEWS.VI's 0x4C bytes: u32 mode, then three 24-byte records, one per mode: player 1's rectangle
// {s16 x, y, w, h} of the 384 x 240 draw area, player 2's, and two {s16 dx, dy} nudges of the projection
// centre (0 in the player's file). The front end's commit RASHCDF 0x8007F37C calls ViewModeSet(session
// +0x17) - the multiplayer options' chooser 29 - and the race reads the rectangle through *(0x8005B474)
// for the per-player draw area (RASHCDG 0x800C8B24), the GTE offset (SLUS 0x80011C4C / 0x80011FFC:
// the rectangle's centre plus the nudge) and the HUD's divider (RASHCDG 0x8005E848).
#include <cstdint>

#include "game/sim/road_query.h"

namespace rr::sim {

constexpr uint32_t kViewsFile = 0x800D6C68;      // u32 mode + 3 x 24-byte records
constexpr uint32_t kViewsRecords = 0x800D6C6C;
constexpr uint32_t kViewsRecordBytes = 24;
constexpr uint32_t kViewsFileBytes = 0x4C;       // what ViewsLoad reads
constexpr uint32_t kViewsPtrGp = 0x7E8;          // gp+0x7E8 = 0x8005B474, -> the mode's record
constexpr uint32_t kViewModeWriteFn = 0x8001B670, kViewModeSetFn = 0x8001B67C, kViewModeFn = 0x8001C498;

void ViewModeWrite(GuestRam& g, uint32_t mode);
void ViewModeSet(GuestRam& g, uint32_t mode);
uint32_t ViewMode(GuestRam& g);

// The draw-area rectangle of player p (0 or 1) under the current record, and the projection centre the
// GTE offset gets (the rectangle's centre, w and h halved as the original does - `(u16)w << 16 >> 17` -
// plus the record's nudge at +0x10 + 4p). False when the pointer names no record.
struct SplitRect {
    int16_t x = 0, y = 0, w = 0, h = 0;
    int32_t cx = 0, cy = 0;
};
bool SplitViewRect(GuestRam& g, uint32_t p, SplitRect& out);

} // namespace rr::sim
