#pragma once
// Turning the decoded world into triangles: the road surface, the debug ribbon, the scene cells,
// and the stream walk that finds the chunks a race needs.
//
// None of this touches GL. It is the CPU half of the renderer, shared by `rrview` (which checks it
// per pixel) and `rrgame` (which plays it).
#include "render/gl_api.h"
#include "rrformats/cell.h"
#include "rrformats/cell_draw_tables.h"
#include "rrformats/chunk.h"
#include "rrformats/level_bundle.h"
#include "rrformats/rmd3.h"
#include "rrformats/road.h"
#include "rrvfs/disc_image.h"

#include <array>
#include <cstdint>
#include <functional>
#include <vector>

namespace rr::render {

// One run of one cell's primitives that share a texture page. A cell carries one or two texture
// keys and some primitives take the runtime road/ground page instead, so a cell contributes up to
// three runs and each one binds a different page.
struct CellRange {
    float centre[3] = {};
    GLint first = 0;
    GLsizei count = 0;
    uint16_t texKey = 0; // the key this run samples, or 0x7800 / 0x7C00 for the two fixed references
    int band = 0;        // 0 = region 6, 1 and 2 = region 7
    std::vector<rr::ResidencyWindow> windows; // the game's own rule for when this cell is needed
    // Which cell (index into the vector the soup was built from) and which region-2 group this run
    // belongs to, when the soup was built per group (`BuildCellSoup` mode 3). -1 for a run of the
    // older per-cell layouts, which the renderer draws without a level-of-detail choice.
    size_t cell = 0;
    int group = -1;
};

// What the level bundle and the race overlay give the cell draw (docs\formats\scene_cell.md 13).
// `shade` and `windows` are the switches `rrview --legacy` turns off to draw the frame without the
// original's shading and texture windows, for a before/after on one binary.
struct CellLook {
    bool shade = true;
    bool windows = true;
    bool quadDiagonal13 = true;          // split a quad the way the GPU gets it: (i0,i1,i3)+(i1,i3,i2)
    std::array<uint32_t, 256> colours{}; // DATA\GAMEBIN1.DAT bundle section 7
    bool haveColours = false;
    rr::CellDrawTables tables{};
    bool haveTables = false;
    // Negative controls of `rrview --roadcheck`: 1 = every band-2 lane strip takes the OTHER UV
    // template kind (kind ^ 1), 2 = the NEXT palette row, 3 = the near/far test at the wrong
    // depth (the near path taken for every quad).
    int band2Mutate = 0;
    // The cell emitters' back-face test (RASHCDG 0x8006A630 / 0x8006C888 / 0x8006D350 /
    // 0x8006DC20: GTE NCLIP, a primitive without record +0 bit 2 dropped when it faces away) applied to the fine near
    // groups on their own projected corners. Off: every near primitive drawn.
    bool nearNclip = false;
};

// The camera as the cell draw code needs it: the eye and the three rows of the view frame, in
// world units, `right` pointing to screen right and `up` to screen up.
struct CellView {
    float eye[3] = {};
    float right[3] = {1, 0, 0};
    float up[3] = {0, -1, 0};
    float forward[3] = {0, 0, 1};
};

// The per-cell draw control word `SLUS_010.53 0x80035680` writes to slot `+0x38` (the dispatcher's
// `ctx+0x34`), one nibble per fine group k in [0, B): bit 0 draw, bit 1 FINE (region 7), bit 2
// selects between the two routines of each pair. For each group the routine transforms the
// region-3 boundary polygon of sub-area k (`0x800351EC`) and takes its minimum view depth, the mean
// depth of its first four vertices and a horizontal frustum code:
//   * invisible (every vertex outside the same side, |x| > 16|z|/21, or behind z < -40): nibble 0;
//   * fine when the cell has a region 7, min depth < 9600 cell units (4800 on set 2) and fewer
//     than four cells have gone fine before it this frame (`s5 < 4`);
//   * bit 2 when the frustum test straddles or the mean depth is < 2048, and bits 2|3 when the
//     mean depth is < the fine threshold (`0x80035830..0x80035850`).
// `keepCulled` makes a group the ORIGINAL culls on its horizontal frustum draw coarse instead:
// our window is wider than the console's 4:3 frame, so its cull would open holes at the sides.
struct CellLodGroup {
    int visibility = 0; // 0 invisible, 1 straddling, 2 inside
    int32_t minZ = 0, meanZ = 0;
};
uint32_t CellLodWord(const rr::CellData& cell, const CellView& view, int set, int& fineCells, bool keepCulled,
                     std::vector<CellLodGroup>* groups = nullptr, bool maxDetail = false);
// `maxDetail` (the PC graphics settings): every group of a cell with a region 7
// goes fine at any distance and any number of cells (the original: nearer than 9600 / 4800 cell units and at most
// four cells a frame); the near-routine bits keep the original's thresholds.

// Band 2 - the road surface. `RASHCDG 0x8006E474` (and `0x8006F5D0`) draw each band-2 quad in one of
// two ways, by the largest view depth of its four corners (docs\formats\scene_cell.md 13.3):
//   * far (>= 4096 cell units): one GT4 with the primitive's own UVs, palette row `g`, where
//     g = (attr >> 2) & 0xC, Gouraud-shaded from each corner's colour-table entry;
//   * near: split into n = stripLines[attr & 0xF] >> 28 lane strips along the i0->i1 / i3->i2
//     edges, each strip textured by UV template 2*kind + nearByDepth[maxZ / 512] and palette row
//     g + 1 + clutsel, and lane lines drawn as flat quads 1/64..3/64 of a strip in from its edges.
// The subdivision the console then applies (`0x80069CF0`) interpolates UVs, colours and positions
// bilinearly, which is what a perspective-correct rasteriser does without it.
struct Band2Strip {
    float p[4][3] = {};    // corners in world units, in the GPU's vertex order
    uint8_t uv[4][2] = {}; // texel coordinates in the road page
    uint8_t rgb[4][3] = {};
    int32_t shade[4] = {}; // the colour-table index each corner's rgb came from (the subdivider averages it)
    int paletteRow = 0;
    bool nearPath = false;
    size_t cell = 0;
    int group = 0;
    int32_t otKey = 0; // the quad's ordering-table key: its largest corner view depth, cell units (>= 0)
};
struct Band2Line {
    float p[4][3] = {};
    uint32_t colour = 0; // 0x00BBGGRR
    size_t cell = 0;     // the strip's cell and region-2 group
    int group = 0;
    int32_t otKey = 0;   // the quad's (the lines share its slot: 0x8006E474 chains them before its strips)
};
struct Band2Frame {
    std::vector<Band2Strip> strips;
    std::vector<Band2Line> lines;
    size_t quadsFar = 0, quadsNear = 0, trianglesNotDrawn = 0, paletteOutOfRange = 0;
};
// The PC graphics settings' maximum detail: `allNear` takes the near path - the lane strips
// and the lane LINES - for every quad whatever its depth (the original: only when its farthest corner is nearer than
// 4096 cell units, 64 world units); `fixedDepth` >= 0 stands for the quads' depth (the UV template and the table
// key), so a band built once is the same from every view.
struct Band2Options {
    bool allNear = false;
    int32_t fixedDepth = -1;
    // Always the full-size road template (nearByDepth's 1) - with mipmapped smooth textures the GPU's filtering takes
    // the place of the original's smaller far template, and one template everywhere leaves no seam where they meet.
    bool fullTexture = false;
};
void CollectBand2(const rr::CellData& cell, size_t cellIndex, size_t fineGroup, const CellView& view,
                  const CellLook& look, Band2Frame& out, const Band2Options* options = nullptr);
// Triangles for a frame's band 2: strips as a `grid` x `grid` bilinear mesh each, lines flat.
// `uvInset` (the PC settings' smooth textures): each near strip's texel coordinates pulled that far inside its UV
// template, so bilinear filtering never reads the texels past the template's edge (0: the original's, unchanged).
void AppendBand2Soup(const Band2Frame& frame, int grid, rr::TriangleSoup& road, rr::TriangleSoup& lines, float uvInset = 0.0f);

// The original's subdivision of NEAR polygons (docs\formats\scene_cell.md 13.8), for the product.
// The PS1 GPU maps texels and colours affinely per triangle; the console hides the error close up by cutting the
// near road strips (`0x80069CF0`, level 5) and the fine near groups' polygons with a vertex nearer than 1024 cell
// units (`0x8006929C` / `0x80069784`, level 5 - (attr & 15)) into pieces by the depth of their corners. These run
// the PORTED subdividers (src\game\sim\subdiv.h) on the view the cell draw sees - x, y, z in cell units << 8,
// y scaled by the GTE row aspect, SXY by the GTE's own RTPS arithmetic (H 237, centre 192 / 120) - and turn every
// piece back into world-space triangles that the renderer then draws with screen-space (affine) interpolation.
// `sideSqueeze` > 1 pulls the screen x toward the centre before the outcodes, so a window wider than the console's
// 384 columns does not lose the pieces the console drops off its sides.
struct SubdivStats {
    size_t roadStrips = 0, roadPieces = 0;              // band-2 near strips through 0x80069CF0 and their GT4s
    size_t nearPrims = 0, splitPrims = 0, pieces = 0;   // fine near groups: primitives, subdivided ones, their pieces
    size_t culledPrims = 0, refused = 0;                // dropped on the outcodes; left the 24 records
    size_t backPrims = 0;                               // dropped by the back-face test (CellLook::nearNclip)
};
void AppendBand2SoupSubdivided(const Band2Frame& frame, const CellView& view, const CellLook& look, float sideSqueeze,
                               rr::TriangleSoup& road, rr::TriangleSoup& lines, SubdivStats& stats);
// One fine near group's primitives on page `texKey` (the 0x8006A630 path), subdivided where the original does.
void AppendFineNearGroup(const rr::CellData& cell, int group, uint16_t texKey, const CellView& view,
                         const CellLook& look, float sideSqueeze, rr::TriangleSoup& out, SubdivStats& stats);

// The shade and texture window a cell primitive's vertex `index` of band `band` is drawn with (scene_cell.md 13.1 /
// 13.2; BuildCellSoup's, also used by the GTE projection's per-frame builders, gte_proj.h).
void ShadeCellVertex(const rr::CellData& cell, const rr::CellPrimitive& prim, int band, uint16_t index,
                     const CellLook& look, rr::TriangleSoup::Vertex& v);

// Joins every type-8 cell to the type-9 chunk carrying its region 7 (scene_cell.md 3.2), by
// resource id exactly as `SLUS_010.53 0x80032B7C` does it. Returns how many were joined.
size_t JoinCellRegion7(const rr::DiscImage& disc, int set, const std::vector<rr::RouteLeg>& legs,
                       std::vector<rr::CellData>& cells, size_t* failed = nullptr);

// Which band a primitive belongs to, from the region-2 group it came from.
int CellBandOf(const rr::CellData& cell, const rr::CellPrimitive& prim);

// The page key a primitive samples, resolved through the half of the cell's header pair its band
// uses (docs\formats\scene_cell.md 12.6). `0x7800` and `0x7C00` name no resource of the cell's own
// and are passed through: `0x7800` is the fixed `DATA\G_OBJ01.GTP` page and `0x7C00` is band 2's
// constant, which no fix-up pass ever resolves.
uint16_t CellPrimitiveKey(const rr::CellData& cell, const rr::CellPrimitive& prim);

// The primitives of one cell, chosen the way the draw dispatcher `RASHCDG 0x80068FCC` chooses them.
// mode 0 = every group coarse, 1 = the B groups fine, 2 = the fine groups ALONE (a diagnostic),
// 3 = every group of bands 0 and 1 (the renderer then picks per group and per frame).
void CollectCellPrimitives(const rr::CellData& cell, int mode, bool wantBand2,
                           std::vector<const rr::CellPrimitive*>& out);

// Builds the roadside world from already-parsed scene cells. Band-0 primitives carry no normals,
// so they are emitted UNLIT with a zero normal, exactly as the props are - the console cannot
// Gouraud-shade a mesh that ships no normal array either, and inventing a face normal here would
// make every drawn pixel differ from the palette entry it samples.
// Mode 3 emits one run per (cell, region-2 group, page key) and fills every vertex's shade and
// texture window from `look`: band 0 flat-shaded by colour-table entry `attr & 0xFF` under window
// band0Window[t] (t = disc flags >> 4, t < 15), band 1 Gouraud-shaded by each vertex's `w & 0xFF`
// under band1Window[flags >> 4]. Band 2 is not in the static soup: it is built per frame.
rr::TriangleSoup BuildCellSoup(const std::vector<rr::CellData>& cells, int mode, bool withBand2,
                               bool stripQuads, std::vector<CellRange>* ranges = nullptr,
                               const CellLook* look = nullptr);

// Every texture key the loaded cells reference, so the stream can be scanned once for them.
std::vector<uint16_t> CellTextureKeys(const std::vector<rr::CellData>& cells, bool withBand1);

// Walks the stream ranges a race's legs cover, in file order, and hands every chunk whose type
// `wants` accepts to `visit` together with its absolute offset in the stream.
size_t ScanRaceStream(const rr::DiscImage& disc, int set, const std::vector<rr::RouteLeg>& legs,
                      const std::function<bool(uint8_t)>& wants,
                      const std::function<void(uint32_t, const std::vector<uint8_t>&)>& visit);

// A road surface built from the type-3 chunks of one .STP. The half width is provisional: XSAI
// proves a lane width of 9.766 world units with 2 lanes, but the exact lateral extent lives in
// XSDH, whose field split is not decoded.
rr::TriangleSoup BuildRoadSurface(const rr::DiscImage& disc, const rr::DiscFile& stp, size_t& roadChunks,
                                  size_t& sliceCount, std::vector<rr::RoadSlice>* outPath);

// A ribbon of road surface around a centre line, with the same provisional half width.
rr::TriangleSoup BuildRibbon(const std::vector<rr::RoadSlice>& path, float halfWidth);

// The roadside world from the scene cells of one .STP, with a per-triangle face normal. Used by
// the single-file road view, which has no cell ranges and no textures.
rr::TriangleSoup BuildSceneCells(const rr::DiscImage& disc, const rr::DiscFile& stp, size_t& cellCount);

// ---------------------------------------------------------------- driving on the decoded road
// A frame on the road: everything the game itself stores per slice, interpolated between slices.
struct RoadFrame {
    float pos[3] = {};
    float tangent[3] = {};
    float lateral[3] = {};
    float normal[3] = {};
};

RoadFrame SampleRoad(const std::vector<rr::RoadSlice>& path, float distance, size_t* indexOut = nullptr);

} // namespace rr::render
