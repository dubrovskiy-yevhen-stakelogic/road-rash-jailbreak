#pragma once
// GameFrame step 4, the per-view bookkeeping of every drawable entity, transcribed from our own
// disassembly of
//
//   RASHCDG.BIN  SHA-1 cfe43a7786759f2cb9c57751cf99e84d1074782c, loaded at 0x8005B5E8
//
// and accepted only by `rrverify phys` (tools\rrverify\rows_vis.inc, row `view_pass`).
//
//   RASHCDG 0x8008CFDC ViewPass()   the two view counters 0x800CD540 / 0x800CD542 saved and cleared;
//       pool 0 (the bikes, *(0x800CE4DC) + 1 of them): a live bike (+0x140) gets ViewDistance 0x8008DBCC
//       and LodChoice 0x800667C4 and, per view p whose record 0x800CD898 + 1132 p is not frozen
//       (+0x224 bit 0), its "near" bit 0x10 << p of +0x140 set when its distance +0x2C + 4 p is under
//       0x300 (0x1000 when the bike is within 0x2000 of the view's progress +0x144, the distance then
//       taken + 0x1000), with +0x352 the nearest distance >> 6 and view p's counter + 1 (a frozen view
//       keeps its saved count); its rider, when live, the bike's two distances while on the bike (+0x25C
//       below 3) or its own ViewDistance, then LodChoice; the counters summed; player 1's / 2's passenger
//       likewise; pools 3 and 4 (+0xAC, +0x140) ViewDistance and LodChoice; pools 5 and 6 ViewDistance;
//       the three 280-byte records *(0x8005B304) (while *(0x8005B314)) ViewDistance.
//
// The product ran the bike half of ViewDistance in its world pass and the animation half of LodChoice at
// LOD 0 in its place (rider_pose.h PoseLodPass); this is the whole step, and the LOD bytes +0x0A it leaves
// are what the renderer draws each rider and bike at (the model draw's LodSelect 0x8001298C reads them).
#include <cstdint>

#include "game/sim/road_query.h"

namespace rr::sim {

constexpr uint32_t kViewPassFn = 0x8008CFDC;
constexpr uint32_t kViewCounters = 0x800CD540; // u16[2]

// False when an address faulted (the pass stops there, as the console would crash).
bool ViewPass(GuestRam& g);

} // namespace rr::sim
