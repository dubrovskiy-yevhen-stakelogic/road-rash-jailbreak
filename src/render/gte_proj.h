#pragma once
// The world's vertices on the screen the way the ORIGINAL puts them there (the GTE projection): every
// cell vertex through the GTE's own fixed-point RTPS with the cell's camera transform, instead of a float GL camera.
//
// Read from our disassembly of SLUS_010.53 (SHA-1 67ed165a2c517d4e6106fb0dfa324d66dd9a76f1) and RASHCDG.BIN (SHA-1
// cfe43a7786759f2cb9c57751cf99e84d1074782c):
//   * RASHCDG 0x80069200 (the per-cell draw, a1 = the cell record 0x800D87EC + 0x70 i, i.e. the slot 0x800D87E8 + 4)
//     pushes the slot's eight words +0x10..+0x2C onto the matrix stack *(0x800CC870): RT = +0x10 (the camera matrix,
//     second row scaled 3412/4096, written by SLUS 0x800353C4 - PORTED, cell_sort.h), TR = +0x24 (the cell origin in
//     the camera frame);
//   * 0x80068EB8 / 0x80068D50 / 0x80068E2C run the vertex pass over the cell's vertex groups: SLUS 0x8001034C (the
//     near groups) / 0x800104C8 (the others): ctc2 RT / TR, RTPS 0x180001 of each 8-byte vertex, SXY2 into
//     *(0x8005ACB4) + 4 v, MAC3 into *(0x8005ACB0) + 16 v + 8, and (0x8001034C only) the byte *(0x8005ACB8) + v:
//     0x20 z < 1024, 0x10 z < 40, 8 SY > 240, (SX >u 384) + (SX < 0) (disassembly 0x800103DC..0x80010420);
//   * the band 0 / 1 emitters (0x8006D350, 0x8006DC20, 0x8006C888, 0x8006A630's whole polygons) and band 2's far quads
//     (0x8006E474 / 0x8006F5D0, max depth >= 0x1000) put those SXY into their packets as they are;
//   * 0x8001064C then moves RT / TR into LLM / BK and zeroes RT / TR: the near polygons' corners (0x8006A630, band 2's
//     near path) are MVMVA 0x4A2012 (LLM x V0 + BK, sf 1) << 8 and go through RTPS with TR = record >> 5; band 2's
//     lane strips step between the corners by (end - start) * 0x800CCA10[n] >> 12 (32-bit, 0x8006EC44..0x8006EE98)
//     and its lane lines sit (step >> 6) and (step >> 5) in from the strip edges (0x8006EEC4..0x8006F424); the
//     PORTED subdividers (subdiv.h) cut them.
//
// The renderer then draws each vertex at its SXY (shaders.cpp aScreen / uGte: an orthographic map of the 384 x 240
// screen onto the picture, from the same projection the float camera uses, so a wider window shows SX beyond 0..384 -
// the GTE's SX runs -1024..1023) with the view depth MAC3 as w. A vertex the GTE cannot put on the screen (MAC3 at or
// below H / 2 = 118, where the divide saturates, or SX / SY clamped to +-1023) keeps the float camera's projection.
// RRJB_PROJ=float: every vertex through the float camera (the negative control).
#include "game/sim/model_draw.h"
#include "render/gl_api.h"
#include "render/scene_geometry.h"
#include "rrformats/cell.h"
#include "rrformats/rmd3.h"

#include <array>
#include <cstdint>
#include <vector>

namespace rr::render {

// One cell's vertex pass: the slot's RT / TR in `gte` (H 237, the view's OFX / OFY) and per cell vertex the SXY2,
// MAC3 and 0x8001034C's byte.
struct GteCell {
    rr::sim::model::ModelGte gte;
    std::vector<uint32_t> sxy;
    std::vector<int32_t> z;
    std::vector<uint8_t> flags;
    bool valid = false;
};
// The slot record at `slot` (0x800D87E8 + 0x70 i) and the cell it holds: false when the words are not a camera
// matrix (every row zero) - the slot was not transformed this frame.
bool GteCellFromSlot(const uint8_t* ram, uint32_t slot, const rr::CellData& cell, int32_t ofx, int32_t ofy, GteCell& out);

// The aScreen attribute of one soup vertex (shaders.cpp): SX, SY, the view depth in cell units, and 2 when the GTE's
// point is used (anything else: the float camera's).
struct GteScreen {
    float sx = 0, sy = 0, z = 0, use = 0;
};
using GteScreens = std::vector<GteScreen>;
// `sz3`: the SZ3 the SXY was divided by when it is not the view depth itself (a near record's TR z = record z >> 5).
GteScreen GteScreenOf(uint32_t sxy, double zCell, double sz3 = -1.0);

// What the per-frame builders did, for the race log.
struct GteStats {
    size_t frames = 0, cells = 0, cellsNoSlot = 0, vertices = 0, floatVertices = 0, nearPrims = 0, band2Far = 0, band2Near = 0,
           lines = 0;
};

// 0x8006A630 (the fine NEAR groups) on the GTE's own numbers: whole polygons at the vertex pass's SXY, polygons with a
// corner nearer than 1024 through the PORTED subdivider from their MVMVA records. `view` only places the float
// fallback (a piece's world point for w and the clip planes).
void AppendFineNearGroupGte(const rr::CellData& cell, const GteCell& gc, int group, uint16_t texKey, const CellView& view,
                            const CellLook& look, float sideSqueeze, rr::TriangleSoup& out, GteScreens& screens,
                            SubdivStats& stats, GteStats& gstats);

// 0x8006E474 / 0x8006F5D0 (band 2 of fine group `fineGroup`) on the GTE's own numbers: far quads one GT4 at the vertex
// pass's SXY, near quads cut into lane strips (SubRoad) and lane lines (SubLine below 0x640, else one F4) from the
// stepped records. The soups' vertices carry the same attributes AppendBand2Soup(Subdivided) gives them.
void AppendBand2Gte(const rr::CellData& cell, size_t cellIndex, size_t fineGroup, const GteCell& gc, const CellView& view,
                    const CellLook& look, float sideSqueeze, rr::TriangleSoup& road, GteScreens& roadScreens,
                    rr::TriangleSoup& lines, GteScreens& lineScreens, SubdivStats& stats, GteStats& gstats);

// The screen buffer of a vertex array: attribute 7 (aScreen) from `vbo`, filled with `screens` (after the GPU's
// large-polygon rule marked the triangles it refuses, GteRejectBig).
void GteAttachScreens(GLuint vao, GLuint& vbo, GteScreens& screens);

// RRJB_PROJ=float turns the GTE projection off.
bool GteProjOn();

// ---------------------------------------------------------------------------- the other objects
// The GPU's large-polygon rule: a triangle whose corners span 1024 columns or more, or 512 lines or more, is not drawn
// (the hardware's; tools\scout\psxgpu.py triangle() refuses the same). A quad is two triangles, each tested alone.
// RRJB_GTE_BIG=draw: every triangle drawn (the control).
bool GpuRejects(uint32_t sxyA, uint32_t sxyB, uint32_t sxyC);
bool GpuRejectOn();
// The soup triangle starting at `v` (three vertices) collapsed onto its first corner: rasterises nothing.
void CollapseTriangle(rr::TriangleSoup::Vertex* v);
// Per-frame screen buffers (aScreen, attribute 7): each triangle (three consecutive entries from 0) whose three corners are
// GTE points and which the GPU would refuse is marked w = 3 (the vertex shader puts it outside the clip volume).
void GteRejectBig(GteScreen* s, size_t n);
// Run totals: triangles refused by the rule, per path (0 cells, 1 models).
size_t& GteRejected(int path);
// A vertex whose divide saturates (MAC3 <= H / 2) or whose SX / SY clamps is drawn at the SXY the GTE gave it, as the
// console draws it, its depth held at the near plane or beyond (w >= kGteMinW world units); RRJB_GTE_SAT=float: the float
// camera's projection of it instead (the control).
bool GteSaturatedOn();
constexpr double kGteMinW = 0.3;
// A vertex array with the soup layout (attributes 0..6: rmd3.h TriangleSoup::Vertex) whose buffer is refilled per draw.
void GteStreamArray(GLuint& vao, GLuint& vbo);
void GteStreamUpload(GLuint vao, GLuint vbo, const std::vector<rr::TriangleSoup::Vertex>& v);

} // namespace rr::render
