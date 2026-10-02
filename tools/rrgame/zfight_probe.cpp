// DEVELOPMENT: the depth-fight detector. RRJB_ZFIGHT=<every N frames>[,<dir>[,<min>[,<from>]]] re-draws
// view 0 of a sampled frame into a private 960-pixel-wide target four times from the same state:
//   A, A again - the control: a view drawn twice must come out equal (0 pixels);
//   B, jitter  - the projection's near-plane depth term scaled by 1 + 1e-3 (x, y, w and so the coverage and every
//                attribute stay; only the depths move a little and round differently): a pixel that changes is decided
//                by less than the depth buffer's precision - two near-coplanar surfaces fighting;
//   C, ties    - glDepthFunc(GL_LEQUAL) instead of GL_LESS: a pixel that changes is covered twice at EXACTLY the same
//                depth by different colours (a double draw of one surface, or coplanar layers).
//   D, thin    - every decal pass (the lane lines, the cells' and the models' layers) moved RRJB_ZFIGHT_THIN world units
//                (default 0.005) farther along its view rays: a pixel that changes is a decal decided by less than that
//                against its plate - the float rounding of a moving eye decides it anew every frame.
// Prints `zfight: frame F control X jitter Y ties Z thin T` (pixels of W x H) and a sample pixel for RRJB_PICK; with
// <dir> writes F_view.png (A), F_lequal.png (C) and F_mask.png (A darkened, jitter red, ties cyan, thin yellow) for frames
// with at least <min> such pixels (default 1; 0: every probed frame). Nothing of it runs without the variable; it
// draws only into its own target and restores the caller's framebuffer and viewport.
#include "race_render.h"

#include "render/frame_shot.h"
#include "render/gl_api.h"
#include "render/render_target.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <tuple>
#include <vector>

namespace rrgame {

namespace {

struct ProbeConfig {
    long every = 0;
    std::string dir;
    size_t minSaved = 1;
    long from = 0; // the first frame probed (a close look at one place)
};

const ProbeConfig& Config() {
    static const ProbeConfig config = [] {
        ProbeConfig c;
        if (const char* e = std::getenv("RRJB_ZFIGHT")) {
            const std::string s = e;
            c.every = std::atol(s.c_str());
            const size_t comma = s.find(',');
            if (comma != std::string::npos) c.dir = s.substr(comma + 1);
            const size_t comma2 = c.dir.find(',');
            if (comma2 != std::string::npos) {
                c.minSaved = static_cast<size_t>(std::max(0L, std::atol(c.dir.c_str() + comma2 + 1)));
                const size_t comma3 = c.dir.find(',', comma2 + 1);
                if (comma3 != std::string::npos) c.from = std::atol(c.dir.c_str() + comma3 + 1);
                c.dir.resize(comma2);
            }
        }
        return c;
    }();
    return config;
}

size_t Differ(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b, std::vector<uint8_t>* mask, uint8_t bit) {
    size_t n = 0;
    for (size_t i = 0; i + 3 < a.size() && i + 3 < b.size(); i += 4)
        if (a[i] != b[i] || a[i + 1] != b[i + 1] || a[i + 2] != b[i + 2]) {
            ++n;
            if (mask) (*mask)[i / 4] |= bit;
        }
    return n;
}

} // namespace

namespace {

std::vector<float> g_precisionPoints; // RRJB_ZFIGHT_PRECISION: the cell soup's positions (ZFightSoupReport keeps them)

// DEVELOPMENT: RRJB_ZFIGHT_PRECISION=1 - the depth the vertex stage computes for the cell soup's vertices
// of this view, in float as the shader does, against the same transform in double: the absolute transform VP world (the
// matrix's translation holds the eye's thousands of world units) and the camera-relative one VP' (world - eye)
// (multiview.h SetRenderOrigin). The error of NDC z turned into view depth (dz/dw = -b / w^2, b the eye's clip z), per
// distance band, next to the depth buffer's own step there (24 bits).
void PrecisionReport(const GameView& view, long frame) {
    if (g_precisionPoints.empty()) return;
    const rr::render::Mat4 vp = rr::render::Multiply(view.proj, view.view);
    const float* m = vp.m;
    double b = 0.0;
    for (int k = 0; k < 3; ++k) b += static_cast<double>(m[4 * k + 2]) * view.eye[k];
    b += m[14];
    float rel[16];
    std::memcpy(rel, m, sizeof(rel));
    for (int i = 0; i < 4; ++i)
        rel[12 + i] = static_cast<float>(static_cast<double>(m[i]) * view.eye[0] + static_cast<double>(m[4 + i]) * view.eye[1] +
                                         static_cast<double>(m[8 + i]) * view.eye[2] + static_cast<double>(m[12 + i]));
    constexpr double kEdges[] = {0.5, 2.0, 5.0, 10.0, 20.0, 50.0, 100.0, 200.0, 500.0};
    constexpr int kBands = 8;
    double sumA[kBands] = {}, sumR[kBands] = {}, maxA[kBands] = {}, maxR[kBands] = {};
    size_t count[kBands] = {};
    const auto ndcZ = [](const float* mm, float x, float y, float z) { // float, as the vertex stage sums a row
        float cz = mm[2] * x;
        cz += mm[6] * y;
        cz += mm[10] * z;
        cz += mm[14];
        float cw = mm[3] * x;
        cw += mm[7] * y;
        cw += mm[11] * z;
        cw += mm[15];
        return std::make_pair(static_cast<double>(cz) / static_cast<double>(cw), static_cast<double>(cw));
    };
    for (size_t i = 0; i + 2 < g_precisionPoints.size(); i += 3) {
        const float x = g_precisionPoints[i], y = g_precisionPoints[i + 1], z = g_precisionPoints[i + 2];
        double cx = 0.0, cy = 0.0, cz = 0.0, cw = 0.0;
        const double p[4] = {x, y, z, 1.0};
        for (int k = 0; k < 4; ++k) {
            cx += static_cast<double>(m[4 * k]) * p[k], cy += static_cast<double>(m[4 * k + 1]) * p[k];
            cz += static_cast<double>(m[4 * k + 2]) * p[k], cw += static_cast<double>(m[4 * k + 3]) * p[k];
        }
        if (cw < kEdges[0] || cw >= kEdges[kBands]) continue;
        if (std::fabs(cx) > 1.1 * cw || std::fabs(cy) > 1.1 * cw) continue; // in the picture only
        int band = 0;
        while (band + 1 < kBands && cw >= kEdges[band + 1]) ++band;
        const double ref = cz / cw;
        const double toDepth = cw * cw / std::fabs(b);
        const double ea = std::fabs(ndcZ(m, x, y, z).first - ref) * toDepth;
        // the camera-relative sum against ITS matrix in double: the float rounding of that matrix's translation (a few
        // 1e-5, the eye's own) moves every vertex of the view alike - a near plane off by that much, no fight - where the
        // arithmetic below is new for every vertex
        const float dx = x - view.eye[0], dy = y - view.eye[1], dz = z - view.eye[2];
        double rz = 0.0, rw = 0.0;
        const double d4[4] = {dx, dy, dz, 1.0};
        for (int k = 0; k < 4; ++k) rz += static_cast<double>(rel[4 * k + 2]) * d4[k], rw += static_cast<double>(rel[4 * k + 3]) * d4[k];
        const double er = std::fabs(ndcZ(rel, dx, dy, dz).first - rz / rw) * toDepth;
        sumA[band] += ea * ea, sumR[band] += er * er;
        maxA[band] = std::max(maxA[band], ea), maxR[band] = std::max(maxR[band], er);
        ++count[band];
    }
    std::printf("zfight precision: frame %ld eye (%.1f %.1f %.1f) b %.5f - view depth error of the vertex stage, world units "
                "rms / max: absolute | camera-relative | 24-bit step\n",
                frame, view.eye[0], view.eye[1], view.eye[2], b);
    for (int k = 0; k < kBands; ++k) {
        if (count[k] == 0) continue;
        const double mid = 0.5 * (kEdges[k] + kEdges[k + 1]);
        std::printf("zfight precision:   %5.1f..%5.1f m %7zu vertices  %.5f / %.5f | %.7f / %.7f | %.5f\n", kEdges[k], kEdges[k + 1],
                    count[k], std::sqrt(sumA[k] / static_cast<double>(count[k])), maxA[k],
                    std::sqrt(sumR[k] / static_cast<double>(count[k])), maxR[k], mid * mid / std::fabs(b) / 8388608.0);
    }
    std::fflush(stdout);
}

} // namespace

bool ZFightProbeWanted(long frame) {
    const ProbeConfig& c = Config();
    return c.every > 0 && frame > 0 && frame >= c.from && frame % c.every == 0;
}

void ZFightProbe(RaceRenderer& renderer, const GameView& view, long frame) {
    if (!ZFightProbeWanted(frame)) return;
    if (std::getenv("RRJB_ZFIGHT_PRECISION") != nullptr) PrecisionReport(view, frame);
    static rr::render::RenderTarget target;
    const int w = 960;
    const float aspect = view.viewport[3] > 0 ? static_cast<float>(view.viewport[2]) / static_cast<float>(view.viewport[3]) : 16.0f / 9.0f;
    const int h = std::max(16, static_cast<int>(static_cast<float>(w) / aspect));
    if (!target.Ensure(w, h, 1)) return;
    const auto draw = [&](const GameView& v, bool lequal) {
        target.Bind();
        glViewport(0, 0, w, h);
        glDisable(GL_SCISSOR_TEST);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
        glDepthFunc(lequal ? GL_LEQUAL : GL_LESS);
        renderer.RenderView(v, frame);
        glDepthFunc(GL_LESS);
        target.Bind();
        return rr::render::ReadFrame(w, h);
    };
    GameView v = view;
    v.framebuffer = 0; // drawn into the bound target (RenderView binds nothing itself)
    v.viewport[0] = v.viewport[1] = 0;
    v.viewport[2] = w;
    v.viewport[3] = h;
    v.split = false;
    const std::vector<uint8_t> a = draw(v, false);
    const std::vector<uint8_t> a2 = draw(v, false);
    GameView j = v;
    // the depth row's near-plane term (z_ndc = -m10 - m14 / z_eye): every depth moves by ~2e-3 n / z, monotonic in
    // z, nothing crosses the far plane - only the rounding of the stored depths changes
    j.proj.m[14] += 1e-3f * j.proj.m[14];
    const std::vector<uint8_t> b = draw(j, false);
    const std::vector<uint8_t> c = draw(v, true);
    static const float thin = [] { // D: the decal passes moved this far along their rays
        const char* e = std::getenv("RRJB_ZFIGHT_THIN");
        return e != nullptr ? static_cast<float>(std::atof(e)) : 0.005f;
    }();
    rr::render::RaceScene::SetDecalProbeShift(thin);
    const std::vector<uint8_t> d = draw(v, false);
    rr::render::RaceScene::SetDecalProbeShift(0.0f);
    std::vector<uint8_t> mask(static_cast<size_t>(w) * h, 0);
    const size_t control = Differ(a, a2, nullptr, 0);
    const size_t jitter = Differ(a, b, &mask, 1);
    const size_t ties = Differ(a, c, &mask, 2);
    const size_t thinPixels = Differ(a, d, &mask, 4);
    std::printf("zfight: frame %ld control %zu jitter %zu ties %zu thin %zu of %dx%d\n", frame, control, jitter, ties, thinPixels,
                w, h);
    { // the middle one of the marked pixels (top-left origin), for RRJB_PICK on the probe's own target
        std::vector<size_t> marked;
        for (size_t i = 0; i < mask.size(); ++i)
            if (mask[i]) marked.push_back(i);
        if (!marked.empty()) {
            const size_t i = marked[marked.size() / 2];
            std::printf("zfight: frame %ld sample pixel %zu,%zu (mask %u)\n", frame, i % static_cast<size_t>(w),
                        static_cast<size_t>(h) - 1 - i / static_cast<size_t>(w), mask[i]);
        }
    }
    if (!Config().dir.empty() && jitter + ties + thinPixels >= Config().minSaved) {
        char name[64];
        std::snprintf(name, sizeof name, "/%06ld_view.png", frame);
        rr::render::SaveShot(Config().dir + name, w, h, a);
        std::snprintf(name, sizeof name, "/%06ld_lequal.png", frame);
        rr::render::SaveShot(Config().dir + name, w, h, c);
        std::vector<uint8_t> m = a;
        for (size_t i = 0; i < mask.size(); ++i) {
            uint8_t* p = &m[4 * i];
            if (mask[i] & 4) { // thin: yellow
                p[0] = 255, p[1] = 255, p[2] = 0;
            } else if (mask[i] & 2) {
                p[0] = 0, p[1] = 255, p[2] = 255;
            } else if (mask[i] & 1) {
                p[0] = 255, p[1] = 0, p[2] = 0;
            } else {
                p[0] = static_cast<uint8_t>(p[0] / 3), p[1] = static_cast<uint8_t>(p[1] / 3), p[2] = static_cast<uint8_t>(p[2] / 3);
            }
        }
        std::snprintf(name, sizeof name, "/%06ld_mask.png", frame);
        rr::render::SaveShot(Config().dir + name, w, h, m);
    }
    std::fflush(stdout);
    rr::render::BindFramebuffer(view.framebuffer);
    glViewport(view.viewport[0], view.viewport[1], view.viewport[2], view.viewport[3]);
}

} // namespace rrgame

// DEVELOPMENT: RRJB_ZFIGHT_SOUP=1 - the static cell soup's coplanar overlaps: every pair of triangles
// (not a sub-area's coarse / fine alternatives) that lie in one plane (normal within ~0.3 degrees, offset within 0.05 world units) and
// overlap with a positive area, counted per pair of runs; `exact` when the three corners are the same points.
namespace rrgame {

namespace {

struct SoupTri {
    float p[3][3];
    float n[3], d;
    size_t run, first; // the run, the triangle's first soup vertex
    bool flip = false, oneSided = false;
};

// 2D triangle overlap with a positive area (separating axis over the six edges, shrunk by eps).
bool Overlap2(const float a[3][2], const float b[3][2]) {
    const auto sep = [](const float s[3][2], const float o[3][2]) {
        for (int e = 0; e < 3; ++e) {
            const float ex = s[(e + 1) % 3][0] - s[e][0], ey = s[(e + 1) % 3][1] - s[e][1];
            const float nx = -ey, ny = ex;
            float smin = 1e30f, smax = -1e30f, omin = 1e30f, omax = -1e30f;
            for (int k = 0; k < 3; ++k) {
                const float ps = nx * s[k][0] + ny * s[k][1], po = nx * o[k][0] + ny * o[k][1];
                smin = std::min(smin, ps), smax = std::max(smax, ps);
                omin = std::min(omin, po), omax = std::max(omax, po);
            }
            const float len = std::sqrt(nx * nx + ny * ny), eps = 1e-3f * len;
            if (omax <= smin + eps || omin >= smax - eps) return true;
        }
        return false;
    };
    return !sep(a, b) && !sep(b, a);
}

} // namespace

void ZFightSoupReport(const rr::TriangleSoup& soup, const std::vector<rr::render::CellRange>& ranges) {
    if (std::getenv("RRJB_ZFIGHT_PRECISION") != nullptr) { // the probe's precision report
        g_precisionPoints.clear();
        for (const rr::TriangleSoup::Vertex& v : soup.vertices) g_precisionPoints.insert(g_precisionPoints.end(), {v.x, v.y, v.z});
    }
    if (const char* at = std::getenv("RRJB_ZFIGHT_NEAR")) { // "x,z,r": every triangle with a corner that near (x, z)
        float x = 0.0f, z = 0.0f, r = 0.0f;
        if (std::sscanf(at, "%f,%f,%f", &x, &z, &r) == 3)
            for (const rr::render::CellRange& range : ranges)
                for (GLint k = range.first; k + 3 <= range.first + range.count; k += 3) {
                    const rr::TriangleSoup::Vertex* v = &soup.vertices[static_cast<size_t>(k)];
                    bool hit = false;
                    for (int c = 0; c < 3; ++c) hit = hit || std::hypot(v[c].x - x, v[c].z - z) < r;
                    if (!hit) continue;
                    std::printf("zfight near: c%zu g%d tri %d tp %04X ot %.0f | (%.3f %.3f %.3f) (%.3f %.3f %.3f) (%.3f %.3f %.3f) uv "
                                "(%.0f,%.0f) (%.0f,%.0f) (%.0f,%.0f)\n",
                                range.cell, range.group, k, v[0].tpage, v[0].ot, v[0].x, v[0].y, v[0].z, v[1].x, v[1].y, v[1].z,
                                v[2].x, v[2].y, v[2].z, v[0].u, v[0].v, v[1].u, v[1].v, v[2].u, v[2].v);
                }
    }
    if (const char* list = std::getenv("RRJB_ZFIGHT_TRIS")) { // "first,first,..": those soup triangles' corners
        for (const char* p = list; *p;) {
            const size_t k = static_cast<size_t>(std::strtoul(p, nullptr, 10));
            for (size_t c = 0; c < 3 && k + c < soup.vertices.size(); ++c) {
                const rr::TriangleSoup::Vertex& v = soup.vertices[k + c];
                std::printf("zfight tri %zu.%zu: pos (%.3f, %.3f, %.3f) uv (%.0f, %.0f) clut %04X tpage %04X ot %.0f one-sided %u\n", k, c,
                            v.x, v.y, v.z, v.u, v.v, v.clut, v.tpage, v.ot, v.window[3]);
            }
            while (*p && *p != ',') ++p;
            if (*p == ',') ++p;
        }
    }
    const char* env = std::getenv("RRJB_ZFIGHT_SOUP");
    if (env == nullptr || *env == '0') return;
    std::vector<SoupTri> tris;
    for (size_t r = 0; r < ranges.size(); ++r)
        for (GLint k = ranges[r].first; k + 3 <= ranges[r].first + ranges[r].count; k += 3) {
            SoupTri t{};
            for (int c = 0; c < 3; ++c) {
                const rr::TriangleSoup::Vertex& v = soup.vertices[static_cast<size_t>(k + c)];
                t.p[c][0] = v.x, t.p[c][1] = v.y, t.p[c][2] = v.z;
            }
            float e1[3], e2[3];
            for (int a = 0; a < 3; ++a) e1[a] = t.p[1][a] - t.p[0][a], e2[a] = t.p[2][a] - t.p[0][a];
            float n[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
            const float l = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
            if (l < 1e-6f) continue;
            int big = 0;
            for (int a = 1; a < 3; ++a)
                if (std::fabs(n[a]) > std::fabs(n[big])) big = a;
            const float s = n[big] < 0.0f ? -1.0f / l : 1.0f / l;
            t.flip = n[big] < 0.0f;
            t.oneSided = soup.vertices[static_cast<size_t>(k)].window[3] != 0;
            for (int a = 0; a < 3; ++a) t.n[a] = n[a] * s;
            t.d = t.n[0] * t.p[0][0] + t.n[1] * t.p[0][1] + t.n[2] * t.p[0][2];
            t.run = r;
            t.first = static_cast<size_t>(k);
            tris.push_back(t);
        }
    // buckets by the quantised plane; a triangle is looked up in its own and the neighbouring offset bucket
    const auto key = [](const SoupTri& t, int dd) {
        return std::make_tuple(static_cast<int>(std::lround(t.n[0] * 200.0f)), static_cast<int>(std::lround(t.n[1] * 200.0f)),
                               static_cast<int>(std::lround(t.n[2] * 200.0f)), static_cast<int>(std::floor(t.d * 20.0f)) + dd);
    };
    std::map<std::tuple<int, int, int, int>, std::vector<size_t>> buckets;
    for (size_t i = 0; i < tris.size(); ++i) buckets[key(tris[i], 0)].push_back(i);
    std::map<std::pair<size_t, size_t>, std::array<size_t, 2>> pairs; // runs -> {overlapping, exact}
    size_t overlapping = 0, oppositeExact = 0, sameExact = 0;
    for (size_t i = 0; i < tris.size(); ++i) {
        const SoupTri& a = tris[i];
        int big = 0;
        for (int ax = 1; ax < 3; ++ax)
            if (std::fabs(a.n[ax]) > std::fabs(a.n[big])) big = ax;
        const int u = (big + 1) % 3, w = (big + 2) % 3;
        for (int dd = -1; dd <= 1; ++dd) {
            const auto it = buckets.find(key(a, dd));
            if (it == buckets.end()) continue;
            for (size_t j : it->second) {
                if (j <= i) continue;
                const SoupTri& b = tris[j];
                const rr::render::CellRange &rA = ranges[a.run], &rB = ranges[b.run];
                // one cell's coarse (band 0) and fine (band 1) versions of a sub-area: CellLodWord draws one of the two
                if (rA.cell == rB.cell && rA.band != rB.band) continue;
                if (std::fabs(a.d - b.d) > 0.05f) continue;
                float pa[3][2], pb[3][2];
                for (int c = 0; c < 3; ++c) {
                    pa[c][0] = a.p[c][u], pa[c][1] = a.p[c][w];
                    pb[c][0] = b.p[c][u], pb[c][1] = b.p[c][w];
                }
                if (!Overlap2(pa, pb)) continue;
                bool exact = true;
                for (int c = 0; c < 3 && exact; ++c) {
                    bool found = false;
                    for (int e = 0; e < 3 && !found; ++e)
                        found = std::fabs(a.p[c][0] - b.p[e][0]) + std::fabs(a.p[c][1] - b.p[e][1]) + std::fabs(a.p[c][2] - b.p[e][2]) < 1e-3f;
                    exact = found;
                }
                auto& slot = pairs[{std::min(a.run, b.run), std::max(a.run, b.run)}];
                ++slot[0];
                if (exact) ++slot[1];
                // the winding: a front / back pair of one face (opposite normals before the sign fold) is not a fight
                // when the emitters' back-face test keeps one of them
                if (exact && a.flip != b.flip) ++oppositeExact;
                if (exact && a.flip == b.flip) ++sameExact;
                static const int detailCell = std::atoi(env) - 1; // RRJB_ZFIGHT_SOUP=<cell + 1>: that cell's pairs
                if (detailCell >= 0 && (rA.cell == static_cast<size_t>(detailCell) || rB.cell == static_cast<size_t>(detailCell))) {
                    const auto area = [](const SoupTri& t) {
                        float e1[3], e2[3];
                        for (int q = 0; q < 3; ++q) e1[q] = t.p[1][q] - t.p[0][q], e2[q] = t.p[2][q] - t.p[0][q];
                        const float c[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
                        return 0.5f * std::sqrt(c[0] * c[0] + c[1] * c[1] + c[2] * c[2]);
                    };
                    const auto uvs = [&](size_t tri) {
                        char buf[96];
                        const rr::TriangleSoup::Vertex* v = &soup.vertices[tri];
                        std::snprintf(buf, sizeof buf, "uv (%.0f,%.0f)(%.0f,%.0f)(%.0f,%.0f) clut %04X tp %04X", v[0].u, v[0].v, v[1].u,
                                      v[1].v, v[2].u, v[2].v, v[0].clut, v[0].tpage);
                        return std::string(buf);
                    };
                    std::printf("zfight soup pair: c%zu g%d tri@%zu area %.3f %s | c%zu g%d tri@%zu area %.3f %s | d %.4f %s%s\n", rA.cell,
                                rA.group, a.first, area(a), uvs(a.first).c_str(), rB.cell, rB.group, b.first, area(b),
                                uvs(b.first).c_str(), a.d - b.d, exact ? "exact" : "", a.flip != b.flip ? " opposite" : "");
                }
                ++overlapping;
            }
        }
    }
    size_t sameGroup = 0, sameCell = 0, crossCell = 0, exactPairs = 0;
    for (const auto& [runs, n] : pairs) {
        const rr::render::CellRange &r0 = ranges[runs.first], &r1 = ranges[runs.second];
        (r0.cell != r1.cell ? crossCell : r0.group == r1.group ? sameGroup : sameCell) += n[0];
        exactPairs += n[1];
    }
    std::printf("zfight soup: %zu triangles, %zu coplanar overlapping pairs (%zu exact) in %zu run pairs: %zu within one group, %zu "
                "between groups of one cell, %zu across cells\n",
                tris.size(), overlapping, exactPairs, pairs.size(), sameGroup, sameCell, crossCell);
    std::printf("zfight soup: exact pairs of the same winding %zu, of opposite windings %zu\n", sameExact, oppositeExact);
    std::vector<std::pair<size_t, std::pair<size_t, size_t>>> top;
    for (const auto& [runs, n] : pairs) top.push_back({n[0], runs});
    std::sort(top.rbegin(), top.rend());
    for (size_t k = 0; k < top.size() && k < 40; ++k) {
        const rr::render::CellRange& ra = ranges[top[k].second.first];
        const rr::render::CellRange& rb = ranges[top[k].second.second];
        std::printf("zfight soup:   cell %zu group %d band %d key %04X  x  cell %zu group %d band %d key %04X : %zu pairs (%zu exact)\n",
                    ra.cell, ra.group, ra.band, ra.texKey, rb.cell, rb.group, rb.band, rb.texKey, top[k].first,
                    pairs[top[k].second][1]);
    }
    std::fflush(stdout);
}

} // namespace rrgame

namespace rrgame {

// DEVELOPMENT: RRJB_ZFIGHT_COVER=1 - where the route runs under the static soup (tunnels, underpasses, overhangs):
// each run of slices with a cell triangle 2..25 world units over the road point (world y points down), with its
// route distance, so `--start D` can put a scripted run there.
void ZFightCoverReport(const rr::TriangleSoup& soup, const std::vector<rr::render::CellRange>& ranges,
                       const std::vector<rr::RoadSlice>& path) {
    const char* env = std::getenv("RRJB_ZFIGHT_COVER");
    if (env == nullptr || *env == '0' || path.empty()) return;
    constexpr float kCell = 16.0f;
    std::map<std::pair<int, int>, std::vector<size_t>> grid; // xz cell -> first soup vertex of each triangle
    for (const rr::render::CellRange& r : ranges)
        for (GLint k = r.first; k + 3 <= r.first + r.count; k += 3) {
            const rr::TriangleSoup::Vertex* v = &soup.vertices[static_cast<size_t>(k)];
            float x0 = 1e30f, x1 = -1e30f, z0 = 1e30f, z1 = -1e30f;
            for (int c = 0; c < 3; ++c) x0 = std::min(x0, v[c].x), x1 = std::max(x1, v[c].x), z0 = std::min(z0, v[c].z), z1 = std::max(z1, v[c].z);
            if (x1 - x0 > 400.0f || z1 - z0 > 400.0f) continue;
            for (int gx = static_cast<int>(std::floor(x0 / kCell)); gx <= static_cast<int>(std::floor(x1 / kCell)); ++gx)
                for (int gz = static_cast<int>(std::floor(z0 / kCell)); gz <= static_cast<int>(std::floor(z1 / kCell)); ++gz)
                    grid[{gx, gz}].push_back(static_cast<size_t>(k));
        }
    const auto over = [&](float px, float py, float pz, size_t& cellOut) {
        const auto it = grid.find({static_cast<int>(std::floor(px / kCell)), static_cast<int>(std::floor(pz / kCell))});
        if (it == grid.end()) return false;
        for (const size_t k : it->second) {
            const rr::TriangleSoup::Vertex* v = &soup.vertices[k];
            const float ax = v[0].x - px, az = v[0].z - pz, bx = v[1].x - px, bz = v[1].z - pz, cx = v[2].x - px, cz = v[2].z - pz;
            const float s0 = ax * bz - az * bx, s1 = bx * cz - bz * cx, s2 = cx * az - cz * ax;
            if (!((s0 >= 0 && s1 >= 0 && s2 >= 0) || (s0 <= 0 && s1 <= 0 && s2 <= 0))) continue;
            const float sum = s0 + s1 + s2;
            if (std::fabs(sum) < 1e-6f) continue;
            const float y = (v[2].y * s0 + v[0].y * s1 + v[1].y * s2) / sum; // barycentric: s0 is opposite v[2]
            const float h = py - y;                                          // y down: above the road = smaller y
            if (h > 2.0f && h < 25.0f) {
                for (const rr::render::CellRange& r : ranges)
                    if (static_cast<GLint>(k) >= r.first && static_cast<GLint>(k) < r.first + r.count) cellOut = r.cell;
                return true;
            }
        }
        return false;
    };
    size_t runs = 0;
    for (size_t i = 0; i < path.size();) {
        size_t cell = 0;
        if (!over(rr::WorldX(path[i]), rr::WorldY(path[i]), rr::WorldZ(path[i]), cell)) {
            ++i;
            continue;
        }
        const size_t from = i;
        size_t lastCell = cell;
        while (i < path.size() && over(rr::WorldX(path[i]), rr::WorldY(path[i]), rr::WorldZ(path[i]), lastCell)) ++i;
        const float d0 = static_cast<float>(path[from].distance) / 65536.0f, d1 = static_cast<float>(path[i - 1].distance) / 65536.0f;
        std::printf("zfight cover: route %.0f..%.0f (%.0f units, slices %zu..%zu) under cells %zu..%zu\n", d0, d1, d1 - d0, from,
                    i - 1, cell, lastCell);
        ++runs;
    }
    std::printf("zfight cover: %zu covered stretches over %zu slices\n", runs, path.size());
    std::fflush(stdout);
}

} // namespace rrgame
