#pragma once
// The cells' view sort of the frame, transcribed from our own disassembly of
//
//   SLUS_010.53  SHA-1 67ed165a2c517d4e6106fb0dfa324d66dd9a76f1, text at 0x80010000
//
// and accepted only by `rrverify phys` rows (tools\rrverify\rows_cells.inc). SLUS 0x800358C0 runs, per view p, after the
// draw list 0x80035F48: 0x800353C4(gp+0x8EC + 4p, n, list, p), 0x80036438(n, list, p), 0x80035680(n, list, p), with
// n = *(gp+0x8DC + 4p) and list = 0x800D9B80 + 48p (s32 slot indices, -1 = none).
//
// The cell context of slot s is ctx = 0x800D87EC + 0x70 s (the slot record 0x800D87E8 + 4): +0x00 the body, +0x04 the
// resource id (the slot's +0x08), +0x0C..+0x1C the view's rotation (five GTE words), +0x20..+0x28 the cell's
// translation in view space, +0x2C the largest and +0x30 the smallest view depth of region-3 polygon 0, +0x34 the draw
// control word, +0x3C the mean depth of the first four corners of sub-area 1's polygon.
//
//   SLUS 0x80035040 CellDepthRange(ctx, poly, n)    MVMVA (RT, TR of ctx) of each listed vertex of the body's
//                                                   vertex array (+0x34, 8 bytes each from +4); +0x2C = max z from
//                                                   -641, +0x30 = min z from 0xFFFF; returns 0 when every vertex is
//                                                   outside one common side (behind: 16 z < -640; right: |16 z| < 21 x;
//                                                   left: 21 x < -|16 z|), 1 when any is outside the 45-degree planes
//                                                   (|16 z| vs 16 x), else 2
//   SLUS 0x800351EC CellRegionRange(ctx, poly, n, mean, min, max)  the same transform, each vertex's MAC1..3 stored at
//                                                   *(0x8005ACB0) + 16 i; *min from 0xFFFFFF, *max from -641, *mean
//                                                   the first four depths summed >> 2; the same codes (from 7)
//   SLUS 0x800106DC ApplyRows(v, out, m)            out[r] = sum_c (m[3r + c] * v[c]) >> 12 (each term shifted)
//   SLUS 0x800353C4 CellViewCull(count, n, list, p) for each listed slot: the render camera's rotation into ctx +0x0C,
//                                                   the body's origin (+0x08..) less the camera's eye (+0x1C..) turned
//                                                   by it into +0x20.., CellDepthRange over region-3 polygon 0; the slot
//                                                   whose id (+0x08) is the view record's cell +0xB0 (0x800CD948 +
//                                                   0x46C p, written by EntityCell 0x8008B99C in the draw loop) keeps
//                                                   a negative max as 0x500 and becomes gp+0x254; any other slot the
//                                                   range test culls (0) is set to -1 and *count decremented. The
//                                                   camera's slot is swapped to the list's front.
//   SLUS 0x80036438 CellListSort(n, list, p)        (after 0x8002E698, a GTE-only light product whose result it drops)
//                                                   the camera's slot first, then a bubble sort of the rest by +0x2C
//                                                   ascending - by +0x3C when the two maxima are within 0x280 - with
//                                                   -1 entries skipped
//   SLUS 0x80035680 CellLodPass(n, list, p)         per listed slot and sub-area k: CellRegionRange over the region-3
//                                                   polygon k; +0x3C = the mean of k = 1; the nibble k of +0x34 (via
//                                                   0x80036614): 0 culled, else 1 / 5 (5 when straddling or mean <
//                                                   0x800), 3 / 7 fine when the body has +0x3C, min < 9600 (4800 in a
//                                                   two-player race) and fewer than four slots have gone fine; | 12
//                                                   when mean < 9600 (4800)
//   SLUS 0x80021988 (its map only)                 near gp+0x848 = min - 0x3C0 (0 when min or max < 0), shift
//                                                   gp+0x84C = 0x80053224[min >> 11] (at most index 9; index 0 then),
//                                                   length gp+0x170 = (max - min + 0x780 + 2^shift) >> shift, + 0x300
//                                                   while near < 0x1000, at most 0x513. The rest of the function - the
//                                                   frame record's OT buffers (*(0x8005B470) +0xF4 / +0xF8.., the busy
//                                                   flags 0x800D75D0 the GPU's completion clears) and libgpu's
//                                                   ClearOTagR 0x80048CAC - is the console's GPU double-buffering; it has
//                                                   no bench row (DMA channel 6 and VSync(-1) are hardware the phys
//                                                   harness does not model) and is not ported
//
// `widen` (OURS, the product's wide window): 4096 is the console's 4:3 side test, bit for bit. A larger value divides
// the view x by widen / 4096 before the side tests, so the planes open by that factor - a 16:9 picture keeps the
// original's vertical field and shows (16/9) / (4/3) more at each side, and a cell or sub-area the 4:3 test drops
// there is in the picture.
#include <cstdint>

#include "game/sim/road_query.h"

namespace rr::sim {

constexpr uint32_t kCellDepthRangeFn = 0x80035040, kCellRegionRangeFn = 0x800351EC, kApplyRowsFn = 0x800106DC,
                   kCellViewCullFn = 0x800353C4, kCellListSortFn = 0x80036438, kCellLodPassFn = 0x80035680,
                   kCellLodSetFn = 0x80036614, kOtTableSetupFn = 0x80021988, kCellLightDotFn = 0x8002E698;
constexpr uint32_t kCellCtx = 0x800D87EC, kCellCtxStride = 0x70; // the slot record 0x800D87E8 + 4
constexpr uint32_t kCellViewRecords = 0x800CD898, kCellViewStride = 1132; // +0xB0 the view's cell
constexpr uint32_t kCellRenderCams = 0x8005AEC0;                 // RenderCam *[2]: +0x1C.. the eye, +0x5C.. the rows
constexpr uint32_t kCellVertexBuffer = 0x8005ACB0;               // the model draw's camera-space vertex buffer
constexpr uint32_t kCellOtShiftTable = 0x80053224;               // s32[10]
constexpr uint32_t kCellFrameRecord = 0x8005B470, kCellVsync = 0x8005B46C;
constexpr uint32_t kCellOtBusy = 0x800D75D0, kCellOtStamp = 0x800D75C0, kCellOtMark = 0x800D75B0;
constexpr uint32_t kCellGameState = 0x8005B2F8;
// gp-relative words
constexpr uint32_t kGpCameraSlot = 0x254, kGpOtLength = 0x170, kGpOtBuffer = 0x174, kGpOtNext = 0x178,
                   kGpOtNear = 0x848, kGpOtShift = 0x84C;
constexpr int32_t kWidenUnit = 4096;

int32_t CellDepthRange(GuestRam& g, uint32_t ctx, uint32_t poly, int32_t n, int32_t widen = kWidenUnit);
int32_t CellRegionRange(GuestRam& g, uint32_t ctx, uint32_t poly, int32_t n, uint32_t mean, uint32_t min, uint32_t max,
                        int32_t widen = kWidenUnit);
void ApplyRows(GuestRam& g, uint32_t v, uint32_t out, uint32_t m);
void CellViewCull(GuestRam& g, uint32_t count, int32_t n, uint32_t list, uint32_t p, int32_t widen = kWidenUnit);
// Returns the bubble sort's passes; false in `settled` when it did not settle within `maxPasses` (the console would
// spin on - never seen; the product stops there and says so).
int32_t CellListSort(GuestRam& g, int32_t n, uint32_t list, uint32_t p, bool* settled = nullptr, int32_t maxPasses = 1 << 30);
void CellLodPass(GuestRam& g, int32_t n, uint32_t list, uint32_t p, int32_t widen = kWidenUnit);

// 0x80021988's table map, the part a renderer needs: near offset, shift, length (clamped to 0x513).
struct OtTableMap {
    int32_t nearOffset = 0, shift = 5, length = 0;
};
OtTableMap OtTableMapOf(GuestRam& g, int32_t min, int32_t max);

} // namespace rr::sim
