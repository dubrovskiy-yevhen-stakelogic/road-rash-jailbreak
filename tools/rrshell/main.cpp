// rrshell - development checks of the front end.
//
//   rrshell vram <statedir> <out.png>            the capture's whole VRAM as a 1024x512 picture
//
// The product is `rrgame` (started without --race it opens the front end); this tool only holds the
// checks that compare our shell with the original's, so they do not crowd rrgame's command line.
#include "game/shell/front_end.h"
#include "game/shell/shell_arena.h"
#include "game/shell/shell_card.h"
#include "game/shell/shell_sound.h"
#include "platform/png.h"
#include "rrvfs/disc_image.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <string>
#include <utility>
#include <vector>

namespace {

bool ReadAll(const std::string& path, std::vector<uint8_t>& out) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    out.resize(n > 0 ? static_cast<size_t>(n) : 0);
    const size_t got = out.empty() ? 0 : std::fread(out.data(), 1, out.size(), f);
    std::fclose(f);
    return got == out.size();
}

int CmdVram(const std::string& stateDir, const std::string& outPath, int cropX = 0, int cropY = 0, int cropW = 0,
            int cropH = 0, bool swapBytes = false) {
    std::vector<uint8_t> vram;
    if (!ReadAll(stateDir + "/vram.bin", vram) || vram.size() != 1024u * 512u * 2u) {
        std::fprintf(stderr, "cannot read %s/vram.bin (1 MiB)\n", stateDir.c_str());
        return 1;
    }
    // The RetroArch/PCSX capture (retro-shell) stores every VRAM halfword BIG-endian: read little-endian
    // it is colour noise, read swapped it is the menu (measured on work\oracle\state\retro-shell).
    if (swapBytes)
        for (size_t i = 0; i + 1 < vram.size(); i += 2) std::swap(vram[i], vram[i + 1]);
    std::vector<uint8_t> rgba(1024u * 512u * 4u);
    for (size_t i = 0; i < 1024u * 512u; ++i) {
        const uint16_t p = static_cast<uint16_t>(vram[2 * i] | (vram[2 * i + 1] << 8));
        rgba[4 * i + 0] = static_cast<uint8_t>((p & 31) << 3);
        rgba[4 * i + 1] = static_cast<uint8_t>(((p >> 5) & 31) << 3);
        rgba[4 * i + 2] = static_cast<uint8_t>(((p >> 10) & 31) << 3);
        rgba[4 * i + 3] = 255;
    }
    rr::WritePng(outPath, 1024, 512, rgba);
    std::printf("wrote %s\n", outPath.c_str());
    // The same memory read as 24-bit pixels (the display mode MDEC pictures are shown in), 1024 x 512
    // halfwords = 682 x 512 pixels, for a human to find the displayed buffer.
    std::vector<uint8_t> rgb24(682u * 512u * 4u);
    for (size_t y = 0; y < 512; ++y)
        for (size_t x = 0; x < 682; ++x) {
            const uint8_t* s = vram.data() + y * 2048u + x * 3u;
            uint8_t* d = rgb24.data() + (y * 682u + x) * 4u;
            d[0] = s[0];
            d[1] = s[1];
            d[2] = s[2];
            d[3] = 255;
        }
    rr::WritePng(outPath + ".24.png", 682, 512, rgb24);
    if (cropW > 0) {
        // The crop read as 24-bit pixels from halfword `cropX` on (`cropW` halfwords = 2/3 as many pixels).
        const int w24 = cropW * 2 / 3;
        std::vector<uint8_t> c24(static_cast<size_t>(w24) * cropH * 4u);
        for (int y = 0; y < cropH; ++y)
            for (int x = 0; x < w24; ++x) {
                const uint8_t* s = vram.data() + static_cast<size_t>(cropY + y) * 2048u + cropX * 2u + x * 3u;
                uint8_t* d = &c24[(static_cast<size_t>(y) * w24 + x) * 4u];
                d[0] = s[0];
                d[1] = s[1];
                d[2] = s[2];
                d[3] = 255;
            }
        rr::WritePng(outPath + ".crop24.png", w24, cropH, c24);
        std::vector<uint8_t> crop(static_cast<size_t>(cropW) * cropH * 4u);
        for (int y = 0; y < cropH; ++y)
            for (int x = 0; x < cropW; ++x)
                std::memcpy(&crop[(static_cast<size_t>(y) * cropW + x) * 4u],
                            &rgba[(static_cast<size_t>(cropY + y) * 1024u + cropX + x) * 4u], 4);
        rr::WritePng(outPath + ".crop.png", cropW, cropH, crop);
    }
    return 0;
}

// geninit: the net effect of one of RASHCDF's table initialisers (straight-line constant stores and
// count-down fill loops, no calls), evaluated from the player's own image and printed as C++ for the port
// in src\game\shell\shell_init.cpp. DEVELOPMENT ONLY: the product runs the printed port, never this.
// An instruction outside the subset stops the evaluation with an error rather than being skipped.
int CmdGenInit(const std::string& overlayPath, uint32_t entry, const std::string& name) {
    std::vector<uint8_t> img;
    if (!ReadAll(overlayPath, img)) {
        std::fprintf(stderr, "cannot read %s\n", overlayPath.c_str());
        return 1;
    }
    const uint32_t base = 0x8005B5E8u;
    auto word = [&](uint32_t a) -> uint32_t {
        const uint32_t o = a - base;
        if (a < base || o + 4u > img.size()) return 0xFFFFFFFFu;
        return static_cast<uint32_t>(img[o] | (img[o + 1] << 8) | (img[o + 2] << 16) | (img[o + 3] << 24));
    };
    uint32_t r[32] = {};
    struct Store {
        uint32_t value;
        int bytes;
    };
    std::vector<std::pair<uint32_t, Store>> stores; // program order; later ones win
    uint32_t pc = entry;
    bool pendingBranch = false;
    uint32_t branchTarget = 0;
    for (int steps = 0; steps < 200000; ++steps) {
        const uint32_t w = word(pc);
        const uint32_t op = w >> 26, rs = (w >> 21) & 31u, rt = (w >> 16) & 31u, rd = (w >> 11) & 31u;
        const uint32_t sh = (w >> 6) & 31u, fn = w & 63u;
        const uint32_t imm = w & 0xFFFFu;
        const uint32_t simm = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(imm)));
        const bool inDelay = pendingBranch;
        uint32_t nextPc = pc + 4u;
        bool taken = false;
        uint32_t target = 0;
        bool isJrRa = false;
        switch (op) {
        case 0x00:
            if (fn == 0x21 || fn == 0x25) r[rd] = (fn == 0x21) ? r[rs] + r[rt] : (r[rs] | r[rt]);
            else if (fn == 0x00) r[rd] = r[rt] << sh;
            else if (fn == 0x08 && rs == 31) isJrRa = true;
            else {
                std::fprintf(stderr, "0x%08X: special function 0x%02X outside the subset\n", pc, fn);
                return 1;
            }
            break;
        case 0x01: {
            const int32_t v = static_cast<int32_t>(r[rs]);
            if (rt == 1) taken = v >= 0;
            else if (rt == 0) taken = v < 0;
            else {
                std::fprintf(stderr, "0x%08X: regimm %u outside the subset\n", pc, rt);
                return 1;
            }
            target = pc + 4u + (simm << 2);
            break;
        }
        case 0x02: taken = true; target = (pc & 0xF0000000u) | ((w & 0x03FFFFFFu) << 2); break;
        case 0x04: taken = r[rs] == r[rt]; target = pc + 4u + (simm << 2); break;
        case 0x05: taken = r[rs] != r[rt]; target = pc + 4u + (simm << 2); break;
        case 0x06: taken = static_cast<int32_t>(r[rs]) <= 0; target = pc + 4u + (simm << 2); break;
        case 0x07: taken = static_cast<int32_t>(r[rs]) > 0; target = pc + 4u + (simm << 2); break;
        case 0x09: r[rt] = r[rs] + simm; break;
        case 0x0A: r[rt] = static_cast<int32_t>(r[rs]) < static_cast<int32_t>(simm) ? 1u : 0u; break;
        case 0x0B: r[rt] = r[rs] < simm ? 1u : 0u; break;
        case 0x0D: r[rt] = r[rs] | imm; break;
        case 0x0F: r[rt] = imm << 16; break;
        case 0x28: stores.push_back({r[rs] + simm, {r[rt] & 0xFFu, 1}}); break;
        case 0x29: stores.push_back({r[rs] + simm, {r[rt] & 0xFFFFu, 2}}); break;
        case 0x2B: stores.push_back({r[rs] + simm, {r[rt], 4}}); break;
        default:
            std::fprintf(stderr, "0x%08X: opcode 0x%02X outside the subset\n", pc, op);
            return 1;
        }
        r[0] = 0;
        const bool isBranch = (op >= 0x01 && op <= 0x07) || isJrRa;
        if (inDelay) {
            if (branchTarget == 0xFFFFFFFFu) break; // the delay slot of jr ra
            nextPc = branchTarget;
            pendingBranch = false;
        } else if (isBranch) {
            if (isJrRa) {
                pendingBranch = true;
                branchTarget = 0xFFFFFFFFu;
            } else if (taken) {
                pendingBranch = true;
                branchTarget = target;
            } else {
                pendingBranch = true;
                branchTarget = pc + 8u;
            }
        }
        pc = nextPc;
    }
    // The net effect, by address. A later store wins; stores of different widths to one word are
    // reported as bytes.
    std::vector<std::pair<uint32_t, uint8_t>> bytes;
    {
        std::vector<std::pair<uint32_t, uint8_t>> all;
        for (const auto& s : stores)
            for (int b = 0; b < s.second.bytes; ++b)
                all.push_back({s.first + static_cast<uint32_t>(b), static_cast<uint8_t>(s.second.value >> (8 * b))});
        std::vector<std::pair<uint32_t, uint8_t>> last;
        for (size_t i = 0; i < all.size(); ++i) {
            bool later = false;
            for (size_t j = i + 1; j < all.size(); ++j)
                if (all[j].first == all[i].first) {
                    later = true;
                    break;
                }
            if (!later) last.push_back(all[i]);
        }
        std::sort(last.begin(), last.end());
        bytes = last;
    }
    std::printf("// %s: the net effect of RASHCDF 0x%08X, %zu store(s) evaluated from the image.\n", name.c_str(),
                entry, stores.size());
    std::printf("void %s(GuestRam& g) {\n", name.c_str());
    size_t i = 0;
    while (i < bytes.size()) {
        // Words where four aligned bytes are present.
        const uint32_t a = bytes[i].first;
        if ((a & 3u) == 0 && i + 3 < bytes.size() && bytes[i + 3].first == a + 3u) {
            const uint32_t v = static_cast<uint32_t>(bytes[i].second | (bytes[i + 1].second << 8) |
                                                     (bytes[i + 2].second << 16) | (bytes[i + 3].second << 24));
            // A run of the same word.
            size_t n = 1;
            while (i + 4 * n + 3 < bytes.size() && bytes[i + 4 * n].first == a + 4u * n &&
                   bytes[i + 4 * n + 3].first == a + 4u * n + 3u) {
                const uint32_t v2 = static_cast<uint32_t>(bytes[i + 4 * n].second | (bytes[i + 4 * n + 1].second << 8) |
                                                          (bytes[i + 4 * n + 2].second << 16) |
                                                          (bytes[i + 4 * n + 3].second << 24));
                if (v2 != v) break;
                ++n;
            }
            if (n >= 4) {
                std::printf("    for (uint32_t i = 0; i < %zuu; ++i) g.W32(0x%08Xu + 4u * i, 0x%08Xu);\n", n, a, v);
            } else {
                for (size_t k = 0; k < n; ++k) std::printf("    g.W32(0x%08Xu, 0x%08Xu);\n", a + 4u * static_cast<uint32_t>(k), v);
            }
            i += 4 * n;
            continue;
        }
        std::printf("    g.W8(0x%08Xu, 0x%02Xu);\n", a, bytes[i].second);
        ++i;
    }
    std::printf("}\n");
    return 0;
}

// A host that records every seam call and does nothing else.
struct RecordingCallees final : rr::shell::ShellCallees {
    std::vector<uint32_t> calls;
    bool Call(uint32_t address, const uint32_t*, int, uint32_t* v0) override {
        calls.push_back(address);
        if (v0 != nullptr) *v0 = 0;
        return true;
    }
};

// arenacheck: the arena our ported boot builds from the disc against the capture of the running shell.
int CmdArenaCheck(const std::string& discPath, const std::string& stateDir, bool mutate) {
    rr::DiscImage disc(discPath);
    rr::shell::ShellArena arena;
    RecordingCallees k;
    std::string report;
    rr::shell::BuildShellArena(disc, arena, k, 0, &report);
    if (mutate) {
        // The negative control: one entry of the advance table off by one must be caught.
        rr::shell::GuestRam g = arena.View();
        g.W32(rr::shell::kNavAdvance + 4u * 4u, g.U32(rr::shell::kNavAdvance + 4u * 4u) + 1u);
    }
    std::vector<uint8_t> cap;
    if (!ReadAll(stateDir + "/ram.bin", cap) || cap.size() != 0x200000u) {
        std::fprintf(stderr, "cannot read %s/ram.bin\n", stateDir.c_str());
        return 1;
    }
    std::printf("%s", report.c_str());
    std::printf("seam calls during the boot: %zu\n", k.calls.size());
    std::string cmp;
    const size_t diff = rr::shell::CompareArenaWithCapture(arena, cap, cmp);
    std::printf("the arena against %s/ram.bin:\n%s", stateDir.c_str(), cmp.c_str());
    std::printf("%s: %zu byte(s) differ\n", diff == 0 ? "PASS" : "FAIL", diff);
    return diff == 0 ? 0 : 1;
}

// screens: every screen record and its widgets as the arena holds them after the boot.
int CmdScreens(const std::string& discPath, int only) {
    rr::DiscImage disc(discPath);
    rr::shell::ShellArena arena;
    RecordingCallees k;
    rr::shell::BuildShellArena(disc, arena, k, 0, nullptr);
    rr::shell::GuestRam g = arena.View();
    for (int id = 0; id < rr::shell::kScreenCount; ++id) {
        if (only >= 0 && id != only) continue;
        const uint32_t s = g.U32(rr::shell::kScreenTable + 4u * static_cast<uint32_t>(id));
        if (s == 0) continue;
        const int n = g.S16(s + 8u);
        std::printf("screen %2d @%08X flags %04X sel %d id %d items %d parent %d anim %u/%u pads %d+%d  input %08X\n",
                    id, s, g.U16(s), g.S16(s + 4u), g.S16(s + 6u), n, g.S16(s + 10u), g.U8(s + 12u), g.U8(s + 13u),
                    g.S8(s + 14u), g.S8(s + 15u), g.U32(rr::shell::kScreenInput + 4u * static_cast<uint32_t>(id)));
        const uint32_t items = g.U32(s + 16u);
        for (int i = 0; items != 0 && i < n && i < 64; ++i) {
            const uint32_t w = items + 120u * static_cast<uint32_t>(i);
            std::printf("   w%-2d t%-2d fl %04X c0 %08X c1 %08X |", i, g.S16(w + 8u), g.U16(w + 10u), g.U32(w), g.U32(w + 4u));
            for (uint32_t o = 0x10; o < 0x78; o += 2) std::printf(" %04X", g.U16(w + o));
            std::printf("\n");
        }
    }
    return 0;
}

// shot: the product's front end run headless for N frames with a key script, our picture written as a
// PNG at the frames asked for, one status line per frame the script touches.
//   script: "<frame>:<keys>;<frame>:<keys>..." keys = up down left right x(cross) t(triangle) s(square) c(circle)
//   shots:  "<frame>,<frame>,..."  -> <prefix>_<frame>.png
int CmdShot(const std::string& discPath, const std::string& prefix, int frames, const std::string& script,
            const std::string& shots) {
    rr::DiscImage disc(discPath);
    rr::shell::FrontEnd fe(disc, 0);
    std::printf("%s", fe.ArenaReport().c_str());
    std::vector<std::pair<int, std::string>> events;
    for (size_t at = 0; at < script.size();) {
        const size_t semi = script.find(';', at);
        const std::string e = script.substr(at, semi == std::string::npos ? std::string::npos : semi - at);
        const size_t colon = e.find(':');
        if (colon != std::string::npos) events.push_back({std::atoi(e.substr(0, colon).c_str()), e.substr(colon + 1)});
        if (semi == std::string::npos) break;
        at = semi + 1;
    }
    std::vector<int> shotFrames;
    for (size_t at = 0; at < shots.size();) {
        const size_t comma = shots.find(',', at);
        shotFrames.push_back(std::atoi(shots.substr(at, comma == std::string::npos ? std::string::npos : comma - at).c_str()));
        if (comma == std::string::npos) break;
        at = comma + 1;
    }
    int lastScreen = -1;
    for (int f = 1; f <= frames; ++f) {
        rr::shell::ShellKeys k;
        bool touched = false;
        for (const auto& ev : events) {
            if (ev.first != f) continue;
            touched = true;
            for (size_t at = 0; at < ev.second.size();) {
                const size_t comma = ev.second.find(',', at);
                const std::string key = ev.second.substr(at, comma == std::string::npos ? std::string::npos : comma - at);
                if (key == "up") k.up = true;
                if (key == "down") k.down = true;
                if (key == "left") k.left = true;
                if (key == "right") k.right = true;
                if (key == "x") k.cross = true;
                if (key == "t") k.triangle = true;
                if (key == "s") k.square = true;
                if (key == "c") k.circle = true;
                if (key.size() > 1 && key[0] == 'g') // development: GotoScreen(n), the boot's own entry 0x800809E0
                    rr::shell::GotoScreen(fe.Ram(), std::atoi(key.c_str() + 1));
                if (comma == std::string::npos) break;
                at = comma + 1;
            }
        }
        fe.Frame(k);
        const int cur = fe.CurrentScreen();
        if (touched || cur != lastScreen || fe.RaceRequested())
            std::printf("f%-5d %s%s\n", f, fe.Status().c_str(), fe.RaceRequested() ? "  <- game_state+0x00 = 3: RACE" : "");
        lastScreen = cur;
        for (int s : shotFrames)
            if (s == f) {
                std::vector<uint8_t> rgba;
                fe.Draw(rgba);
                const std::string path = prefix + "_" + std::to_string(f) + ".png";
                rr::WritePng(path, rr::shell::ShellView::kWidth, rr::shell::ShellView::kHeight, rgba);
                std::printf("wrote %s\n", path.c_str());
            }
        if (fe.RaceRequested()) {
            rr::shell::Handover h;
            fe.BeginRace(h);
            std::printf("handover: set %d race %d bike .PH %s, game_state +0x04 0x%02X +0x3C %u +0x3A %u\n", h.set, h.raceId,
                        h.playerBikePh.c_str(), h.gameState[4], h.gameState[0x3C], h.gameState[0x3A]);
            break;
        }
    }
    for (const std::string& m : fe.View().Missing()) std::printf("view: %s\n", m.c_str());
    return 0;
}

// framecheck: our main menu against the capture's displayed frame. The capture (retro-shell) holds the
// menu's framebuffer at real VRAM (0, 0); its vram.bin is stored 281 bytes late (one byte plus 140
// halfwords: real halfword k = raw[2(k+140)-1] | raw[2(k+140)] << 8), measured by locating all 26
// FEMISC shapes and the sprite records' own VRAM rectangles in it.
int CmdFrameCheck(const std::string& discPath, const std::string& stateDir, const std::string& prefix) {
    std::vector<uint8_t> raw;
    if (!ReadAll(stateDir + "/vram.bin", raw) || raw.size() != 1024u * 512u * 2u) {
        std::fprintf(stderr, "cannot read %s/vram.bin\n", stateDir.c_str());
        return 1;
    }
    auto real = [&](int x, int y) -> uint16_t {
        const size_t k = static_cast<size_t>(y) * 1024u + static_cast<size_t>(x) + 140u;
        if (2 * k < 1 || 2 * k >= raw.size()) return 0;
        return static_cast<uint16_t>(raw[2 * k - 1] | (raw[2 * k] << 8));
    };
    rr::DiscImage disc(discPath);
    rr::shell::FrontEnd fe(disc, 0);
    rr::shell::GotoScreen(fe.Ram(), 4);
    // 166 frames: the panel's RR_LOGO.STR on its frame 41 (one frame per 4 vblanks), the frame the
    // capture's front buffer holds (located in its VRAM by the asset check).
    for (int f = 0; f < 166; ++f) fe.Frame(rr::shell::ShellKeys{});
    std::vector<uint8_t> ours;
    fe.Draw(ours);
    const int W = rr::shell::ShellView::kWidth, H = rr::shell::ShellView::kHeight;
    std::vector<uint8_t> side(static_cast<size_t>(W) * 2u * H * 4u);
    size_t close = 0, exact5 = 0, total = 0, outClose = 0, outTotal = 0;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const uint16_t p = real(x, y);
            const uint8_t cr = static_cast<uint8_t>((p & 31) << 3), cg = static_cast<uint8_t>(((p >> 5) & 31) << 3),
                          cb = static_cast<uint8_t>(((p >> 10) & 31) << 3);
            const uint8_t* o = &ours[(static_cast<size_t>(y) * W + static_cast<size_t>(x)) * 4u];
            uint8_t* l = &side[(static_cast<size_t>(y) * W * 2u + static_cast<size_t>(x)) * 4u];
            uint8_t* rgt = &side[(static_cast<size_t>(y) * W * 2u + static_cast<size_t>(W + x)) * 4u];
            l[0] = cr; l[1] = cg; l[2] = cb; l[3] = 255;
            std::memcpy(rgt, o, 4);
            ++total;
            const int dr = std::abs(o[0] - cr), dg = std::abs(o[1] - cg), db = std::abs(o[2] - cb);
            const bool ok24 = dr <= 24 && dg <= 24 && db <= 24;
            if (ok24) ++close;
            if ((o[0] >> 3) == (cr >> 3) && (o[1] >> 3) == (cg >> 3) && (o[2] >> 3) == (cb >> 3)) ++exact5;
            // Outside the panel whose film runs on the clock (RR_LOGO.STR at 230,111, 256 x 112).
            if (!(x >= 230 && x < 486 && y >= 111 && y < 223)) {
                ++outTotal;
                if (ok24) ++outClose;
            }
        }
    rr::WritePng(prefix + "_capture_vs_ours.png", W * 2, H, side);
    std::printf("main menu (screen 4, Solo highlighted), 512 x 240: %.1f%% of pixels within 24 levels of the capture, "
                "%.1f%% equal at 5 bits; outside the animated panel %.1f%% within 24 levels\n"
                "wrote %s_capture_vs_ours.png (left the capture, right ours)\n",
                100.0 * static_cast<double>(close) / static_cast<double>(total),
                100.0 * static_cast<double>(exact5) / static_cast<double>(total),
                100.0 * static_cast<double>(outClose) / static_cast<double>(outTotal), prefix.c_str());
    return 0;
}

// cardcheck: the save format against the player's own cards. (1) the career card: every record's
// checksum recomputed; (2) its career slot 0 loaded into a fresh arena with the ported LoadSlot copies
// and saved again with the ported SaveSlot into a freshly formatted card - the 484-byte record must
// come out byte for byte what the card holds; (3) our formatted card against the player's empty card.
int CmdCardCheck(const std::string& discPath, const std::string& careerCard, const std::string& emptyCard, bool mutate) {
    rr::DiscImage disc(discPath);
    rr::shell::ShellArena arena;
    RecordingCallees k;
    rr::shell::BuildShellArena(disc, arena, k, 0, nullptr);
    rr::shell::GuestRam g = arena.View();
    rr::shell::CardImage user;
    std::string error;
    if (!rr::shell::LoadCardFile(careerCard, user, error)) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }
    if (mutate) user.raw[static_cast<size_t>(user.block) * 8192u + 0x834u + 40u] ^= 1u; // negative control
    int bad = 0;
    std::printf("(1) %s\n%s", careerCard.c_str(), rr::shell::CheckCard(user, &bad).c_str());
    bool ok = bad == 0;
    if (!rr::shell::LoadCareer(user, g, 0, error)) {
        std::printf("(2) load: %s\n", error.c_str());
        ok = false;
    } else {
        rr::shell::CardImage ours = rr::shell::FormatCard();
        if (!rr::shell::SaveCareer(ours, g, 0, error)) {
            std::printf("(2) save: %s\n", error.c_str());
            ok = false;
        } else {
            const uint8_t* a = user.raw.data() + static_cast<size_t>(user.block) * 8192u + 0x834u;
            const uint8_t* b = ours.raw.data() + static_cast<size_t>(ours.block) * 8192u + 0x834u;
            size_t diff = 0;
            for (size_t i = 0; i < 484u; ++i) diff += a[i] != b[i];
            const uint8_t* ha = user.raw.data() + static_cast<size_t>(user.block) * 8192u;
            const uint8_t* hb = ours.raw.data() + static_cast<size_t>(ours.block) * 8192u;
            size_t hdiff = 0;
            for (size_t i = 0; i < 0x200u; ++i) hdiff += ha[i] != hb[i];
            const uint8_t* da = user.raw.data() + static_cast<size_t>(user.block) * 128u;
            const uint8_t* db = ours.raw.data() + static_cast<size_t>(ours.block) * 128u;
            size_t ddiff = 0;
            for (size_t i = 0; i < 128u; ++i) ddiff += da[i] != db[i];
            std::printf("(2) career slot 0 loaded (LoadSlot copies) and saved again (SaveSlot, checksum): record %zu of "
                        "484 bytes differ; SC header %zu of 512; directory frame %zu of 128\n",
                        diff, hdiff, ddiff);
            ok = ok && diff == 0 && hdiff == 0 && ddiff == 0;
        }
    }
    if (!emptyCard.empty()) {
        rr::shell::CardImage empty;
        if (rr::shell::LoadCardFile(emptyCard, empty, error)) {
            const rr::shell::CardImage fresh = rr::shell::FormatCard();
            size_t diff = 0;
            for (size_t i = 0; i < fresh.raw.size(); ++i) diff += fresh.raw[i] != empty.raw[i];
            std::printf("(3) a formatted card against %s: %zu of 131072 bytes differ\n", emptyCard.c_str(), diff);
            ok = ok && diff == 0;
        }
    }
    std::printf("%s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

// soundcheck: the front end run headless from the boot through the main menu into Options -> Sound
// (the script below), its UI clicks and music requests played through ShellSound into a mixer that is
// rendered after every frame, and what came out measured - so "the menu makes a sound" is a number.
// Then the 15 sounds of FRONTEND.VUK each keyed alone and measured, and a control: the same run with
// the effects slider at 0 (0x800D6C0C) must render the clicks silent.
int CmdSoundCheck(const std::string& discPath) {
    rr::DiscImage disc(discPath);
    rr::shell::ShellSound sound(disc);
    if (!sound.Ready()) {
        std::printf("FAIL: %s\n", sound.Error().c_str());
        return 1;
    }
    auto peakOf = [](rr::audio::Mixer& m, size_t frames) {
        std::vector<int16_t> buf(frames * 2);
        m.Mix(buf.data(), frames);
        int peak = 0;
        for (int16_t v : buf) peak = std::max(peak, v < 0 ? -static_cast<int>(v) : static_cast<int>(v));
        return peak;
    };
    int loud = 0;
    for (uint32_t n = 0; n < 15u; ++n) {
        rr::shell::FrontEnd fe(disc, 0);
        rr::audio::Mixer mixer(44100, 4);
        sound.Stop(mixer);
        sound.Frame(fe.Ram(), {n}, -1, false, mixer);
        const int peak = peakOf(mixer, 44100 / 4);
        std::printf("  click %2u: peak %5d over the first 250 ms\n", n, peak);
        loud += peak > 256 ? 1 : 0;
    }
    // The menu itself: every frame's clicks and music requests, as rrgame plays them.
    rr::shell::FrontEnd fe(disc, 0);
    rr::audio::Mixer mixer(44100, 4);
    sound.Stop(mixer);
    int menuPeak = 0, musicPeak = 0;
    size_t clicks = 0, requests = 0;
    for (int f = 1; f <= 400; ++f) {
        rr::shell::ShellKeys k;
        if (f == 2) rr::shell::GotoScreen(fe.Ram(), 4); // the main menu, as rrgame's "g4"
        k.down = (f == 20 || f == 40);
        k.cross = (f == 60);
        fe.Frame(k);
        clicks += fe.Callees().uiSounds.size();
        requests += fe.Callees().musicRequest >= 0 ? 1u : 0u;
        sound.Frame(fe.Ram(), fe.Callees().uiSounds, fe.Callees().musicRequest, fe.Callees().musicStop, mixer);
        const int p = peakOf(mixer, 735);
        menuPeak = std::max(menuPeak, p);
        if (sound.MusicTrack() >= 0) musicPeak = std::max(musicPeak, p);
    }
    std::printf("menu run: %zu click request(s), %zu music request(s), track %d, peak %d (with music %d)\n", clicks,
                requests, sound.MusicTrack(), menuPeak, musicPeak);
    // The music's loop, as the SPU plays it (shell_sound.h): the two streamed voices live in the track's
    // SPU image 0xFFB0..0x7FFB0; rendered on for 40 s, the left voice's address must come back (the ADPCM
    // loop flags) and the music must still sound after it has.
    bool looped = false;
    int afterLoop = 0;
    {
        const auto spu = sound.Spu();
        int ch = -1;
        for (int c = 0; c < rr::audio::SpuVoices::kChannels && ch < 0; ++c) {
            const auto st = spu->State(c);
            if (st.on && st.address >= 0xFFB0u && st.address < 0x47FB0u) ch = c;
        }
        uint32_t last = ch >= 0 ? spu->State(ch).address : 0;
        for (int sec = 1; sec <= 40 && ch >= 0; ++sec) {
            int p = 0;
            for (int f = 0; f < 60; ++f) {
                rr::shell::ShellKeys none;
                fe.Frame(none);
                sound.Frame(fe.Ram(), fe.Callees().uiSounds, fe.Callees().musicRequest, fe.Callees().musicStop, mixer);
                p = std::max(p, peakOf(mixer, 735));
                const auto st = spu->State(ch);
                if (st.address + 0x10000u < last && !looped) {
                    looped = true;
                    std::printf("music: voice %d went back from 0x%05X to 0x%05X (repeat 0x%05X) %.1f s after the menu run\n", ch,
                                last, st.address, st.repeat, sec - 1 + f / 60.0);
                }
                last = st.address;
            }
            if (looped) afterLoop = std::max(afterLoop, p);
        }
        if (ch < 0) std::printf("music: no streamed voice is sounding\n");
        std::printf("music: after the loop peak %d\n", afterLoop);
    }
    // The jukebox (screen 47): Down moves the track chooser, Square starts the chosen song - the shell's
    // fe+0x0E must become the music that plays.
    bool jukebox = false;
    {
        rr::shell::GotoScreen(fe.Ram(), 47);
        const int before = sound.MusicTrack();
        for (int f = 1; f <= 120; ++f) {
            rr::shell::ShellKeys k;
            k.down = f == 30;
            k.square = f == 60;
            fe.Frame(k);
            sound.Frame(fe.Ram(), fe.Callees().uiSounds, fe.Callees().musicRequest, fe.Callees().musicStop, mixer);
            peakOf(mixer, 735);
        }
        const int chosen = fe.Ram().U8(rr::shell::kFeTrack);
        jukebox = sound.MusicTrack() == chosen && chosen != before && sound.MusicPlaying();
        std::printf("jukebox: track %d before, the chooser's %d, playing %d (%s)\n", before, chosen, sound.MusicTrack(),
                    sound.MusicPlaying() ? "sounding" : "silent");
    }
    // The control: the effects slider at 0 silences a click.
    rr::shell::FrontEnd quiet(disc, 0);
    quiet.Ram().W32(0x800D6C0Cu, 0);
    rr::audio::Mixer qm(44100, 4);
    sound.Stop(qm);
    sound.Frame(quiet.Ram(), {1u}, -1, false, qm);
    const int silent = peakOf(qm, 44100 / 4);
    std::printf("control: click 1 with the effects slider at 0: peak %d\n", silent);
    const bool ok = loud >= 10 && silent == 0 && clicks > 0 && looped && afterLoop > 256 && jukebox;
    std::printf("%s: %d of 15 sounds audible, the slider control %s\n", ok ? "PASS" : "FAIL", loud,
                silent == 0 ? "silent" : "NOT silent");
    return ok ? 0 : 1;
}

int Usage() {
    std::fprintf(stderr,
                 "usage: rrshell vram <statedir> <out.png> [x y w h]\n"
                 "       rrshell vramswap <statedir> <out.png> x y w h\n"
                 "       rrshell geninit <RASHCDF.BIN> <entry hex> <FunctionName>\n");
    return 2;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) return Usage();
    const std::string cmd = argv[1];
    if (cmd == "vram" && argc == 4) return CmdVram(argv[2], argv[3]);
    if (cmd == "vram" && argc == 8)
        return CmdVram(argv[2], argv[3], std::atoi(argv[4]), std::atoi(argv[5]), std::atoi(argv[6]),
                       std::atoi(argv[7]), false);
    if (cmd == "vramswap" && argc == 8)
        return CmdVram(argv[2], argv[3], std::atoi(argv[4]), std::atoi(argv[5]), std::atoi(argv[6]),
                       std::atoi(argv[7]), true);
    if ((cmd == "arenacheck" || cmd == "arenacheck-mutate") && argc == 4) {
        try {
            return CmdArenaCheck(argv[2], argv[3], cmd == "arenacheck-mutate");
        } catch (const std::exception& e) {
            std::fprintf(stderr, "error: %s\n", e.what());
            return 1;
        }
    }
    if (cmd == "screens" && (argc == 3 || argc == 4)) return CmdScreens(argv[2], argc == 4 ? std::atoi(argv[3]) : -1);
    if (cmd == "shot" && argc >= 5) {
        try {
            return CmdShot(argv[2], argv[3], std::atoi(argv[4]), argc > 5 ? argv[5] : "", argc > 6 ? argv[6] : "");
        } catch (const std::exception& e) {
            std::fprintf(stderr, "error: %s\n", e.what());
            return 1;
        }
    }
    if (cmd == "framecheck" && argc == 5) return CmdFrameCheck(argv[2], argv[3], argv[4]);
    if ((cmd == "cardcheck" || cmd == "cardcheck-mutate") && (argc == 4 || argc == 5))
        return CmdCardCheck(argv[2], argv[3], argc == 5 ? argv[4] : "", cmd == "cardcheck-mutate");
    if (cmd == "soundcheck" && argc == 3) {
        try {
            return CmdSoundCheck(argv[2]);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "error: %s\n", e.what());
            return 1;
        }
    }
    if (cmd == "geninit" && argc == 5)
        return CmdGenInit(argv[2], static_cast<uint32_t>(std::strtoul(argv[3], nullptr, 16)), argv[4]);
    return Usage();
}
