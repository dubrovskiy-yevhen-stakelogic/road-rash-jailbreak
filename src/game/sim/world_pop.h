#pragma once
// The world around the road: the cell walker and what it spawns - the roadside props (pools 4 / 5),
// the static collision volumes (pool 6) and their per-player lists - and the passes that keep and
// release them (docs\formats\scene_cell.md 5, docs\formats\population.md 4).
// Ported function by function from our own disassembly of the player's images:
//
//   RASHCDG.BIN  SHA-1 cfe43a7786759f2cb9c57751cf99e84d1074782c, loaded at 0x8005B5E8
//   SLUS_010.53  SHA-1 67ed165a2c517d4e6106fb0dfa324d66dd9a76f1, text at 0x80010000
//
// Each function is accepted by its own row of `rrverify phys` (tools\rrverify\rows_world.inc).
//
// MEMORY MODEL, STACK and THE CALL SEAM are recover.h's: guest addresses through `GuestRam`; `sp` is
// the stack pointer at the function's entry and the locals whose address a callee receives live at
// the original's frame offsets; every callee that is not a ported function run inline goes through
// `RecoverCallees::Call(fn, ...)` - the bench answers it with the original code, the product with its
// natives (src\game\world_pop_product.cpp). The seams here: the pedestrian spawner 0x800CB8C8, the
// hazard-object spawner 0x800A0A20, SLUS 0x8002090C and 0x80093F94 (the dead kind-0 arm), BuildObb
// 0x8008BA18 and GroundQuery 0x800A7BF8. Everything else is called natively (the road layer through
// `RoadRuntimeCallees`, which the product and the bench both serve with RoadRuntimeNative).
#include <cstdint>

#include "game/sim/integrator.h"
#include "game/sim/recover.h"
#include "game/sim/road_query.h"
#include "game/sim/road_runtime.h"

namespace rr::sim {

// ---------------------------------------------------------------------------- addresses
constexpr uint32_t kWpGameStatePtr = 0x8005B2F8; // -> game_state: +4 mode byte, +0x10 clock, +0x30 players
constexpr uint32_t kWpPlayerBikes  = 0x8005B268; // -> the bike of player p (4 p)
constexpr uint32_t kWpCellSlots    = 0x800D87E8; // 24 x 112: +0 id, +4 body, +8 id, +0x4C -> extents
constexpr uint32_t kWpViews        = 0x800CD898; // view records, stride 1132 (+0xB8 / +0xC0 the eye)
constexpr uint32_t kWpPoolTable    = 0x800CE4D0; // 7 x {base, stride, -> live, -> high}
constexpr uint32_t kWpPool4Ctrl    = 0x800CD6C8; // +0 live, +4 next free, +8 high, +12 base (596 bytes)
constexpr uint32_t kWpPool5Ctrl    = 0x800CE598; // +0 live, +4 next free, +8 high, +12 base (-452 bytes)
constexpr uint32_t kWpPool6Ctrl    = 0x800CD6A8; // +0 live, +4 next free, +8 high, +12 cap - 1, +16..+24 band
                                                 // totals, +28 base (280 bytes)
constexpr uint32_t kWpPool6Ptr     = 0x800CD6C4; // = kWpPool6Ctrl + 28
constexpr uint32_t kWpPool6Cap     = 0x8005B214; // 24 (32 in two-player mode)
constexpr uint32_t kWpStoreFree    = 0x800D1814; // bytes left for pools 4 / 5 (8344 at the start)
constexpr uint32_t kWpStoreLow     = 0x800D1660; // the lowest free store record
constexpr uint32_t kWpStore        = 0x800D1664; // 18 x 24-byte store records (the props' part slots)
constexpr uint32_t kWpVolumeLists  = 0x800D5CF8; // per player, 32 bytes: +0 cap, +4 -> {s16 idx, s16 next,
                                                 // s32 distance}[cap], +8.. three band counts, +20.. heads
constexpr uint32_t kWpPieces       = 0x800D4B10; // the resident road pieces, 16 bytes (count - 1: 0x8005B31C)
constexpr uint32_t kWpPiecesLast   = 0x8005B31C;
constexpr uint32_t kWpHazardCount  = 0x8005B314; // s32: the hazard objects out (the class 0 / 9 arm, < 3)
constexpr uint32_t kWpHazardTable  = 0x8005B328; // {s8 class, s8 slot, ..}[2] (SLUS data)
constexpr uint32_t kWpHazardNext   = 0x8005B260; // the first slot the hazard arm scans
constexpr uint32_t kWpHazardPool   = 0x8005B24C; // -> 6 x 312-byte hazard objects
constexpr uint32_t kWpClass50      = 0x800D43C0; // 28-byte records by player (the class-50 arm)
constexpr uint32_t kWpPropClass    = 0x800CE592; // s16: the registry slot of the prop model (family 6)
constexpr uint32_t kWpRegistry     = 0x800CE1B0; // 50 x 16
constexpr uint32_t kWpSinCos       = 0x8005624C; // SLUS {s16 sin, s16 cos}[4096]
constexpr uint32_t kWpQuatNext     = 0x8005B61C; // RASHCDG data: the three "next axis" words of 0x800716C0
constexpr uint32_t kWpVolumeRadius = 0x8005AD58; // gp+204: 80.0, the collision-volume window

// ---------------------------------------------------------------------------- the seams
constexpr uint32_t kWpPedSpawnFn    = 0x800CB8C8; // (rec, 1, bike)           -> v0 non-zero: spawned
constexpr uint32_t kWpHazardSpawnFn = 0x800A0A20; // (cls, 1, pos, 0, [entry])
constexpr uint32_t kWpResetBikeFn   = 0x8002090C; // SLUS (e)
constexpr uint32_t kWpTransitionFn  = 0x80093F94; // (e, old)
constexpr uint32_t kWpBuildObbFn    = 0x8008BA18; // (e)
constexpr uint32_t kWpGroundQueryFn = 0x800A7BF8; // (e, ref, point, normal, [hint]) -> v0

// ---------------------------------------------------------------------------- the ported functions
constexpr uint32_t kCellWalkerFn     = 0x8009C308;
constexpr uint32_t kCellsAroundFn    = 0x8009FAD8;
constexpr uint32_t kIdInListFn       = 0x8009FC4C;
constexpr uint32_t kCellSlotForFn    = 0x80013204; // SLUS
constexpr uint32_t kCellRecordsFn    = 0x8009CA88;
constexpr uint32_t kCellFarFn        = 0x8009F054;
constexpr uint32_t kOtherSlotFn      = 0x80013360; // SLUS
constexpr uint32_t kCellPiecesFn     = 0x8009EF8C;
constexpr uint32_t kResidentPiecesFn = 0x8003C42C; // SLUS
constexpr uint32_t kPieceInCellFn    = 0x8009F11C;
constexpr uint32_t kRecordWindowFn   = 0x8009C4FC;
constexpr uint32_t kRecordJunctionFn = 0x8009F288;
constexpr uint32_t kPieceChildFn     = 0x8003C494; // SLUS
constexpr uint32_t kRecordStampFn    = 0x8009C41C;
constexpr uint32_t kRecordSpawnFn    = 0x8009C654;
constexpr uint32_t kPropKindFn       = 0x8009C5E4;
constexpr uint32_t kVolumeMaskFn     = 0x80013294; // SLUS
constexpr uint32_t kVolumeRoomFn     = 0x8008D9E4;
constexpr uint32_t kClass50Fn        = 0x80012BA8; // SLUS
constexpr uint32_t kPropAlloc4Fn     = 0x800A2630;
constexpr uint32_t kPropAlloc5Fn     = 0x800A2448;
constexpr uint32_t kPropInitFn       = 0x800A07D0;
constexpr uint32_t kPropRowsFn       = 0x800A3F14;
constexpr uint32_t kJunctionSliceFn  = 0x8003C758; // SLUS
constexpr uint32_t kMatrixQuatFn     = 0x800716C0;
constexpr uint32_t kPropPassFn       = 0x800A2898;
constexpr uint32_t kPropSettleFn     = 0x800A3A84;
constexpr uint32_t kPool5PassFn      = 0x8009ACA4;
constexpr uint32_t kVolumeSpawnFn    = 0x8009BB48;
constexpr uint32_t kVolumeEvictFn    = 0x8008DA20;
constexpr uint32_t kVolumeTakeFn     = 0x8008DECC;
constexpr uint32_t kVolumeUnstampFn  = 0x8009F3D0;
constexpr uint32_t kVolumeListsFn    = 0x8008D89C;
constexpr uint32_t kListsClearFn     = 0x8008DCA0;
constexpr uint32_t kVolumeFileFn     = 0x8008DD20;
constexpr uint32_t kListAppendFn     = 0x8008DE18;
constexpr uint32_t kVolumePassFn     = 0x8009AB60;
constexpr uint32_t kDrawLoopFn       = 0x8008D56C;

// ============================================================================ SLUS leaves
// 0x80013204, 36 instructions: player p's slot (0x800D87E8 + 1344 p, twelve of 112 bytes) whose +0 is
// not -1, +8 not -1, +4 non-zero and +8 == id; 0 when none.
uint32_t CellSlotFor(GuestRam& g, uint32_t id, uint32_t p);
// 0x80013360, 15 instructions: CellSlotFor(id, p == 0) in two-player mode (game_state+4 bit 4), else 0.
uint32_t OtherSlot(GuestRam& g, uint32_t id, uint32_t p);
// 0x8003C42C, 26 instructions: up to `max` keys of the resident road pieces (0x800D4B10, 16 bytes,
// last index *(0x8005B31C)) not -1, into `out`; returns how many.
int32_t ResidentPieces(GuestRam& g, uint32_t out, int32_t max);
// 0x8003C494, 35 instructions: the 20-byte child record of road object `piece`'s BTT (+0x0C -> +0x48,
// count s16 +0x1E) whose +0 is `key`, or 0.
uint32_t PieceChild(GuestRam& g, uint32_t key, uint32_t piece);
// 0x8003C758, 58 instructions: the slice of a junction cursor nearest `pos` (reads only; the callers
// here discard it). Returns the original's v0.
uint32_t JunctionSlice(GuestRam& g, uint32_t cursor, uint32_t pos);
// 0x80013294, 51 instructions: the per-player bit of pool-6 volume `h` (+3), set / cleared by the window
// predicate 0x80013110(kind, vol + 12, p, 0, 1). Returns 1 when the bit changed.
int32_t VolumeMask(GuestRam& g, uint32_t kind, uint32_t h, uint32_t p);
// 0x80012BA8, 29 instructions: the class-50 record of player p (0x800D43C0 + 28 p).
void Class50(GuestRam& g, uint32_t rec, uint32_t cellId, uint32_t p);
// 0x800716C0, 179 instructions, frame 56: the unit quaternion of the 3x3 (s16) at `m` into `q` (x, y,
// z, w). `t.sqrt` (SqrtGte's table) is read.
void MatrixQuat(GuestRam& g, uint32_t m, uint32_t q, const BikeTables& t);

// ============================================================================ the cell walker
// 0x8009FC4C, 13 instructions, a leaf: 1 when `id` is one of the n words at `list`.
int32_t IdInList(GuestRam& g, uint32_t id, uint32_t list, int32_t n);
// 0x8009F054, 50 instructions, a leaf: 1 when the extent array `ext` is 0, or one of its (up to four)
// extents lies on player p's road with both ends more than 19200 (300.0 world units, in 1/64) from
// his along; 0 otherwise and always while he is in a junction (+0x16A == 1).
int32_t CellFar(GuestRam& g, uint32_t ext, uint32_t p);
// 0x8009F11C, 91 instructions, frame 56: 1 when road piece `key` has a child whose road is one of the
// extents and whose span (+0x18 / +0x1C >> 10 of the piece or of its junction record) meets it.
int32_t PieceInCell(GuestRam& g, uint32_t key, uint32_t ext);
// 0x8009EF8C, 50 instructions, frame 72: the resident pieces (up to 6) that PieceInCell accepts for
// extent array `ext`, into `out` (at most `max`). Returns how many.
int32_t CellPieces(GuestRam& g, uint32_t ext, uint32_t out, int32_t max, uint32_t sp);
// 0x8009FAD8, 93 instructions, frame 336: the cells around player p (his bike, or the rider while +0x25C
// > 2): its own cell +0xB0 (when not -1), then, unless it is in a junction, the cells 150.0 and 250.0
// along the road ahead / behind (by +0x16C) that are past its end node's distance, each once.
int32_t CellsAround(GuestRam& g, uint32_t out, uint32_t p, uint32_t sp);
// 0x8009F288, 82 instructions, frame 40: a record keyed to a junction child (+8 & 0xFFFE0000) is moved
// onto the road piece of the first listed piece that holds it (+8, +0x20, +0x24 rewritten); 1 when it
// landed inside the piece.
int32_t RecordJunction(GuestRam& g, uint32_t rec, uint32_t list, int32_t n);
// 0x8009C4FC, 58 instructions, frame 56: the record's road gate (its cached piece +6, or the resident
// gate 0x80039DFC, which caches it) and then the window predicate 0x80013110(kind, rec + 20, p, 0, 0).
int32_t RecordWindow(GuestRam& g, uint32_t rec, uint32_t list, int32_t n, uint32_t p, uint32_t sp);
// 0x8009C41C, 56 instructions: in two-player mode, the record `idx` of kind `kind` of slot `slot`'s
// region 0 gets +4 = `value`.
void RecordStamp(GuestRam& g, uint32_t slot, uint32_t kind, uint32_t idx, uint32_t value);
// 0x8009C5E4, 28 instructions: the DOD3 kind code (+0x0E & 0xF80) >> 7 of group `cls` of the prop
// model, or -1.
int32_t PropKind(GuestRam& g, int32_t cls);
// 0x8008D9E4, 15 instructions: pool 6 has room for player p (one player: always; two: its band total
// 0x800CD6B8 + 4 p below 16).
int32_t VolumeRoom(GuestRam& g, uint32_t p);

// 0x8009C654, 269 instructions, frame 40: one record's spawn by its kind (0 cop - dead, 2 pedestrian,
// 4 prop / class 50 / hazard object, 6 collision volume); +4 = the result when non-zero. `v0` = the
// result (a volume's handle, else 0 / 1). False: a seam refused or the view faulted.
bool RecordSpawn(GuestRam& g, uint32_t rec, uint32_t cellId, uint32_t p, uint32_t sp, const BikeTables& t,
                 RecoverCallees& c, uint32_t& v0);
// 0x8009CA88, 293 instructions, frame 80: the five region-0 arrays of one cell for player p.
bool CellRecords(GuestRam& g, uint32_t id, uint32_t region0, uint32_t ext, int32_t onRoute, uint32_t p,
                 uint32_t sp, const BikeTables& t, RecoverCallees& c);
// 0x8009C308, 69 instructions, frame 72: CellRecords over the cells around every player.
bool CellWalker(GuestRam& g, uint32_t sp, const BikeTables& t, RecoverCallees& c);

// ============================================================================ props (pools 4 / 5)
// 0x800A3F14, 112 instructions: the prop's rows +0x1B0..+0x1C0 from the record's flags +0x0C (bit 0: the
// slice's frame, bit 1: the record's own forward n; both: the side row = OP(up, n), normalised).
bool PropRows(GuestRam& g, uint32_t rec, uint32_t e, const BikeTables& t);
// 0x800A07D0, 148 instructions, frame 96: a prop entity `e` of pool `pool` set up from record `rec`
// (road seat, position, model group, rows, route, window test; on the road: its classification,
// settling, box and heading). Released again when anything failed. `v0` = 1 on success.
bool PropInit(GuestRam& g, uint32_t rec, uint32_t e, uint32_t pool, uint32_t bike, uint32_t sp,
              const BikeTables& t, RecoverCallees& c, uint32_t& v0);
// 0x800A2630 (pool 4, 154 instructions) / 0x800A2448 (pool 5, 122): allocate, PropInit, and the mass
// +0x13C (and for pool 4 its rest point, rows, surface classification and quaternion +0x230). `v0` = the
// entity or 0.
bool PropAlloc4(GuestRam& g, uint32_t rec, uint32_t bike, uint32_t sp, const BikeTables& t, RecoverCallees& c,
                uint32_t& v0);
bool PropAlloc5(GuestRam& g, uint32_t rec, uint32_t bike, uint32_t sp, const BikeTables& t, RecoverCallees& c,
                uint32_t& v0);
// 0x800A3A84, 142 instructions, frame 72: a moving prop's ground contact.
bool PropSettle(GuestRam& g, uint32_t e, uint32_t sp, RecoverCallees& c);
// 0x800A2898, 115 instructions, frame 48: every pool-4 prop: a moving one integrated and settled, then
// the window test (released outside it).
bool PropPass(GuestRam& g, int32_t dt, uint32_t sp, RecoverCallees& c);
// 0x8009ACA4, 41 instructions: every pool-5 entity's window test (released outside it).
bool Pool5Pass(GuestRam& g, uint32_t sp);

// ============================================================================ collision volumes (pool 6)
// 0x8009BB48, 496 instructions, frame 112: a volume from record `rec` (seat, position, frame, footprint
// corners, heading, mass +0x90, window test). `v0` = the entity or 0.
bool VolumeSpawn(GuestRam& g, uint32_t rec, uint32_t bike, uint32_t sp, const BikeTables& t, uint32_t& v0);
// 0x8009F3D0, 31 instructions: the volume's cell record +4 = 0 (spawnable again) when its cell is still
// in player p's slots.
void VolumeUnstamp(GuestRam& g, uint32_t vol, uint32_t p);
// 0x8008DCA0 (32) / 0x8008DE18 (45) / 0x8008DD20 (62) / 0x8008DECC (42): player p's volume lists.
void ListsClear(GuestRam& g, uint32_t p);
void ListAppend(GuestRam& g, uint32_t p, uint32_t band, uint32_t idx, uint32_t dist);
void VolumeFile(GuestRam& g, uint32_t p, uint32_t vol, uint32_t idx);
uint32_t VolumeTake(GuestRam& g, uint32_t p, uint32_t band);
// 0x8008D89C, 82 instructions: every player's lists rebuilt from the live volumes.
void VolumeLists(GuestRam& g);
// 0x8008DA20, 98 instructions: record `rec` found pool 6 full - a volume from player p's farther band
// is taken, unstamped and released, and the lists rebuilt.
bool VolumeEvict(GuestRam& g, uint32_t rec, uint32_t p);
// 0x8009AB60, 81 instructions: every volume's window test; outside: unstamped for every player and
// released; inside: VolumeMask per player. Then VolumeLists.
bool VolumePass(GuestRam& g, uint32_t sp);

} // namespace rr::sim
