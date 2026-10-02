#pragma once
// The sky's own programs in the product: the PORTED sky draw RASHCDG
// 0x80064B9C (src\game\sim\sky_draw.h) runs on the arena every frame of a one-player race and emits the original's
// panorama and cloud packets into the sky OT *(0x8005B59C); the renderer (render\sky_gpu.h) draws exactly those
// packets as the GPU does, their texels out of the host VRAM below.
//
// THE HARDWARE, the host's (named OURS where it is ours):
//   * the MDEC: DecDCTin 0x8004D90C (its command-word bits 27 / 25 written as the library writes them) and
//     DecDCTout 0x8004D988: the run-level stream the PORTED DctVlc left is decoded into 15-bit macroblocks at the
//     out address with the tables DecDCTReset uploads (the EXE's quant tables 0x8005A250 / 0x8005A290 and IDCT
//     matrix 0x8005A2D4, read from the arena); the decode completes at once and the out callback SkyInit
//     registered (SLUS 0x80013AE4, PORTED MdecDone) runs - the draw's poll loop then finds +0x18 = 2 exactly as
//     after the console's interrupt (OURS: when; the console's CPU spins in 0x800644F4 meanwhile);
//   * the GPU's VRAM: LoadImage 0x80048A6C writes the RECT into a host 1024 x 512 image (`SkyVram`); DrawSync
//     answers 0 (idle); the cloud TIM of the level bundle's type-3 section is uploaded there by the transcribed
//     driver of RASHCDI 0x80060F58 (the rectangles of the EXE's configuration table, group 12);
//   * the packet heap manager 0x80021C98 is PORTED (sim\sky_split.h) on the product's one-buffer ring
//     (SkyHeapSetUp); its flush 0x80021BE8 PORTED, whose DrawOTag is counted, not drawn early (the renderer draws the
//     frame's lists at its end), and ClearOTagR transcribed.
//   * the gradient 0x80063C5C and the two-player sky 0x80064CC8 run inside the sky draw (sky_split.h);
//     two players draw per view into 0x800D9BE0 + 0x14 v at the view rectangle's GTE offset; the wide picture and the
//     float camera draw the packets too (SkySetPicture: the sky's programs drawn on past the console's field, OURS).
//     RRJB_SKY3=off: the sky draw's seams (the renderer's gradient, its two-player sky and wide picture).
//
// LEVEL LOAD (RASHCDI 0x80060BE8(1), one player): the cache origin *(0x8005B390 / 0x8005B394) from the
// configuration table, the DctVlc tables (VlcBuild 0x800202B8 PORTED - OURS: kept host-side, the arena's bump
// region has no room for 0x800201C0's two blocks), SkyInit (the ported loader's), TileRecords 0x80060AA8 PORTED; the cloud tables (0x80060780 /
// 0x80061100 PORTED, 0x8005B308 = 1). The sky OT: 0x800D9BE0 (view 0, buffer 0 of 0x800C89A0's pair), cleared as
// ClearOTagR(5) every frame (OURS: the frame flip between the two buffers is not modelled).
//
// RRJB_SKY2=off: nothing of this runs and the renderer draws its own cylinder and cloud ring (the control).
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "game/sim/road_query.h"
#include "rrvfs/disc_image.h"

namespace rr::game {

bool SkyPortOn();

// One packet of the sky OT as the GPU reads it: a POLY_FT4 (0x2C panorama, 0x2F clouds), a POLY_G4 (0x38, the
// gradient: rgb per vertex) or a DR_TPAGE + SPRT (0x64.., the two-player sky: x[0] y[0] u[0] v[0], w x h,
// the tpage from the DR_TPAGE word).
struct SkyPacket {
    uint32_t address = 0;
    uint32_t cmd = 0;    // 0x2C (panorama) / 0x2F (clouds) / 0x38 (gradient) / 0x64.. (two-player sky)
    uint32_t colour = 0; // the command word's low 24 bits
    int16_t x[4] = {}, y[4] = {};
    uint8_t u[4] = {}, v[4] = {};
    uint16_t tpage = 0, clut = 0;
    uint32_t rgb[4] = {}; // G4: the four vertex colours
    int16_t w = 0, h = 0; // SPRT: its size (0 for the polygons)
    int entry = 0;        // the OT entry it was linked into (0 panorama / sprites, 1 clouds, 3 gradient)
};

struct SkyVram {
    static constexpr int kWidth = 1024, kHeight = 512;
    std::vector<uint16_t> px = std::vector<uint16_t>(static_cast<size_t>(kWidth) * kHeight, 0);
    uint64_t generation = 1; // bumped by every write
    uint16_t At(int x, int y) const { return px[static_cast<size_t>(y & 511) * kWidth + static_cast<size_t>(x & 1023)]; }
};

// ---------------------------------------------------------------------------- level load (stream_session.cpp)
// Before SkyInit: the cache origin and the DctVlc tables (host-side).
std::string SkySetUpBefore(rr::sim::GuestRam& g);
// After SkyInit: the cache's slot records.
std::string SkySetUpAfter(rr::sim::GuestRam& g);
// The cloud layer of race `raceId`'s level bundle (tools\rrgame\main.cpp, after the session is built); first the
// two-player sky's tables: the bundle's type-10 section through the PORTED RASHCDI 0x800627A8, and
// with two players or the split stream RASHCDI 0x80060EFC / 0x80060C78 transcribed (OURS: DATA\FE\<name> read on the
// disc image, ReadTIM, LoadImage into the host VRAM).
std::string SkyCloudSetUp(rr::sim::GuestRam& g, const rr::DiscImage& disc, int raceId);
// Called by race_session.cpp after the packet heap is placed: the heap ring state the PORTED heap manager SLUS
// 0x80021C98 reads - OURS, named: the product's frame starts at `base` every frame (the GPU is done with the last), so
// one buffer: record +0xF0 = base, gp+0x850 = the end, 0x800D75B0[] = base (physical, as the flush stores it), gp+0x178 =
// gp+0x174. A full heap then flushes (0x80021BE8 PORTED) and restarts at the base.
std::string SkyHeapSetUp(rr::sim::GuestRam& g, uint32_t record, uint32_t base, uint32_t end);

// ---------------------------------------------------------------------------- per frame
// ProductStreamCallees' answer for RASHCDG 0x800662BC (SkyFrame's decode restart): the PORTED DecodeStart on the
// host's MDEC. False: the port is off (the caller counts the seam).
bool SkyDecodeFromStream(rr::sim::GuestRam& g, uint32_t sp);
// The sky draw of view 0 (one player): RASHCDG 0x80064B9C PORTED on `ram`, the packets it linked collected in the
// GPU's order (entry 4 .. 0: the clouds, then the panorama). False: nothing to draw (the renderer's sky then).
bool SkyFrameDraw(uint8_t* ram, int players, uint32_t frame);
const std::vector<SkyPacket>& SkyFramePackets();
// View `view`'s packets (two players: one list per view), null when none.
const std::vector<SkyPacket>* SkyFramePacketsOf(int view);
// The gradient comes from the PORTED 0x80063C5C's packets (the renderer's own gradient then stays off).
bool SkyGradientPorted();
// The picture of view `view` reaches console x `left` .. `right` (main.cpp, from its projection): wider than the view's
// own field, the sky's programs draw on to its edges (OURS, the wide picture). Used from the next draw.
void SkySetPicture(int view, double left, double right);
const SkyVram& SkyHostVram();
// rrgame --parity: the capture's tile cache was decoded before the capture and lives in its VRAM, not in its RAM.
// The resident columns' tiles are decoded again (the PORTED DctVlc and HorzCut, the host's MDEC) into the slots the
// draw's own slot walk gives them (OURS: a reconstruction of VRAM from the capture's RAM).
std::string SkyParityRebuild(const uint8_t* ram);
// The race log's counter line.
std::string SkyTotals();

} // namespace rr::game
