#include "game/hud_view.h"

#include "game/sim/hud.h"
#include "game/sim/race.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <sstream>

namespace rr::game {
namespace {

uint32_t RamWord(const uint8_t* ram, uint32_t a) {
    const uint32_t o = a & 0x1FFFFCu;
    return static_cast<uint32_t>(ram[o]) | (static_cast<uint32_t>(ram[o + 1]) << 8) |
           (static_cast<uint32_t>(ram[o + 2]) << 16) | (static_cast<uint32_t>(ram[o + 3]) << 24);
}

int32_t Sext11(uint32_t v) { return static_cast<int32_t>(v << 21) >> 21; }

float Channel5(uint32_t c5) { return static_cast<float>((c5 << 3) | (c5 >> 2)) / 255.0f; }

// The GPU's draw state the HUD's packets change.
struct DrawState {
    uint32_t tpage = 0;      // E1 bits 0..8 (page x, page y, semi mode, depth)
    uint32_t texWindow = 0;  // E2
    int32_t areaX0 = 0, areaY0 = 0, areaX1 = HudOverlay::kWidth - 1, areaY1 = HudOverlay::kHeight - 1;
    int32_t offX = 0, offY = 0;
};

class Raster {
public:
    // `originX/Y`: the VRAM position of the overlay's top-left pixel - the frame's drawing offset. The
    // packets' vertices are relative to the drawing offset (E5), draw-area words (E3/E4) are absolute.
    Raster(const HudVram& v, HudOverlay& o, HudRasterStats& s, int32_t originX, int32_t originY)
        : vram_(v), out_(o), stats_(s), ox_(originX), oy_(originY) {
        st_.offX = originX;
        st_.offY = originY;
        st_.areaX0 = originX;
        st_.areaY0 = originY;
        st_.areaX1 = originX + HudOverlay::kWidth - 1;
        st_.areaY1 = originY + HudOverlay::kHeight - 1;
    }

    void Packet(const std::vector<uint32_t>& w) {
        if (w.empty()) return;
        const uint32_t cmd = w[0] >> 24;
        if (cmd >= 0x20 && cmd < 0x40) return Polygon(w, cmd);
        if (cmd >= 0x40 && cmd < 0x60) return Line(w, cmd);
        if (cmd >= 0x60 && cmd < 0x80) return Rectangle(w, cmd);
        switch (cmd) {
        case 0x00: return;
        case 0xE1:
            st_.tpage = w[0] & 0x1FFu;
            ++stats_.modes;
            return;
        case 0xE2:
            st_.texWindow = w[0] & 0xFFFFFu;
            ++stats_.modes;
            return;
        case 0xE3:
            st_.areaX0 = static_cast<int32_t>(w[0] & 0x3FFu);
            st_.areaY0 = static_cast<int32_t>((w[0] >> 10) & 0x3FFu);
            ++stats_.areas;
            return;
        case 0xE4:
            st_.areaX1 = static_cast<int32_t>(w[0] & 0x3FFu);
            st_.areaY1 = static_cast<int32_t>((w[0] >> 10) & 0x3FFu);
            ++stats_.areas;
            return;
        case 0xE5:
            st_.offX = Sext11(w[0] & 0x7FFu);
            st_.offY = Sext11((w[0] >> 11) & 0x7FFu);
            ++stats_.areas;
            return;
        case 0xE6:
            ++stats_.modes;
            return;
        default:
            ++stats_.unknown;
            return;
        }
    }

private:
    uint16_t Texel(uint32_t tpage, uint32_t clut, int32_t u, int32_t v) const {
        u &= 0xFF;
        v &= 0xFF;
        // E2 texture window
        const int32_t mx = static_cast<int32_t>(st_.texWindow & 0x1Fu) * 8, my = static_cast<int32_t>((st_.texWindow >> 5) & 0x1Fu) * 8;
        const int32_t ox = static_cast<int32_t>((st_.texWindow >> 10) & 0x1Fu) * 8, oy = static_cast<int32_t>((st_.texWindow >> 15) & 0x1Fu) * 8;
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

    // One pixel: `c15` a 15-bit colour, `semi` whether this pixel blends (mode from the tpage).
    void Plot(int32_t x, int32_t y, uint16_t c15, bool semi, uint32_t semiMode) {
        if (x < st_.areaX0 || x > st_.areaX1 || y < st_.areaY0 || y > st_.areaY1) return;
        x -= ox_;
        y -= oy_;
        if (x < 0 || y < 0 || x >= HudOverlay::kWidth || y >= HudOverlay::kHeight) return;
        const size_t i = static_cast<size_t>(y) * HudOverlay::kWidth + static_cast<size_t>(x);
        float* p = &out_.rgba[i * 4];
        const float f[3] = {Channel5(c15 & 31u), Channel5((c15 >> 5) & 31u), Channel5((c15 >> 10) & 31u)};
        if (!semi) {
            for (int k = 0; k < 3; ++k) p[k] = f[k];
            p[3] = 1.0f;
            out_.opaque[i] = 1;
            out_.opaqueColour[i] = static_cast<uint16_t>(c15 & 0x7FFFu);
            return;
        }
        out_.opaque[i] = 0;
        switch (semiMode) {
        case 0:
            for (int k = 0; k < 3; ++k) p[k] = 0.5f * f[k] + 0.5f * p[k];
            p[3] = 0.5f + 0.5f * p[3];
            break;
        case 1:
            for (int k = 0; k < 3; ++k) p[k] = std::min(1.0f, p[k] + f[k]);
            break;
        case 2:
            for (int k = 0; k < 3; ++k) p[k] = std::max(0.0f, p[k] - f[k]);
            ++stats_.subtractive;
            break;
        default:
            for (int k = 0; k < 3; ++k) p[k] = std::min(1.0f, p[k] + 0.25f * f[k]);
            break;
        }
    }

    // The colour a textured pixel gets: raw, or modulated by the packet colour (128 = 1.0).
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

    void Rectangle(const std::vector<uint32_t>& w, uint32_t cmd) {
        const bool textured = (cmd & 4u) != 0, semi = (cmd & 2u) != 0, raw = (cmd & 1u) != 0;
        size_t k = 1;
        if (k >= w.size()) return;
        const int32_t x0 = Sext11(w[k] & 0x7FFu) + st_.offX, y0 = Sext11((w[k] >> 16) & 0x7FFu) + st_.offY;
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
        const uint32_t semiMode = (st_.tpage >> 5) & 3u;
        if (textured) ++stats_.sprites;
        else ++stats_.tiles;
        for (int32_t dy = 0; dy < hgt; ++dy)
            for (int32_t dx = 0; dx < wdt; ++dx) {
                if (!textured) {
                    Plot(x0 + dx, y0 + dy, Rgb15(rgb), semi, semiMode);
                    continue;
                }
                const uint16_t t = Texel(st_.tpage, clut, static_cast<int32_t>(uv & 0xFFu) + dx,
                                         static_cast<int32_t>(uv >> 8) + dy);
                if (t == 0) continue;
                Plot(x0 + dx, y0 + dy, Modulate(t, rgb, raw), semi && (t & 0x8000u), semiMode);
            }
    }

    struct Vertex {
        int32_t x = 0, y = 0;
        int32_t u = 0, v = 0;
        uint32_t rgb = 0;
    };

    void Polygon(const std::vector<uint32_t>& w, uint32_t cmd) {
        const bool gouraud = (cmd & 0x10u) != 0, quad = (cmd & 8u) != 0, textured = (cmd & 4u) != 0;
        const bool semi = (cmd & 2u) != 0, raw = (cmd & 1u) != 0;
        const int n = quad ? 4 : 3;
        Vertex vx[4];
        size_t k = 0;
        uint32_t clut = 0, tpage = st_.tpage;
        for (int i = 0; i < n; ++i) {
            if (i == 0 || gouraud) {
                if (k >= w.size()) return;
                vx[i].rgb = w[k++] & 0xFFFFFFu;
            } else {
                vx[i].rgb = vx[0].rgb;
            }
            if (k >= w.size()) return;
            vx[i].x = Sext11(w[k] & 0x7FFu) + st_.offX;
            vx[i].y = Sext11((w[k] >> 16) & 0x7FFu) + st_.offY;
            ++k;
            if (textured) {
                if (k >= w.size()) return;
                vx[i].u = static_cast<int32_t>(w[k] & 0xFFu);
                vx[i].v = static_cast<int32_t>((w[k] >> 8) & 0xFFu);
                if (i == 0) clut = w[k] >> 16;
                if (i == 1) {
                    // A textured polygon carries its own page: it replaces the draw mode's.
                    tpage = (w[k] >> 16) & 0x1FFu;
                    st_.tpage = (st_.tpage & ~0x1FFu) | tpage;
                }
                ++k;
            }
        }
        ++stats_.polygons;
        const uint32_t semiMode = (tpage >> 5) & 3u;
        auto triangle = [&](const Vertex& a, const Vertex& b, const Vertex& c) {
            const int64_t area = static_cast<int64_t>(b.x - a.x) * (c.y - a.y) - static_cast<int64_t>(c.x - a.x) * (b.y - a.y);
            if (area == 0) return;
            const int32_t minX = std::max({std::min({a.x, b.x, c.x}), ox_});
            const int32_t maxX = std::min({std::max({a.x, b.x, c.x}), ox_ + HudOverlay::kWidth});
            const int32_t minY = std::max({std::min({a.y, b.y, c.y}), oy_});
            const int32_t maxY = std::min({std::max({a.y, b.y, c.y}), oy_ + HudOverlay::kHeight});
            const Vertex* v[3] = {&a, &b, &c};
            // Edge functions evaluated at integer pixel positions; the top-left rule leaves out the
            // right and bottom edges, so an axis-aligned quad covers [x0, x1) x [y0, y1) (RRJB_EDGE=gl: the
            // mirrored rule, which keeps the bottom and right edges - the negative control).
            static const bool mirrored = std::getenv("RRJB_EDGE") != nullptr && std::strcmp(std::getenv("RRJB_EDGE"), "gl") == 0;
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
                            // top-left: keep a top edge (horizontal, going the interior's way) or a left edge
                            const int32_t dy = ccw ? (q.y - p.y) : (p.y - q.y);
                            const int32_t dx = ccw ? (q.x - p.x) : (p.x - q.x);
                            // ccw (area > 0) is clockwise on the y-down screen: the interior lies right of p -> q, so
                            // a top edge runs to +x and a left edge runs up
                            const bool topLeft = mirrored ? ((dy == 0 && dx < 0) || dy > 0) : ((dy == 0 && dx > 0) || dy < 0);
                            if (!topLeft) inside = false;
                        }
                    }
                    if (!inside) continue;
                    // barycentric weights of a (e[1]), b (e[2]), c (e[0]) over |area|
                    const double A = static_cast<double>(ccw ? area : -area);
                    const double wa = static_cast<double>(e[1]) / A, wb = static_cast<double>(e[2]) / A,
                                 wc = static_cast<double>(e[0]) / A;
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
                        Plot(x, y, Rgb15(rgb), semi, semiMode);
                        continue;
                    }
                    const int32_t u = static_cast<int32_t>(std::floor(wa * a.u + wb * b.u + wc * c.u + 1e-6));
                    const int32_t vv = static_cast<int32_t>(std::floor(wa * a.v + wb * b.v + wc * c.v + 1e-6));
                    const uint16_t t = Texel(tpage, clut, u, vv);
                    if (t == 0) continue;
                    Plot(x, y, Modulate(t, rgb, raw), semi && (t & 0x8000u), semiMode);
                }
        };
        triangle(vx[0], vx[1], vx[2]);
        if (quad) triangle(vx[1], vx[2], vx[3]);
    }

    void Line(const std::vector<uint32_t>& w, uint32_t cmd) {
        const bool gouraud = (cmd & 0x10u) != 0, poly = (cmd & 8u) != 0, semi = (cmd & 2u) != 0;
        if (poly) {
            ++stats_.unknown; // polylines are not in the HUD
            return;
        }
        if (w.size() < (gouraud ? 4u : 3u)) return;
        const uint32_t rgb = w[0] & 0xFFFFFFu;
        const int32_t x0 = Sext11(w[1] & 0x7FFu) + st_.offX, y0 = Sext11((w[1] >> 16) & 0x7FFu) + st_.offY;
        const uint32_t p1 = gouraud ? w[3] : w[2];
        const int32_t x1 = Sext11(p1 & 0x7FFu) + st_.offX, y1 = Sext11((p1 >> 16) & 0x7FFu) + st_.offY;
        ++stats_.lines;
        const int32_t dx = std::abs(x1 - x0), dy = std::abs(y1 - y0);
        const int32_t steps = std::max(dx, dy);
        for (int32_t s = 0; s <= steps; ++s) {
            const int32_t x = steps ? x0 + static_cast<int32_t>(std::lround(static_cast<double>(x1 - x0) * s / steps)) : x0;
            const int32_t y = steps ? y0 + static_cast<int32_t>(std::lround(static_cast<double>(y1 - y0) * s / steps)) : y0;
            Plot(x, y, Rgb15(rgb), semi, (st_.tpage >> 5) & 3u);
        }
    }

    const HudVram& vram_;
    HudOverlay& out_;
    HudRasterStats& stats_;
    int32_t ox_ = 0, oy_ = 0;
    DrawState st_;
};

} // namespace

void HudOverlay::Clear() {
    std::fill(rgba.begin(), rgba.end(), 0.0f);
    std::fill(opaque.begin(), opaque.end(), uint8_t{0});
    std::fill(opaqueColour.begin(), opaqueColour.end(), uint16_t{0});
}

std::vector<uint8_t> HudOverlay::Bytes() const {
    std::vector<uint8_t> b(rgba.size());
    for (size_t i = 0; i < rgba.size(); ++i)
        b[i] = static_cast<uint8_t>(std::clamp(static_cast<int>(std::lround(rgba[i] * 255.0f)), 0, 255));
    return b;
}

std::vector<HudPacket> WalkHudList(const uint8_t* ram, uint32_t head, size_t limit) {
    std::vector<HudPacket> out;
    uint32_t at = head & 0xFFFFFFu;
    size_t steps = 0;
    while (at != 0xFFFFFFu && steps++ < limit * 4) {
        if (at >= 0x200000u) break;
        const uint32_t tag = RamWord(ram, at);
        const uint32_t n = tag >> 24;
        if (n != 0) {
            HudPacket p;
            p.address = 0x80000000u | at;
            for (uint32_t k = 0; k < n; ++k) p.words.push_back(RamWord(ram, at + 4u + 4u * k));
            out.push_back(std::move(p));
            if (out.size() >= limit) break;
        }
        at = tag & 0xFFFFFFu;
    }
    return out;
}

HudRasterStats RasterizeHud(const std::vector<HudPacket>& packets, const HudVram& vram, HudOverlay& out,
                            int32_t originX, int32_t originY) {
    HudRasterStats stats;
    Raster r(vram, out, stats, originX, originY);
    for (const HudPacket& p : packets) r.Packet(p.words);
    return stats;
}

// ============================================================================ the trace comparison
namespace {

bool ReadAll(const std::string& path, std::vector<uint8_t>& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

std::vector<std::vector<std::string>> ReadCsv(const std::string& path) {
    std::vector<std::vector<std::string>> rows;
    std::ifstream f(path);
    std::string line;
    bool header = true;
    while (std::getline(f, line)) {
        if (header) {
            header = false;
            continue;
        }
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::vector<std::string> cells;
        std::string cell;
        std::istringstream ss(line);
        while (std::getline(ss, cell, ',')) cells.push_back(cell);
        rows.push_back(std::move(cells));
    }
    return rows;
}

uint64_t Num(const std::string& s) { return s.empty() ? 0 : std::strtoull(s.c_str(), nullptr, 0); }

// The check's callees: the sound calls are recorded, ComputePlace runs PORTED over host views of the
// capture's pool (as `rrverify phys` resolves it), the unported elements are recorded and skipped.
struct CheckCallees final : rr::sim::HudCallees {
    uint8_t* ram = nullptr;
    std::vector<std::string> log;
    bool PlaySound(uint32_t, int32_t id) override {
        log.push_back("PlaySound3D(0, 0, " + std::to_string(id) + ", 0)");
        return true;
    }
    bool StopCountdownVoice(uint32_t) override {
        log.push_back("SLUS 0x80016528");
        return true;
    }
    bool HeapOverflow(uint32_t, uint32_t, uint32_t, uint32_t*) override {
        log.push_back("SLUS 0x80021C98 (heap full) - refused");
        return false;
    }
    bool Unported(uint32_t, uint32_t address, const uint32_t*, int) override {
        char b[64];
        std::snprintf(b, sizeof(b), "RASHCDG 0x%08X skipped (not ported)", address);
        log.push_back(b);
        return true;
    }
    bool ComputePlace(uint32_t, uint32_t bike, int32_t mode, int32_t* place) override {
        const uint32_t base = RamWord(ram, 0x800CE4D0u), stride = RamWord(ram, 0x800CE4D4u);
        const int32_t high = static_cast<int32_t>(RamWord(ram, RamWord(ram, 0x800CE4DCu)));
        const int16_t armed = static_cast<int16_t>(RamWord(ram, 0x800D6180u) >> 16);
        auto at = [&](uint32_t a) -> const uint8_t* { return (a >= 0x80000000u && (a & 0x1FFFFFu) < 0x1FFF00u) ? ram + (a & 0x1FFFFFu) : nullptr; };
        auto binding = [&](uint32_t e) {
            rr::sim::RouteBinding b;
            const uint32_t ro = RamWord(ram, e + 0x1ACu);
            b.routeArmed = armed;
            b.routeObject = at(ro);
            if (b.routeObject) {
                b.firstWord = RamWord(ram, ro);
                b.legs = b.routeObject + 20;
                b.legCount = static_cast<int32_t>(RamWord(ram, ro + 12u));
            }
            return b;
        };
        std::vector<rr::sim::PlaceNode> nodes(static_cast<size_t>(std::max(high + 1, 0)));
        for (int32_t i = 0; i <= high; ++i) {
            const uint32_t e = base + stride * static_cast<uint32_t>(i);
            nodes[static_cast<size_t>(i)].entity = at(e);
            nodes[static_cast<size_t>(i)].riderDef = at(RamWord(ram, e + 0x43Cu));
            nodes[static_cast<size_t>(i)].binding = binding(e);
        }
        rr::sim::ComputePlaceEnv env;
        env.gameState = at(RamWord(ram, 0x8005B2F8u));
        env.liveBikes = static_cast<int32_t>(RamWord(ram, 0x8005B1F8u));
        env.raceFlags = RamWord(ram, 0x8005AD48u);
        env.pool = nodes.data();
        env.poolHigh = high;
        env.poolCount = static_cast<int32_t>(nodes.size());
        const bool ok = rr::sim::ComputePlace(at(bike), at(RamWord(ram, bike + 0x43Cu)), binding(bike), mode, env, place);
        log.push_back("ComputePlace(bike, " + std::to_string(mode) + ") = " + std::to_string(*place) + " (ported)");
        return ok;
    }
};

std::string ItemName(const std::vector<std::string>& names, uint32_t items, uint32_t address) {
    if (address >= items && address < items + 3960u) {
        const uint32_t i = (address - items) / 36u;
        return (i < names.size()) ? names[i] : ("item " + std::to_string(i));
    }
    return "heap packet (text / TKO icon)";
}

// The item names, positionally, out of the player's own DASH1P.CSV (the rows after the 22 textures and 62
// arts), for the report only.
std::vector<std::string> ItemNames(const DiscImage& disc) {
    std::vector<std::string> names;
    const auto f = disc.Find("DATA/DASH1P.CSV");
    if (!f) return names;
    const std::vector<uint8_t> b = disc.ReadFile(*f);
    std::string text(b.begin(), b.end());
    std::istringstream ss(text);
    std::string line;
    std::vector<std::string> lines;
    while (std::getline(ss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) lines.push_back(line);
    }
    size_t counts = 0;
    for (size_t i = 0; i < lines.size(); ++i)
        if (lines[i].rfind("TexArtDashCounts", 0) == 0) counts = i;
    for (size_t i = counts + 1 + 22 + 62; i < lines.size(); ++i) names.push_back(lines[i].substr(0, lines[i].find(',')));
    return names;
}

std::string Hex(uint32_t v) {
    char b[16];
    std::snprintf(b, sizeof(b), "0x%08X", v);
    return b;
}

} // namespace

bool CheckHudAgainstTrace(const DiscImage& disc, const std::string& stateDir, const std::string& traceDir,
                          std::string& report, bool mutate, std::vector<uint8_t>* shownOut,
                          std::vector<uint8_t>* composedOut) {
    report.clear();
    std::vector<uint8_t> ram;
    if (!ReadAll(stateDir + "\\ram.bin", ram) || ram.size() < 0x200000u) {
        report = "no ram.bin under " + stateDir + "\n";
        return false;
    }
    // 1. The state at HudFrame's entry: the capture plus every CPU store the traced run made before it.
    uint64_t entrySeq = 0, exitSeq = 0;
    for (const auto& r : ReadCsv(traceDir + "\\probes.csv")) {
        if (r.size() < 3) continue;
        if (r[2] == "hud_entry" && entrySeq == 0) entrySeq = Num(r[0]);
        if (r[2] == "hud_exit" && exitSeq == 0) exitSeq = Num(r[0]);
    }
    if (entrySeq == 0 || exitSeq == 0) {
        report = "the trace under " + traceDir + " has no hud_entry / hud_exit probe hit\n";
        return false;
    }
    size_t replayed = 0;
    for (const auto& r : ReadCsv(traceDir + "\\watch.csv")) {
        if (r.size() < 7 || r[2] != "write") continue;
        if (Num(r[0]) >= entrySeq) break;
        const uint32_t a = static_cast<uint32_t>(Num(r[4])) & 0x1FFFFFu;
        const uint32_t n = static_cast<uint32_t>(Num(r[5]));
        const uint32_t v = static_cast<uint32_t>(Num(r[6]));
        for (uint32_t k = 0; k < n && a + k < ram.size(); ++k) ram[a + k] = static_cast<uint8_t>(v >> (8 * k));
        ++replayed;
    }
    // the negative control: one bit of three inputs flipped
    if (mutate) {
        const uint32_t bike = RamWord(ram.data(), rr::sim::kHudPlayerBikes);
        ram[(bike + 480u + 2u) & 0x1FFFFFu] ^= 0x01; // the speed, one bit: its digits change
    }
    // 2. The PORTED HudFrame on it.
    const uint32_t ot = RamWord(ram.data(), rr::sim::kHudOt);
    rr::sim::GuestRam g(ram.data(), 0x8005AC8Cu);
    CheckCallees callees;
    callees.ram = ram.data();
    const uint32_t heapEntry = RamWord(ram.data(), RamWord(ram.data(), rr::sim::kHudHeapPtr) + 268u);
    const bool ran = rr::sim::HudFrame(g, callees, 0x801FFE00u);
    const std::vector<HudPacket> ours = WalkHudList(ram.data(), RamWord(ram.data(), ot));

    // 3. The original's GP0 commands: the GPU stream's first transfer after HudFrame returned, one row
    // of prims.csv per command with the RAM address its first word came from. That transfer is the
    // whole ordering table; the HUD slot's part of it starts at item 0 (HudFrame links it last, so it
    // heads the slot) and runs while the commands come from the item records or the packet heap.
    struct Command {
        uint32_t address = 0; // of its first word
        std::vector<uint32_t> words;
    };
    const uint32_t items = RamWord(ram.data(), rr::sim::kHudItemsPtr);
    auto inHud = [&](uint32_t a, uint32_t heapFrom, uint32_t heapTo) {
        return (a >= items && a < items + 3960u) || (a >= heapFrom && a < heapTo);
    };
    std::vector<Command> originalAll;
    uint64_t drawSeq = 0;
    for (const auto& r : ReadCsv(traceDir + "\\prims.csv")) {
        if (r.size() < 10) continue;
        const uint64_t seq = Num(r[1]);
        if (seq <= exitSeq) continue;
        if (drawSeq == 0) drawSeq = seq;
        if (seq != drawSeq) break;
        Command c;
        c.address = static_cast<uint32_t>(Num(r[4]));
        const uint32_t n = static_cast<uint32_t>(Num(r[3]));
        for (uint32_t k = 0; k < n && 9u + k < r.size(); ++k) c.words.push_back(static_cast<uint32_t>(Num(r[9 + k])));
        originalAll.push_back(std::move(c));
    }
    // Our packets split into GP0 commands the way the GPU splits a packet's words.
    auto commandWords = [](uint32_t w0) -> uint32_t {
        const uint32_t cmd = w0 >> 24;
        if (cmd >= 0x20 && cmd < 0x40) {
            const bool gouraud = cmd & 0x10u, quad = cmd & 8u, tex = cmd & 4u;
            const uint32_t n = quad ? 4u : 3u;
            return 1u + n * (tex ? 2u : 1u) + (gouraud ? n - 1u : 0u);
        }
        if (cmd >= 0x40 && cmd < 0x60) return (cmd & 0x10u) ? 4u : 3u;
        if (cmd >= 0x60 && cmd < 0x80) return 2u + ((cmd & 4u) ? 1u : 0u) + (((cmd >> 3) & 3u) == 0 ? 1u : 0u);
        if (cmd == 0x80) return 4u;
        if (cmd == 0xA0 || cmd == 0xC0) return 3u;
        if (cmd == 0x02) return 3u;
        return 1u;
    };
    std::vector<Command> ourAll;
    for (const HudPacket& p : ours) {
        size_t k = 0;
        while (k < p.words.size()) {
            Command c;
            c.address = p.address + 4u + 4u * static_cast<uint32_t>(k);
            const uint32_t n = std::min<uint32_t>(commandWords(p.words[k]), static_cast<uint32_t>(p.words.size() - k));
            for (uint32_t j = 0; j < n; ++j) c.words.push_back(p.words[k + j]);
            ourAll.push_back(std::move(c));
            k += n;
        }
    }
    // The HUD slot's range on each side. The heap part is bounded by what OUR run took from the heap,
    // [its next-free at entry, its next-free after our run): the original's HUD packets must fall in it.
    const uint32_t heapTo = RamWord(ram.data(), RamWord(ram.data(), rr::sim::kHudHeapPtr) + 268u) | 0x80000000u;
    const uint32_t heapFrom = heapEntry | 0x80000000u;
    std::vector<Command> originalHud, oursHud;
    size_t originalBefore = 0, originalAfter = 0;
    {
        size_t i = 0;
        while (i < originalAll.size() && originalAll[i].address != items + 4u) ++i;
        originalBefore = i;
        while (i < originalAll.size() && inHud(originalAll[i].address, heapFrom, heapTo)) originalHud.push_back(originalAll[i++]);
        originalAfter = originalAll.size() - i;
    }
    for (const Command& c : ourAll) {
        if (!inHud(c.address, heapFrom, heapTo)) break;
        oursHud.push_back(c);
    }

    const std::vector<std::string> names = ItemNames(disc);
    size_t same = 0, differ = 0;
    std::string details;
    const size_t common = std::min(originalHud.size(), oursHud.size());
    for (size_t k = 0; k < common; ++k) {
        const Command& o = originalHud[k];
        const Command& m = oursHud[k];
        if (o.address == m.address && o.words == m.words) {
            ++same;
            continue;
        }
        ++differ;
        details += "    DIFFER #" + std::to_string(k) + "  original " + Hex(o.address) + " (" + ItemName(names, items, o.address - 4u) +
                   ") / ours " + Hex(m.address) + " (" + ItemName(names, items, m.address - 4u) + "):";
        for (size_t j = 0; j < std::max(o.words.size(), m.words.size()); ++j) {
            const uint32_t x = j < o.words.size() ? o.words[j] : 0, y = j < m.words.size() ? m.words[j] : 0;
            if (x != y) details += " w" + std::to_string(j) + " " + Hex(x) + "/" + Hex(y);
        }
        details += "\n";
    }
    for (size_t k = common; k < originalHud.size(); ++k)
        details += "    ORIGINAL ONLY #" + std::to_string(k) + "  " + Hex(originalHud[k].address) + "  " +
                   ItemName(names, items, originalHud[k].address - 4u) + "\n";
    for (size_t k = common; k < oursHud.size(); ++k)
        details += "    OURS ONLY #" + std::to_string(k) + "  " + Hex(oursHud[k].address) + "  " +
                   ItemName(names, items, oursHud[k].address - 4u) + "\n";
    const size_t origOnly = originalHud.size() - common, oursOnly = oursHud.size() - common;
    const bool sameOrder = differ == 0 && origOnly == 0 && oursOnly == 0;
    const size_t origOther = originalBefore + originalAfter;
    const std::vector<Command>& origHud = originalHud;
    const std::vector<Command>& original = originalAll;

    // 4. The element set, by name, as drawn.
    std::string elements;
    {
        std::map<std::string, int> count;
        std::vector<std::string> order;
        for (const HudPacket& p : ours) {
            const std::string n = ItemName(names, items, p.address);
            if (count[n]++ == 0) order.push_back(n);
        }
        for (const std::string& n : order)
            elements += "    " + n + (count[n] > 1 ? " x" + std::to_string(count[n]) : "") + "\n";
    }

    // 5. The pixels: our packets rasterised by our rasteriser, against the frame the console showed at
    // capture time (vram.bin's display area) on every pixel our HUD draws opaque. The shown frame is
    // the one BEFORE the traced frame, so a value that changed in between (a digit) is counted here.
    HudVram vram;
    std::string build;
    size_t pxCompared = 0, pxSame = 0, pxNear = 0;
    std::vector<uint8_t> vramCap;
    int dispX = 0, dispY = 0;
    if (ReadAll(stateDir + "\\vram.bin", vramCap) && vramCap.size() >= 1024u * 512u * 2u) {
        for (int y = 0; y < HudVram::kRows; ++y)
            for (int x = 0; x < HudVram::kWidth; ++x) {
                const size_t o = (static_cast<size_t>(y) * 1024 + static_cast<size_t>(HudVram::kX + x)) * 2;
                vram.page[static_cast<size_t>(y) * HudVram::kWidth + static_cast<size_t>(x)] =
                    static_cast<uint16_t>(vramCap[o] | (vramCap[o + 1] << 8));
            }
        vram.rowsWritten = HudVram::kRows;
        // the display origin the capture's GPU state names
        std::vector<uint8_t> js;
        if (ReadAll(stateDir + "\\gpu.json", js)) {
            const std::string s(js.begin(), js.end());
            auto field = [&](const char* key) {
                const size_t p = s.find(key);
                return p == std::string::npos ? 0 : std::atoi(s.c_str() + s.find(':', p) + 1);
            };
            dispX = field("\"display_address_start_x\"");
            dispY = field("\"display_address_start_y\"");
        }
        HudOverlay overlay;
        // the frame's drawing offset, as C5558 reads it: the current buffer's DRAWENV in the heap record
        const uint32_t heapRec = RamWord(ram.data(), rr::sim::kHudHeapPtr);
        const uint32_t env = heapRec + 112u * ram[(heapRec + 5u) & 0x1FFFFFu];
        const int32_t originX = static_cast<int16_t>(RamWord(ram.data(), env + 24u) & 0xFFFFu);
        const int32_t originY = static_cast<int16_t>(RamWord(ram.data(), env + 24u) >> 16);
        RasterizeHud(ours, vram, overlay, originX, originY);
        for (int y = 0; y < HudOverlay::kHeight; ++y)
            for (int x = 0; x < HudOverlay::kWidth; ++x) {
                const size_t i = static_cast<size_t>(y) * HudOverlay::kWidth + static_cast<size_t>(x);
                if (!overlay.opaque[i]) continue;
                const size_t o = (static_cast<size_t>(dispY + y) * 1024 + static_cast<size_t>(dispX + x)) * 2;
                if (o + 1 >= vramCap.size()) continue;
                const uint16_t shown = static_cast<uint16_t>((vramCap[o] | (vramCap[o + 1] << 8)) & 0x7FFF);
                const uint16_t mine = overlay.opaqueColour[i];
                ++pxCompared;
                if (shown == mine) {
                    ++pxSame;
                    continue;
                }
                bool near = true;
                for (int k = 0; k < 3; ++k)
                    if (std::abs(static_cast<int>((shown >> (5 * k)) & 31) - static_cast<int>((mine >> (5 * k)) & 31)) > 1) near = false;
                if (near) ++pxNear;
            }
        // pictures for a human: the shown frame, and our HUD over the shown frame's picture area
        if (shownOut != nullptr && composedOut != nullptr) {
            std::vector<uint8_t> shown(static_cast<size_t>(HudOverlay::kWidth) * HudOverlay::kHeight * 4);
            std::vector<uint8_t> composed(shown.size());
            const std::vector<uint8_t> ob = overlay.Bytes();
            for (int y = 0; y < HudOverlay::kHeight; ++y)
                for (int x = 0; x < HudOverlay::kWidth; ++x) {
                    const size_t o = (static_cast<size_t>(dispY + y) * 1024 + static_cast<size_t>(dispX + x)) * 2;
                    const uint16_t c = static_cast<uint16_t>(vramCap[o] | (vramCap[o + 1] << 8));
                    const size_t i = (static_cast<size_t>(y) * HudOverlay::kWidth + static_cast<size_t>(x)) * 4;
                    const float rgb[3] = {Channel5(c & 31u), Channel5((c >> 5) & 31u), Channel5((c >> 10) & 31u)};
                    for (int k = 0; k < 3; ++k) {
                        shown[i + static_cast<size_t>(k)] = static_cast<uint8_t>(rgb[k] * 255.0f);
                        const float a = overlay.rgba[i + 3];
                        composed[i + static_cast<size_t>(k)] = static_cast<uint8_t>(std::clamp(
                            (overlay.rgba[i + static_cast<size_t>(k)] + (1.0f - a) * rgb[k]) * 255.0f, 0.0f, 255.0f));
                    }
                    shown[i + 3] = composed[i + 3] = 255;
                }
            *shownOut = std::move(shown);
            *composedOut = std::move(composed);
        }
    }

    char line[512];
    std::snprintf(line, sizeof(line),
                  "HUD check: %s  (the ported HudFrame on the state at the original's HudFrame entry)%s\n"
                  "  state         %s + %zu CPU stores replayed up to seq %llu (HudFrame entry), exit seq %llu\n"
                  "  our HudFrame  %s; %zu packets linked; callees: %zu\n",
                  traceDir.c_str(), mutate ? "  [NEGATIVE CONTROL: one bit of the player's speed flipped]" : "",
                  stateDir.c_str(), replayed, static_cast<unsigned long long>(entrySeq),
                  static_cast<unsigned long long>(exitSeq), ran ? "ran" : "REFUSED", ours.size(), callees.log.size());
    report += line;
    for (const std::string& c : callees.log) report += "      " + c + "\n";
    std::snprintf(line, sizeof(line),
                  "  original      GPU transfer at seq %llu: %zu packets, %zu of them at HUD addresses, %zu elsewhere "
                  "(the rest of the ordering table)\n",
                  static_cast<unsigned long long>(drawSeq), original.size(), origHud.size(), origOther);
    report += line;
    std::snprintf(line, sizeof(line),
                  "  packets       %zu identical, %zu differ, %zu only in the original, %zu only in ours; common order %s\n",
                  same, differ, origOnly, oursOnly, sameOrder ? "identical" : "DIFFERS");
    report += line;
    report += details;
    report += "  elements drawn (ours, list order):\n" + elements;
    std::snprintf(line, sizeof(line),
                  "  pixels        %zu opaque HUD pixels against the capture's shown frame: %zu identical, %zu within one "
                  "5-bit step, %zu other\n",
                  pxCompared, pxSame, pxNear, pxCompared - pxSame - pxNear);
    report += line;
    const bool pass = ran && differ == 0 && oursOnly == 0 && sameOrder && same > 0;
    std::snprintf(line, sizeof(line), "  verdict       %s (every packet of ours is the original's, byte for byte, in its order%s)\n",
                  pass ? "PASS" : "FAIL", origOnly ? "; the original-only packets are listed above" : "");
    report += line;
    return pass;
}

bool RunHudFrameOnCapture(uint8_t* ram) {
    if (std::getenv("RRJB_PARITY_HUD") != nullptr && std::strcmp(std::getenv("RRJB_PARITY_HUD"), "off") == 0) return false;
    rr::sim::GuestRam g(ram, 0x8005AC8Cu);
    const uint32_t ot = g.U32(rr::sim::kHudOt), ot2 = g.U32(rr::sim::kHudOt2);
    if (ot < 0x80000000u || ot >= 0x80200000u || ot2 < 0x80000000u || ot2 >= 0x80200000u) return false;
    g.W32(ot, 0x00FFFFFFu);
    g.W32(ot2, 0x00FFFFFFu);
    CheckCallees callees;
    callees.ram = ram;
    const bool ran = rr::sim::HudFrame(g, callees, 0x801FFE00u);
    if (std::getenv("RRJB_PARITY_HUD_LOG") != nullptr) { // DEVELOPMENT: what the re-run linked
        const uint32_t items = g.U32(rr::sim::kHudItemsPtr);
        std::printf("parity hud: ot 0x%08X items 0x%08X:", ot, items);
        for (const HudPacket& p : WalkHudList(ram, g.U32(ot)))
            if (p.address >= items && p.address < items + 3960u) std::printf(" %u", (p.address - items) / 36u);
            else std::printf(" [0x%08X w0 0x%08X]", p.address, p.words.empty() ? 0u : p.words[0]);
        std::printf("\n");
        for (const std::string& l : callees.log) std::printf("parity hud:   %s\n", l.c_str());
    }
    return ran && !g.Faulted();
}

} // namespace rr::game
