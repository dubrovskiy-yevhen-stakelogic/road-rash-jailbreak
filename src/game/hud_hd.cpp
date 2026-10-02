// The race HUD in HD - see hud_hd.h. The draw-state handling, the texel fetch, the modulation and the blend rules are
// hud_view.cpp's Raster, read at a finer grid; only where a pixel's texel comes from differs.
#include "game/hud_hd.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace rr::game {
namespace {

int32_t Sext11(uint32_t v) { return static_cast<int32_t>(v << 21) >> 21; }
float Channel5(uint32_t c5) { return static_cast<float>((c5 << 3) | (c5 >> 2)) / 255.0f; }

class HdRaster {
public:
    HdRaster(const HudVram& v, const std::vector<HudHd::Region>& regions, std::vector<uint8_t>& out, int scale,
             HudRasterStats& s, int32_t originX, int32_t originY, uint64_t& contourPixels)
        : vram_(v), regions_(regions), out_(out), s_(scale), W_(HudOverlay::kWidth * scale), H_(HudOverlay::kHeight * scale),
          stats_(s), ox_(originX), oy_(originY), contour_(contourPixels) {
        offX_ = originX;
        offY_ = originY;
        areaX0_ = originX;
        areaY0_ = originY;
        areaX1_ = originX + HudOverlay::kWidth - 1;
        areaY1_ = originY + HudOverlay::kHeight - 1;
    }

    void Packet(const std::vector<uint32_t>& w) {
        if (w.empty()) return;
        const uint32_t cmd = w[0] >> 24;
        if (cmd >= 0x20 && cmd < 0x40) return Polygon(w, cmd);
        if (cmd >= 0x40 && cmd < 0x60) return Line(w, cmd);
        if (cmd >= 0x60 && cmd < 0x80) return Rectangle(w, cmd);
        switch (cmd) {
        case 0x00: return;
        case 0xE1: tpage_ = w[0] & 0x1FFu; ++stats_.modes; return;
        case 0xE2: texWindow_ = w[0] & 0xFFFFFu; ++stats_.modes; return;
        case 0xE3:
            areaX0_ = static_cast<int32_t>(w[0] & 0x3FFu);
            areaY0_ = static_cast<int32_t>((w[0] >> 10) & 0x3FFu);
            ++stats_.areas;
            return;
        case 0xE4:
            areaX1_ = static_cast<int32_t>(w[0] & 0x3FFu);
            areaY1_ = static_cast<int32_t>((w[0] >> 10) & 0x3FFu);
            ++stats_.areas;
            return;
        case 0xE5:
            offX_ = Sext11(w[0] & 0x7FFu);
            offY_ = Sext11((w[0] >> 11) & 0x7FFu);
            ++stats_.areas;
            return;
        case 0xE6: ++stats_.modes; return;
        default: ++stats_.unknown; return;
        }
    }

private:
    uint16_t Texel(uint32_t tpage, uint32_t clut, int32_t u, int32_t v) const { // hud_view.cpp Raster::Texel
        u &= 0xFF;
        v &= 0xFF;
        const int32_t mx = static_cast<int32_t>(texWindow_ & 0x1Fu) * 8, my = static_cast<int32_t>((texWindow_ >> 5) & 0x1Fu) * 8;
        const int32_t ox = static_cast<int32_t>((texWindow_ >> 10) & 0x1Fu) * 8, oy = static_cast<int32_t>((texWindow_ >> 15) & 0x1Fu) * 8;
        u = (u & ~mx) | (ox & mx);
        v = (v & ~my) | (oy & my);
        const int32_t bx = static_cast<int32_t>(tpage & 0xFu) * 64, by = static_cast<int32_t>((tpage >> 4) & 1u) * 256;
        const uint32_t depth = (tpage >> 7) & 3u;
        const int32_t cx = static_cast<int32_t>(clut & 0x3Fu) * 16, cy = static_cast<int32_t>((clut >> 6) & 0x1FFu);
        if (depth == 0) {
            const uint16_t hw = vram_.At(bx + u / 4, by + v);
            return vram_.At(cx + ((hw >> ((u & 3) * 4)) & 0xF), cy);
        }
        if (depth == 1) {
            const uint16_t hw = vram_.At(bx + u / 2, by + v);
            return vram_.At(cx + ((hw >> ((u & 1) * 8)) & 0xFF), cy);
        }
        return vram_.At(bx + u, by + v);
    }
    uint16_t ClutColour(uint32_t clut, uint32_t index) const {
        return vram_.At(static_cast<int>(clut & 0x3Fu) * 16 + static_cast<int>(index), static_cast<int>((clut >> 6) & 0x1FFu));
    }
    static uint16_t Modulate(uint16_t texel, uint32_t rgb, bool raw) {
        if (raw) return texel;
        uint32_t out = texel & 0x8000u;
        for (int k = 0; k < 3; ++k) {
            const uint32_t t = (texel >> (5 * k)) & 31u;
            const uint32_t c = (rgb >> (8 * k)) & 0xFFu;
            out |= std::min<uint32_t>(31u, (t * c) >> 7) << (5 * k);
        }
        return static_cast<uint16_t>(out);
    }
    static uint16_t Rgb15(uint32_t rgb) {
        return static_cast<uint16_t>(((rgb >> 3) & 31u) | (((rgb >> 11) & 31u) << 5) | (((rgb >> 19) & 31u) << 10));
    }
    // A pack region for a 4-bit primitive of the HUD page (tpage 15) covering u0..u1 x v0..v1 with `clut`.
    const HudHd::Region* Find(uint32_t tpage, uint32_t clut, int u0, int v0, int u1, int v1) const {
        if ((tpage & 0x1Fu) != 15u || ((tpage >> 7) & 3u) != 0 || texWindow_ != 0) return nullptr;
        for (const HudHd::Region& r : regions_)
            if (r.clut == clut && u0 >= r.u && v0 >= r.v && u1 <= r.u + r.w && v1 <= r.v + r.h) return &r;
        return nullptr;
    }
    // The blend of hud_view.cpp Raster::Plot on premultiplied floats.
    static void Apply(float* p, uint16_t c15, bool semi, uint32_t mode) {
        const float f[3] = {Channel5(c15 & 31u), Channel5((c15 >> 5) & 31u), Channel5((c15 >> 10) & 31u)};
        if (!semi) {
            for (int k = 0; k < 3; ++k) p[k] = f[k];
            p[3] = 1.0f;
            return;
        }
        switch (mode) {
        case 0:
            for (int k = 0; k < 3; ++k) p[k] = 0.5f * f[k] + 0.5f * p[k];
            p[3] = 0.5f + 0.5f * p[3];
            break;
        case 1:
            for (int k = 0; k < 3; ++k) p[k] = std::min(1.0f, p[k] + f[k]);
            break;
        case 2:
            for (int k = 0; k < 3; ++k) p[k] = std::max(0.0f, p[k] - f[k]);
            break;
        default:
            for (int k = 0; k < 3; ++k) p[k] = std::min(1.0f, p[k] + 0.25f * f[k]);
            break;
        }
    }
    // HD pixel (X, Y) of the overlay, inside the draw area (VRAM coordinates of its base pixel)?
    uint8_t* Pixel(int X, int Y) {
        if (X < 0 || Y < 0 || X >= W_ || Y >= H_) return nullptr;
        const int bx = ox_ + X / s_, by = oy_ + Y / s_;
        if (bx < areaX0_ || bx > areaX1_ || by < areaY0_ || by > areaY1_) return nullptr;
        return &out_[(static_cast<size_t>(Y) * static_cast<size_t>(W_) + static_cast<size_t>(X)) * 4u];
    }
    static void Load(const uint8_t* d, float* p) {
        for (int k = 0; k < 4; ++k) p[k] = static_cast<float>(d[k]) / 255.0f;
    }
    static void Store(uint8_t* d, const float* p) {
        for (int k = 0; k < 4; ++k) d[k] = static_cast<uint8_t>(std::clamp(static_cast<int>(std::lround(p[k] * 255.0f)), 0, 255));
    }
    void PlotFlat(int X, int Y, uint16_t c15, bool semi, uint32_t mode) {
        uint8_t* d = Pixel(X, Y);
        if (d == nullptr) return;
        if (semi && mode == 2) ++stats_.subtractive;
        float p[4];
        Load(d, p);
        Apply(p, c15, semi, mode);
        Store(d, p);
    }
    // A textured HD pixel: the region's contour texel (u4, v4 in 4x texels of the page) through the live CLUT, or the
    // original texel (u, v) enlarged.
    void PlotTextured(int X, int Y, const HudHd::Region* r, int u4, int v4, int u, int v, uint32_t tpage, uint32_t clut,
                      uint32_t rgb, bool raw, bool semi, uint32_t mode) {
        if (r == nullptr || u4 < r->u * 4 || v4 < r->v * 4 || u4 >= (r->u + r->w) * 4 || v4 >= (r->v + r->h) * 4) {
            const uint16_t t = Texel(tpage, clut, u, v);
            if (t != 0) PlotFlat(X, Y, Modulate(t, rgb, raw), semi && (t & 0x8000u), mode);
            return;
        }
        const uint8_t* c = &r->atlas->abw[(static_cast<size_t>(v4 - r->v * 4) * static_cast<size_t>(r->atlas->width) +
                                           static_cast<size_t>(u4 - r->u * 4)) * 3u];
        const uint16_t ta = ClutColour(clut, c[0]), tb = ClutColour(clut, c[1]);
        const float w = static_cast<float>(c[2]) / 255.0f;
        if (ta == 0 && (c[2] == 0 || tb == 0)) return;
        uint8_t* d = Pixel(X, Y);
        if (d == nullptr) return;
        float p[4], pa[4], pb[4];
        Load(d, p);
        std::memcpy(pa, p, sizeof(p));
        std::memcpy(pb, p, sizeof(p));
        if (ta != 0) Apply(pa, Modulate(ta, rgb, raw), semi && (ta & 0x8000u), mode);
        if (c[2] != 0 && tb != 0) Apply(pb, Modulate(tb, rgb, raw), semi && (tb & 0x8000u), mode);
        for (int k = 0; k < 4; ++k) p[k] = pa[k] * (1.0f - w) + pb[k] * w;
        Store(d, p);
        ++contour_;
    }

    void Rectangle(const std::vector<uint32_t>& w, uint32_t cmd) {
        const bool textured = (cmd & 4u) != 0, semi = (cmd & 2u) != 0, raw = (cmd & 1u) != 0;
        size_t k = 1;
        if (k >= w.size()) return;
        const int32_t x0 = Sext11(w[k] & 0x7FFu) + offX_, y0 = Sext11((w[k] >> 16) & 0x7FFu) + offY_;
        ++k;
        uint32_t uv = 0, clut = 0;
        if (textured) {
            if (k >= w.size()) return;
            uv = w[k] & 0xFFFFu;
            clut = w[k] >> 16;
            ++k;
        }
        int32_t wdt = 0, hgt = 0;
        switch ((cmd >> 3) & 3u) {
        case 0:
            if (k >= w.size()) return;
            wdt = static_cast<int32_t>(w[k] & 0x3FFu);
            hgt = static_cast<int32_t>((w[k] >> 16) & 0x1FFu);
            break;
        case 1: wdt = hgt = 1; break;
        case 2: wdt = hgt = 8; break;
        default: wdt = hgt = 16; break;
        }
        const uint32_t rgb = w[0] & 0xFFFFFFu;
        const uint32_t semiMode = (tpage_ >> 5) & 3u;
        const int u0 = static_cast<int>(uv & 0xFFu), v0 = static_cast<int>(uv >> 8);
        const HudHd::Region* region = textured ? Find(tpage_, clut, u0, v0, u0 + wdt, v0 + hgt) : nullptr;
        if (textured) ++stats_.sprites;
        else ++stats_.tiles;
        const int step = rr::hd::kScale / s_;
        for (int32_t dy = 0; dy < hgt; ++dy)
            for (int32_t dx = 0; dx < wdt; ++dx)
                for (int j = 0; j < s_; ++j)
                    for (int i = 0; i < s_; ++i) {
                        const int X = (x0 + dx - ox_) * s_ + i, Y = (y0 + dy - oy_) * s_ + j;
                        if (!textured) {
                            PlotFlat(X, Y, Rgb15(rgb), semi, semiMode);
                            continue;
                        }
                        const int u = u0 + dx, v = v0 + dy;
                        PlotTextured(X, Y, region, u * 4 + i * step + step / 2, v * 4 + j * step + step / 2, u, v, tpage_, clut,
                                     rgb, raw, semi, semiMode);
                    }
    }

    struct Vertex {
        int32_t x = 0, y = 0; // HD pixels of the overlay
        int32_t u = 0, v = 0;
        uint32_t rgb = 0;
    };

    void Polygon(const std::vector<uint32_t>& w, uint32_t cmd) {
        const bool gouraud = (cmd & 0x10u) != 0, quad = (cmd & 8u) != 0, textured = (cmd & 4u) != 0;
        const bool semi = (cmd & 2u) != 0, raw = (cmd & 1u) != 0;
        const int n = quad ? 4 : 3;
        Vertex vx[4];
        size_t k = 0;
        uint32_t clut = 0, tpage = tpage_;
        for (int i = 0; i < n; ++i) {
            if (i == 0 || gouraud) {
                if (k >= w.size()) return;
                vx[i].rgb = w[k++] & 0xFFFFFFu;
            } else {
                vx[i].rgb = vx[0].rgb;
            }
            if (k >= w.size()) return;
            vx[i].x = (Sext11(w[k] & 0x7FFu) + offX_ - ox_) * s_;
            vx[i].y = (Sext11((w[k] >> 16) & 0x7FFu) + offY_ - oy_) * s_;
            ++k;
            if (textured) {
                if (k >= w.size()) return;
                vx[i].u = static_cast<int32_t>(w[k] & 0xFFu);
                vx[i].v = static_cast<int32_t>((w[k] >> 8) & 0xFFu);
                if (i == 0) clut = w[k] >> 16;
                if (i == 1) {
                    tpage = (w[k] >> 16) & 0x1FFu;
                    tpage_ = (tpage_ & ~0x1FFu) | tpage;
                }
                ++k;
            }
        }
        ++stats_.polygons;
        const uint32_t semiMode = (tpage >> 5) & 3u;
        const HudHd::Region* region = nullptr;
        if (textured) {
            int umin = 255, vmin = 255, umax = 0, vmax = 0;
            for (int i = 0; i < n; ++i) {
                umin = std::min(umin, vx[i].u), umax = std::max(umax, vx[i].u);
                vmin = std::min(vmin, vx[i].v), vmax = std::max(vmax, vx[i].v);
            }
            region = Find(tpage, clut, umin, vmin, umax + 1, vmax + 1);
        }
        auto triangle = [&](const Vertex& a, const Vertex& b, const Vertex& c) {
            const int64_t area = static_cast<int64_t>(b.x - a.x) * (c.y - a.y) - static_cast<int64_t>(c.x - a.x) * (b.y - a.y);
            if (area == 0) return;
            const int32_t minX = std::max({std::min({a.x, b.x, c.x}), 0});
            const int32_t maxX = std::min({std::max({a.x, b.x, c.x}), W_});
            const int32_t minY = std::max({std::min({a.y, b.y, c.y}), 0});
            const int32_t maxY = std::min({std::max({a.y, b.y, c.y}), H_});
            const Vertex* v[3] = {&a, &b, &c};
            auto edge = [&](const Vertex& p, const Vertex& q, int32_t x, int32_t y) {
                return static_cast<int64_t>(q.x - p.x) * (y - p.y) - static_cast<int64_t>(q.y - p.y) * (x - p.x);
            };
            const bool ccw = area > 0;
            for (int32_t y = minY; y < maxY; ++y)
                for (int32_t x = minX; x < maxX; ++x) {
                    int64_t e[3];
                    bool inside = true;
                    for (int j = 0; j < 3 && inside; ++j) {
                        const Vertex& p = *v[j];
                        const Vertex& q = *v[(j + 1) % 3];
                        e[j] = ccw ? edge(p, q, x, y) : -edge(p, q, x, y);
                        if (e[j] < 0) inside = false;
                        else if (e[j] == 0) {
                            const int32_t dy = ccw ? (q.y - p.y) : (p.y - q.y);
                            const int32_t dx = ccw ? (q.x - p.x) : (p.x - q.x);
                            if (!((dy == 0 && dx > 0) || dy < 0)) inside = false;
                        }
                    }
                    if (!inside) continue;
                    const double A = static_cast<double>(ccw ? area : -area);
                    const double wa = static_cast<double>(e[1]) / A, wb = static_cast<double>(e[2]) / A, wc = static_cast<double>(e[0]) / A;
                    uint32_t rgb = a.rgb;
                    if (gouraud) {
                        rgb = 0;
                        for (int ch = 0; ch < 3; ++ch) {
                            const double mix = wa * ((a.rgb >> (8 * ch)) & 0xFFu) + wb * ((b.rgb >> (8 * ch)) & 0xFFu) +
                                               wc * ((c.rgb >> (8 * ch)) & 0xFFu);
                            rgb |= static_cast<uint32_t>(std::clamp(static_cast<int>(mix), 0, 255)) << (8 * ch);
                        }
                    }
                    if (!textured) {
                        PlotFlat(x, y, Rgb15(rgb), semi, semiMode);
                        continue;
                    }
                    const double uf = wa * a.u + wb * b.u + wc * c.u, vf = wa * a.v + wb * b.v + wc * c.v;
                    PlotTextured(x, y, region, static_cast<int>(std::floor(uf * 4.0 + 1e-6)), static_cast<int>(std::floor(vf * 4.0 + 1e-6)),
                                 static_cast<int32_t>(std::floor(uf + 1e-6)), static_cast<int32_t>(std::floor(vf + 1e-6)), tpage, clut,
                                 rgb, raw, semi, semiMode);
                }
        };
        triangle(vx[0], vx[1], vx[2]);
        if (quad) triangle(vx[1], vx[2], vx[3]);
    }

    void Line(const std::vector<uint32_t>& w, uint32_t cmd) {
        const bool gouraud = (cmd & 0x10u) != 0, poly = (cmd & 8u) != 0, semi = (cmd & 2u) != 0;
        if (poly) {
            ++stats_.unknown;
            return;
        }
        if (w.size() < (gouraud ? 4u : 3u)) return;
        const uint32_t rgb = w[0] & 0xFFFFFFu;
        const int32_t x0 = Sext11(w[1] & 0x7FFu) + offX_, y0 = Sext11((w[1] >> 16) & 0x7FFu) + offY_;
        const uint32_t p1 = gouraud ? w[3] : w[2];
        const int32_t x1 = Sext11(p1 & 0x7FFu) + offX_, y1 = Sext11((p1 >> 16) & 0x7FFu) + offY_;
        ++stats_.lines;
        const int32_t steps = std::max(std::abs(x1 - x0), std::abs(y1 - y0));
        for (int32_t s = 0; s <= steps; ++s) {
            const int32_t x = steps ? x0 + static_cast<int32_t>(std::lround(static_cast<double>(x1 - x0) * s / steps)) : x0;
            const int32_t y = steps ? y0 + static_cast<int32_t>(std::lround(static_cast<double>(y1 - y0) * s / steps)) : y0;
            for (int j = 0; j < s_; ++j)
                for (int i = 0; i < s_; ++i) PlotFlat((x - ox_) * s_ + i, (y - oy_) * s_ + j, Rgb15(rgb), semi, (tpage_ >> 5) & 3u);
        }
    }

    const HudVram& vram_;
    const std::vector<HudHd::Region>& regions_;
    std::vector<uint8_t>& out_;
    int s_, W_, H_;
    HudRasterStats& stats_;
    int32_t ox_, oy_;
    uint64_t& contour_;
    uint32_t tpage_ = 0, texWindow_ = 0;
    int32_t areaX0_ = 0, areaY0_ = 0, areaX1_ = 0, areaY1_ = 0, offX_ = 0, offY_ = 0;
};

} // namespace

bool HudHd::Prepare(const HudVram& vram) {
    const uint64_t page = rr::hd::Fnv64(vram.page.data(), vram.page.size() * sizeof(uint16_t));
    if (generation_ == rr::hd::Generation() && page == pageHash_) return !regions_.empty();
    generation_ = rr::hd::Generation();
    pageHash_ = page;
    regions_.clear();
    offered_ = 0;
    rr::hd::Pack* pack = rr::hd::Active();
    if (pack == nullptr) return false;
    for (const rr::hd::Entry& e : pack->Entries()) {
        if (e.kind != "hud") continue;
        ++offered_;
        if (rr::hd::SourceHashHudRegion(vram.page.data(), e.a, e.b, e.c, e.d, static_cast<uint32_t>(e.e)) != e.hash) continue;
        Region r;
        r.u = e.a, r.v = e.b, r.w = e.c, r.h = e.d;
        r.clut = static_cast<uint16_t>(e.e);
        for (const auto& l : loaded_)
            if (l.first == e.file) r.atlas = l.second;
        if (!r.atlas) {
            rr::hd::Contour c;
            if (!pack->HudRegion(e, c)) continue;
            r.atlas = std::make_shared<rr::hd::Contour>(std::move(c));
            loaded_.push_back({e.file, r.atlas});
        }
        regions_.push_back(r);
    }
    return !regions_.empty();
}

HudRasterStats HudHd::Rasterize(const std::vector<HudPacket>& packets, const HudVram& vram, int32_t originX, int32_t originY,
                                int scale, std::vector<uint8_t>& out) {
    const size_t bytes = static_cast<size_t>(HudOverlay::kWidth) * static_cast<size_t>(HudOverlay::kHeight) * 16u *
                         static_cast<size_t>(scale * scale) / 4u;
    out.assign(bytes, 0);
    ++frames_;
    HudRasterStats stats;
    HdRaster r(vram, regions_, out, scale, stats, originX, originY, contourPixels_);
    for (const HudPacket& p : packets) r.Packet(p.words);
    return stats;
}

std::string HudHd::Report() const {
    char b[240];
    std::snprintf(b, sizeof(b), "hud HD: %zu of %zu pack regions match this HUD page; %llu frames drawn HD, %llu pixels from contours",
                  regions_.size(), offered_, static_cast<unsigned long long>(frames_),
                  static_cast<unsigned long long>(contourPixels_));
    return b;
}

HudHd::~HudHd() {
    if (frames_ != 0) std::printf("%s\n", Report().c_str());
}

} // namespace rr::game
