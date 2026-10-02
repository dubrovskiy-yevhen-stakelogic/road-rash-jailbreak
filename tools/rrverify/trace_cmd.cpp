// rrverify trace - record one real frame of the game inside the oracle interpreter.
//
// It RESUMES the captured mid-race machine state (work\oracle\state\rr-race) at the pc the console
// was stopped at, with the peripheral models of src\interp\devices.h attached, and records:
//
//   * every COP2 command with the full GTE register file before and after it,
//   * every word that reaches the GPU (CPU stores into GP0/GP1 and DMA2 linked-list words, each
//     tagged with the guest RAM address it came from),
//   * reads and writes inside named watch ranges,
//   * optionally every jal/jalr.
//
// Everything is off unless asked for, and the whole subcommand is a development facility.
#include <algorithm>
#include <array>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "interp/devices.h"
#include "interp/gte.h"
#include "interp/r3000.h"
#include "interp/snapshot.h"
#include "interp/trace.h"

namespace {

using rr::interp::Cop2Record;
using rr::interp::Cpu;
using rr::interp::Devices;
using rr::interp::GpuWord;
using rr::interp::GpuWordSource;
using rr::interp::MemEvent;
using rr::interp::Memory;
using rr::interp::Tracer;
using rr::interp::Trap;
using rr::interp::TrapKind;

std::string Join(const std::string& dir, const std::string& name) {
    std::string s = dir;
    if (!s.empty() && s.back() != '\\' && s.back() != '/') s.push_back('\\');
    return s + name;
}

bool WriteWholeFile(const std::string& path, const void* data, size_t size) {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (f == nullptr) return false;
    const size_t got = size == 0 ? 0 : std::fwrite(data, 1, size, f);
    std::fclose(f);
    return got == size;
}

uint32_t ParseNumber(const char* s) {
    return static_cast<uint32_t>(std::strtoull(s, nullptr, 0));
}

// The GTE command mnemonics, indexed by the low 6 bits of the command word.
const char* GteMnemonic(uint32_t instruction) {
    switch (instruction & 0x3Fu) {
    case 0x01: return "RTPS";
    case 0x06: return "NCLIP";
    case 0x0C: return "OP";
    case 0x10: return "DPCS";
    case 0x11: return "INTPL";
    case 0x12: return "MVMVA";
    case 0x13: return "NCDS";
    case 0x14: return "CDP";
    case 0x16: return "NCDT";
    case 0x1B: return "NCCS";
    case 0x1C: return "CC";
    case 0x1E: return "NCS";
    case 0x20: return "NCT";
    case 0x28: return "SQR";
    case 0x29: return "DCPL";
    case 0x2A: return "DPCT";
    case 0x2D: return "AVSZ3";
    case 0x2E: return "AVSZ4";
    case 0x30: return "RTPT";
    case 0x3D: return "GPF";
    case 0x3E: return "GPL";
    case 0x3F: return "NCCT";
    default: return "?";
    }
}

// ---------------------------------------------------------------- peripheral state from the .sav
//
// The savestate parser locates the DMA, InterruptController and Timers blocks by marker but does not
// parse them. Without them a resumed frame cannot run: I_MASK would be 0 so
// VBlank could never fire, DPCR would have every channel disabled, and timer 1 would not be in its
// hblank mode - and libetc reads timer 1 twice in a row and loops until the two reads agree, which
// a counter running at the CPU clock never does.
//
// The three blocks are parsed here, structurally: each one must be EXACTLY the size implied by the
// distance between its marker and the next, or the parse is refused. That is what makes the field
// assignment below a reading rather than a guess.
//
//   DMA   103 bytes = u32 head, then 7 x {u32 madr, u32 bcr, u32 chcr, u8 request}, then u32 DPCR,
//         u32 DICR. Confirmed by the result: channel 2 holds chcr 0x401 (linked list from RAM) with
//         madr 0xFFFFFF (a finished list), DPCR 0x3B3BBBBB enables exactly the six channels the
//         game uses, and DICR's master-enable bit is set with channels 1 and 4 armed.
//   InterruptController  8 bytes = u32 I_STAT, u32 I_MASK.
//   Timers 59 bytes = 3 x {u32 mode, u32 counter, u32 target, 5 bools}, then 8 trailing bytes.
//         Confirmed by the result: counter 1 selects the hblank clock and counter 2 selects
//         sysclk/8 with target 0xFFFF and its IRQ armed - the standard libetc configuration.
struct PeripheralState {
    bool haveIrq = false, haveDma = false, haveTimers = false;
    uint32_t istat = 0, imask = 0;
    uint32_t madr[7]{}, bcr[7]{}, chcr[7]{};
    uint32_t dpcr = 0, dicr = 0;
    uint32_t timerMode[3]{}, timerCounter[3]{}, timerTarget[3]{};
};

size_t FindMarker(const std::vector<uint8_t>& sav, const char* name) {
    const size_t n = std::strlen(name);
    std::vector<uint8_t> pattern(4 + n);
    const uint32_t len = static_cast<uint32_t>(n);
    std::memcpy(pattern.data(), &len, 4);
    std::memcpy(pattern.data() + 4, name, n);
    const auto it = std::search(sav.begin(), sav.end(), pattern.begin(), pattern.end());
    return (it == sav.end()) ? static_cast<size_t>(-1) : static_cast<size_t>(it - sav.begin());
}

uint32_t Le32(const std::vector<uint8_t>& b, size_t at) {
    uint32_t v = 0;
    std::memcpy(&v, b.data() + at, 4);
    return v;
}

bool ParsePeripheralState(const std::string& savPath, PeripheralState& out, std::string& error) {
    std::vector<uint8_t> sav;
    if (!rr::interp::ReadWholeFile(savPath, sav, error)) return false;

    const size_t dma = FindMarker(sav, "DMA");
    const size_t irq = FindMarker(sav, "InterruptController");
    const size_t gpu = FindMarker(sav, "GPU");
    const size_t timers = FindMarker(sav, "Timers");
    const size_t spu = FindMarker(sav, "SPU");
    if (dma == static_cast<size_t>(-1) || irq == static_cast<size_t>(-1) ||
        gpu == static_cast<size_t>(-1) || timers == static_cast<size_t>(-1) ||
        spu == static_cast<size_t>(-1)) {
        error = "the savestate does not carry all five markers (DMA, InterruptController, GPU, Timers, SPU)";
        return false;
    }

    const size_t dmaStart = dma + 4 + 3, dmaSize = irq - dmaStart;
    const size_t irqStart = irq + 4 + 19, irqSize = gpu - irqStart;
    const size_t timersStart = timers + 4 + 6, timersSize = spu - timersStart;
    if (dmaSize != 103 || irqSize != 8 || timersSize != 59) {
        char buf[192];
        std::snprintf(buf, sizeof(buf),
                      "unexpected block sizes (DMA %zu, InterruptController %zu, Timers %zu; "
                      "expected 103 / 8 / 59)", dmaSize, irqSize, timersSize);
        error = buf;
        return false;
    }

    out.istat = Le32(sav, irqStart);
    out.imask = Le32(sav, irqStart + 4);
    out.haveIrq = true;

    for (int c = 0; c < 7; ++c) {
        const size_t o = dmaStart + 4 + static_cast<size_t>(c) * 13;
        out.madr[c] = Le32(sav, o);
        out.bcr[c] = Le32(sav, o + 4);
        out.chcr[c] = Le32(sav, o + 8);
        if (sav[o + 12] > 1) { error = "DMA channel request flag is not a bool"; return false; }
    }
    out.dpcr = Le32(sav, dmaStart + 4 + 91);
    out.dicr = Le32(sav, dmaStart + 4 + 95);
    out.haveDma = true;

    for (int t = 0; t < 3; ++t) {
        const size_t o = timersStart + static_cast<size_t>(t) * 17;
        out.timerMode[t] = Le32(sav, o);
        out.timerCounter[t] = Le32(sav, o + 4);
        out.timerTarget[t] = Le32(sav, o + 8);
        if (out.timerMode[t] > 0xFFFFu || out.timerCounter[t] > 0xFFFFu || out.timerTarget[t] > 0xFFFFu) {
            error = "a root counter field is out of range - the Timers layout does not hold";
            return false;
        }
    }
    out.haveTimers = true;
    return true;
}

// ---------------------------------------------------------------- GPU packet decoding
//
// Reassembles the recorded word stream into GP0 packets. The point is the provenance: every word
// carries the guest RAM address it was fetched from, so the tpage half-word of a textured polygon
// can be followed back to the exact byte in the packet buffer that produced it - which is what
// turns "the game sends this tpage" into "this instruction wrote it".

uint32_t Gp0PacketLength(uint32_t word) {
    const uint32_t c = word >> 24;
    if (c == 0x02) return 3;
    if (c >= 0x20 && c <= 0x3F) {
        const uint32_t verts = (c & 0x08u) ? 4u : 3u;
        const uint32_t textured = (c & 0x04u) ? 1u : 0u;
        const uint32_t gouraud = (c & 0x10u) ? 1u : 0u;
        return 1u + verts * (1u + textured) + (gouraud ? verts - 1u : 0u);
    }
    if (c >= 0x40 && c <= 0x5F) {
        if (c & 0x08u) return 0;
        return (c & 0x10u) ? 4u : 3u;
    }
    if (c >= 0x60 && c <= 0x7F) {
        const uint32_t size = (c >> 3) & 3u;
        return 2u + ((c & 0x04u) ? 1u : 0u) + (size == 0 ? 1u : 0u);
    }
    if (c >= 0x80 && c <= 0x9F) return 4;
    if (c >= 0xA0 && c <= 0xDF) return 3;
    return 1;
}

struct Packet {
    size_t firstWord = 0;
    uint32_t words = 0;
    uint32_t command = 0;
    uint32_t tpage = 0xFFFFFFFFu;
    uint32_t clut = 0xFFFFFFFFu;
    uint32_t tpageAddress = 0;  // guest RAM address the tpage half-word came from
    uint32_t clutAddress = 0;
};

void DecodeGpuStream(const rr::interp::Tracer& tracer, const std::string& outDir,
                     const std::vector<uint16_t>& vram, const std::string& capturePath);

struct TraceOptions {
    std::string stateDir = "work\\oracle\\state\\rr-race";
    std::string outDir = "work\\oracle\\trace";
    std::string savPath = "work\\oracle\\vr_capture\\rr-race.sav";
    std::string capturePath = "work\\oracle\\vr_capture\\scene.csv";
    uint64_t maxSteps = 200ull * 1000ull * 1000ull;
    uint64_t instructionsPerFrame = 300000;
    uint64_t stopAfterVblanks = 0;   // 0 = do not stop on VBlank count
    uint64_t stopAfterFrames = 3;    // GP1(05h) buffer swaps; 0 = do not stop on them
    // The savestate does not carry the interrupt controller, so I_MASK has to be reconstructed.
    // Default: VBlank (the only source this model paces) and DMA (which is gated by the guest's own
    // DICR, so it only fires if the guest asked for it). Overridden by the real I_MASK whenever the
    // savestate can be parsed.
    uint32_t imask = 0x0009;
    bool imaskOverridden = false;
    uint16_t padButtons = 0xFFFF; // active low, as the SIO0 model wants it
    bool cop2 = true;
    bool gpu = true;
    bool calls = false;
    bool explore = false;
    bool dumpVram = false;
    // --dump-ram-frame F: the guest RAM at the moment frame F's ordering table starts to reach the GPU (its first
    // DMA2 linked-list GP0 word after the F-th GP1(05h)) - the state that frame's packets were built from
    // (rrgame --parity). Written to <out>\ram_frame<F>.bin.
    int dumpRamFrame = -1;
    bool watchDod3 = false;
    uint32_t dod3Offset = 0x14;
    std::vector<std::string> watchSpecs;
    std::vector<std::string> probeSpecs;
};

struct WatchSpec {
    uint32_t address;
    uint32_t length;
    std::string name;
};

bool ParseWatch(const std::string& spec, WatchSpec& out) {
    // ADDR:LEN[:name]
    const size_t a = spec.find(':');
    if (a == std::string::npos) return false;
    const size_t b = spec.find(':', a + 1);
    out.address = ParseNumber(spec.substr(0, a).c_str());
    out.length = ParseNumber(spec.substr(a + 1, (b == std::string::npos) ? std::string::npos : b - a - 1).c_str());
    out.name = (b == std::string::npos) ? spec.substr(0, a) : spec.substr(b + 1);
    return out.length != 0;
}

void DecodeGpuStream(const rr::interp::Tracer& tracer, const std::string& outDir,
                     const std::vector<uint16_t>& vram, const std::string& capturePath) {
    std::vector<Packet> packets;
    uint32_t currentDrawMode = 0;
    size_t i = 0;
    size_t blitSkip = 0;
    uint64_t gp1Count = 0;
    while (i < tracer.gpu.size()) {
        const GpuWord& w = tracer.gpu[i];
        if (w.port == 1) { ++gp1Count; ++i; continue; }
        if (blitSkip > 0) { --blitSkip; ++i; continue; }
        const uint32_t len = Gp0PacketLength(w.value);
        if (len == 0) { // polyline: consume up to the terminator
            size_t j = i + 1;
            while (j < tracer.gpu.size() && (tracer.gpu[j].value & 0xF000F000u) != 0x50005000u) ++j;
            i = j + 1;
            continue;
        }
        Packet p;
        p.firstWord = i;
        p.words = len;
        p.command = w.value;
        const uint32_t c = w.value >> 24;
        if (c >= 0x20 && c <= 0x3F && (c & 0x04u)) {
            const bool gouraud = (c & 0x10u) != 0;
            const size_t clutAt = i + 2;
            const size_t tpageAt = i + (gouraud ? 5 : 4);
            if (clutAt < tracer.gpu.size()) {
                p.clut = tracer.gpu[clutAt].value >> 16;
                p.clutAddress = tracer.gpu[clutAt].address;
            }
            if (tpageAt < tracer.gpu.size()) {
                p.tpage = tracer.gpu[tpageAt].value >> 16;
                p.tpageAddress = tracer.gpu[tpageAt].address;
            }
        } else if (c >= 0x60 && c <= 0x7F && (c & 0x04u)) {
            const size_t clutAt = i + 2;
            if (clutAt < tracer.gpu.size()) {
                p.clut = tracer.gpu[clutAt].value >> 16;
                p.clutAddress = tracer.gpu[clutAt].address;
            }
            p.tpage = currentDrawMode & 0x7FFu; // sprites take the page from GP0(E1)
        } else if (c == 0xE1) {
            currentDrawMode = w.value & 0xFFFFFFu;
        } else if (c == 0xA0) {
            const uint32_t width = (i + 2 < tracer.gpu.size()) ? (tracer.gpu[i + 2].value & 0xFFFFu) : 0;
            const uint32_t height = (i + 2 < tracer.gpu.size()) ? (tracer.gpu[i + 2].value >> 16) : 0;
            blitSkip = (static_cast<size_t>(width ? width : 0x400u) * (height ? height : 0x200u) + 1) / 2;
        }
        packets.push_back(p);
        i += len;
    }

    std::printf("\n--- GPU stream: %zu packets, %" PRIu64 " GP1 commands ---\n", packets.size(), gp1Count);

    std::map<uint32_t, uint64_t> byCommand;
    std::map<std::pair<uint32_t, uint32_t>, uint64_t> byTpageClut;
    for (const Packet& p : packets) {
        byCommand[p.command >> 24]++;
        if (p.tpage != 0xFFFFFFFFu) byTpageClut[{p.tpage & 0x7FFu, p.clut}]++;
    }
    std::printf("  by GP0 command byte:\n");
    for (const auto& kv : byCommand) std::printf("    0x%02X  %" PRIu64 "\n", kv.first, kv.second);

    std::printf("  textured primitives by (tpage, clut):\n");
    std::printf("    tpage  pageX pageY bpp   clut   clutX clutY   count   first texel in VRAM\n");
    for (const auto& kv : byTpageClut) {
        const uint32_t tp = kv.first.first;
        const uint32_t clut = kv.first.second;
        const uint32_t px = (tp & 0x0Fu) * 64u;
        const uint32_t py = ((tp >> 4) & 1u) * 256u;
        const uint32_t bppSel = (tp >> 7) & 3u;
        const uint32_t cx = (clut == 0xFFFFFFFFu) ? 0 : (clut & 0x3Fu) * 16u;
        const uint32_t cy = (clut == 0xFFFFFFFFu) ? 0 : (clut >> 6) & 0x1FFu;
        const uint16_t texel = vram[(static_cast<size_t>(py) * 1024u) + px];
        std::printf("    0x%03X  %5u %5u %3s   0x%04X %5u %5u  %6" PRIu64 "   0x%04X\n", tp, px, py,
                    bppSel == 0 ? "4" : (bppSel == 1 ? "8" : "16"), clut & 0xFFFFu, cx, cy, kv.second,
                    texel);
    }

    // Provenance: which instruction wrote the tpage half-word of each textured primitive. This is
    // the whole point of the exercise - it joins "a GPU word the hardware received" to "the guest
    // instruction that produced it". Needs a write watch covering the packet buffer.
    if (!tracer.mem.empty()) {
        std::map<uint32_t, std::vector<std::pair<uint64_t, uint32_t>>> writes; // word address -> (seq, pc)
        for (const MemEvent& e : tracer.mem) {
            if (!e.isWrite) continue;
            const uint32_t base = 0x80000000u | (e.address & 0x001FFFFFu);
            for (uint32_t b = 0; b < e.size; ++b) writes[(base + b) & ~3u].emplace_back(e.seq, e.pc);
        }
        std::map<std::pair<uint32_t, uint32_t>, uint64_t> tpageByPc; // (tpage, pc) -> count
        uint64_t unresolved = 0;
        for (const Packet& p : packets) {
            if (p.tpage == 0xFFFFFFFFu || p.tpageAddress == 0) continue;
            const uint64_t seq = tracer.gpu[p.firstWord].seq;
            const uint32_t key = (0x80000000u | (p.tpageAddress & 0x001FFFFFu)) & ~3u;
            auto it = writes.find(key);
            if (it == writes.end()) { ++unresolved; continue; }
            uint32_t pc = 0;
            bool found = false;
            for (const auto& wv : it->second) {
                if (wv.first < seq) { pc = wv.second; found = true; }
                else break;
            }
            if (!found) { ++unresolved; continue; }
            tpageByPc[{p.tpage & 0x7FFu, pc}]++;
        }
        if (!tpageByPc.empty() || unresolved != 0) {
            std::printf("  tpage word provenance (which instruction wrote it):\n");
            for (const auto& kv : tpageByPc)
                std::printf("    tpage 0x%03X  written by pc 0x%08X   %6" PRIu64 " primitives\n",
                            kv.first.first, kv.first.second, kv.second);
            std::printf("    unresolved (built before the trace started): %" PRIu64 "\n", unresolved);
        }
    }

    // Independent corroboration: the (page, palette) pairs an EMULATOR recorded for a different
    // frame of the same race. Values are compared modulo the two GP0(E1)-only bits 9 and 10, which
    // a polygon packet's tpage half-word does not carry.
    if (!capturePath.empty()) {
        std::FILE* f = std::fopen(capturePath.c_str(), "rb");
        if (f == nullptr) {
            std::printf("  (no capture at %s to cross-check against)\n", capturePath.c_str());
        } else {
            std::map<std::pair<uint32_t, uint32_t>, uint64_t> theirs;
            char line[512];
            bool first = true;
            while (std::fgets(line, sizeof(line), f) != nullptr) {
                if (first) { first = false; continue; }
                unsigned idx = 0, order = 0, flags = 0, drawMode = 0, palette = 0;
                if (std::sscanf(line, "%u,%u,%u,%u,%u", &idx, &order, &flags, &drawMode, &palette) != 5) continue;
                (void)idx; (void)order; (void)flags;
                theirs[{drawMode & 0x1FFu, palette}]++;
            }
            std::fclose(f);
            std::map<std::pair<uint32_t, uint32_t>, uint64_t> ours;
            for (const auto& kv : byTpageClut) ours[{kv.first.first & 0x1FFu, kv.first.second & 0xFFFFu}] += kv.second;
            size_t both = 0, onlyTheirs = 0, onlyOurs = 0;
            for (const auto& kv : theirs) { if (ours.count(kv.first)) ++both; else ++onlyTheirs; }
            for (const auto& kv : ours) if (!theirs.count(kv.first)) ++onlyOurs;
            std::printf("\n  cross-check against %s (a different frame, recorded by an emulator):\n",
                        capturePath.c_str());
            std::printf("    (page, palette) pairs: %zu in both, %zu only in the capture, %zu only here\n",
                        both, onlyTheirs, onlyOurs);
            if (onlyTheirs) {
                std::printf("    only in the capture:");
                for (const auto& kv : theirs) if (!ours.count(kv.first))
                    std::printf(" (0x%03X,0x%04X)", kv.first.first, kv.first.second);
                std::printf("\n");
            }
            if (onlyOurs) {
                std::printf("    only here:          ");
                for (const auto& kv : ours) if (!theirs.count(kv.first))
                    std::printf(" (0x%03X,0x%04X)", kv.first.first, kv.first.second);
                std::printf("\n");
            }
        }
    }

    const std::string path = Join(outDir, "prims.csv");
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (f == nullptr) return;
    std::fprintf(f, "i,seq,cmd,words,srcAddr,tpage,tpageAddr,clut,clutAddr,w0,w1,w2,w3,w4,w5,w6,w7,w8,w9,w10,w11\n");
    for (size_t n = 0; n < packets.size(); ++n) {
        const Packet& p = packets[n];
        const GpuWord& first = tracer.gpu[p.firstWord];
        std::fprintf(f, "%zu,%" PRIu64 ",0x%02X,%u,0x%08X,", n, first.seq, p.command >> 24, p.words,
                     first.address);
        if (p.tpage == 0xFFFFFFFFu) std::fprintf(f, ",,");
        else std::fprintf(f, "0x%04X,0x%08X,", p.tpage, p.tpageAddress);
        if (p.clut == 0xFFFFFFFFu) std::fprintf(f, ",");
        else std::fprintf(f, "0x%04X,0x%08X", p.clut, p.clutAddress);
        for (uint32_t k = 0; k < 12; ++k) {
            if (k < p.words && p.firstWord + k < tracer.gpu.size())
                std::fprintf(f, ",0x%08X", tracer.gpu[p.firstWord + k].value);
            else std::fprintf(f, ",");
        }
        std::fprintf(f, "\n");
    }
    std::fclose(f);
    std::printf("wrote %s (%zu packets)\n", path.c_str(), packets.size());
}

} // namespace

// ---------------------------------------------------------------- COP2 replay
//
// What this DOES prove: the recorded triples are complete and self-contained (each record carries
// the entire input register file), the harness is order-independent, and our Gte is deterministic.
// What it does NOT prove: agreement with hardware. The log was produced by this same Gte, so a
// clean replay is a consistency check, not a validation. The value of the corpus is that it is a
// set of (inputs, opcode) cases generated by the ORIGINAL game on real data - i.e. ready-made test
// vectors for a console run.
int CmdReplay(int argc, char** argv) {
    std::string path = "work\\oracle\\trace\\cop2.bin";
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--in" && i + 1 < argc) path = argv[++i];
        else { std::printf("replay: unknown option %s\n", a.c_str()); return 2; }
    }
    std::vector<uint8_t> blob;
    std::string error;
    if (!rr::interp::ReadWholeFile(path, blob, error)) { std::printf("FAIL: %s\n", error.c_str()); return 1; }
    if (blob.size() % sizeof(Cop2Record) != 0) {
        std::printf("FAIL: %s is %zu bytes, not a multiple of the %zu-byte record\n", path.c_str(),
                    blob.size(), sizeof(Cop2Record));
        return 1;
    }
    const Cop2Record* rec = reinterpret_cast<const Cop2Record*>(blob.data());
    const size_t n = blob.size() / sizeof(Cop2Record);
    std::printf("=== rrverify replay ===\n%zu COP2 records from %s\n", n, path.c_str());

    rr::interp::Gte gte;
    size_t mismatches = 0, firstBad = 0;
    // Forward, then backward. A backward pass that also matches proves every record carries the
    // complete input state - no record depends on the one before it.
    for (int pass = 0; pass < 2; ++pass) {
        for (size_t k = 0; k < n; ++k) {
            const Cop2Record& r = rec[(pass == 0) ? k : (n - 1 - k)];
            std::memcpy(gte.dr, r.inDr, sizeof(gte.dr));
            std::memcpy(gte.cr, r.inCr, sizeof(gte.cr));
            gte.Execute(r.instruction);
            if (gte.unimplemented) {
                gte.unimplemented = false;
                std::printf("FAIL: record %zu (pc 0x%08X) hit an unimplemented GTE case: %s\n", k, r.pc,
                            gte.unimplementedDetail.c_str());
                return 1;
            }
            bool bad = gte.cr[31] != r.outFlag;
            for (int i = 0; i < 32 && !bad; ++i) bad = gte.dr[i] != r.outDr[i];
            if (bad) {
                if (mismatches == 0) firstBad = k;
                ++mismatches;
            }
        }
    }
    std::printf("replay forward+backward: %zu of %zu comparisons differ%s\n", mismatches, 2 * n,
                mismatches ? "" : "  (0 = the records are complete and the GTE is deterministic)");
    if (mismatches) std::printf("  first at record %zu\n", firstBad);

    // ---- coverage: what the game's own code actually asks the GTE to do
    std::map<std::string, std::array<uint64_t, 4>> byCmd; // [sf*2+lm]
    std::map<std::string, uint64_t> mvmva;
    std::array<uint64_t, 32> flagBits{};
    uint64_t divideOverflow = 0, anyFlag = 0;
    uint64_t divideCases = 0, divideExact = 0, divideMaxErr = 0;
    for (size_t k = 0; k < n; ++k) {
        const Cop2Record& r = rec[k];
        const char* m = GteMnemonic(r.instruction);
        const uint32_t sf = (r.instruction >> 19) & 1u;
        const uint32_t lm = (r.instruction >> 10) & 1u;
        byCmd[m][sf * 2 + lm]++;
        if ((r.instruction & 0x3Fu) == 0x12u) {
            char buf[48];
            std::snprintf(buf, sizeof(buf), "mx=%u v=%u cv=%u", (r.instruction >> 17) & 3u,
                          (r.instruction >> 15) & 3u, (r.instruction >> 13) & 3u);
            mvmva[buf]++;
        }
        for (int b = 12; b < 32; ++b) if (r.outFlag & (1u << b)) flagBits[static_cast<size_t>(b)]++;
        if (r.outFlag & 0x80000000u) ++anyFlag;
        if (r.outFlag & (1u << 17)) ++divideOverflow;
        // The perspective divide of every RTPS/RTPT: our UNR result against the exact quotient.
        const uint32_t op = r.instruction & 0x3Fu;
        if (op == 0x01u || op == 0x30u) {
            const uint32_t sz3 = r.outDr[19] & 0xFFFFu;
            if (sz3 != 0) {
                ++divideCases;
                divideExact = std::min<uint64_t>(divideExact ? divideExact : sz3, sz3);
                divideMaxErr = std::max<uint64_t>(divideMaxErr, sz3);
            }
        }
    }
    std::printf("\n--- coverage (what the original code asks of the GTE) ---\n");
    std::printf("  command  sf=0,lm=0  sf=0,lm=1  sf=1,lm=0  sf=1,lm=1\n");
    for (const auto& kv : byCmd)
        std::printf("  %-7s %10" PRIu64 " %10" PRIu64 " %10" PRIu64 " %10" PRIu64 "\n", kv.first.c_str(),
                    kv.second[0], kv.second[1], kv.second[2], kv.second[3]);
    if (!mvmva.empty()) {
        std::printf("  MVMVA operand selects used:\n");
        for (const auto& kv : mvmva) std::printf("    %-20s %" PRIu64 "\n", kv.first.c_str(), kv.second);
        std::printf("    (matrix select 3 - the undocumented hardware case this GTE traps on - "
                    "appears %s)\n",
                    mvmva.count("mx=3 v=0 cv=0") || mvmva.count("mx=3 v=1 cv=0") ? "YES" : "never");
    }
    std::printf("  FLAG bits set at least once:");
    bool any = false;
    for (int b = 12; b < 32; ++b) {
        if (flagBits[static_cast<size_t>(b)] == 0) continue;
        std::printf(" %d(%" PRIu64 ")", b, flagBits[static_cast<size_t>(b)]);
        any = true;
    }
    std::printf("%s\n", any ? "" : " none");
    std::printf("  commands that raised any error flag: %" PRIu64 " of %zu\n", anyFlag, n);
    std::printf("  divide overflow (FLAG bit 17):       %" PRIu64 "\n", divideOverflow);
    std::printf("  RTPS/RTPT divides with SZ3 != 0:     %" PRIu64 "  (SZ3 in %" PRIu64 "..%" PRIu64
                ", H = %u)\n", divideCases, divideExact, divideMaxErr, n ? (rec[0].inCr[26] & 0xFFFFu) : 0u);
    return mismatches == 0 ? 0 : 1;
}

int CmdTrace(int argc, char** argv) {
    TraceOptions opt;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--state" && i + 1 < argc) opt.stateDir = argv[++i];
        else if (a == "--out" && i + 1 < argc) opt.outDir = argv[++i];
        else if (a == "--max-steps" && i + 1 < argc) opt.maxSteps = std::strtoull(argv[++i], nullptr, 0);
        else if (a == "--frame-instructions" && i + 1 < argc) opt.instructionsPerFrame = std::strtoull(argv[++i], nullptr, 0);
        else if (a == "--vblanks" && i + 1 < argc) opt.stopAfterVblanks = std::strtoull(argv[++i], nullptr, 0);
        else if (a == "--frames" && i + 1 < argc) opt.stopAfterFrames = std::strtoull(argv[++i], nullptr, 0);
        else if (a == "--imask" && i + 1 < argc) { opt.imask = ParseNumber(argv[++i]); opt.imaskOverridden = true; }
        else if (a == "--pad" && i + 1 < argc) { opt.padButtons = static_cast<uint16_t>(ParseNumber(argv[++i])); }
        else if (a == "--sav" && i + 1 < argc) opt.savPath = argv[++i];
        else if (a == "--capture" && i + 1 < argc) opt.capturePath = argv[++i];
        else if (a == "--no-capture") opt.capturePath.clear();
        else if (a == "--no-sav") opt.savPath.clear();
        else if (a == "--no-cop2") opt.cop2 = false;
        else if (a == "--no-gpu") opt.gpu = false;
        else if (a == "--calls") opt.calls = true;
        else if (a == "--explore") opt.explore = true;
        else if (a == "--dump-vram") opt.dumpVram = true;
        else if (a == "--dump-ram-frame" && i + 1 < argc) opt.dumpRamFrame = std::atoi(argv[++i]);
        else if (a == "--watch" && i + 1 < argc) opt.watchSpecs.push_back(argv[++i]);
        else if (a == "--probe" && i + 1 < argc) opt.probeSpecs.push_back(argv[++i]);
        else if (a == "--watch-dod3") opt.watchDod3 = true;
        else if (a == "--dod3-offset" && i + 1 < argc) opt.dod3Offset = ParseNumber(argv[++i]);
        else {
            std::printf("trace: unknown option %s\n", a.c_str());
            return 2;
        }
    }

    Memory mem;
    Cpu cpu(mem);
    rr::interp::SnapshotInfo info;
    std::string error;
    if (!rr::interp::LoadSnapshot(opt.stateDir, mem, cpu, info, error)) {
        std::printf("FAIL: %s\n", error.c_str());
        return 1;
    }

    Devices dev(mem);
    std::vector<uint8_t> blob;
    if (rr::interp::ReadWholeFile(Join(opt.stateDir, "vram.bin"), blob, error)) {
        if (!dev.LoadVram(blob, error)) { std::printf("FAIL: %s\n", error.c_str()); return 1; }
    } else {
        std::printf("FAIL: %s\n", error.c_str());
        return 1;
    }
    if (rr::interp::ReadWholeFile(Join(opt.stateDir, "spuram.bin"), blob, error)) {
        if (!dev.LoadSpuRam(blob, error)) { std::printf("FAIL: %s\n", error.c_str()); return 1; }
    }

    Tracer tracer;
    tracer.traceCop2 = opt.cop2;
    tracer.traceGpu = opt.gpu;
    tracer.traceCalls = opt.calls;
    for (const std::string& s : opt.watchSpecs) {
        WatchSpec w;
        if (!ParseWatch(s, w)) { std::printf("FAIL: bad --watch spec '%s'\n", s.c_str()); return 1; }
        tracer.AddWatch(w.address, w.length, w.name);
    }
    for (const std::string& s : opt.probeSpecs) {
        const size_t c = s.find(':');
        const uint32_t at = ParseNumber(s.substr(0, c).c_str());
        tracer.AddProbe(at, (c == std::string::npos) ? s : s.substr(c + 1));
    }

    // Every DOD3 chunk currently resident in guest RAM, watched at +`dod3Offset`. The RMD3 loader
    // rewrites DOD3+0x20..+0x30 into absolute pointers but leaves everything
    // else as it was on the disc, so a tag scan finds exactly the loaded groups.
    std::vector<uint32_t> dod3Bases;
    const uint32_t kDod3Header = 0x40;
    if (opt.watchDod3) {
        const std::vector<uint8_t>& ram = mem.ram();
        for (size_t o = 0; o + kDod3Header < ram.size(); o += 4) {
            if (ram[o] != 'D' || ram[o + 1] != 'O' || ram[o + 2] != 'D' || ram[o + 3] != '3') continue;
            const uint32_t at = 0x80000000u + static_cast<uint32_t>(o);
            dod3Bases.push_back(at);
            char name[64];
            std::snprintf(name, sizeof(name), "DOD3@0x%08X", at);
            // The whole header, so one run produces the complete per-field read profile rather than
            // a yes/no answer about one offset.
            tracer.AddWatch(at, kDod3Header, name, true, true);
        }
        std::printf("DOD3 chunks in RAM: %zu, watching the whole 0x%02X-byte header of each\n",
                    dod3Bases.size(), kDod3Header);
    }

    dev.tracer = &tracer;
    // The digital pad the SIO0 model answers with. Active low, so the default 0xFFFF is "nothing
    // pressed"; `--pad 0xBFFF` holds Cross, and so on. Without this the throttle path of the race
    // code is simply never entered and a trace cannot see it.
    dev.padButtons = opt.padButtons;
    dev.instructionsPerFrame = opt.instructionsPerFrame;
    dev.exploreUnmodelled = opt.explore;
    // The interrupt controller is part of the machine state we do not have in the snapshot (it is a
    // component this project's savestate extractor does not decode), so it is rebuilt from what the
    // guest's own kernel has configured: the game re-writes I_MASK every frame, and until it does,
    // only the sources SR.IM2 allows can fire anyway.
    dev.imask = opt.imask;
    dev.istat = 0;

    PeripheralState ps;
    bool haveRealPeripherals = false;
    if (!opt.savPath.empty()) {
        std::string perr;
        if (!ParsePeripheralState(opt.savPath, ps, perr)) {
            std::printf("FAIL: cannot read the peripheral state from %s: %s\n", opt.savPath.c_str(),
                        perr.c_str());
            std::printf("      (pass --no-sav to run on the reconstructed defaults instead, and say so)\n");
            return 1;
        }
        haveRealPeripherals = true;
        if (!opt.imaskOverridden) dev.imask = ps.imask;
        dev.istat = ps.istat;
        dev.dpcr = ps.dpcr;
        dev.dicr = ps.dicr;
        for (int c = 0; c < 7; ++c) {
            dev.dma[c].madr = ps.madr[c];
            dev.dma[c].bcr = ps.bcr[c];
            // The start bit is deliberately not restored: a transfer that was mid-flight on the
            // console has no meaning in a model that completes every transfer instantly.
            dev.dma[c].chcr = ps.chcr[c] & ~0x11000000u;
        }
        for (int t = 0; t < 3; ++t) dev.SetTimer(t, ps.timerMode[t], ps.timerCounter[t], ps.timerTarget[t]);
    }

    cpu.devices = &dev;
    cpu.tracer = &tracer;
    cpu.trapOnBiosVector = false;
    cpu.handleExceptions = true;
    cpu.takeInterrupts = true;

    std::printf("=== rrverify trace ===\n");
    std::printf("state            %s (frame %u)\n", opt.stateDir.c_str(), info.frameNumber);
    std::printf("resume pc        0x%08X\n", cpu.pc);
    std::printf("SR / CAUSE       0x%08X / 0x%08X\n", cpu.cop0[12], cpu.cop0[13]);
    std::printf("vblank pacing    every %" PRIu64 " retired instructions\n", opt.instructionsPerFrame);
    std::printf("stop after       %" PRIu64 " buffer swaps / %" PRIu64 " vblanks / %" PRIu64 " instructions\n",
                opt.stopAfterFrames, opt.stopAfterVblanks, opt.maxSteps);
    if (haveRealPeripherals) {
        std::printf("peripherals      recovered from %s\n", opt.savPath.c_str());
        std::printf("  I_STAT/I_MASK  0x%08X / 0x%08X\n", ps.istat, ps.imask);
        std::printf("  DPCR / DICR    0x%08X / 0x%08X\n", ps.dpcr, ps.dicr);
        for (int t = 0; t < 3; ++t)
            std::printf("  timer %d        mode 0x%04X (clock source %u) counter %u target %u\n", t,
                        ps.timerMode[t], (ps.timerMode[t] >> 8) & 3u, ps.timerCounter[t], ps.timerTarget[t]);
    } else {
        std::printf("peripherals      RECONSTRUCTED (--no-sav): I_MASK 0x%04X, DPCR/DICR at reset\n",
                    opt.imask);
    }
    std::printf("I_MASK in use    0x%08X\n", dev.imask);
    std::printf("watches          %zu\n", tracer.watches.size());

    // Run in slices so the vblank budget can end the run without a sentinel.
    const uint64_t startRetired = cpu.instructionsRetired;
    Trap trap;
    const uint64_t slice = 20000;
    bool ramDumped = opt.dumpRamFrame < 0;
    while (cpu.instructionsRetired - startRetired < opt.maxSteps) {
        // once frame F's swap has happened, step finely so the dump lands on its first ordering-table word
        const bool fine = !ramDumped && dev.displayStartCount >= static_cast<uint64_t>(opt.dumpRamFrame);
        const size_t gpuBefore = tracer.gpu.size();
        trap = cpu.Run(0xFFFFFFFFu, fine ? 1 : slice);
        if (fine) {
            for (size_t k = gpuBefore; k < tracer.gpu.size() && !ramDumped; ++k) {
                const GpuWord& w = tracer.gpu[k];
                if (w.port != 0 || w.source != GpuWordSource::DmaLinkedList) continue;
                const std::string path = Join(opt.outDir, "ram_frame" + std::to_string(opt.dumpRamFrame) + ".bin");
                if (WriteWholeFile(path, mem.ram().data(), mem.ram().size()))
                    std::printf("wrote %s (guest RAM at frame %d's first DMA2 linked-list word, pc 0x%08X, "
                                "%" PRIu64 " instructions in)\n",
                                path.c_str(), opt.dumpRamFrame, cpu.pc, cpu.instructionsRetired - startRetired);
                ramDumped = true;
            }
        }
        if (trap.kind != TrapKind::StepLimit) break;
        if (opt.stopAfterVblanks != 0 && dev.vblankCount >= opt.stopAfterVblanks) break;
        if (opt.stopAfterFrames != 0 && dev.displayStartCount >= opt.stopAfterFrames) break;
    }

    const uint64_t ran = cpu.instructionsRetired - startRetired;
    std::printf("\n--- run ---\n");
    std::printf("instructions     %" PRIu64 "\n", ran);
    std::printf("stopped          %s\n", trap.kind == TrapKind::StepLimit ? "budget/vblank" : trap.ToString().c_str());
    std::printf("pc at stop       0x%08X\n", cpu.pc);
    std::printf("vblanks          %" PRIu64 "  interrupts %" PRIu64 "  syscalls %" PRIu64 "\n",
                dev.vblankCount, cpu.interruptsTaken, cpu.syscallsTaken);
    std::printf("buffer swaps     %" PRIu64 " (GP1(05h))   pad polls %" PRIu64 " (%" PRIu64
                " bytes on SIO0, pad 0x%04X)\n",
                dev.displayStartCount, dev.padPollCount, dev.padBytesSent, opt.padButtons);
    std::printf("GP0 / GP1 words  %" PRIu64 " / %" PRIu64 "\n", dev.gp0WordsSeen, dev.gp1WordsSeen);
    std::printf("DMA2 packets     %" PRIu64 " (%" PRIu64 " words)\n", dev.dmaLinkedListPackets,
                dev.dmaLinkedListWords);
    std::printf("VRAM words up    %" PRIu64 "\n", dev.vramWordsUploaded);
    std::printf("COP2 commands    %" PRIu64 "%s\n", tracer.cop2Count, tracer.cop2Truncated ? " (TRUNCATED)" : "");

    if (!dev.unmodelled.empty()) {
        std::printf("\n--- unmodelled hardware registers touched (--explore) ---\n");
        for (const auto& u : dev.unmodelled) {
            std::printf("  %s 0x%08X size %u x%" PRIu64 "  first pc 0x%08X value 0x%08X\n",
                        u.isWrite ? "write" : "read ", u.address, u.size, u.count, u.pc, u.value);
        }
    }

    if (!tracer.watches.empty()) {
        uint64_t totalReads = 0, totalWrites = 0;
        size_t hitRanges = 0;
        for (const auto& w : tracer.watches) {
            totalReads += w.readHits;
            totalWrites += w.writeHits;
            if (w.readHits || w.writeHits) ++hitRanges;
        }
        std::printf("\n--- watches: %zu ranges, %zu with hits, %" PRIu64 " reads / %" PRIu64 " writes ---\n",
                    tracer.watches.size(), hitRanges, totalReads, totalWrites);
        size_t shown = 0;
        for (const auto& w : tracer.watches) {
            if (w.readHits == 0 && w.writeHits == 0) continue;
            if (++shown > 24) { std::printf("  ... %zu more ranges with hits\n", hitRanges - 24); break; }
            std::printf("  %-28s 0x%08X..0x%08X  reads %" PRIu64 "  writes %" PRIu64 "\n",
                        w.name.c_str(), w.low, w.high, w.readHits, w.writeHits);
        }
        if (hitRanges == 0) std::printf("  NO ACCESS to any watched range during the whole run.\n");
        // Which instructions touched a watched range at all - the answer to "is this field read".
        std::map<uint32_t, std::pair<uint64_t, uint64_t>> byPc;
        for (const MemEvent& e : tracer.mem) {
            auto& r = byPc[e.pc];
            if (e.isWrite) ++r.second; else ++r.first;
        }
        if (!byPc.empty()) {
            std::printf("  accessing instructions:\n");
            size_t n = 0;
            for (const auto& kv : byPc) {
                if (++n > 40) { std::printf("    ... %zu more\n", byPc.size() - 40); break; }
                std::printf("    pc 0x%08X  reads %" PRIu64 "  writes %" PRIu64 "\n", kv.first,
                            kv.second.first, kv.second.second);
            }
        }
    }

    if (!dod3Bases.empty()) {
        // Per-field read profile of the DOD3 header over the whole run. `touched` counts how many
        // distinct DOD3 chunks were touched at all, which bounds the negative: a field can only be
        // said to be unread for the groups that were actually drawn.
        struct FieldStat { uint64_t reads = 0, writes = 0; std::map<uint32_t, uint64_t> pcs; };
        std::vector<FieldStat> field(kDod3Header);
        std::map<uint32_t, uint64_t> touchedChunks;
        for (const MemEvent& e : tracer.mem) {
            const uint32_t addr = 0x80000000u | (e.address & 0x001FFFFFu);
            auto it = std::upper_bound(dod3Bases.begin(), dod3Bases.end(), addr);
            if (it == dod3Bases.begin()) continue;
            --it;
            const uint32_t off = addr - *it;
            if (off >= kDod3Header) continue;
            touchedChunks[*it]++;
            for (uint32_t b = 0; b < e.size && off + b < kDod3Header; ++b) {
                if (e.isWrite) ++field[off + b].writes; else ++field[off + b].reads;
            }
            field[off].pcs[e.pc]++;
        }
        std::printf("\n--- DOD3 header read profile (%zu chunks resident, %zu touched) ---\n",
                    dod3Bases.size(), touchedChunks.size());
        std::printf("  off   bytes-read  bytes-written  instructions\n");
        for (uint32_t o = 0; o < kDod3Header; o += 2) {
            const uint64_t r = field[o].reads + field[o + 1].reads;
            const uint64_t w = field[o].writes + field[o + 1].writes;
            std::string pcs;
            for (const auto& kv : field[o].pcs) {
                char b[16];
                std::snprintf(b, sizeof(b), " %08X", kv.first);
                pcs += b;
            }
            std::printf("  +%02X   %10" PRIu64 "  %13" PRIu64 "  %s\n", o, r, w, pcs.c_str());
        }
    }

    if (!tracer.probes.empty()) {
        std::printf("\n--- pc probes ---\n");
        std::map<uint32_t, uint64_t> hits;
        for (const auto& h : tracer.probeHits) hits[h.pc]++;
        for (const auto& p : tracer.probes)
            std::printf("  0x%08X %-24s hits %" PRIu64 "\n", p.first, p.second.c_str(), hits[p.first]);
    }

    // ---------------------------------------------------------------- artifacts
    std::string cmd = "cmd /c if not exist \"" + opt.outDir + "\" mkdir \"" + opt.outDir + "\"";
    if (std::system(cmd.c_str()) != 0) {
        std::printf("note: could not create %s\n", opt.outDir.c_str());
    }

    if (!tracer.cop2.empty()) {
        const std::string path = Join(opt.outDir, "cop2.bin");
        if (!WriteWholeFile(path, tracer.cop2.data(), tracer.cop2.size() * sizeof(Cop2Record)))
            std::printf("note: could not write %s\n", path.c_str());
        else
            std::printf("\nwrote %s (%zu records of %zu bytes)\n", path.c_str(), tracer.cop2.size(),
                        sizeof(Cop2Record));
    }
    if (!tracer.gpu.empty()) {
        const std::string path = Join(opt.outDir, "gpu.bin");
        if (!WriteWholeFile(path, tracer.gpu.data(), tracer.gpu.size() * sizeof(GpuWord)))
            std::printf("note: could not write %s\n", path.c_str());
        else
            std::printf("wrote %s (%zu words of %zu bytes)\n", path.c_str(), tracer.gpu.size(),
                        sizeof(GpuWord));
    }
    if (!tracer.mem.empty()) {
        const std::string path = Join(opt.outDir, "watch.csv");
        std::FILE* f = std::fopen(path.c_str(), "wb");
        if (f != nullptr) {
            std::fprintf(f, "seq,pc,kind,watch,address,size,value\n");
            for (const MemEvent& e : tracer.mem) {
                std::fprintf(f, "%" PRIu64 ",0x%08X,%s,%s,0x%08X,%u,0x%08X\n", e.seq, e.pc,
                             e.isWrite ? "write" : "read",
                             tracer.watches[e.watchIndex].name.c_str(), e.address, e.size, e.value);
            }
            std::fclose(f);
            std::printf("wrote %s (%zu events)\n", path.c_str(), tracer.mem.size());
        }
    }
    if (!tracer.calls.empty()) {
        const std::string path = Join(opt.outDir, "calls.bin");
        if (WriteWholeFile(path, tracer.calls.data(), tracer.calls.size() * sizeof(rr::interp::CallEvent)))
            std::printf("wrote %s (%zu calls)\n", path.c_str(), tracer.calls.size());
    }
    if (!tracer.probeHits.empty()) {
        const std::string path = Join(opt.outDir, "probes.csv");
        std::FILE* f = std::fopen(path.c_str(), "wb");
        if (f != nullptr) {
            std::fprintf(f, "seq,pc,name");
            static const char* kReg[32] = {"zero","at","v0","v1","a0","a1","a2","a3","t0","t1","t2",
                "t3","t4","t5","t6","t7","s0","s1","s2","s3","s4","s5","s6","s7","t8","t9","k0","k1",
                "gp","sp","fp","ra"};
            for (int i = 0; i < 32; ++i) std::fprintf(f, ",%s", kReg[i]);
            std::fprintf(f, ",hi,lo\n");
            for (const auto& h : tracer.probeHits) {
                const char* name = "";
                for (const auto& p : tracer.probes) if (p.first == h.pc) { name = p.second.c_str(); break; }
                std::fprintf(f, "%" PRIu64 ",0x%08X,%s", h.seq, h.pc, name);
                for (int i = 0; i < 32; ++i) std::fprintf(f, ",0x%08X", h.regs[i]);
                std::fprintf(f, ",0x%08X,0x%08X\n", h.hi, h.lo);
            }
            std::fclose(f);
            std::printf("wrote %s (%zu hits)\n", path.c_str(), tracer.probeHits.size());
        }
    }
    if (opt.dumpVram) {
        const std::string path = Join(opt.outDir, "vram_after.bin");
        if (WriteWholeFile(path, dev.vram.data(), dev.vram.size() * 2))
            std::printf("wrote %s\n", path.c_str());
    }

    // ---------------------------------------------------------------- quick shape report
    if (!tracer.cop2.empty()) {
        std::map<std::string, uint64_t> byMnemonic;
        std::map<uint32_t, uint64_t> byPc;
        for (const Cop2Record& r : tracer.cop2) {
            byMnemonic[GteMnemonic(r.instruction)]++;
            byPc[r.pc]++;
        }
        std::printf("\n--- COP2 by command ---\n");
        for (const auto& kv : byMnemonic) std::printf("  %-6s %" PRIu64 "\n", kv.first.c_str(), kv.second);
        std::vector<std::pair<uint32_t, uint64_t>> top(byPc.begin(), byPc.end());
        std::sort(top.begin(), top.end(), [](auto& a, auto& b) { return a.second > b.second; });
        std::printf("--- COP2 by issuing pc (top 20 of %zu) ---\n", top.size());
        for (size_t i = 0; i < top.size() && i < 20; ++i)
            std::printf("  0x%08X  %" PRIu64 "\n", top[i].first, top[i].second);
    }

    if (!tracer.gpu.empty()) DecodeGpuStream(tracer, opt.outDir, dev.vram, opt.capturePath);

    return (trap.kind == TrapKind::StepLimit || trap.kind == TrapKind::Halted) ? 0 : 1;
}
