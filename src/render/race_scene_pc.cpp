// The PC graphics settings in the race scene (race_scene.h SetPcOptions): smooth
// textures, the frustum cull of the cell runs and props, and maximum detail's road surface - band 2 with its lane lines
// at every distance. None of it runs unless the settings ask for it: with the defaults RaceScene draws the original's
// frame unchanged.
#include "render/race_scene.h"

#include "render/smooth_texture.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>

namespace rr::render {

namespace {

constexpr GLenum kTexture2DArray = 0x8C1A;
constexpr GLint kSmoothUnit = 2, kSmoothLayerUnit = 3; // units 0 / 1 the index image and the palettes, 5 / 6 the OT's

// A vertex array over a soup, the layout of race_scene.cpp's BindSoup (attributes 0..6).
void SoupArray(GLuint& vao, GLuint& vbo) {
    gl.GenVertexArrays(1, &vao);
    gl.BindVertexArray(vao);
    gl.GenBuffers(1, &vbo);
    gl.BindBuffer(GL_ARRAY_BUFFER, vbo);
    const GLsizei stride = static_cast<GLsizei>(sizeof(rr::TriangleSoup::Vertex));
    gl.VertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(0));
    gl.EnableVertexAttribArray(0);
    gl.VertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(sizeof(float) * 3));
    gl.EnableVertexAttribArray(1);
    gl.VertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(sizeof(float) * 6));
    gl.EnableVertexAttribArray(2);
    gl.VertexAttribPointer(3, 1, GL_UNSIGNED_SHORT, GL_FALSE, stride, reinterpret_cast<void*>(sizeof(float) * 8 + sizeof(uint16_t)));
    gl.EnableVertexAttribArray(3);
    gl.VertexAttribPointer(4, 4, GL_UNSIGNED_BYTE, GL_TRUE, stride, reinterpret_cast<void*>(offsetof(rr::TriangleSoup::Vertex, shade)));
    gl.EnableVertexAttribArray(4);
    gl.VertexAttribPointer(5, 4, GL_UNSIGNED_BYTE, GL_FALSE, stride, reinterpret_cast<void*>(offsetof(rr::TriangleSoup::Vertex, window)));
    gl.EnableVertexAttribArray(5);
    gl.VertexAttribPointer(6, 1, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(offsetof(rr::TriangleSoup::Vertex, ot)));
    gl.EnableVertexAttribArray(6);
}

void Upload(GLuint& vao, GLuint& vbo, const std::vector<rr::TriangleSoup::Vertex>& v, GLenum usage) {
    if (vao == 0) SoupArray(vao, vbo);
    gl.BindVertexArray(vao);
    gl.BindBuffer(GL_ARRAY_BUFFER, vbo);
    gl.BufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(v.size() * sizeof(rr::TriangleSoup::Vertex)), v.empty() ? nullptr : v.data(),
                  usage);
}

// The distance from `p` to the box (0 inside).
float BoxDistance(const std::array<float, 6>& b, const float p[3]) {
    float d2 = 0.0f;
    for (int k = 0; k < 3; ++k) {
        const float d = std::max({b[k] - p[k], 0.0f, p[k] - b[3 + k]});
        d2 += d * d;
    }
    return std::sqrt(d2);
}

} // namespace

void RaceScene::PcPrograms() {
    smoothLocation_ = gl.GetUniformLocation(program_, "uSmooth");
    smoothTexLocation_ = gl.GetUniformLocation(program_, "uSmoothTex");
    smoothLayerLocation_ = gl.GetUniformLocation(program_, "uSmoothLayer");
    layerPullLocation_ = gl.GetUniformLocation(program_, "uLayerPull");
    layerEyeLocation_ = gl.GetUniformLocation(program_, "uLayerEye");
    gl.UseProgram(program_);
    gl.Uniform1i(smoothLocation_, 0);
    // two samplers of different types on one unit make every draw invalid: these get units of their own
    gl.Uniform1i(smoothTexLocation_, kSmoothUnit);
    gl.Uniform1i(smoothLayerLocation_, kSmoothLayerUnit);
    // shaders.cpp uSmoothWiden: an object's minified footprint grows from 2 texels a pixel on, to sqrt(2)
    // times its size (half a mip level) at 2.8, so the average colour of a small distant object holds still under
    // sub-pixel motion. A factor of 2 steadies it more but fades a white car 150 units away into
    // the road. DEVELOPMENT: RRJB_SMOOTH_WIDEN=off - the footprint the derivatives give (the control);
    // =<factor> - another factor.
    float widen = 1.41421356f;
    if (const char* e = std::getenv("RRJB_SMOOTH_WIDEN")) widen = std::string(e) == "off" ? 1.0f : static_cast<float>(std::atof(e));
    gl.Uniform1f(gl.GetUniformLocation(program_, "uSmoothWiden"), widen);
    centroidLocation_ = gl.GetUniformLocation(program_, "uCentroidTexel");
    gl.Uniform1i(centroidLocation_, 0);
}

void RaceScene::BindSmooth(const GpuIndexedTexture& t) const {
    // shaders.cpp vTexelPC: every textured draw binds through here - the texel lookups at the centroid in a
    // multisampled picture (PcOptions::centroid). DEVELOPMENT: RRJB_CENTROID=off - at the pixel's centre (the control)
    static const bool centroidOff = std::getenv("RRJB_CENTROID") != nullptr && std::string(std::getenv("RRJB_CENTROID")) == "off";
    gl.Uniform1i(centroidLocation_, pc_.centroid && !centroidOff ? 1 : 0);
    const SmoothTexture* s = pc_.smoothTextures && t.Valid() ? SmoothFor(t.indexTexture) : nullptr;
    gl.Uniform1i(smoothLocation_, s != nullptr ? 1 : 0);
    if (s == nullptr) return;
    gl.ActiveTexture(GL_TEXTURE0 + kSmoothUnit);
    glBindTexture(kTexture2DArray, s->array);
    gl.ActiveTexture(GL_TEXTURE0 + kSmoothLayerUnit);
    glBindTexture(GL_TEXTURE_2D, s->layerMap);
    gl.ActiveTexture(GL_TEXTURE0);
}

void RaceScene::PcBoxes(const rr::TriangleSoup& soup) {
    rangeBox_.assign(cellRanges_.size(), {1e30f, 1e30f, 1e30f, -1e30f, -1e30f, -1e30f});
    size_t cells = 0;
    for (const CellRange& r : cellRanges_) cells = std::max(cells, r.cell + 1);
    cellBox_.assign(cells, {1e30f, 1e30f, 1e30f, -1e30f, -1e30f, -1e30f});
    std::map<uint16_t, std::set<int>> rows;
    for (size_t i = 0; i < cellRanges_.size(); ++i) {
        const CellRange& r = cellRanges_[i];
        std::array<float, 6>& b = rangeBox_[i];
        for (GLint k = r.first; k < r.first + r.count; ++k) {
            const size_t s = static_cast<size_t>(k);
            if (s >= soup.vertices.size()) break;
            const rr::TriangleSoup::Vertex& v = soup.vertices[s];
            const float p[3] = {v.x, v.y, v.z};
            for (int a = 0; a < 3; ++a) {
                b[a] = std::min(b[a], p[a]);
                b[3 + a] = std::max(b[3 + a], p[a]);
            }
            rows[r.texKey].insert(static_cast<int>(v.tpage));
        }
        std::array<float, 6>& c = cellBox_[r.cell];
        for (int a = 0; a < 3; ++a) {
            c[a] = std::min(c[a], b[a]);
            c[3 + a] = std::max(c[3 + a], b[3 + a]);
        }
    }
    // band 2 (the road surface, region 7) is not in the soup: the cells' own vertices widen their boxes
    if (cellData_ != nullptr)
        for (size_t c = 0; c < cellBox_.size() && c < cellData_->size(); ++c) {
            const rr::CellData& cell = (*cellData_)[c];
            for (size_t i = 0; i < cell.vertexCount; ++i) {
                const float p[3] = {rr::CellWorldX(cell, i), rr::CellWorldY(cell, i), rr::CellWorldZ(cell, i)};
                for (int a = 0; a < 3; ++a) {
                    cellBox_[c][a] = std::min(cellBox_[c][a], p[a]);
                    cellBox_[c][3 + a] = std::max(cellBox_[c][3 + a], p[a]);
                }
            }
        }
    pageRows_.clear();
    for (const auto& [key, set] : rows) pageRows_[key] = std::vector<int>(set.begin(), set.end());
    pageRowsNoted_ = false;
}

void RaceScene::PcFrustum(const Mat4& viewProj) {
    const float* m = viewProj.m;
    const auto row = [m](int i, float out[4]) {
        for (int c = 0; c < 4; ++c) out[c] = m[4 * c + i];
    };
    float r0[4], r1[4], r2[4], r3[4];
    row(0, r0);
    row(1, r1);
    row(2, r2);
    row(3, r3);
    for (int k = 0; k < 4; ++k) {
        frustum_[0][k] = r3[k] + r0[k];
        frustum_[1][k] = r3[k] - r0[k];
        frustum_[2][k] = r3[k] + r1[k];
        frustum_[3][k] = r3[k] - r1[k];
        frustum_[4][k] = r3[k] + r2[k];
        frustum_[5][k] = r3[k] - r2[k];
    }
    for (auto& p : frustum_) {
        const float l = std::sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
        if (l > 0.0f)
            for (float& c : p) c /= l;
    }
    // once: the props' bounding radii, and the pages' palette rows for the smooth textures
    if (propRadius_.size() != propFirst_.size()) {
        propRadius_.assign(propFirst_.size(), 0.0f);
        for (size_t g = 0; g < propFirst_.size(); ++g)
            for (GLsizei k = 0; k < propCount_[g]; ++k) {
                const rr::TriangleSoup::Vertex& v = propSoup_.vertices[static_cast<size_t>(propFirst_[g] + k)];
                propRadius_[g] = std::max(propRadius_[g], std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z) / kModelUnitsPerWorldUnit);
            }
    }
    if (pc_.smoothTextures && !pageRowsNoted_) {
        pageRowsNoted_ = true;
        for (const auto& [key, rows] : pageRows_) {
            const auto t = cellTextures_.find(key);
            if (t != cellTextures_.end() && key != rr::kCellTexBand2) NotePaletteRows(t->second.indexTexture, rows);
        }
    }
}

bool RaceScene::BoxVisible(const std::array<float, 6>& b) const {
    if (b[0] > b[3]) return false; // empty
    for (const auto& p : frustum_) {
        const float x = p[0] >= 0.0f ? b[3] : b[0], y = p[1] >= 0.0f ? b[4] : b[1], z = p[2] >= 0.0f ? b[5] : b[2];
        if (p[0] * x + p[1] * y + p[2] * z + p[3] < 0.0f) return false;
    }
    return true;
}

bool RaceScene::SphereVisible(const float c[3], float radius) const {
    for (const auto& p : frustum_)
        if (p[0] * c[0] + p[1] * c[1] + p[2] * c[2] + p[3] < -radius) return false;
    return true;
}

// Every fine group's band 2 once, near path at the farthest near template: the same from every view, so it lives in a
// static buffer; per piece a range of the road and of the lines.
void RaceScene::BuildStaticBand2() {
    staticBand2Built_ = true;
    staticBand2_.clear();
    staticBand2Index_.clear();
    if (cellData_ == nullptr || !look_.haveTables) return;
    rr::TriangleSoup road, lines;
    Band2Options options;
    options.allNear = true;
    options.fixedDepth = 4095; // the last entry of the near templates (nearByDepth[7])
    options.fullTexture = pc_.smoothTextures;
    staticBand2Full_ = pc_.smoothTextures;
    const CellView noView;
    for (size_t c = 0; c < cellData_->size(); ++c) {
        const rr::CellData& cell = (*cellData_)[c];
        if (!cell.region7Present) continue;
        for (size_t k = 0; k < cell.countB; ++k) {
            Band2Frame frame;
            CollectBand2(cell, c, k, noView, look_, frame, &options);
            StaticBand2Piece piece;
            piece.cell = c;
            piece.group = static_cast<int>(cell.countA + 2u * cell.countB + k);
            piece.roadFirst = static_cast<GLint>(road.vertices.size());
            piece.lineFirst = static_cast<GLint>(lines.vertices.size());
            AppendBand2Soup(frame, 1, road, lines, pc_.smoothTextures ? 0.5f : 0.0f);
            piece.roadCount = static_cast<GLsizei>(road.vertices.size()) - piece.roadFirst;
            piece.lineCount = static_cast<GLsizei>(lines.vertices.size()) - piece.lineFirst;
            if (piece.roadCount == 0 && piece.lineCount == 0) continue;
            staticBand2Index_[{c, piece.group}] = staticBand2_.size();
            staticBand2_.push_back(piece);
        }
    }
    // the ordering-table order (a user's choice with maximum detail): the model rule's key from the buffer
    for (rr::TriangleSoup::Vertex& v : road.vertices) v.ot = -1.0f;
    for (rr::TriangleSoup::Vertex& v : lines.vertices) v.ot = -1.0f;
    Upload(staticRoadVao_, staticRoadVbo_, road.vertices, GL_STATIC_DRAW);
    Upload(staticLineVao_, staticLineVbo_, lines.vertices, GL_STATIC_DRAW);
}

void RaceScene::PcDrawBand2(const DrawRequest& request, const std::vector<char>& isResident, int cellsDebug, int otherDebug) {
    if (!staticBand2Built_ || staticBand2Full_ != pc_.smoothTextures) BuildStaticBand2();
    const auto roadTexture = cellTextures_.find(rr::kCellTexBand2);
    if (roadTexture == cellTextures_.end() || !roadTexture->second.Valid() || cellData_ == nullptr) return;
    // A group whose cell comes nearer than this is built for the frame: the original's near templates change with the
    // depth below 4096 cell units (64 world units) and the subdivision cuts the near strips.
    constexpr float kNear = 72.0f;
    struct Piece {
        size_t cell;
        int group;
        bool dynamic;
        GLint roadFirst, lineFirst;
        GLsizei roadCount, lineCount;
    };
    std::vector<Piece> pieces;
    rr::TriangleSoup road, lines;
    Band2Options options;
    options.allNear = true;
    options.fullTexture = pc_.smoothTextures;
    for (size_t c = 0; c < isResident.size() && c < cellData_->size(); ++c) {
        if (!isResident[c]) continue;
        const rr::CellData& cell = (*cellData_)[c];
        if (!cell.region7Present || c >= cellBox_.size()) continue;
        if (pc_.cull && !BoxVisible(cellBox_[c])) {
            stats_.band2Culled += cell.countB;
            continue;
        }
        // RRJB_PC_STATIC_BAND2=off (DEVELOPMENT, a measurement): every group built per frame, no static band
        static const bool noStatic = std::getenv("RRJB_PC_STATIC_BAND2") != nullptr && std::string(std::getenv("RRJB_PC_STATIC_BAND2")) == "off";
        const bool isNear = noStatic || BoxDistance(cellBox_[c], request.eye) < kNear;
        for (size_t k = 0; k < cell.countB; ++k) {
            if (((lodWords_[c] >> (4 * k)) & 3u) != 3u) continue;
            const int group = static_cast<int>(cell.countA + 2u * cell.countB + k);
            if (isNear) {
                Band2Frame frame;
                CollectBand2(cell, c, k, request.cellView, look_, frame, &options);
                Piece p{c, group, true, static_cast<GLint>(road.vertices.size()), static_cast<GLint>(lines.vertices.size()), 0, 0};
                if (subdivide_)
                    AppendBand2SoupSubdivided(frame, request.cellView, look_, request.sideSqueeze, road, lines, subdivStats_);
                else
                    AppendBand2Soup(frame, 4, road, lines, pc_.smoothTextures ? 0.5f : 0.0f);
                p.roadCount = static_cast<GLsizei>(road.vertices.size()) - p.roadFirst;
                p.lineCount = static_cast<GLsizei>(lines.vertices.size()) - p.lineFirst;
                pieces.push_back(p);
            } else if (const auto it = staticBand2Index_.find({c, group}); it != staticBand2Index_.end()) {
                const StaticBand2Piece& s = staticBand2_[it->second];
                pieces.push_back({c, group, false, s.roadFirst, s.lineFirst, s.roadCount, s.lineCount});
            }
            ++stats_.band2Pieces;
        }
    }
    if (pieces.empty()) return;
    Upload(pcRoadVao_, pcRoadVbo_, road.vertices, GL_DYNAMIC_DRAW);
    Upload(pcLineVao_, pcLineVbo_, lines.vertices, GL_DYNAMIC_DRAW);
    BindIndexed(roadTexture->second);
    gl.Uniform1i(debugLocation_, cellsDebug);
    for (int pass = 0; pass < 2; ++pass) {
        const bool dynamic = pass == 1;
        gl.BindVertexArray(dynamic ? pcRoadVao_ : staticRoadVao_);
        OtSource(dynamic ? pcRoadVbo_ : staticRoadVbo_);
        for (const Piece& p : pieces)
            if (p.dynamic == dynamic && p.roadCount > 0) {
                OtCell(p.cell, p.group, false);
                glDrawArrays(GL_TRIANGLES, p.roadFirst, p.roadCount);
            }
    }
    // The lane lines lie in the road's plane: in front of their strips (the original chains them before the strips).
    gl.Uniform1i(texturedLocation_, 0);
    gl.Uniform1i(debugLocation_, otherDebug);
    BeginLines(); // with the depth buffer a decal pass
    for (int pass = 0; pass < 2; ++pass) {
        const bool dynamic = pass == 1;
        gl.BindVertexArray(dynamic ? pcLineVao_ : staticLineVao_);
        OtSource(dynamic ? pcLineVbo_ : staticLineVbo_);
        for (const Piece& p : pieces)
            if (p.dynamic == dynamic && p.lineCount > 0) {
                OtCell(p.cell, p.group, true);
                glDrawArrays(GL_TRIANGLES, p.lineFirst, p.lineCount);
            }
    }
    EndDecal();
    gl.BindVertexArray(cellVao_);
}

// ---- coplanar layers
//
// Some cell groups carry two primitives in ONE plane over the same area: a face drawn twice with two texture mappings
// (the hazard-striped barriers of the city races), two overlapping ivy strips over a tunnel mouth (race 1/25), a decal
// over a wall. Some pairs lie a few centimetres apart (the tunnel's strips: 2.6 cm at one end). The console draws them
// through its ordering table: each primitive at the slot of its farthest corner, and within a slot the first linked
// painted last (race_scene_ot.cpp) - so a whole primitive covers the other. A depth buffer compares the two depths
// pixel by pixel: in one plane they round differently for two different triangles and the textures mix in a
// dither-like pattern; a few centimetres apart the strips cross along a line that moves with the eye (in VR each eye
// sees its own crossing). With the depth buffer each pair gets the table's outcome per primitive, fixed per pair: the
// smaller primitive (whose farthest corner is the nearer one from most views, its slot drawn later) on top, of two
// alike the first in the record (the first linked). The winner is drawn once more after its run, from an element
// buffer over the same vertices (the GTE points too), pulled kLayerPull world units a layer toward the eye along its
// own view rays (shaders.cpp uLayerPull: the picture does not move) plus a polygon offset step a layer.
// The pairs follow coplanar.h's wider rule (strips a few degrees apart, the tunnel ceiling of 1/25), and the pull grows
// by each winner's LIFT (the pair's misfit along the ray, attribute 8): a fixed 8 cm is less than the misfit wherever a
// pair is seen obliquely. The float sum of VP world (world coordinates in the thousands) is a separate source of depth
// noise, handled by the camera-relative transform (multiview.h SetRenderOrigin).
namespace {

constexpr GLenum kElementArrayBuffer = 0x8893; // GL_ELEMENT_ARRAY_BUFFER
constexpr float kLayerPlane = 0.05f;           // world units: a primitive's corners this near the other's plane
// world units a layer is pulled toward the eye beyond its lift (the pair's misfit along the ray); a fixed 0.08 with no
// lift is less than the misfit wherever a pair is seen obliquely (the tunnel's ceiling from under it)
constexpr float kLayerPull = 0.02f;
// DEVELOPMENT: RRJB_LAYERS=1bx - the older layers (the strict pair rule, no lift, 0.08 a layer): the control
bool Layers1bx() {
    static const bool on = std::getenv("RRJB_LAYERS") != nullptr && std::string(std::getenv("RRJB_LAYERS")) == "1bx";
    return on;
}
constexpr int kLayerCap = 3;

} // namespace

void RaceScene::PcLayers(const rr::TriangleSoup& soup) {
    layerPrims_.clear();
    layerPairs_.clear();
    layerPairMisfit_.clear();
    runLayers_.clear();
    layersBuilt_ = false;
    layeredPrims_ = 0;
    layerMax_ = 0;
    // the primitives: the static soup's key rule (rmd3.h Vertex::ot, -(2 + k + 8 size)) says where each one starts and
    // how many soup vertices (3 or 6) it has
    std::vector<CoplanarPrim> prims;
    for (size_t r = 0; r < cellRanges_.size(); ++r) {
        const CellRange& range = cellRanges_[r];
        GLint i = range.first;
        const GLint end = std::min<GLint>(range.first + range.count, static_cast<GLint>(soup.vertices.size()));
        while (i + 3 <= end) {
            const float ot = soup.vertices[static_cast<size_t>(i)].ot;
            int size = 3;
            if (ot <= -2.0f) {
                const int code = static_cast<int>(-ot) - 2;
                if (code % 8 == 0 && (code / 8 == 3 || code / 8 == 6)) size = code / 8;
            }
            if (i + size > end) size = 3;
            CoplanarPrim p;
            p.run = r;
            p.first = i;
            p.size = size;
            CoplanarMeasure(soup.vertices, p);
            i += size;
            if (p.area < 1e-6f) continue;
            prims.push_back(p);
        }
    }
    // a pair: two of their triangles near parallel, the part of each over the other of positive area and within
    // kLayerPlane of the other's plane (coplanar.h: the all-corners rule, widened); candidates on a 16-unit
    // grid, one larger than 512 units in any direction (open ground) takes none
    CoplanarRule rule;
    rule.plane = kLayerPlane;
    rule.grid = 16.0f;
    rule.largest = 512.0f;
    rule.legacy = Layers1bx();
    const std::vector<CoplanarPair> pairs = FindCoplanarPairs(soup.vertices, prims, rule);
    // keep only the primitives in some pair
    std::vector<uint32_t> remap(prims.size(), UINT32_MAX);
    for (const CoplanarPair& pair : pairs)
        for (const uint32_t p : {pair.a, pair.b})
            if (remap[p] == UINT32_MAX) {
                remap[p] = static_cast<uint32_t>(layerPrims_.size());
                LayerPrim l;
                l.run = prims[p].run;
                l.first = prims[p].first;
                l.size = prims[p].size;
                l.area = prims[p].area;
                CoplanarNormal(soup.vertices, prims[p], l.n);
                layerPrims_.push_back(l);
            }
    for (const CoplanarPair& pair : pairs) {
        layerPairs_.emplace_back(remap[pair.a], remap[pair.b]);
        layerPairMisfit_.push_back(pair.misfit);
    }
}

void RaceScene::PcBuildLayers() {
    layersBuilt_ = true;
    runLayers_.assign(cellRanges_.size(), {});
    layeredPrims_ = 0;
    layerMax_ = 0;
    // DEVELOPMENT: RRJB_LAYERS=off - the depth buffer without the layers (the before / after control)
    static const bool off = std::getenv("RRJB_LAYERS") != nullptr && std::string(std::getenv("RRJB_LAYERS")) == "off";
    if (layerPairs_.empty() || off) return;
    // A cell's coarse group a + k (band 0) and its fine group a + B + k (band 1) are never drawn together
    // (CellLodWord's nibble): such a pair is no pair.
    const auto alternatives = [&](const LayerPrim& p, const LayerPrim& q) {
        const CellRange &rp = cellRanges_[p.run], &rq = cellRanges_[q.run];
        if (rp.cell != rq.cell || cellData_ == nullptr || rp.cell >= cellData_->size() || rp.group < 0 || rq.group < 0) return false;
        const rr::CellData& cell = (*cellData_)[rp.cell];
        const int a = cell.countA, b = cell.countB;
        const int lo = std::min(rp.group, rq.group), hi = std::max(rp.group, rq.group);
        return lo >= a && lo < a + b && hi == lo + b;
    };
    // the winner: the smaller primitive (areas compared to 1 %), of two alike the first in the soup - a strict order,
    // weakest first, so each primitive's layer is one above the highest layer it covers
    const auto rank = [&](uint32_t i) {
        const LayerPrim& p = layerPrims_[i];
        return std::make_pair(-static_cast<int>(std::lround(std::log(std::max(p.area, 1e-6f)) / std::log(1.01f))), -p.first);
    };
    std::vector<std::vector<std::pair<uint32_t, float>>> beneath(layerPrims_.size()); // what it covers, the misfit
    for (size_t k = 0; k < layerPairs_.size(); ++k) {
        const auto [a, b] = layerPairs_[k];
        if (alternatives(layerPrims_[a], layerPrims_[b])) continue;
        const bool aWins = rank(a) > rank(b);
        (aWins ? beneath[a] : beneath[b]).emplace_back(aWins ? b : a, layerPairMisfit_[k]);
    }
    std::vector<uint32_t> order(layerPrims_.size());
    for (uint32_t i = 0; i < order.size(); ++i) order[i] = i;
    std::sort(order.begin(), order.end(), [&](uint32_t x, uint32_t y) { return rank(x) < rank(y); });
    std::vector<int> layer(layerPrims_.size(), 0);
    std::vector<float> lift(layerPrims_.size(), 0.0f); // the misfit to what it covers plus that one's lift
    for (const uint32_t p : order)
        for (const auto& [q, misfit] : beneath[p]) {
            layer[p] = std::min(kLayerCap, std::max(layer[p], layer[q] + 1));
            lift[p] = std::max(lift[p], misfit + lift[q]);
        }
    if (const char* dump = std::getenv("RRJB_LAYERS_DUMP")) { // DEVELOPMENT: "<soup vertex>": its primitive's layer, pairs
        const GLint at = std::atoi(dump);
        for (uint32_t i = 0; i < layerPrims_.size(); ++i) {
            const LayerPrim& p = layerPrims_[i];
            if (at < p.first || at >= p.first + p.size) continue;
            std::printf("layers: primitive at %d (%d vertices, run %zu, area %.2f) layer %d lift %.4f normal (%.3f %.3f %.3f), covers", p.first, p.size, p.run, p.area, layer[i], lift[i], p.n[0], p.n[1], p.n[2]);
            for (const auto& [q, misfit] : beneath[i]) std::printf(" %d (%.3f)", layerPrims_[q].first, misfit);
            std::printf("\n");
        }
    }
    // the element buffer: per run, per layer, the winners' vertices in soup order
    std::vector<uint32_t> indices;
    std::vector<std::vector<uint32_t>> byRun(cellRanges_.size());
    for (uint32_t i = 0; i < layerPrims_.size(); ++i)
        if (layer[i] > 0) byRun[layerPrims_[i].run].push_back(i);
    for (size_t r = 0; r < byRun.size(); ++r) {
        if (byRun[r].empty()) continue;
        for (int l = 1; l <= kLayerCap; ++l) {
            LayerDraw draw;
            draw.layer = l;
            draw.first = static_cast<GLsizei>(indices.size());
            for (const uint32_t i : byRun[r]) {
                if (layer[i] != l) continue;
                for (int k = 0; k < layerPrims_[i].size; ++k) indices.push_back(static_cast<uint32_t>(layerPrims_[i].first + k));
                ++layeredPrims_;
                layerMax_ = std::max(layerMax_, l);
            }
            draw.count = static_cast<GLsizei>(indices.size()) - draw.first;
            if (draw.count > 0) runLayers_[r].push_back(draw);
        }
    }
    if (indices.empty() || cellVao_ == 0) return;
    if (!Layers1bx()) { // the winners' lifts (coplanar.h UploadLift): the pull clears the pair's misfit along every ray
        std::vector<float> perVertex(3 * static_cast<size_t>(std::max<GLsizei>(cellVertexCount_, 0)), 0.0f);
        for (uint32_t i = 0; i < layerPrims_.size(); ++i) {
            const LayerPrim& p = layerPrims_[i];
            if (layer[i] == 0 || lift[i] <= 0.0f) continue;
            for (int k = 0; k < p.size; ++k) {
                const size_t v = static_cast<size_t>(p.first + k);
                if (3 * v + 2 >= perVertex.size()) break;
                for (int a = 0; a < 3; ++a) perVertex[3 * v + static_cast<size_t>(a)] = p.n[a] * lift[i];
            }
        }
        UploadLift(cellVao_, layerLiftVbo_, perVertex);
    }
    gl.BindVertexArray(cellVao_); // the element buffer belongs to the cells' vertex array
    if (layerEbo_ == 0) gl.GenBuffers(1, &layerEbo_);
    gl.BindBuffer(kElementArrayBuffer, layerEbo_);
    gl.BufferData(kElementArrayBuffer, static_cast<GLsizeiptr>(indices.size() * sizeof(uint32_t)), indices.data(), GL_STATIC_DRAW);
}

void RaceScene::PcDrawLayers(size_t run, float eyeClipZ) const {
    if (run >= runLayers_.size() || runLayers_[run].empty() || layerEbo_ == 0) return;
    gl.Uniform3f(layerEyeLocation_, eyeWorld_[0], eyeWorld_[1], eyeWorld_[2]);
    glEnable(GL_POLYGON_OFFSET_FILL);
    for (const LayerDraw& d : runLayers_[run]) {
        // kLayerPull world units a layer in front plus the winner's lift over the cosine to the ray (shaders.cpp), and
        // a polygon offset step a layer (far away, where the buffer's precision is coarser than the pull)
        const float l = static_cast<float>(d.layer);
        gl.Uniform2f(layerPullLocation_, (Layers1bx() ? 0.08f : kLayerPull) * l - decalProbeShift_, eyeClipZ);
        glPolygonOffset(-1.0f * l, -4.0f * l);
        glDrawElements(GL_TRIANGLES, d.count, GL_UNSIGNED_INT,
                       reinterpret_cast<const void*>(static_cast<size_t>(d.first) * sizeof(uint32_t)));
    }
    gl.Uniform2f(layerPullLocation_, 0.0f, 0.0f);
    glDisable(GL_POLYGON_OFFSET_FILL);
}

// ---- decal passes
//
// A decal drawn over a plate already in the depth buffer: the lane lines over the road (band 2), a model's winners over
// its own losers (coplanar.h ModelLayers). The polygon offset alone (-1 slope, -4 steps) covered them only where the
// surface is seen at a grazing angle: seen more head-on - the road beside the bike from a VR head 1.2 m up, a sign
// face - the slope term is a few thousandths of a millimetre at the 0.05 near plane, less than the two surfaces' own
// misfit (the road strips are triangulated from the bilinear quad the lines lie on, float corners of coordinates in the
// thousands) and far less than the float noise of an absolute transform (~1 cm of depth per metre at near 0.05, removed
// by multiview.h SetRenderOrigin), so the decal would lose or win pixel by pixel, differently every frame and in each
// eye. A pull along the view rays
// (shaders.cpp uLayerPull: the picture keeps its place) sets a margin in world units at every distance; the slope term
// still grows it far away. The decal does not write its depth: what is drawn later meets the plate's.
namespace {

constexpr float kLinePull = 0.05f;  // world units: the lane lines over the road
constexpr float kModelPull = 0.03f; // world units a layer: a model's winners (their plates within 1 cm, coplanar.cpp)

} // namespace

void RaceScene::BeginDecal(float pull, float steps, bool writeDepth) const {
    glEnable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(-1.0f * steps, -4.0f * steps);
    gl.Uniform2f(layerPullLocation_, pull - decalProbeShift_, eyeClipZ_);
    gl.Uniform3f(layerEyeLocation_, eyeWorld_[0], eyeWorld_[1], eyeWorld_[2]);
    if (!writeDepth) glDepthMask(GL_FALSE);
}

void RaceScene::EndDecal() const {
    gl.Uniform2f(layerPullLocation_, 0.0f, 0.0f);
    glDisable(GL_POLYGON_OFFSET_FILL);
    glDepthMask(GL_TRUE);
}

void RaceScene::DrawModelLayers(const ModelLayers& layers, size_t range) const {
    if (otOrder_) return; // the ordering table resolves them as the console does
    const std::vector<ModelLayers::Draw>& draws = layers.Of(range);
    if (draws.empty()) return;
    for (const ModelLayers::Draw& d : draws) {
        const float l = static_cast<float>(d.layer);
        BeginDecal(kModelPull * l, l, false);
        glDrawElements(GL_TRIANGLES, d.count, GL_UNSIGNED_INT, reinterpret_cast<const void*>(static_cast<size_t>(d.first) * sizeof(uint32_t)));
    }
    EndDecal();
}

void RaceScene::BeginLines() const {
    // DEVELOPMENT: RRJB_DECALS=off - the lines with the polygon offset only (the before / after control)
    static const bool off = std::getenv("RRJB_DECALS") != nullptr && std::string(std::getenv("RRJB_DECALS")) == "off";
    if (!otOrder_ && !off) {
        BeginDecal(kLinePull, 1.0f, false);
        return;
    }
    glEnable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(-1.0f, -4.0f);
    if (!otOrder_ && decalProbeShift_ != 0.0f) gl.Uniform2f(layerPullLocation_, -decalProbeShift_, eyeClipZ_);
}

} // namespace rr::render
