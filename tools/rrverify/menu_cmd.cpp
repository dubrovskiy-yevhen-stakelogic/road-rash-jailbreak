// rrverify menuprims - the product's menu picture against the ORIGINAL's primitives for the same
// menu state.
//
// The product draws the front end natively (shell_view.h). This check asks the original what it would
// draw on that frame: the product's front end is driven to a menu state with a key script, its shell
// state (the RASHCDF overlay's data and bss, the session and player records) is planted into a
// capture of the running shell (work\oracle\state\retro-shell: RAM with the sprite records, the fonts,
// the display environment, and the VRAM those point into), and the ORIGINAL's widget pass RASHCDF
// 0x8006D3E0 is run in the interpreter for every live screen, as ScreenTick 0x80066C34 runs it (fe+0x08
// the screen, fe+0x11 its layer). The primitives it links into the shell's ordering table are then
// walked as DrawOTag walks them (the table 0x8009CFC8 cleared first the way 0x80080400 clears it, 72
// entries, drawn from the last) and rasterised over the capture's VRAM: that is the original's frame
// for this state, pixel by pixel. The product's frame is compared against it.
//
// What cannot be compared, named: a picture the capture does not hold in VRAM (the capture is the main
// menu; a screen's .STR frames are decoded into a shared VRAM slot when it is entered) - the original's
// emitter asks for the decode and draws the slot as it is, the product draws the picture from the disc.
// Those pixels (the product's blits of a FourCC that is not loaded, or not its shared slot's occupant in
// the capture, and film frames) are masked out and counted.
//
//   rrverify menuprims --disc <bin> --state <retro-shell> --script "<f:keys;...>" --frames N --out <prefix>
//                      [--min-match P] [--max-extra N]
//   keys: up down left right x t s c; g<n> GotoScreen(n); v<n> career venue n (as rrgame's --shell-script)
//
//   --calls  also list the original's text calls with their callers, ours, and every source.
//
// Output: <prefix>_orig_vs_ours.png (top: left the original, right ours; below: the pixels that differ)
// and one line per source the original drew. PASS when the compared pixels match at least P % (default
// 99, within 24 levels per channel) and at most N (default 0) pixels are drawn by one side only.
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "game/shell/front_end.h"
#include "game/shell/shell_text.h"
#include "interp/r3000.h"
#include "interp/snapshot.h"
#include "platform/png.h"
#include "rrvfs/disc_image.h"

namespace {

using rr::interp::Cpu;
using rr::interp::Memory;
using rr::interp::Trap;
using rr::interp::TrapKind;

constexpr uint32_t kSentinel = 0x00000DEC;
constexpr int W = rr::shell::ShellView::kWidth, H = rr::shell::ShellView::kHeight;
constexpr uint32_t kSpriteIds = 0x80088E0C;   // the 300 FourCCs 0x800705DC / 0x800700F0 search
constexpr uint32_t kSpriteRecs = 0x8009DDE0;  // their 36-byte sprite records (+0 flags, +0x10 VRAM rect)
constexpr uint32_t kOtPtr = 0x8009CFC8;       // the frame's ordering table (0x80080400: 72 entries)
constexpr uint32_t kOtPtr2 = 0x8009CCE4;      // its second table

std::string FourCC(uint32_t w) {
    std::string s(4, ' ');
    for (int i = 0; i < 4; ++i) {
        const char c = static_cast<char>((w >> (8 * i)) & 0xFFu);
        s[static_cast<size_t>(i)] = (c >= 32 && c < 127) ? c : '?';
    }
    return s;
}

struct Opt {
    std::string disc, state, script, out = "menuprims";
    int frames = 60;
    double minMatch = 99.0;
    long maxExtra = 0;
    bool calls = false;
};

struct Vram {
    std::vector<uint16_t> px = std::vector<uint16_t>(1024u * 512u, 0);
    uint16_t At(int x, int y) const { return px[static_cast<size_t>(y & 511) * 1024u + static_cast<size_t>(x & 1023)]; }
};

// The capture's vram.bin is stored 281 bytes late: real halfword k is
// raw[2(k+140)-1] | raw[2(k+140)] << 8.
bool LoadVram(const std::string& path, Vram& v) {
    std::vector<uint8_t> raw;
    std::string error;
    if (!rr::interp::ReadWholeFile(path, raw, error) || raw.size() != 1024u * 512u * 2u) return false;
    for (size_t k = 0; k < v.px.size(); ++k) {
        const size_t i = 2 * (k + 140u);
        v.px[k] = (i >= 1 && i < raw.size()) ? static_cast<uint16_t>(raw[i - 1] | (raw[i] << 8)) : 0;
    }
    return true;
}

// One source of pixels on the original's side: a sprite record's FourCC, a font sheet, or VRAM.
struct Source {
    std::string name;
    long drawn = 0;
};

class Raster {
public:
    const Memory& mem;
    const Vram& vram;
    std::vector<uint8_t> rgb = std::vector<uint8_t>(static_cast<size_t>(W) * H * 3u, 0);
    std::vector<int> owner = std::vector<int>(static_cast<size_t>(W) * H, -1); // index into sources
    std::vector<Source> sources;
    std::map<std::string, int> sourceIndex;
    long prims = 0, skipped = 0;
    std::vector<std::string> notes;
    // GPU state.
    uint32_t tpage = 0;
    int clipX0 = 0, clipY0 = 0, clipX1 = W - 1, clipY1 = H - 1, offX = 0, offY = 0;
    // The draw buffer the frame is: a drawing-area primitive (GP0 E3 / E4) is in VRAM coordinates, the
    // frame starts at the buffer's origin (the display environment *(0x8005B470), buffer +6).
    int bufX = 0, bufY = 0;

    Raster(const Memory& m, const Vram& v) : mem(m), vram(v) {}

    int SourceId(const std::string& name) {
        auto it = sourceIndex.find(name);
        if (it != sourceIndex.end()) return it->second;
        sources.push_back({name, 0});
        return sourceIndex[name] = static_cast<int>(sources.size() - 1);
    }

    // Which sprite record's VRAM rectangle holds this VRAM texel (15-bit pages), by FourCC.
    // Several records share a VRAM slot (the titles, the logos): the one the pass uploaded last there
    // (`uploaded`, filled by the LoadImage hook) names it, else one flagged uploaded (0x20000000).
    std::vector<std::pair<std::array<int, 4>, std::string>> uploaded;
    std::string SpriteAt(int vx, int vy) const {
        for (auto it = uploaded.rbegin(); it != uploaded.rend(); ++it) {
            const auto& r = it->first;
            if (vx >= r[0] && vx < r[0] + r[2] && vy >= r[1] && vy < r[1] + r[3]) return it->second;
        }
        std::string loaded;
        for (uint32_t i = 0; i < 300u; ++i) {
            const uint32_t rec = kSpriteRecs + 36u * i;
            const uint32_t f = mem.PeekWord(rec);
            if ((f & 0x10000000u) == 0) continue;
            const int x = static_cast<int16_t>(Half(rec + 0x10u)), y = static_cast<int16_t>(Half(rec + 0x12u));
            const int w = static_cast<int16_t>(Half(rec + 0x14u)), h = static_cast<int16_t>(Half(rec + 0x16u));
            if (!(vx >= x && vx < x + w && vy >= y && vy < y + h)) continue;
            if (f & 0x20000000u) return FourCC(mem.PeekWord(kSpriteIds + 4u * i));
            if (loaded.empty()) loaded = FourCC(mem.PeekWord(kSpriteIds + 4u * i));
        }
        if (!loaded.empty()) return loaded;
        char b[32];
        std::snprintf(b, sizeof(b), "vram(%d,%d)", vx, vy);
        return b;
    }
    uint16_t Half(uint32_t a) const { return static_cast<uint16_t>(mem.PeekByte(a) | (mem.PeekByte(a + 1u) << 8)); }

    void Plot(int x, int y, uint16_t c5, bool semi, int src) {
        x += offX, y += offY;
        if (x < clipX0 || y < clipY0 || x > clipX1 || y > clipY1 || x < 0 || y < 0 || x >= W || y >= H) return;
        uint8_t* d = &rgb[(static_cast<size_t>(y) * W + static_cast<size_t>(x)) * 3u];
        const int f[3] = {(c5 & 31) << 3, ((c5 >> 5) & 31) << 3, ((c5 >> 10) & 31) << 3};
        for (int k = 0; k < 3; ++k) {
            int v = f[k];
            if (semi) {
                const int b = d[k];
                switch ((tpage >> 5) & 3u) {
                case 0: v = (b + f[k]) / 2; break;
                case 1: v = std::min(255, b + f[k]); break;
                case 2: v = std::max(0, b - f[k]); break;
                default: v = std::min(255, b + f[k] / 4); break;
                }
            }
            d[k] = static_cast<uint8_t>(v);
        }
        owner[static_cast<size_t>(y) * W + static_cast<size_t>(x)] = src;
        ++sources[static_cast<size_t>(src)].drawn;
    }

    // A textured or flat axis-aligned rectangle (sprites, tiles, and quads that are rectangles).
    void Rect(int x, int y, int w, int h, uint32_t colour, bool textured, bool raw, bool semi, int u0, int v0,
              uint32_t clut, uint32_t page) {
        ++prims;
        const int depth = static_cast<int>((page >> 7) & 3u);
        const int tpx = static_cast<int>(page & 15u) * 64, tpy = static_cast<int>((page >> 4) & 1u) * 256;
        const int cx = static_cast<int>(clut & 0x3Fu) * 16, cy = static_cast<int>((clut >> 6) & 0x1FFu);
        std::string name = "flat";
        if (textured) {
            if (depth == 2) name = SpriteAt(tpx + u0, tpy + v0);
            else {
                char b[48];
                std::snprintf(b, sizeof(b), "%dbpp page(%d,%d) clut(%d,%d)%s mode %u", depth == 0 ? 4 : 8, tpx, tpy, cx, cy,
                              semi ? " semi" : "", (page >> 5) & 3u);
                name = b;
            }
        }
        const int src = SourceId(name);
        const int cr = static_cast<int>(colour & 0xFFu), cg = static_cast<int>((colour >> 8) & 0xFFu),
                  cb = static_cast<int>((colour >> 16) & 0xFFu);
        for (int dy = 0; dy < h; ++dy)
            for (int dx = 0; dx < w; ++dx) {
                uint16_t c5;
                bool stp = true;
                if (textured) {
                    const int u = (u0 + dx) & 0xFF, v = (v0 + dy) & 0xFF;
                    uint16_t t;
                    if (depth == 0) {
                        const uint16_t word = vram.At(tpx + u / 4, tpy + v);
                        t = vram.At(cx + ((word >> ((u & 3) * 4)) & 0xF), cy);
                    } else if (depth == 1) {
                        const uint16_t word = vram.At(tpx + u / 2, tpy + v);
                        t = vram.At(cx + ((word >> ((u & 1) * 8)) & 0xFF), cy);
                    } else {
                        t = vram.At(tpx + u, tpy + v);
                    }
                    if (t == 0) continue; // transparent black
                    stp = (t & 0x8000u) != 0;
                    if (raw) c5 = t & 0x7FFFu;
                    else {
                        const int r = std::min(31, ((t & 31) * cr) >> 7), g = std::min(31, (((t >> 5) & 31) * cg) >> 7),
                                  b = std::min(31, (((t >> 10) & 31) * cb) >> 7);
                        c5 = static_cast<uint16_t>(r | (g << 5) | (b << 10));
                    }
                } else {
                    c5 = static_cast<uint16_t>((cr >> 3) | ((cg >> 3) << 5) | ((cb >> 3) << 10));
                }
                Plot(x + dx, y + dy, c5, semi && stp, src);
            }
    }

    uint32_t Word(uint32_t a) const { return mem.PeekWord(a); }
    static int X(uint32_t w) { return static_cast<int16_t>(w & 0xFFFFu); }
    static int Y(uint32_t w) { return static_cast<int16_t>(w >> 16); }

    // The GP0 commands of one packet (after its tag word).
    void Packet(uint32_t a, uint32_t words) {
        uint32_t i = 0;
        while (i < words) {
            const uint32_t w0 = Word(a + 4u * i);
            const uint32_t cmd = w0 >> 24;
            if (cmd == 0x00 || cmd == 0x01) { ++i; continue; }
            if (cmd == 0x02) { // fill: colour, xy, wh (not clipped, no offset)
                if (i + 2 >= words) break;
                const uint32_t xy = Word(a + 4u * (i + 1)), wh = Word(a + 4u * (i + 2));
                const int src = SourceId("fill");
                for (int y = Y(xy); y < Y(xy) + Y(wh); ++y)
                    for (int x = X(xy); x < X(xy) + X(wh); ++x)
                        if (x >= 0 && y >= 0 && x < W && y < H) {
                            uint8_t* d = &rgb[(static_cast<size_t>(y) * W + static_cast<size_t>(x)) * 3u];
                            d[0] = static_cast<uint8_t>(w0 & 0xF8u);
                            d[1] = static_cast<uint8_t>((w0 >> 8) & 0xF8u);
                            d[2] = static_cast<uint8_t>((w0 >> 16) & 0xF8u);
                            owner[static_cast<size_t>(y) * W + static_cast<size_t>(x)] = src;
                        }
                ++prims;
                i += 3;
                continue;
            }
            if (cmd >= 0x20 && cmd < 0x40) {
                const bool quad = (cmd & 8u) != 0, tex = (cmd & 4u) != 0, gour = (cmd & 0x10u) != 0;
                const uint32_t nv = quad ? 4u : 3u;
                const uint32_t size = 1u + nv * (1u + (tex ? 1u : 0u)) + (gour ? nv - 1u : 0u);
                if (i + size > words) break;
                int vx[4] = {}, vy[4] = {}, vu[4] = {}, vv[4] = {};
                uint32_t clut = 0, page = tpage;
                uint32_t at = i + 1;
                for (uint32_t k = 0; k < nv; ++k) {
                    if (gour && k > 0) ++at;
                    const uint32_t xy = Word(a + 4u * at++);
                    vx[k] = X(xy), vy[k] = Y(xy);
                    if (tex) {
                        const uint32_t uv = Word(a + 4u * at++);
                        vu[k] = static_cast<int>(uv & 0xFFu), vv[k] = static_cast<int>((uv >> 8) & 0xFFu);
                        if (k == 0) clut = uv >> 16;
                        if (k == 1) page = (uv >> 16) & 0x1FFu;
                    }
                }
                if (tex) tpage = (tpage & ~0x1FFu) | page; // a textured polygon sets the draw mode's page
                const bool rect = quad && !gour && vy[0] == vy[1] && vx[0] == vx[2] && vx[1] == vx[3] && vy[2] == vy[3];
                const int x0 = std::min({vx[0], vx[1], vx[2], vx[3]}), x1 = std::max({vx[0], vx[1], vx[2], vx[3]});
                const int y0 = std::min({vy[0], vy[1], vy[2], vy[3]}), y1 = std::max({vy[0], vy[1], vy[2], vy[3]});
                bool flatRect = quad && !gour && !tex;
                for (int k = 0; k < 4 && flatRect; ++k) flatRect = (vx[k] == x0 || vx[k] == x1) && (vy[k] == y0 || vy[k] == y1);
                if (!rect && flatRect) { // a flat quad given in another vertex order: its box
                    Rect(x0, y0, x1 - x0, y1 - y0, w0 & 0xFFFFFFu, false, false, (cmd & 2u) != 0, 0, 0, 0, tpage);
                } else if (!rect && !tex && !gour && quad) { // any other flat quad: two triangles
                    const int src = SourceId("flat quad");
                    const uint16_t c5 = static_cast<uint16_t>(((w0 & 0xFFu) >> 3) | (((w0 >> 11) & 31u) << 5) | (((w0 >> 19) & 31u) << 10));
                    const int32_t qx[4] = {vx[0], vx[1], vx[2], vx[3]}, qy[4] = {vy[0], vy[1], vy[2], vy[3]};
                    rr::shell::RasterQuad(qx, qy, [&](int32_t x, int32_t y) { Plot(x, y, c5, (cmd & 2u) != 0, src); });
                    ++prims;
                } else if (rect) {
                    Rect(std::min(vx[0], vx[1]), std::min(vy[0], vy[2]), std::abs(vx[1] - vx[0]), std::abs(vy[2] - vy[0]),
                         w0 & 0xFFFFFFu, tex, (cmd & 1u) != 0, (cmd & 2u) != 0, std::min(vu[0], vu[1]),
                         std::min(vv[0], vv[2]), clut, page);
                } else {
                    ++skipped;
                    char b[160];
                    std::snprintf(b, sizeof(b), "polygon 0x%02X not a rectangle (not rasterised): (%d,%d) (%d,%d) (%d,%d) (%d,%d)",
                                  cmd, vx[0], vy[0], vx[1], vy[1], vx[2], vy[2], vx[3], vy[3]);
                    notes.push_back(b);
                }
                i += size;
                continue;
            }
            if (cmd >= 0x40 && cmd < 0x60) { // lines: flat ones rasterised (Bresenham, both ends; a poly-line's
                                            // shared corners drawn once), Gouraud ones not
                if (cmd & 0x10u) { ++skipped; notes.push_back("a Gouraud line (not rasterised)"); break; }
                std::vector<std::pair<int, int>> pts;
                uint32_t at = i + 1;
                for (; at < words; ++at) {
                    const uint32_t v = Word(a + 4u * at);
                    if ((cmd & 8u) && (v & 0xF000F000u) == 0x50005000u && pts.size() >= 2) { ++at; break; }
                    pts.push_back({X(v), Y(v)});
                    if (!(cmd & 8u) && pts.size() == 2) { ++at; break; }
                }
                std::vector<std::pair<int, int>> px;
                for (size_t k = 1; k < pts.size(); ++k) {
                    int x0 = pts[k - 1].first, y0 = pts[k - 1].second;
                    const int x1 = pts[k].first, y1 = pts[k].second;
                    const int dx = std::abs(x1 - x0), sx = x0 < x1 ? 1 : -1, dy = -std::abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
                    int err = dx + dy;
                    for (;;) {
                        px.push_back({x0, y0});
                        if (x0 == x1 && y0 == y1) break;
                        const int e2 = 2 * err;
                        if (e2 >= dy) { err += dy; x0 += sx; }
                        if (e2 <= dx) { err += dx; y0 += sy; }
                    }
                }
                std::sort(px.begin(), px.end());
                px.erase(std::unique(px.begin(), px.end()), px.end());
                const int src = SourceId("line");
                const uint16_t c5 = static_cast<uint16_t>(((w0 & 0xFFu) >> 3) | (((w0 >> 11) & 31u) << 5) | (((w0 >> 19) & 31u) << 10));
                for (const auto& q : px) Plot(q.first, q.second, c5, (cmd & 2u) != 0, src);
                ++prims;
                i = at;
                continue;
            }
            if (cmd >= 0x60 && cmd < 0x80) {
                const bool tex = (cmd & 4u) != 0;
                const uint32_t sz = (cmd >> 3) & 3u;
                const uint32_t size = 2u + (tex ? 1u : 0u) + (sz == 0 ? 1u : 0u);
                if (i + size > words) break;
                const uint32_t xy = Word(a + 4u * (i + 1));
                uint32_t uv = 0, at = i + 2;
                if (tex) uv = Word(a + 4u * at++);
                int w = 1, h = 1;
                if (sz == 0) { const uint32_t wh = Word(a + 4u * at); w = X(wh); h = Y(wh); }
                else if (sz == 2) w = h = 8;
                else if (sz == 3) w = h = 16;
                Rect(X(xy), Y(xy), w, h, w0 & 0xFFFFFFu, tex, (cmd & 1u) != 0, (cmd & 2u) != 0,
                     static_cast<int>(uv & 0xFFu), static_cast<int>((uv >> 8) & 0xFFu), uv >> 16, tpage);
                i += size;
                continue;
            }
            if (cmd == 0xE1) { tpage = w0 & 0x7FFu; ++i; continue; }
            if (cmd == 0xE2) {
                if ((w0 & 0xFFFFFu) != 0) notes.push_back("a texture window (ignored)");
                ++i;
                continue;
            }
            if (cmd == 0xE3) { clipX0 = static_cast<int>(w0 & 0x3FFu) - bufX; clipY0 = static_cast<int>((w0 >> 10) & 0x1FFu) - bufY; ++i; continue; }
            if (cmd == 0xE4) { clipX1 = static_cast<int>(w0 & 0x3FFu) - bufX; clipY1 = static_cast<int>((w0 >> 10) & 0x1FFu) - bufY; ++i; continue; }
            if (cmd == 0xE5) {
                offX = static_cast<int>(static_cast<int32_t>(w0 << 21) >> 21);
                offY = static_cast<int>(static_cast<int32_t>((w0 >> 11) << 21) >> 21);
                ++i;
                continue;
            }
            if (cmd == 0xE6) { ++i; continue; }
            char b[64];
            std::snprintf(b, sizeof(b), "GP0 0x%02X (not rasterised)", cmd);
            notes.push_back(b);
            ++skipped;
            break;
        }
    }

    // DrawOTag from the table's last entry.
    void WalkOt(uint32_t ot, uint32_t entries) {
        uint32_t p = ot + 4u * (entries - 1u);
        for (int guard = 0; guard < 100000; ++guard) {
            const uint32_t tag = Word(p);
            const uint32_t len = tag >> 24;
            if (len != 0) Packet(p + 4u, len);
            const uint32_t next = tag & 0xFFFFFFu;
            if (next == 0xFFFFFFu) return;
            p = 0x80000000u | next;
        }
        notes.push_back("the ordering table does not end");
    }
};

void ClearOtR(Memory& m, uint32_t ot, uint32_t n) {
    for (uint32_t i = 0; i < n; ++i) m.PokeWord(ot + 4u * i, i == 0 ? 0x00FFFFFFu : ((ot + 4u * (i - 1u)) & 0xFFFFFFu));
}

bool ParseArgs(int argc, char** argv, Opt& o) {
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : std::string(); };
        if (a == "--disc") o.disc = next();
        else if (a == "--state") o.state = next();
        else if (a == "--script") o.script = next();
        else if (a == "--frames") o.frames = std::atoi(next().c_str());
        else if (a == "--out") o.out = next();
        else if (a == "--min-match") o.minMatch = std::atof(next().c_str());
        else if (a == "--max-extra") o.maxExtra = std::atol(next().c_str());
        else if (a == "--calls") o.calls = true;
        else return false;
    }
    return !o.disc.empty() && !o.state.empty();
}

} // namespace

int CmdMenuPrims(int argc, char** argv) {
    Opt o;
    if (!ParseArgs(argc, argv, o)) {
        std::fprintf(stderr, "usage: rrverify menuprims --disc <bin> --state <retro-shell> --script \"f:keys;...\" "
                             "--frames N --out <prefix> [--min-match P] [--max-extra N]\n");
        return 2;
    }
    namespace sh = rr::shell;
    // ---- ours: the product's front end driven by the script.
    rr::DiscImage disc(o.disc);
    sh::FrontEnd fe(disc, 0);
    std::vector<std::pair<int, std::string>> events;
    for (size_t at = 0; at < o.script.size();) {
        const size_t semi = o.script.find(';', at);
        const std::string e = o.script.substr(at, semi == std::string::npos ? std::string::npos : semi - at);
        const size_t colon = e.find(':');
        if (colon != std::string::npos) events.push_back({std::atoi(e.substr(0, colon).c_str()), e.substr(colon + 1)});
        if (semi == std::string::npos) break;
        at = semi + 1;
    }
    std::vector<uint8_t> before;
    for (int f = 1; f <= o.frames; ++f) {
        sh::ShellKeys k;
        for (const auto& ev : events) {
            if (ev.first != f) continue;
            for (size_t at = 0; at < ev.second.size();) {
                const size_t comma = ev.second.find(',', at);
                const std::string key = ev.second.substr(at, comma == std::string::npos ? std::string::npos : comma - at);
                if (key == "up") k.up = true;
                else if (key == "down") k.down = true;
                else if (key == "left") k.left = true;
                else if (key == "right") k.right = true;
                else if (key == "x") k.cross = true;
                else if (key == "t") k.triangle = true;
                else if (key == "s") k.square = true;
                else if (key == "c") k.circle = true;
                else if (key.size() > 1 && key[0] == 'g') sh::GotoScreen(fe.Ram(), std::atoi(key.c_str() + 1));
                else if (key.size() > 1 && key[0] == 'v') fe.Ram().W8(sh::kSession + 4u, static_cast<uint8_t>(std::atoi(key.c_str() + 1)));
                if (comma == std::string::npos) break;
                at = comma + 1;
            }
        }
        // The shell as it stands before the last frame: the original's pass is run on it, so the frame it
        // draws is the same frame step the product's last frame drew (the stat bars and the arrows animate
        // one step a pass). The last frame of a script presses nothing.
        if (f == o.frames) before = fe.Arena().ram;
        fe.Frame(k);
    }
    std::vector<sh::ShellView::BlitNote> blits;
    fe.MutableView().SetBlitLog(&blits);
    std::vector<uint8_t> ours;
    fe.Draw(ours);
    fe.MutableView().SetBlitLog(nullptr);
    std::printf("ours: %s\n", fe.Status().c_str());
    if (o.calls)
        std::printf("  ours: fe+0x17 %d, fe+0x13 0x%02X, last item %08X, BLAW record flags %08X\n", fe.Ram().S8(sh::kFeValid),
                    fe.Ram().U8(sh::kFeEdges), fe.Ram().U32(sh::kFeLastItem), fe.Ram().U32(0x8009DDE0u + 2u * 0x24u));
    if (o.calls)
        for (const sh::TextCall& t : fe.Callees().texts)
            std::printf("  ours call widget %08X ot %08X %s%s%s sprite %s src %d,%d %dx%d at %d,%d %dx%d rgb %06X \"%s\"\n",
                        t.widget, t.ot, t.tile ? "tile " : "", t.box ? "box " : "", t.quad ? "quad " : "",
                        t.sprite ? FourCC(t.sprite).c_str() : "-", t.sx, t.sy, t.sw, t.sh, t.x, t.y, t.w, t.h, t.rgb,
                        t.text.c_str());

    // ---- the original: the capture with our shell state planted, its widget pass run.
    Memory mem;
    Cpu cpu(mem);
    rr::interp::SnapshotInfo info;
    std::string error;
    if (!rr::interp::LoadSnapshot(o.state, mem, cpu, info, error)) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }
    Vram vram;
    if (!LoadVram(o.state + "\\vram.bin", vram)) {
        std::fprintf(stderr, "cannot read %s\\vram.bin\n", o.state.c_str());
        return 1;
    }
    const std::vector<uint8_t>& arena = before;
    auto plant = [&](uint32_t from, uint32_t to) {
        mem.WriteBlock(from, arena.data() + (from & 0x1FFFFFu), to - from);
    };
    const uint32_t otKeep = mem.PeekWord(kOtPtr), ot2Keep = mem.PeekWord(kOtPtr2);
    plant(0x8005B5E8u, kSpriteRecs);       // RASHCDF's code, data and bss below the sprite records
    plant(sh::kSession, sh::kSession + 0x220u); // the session and the six player records
    mem.PokeWord(kOtPtr, otKeep);          // the machine's own ordering tables
    mem.PokeWord(kOtPtr2, ot2Keep);
    // The attract idle 0x8005ACAC as the last frame's pass saw it (the vsync callback and the input pass
    // have already counted or reset it when the widget pass runs): the panel films' gate 0x8006E6F4 reads it.
    mem.PokeWord(0x8005ACACu, fe.Ram().U32(sh::kIdle));
    // The
    // film library and the file layer it reaches are the console's CD and MDEC (hardware the interpreter
    // does not have): each is answered at its entry the way the product's library answers (below), and
    // its calls are compared with the product's.
    // The shared VRAM slots' occupants in the capture (0x800700F0: titles 0x800A082C, logos and pictures
    // 0x800A0814, 0x800705DC's 0x800A0810).
    const uint32_t slotTitle = mem.PeekWord(0x800A082Cu), slotLogo = mem.PeekWord(0x800A0814u), slotShared = mem.PeekWord(0x800A0810u);
    std::vector<uint32_t> captureFlags(300); // the sprite records as the capture holds them, before the pass
    for (uint32_t i = 0; i < 300u; ++i) captureFlags[i] = mem.PeekWord(kSpriteRecs + 36u * i);
    ClearOtR(mem, otKeep, 72);
    ClearOtR(mem, ot2Keep, 72);
    const uint32_t env = mem.PeekWord(0x8005B470u);
    const uint32_t cursor0 = mem.PeekWord(env + 0x10Cu);
    cpu.trapOnBiosVector = false; // the text calls reach strlen through the A0 vector
    const uint32_t sp = 0x801FF000u;
    const uint32_t screens = mem.PeekWord(sh::kScreenTablePtr);
    const int cur = static_cast<int16_t>(mem.PeekByte(sh::kFeCur) | (mem.PeekByte(sh::kFeCur + 1u) << 8));
    int passes = 0, uploads = 0;
    Raster raster(mem, vram);
    {
        const uint32_t b = env + 0x70u * mem.PeekByte(env + 6u);
        raster.bufX = static_cast<int16_t>(raster.Half(b + 24u));
        raster.bufY = static_cast<int16_t>(raster.Half(b + 26u));
    }
    cpu.AddHook(0x80048A6Cu, "LoadImage");
    struct FilmStub {
        uint32_t address;
        const char* name;
    };
    static const FilmStub kFilmStubs[] = {{0x8005F36Cu, "FilmStart"}, {0x8005F484u, "FilmPicture"}, {0x8005F7E0u, "FilmStop"},
                                          {0x8001458Cu, "FileOpen"},  {0x8001460Cu, "FileClose"}};
    for (const FilmStub& f : kFilmStubs) cpu.AddHook(f.address, f.name);
    std::vector<sh::PanelFilms::Call> origFilm;
    if (o.calls) {
        cpu.AddHook(sh::kTextId, "DrawStringId");
        cpu.AddHook(sh::kTextStr, "DrawString");
        cpu.AddHook(sh::kTextAt, "DrawStringIdAt");
    }
    auto tick = [&](int i) -> bool {
        const uint32_t s = mem.PeekWord(screens + 4u * static_cast<uint32_t>(i));
        if (s == 0) return true;
        const uint16_t flags = static_cast<uint16_t>(mem.PeekByte(s) | (mem.PeekByte(s + 1u) << 8));
        const int16_t t = static_cast<int16_t>(mem.PeekByte(s + 2u) | (mem.PeekByte(s + 3u) << 8));
        // ScreenTick's live arm: the screen's tick handler (the table 0x8009CFD0) - 0x8006D630 sets fe+0x11
        // and runs the widget pass; the films' (0x8006DE5C) and screen 57's (0x8006E008) are not frames.
        const uint32_t th = mem.PeekWord(0x8009CFD0u + 4u * static_cast<uint32_t>(i));
        if (!(flags & 2u) || t != 0 || th == 0 || th == 0x8006DE5Cu || th == 0x8006E008u) return true;
        const uint16_t id = static_cast<uint16_t>(i);
        mem.WriteBlock(sh::kFe + 0x08u, &id, 2);
        const uint8_t layer = mem.PeekByte(s + 0x0Cu);
        mem.WriteBlock(sh::kFe + 0x11u, &layer, 1);
        cpu.regs[4] = s;
        cpu.regs[29] = sp;
        cpu.regs[31] = kSentinel;
        cpu.pc = th;
        cpu.npc = th + 4u;
        cpu.loadDelayReg = Cpu::kNoLoadDelay;
        Trap tr;
        for (;;) {
            tr = cpu.Run(kSentinel, 50'000'000);
            if (tr.kind != TrapKind::Hook) break;
            {
                const std::string& hn = cpu.lastHookName();
                if (hn == "FilmStart" || hn == "FilmPicture" || hn == "FilmStop" || hn == "FileOpen" || hn == "FileClose") {
                    uint32_t v0 = 0;
                    if (hn == "FilmStart") {
                        origFilm.push_back({0x8005F36Cu, static_cast<int32_t>(cpu.regs[7]),
                                            static_cast<int32_t>(mem.PeekWord(cpu.regs[29] + 16u))});
                        v0 = 1;
                    } else if (hn == "FilmPicture") {
                        origFilm.push_back({0x8005F484u, static_cast<int32_t>(cpu.regs[4]), static_cast<int32_t>(cpu.regs[5])});
                        v0 = 1;
                    } else if (hn == "FilmStop") {
                        origFilm.push_back({0x8005F7E0u, static_cast<int32_t>(cpu.regs[4]), 0});
                    } else if (hn == "FileOpen") {
                        v0 = 0; // a handle
                    }
                    cpu.regs[2] = v0;
                    cpu.pc = cpu.regs[31];
                    cpu.npc = cpu.pc + 4u;
                    continue;
                }
            }
            if (cpu.lastHookName() != "LoadImage") { // a text call, listed with its caller (--calls)
                const uint32_t a1 = cpu.regs[5];
                std::string text;
                uint32_t p = a1;
                if (cpu.lastHookName() != "DrawString") {
                    const uint32_t table = mem.PeekWord(0x8005B544u);
                    p = a1 < 1945u ? mem.PeekWord(table + 4u * a1) : 0u;
                }
                for (uint32_t k = 0; p != 0 && k < 64u; ++k) {
                    const uint8_t ch = mem.PeekByte(p + k);
                    if (ch == 0) break;
                    text.push_back(static_cast<char>(ch));
                }
                std::printf("  call %s from 0x%08X: \"%s\"\n", cpu.lastHookName().c_str(), cpu.regs[31] - 8u, text.c_str());
                cpu.Step();
                continue;
            }
            // SLUS LoadImage(rect*, pixels*) - the sprite upload 0x80065768 makes for a loaded record not
            // in VRAM yet: done into the capture's VRAM copy, then back to the caller.
            const uint32_t rc = cpu.regs[4], src = cpu.regs[5];
            const int x = static_cast<int16_t>(raster.Half(rc)), y = static_cast<int16_t>(raster.Half(rc + 2u));
            const int w = static_cast<int16_t>(raster.Half(rc + 4u)), h = static_cast<int16_t>(raster.Half(rc + 6u));
            for (int yy = 0; yy < h; ++yy)
                for (int xx = 0; xx < w; ++xx)
                    vram.px[static_cast<size_t>((y + yy) & 511) * 1024u + static_cast<size_t>((x + xx) & 1023)] =
                        raster.Half(src + 2u * static_cast<uint32_t>(yy * w + xx));
            std::string who = "?";
            for (uint32_t r = 0; r < 300u; ++r)
                if (mem.PeekWord(kSpriteRecs + 36u * r + 4u) + 0x10u == src) who = FourCC(mem.PeekWord(kSpriteIds + 4u * r));
            raster.uploaded.push_back({{x, y, w, h}, who});
            ++uploads;
            cpu.regs[2] = 0;
            cpu.pc = cpu.regs[31];
            cpu.npc = cpu.pc + 4u;
        }
        ++passes;
        if (tr.kind != TrapKind::Halted) {
            std::printf("the original's widget pass for screen %d stopped: %s\n", i, tr.ToString().c_str());
            return false;
        }
        return true;
    };
    bool ran = true;
    for (int i = 0; i < sh::kScreenCount; ++i)
        if (i != cur) ran = tick(i) && ran;
    ran = tick(cur) && ran;
    const uint32_t cursor1 = mem.PeekWord(env + 0x10Cu);
    // The second table (0x8009CCE4, the backdrop's) is under the first: drawn before it.
    raster.WalkOt(ot2Keep, 72);
    raster.WalkOt(otKeep, 72);
    std::printf("original: %d sprite uploads (LoadImage) into the capture's VRAM\n", uploads);
    std::printf("original: %d widget passes, packet buffer 0x%08X..0x%08X (%u bytes), %ld primitives rasterised, %ld not\n",
                passes, cursor0, cursor1, cursor1 - cursor0, raster.prims, raster.skipped);
    for (const std::string& n : raster.notes) std::printf("  note: %s\n", n.c_str());
    if (o.calls) {
        for (const Source& s : raster.sources) std::printf("  original source %-36s %ld px drawn\n", s.name.c_str(), s.drawn);
        for (const auto& b : blits) std::printf("  ours blit %-12s at %d,%d %dx%d\n", b.id.c_str(), b.x, b.y, b.w, b.h);
    }

    // ---- what the product draws that the capture cannot show: pictures not resident in its VRAM - a
    // record of a shared slot (the titles 0x800000, the logos and bike pictures 0x400000, 0x100000) that
    // is not the slot's occupant (0x20000000): the original's emitter asks 0x8007A400 to decode it and
    // draws the slot as it is this frame. Films are not primitives.
    std::map<std::string, bool> resident;
    for (uint32_t i = 0; i < 300u; ++i) {
        const uint32_t f = captureFlags[i], id = mem.PeekWord(kSpriteIds + 4u * i);
        bool in = (f & 0x50000000u) != 0; // loaded, or a composite (BAC1/BAC2) of loaded parts
        if (f & 0x800000u) in = in && id == slotTitle;
        if (f & 0x400000u) in = in && id == slotLogo;
        if (f & 0x100000u) in = in && id == slotShared;
        resident[FourCC(id)] = in;
    }
    std::vector<uint8_t> mask(static_cast<size_t>(W) * H, 0);
    std::map<std::string, long> maskedBy;
    for (const auto& b : blits) {
        const auto it = resident.find(b.id);
        if (it != resident.end() && it->second) continue;
        for (int y = std::max(0, b.y); y < std::min(H, b.y + b.h); ++y)
            for (int x = std::max(0, b.x); x < std::min(W, b.x + b.w); ++x) {
                const size_t k = static_cast<size_t>(y) * W + static_cast<size_t>(x);
                if (!mask[k]) ++maskedBy[b.id];
                mask[k] = 1;
            }
    }

    // ---- compare.
    std::vector<uint8_t> side(static_cast<size_t>(W) * 2u * H * 2u * 4u, 0);
    long compared = 0, close = 0, onlyOrig = 0, onlyOurs = 0, masked = 0;
    std::map<std::string, std::array<long, 2>> bySource; // original source -> {pixels, matching}
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const size_t k = static_cast<size_t>(y) * W + static_cast<size_t>(x);
            const uint8_t* a = &raster.rgb[k * 3u];
            const uint8_t* b = &ours[k * 4u];
            uint8_t* l = &side[(static_cast<size_t>(y) * W * 2u + static_cast<size_t>(x)) * 4u];
            uint8_t* r = &side[(static_cast<size_t>(y) * W * 2u + static_cast<size_t>(W + x)) * 4u];
            l[0] = a[0], l[1] = a[1], l[2] = a[2], l[3] = 255;
            r[0] = b[0], r[1] = b[1], r[2] = b[2], r[3] = 255;
            uint8_t* d = &side[(static_cast<size_t>(H + y) * W * 2u + static_cast<size_t>(x)) * 4u];
            d[3] = 255;
            if (mask[k]) {
                ++masked;
                d[0] = d[1] = d[2] = 40;
                continue;
            }
            const bool origDrawn = raster.owner[k] >= 0;
            const bool oursDrawn = b[0] | b[1] | b[2];
            const bool ok = std::abs(a[0] - b[0]) <= 24 && std::abs(a[1] - b[1]) <= 24 && std::abs(a[2] - b[2]) <= 24;
            ++compared;
            if (ok) ++close;
            // The product's frame has no coverage mask: a pixel is "drawn by ours" when it is not black. A
            // black texel the original drew over black (RRLG's opaque black, the MDEC's mask bit) is the same
            // picture, not a pixel only one side drew.
            if (origDrawn && !oursDrawn && std::max({a[0], a[1], a[2]}) > 24) { // (black within the 24 levels)
                ++onlyOrig;
                if (o.calls && onlyOrig <= 8)
                    std::printf("  drawn only by the original at %d,%d: %s rgb %d,%d,%d\n", x, y,
                                raster.sources[static_cast<size_t>(raster.owner[k])].name.c_str(), a[0], a[1], a[2]);
            }
            if (!origDrawn && oursDrawn) ++onlyOurs;
            if (origDrawn) {
                auto& s = bySource[raster.sources[static_cast<size_t>(raster.owner[k])].name];
                ++s[0];
                if (ok) ++s[1];
            }
            if (!ok) {
                d[0] = origDrawn ? 255 : 0;
                d[1] = oursDrawn ? 255 : 0;
                d[2] = 64;
            } else {
                d[0] = static_cast<uint8_t>(a[0] / 4), d[1] = static_cast<uint8_t>(a[1] / 4), d[2] = static_cast<uint8_t>(a[2] / 4);
            }
        }
    rr::WritePng(o.out + "_orig_vs_ours.png", W * 2, H * 2, side);
    std::printf("sources the original drew (pixels on top / matching ours within 24):\n");
    for (const auto& [name, s] : bySource) std::printf("  %-36s %6ld  %6ld  %.1f%%\n", name.c_str(), s[0], s[1], 100.0 * s[1] / std::max(1L, s[0]));
    for (const auto& [name, n] : maskedBy) std::printf("  masked (not resident in the capture's VRAM): %-10s %ld px\n", name.c_str(), n);
    const double pct = 100.0 * static_cast<double>(close) / static_cast<double>(std::max(1L, compared));
    // The panel films (shell_panel.h): the library calls of the original's pass on this state against the
    // product's last frame's - which film widget opens, steps or stops a film, and where its picture goes.
    auto filmName = [](uint32_t a) { return a == 0x8005F36Cu ? "start" : a == 0x8005F484u ? "picture" : "stop"; };
    const auto& ourFilm = fe.Films().Calls();
    bool filmSame = ourFilm.size() == origFilm.size();
    for (size_t i = 0; filmSame && i < ourFilm.size(); ++i)
        filmSame = ourFilm[i].address == origFilm[i].address && ourFilm[i].x == origFilm[i].x && ourFilm[i].y == origFilm[i].y;
    std::string oursList, origList;
    for (const auto& c : ourFilm) oursList += std::string(" ") + filmName(c.address) + "(" + std::to_string(c.x) + "," + std::to_string(c.y) + ")";
    for (const auto& c : origFilm) origList += std::string(" ") + filmName(c.address) + "(" + std::to_string(c.x) + "," + std::to_string(c.y) + ")";
    std::printf("panel films: the attract idle 0x8005ACAC %d before the last frame, %d after; fe+0x0A 0x%04X\n",
                static_cast<int32_t>(before[0x5ACAC] | (before[0x5ACAD] << 8) | (before[0x5ACAE] << 16) | (before[0x5ACAF] << 24)),
                fe.Ram().S32(sh::kIdle), fe.Ram().U16(sh::kFeMedia));
    for (const auto& s : fe.Films().Shown())
        std::printf("panel films: ours shows %s picture %d at %d,%d\n", s.name.c_str(), s.picture, s.x, s.y);
    std::printf("panel films: the original's library calls:%s | ours:%s -> %s\n", origList.empty() ? " none" : origList.c_str(),
                oursList.empty() ? " none" : oursList.c_str(), filmSame ? "same" : "DIFFERENT");
    const bool pass = ran && filmSame && pct >= o.minMatch && onlyOurs + onlyOrig <= o.maxExtra;
    std::printf("menuprims: %ld pixels compared (%ld masked), %.2f%% within 24 levels, drawn only by the original %ld, "
                "only by ours %ld -> %s\nwrote %s_orig_vs_ours.png (top: left the original, right ours; below: the "
                "differing pixels, red = the original drew, green = ours drew)\n",
                compared, masked, pct, onlyOrig, onlyOurs, pass ? "PASS" : "FAIL", o.out.c_str());
    return pass ? 0 : 1;
}
