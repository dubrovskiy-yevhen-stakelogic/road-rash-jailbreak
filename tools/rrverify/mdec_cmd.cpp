// rrverify mdec - the MDEC on real data.
//
// The ORIGINAL's code decodes, on the interpreter with its MDEC device (src\interp\devices.cpp, whose arithmetic is
// rr::mdec - the product's): the game's DctVlc SLUS 0x80020400 turns the bitstream into run-levels, the library's
// DecDCTReset body 0x8004DA68 uploads the EXE's tables through MDEC commands 2 / 3, DecDCTin 0x8004D90C and
// DecDCTout 0x8004D988 run the decode over DMA 0 / 1, DecDCToutSync 0x8004D9E4 waits for it. The PRODUCT's paths
// decode the same data:
//   * films (the menu panels, the loading screens, the cutscenes): rr::shell::DecodeMdecChunk - the product's own
//     VLC decoder (shell_assets.cpp) and rr::mdec; the original's side is a machine with only SLUS_010.53 loaded;
//   * the sky's column strips (the pairs the capture holds): the PORTED DctVlc (rr::sim) and rr::mdec over the
//     command word DecDCTin leaves, as sky_product.cpp runs them; the original's side is the capture itself.
// Every output word is compared. `--retro-shell <dir>` also compares RR_LOGO.STR's frames with the menu film the
// RetroArch capture shows at (230, 111) (that capture's own MDEC is its emulator's).
//
// `--device-model legacy|emuold|hardware` gives the interpreter's device another arithmetic than the product's: the
// negative control (the check must FAIL).
//
// usage: rrverify mdec --disc <bin> [--state <capture dir>]... [--frames <per file>] [--retro-shell <dir>]
//                      [--device-model <m>]
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "game/shell/shell_assets.h"
#include "game/sim/sky_draw.h"
#include "interp/devices.h"
#include "interp/r3000.h"
#include "interp/snapshot.h"
#include "rrformats/mdec.h"
#include "rrvfs/disc_image.h"

namespace {

using rr::interp::Cpu;
using rr::interp::Memory;
using rr::interp::Trap;
using rr::interp::TrapKind;

constexpr uint32_t kSentinel = 0x00000DEC;
constexpr uint32_t kGp = 0x8005AC8Cu;
constexpr uint32_t kDctVlc = 0x80020400u, kVlcBuild = 0x800202B8u, kDctResetBody = 0x8004DA68u, kDctIn = 0x8004D90Cu,
                   kDctOut = 0x8004D988u, kDctOutSync = 0x8004D9E4u;
// The film machine's buffers (nothing but SLUS_010.53 lives in its RAM).
constexpr uint32_t kTab0 = 0x80100000u, kTab1 = 0x80108000u, kSyms = 0x8010F000u, kBs = 0x80110000u, kRunLevels = 0x80130000u,
                   kOut = 0x80160000u, kStack = 0x801FFF00u;

rr::mdec::Model gDeviceModel = rr::mdec::Model::Hardware;

struct Guest {
    Memory mem;
    Cpu cpu;
    rr::interp::Devices dev;
    Guest() : cpu(mem), dev(mem) {
        cpu.devices = &dev;
        cpu.takeInterrupts = false;
        dev.mdecModel = gDeviceModel;
    }
    bool Call(uint32_t fn, std::array<uint32_t, 4> a, uint32_t& v0, std::string& why) {
        for (int i = 0; i < 4; ++i) cpu.regs[4 + i] = a[static_cast<size_t>(i)];
        cpu.regs[28] = kGp;
        cpu.regs[29] = kStack;
        cpu.regs[31] = kSentinel;
        cpu.pc = fn;
        cpu.npc = fn + 4u;
        cpu.loadDelayReg = Cpu::kNoLoadDelay;
        const Trap t = cpu.Run(kSentinel, 400'000'000);
        v0 = cpu.regs[2];
        if (t.kind != TrapKind::Halted) {
            char b[64];
            std::snprintf(b, sizeof(b), "0x%08X: ", fn);
            why = b + t.ToString();
            return false;
        }
        return true;
    }
    // DecDCTin(runLevels, mode), DecDCTout(out, words), DecDCToutSync(0): the original's decode.
    bool Decode(uint32_t runLevels, uint32_t mode, uint32_t out, uint32_t words, std::string& why) {
        uint32_t v0 = 0;
        if (!Call(kDctIn, {runLevels, mode, 0, 0}, v0, why) || !Call(kDctOut, {out, words, 0, 0}, v0, why) ||
            !Call(kDctOutSync, {0, 0, 0, 0}, v0, why))
            return false;
        if (v0 != 0) {
            why = "DecDCToutSync timed out (the device never delivered the words)";
            return false;
        }
        return true;
    }
};

struct Tally {
    uint64_t words = 0, equalWords = 0, pixels = 0, equalPixels = 0, frames = 0, exactFrames = 0;
    void Add(uint64_t eq, uint64_t n) {
        ++frames;
        pixels += n;
        equalPixels += eq;
        if (eq == n) ++exactFrames;
    }
};

double Pct(uint64_t a, uint64_t b) { return b ? 100.0 * static_cast<double>(a) / static_cast<double>(b) : 0.0; }

std::vector<std::span<const uint8_t>> TcmChunks(std::span<const uint8_t> d) {
    std::vector<std::span<const uint8_t>> out;
    const auto u32 = [&](size_t o) { return static_cast<uint32_t>(d[o] | (d[o + 1] << 8) | (d[o + 2] << 16) | (static_cast<uint32_t>(d[o + 3]) << 24)); };
    const uint32_t n = u32(0);
    for (uint32_t i = 0; i < n; ++i) out.push_back(d.subspan(u32(4 + 8 * i + 4), u32(4 + 8 * i)));
    return out;
}

// One film chunk through both sides; the product's picture is returned for the capture comparison.
bool FilmFrame(Guest& g, std::span<const uint8_t> chunk, const rr::shell::MdecCodebook& book, Tally& t,
               rr::shell::Picture15& pic, std::string& why) {
    pic = rr::shell::DecodeMdecChunk(chunk, book);
    const int mbw = (pic.width + 15) / 16, mbh = (pic.height + 15) / 16;
    const uint32_t words = static_cast<uint32_t>(mbw * mbh) * 128u;
    g.mem.WriteBlock(kBs, chunk.data() + 0x10, chunk.size() - 0x10);
    uint32_t v0 = 0;
    if (!g.Call(kDctVlc, {kBs, kRunLevels, 0, 0}, v0, why)) return false;
    // DecDCTin's mode 2 (RASHCDF 0x80062AB4 loads it from 0x800810EC): 15-bit, STP set
    if (!g.Decode(kRunLevels, 2u, kOut, words, why)) return false;
    uint64_t eq = 0, n = 0;
    for (int k = 0; k < mbw * mbh; ++k) {
        const int mx = k / mbh, my = k % mbh; // column-major, as the films are laid out
        for (int i = 0; i < 256; ++i) {
            const int x = mx * 16 + (i & 15), y = my * 16 + (i >> 4);
            if (x >= pic.width || y >= pic.height) continue;
            const uint32_t w = g.mem.PeekWord(kOut + 4u * static_cast<uint32_t>((k * 256 + i) / 2));
            const uint16_t theirs = static_cast<uint16_t>((i & 1) ? (w >> 16) : (w & 0xFFFFu));
            ++n;
            if (theirs == pic.px[static_cast<size_t>(y) * static_cast<size_t>(pic.width) + static_cast<size_t>(x)]) ++eq;
        }
    }
    t.words += words;
    t.Add(eq, n);
    return true;
}

bool LoadVramShifted(const std::string& path, std::vector<uint16_t>& px) { // retro-shell: 281 bytes late
    std::vector<uint8_t> raw;
    std::string error;
    if (!rr::interp::ReadWholeFile(path, raw, error) || raw.size() != 1024u * 512u * 2u) return false;
    px.assign(1024u * 512u, 0);
    for (size_t k = 0; k < px.size(); ++k) {
        const size_t i = 2 * (k + 140u);
        px[k] = (i >= 1 && i < raw.size()) ? static_cast<uint16_t>(raw[i - 1] | (raw[i] << 8)) : 0;
    }
    return true;
}

} // namespace

int CmdMdec(int argc, char** argv) {
    std::string disc, retroShell;
    std::vector<std::string> states;
    int perFile = 8;
    gDeviceModel = rr::mdec::ActiveModel();
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--disc" && i + 1 < argc) disc = argv[++i];
        else if (a == "--state" && i + 1 < argc) states.push_back(argv[++i]);
        else if (a == "--frames" && i + 1 < argc) perFile = std::max(1, std::atoi(argv[++i]));
        else if (a == "--retro-shell" && i + 1 < argc) retroShell = argv[++i];
        else if (a == "--device-model" && i + 1 < argc) {
            const std::string m = argv[++i];
            gDeviceModel = m == "legacy" ? rr::mdec::Model::Legacy : m == "emuold" ? rr::mdec::Model::EmuOld : rr::mdec::Model::Hardware;
        }
        else {
            std::fprintf(stderr, "usage: rrverify mdec --disc <bin> [--state <capture>]... [--frames n] [--retro-shell <dir>]\n");
            return 2;
        }
    }
    if (disc.empty()) {
        std::fprintf(stderr, "mdec: --disc is required\n");
        return 2;
    }
    const rr::mdec::Model model = rr::mdec::ActiveModel();
    std::printf("mdec: the MDEC arithmetic: the product's %s, the interpreter's device %s%s\n", rr::mdec::ModelName(model),
                rr::mdec::ModelName(gDeviceModel), gDeviceModel != model ? " (NEGATIVE CONTROL: must FAIL)" : "");
    rr::DiscImage image(disc);
    const auto slusFile = image.Find("/SLUS_010.53");
    if (!slusFile) {
        std::fprintf(stderr, "mdec: SLUS_010.53 is not on the disc\n");
        return 1;
    }
    const std::vector<uint8_t> slus = image.ReadFile(*slusFile);
    const rr::shell::MdecCodebook exeBook = rr::shell::LoadDefaultCodebook(slus);
    bool ok = true;
    std::string why;

    // ------------------------------------------------------------------ the films
    Guest g;
    {
        const uint32_t tAddr = static_cast<uint32_t>(slus[0x18] | (slus[0x19] << 8) | (slus[0x1A] << 16) | (slus[0x1B] << 24));
        const uint32_t tSize = static_cast<uint32_t>(slus[0x1C] | (slus[0x1D] << 8) | (slus[0x1E] << 16) | (slus[0x1F] << 24));
        g.mem.WriteBlock(tAddr, slus.data() + 0x800, std::min<size_t>(tSize, slus.size() - 0x800));
        uint32_t v0 = 0;
        if (!g.Call(kDctResetBody, {0, 0, 0, 0}, v0, why)) {
            std::printf("mdec: the original's DecDCTReset failed: %s\n", why.c_str());
            return 1;
        }
    }
    const char* films[] = {"RR_LOGO.STR", "BIKES.STR", "BGRND3.STR", "TTL_CON2.STR", "COP1.STR", "MODES.STR",
                           "FSLOAD.TCM", "EA_LOGO.WVE", "INTRO.WVE", "JAILBRAK.WVE"};
    Tally all;
    for (const char* name : films) {
        const auto f = image.Find(std::string("/DATA/FE/") + name);
        if (!f) {
            std::printf("  %-13s not on the disc\n", name);
            ok = false;
            continue;
        }
        const std::vector<uint8_t> file = image.ReadFile(*f);
        const bool tcm = std::strstr(name, ".TCM") != nullptr;
        const auto chunks = tcm ? TcmChunks(file) : rr::shell::ListStrChunks(file);
        const rr::shell::MdecCodebook book = tcm ? exeBook : rr::shell::CodebookForStream(file, exeBook);
        uint32_t syms = 0;
        if (book.symbols != exeBook.symbols) { // a .WVE's VLC0 table: the game's VlcBuild with its symbols
            for (size_t k = 0; k < book.symbols.size(); ++k) g.mem.WriteBlock(kSyms + 2u * static_cast<uint32_t>(k), &book.symbols[k], 2);
            syms = kSyms;
        }
        uint32_t v0 = 0;
        if (!g.Call(kVlcBuild, {kTab0, kTab1, syms, 0}, v0, why)) {
            std::printf("  %-13s the original's VlcBuild failed: %s\n", name, why.c_str());
            ok = false;
            continue;
        }
        Tally t;
        const size_t step = std::max<size_t>(1, chunks.size() / static_cast<size_t>(perFile));
        for (size_t k = 0; k < chunks.size() && t.frames < static_cast<uint64_t>(perFile); k += step) {
            rr::shell::Picture15 pic;
            if (!FilmFrame(g, chunks[k], book, t, pic, why)) {
                std::printf("  %-13s frame %zu: %s\n", name, k, why.c_str());
                ok = false;
                break;
            }
        }
        std::printf("  %-13s %llu frames of %zu: %llu of %llu pixels equal (%.3f %%), %llu frames bit-exact, %llu MDEC words\n", name,
                    static_cast<unsigned long long>(t.frames), chunks.size(), static_cast<unsigned long long>(t.equalPixels),
                    static_cast<unsigned long long>(t.pixels), Pct(t.equalPixels, t.pixels),
                    static_cast<unsigned long long>(t.exactFrames), static_cast<unsigned long long>(t.words));
        all.frames += t.frames;
        all.exactFrames += t.exactFrames;
        all.pixels += t.pixels;
        all.equalPixels += t.equalPixels;
        all.words += t.words;
        ok = ok && t.frames != 0 && t.equalPixels == t.pixels;
    }
    std::printf("mdec: films, the product's decoder against the original's DctVlc + the MDEC device: %llu frames, %llu of %llu "
                "pixels equal (%.3f %%), %llu frames bit-exact; the device ran %llu commands, %llu macroblocks\n",
                static_cast<unsigned long long>(all.frames), static_cast<unsigned long long>(all.equalPixels),
                static_cast<unsigned long long>(all.pixels), Pct(all.equalPixels, all.pixels),
                static_cast<unsigned long long>(all.exactFrames), static_cast<unsigned long long>(g.dev.mdecCommands),
                static_cast<unsigned long long>(g.dev.mdecMacroblocks));

    // ------------------------------------------------------------------ the sky strips of the captures
    namespace s = rr::sim;
    for (const std::string& dir : states) {
        Guest c;
        rr::interp::SnapshotInfo info;
        if (!rr::interp::LoadSnapshot(dir, c.mem, c.cpu, info, why)) {
            std::printf("mdec: %s: %s\n", dir.c_str(), why.c_str());
            ok = false;
            continue;
        }
        const std::vector<uint8_t> ram0 = c.mem.ram(); // the product's side starts from the capture's RAM
        uint32_t v0 = 0;
        if (!c.Call(kDctResetBody, {0, 0, 0, 0}, v0, why)) {
            std::printf("mdec: %s: DecDCTReset: %s\n", dir.c_str(), why.c_str());
            ok = false;
            continue;
        }
        const uint32_t sky = c.mem.PeekWord(s::kSkyBlockPtr);
        const uint32_t mode = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(c.mem.PeekWord(sky + 20u) >> 16)));
        rr::mdec::Tables tables;
        for (uint32_t k = 0; k < 64; ++k) {
            tables.luma[k] = c.mem.PeekByte(0x8005A250u + k);
            tables.chroma[k] = c.mem.PeekByte(0x8005A290u + k);
            tables.idct[k] = static_cast<int16_t>(c.mem.PeekByte(0x8005A2D4u + 2u * k) | (c.mem.PeekByte(0x8005A2D5u + 2u * k) << 8));
        }
        uint64_t pairs = 0, exactPairs = 0, words = 0, equalWords = 0, rlWords = 0, rlEqual = 0;
        for (uint32_t pair = 0; pair < 55u && sky != 0; ++pair) {
            const uint32_t e = c.mem.PeekWord(sky + 1000u + 4u * pair);
            if (e == 0 || c.mem.PeekWord(e + 12u) == 0) continue;
            const uint32_t s1 = e + 8u;
            const uint32_t vlcOut = sky - (static_cast<uint32_t>(c.mem.PeekByte(s1 + 8u) | (c.mem.PeekByte(s1 + 9u) << 8)) * 4u - 9920u);
            const uint32_t out = sky + 1220u;
            const uint32_t n = (c.mem.PeekWord(s1 + 4u) * c.mem.PeekWord(s1)) / 2u;
            // the original
            if (!c.Call(kDctVlc, {s1 + 8u, vlcOut, 0, 0}, v0, why) || !c.Decode(vlcOut, mode, out, n, why)) {
                std::printf("mdec: %s pair %u: %s\n", dir.c_str(), pair, why.c_str());
                ok = false;
                break;
            }
            // the product: the PORTED DctVlc on the capture's RAM, DecDCTin's command bits, rr::mdec
            std::vector<uint8_t> ram = ram0;
            s::GuestRam pg(ram.data(), kGp);
            s::DctVlc(pg, s1 + 8u, vlcOut);
            uint32_t cmd = pg.U32(vlcOut);
            cmd = (mode & 1u) ? (cmd & 0xF7FFFFFFu) : (cmd | 0x08000000u);
            cmd = (mode & 2u) ? (cmd | 0x02000000u) : (cmd & 0xFDFFFFFFu);
            std::vector<uint16_t> hw((cmd & 0xFFFFu) * 2u);
            for (uint32_t k = 0; k < hw.size(); ++k) hw[k] = pg.U16(vlcOut + 4u + 2u * k);
            for (uint32_t k = 0; k < (cmd & 0xFFFFu); ++k) {
                ++rlWords;
                if (pg.U32(vlcOut + 4u + 4u * k) == c.mem.PeekWord(vlcOut + 4u + 4u * k)) ++rlEqual;
            }
            std::vector<uint32_t> mine(n, 0u);
            rr::mdec::Input in{hw.data(), hw.size(), 0};
            size_t produced = 0;
            rr::mdec::DecodeCommand(cmd, in, tables, model, mine.data(), mine.size(), produced);
            uint64_t eq = 0;
            for (uint32_t k = 0; k < n; ++k)
                if (mine[k] == c.mem.PeekWord(out + 4u * k)) ++eq;
            ++pairs;
            words += n;
            equalWords += eq;
            if (eq == n) ++exactPairs;
        }
        std::printf("mdec: %s sky strips, the product's (PORTED DctVlc + rr::mdec) against the original's (DctVlc + the MDEC "
                    "device): %llu column pairs, %llu bit-exact; run-level words %llu of %llu equal; output words %llu of %llu "
                    "equal (%.3f %%)\n",
                    dir.c_str(), static_cast<unsigned long long>(pairs), static_cast<unsigned long long>(exactPairs),
                    static_cast<unsigned long long>(rlEqual), static_cast<unsigned long long>(rlWords),
                    static_cast<unsigned long long>(equalWords), static_cast<unsigned long long>(words), Pct(equalWords, words));
        ok = ok && pairs != 0 && equalWords == words && rlEqual == rlWords;
    }

    // ------------------------------------------------------------------ the RetroArch capture's menu film
    if (!retroShell.empty()) {
        std::vector<uint16_t> vram;
        const auto f = image.Find("/DATA/FE/RR_LOGO.STR");
        if (!LoadVramShifted(retroShell + "/vram.bin", vram) || !f) {
            std::printf("mdec: cannot read %s/vram.bin\n", retroShell.c_str());
            return 1;
        }
        const std::vector<uint8_t> file = image.ReadFile(*f);
        const auto chunks = rr::shell::ListStrChunks(file);
        size_t best = 0, bestEq = 0, bestNear = 0, n = 0;
        for (size_t k = 0; k < chunks.size(); ++k) {
            const rr::shell::Picture15 p = rr::shell::DecodeMdecChunk(chunks[k], exeBook);
            size_t eq = 0, nr = 0;
            n = p.px.size();
            for (int y = 0; y < p.height; ++y)
                for (int x = 0; x < p.width; ++x) {
                    const uint16_t ours = p.px[static_cast<size_t>(y * p.width + x)] & 0x7FFFu;
                    const uint16_t theirs = vram[static_cast<size_t>(111 + y) * 1024u + static_cast<size_t>(230 + x)] & 0x7FFFu;
                    if (ours == theirs) ++eq;
                    bool near = true;
                    for (int c5 = 0; c5 < 15; c5 += 5) {
                        const int d = static_cast<int>((ours >> c5) & 31u) - static_cast<int>((theirs >> c5) & 31u);
                        near = near && d >= -1 && d <= 1;
                    }
                    if (near) ++nr;
                }
            if (eq > bestEq) {
                bestEq = eq;
                bestNear = nr;
                best = k;
            }
        }
        std::printf("mdec: the RetroArch capture's menu film at (230, 111): RR_LOGO.STR frame %zu, %zu of %zu pixels equal "
                    "(%.2f %%), within one step %zu (%.2f %%) - that capture's MDEC is its emulator's\n",
                    best, bestEq, n, Pct(bestEq, n), bestNear, Pct(bestNear, n));
    }
    std::printf("mdec verdict %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
