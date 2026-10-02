// `rrgame --fxcheck <state dir> <prims.csv>` (and `--fxcheck-mutate <n> ...`): the effects as the product
// draws them against the ORIGINAL's packets of the same frame.
//
// The capture's RAM, scratchpad and COP2 constants are taken as they are (the snapshot is taken inside
// the frame's cell draw, before the model draw and the effect pass of RASHCDG 0x800674D4 run); the
// PORTED RenderCamera 0x8002F17C, the depth ranges of 0x800674D4 and the PORTED EffectPass 0x8002823C
// run over the capture's own draw list (*(0x8005B280), linked through +0xA8), and every packet the pass
// links is compared, word for word, with the original's effect packets out of `psxgpu.py --prims`
// (GP0 0x2E / 0x2F on a texture page of the effect sheet). Then every texel those packets sample is
// read twice through the RENDERER's rule (render/fx_draw.h FxDraw::Texel): out of the sheet the
// product builds from the disc, and out of the capture's VRAM.
//
// Negative controls: mutate 1 builds the sheet without the per-sprite offsets (texels must differ),
// mutate 2 leaves the render camera's second row unscaled (placements must differ).
#include "game/fx_runtime.h"
#include "game/sim/effects.h"
#include "render/fx_draw.h"
#include "rrvfs/disc_image.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
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

// cpu.json's "gte_cr32": [ 32 numbers ].
bool GteControl(const std::string& json, uint32_t cr[32]) {
    const size_t at = json.find("\"gte_cr32\"");
    if (at == std::string::npos) return false;
    size_t p = json.find('[', at);
    if (p == std::string::npos) return false;
    ++p;
    for (int k = 0; k < 32; ++k) {
        while (p < json.size() && (json[p] == ' ' || json[p] == '\n' || json[p] == '\r' || json[p] == ',')) ++p;
        char* end = nullptr;
        const unsigned long long v = std::strtoull(json.c_str() + p, &end, 10);
        if (end == json.c_str() + p) return false;
        cr[k] = static_cast<uint32_t>(v);
        p = static_cast<size_t>(end - json.c_str());
    }
    return true;
}

} // namespace

int FxCheck(const rr::DiscImage& disc, const std::string& dir, const std::string& primsCsv, int mutate) {
    std::vector<uint8_t> ram = LoadFile(dir + "\\ram.bin");
    std::vector<uint8_t> spad = LoadFile(dir + "\\scratchpad.bin");
    const std::vector<uint8_t> vramBytes = LoadFile(dir + "\\vram.bin");
    const std::vector<uint8_t> cpuBytes = LoadFile(dir + "\\cpu.json");
    const std::string cpu(cpuBytes.begin(), cpuBytes.end());
    uint32_t cr[32] = {};
    if (ram.size() < rr::sim::GuestRam::kRamSize || spad.size() < 1024 || vramBytes.size() < 1024u * 512u * 2u ||
        !GteControl(cpu, cr)) {
        std::printf("fxcheck: %s lacks ram.bin / scratchpad.bin / vram.bin / cpu.json gte_cr32\n", dir.c_str());
        return 2;
    }
    rr::sim::GuestRam g(ram.data(), 0x8005AC8C);
    g.SetScratchpad(spad.data());
    const uint32_t gs = g.U32(0x8005B2F8u);
    const int players = static_cast<int>(g.U32(gs + 0x30u));
    const int raceId = static_cast<int>(g.U32(gs + 0x40u));

    // The sheet the product builds from the disc (into a scratch image: the capture keeps its own).
    std::vector<uint8_t> scratch(rr::sim::GuestRam::kRamSize, 0);
    rr::sim::GuestRam sg(scratch.data(), 0x8005AC8C);
    rr::game::FxVram ours;
    std::string report;
    if (!rr::game::BuildFxSheet(sg, disc, raceId, players, ours, report, mutate == 1)) {
        std::printf("fxcheck: %s\n", report.c_str());
        return 2;
    }
    rr::game::FxVram cap;
    for (size_t i = 0; i < cap.px.size(); ++i)
        cap.px[i] = static_cast<uint16_t>(vramBytes[2 * i] | (vramBytes[2 * i + 1] << 8));
    std::set<uint32_t> sheetPages;
    for (uint32_t k = 0; k < 11; ++k) sheetPages.insert(sg.U16(rr::sim::kFxSpriteTable + 28u * k + 16u) & 0x19Fu);

    // The frame's render camera, the depth ranges, the pass over the draw list.
    rr::sim::RenderCamera(g, 0);
    if (mutate == 2) { // the negative control: the second row as the view record holds it, unscaled
        const uint32_t rc = g.U32(0x8005AEC0u);
        for (uint32_t k = 0; k < 3; ++k) g.W16(rc + 0x62u + 2u * k, g.U16(rr::sim::kFxViewRecords + 0x1B6u + 2u * k));
    }
    rr::game::FxDepthRanges(g);
    struct Sink final : rr::sim::FxPacketSink {
        std::vector<uint32_t> at;
        void Packet(uint32_t address, int32_t, uint32_t) override { at.push_back(address); }
    } sink;
    struct NoSirens final : rr::sim::FxCallees {
        bool SirenStop(uint32_t) override { return true; }
        bool SirenStart(uint32_t) override { return true; }
    } sirens;
    rr::sim::FxEnv env;
    env.gte = rr::sim::FxGte::FromControl(cr);
    env.sink = &sink;
    env.callees = &sirens;
    size_t entities = 0;
    for (uint32_t e = g.U32(0x8005B280u); e != 0 && entities < 256; e = g.U32(e + 0xA8u), ++entities) {
        rr::sim::EffectPass(g, env, e, 0);
        if (env.refused || g.Faulted()) {
            std::printf("fxcheck: the pass stopped: %s (fault 0x%08X)\n", env.why.c_str(), g.FaultAddress());
            return 1;
        }
    }

    // The original's effect packets.
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
    std::printf("fxcheck %s: race %d, %d player(s), %zu entities on the draw list%s\n", dir.c_str(), raceId, players,
                entities, mutate == 1 ? " [MUTATED: sheet offsets dropped]" : mutate == 2 ? " [MUTATED: camera row unscaled]" : "");
    std::printf("  the original's effect packets (GP0 0x2E/0x2F on the sheet's pages): %zu; ours: %zu\n", orig.size(),
                sink.at.size());
    size_t equal = 0, texels = 0, texelsDiff = 0, near4 = 0;
    std::vector<bool> used(sink.at.size(), false);
    for (const Orig& o : orig) {
        bool found = false;
        double best = 1e9;
        size_t nearest = sink.at.size();
        for (size_t i = 0; i < sink.at.size(); ++i) {
            if (used[i]) continue;
            bool same = true;
            // Words 6 and 8 carry the third and fourth UV in their low halves; their high halves are
            // padding the GPU ignores, left as whatever the heap held (the original's packet sits at
            // another heap address than ours), so they are not compared.
            static constexpr uint32_t kMask[9] = {~0u, ~0u, ~0u, ~0u, ~0u, ~0u, 0xFFFFu, ~0u, 0xFFFFu};
            for (uint32_t k = 0; k < 9 && same; ++k) same = ((g.U32(sink.at[i] + 4u + 4u * k) ^ o.w[k]) & kMask[k]) == 0;
            if (same) {
                used[i] = true;
                found = true;
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
        if (found) {
            ++equal;
            ++near4;
        } else if (best <= 4.0) {
            ++near4;
        }
        std::printf("    row %4zu: %08X %08X %08X ... tpage %04X clut %04X  %s\n", o.row, o.w[0], o.w[1], o.w[2],
                    o.w[4] >> 16, o.w[2] >> 16, found ? "EQUAL word for word" : "no equal packet of ours");
        if (!found && nearest < sink.at.size()) {
            std::printf("              ours nearest:");
            for (uint32_t k = 0; k < 9; ++k) std::printf(" %08X", g.U32(sink.at[nearest] + 4u + 4u * k));
            std::printf("\n              original    :");
            for (uint32_t k = 0; k < 9; ++k) std::printf(" %08X", o.w[k]);
            std::printf("\n");
        }
        // The texels the packet samples, through the renderer's rule, ours against the capture's.
        const uint16_t tpage = static_cast<uint16_t>(o.w[4] >> 16), clut = static_cast<uint16_t>(o.w[2] >> 16);
        int u0 = 255, u1 = 0, v0 = 255, v1 = 0;
        for (uint32_t v = 0; v < 4; ++v) {
            const uint32_t uv = o.w[2 + 2 * v];
            u0 = std::min(u0, static_cast<int>(uv & 0xFF));
            u1 = std::max(u1, static_cast<int>(uv & 0xFF));
            v0 = std::min(v0, static_cast<int>((uv >> 8) & 0xFF));
            v1 = std::max(v1, static_cast<int>((uv >> 8) & 0xFF));
        }
        for (int v = v0; v <= v1; ++v)
            for (int u = u0; u <= u1; ++u) {
                ++texels;
                if (rr::render::FxDraw::Texel(ours, tpage, clut, u, v) != rr::render::FxDraw::Texel(cap, tpage, clut, u, v))
                    ++texelsDiff;
            }
    }
    std::printf("  packets equal word for word: %zu of %zu (within 4 px: %zu)\n", equal, orig.size(), near4);
    std::printf("  texels those packets sample, the product's sheet against the capture's VRAM: %zu, differ %zu\n",
                texels, texelsDiff);
    const bool pass = !orig.empty() && equal == orig.size() && sink.at.size() == orig.size() && texelsDiff == 0;
    std::printf("  verdict %s\n", pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}
