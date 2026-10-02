// The sky's own programs in the product (sky_product.h).
#include "game/sky_product.h"

#include "game/sim/effects.h"
#include "game/sim/sky_draw.h"
#include "game/sim/sky_split.h"
#include "game/sim/split_view.h"
#include "game/sim/stream_cd.h" // SetSkyFrameWide
#include "game/stream_product.h"
#include "rrformats/level_bundle.h"
#include "rrformats/mdec.h"
#include "rrformats/sky_clouds.h"

#include <algorithm>
#include <cmath>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace rr::game {

namespace s = rr::sim;

namespace {

constexpr uint32_t kGp = 0x8005AC8Cu;
constexpr uint32_t kSkyOtAt = 0x800D9BE0u; // 0x800C89A0's first buffer, view 0 (5 entries)
constexpr uint32_t kSkySp = 0x801FFB00u;   // OURS: the stack the product runs the sky draw at (nothing reads it)

struct Totals {
    size_t frames = 0, drawn = 0, panoPackets = 0, cloudPackets = 0, decodes = 0, mdecBlocks = 0, uploads = 0,
           cuts = 0, heapFull = 0, refused = 0, unported = 0, noTables = 0, badMdec = 0;
    size_t maxPano = 0, maxClouds = 0;
    size_t captureTotal = 0, captureEqual = 0, primsTotal = 0, primsEqual = 0; // --parity: the capture's own sky packets, word for word
    // the gradient, the two-player sky, the heap ring (RRJB_SKY3)
    size_t gradPackets = 0, splitPackets = 0, viewDraws = 0, flushes = 0, drawOTags = 0, clears = 0, wideFrames = 0,
           widePanoAdded = 0, widePanoMissing = 0, gradTotal = 0, gradEqual = 0;
    std::string splitSetUp, heapSetUp;
    std::string setUp, why;
};
Totals& T() {
    static Totals t;
    return t;
}
SkyVram& Vram() {
    static SkyVram v;
    return v;
}
std::vector<SkyPacket>& Packets(uint32_t view = 0) { // one list per view
    static std::vector<SkyPacket> p[2];
    return p[view & 1u];
}
bool Sky3On() { // sky_split.h; RRJB_SKY3=off: the sky draw's seams and the renderer's gradient
    static const bool on = std::getenv("RRJB_SKY3") == nullptr || std::strcmp(std::getenv("RRJB_SKY3"), "off") != 0;
    return on;
}
// The picture's console-x extent per view (main.cpp, from the view's projection): the wide picture's reach.
struct Picture {
    bool set = false;
    double left = 0, right = 384;
};
Picture& PictureOf(uint32_t view) {
    static Picture p[2];
    return p[view & 1u];
}
s::VlcHostTables& VlcTables() {
    static s::VlcHostTables t;
    return t;
}
bool& VlcReady() {
    static bool ready = false;
    return ready;
}
std::array<uint8_t, 1024>& Scratch() {
    static std::array<uint8_t, 1024> spad{};
    return spad;
}

bool EnvOff(const char* name) {
    const char* v = std::getenv(name);
    return v != nullptr && std::strcmp(v, "off") == 0;
}

// ---------------------------------------------------------------------------- the MDEC (the host's)
// The unit's arithmetic is rr::mdec (src\rrformats\mdec.h: the console's dequantisation, IDCT, colour and 15-bit
// packing), shared with the films and the interpreter's MDEC device. Here only the DMA
// plumbing: the decode command at DecDCTin's buffer, the tables DecDCTReset uploads (commands 2 and 3) read out of
// the arena's EXE data, the out words DecDCTout asked for.
struct Mdec {
    uint32_t in = 0; // DecDCTin's buffer
    bool haveIn = false;
    std::vector<uint16_t> halfwords;
    std::vector<uint32_t> words;
    // Decodes the run-level stream at `in` into `count` output words at `out` (15-bit mode).
    bool Out(s::GuestRam& g, uint32_t out, uint32_t count) {
        if (!haveIn) return false;
        haveIn = false;
        rr::mdec::Tables t;
        for (uint32_t k = 0; k < 64; ++k) {
            t.luma[k] = g.U8(0x8005A250u + k);
            t.chroma[k] = g.U8(0x8005A290u + k);
            t.idct[k] = g.S16(0x8005A2D4u + 2u * k);
        }
        const uint32_t cmd = g.U32(in);
        if ((cmd >> 29) != 1u || ((cmd >> 27) & 3u) != 3u) {
            ++T().badMdec;
            return false;
        }
        halfwords.resize((cmd & 0xFFFFu) * 2u);
        for (uint32_t k = 0; k < halfwords.size(); ++k) halfwords[k] = g.U16(in + 4u + 2u * k);
        rr::mdec::Input src{halfwords.data(), halfwords.size(), 0};
        words.assign(count, 0u);
        size_t produced = 0;
        T().mdecBlocks += 6u * rr::mdec::DecodeCommand(cmd, src, t, rr::mdec::ActiveModel(), words.data(), words.size(), produced);
        for (uint32_t k = 0; k < count && k < produced; ++k) g.W32(out + 4u * k, words[k]);
        return !g.Faulted();
    }
};
Mdec& TheMdec() {
    static Mdec m;
    return m;
}

void LoadImage(s::GuestRam& g, uint32_t rect, uint32_t src) {
    const int x = g.S16(rect), y = g.S16(rect + 2u), w = g.S16(rect + 4u), h = g.S16(rect + 6u);
    SkyVram& v = Vram();
    for (int r = 0; r < h; ++r)
        for (int c = 0; c < w; ++c)
            v.px[static_cast<size_t>((y + r) & 511) * SkyVram::kWidth + static_cast<size_t>((x + c) & 1023)] =
                g.U16(src + static_cast<uint32_t>(r * w + c) * 2u);
    ++v.generation;
    ++T().uploads;
}

// The hardware and library callees of the sky's programs, as the product answers them.
struct Callees final : s::RecoverCallees {
    s::GuestRam& g;
    explicit Callees(s::GuestRam& gg) : g(gg) {}
    bool Call(uint32_t fn, const uint32_t* a, int, uint32_t, uint32_t& v0) override {
        v0 = 0;
        switch (fn) {
        case s::kDecDctInSyncFn:
        case s::kDecDctOutSyncFn:
        case s::kDecDctResetFn:
        case s::kSkyDrawSyncFn:
        case s::kSkyEmptyFn:
        case 0x8005E840u: // RASHCDG: an empty function (SkyFrame's)
            return true;
        case s::kDecDctInFn: { // SLUS 0x8004D90C: the command word's depth / STP bits, then the DMA
            uint32_t w = g.U32(a[0]);
            w = (a[1] & 1u) ? (w & 0xF7FFFFFFu) : (w | 0x08000000u);
            w = (a[1] & 2u) ? (w | 0x02000000u) : (w & 0xFDFFFFFFu);
            g.W32(a[0], w);
            TheMdec().in = a[0];
            TheMdec().haveIn = true;
            return !g.Faulted();
        }
        case s::kDecDctOutFn:
            if (!TheMdec().Out(g, a[0], a[1])) {
                T().why = "the MDEC refused a decode";
                return false;
            }
            ++T().decodes;
            s::MdecDone(g); // the out callback SkyInit registered (SLUS 0x80013AE4)
            return !g.Faulted();
        case s::kSkyLoadImageFn: LoadImage(g, a[0], a[1]); return !g.Faulted();
        case s::kHeapFullFn: // RRJB_SKY3=off only: the manager through the seam
            ++T().heapFull;
            T().why = "the packet heap is full (RRJB_SKY3=off: the heap manager 0x80021C98 not run)";
            return false;
        case s::kSkySplitFn:
        case s::kSkyGradFn:
            ++T().unported; // RRJB_SKY3=off only: two players' sky / the gradient left to the renderer
            return true;
        case s::kHeapFlushFn: // the heap's flush 0x80021BE8 PORTED, its library below
            ++T().flushes;
            return s::HeapFlush(g, kSkySp - 1024u, *this);
        case s::kPrintfFn: return true; // the BIOS's printf: nothing the game reads
        case s::kDrawOTagFn:
            // the GPU draws the frame's table now: OURS, named - the product's renderer draws the frame's lists at
            // the frame's end, so what the flush would have drawn early is counted, not drawn early
            ++T().drawOTags;
            return true;
        case s::kClearOTagRFn: { // libgpu ClearOTagR(ot, n): each entry links to the one below it, entry 0 ends
            const uint32_t ot = a[0], n = a[1];
            if (ot == 0 || n > 0x1000u) return true; // no table placed (OURS: the product's record without one)
            for (uint32_t i = 0; i < n; ++i) g.W32(ot + 4u * i, i == 0 ? 0x00FFFFFFu : ((ot + 4u * (i - 1u)) & 0x00FFFFFFu));
            ++T().clears;
            return !g.Faulted();
        }
        default: {
            char b[96];
            std::snprintf(b, sizeof(b), "the sky asked for 0x%08X, which the product does not answer", fn);
            T().why = b;
            return false;
        }
        }
    }
};

s::FxGte OnePlayerGte() { // SLUS 0x80011C4C: one player's GTE offset 192 / 120, H 237
    s::FxGte gte;
    gte.ofx = 192 << 16;
    gte.ofy = 120 << 16;
    gte.h = 237;
    return gte;
}

SkyPacket ReadPacket(const s::GuestRam& gc, uint32_t at, int entry) {
    s::GuestRam& g = const_cast<s::GuestRam&>(gc);
    SkyPacket p;
    p.address = at;
    p.entry = entry;
    const uint32_t size = (g.U32(at) >> 24) & 0x7Fu;
    if (size == 8u) { // the gradient's POLY_G4 (0x800642F8): colour, xy per vertex
        p.cmd = g.U32(at + 4u) >> 24;
        p.colour = g.U32(at + 4u) & 0xFFFFFFu;
        for (uint32_t k = 0; k < 4; ++k) {
            p.rgb[k] = g.U32(at + 4u + 8u * k) & 0xFFFFFFu;
            const uint32_t xy = g.U32(at + 8u + 8u * k);
            p.x[k] = static_cast<int16_t>(xy & 0xFFFFu);
            p.y[k] = static_cast<int16_t>(xy >> 16);
        }
        return p;
    }
    if (size == 5u) { // the two-player sky's DR_TPAGE + SPRT (0x80064CC8)
        p.tpage = static_cast<uint16_t>(g.U32(at + 4u) & 0x1FFu);
        p.cmd = g.U32(at + 8u) >> 24;
        p.colour = g.U32(at + 8u) & 0xFFFFFFu;
        const uint32_t xy = g.U32(at + 12u), uv = g.U32(at + 16u), wh = g.U32(at + 20u);
        p.x[0] = static_cast<int16_t>(xy & 0xFFFFu);
        p.y[0] = static_cast<int16_t>(xy >> 16);
        p.u[0] = static_cast<uint8_t>(uv & 0xFFu);
        p.v[0] = static_cast<uint8_t>((uv >> 8) & 0xFFu);
        p.clut = static_cast<uint16_t>(uv >> 16);
        p.w = static_cast<int16_t>(wh & 0x3FFu);
        p.h = static_cast<int16_t>((wh >> 16) & 0x1FFu);
        return p;
    }
    const uint32_t w1 = g.U32(at + 4u);
    p.cmd = w1 >> 24;
    p.colour = w1 & 0xFFFFFFu;
    for (uint32_t k = 0; k < 4; ++k) {
        const uint32_t xy = g.U32(at + 8u + 8u * k), uv = g.U32(at + 12u + 8u * k);
        p.x[k] = static_cast<int16_t>(xy & 0xFFFFu);
        p.y[k] = static_cast<int16_t>(xy >> 16);
        p.u[k] = static_cast<uint8_t>(uv & 0xFFu);
        p.v[k] = static_cast<uint8_t>((uv >> 8) & 0xFFu);
        if (k == 0) p.clut = static_cast<uint16_t>(uv >> 16);
        if (k == 1) p.tpage = static_cast<uint16_t>(uv >> 16);
    }
    return p;
}

// psxgpu.py's `--prims` CSV of the original's traced frame: its sky primitives (0x2C on a tile-cache page, 0x2F on a
// cloud slice's page) in drawing order against this frame's packets, vertex for vertex (x, y, u, v).
void ComparePrims(const char* path) {
    FILE* f = std::fopen(path, "r");
    if (f == nullptr) return;
    std::vector<std::array<int, 18>> rows; // cmd, tpage, then x y u v per vertex
    char line[1024];
    while (std::fgets(line, sizeof(line), f)) {
        std::vector<std::string> c;
        std::string cur;
        for (const char* q = line; *q && *q != 10 && *q != 13; ++q) {
            if (*q == ',') {
                c.push_back(cur);
                cur.clear();
            } else {
                cur += *q;
            }
        }
        c.push_back(cur);
        if (c.size() >= 32 && c[0] == "0x38") { // a gradient quad - equal to one of this frame's G4s
            ++T().gradTotal;
            for (const SkyPacket& p : Packets()) {
                if (p.cmd != 0x38u) continue;
                bool same = true;
                for (int k = 0; k < 4; ++k) {
                    const uint32_t rgb = static_cast<uint32_t>(std::atoi(c[8 + 7 * k].c_str())) |
                                         (static_cast<uint32_t>(std::atoi(c[9 + 7 * k].c_str())) << 8) |
                                         (static_cast<uint32_t>(std::atoi(c[10 + 7 * k].c_str())) << 16);
                    same = same && std::atoi(c[4 + 7 * k].c_str()) == p.x[k] && std::atoi(c[5 + 7 * k].c_str()) == p.y[k] &&
                           rgb == p.rgb[k];
                }
                if (same) {
                    ++T().gradEqual;
                    break;
                }
            }
            continue;
        }
        if (c.size() < 32 || (c[0] != "0x2C" && c[0] != "0x2F") || c[1].empty()) continue;
        std::array<int, 18> r{};
        r[0] = static_cast<int>(std::strtol(c[0].c_str(), nullptr, 16));
        r[1] = static_cast<int>(std::strtol(c[1].c_str(), nullptr, 16));
        for (int k = 0; k < 4; ++k)
            for (int j = 0; j < 4; ++j) r[2 + 4 * k + j] = std::atoi(c[4 + 7 * k + j].c_str());
        rows.push_back(r);
    }
    std::fclose(f);
    std::vector<SkyPacket> ft4; // the panorama's and the clouds' (the gradient's G4s are compared above)
    for (const SkyPacket& p : Packets())
        if (p.cmd >= 0x2Cu && p.cmd <= 0x2Fu) ft4.push_back(p);
    std::vector<std::array<int, 18>> sky;
    for (const auto& r : rows)
        for (const SkyPacket& p : ft4)
            if (static_cast<uint32_t>(r[0]) == p.cmd && static_cast<uint16_t>(r[1]) == p.tpage) {
                sky.push_back(r);
                break;
            }
    Totals& t = T();
    t.primsTotal += sky.size();
    for (size_t k = 0; k < sky.size() && k < ft4.size(); ++k) {
        const SkyPacket& p = ft4[k];
        bool same = static_cast<uint32_t>(sky[k][0]) == p.cmd;
        for (int v = 0; v < 4; ++v)
            same = same && sky[k][2 + 4 * v] == p.x[v] && sky[k][3 + 4 * v] == p.y[v] && sky[k][4 + 4 * v] == p.u[v] &&
                   sky[k][5 + 4 * v] == p.v[v];
        if (same) ++t.primsEqual;
    }
}

} // namespace

bool SkyPortOn() {
    static const bool on = !EnvOff("RRJB_SKY2");
    return on && StreamSkyPorted();
}

const std::vector<SkyPacket>& SkyFramePackets() { return Packets(); }
const std::vector<SkyPacket>* SkyFramePacketsOf(int view) {
    const std::vector<SkyPacket>& p = Packets(static_cast<uint32_t>(view));
    return p.empty() ? nullptr : &p;
}
bool SkyGradientPorted() { return SkyPortOn() && Sky3On(); }
void SkySetPicture(int view, double left, double right) {
    Picture& p = PictureOf(static_cast<uint32_t>(view));
    p.set = std::isfinite(left) && std::isfinite(right);
    p.left = left;
    p.right = right;
}
const SkyVram& SkyHostVram() { return Vram(); }

// ============================================================================ level load
std::string SkySetUpBefore(s::GuestRam& g) {
    if (!SkyPortOn()) return "the sky tiles SWITCHED OFF (RRJB_SKY2=off): the renderer's own sky";
    // RASHCDI 0x80060BE8(1): the cache origin from the configuration table's entry 1 (0x800533D8 + 1 / + 2)
    const uint32_t b = g.U8(0x800533DAu), yLow = g.U8(0x800533D9u);
    g.W32(s::kSkyVramX, (b & 0xFu) << 6);
    g.W32(s::kSkyVramY, (b & 0x10u) * 0x10u + yLow);
    // 0x800201C0 / 0x8002026C(0): the DctVlc tables by the PORTED VlcBuild 0x800202B8 from the arena's descriptors
    // (0x800528A0 / 0x80052A20, symbols 0x80052C20) - OURS: built in a scratch image and kept host-side, the arena's
    // bump region has no room for their 26 KiB (the loads placed after the sky block ran out of it)
    std::vector<uint8_t> image(s::GuestRam::kRamSize, 0);
    g.ReadBlock(0x80052000u, image.data() + 0x52000u, 0x1000u);
    s::GuestRam t(image.data(), kGp);
    constexpr uint32_t kTab0 = 0x80100000u, kTab1 = 0x80108000u;
    s::VlcBuild(t, kTab0, kTab1, 0);
    VlcTables().tab0.assign(image.begin() + 0x100000, image.begin() + 0x106000);
    VlcTables().tab1.assign(image.begin() + 0x108000, image.begin() + 0x108600);
    VlcReady() = !t.Faulted();
    char line[200];
    std::snprintf(line, sizeof(line),
                  "the sky tiles: the tile cache at VRAM (%u, %u), the DctVlc tables host-side (VlcBuild 0x800202B8 "
                  "PORTED)%s",
                  g.U32(s::kSkyVramX), g.U32(s::kSkyVramY), VlcReady() ? "" : " - FAILED");
    T().setUp = line;
    return line;
}

std::string SkySetUpAfter(s::GuestRam& g) {
    if (!SkyPortOn() || g.U32(s::kSkyBlockPtr) == 0) return "the sky tiles: no tile records (the port is off)";
    s::TileRecords(g);
    // the sky OT (0x800C89A0: view 0 of buffer 0)
    g.W32(s::kSkyOtPtr, kSkyOtAt);
    char line[160];
    std::snprintf(line, sizeof(line), "the sky tiles: %u cache slot records (TileRecords 0x80060AA8 PORTED), the sky OT at 0x%08X",
                  g.U16(g.U32(s::kSkyBlockPtr) + 0x28u), kSkyOtAt);
    return line;
}

namespace {
std::string SplitSkySetUp(s::GuestRam& g, const rr::DiscImage& disc, int raceId);
std::string CloudSetUp(s::GuestRam& g, const rr::DiscImage& disc, int raceId);

// RASHCDI 0x800619A0: the level file is "data\gamebin%d.dat" of game_state+0x30 (GAMEBIN2.DAT for two players - its
// bundles carry the type-10 section), entry [raceId - 1] of its table; one player keeps SelectLevelBundle's answer
// (the resident-bundle rule, level_bundle.h).
bool LevelFile(s::GuestRam& g, const rr::DiscImage& disc, int raceId, std::vector<uint8_t>& file, rr::LevelBundle& bundle,
               std::string& name, std::string& why) {
    const uint32_t players = g.U32(g.U32(s::kSkyGameState) + 0x30u);
    name = players == 2 ? "DATA/GAMEBIN2.DAT" : "DATA/GAMEBIN1.DAT";
    const auto bin = disc.Find(name);
    if (!bin) {
        why = name + " is not on the disc";
        return false;
    }
    file = disc.ReadFile(*bin);
    try {
        const size_t index = players == 2 ? static_cast<size_t>(raceId > 0 ? raceId - 1 : 0) : rr::LevelBundleIndexForRace(raceId);
        bundle = rr::ParseLevelBundle(file, index);
    } catch (const std::exception& e) {
        why = e.what();
        return false;
    }
    return true;
}
} // namespace

std::string SkyCloudSetUp(s::GuestRam& g, const rr::DiscImage& disc, int raceId) {
    const std::string split = SplitSkySetUp(g, disc, raceId); // the gradient / two-player sky tables
    T().splitSetUp = split;
    return split + "\n" + CloudSetUp(g, disc, raceId);
}

namespace {
std::string CloudSetUp(s::GuestRam& g, const rr::DiscImage& disc, int raceId) {
    if (!SkyPortOn()) return "the sky tiles: no cloud tables (RRJB_SKY2=off)";
    std::vector<uint8_t> file;
    rr::LevelBundle lb;
    std::string fileName, fileWhy;
    if (Sky3On()) { // the players' level file (GAMEBIN2.DAT for two players)
        if (!LevelFile(g, disc, raceId, file, lb, fileName, fileWhy)) return "the sky tiles: no cloud tables: " + fileWhy;
    } else {
        const auto bin = disc.Find("DATA/GAMEBIN1.DAT");
        if (!bin) return "the sky tiles: no cloud tables (DATA/GAMEBIN1.DAT is not on the disc)";
        file = disc.ReadFile(*bin);
    }
    rr::SkyClouds c;
    try {
        c = rr::ParseSkyClouds(file, Sky3On() ? lb : rr::ParseLevelBundle(file, rr::LevelBundleIndexForRace(raceId)));
    } catch (const std::exception& e) {
        return std::string("the sky tiles: no cloud tables: ") + e.what();
    }
    if (!c.valid) return "the sky tiles: this level bundle has no cloud layer (type 3); 0x8005B308 stays 0";
    // RASHCDI 0x80060F58(payload, players): the TIM to the configuration table's group-12 rectangle and its CLUT
    // 63 rows below (LoadImage - the host's VRAM), the band words, four slices, the ring, the draw flag.
    const uint32_t players = g.U32(g.U32(s::kSkyGameState) + 0x30u);
    const uint32_t iv = (players - 1u) * 0x58u;
    const uint32_t e6 = g.U8(0x800533E6u + iv), e5 = g.U8(0x800533E5u + iv);
    const int px = static_cast<int>((e6 & 0xFu) << 6), py = static_cast<int>(e5 + (e6 & 0x10u) * 0x10u);
    SkyVram& v = Vram();
    const int wHalf = c.width / 4;
    for (int y = 0; y < c.rows; ++y)
        for (int x = 0; x < wHalf; ++x) {
            uint16_t hw = 0;
            for (int k = 0; k < 4; ++k)
                hw = static_cast<uint16_t>(hw | (c.indices[static_cast<size_t>(y) * static_cast<size_t>(c.width) +
                                                           static_cast<size_t>(x * 4 + k)] << (4 * k)));
            v.px[static_cast<size_t>((py + y) & 511) * SkyVram::kWidth + static_cast<size_t>((px + x) & 1023)] = hw;
        }
    for (int k = 0; k < 16; ++k)
        v.px[static_cast<size_t>((py + 63) & 511) * SkyVram::kWidth + static_cast<size_t>((px + k) & 1023)] = c.clut[k];
    ++v.generation;
    g.W32(s::kCloudBand, static_cast<uint32_t>(c.height));
    g.W32(s::kCloudBand + 4u, static_cast<uint32_t>(c.top));
    g.W32(s::kCloudBand + 0xCu, c.field0C);
    g.W32(s::kCloudBand + 0x10u, c.field10);
    // the slices: CloudSlice(0x800D440C + 12 k, 8, {x + 16 k, y, 16 (15 for the last), 0x3E}, clut x, clut y)
    constexpr uint32_t rect = 0x801FFA00u; // OURS: the RECT the original keeps in its frame (the product's stack)
    s::GuestRam& gs = g;
    for (uint32_t k = 0; k < 4; ++k) {
        gs.W32(rect, static_cast<uint32_t>(px) + 16u * k);
        gs.W32(rect + 4u, static_cast<uint32_t>(py));
        gs.W32(rect + 8u, k == 3u ? 15u : 16u);
        gs.W32(rect + 12u, 0x3Eu);
        s::CloudSlice(gs, s::kCloudSlices + 12u * k, 8u, rect, static_cast<uint32_t>(px), static_cast<uint32_t>(py + 63));
    }
    s::CloudRing(g);
    g.W32(s::kCloudOn, 1u);
    char line[220];
    std::snprintf(line, sizeof(line),
                  "the sky tiles: the cloud layer of level bundle %zu (%dx%d 4-bit at VRAM (%d, %d), CLUT (%d, %d)), "
                  "slices 0x80060780 / ring 0x80061100 PORTED, 0x8005B308 = 1",
                  c.bundle, c.width, c.rows, px, py, px, py + 63);
    return line;
}
} // namespace

// ============================================================================ per frame
bool SkyDecodeFromStream(s::GuestRam& g, uint32_t sp) {
    if (!SkyPortOn() || !VlcReady()) return false;
    s::SetVlcHostTables(&VlcTables());
    Callees c(g);
    if (!s::DecodeStart(g, sp, c)) {
        ++T().refused;
        g.ClearFault();
    }
    return true;
}

namespace {

// OURS (the wide picture): the view's picture reaches `left` / `right` console pixels past the view's own
// edges; the original's field in angle units (atan of the edge over H = 237) opened by as much - the gradient's edge
// yaw, and the panorama's columns (4096 / 110 units) and the cloud ring's segments (4096 / 24) that cover it.
bool WideOf(s::GuestRam& g, uint32_t view, int players, int32_t ofx, s::SkyWide& w) {
    const Picture& p = PictureOf(view);
    if (!p.set) return false;
    double x0 = 0, x1 = 384;
    if (players == 2) {
        s::SplitRect r;
        if (s::SplitViewRect(g, view, r)) x0 = r.x, x1 = r.x + r.w;
    }
    w.left = std::max(0, static_cast<int32_t>(std::ceil(x0 - p.left - 0.5))); // half a pixel: the projection's rounding
    w.right = std::max(0, static_cast<int32_t>(std::ceil(p.right - x1 - 0.5)));
    if (w.left == 0 && w.right == 0) return false; // the console's field: the original, bit for bit
    constexpr double kUnit = 4096.0 / 6.283185307179586, kH = 237.0;
    const double aL = (std::atan((ofx - (x0 - w.left)) / kH) - std::atan((ofx - x0) / kH)) * kUnit;
    const double aR = (std::atan((x1 + w.right - ofx) / kH) - std::atan((x1 - ofx) / kH)) * kUnit;
    w.edgeAngle = 426 + static_cast<int32_t>(std::lround((aL + aR) * 0.5));
    w.columnsLeft = static_cast<int32_t>(std::ceil(aL / (4096.0 / 110.0)));
    w.columnsRight = static_cast<int32_t>(std::ceil(aR / (4096.0 / 110.0)));
    w.segmentsLeft = static_cast<int32_t>(std::ceil(aL / (4096.0 / 24.0)));
    w.segmentsRight = static_cast<int32_t>(std::ceil(aR / (4096.0 / 24.0)));
    return true;
}

// A packet of the sky OT as the GPU takes it: FT4 (9 words), G4 (8), DR_TPAGE + SPRT (5).
bool SkyPacketSize(uint32_t tag) {
    const uint32_t n = (tag >> 24) & 0x7Fu; // KSEG0 addresses leave bit 31 in the size byte
    return n == 9u || n == 8u || n == 5u;
}

} // namespace

bool SkyFrameDraw(uint8_t* ram, int players, uint32_t frame) {
    Packets(0).clear();
    Packets(1).clear();
    if (!SkyPortOn() || (players != 1 && (players != 2 || !Sky3On()))) return false;
    s::GuestRam g(ram, kGp);
    if (g.U32(s::kSkyBlockPtr) == 0 || !VlcReady() || g.U32(s::kSkyOtPtr) == 0) {
        ++T().noTables;
        return false;
    }
    s::SetVlcHostTables(&VlcTables());
    ++T().frames;
    g.SetScratchpad(Scratch().data());
    bool any = false;
    for (uint32_t view = 0; view < static_cast<uint32_t>(players); ++view) {
        std::vector<SkyPacket>& list = Packets(view);
        // the view's sky OT: 0x800C8B24 points *(0x8005B59C) at 0x800D9BE0 + 0x28 buffer + 0x14 view (buffer 0 - the
        // frame flip is not modelled); a --parity capture keeps its own (one player)
        const uint32_t ownOt = kSkyOtAt + 0x14u * view;
        if (players == 2) g.W32(s::kSkyOtPtr, ownOt);
        const uint32_t ot = g.U32(s::kSkyOtPtr);
        if (ot == ownOt) // the product's own OT: 0x800C89A0's ClearOTagR(ot, 5) for this frame
            for (uint32_t k = 0; k < 5; ++k) g.W32(ot + 4u * k, k == 0 ? 0x00FFFFFFu : ((ot + 4u * (k - 1u)) & 0x00FFFFFFu));
        uint32_t before[5];
        for (uint32_t k = 0; k < 5; ++k) before[k] = g.U32(ot + 4u * k);
        s::FxGte gte = OnePlayerGte();
        if (players == 2) { // SLUS 0x80011C4C: the view rectangle's centre (fx_runtime.cpp, as the model pass)
            s::SplitRect r;
            if (s::SplitViewRect(g, view, r)) gte.ofx = r.cx << 16, gte.ofy = r.cy << 16;
        }
        s::SkyWide wide;
        s::SkyDrawOptions opts;
        opts.seams = !Sky3On();
        if (!opts.seams && ot == ownOt && WideOf(g, view, players, gte.ofx >> 16, wide)) {
            opts.wide = &wide;
            ++T().wideFrames;
        }
        Callees c(g);
        // OURS: SkyFrame keeps the wide picture's columns decoded too (stream_cd.h SetSkyFrameWide)
        s::SetSkyFrameWide(opts.wide != nullptr ? wide.columnsLeft : 0, opts.wide != nullptr ? wide.columnsRight : 0);
        const bool ok = s::SkyDraw(g, view, gte, kSkySp, c, opts);
        s::SetSkyFrameWide(0, 0);
        T().widePanoAdded += static_cast<size_t>(wide.panoAdded);
        T().widePanoMissing += static_cast<size_t>(wide.panoMissing);
        if (!ok || g.Faulted()) {
            ++T().refused;
            g.ClearFault();
            list.clear();
            continue;
        }
        ++T().viewDraws;
        // the GPU walks the table from entry 4 down: entry 3 (the gradient), 1 (the clouds), 0 (the panorama, or the
        // two-player sky's sprites); within an entry the last linked packet first. Only what this draw linked (a
        // capture's OT still holds the capture's own).
        size_t pano = 0, clouds = 0;
        for (int e = 3; e >= 0; --e) {
            uint32_t at = g.U32(ot + 4u * static_cast<uint32_t>(e)) & 0x00FFFFFFu;
            for (int guard = 0; guard < 4096 && at != (before[e] & 0x00FFFFFFu) && at != 0x00FFFFFFu; ++guard) {
                const uint32_t addr = at | 0x80000000u;
                const uint32_t tag = g.U32(addr);
                if (SkyPacketSize(tag)) {
                    list.push_back(ReadPacket(g, addr, e));
                    const SkyPacket& p = list.back();
                    if (p.cmd == 0x38u) ++T().gradPackets;
                    else if (p.w != 0) ++T().splitPackets;
                    else ++(e == 0 ? pano : clouds);
                }
                at = tag & 0x00FFFFFFu;
            }
        }
        if (ot != ownOt) { // --parity: the capture's own packets of this frame are still linked after ours
            for (int e = 3; e >= 0; --e) {
                if (e == 2) continue; // entry 2: 0x8002C4F8's (not the sky draw's)
                std::vector<uint32_t> theirs;
                const uint32_t stop = e == 0 ? 0x00FFFFFFu : ((ot + 4u * static_cast<uint32_t>(e - 1)) & 0x00FFFFFFu);
                uint32_t at = before[e] & 0x00FFFFFFu;
                for (int guard = 0; guard < 4096 && at != stop && at != 0x00FFFFFFu; ++guard) {
                    const uint32_t tag = g.U32(at | 0x80000000u);
                    if (SkyPacketSize(tag)) theirs.push_back(at | 0x80000000u);
                    at = tag & 0x00FFFFFFu;
                }
                std::vector<uint32_t> mine;
                for (const SkyPacket& p : list)
                    if (p.entry == e) mine.push_back(p.address);
                T().captureTotal += theirs.size();
                for (size_t k = 0; k < theirs.size() && k < mine.size(); ++k) {
                    const uint32_t n = (g.U32(theirs[k]) >> 24) & 0x7Fu;
                    bool same = n == ((g.U32(mine[k]) >> 24) & 0x7Fu);
                    for (uint32_t w = 1; w <= n && same; ++w) {
                        // the G4's colours 1..3 leave their top byte as the heap had it (0x800642F8 writes three)
                        const uint32_t mask = (n == 8u && (w == 3u || w == 5u || w == 7u)) ? 0x00FFFFFFu : 0xFFFFFFFFu;
                        same = (g.U32(theirs[k] + 4u * w) & mask) == (g.U32(mine[k] + 4u * w) & mask);
                    }
                    if (same) ++T().captureEqual;
                }
            }
        }
        if (view == 0)
            if (const char* csv = std::getenv("RRJB_SKY2_PRIMS")) // --parity check: the traced frame's own sky primitives
                ComparePrims(csv);
        if (const char* dump = std::getenv("RRJB_SKY2_DUMP")) // DEVELOPMENT: the frame's packets as text
            if (FILE* f = std::fopen(dump, "a")) {
                for (const SkyPacket& p : list)
                    std::fprintf(f, "%u v%u %d 0x%02X %04X %04X %d,%d %d,%d %d,%d %d,%d  %u,%u %u,%u %u,%u %u,%u  %dx%d %06X\n", frame,
                                 view, p.entry, p.cmd, p.tpage, p.clut, p.x[0], p.y[0], p.x[1], p.y[1], p.x[2], p.y[2], p.x[3],
                                 p.y[3], p.u[0], p.v[0], p.u[1], p.v[1], p.u[2], p.v[2], p.u[3], p.v[3], p.w, p.h, p.colour);
                std::fclose(f);
            }
        Totals& t = T();
        t.panoPackets += pano;
        t.cloudPackets += clouds;
        t.maxPano = std::max(t.maxPano, pano);
        t.maxClouds = std::max(t.maxClouds, clouds);
        any = any || !list.empty();
    }
    g.SetScratchpad(nullptr);
    if (any) ++T().drawn;
    return any;
}

// ============================================================================ the parity capture's cache
// RRJB_SKY2_VRAM=<capture vram.bin>: the rebuilt tiles against the capture's own VRAM, halfword for halfword - the
// capture's MDEC output (the reference emulator's) against ours. A tile the capture had
// not refilled yet (another column in the slot) shows as a tile with few equal halfwords.
std::string SkyTilesAgainstCapture(const std::vector<std::pair<int, int>>& tileAt) {
    const char* path = std::getenv("RRJB_SKY2_VRAM");
    if (path == nullptr) return "";
    std::vector<uint8_t> raw(1024u * 512u * 2u);
    FILE* f = std::fopen(path, "rb");
    const size_t got = f != nullptr ? std::fread(raw.data(), 1, raw.size(), f) : 0;
    if (f != nullptr) std::fclose(f);
    if (got != raw.size()) return "; RRJB_SKY2_VRAM: cannot read " + std::string(path);
    size_t total = 0, equal = 0, near1 = 0, exactTiles = 0, staleTiles = 0, liveTotal = 0, liveEqual = 0;
    for (const auto& [x0, y0] : tileAt) {
        size_t eq = 0, nr = 0;
        for (int yy = 0; yy < 16; ++yy)
            for (int xx = 0; xx < 16; ++xx) {
                const size_t at = static_cast<size_t>((y0 + yy) & 511) * 1024u + static_cast<size_t>((x0 + xx) & 1023);
                const uint16_t theirs = static_cast<uint16_t>(raw[2 * at] | (raw[2 * at + 1] << 8));
                const uint16_t ours = Vram().px[at];
                if (theirs == ours) ++eq;
                bool close = (theirs & 0x8000u) == (ours & 0x8000u);
                for (int c = 0; c < 15; c += 5) {
                    const int d = static_cast<int>((theirs >> c) & 31u) - static_cast<int>((ours >> c) & 31u);
                    close = close && d >= -1 && d <= 1;
                }
                if (close) ++nr;
            }
        total += 256;
        equal += eq;
        near1 += nr;
        if (eq == 256) ++exactTiles;
        if (nr < 128) ++staleTiles; // not this column's pixels at all
        else {
            liveTotal += 256;
            liveEqual += eq;
        }
    }
    char b[400];
    std::snprintf(b, sizeof(b),
                  "; tiles against the capture's VRAM (MDEC %s): %zu tiles, %zu bit-exact, %zu not refilled by the capture; "
                  "halfwords equal %zu of %zu (%.2f %%), within one step %zu; in the refilled tiles %zu of %zu (%.2f %%)",
                  rr::mdec::ModelName(rr::mdec::ActiveModel()), tileAt.size(), exactTiles, staleTiles, equal, total,
                  total ? 100.0 * static_cast<double>(equal) / static_cast<double>(total) : 0.0, near1, liveEqual, liveTotal,
                  liveTotal ? 100.0 * static_cast<double>(liveEqual) / static_cast<double>(liveTotal) : 0.0);
    return b;
}

std::string SkyParityRebuild(const uint8_t* ram) {
    if (!SkyPortOn()) return "sky2: the port is off";
    std::vector<uint8_t> copy(ram, ram + s::GuestRam::kRamSize);
    s::GuestRam g(copy.data(), kGp);
    g.SetScratchpad(Scratch().data());
    const uint32_t sky = g.U32(s::kSkyBlockPtr);
    if (sky == 0 || !VlcReady()) return "sky2: no sky block in the capture / no DctVlc tables";
    s::SetVlcHostTables(&VlcTables());
    const int32_t ring = g.S16(sky + 0x26u), cap = g.S16(sky + 0x28u), head = g.S16(sky + 0x2Eu), tail = g.S16(sky + 0x2Cu);
    int32_t slot = g.S16(sky + 0x24u);
    if (!(slot < ring)) slot -= ring;
    if (head < slot && slot < tail) slot = tail;
    int32_t limit = head < slot ? cap : head + 1;
    const uint32_t codes = g.U32(sky + 0x3DCu) + 8u, horz = g.U32(sky + 0x3E4u) + 0x48u;
    Callees c(g);
    int32_t col = g.S16(sky + 0x34u);
    const int32_t last = g.S16(sky + 0x36u);
    int32_t decodedPair = -1;
    size_t tiles = 0, columns = 0;
    std::vector<std::pair<int, int>> tileAt; // the rebuilt tiles' VRAM corners (RRJB_SKY2_VRAM)
    for (int n = 0; n < 110; ++n) {
        const uint32_t count = g.U8(sky + static_cast<uint32_t>(col) + 0xB6u);
        const int32_t pair = col / 2;
        const uint32_t e = g.U32(sky + 1000u + 4u * static_cast<uint32_t>(pair));
        if (count != 0 && e != 0 && g.U32(e + 12u) != 0) {
            if (pair != decodedPair) { // DctVlc + the MDEC on the copy, exactly as DecodeStart does
                const uint32_t s1 = e + 8u;
                const uint32_t vlcOut = sky - (static_cast<uint32_t>(g.U16(s1 + 8u)) * 4u - 9920u);
                s::DctVlc(g, s1 + 8u, vlcOut);
                const uint32_t args[2] = {vlcOut, static_cast<uint32_t>(g.S16(sky + 22u))};
                uint32_t v0 = 0;
                c.Call(s::kDecDctInFn, args, 2, 0, v0);
                TheMdec().Out(g, sky + 1220u, (g.U32(s1 + 4u) * g.U32(s1)) / 2u);
                decodedPair = pair;
            }
            // the column's tiles: its non-empty STEN bands in order, a code-2 one cut by the next HORZ record
            uint32_t tile = sky + 0x4C4u + static_cast<uint32_t>(g.S16(sky + 0x124u + 2u * static_cast<uint32_t>(col)));
            uint32_t rec = horz + static_cast<uint32_t>(g.S16(sky + 0x200u + 2u * static_cast<uint32_t>(col))) * 8u;
            for (uint32_t band = 0; band < 8u; ++band) {
                const uint32_t bit = static_cast<uint32_t>(col) * 8u + band;
                const uint32_t code = (g.U8(codes + bit / 4u) >> (6u - 2u * (bit % 4u))) & 3u;
                if (code == 0) continue;
                if (code == 2u) {
                    s::HorzCut(g, tile, rec);
                    rec += 8u;
                }
                // the draw's slot walk
                const uint32_t r = sky + 0x2744u + 12u * static_cast<uint32_t>(slot);
                const uint16_t tpage = g.U16(r + 6u);
                const int vx = (tpage & 0xF) * 64 + (g.U8(r + 8u) & 0xFF), vy = ((tpage >> 4) & 1) * 256 + g.U8(r + 1u);
                for (int yy = 0; yy < 16; ++yy)
                    for (int xx = 0; xx < 16; ++xx)
                        Vram().px[static_cast<size_t>((vy + yy) & 511) * SkyVram::kWidth + static_cast<size_t>((vx + xx) & 1023)] =
                            g.U16(tile + static_cast<uint32_t>(yy * 16 + xx) * 2u);
                ++tiles;
                tileAt.emplace_back(vx, vy);
                tile += 512u;
                ++slot;
                if (!(slot < limit)) {
                    if (slot == head + 1) {
                        slot = tail;
                        limit = cap;
                    } else if (!(slot < cap)) {
                        slot = 0;
                        limit = head + 1;
                    }
                }
            }
            ++columns;
        } else {
            slot += static_cast<int32_t>(count); // a column without its strip keeps its slots (not expected)
        }
        if (col == last) break;
        col = col == 109 ? 0 : col + 1;
    }
    ++Vram().generation;
    std::string vramCheck = SkyTilesAgainstCapture(tileAt);
    char line[200];
    std::snprintf(line, sizeof(line), "sky2: the capture's tile cache decoded again: columns %d..%d, %zu tiles in %zu columns%s",
                  g.S16(sky + 0x34u), last, tiles, columns, g.Faulted() ? " (a fault!)" : "");
    return std::string(line) + vramCheck;
}

std::string SkyTotals() {
    const Totals& t = T();
    char b[900];
    char parity[256] = "";
    if (t.captureTotal != 0)
        std::snprintf(parity, sizeof(parity), "; the capture's own sky packets: %zu of %zu equal word for word", t.captureEqual,
                      t.captureTotal);
    if (t.primsTotal != 0)
        std::snprintf(parity + std::strlen(parity), sizeof(parity) - std::strlen(parity),
                      "; the traced frame's sky primitives: %zu of %zu equal", t.primsEqual, t.primsTotal);
    std::snprintf(b, sizeof(b),
                  "sky2: %s; %zu frame(s) drawn by the PORTED sky draw 0x80064B9C of %zu, %zu panorama packets (max %zu a "
                  "frame), %zu cloud packets (max %zu), %zu MDEC strip decodes (%zu blocks, arithmetic %s), %zu tile uploads, refused %zu, "
                  "heap full %zu, unported callees %zu, frames without tables %zu, MDEC commands refused %zu%s%s%s",
                  SkyPortOn() ? "PORTED" : "OFF (RRJB_SKY2=off: the renderer's sky)", t.drawn, t.frames, t.panoPackets,
                  t.maxPano, t.cloudPackets, t.maxClouds, t.decodes, t.mdecBlocks, rr::mdec::ModelName(rr::mdec::ActiveModel()), t.uploads,
                  t.refused, t.heapFull,
                  t.unported, t.noTables, t.badMdec, parity, t.why.empty() ? "" : "; last refusal: ", t.why.c_str());
    char grad[160] = "";
    if (t.gradTotal != 0)
        std::snprintf(grad, sizeof(grad), "; the traced frame's gradient quads: %zu of %zu equal", t.gradEqual, t.gradTotal);
    char b3[900];
    std::snprintf(b3, sizeof(b3),
                  "\nsky3: %s; %zu view draw(s), %zu gradient packets (0x80063C5C), %zu two-player sky packets (0x80064CC8), "
                  "heap flushes %zu (0x80021BE8: tables drawn early by the console %zu, cleared %zu), wide frames %zu "
                  "(panorama columns added %zu, not in the decoded window %zu)%s",
                  SkyGradientPorted() ? "PORTED (the gradient, the two-player sky, the heap manager)"
                                      : "OFF (RRJB_SKY3=off: the renderer's gradient and two-player sky)",
                  t.viewDraws, t.gradPackets, t.splitPackets, t.flushes, t.drawOTags, t.clears, t.wideFrames,
                  t.widePanoAdded, t.widePanoMissing, grad);
    return std::string(b) + b3;
}

// ============================================================================ the gradient / two-player sky: level load
namespace {

constexpr uint32_t kSetUpScratch = 0x801FE000u; // OURS: where the type-10 section is laid for 0x800627A8 (then cleared)

void PutVram(int x, int y, uint16_t v) {
    Vram().px[static_cast<size_t>(y & 511) * SkyVram::kWidth + static_cast<size_t>(x & 1023)] = v;
}

uint32_t Le32(const std::vector<uint8_t>& f, size_t at) {
    return at + 4 <= f.size() ? static_cast<uint32_t>(f[at] | (f[at + 1] << 8) | (f[at + 2] << 16) | (static_cast<uint32_t>(f[at + 3]) << 24)) : 0u;
}
uint16_t Le16(const std::vector<uint8_t>& f, size_t at) {
    return at + 2 <= f.size() ? static_cast<uint16_t>(f[at] | (f[at + 1] << 8)) : uint16_t{0};
}

// ReadTIM's view of a TIM: the CLUT block (when flags bit 3) and the image block, each {x, y, w, h} + halfwords.
struct TimBlocks {
    bool ok = false, clut = false;
    uint16_t cr[4] = {}, pr[4] = {};
    size_t cdata = 0, pdata = 0;
};
TimBlocks ReadTim(const std::vector<uint8_t>& f) {
    TimBlocks t;
    if (Le32(f, 0) != 0x10u) return t;
    size_t at = 8;
    if (Le32(f, 4) & 8u) {
        const uint32_t n = Le32(f, at);
        for (int k = 0; k < 4; ++k) t.cr[k] = Le16(f, at + 4 + 2 * static_cast<size_t>(k));
        t.cdata = at + 12;
        t.clut = true;
        at += n;
    }
    for (int k = 0; k < 4; ++k) t.pr[k] = Le16(f, at + 4 + 2 * static_cast<size_t>(k));
    t.pdata = at + 12;
    t.ok = t.pdata + static_cast<size_t>(t.pr[2]) * t.pr[3] * 2 <= f.size();
    return t;
}
// LoadImage of a {x, y, w, h} from the file's halfwords at `data`.
void LoadRect(const std::vector<uint8_t>& f, size_t data, int x, int y, int w, int h) {
    for (int r = 0; r < h; ++r)
        for (int c = 0; c < w; ++c) PutVram(x + c, y + r, Le16(f, data + 2 * (static_cast<size_t>(r) * static_cast<size_t>(w) + static_cast<size_t>(c))));
    ++Vram().generation;
}

std::string SplitSkySetUp(s::GuestRam& g, const rr::DiscImage& disc, int raceId) {
    if (!SkyPortOn() || !Sky3On()) return "the sky tables: no gradient / two-player sky tables (RRJB_SKY3=off / RRJB_SKY2=off)";
    std::vector<uint8_t> file;
    rr::LevelBundle bundle;
    std::string fileName, why;
    if (!LevelFile(g, disc, raceId, file, bundle, fileName, why)) return "the sky tables: no gradient / two-player sky tables: " + why;
    char line[480];
    std::string grad;
    // the type-1 section through RASHCDI 0x80062384 PORTED (and 0x800611C4): the gradient's colours, 0x8005AD28 = 1
    if (const rr::LevelBundleSection* g1 = bundle.Find(1)) {
        const size_t at = g1->payload - 4;
        const size_t bytes = std::min<size_t>(4 + 16, file.size() - at);
        for (size_t k = 0; k < bytes; ++k) g.W8(kSetUpScratch + static_cast<uint32_t>(k), file[at + k]);
        s::GradLoad(g, kSetUpScratch, kSetUpScratch + 4u);
        for (size_t k = 0; k < bytes; ++k) g.W8(kSetUpScratch + static_cast<uint32_t>(k), 0);
        char b1[200];
        std::snprintf(b1, sizeof(b1), "the type-1 section through 0x80062384 PORTED (top %06X horizon %06X, sun %06X / %06X; 0x8005AD28 = %u); ",
                      g.U32(s::kGradTop) & 0xFFFFFFu, g.U32(s::kGradHorizon) & 0xFFFFFFu, g.U32(s::kGradSunA) & 0xFFFFFFu,
                      g.U32(s::kGradSunB) & 0xFFFFFFu, g.U32(s::kGradOn));
        grad = b1;
    } else {
        grad = "no type-1 section (0x8005AD28 stays " + std::to_string(g.U32(s::kGradOn)) + "); ";
    }
    const rr::LevelBundleSection* sec = bundle.Find(10);
    if (sec == nullptr) {
        std::snprintf(line, sizeof(line), "the sky tables: %s bundle %zu: %sno type-10 section (the two-player sky); 0x8005AD24 stays %u",
                      fileName.c_str(), bundle.index, grad.c_str(), g.U32(s::kSplitSkyOn2));
        return line;
    }
    // RASHCDI 0x800627A8 PORTED, on the section laid out in guest memory as the loader's buffer holds it
    const size_t at = sec->payload - 4;
    const size_t bytes = std::min<size_t>(4 + s::kSplitSkyBytes, file.size() - at);
    for (size_t k = 0; k < bytes; ++k) g.W8(kSetUpScratch + static_cast<uint32_t>(k), file[at + k]);
    s::SplitSkyLoad(g, kSetUpScratch, kSetUpScratch + 4u);
    for (size_t k = 0; k < bytes; ++k) g.W8(kSetUpScratch + static_cast<uint32_t>(k), 0);
    const uint32_t on = g.U32(s::kSplitSkyOn2);
    const uint32_t players = g.U32(g.U32(s::kSkyGameState) + 0x30u);
    const uint32_t split = g.U32(0x8005B2D0u);
    // RASHCDI 0x8006068C: one player on the ordinary stream takes the panorama (0x80060BE8)
    if (on == 0 || (players == 1 && split == 0)) {
        std::snprintf(line, sizeof(line),
                      "the sky tables: %s bundle %zu: %sthe type-10 section (tag 0x%X) through 0x800627A8 PORTED: 0x8005AD24 = %u; "
                      "%s",
                      fileName.c_str(), bundle.index, grad.c_str(), sec->tag, on,
                      on == 0 ? "no two-player sky" : "one player on the ordinary stream: the panorama");
        return line;
    }
    // RASHCDI 0x80060C78(players), transcribed: the 32 records' TIMs DATA\FE\<name> into the configuration table's
    // group-20 rectangle (0x800533B4 + 0x58 (players - 1) + 81 / 82), 8 x 32 rows a column of 32 halfwords
    const uint32_t cfg = 0x800533B4u + (players - 1u) * 0x58u;
    const uint32_t b81 = g.U8(cfg + 81u), b82 = g.U8(cfg + 82u);
    const uint32_t pageY = b81 + (b82 & 0x10u) * 0x10u;
    uint32_t loaded = 0, cluts = 0, missing = 0;
    for (uint32_t i = 0; i < s::kSplitSkyRecordCount; ++i) {
        const uint32_t rec = s::kSplitSkyRecords + s::kSplitSkyRecordBytes * i;
        std::string name;
        for (uint32_t k = 0; k < 32u; ++k) {
            const char ch = static_cast<char>(g.U8(rec + 12u + k));
            if (ch == 0) break;
            name += ch;
        }
        if (name.empty() || name.compare(0, 5, "dummy") == 0) continue; // strlen / strncmp(name, "dummy", 5)
        const auto tf = disc.Find("DATA/FE/" + name);
        if (!tf) {
            ++missing; // 0x8001408C fails: the record keeps what the section gave it
            continue;
        }
        const std::vector<uint8_t> tim = disc.ReadFile(*tf);
        const TimBlocks t = ReadTim(tim);
        if (!t.ok) {
            ++missing;
            continue;
        }
        const uint16_t px = static_cast<uint16_t>((b82 & 0xFu) * 64u + (loaded >> 3) * 32u);
        const uint16_t py = static_cast<uint16_t>(pageY + (loaded & 7u) * 32u);
        LoadRect(tim, t.pdata, px, py, t.pr[2], t.pr[3]);
        g.W32(rec + 4u, 0xE1000200u | ((pageY & 0x100u) >> 4) | (b82 & 0xFu) | 0x80u | ((pageY & 0x200u) << 2));
        g.W8(rec + 0u, static_cast<uint8_t>((px & 0xFFu) << 1));
        g.W8(rec + 1u, static_cast<uint8_t>(py & 0xFFu));
        ++loaded;
        if (g.U32(rec + 8u) == i && t.clut) { // this record owns its CLUT: 128 entries under its column of tiles
            const uint16_t cx = static_cast<uint16_t>((b82 & 0xFu) << 6);
            const uint16_t cy = static_cast<uint16_t>(pageY + cluts * 32u + 31u);
            LoadRect(tim, t.cdata, cx, cy, 128, t.cr[3]);
            ++cluts;
            g.W16(rec + 2u, static_cast<uint16_t>((cy << 6) | ((cx >> 4) & 0x3Fu)));
        }
    }
    for (uint32_t i = 0; i < s::kSplitSkyRecordCount; ++i) { // the records that share an owner's CLUT
        const uint32_t rec = s::kSplitSkyRecords + s::kSplitSkyRecordBytes * i;
        const uint32_t owner = g.U32(rec + 8u);
        if (owner != i) g.W16(rec + 2u, g.U16(s::kSplitSkyRecords + owner * s::kSplitSkyRecordBytes + 2u));
    }
    // 0x80060EFC: the 27 columns' x (64 apart); 0x80013F8C: the counts and centres when gp+0x98 is set
    for (uint32_t k = 0; k < 27u; ++k) g.W16(s::kSplitSkyColX + 2u * k, static_cast<uint16_t>(64u * k));
    if (g.U32(kGp + 0x98u) != 0) {
        const uint32_t r = g.U32(s::kSplitViewsPtr);
        int32_t w = g.S16(r + 4u);
        if (w < 0) w += 63;
        g.W16(s::kSplitSkyCols, static_cast<uint16_t>((w >> 6) + 1));
        g.W16(s::kSplitSkyCentre, static_cast<uint16_t>(g.S16(r) + (g.S16(r + 4u) >> 1)));
        g.W16(s::kSplitSkyCentre + 2u, static_cast<uint16_t>(g.S16(r + 8u) + (g.S16(r + 12u) >> 1)));
    }
    std::snprintf(line, sizeof(line),
                  "the sky tables: %s bundle %zu: %sthe type-10 section through 0x800627A8 PORTED (0x8005AD24 = 1); the "
                  "two-player sky's TIMs by 0x80060C78 transcribed: %u of DATA\\FE\\ at VRAM (%u, %u).., %u CLUTs, %u not read",
                  fileName.c_str(), bundle.index, grad.c_str(), loaded, (b82 & 0xFu) * 64u, pageY, cluts, missing);
    return line;
}

} // namespace

std::string SkyHeapSetUp(s::GuestRam& g, uint32_t record, uint32_t base, uint32_t end) {
    if (!SkyPortOn() || !Sky3On()) return "the sky tables: the heap ring not set (RRJB_SKY3=off)";
    if (record == 0 || base == 0 || end <= base) return "the sky tables: the heap ring not set (no packet heap placed)";
    g.W32(record + 0xF0u, base);
    g.W32(kGp + s::kGpHeapEnd, end);
    for (uint32_t b = 0; b < 4u; ++b) {
        g.W32(s::kHeapMarks + 4u * b, base & 0x00FFFFFFu);
        g.W32(s::kHeapStamps + 4u * b, 0);
        g.W32(s::kHeapBusy + 64u * b, 0);
    }
    g.W32(kGp + s::kGpFrameOtNext, g.U32(kGp + s::kGpFrameOtBuf));
    char line[300];
    std::snprintf(line, sizeof(line),
                  "the sky tables: the packet heap ring for SLUS 0x80021C98 (PORTED) - OURS: one buffer, the frame starting at "
                  "0x%08X every frame, the end 0x%08X (gp+0x850), the marks 0x800D75B0 = 0x%06X",
                  base, end, base & 0x00FFFFFFu);
    T().heapSetUp = line;
    return line;
}

} // namespace rr::game
