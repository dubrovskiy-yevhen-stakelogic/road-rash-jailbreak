// The 3D scene in the original's ordering-table order (race_scene.h SetOtOrder).
//
// The console draws with no depth buffer. Every emitter links its primitive into an ordering table (*(0x8005B470) +
// 0x108, ClearOTagR'd, DrawOTag from entry *(0x8005ADFC) - 1 down to 0) at the slot its depth key picks, AddPrim-style
// (the newest first in the slot's chain, so the first linked is painted last). The keys, read from our disassembly
// (SLUS_010.53 / RASHCDG.BIN):
//   * models, SLUS 0x800251E4: (SZ[i0] + SZ[i1] + 2 SZ[i2]) >> (exp + 2) of each record's first three corners (the
//     view depth in cell units, 1/64 world unit) - quads and triangles alike;
//   * cells, RASHCDG 0x8006D350 / 0x8006DC20 (region 6), 0x8006A630 / 0x8006C888 (band 1), 0x8006E474 / 0x8006F5D0
//     (band 2): the largest view depth of the primitive's corners; a near polygon's pieces (0x8006929C, 0x80069784,
//     0x80069CF0) and band 2's lane lines are chained into the parent's slot;
//   * the shadow, SLUS 0x80025EE0: AVSZ4 * 4 (the port hands us its slot);
//   * the effects (fx_draw.h): OTZ * 4.
// All go through one map (ot_order.h), clamped to the table's length - 2.
//
// THE TWO TABLES. SLUS 0x80035958 does not draw a view through one table. Its cell list (0x800D9B80 + 48 p) comes
// sorted: 0x80035040 (via 0x800353C4) takes each cell's depth range over its region-3 polygon 0 (+0x2C max from -641,
// +0x30 min from 0xFFFF), 0x800353C4 puts the cell the camera is in first, 0x80036438 bubble-sorts the rest by the
// max (by +0x3C, sub-area 1's min, when two maxima are within 0x280). Walking the list from its end, a cell goes to the
// FIRST table when it draws no fine group (+0x34 & 0x22222222 == 0) or lies far (min > 0x7FF or max > 0x3FFF), else
// to the second; each table gets its own map from 0x80021988(min, max) of its cells - near = min - 0x3C0 and the
// shift from the EXE's table 0x80053224[min >> 11] when min >= 0, else near 0 and the table's first shift; length
// ((max - min) + 0x780 + 2^shift) >> shift, + 0x300 while near < 0x1000, at most 0x513 - then the objects filed in
// those cells (0x800674D4) and the cells (0x80069200) are linked and the table is drawn (0x80048DB4) before the next
// is built. So everything of the second (near) table is painted over everything of the first.
//
// The renderer keeps GL's depth test but gives every primitive its slot as its depth, constant across the primitive
// (shaders.cpp uOtOrder), the first table in [0.5, 1), the second in [0, 0.5): with GL_LESS a nearer slot covers a
// farther one whatever the real geometry says, and within one slot the first drawn wins, as the first linked does.
// Within a slot, the link order: a table's objects first (0x800674D4 runs before the cells), their shadow at the
// emitter's tail, then cell by cell from the far end of the list, and in a cell (the dispatcher 0x80068FCC) the
// always-coarse groups, then per sub-area k the lane lines (chained before their strips), the road, band 1 or the
// coarse group. The semi-transparent shadow is drawn after the whole opaque view, depth-tested against the slots and
// not written, so what is linked nearer covers it and what is linked farther lies under it.
#include "render/race_scene.h"

#include "render/ot_order.h"
#include "game/sim/cell_sort.h" // 0x80021988's map (OtTableMapOf)

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace rr::render {

namespace {
constexpr GLint kOtUnit = 5; // the texture unit of uOtVerts (units 0 and 1 are the index image and the palettes)
static_assert(sizeof(rr::TriangleSoup::Vertex) % sizeof(float) == 0, "the buffer texture reads the soup as floats");

int32_t ReadS32(const uint8_t* ram, uint32_t address) {
    int32_t v = 0;
    std::memcpy(&v, ram + (address & 0x1FFFFFu), 4);
    return v;
}

// Ranks within one slot, in 1024ths: the objects 0, their shadow 1, then 48 per cell in link order.
constexpr int kRankShadow = 1, kRankCellBase = 4, kRankPerCell = 48;

// The cell record of the list slot i (0x800D87EC + 0x70 i): +0x04 the id, +0x2C / +0x30 the depth range.
constexpr uint32_t kCellCtx = 0x800D87ECu, kCellCtxBytes = 0x70u;
} // namespace

void RaceScene::OtPrograms() {
    otOrderLocation_ = gl.GetUniformLocation(program_, "uOtOrder");
    otVertsLocation_ = gl.GetUniformLocation(program_, "uOtVerts");
    otStrideLocation_ = gl.GetUniformLocation(program_, "uOtStride");
    otRankLocation_ = gl.GetUniformLocation(program_, "uOtRank");
    otNearLocation_ = gl.GetUniformLocation(program_, "uOtNear");
    otShiftLocation_ = gl.GetUniformLocation(program_, "uOtShift");
    otMaxLocation_ = gl.GetUniformLocation(program_, "uOtMax");
    otBaseLocation_ = gl.GetUniformLocation(program_, "uOtBase");
    gl.UseProgram(program_);
    // A samplerBuffer left on unit 0 next to the sampler2D there would make every draw an error.
    gl.Uniform1i(otVertsLocation_, kOtUnit);
    gl.Uniform1i(otOrderLocation_, 0);
    gl.Uniform1i(otStrideLocation_, static_cast<GLint>(sizeof(rr::TriangleSoup::Vertex) / sizeof(float)));
    gl.Uniform1f(otRankLocation_, 0.0f);
    // A vertex array that does not feed attribute 6 (the rivals', the cars', the pedestrians', the weapons' buffers)
    // reads this: the model rule.
    gl.VertexAttrib1f(6, -1.0f);
}

void RaceScene::OtBeginDraw() {
    gl.Uniform1i(otOrderLocation_, otOrder_ ? 1 : 0);
    if (!otOrder_) return;
    OtUsePass(otPass_[otPass_[1].used ? 1 : 0], 0);
    glDepthFunc(GL_LESS); // the first drawn wins a tie, as the first linked into a slot does
    glEnable(GL_CLIP_DISTANCE0);
    glEnable(GL_CLIP_DISTANCE1);
}

void RaceScene::OtUsePass(const OtPassMap& map, int rank) const {
    gl.Uniform1i(otNearLocation_, map.nearOffset);
    gl.Uniform1i(otShiftLocation_, map.shift);
    gl.Uniform1i(otMaxLocation_, map.maxSlot);
    gl.Uniform1f(otBaseLocation_, map.base);
    gl.Uniform1f(otRankLocation_, static_cast<float>(rank));
}

// ---------------------------------------------------------------------------- the per-cell tables
// With the arena's own sort (the PORTED 0x800353C4 / 0x80036438 / 0x80035680 the session ran, or a capture's), the
// plan is 0x80035958's walk of the list itself: from its end, a slot of -1 skipped, a cell whose largest depth +0x2C is
// not positive skipped (not drawn at all), else into the FIRST table when its draw word +0x34 has no fine group or it
// lies far (min +0x30 > 0x7FF or max > 0x3FFF), into the second otherwise; each table's map 0x80021988(min, max) of
// its cells, PORTED (cell_sort.h OtTableMapOf).
bool RaceScene::OtArenaLodWords() {
    if (std::getenv("RRJB_CELL_SORT_TRACE")) // DEVELOPMENT
        std::printf("cellsort: arena words: on %d ram %d ids %d\n", otArena_ ? 1 : 0, otRam_ ? 1 : 0, otCellIds_ ? 1 : 0);
    if (!otArena_ || !otRam_ || !otCellIds_) return false;
    rr::sim::GuestRam g(const_cast<uint8_t*>(otRam_), 0x8005AC8Cu);
    const uint32_t v = static_cast<uint32_t>(otView_ & 1);
    const int32_t n = std::min(g.S32(g.gp() + 0x8DCu + 4u * v), 12);
    for (int32_t k = 0; k < n; ++k) {
        const int32_t slot = g.S32(0x800D9B80u + 48u * v + 4u * static_cast<uint32_t>(k));
        if (slot < 0 || slot >= 24) continue;
        const uint32_t ctx = kCellCtx + kCellCtxBytes * static_cast<uint32_t>(slot);
        const auto it = otCellIds_->find(g.U32(ctx + 4u));
        if (it != otCellIds_->end() && it->second < lodWords_.size()) lodWords_[it->second] = g.U32(ctx + 0x34u);
        if (std::getenv("RRJB_CELL_SORT_TRACE")) // DEVELOPMENT
            std::printf("cellsort: view %u slot %d id %08X -> cell %d word %08X max %d min %d\n", v, slot, g.U32(ctx + 4u),
                        it != otCellIds_->end() ? static_cast<int>(it->second) : -1, g.U32(ctx + 0x34u), g.S32(ctx + 0x2Cu),
                        g.S32(ctx + 0x30u));
    }
    return true;
}

void RaceScene::OtPlanArena() {
    rr::sim::GuestRam g(const_cast<uint8_t*>(otRam_), 0x8005AC8Cu);
    const uint32_t v = static_cast<uint32_t>(otView_ & 1);
    const int32_t n = std::min(g.S32(g.gp() + 0x8DCu + 4u * v), 12);
    int32_t lo[2] = {0x4000, 0x4000}, hi[2] = {0, 0};
    int16_t seq[2] = {0, 0};
    for (int32_t k = n - 1; k >= 0; --k) {
        const int32_t slot = g.S32(0x800D9B80u + 48u * v + 4u * static_cast<uint32_t>(k));
        if (slot < 0 || slot >= 24) continue;
        const uint32_t ctx = kCellCtx + kCellCtxBytes * static_cast<uint32_t>(slot);
        const int32_t mx = g.S32(ctx + 0x2Cu), mn = g.S32(ctx + 0x30u);
        if (!(0 < mx)) continue;
        const int pass = ((g.U32(ctx + 0x34u) & 0x22222222u) == 0 || 0x7FF < mn || 0x3FFF < mx) ? 1 : 2;
        const auto it = otCellIds_->find(g.U32(ctx + 4u));
        if (it != otCellIds_->end() && it->second < otCellPass_.size()) {
            otCellPass_[it->second] = static_cast<int8_t>(pass);
            otCellSeq_[it->second] = seq[pass - 1];
            ++otCheck_.cells;
            ++otCheck_.depthsEqual; // the arena's own
        }
        ++seq[pass - 1];
        lo[pass - 1] = std::min(lo[pass - 1], mn);
        hi[pass - 1] = std::max(hi[pass - 1], mx);
    }
    for (int p = 0; p < 2; ++p) {
        if (seq[p] == 0) continue;
        const rr::sim::OtTableMap map = rr::sim::OtTableMapOf(g, lo[p], hi[p]);
        OtPassMap& m = otPass_[p];
        m.used = true;
        m.nearOffset = map.nearOffset;
        m.shift = map.shift & 31;
        m.maxSlot = std::max(map.length - 2, 0);
    }
    if (otPass_[0].used && otPass_[1].used) ++otCheck_.twoPassFrames;
    if (otCheckOn_) {
        ++otCheck_.passes;
        const OtPassMap& last = otPass_[1].used ? otPass_[1] : otPass_[0];
        otCheck_.mapEqual = last.used && last.nearOffset == ReadS32(otRam_, 0x8005B4D4u) &&
                            last.shift == ReadS32(otRam_, 0x8005B4D8u) && last.maxSlot + 2 == ReadS32(otRam_, 0x8005ADFCu);
    }
}

int RaceScene::OtTableOfEntity(uint32_t e) const {
    if (!otArena_ || !otRam_ || !otCellIds_ || e < 0x80000000u || e >= 0x80200000u) return 0;
    const auto it = otCellIds_->find(static_cast<uint32_t>(ReadS32(otRam_, e + 0xB0u)));
    if (it == otCellIds_->end() || it->second >= otCellPass_.size() || otCellPass_[it->second] == 0) return -1;
    return otCellPass_[it->second];
}

void RaceScene::OtPlan(const std::vector<size_t>& visit, const CellView& view) {
    const size_t n = cellData_ ? cellData_->size() : 0;
    otCellPass_.assign(n, 0);
    otCellSeq_.assign(n, 0);
    otPass_[0] = OtPassMap{};
    otPass_[1] = OtPassMap{};
    otPass_[1].base = 0.0f;
    ++otCheck_.frames;
    if (otArena_ && otRam_ && otCellIds_) {
        OtPlanArena();
        return;
    }
    struct Entry {
        size_t cell = 0;
        int32_t maxZ = -641, minZ = 0xFFFF, sub1 = 0;
    };
    std::vector<Entry> list;
    for (size_t c : visit) {
        if (c >= n) continue;
        bool seen = false;
        for (const Entry& e : list) seen = seen || e.cell == c;
        if (seen) continue;
        const rr::CellData& cell = (*cellData_)[c];
        Entry e;
        e.cell = c;
        // 0x80035040: the view depth (MVMVA, cell units) of every vertex of the region-3 polygon 0
        if (cell.region3.size() >= 2u) {
            const int32_t from = cell.region3[0], to = cell.region3[1];
            for (int32_t i = from; i >= 0 && i < to && static_cast<size_t>(i) < cell.region3.size(); ++i) {
                const int16_t index = cell.region3[static_cast<size_t>(i)];
                if (index < 0 || static_cast<size_t>(index) >= cell.vertexCount) continue;
                const double d[3] = {(rr::CellWorldX(cell, static_cast<size_t>(index)) - view.eye[0]) * 64.0,
                                     (rr::CellWorldY(cell, static_cast<size_t>(index)) - view.eye[1]) * 64.0,
                                     (rr::CellWorldZ(cell, static_cast<size_t>(index)) - view.eye[2]) * 64.0};
                const int32_t z = static_cast<int32_t>(
                    std::floor(d[0] * view.forward[0] + d[1] * view.forward[1] + d[2] * view.forward[2]));
                e.minZ = std::min(e.minZ, z);
                e.maxZ = std::max(e.maxZ, z);
            }
        }
        // +0x3C: sub-area 1's min depth (0x80035680 stores it; the sort reads the one it holds)
        std::vector<CellLodGroup> groups;
        int fine = 0;
        CellLodWord(cell, view, raceSet_, fine, true, &groups);
        if (groups.size() > 1) e.sub1 = groups[1].minZ;
        list.push_back(e);
    }
    // 0x800353C4: the camera's cell to the front (its max, when negative, becomes 0x500); 0x80036438 sorts the rest
    size_t sortFrom = 0;
    for (size_t i = 0; i < list.size(); ++i)
        if (otCameraCell_ >= 0 && list[i].cell == static_cast<size_t>(otCameraCell_)) {
            std::swap(list[0], list[i]);
            if (list[0].maxZ < 0) list[0].maxZ = 0x500;
            sortFrom = 1;
            break;
        }
    for (bool swapped = true; swapped;) {
        swapped = false;
        for (size_t i = sortFrom; i + 1 < list.size(); ++i) {
            const Entry& a = list[i];
            const Entry& b = list[i + 1];
            const int32_t d = a.maxZ - b.maxZ;
            const bool swap = (d < 0 ? -d : d) < 0x280 ? b.sub1 < a.sub1 : b.maxZ < a.maxZ;
            if (swap) {
                std::swap(list[i], list[i + 1]);
                swapped = true;
            }
        }
    }
    // 0x80035958: from the list's end, each cell into the first or the second table
    int32_t lo[2] = {0x4000, 0x4000}, hi[2] = {0, 0};
    int16_t seq[2] = {0, 0};
    for (size_t i = list.size(); i-- > 0;) {
        const Entry& e = list[i];
        const uint32_t word = e.cell < lodWords_.size() ? lodWords_[e.cell] : 0u;
        const int pass = ((word & 0x22222222u) == 0 || e.minZ > 0x7FF || e.maxZ > 0x3FFF) ? 1 : 2;
        otCellPass_[e.cell] = static_cast<int8_t>(pass);
        otCellSeq_[e.cell] = seq[pass - 1]++;
        lo[pass - 1] = std::min(lo[pass - 1], e.minZ);
        hi[pass - 1] = std::max(hi[pass - 1], e.maxZ);
    }
    // 0x80021988(min, max): each table's map
    for (int p = 0; p < 2; ++p) {
        if (seq[p] == 0) continue;
        OtPassMap& m = otPass_[p];
        m.used = true;
        const auto table = [&](int32_t i) { return otRam_ ? ReadS32(otRam_, 0x80053224u + 4u * static_cast<uint32_t>(i)) : 5; };
        int32_t span = 0, nearOffset = 0, shift = table(0);
        if (hi[p] < 0) {
            span = 0x780;
        } else if (lo[p] < 0) {
            span = hi[p] + 0x780;
        } else {
            span = hi[p] - lo[p] + 0x780;
            nearOffset = lo[p] - 0x3C0;
            shift = table(std::min(lo[p] >> 11, 9));
        }
        if (shift < 0 || shift > 15) shift = 5;
        int32_t length = (span + (1 << shift)) >> shift;
        if (nearOffset < 0x1000) length += 0x300;
        length = std::min(length, 0x513);
        m.nearOffset = nearOffset;
        m.shift = shift;
        m.maxSlot = std::max(length - 2, 0);
    }
    if (otPass_[0].used && otPass_[1].used) ++otCheck_.twoPassFrames;
    // The check: the arena's own depths of these cells (in --parity the capture's, written by the original) and its
    // map (the last table the capture had set up).
    if (otCheckOn_ && otRam_ && otCellIds_) {
        ++otCheck_.passes;
        for (uint32_t slot = 0; slot < 24u; ++slot) {
            const uint32_t ctx = kCellCtx + kCellCtxBytes * slot;
            const auto it = otCellIds_->find(static_cast<uint32_t>(ReadS32(otRam_, ctx + 4u)));
            if (it == otCellIds_->end()) continue;
            for (const Entry& e : list) {
                if (e.cell != it->second) continue;
                ++otCheck_.cells;
                const int32_t dMax = std::abs(e.maxZ - ReadS32(otRam_, ctx + 0x2Cu)), dMin = std::abs(e.minZ - ReadS32(otRam_, ctx + 0x30u));
                otCheck_.worst = std::max(otCheck_.worst, std::max(dMax, dMin));
                if (std::max(dMax, dMin) <= 16) ++otCheck_.depthsEqual; // our float camera against the GTE's
                if (std::getenv("RRJB_OT_TRACE")) // DEVELOPMENT: ours against the arena's
                    std::printf("otsort: cell %zu (slot %u) max %d / %d, min %d / %d, sub1 %d / %d, pass %d seq %d lod %08X / %08X\n", e.cell,
                                slot, e.maxZ, ReadS32(otRam_, ctx + 0x2Cu), e.minZ, ReadS32(otRam_, ctx + 0x30u), e.sub1,
                                ReadS32(otRam_, ctx + 0x3Cu), otCellPass_[e.cell], otCellSeq_[e.cell],
                                e.cell < lodWords_.size() ? lodWords_[e.cell] : 0u, static_cast<uint32_t>(ReadS32(otRam_, ctx + 0x34u)));
            }
        }
        const OtPassMap& last = otPass_[1].used ? otPass_[1] : otPass_[0];
        otCheck_.mapEqual = last.used && last.nearOffset == ReadS32(otRam_, 0x8005B4D4u) &&
                            last.shift == ReadS32(otRam_, 0x8005B4D8u) && last.maxSlot + 2 == ReadS32(otRam_, 0x8005ADFCu);
    }
}

void RaceScene::OtCell(size_t cell, int group, bool line) const {
    if (!otOrder_) return;
    const int pass = cell < otCellPass_.size() && otCellPass_[cell] != 0 ? otCellPass_[cell] : 1;
    int rank = 0; // the dispatcher RASHCDG 0x80068FCC's order within the cell
    if (cellData_ && cell < cellData_->size() && group >= 0) {
        const rr::CellData& c = (*cellData_)[cell];
        const int a = c.countA, b = std::max<int>(1, c.countB);
        if (group >= a) {
            const int k = (group - a) % b;
            const bool band2 = group >= a + 2 * static_cast<int>(c.countB);
            rank = band2 ? (line ? 1 : 2) + 3 * k : 3 + 3 * k;
        }
    }
    rank = std::min(rank, kRankPerCell - 1);
    const int seq = cell < otCellSeq_.size() ? otCellSeq_[cell] : 0;
    OtUsePass(OtPass(pass), std::min(kRankCellBase + seq * kRankPerCell + rank, 1022));
}

int RaceScene::OtPassOfObject(uint32_t obj) const {
    int pass = otPass_[1].used ? 2 : 1;
    if (otRam_ && otCellIds_ && obj >= 0x80000000u && obj < 0x80200000u) {
        const auto it = otCellIds_->find(static_cast<uint32_t>(ReadS32(otRam_, obj + 0xB0u)));
        if (it != otCellIds_->end() && it->second < otCellPass_.size() && otCellPass_[it->second] != 0)
            pass = otCellPass_[it->second];
    }
    return OtPass(pass).used ? pass : (pass == 2 ? 1 : 2);
}

void RaceScene::OtObject(uint32_t obj) const {
    if (!otOrder_) return;
    OtUsePass(OtPass(OtPassOfObject(obj)), 0);
}

void RaceScene::OtSource(GLuint vbo) const {
    if (!otOrder_ || vbo == 0) return;
    // One buffer texture per vertex buffer, attached once: it names the buffer object, so a store respecified by
    // BufferData (the pedestrians', the weapons', the posed machines') is what it reads next. No glGet here: a query
    // per draw would wait on the pipeline.
    GLuint& texture = otTextures_[vbo];
    gl.ActiveTexture(GL_TEXTURE0 + kOtUnit);
    if (texture == 0) {
        glGenTextures(1, &texture);
        glBindTexture(GL_TEXTURE_BUFFER, texture);
        gl.TexBuffer(GL_TEXTURE_BUFFER, GL_R32F, vbo);
    } else {
        glBindTexture(GL_TEXTURE_BUFFER, texture);
    }
    gl.ActiveTexture(GL_TEXTURE0); // every other binding of the renderer is made on unit 0 or restores it
    ++otSources_;
}

void RaceScene::FlushOtShadows(const Mat4& viewProj) {
    if (!otOrder_) return;
    DrawRequest request;
    request.viewProj = viewProj;
    otFlushing_ = true;
    DrawPortedShadows(request, 0);
    otFlushing_ = false;
}

void RaceScene::OtEnd() const {
    if (!otOrder_) return;
    glDisable(GL_CLIP_DISTANCE0);
    glDisable(GL_CLIP_DISTANCE1);
}

} // namespace rr::render
