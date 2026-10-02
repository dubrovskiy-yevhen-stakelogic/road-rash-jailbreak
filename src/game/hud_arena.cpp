#include "game/hud_arena.h"
#include "game/sim/modes.h" // HudClockDigits SLUS 0x80013B90

#include "game/sim/fixed.h"
#include "game/sim/hud.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>

namespace rr::game {
namespace {

using rr::sim::GuestRam;

constexpr uint32_t kGameStatePtr = 0x8005B2F8;
constexpr uint32_t kTextures = 0x800D49B0; // 8 bytes each: +0 page, +2 clut, +4 x, +5 y, +6 ?
constexpr uint32_t kArts = rr::sim::kHudArtTable;
constexpr uint32_t kTextureCount = 0x8005B388;
constexpr uint32_t kClutTableX = 0x8005B2B4, kClutTableY = 0x8005B2BC; // u16, the CSV's ClutTable line
constexpr uint32_t kRadarDist = 0x8005B258, kNitroAt = 0x8005B2C8, kTimerAt = 0x8005B2C0, kRadarBase = 0x8005B238;
constexpr uint32_t kLayoutDone = 0x8005ACE0, kClockReset = 0x8005ACE4, kItemBuffer = 0x8005B2F0;
constexpr uint32_t kRadarRect = 0x800D63E8, kRadarScale = 0x8005B234;
constexpr uint32_t kFontInitDone = 0x8005AE74, kLocBuffer = 0x8005AE78;
constexpr uint32_t kFontIndex = 0x8005B540, kFontCluts = 0x8005B54C, kFontX = 0x8005B54E, kFontY = 0x8005B550;
constexpr uint32_t kFontRecordPtr = 0x8005B554, kFontPageX = 0x8005B548, kFontPageW = 0x8005B54A;
constexpr uint32_t kVramOrigins = 0x800533B4; // SLUS data: 88-byte records by player count; +77/+78
constexpr uint32_t kFontClutData = 0x80053A68; // SLUS data: the font's 16-entry CLUT
constexpr uint32_t kSplitScratchA = 0x800D9C40, kSplitScratchB = 0x800D5F60, kLaneLength = 0x8005B244;
constexpr uint32_t kHudDirtyFlag = 0x8005ACD4, kHudGate = 0x8005AD20;
constexpr uint32_t kRashcdiBase = 0x8005B5E8;
constexpr uint32_t kBarColourInRashcdi = 0x8005B738; // RASHCDI data: {r, g, b, 0} of the graph tiles

uint32_t Items(GuestRam& g, int32_t p) { return g.U32(rr::sim::kHudItemsPtr + 4u * static_cast<uint32_t>(p)); }
int32_t Players(GuestRam& g) { return g.S32(g.U32(kGameStatePtr) + 48u); }
uint32_t RaceType(GuestRam& g) { return g.U8(g.U32(kGameStatePtr) + 4u); }
void Zero(GuestRam& g, uint32_t a, uint32_t n) {
    for (uint32_t k = 0; k < n; ++k) g.W8(a + k, 0);
}

// BIOS A(10h) atoi: leading blanks, an optional sign, decimal digits.
int32_t Atoi(const std::vector<uint8_t>& b, size_t at) {
    auto c = [&](size_t i) -> uint8_t { return i < b.size() ? b[i] : 0; };
    while (c(at) == ' ' || c(at) == '\t') ++at;
    bool neg = false;
    if (c(at) == '-' || c(at) == '+') neg = c(at++) == '-';
    int32_t v = 0;
    while (c(at) >= '0' && c(at) <= '9') v = v * 10 + (c(at++) - '0');
    return neg ? -v : v;
}

// RASHCDI 0x8005F104: one CSV line from `at`. Every comma becomes a NUL and the character after it a
// field start; the first five fields go to `out` (u16, atoi), the rest are dropped; the answer is the
// characters consumed up to the CR, plus the CR/LF pair. At most 80 characters are looked at.
size_t Tokenize(std::vector<uint8_t>& b, size_t at, uint16_t out[5]) {
    auto c = [&](size_t i) -> uint8_t { return i < b.size() ? b[i] : 0; };
    size_t fields[80];
    int32_t count = 0, n = 0;
    size_t p = at;
    uint8_t ch = c(p);
    if (ch != 13) {
        while (n < 80) {
            if (ch == ',') {
                fields[count++] = p + 1;
                if (p < b.size()) b[p] = 0;
            }
            ++p;
            ch = c(p);
            ++n;
            if (ch == 13) break;
        }
    }
    for (int32_t i = 0; i < 5; ++i)
        out[i] = (i < count) ? static_cast<uint16_t>(Atoi(b, fields[i])) : 0;
    return static_cast<size_t>(n) + 2u;
}

// SLUS 0x8004CD84 GetClut.
uint16_t GetClut(int32_t x, int32_t y) { return static_cast<uint16_t>((y << 6) | ((x >> 4) & 0x3F)); }

// SLUS 0x8004CE44 (libgpu SetDrawMode with a texture window argument; the loader passes none).
void SetDrawModeTw(GuestRam& g, uint32_t p, uint32_t dfe, uint32_t dtd, uint32_t tpage) {
    g.W8(p + 3u, 2);
    uint32_t w = (dtd != 0) ? 0xE1000200u : 0xE1000000u;
    uint32_t t = tpage & 0x9FFu;
    if (dfe != 0) t |= 0x400u;
    g.W32(p + 4u, w | t);
    g.W32(p + 8u, 0);
}

std::vector<uint8_t> ReadDiscFile(const DiscImage& disc, const std::string& path) {
    const auto f = disc.Find(path);
    if (!f) return {};
    return disc.ReadFile(*f);
}

// ---------------------------------------------------------------------------- RASHCDI 0x8005ED94
void HudAlloc(GuestRam& g, const HudPlacement& at) {
    const int32_t np = Players(g);
    g.W32(kItemBuffer, at.items);
    for (int32_t p = 0; p < np; ++p) {
        g.W32(rr::sim::kHudFinished + 4u * static_cast<uint32_t>(p), 0);
        g.W32(rr::sim::kHudItemsPtr + 4u * static_cast<uint32_t>(p), at.items + 3960u * static_cast<uint32_t>(p));
    }
    g.W32(kTextureCount, 0);
    for (uint32_t i = 0; i < 22; ++i) {
        g.W16(kTextures + 8u * i + 2u, 0xFFFF);
        g.W16(kTextures + 8u * i + 0u, 0xFFFF);
    }
    for (uint32_t i = 0; i < 62; ++i) {
        g.W16(kArts + 16u * i + 6u, 0xFFFF);
        g.W16(kArts + 16u * i + 10u, 0xFFFF);
    }
    for (int32_t p = 0; p < np; ++p)
        for (uint32_t i = 0; i < 110; ++i) {
            const uint32_t it = Items(g, p) + 36u * i;
            g.W32(it + 20u, 0);
            g.W16(it + 30u, 0);
            g.W16(it + 28u, 0);
        }
    g.W32(rr::sim::kHudTarget + 4u, 0);
    g.W32(rr::sim::kHudTarget, 0);
    g.W32(rr::sim::kHudEnabled, 1);
    g.W32(rr::sim::kHudFrameCount, 0);
    // 0x8005EBDC: the split layout, -1 unless two players - then SLUS 0x8001C498's answer, the view mode
    // *(0x800D6C68) the race's VIEWS.VI load and SLUS 0x8001B67C left (mp_session.cpp).
    g.W32(rr::sim::kHudSplitMode, np == 2 ? g.U32(0x800D6C68u) : 0xFFFFFFFFu);
}

// ---------------------------------------------------------------------------- RASHCDI 0x8005F1E4
bool ParseCsv(GuestRam& g, std::vector<uint8_t> b) {
    const int32_t np = Players(g);
    size_t at = 0;
    uint16_t f[5];
    at += Tokenize(b, at, f); // ClutTable
    g.W16(kClutTableX, f[0]);
    g.W16(kClutTableY, f[1]);
    auto timerLine = [&](std::initializer_list<uint32_t> offsets) {
        at += Tokenize(b, at, f);
        for (int32_t p = 0; p < np; ++p) {
            const uint32_t d = rr::sim::kHudDash224 + 224u * static_cast<uint32_t>(p);
            for (uint32_t o : offsets) {
                Zero(g, d + o, 32);
                g.W32(d + o + 4u, f[0]);
                g.W32(d + o + 8u, f[1]);
                g.W32(d + o + 24u, f[2]);
                g.W32(d + o + 28u, f[3]);
            }
        }
    };
    timerLine({0u, 128u, 192u}); // SignFlash: the sign, the TKO icon, the red bars
    timerLine({32u});            // NitroFlash
    timerLine({64u});            // DemoFlash
    timerLine({160u});           // SplitTimeFlash
    timerLine({96u});            // MsgFlash
    for (uint32_t which = 0; which < 2; ++which) { // PSlide, OSlide
        at += Tokenize(b, at, f);
        for (int32_t p = 0; p < np; ++p) {
            const uint32_t s = rr::sim::kHudSlide72 + 72u * static_cast<uint32_t>(p) + 36u * which;
            for (uint32_t k = 0; k < 5; ++k) g.W32(s + 8u + 4u * k, f[k]);
        }
    }
    for (int32_t p = 0; p < np; ++p) {
        const uint32_t pp = 4u * static_cast<uint32_t>(p);
        for (uint32_t dst : {kRadarDist, kNitroAt, kTimerAt}) {
            at += Tokenize(b, at, f);
            g.W16(dst + pp, f[0]);
            g.W16(dst + pp + 2u, f[1]);
        }
        at += Tokenize(b, at, f);
        g.W16(kRadarBase + pp, f[0]);
        g.W16(kRadarBase + pp + 2u, f[1]);
    }
    at += Tokenize(b, at, f); // TexArtDashCounts
    g.W32(kTextureCount, f[0]);
    if (f[0] != 0)
        for (uint32_t i = 0; i < g.U32(kTextureCount); ++i) {
            at += Tokenize(b, at, f);
            const uint32_t t = kTextures + 8u * i;
            g.W8(t + 4u, static_cast<uint8_t>(f[0]));
            g.W8(t + 5u, static_cast<uint8_t>(f[1]));
            g.W16(t + 6u, f[2]);
        }
    for (uint32_t i = 0; i < 62; ++i) {
        at += Tokenize(b, at, f);
        const uint32_t a = kArts + 16u * i;
        const uint8_t x = static_cast<uint8_t>(f[0]), y = static_cast<uint8_t>(f[1]);
        g.W8(a + 4u, x);
        g.W8(a + 5u, y);
        g.W8(a + 8u, static_cast<uint8_t>(x + static_cast<uint8_t>(f[2]) - 1u));
        g.W8(a + 9u, y);
        g.W8(a + 12u, x);
        g.W8(a + 13u, static_cast<uint8_t>(y + static_cast<uint8_t>(f[3]) - 1u));
        g.W8(a + 14u, static_cast<uint8_t>(x + static_cast<uint8_t>(f[2]) - 1u));
        g.W8(a + 15u, static_cast<uint8_t>(y + static_cast<uint8_t>(f[3]) - 1u));
        g.W8(a + 2u, static_cast<uint8_t>(f[2]));
        g.W8(a + 3u, static_cast<uint8_t>(f[3]));
        g.W16(a + 0u, f[4]);
    }
    for (int32_t p = 0; p < np; ++p) {
        const uint32_t base = Items(g, p);
        for (uint32_t it = base; it < base + 3960u; it += 36u) {
            at += Tokenize(b, at, f);
            g.W16(it + 24u, f[0]);
            g.W8(it + 33u, 0xFF);
            g.W16(it + 34u, 0);
            g.W16(it + 26u, f[1]);
            if (f[4] == 0) {
                g.W8(it + 33u, static_cast<uint8_t>(f[2]));
                g.W16(it + 34u, static_cast<uint16_t>(f[3] & 1u));
            } else if (f[4] != 2) {
                g.W16(it + 34u, static_cast<uint16_t>(f[3] & 1u));
            }
            g.W8(it + 32u, static_cast<uint8_t>(f[4]));
        }
    }
    return !g.Faulted();
}

// ---------------------------------------------------------------------------- RASHCDI 0x8005FA88
bool HudLayout(GuestRam& g, const DiscImage& disc, bool noArtFixup, std::string& report) {
    if (g.U32(rr::sim::kHudEnabled) == 0) return true;
    const int32_t np = Players(g);
    const char* csv = "DATA/DASH1P.CSV";
    if (np != 1) {
        const int32_t split = g.S32(rr::sim::kHudSplitMode);
        csv = (split == 2) ? "DATA/DASH2PS.CSV" : (split == 0 ? "DATA/DASH2PH.CSV" : "DATA/DASH2PV.CSV");
    }
    const std::vector<uint8_t> text = ReadDiscFile(disc, csv);
    if (text.empty()) {
        report += std::string("  ") + csv + " is not on the disc\n";
        return false;
    }
    if (!ParseCsv(g, text)) return false;
    // 0x8005F9EC per texture: its tpage column and its CLUT slot (row ClutTableY + i/4, block i%4).
    for (uint32_t i = 0; i < g.U32(kTextureCount); ++i) {
        const uint32_t t = kTextures + 8u * i;
        const int32_t page = (((g.U8(t + 4u) >> 2) + 960) & 0x3FF) >> 6;
        g.W16(t + 0u, static_cast<uint16_t>(page));
        int32_t cx = static_cast<int32_t>(g.U16(kClutTableX)) + static_cast<int32_t>((i & 3u) << 6);
        if (cx < 0) cx += 3;
        cx = (cx >> 2) + 960;
        const int32_t cy = static_cast<int16_t>(g.U16(kClutTableY) + static_cast<uint16_t>(i >> 2));
        g.W16(t + 2u, GetClut(static_cast<int16_t>(cx), cy));
    }
    // 0x8005EFFC per art: its texture's page and CLUT, and its rectangle moved to the texture's origin.
    for (uint32_t i = 0; i < 62; ++i) {
        const uint32_t a = kArts + 16u * i;
        const uint32_t t = kTextures + 8u * g.U16(a);
        g.W16(a + 10u, g.U16(t + 0u));
        g.W16(a + 6u, g.U16(t + 2u));
        if (noArtFixup) continue; // the check's negative control
        const uint8_t tx = g.U8(t + 4u), ty = g.U8(t + 5u);
        for (uint32_t o : {4u, 8u, 12u, 14u}) g.W8(a + o, static_cast<uint8_t>(g.U8(a + o) + tx));
        for (uint32_t o : {5u, 9u, 13u, 15u}) g.W8(a + o, static_cast<uint8_t>(g.U8(a + o) + ty));
    }
    for (int32_t p = 0; p < np; ++p) {
        const uint32_t base = Items(g, p);
        for (uint32_t i = 0; i < 110; ++i) {
            const uint32_t it = base + 36u * i;
            const int32_t art = g.S8(it + 33u);
            if (art >= 0) rr::sim::HudSetArtFull(g, it, kArts + 16u * static_cast<uint32_t>(art));
        }
        // The career and the Five-O layouts move the radar, nitro and clock blocks (0x8005FC58).
        const uint32_t type = RaceType(g);
        const uint32_t gs = g.U32(kGameStatePtr);
        const uint32_t pp = 4u * static_cast<uint32_t>(p);
        if (type == 33 || ((type & 1u) && g.S32(gs + 72u + pp) >= 18)) {
            auto shift = [&](uint32_t anchor, int32_t first, int32_t endExclusive, uint32_t head) {
                const int32_t dx = g.S16(anchor + pp) - static_cast<int32_t>(g.U16(base + 36u * static_cast<uint32_t>(head) + 24u));
                const int32_t dy = g.S16(anchor + pp + 2u) - static_cast<int32_t>(g.U16(base + 36u * static_cast<uint32_t>(head) + 26u));
                for (int32_t k = first; k < endExclusive; ++k) {
                    const uint32_t it = base + 36u * static_cast<uint32_t>(k);
                    g.W16(it + 24u, static_cast<uint16_t>(g.U16(it + 24u) + dx));
                    g.W16(it + 26u, static_cast<uint16_t>(g.U16(it + 26u) + dy));
                }
            };
            shift(kRadarDist, 95, 100, 95);
            if (type != 33) {
                shift(kNitroAt, 84, 94, 84);
                shift(kTimerAt, 53, 60, 53);
            }
            const uint32_t rb = base + 36u * 104u;
            const int32_t y = g.S16(kRadarBase + pp + 2u);
            g.W16(rb + 24u, g.U16(kRadarBase + pp));
            g.W16(rb + 26u, static_cast<uint16_t>(y));
        }
    }
    g.W32(kLayoutDone, 1);
    return !g.Faulted();
}

// ---------------------------------------------------------------------------- RASHCDI 0x80063C20
void FontInit(GuestRam& g) {
    if (g.U32(kFontInitDone) != 0) return;
    g.W32(kFontInitDone, 1);
    for (uint32_t i = 0; i < 4; ++i) {
        g.W8(rr::sim::kHudFontRecords + 24u * i, 0);
        g.W32(rr::sim::kHudFontRecords + 24u * i + 4u, 0);
    }
    g.W32(kFontRecordPtr, 0);
    g.W32(kFontIndex, 0);
    g.W16(kFontCluts, 0);
    g.W32(rr::sim::kHudStringTable, 0);
    g.W32(kLocBuffer, 0);
    const uint32_t rec = kVramOrigins + 88u * static_cast<uint32_t>(Players(g) - 1);
    g.W16(kFontX, static_cast<uint16_t>((g.U8(rec + 78u) & 0xFu) << 6));
    g.W16(kFontPageX, 960);
    g.W16(kFontPageW, 256);
    g.W16(kFontY, static_cast<uint16_t>(g.U8(rec + 77u) + ((g.U8(rec + 78u) & 0x10u) << 4)));
}

void PutVram(HudVram& vram, int x, int y, uint16_t v) {
    if (x < HudVram::kX || x >= HudVram::kX + HudVram::kWidth || y < 0 || y >= HudVram::kRows) return;
    vram.page[static_cast<size_t>(y) * HudVram::kWidth + static_cast<size_t>(x - HudVram::kX)] = v;
    vram.rowsWritten = std::max(vram.rowsWritten, y + 1);
}

// ---------------------------------------------------------------------------- RASHCDI 0x80061528
int32_t FontLoad(GuestRam& g, const DiscImage& disc, const HudPlacement& at, HudVram& vram, std::string& report) {
    if (g.U16(kFontCluts) == 0) {
        // SLUS 0x8002CA64: the font CLUT, 16 entries at (FontX + 16 slot, FontY), its id at gp+2220.
        uint32_t slot = g.U16(kFontCluts);
        if (slot >= 4u) {
            g.W16(kFontCluts, 0);
            slot = 0;
        }
        const int32_t x = static_cast<int16_t>(g.U16(kFontX)) + static_cast<int32_t>(slot << 4);
        const int32_t y = static_cast<int16_t>(g.U16(kFontY));
        g.W16(kFontCluts, static_cast<uint16_t>(slot + 1u));
        for (int k = 0; k < 16; ++k) PutVram(vram, x + k, y, g.U16(kFontClutData + 2u * static_cast<uint32_t>(k)));
        g.W16(rr::sim::kHudClutTable + 2u * slot, GetClut(x, y));
        // The loader stores the slot into the CURRENT font record +3 - which is 0 at this point, so
        // the byte lands at guest address 3, exactly as on the console.
        g.W8(g.U32(kFontRecordPtr) + 3u, static_cast<uint8_t>(slot));
        g.W16(kFontY, static_cast<uint16_t>(g.U16(kFontY) + 1u));
    }
    int32_t index = static_cast<int32_t>(g.U32(kFontIndex)) + 1;
    if (index >= 4) index = 0;
    if (g.S8(rr::sim::kHudFontRecords + 24u * static_cast<uint32_t>(index)) != 0) {
        index = 0;
        while (index < 4 && g.S8(rr::sim::kHudFontRecords + 24u * static_cast<uint32_t>(index)) != 0) ++index;
    }
    const std::vector<uint8_t> file = ReadDiscFile(disc, "DATA/GAMEFONT.PFN");
    if (file.size() < 0x20) {
        report += "  DATA/GAMEFONT.PFN is not on the disc\n";
        return -1;
    }
    auto u32 = [&](size_t o) {
        return static_cast<uint32_t>(file[o]) | (static_cast<uint32_t>(file[o + 1]) << 8) |
               (static_cast<uint32_t>(file[o + 2]) << 16) | (static_cast<uint32_t>(file[o + 3]) << 24);
    };
    const uint32_t blockBytes = u32(0x1C);
    const uint32_t bitmapBytes = u32(4) - blockBytes;
    if (blockBytes > file.size() || u32(4) > file.size()) return -1;
    g.WriteBlock(at.fontBlock, file.data(), blockBytes);
    g.WriteBlock(at.fontBitmap, file.data() + blockBytes, bitmapBytes);
    const uint32_t rec = rr::sim::kHudFontRecords + 24u * static_cast<uint32_t>(index);
    g.W32(kFontIndex, static_cast<uint32_t>(index));
    g.W32(kFontRecordPtr, rec);
    g.W8(rec, 1);
    g.W32(rec + 4u, at.fontBlock);
    g.W32(rec + 12u, at.fontBitmap);
    g.W32(rec + 8u, at.fontBlock + g.U32(at.fontBlock + 20u));
    // 0x80061430: the bitmap's VRAM origin written into its header, the next free row, the upload
    // (0x80061258) and the record's page, u and v.
    const uint32_t bm = at.fontBitmap;
    g.W32(bm + 12u, (g.U32(bm + 12u) & 0xFFFFF000u) | (g.U16(kFontX) & 0xFFFu));
    g.W32(bm + 12u, (g.U32(bm + 12u) & 0xF000FFFFu) | ((static_cast<uint32_t>(g.U16(kFontY)) & 0xFFFu) << 16));
    g.W16(kFontY, static_cast<uint16_t>(g.U16(kFontY) + g.U16(bm + 6u)));
    const int32_t x = static_cast<int32_t>(g.U32(bm + 12u) & 0xFFFu);
    const int32_t y = static_cast<int32_t>(g.U16(bm + 14u) & 0xFFFu);
    const int32_t w = static_cast<int16_t>(g.U16(bm + 4u)) >> 2;
    const int32_t h = g.U16(bm + 6u);
    for (int32_t r = 0; r < h; ++r)
        for (int32_t c = 0; c < w; ++c)
            PutVram(vram, x + c, y + r, g.U16(bm + 16u + 2u * static_cast<uint32_t>(r * w + c)));
    const uint32_t tpage = ((static_cast<uint32_t>(y) & 0x100u) >> 4) | ((static_cast<uint32_t>(x) & 0x3FFu) >> 6) |
                           ((static_cast<uint32_t>(y) & 0x200u) << 2);
    g.W16(rec + 16u, static_cast<uint16_t>(tpage));
    g.W8(rec + 18u, static_cast<uint8_t>((x - (x & 0x1FC0)) << 2));
    g.W8(rec + 19u, static_cast<uint8_t>(y - (y & 0x1F00)));
    g.W8(rec + 3u, 0);
    g.W8(rec + 2u, 3);
    // the tallest glyph and the widest advance
    g.W16(rec + 20u, 0);
    g.W16(rec + 22u, 0);
    const uint32_t glyphs = g.U16(g.U32(rec + 4u) + 10u);
    for (uint32_t i = 0; i < glyphs; ++i) {
        const uint32_t gl = g.U32(rec + 8u) + 11u * i;
        if (g.S16(rec + 20u) < static_cast<int32_t>(g.U8(gl + 3u))) g.W16(rec + 20u, g.U8(gl + 3u));
        if (g.S16(rec + 22u) < g.S8(gl + 8u)) g.W16(rec + 22u, static_cast<uint16_t>(g.S8(gl + 8u)));
    }
    return static_cast<int8_t>(index);
}

// ---------------------------------------------------------------------------- RASHCDI 0x80063D94
bool StringsLoad(GuestRam& g, const DiscImage& disc, const HudPlacement& at, std::string& report) {
    const std::vector<uint8_t> file = ReadDiscFile(disc, "DATA/GAMESTRG.LOC");
    if (file.size() < 0x24) {
        report += "  DATA/GAMESTRG.LOC is not on the disc\n";
        return false;
    }
    g.WriteBlock(at.strings, file.data(), static_cast<uint32_t>(file.size()));
    g.W32(kLocBuffer, at.strings);
    const uint32_t base = at.strings;
    const uint32_t table = base + g.U32(base + 16u) + 16u;
    const int32_t count = g.S32(base + 32u);
    for (int32_t i = 0; i < count; ++i) {
        const uint32_t e = table + 4u * static_cast<uint32_t>(i);
        g.W32(e, g.U32(e) + base + g.U32(base + 16u));
    }
    g.W32(rr::sim::kHudStringTable, table);
    return !g.Faulted();
}

// ---------------------------------------------------------------------------- RASHCDI 0x8005FE24
void Prelink(GuestRam& g) {
    if (g.U32(rr::sim::kHudEnabled) == 0) return;
    g.W32(kHudDirtyFlag, 0);
    int32_t lane = g.S32(kLaneLength);
    if (lane < 0) lane += 3;
    const int32_t quarter = lane >> 2;
    g.W32(kSplitScratchA + 12u, 0);
    g.W32(kSplitScratchB + 12u, 0);
    for (int32_t k = 0; k < 3; ++k) {
        g.W32(kSplitScratchB + 4u * static_cast<uint32_t>(k), static_cast<uint32_t>(quarter + (2 - k) * quarter));
        g.W32(kSplitScratchA + 4u * static_cast<uint32_t>(k), 0);
    }
    if (g.U32(kHudGate) == 0) return;
    const int32_t np = Players(g);
    for (int32_t p = 0; p < np; ++p) {
        const uint32_t otp = rr::sim::kHudOt2 + 4u * static_cast<uint32_t>(p);
        uint32_t item = (Items(g, p) + 3924u) & 0x00FFFFFFu;
        for (int32_t i = 109; i >= 0; --i, item -= 36u) {
            const uint32_t words = item + 4u;
            const int32_t kind = g.S8(words + 28u);
            const uint32_t semi = static_cast<uint32_t>(static_cast<int32_t>(g.S16(words + 30u))) << 25;
            if (kind == 0) {
                g.W32(item, g.U32(g.U32(otp)) | 0x04000000u);
                g.W32(words, semi | 0x64808080u);
                g.W32(words + 8u, g.U32(g.U32(words + 16u) + 4u));
            } else if (kind == 1) {
                g.W32(item, g.U32(g.U32(otp)) | 0x03000000u);
                g.W32(words, semi | 0x60808080u);
            } else {
                SetDrawModeTw(g, item, 1, 1, 15);
                g.W32(item, g.U32(g.U32(otp)) | 0x02000000u);
            }
            g.W32(g.U32(otp), item);
        }
    }
}

// RASHCDI 0x80060028: a flat tile item of colour `rgb`.
void BarTile(GuestRam& g, uint32_t it, const uint8_t rgb[3]) {
    const uint32_t semi = static_cast<uint32_t>(static_cast<int32_t>(g.S16(it + 34u))) << 25;
    g.W32(it + 4u, semi | 0x60000000u | (static_cast<uint32_t>(rgb[2]) << 16) | (static_cast<uint32_t>(rgb[1]) << 8) | rgb[0]);
    g.W32(it + 8u, g.U16(it + 24u) | (static_cast<uint32_t>(g.U16(it + 26u)) << 16));
    g.W32(it + 12u, g.U16(it + 28u) | (static_cast<uint32_t>(g.U16(it + 30u)) << 16));
}

// ---------------------------------------------------------------------------- RASHCDI 0x80060088
bool HudReset(GuestRam& g, const DiscImage& disc, std::string& report) {
    const int32_t np = Players(g);
    for (int32_t p = 0; p < np; ++p) {
        const uint32_t it = Items(g, p);
        const uint32_t d = rr::sim::kHudDash224 + 224u * static_cast<uint32_t>(p);
        for (uint32_t o : {0u, 16u, 12u, 96u, 112u, 108u, 128u, 144u, 140u, 32u, 48u, 44u}) g.W32(d + o, 0);
        const uint32_t s = rr::sim::kHudSlide72 + 72u * static_cast<uint32_t>(p);
        g.W32(s + 28u, 17);
        g.W32(s + 32u, 30);
        g.W32(s + 64u, 31);
        g.W32(s + 0u, 0);
        g.W32(s + 4u, 0);
        g.W32(s + 36u, 0);
        g.W32(s + 40u, 0);
        g.W32(s + 68u, 44);
        const uint32_t st = rr::sim::kHudState88 + 88u * static_cast<uint32_t>(p);
        for (uint32_t o = 0; o <= 64u; o += 4) g.W32(st + o, 0);
        g.W32(rr::sim::kHudOdometer + 4u * static_cast<uint32_t>(p), 0);
        for (uint32_t o : {84u, 68u, 72u, 76u, 80u}) g.W32(st + o, 0xFFFFFFFFu);
        for (uint32_t k = 0; k < 5; ++k)
            for (uint32_t first : {53u, 60u, 67u, 74u}) rr::sim::HudSetArtFull(g, it + 36u * (first + k), kArts + 16u * 5u);
        for (uint32_t colon : {58u, 59u, 65u, 66u, 72u, 73u}) rr::sim::HudSetArtFull(g, it + 36u * colon, kArts + 16u * 15u);
        for (uint32_t star : {25u, 30u, 39u, 44u}) rr::sim::HudSetArtFull(g, it + 36u * star, kArts + 16u * 61u);
        rr::sim::HudSetArtFull(g, it + 36u * 104u, kArts + 16u * 18u);
        for (uint32_t graph : {19u, 22u, 27u, 33u, 36u, 41u}) rr::sim::HudSetArtFull(g, it + 36u * graph, kArts + 16u * 17u);
    }
    Prelink(g);
    // The graph tiles' colour, 4 bytes of RASHCDI data (resident while the loader runs; read here from
    // the file itself).
    const std::vector<uint8_t> rashcdi = ReadDiscFile(disc, "RASHCDI.BIN");
    const uint32_t off = kBarColourInRashcdi - kRashcdiBase;
    if (rashcdi.size() < off + 4u) {
        report += "  RASHCDI.BIN is not on the disc\n";
        return false;
    }
    const uint8_t rgb[3] = {rashcdi[off], rashcdi[off + 1u], rashcdi[off + 2u]};
    for (int32_t p = 0; p < np; ++p) {
        const uint32_t it = Items(g, p);
        for (uint32_t graph : {19u, 22u, 27u, 33u, 36u, 41u}) BarTile(g, it + 36u * graph, rgb);
        const uint16_t h = g.U16(it + 714u);
        for (uint32_t o : {1038u, 858u, 750u, 1542u, 1362u, 1254u}) g.W16(it + o, h);
        const uint32_t type = RaceType(g);
        // 0x80060454..0x800605BC: the timed races' clocks, through the PORTED digit clock SLUS 0x80013B90
        // (modes.h HudClockDigits): with race type bit 2 (not 36) the race's Time Trial record (SLUS table
        // 0x80053A88, 176 bytes per race 56..64: +0x0C into items 60.., +0x00 into 67.. and 74..); in race
        // types 44 / 36 the time limit 0x8005ACC8 into 53... Seconds * 256 = (ticks << 8) / 300.
        auto sec256 = [](int32_t ticks) { return static_cast<int32_t>(static_cast<uint32_t>(ticks) << 8) / 300; };
        if ((type & 4u) && type != 36u) {
            auto rec = [&]() { return 0x80053A88u + 176u * static_cast<uint32_t>(g.S32(g.U32(0x8005B2F8u) + 64u) - 56); };
            rr::sim::HudClockDigits(g, sec256(g.S32(rec() + 12u)), it + 2160u, kArts + 16u * 5u);
            rr::sim::HudClockDigits(g, sec256(g.S32(rec())), it + 2412u, kArts + 16u * 5u);
            rr::sim::HudClockDigits(g, sec256(g.S32(rec())), it + 2664u, kArts + 16u * 5u);
        }
        if (RaceType(g) == 44u || RaceType(g) == 36u)
            rr::sim::HudClockDigits(g, sec256(g.S32(0x8005ACC8u)), it + 1908u, kArts + 16u * 5u);
        for (uint32_t k = 0; k < 5; ++k) {
            const uint32_t c = it + 36u * (67u + k) + 4u, d = it + 36u * (74u + k) + 4u;
            g.W8(c, 128);
            g.W8(c + 1u, 128);
            g.W8(c + 2u, 0);
            g.W8(d, 129);
            g.W8(d + 1u, 128);
            g.W8(d + 2u, 0);
        }
        for (uint32_t item : {72u, 73u, 79u, 80u}) {
            const uint32_t c = it + 36u * item + 4u;
            g.W8(c, 128);
            g.W8(c + 1u, 128);
            g.W8(c + 2u, 0);
        }
        g.W32(kClockReset + 4u * static_cast<uint32_t>(p), 0);
    }
    // 0x8005EC20: the radar strip's rectangle and scale, and the widths of GAMESTRG 15..29.
    for (int32_t p = 0; p < np; ++p) {
        const uint32_t rb = Items(g, p) + 3744u;
        const uint32_t r = kRadarRect + 12u * static_cast<uint32_t>(p);
        g.W16(r + 0u, g.U16(rb + 24u));
        g.W16(r + 2u, g.U16(rb + 26u));
        g.W16(r + 4u, g.U16(rb + 28u));
        g.W16(r + 6u, g.U16(rb + 30u));
        g.W16(r + 8u, static_cast<uint16_t>(g.U16(rb + 24u) + (g.U16(rb + 28u) >> 1)));
        g.W16(r + 10u, static_cast<uint16_t>(g.U16(rb + 26u) + (g.U16(rb + 30u) >> 1)));
    }
    const int32_t h16 = static_cast<int32_t>(static_cast<uint32_t>(g.U16(Items(g, 0) + 3774u)) << 16);
    if (h16 > 0)
        g.W32(kRadarScale, rr::sim::FixDiv(static_cast<uint32_t>(h16), 0xC80000u));
    else
        g.W32(kRadarScale, 0u - rr::sim::FixDiv(static_cast<uint32_t>(-h16), 0xC80000u));
    for (uint32_t i = 0; i < 15; ++i) {
        const uint32_t str = g.U32(g.U32(rr::sim::kHudStringTable) + 4u * i + 60u);
        g.W32(rr::sim::kHudMessageWidth + 4u * i,
              static_cast<uint32_t>(rr::sim::HudStringWidth(g, g.S32(rr::sim::kHudFontIndex), str)));
    }
    return !g.Faulted();
}

} // namespace

HudPlacement ProductHudPlacement(int players) {
    HudPlacement at;
    at.items = 0x800DA000;      // 3960 bytes
    at.fontBlock = 0x800DAF80;  // 0x440
    at.fontBitmap = 0x800DB3C0; // 16 + 7936 bytes (freed by the loader; later the LOC file's place)
    at.strings = 0x800DB3C0;    // the LOC file reuses the freed bitmap buffer, as in every capture
    at.heapBase = 0x800DD400;
    at.heapEnd = 0x800DFF00;
    if (players == 2) {         // 2 x 3960 item bytes to 0x800DBEF0, then the same blocks in the same order
        at.fontBlock = 0x800DBF00;
        at.fontBitmap = 0x800DC340;
        at.strings = 0x800DC340;
        at.heapBase = 0x800DE280;
    }
    return at;
}

bool BuildHudArena(GuestRam& g, const DiscImage& disc, const HudPlacement& at, HudVram& vram, std::string& report,
                   bool mutateNoArtFixup) {
    HudAlloc(g, at);
    // 0x8005EF48: DASH?P.TEX, 64 x 216 halfwords, to VRAM (960, 0).
    {
        const std::vector<uint8_t> tex = ReadDiscFile(disc, Players(g) == 1 ? "DATA/DASH1P.TEX" : "DATA/DASH2P.TEX");
        if (tex.size() < 64u * 216u * 2u) {
            report += "  DASH?P.TEX is not on the disc\n";
            return false;
        }
        for (int y = 0; y < 216; ++y)
            for (int x = 0; x < 64; ++x)
                PutVram(vram, 960 + x, y, static_cast<uint16_t>(tex[(static_cast<size_t>(y) * 64 + x) * 2] |
                                                                 (tex[(static_cast<size_t>(y) * 64 + x) * 2 + 1] << 8)));
    }
    if (!HudLayout(g, disc, mutateNoArtFixup, report)) return false;
    FontInit(g);
    const int32_t font = FontLoad(g, disc, at, vram, report);
    if (font < 0) return false;
    g.W32(rr::sim::kHudFontIndex, static_cast<uint32_t>(font));
    if (!StringsLoad(g, disc, at, report)) return false;
    // The frame's HUD slots and packet heap (the original's GPU set-up; ours as a placement).
    g.W32(rr::sim::kHudOt, at.ot);
    g.W32(rr::sim::kHudOt2, at.ot2);
    g.W32(at.ot, 0x00FFFFFFu);
    g.W32(at.ot2, 0x00FFFFFFu);
    if (Players(g) == 2) { // player 2's slots (0x8005B594 / 0x8005B5A4)
        g.W32(rr::sim::kHudOt + 4u, at.otP2);
        g.W32(rr::sim::kHudOt2 + 4u, at.ot2P2);
        g.W32(at.otP2, 0x00FFFFFFu);
        g.W32(at.ot2P2, 0x00FFFFFFu);
    }
    g.W32(rr::sim::kHudHeapPtr, at.heapRecord);
    g.W32(at.heapRecord + 268u, at.heapBase);
    g.W32(rr::sim::kHudHeapEnd, at.heapEnd);
    if (!HudReset(g, disc, report)) return false;
    char line[200];
    std::snprintf(line, sizeof(line),
                  "  HUD arena: %u textures, 62 arts, 110 items at 0x%08X, font %d, strings at 0x%08X, "
                  "VRAM rows 0..%d\n",
                  g.U32(kTextureCount), at.items, font, g.U32(rr::sim::kHudStringTable), vram.rowsWritten - 1);
    report += line;
    return !g.Faulted();
}

void HudBeginFrame(GuestRam& g, const HudPlacement& at) {
    g.W32(at.ot, 0x00FFFFFFu);
    g.W32(at.ot2, 0x00FFFFFFu);
    if (Players(g) == 2) {
        g.W32(at.otP2, 0x00FFFFFFu);
        g.W32(at.ot2P2, 0x00FFFFFFu);
    }
    g.W32(g.U32(rr::sim::kHudHeapPtr) + 268u, at.heapBase);
}

// ============================================================================ the check
namespace {

bool ReadAll(const std::string& path, std::vector<uint8_t>& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

uint32_t Word(const std::vector<uint8_t>& ram, uint32_t a) {
    const uint32_t o = a & 0x1FFFFFu;
    return static_cast<uint32_t>(ram[o]) | (static_cast<uint32_t>(ram[o + 1]) << 8) |
           (static_cast<uint32_t>(ram[o + 2]) << 16) | (static_cast<uint32_t>(ram[o + 3]) << 24);
}

// Loads SLUS_010.53's text and RASHCDG.BIN into a scratch image, as the product's arena does.
bool LoadImages(const DiscImage& disc, std::vector<uint8_t>& ram) {
    const std::vector<uint8_t> exe = ReadDiscFile(disc, "SLUS_010.53");
    const std::vector<uint8_t> ovl = ReadDiscFile(disc, "RASHCDG.BIN");
    if (exe.size() < 0x800 || ovl.empty()) return false;
    auto hdr = [&](size_t o) {
        return static_cast<uint32_t>(exe[o]) | (static_cast<uint32_t>(exe[o + 1]) << 8) |
               (static_cast<uint32_t>(exe[o + 2]) << 16) | (static_cast<uint32_t>(exe[o + 3]) << 24);
    };
    const uint32_t text = hdr(0x18), size = hdr(0x1C);
    if (0x800u + size > exe.size()) return false;
    std::memcpy(ram.data() + (text & 0x1FFFFFu), exe.data() + 0x800, size);
    std::memcpy(ram.data() + (0x8005B5E8u & 0x1FFFFFu), ovl.data(), ovl.size());
    return true;
}

} // namespace

bool CheckHudArena(const DiscImage& disc, const std::string& stateDir, std::string& report, bool mutate) {
    std::vector<uint8_t> cap, vramCap;
    if (!ReadAll(stateDir + "\\ram.bin", cap) || cap.size() < 0x200000u) {
        report = "no ram.bin under " + stateDir + "\n";
        return false;
    }
    const bool haveVram = ReadAll(stateDir + "\\vram.bin", vramCap) && vramCap.size() >= 1024u * 512u * 2u;
    const uint32_t items = Word(cap, rr::sim::kHudItemsPtr);
    if (items < 0x80000000u || Word(cap, rr::sim::kHudEnabled) == 0) {
        report = stateDir + " holds no race HUD (the item pointer is 0) - refused\n";
        return false;
    }
    // The placement the capture's own allocator chose.
    HudPlacement at;
    at.items = items;
    const uint32_t fontRec = rr::sim::kHudFontRecords + 24u * Word(cap, rr::sim::kHudFontIndex);
    at.fontBlock = Word(cap, fontRec + 4u);
    at.fontBitmap = Word(cap, fontRec + 12u);
    at.strings = Word(cap, kLocBuffer);
    at.heapRecord = Word(cap, rr::sim::kHudHeapPtr);
    at.heapBase = Word(cap, at.heapRecord + 268u);
    at.heapEnd = Word(cap, rr::sim::kHudHeapEnd);
    at.ot = Word(cap, rr::sim::kHudOt);
    at.ot2 = Word(cap, rr::sim::kHudOt2);

    std::vector<uint8_t> ram(0x200000u, 0);
    if (!LoadImages(disc, ram)) {
        report = "SLUS_010.53 / RASHCDG.BIN are not on the disc\n";
        return false;
    }
    // game_state as the capture has it (player count, race type) - the loader's only input.
    const uint32_t gs = Word(cap, kGameStatePtr);
    std::memcpy(ram.data() + (gs & 0x1FFFFFu), cap.data() + (gs & 0x1FFFFFu), 0x60);
    GuestRam g(ram.data(), 0x8005AC8Cu);
    g.W32(kGameStatePtr, gs);
    g.W32(kLaneLength, Word(cap, kLaneLength));
    HudVram vram;
    std::string build;
    if (!BuildHudArena(g, disc, at, vram, build, mutate)) {
        report = "the HUD arena builder refused:\n" + build;
        return false;
    }
    // The panels' slides, as the capture has them, applied to the built items (the loader leaves the
    // panels shown; HudFrame slides them): the item y is the one field a slide moves.
    for (uint32_t k = 0; k < 2; ++k) {
        const uint32_t s = rr::sim::kHudSlide72 + 36u * k;
        const int32_t state = static_cast<int32_t>(Word(cap, s)), counter = static_cast<int32_t>(Word(cap, s + 4u));
        const int32_t sign = static_cast<int32_t>(Word(cap, s + 24u));
        int32_t offset = 0;
        if (state == 2) offset = static_cast<int32_t>(Word(cap, s + 20u)) * sign;
        if (state == 1 || state == 3) offset = counter * sign;
        if (offset != 0)
            rr::sim::HudSlideItems(g, items, static_cast<int16_t>(offset), static_cast<int32_t>(Word(cap, s + 28u)),
                                   static_cast<int32_t>(Word(cap, s + 32u)));
    }

    struct Tally {
        const char* what;
        size_t compared = 0, differ = 0;
        uint32_t first = 0;
    };
    std::vector<Tally> tallies;
    auto compare = [&](const char* what, uint32_t a, uint32_t n) {
        Tally t{what};
        for (uint32_t k = 0; k < n; ++k) {
            const uint32_t o = (a + k) & 0x1FFFFFu;
            ++t.compared;
            if (ram[o] != cap[o]) {
                if (t.differ == 0) t.first = a + k;
                ++t.differ;
            }
        }
        tallies.push_back(t);
    };
    compare("art table 0x800D45D0 (62 x 16)", kArts, 62u * 16u);
    compare("texture table 0x800D49B0 (22 x 8)", kTextures, 22u * 8u);
    // The items: their layout fields always; their packet words where HudFrame never rewrites them.
    // Items whose art or size a frame changes (digits, weapons, bars, nitro, radar) and the items a
    // LinkRange ends on (their tag points into the frame's list) are compared field by field below.
    static const int kDynamicArt[] = {6, 7, 10, 11, 12, 13, 14, 15, 16, 20, 23, 24, 28, 29, 31, 34, 37, 38, 42, 43,
                                      45, 46, 47, 48, 49, 52, 84, 85, 86, 87, 88, 89, 90, 91, 92, 93, 95, 96, 97, 98, 99};
    // Every j of a LinkRange(i, j) in hud.cpp, and item 109 (its tag is the OT slot's content at set-up).
    static const int kRangeEnds[] = {0, 1, 4, 5, 7, 8, 9, 12, 16, 24, 25, 29, 30, 38, 39, 43, 44, 46, 47, 49,
                                     50, 51, 52, 81, 82, 83, 84, 85, 86, 87, 88, 89, 90, 91, 99, 104, 108, 109};
    Tally layout{"items +0x18..+0x23 (position, kind, art index, semi) x 110"};
    Tally packets{"items: packet words of the items no frame rewrites"};
    Tally tags{"items: the prelinked chain tags"};
    for (int i = 0; i < 110; ++i) {
        const uint32_t it = items + 36u * static_cast<uint32_t>(i);
        auto cmp = [&](Tally& t, uint32_t from, uint32_t to) {
            for (uint32_t k = from; k < to; ++k) {
                const uint32_t o = (it + k) & 0x1FFFFFu;
                ++t.compared;
                if (ram[o] != cap[o]) {
                    if (t.differ == 0) t.first = it + k;
                    ++t.differ;
                }
            }
        };
        cmp(layout, 24, 28);
        cmp(layout, 32, 36);
        const int kind = static_cast<int8_t>(cap[(it + 32u) & 0x1FFFFFu]);
        const bool dynamic = std::find(std::begin(kDynamicArt), std::end(kDynamicArt), i) != std::end(kDynamicArt);
        const bool rangeEnd = std::find(std::begin(kRangeEnds), std::end(kRangeEnds), i) != std::end(kRangeEnds);
        if (!rangeEnd) cmp(tags, 0, 4);
        if (!dynamic) {
            if (kind == 0) {
                cmp(packets, 4, 24);
                cmp(packets, 28, 32);
            } else if (kind == 1) {
                cmp(packets, 4, 16);
            } else {
                cmp(packets, 4, 12);
            }
        }
    }
    tallies.push_back(layout);
    tallies.push_back(packets);
    tallies.push_back(tags);
    for (uint32_t p = 0; p < 1; ++p) {
        const uint32_t d = rr::sim::kHudDash224;
        Tally t{"flash timers' parameters (7 x +4 +8 +24 +28)"};
        for (uint32_t k = 0; k < 7; ++k)
            for (uint32_t o : {4u, 8u, 24u, 28u})
                for (uint32_t b = 0; b < 4; ++b) {
                    const uint32_t a = (d + 32u * k + o + b) & 0x1FFFFFu;
                    ++t.compared;
                    if (ram[a] != cap[a]) {
                        if (t.differ == 0) t.first = d + 32u * k + o;
                        ++t.differ;
                    }
                }
        tallies.push_back(t);
        Tally s{"panel slides' parameters (2 x +8..+0x23)"};
        for (uint32_t k = 0; k < 2; ++k)
            for (uint32_t o = 8; o < 36; ++o) {
                const uint32_t a = (rr::sim::kHudSlide72 + 36u * k + o) & 0x1FFFFFu;
                ++s.compared;
                if (ram[a] != cap[a]) {
                    if (s.differ == 0) s.first = rr::sim::kHudSlide72 + 36u * k + o;
                    ++s.differ;
                }
            }
        tallies.push_back(s);
    }
    compare("font record (24 bytes)", fontRec, 24);
    compare("font header block (PFN header + glyph table)", at.fontBlock, 0x440);
    compare("font CLUT id table gp+2220 (4 x u16)", rr::sim::kHudClutTable, 8);
    compare("font globals 0x8005B540..0x8005B557", kFontIndex, 0x18);
    compare("GAMESTRG.LOC, relocated (the string table)", at.strings,
            static_cast<uint32_t>(ReadDiscFile(disc, "DATA/GAMESTRG.LOC").size()));
    compare("message widths 0x800D6120 (15 x s32)", rr::sim::kHudMessageWidth, 60);
    compare("radar-strip rectangle 0x800D63E8 and scale 0x8005B234", kRadarRect, 12);
    compare("radar-strip scale 0x8005B234", kRadarScale, 4);
    compare("CSV placements 0x8005B238/B258/B2C0/B2C8", kRadarBase, 4);
    compare("CSV placements (radar distance)", kRadarDist, 4);
    compare("CSV placements (timer, nitro)", kTimerAt, 4);
    compare("CSV placements (nitro)", kNitroAt, 4);
    compare("CLUT table line 0x8005B2B4/B2BC", kClutTableX, 2);
    compare("CLUT table line (y)", kClutTableY, 2);
    compare("HUD globals ACD8/ACEC/AD14/B388/ACE0", rr::sim::kHudEnabled, 4);
    compare("HUD globals (items pointer)", rr::sim::kHudItemsPtr, 4);
    compare("HUD globals (split layout)", rr::sim::kHudSplitMode, 4);
    compare("HUD globals (texture count)", kTextureCount, 4);
    compare("HUD globals (layout done)", kLayoutDone, 4);
    compare("HUD globals (font index)", rr::sim::kHudFontIndex, 4);

    size_t vramCompared = 0, vramDiffer = 0;
    int firstRow = -1;
    if (haveVram) {
        for (int y = 0; y < vram.rowsWritten; ++y)
            for (int x = HudVram::kX; x < HudVram::kX + HudVram::kWidth; ++x) {
                const size_t o = (static_cast<size_t>(y) * 1024 + static_cast<size_t>(x)) * 2;
                const uint16_t v = static_cast<uint16_t>(vramCap[o] | (vramCap[o + 1] << 8));
                ++vramCompared;
                if (v != vram.At(x, y)) {
                    if (firstRow < 0) firstRow = y;
                    ++vramDiffer;
                }
            }
    }

    size_t total = 0, differ = 0;
    char line[256];
    report.clear();
    std::snprintf(line, sizeof(line), "HUD arena check against %s (items at 0x%08X)%s\n", stateDir.c_str(), items,
                  mutate ? "  [NEGATIVE CONTROL: the art fix-up 0x8005EFFC left out]" : "");
    report += line;
    for (const Tally& t : tallies) {
        total += t.compared;
        differ += t.differ;
        std::snprintf(line, sizeof(line), "  %-70s %6zu bytes, %5zu differ%s", t.what, t.compared, t.differ,
                      t.differ ? "" : "\n");
        report += line;
        if (t.differ) {
            std::snprintf(line, sizeof(line), " (first at 0x%08X)\n", t.first);
            report += line;
        }
    }
    std::snprintf(line, sizeof(line), "  %-70s %6zu texels, %5zu differ%s\n", "VRAM page (960..1023, rows the loaders write)",
                  vramCompared, vramDiffer, haveVram ? "" : "  (no vram.bin)");
    report += line;
    if (vramDiffer) {
        std::snprintf(line, sizeof(line), "    first differing row %d\n", firstRow);
        report += line;
    }
    std::snprintf(line, sizeof(line), "  TOTAL %zu bytes + %zu texels compared, %zu + %zu differ -> %s\n", total,
                  vramCompared, differ, vramDiffer, (differ == 0 && vramDiffer == 0) ? "PASS" : "FAIL");
    report += line;
    return differ == 0 && vramDiffer == 0 && haveVram;
}

// ---------------------------------------------------------------------------- HD media (docs\HD-MEDIA.md)
bool BuildHudPageForHd(const DiscImage& disc, int players, int splitMode, HudVram& vram, std::vector<HudRegion>& regions,
                       std::string& report) {
    std::vector<uint8_t> ram(0x200000u, 0);
    if (!LoadImages(disc, ram)) {
        report = "SLUS_010.53 / RASHCDG.BIN are not on the disc\n";
        return false;
    }
    GuestRam g(ram.data(), 0x8005AC8Cu);
    // game_state (the loader's only input, as --hudarenacheck copies it from a capture): the player count at +48, race
    // type 0 at +4 (the career shifts move items, not art), the two-player view mode the split layout comes from.
    const uint32_t gs = 0x800D0000u;
    g.W32(kGameStatePtr, gs);
    g.W32(gs + 48u, static_cast<uint32_t>(players));
    g.W8(gs + 4u, 0);
    g.W32(0x800D6C68u, static_cast<uint32_t>(splitMode));
    g.W32(kLaneLength, 0x10000u);
    vram = HudVram{};
    std::string build;
    if (!BuildHudArena(g, disc, ProductHudPlacement(players), vram, build)) {
        report = "the HUD arena builder refused:\n" + build;
        return false;
    }
    regions.clear();
    for (uint32_t i = 0; i < 62; ++i) { // the art records after the fix-up: rectangle and CLUT of every HUD sprite
        const uint32_t a = kArts + 16u * i;
        if (g.U16(a + 6u) == 0xFFFFu || g.U16(a + 10u) != 15u) continue;
        HudRegion r;
        r.u = g.U8(a + 4u);
        r.v = g.U8(a + 5u);
        r.w = std::min<int>(g.U8(a + 2u), 256 - r.u);
        r.h = std::min<int>(g.U8(a + 3u), 256 - r.v);
        r.clut = g.U16(a + 6u);
        r.what = "art " + std::to_string(i);
        if (r.w > 0 && r.h > 0) regions.push_back(r);
    }
    // The font's bitmap: its VRAM origin (the record's bitmap header +12 / +14) and the font CLUT (slot 0).
    const uint32_t rec = rr::sim::kHudFontRecords + 24u * g.U32(kFontIndex);
    const uint32_t bm = g.U32(rec + 12u);
    const int x = static_cast<int>(g.U32(bm + 12u) & 0xFFFu), y = static_cast<int>(g.U16(bm + 14u) & 0xFFFu);
    const int w = (static_cast<int16_t>(g.U16(bm + 4u)) >> 2) * 4, h = g.U16(bm + 6u);
    HudRegion f;
    f.u = (x - HudVram::kX) * 4;
    f.v = y;
    f.w = std::min(w, 256 - f.u);
    f.h = std::min(h, 256 - f.v);
    f.clut = g.U16(rr::sim::kHudClutTable);
    f.what = "font";
    f.font = true;
    if (f.u >= 0 && f.w > 0 && f.h > 0) regions.push_back(f);
    char line[160];
    std::snprintf(line, sizeof(line), "HUD page for %d player(s), split %d: %zu regions, rows 0..%d\n", players, splitMode,
                  regions.size(), vram.rowsWritten - 1);
    report = line;
    return !g.Faulted();
}

} // namespace rr::game
