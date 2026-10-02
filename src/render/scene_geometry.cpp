#include "render/scene_geometry.h"

#include "game/sim/model_draw.h"
#include "game/sim/subdiv.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>
#include <string>

namespace rr::render {

int CellBandOf(const rr::CellData& cell, const rr::CellPrimitive& prim) {
    const size_t a = cell.countA, b = cell.countB;
    if (prim.group < a + b) return 0;
    return prim.group < a + 2 * b ? 1 : 2;
}

uint16_t CellPrimitiveKey(const rr::CellData& cell, const rr::CellPrimitive& prim) {
    if (prim.texRef == rr::kCellTexRuntimePage || prim.texRef == rr::kCellTexBand2) return prim.texRef;
    const std::vector<uint16_t>& keys = CellBandOf(cell, prim) == 0 ? cell.texKeyBand0 : cell.texKeyBand1;
    const int found = rr::CellTextureSlot(keys, prim.texRef);
    return found < 0 ? 0 : keys[static_cast<size_t>(found)];
}

void CollectCellPrimitives(const rr::CellData& cell, int mode, bool wantBand2,
                           std::vector<const rr::CellPrimitive*>& out) {
    // groups [0, A) always come from region 6, and each of the B groups after them is drawn EITHER
    // as region-6 group A+k (coarse) OR as region-7 groups A+B+k and A+2B+k (fine). The console
    // picks per group from a nibble in its draw context; which value that nibble takes is not
    // established, so `fine` takes the whole cell one way or the other and a cell with no region 7
    // can only go coarse.
    const bool fine = mode != 0 && cell.region7Present;
    for (const rr::CellPrimitive& prim : cell.band0) {
        if (mode == 2) break;
        if (prim.group < cell.countA || !fine) out.push_back(&prim);
    }
    if (!fine) return;
    for (const rr::CellPrimitive& prim : cell.band1) out.push_back(&prim);
    if (wantBand2)
        for (const rr::CellPrimitive& prim : cell.band2) out.push_back(&prim);
}

// The shade and texture window of one cell primitive, the way the original's draw routines give
// them to the GPU (docs\formats\scene_cell.md 13.1 / 13.2).
void ShadeCellVertex(const rr::CellData& cell, const rr::CellPrimitive& prim, int band, uint16_t index,
                     const CellLook& look, rr::TriangleSoup::Vertex& v) {
    if (look.shade && look.haveColours) {
        // Band 0: ONE colour word per primitive, `0x800D4CA8[attr & 0xFF]` (`RASHCDG 0x8006DB14..
        // 0x8006DB34`: `lbu 62(scratch)` = record +0x02, low byte). Bands 1 and 2: one per vertex,
        // `0x800D4CA8[w & 0xFF]` (`0x8006CBF0..0x8006CC0C`).
        const uint32_t word = band == 0 ? look.colours[prim.attr & 0xFFu]
                                        : look.colours[static_cast<uint16_t>(cell.vertexW[index]) & 0xFFu];
        v.shade[0] = static_cast<uint8_t>(word & 0xFF);
        v.shade[1] = static_cast<uint8_t>((word >> 8) & 0xFF);
        v.shade[2] = static_cast<uint8_t>((word >> 16) & 0xFF);
        v.shade[3] = 1;
    }
    if (look.windows && look.haveTables) {
        const unsigned tile = prim.flags >> 4;
        uint32_t word = 0;
        // Band 0's flags are rewritten at load (`SLUS 0x80033EA0`): tile t < 15 becomes window
        // t (+15 in the lower half of the VRAM page, i.e. the SAME tile of the image we bind), 15
        // becomes 30 = no window. Band 1 indexes its own table with the flags as they ship.
        if (band == 0 && tile < 15) word = look.tables.band0Window[tile];
        if (band == 1) word = look.tables.band1Window[tile & 15];
        const rr::TextureWindow w = rr::DecodeTextureWindow(word);
        if (w.width > 0 && w.width == w.height && w.width < 256) {
            v.window[0] = static_cast<uint8_t>(w.x);
            v.window[1] = static_cast<uint8_t>(w.y);
            v.window[2] = static_cast<uint8_t>(w.width);
        }
    }
}

rr::TriangleSoup BuildCellSoup(const std::vector<rr::CellData>& cells, int mode, bool withBand2,
                               bool stripQuads, std::vector<CellRange>* ranges, const CellLook* look) {
    rr::TriangleSoup soup;
    const CellLook plain{false, false, false};
    const CellLook& how = look ? *look : plain;
    std::vector<const rr::CellPrimitive*> drawn;
    for (size_t cellIndex = 0; cellIndex < cells.size(); ++cellIndex) {
        const rr::CellData& cell = cells[cellIndex];
        drawn.clear();
        if (mode == 3) {
            // Every group of bands 0 and 1: the renderer picks coarse or fine per group per frame.
            for (const rr::CellPrimitive& prim : cell.band0) drawn.push_back(&prim);
            if (cell.region7Present)
                for (const rr::CellPrimitive& prim : cell.band1) drawn.push_back(&prim);
        } else {
            CollectCellPrimitives(cell, mode, withBand2, drawn);
        }
        // Group this cell's primitives by the key they sample (and, in mode 3, by region-2 group),
        // so each run is one page bind. A run on a key of the cell's own never mixes bands, because
        // the two halves of the header pair are disjoint: a band-0 key carries bit 15 and a band-1
        // key does not. The one run that can mix them is the `0x7800` run, because both bands reach
        // the same fixed page through it - which is why `range.band` is a label on the report and
        // nothing binds to it.
        std::vector<std::pair<int, uint16_t>> keys;
        for (const rr::CellPrimitive* prim : drawn) {
            const std::pair<int, uint16_t> key{mode == 3 ? static_cast<int>(prim->group) : -1,
                                               CellPrimitiveKey(cell, *prim)};
            if (std::find(keys.begin(), keys.end(), key) == keys.end()) keys.push_back(key);
        }
        for (size_t slot = 0; slot < keys.size(); ++slot) {
            const size_t firstVertex = soup.vertices.size();
            int band = 0;
            const auto emitTriangle = [&](const rr::CellPrimitive& prim, int i0, int i1, int i2) {
                const int corner[3] = {i0, i1, i2};
                for (int k = 0; k < 3; ++k) {
                    const uint16_t index = prim.index[corner[k]];
                    rr::TriangleSoup::Vertex v;
                    v.x = rr::CellWorldX(cell, index);
                    v.y = rr::CellWorldY(cell, index);
                    v.z = rr::CellWorldZ(cell, index);
                    // Texel coordinates as the primitive stores them; v == 128 occurs on a handful
                    // of edges and is clamped by the sampler, the way the page's last row is.
                    v.u = static_cast<float>(prim.u[corner[k]]);
                    v.v = static_cast<float>(prim.v[corner[k]]);
                    v.tpage = prim.pal;
                    ShadeCellVertex(cell, prim, band, index, how, v);
                    // one-sided unless record +0 bit 2 (the emitters' NCLIP; drawn with uNclip on)
                    v.window[3] = (prim.flags & 4u) != 0 ? 0 : 1;
                    soup.vertices.push_back(v);
                }
            };
            // The ordering-table key rule of the primitive just emitted (rmd3.h TriangleSoup::Vertex::ot): the
            // largest view depth of its corners, i.e. of its 3 or 6 soup vertices (RASHCDG 0x8006D350 & co).
            const auto keyRule = [&](size_t from) {
                const size_t size = soup.vertices.size() - from;
                for (size_t k = 0; k < size; ++k) soup.vertices[from + k].ot = -static_cast<float>(2 + k + 8 * size);
            };
            for (const rr::CellPrimitive* prim : drawn) {
                if (mode == 3 && static_cast<int>(prim->group) != keys[slot].first) continue;
                if (CellPrimitiveKey(cell, *prim) != keys[slot].second) continue;
                band = CellBandOf(cell, *prim);
                const size_t primFirst = soup.vertices.size();
                if (!prim->quad) {
                    emitTriangle(*prim, 0, 1, 2);
                    keyRule(primFirst);
                    continue;
                }
                // A cell quad stores its four corners going ROUND the quad, not in the PS1 strip
                // order (545 770 quads over all 102 stream files, scene_cell.md 4.3), and the draw
                // routines hand them to the GPU as (i0, i1, i3, i2) - e.g. band 0 at `RASHCDG
                // 0x8006DB3C..0x8006DBA8` writes the vertices of record +0x10, +0x12, +0x16, +0x14 -
                // so the GT4 is split along the i1-i3 diagonal: (i0,i1,i3) + (i1,i3,i2).
                // `--quad-strip` puts the old bow-tie split back and `--legacy` the 0-2 diagonal
                // (controls).
                if (stripQuads) {
                    emitTriangle(*prim, 0, 1, 2);
                    emitTriangle(*prim, 1, 3, 2);
                } else if (how.quadDiagonal13) {
                    emitTriangle(*prim, 0, 1, 3);
                    emitTriangle(*prim, 1, 2, 3);
                } else {
                    emitTriangle(*prim, 0, 1, 2);
                    emitTriangle(*prim, 0, 2, 3);
                }
                keyRule(primFirst);
            }
            if (soup.vertices.size() == firstVertex) continue;
            if (!ranges) continue;
            CellRange range;
            range.first = static_cast<GLint>(firstVertex);
            range.count = static_cast<GLsizei>(soup.vertices.size() - firstVertex);
            range.texKey = keys[slot].second;
            range.band = band;
            range.windows = cell.header.windows;
            range.cell = cellIndex;
            range.group = keys[slot].first;
            for (int k = 0; k < 3; ++k)
                range.centre[k] = static_cast<float>(cell.origin[k]) / rr::kCellUnitsPerWorldUnit;
            ranges->push_back(range);
        }
    }
    return soup;
}

namespace {

// View-space coordinates of a cell vertex in CELL units (world * 64), the unit the original's
// thresholds are written in: the GTE transforms region-5 vertices with the camera rotation (rows
// 0 and 2 unit length) and a per-cell translation, and `0x80010528`/`0x800103BC` see
// `MAC3 = forward . (origin + v - eye * 64)`.
struct CellViewPoint {
    double x = 0, y = 0, z = 0;
};
CellViewPoint ToCellView(const CellView& view, float wx, float wy, float wz) {
    const double d[3] = {(static_cast<double>(wx) - view.eye[0]) * 64.0, (static_cast<double>(wy) - view.eye[1]) * 64.0,
                         (static_cast<double>(wz) - view.eye[2]) * 64.0};
    CellViewPoint p;
    p.x = d[0] * view.right[0] + d[1] * view.right[1] + d[2] * view.right[2];
    p.y = -(d[0] * view.up[0] + d[1] * view.up[1] + d[2] * view.up[2]);
    p.z = d[0] * view.forward[0] + d[1] * view.forward[1] + d[2] * view.forward[2];
    return p;
}

} // namespace

uint32_t CellLodWord(const rr::CellData& cell, const CellView& view, int set, int& fineCells, bool keepCulled,
                     std::vector<CellLodGroup>* groups, bool maxDetail) {
    // `SLUS 0x800356C0..0x800356D0`: 9600, or 4800 when game_state+0x30 (the road set) is 2.
    const int32_t fineDepth = set == 2 ? 4800 : 9600;
    uint32_t word = 0;
    bool wentFine = false;
    if (groups) groups->assign(cell.countB, CellLodGroup{});
    for (size_t k = 0; k < cell.countB; ++k) {
        const std::vector<int16_t> polygon = rr::CellSubAreaPolygon(cell, k);
        // `0x800351EC`: min depth from 0xFFFFFF, max from -641, the first four depths summed and
        // divided by four, and two outcodes per vertex: the AND over |16 z| < 21 |x| (and z < -40,
        // bit 2) says "all outside one side", the OR over |z| < |x| (and behind) "straddles".
        int64_t minZ = 0xFFFFFF, sum4 = 0;
        int andCode = 7, orCode = 0;
        for (size_t n = 0; n < polygon.size(); ++n) {
            const int16_t index = polygon[n];
            if (index < 0 || static_cast<size_t>(index) >= cell.vertexCount) continue;
            const CellViewPoint p = ToCellView(view, rr::CellWorldX(cell, static_cast<size_t>(index)),
                                               rr::CellWorldY(cell, static_cast<size_t>(index)),
                                               rr::CellWorldZ(cell, static_cast<size_t>(index)));
            const int64_t z = static_cast<int64_t>(std::floor(p.z));
            const int64_t x = static_cast<int64_t>(std::floor(p.x));
            if (z < minZ) minZ = z;
            if (n < 4) sum4 += z;
            const int64_t z16 = z * 16;
            const int behind = z16 < -640 ? 4 : 0;
            const int64_t az16 = z16 < 0 ? -z16 : z16;
            int code = behind;
            if (az16 < 21 * x) code |= 2;
            if (21 * x < -az16) code |= 1;
            andCode &= code;
            int loose = behind;
            if (az16 < 16 * x) loose |= 2;
            if (16 * x < -az16) loose |= 1;
            orCode |= loose;
        }
        const int32_t meanZ = static_cast<int32_t>(sum4 >> 2);
        const int visibility = andCode != 0 ? 0 : (orCode != 0 ? 1 : 2);
        if (groups) {
            (*groups)[k].visibility = visibility;
            (*groups)[k].minZ = static_cast<int32_t>(minZ);
            (*groups)[k].meanZ = meanZ;
        }
        uint32_t nibble = 0;
        if (visibility != 0 || keepCulled) {
            const bool nearArm = visibility == 1 || meanZ < 2048;
            if (cell.region7Present && (maxDetail || (minZ < fineDepth && fineCells < 4))) {
                wentFine = true;
                nibble = nearArm ? 7u : 3u;
            } else {
                nibble = nearArm ? 5u : 1u;
            }
            if (meanZ < fineDepth) nibble |= 12u;
            if (visibility == 0 && !maxDetail) nibble &= ~2u; // ours: a group the console culls is drawn coarse
        }
        word |= nibble << (4 * k);
    }
    if (wentFine) ++fineCells;
    return word;
}

void CollectBand2(const rr::CellData& cell, size_t cellIndex, size_t fineGroup, const CellView& view,
                  const CellLook& look, Band2Frame& out, const Band2Options* options) {
    if (!cell.region7Present || !look.haveTables) return;
    const size_t group = static_cast<size_t>(cell.countA) + 2u * cell.countB + fineGroup;
    const rr::CellDrawTables& t = look.tables;
    const auto colourOf = [&](int32_t shadeIndex, uint8_t rgb[3]) {
        const uint32_t word = look.haveColours ? look.colours[static_cast<uint32_t>(shadeIndex) & 0xFFu] : 0x808080u;
        rgb[0] = static_cast<uint8_t>(word & 0xFF);
        rgb[1] = static_cast<uint8_t>((word >> 8) & 0xFF);
        rgb[2] = static_cast<uint8_t>((word >> 16) & 0xFF);
    };
    for (const rr::CellPrimitive& prim : cell.band2) {
        if (prim.group != group) continue;
        // `0x8006E474` reads group+8, the QUAD count, and nothing draws a band-2 triangle.
        if (!prim.quad) {
            ++out.trianglesNotDrawn;
            continue;
        }
        double world[4][3];
        int64_t depth[4];
        int32_t shadeIndex[4];
        for (int c = 0; c < 4; ++c) {
            const uint16_t index = prim.index[c];
            world[c][0] = rr::CellWorldX(cell, index);
            world[c][1] = rr::CellWorldY(cell, index);
            world[c][2] = rr::CellWorldZ(cell, index);
            depth[c] = static_cast<int64_t>(std::floor(
                ToCellView(view, static_cast<float>(world[c][0]), static_cast<float>(world[c][1]),
                           static_cast<float>(world[c][2]))
                    .z));
            shadeIndex[c] = static_cast<uint16_t>(cell.vertexW[index]) & 0xFF;
        }
        // `0x8006E5BC..0x8006E640`: the largest of the four view depths.
        int64_t maxZ = std::max(std::max(depth[0], depth[1]), std::max(depth[2], depth[3]));
        if (options != nullptr && options->fixedDepth >= 0) maxZ = options->fixedDepth; // a view-free build
        const int group4 = static_cast<int>((prim.attr >> 2) & 0xC);
        const bool nearPath = look.band2Mutate == 3 || (options != nullptr && options->allNear) ? true : maxZ < 4096;
        const int32_t otKey = static_cast<int32_t>(std::clamp<int64_t>(maxZ, 0, 1 << 24)); // 0x8006E5BC's slot depth
        if (!nearPath) {
            // The far path: the record's own UVs, palette `0x800D5EC8[(attr >> 1 & 0x18) / 2]`, the
            // GT4 vertices in the order (i0, i1, i3, i2) (`0x8006E7C0..0x8006E918`).
            ++out.quadsFar;
            Band2Strip strip;
            strip.nearPath = false;
            strip.cell = cellIndex;
            strip.group = static_cast<int>(group);
            strip.paletteRow = group4 + (look.band2Mutate == 2 ? 1 : 0);
            strip.otKey = otKey;
            const int order[4] = {0, 1, 3, 2};
            for (int k = 0; k < 4; ++k) {
                const int c = order[k];
                for (int a = 0; a < 3; ++a) strip.p[k][a] = static_cast<float>(world[c][a]);
                strip.uv[k][0] = prim.u[c];
                strip.uv[k][1] = prim.v[c];
                strip.shade[k] = shadeIndex[c];
                colourOf(shadeIndex[c], strip.rgb[k]);
            }
            out.strips.push_back(strip);
            continue;
        }
        ++out.quadsNear;
        const unsigned templ = prim.attr & 0xFu;
        const uint32_t kinds = t.stripKinds[templ];
        const uint32_t lines = t.stripLines[templ];
        const int n = static_cast<int>(lines >> 28);
        if (n <= 0) continue;
        const int64_t absZ = maxZ < 0 ? -maxZ : maxZ;
        const size_t depthIndex = static_cast<size_t>(absZ / 512);
        // past the table (a near path taken for a far quad: maximum detail) the farthest entry's template
        const int nearTexture = options != nullptr && options->fullTexture ? 1
                                : depthIndex < t.nearByDepth.size() ? t.nearByDepth[depthIndex]
                                : (options != nullptr && options->allNear ? t.nearByDepth.back() : 0);
        // The strip edges: P_j on i0 -> i1 and Q_j on i3 -> i2, `0x8006EC44..0x8006EE98`. The
        // console steps by (end - start) * (4096 / n) >> 12 and so lands a hair short of the far
        // corner on the inner edges; the far corner itself is the vertex, exactly.
        const auto lerp3 = [](const double a[3], const double b[3], double f, double out3[3]) {
            for (int k = 0; k < 3; ++k) out3[k] = a[k] + (b[k] - a[k]) * f;
        };
        std::vector<std::array<double, 3>> P(static_cast<size_t>(n) + 1), Q(static_cast<size_t>(n) + 1);
        std::vector<int32_t> shadeP(static_cast<size_t>(n) + 1), shadeQ(static_cast<size_t>(n) + 1);
        const int32_t recip[8] = {0, 4096, 2048, 1365, 1024, 819, 682, 585}; // `RASHCDG 0x800CCA10`
        const int32_t stepP = ((shadeIndex[1] - shadeIndex[0]) * recip[n < 8 ? n : 7]) >> 12;
        const int32_t stepQ = ((shadeIndex[2] - shadeIndex[3]) * recip[n < 8 ? n : 7]) >> 12;
        for (int j = 0; j <= n; ++j) {
            const double f = static_cast<double>(j) / n;
            lerp3(world[0], world[1], f, P[static_cast<size_t>(j)].data());
            lerp3(world[3], world[2], f, Q[static_cast<size_t>(j)].data());
            shadeP[static_cast<size_t>(j)] = j == n ? shadeIndex[1] : shadeIndex[0] + j * stepP;
            shadeQ[static_cast<size_t>(j)] = j == n ? shadeIndex[2] : shadeIndex[3] + j * stepQ;
        }
        // Lane lines first (`0x8006EEC4..0x8006F424`): in front of the strips on the console
        // because they are chained before them in the packet list.
        for (int j = 0; j < n; ++j) {
            const size_t a = static_cast<size_t>(j), b = static_cast<size_t>(j) + 1;
            double dP[3], dQ[3];
            for (int k = 0; k < 3; ++k) {
                dP[k] = (world[1][k] - world[0][k]) / n;
                dQ[k] = (world[2][k] - world[3][k]) / n;
            }
            for (int side = 0; side < 2; ++side) {
                const unsigned colourIndex = (lines >> (4 * j + 2 * side)) & 3u;
                if (colourIndex == 0) continue;
                const double sign = side == 0 ? 1.0 : -1.0;
                const std::array<double, 3>& baseP = side == 0 ? P[a] : P[b];
                const std::array<double, 3>& baseQ = side == 0 ? Q[a] : Q[b];
                Band2Line line;
                for (int k = 0; k < 3; ++k) {
                    // `step >> 6` in, then `step >> 5` further: a line 1/32 of a strip wide.
                    const double p0 = baseP[k] + sign * dP[k] / 64.0, q0 = baseQ[k] + sign * dQ[k] / 64.0;
                    line.p[0][k] = static_cast<float>(p0);
                    line.p[1][k] = static_cast<float>(q0);
                    line.p[2][k] = static_cast<float>(p0 + sign * dP[k] / 32.0);
                    line.p[3][k] = static_cast<float>(q0 + sign * dQ[k] / 32.0);
                }
                line.colour = t.lineColour[colourIndex];
                line.otKey = otKey;
                line.cell = cellIndex;
                line.group = static_cast<int>(group);
                out.lines.push_back(line);
            }
        }
        // The strips (`0x8006F434..0x8006F548`): template 2 * kind + near, palette g + 1 + clutsel,
        // corners (P_j, Q_j, P_j+1, Q_j+1) with the template's UVs in that order.
        for (int j = 0; j < n; ++j) {
            const size_t a = static_cast<size_t>(j), b = static_cast<size_t>(j) + 1;
            unsigned kind = (kinds >> (5 * j)) & 3u;
            if (look.band2Mutate == 1) kind ^= 1u;
            const unsigned clutsel = (kinds >> (5 * j + 2)) & 3u;
            const std::array<uint16_t, 4>& uv = t.stripUv[2 * kind + static_cast<unsigned>(nearTexture)];
            Band2Strip strip;
            strip.nearPath = true;
            strip.cell = cellIndex;
            strip.group = static_cast<int>(group);
            strip.paletteRow = group4 + 1 + static_cast<int>(clutsel) + (look.band2Mutate == 2 ? 1 : 0);
            strip.otKey = otKey;
            const std::array<double, 3>* corner[4] = {&P[a], &Q[a], &P[b], &Q[b]};
            const int32_t shade[4] = {shadeP[a], shadeQ[a], shadeP[b], shadeQ[b]};
            for (int k = 0; k < 4; ++k) {
                for (int c = 0; c < 3; ++c) strip.p[k][c] = static_cast<float>((*corner[k])[static_cast<size_t>(c)]);
                strip.uv[k][0] = static_cast<uint8_t>(uv[static_cast<size_t>(k)] & 0xFF);
                strip.uv[k][1] = static_cast<uint8_t>(uv[static_cast<size_t>(k)] >> 8);
                strip.shade[k] = shade[k];
                colourOf(shade[k], strip.rgb[k]);
            }
            if (strip.paletteRow >= 12) ++out.paletteOutOfRange;
            out.strips.push_back(strip);
        }
    }
}

void AppendBand2Soup(const Band2Frame& frame, int grid, rr::TriangleSoup& road, rr::TriangleSoup& lines, float uvInset) {
    grid = std::max(1, grid);
    for (const Band2Strip& strip : frame.strips) {
        // the template's texel rectangle, for the smooth textures' inset (uvInset)
        float uvMin[2] = {255.0f, 255.0f}, uvMax[2] = {0.0f, 0.0f};
        for (int k = 0; k < 4; ++k)
            for (int a = 0; a < 2; ++a) {
                uvMin[a] = std::min(uvMin[a], static_cast<float>(strip.uv[k][a]));
                uvMax[a] = std::max(uvMax[a], static_cast<float>(strip.uv[k][a]));
            }
        // Bilinear in the quad's own parameters (s, t). A near strip's corners (P_j, Q_j, P_j+1,
        // Q_j+1) are (0,0), (0,1), (1,0), (1,1); the far quad's, in GPU order (i0, i1, i3, i2),
        // are (0,0), (1,0), (0,1), (1,1), and its 1 x 1 grid is exactly the GPU's two triangles.
        const int cells = strip.nearPath ? grid : 1;
        const auto at = [&](double s, double t, rr::TriangleSoup::Vertex& v) {
            const double w[4] = {(1 - s) * (1 - t), (1 - s) * t, s * (1 - t), s * t};
            // near strip corner order (P_j, Q_j, P_j+1, Q_j+1): s along P->P', t along P->Q
            // far quad order (i0, i1, i3, i2): corner 1 is (s=1,t=0) and corner 2 is (s=0,t=1)
            const int map[4] = {0, strip.nearPath ? 1 : 2, strip.nearPath ? 2 : 1, 3};
            double pos[3] = {0, 0, 0}, uv[2] = {0, 0}, rgb[3] = {0, 0, 0};
            for (int k = 0; k < 4; ++k) {
                const int c = map[k];
                for (int a = 0; a < 3; ++a) pos[a] += w[k] * strip.p[c][a];
                for (int a = 0; a < 2; ++a) uv[a] += w[k] * strip.uv[c][a];
                for (int a = 0; a < 3; ++a) rgb[a] += w[k] * strip.rgb[c][a];
            }
            v.x = static_cast<float>(pos[0]);
            v.y = static_cast<float>(pos[1]);
            v.z = static_cast<float>(pos[2]);
            v.u = static_cast<float>(uv[0]);
            v.v = static_cast<float>(uv[1]);
            if (uvInset > 0.0f) { // inside [min + inset, max - inset] of the template
                v.u = uvMax[0] - uvMin[0] > 2.0f * uvInset ? std::clamp(v.u, uvMin[0] + uvInset, uvMax[0] - uvInset) : v.u;
                v.v = uvMax[1] - uvMin[1] > 2.0f * uvInset ? std::clamp(v.v, uvMin[1] + uvInset, uvMax[1] - uvInset) : v.v;
            }
            v.tpage = static_cast<uint16_t>(std::clamp(strip.paletteRow, 0, 11));
            for (int a = 0; a < 3; ++a) v.shade[a] = static_cast<uint8_t>(std::clamp(rgb[a] + 0.5, 0.0, 255.0));
            v.shade[3] = 1;
            v.ot = static_cast<float>(strip.otKey);
        };
        for (int i = 0; i < cells; ++i)
            for (int j = 0; j < cells; ++j) {
                const double s0 = static_cast<double>(i) / cells, s1 = static_cast<double>(i + 1) / cells;
                const double t0 = static_cast<double>(j) / cells, t1 = static_cast<double>(j + 1) / cells;
                rr::TriangleSoup::Vertex v00, v10, v01, v11;
                at(s0, t0, v00);
                at(s1, t0, v10);
                at(s0, t1, v01);
                at(s1, t1, v11);
                // Split along the (1,0)-(0,1) diagonal, as the GPU splits (v0,v1,v2,v3).
                for (const rr::TriangleSoup::Vertex* v : {&v00, &v10, &v01, &v10, &v11, &v01}) road.vertices.push_back(*v);
            }
    }
    for (const Band2Line& line : frame.lines) {
        rr::TriangleSoup::Vertex v[4];
        for (int k = 0; k < 4; ++k) {
            v[k].x = line.p[k][0];
            v[k].y = line.p[k][1];
            v[k].z = line.p[k][2];
            v[k].shade[0] = static_cast<uint8_t>(line.colour & 0xFF);
            v[k].shade[1] = static_cast<uint8_t>((line.colour >> 8) & 0xFF);
            v[k].shade[2] = static_cast<uint8_t>((line.colour >> 16) & 0xFF);
            v[k].shade[3] = 2;
            v[k].ot = static_cast<float>(line.otKey);
        }
        for (int k : {0, 1, 2, 1, 3, 2}) lines.vertices.push_back(v[k]);
    }
}

// ============================================================================ near subdivision (scene_cell.md 13.8)
namespace {

namespace SD = rr::sim::subdiv;

constexpr double kRecordsPerWorldUnit = 64.0 * 256.0; // cell units << 8
constexpr double kGteRowAspect = 3412.0 / 4096.0;     // race_scene.h kGteAspectRow: the view matrix's row 1

// The view the cell draw sees (MVMVA LLM x V + BK, sf 1, then << 8) and the GTE's RTPS with RT = 0.
struct NearCamera {
    CellView v;
    double squeeze = 1.0;
    rr::sim::model::ModelGte gte;
    NearCamera(const CellView& view, float sideSqueeze) : v(view), squeeze(std::max(1.0f, sideSqueeze)) {
        gte.h = 237;
        gte.ofx = 192 << 16;
        gte.ofy = 120 << 16;
        // The screen's own frame, as the GL view (mat4.cpp LookAt) builds it from the same eye, forward and up: x =
        // forward x up (CellView's `right` is up x forward, i.e. screen LEFT - CellLodWord only uses |x|), up = x x
        // forward, square to forward. The pieces go back to world through its transpose.
        const double f[3] = {v.forward[0], v.forward[1], v.forward[2]};
        double s[3] = {f[1] * v.up[2] - f[2] * v.up[1], f[2] * v.up[0] - f[0] * v.up[2], f[0] * v.up[1] - f[1] * v.up[0]};
        const double sl = std::sqrt(s[0] * s[0] + s[1] * s[1] + s[2] * s[2]);
        if (sl > 0.0) {
            for (double& c : s) c /= sl;
            const double u[3] = {s[1] * f[2] - s[2] * f[1], s[2] * f[0] - s[0] * f[2], s[0] * f[1] - s[1] * f[0]};
            for (int k = 0; k < 3; ++k) {
                v.right[k] = static_cast<float>(s[k]);
                v.up[k] = static_cast<float>(u[k]);
            }
        }
    }
    static int32_t Round(double a) { return static_cast<int32_t>(std::clamp(std::floor(a + 0.5), -1.0e9, 1.0e9)); }
    void ToView(double wx, double wy, double wz, SD::Rec& r) const {
        const double d[3] = {wx - v.eye[0], wy - v.eye[1], wz - v.eye[2]};
        r.x = Round((d[0] * v.right[0] + d[1] * v.right[1] + d[2] * v.right[2]) * kRecordsPerWorldUnit);
        r.y = Round(-(d[0] * v.up[0] + d[1] * v.up[1] + d[2] * v.up[2]) * kGteRowAspect * kRecordsPerWorldUnit);
        r.z = Round((d[0] * v.forward[0] + d[1] * v.forward[1] + d[2] * v.forward[2]) * kRecordsPerWorldUnit);
    }
    void ToWorld(const SD::Rec& r, float out[3]) const {
        const double x = r.x / kRecordsPerWorldUnit, u = -(r.y / kGteRowAspect) / kRecordsPerWorldUnit,
                     z = r.z / kRecordsPerWorldUnit;
        for (int k = 0; k < 3; ++k)
            out[k] = static_cast<float>(v.eye[k] + x * v.right[k] + u * v.up[k] + z * v.forward[k]);
    }
    uint32_t Project(int32_t tx, int32_t ty, int32_t tz) {
        gte.tr[0] = tx;
        gte.tr[1] = ty;
        gte.tr[2] = tz;
        const int16_t zero[3] = {0, 0, 0};
        int32_t mac[3];
        uint32_t sxy = gte.Rtps(zero, mac);
        if (squeeze > 1.0) {
            const int32_t sx = static_cast<int16_t>(sxy & 0xFFFFu);
            const int32_t pulled = 192 + static_cast<int32_t>(std::lround((sx - 192) / squeeze));
            sxy = (sxy & 0xFFFF0000u) | (static_cast<uint32_t>(pulled) & 0xFFFFu);
        }
        return sxy;
    }
    // A record for a world point: its view point and SXY.
    void Corner(double wx, double wy, double wz, SD::Rec& r) {
        ToView(wx, wy, wz, r);
        r.sxy = Project(r.x >> 5, r.y >> 5, r.z >> 5);
    }
};

// SLUS 0x8001034C's per-vertex byte (the cell vertex pass): 0x20 nearer than 1024 cell units, 0x10 than 40, 8 below
// line 240, 2 left of column 0, 1 right of 384.
uint8_t VertexByte(uint32_t sxy, int32_t zCell) {
    const int32_t sy = static_cast<int32_t>(sxy) >> 16, sx = static_cast<int16_t>(sxy & 0xFFFFu);
    uint32_t f = (zCell < 1024 ? 0x20u : 0u) | (zCell < 40 ? 0x10u : 0u) | (sy > 240 ? 8u : 0u);
    f += (static_cast<uint32_t>(sx) > 384u ? 1u : 0u) + (static_cast<uint32_t>(sx) >> 31);
    return static_cast<uint8_t>(f);
}
// The depth bit 0x8006A630 / 0x8006E474 OR into a corner's byte: 0x80 below 0xC800, 0x40 below 0x32000, else 0x20.
uint8_t CornerDepthBit(int32_t z) { return static_cast<uint8_t>(z < 0x32000 ? (z < 0xC800 ? 0x80u : 0x40u) : 0x20u); }

// Turns the pieces into triangles of `out` (a GT4 (A, B, C, D) is the GPU's (A, B, C) + (B, C, D)).
struct SoupEnv final : SD::Env {
    NearCamera& cam;
    const CellLook& look;
    rr::TriangleSoup& out;
    rr::TriangleSoup::Vertex proto; // page row, window, mode
    size_t pieces = 0;
    SoupEnv(NearCamera& c, const CellLook& l, rr::TriangleSoup& o) : cam(c), look(l), out(o) {}
    uint32_t Project(int32_t tx, int32_t ty, int32_t tz) override { return cam.Project(tx, ty, tz); }
    uint32_t Colour(uint32_t index) override { return look.haveColours ? look.colours[index & 0xFFu] : 0x808080u; }
    bool Stored(int, const SD::Rec&, bool) override { return true; }
    void Put(const SD::Rec& r) {
        rr::TriangleSoup::Vertex v = proto;
        float p[3];
        cam.ToWorld(r, p);
        v.x = p[0];
        v.y = p[1];
        v.z = p[2];
        v.u = static_cast<float>(r.uv & 0xFFu);
        v.v = static_cast<float>((r.uv >> 8) & 0xFFu);
        v.shade[0] = static_cast<uint8_t>(r.rgb & 0xFFu);
        v.shade[1] = static_cast<uint8_t>((r.rgb >> 8) & 0xFFu);
        v.shade[2] = static_cast<uint8_t>((r.rgb >> 16) & 0xFFu);
        out.vertices.push_back(v);
    }
    bool Gt4(const SD::Rec* r, const int v[4]) override {
        for (int k : {0, 1, 2, 1, 3, 2}) Put(r[v[k]]);
        ++pieces;
        return true;
    }
    bool Gt3(const SD::Rec* r, const int v[3]) override {
        for (int k = 0; k < 3; ++k) Put(r[v[k]]);
        ++pieces;
        return true;
    }
    bool F4(const SD::Rec*, const int[4], uint32_t) override { return true; }
};

SD::Kids KidsOf(const CellLook& look) {
    SD::Kids kids;
    for (size_t k = 0; k < 4; ++k) kids.tri[k] = look.tables.subTri[k];
    for (size_t k = 0; k < 4; ++k) kids.quad[k] = look.tables.subQuad[k];
    for (size_t k = 0; k < 2; ++k) kids.line[k] = look.tables.subLine[k];
    return kids;
}

} // namespace

void AppendBand2SoupSubdivided(const Band2Frame& frame, const CellView& view, const CellLook& look, float sideSqueeze,
                               rr::TriangleSoup& road, rr::TriangleSoup& lines, SubdivStats& stats) {
    Band2Frame whole; // the far quads (one GT4 each) and the lane lines go the old way
    whole.lines = frame.lines;
    NearCamera cam(view, sideSqueeze);
    SoupEnv env(cam, look, road);
    const SD::Kids kids = KidsOf(look);
    for (const Band2Strip& strip : frame.strips) {
        if (!strip.nearPath) {
            whole.strips.push_back(strip);
            continue;
        }
        // 0x8006F448..0x8006F534: the strip's records P_j, Q_j, P_j+1, Q_j+1 into 0..3 with the template's texels,
        // then 0x80069CF0(4, 0x01030200, 5) - the corners taken round as 0, 2, 3, 1. The records' outcode: the
        // points stepped between the quad's corners carry the subdivider's own byte (0x8006EC44..0x8006EE98), the
        // quad's own corners (j = 0 / n) the vertex pass's byte with the 0x32000 depth bit; ours: all the former.
        SD::Subdivider sd(env, kids);
        sd.nudge = false; // ours: see subdiv.h
        for (int k = 0; k < 4; ++k) {
            SD::Rec& r = sd.rec[k];
            cam.Corner(strip.p[k][0], strip.p[k][1], strip.p[k][2], r);
            r.uv = static_cast<uint32_t>(strip.uv[k][0]) | static_cast<uint32_t>(strip.uv[k][1]) << 8;
            r.shade = strip.shade[k];
            r.rgb = (env.Colour(static_cast<uint32_t>(strip.shade[k]) & 0xFFu) & 0xFFFFFFu) |
                    static_cast<uint32_t>(SD::Outcode(r.sxy, r.z)) << 24;
        }
        env.proto = rr::TriangleSoup::Vertex{};
        env.proto.tpage = static_cast<uint16_t>(std::clamp(strip.paletteRow, 0, 11));
        env.proto.shade[3] = 1;
        env.proto.ot = static_cast<float>(strip.otKey); // every piece sits in the quad's slot (0x8006E474)
        const size_t before = env.pieces;
        if (!sd.Road(4, 0x01030200u, 5)) ++stats.refused;
        ++stats.roadStrips;
        stats.roadPieces += env.pieces - before;
    }
    AppendBand2Soup(whole, 1, road, lines);
}

void AppendFineNearGroup(const rr::CellData& cell, int group, uint16_t texKey, const CellView& view,
                         const CellLook& look, float sideSqueeze, rr::TriangleSoup& out, SubdivStats& stats) {
    NearCamera cam(view, sideSqueeze);
    SoupEnv env(cam, look, out);
    const SD::Kids kids = KidsOf(look);
    for (const rr::CellPrimitive& prim : cell.band1) {
        if (static_cast<int>(prim.group) != group || CellPrimitiveKey(cell, prim) != texKey) continue;
        ++stats.nearPrims;
        const int n = prim.quad ? 4 : 3;
        SD::Subdivider sd(env, kids);
        sd.nudge = false; // ours: see subdiv.h
        uint8_t andBits = 0x1F, orBits = 0;
        for (int k = 0; k < n; ++k) {
            const uint16_t index = prim.index[k];
            SD::Rec& r = sd.rec[k];
            cam.Corner(rr::CellWorldX(cell, index), rr::CellWorldY(cell, index), rr::CellWorldZ(cell, index), r);
            const uint8_t byte = VertexByte(r.sxy, r.z >> 8);
            andBits &= byte;
            orBits |= byte;
            r.uv = static_cast<uint32_t>(prim.u[k]) | static_cast<uint32_t>(prim.v[k]) << 8;
            r.shade = static_cast<uint16_t>(cell.vertexW[index]) & 0xFF;
            r.rgb = (env.Colour(static_cast<uint32_t>(r.shade)) & 0xFFFFFFu) |
                    static_cast<uint32_t>(byte | CornerDepthBit(r.z)) << 24;
        }
        // 0x8006A630: every corner outside one side of the screen - not drawn.
        if ((andBits & 0x1F) != 0) {
            ++stats.culledPrims;
            continue;
        }
        // 0x8006A630's back-face test: a primitive without record +0 bit 2 is dropped when GTE NCLIP of
        // its corners (i0, i1, i2) is negative - a triangle always (0x8006A6D4), a quad only while no corner is nearer
        // than 40 (byte bit 0x10) and only when (i0, i3, i2) is then positive too (0x8006AFD4..0x8006B02C).
        if (look.nearNclip && (prim.flags & 4u) == 0) {
            const auto nclip = [&](int a, int b, int c) {
                const int64_t ax = static_cast<int16_t>(sd.rec[a].sxy & 0xFFFFu), ay = static_cast<int16_t>(sd.rec[a].sxy >> 16);
                const int64_t bx = static_cast<int16_t>(sd.rec[b].sxy & 0xFFFFu), by = static_cast<int16_t>(sd.rec[b].sxy >> 16);
                const int64_t cx = static_cast<int16_t>(sd.rec[c].sxy & 0xFFFFu), cy = static_cast<int16_t>(sd.rec[c].sxy >> 16);
                return ax * by + bx * cy + cx * ay - ax * cy - bx * ay - cx * by;
            };
            const bool back = !prim.quad ? nclip(0, 1, 2) < 0
                                         : ((orBits & 0x10) == 0 && nclip(0, 1, 2) < 0 && nclip(0, 3, 2) > 0);
            if (back) {
                ++stats.backPrims;
                continue;
            }
        }
        // The vertex attributes BuildCellSoup gives a band-1 vertex (texture window, page row, modulated).
        rr::TriangleSoup::Vertex proto;
        proto.tpage = prim.pal;
        ShadeCellVertex(cell, prim, 1, prim.index[0], look, proto);
        proto.shade[3] = 1;
        {   // 0x8006A630's slot depth: the largest corner view depth; the pieces are chained into that one slot
            int32_t maxZ = sd.rec[0].z >> 8;
            for (int k = 1; k < n; ++k) maxZ = std::max(maxZ, sd.rec[k].z >> 8);
            proto.ot = static_cast<float>(std::max(maxZ, 0));
        }
        env.proto = proto;
        if ((orBits & 0x20) == 0) {
            // a whole GT3 / GT4 (0x8006A630's own packet): (i0, i1, i2) / (i0, i1, i3, i2)
            const int tri[3] = {0, 1, 2};
            const int quad[4] = {0, 1, 3, 2};
            if (prim.quad) env.Gt4(sd.rec, quad);
            else env.Gt3(sd.rec, tri);
            continue;
        }
        ++stats.splitPrims;
        const size_t before = env.pieces;
        const int32_t level = 5 - static_cast<int32_t>(prim.attr & 0xFu);
        const bool ok = prim.quad ? sd.Quad(4, 0x03020100u, level) : sd.Tri(3, 0x00020100u, level);
        if (!ok) ++stats.refused;
        stats.pieces += env.pieces - before;
        if (std::getenv("RRJB_NEAR_TRACE")) { // DEVELOPMENT
            std::printf("near: group %d attr %02X level %d pieces %zu uv", group, prim.attr, level, env.pieces - before);
            for (int k = 0; k < n; ++k)
                std::printf(" (%d,%d z%d uv %u,%u)", static_cast<int16_t>(sd.rec[k].sxy & 0xFFFF),
                            static_cast<int16_t>(sd.rec[k].sxy >> 16), sd.rec[k].z >> 8, prim.u[k], prim.v[k]);
            std::printf("\n");
        }
    }
}

size_t JoinCellRegion7(const rr::DiscImage& disc, int set, const std::vector<rr::RouteLeg>& legs,
                       std::vector<rr::CellData>& cells, size_t* failed) {
    size_t joined = 0, bad = 0;
    ScanRaceStream(
        disc, set, legs, [](uint8_t type) { return type == 9; },
        [&](uint32_t, const std::vector<uint8_t>& chunk) {
            const rr::ChunkHeader header = rr::ParseChunkHeader(chunk);
            for (rr::CellData& cell : cells) {
                if (cell.region7Present || cell.header.id != header.id) continue;
                try {
                    rr::AttachCellRegion7(cell, chunk);
                    ++joined;
                } catch (const std::exception&) {
                    ++bad;
                }
            }
        });
    if (failed) *failed = bad;
    return joined;
}

std::vector<uint16_t> CellTextureKeys(const std::vector<rr::CellData>& cells, bool withBand1) {
    std::vector<uint16_t> keys;
    const auto add = [&](uint16_t key) {
        if (std::find(keys.begin(), keys.end(), key) == keys.end()) keys.push_back(key);
    };
    for (const rr::CellData& cell : cells) {
        for (uint16_t key : cell.texKeyBand0) add(key);
        if (withBand1)
            for (uint16_t key : cell.texKeyBand1) add(key);
    }
    return keys;
}

size_t ScanRaceStream(const rr::DiscImage& disc, int set, const std::vector<rr::RouteLeg>& legs,
                      const std::function<bool(uint8_t)>& wants,
                      const std::function<void(uint32_t, const std::vector<uint8_t>&)>& visit) {
    const auto stream = disc.Find("DATA/STREAM" + std::to_string(set) + ".STR");
    const auto tocFile = disc.Find("DATA/STREAM" + std::to_string(set) + ".TOC");
    if (!stream || !tocFile) return 0;
    const rr::StreamToc toc = rr::ParseStreamToc(disc.ReadFile(*tocFile));

    // The same ranges the world loader walked, de-duplicated: two legs of one race often share a
    // road, and a chunk read twice would look like two copies of a resource that ships once.
    std::vector<std::pair<uint32_t, uint32_t>> ranges;
    for (const rr::RouteLeg& leg : legs)
        for (int direction : {+1, -1}) {
            const rr::StreamRange range = rr::StreamRangeFor(toc, leg.road, direction);
            if (range.size == 0) continue;
            const std::pair<uint32_t, uint32_t> entry{range.offset, range.size};
            if (std::find(ranges.begin(), ranges.end(), entry) == ranges.end()) ranges.push_back(entry);
        }
    // In file order, because the two halves of a type-1 texture page are told apart by nothing but
    // the order they appear in (scene_cell.md 12.4 matched them against VRAM that way).
    std::sort(ranges.begin(), ranges.end());

    std::vector<uint8_t> chunk(rr::kChunkSize);
    uint8_t keyBytes[4] = {};
    size_t scanned = 0;
    for (const auto& range : ranges)
        for (uint32_t offset = 0; offset < range.second; offset += rr::kChunkSize) {
            ++scanned;
            disc.ReadForm1(stream->lba, range.first + offset, keyBytes, sizeof(keyBytes));
            const uint32_t key = static_cast<uint32_t>(keyBytes[0]) | (keyBytes[1] << 8) |
                                 (keyBytes[2] << 16) | (static_cast<uint32_t>(keyBytes[3]) << 24);
            if (!wants(static_cast<uint8_t>(key >> 28))) continue;
            disc.ReadForm1(stream->lba, range.first + offset, chunk.data(), chunk.size());
            visit(range.first + offset, chunk);
        }
    return scanned;
}

rr::TriangleSoup BuildRoadSurface(const rr::DiscImage& disc, const rr::DiscFile& stp, size_t& roadChunks,
                                  size_t& sliceCount, std::vector<rr::RoadSlice>* outPath) {
    rr::TriangleSoup soup;
    const size_t chunkCount = (stp.size - 0x800) / rr::kChunkSize;
    std::vector<uint8_t> chunk(rr::kChunkSize);
    for (size_t i = 0; i < chunkCount; ++i) {
        disc.ReadForm1(stp.lba, 0x800 + i * rr::kChunkSize, chunk.data(), chunk.size());
        if (rr::ParseChunkHeader(chunk).type != static_cast<uint8_t>(rr::ChunkType::Road)) continue;
        const rr::RoadObject object = rr::ParseRoadChunk(chunk);
        ++roadChunks;
        sliceCount += object.slices.size();
        if (outPath) {
            // Keep the longest continuous run of slices seen - one drivable stretch of road.
            for (const rr::SliceRun& run : object.runs)
                if (run.count > outPath->size())
                    outPath->assign(object.slices.begin() + static_cast<ptrdiff_t>(run.first),
                                    object.slices.begin() + static_cast<ptrdiff_t>(run.first + run.count));
        }

        const float laneWidth = object.laneWidth > 0.0f ? object.laneWidth : 9.766f;
        const float lanes = object.laneCount > 0 ? static_cast<float>(object.laneCount) : 2.0f;
        const float halfWidth = laneWidth * lanes;

        for (const rr::SliceRun& run : object.runs) {
            for (size_t k = run.first; k + 1 < run.first + run.count; ++k) {
                const rr::RoadSlice* pair[2] = {&object.slices[k], &object.slices[k + 1]};
                rr::TriangleSoup::Vertex corner[4];
                for (int side = 0; side < 2; ++side)
                    for (int end = 0; end < 2; ++end) {
                        const rr::RoadSlice& s = *pair[end];
                        const float lateral[3] = {s.m[0] / 4096.0f, s.m[1] / 4096.0f, s.m[2] / 4096.0f};
                        const float sign = side == 0 ? -1.0f : 1.0f;
                        rr::TriangleSoup::Vertex v;
                        v.x = rr::WorldX(s) + lateral[0] * halfWidth * sign;
                        v.y = rr::WorldY(s) + lateral[1] * halfWidth * sign;
                        v.z = rr::WorldZ(s) + lateral[2] * halfWidth * sign;
                        v.nx = s.m[3] / 4096.0f; // row 1 is the surface normal
                        v.ny = s.m[4] / 4096.0f;
                        v.nz = s.m[5] / 4096.0f;
                        corner[static_cast<size_t>(side * 2 + end)] = v;
                    }
                // corner[0]=left/near corner[1]=left/far corner[2]=right/near corner[3]=right/far
                const int order[6] = {0, 1, 2, 1, 3, 2};
                for (int t = 0; t < 6; ++t) soup.vertices.push_back(corner[order[t]]);
            }
        }
    }
    return soup;
}

rr::TriangleSoup BuildRibbon(const std::vector<rr::RoadSlice>& path, float halfWidth) {
    rr::TriangleSoup soup;
    for (size_t k = 0; k + 1 < path.size(); ++k) {
        const rr::RoadSlice* pair[2] = {&path[k], &path[k + 1]};
        // A path assembled across roads has jumps where one leg ends and the next begins; skip a
        // segment that is implausibly long rather than drawing a ribbon across the map.
        const float dx = rr::WorldX(*pair[1]) - rr::WorldX(*pair[0]);
        const float dy = rr::WorldY(*pair[1]) - rr::WorldY(*pair[0]);
        const float dz = rr::WorldZ(*pair[1]) - rr::WorldZ(*pair[0]);
        if (dx * dx + dy * dy + dz * dz > 200.0f * 200.0f) continue;

        rr::TriangleSoup::Vertex corner[4];
        for (int side = 0; side < 2; ++side)
            for (int end = 0; end < 2; ++end) {
                const rr::RoadSlice& s = *pair[end];
                const float lateral[3] = {s.m[0] / 4096.0f, s.m[1] / 4096.0f, s.m[2] / 4096.0f};
                const float sign = side == 0 ? -1.0f : 1.0f;
                rr::TriangleSoup::Vertex v;
                v.x = rr::WorldX(s) + lateral[0] * halfWidth * sign;
                v.y = rr::WorldY(s) + lateral[1] * halfWidth * sign;
                v.z = rr::WorldZ(s) + lateral[2] * halfWidth * sign;
                v.nx = s.m[3] / 4096.0f;
                v.ny = s.m[4] / 4096.0f;
                v.nz = s.m[5] / 4096.0f;
                corner[static_cast<size_t>(side * 2 + end)] = v;
            }
        const int order[6] = {0, 1, 2, 1, 3, 2};
        for (int t = 0; t < 6; ++t) soup.vertices.push_back(corner[order[t]]);
    }
    return soup;
}

rr::TriangleSoup BuildSceneCells(const rr::DiscImage& disc, const rr::DiscFile& stp, size_t& cellCount) {
    rr::TriangleSoup soup;
    const size_t chunkCount = (stp.size - 0x800) / rr::kChunkSize;
    std::vector<uint8_t> chunk(rr::kChunkSize);
    for (size_t i = 0; i < chunkCount; ++i) {
        disc.ReadForm1(stp.lba, 0x800 + i * rr::kChunkSize, chunk.data(), chunk.size());
        const uint8_t type = rr::ParseChunkHeader(chunk).type;
        if (type != 0 && type != 8) continue;
        const rr::CellData cell = rr::ParseCellChunk(chunk);
        ++cellCount;

        const auto emitTriangle = [&](uint16_t a, uint16_t b, uint16_t c) {
            const float p[3][3] = {{rr::CellWorldX(cell, a), rr::CellWorldY(cell, a), rr::CellWorldZ(cell, a)},
                                   {rr::CellWorldX(cell, b), rr::CellWorldY(cell, b), rr::CellWorldZ(cell, b)},
                                   {rr::CellWorldX(cell, c), rr::CellWorldY(cell, c), rr::CellWorldZ(cell, c)}};
            const float e0[3] = {p[1][0] - p[0][0], p[1][1] - p[0][1], p[1][2] - p[0][2]};
            const float e1[3] = {p[2][0] - p[0][0], p[2][1] - p[0][1], p[2][2] - p[0][2]};
            float n[3] = {e0[1] * e1[2] - e0[2] * e1[1], e0[2] * e1[0] - e0[0] * e1[2],
                          e0[0] * e1[1] - e0[1] * e1[0]};
            const float length = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
            if (length > 0.0f)
                for (float& component : n) component /= length;
            for (int k = 0; k < 3; ++k) {
                rr::TriangleSoup::Vertex v;
                v.x = p[k][0];
                v.y = p[k][1];
                v.z = p[k][2];
                v.nx = n[0];
                v.ny = n[1];
                v.nz = n[2];
                soup.vertices.push_back(v);
            }
        };
        // Band 0 only. The three bands are levels of detail of the same geometry, so drawing more
        // than one band draws the same world twice.
        for (const rr::CellPrimitive& prim : cell.band0) {
            // The corners go round the quad - see the note in BuildCellSoup.
            emitTriangle(prim.index[0], prim.index[1], prim.index[2]);
            if (prim.quad) emitTriangle(prim.index[0], prim.index[2], prim.index[3]);
        }
    }
    return soup;
}

RoadFrame SampleRoad(const std::vector<rr::RoadSlice>& path, float distance, size_t* indexOut) {
    RoadFrame frame;
    if (path.empty()) return frame;
    // `distance` is the running distance the slices themselves carry, in 16.16 world units.
    const float first = static_cast<float>(path.front().distance) / 65536.0f;
    const float last = static_cast<float>(path.back().distance) / 65536.0f;
    const float span = last - first;
    float wanted = span > 0.0f ? std::fmod(distance - first, span) : 0.0f;
    if (wanted < 0.0f) wanted += span;
    wanted += first;

    size_t i = 0;
    while (i + 2 < path.size() && static_cast<float>(path[i + 1].distance) / 65536.0f < wanted) ++i;
    if (indexOut) *indexOut = i;
    const rr::RoadSlice& a = path[i];
    const rr::RoadSlice& b = path[i + 1 < path.size() ? i + 1 : i];
    const float da = static_cast<float>(a.distance) / 65536.0f;
    const float db = static_cast<float>(b.distance) / 65536.0f;
    const float t = db > da ? std::clamp((wanted - da) / (db - da), 0.0f, 1.0f) : 0.0f;

    const float pa[3] = {rr::WorldX(a), rr::WorldY(a), rr::WorldZ(a)};
    const float pb[3] = {rr::WorldX(b), rr::WorldY(b), rr::WorldZ(b)};
    for (int k = 0; k < 3; ++k) {
        frame.pos[k] = pa[k] + (pb[k] - pa[k]) * t;
        frame.lateral[k] = a.m[0 + k] / 4096.0f;
        frame.normal[k] = a.m[3 + k] / 4096.0f;
        frame.tangent[k] = a.m[6 + k] / 4096.0f;
    }
    return frame;
}

} // namespace rr::render
