// The shell's guest arena (shell_arena.h). The four table initialisers at the end of this file
// (NavInit, ScreenTableInit, ChooserTableInit, HandlerTableInit) are the ports of RASHCDF's
// straight-line initialisers written as their net effect, address by address; they were produced by
// `rrshell geninit` from our own disassembly of the player's image and are checked against the capture
// by `rrshell arenacheck`.
#include "game/shell/shell_arena.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace rr::shell {

namespace {

constexpr uint32_t kExeLoad = 0x80010000;
constexpr uint32_t kExeHeader = 0x800;
constexpr uint32_t kOverlayLoad = 0x8005B5E8;

void Load(GuestRam& g, uint32_t address, const std::vector<uint8_t>& bytes, size_t from) {
    if (bytes.size() <= from) return;
    const size_t n = bytes.size() - from;
    const size_t room = static_cast<size_t>(0x80200000u - address);
    g.WriteBlock(address, bytes.data() + from, static_cast<uint32_t>(n < room ? n : room));
}

} // namespace

// SLUS 0x80011738 (asm 0x80011738..0x800117B8).
void GameStateInit(GuestRam& g) {
    const uint32_t gs = kGameState;
    g.W32(kGameStatePtr, gs);
    g.W8(gs + 3u, 1);
    g.W8(gs + 4u, 34);
    g.W32(gs + 0x40u, 4);
    g.W32(gs + 0x4Cu, 9);
    g.W8(gs + 5u, 3);
    g.W8(gs + 7u, 3);
    g.W32(gs + 0x0Cu, 0);
    g.W16(gs + 0x28u, 0);
    g.W16(gs + 0x2Au, 0);
    g.W32(gs + 0x2Cu, 0);
    g.W16(gs + 0x3Au, 0);
    g.W32(gs + 0x3Cu, 0);
    g.W32(gs + 0x44u, 0);
    g.W8(gs + 0x38u, 0);
    g.W32(gs + 0x30u, 1);
    g.W32(gs + 0x48u, 0);
    g.W32(gs + 0x58u, 0);
    g.W32(gs + 0x5Cu, 0);
    g.W32(gs + 0x60u, 1);
    g.W8(gs + 6u, 1);
    g.W16(gs + 8u, 120);
}

// RASHCDF 0x8007F20C (asm 0x8007F20C..0x8007F2A4): a volume from a slider value, clamped to 0..128,
// into 0x800D6C00[i] and 0x800D6C20[i].
void VolumeSet(GuestRam& g, int32_t index, int32_t value) {
    const uint32_t i = static_cast<uint32_t>(index);
    const int32_t a = static_cast<int32_t>(static_cast<uint32_t>(value) * g.U32(0x80052638u + 4u * i)) >> 4;
    int32_t v = static_cast<int32_t>(static_cast<uint32_t>(a) * g.U32(0x80052650u)) >> 7;
    if (index == 5 && v < 0) v = g.S32(0x800D6C14u);
    const int32_t low = (v >= 0) ? v : 0;
    const int32_t over = 128 - v;
    const int32_t r = low + (over < 0 ? over : 0);
    g.W32(0x800D6C00u + 4u * i, static_cast<uint32_t>(r));
    g.W32(0x800D6C20u + 4u * i, static_cast<uint32_t>(r));
}

// RASHCDF 0x8007FDA8 (asm 0x8007FDA8..0x8007FED8).
void SessionInit(GuestRam& g) {
    const uint32_t s = kSession;
    g.W32(s, 32);
    g.W8(s + 6u, 1);
    g.W8(s + 4u, 0);
    g.W8(s + 9u, 1);
    g.W8(s + 10u, 1);
    g.W8(s + 11u, 1);
    g.W8(s + 20u, 1);
    g.W8(s + 23u, 0);
    g.W8(s + 25u, 1);
    g.W8(s + 26u, 1);
    g.W8(s + 27u, 0);
    g.W8(s + 28u, 0);
    g.W8(s + 24u, 0);
    g.W8(s + 18u, 1);
    g.W8(s + 29u, 0);
    for (uint32_t p = 0; p < 6u; ++p) {
        g.W8(kPlayers + 36u * p + 23u, 1);
        g.W8(kPlayers + 36u * p + 8u, 0);
    }
    for (uint32_t i = 0; i < 7u; ++i) {
        const uint32_t v = g.U32(0x80088BD0u + 16u * i + 12u);
        g.W32(s + 0xD4u + 4u * i, v);
        VolumeSet(g, static_cast<int32_t>(i), static_cast<int32_t>(v));
    }
    for (uint32_t t = 0; t < 18u; ++t) g.W8(s + 0xC0u + t, 1);
    g.W8(s + 0x0Fu, static_cast<uint8_t>(Rand(g) % 6u + 32u));
}

// RASHCDF 0x800665E8 (asm 0x800665E8..0x800667E0).
void FrontendInit(GuestRam& g, uint32_t seed) {
    const uint32_t fe = kFe;
    g.W16(fe, 0);
    g.W32(kIdle, 0);
    g.W32(0x80088C50u, 0);
    g.W8(fe + 0x1Fu, 56);
    g.W16(fe + 2u, 0);
    g.W16(fe + 10u, 0);
    g.W8(fe + 19u, 0);
    g.W8(fe + 17u, 0);
    g.W8(fe + 18u, 0);
    g.W32(fe + 0xA4u, 0);
    g.W32(fe + 0xA8u, 0);
    g.W32(fe + 0x84u, 0);
    g.W32(fe + 0x88u, 0);
    g.W32(fe + 0x94u, 0);
    g.W8(fe + 22u, 0);
    g.W32(fe + 0xB4u, 0xFFFFFFFFu);
    // srand(((root counter 2 & 0xFF) * 3305) >> 6): OURS, the counter value is the caller's `seed`.
    g.W32(g.gp() + 2076u, static_cast<uint32_t>(static_cast<int32_t>((seed & 0xFFu) * 3305u) >> 6));
    for (int n = 0; n < 200; ++n) {
        const uint32_t t = Rand(g) % 18u;
        g.W8(fe + 14u, static_cast<uint8_t>(t));
        if (g.S8(kSession + 0xC0u + t) != 0) break;
    }
    g.W8(fe + 26u, 0xFF);
    g.W8(fe + 27u, 0xFF);
    g.W32(fe + 0xACu, 300);
    g.W32(fe + 0xB0u, 300);
    g.W8(fe + 15u, 0);
    g.W8(fe + 16u, 0);
    g.W8(fe + 21u, 0);
    g.W8(fe + 30u, 0);
    g.W32(fe + 40u, 0);
    g.W32(fe + 44u, 0);
    g.W16(fe + 28u, 0);
    for (uint32_t o = 68; o <= 110; o += 2) g.W16(fe + o, 0);
    g.W8(fe + 118u, 0);
    g.W32(fe + 48u, g.U32(0x80010E3Cu));
    g.W32(fe + 52u, g.U32(0x80010E40u));
    for (uint32_t o = 0x38; o <= 0x40; o += 4) g.W32(fe + o, 0);
    NavInit(g);
    HandlerTableInit(g);
}

// RASHCDF 0x80080C04: every screen's selection to its starting item (0x80080C50), the option mask of
// venue 0 (0x80064254) and every chooser re-validated and applied (0x80063BFC); fe+0x0E kept.
// The resident text resources of the persistent-resource loader 0x80080544, as their net effect: the
// resource records 2..5 (FESTRING.LOC and the three .PFN fonts, frontend.md 5.2) read to the
// addresses the capture's records hold (the loader's allocator is not ported - OURS: those four
// addresses), the string table relocated by RASHCDF 0x80065F4C (PORTED), and the font slots filled by
// the font arm of 0x80065CA8 (PORTED: the first free slot from 1, +0 loaded, +4/+8/+0xC the file, its
// glyphs, its sheet, +0x14 / +0x16 the largest glyph height / advance, the handle by id-in-class;
// the VRAM upload 0x80066188 is the renderer's). Checked against the capture by `rrshell arenacheck`.
void LoadTextResources(const DiscImage& disc, GuestRam& g) {
    struct Res {
        uint32_t record;
        const char* path;
        uint32_t at;
    };
    const Res res[] = {{2, "DATA/FE/FESTRING.LOC", 0x800A0B08u},
                       {3, "DATA/FE/BTN_FONT.PFN", 0x800B6798u},
                       {4, "DATA/FE/MINIFONT.PFN", 0x800B7FC4u},
                       {5, "DATA/FE/HDR_FONT.PFN", 0x800B9894u}};
    int32_t slot = 0; // DAT_8005B540, the last slot handed out
    for (const Res& r : res) {
        const auto f = disc.Find(r.path);
        if (!f) throw std::runtime_error(std::string("the disc has no ") + r.path);
        const std::vector<uint8_t> bytes = disc.ReadFile(*f);
        g.WriteBlock(r.at, bytes.data(), static_cast<uint32_t>(bytes.size()));
        const uint32_t rec = 0x800897E4u + 32u * r.record;
        g.W32(rec + 4u, r.at);
        if (r.record == 2) { // RASHCDF 0x80065F4C
            const uint32_t d = r.at;
            g.W32(0x8005AE78u, d);
            const uint32_t head = g.U32(d + 0x10u);
            for (int32_t i = 0; i < g.S32(d + 0x20u); ++i) {
                const uint32_t p = d + head + 0x10u + 4u * static_cast<uint32_t>(i);
                g.W32(p, d + g.U32(p) + g.U32(d + 0x10u));
            }
            g.W32(0x8005B544u, d + head + 0x10u);
            continue;
        }
        // RASHCDF 0x80065CA8, the font arm.
        int32_t s = slot + 1 > 3 ? 0 : slot + 1;
        if (g.S8(0x800D8078u + 0x18u * static_cast<uint32_t>(s)) != 0)
            for (s = 0; s < 4 && g.S8(0x800D8078u + 0x18u * static_cast<uint32_t>(s)) != 0; ++s) {}
        slot = s;
        const uint32_t sr = 0x800D8078u + 0x18u * static_cast<uint32_t>(s);
        g.W8(sr, 1);
        g.W8(sr + 2u, 3); // 0x80066188's tail: blend mode 3, index 0
        g.W8(sr + 3u, 0);
        g.W32(sr + 4u, r.at);
        g.W32(sr + 8u, r.at + g.U32(r.at + 0x14u));
        g.W32(sr + 12u, r.at + g.U32(r.at + 0x1Cu));
        const uint8_t id = g.U8(rec + 3u);
        if (id == 1) g.W32(0x8009C5B8u, static_cast<uint32_t>(s));
        else if (id == 2) g.W32(0x8009C5C0u, static_cast<uint32_t>(s));
        else if (id == 3) g.W32(0x8009C5BCu, static_cast<uint32_t>(s));
        g.W16(sr + 0x14u, 0);
        g.W16(sr + 0x16u, 0);
        for (uint32_t i = 0; i < g.U16(r.at + 10u); ++i) {
            const uint32_t gl = g.U32(sr + 8u) + 11u * i;
            if (g.S16(sr + 0x14u) < static_cast<int32_t>(g.U8(gl + 3u))) g.W16(sr + 0x14u, g.U8(gl + 3u));
            if (g.S16(sr + 0x16u) < g.S8(gl + 8u)) g.W16(sr + 0x16u, static_cast<uint16_t>(g.S8(gl + 8u)));
        }
    }
    g.W32(0x8005B540u, static_cast<uint32_t>(slot));
}

bool BootSelections(GuestRam& g, ShellCallees& k) {
    const uint8_t track = g.U8(kFe + 14u);
    for (uint32_t i = 0; i < static_cast<uint32_t>(kScreenCount); ++i) {
        const uint32_t s = g.U32(g.U32(kScreenTablePtr) + 4u * i);
        g.W32(kCurScreen, s);
        if (s != 0) g.W16(g.U32(kCurScreen) + 4u, static_cast<uint16_t>(FirstItem(g, s)));
    }
    SetOptionMask(g, 0);
    for (uint32_t i = 0; i < 37u; ++i) {
        const uint32_t c = g.U32(kChoosers + 4u * i);
        if (c == 0 || g.U32(c + 4u) == 0) continue;
        g.W8(c + 3u, static_cast<uint8_t>(ChooserFirstValid(g, c)));
        if (!ChooserApply(g, k, c)) return false;
    }
    g.W8(kFe + 14u, track);
    return true;
}

void BuildShellArena(const DiscImage& disc, ShellArena& arena, ShellCallees& k, uint32_t seed, std::string* report) {
    const auto exeFile = disc.Find("SLUS_010.53");
    const auto shellFile = disc.Find("RASHCDF.BIN");
    if (!exeFile || !shellFile) throw std::runtime_error("this disc carries no SLUS_010.53 / RASHCDF.BIN");
    const std::vector<uint8_t> exe = disc.ReadFile(*exeFile);
    const std::vector<uint8_t> shell = disc.ReadFile(*shellFile);
    if (exe.size() < kExeHeader || std::memcmp(exe.data(), "PS-X EXE", 8) != 0)
        throw std::runtime_error("SLUS_010.53 is not a PS-X EXE");
    std::fill(arena.ram.begin(), arena.ram.end(), uint8_t{0});
    std::fill(arena.scratchpad.begin(), arena.scratchpad.end(), uint8_t{0});
    GuestRam g = arena.View();
    Load(g, kExeLoad, exe, kExeHeader);
    Load(g, kOverlayLoad, shell, 0);
    // SLUS 0x800118A0(0x10), the overlay loader's net effect: the resident-overlay mask 1 | 0x10 (the
    // text call's wrap mode reads bit 0x10, shell_text.h).
    g.W32(0x8005ACA8u, 0x11u);
    LoadTextResources(disc, g);
    // The cold boot RASHCDF 0x8007FEDC, in its order.
    GameStateInit(g);                      // SLUS 0x80011738
    // 0x80080CC8 (video and draw environments): the product's renderer.
    SessionInit(g);                        // 0x8007FDA8
    // 0x80080544, the persistent resources: game_state+0x34 = 4 and the parts that are logic.
    g.W32(g.U32(kGameStatePtr) + 0x34u, 4);
    ChooserTableInit(g);                   // 0x80063720 (called from 0x80080544)
    PadConfig(g, kPadLive, 0);             // SLUS 0x8001C4A8 twice
    PadConfig(g, kPadLive + 192u, 0);
    g.W32(0x8009C2F0u, 0);
    g.W32(0x8009C2F4u, 0);
    FrontendInit(g, seed);                 // 0x800665E8 (with NavInit, HandlerTableInit)
    ScreenTableInit(g);                    // 0x800806D0
    GotoScreen(g, 0);                      // 0x800809E0
    const bool ok = BootSelections(g, k);  // 0x80080C04
    if (g.Faulted()) {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "the boot faulted at 0x%08X", g.FaultAddress());
        throw std::runtime_error(buf);
    }
    if (report != nullptr) {
        *report += "shell arena: SLUS_010.53 (" + std::to_string(exe.size()) + " bytes) and RASHCDF.BIN (" +
                   std::to_string(shell.size()) + " bytes) loaded; ported boot run";
        *report += ok ? "\n" : " - a seam refused\n";
    }
}

size_t CompareArenaWithCapture(const ShellArena& arena, const std::vector<uint8_t>& cap, std::string& report) {
    struct Region {
        const char* what;
        uint32_t address, bytes;
    };
    // The tables the boot writes and nothing writes afterwards.
    const Region regions[] = {
        {"screen table pointer 0x8009C68C", kScreenTablePtr, 4},
        {"screen table 0x800A0880 (59)", kScreenTable, 4u * kScreenCount},
        {"advance / back tables 0x8009C6C8 (2 x 59)", kNavAdvance, 8u * kScreenCount},
        {"slider commit 0x8009C8A8 (6)", kSliderCommit, 24},
        {"screen input handlers 0x8009C8C0 (59)", kScreenInput, 4u * kScreenCount},
        {"action code tables 0x8009C9B0 / 0x8009CAF8 (2 x 81)", kCodeGroup, 8u * 81u},
        {"object handlers 0x8009CC40 (37)", kObjectInput, 4u * 37u},
        {"chooser table 0x8009C4B0 (37)", kChoosers, 4u * 37u},
        {"widget update/draw tables 0x8009CF28 (2 x 20)", 0x8009CF28u, 160},
        {"screen transition/tick tables 0x8009CFD0 / 0x8009D0C0 (59)", 0x8009CFD0u, 4u * kScreenCount},
        {"", 0x8009D0C0u, 4u * kScreenCount},
        {"game_state pointer 0x8005B2F8", kGameStatePtr, 4},
        {"the overlay's code 0x8005B5E8..0x80081000", 0x8005B5E8u, 0x80081000u - 0x8005B5E8u},
        // The text resources (LoadTextResources): the four files as relocated, the string table
        // pointer, the three font slots' fields the port writes, the three handles.
        {"FESTRING.LOC + the three .PFN, relocated (0x800A0B08..)", 0x800A0B08u, 0x800BBA64u - 0x800A0B08u},
        {"string table pointer 0x8005B544", 0x8005B544u, 4},
        {"font slot 1 +0x00..+0x0F", 0x800D8090u, 16},
        {"font slot 1 +0x14..+0x17", 0x800D80A4u, 4},
        {"font slot 2 +0x00..+0x0F", 0x800D80A8u, 16},
        {"font slot 2 +0x14..+0x17", 0x800D80BCu, 4},
        {"font slot 3 +0x00..+0x0F", 0x800D80C0u, 16},
        {"font slot 3 +0x14..+0x17", 0x800D80D4u, 4},
        {"font handles 0x8009C5B8..0x8009C5C3", 0x8009C5B8u, 12},
    };
    // Not compared: the word the VRAM upload 0x80066188 rewrites in each font's sheet header (+0x0C,
    // its VRAM x / y) - the renderer's placement, not ported.
    std::vector<uint32_t> skip;
    for (uint32_t font : {0x800B6798u, 0x800B7FC4u, 0x800B9894u}) {
        const auto word = [&](uint32_t a) {
            const uint32_t o = a & 0x1FFFFFu;
            return static_cast<uint32_t>(arena.ram[o]) | (static_cast<uint32_t>(arena.ram[o + 1]) << 8) |
                   (static_cast<uint32_t>(arena.ram[o + 2]) << 16) | (static_cast<uint32_t>(arena.ram[o + 3]) << 24);
        };
        const uint32_t at = font + word(font + 0x1Cu) + 0x0Cu;
        for (uint32_t b = 0; b < 4u; ++b) skip.push_back(at + b);
    }
    size_t total = 0;
    char line[256];
    for (const Region& r : regions) {
        size_t diff = 0;
        uint32_t first = 0;
        for (uint32_t i = 0; i < r.bytes; ++i) {
            const uint32_t o = (r.address + i) & 0x1FFFFFu;
            if (o >= cap.size()) break;
            if (std::find(skip.begin(), skip.end(), r.address + i) != skip.end()) continue;
            if (arena.ram[o] != cap[o]) {
                if (diff == 0) first = r.address + i;
                ++diff;
            }
        }
        total += diff;
        std::snprintf(line, sizeof(line), "  %-62s %6u bytes  %s", r.what[0] ? r.what : "  (the second)", r.bytes,
                      diff == 0 ? "EQUAL" : "DIFFER");
        report += line;
        if (diff != 0) {
            std::snprintf(line, sizeof(line), " - %zu byte(s), first at 0x%08X", diff, first);
            report += line;
        }
        report += "\n";
    }
    return total;
}

// ---------------------------------------------------------------------------- the four initialisers
// NavInit: the net effect of RASHCDF 0x80068E88, 548 store(s) evaluated from the image.
void NavInit(GuestRam& g) {
    g.W32(0x8009C6C8u, 0x00000001u);
    g.W32(0x8009C6CCu, 0x00000003u);
    g.W32(0x8009C6D0u, 0x00000004u);
    g.W32(0x8009C6D4u, 0x00000002u);
    g.W32(0x8009C6D8u, 0xFFFFFFFFu);
    g.W32(0x8009C6DCu, 0xFFFFFFFFu);
    g.W32(0x8009C6E0u, 0x00000007u);
    g.W32(0x8009C6E4u, 0xFFFFFFFFu);
    g.W32(0x8009C6E8u, 0xFFFFFFFFu);
    g.W32(0x8009C6ECu, 0x00000008u);
    g.W32(0x8009C6F0u, 0x00000008u);
    g.W32(0x8009C6F4u, 0x0000000Au);
    g.W32(0x8009C6F8u, 0x0000000Du);
    g.W32(0x8009C6FCu, 0x00000008u);
    g.W32(0x8009C700u, 0x00000008u);
    g.W32(0x8009C704u, 0x0000000Au);
    g.W32(0x8009C708u, 0x00000011u);
    g.W32(0x8009C70Cu, 0x00000008u);
    g.W32(0x8009C710u, 0x00000008u);
    g.W32(0x8009C714u, 0x0000000Au);
    g.W32(0x8009C718u, 0x00000015u);
    g.W32(0x8009C71Cu, 0x00000016u);
    g.W32(0x8009C720u, 0x00000004u);
    g.W32(0x8009C724u, 0x00000008u);
    g.W32(0x8009C728u, 0xFFFFFFFFu);
    g.W32(0x8009C72Cu, 0x00000018u);
    g.W32(0x8009C730u, 0x00000018u);
    g.W32(0x8009C734u, 0xFFFFFFFFu);
    g.W32(0x8009C738u, 0x0000001Bu);
    for (uint32_t i = 0; i < 8u; ++i) g.W32(0x8009C73Cu + 4u * i, 0xFFFFFFFFu);
    g.W32(0x8009C75Cu, 0x00000024u);
    g.W32(0x8009C760u, 0xFFFFFFFFu);
    g.W32(0x8009C764u, 0x00000026u);
    g.W32(0x8009C768u, 0xFFFFFFFFu);
    g.W32(0x8009C76Cu, 0x00000028u);
    for (uint32_t i = 0; i < 5u; ++i) g.W32(0x8009C770u + 4u * i, 0xFFFFFFFFu);
    for (uint32_t i = 0; i < 5u; ++i) g.W32(0x8009C784u + 4u * i, 0x0000002Au);
    for (uint32_t i = 0; i < 4u; ++i) g.W32(0x8009C798u + 4u * i, 0xFFFFFFFFu);
    g.W32(0x8009C7A8u, 0x0000002Au);
    g.W32(0x8009C7ACu, 0xFFFFFFFFu);
    g.W32(0x8009C7B0u, 0x00000004u);
    for (uint32_t i = 0; i < 5u; ++i) g.W32(0x8009C7B8u + 4u * i, 0xFFFFFFFFu);
    g.W32(0x8009C7CCu, 0x00000004u);
    g.W32(0x8009C7D0u, 0x00000005u);
    g.W32(0x8009C7D4u, 0x00000005u);
    g.W32(0x8009C7D8u, 0x00000005u);
    g.W32(0x8009C7DCu, 0x00000008u);
    for (uint32_t i = 0; i < 14u; ++i) g.W32(0x8009C7E0u + 4u * i, 0xFFFFFFFFu);
    g.W32(0x8009C818u, 0x00000005u);
    g.W32(0x8009C81Cu, 0x00000018u);
    g.W32(0x8009C820u, 0x00000018u);
    g.W32(0x8009C824u, 0x00000005u);
    g.W32(0x8009C828u, 0xFFFFFFFFu);
    g.W32(0x8009C82Cu, 0x00000004u);
    g.W32(0x8009C830u, 0x0000001Du);
    g.W32(0x8009C834u, 0x0000001Eu);
    g.W32(0x8009C838u, 0x0000001Du);
    g.W32(0x8009C83Cu, 0x00000020u);
    g.W32(0x8009C840u, 0x0000001Du);
    g.W32(0x8009C844u, 0x00000022u);
    g.W32(0x8009C848u, 0x0000001Du);
    g.W32(0x8009C84Cu, 0xFFFFFFFFu);
    g.W32(0x8009C850u, 0x0000001Du);
    g.W32(0x8009C854u, 0x00000026u);
    g.W32(0x8009C858u, 0x00000026u);
    g.W32(0x8009C85Cu, 0x00000028u);
    g.W32(0x8009C860u, 0x00000004u);
    for (uint32_t i = 0; i < 9u; ++i) g.W32(0x8009C864u + 4u * i, 0x0000002Au);
    for (uint32_t i = 0; i < 4u; ++i) g.W32(0x8009C888u + 4u * i, 0xFFFFFFFFu);
    g.W32(0x8009C898u, 0x0000002Au);
    g.W32(0x8009C89Cu, 0xFFFFFFFFu);
    g.W32(0x8009C8A0u, 0x00000004u);
    for (uint32_t i = 0; i < 6u; ++i) g.W32(0x8009C8A8u + 4u * i, 0xFFFFFFFFu);
    g.W32(0x8009C8C0u, 0x8006AAF8u);
    g.W32(0x8009C8C4u, 0x8006A8FCu);
    g.W32(0x8009C8C8u, 0x8006A8FCu);
    g.W32(0x8009C8CCu, 0x8006AC80u);
    g.W32(0x8009C8D0u, 0x80069418u);
    g.W32(0x8009C8D4u, 0x80069418u);
    g.W32(0x8009C8D8u, 0x8006A8FCu);
    for (uint32_t i = 0; i < 4u; ++i) g.W32(0x8009C8DCu + 4u * i, 0x80069418u);
    g.W32(0x8009C8ECu, 0x8006A8FCu);
    g.W32(0x8009C8F0u, 0x8006A8FCu);
    g.W32(0x8009C8F4u, 0x80069418u);
    g.W32(0x8009C8F8u, 0x80069418u);
    g.W32(0x8009C8FCu, 0x8006A8FCu);
    g.W32(0x8009C900u, 0x8006A8FCu);
    g.W32(0x8009C904u, 0x80069418u);
    g.W32(0x8009C908u, 0x80069418u);
    g.W32(0x8009C90Cu, 0x8006A8FCu);
    g.W32(0x8009C910u, 0x8006A8FCu);
    g.W32(0x8009C914u, 0x8006A838u);
    g.W32(0x8009C918u, 0x8006A8FCu);
    g.W32(0x8009C91Cu, 0x80069418u);
    g.W32(0x8009C920u, 0x80069418u);
    g.W32(0x8009C924u, 0x80069440u);
    g.W32(0x8009C928u, 0x80069560u);
    for (uint32_t i = 0; i < 12u; ++i) g.W32(0x8009C92Cu + 4u * i, 0x80069418u);
    g.W32(0x8009C95Cu, 0x80069440u);
    g.W32(0x8009C960u, 0x80069418u);
    g.W32(0x8009C964u, 0x80069418u);
    g.W32(0x8009C968u, 0x8006AE6Cu);
    g.W32(0x8009C96Cu, 0x8006C06Cu);
    g.W32(0x8009C970u, 0x8006C124u);
    g.W32(0x8009C974u, 0x8006C1DCu);
    g.W32(0x8009C978u, 0x8006C290u);
    g.W32(0x8009C97Cu, 0x8006A610u);
    g.W32(0x8009C980u, 0x80069FF0u);
    g.W32(0x8009C984u, 0x8006A390u);
    g.W32(0x8009C988u, 0x8006A838u);
    g.W32(0x8009C98Cu, 0x8006A518u);
    g.W32(0x8009C990u, 0x80069A8Cu);
    g.W32(0x8009C994u, 0x8006AD20u);
    g.W32(0x8009C998u, 0x8006AB58u);
    g.W32(0x8009C99Cu, 0x8006AB58u);
    g.W32(0x8009C9A0u, 0x80069618u);
    g.W32(0x8009C9A4u, 0x8006C354u);
    g.W32(0x8009C9A8u, 0x8006A8FCu);
    for (uint32_t i = 0; i < 9u; ++i) g.W32(0x8009C9B0u + 4u * i, 0xFFFFFFFFu);
    g.W32(0x8009C9D4u, 0x00000008u);
    g.W32(0x8009C9D8u, 0x00000008u);
    g.W32(0x8009C9DCu, 0xFFFFFFFFu);
    g.W32(0x8009C9E0u, 0x0000000Bu);
    g.W32(0x8009C9E4u, 0x0000000Bu);
    g.W32(0x8009C9E8u, 0xFFFFFFFFu);
    g.W32(0x8009C9ECu, 0xFFFFFFFFu);
    g.W32(0x8009C9F0u, 0x0000000Fu);
    g.W32(0x8009C9F4u, 0x0000000Fu);
    g.W32(0x8009C9F8u, 0x0000000Fu);
    for (uint32_t i = 0; i < 7u; ++i) g.W32(0x8009C9FCu + 4u * i, 0xFFFFFFFFu);
    g.W32(0x8009CA18u, 0x00000019u);
    for (uint32_t i = 0; i < 6u; ++i) g.W32(0x8009CA1Cu + 4u * i, 0xFFFFFFFFu);
    g.W32(0x8009CA34u, 0x00000020u);
    g.W32(0x8009CA38u, 0x00000020u);
    g.W32(0x8009CA3Cu, 0x00000020u);
    g.W32(0x8009CA40u, 0xFFFFFFFFu);
    g.W32(0x8009CA44u, 0x00000024u);
    g.W32(0x8009CA48u, 0xFFFFFFFFu);
    g.W32(0x8009CA4Cu, 0x00000026u);
    g.W32(0x8009CA50u, 0x00000026u);
    g.W32(0x8009CA54u, 0x00000026u);
    g.W32(0x8009CA58u, 0xFFFFFFFFu);
    g.W32(0x8009CA5Cu, 0x0000002Au);
    g.W32(0x8009CA60u, 0xFFFFFFFFu);
    g.W32(0x8009CA64u, 0x0000002Cu);
    g.W32(0x8009CA68u, 0x0000002Cu);
    g.W32(0x8009CA6Cu, 0xFFFFFFFFu);
    for (uint32_t i = 0; i < 4u; ++i) g.W32(0x8009CA70u + 4u * i, 0x0000002Fu);
    g.W32(0x8009CA80u, 0xFFFFFFFFu);
    g.W32(0x8009CA84u, 0x00000034u);
    g.W32(0x8009CA88u, 0x00000034u);
    g.W32(0x8009CA8Cu, 0xFFFFFFFFu);
    g.W32(0x8009CA90u, 0x00000037u);
    g.W32(0x8009CA94u, 0x00000037u);
    g.W32(0x8009CA98u, 0x00000037u);
    g.W32(0x8009CA9Cu, 0xFFFFFFFFu);
    g.W32(0x8009CAA0u, 0xFFFFFFFFu);
    g.W32(0x8009CAA4u, 0xFFFFFFFFu);
    g.W32(0x8009CAA8u, 0x0000003Du);
    for (uint32_t i = 0; i < 18u; ++i) g.W32(0x8009CAACu + 4u * i, 0xFFFFFFFFu);
    g.W32(0x8009CAF8u, 0xFFFFFFFFu);
    g.W32(0x8009CAFCu, 0xFFFFFFFFu);
    g.W32(0x8009CB00u, 0x00000005u);
    g.W32(0x8009CB04u, 0x0000001Du);
    g.W32(0x8009CB08u, 0x0000002Au);
    g.W32(0x8009CB0Cu, 0x00000006u);
    g.W32(0x8009CB10u, 0x0000001Bu);
    g.W32(0x8009CB14u, 0x00000018u);
    g.W32(0x8009CB18u, 0x00000008u);
    g.W32(0x8009CB1Cu, 0xFFFFFFFFu);
    g.W32(0x8009CB20u, 0xFFFFFFFFu);
    g.W32(0x8009CB24u, 0x00000039u);
    g.W32(0x8009CB28u, 0xFFFFFFFFu);
    g.W32(0x8009CB2Cu, 0xFFFFFFFFu);
    g.W32(0x8009CB30u, 0x00000009u);
    g.W32(0x8009CB34u, 0x00000039u);
    g.W32(0x8009CB38u, 0xFFFFFFFFu);
    g.W32(0x8009CB3Cu, 0xFFFFFFFFu);
    g.W32(0x8009CB40u, 0xFFFFFFFFu);
    g.W32(0x8009CB44u, 0x0000001Au);
    g.W32(0x8009CB48u, 0x00000019u);
    for (uint32_t i = 0; i < 4u; ++i) g.W32(0x8009CB4Cu + 4u * i, 0xFFFFFFFFu);
    g.W32(0x8009CB5Cu, 0x00000039u);
    g.W32(0x8009CB60u, 0xFFFFFFFFu);
    g.W32(0x8009CB64u, 0x0000001Eu);
    g.W32(0x8009CB68u, 0x00000020u);
    g.W32(0x8009CB6Cu, 0x00000024u);
    g.W32(0x8009CB70u, 0x00000022u);
    g.W32(0x8009CB74u, 0x00000026u);
    g.W32(0x8009CB78u, 0x0000001Fu);
    g.W32(0x8009CB7Cu, 0xFFFFFFFFu);
    g.W32(0x8009CB80u, 0xFFFFFFFFu);
    g.W32(0x8009CB84u, 0xFFFFFFFFu);
    g.W32(0x8009CB88u, 0x00000039u);
    g.W32(0x8009CB8Cu, 0xFFFFFFFFu);
    g.W32(0x8009CB90u, 0x00000021u);
    g.W32(0x8009CB94u, 0xFFFFFFFFu);
    g.W32(0x8009CB98u, 0xFFFFFFFFu);
    g.W32(0x8009CB9Cu, 0xFFFFFFFFu);
    g.W32(0x8009CBA0u, 0x00000039u);
    g.W32(0x8009CBA4u, 0xFFFFFFFFu);
    g.W32(0x8009CBA8u, 0x00000039u);
    g.W32(0x8009CBACu, 0xFFFFFFFFu);
    g.W32(0x8009CBB0u, 0xFFFFFFFFu);
    g.W32(0x8009CBB4u, 0x00000023u);
    for (uint32_t i = 0; i < 4u; ++i) g.W32(0x8009CBB8u + 4u * i, 0xFFFFFFFFu);
    g.W32(0x8009CBC8u, 0x00000039u);
    g.W32(0x8009CBCCu, 0xFFFFFFFFu);
    g.W32(0x8009CBD0u, 0xFFFFFFFFu);
    g.W32(0x8009CBD4u, 0x00000028u);
    g.W32(0x8009CBD8u, 0xFFFFFFFFu);
    g.W32(0x8009CBDCu, 0xFFFFFFFFu);
    g.W32(0x8009CBE0u, 0xFFFFFFFFu);
    g.W32(0x8009CBE4u, 0x0000001Au);
    g.W32(0x8009CBE8u, 0x00000027u);
    g.W32(0x8009CBECu, 0x00000039u);
    g.W32(0x8009CBF0u, 0xFFFFFFFFu);
    g.W32(0x8009CBF4u, 0x0000002Bu);
    g.W32(0x8009CBF8u, 0x0000002Cu);
    g.W32(0x8009CBFCu, 0x0000002Du);
    g.W32(0x8009CC00u, 0x0000002Eu);
    g.W32(0x8009CC04u, 0x00000031u);
    g.W32(0x8009CC08u, 0x00000030u);
    g.W32(0x8009CC0Cu, 0x0000002Fu);
    g.W32(0x8009CC10u, 0x00000032u);
    g.W32(0x8009CC14u, 0x00000033u);
    for (uint32_t i = 0; i < 9u; ++i) g.W32(0x8009CC18u + 4u * i, 0xFFFFFFFFu);
    for (uint32_t i = 0; i < 31u; ++i) g.W32(0x8009CC40u + 4u * i, 0x8006B7BCu);
    g.W32(0x8009CCBCu, 0x8006BE84u);
    for (uint32_t i = 0; i < 5u; ++i) g.W32(0x8009CCC0u + 4u * i, 0x8006B7BCu);
}
// ScreenTableInit: the net effect of RASHCDF 0x800806D0, 60 store(s) evaluated from the image.
void ScreenTableInit(GuestRam& g) {
    g.W32(0x8009C68Cu, 0x800A0880u);
    g.W32(0x800A0880u, 0x80099064u);
    g.W32(0x800A0884u, 0x80099078u);
    g.W32(0x800A0888u, 0x8009908Cu);
    g.W32(0x800A088Cu, 0x800990A0u);
    g.W32(0x800A0890u, 0x800990B4u);
    g.W32(0x800A0894u, 0x800990C8u);
    g.W32(0x800A0898u, 0x800992F8u);
    g.W32(0x800A089Cu, 0x800990DCu);
    g.W32(0x800A08A0u, 0x800990F0u);
    g.W32(0x800A08A4u, 0x80099104u);
    g.W32(0x800A08A8u, 0x8009926Cu);
    g.W32(0x800A08ACu, 0x80099320u);
    g.W32(0x800A08B0u, 0x8009930Cu);
    g.W32(0x800A08B4u, 0x80099280u);
    g.W32(0x800A08B8u, 0x80099294u);
    g.W32(0x800A08BCu, 0x80099348u);
    g.W32(0x800A08C0u, 0x80099334u);
    g.W32(0x800A08C4u, 0x800992A8u);
    g.W32(0x800A08C8u, 0x800992BCu);
    g.W32(0x800A08CCu, 0x80099398u);
    g.W32(0x800A08D0u, 0x8009935Cu);
    g.W32(0x800A08D4u, 0x80099370u);
    g.W32(0x800A08D8u, 0x80099384u);
    g.W32(0x800A08DCu, 0x800992D0u);
    g.W32(0x800A08E0u, 0x80099118u);
    g.W32(0x800A08E4u, 0x8009912Cu);
    g.W32(0x800A08E8u, 0x80099460u);
    g.W32(0x800A08ECu, 0x80099154u);
    g.W32(0x800A08F0u, 0x800993ACu);
    g.W32(0x800A08F4u, 0x80099168u);
    g.W32(0x800A08F8u, 0x8009917Cu);
    g.W32(0x800A08FCu, 0x80099190u);
    g.W32(0x800A0900u, 0x800991A4u);
    g.W32(0x800A0904u, 0x800991B8u);
    g.W32(0x800A0908u, 0x800991E0u);
    g.W32(0x800A090Cu, 0x800991F4u);
    g.W32(0x800A0910u, 0x800991CCu);
    g.W32(0x800A0914u, 0x800993C0u);
    g.W32(0x800A0918u, 0x80099208u);
    g.W32(0x800A091Cu, 0x80099140u);
    g.W32(0x800A0920u, 0x8009921Cu);
    g.W32(0x800A0924u, 0x80099474u);
    g.W32(0x800A0928u, 0x800993D4u);
    g.W32(0x800A092Cu, 0x800993FCu);
    g.W32(0x800A0930u, 0x80099410u);
    g.W32(0x800A0934u, 0x80099424u);
    g.W32(0x800A0938u, 0x80099438u);
    g.W32(0x800A093Cu, 0x80099244u);
    g.W32(0x800A0940u, 0x80099258u);
    g.W32(0x800A0944u, 0x8009944Cu);
    g.W32(0x800A0948u, 0x80099230u);
    g.W32(0x800A094Cu, 0x800993E8u);
    g.W32(0x800A0950u, 0x800994C4u);
    g.W32(0x800A0954u, 0x80099488u);
    g.W32(0x800A0958u, 0x8009949Cu);
    g.W32(0x800A095Cu, 0x800994B0u);
    g.W32(0x800A0960u, 0x800994D8u);
    g.W32(0x800A0964u, 0x800994ECu);
    g.W32(0x800A0968u, 0x800992E4u);
}
// ChooserTableInit: the net effect of RASHCDF 0x80063720, 74 store(s) evaluated from the image.
void ChooserTableInit(GuestRam& g) {
    g.W32(0x8009C4B0u, 0x800812D0u);
    g.W32(0x8009C4B4u, 0x80081794u);
    g.W32(0x8009C4B8u, 0x800817C0u);
    g.W32(0x8009C4BCu, 0x80086064u);
    g.W32(0x8009C4C0u, 0x80086C90u);
    g.W32(0x8009C4C4u, 0x80086C98u);
    g.W32(0x8009C4C8u, 0x80087098u);
    g.W32(0x8009C4CCu, 0x80086D30u);
    g.W32(0x8009C4D0u, 0x80086FF8u);
    g.W32(0x8009C4D4u, 0x80086D80u);
    g.W32(0x8009C4D8u, 0x8008706Cu);
    g.W32(0x8009C4DCu, 0x800884B0u);
    g.W32(0x8009C4E0u, 0x800884B8u);
    g.W32(0x8009C4E4u, 0x800884C0u);
    g.W32(0x8009C4E8u, 0x80087F40u);
    g.W32(0x8009C4ECu, 0x80088020u);
    g.W32(0x8009C4F0u, 0x800880B8u);
    g.W32(0x8009C4F4u, 0x800880C0u);
    g.W32(0x8009C4F8u, 0x800881A0u);
    g.W32(0x8009C4FCu, 0x80088310u);
    g.W32(0x8009C500u, 0x80088320u);
    g.W32(0x8009C504u, 0x80088318u);
    g.W32(0x8009C508u, 0x80088460u);
    g.W32(0x8009C50Cu, 0x80088370u);
    g.W32(0x8009C510u, 0x80088378u);
    g.W32(0x8009C514u, 0x80088380u);
    g.W32(0x8009C518u, 0x80088480u);
    g.W32(0x8009C51Cu, 0x80088488u);
    g.W32(0x8009C520u, 0x800884F8u);
    g.W32(0x8009C524u, 0x800884F0u);
    g.W32(0x8009C528u, 0x800884E8u);
    g.W32(0x8009C52Cu, 0x80088BC8u);
    g.W32(0x8009C530u, 0x80088500u);
    g.W32(0x8009C534u, 0x80086E28u);
    g.W32(0x8009C538u, 0x80086F84u);
    g.W32(0x8009C53Cu, 0x800884A8u);
    g.W32(0x8009C540u, 0x800884C8u);
}
// HandlerTableInit: the net effect of RASHCDF 0x8006D174, 234 store(s) evaluated from the image.
void HandlerTableInit(GuestRam& g) {
    g.W32(0x8009CF28u, 0x00000000u);
    g.W32(0x8009CF2Cu, 0x8006E400u);
    for (uint32_t i = 0; i < 5u; ++i) g.W32(0x8009CF30u + 4u * i, 0x8006E4D8u);
    g.W32(0x8009CF44u, 0x8006E894u);
    g.W32(0x8009CF48u, 0x8006E8FCu);
    g.W32(0x8009CF4Cu, 0x8006E7A4u);
    g.W32(0x8009CF50u, 0x8006EDE4u);
    g.W32(0x8009CF54u, 0x8006ECA0u);
    g.W32(0x8009CF58u, 0x8006F764u);
    g.W32(0x8009CF5Cu, 0x8006EF30u);
    g.W32(0x8009CF60u, 0x8006F9A4u);
    g.W32(0x8009CF64u, 0x8006FA38u);
    g.W32(0x8009CF68u, 0x8006FAC8u);
    g.W32(0x8009CF6Cu, 0x8006F4A0u);
    g.W32(0x8009CF70u, 0x8006F0B8u);
    g.W32(0x8009CF74u, 0x80070580u);
    g.W32(0x8009CF78u, 0x00000000u);
    g.W32(0x8009CF7Cu, 0x8006E3BCu);
    for (uint32_t i = 0; i < 5u; ++i) g.W32(0x8009CF80u + 4u * i, 0x8006E4D8u);
    g.W32(0x8009CF94u, 0x8006E894u);
    g.W32(0x8009CF98u, 0x8006E8FCu);
    g.W32(0x8009CF9Cu, 0x8006E7A4u);
    g.W32(0x8009CFA0u, 0x8006EDE4u);
    g.W32(0x8009CFA4u, 0x8006ECA0u);
    g.W32(0x8009CFA8u, 0x8006F764u);
    g.W32(0x8009CFACu, 0x8006EF30u);
    g.W32(0x8009CFB0u, 0x8006F9A4u);
    g.W32(0x8009CFB4u, 0x8006FA38u);
    g.W32(0x8009CFB8u, 0x8006FAC8u);
    g.W32(0x8009CFBCu, 0x8006F4A0u);
    g.W32(0x8009CFC0u, 0x8006F0B8u);
    g.W32(0x8009CFC4u, 0x80070580u);
    g.W32(0x8009CFD0u, 0x8006DE5Cu);
    g.W32(0x8009CFD4u, 0x8006DE5Cu);
    g.W32(0x8009CFD8u, 0x8006DE5Cu);
    g.W32(0x8009CFDCu, 0x8006D630u);
    g.W32(0x8009CFE0u, 0x8006D630u);
    g.W32(0x8009CFE4u, 0x8006D630u);
    g.W32(0x8009CFE8u, 0x8006DE5Cu);
    for (uint32_t i = 0; i < 4u; ++i) g.W32(0x8009CFECu + 4u * i, 0x8006D630u);
    g.W32(0x8009CFFCu, 0x8006DE5Cu);
    g.W32(0x8009D000u, 0x8006DE5Cu);
    g.W32(0x8009D004u, 0x8006D630u);
    g.W32(0x8009D008u, 0x8006D630u);
    g.W32(0x8009D00Cu, 0x8006DE5Cu);
    g.W32(0x8009D010u, 0x8006DE5Cu);
    g.W32(0x8009D014u, 0x8006D630u);
    g.W32(0x8009D018u, 0x8006D630u);
    g.W32(0x8009D01Cu, 0x8006DE5Cu);
    g.W32(0x8009D020u, 0x8006DE5Cu);
    g.W32(0x8009D024u, 0x8006D630u);
    g.W32(0x8009D028u, 0x8006DE5Cu);
    g.W32(0x8009D02Cu, 0x8006D630u);
    g.W32(0x8009D030u, 0x8006D630u);
    g.W32(0x8009D034u, 0x8006D658u);
    for (uint32_t i = 0; i < 13u; ++i) g.W32(0x8009D038u + 4u * i, 0x8006D630u);
    g.W32(0x8009D06Cu, 0x8006D658u);
    for (uint32_t i = 0; i < 11u; ++i) g.W32(0x8009D070u + 4u * i, 0x8006D630u);
    g.W32(0x8009D09Cu, 0x8006D658u);
    g.W32(0x8009D0A0u, 0x8006D630u);
    g.W32(0x8009D0A4u, 0x8006D658u);
    g.W32(0x8009D0A8u, 0x8006D658u);
    g.W32(0x8009D0ACu, 0x8006D658u);
    g.W32(0x8009D0B0u, 0x8006D630u);
    g.W32(0x8009D0B4u, 0x8006E008u);
    g.W32(0x8009D0B8u, 0x8006DE5Cu);
    g.W32(0x8009D0C0u, 0x8006DB5Cu);
    g.W32(0x8009D0C4u, 0x8006DB5Cu);
    g.W32(0x8009D0C8u, 0x8006DB5Cu);
    g.W32(0x8009D0CCu, 0x8006D5B0u);
    g.W32(0x8009D0D0u, 0x8006D5B0u);
    g.W32(0x8009D0D4u, 0x8006D5B0u);
    g.W32(0x8009D0D8u, 0x8006DB5Cu);
    for (uint32_t i = 0; i < 4u; ++i) g.W32(0x8009D0DCu + 4u * i, 0x8006D5B0u);
    g.W32(0x8009D0ECu, 0x8006DB5Cu);
    g.W32(0x8009D0F0u, 0x8006DB5Cu);
    g.W32(0x8009D0F4u, 0x8006D5B0u);
    g.W32(0x8009D0F8u, 0x8006D5B0u);
    g.W32(0x8009D0FCu, 0x8006DB5Cu);
    g.W32(0x8009D100u, 0x8006DB5Cu);
    g.W32(0x8009D104u, 0x8006D5B0u);
    g.W32(0x8009D108u, 0x8006D5B0u);
    g.W32(0x8009D10Cu, 0x8006DB5Cu);
    g.W32(0x8009D110u, 0x8006DB5Cu);
    g.W32(0x8009D114u, 0x8006D5B0u);
    g.W32(0x8009D118u, 0x8006DB5Cu);
    g.W32(0x8009D11Cu, 0x8006D5B0u);
    g.W32(0x8009D120u, 0x8006D5B0u);
    g.W32(0x8009D124u, 0x8006D8B4u);
    for (uint32_t i = 0; i < 13u; ++i) g.W32(0x8009D128u + 4u * i, 0x8006D5B0u);
    g.W32(0x8009D15Cu, 0x8006D8B4u);
    for (uint32_t i = 0; i < 11u; ++i) g.W32(0x8009D160u + 4u * i, 0x8006D5B0u);
    g.W32(0x8009D18Cu, 0x8006D8B4u);
    g.W32(0x8009D190u, 0x8006D5B0u);
    g.W32(0x8009D194u, 0x8006D8B4u);
    g.W32(0x8009D198u, 0x8006D8B4u);
    g.W32(0x8009D19Cu, 0x8006D8B4u);
    g.W32(0x8009D1A0u, 0x8006D5B0u);
    g.W32(0x8009D1A4u, 0x8006E008u);
    g.W32(0x8009D1A8u, 0x8006DB5Cu);
}

} // namespace rr::shell
