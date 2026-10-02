#pragma once
// The sky's own programs: the panorama's MDEC column decode and its
// VRAM tile cache, the panorama and cloud-ring packets, and the level-load tables they read. Transcribed from our
// own disassembly of the player's own images:
//
//   SLUS_010.53  SHA-1 67ed165a2c517d4e6106fb0dfa324d66dd9a76f1, text at 0x80010000
//   RASHCDG.BIN  SHA-1 cfe43a7786759f2cb9c57751cf99e84d1074782c, loaded at 0x8005B5E8 (the race)
//   RASHCDI.BIN  SHA-1 9a8b79d8739713c12b0a2dc4d75ef8f0a2cc0b06, loaded at 0x8005B5E8 (the race loader)
//
// with the Ghidra pseudo-C of work\ghidra as the reading aid, each accepted only by its row of `rrverify phys`
// (tools\rrverify\rows_sky2.inc).
//
//   RASHCDG 0x80064B9C SkyDraw        per view: TR = 0, RT = the render camera +0x5C (SetRotMatrix 0x8004D154),
//                                     SkyView 0x800650D0 (stream_cd.h), the panorama (one player, no split, a
//                                     panorama decoded: +0x14 != -1), 0x80064CC8 (two players / split,
//                                     sky_split.h), the clouds (0x8005B308), the gradient 0x80063C5C
//                                     (0x8005AD28, sky_split.h)
//   RASHCDG 0x800644F4 PanoDraw       waits for the column decodes (+0x18: 2 = a strip decoded -> TileUpload,
//                                     1 = the MDEC busy; 10000 polls: DecDCTReset(1) and nothing drawn), then 25
//                                     columns from +0x30: every column edge RTPS'd at (sin, 279 row - 0x8B8, cos)
//                                     of the EXE table 0x8005624C (column c at angle (c * 0x94F) >> 6), SXY into
//                                     two scratchpad rows (0x1F800038 / 0x1F800080), one POLY_FT4 (0x2C, colour
//                                     *(0x8005237C)) per tile, its UVs / tpage from the tile's cache record
//                                     (+0x2744 + 12 slot), linked into the sky OT's entry 0 *(0x8005B59C)
//   RASHCDG 0x800657A8 TileUpload     the decoded strip's tiles (+0x4C4, 512 bytes each) into the VRAM ring
//                                     (LoadImage 0x80048A6C at the rect 0x800CCD70, 16 x 16, stepping down the
//                                     page columns +0x40[], across 0xC0 halfwords from *(0x8005B390)), a code-2
//                                     tile first cut by its HORZ record (HorzCut); the ring indices +0x24 / +0x26
//                                     / +0x2C / +0x2E and the columns decoded +0x34 / +0x36 kept; +0x1E counts
//                                     the columns still to decode; then DrawSync and the next DecodeStart
//   RASHCDG 0x800662BC DecodeStart    the next column PAIR's MDEC strip (*(+1000 + 4 pair) + 8): DecDCTvlc into
//                                     sky + 0x26C0 - 4 n, DecDCTin(mode +0x16), DecDCTout(sky + 0x4C4, w h / 2);
//                                     +0x18 = 1 (the MDEC busy), +0x14 the pair, +0x2DC the strip; the pair
//                                     already decoded -> TileUpload
//   SLUS 0x80013AE4 MdecDone          the MDEC-out callback: +0x18 = 2
//   SLUS 0x800102A4 HorzCut           a tile's 16 rows, each cut from the right by the record's nibble n (n != 0:
//                                     halfwords 15 - n .. 15 = 0, transparent)
//   SLUS 0x80020400 DctVlc            the game's BS VLC decoder (DecDCTvlc) with the tables *(gp+0x120 / 0x124)
//   SLUS 0x800202B8 VlcBuild          those two tables from the descriptors 0x800528A0 / 0x80052A20
//   SLUS 0x8004D1B4 RotTransPers      libgte: RTPS (its SXY; IR0 / FLAG go to the caller's frame), a leaf
//   RASHCDG 0x8006396C CloudDraw      eight ring segments from (0x800D5EE8 column * 24) / 110: POLY_FT4 0x2F, the
//                                     ring 0x800D443C (16 bytes a point: top / bottom SVECTOR), the slices
//                                     0x800D440C (segment & 3), culled when both edges are off one side; linked
//                                     into the sky OT's entry 1
//   RASHCDI 0x80060AA8 TileRecords    the cache's 12 x 16 slot records, +0x28 the count (its callee TileRecord
//                                     0x80060894 - GetTPage(15-bit) / GetClut, the turned UVs of a 16 x 16 rect -
//                                     transcribed inline: the RECT lives in the original's frame)
//   RASHCDI 0x80060780 CloudSlice     one cloud slice record (4-bit)
//   RASHCDI 0x80061100 CloudRing      the 25 ring points
//
// THE HARDWARE (MDEC, GPU) and the library around it are the host's: DecDCTReset 0x8004D7B4, DecDCTin/out sync
// 0x8004D9A8 / 0x8004D9E4, DecDCTin 0x8004D90C, DecDCTout 0x8004D988, LoadImage 0x80048A6C, DrawSync 0x800487C0,
// the empty 0x800662B4 and the packet heap manager 0x80021C98 are callees (RecoverCallees): the bench has the
// original code answer them (the MDEC ones stubbed on both sides - the interpreter has no MDEC), the product
// its MDEC and VRAM model (sky_product.h).
#include <cstdint>
#include <vector>

#include "game/sim/effects.h"   // FxGte: RT / TR / OFX / OFY / H and RTPS as the GTE computes them
#include "game/sim/recover.h"   // RecoverCallees
#include "game/sim/road_query.h"

namespace rr::sim {

// ---------------------------------------------------------------------------- functions
constexpr uint32_t kSkyDrawFn = 0x80064B9C, kSkyPanoFn = 0x800644F4, kSkyTileFn = 0x800657A8,
                   kSkyDecodeFn = 0x800662BC, kSkyCloudFn = 0x8006396C, kSkyGradFn = 0x80063C5C,
                   kSkySplitFn = 0x80064CC8, kSkyEmptyFn = 0x800662B4;
constexpr uint32_t kHorzCutFn = 0x800102A4, kDctVlcFn = 0x80020400, kVlcBuildFn = 0x800202B8,
                   kMdecDoneFn = 0x80013AE4, kRotTransPersFn = 0x8004D1B4, kSetRotMatrixFn = 0x8004D154;
constexpr uint32_t kTileRecordsFn = 0x80060AA8, kTileRecordFn = 0x80060894, kCloudSliceFn = 0x80060780,
                   kCloudRingFn = 0x80061100;
// the host's (hardware and its library)
constexpr uint32_t kDecDctResetFn = 0x8004D7B4, kDecDctInSyncFn = 0x8004D9A8, kDecDctOutSyncFn = 0x8004D9E4,
                   kDecDctInFn = 0x8004D90C, kDecDctOutFn = 0x8004D988, kSkyLoadImageFn = 0x80048A6C,
                   kSkyDrawSyncFn = 0x800487C0, kHeapFullFn = 0x80021C98;
// frames (the prologues' `addiu sp,sp,-N`)
constexpr uint32_t kSkyDrawFrame = 24, kSkyPanoFrame = 80, kSkyTileFrame = 80, kSkyDecodeFrame = 40,
                   kSkyCloudFrame = 104, kTileRecordsFrame = 72, kCloudSliceFrame = 40;

// ---------------------------------------------------------------------------- data
constexpr uint32_t kSkyBlockPtr = 0x8005B278;   // -> the sky block (stream.h kStSkyPtr)
constexpr uint32_t kSkyOtPtr = 0x8005B59C;      // -> this view's sky OT (5 entries; 0 panorama, 1 clouds)
constexpr uint32_t kSkyPacketRec = 0x8005B470;  // -> the frame record: +0x10C the packet heap's next byte
constexpr uint32_t kSkyPacketEnd = 0x8005B4D0;  // the packet heap's limit (compared unsigned)
constexpr uint32_t kSkyColour = 0x8005237C;     // the panorama's modulation colour
constexpr uint32_t kSkySinCos = 0x8005624C;     // {s16 sin, s16 cos}[4096]
constexpr uint32_t kSkyRect = 0x800CCD70;       // the tile upload's RECT {x, y, w, h}
constexpr uint32_t kSkyVramX = 0x8005B390, kSkyVramY = 0x8005B394; // the tile cache's VRAM origin (words)
constexpr uint32_t kSkyColumn = 0x800D5EE8;     // s16: the view's first sky column (SkyView)
constexpr uint32_t kCloudOn = 0x8005B308, kGradOn = 0x8005AD28, kSplitSkyOn = 0x8005AD24;
constexpr uint32_t kSkySplitFlag = 0x8005B2D0;  // stream_cd.h kSkySplit
constexpr uint32_t kCloudBand = 0x800D43F8;     // +0 height, +4 top, +0xC, +0x10 (the type-3 payload's words)
constexpr uint32_t kCloudSlices = 0x800D440C;   // 4 x 12 bytes
constexpr uint32_t kCloudRing = 0x800D443C;     // 25 x 16 bytes
constexpr uint32_t kSkyRenderCams = 0x8005AEC0; // -> the render camera of view 0 / 1 (+0x5C RT)
constexpr uint32_t kSkyGameState = 0x8005B2F8;  // -> game_state (+0x30 players)
constexpr uint32_t kGpVlcTab0 = 0x120, kGpVlcTab1 = 0x124; // the DctVlc tables (gp+0x128 / 0x12C: their blocks)

// ============================================================================ per frame
struct SkyWide; // sky_split.h: the product's wide picture (OURS)
// How the sky draw reaches the rest of the sky's programs. The default (sky_split.h): the two-player sky
// 0x80064CC8, the gradient 0x80063C5C and the heap manager 0x80021C98 PORTED, called natively.
// `seams`: all three through the callees (the product's RRJB_SKY3=off control).
struct SkyDrawOptions {
    const SkyWide* wide = nullptr;
    bool seams = false;
};
// 0x80064B9C(view). `gte`: the frame's projection (OFX / OFY / H); RT / TR are set here as the original sets them.
// SkyView runs natively (stream_cd.h, PORTED).
bool SkyDraw(GuestRam& g, uint32_t view, FxGte& gte, uint32_t sp, RecoverCallees& c, const SkyDrawOptions& o = {});
// 0x800644F4. The scratchpad must be attached (g.SetScratchpad): its rows and counters live there.
bool PanoDraw(GuestRam& g, FxGte& gte, uint32_t sp, RecoverCallees& c, const SkyDrawOptions& o = {});
// 0x800657A8: 1 when the columns to decode ran out (+0x18 = +0x1E = 0), else 0 (the next decode started).
bool TileUpload(GuestRam& g, uint32_t sp, RecoverCallees& c, uint32_t& v0);
// 0x800662BC.
bool DecodeStart(GuestRam& g, uint32_t sp, RecoverCallees& c);
// SLUS 0x80013AE4.
void MdecDone(GuestRam& g);
// SLUS 0x800102A4(tile, record).
void HorzCut(GuestRam& g, uint32_t tile, uint32_t record);
// SLUS 0x80020400(bs, out): 0.
uint32_t DctVlc(GuestRam& g, uint32_t bs, uint32_t out);
// The product's DctVlc tables when they are not in the arena (sky_product.h: its bump region has no room for
// 0x800201C0's 26 KiB): tab0 = 0x2000 lengths + 0x2000 symbols, tab1 = 0x200 + 0x200, built by the PORTED VlcBuild.
// Null (the default, the bench): the tables *(gp+0x120 / 0x124) in guest RAM.
struct VlcHostTables {
    std::vector<uint8_t> tab0, tab1;
};
void SetVlcHostTables(const VlcHostTables* tables);
// 0x8006396C.
bool CloudDraw(GuestRam& g, FxGte& gte, uint32_t sp, RecoverCallees& c, const SkyDrawOptions& o = {});

// ============================================================================ level load
// SLUS 0x800202B8(tab0, tab1, symbols): 0, or -1 with a null table. `symbols` 0: the EXE's 0x80052C20.
int32_t VlcBuild(GuestRam& g, uint32_t tab0, uint32_t tab1, uint32_t symbols);
// RASHCDI 0x80060AA8 (every slot record of the sky block's cache).
void TileRecords(GuestRam& g);
// RASHCDI 0x80060780(record, mode, rect, clutX, clutY).
void CloudSlice(GuestRam& g, uint32_t record, uint32_t mode, uint32_t rect, uint32_t clutX, uint32_t clutY);
// RASHCDI 0x80061100.
void CloudRing(GuestRam& g);
// libgpu SLUS 0x8004CD44 GetTPage / 0x8004CD84 GetClut (leaves).
uint16_t GpuTPage(uint32_t tp, uint32_t abr, uint32_t x, uint32_t y);
uint16_t GpuClut(uint32_t x, uint32_t y);

} // namespace rr::sim
