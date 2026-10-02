// Coplanar layers, the shared pair finder and the models' layers (coplanar.h).
#include "render/coplanar.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <tuple>
#include <unordered_map>

namespace rr::render {

namespace {

constexpr GLenum kElementArrayBuffer = 0x8893; // GL_ELEMENT_ARRAY_BUFFER

using P3 = std::array<double, 3>;

P3 Sub(const P3& a, const P3& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
P3 Cross(const P3& a, const P3& b) { return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]}; }
double Dot(const P3& a, const P3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

// The part of triangle `t` over triangle `a` - inside the prism of a's edges, square to a's plane (Sutherland-Hodgman,
// relative to a's first corner: the world's coordinates run to thousands). `area` its area dropped on a's plane, `reach`
// the largest distance of it from that plane.
void OverPart(const rr::TriangleSoup::Vertex* a, const rr::TriangleSoup::Vertex* t, double& area, double& reach) {
    const P3 o = {a[0].x, a[0].y, a[0].z};
    P3 c[3];
    for (int k = 0; k < 3; ++k) c[k] = Sub({a[k].x, a[k].y, a[k].z}, o);
    P3 n = Cross(Sub(c[1], c[0]), Sub(c[2], c[0]));
    const double nl = std::sqrt(Dot(n, n));
    area = reach = 0.0;
    if (nl <= 0.0) return;
    for (double& x : n) x /= nl;
    // a triangle clipped by three planes: at most six corners
    std::array<P3, 8> poly = {}, next = {};
    size_t count = 3, nextCount = 0;
    for (int k = 0; k < 3; ++k) poly[static_cast<size_t>(k)] = Sub({t[k].x, t[k].y, t[k].z}, o);
    for (int e = 0; e < 3 && count > 0; ++e) {
        // the side plane through edge e, its normal pointing into the triangle
        const P3 edge = Sub(c[(e + 1) % 3], c[e]);
        P3 m = Cross(n, edge);
        if (Dot(m, Sub(c[(e + 2) % 3], c[e])) < 0.0) m = {-m[0], -m[1], -m[2]};
        nextCount = 0;
        for (size_t i = 0; i < count; ++i) {
            const P3& p = poly[i];
            const P3& q = poly[(i + 1) % count];
            const double dp = Dot(m, Sub(p, c[e])), dq = Dot(m, Sub(q, c[e]));
            if (dp >= 0.0 && nextCount < next.size()) next[nextCount++] = p;
            if ((dp >= 0.0) != (dq >= 0.0) && nextCount < next.size()) {
                const double f = dp / (dp - dq);
                next[nextCount++] = {p[0] + (q[0] - p[0]) * f, p[1] + (q[1] - p[1]) * f, p[2] + (q[2] - p[2]) * f};
            }
        }
        poly = next;
        count = nextCount;
    }
    if (count < 3) return;
    P3 sum = {0.0, 0.0, 0.0};
    for (size_t i = 0; i < count; ++i) {
        const P3 s = Cross(poly[i], poly[(i + 1) % count]);
        for (int k = 0; k < 3; ++k) sum[k] += s[k];
        reach = std::max(reach, std::fabs(Dot(n, poly[i])));
    }
    area = 0.5 * std::fabs(Dot(sum, n));
}

} // namespace

float CoplanarTriArea2(const rr::TriangleSoup::Vertex* v, float out[3]) {
    const float e1[3] = {v[1].x - v[0].x, v[1].y - v[0].y, v[1].z - v[0].z};
    const float e2[3] = {v[2].x - v[0].x, v[2].y - v[0].y, v[2].z - v[0].z};
    out[0] = e1[1] * e2[2] - e1[2] * e2[1];
    out[1] = e1[2] * e2[0] - e1[0] * e2[2];
    out[2] = e1[0] * e2[1] - e1[1] * e2[0];
    return std::sqrt(out[0] * out[0] + out[1] * out[1] + out[2] * out[2]);
}

void CoplanarMeasure(const std::vector<rr::TriangleSoup::Vertex>& soup, CoplanarPrim& p) {
    p.area = 0.0f;
    for (int a = 0; a < 3; ++a) p.box[a] = 1e30f, p.box[3 + a] = -1e30f;
    for (int t = 0; t + 3 <= p.size; t += 3) {
        float c[3];
        p.area += 0.5f * CoplanarTriArea2(&soup[static_cast<size_t>(p.first + t)], c);
    }
    for (int k = 0; k < p.size; ++k) {
        const rr::TriangleSoup::Vertex& v = soup[static_cast<size_t>(p.first + k)];
        const float q[3] = {v.x, v.y, v.z};
        for (int a = 0; a < 3; ++a) p.box[a] = std::min(p.box[a], q[a]), p.box[3 + a] = std::max(p.box[3 + a], q[a]);
    }
}

std::vector<CoplanarPair> FindCoplanarPairs(const std::vector<rr::TriangleSoup::Vertex>& soup, const std::vector<CoplanarPrim>& prims,
                                            const CoplanarRule& rule) {
    // candidates: primitives that share a cell of a grid over their bounds (one larger than rule.largest in any
    // direction takes none)
    const float grid = rule.grid, plane = rule.plane;
    std::unordered_map<uint64_t, std::vector<uint32_t>> cells;
    const auto cellKey = [](int x, int y, int z) {
        return (static_cast<uint64_t>(static_cast<uint32_t>(x) & 0x1FFFFFu) << 42) |
               (static_cast<uint64_t>(static_cast<uint32_t>(y) & 0x1FFFFFu) << 21) | (static_cast<uint64_t>(static_cast<uint32_t>(z) & 0x1FFFFFu));
    };
    const auto lowCell = [&](const CoplanarPrim& g, int a) { return static_cast<int>(std::floor((g.box[a] - plane) / grid)); };
    const auto cellsOf = [&](const CoplanarPrim& g, const auto& visit) {
        for (int a = 0; a < 3; ++a)
            if (g.box[3 + a] - g.box[a] > rule.largest) return;
        int lo[3], hi[3];
        for (int a = 0; a < 3; ++a) {
            lo[a] = lowCell(g, a);
            hi[a] = static_cast<int>(std::floor((g.box[3 + a] + plane) / grid));
        }
        for (int x = lo[0]; x <= hi[0]; ++x)
            for (int y = lo[1]; y <= hi[1]; ++y)
                for (int z = lo[2]; z <= hi[2]; ++z) visit(cellKey(x, y, z), x, y, z);
    };
    for (size_t p = 0; p < prims.size(); ++p)
        if (prims[p].area > 0.0f) cellsOf(prims[p], [&](uint64_t k, int, int, int) { cells[k].push_back(static_cast<uint32_t>(p)); });
    // a shared area, not an edge: more than a square of a fifth of the tolerance (1 cm for the cells' 5 cm)
    const double minArea = 0.04 * static_cast<double>(plane) * static_cast<double>(plane);
    std::vector<CoplanarPair> pairs;
    for (size_t p = 0; p < prims.size(); ++p) {
        const CoplanarPrim& gp = prims[p];
        if (gp.area <= 0.0f) continue;
        cellsOf(gp, [&](uint64_t cell, int x, int y, int z) {
            for (const uint32_t q : cells[cell]) {
                if (q <= p) continue;
                const CoplanarPrim& gq = prims[q];
                if (rule.sameRunOnly && gq.run != gp.run) continue;
                // a pair is met in every cell both touch: taken in the first of them only
                if (x != std::max(lowCell(gp, 0), lowCell(gq, 0)) || y != std::max(lowCell(gp, 1), lowCell(gq, 1)) ||
                    z != std::max(lowCell(gp, 2), lowCell(gq, 2)))
                    continue;
                bool apart = false;
                for (int a = 0; a < 3 && !apart; ++a) apart = gq.box[a] > gp.box[3 + a] + plane || gp.box[a] > gq.box[3 + a] + plane;
                if (apart) continue;
                // triangle by triangle (a quad need not be flat): near parallel, the part of each over the other with an
                // area and within `plane` of the other's plane
                bool overlap = false, tooFar = false;
                double misfit = 0.0;
                for (int ta = 0; ta + 3 <= gp.size && !tooFar; ta += 3)
                    for (int tb = 0; tb + 3 <= gq.size && !tooFar; tb += 3) {
                        const rr::TriangleSoup::Vertex* va = &soup[static_cast<size_t>(gp.first + ta)];
                        const rr::TriangleSoup::Vertex* vb = &soup[static_cast<size_t>(gq.first + tb)];
                        float na[3], nb[3];
                        const float la = CoplanarTriArea2(va, na), lb = CoplanarTriArea2(vb, nb);
                        if (la < 1e-6f || lb < 1e-6f) continue;
                        const float dot = na[0] * nb[0] + na[1] * nb[1] + na[2] * nb[2];
                        if (std::fabs(dot) < (rule.legacy ? 0.998f : 0.9f) * la * lb) continue;
                        if (rule.legacy) { // the older rule: every corner of each within `plane` of the other's plane
                            const auto inPlane = [plane](const rr::TriangleSoup::Vertex* at, const float* n, float l,
                                                         const rr::TriangleSoup::Vertex* tri) {
                                for (int k = 0; k < 3; ++k) {
                                    const float d = n[0] * (tri[k].x - at[0].x) + n[1] * (tri[k].y - at[0].y) + n[2] * (tri[k].z - at[0].z);
                                    if (std::fabs(d) > plane * l) return false;
                                }
                                return true;
                            };
                            if (!inPlane(va, na, la, vb) || !inPlane(vb, nb, lb, va)) continue;
                            double areaAB, farAB;
                            OverPart(va, vb, areaAB, farAB);
                            if (areaAB > minArea) overlap = true;
                            continue;
                        }
                        // one-sided front and back of one face: the back-face test keeps one of them, no fight
                        if (rule.facing && dot < 0.0f && va[0].window[3] != 0 && vb[0].window[3] != 0) continue;
                        double areaAB, farAB, areaBA, farBA;
                        OverPart(va, vb, areaAB, farAB);
                        if (areaAB <= minArea) continue;
                        OverPart(vb, va, areaBA, farBA);
                        if (areaBA <= minArea) continue;
                        // two triangles that share an area but part more than the tolerance over it: not layers (a
                        // crossing, a step) - the whole pair is dropped, as the older rule drops any stray corner
                        if (farAB > plane || farBA > plane) {
                            tooFar = true;
                            continue;
                        }
                        overlap = true;
                        misfit = std::max({misfit, farAB, farBA});
                    }
                if (overlap && !tooFar) pairs.push_back({static_cast<uint32_t>(p), q, static_cast<float>(misfit)});
            }
        });
    }
    return pairs;
}

void CoplanarNormal(const std::vector<rr::TriangleSoup::Vertex>& soup, const CoplanarPrim& p, float n[3]) {
    const float l = CoplanarTriArea2(&soup[static_cast<size_t>(p.first)], n);
    for (int a = 0; a < 3; ++a) n[a] = l > 0.0f ? n[a] / l : 0.0f;
}

std::vector<int> CoplanarLayerOf(const std::vector<CoplanarPrim>& prims, const std::vector<CoplanarPair>& pairs, int cap,
                                 const std::function<bool(uint32_t, uint32_t)>& skip, std::vector<float>* lift) {
    // the winner: the higher priority, the smaller primitive (areas compared to 1 %), of two alike the first in the
    // soup - a strict order, weakest first, so each primitive's layer is one above the highest layer it covers
    const auto rank = [&](uint32_t i) {
        const CoplanarPrim& p = prims[i];
        return std::make_tuple(p.priority, -static_cast<int>(std::lround(std::log(std::max(p.area, 1e-6f)) / std::log(1.01f))), -p.first);
    };
    std::vector<std::vector<std::pair<uint32_t, float>>> beneath(prims.size());
    for (const CoplanarPair& pair : pairs) {
        if (skip && skip(pair.a, pair.b)) continue;
        const bool aWins = rank(pair.a) > rank(pair.b);
        (aWins ? beneath[pair.a] : beneath[pair.b]).emplace_back(aWins ? pair.b : pair.a, pair.misfit);
    }
    std::vector<uint32_t> order(prims.size());
    for (uint32_t i = 0; i < order.size(); ++i) order[i] = i;
    std::sort(order.begin(), order.end(), [&](uint32_t x, uint32_t y) { return rank(x) < rank(y); });
    std::vector<int> layer(prims.size(), 0);
    if (lift != nullptr) lift->assign(prims.size(), 0.0f);
    for (const uint32_t p : order)
        for (const auto& [q, misfit] : beneath[p]) {
            layer[p] = std::min(cap, std::max(layer[p], layer[q] + 1));
            if (lift != nullptr) (*lift)[p] = std::max((*lift)[p], misfit + (*lift)[q]);
        }
    return layer;
}

void UploadLift(GLuint vao, GLuint& vbo, const std::vector<float>& lift) {
    GLint previous = 0; // the caller's array buffer stays bound (a soup upload may follow)
    glGetIntegerv(0x8894 /* GL_ARRAY_BUFFER_BINDING */, &previous);
    struct Restore {
        GLint id;
        ~Restore() { gl.BindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(id)); }
    } restore{previous};
    gl.BindVertexArray(vao);
    if (vbo == 0) gl.GenBuffers(1, &vbo);
    gl.BindBuffer(GL_ARRAY_BUFFER, vbo);
    gl.BufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(lift.size() * sizeof(float)), lift.empty() ? nullptr : lift.data(), GL_STATIC_DRAW);
    gl.VertexAttribPointer(kLiftAttribute, 3, GL_FLOAT, GL_FALSE, 3 * static_cast<GLsizei>(sizeof(float)), nullptr);
    gl.EnableVertexAttribArray(kLiftAttribute);
}

// ---- the models

void ModelLayers::Build(GLuint vao, const std::vector<rr::TriangleSoup::Vertex>& soup,
                        const std::vector<std::pair<GLint, GLsizei>>& ranges, float unitsPerWorld, bool posed) {
    draws_.assign(ranges.size(), {});
    pairs_ = winners_ = 0;
    // DEVELOPMENT: RRJB_DECALS=off - the models without their layers (the before / after control)
    static const bool off = std::getenv("RRJB_DECALS") != nullptr && std::string(std::getenv("RRJB_DECALS")) == "off";
    if (off || vao == 0) return;
    // A model primitive is six soup vertices (rmd3.cpp BuildTriangleSoup): a quad's two triangles; a 3-corner one's
    // triangle and a zero-area (i2, i2, i2) that keeps the stride (rmd3.h SoupCorners). The tolerances in world units: a decal
    // lies within a centimetre of its plate (the model's coordinates are integers of 1/1024 world unit, or coarser);
    // candidates on a 0.5-unit grid; a primitive longer than 64 units is ground, not a decal's plate.
    CoplanarRule rule;
    rule.plane = 0.01f * unitsPerWorld;
    rule.grid = 0.5f * unitsPerWorld;
    rule.largest = 64.0f * unitsPerWorld;
    rule.sameRunOnly = posed;
    rule.facing = true;
    std::vector<CoplanarPrim> prims;
    for (size_t r = 0; r < ranges.size(); ++r) {
        const GLint end = std::min<GLint>(ranges[r].first + ranges[r].second, static_cast<GLint>(soup.size()));
        for (GLint i = ranges[r].first; i + 6 <= end; i += 6) {
            CoplanarPrim p;
            p.run = r;
            p.first = i;
            // a 3-corner primitive (clut bit 13 clear) is the emitter's GT3 (i0, i1, i2) - rmd3.md 3.3; its soup's second
            // triangle has no area and takes no part in a pair nor in a redraw
            p.size = (soup[static_cast<size_t>(i)].clut & 0x2000u) ? 6 : 3;
            // a one-sided face (a sign's plate) over a two-sided primitive (its post, a frame seen from both sides):
            // the post of a HAZARD sign rises 1.6 cm into the plane of its plate (rmd3.md 3: the front / back plates)
            p.priority = soup[static_cast<size_t>(i)].window[3] != 0 ? 1 : 0;
            CoplanarMeasure(soup, p);
            prims.push_back(p);
        }
    }
    std::vector<CoplanarPair> pairs = FindCoplanarPairs(soup, prims, rule);
    // a pair across two ranges (two groups of one file: LODs, other models) is no pair - one of them is drawn
    pairs.erase(std::remove_if(pairs.begin(), pairs.end(), [&](const CoplanarPair& p) { return prims[p.a].run != prims[p.b].run; }),
                pairs.end());
    pairs_ = pairs.size();
    if (pairs.empty()) return;
    std::vector<float> lifts;
    const std::vector<int> layer = CoplanarLayerOf(prims, pairs, 3, {}, posed ? nullptr : &lifts);
    if (std::getenv("RRJB_DECALS_DUMP") != nullptr) { // DEVELOPMENT: every pair
        for (const CoplanarPair& pair : pairs) {
            std::printf("decals: range %zu prims at %d (area %.2f layer %d) and %d (area %.2f layer %d), misfit %.4f\n",
                        prims[pair.a].run, prims[pair.a].first, prims[pair.a].area / (unitsPerWorld * unitsPerWorld), layer[pair.a],
                        prims[pair.b].first, prims[pair.b].area / (unitsPerWorld * unitsPerWorld), layer[pair.b],
                        pair.misfit / unitsPerWorld);
            for (const uint32_t i : {pair.a, pair.b}) {
                const rr::TriangleSoup::Vertex* v = &soup[static_cast<size_t>(prims[i].first)];
                std::printf("decals:   %d one-sided %u clut %04X tpage %04X:", prims[i].first, v[0].window[3], v[0].clut, v[0].tpage);
                for (int k = 0; k < prims[i].size; ++k) std::printf(" (%.0f %.0f %.0f | %.0f %.0f)", v[k].x, v[k].y, v[k].z, v[k].u, v[k].v);
                std::printf("\n");
            }
        }
    }
    std::vector<uint32_t> indices;
    std::vector<std::vector<uint32_t>> byRun(ranges.size());
    for (uint32_t i = 0; i < prims.size(); ++i)
        if (layer[i] > 0) byRun[prims[i].run].push_back(i);
    for (size_t r = 0; r < byRun.size(); ++r)
        for (int l = 1; l <= 3; ++l) {
            Draw d;
            d.layer = l;
            d.first = static_cast<GLsizei>(indices.size());
            for (const uint32_t i : byRun[r]) {
                if (layer[i] != l) continue;
                for (int k = 0; k < prims[i].size; ++k) indices.push_back(static_cast<uint32_t>(prims[i].first + k));
                ++winners_;
            }
            d.count = static_cast<GLsizei>(indices.size()) - d.first;
            if (d.count > 0) draws_[r].push_back(d);
        }
    if (indices.empty()) return;
    if (!posed) { // the winners' lifts: the plane's unit normal times the lift (soup units; the shader turns it by uModel)
        std::vector<float> perVertex(3 * soup.size(), 0.0f);
        for (uint32_t i = 0; i < prims.size(); ++i) {
            if (layer[i] == 0 || lifts[i] <= 0.0f) continue;
            float n[3];
            CoplanarNormal(soup, prims[i], n);
            for (int k = 0; k < prims[i].size; ++k)
                for (int a = 0; a < 3; ++a) perVertex[3 * static_cast<size_t>(prims[i].first + k) + static_cast<size_t>(a)] = n[a] * lifts[i];
        }
        UploadLift(vao, liftVbo_, perVertex);
    }
    gl.BindVertexArray(vao); // the element buffer belongs to the soup's vertex array
    gl.GenBuffers(1, &ebo_);
    gl.BindBuffer(kElementArrayBuffer, ebo_);
    gl.BufferData(kElementArrayBuffer, static_cast<GLsizeiptr>(indices.size() * sizeof(uint32_t)), indices.data(), GL_STATIC_DRAW);
}

const std::vector<ModelLayers::Draw>& ModelLayers::Of(size_t range) const {
    static const std::vector<Draw> none;
    return range < draws_.size() && ebo_ != 0 ? draws_[range] : none;
}

} // namespace rr::render
