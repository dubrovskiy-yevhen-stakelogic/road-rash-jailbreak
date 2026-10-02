#pragma once
// The rest of the sky's programs: the gradient, the two-player sky and
// the frame's packet heap manager. Transcribed from our own disassembly of the player's own images:
//
//   SLUS_010.53  SHA-1 67ed165a2c517d4e6106fb0dfa324d66dd9a76f1, text at 0x80010000
//   RASHCDG.BIN  SHA-1 cfe43a7786759f2cb9c57751cf99e84d1074782c, loaded at 0x8005B5E8 (the race)
//   RASHCDI.BIN  SHA-1 9a8b79d8739713c12b0a2dc4d75ef8f0a2cc0b06, loaded at 0x8005B5E8 (the race loader)
//
// with the Ghidra pseudo-C of work\ghidra as the reading aid (it takes 0x8001FC90 and 0x80021C98 for functions that
// do not return and drops the gradient's second half - the listing is followed), each accepted only by its row of
// `rrverify phys` (tools\rrverify\rows_sky3.inc).
//
//   SLUS 0x80021C98 HeapNext(cur, n)  the frame's packet heap manager: a request of n bytes at the cursor that would
//                                     pass the limit gp+0x844 (= 0x8005B4D0). The heap is a ring shared by the
//                                     frame buffers in flight: 0x800D75B0[b] is where buffer b's packets begin,
//                                     0x800D75C0[b] its VSync stamp, 0x800D75D0[16 b] its busy flag (the GPU's
//                                     completion clears it; one older than 60 VSyncs is taken as done), gp+0x174
//                                     the buffer being built, gp+0x178 the oldest one in flight, gp+0x850 the
//                                     heap's end, record +0xF0 its start, +0xF4 the buffer count. It moves the
//                                     limit past the finished buffers, wraps to the start, and when nothing frees
//                                     room calls the flush 0x80021BE8. Returns the address to build at.
//   SLUS 0x80021BE8 HeapFlush         printf, DrawOTag of the frame's table, DrawSync, printf, ClearOTagR, DrawSync,
//                                     then gp+0x844 = gp+0x850 and 0x800D75B0[gp+0x174] = the heap's start
//   SLUS 0x8001FC90 FixMul(a, b)      (a * b) >> 16 of the 64-bit product (fixed.h, the fix_mul row)
//   RASHCDG 0x80063C5C GradDraw(view) the Gouraud sky: RTPS of the view's yaw (+0x7C) on the horizon, the screen-edge
//                                     yaws +-426 from the level's sun yaw 0x80052388 (folded to 0..2048, scaled to
//                                     0..65536) blend the colours 0x800523F0 / F4 per edge; with the sun inside the
//                                     field (mode 1 / 2) a middle point at the sun's yaw (or its opposite) and the
//                                     level's elevation 0x8005238C carries the sun colour: four POLY_G4, else two.
//                                     The corners 0x800523D0 (one player: the EXE's (0,0) (384,0), the horizon's
//                                     rows; two players: the view's rectangle *(0x8005B474) + 8 v), 10 rows under
//                                     the horizon, less *(0x800D513C) when the level has the two-player sky.
//                                     Linked into the sky OT's entry 3.
//   RASHCDG 0x800642F8 GradQuad       one POLY_G4 (0x38) from the heap into *ot
//   RASHCDG 0x80064CC8 SplitSkyDraw(v) the two-player sky (and the split stream's one-player races): SPRT 64 x 31
//                                     with their DR_TPAGE (packets of 6 words), two rows of the level's 27 columns
//                                     around the view's centre, picked by the index table 0x800D56C4 from the 32
//                                     records 0x800D5144 (44 bytes: uv / clut, tpage word, the CLUT owner, the TIM's
//                                     name) of the type-10 section; entry 0 of the sky OT
//   RASHCDI 0x800627A8 SplitSkyLoad    the bundle's type-10 section: its 0x5F8 bytes to 0x800D5138 when the tag is
//                                     0x5FC, 0x8005AD24 = that test
//   RASHCDI 0x80062384 GradLoad        the bundle's type-1 section: the top / horizon colours (payload 0..2, 4..6) to
//                                     0x800523F8 / FC, the sun pair (8..10, 12..14) to 0x800523F0 / F4, then
//   RASHCDI 0x800611C4 GradInit        0x8005AD28 = 1 (the gradient drawn), the corners (0,0) (384,0) (384,240)
//                                     (0,240), 0x800523E0.. the colours' copies
// The level file is DATA\GAMEBIN<players>.DAT (RASHCDI 0x800619A0: "data\gamebin%d.dat" of game_state+0x30): the
// type-10 sections are in GAMEBIN2.DAT only.
//
// THE LOADER's TIM part (RASHCDI 0x80060EFC / 0x80060C78: DATA\FE\<name> read, ReadTIM, LoadImage) touches the CD and
// the GPU; the product transcribes it on its disc reader and host VRAM (game\sky_product.h), no bench row.
#include <cstdint>

#include "game/sim/effects.h"   // FxGte
#include "game/sim/fixed.h"     // FixMul SLUS 0x8001FC90 (the fix_mul row)
#include "game/sim/recover.h"   // RecoverCallees
#include "game/sim/road_query.h"

namespace rr::sim {

// ---------------------------------------------------------------------------- functions
constexpr uint32_t kHeapNextFn = 0x80021C98, kHeapFlushFn = 0x80021BE8, kFixMulFn = 0x8001FC90,
                   kGradQuadFn = 0x800642F8, kSplitSkyLoadFn = 0x800627A8, kSplitSkyTimsFn = 0x80060C78,
                   kSplitSkySetUpFn = 0x80060EFC, kGradLoadFn = 0x80062384, kGradInitFn = 0x800611C4;
// the host's (libgpu / BIOS)
constexpr uint32_t kPrintfFn = 0x80044894, kDrawOTagFn = 0x80048DB4, kClearOTagRFn = 0x80048CAC;
constexpr uint32_t kHeapNextFrame = 24, kHeapFlushFrame = 24, kGradFrame = 184, kGradQuadFrame = 56, kSplitSkyFrame = 72;

// ---------------------------------------------------------------------------- data
constexpr uint32_t kGpHeapLimit = 0x844, kGpHeapEnd = 0x850, kGpFrameOtLen = 0x170, kGpFrameOtBuf = 0x174,
                   kGpFrameOtNext = 0x178;
constexpr uint32_t kHeapMarks = 0x800D75B0, kHeapStamps = 0x800D75C0, kHeapBusy = 0x800D75D0; // busy: stride 64
constexpr uint32_t kHeapVsync = 0x8005B46C;
constexpr uint32_t kGradCorners = 0x800523D0;  // s16 (x, y)[4]
constexpr uint32_t kGradSunA = 0x800523F0, kGradSunB = 0x800523F4, kGradTop = 0x800523F8, kGradHorizon = 0x800523FC;
constexpr uint32_t kSunYaw = 0x80052388, kSunElevation = 0x8005238C, kGradYawCopy = 0x800CCD68;
constexpr uint32_t kSplitViewsPtr = 0x8005B474; // -> {x, y, w, h} s16 per view
constexpr uint32_t kSplitSkyBase = 0x800D5138, kSplitSkyBytes = 0x5F8;
constexpr uint32_t kSplitSkyLift = 0x800D513C;  // s16: the rows the two-player sky lifts the horizon by
constexpr uint32_t kSplitSkyCmd = 0x800D5140;   // the SPRT's colour / command word
constexpr uint32_t kSplitSkyRecords = 0x800D5144, kSplitSkyRecordBytes = 44, kSplitSkyRecordCount = 32;
constexpr uint32_t kSplitSkyIndex = 0x800D56C4; // s16 [27 columns][2 rows]; -1 none
constexpr uint32_t kSplitSkyCols = 0x800D5EEC, kSplitSkyCentre = 0x800D5EEE, kSplitSkyColX = 0x800D5EF2;
constexpr uint32_t kSplitSkyOn2 = 0x8005AD24;   // the level has the two-player sky (sky_draw.h kSplitSkyOn)

// The product's wide picture (OURS): how far the picture reaches beyond the console's field, in
// console pixels of the view's own projection. Null everywhere below is the original, bit for bit.
struct SkyWide {
    int32_t left = 0, right = 0;                 // pixels beyond the view's left edge (x 0) and right edge (x 384)
    int32_t edgeAngle = 426;                     // the gradient's screen-edge yaw for the widened field (426)
    int32_t columnsLeft = 0, columnsRight = 0;   // panorama columns (of 110) past the original's 25
    int32_t segmentsLeft = 0, segmentsRight = 0; // cloud segments (of 24) past the original's 8
    mutable int32_t panoAdded = 0, panoMissing = 0; // the panorama's extra columns drawn / not in the decoded window
};

// SLUS 0x80021C98: false when the port refuses (a busy buffer that never ages - the console would wait for VSyncs).
bool HeapNext(GuestRam& g, uint32_t cursor, uint32_t size, uint32_t sp, RecoverCallees& c, uint32_t& v0);
// SLUS 0x80021BE8.
bool HeapFlush(GuestRam& g, uint32_t sp, RecoverCallees& c);
// RASHCDG 0x800642F8(ot, xy0, xy1, xy2, xy3, c0, c1, c2, c3): the nine are guest addresses.
bool GradQuad(GuestRam& g, const uint32_t a[9], uint32_t sp, RecoverCallees& c);
// RASHCDG 0x80063C5C. RT / TR as the sky draw left them; RTPS by `gte`.
bool GradDraw(GuestRam& g, uint32_t view, FxGte& gte, uint32_t sp, RecoverCallees& c, const SkyWide* wide = nullptr);
// RASHCDG 0x80064CC8.
bool SplitSkyDraw(GuestRam& g, uint32_t view, FxGte& gte, uint32_t sp, RecoverCallees& c, const SkyWide* wide = nullptr);
// RASHCDI 0x800627A8(section, payload).
void SplitSkyLoad(GuestRam& g, uint32_t section, uint32_t payload);
// RASHCDI 0x80062384(section, payload).
void GradLoad(GuestRam& g, uint32_t section, uint32_t payload);
// RASHCDI 0x800611C4.
void GradInit(GuestRam& g);

} // namespace rr::sim
