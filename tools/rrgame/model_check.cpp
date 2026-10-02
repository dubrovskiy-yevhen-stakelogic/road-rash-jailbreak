// The model draw's effect capture against the ORIGINAL:
//
//   rrgame <disc> --modelplant <state dir> <out dir>
//       a copy of the capture with effect records planted on the player's machine by the PORTED spawners
//       the game itself calls: CopJoin SLUS 0x80028034 (four police lights, state 5; the bike's class
//       +0xB4 set to 18 first, a police class - CopJoin does nothing for a racer's), CrashEmit SLUS
//       0x80027540 (which 0 / kind 0 and which 1 / kind 2, as RASHCDG 0x80074E6C calls it: state 4) and
//       ObjectEffect SLUS 0x800273EC(rider, 0, 1500, 6, 0) (a weapon trail, state 6, on the rider). The
//       original is then run over that copy (`rrverify trace --state <out dir> --frames 2`) and its
//       packets are the oracle.
//   rrgame <disc> --modelfxcheck <state dir> <prims.csv>   (and --modelfxcheck-mutate <n> ...)
//       on the same copy: the PORTED model draw 0x80068468 (with the emitter's capture head, model_
//       runtime.h) over the capture's draw list *(0x8005B280), then the PORTED effect pass - as RASHCDG
//       0x800674D4 runs them (every model of the list first, then the passes) - and every effect packet
//       compared word for word with the original's (GP0 0x2E / 0x2F on the effect sheet's pages).
//       Negative controls: 1 = the model draw not run (the planted points stay), 2 = the capture reads
//       the vertex buffers one vertex off.
#include "game/fx_runtime.h"
#include "game/model_runtime.h"
#include "game/sim/effects.h"
#include "game/sim/model_draw.h"
#include "game/sim/spine.h"
#include "game/sim/traffic_bind.h"
#include "game/sim/weapon.h"
#include "rrformats/level_bundle.h"
#include "rrformats/model_texture.h"
#include "rrformats/texture.h"
#include "rrvfs/disc_image.h"

#include <cmath>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace {

std::vector<uint8_t> LoadFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

bool Numbers(const std::string& json, const char* key, uint32_t out[32]) {
    const size_t at = json.find(key);
    if (at == std::string::npos) return false;
    size_t p = json.find('[', at);
    if (p == std::string::npos) return false;
    ++p;
    for (int k = 0; k < 32; ++k) {
        while (p < json.size() && (json[p] == ' ' || json[p] == '\n' || json[p] == '\r' || json[p] == ',')) ++p;
        char* end = nullptr;
        const unsigned long long v = std::strtoull(json.c_str() + p, &end, 10);
        if (end == json.c_str() + p) return false;
        out[k] = static_cast<uint32_t>(v);
        p = static_cast<size_t>(end - json.c_str());
    }
    return true;
}

constexpr uint32_t kGp = 0x8005AC8C, kPoolBasePtr = 0x800CE4D0;

} // namespace

int ModelPlant(const std::string& dir, const std::string& out) {
    namespace fs = std::filesystem;
    std::vector<uint8_t> ram = LoadFile(dir + "\\ram.bin");
    if (ram.size() < rr::sim::GuestRam::kRamSize) {
        std::printf("modelplant: %s has no ram.bin\n", dir.c_str());
        return 2;
    }
    std::error_code ec;
    fs::create_directories(out, ec);
    for (const auto& f : fs::directory_iterator(dir, ec))
        if (f.is_regular_file() && f.path().filename() != "ram.bin")
            fs::copy_file(f.path(), fs::path(out) / f.path().filename(), fs::copy_options::overwrite_existing, ec);
    rr::sim::GuestRam g(ram.data(), kGp);
    const uint32_t bike = g.U32(kPoolBasePtr);
    const uint32_t rider = g.U32(bike + 0x354u);
    g.W32(bike + 0xB4u, 18u);
    rr::sim::CopJoin(g, bike);
    rr::sim::CrashEmit(g, bike, 0, 0);
    rr::sim::CrashEmit(g, bike, 1, 2);
    rr::sim::SpineIo io;
    io.rootCounter = 0x1234;
    rr::sim::weapon::ObjectEffect(g, rider, 0, 1500, 6, 0, io);
    size_t live = 0;
    for (uint32_t i = 0; i < 20; ++i) {
        const uint32_t w = g.U32(rr::sim::kEffectPool + rr::sim::kEffectRecordBytes * i);
        if ((w >> 6) & 0xFu) {
            ++live;
            std::printf("  record %2u: state %u sub %u kind %u\n", i, (w >> 6) & 0xFu, (w >> 10) & 0xFu, (w >> 14) & 0xFFu);
        }
    }
    std::ofstream f(out + "\\ram.bin", std::ios::binary);
    f.write(reinterpret_cast<const char*>(ram.data()), static_cast<std::streamsize>(ram.size()));
    std::printf("modelplant: %s -> %s: bike 0x%08X class 18, CopJoin + CrashEmit x2 + ObjectEffect(rider 0x%08X, "
                "state 6); %zu effect record(s) live%s\n",
                dir.c_str(), out.c_str(), bike, rider, live, g.Faulted() ? " - FAULTED" : "");
    return g.Faulted() ? 1 : 0;
}

// `rrgame <disc> --carclutcheck <state dir>`: which VRAM palette slot a1 (CLUT (640 + 128 (a1 % 3),
// 511 - a1 / 3), 0x800251E4's bit-10 rule) holds which texture chunk's own CLUT, for the chunks of the
// race's level bundle (sections 8 and 9, in order) - the rule behind the cars' skins a1 29..47.
int CarClutCheck(const rr::DiscImage& disc, const std::string& dir) {
    const std::vector<uint8_t> ram = LoadFile(dir + "\\ram.bin");
    const std::vector<uint8_t> vram = LoadFile(dir + "\\vram.bin");
    const auto bin = disc.Find("DATA/GAMEBIN1.DAT");
    if (ram.size() < rr::sim::GuestRam::kRamSize || vram.size() < 1024u * 512u * 2u || !bin) return 2;
    std::vector<uint8_t> r = ram;
    rr::sim::GuestRam g(r.data(), kGp);
    const int raceId = static_cast<int>(g.U32(g.U32(0x8005B2F8u) + 0x40u));
    const std::vector<uint8_t> file = disc.ReadFile(*bin);
    const rr::LevelBundle bundle = rr::ParseLevelBundle(file, rr::LevelBundleIndexForRace(raceId));
    struct Chunk { int section; uint32_t id; size_t clut; };
    std::vector<Chunk> chunks;
    for (const rr::LevelBundleSection& s : bundle.sections) {
        if (s.type != 8 && s.type != 9) continue;
        size_t at = s.payload;
        const size_t end = s.limit;
        while (at + 0x30 <= end && end <= file.size()) {
            uint32_t tag, len, id;
            std::memcpy(&tag, file.data() + at, 4);
            std::memcpy(&len, file.data() + at + 4, 4);
            std::memcpy(&id, file.data() + at + 0x10, 4);
            if (len == 0 || at + len > end) break;
            uint32_t tim = 0, fl = 0;
            std::memcpy(&tim, file.data() + at + 0x18, 4);
            std::memcpy(&fl, file.data() + at + 0x1C, 4);
            if (tag == 0x5443454Cu && tim == 0x10u && (fl & 8u)) chunks.push_back({s.type, id & 0xFFFFu, at + 0x18 + 0x14});
            at += len;
        }
    }
    std::printf("carclutcheck %s: race %d, %zu LECT chunk(s) with a CLUT in sections 8 / 9\n", dir.c_str(), raceId, chunks.size());
    for (uint32_t a1 = 29; a1 <= 47; ++a1) {
        const int x = 640 + 128 * static_cast<int>(a1 % 3), y = 511 - static_cast<int>(a1 / 3);
        const size_t o = (static_cast<size_t>(y) * 1024u + static_cast<size_t>(x)) * 2u;
        bool zero = true;
        for (size_t k = 0; k < 256; ++k) zero = zero && vram[o + k] == 0;
        std::string who;
        for (size_t n = 0; n < chunks.size(); ++n)
            if (std::memcmp(vram.data() + o, file.data() + chunks[n].clut, 256) == 0)
                who += " #" + std::to_string(n) + "(sec " + std::to_string(chunks[n].section) + " id " +
                       std::to_string(chunks[n].id) + ")";
        std::printf("  a1 %2u (%3d,%3d): %s%s\n", a1, x, y, zero ? "zero" : "set", who.c_str());
    }
    for (uint32_t k = 0; k < 16; ++k) {
        const uint32_t car = 0x800CF660u + 512u * k;
        if ((g.U32(car + 0xACu) & 0xFFFFu) == 0 || (g.U32(car + 0x140u) & 0xFFFFu) == 0) continue;
        const uint32_t reg = g.U32(car + 0x60u);
        std::printf("  car slot %2u: model %u, a1 %u, +0x24 0x%08X\n", k, reg >= 0x80000000u ? g.U32(reg) : 0u,
                    (g.U32(car + 0x24u) >> 12) & 0x3Fu, g.U32(car + 0x24u));
    }
    return 0;
}

// `rrgame <disc> --weapontexcheck <state dir>` (and --weapontexcheck-mutate): the weapon sheet the renderer
// builds (model_texture.h BuildLectAtlas of model 800's DOD3+0x1C out of BBLEVEL<bank+1>.TEX) against the
// capture's VRAM, through the ORIGINAL's rule read out of the capture's own RAM: weapon object 0's model key
// record *(0x8005B2E4) + 12 (s16)obj[+0x4A] gives the tpage (+8), the UV offset (+2) and the CLUT spec
// (+4 / +6) SLUS 0x800251E4's 40-entry loop turns into palette p = VRAM (x0 + 16 (p & mask), y0 - (p >>
// shift)). Every texel of the image and the 16 entries of palettes 0..9 (the ten weapons' groups' CLUT
// indices) are compared. The control reads the palettes from the image's FIRST rows instead.
int WeaponTexCheck(const rr::DiscImage& disc, const std::string& dir, bool mutate) {
    const std::vector<uint8_t> ram = LoadFile(dir + "\\ram.bin");
    const std::vector<uint8_t> vram = LoadFile(dir + "\\vram.bin");
    const auto texFile = disc.Find("DATA/BBLEVEL1.TEX");
    if (ram.size() < rr::sim::GuestRam::kRamSize || vram.size() < 1024u * 512u * 2u || !texFile) return 2;
    std::vector<uint8_t> r = ram;
    rr::sim::GuestRam g(r.data(), kGp);
    auto px = [&](int x, int y) {
        const size_t o = (static_cast<size_t>(y & 511) * 1024u + static_cast<size_t>(x & 1023)) * 2u;
        return static_cast<uint16_t>(vram[o] | (vram[o + 1] << 8));
    };
    const uint32_t obj = 0x800CF018u;
    const uint32_t sheetId = g.U32(g.U32(obj) + 0x1Cu);
    const uint32_t rec = g.U32(0x8005B2E4u) + 12u * static_cast<uint32_t>(g.S16(obj + 0x4Au));
    const uint32_t uvOff = g.U16(rec + 2u), spec = g.U16(rec + 4u), x0 = g.U16(rec + 6u), tpage = g.U16(rec + 8u);
    const int y0 = static_cast<int>((spec >> 4) & 0x1FFu), shift = static_cast<int>(spec & 3u),
              mask = static_cast<int>((spec >> 2) & 3u);
    const int pageX = static_cast<int>(tpage & 0xFu) * 64, pageY = static_cast<int>((tpage >> 4) & 1u) * 256;
    const int imgY = pageY + static_cast<int>(uvOff >> 8), imgX = pageX + static_cast<int>(uvOff & 0xFFu) / 4;
    rr::IndexedTexture atlas;
    try {
        atlas = rr::BuildLectAtlas(disc.ReadFile(*texFile), static_cast<int>(sheetId), 16);
    } catch (const std::exception& e) {
        std::printf("weapontexcheck: %s\n", e.what());
        return 2;
    }
    size_t texels = 0, texelDiff = 0;
    for (int y = 0; y < atlas.height; ++y)
        for (int x = 0; x < atlas.width; ++x) {
            const uint16_t hw = px(imgX + x / 4, imgY + y);
            const uint8_t ours = atlas.indices[static_cast<size_t>(y) * static_cast<size_t>(atlas.width) + static_cast<size_t>(x)];
            ++texels;
            if (((hw >> ((x & 3) * 4)) & 0xFu) != ours) ++texelDiff;
        }
    // palette p of the renderer = Bgr555ToRgba of the disc halfwords; compare as halfwords re-read the same way
    const std::vector<uint8_t> tex = disc.ReadFile(*texFile);
    size_t entries = 0, entryDiff = 0;
    for (int p = 0; p < 10; ++p) {
        const int cx = static_cast<int>(x0) + 16 * (p & mask), cy = y0 - (p >> shift);
        for (int i = 0; i < 16; ++i) {
            const uint16_t cap = px(cx + i, cy);
            uint32_t ours = atlas.palettes[static_cast<size_t>(p) * 16u + static_cast<size_t>(i)];
            if (mutate) { // the control: the same entries out of the image's first rows
                const int base = (p & 3) * 64, row = p >> 2;
                uint16_t hw = 0;
                for (int n = 0; n < 4; ++n)
                    hw = static_cast<uint16_t>(hw | (atlas.indices[static_cast<size_t>(row) * static_cast<size_t>(atlas.width) +
                                                                   static_cast<size_t>(base + 4 * i + n)] << (4 * n)));
                ours = rr::Bgr555ToRgba(hw);
            }
            ++entries;
            if (ours != rr::Bgr555ToRgba(cap)) ++entryDiff;
        }
    }
    (void)tex;
    std::printf("weapontexcheck %s%s: sheet LECT %u; the capture's key record 0x%08X: tpage 0x%04X, uv +0x%04X, CLUT "
                "spec 0x%04X x %u -> image at VRAM (%d, %d), palette p at (%u + 16 (p & %d), %d - (p >> %d))\n",
                dir.c_str(), mutate ? " [MUTATED: palettes from the first rows]" : "", sheetId, rec, tpage, uvOff, spec, x0,
                imgX, imgY, x0, mask, y0, shift);
    std::printf("  texels %zu, differ %zu; palette entries (palettes 0..9) %zu, differ %zu\n", texels, texelDiff, entries,
                entryDiff);
    const bool pass = texels > 0 && texelDiff == 0 && entryDiff == 0;
    std::printf("  verdict %s\n", pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}

int ModelFxCheck(const rr::DiscImage& disc, const std::string& dir, const std::string& primsCsv, int mutate) {
    std::vector<uint8_t> ram = LoadFile(dir + "\\ram.bin");
    std::vector<uint8_t> spad = LoadFile(dir + "\\scratchpad.bin");
    const std::vector<uint8_t> cpuBytes = LoadFile(dir + "\\cpu.json");
    const std::string cpu(cpuBytes.begin(), cpuBytes.end());
    uint32_t cr[32] = {}, dr[32] = {};
    if (ram.size() < rr::sim::GuestRam::kRamSize || spad.size() < 1024 || !Numbers(cpu, "\"gte_cr32\"", cr)) {
        std::printf("modelfxcheck: %s lacks ram.bin / scratchpad.bin / cpu.json gte_cr32\n", dir.c_str());
        return 2;
    }
    Numbers(cpu, "\"gte_dr32\"", dr);
    rr::sim::GuestRam g(ram.data(), kGp);
    g.SetScratchpad(spad.data());
    const uint32_t gs = g.U32(0x8005B2F8u);
    const int players = static_cast<int>(g.U32(gs + 0x30u));
    const int raceId = static_cast<int>(g.U32(gs + 0x40u));
    std::vector<uint8_t> scratch(rr::sim::GuestRam::kRamSize, 0);
    rr::sim::GuestRam sg(scratch.data(), kGp);
    rr::game::FxVram sheet;
    std::string report;
    if (!rr::game::BuildFxSheet(sg, disc, raceId, players, sheet, report)) {
        std::printf("modelfxcheck: %s\n", report.c_str());
        return 2;
    }
    std::set<uint32_t> sheetPages;
    for (uint32_t k = 0; k < 11; ++k) sheetPages.insert(sg.U16(rr::sim::kFxSpriteTable + 28u * k + 16u) & 0x19Fu);

    rr::sim::RenderCamera(g, 0);
    rr::game::FxDepthRanges(g);
    // the model draw over the draw list (0x800674D4's first loop), the effect capture in its emitter
    std::vector<uint32_t> list;
    for (uint32_t e = g.U32(0x8005B280u); e != 0 && list.size() < 256; e = g.U32(e + 0xA8u)) list.push_back(e);
    namespace M = rr::sim::model;
    M::ModelGte gte = M::ModelGte::From(cr, dr);
    struct Head final : M::DrawCallees {
        rr::sim::GuestRam& g;
        int mutate;
        size_t captures = 0;
        Head(rr::sim::GuestRam& gg, int m) : g(gg), mutate(m) {}
        bool Emit(uint32_t obj, uint32_t) override {
            const int32_t head = g.S8(obj + 0x49u);
            if (head == -1) return true;
            const uint32_t st = (g.U32(rr::sim::kEffectPool + rr::sim::kEffectRecordBytes * static_cast<uint32_t>(head)) >> 6) & 0xFu;
            if (g.S8(obj + 8u) < 2 || st == 5u || st == 6u) {
                const uint32_t verts = g.U32(M::kVertsPtr), screen = g.U32(M::kScreenPtr);
                if (mutate == 2) { // the control: the buffers read one vertex off
                    g.W32(M::kVertsPtr, verts + 16u);
                    g.W32(M::kScreenPtr, screen + 4u);
                }
                rr::sim::EffectCaptureChain(g, obj);
                g.W32(M::kVertsPtr, verts);
                g.W32(M::kScreenPtr, screen);
                ++captures;
            }
            return !g.Faulted();
        }
    } head(g, mutate);
    size_t drawn = 0;
    if (mutate != 1)
        for (uint32_t e : list) {
            if (!M::ModelDraw(g, gte, e, 0, head)) {
                std::printf("modelfxcheck: the model draw refused 0x%08X (fault 0x%08X)\n", e, g.FaultAddress());
                return 1;
            }
            ++drawn;
        }
    // the passes (0x800674D4's second loop)
    struct Sink final : rr::sim::FxPacketSink {
        std::vector<uint32_t> at;
        std::vector<uint32_t> state;
        rr::sim::GuestRam* g = nullptr;
        rr::sim::FxEnv* env = nullptr;
        void Packet(uint32_t address, int32_t, uint32_t) override {
            at.push_back(address);
            state.push_back((g->U32(env->drawing) >> 6) & 0xFu);
        }
    } sink;
    struct NoSirens final : rr::sim::FxCallees {
        bool SirenStop(uint32_t) override { return true; }
        bool SirenStart(uint32_t) override { return true; }
    } sirens;
    rr::sim::FxEnv env;
    env.gte = rr::sim::FxGte::FromControl(cr);
    env.sink = &sink;
    env.callees = &sirens;
    sink.g = &g;
    sink.env = &env;
    for (uint32_t e : list) {
        rr::sim::EffectPass(g, env, e, 0);
        if (env.refused || g.Faulted()) {
            std::printf("modelfxcheck: the pass stopped: %s (fault 0x%08X)\n", env.why.c_str(), g.FaultAddress());
            return 1;
        }
    }
    struct Orig {
        uint32_t w[9] = {};
        size_t row = 0;
    };
    std::vector<Orig> orig;
    {
        std::ifstream f(primsCsv);
        std::string line;
        std::getline(f, line);
        size_t row = 0;
        while (std::getline(f, line)) {
            ++row;
            std::vector<std::string> col;
            std::stringstream ss(line);
            std::string c;
            while (std::getline(ss, c, ',')) col.push_back(c);
            if (col.size() < 18) continue;
            const uint32_t cmd = static_cast<uint32_t>(std::strtoul(col[2].c_str(), nullptr, 16));
            if (cmd != 0x2E && cmd != 0x2F) continue;
            const uint32_t tpage = static_cast<uint32_t>(std::strtoul(col[5].c_str(), nullptr, 16));
            if (!sheetPages.count(tpage & 0x19Fu)) continue;
            Orig o;
            o.row = row;
            for (int k = 0; k < 9; ++k) o.w[k] = static_cast<uint32_t>(std::strtoul(col[9 + static_cast<size_t>(k)].c_str(), nullptr, 16));
            orig.push_back(o);
        }
    }
    std::printf("modelfxcheck %s: race %d, %d player(s), %zu entities on the draw list, %zu model draw(s), %zu "
                "capture(s)%s\n",
                dir.c_str(), raceId, players, list.size(), drawn, head.captures,
                mutate == 1 ? " [MUTATED: the model draw not run]" : mutate == 2 ? " [MUTATED: the capture one vertex off]" : "");
    size_t byState[16] = {};
    for (uint32_t s : sink.state) ++byState[s & 15u];
    std::printf("  the original's effect packets (GP0 0x2E/0x2F on the sheet's pages): %zu; ours: %zu (by state: 1 %zu, "
                "2 %zu, 3 %zu, 4 %zu, 5 %zu, 6 %zu, 7 %zu)\n",
                orig.size(), sink.at.size(), byState[1], byState[2], byState[3], byState[4], byState[5], byState[6],
                byState[7]);
    size_t equal = 0;
    std::vector<bool> used(sink.at.size(), false);
    size_t equalByState[16] = {};
    for (const Orig& o : orig) {
        bool found = false;
        double best = 1e9;
        size_t nearest = sink.at.size();
        for (size_t i = 0; i < sink.at.size(); ++i) {
            if (used[i]) continue;
            bool same = true;
            static constexpr uint32_t kMask[9] = {~0u, ~0u, ~0u, ~0u, ~0u, ~0u, 0xFFFFu, ~0u, 0xFFFFu};
            for (uint32_t k = 0; k < 9 && same; ++k) same = ((g.U32(sink.at[i] + 4u + 4u * k) ^ o.w[k]) & kMask[k]) == 0;
            if (same) {
                used[i] = true;
                found = true;
                ++equalByState[sink.state[i] & 15u];
                break;
            }
            double d = 0;
            for (uint32_t v = 0; v < 4; ++v) {
                const uint32_t a = g.U32(sink.at[i] + 8u + 8u * v), b = o.w[1 + 2 * v];
                d = std::max(d, std::abs(static_cast<double>(static_cast<int16_t>(a & 0xFFFF)) - static_cast<int16_t>(b & 0xFFFF)));
                d = std::max(d, std::abs(static_cast<double>(static_cast<int16_t>(a >> 16)) - static_cast<int16_t>(b >> 16)));
            }
            if (d < best) {
                best = d;
                nearest = i;
            }
        }
        if (found) ++equal;
        std::printf("    row %5zu: %08X %08X %08X ... %s\n", o.row, o.w[0], o.w[1], o.w[2],
                    found ? "EQUAL word for word" : "no equal packet of ours");
        if (!found && nearest < sink.at.size()) {
            std::printf("              ours nearest (state %u, %.0f px):", sink.state[nearest], best);
            for (uint32_t k = 0; k < 9; ++k) std::printf(" %08X", g.U32(sink.at[nearest] + 4u + 4u * k));
            std::printf("\n              original    :");
            for (uint32_t k = 0; k < 9; ++k) std::printf(" %08X", o.w[k]);
            std::printf("\n");
        }
    }
    std::printf("  packets equal word for word: %zu of %zu (state 4: %zu, 5: %zu, 6: %zu)\n", equal, orig.size(),
                equalByState[4], equalByState[5], equalByState[6]);
    const bool pass = !orig.empty() && equal == orig.size() && sink.at.size() == orig.size();
    std::printf("  verdict %s\n", pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}
