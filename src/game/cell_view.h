#pragma once
// The scene cells each view draws, in the product (docs\formats\scene_cell.md 14).
//
// THE ARENA - DATA\STREAM<set>.RLS, the release list, loaded as SLUS 0x80023498 / 0x8002428C load it:
// the whole file, its per-road offsets made absolute (the PORTED fix-up, cell_draw.h RlsRelocate),
// gp+0x1A4 = the file, gp+0x88C = its table, the two cursors gp+0x87C / gp+0x880 = 0 (0x80023498).
// OURS, named: where it sits (the session's bump region).
//
// THE FRAME - after the race step, per view p, the PORTED draw list SLUS 0x80035F48 (0x800358C0's first
// call): the release-list record that holds the PLAYER (bike, or the rider off it) names up to six
// cells, each drawn when the session's slot table (race_session.cpp CellStreamPass) holds it. OURS,
// named: 0x800363F0's page test is answered "resolved" - the renderer holds every cell texture of the
// race from the start (race_scene.cpp LoadCellTextures), there is no page to wait for. The culling and
// depth sort the original runs on that list (0x800353C4, 0x80036438, 0x80035680) are PORTED
// (cell_sort_product.h).
//
// THE STREAMER KEY - SLUS 0x80023A14 per view: the road position of the entity 0x80053478 + 0x80 p + 4
// holds (CamTarget SLUS 0x800235B0: the player's bike, the rider while thrown, set by RiderLaunch /
// Remount); a road word with a non-zero high half leaves the key as it was.
#include <cstdint>
#include <string>
#include <vector>

#include "game/sim/road_query.h"
#include "rrvfs/disc_image.h"

namespace rr::game {

std::string BuildRlsArena(rr::sim::GuestRam& g, const rr::DiscImage& disc, int set, uint32_t& from, uint32_t limit);

// The entity the cell streamer of view p follows (0x80053478 + 0x80 p + 4), or `bike` while that word
// is 0 (before the first CamTarget).
uint32_t StreamTarget(rr::sim::GuestRam& g, int p, uint32_t bike);

// Runs the draw list for views 0 .. players - 1. No-op when no release list is loaded.
void CellDrawPass(rr::sim::GuestRam& g, int players);

// The resource ids of view p's draw list as the last pass left it (0x800D9B80 + 48 p, count
// gp+0x8DC + 4 p), in list order; `ok` false when no release list is loaded (nothing to follow).
std::vector<uint32_t> CellDrawIds(const uint8_t* ram, int p, bool& ok);

// The LOD a model object is drawn at in view p: its s8 +0x0A + p, which LodChoice 0x800667C4 (ViewPass,
// view_pass.h) writes and the model draw's LodSelect 0x8001298C reads (0x80067B98). 0 when out of 0..3.
int DrawLod(const uint8_t* ram, uint32_t obj, int p);

// THE RESOURCE LIST - *(0x8005ACBC): the stream buffer records SLUS 0x80030500 walks for the cell a
// road position lies in (count +0xA58, 36-byte records from +0x2C: +0x00 flags, +0x04 index, +0x08 the
// key, +0x0C / +0x14 the chunk, +0x10 its payload, +0x18 its extent array). OURS, named: the product's
// streamer is not the original's, so the list is rewritten each frame from the cells resident in the
// arena, one record per cell in buffer order (the original's record index is its buffer's), each with
// the flags 0x33 every loaded cell of the captures holds.
struct LoadedCell {
    uint32_t at = 0;   // where the chunk is placed (its key word first)
    uint32_t body = 0; // the cell body (the extent array is the 48 bytes before it)
};
void WriteResourceList(rr::sim::GuestRam& g, std::vector<LoadedCell> cells);

// The draw loop RASHCDG 0x8008D56C's cell tests (0x8008B99C, PORTED) for view p's record, every live bike
// and its rider (the bike's cell while riding, +0x25C below 3; its own when off and live, and then
// RiderInstance 0x80085224's stores, bike_parts.h), and the two passenger arms (0x8008D670 / 0x8008D6E0).
// OURS: the rest of the loop (BikeInstance, pools 2..5 and every draw 0x80067AC4) is run elsewhere or by
// the model draw.
void DrawLoopCells(rr::sim::GuestRam& g, int players);

// Whether the model draw 0x80067690 draws entity `obj` in view p: its cell +0xB0 is one of the cells of
// view p's draw list (CellDrawIds). True when no release list is loaded (nothing to follow).
bool InDrawnCell(const uint8_t* ram, uint32_t obj, int p);

// Whether ModelVisible RASHCDG 0x80067AC4 (SHA-1 cfe43a7786759f2cb9c57751cf99e84d1074782c) keeps model
// object `obj` in view p by distance: its view distance +0x2C + 4 p (ViewDistance 0x8008DBCC, 1/64 world
// unit, written by the PORTED ViewPass) must not be negative nor past the range of its DOD3 kind,
// `*(0x800CC6A4 + 4 * kind)` with kind = (DOD3 +0x0E & 0x78) >> 3 (0x80067CC8..0x80067D24; a kind-6
// object at LOD 0 gets 32000 more). The bikes are kind 2 and the riders kind 1, range 7680 (120 world
// units) in every race capture; the frustum half of the test is the renderer's.
bool InDrawRange(const uint8_t* ram, uint32_t obj, int p);

// Run totals for the log: passes, cells listed, listed-but-not-resident (the missing list).
std::string CellDrawTotals();

} // namespace rr::game
