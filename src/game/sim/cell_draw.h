#pragma once
// Which scene cells a view draws (docs\formats\scene_cell.md 14), transcribed from our own
// disassembly of
//
//   SLUS_010.53  SHA-1 67ed165a2c517d4e6106fb0dfa324d66dd9a76f1, text at 0x80010000
//
// and accepted only by `rrverify phys` rows (tools\rrverify\rows_vis.inc).
//
//   SLUS 0x800363F0 CellReady(slot)          0 when one of the slot's two texture keys +0x58 / +0x5C
//                                            (-1 = none) has no resolved page +0x60 / +0x64 yet, else 1
//   SLUS 0x800329BC CellSlotOf(id, p)        the index of view p's slot (12 p .. 12 p + 11) holding
//                                            resource `id` at +0x00, or -1
//   SLUS 0x80035E60 CellResidentList(l, p)   every slot of view p with an id (+0x00, +0x08 != -1), a
//                                            body (+0x04) and CellReady, in slot order, into s32 l[]
//   SLUS 0x80035F48 CellDrawList(p)          view p's draw list 0x800D9B80 + 48 p: with no release
//                                            list (gp+0x88C == 0) CellResidentList; else the record of
//                                            STREAM<n>.RLS whose {road, from, to} holds the player's
//                                            position - the bike *(0x8005B268 + 4 p), or its rider
//                                            while the rider is off the bike (+0x25C 3 / 4), whose
//                                            road word +0x168 has high half 0 and whose +0x172 is the
//                                            distance - found from the cursor gp+0x87C + 4 p (kept, or
//                                            stepped by the direction +0x16C one record), else by a
//                                            scan of up to 140 records from the road's first; its up
//                                            to six u16 cell keys (+0x12, 0xFFFF ends) each go to the
//                                            list when resident and ready, else to the missing list
//                                            0x800D9B68 (count gp+0x8E8). Counts: gp+0x8DC + 4 p (the
//                                            list) and gp+0x8EC + 4 p (a copy the culling pass
//                                            decrements), gp+0x8E4 = list + missing. No record: the
//                                            previous list stays, v0 = 0.
//   SLUS 0x8002428C (its fix-up only)         the release list's per-road offsets made absolute:
//                                            gp+0x1A4 = the file, gp+0x88C = file + 8
//
//   SLUS 0x80030500 CellAt(road, d)          the resource id (28 bits) of the first loaded cell of the
//                                            resource list *(0x8005ACBC) (records +0x2C, 36 bytes, count
//                                            +0xA58; flags & 0x12 == 0x12, key type 0 or 8) with an extent
//                                            {road, from, to} (+0x18 -> up to four, -1 ends) holding
//                                            from <= d < to; -1 when none
//   SLUS 0x80030410 PositionCell(h, r)        for the entity whose handle is at h (h = e + 0xAC): on a road
//                                            (+0x168 high half 0) CellAt(road, +0x170 >> 10), the distance
//                                            first moved r toward view 0's +0x170 >> 10 when on view 0's
//                                            road (never past it); in a junction the junction's road
//                                            *(gp+472)+0x14 record +8, at 1 or the road's length - 1 by its
//                                            side +0x0C (the road record *(gp+472)+0x18 + 16 road, +4)
//   RASHCDG 0x8008B99C EntityCell(e)          e+0xB0 = PositionCell(e + 0xAC, r), r = the model radius
//                                            +0x28 >> the DOD3 shift (u16 +0x0E >> 12) as s16, 0 for
//                                            pool 6 and the views (pool 4 slot >= 30); the draw loop
//                                            0x8008D56C poses and draws an entity only when it is > 0,
//                                            and the model draw 0x80067690 only in the cell it names
//
// The draw list is keyed on the PLAYER (bike or downed rider), not on the camera; the camera only
// transforms and culls the listed cells (0x800353C4, 0x80036438 - PORTED in cell_sort.h).
#include <cstdint>
#include <functional>

#include "game/sim/road_query.h"

namespace rr::sim {

constexpr uint32_t kCellReadyFn = 0x800363F0;
constexpr uint32_t kCellSlotOfFn = 0x800329BC;
constexpr uint32_t kCellResidentListFn = 0x80035E60;
constexpr uint32_t kCellDrawListFn = 0x80035F48;

constexpr uint32_t kCellSlots = 0x800D87E8;     // 24 x 112 bytes (scene_cell.md 1.1)
constexpr uint32_t kCellDrawLists = 0x800D9B80; // s32[12] per view, 48 bytes a view
constexpr uint32_t kCellMissing = 0x800D9B68;   // u32[6]: the listed keys not resident / not ready
constexpr uint32_t kCellViewBikes = 0x8005B268; // Bike*[2]
// gp-relative words
constexpr uint32_t kGpRlsFile = 0x1A4, kGpRlsCursor = 0x87C, kGpRlsTable = 0x88C, kGpDrawCount = 0x8DC,
                   kGpDrawTotal = 0x8E4, kGpMissingCount = 0x8E8, kGpDrawCount2 = 0x8EC;

bool CellReady(GuestRam& g, uint32_t slot);
int32_t CellSlotOf(GuestRam& g, uint32_t id, uint32_t p);
// `ready` stands in for 0x800363F0 (the bench passes CellReady itself).
using CellReadyFn = std::function<bool(GuestRam&, uint32_t)>;
int32_t CellResidentList(GuestRam& g, uint32_t list, uint32_t p, const CellReadyFn& ready);
int32_t CellDrawList(GuestRam& g, uint32_t p, const CellReadyFn& ready);
void RlsRelocate(GuestRam& g, uint32_t file);

constexpr uint32_t kCellAtFn = 0x80030500;
constexpr uint32_t kPositionCellFn = 0x80030410;
constexpr uint32_t kEntityCellFn = 0x8008B99C;
constexpr uint32_t kResourceListPtr = 0x8005ACBC;
int32_t CellAt(GuestRam& g, uint32_t road, int32_t d);
int32_t PositionCell(GuestRam& g, uint32_t h, int32_t radius);
int32_t EntityCell(GuestRam& g, uint32_t e);

} // namespace rr::sim
