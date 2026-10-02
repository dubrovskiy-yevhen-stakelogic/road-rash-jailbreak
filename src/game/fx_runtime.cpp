#include "game/fx_runtime.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>

#include "game/frame_ot.h" // the OT apart from the heap
#include "game/sim/split_view.h"
#include "game/loader_product.h" // EffectSheet PORTED
#include "rrformats/level_bundle.h"

namespace rr::game {
namespace {

using rr::sim::GuestRam;

constexpr uint32_t kGp = 0x8005AC8C;
constexpr uint32_t kExeText = 0x80010000u;       // SLUS_010.53: file offset 0x800 is 0x80010000
constexpr uint32_t kRashcdiBase = 0x8005B5E8u;   // RASHCDI.BIN's load address
constexpr uint32_t kSpriteOffsets = 0x8006B4C0u; // RASHCDI: {u8 x/4, u8 y, u16 CLUT count} per sprite
constexpr uint32_t kTexConfig = 0x800533B4u;     // SLUS: 88 bytes per player count
constexpr uint32_t kClutConfig = 0x80053254u;    // SLUS: 176 bytes per player count
constexpr uint32_t kDepthNear = 0x8005B4D4u, kDepthShift = 0x8005B4D8u;
constexpr int kSprites = 11;

std::vector<uint8_t> Read(const DiscImage& disc, const char* path) {
    const auto f = disc.Find(path);
    return f ? disc.ReadFile(*f) : std::vector<uint8_t>{};
}
uint32_t U32(const std::vector<uint8_t>& v, size_t at) {
    if (at + 4 > v.size()) return 0;
    return static_cast<uint32_t>(v[at]) | (static_cast<uint32_t>(v[at + 1]) << 8) |
           (static_cast<uint32_t>(v[at + 2]) << 16) | (static_cast<uint32_t>(v[at + 3]) << 24);
}
uint16_t U16(const std::vector<uint8_t>& v, size_t at) {
    if (at + 2 > v.size()) return 0;
    return static_cast<uint16_t>(v[at] | (v[at + 1] << 8));
}
uint8_t U8(const std::vector<uint8_t>& v, size_t at) { return at < v.size() ? v[at] : 0; }

void LoadImage(FxVram& vram, int x, int y, int w, int h, const std::vector<uint8_t>& src, size_t at) {
    vram.uploads.push_back({x, y, w, h});
    for (int r = 0; r < h; ++r)
        for (int c = 0; c < w; ++c)
            vram.px[static_cast<size_t>((y + r) & 511) * FxVram::kWidth + static_cast<size_t>((x + c) & 1023)] =
                U16(src, at + (static_cast<size_t>(r) * static_cast<size_t>(w) + static_cast<size_t>(c)) * 2u);
}

} // namespace

// ============================================================================ RASHCDI 0x80061FAC
bool BuildFxSheet(GuestRam& g, const DiscImage& disc, int raceId, int players, FxVram& vram, std::string& report,
                  bool mutate) {
    const std::vector<uint8_t> exe = Read(disc, "SLUS_010.53");
    const std::vector<uint8_t> cdi = Read(disc, "RASHCDI.BIN");
    const std::vector<uint8_t> bin = Read(disc, "DATA/GAMEBIN1.DAT");
    if (exe.empty() || cdi.empty() || bin.empty()) {
        report = "the effect sheet was NOT built: SLUS_010.53, RASHCDI.BIN or DATA\\GAMEBIN1.DAT is missing";
        return false;
    }
    if (players < 1) players = 1;
    const auto exeAt = [](uint32_t a) { return static_cast<size_t>(a - kExeText) + 0x800u; };
    const size_t cfg = exeAt(kTexConfig + 88u * static_cast<uint32_t>(players - 1));
    const size_t cl = exeAt(kClutConfig + 176u * static_cast<uint32_t>(players - 1));
    const uint32_t cfg53 = U8(exe, cfg + 53), cfg54 = U8(exe, cfg + 54);
    const int groupX = static_cast<int>(cfg54 & 0xFu) * 64;
    const int groupY = static_cast<int>(cfg53 + ((cfg54 & 0x10u) << 4));
    size_t payload = 0;
    try {
        const rr::LevelBundle bundle = rr::ParseLevelBundle(bin, rr::LevelBundleIndexForRace(raceId));
        const rr::LevelBundleSection* s = bundle.Find(5);
        if (s == nullptr) {
            report = "the effect sheet was NOT built: the race's level bundle has no type-5 section";
            return false;
        }
        payload = s->payload;
    } catch (const std::exception& e) {
        report = std::string("the effect sheet was NOT built: ") + e.what();
        return false;
    }
    if (LoaderPorted() && !mutate) { // EffectSheet RASHCDI 0x80061FAC PORTED on the section in guest RAM
        // the section's extent: the 11 TIMs and the CLUT rows the table names
        size_t extent = 0x58u + 4u * 64u;
        uint32_t cluts = 0;
        for (int i = 0; i < kSprites; ++i) {
            const size_t tim = U32(bin, payload + 4u * static_cast<size_t>(i));
            size_t p = tim + 8u;
            if (U32(bin, payload + tim + 4u) & 8u) p += U32(bin, payload + p);
            extent = std::max(extent, p + U32(bin, payload + p));
            cluts += U16(cdi, static_cast<size_t>(kSpriteOffsets - kRashcdiBase) + 4u * static_cast<size_t>(i) + 2u);
        }
        for (uint32_t k = 0; k < cluts; ++k) extent = std::max<size_t>(extent, U32(bin, payload + 0x58u + 4u * k) + 32u);
        // transient: the frame's packet heap, empty while the race loads (the original frees the bundle buffer)
        const uint32_t heap = g.U32(rr::sim::kFxPacketHeapPtr);
        const uint32_t at = heap != 0 ? (g.U32(heap + 0x10Cu) + 3u) & ~3u : 0u, end = g.U32(rr::sim::kFxPacketEnd);
        if (at != 0 && end > at && end - at >= extent && payload + extent <= bin.size()) {
            const std::vector<uint8_t> section(bin.begin() + static_cast<std::ptrdiff_t>(payload),
                                               bin.begin() + static_cast<std::ptrdiff_t>(payload + extent));
            uint32_t clutsDone = 0;
            std::string error;
            const bool ok = LoaderEffectSheet(
                g, nullptr, disc, section, at, kLoaderSp,
                [&](int x, int y, int w, int h, uint32_t pixels) {
                    if (w == 16 && h == 1) ++clutsDone;
                    vram.uploads.push_back({x, y, w, h});
                    for (int r = 0; r < h; ++r)
                        for (int c = 0; c < w; ++c)
                            vram.px[static_cast<size_t>((y + r) & 511) * FxVram::kWidth + static_cast<size_t>((x + c) & 1023)] =
                                g.U16(pixels + 2u * static_cast<uint32_t>(r * w + c));
                },
                error);
            std::vector<uint8_t> zero(section.size(), 0); // the transient copy gone, as the original frees it
            g.WriteBlock(at, zero.data(), static_cast<uint32_t>(zero.size()));
            if (ok) {
                char b2[300];
                std::snprintf(b2, sizeof(b2),
                              "the effect sheet (RASHCDI 0x80061FAC PORTED, the race loader): %d sprites, %u CLUTs, "
                              "descriptors at 0x%08X, the section (%zu bytes) transient at 0x%08X, pixels host-side "
                              "(LoadImage is the renderer's)",
                              kSprites, clutsDone, rr::sim::kFxSpriteTable, section.size(), at);
                report = b2;
                return !g.Faulted();
            }
            report = "the effect sheet: " + error + " - the transcription below stands";
        }
    }
    uint32_t clutCount = 0; // s4, running over every sprite
    for (int i = 0; i < kSprites; ++i) {
        const size_t tim = payload + U32(bin, payload + 4u * static_cast<size_t>(i));
        if (U32(bin, tim) != 0x10u) {
            report = "the effect sheet was NOT built: sprite " + std::to_string(i) + " is not a TIM";
            return false;
        }
        size_t p = tim + 8u;
        if (U32(bin, tim + 4u) & 8u) p += U32(bin, p); // the TIM's own CLUT block: OpenTIM skips it here
        const int w = static_cast<int16_t>(U16(bin, p + 8u)), h = static_cast<int16_t>(U16(bin, p + 10u));
        const size_t tbl = static_cast<size_t>(kSpriteOffsets - kRashcdiBase) + 4u * static_cast<size_t>(i);
        const uint32_t b0 = mutate ? 0u : U8(cdi, tbl), b1 = mutate ? 0u : U8(cdi, tbl + 1u);
        const uint16_t cluts = U16(cdi, tbl + 2u);
        const int rx = static_cast<int>(b0) + groupX, ry = groupY + static_cast<int>(b1);
        const uint32_t d = rr::sim::kFxSpriteTable + rr::sim::kFxSpriteBytes * static_cast<uint32_t>(i);
        g.W32(d + 0u, b0 << 2);
        g.W32(d + 4u, static_cast<uint32_t>(ry));
        g.W32(d + 8u, static_cast<uint32_t>(w << 2));
        g.W32(d + 12u, static_cast<uint32_t>(h));
        LoadImage(vram, rx, ry, w, h, bin, p + 12u);
        const uint32_t ux = static_cast<uint32_t>(rx), uy = static_cast<uint32_t>(ry);
        uint32_t tpage = ((uy & 0x100u) >> 4) | ((ux & 0x3FFu) >> 6);
        if (i == 0) tpage |= 0x40u;
        tpage |= (uy & 0x200u) << 2;
        g.W16(d + 16u, static_cast<uint16_t>(tpage));
        g.W16(d + 18u, cluts);
        for (uint32_t k = 0; k < cluts; ++k, ++clutCount) {
            const size_t src = payload + U32(bin, payload + 0x58u + 4u * clutCount);
            const uint32_t per = U8(exe, cl + 0x6Cu);
            if (per == 0) {
                report = "the effect sheet was NOT built: the CLUT row width is 0";
                return false;
            }
            const int cx = static_cast<int>(U16(exe, cl + 0x68u) + (clutCount % per) * U8(exe, cl + 0x6Eu)) + groupX;
            const int cy = static_cast<int>(U16(exe, cl + 0x6Au) + clutCount / per) + (static_cast<int>(cfg54 & 0x10u) << 4);
            LoadImage(vram, cx, cy, 16, 1, bin, src);
            g.W16(d + 20u + 2u * k, static_cast<uint16_t>((static_cast<uint32_t>(cy) << 6) | ((static_cast<uint32_t>(cx) >> 4) & 0x3Fu)));
        }
    }
    // 0x800622BC..0x80062350: one 32 x 1 row of zeroes at (group x + 12, group y + 120).
    const std::vector<uint8_t> zeros(64, 0);
    LoadImage(vram, groupX + 12, groupY + 120, 32, 1, zeros, 0);
    char b[240];
    std::snprintf(b, sizeof(b),
                  "the effect sheet (RASHCDI 0x80061FAC transcribed): %d sprites, %u CLUTs, descriptors at 0x%08X, "
                  "pixels at VRAM (%d, %d) and up (host-side, no VRAM)",
                  kSprites, clutCount, rr::sim::kFxSpriteTable, groupX, groupY);
    report = b;
    return !g.Faulted();
}

int CheckFxSheet(const DiscImage& disc, const std::string& dir, bool mutate) {
    auto load = [](const std::string& path) {
        std::ifstream f(path, std::ios::binary);
        return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    };
    const std::vector<uint8_t> ram = load(dir + "\\ram.bin");
    const std::vector<uint8_t> vram = load(dir + "\\vram.bin");
    if (ram.size() < GuestRam::kRamSize || vram.size() < 1024u * 512u * 2u) {
        std::printf("fxsheetcheck: %s has no ram.bin / vram.bin\n", dir.c_str());
        return 2;
    }
    const uint32_t gs = U32(ram, 0x5B2F8u) & 0x1FFFFFu; // *(0x8005B2F8): game_state
    const int players = static_cast<int>(U32(ram, gs + 0x30u));
    const int raceId = static_cast<int>(U32(ram, gs + 0x40u));
    std::vector<uint8_t> blank(GuestRam::kRamSize, 0);
    GuestRam g(blank.data(), kGp);
    FxVram fv;
    std::string report;
    if (!BuildFxSheet(g, disc, raceId, players, fv, report, mutate)) {
        std::printf("fxsheetcheck: %s\n", report.c_str());
        return 2;
    }
    size_t descBytes = 0, descDiff = 0;
    for (uint32_t a = rr::sim::kFxSpriteTable; a < rr::sim::kFxSpriteTable + kSprites * rr::sim::kFxSpriteBytes; ++a) {
        ++descBytes;
        if (blank[a & 0x1FFFFFu] != ram[a & 0x1FFFFFu]) ++descDiff;
    }
    size_t px = 0, pxDiff = 0, rects = 0, rectsDiff = 0;
    for (const FxVram::Rect& r : fv.uploads) {
        size_t d = 0;
        for (int y = 0; y < r.h; ++y)
            for (int x = 0; x < r.w; ++x) {
                const size_t o = static_cast<size_t>((r.y + y) & 511) * 1024u + static_cast<size_t>((r.x + x) & 1023);
                const uint16_t cap = static_cast<uint16_t>(vram[o * 2] | (vram[o * 2 + 1] << 8));
                ++px;
                if (cap != fv.px[o]) ++d;
            }
        ++rects;
        pxDiff += d;
        if (d) ++rectsDiff;
    }
    std::printf("fxsheetcheck %s: race %d, %d player(s)%s\n  %s\n", dir.c_str(), raceId, players,
                mutate ? " [MUTATED: the per-sprite offsets dropped]" : "", report.c_str());
    std::printf("  descriptor bytes 0x800D4270..: %zu, differ %zu\n", descBytes, descDiff);
    std::printf("  VRAM rectangles uploaded: %zu (%zu halfwords), differ %zu (%zu halfwords)\n", rects, px, rectsDiff,
                pxDiff);
    const bool pass = descDiff == 0 && pxDiff == 0 && rects > 0;
    std::printf("  verdict %s\n", pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}

namespace {
FxPacket PacketFrom(const uint8_t raw[40], uint32_t address) {
    FxPacket p;
    auto w = [&](size_t k) {
        return static_cast<uint32_t>(raw[4 * k]) | (static_cast<uint32_t>(raw[4 * k + 1]) << 8) |
               (static_cast<uint32_t>(raw[4 * k + 2]) << 16) | (static_cast<uint32_t>(raw[4 * k + 3]) << 24);
    };
    p.address = address;
    p.word1 = w(1);
    for (size_t v = 0; v < 4; ++v) {
        const uint32_t xy = w(2 + 2 * v), uv = w(3 + 2 * v);
        p.x[v] = static_cast<int16_t>(xy & 0xFFFFu);
        p.y[v] = static_cast<int16_t>(xy >> 16);
        p.u[v] = static_cast<uint8_t>(uv);
        p.v[v] = static_cast<uint8_t>(uv >> 8);
        if (v == 0) p.clut = static_cast<uint16_t>(uv >> 16);
        if (v == 1) p.tpage = static_cast<uint16_t>(uv >> 16);
    }
    return p;
}
} // namespace

FxPacket ReadFxPacket(const uint8_t* ram, uint32_t address) {
    uint8_t raw[40];
    for (size_t k = 0; k < 40; ++k) raw[k] = ram[(address + k) & 0x1FFFFFu];
    return PacketFrom(raw, address);
}

std::vector<uint32_t> FxEntities(const uint8_t* ram, const std::vector<uint32_t>& bikes) {
    auto word = [&](uint32_t a) {
        const size_t o = a & 0x1FFFFCu;
        return static_cast<uint32_t>(ram[o]) | (static_cast<uint32_t>(ram[o + 1]) << 8) |
               (static_cast<uint32_t>(ram[o + 2]) << 16) | (static_cast<uint32_t>(ram[o + 3]) << 24);
    };
    std::vector<uint32_t> out;
    for (uint32_t e : bikes) {
        out.push_back(e);
        const uint32_t r = word(e + 0x354u);
        if (r >= 0x80000000u && r < 0x80200000u && word(r + 0x34u) == 0 && word(e + 0x38u) != r &&
            word(e + 0x40u) != r)
            out.push_back(r);
    }
    constexpr uint32_t kPool3Control = 0x800CF650u, kPool3Slots = 0x800CF660u, kCarBytes = 512u;
    const int32_t high = static_cast<int32_t>(word(kPool3Control + 8u));
    for (int32_t k = 0; k <= high && k < 16; ++k) {
        const uint32_t car = kPool3Slots + kCarBytes * static_cast<uint32_t>(k);
        if ((word(car + 0xACu) & 0xFFFFu) != 0 && (word(car + 0x140u) & 0xFFFFu) != 0) out.push_back(car);
    }
    return out;
}

// ============================================================================ the runtime
struct FxRuntime::Sirens final : rr::sim::FxCallees {
    FxRuntime* rt;
    explicit Sirens(FxRuntime* r) : rt(r) {}
    // SLUS 0x8001836C / 0x800182B0: the siren voice of a player's police bike (sound, not ported).
    // NOT RUN, counted: their only effect is the voice.
    bool SirenStop(uint32_t) override {
        ++rt->sirens;
        return true;
    }
    bool SirenStart(uint32_t) override {
        ++rt->sirens;
        return true;
    }
};

void FxDepthRanges(GuestRam& g) {
    // RASHCDG 0x800674D4's scratchpad stores (0x800674E4..0x80067570), in its order.
    g.W16(0x1F80000Au, 0x800);
    g.W16(0x1F80000Cu, 0x1000);
    g.W16(0x1F800012u, 0x200);
    g.W16(0x1F800014u, 0x300);
    g.W8(0x1F800018u, 0xFD);
    g.W8(0x1F800019u, 0xFE);
    g.W16(0x1F800008u, 0);
    g.W16(0x1F800010u, 0);
    g.W8(0x1F80001Au, 0);
    g.W32(0x1F800004u, g.U32(kDepthNear));
    g.W32(0x1F80001Cu, g.U32(rr::sim::kFxOtLength) - 2u);
    g.W32(0x1F800000u, g.U32(kDepthShift));
    g.W32(0x1F800208u, g.U32(rr::sim::kFxModelScreen));
}

std::string FxRuntime::Setup(GuestRam& g, const DiscImage& disc, int raceId, int players) {
    std::string report;
    ready_ = false;
    if (!BuildFxSheet(g, disc, raceId, players, vram_, report)) return report;
    const uint32_t heap = g.U32(rr::sim::kFxPacketHeapPtr);
    const uint32_t end = g.U32(rr::sim::kFxPacketEnd);
    otLength_ = g.U32(rr::sim::kFxOtLength);
    if (heap == 0 || end == 0 || otLength_ == 0 || otLength_ > 0x1000u) {
        return report + "; the effect pass does NOT run: the packet heap (the HUD arena's) or the OT length is missing";
    }
    // frame_ot.h: the ordering table SLUS 0x8001C1AC placed apart from the heap (+0x108); the
    // carve from the heap's top below is the control (RRJB_FXOT=top) and the fallback when none was placed.
    const uint32_t own = g.U32(heap + 0x108u);
    const bool placed = FrameOtsPorted() && own != 0 && 4u * otLength_ <= kFrameOtBytes &&
                        (own + kFrameOtBytes <= g.U32(heap + 0x10Cu) || own >= end);
    if (placed) {
        ot_ = own;
    } else {
        ot_ = (end - 4u * otLength_) & ~3u;
        if (ot_ < g.U32(heap + 0x10Cu) + 0x800u) {
            return report + "; the effect pass does NOT run: the packet heap is too small for the ordering table";
        }
        g.W32(rr::sim::kFxPacketEnd, ot_);
        g.W32(heap + 0x108u, ot_);
    }
    g.W32(kDepthNear, 0);
    g.W32(kDepthShift, 5);
    // View 1's render camera, *(0x8005AEC4) (gp+0x238): 0x800D8330 in every capture (rr-race, rr-pack,
    // rr-grid, quick), the 0x80 bytes after view 0's 0x800D82B0 and below the shot table 0x800D83B0. Its
    // writer is not found (no gp+0x238 store in our listings); OURS, named: the capture's value when unset.
    if (g.U32(rr::sim::kFxRenderCamPtrs + 4u) == 0) g.W32(rr::sim::kFxRenderCamPtrs + 4u, 0x800D8330u);
    ready_ = !g.Faulted();
    char b[1100];
    std::snprintf(b, sizeof(b),
                  "; the PORTED effect pass (effects.h, 18 rows) runs each frame after the session's frame (OURS: the "
                  "original runs it in the render, before AudioFrame's RoadNote spawns) on FxEntities (OURS: live "
                  "bikes, riders off their bikes, live cars, in pool order, for the per-cell draw list of 0x80067AC4): "
                  "RenderCamera SLUS 0x8002F17C, the depth ranges of RASHCDG 0x800674D4 (transcribed), OURS: an "
                  "ordering table of %u entries at 0x%08X %s, *(0x8005B4D4) = 0 / *(0x8005B4D8) = 5 as every capture; "
                  "records in states 3-6 (streak, "
                  "nitro flame, police light, weapon trail) %s",
                  otLength_, ot_,
                  placed ? "- the frame record's +0x108, apart from the packet heap as SLUS 0x8001C1AC places it "
                           "(frame_ot.h)"
                         : "carved from the top of the HUD's packet heap (its end moved there)",
                  modelPass ? "take their points from the PORTED model draw's capture 0x80029CA4 and are drawn "
                              "(model_runtime.h; the emitter's glow sprites 0x80027B80 / 0x80028534 PORTED, fx_glow.h)"
                            : "are aged but NOT drawn: no model draw runs, so no capture 0x80029CA4 gives them a point");
    return report + b;
}

bool FxRuntime::RecarveOt(uint8_t* arena) {
    if (!ready_ || arena == nullptr) return false;
    GuestRam g(arena, kGp);
    const uint32_t heap = g.U32(rr::sim::kFxPacketHeapPtr);
    if (heap < 0x80000000u || heap >= 0x80200000u) return false;
    const uint32_t ot = g.U32(heap + 0x108u); // the capture's own ordering table (rr-race 0x800FB974)
    if (ot < 0x80000000u || ot + 4u * otLength_ > 0x80200000u) return false;
    ot_ = ot;
    return !g.Faulted();
}

bool FxRuntime::Frame(uint8_t* arena, const std::vector<uint32_t>& entities, int views) {
    packets_.clear();
    if (!ready_ || arena == nullptr) return false;
    ++frames;
    GuestRam g(arena, kGp);
    g.SetScratchpad(spad_.data());
    struct Sink final : rr::sim::FxPacketSink {
        FxRuntime* rt;
        GuestRam* g;
        rr::sim::FxEnv* env;
        uint32_t view = 0;
        uint32_t entity = 0; // the entity whose pass links it
        void Packet(uint32_t address, int32_t depth, uint32_t function) override {
            FxPacket p;
            p.address = address;
            p.function = function;
            p.depth = depth;
            p.record = env->drawing;
            p.state = env->drawing != 0 ? (g->U32(env->drawing) >> 6) & 0xFu : 0u; // 0: a glow sprite, no record
            p.view = view;
            p.entity = entity;
            rt->packets_.push_back(p);
        }
    };
    Sirens voices(this);
    rr::sim::FxEnv env;
    env.callees = &voices;
    Sink sink;
    sink.rt = this;
    sink.g = &g;
    sink.env = &env;
    env.sink = &sink;
    // ModelLod 0x800667C4's last store, `+9 &= 0xF7`, once per frame on every entity and its two children
    // (the draw cycle 0x8008CFDC runs it before the view loop of SLUS 0x80011C4C; the pose side does the
    // riders too): the first view that walks an entity ages its records, a later view only draws them.
    const bool keep = keepAges_;
    keepAges_ = false;
    for (uint32_t e : entities) {
        if (keep) break; // --parity: the capture's own draw cycle already ran this frame (KeepCaptureAgesOnce)
        g.W8(e + 9u, static_cast<uint8_t>(g.U8(e + 9u) & 0xF7u));
        for (uint32_t k = 0; k < 2; ++k) {
            const uint32_t c = g.U32(e + 0x38u + 8u * k);
            if (c != 0) g.W8(c + 9u, static_cast<uint8_t>(g.U8(c + 9u) & 0xF7u));
        }
    }
    const uint32_t nViews = views == 2 ? 2u : 1u;
    std::vector<bool> modelledView(nViews, false);
    for (uint32_t view = 0; view < nViews && !env.refused && !g.Faulted(); ++view) {
        // The GTE offset of the view (SLUS 0x80011C4C: SetGeomOffset 0x8004D184 at the split rectangle's
        // centre plus the nudge for two players; one player: 192 / 120, every capture's).
        int32_t ofx = 192, ofy = 120;
        if (nViews == 2) {
            rr::sim::SplitRect r;
            if (rr::sim::SplitViewRect(g, view, r)) ofx = r.cx, ofy = r.cy;
        }
        ofx_[view] = ofx, ofy_[view] = ofy;
        rr::sim::RenderCamera(g, view);
        FxDepthRanges(g);
        // ClearOTagR: every entry links to the one below it, entry 0 ends the list (libgpu, OURS).
        for (uint32_t i = 0; i < otLength_; ++i)
            g.W32(ot_ + 4u * i, i == 0 ? 0x00FFFFFFu : ((ot_ + 4u * (i - 1u)) & 0x00FFFFFFu));
        // The GTE's projection constants: every race capture's COP2 control registers (OFX 192, OFY 120,
        // H 237, DQA 0xEF9E, DQB 0x01400000, ZSF4 256), the offset per view; RT and TR are loaded by the
        // pass before any RTPS.
        uint32_t cr[32] = {};
        cr[24] = static_cast<uint32_t>(ofx) << 16;
        cr[25] = static_cast<uint32_t>(ofy) << 16;
        cr[26] = 237;
        cr[27] = 0xFFFFEF9Eu;
        cr[28] = 0x01400000u;
        cr[29] = 341;
        cr[30] = 256;
        env.gte = rr::sim::FxGte::FromControl(cr);
        sink.view = view;
        std::vector<uint32_t> drawn;
        env.drawing = 0;
        sink.entity = 0;
        active_ = &env;
        const bool modelled = modelPass && modelPass(g, entities, drawn, view, ofx, ofy); // model_runtime.h
        active_ = nullptr;
        modelledView[view] = modelled;
        for (uint32_t e : modelled ? drawn : entities) {
            sink.entity = e;
            rr::sim::EffectPass(g, env, e, view);
            if (env.refused || g.Faulted()) break;
        }
    }
    size_t live = 0;
    for (uint32_t i = 0; i < 20; ++i)
        if (((g.U32(rr::sim::kEffectPool + rr::sim::kEffectRecordBytes * i) >> 6) & 0xFu) != 0) ++live;
    maxLive = std::max(maxLive, live);
    if (env.refused || g.Faulted()) {
        ++refusals;
        char b[200];
        std::snprintf(b, sizeof(b), "the effect pass stopped: %s%s0x%08X", env.refused ? env.why.c_str() : "",
                      g.Faulted() ? " a fault at " : " ", g.FaultAddress());
        error_ = b;
        packets_.clear();
        return false;
    }
    for (FxPacket& p : packets_) {
        const uint32_t record = p.record, state = p.state, function = p.function;
        const int32_t depth = p.depth;
        uint8_t raw[40];
        g.ReadBlock(p.address, raw, 40);
        FxPacket q = PacketFrom(raw, p.address);
        q.record = record;
        q.state = state;
        q.function = function;
        q.depth = depth;
        q.view = p.view;
        q.entity = p.entity;
        q.drawable = modelledView[p.view < nViews ? p.view : 0] || !(state >= 3u && state <= 6u); // the model draw captured their points
        p = q;
        ++packetsTotal;
        ++byState[state & 15u];
        if (p.drawable) ++packetsDrawn;
    }
    return true;
}

} // namespace rr::game
