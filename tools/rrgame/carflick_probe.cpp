// DEVELOPMENT: the distant cars' brightness meter. RRJB_CARFLICK=<csv>[,<dir>[,<from>[,<to>]]] re-draws
// view 0 of every frame into a private target of the view's own size (RRJB_CARFLICK_MSAA samples, default 1) twice from
// the same state: A as the frame is drawn, and B with every traffic car in the flat id colour of uDebug 2 (traffic_draw.h
// ProbeIds). A pixel of B that is exactly (slot + 1, 0, 255) is a covered pixel of that car; the csv gets, per frame and
// per drawn car, its distance from the eye, the game's LOD byte +0x08, its +0x24, the pixel count and the mean colour of
// A over those pixels - the car's brightness frame by frame (the column `render` counts the drawn frames: several a game
// frame with the VR smooth motion). RRJB_CARFLICK_SHIFT=<px> draws the pair again with the projection moved that far (a
// sub-pixel move): pixels2 / luma2 the car's mean there, wluma / wluma2 both frames' mean over one window around it,
// shimmer the per-pixel RMS change where it covers both. With <dir>: a 4x crop around each car of game frames from..to.
// Nothing of it runs without the variable; it draws only into its own target and restores the caller's framebuffer.
#include "race_render.h"

#include "render/frame_shot.h"
#include "render/gl_api.h"
#include "render/render_target.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace rrgame {

namespace {

struct CarFlickConfig {
    std::string csv, dir;
    long from = 0, to = -1;
    int samples = 1;
    float shift = 0.0f;
};

const CarFlickConfig& FlickConfig() {
    static const CarFlickConfig config = [] {
        CarFlickConfig c;
        if (const char* e = std::getenv("RRJB_CARFLICK")) {
            std::vector<std::string> parts;
            std::string s = e;
            size_t at = 0;
            while (true) {
                const size_t comma = s.find(',', at);
                parts.push_back(s.substr(at, comma == std::string::npos ? std::string::npos : comma - at));
                if (comma == std::string::npos) break;
                at = comma + 1;
            }
            c.csv = parts[0];
            if (parts.size() > 1) c.dir = parts[1];
            if (parts.size() > 2) c.from = std::atol(parts[2].c_str());
            if (parts.size() > 3) c.to = std::atol(parts[3].c_str());
        }
        if (const char* m = std::getenv("RRJB_CARFLICK_MSAA")) c.samples = std::max(1, std::atoi(m));
        if (const char* m = std::getenv("RRJB_CARFLICK_SHIFT")) c.shift = static_cast<float>(std::atof(m));
        return c;
    }();
    return config;
}

} // namespace

void CarFlickProbe(RaceRenderer& renderer, const GameView& view, long frame) {
    const CarFlickConfig& c = FlickConfig();
    if (c.csv.empty()) return;
    static FILE* out = [&] {
        FILE* f = std::fopen(c.csv.c_str(), "w");
        if (f) std::fprintf(f, "frame,slot,model,dist,lod,flags,pixels,r,g,b,luma,x0,y0,x1,y1,pixels2,luma2,wpixels,wluma,wluma2,render,spixels,shimmer\n");
        return f;
    }();
    if (out == nullptr) return;
    static long renders = 0;
    const long render = renders++; // the drawn frames (several a game frame with the VR smooth motion)
    static rr::render::RenderTarget target;
    const int w = std::min(view.viewport[2], 2048), h = std::min(view.viewport[3], 2048);
    if (w < 16 || h < 16 || !target.Ensure(w, h, c.samples)) return;
    const auto draw = [&](const GameView& v) {
        target.Bind();
        glViewport(0, 0, w, h);
        glDisable(GL_SCISSOR_TEST);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
        renderer.RenderView(v, frame);
        target.Resolve();
        return rr::render::ReadFrame(w, h);
    };
    GameView v = view;
    v.framebuffer = target.DrawFramebuffer();
    v.viewport[0] = v.viewport[1] = 0;
    v.viewport[2] = w;
    v.viewport[3] = h;
    const std::vector<uint8_t> a = draw(v);
    const std::vector<rr::render::TrafficDraw::ProbeCar> cars = rr::render::TrafficDraw::ProbeCars();
    rr::render::TrafficDraw::ProbeIds() = true;
    const std::vector<uint8_t> b = draw(v);
    rr::render::TrafficDraw::ProbeIds() = false;
    // RRJB_CARFLICK_SHIFT=<pixels>: the same frame again with the projection moved that far right and up (a sub-pixel
    // move of everything, as a moving car or head makes it) - a car whose brightness changes under it is aliased
    std::vector<uint8_t> a2, b2;
    if (c.shift != 0.0f) {
        GameView s = v;
        s.proj.m[8] += 2.0f * c.shift / static_cast<float>(w);
        s.proj.m[9] += 2.0f * c.shift / static_cast<float>(h);
        a2 = draw(s);
        rr::render::TrafficDraw::ProbeIds() = true;
        b2 = draw(s);
        rr::render::TrafficDraw::ProbeIds() = false;
    }
    struct Stats {
        size_t n = 0;
        double sum[3] = {0.0, 0.0, 0.0};
        int x0 = 0, y0 = 0, x1 = -1, y1 = -1;
        double Luma() const {
            return n ? (0.299 * sum[0] + 0.587 * sum[1] + 0.114 * sum[2]) / static_cast<double>(n) : 0.0;
        }
    };
    const auto measure = [&](const std::vector<uint8_t>& pa, const std::vector<uint8_t>& pb, uint8_t id) {
        Stats st;
        st.x0 = w, st.y0 = h;
        if (pa.empty()) return st;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const size_t i = 4u * (static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x));
                if (pb[i] != id || pb[i + 1] != 0 || pb[i + 2] != 255) continue;
                if (pa[i] == id && pa[i + 1] == 0 && pa[i + 2] == 255) continue;
                ++st.n;
                for (int k = 0; k < 3; ++k) st.sum[k] += pa[i + static_cast<size_t>(k)];
                st.x0 = std::min(st.x0, x), st.x1 = std::max(st.x1, x), st.y0 = std::min(st.y0, y), st.y1 = std::max(st.y1, y);
            }
        return st;
    };
    for (const auto& car : cars) {
        const uint8_t id = static_cast<uint8_t>(car.slot + 1);
        const Stats st = measure(a, b, id), st2 = measure(a2, b2, id);
        const size_t n = st.n;
        const double* sum = st.sum;
        const int x0 = st.x0, y0 = st.y0, x1 = st.x1, y1 = st.y1;
        const double d = std::sqrt((car.pos[0] - view.eye[0]) * (car.pos[0] - view.eye[0]) +
                                   (car.pos[1] - view.eye[1]) * (car.pos[1] - view.eye[1]) +
                                   (car.pos[2] - view.eye[2]) * (car.pos[2] - view.eye[2]));
        const double r = n ? sum[0] / static_cast<double>(n) : 0.0, g = n ? sum[1] / static_cast<double>(n) : 0.0,
                     bl = n ? sum[2] / static_cast<double>(n) : 0.0;
        // the window: both frames' boxes of the car, 2 pixels wider - the mean luma of each frame over the SAME pixels
        // (an image that is filtered well keeps it under a sub-pixel move; the edge pixels and what lies around count too)
        double wl = 0.0, wl2 = 0.0;
        size_t wn = 0;
        if (n > 0 && st2.n > 0) {
            const int wx0 = std::max(0, std::min(x0, st2.x0) - 2), wx1 = std::min(w - 1, std::max(x1, st2.x1) + 2);
            const int wy0 = std::max(0, std::min(y0, st2.y0) - 2), wy1 = std::min(h - 1, std::max(y1, st2.y1) + 2);
            for (int y = wy0; y <= wy1; ++y)
                for (int x = wx0; x <= wx1; ++x) {
                    const size_t i = 4u * (static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x));
                    wl += 0.299 * a[i] + 0.587 * a[i + 1] + 0.114 * a[i + 2];
                    wl2 += 0.299 * a2[i] + 0.587 * a2[i + 1] + 0.114 * a2[i + 2];
                    ++wn;
                }
            wl /= static_cast<double>(wn), wl2 /= static_cast<double>(wn);
        }
        // per pixel: the RMS luma change between the two over the pixels that are the car's in both (the shimmer)
        double prms = 0.0;
        size_t pn = 0;
        if (!a2.empty())
            for (size_t i = 0; i + 3 < a.size(); i += 4) {
                if (b[i] != id || b[i + 1] != 0 || b[i + 2] != 255 || b2[i] != id || b2[i + 1] != 0 || b2[i + 2] != 255) continue;
                const double dl = (0.299 * a[i] + 0.587 * a[i + 1] + 0.114 * a[i + 2]) - (0.299 * a2[i] + 0.587 * a2[i + 1] + 0.114 * a2[i + 2]);
                prms += dl * dl;
                ++pn;
            }
        if (pn) prms = std::sqrt(prms / static_cast<double>(pn));
        std::fprintf(out, "%ld,%d,%u,%.3f,%d,0x%08X,%zu,%.3f,%.3f,%.3f,%.3f,%d,%d,%d,%d,%zu,%.3f,%zu,%.3f,%.3f,%ld,%zu,%.3f\n", frame, car.slot,
                     car.model, d, car.lodByte, car.flags, n, r, g, bl, 0.299 * r + 0.587 * g + 0.114 * bl, x0, h - 1 - y1, x1,
                     h - 1 - y0, st2.n, st2.Luma(), wn, wl, wl2, render, pn, prms);
        if (!c.dir.empty() && n > 0 && frame >= c.from && (c.to < 0 || frame <= c.to)) {
            constexpr int kCrop = 48, kScale = 4;
            const int cx = (x0 + x1) / 2, cy = (y0 + y1) / 2;
            std::vector<uint8_t> crop(static_cast<size_t>(kCrop * kScale) * (kCrop * kScale) * 4u, 0);
            for (int y = 0; y < kCrop * kScale; ++y)
                for (int x = 0; x < kCrop * kScale; ++x) {
                    const int sx = cx - kCrop / 2 + x / kScale, sy = cy - kCrop / 2 + y / kScale;
                    if (sx < 0 || sy < 0 || sx >= w || sy >= h) continue;
                    const size_t si = 4u * (static_cast<size_t>(sy) * static_cast<size_t>(w) + static_cast<size_t>(sx));
                    const size_t di = 4u * (static_cast<size_t>(y) * static_cast<size_t>(kCrop * kScale) + static_cast<size_t>(x));
                    for (int k = 0; k < 4; ++k) crop[di + static_cast<size_t>(k)] = a[si + static_cast<size_t>(k)];
                }
            char name[96];
            std::snprintf(name, sizeof name, "/car%02d_%06ld.png", car.slot, render);
            rr::render::SaveShot(c.dir + name, kCrop * kScale, kCrop * kScale, crop);
        }
    }
    std::fflush(out);
    rr::render::BindFramebuffer(view.framebuffer);
    glViewport(view.viewport[0], view.viewport[1], view.viewport[2], view.viewport[3]);
}

} // namespace rrgame
