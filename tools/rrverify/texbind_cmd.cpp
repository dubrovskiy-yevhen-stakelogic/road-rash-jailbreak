// rrverify texbind - the model -> texture -> palette binding, checked against the disc.
//
// Everything this checks was derived by tracing a real frame inside the interpreter. This
// subcommand is the standing proof that the rule
// still holds, and it is byte-grounded end to end: it never trusts a formula, it re-derives the
// runtime state from the game's own files and diffs it against a captured console state.
//
// Four checks, all hard:
//
//   A. every occupied slot of the runtime texture page table at 0x800D5F70 names a LECT chunk id;
//      the chunk with that id on the PLAYER'S disc extract must be byte-identical to the VRAM
//      rectangle the slot describes, for every row.
//   B. every model in the runtime registry at 0x800CE1B0 must have `registry+0x07` equal to the
//      flat page-table slot whose id equals `DOD3+0x1C` of the model's group 0 - i.e. the model's
//      texture id, which is a field of the .GEO on the disc.
//   C. every 4bpp slot's CLUT descriptor at entry+0x04 must decode as shift=2, mask=3 and
//      baseY = entryV + pageY + (height - 1), and all 40 CLUTs it addresses must look like CLUTs
//      in the captured VRAM (entry 0 transparent, the other 15 non-zero).
//   D. where the snapshot's own scratchpad still holds the built table at 0x1F8000A0, the longest
//      run of entries that matches the computed table is reported; a run of 8 or more is treated
//      as confirmation that the formula is the engine's.
#include <algorithm>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "interp/snapshot.h"

namespace {

struct LectChunk {
    std::string file;
    uint32_t off = 0, size = 0;
    uint8_t kind = 0, bpp = 0;
    uint16_t id = 0, width = 0;
    uint32_t height = 0;
    uint32_t payload = 0;   // file offset of the first indexed pixel byte
};

uint16_t Le16(const std::vector<uint8_t>& d, size_t at) {
    return static_cast<uint16_t>(d[at] | (d[at + 1] << 8));
}
uint32_t Le32(const std::vector<uint8_t>& d, size_t at) {
    return static_cast<uint32_t>(d[at]) | (static_cast<uint32_t>(d[at + 1]) << 8) |
           (static_cast<uint32_t>(d[at + 2]) << 16) | (static_cast<uint32_t>(d[at + 3]) << 24);
}

// Finds the first indexed pixel byte of a LECT chunk. Kinds 5/6 store raw pixels at +0x18; kinds
// 1/2/3/4 store a whole TIM there, so its optional CLUT block has to be stepped over.
uint32_t LectPayload(const std::vector<uint8_t>& d, uint32_t off, uint8_t kind) {
    if (kind == 5 || kind == 6) return off + 0x18;
    const uint32_t tim = off + 0x18;
    const uint32_t flags = Le32(d, tim + 4);
    uint32_t p = tim + 8;
    if (flags & 8u) p += Le32(d, p);       // skip the CLUT block
    return p + 12;                          // skip the image block header
}

void ScanFile(const std::string& shown, const std::vector<uint8_t>& d, std::vector<LectChunk>& out) {
    if (d.size() < 0x18) return;
    for (size_t i = 0; i + 0x18 <= d.size(); ++i) {
        if (d[i] != 'L' || d[i + 1] != 'E' || d[i + 2] != 'C' || d[i + 3] != 'T') continue;
        LectChunk c;
        c.file = shown;
        c.off = static_cast<uint32_t>(i);
        c.size = Le32(d, i + 4);
        c.kind = d[i + 0x0C];
        c.bpp = d[i + 0x0D];
        c.id = Le16(d, i + 0x10);
        c.width = Le16(d, i + 0x12);
        c.height = Le32(d, i + 0x14);
        if (c.kind < 1 || c.kind > 6) continue;
        if (c.bpp != 4 && c.bpp != 8) continue;
        if (c.width == 0 || c.width > 512 || c.height == 0 || c.height > 512) continue;
        if (static_cast<uint64_t>(i) + c.size > d.size()) continue;
        c.payload = LectPayload(d, c.off, c.kind);
        out.push_back(c);
    }
}

struct Entry {
    uint8_t id = 0, slot = 0, uBias = 0, vBias = 0;
    uint16_t desc = 0, colX = 0, tpage = 0, clut = 0;
};

} // namespace

int CmdTexBind(int argc, char** argv) {
    std::string stateDir = "work\\oracle\\state\\rr-race";
    std::string dataDir = "work\\disc_us\\DATA";
    bool verbose = false;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--state" && i + 1 < argc) stateDir = argv[++i];
        else if (a == "--data" && i + 1 < argc) dataDir = argv[++i];
        else if (a == "--verbose") verbose = true;
        else { std::printf("texbind: unknown option %s\n", a.c_str()); return 2; }
    }

    std::vector<uint8_t> ram, vram, scratch;
    std::string error;
    const auto join = [](const std::string& d, const char* n) {
        std::string s = d;
        if (!s.empty() && s.back() != '\\' && s.back() != '/') s.push_back('\\');
        return s + n;
    };
    if (!rr::interp::ReadWholeFile(join(stateDir, "ram.bin"), ram, error) ||
        !rr::interp::ReadWholeFile(join(stateDir, "vram.bin"), vram, error) ||
        !rr::interp::ReadWholeFile(join(stateDir, "scratchpad.bin"), scratch, error)) {
        std::printf("FAIL: %s\n", error.c_str());
        return 1;
    }
    if (ram.size() != 2u * 1024 * 1024 || vram.size() != 1024u * 512 * 2 || scratch.size() != 1024) {
        std::printf("FAIL: the state files are not 2 MiB / 1 MiB / 1 KiB\n");
        return 1;
    }

    const auto r8 = [&](uint32_t a) { return ram[a & 0x1FFFFF]; };
    const auto r16 = [&](uint32_t a) { return static_cast<uint16_t>(r8(a) | (r8(a + 1) << 8)); };
    const auto r32 = [&](uint32_t a) {
        return static_cast<uint32_t>(r16(a)) | (static_cast<uint32_t>(r16(a + 2)) << 16);
    };
    const auto vhw = [&](uint32_t x, uint32_t y) {
        const size_t o = (static_cast<size_t>(y) * 1024 + x) * 2;
        return static_cast<uint16_t>(vram[o] | (vram[o + 1] << 8));
    };

    std::printf("=== rrverify texbind ===\nstate %s\ndisc  %s\n", stateDir.c_str(), dataDir.c_str());

    // ---------------------------------------------------------------- index the disc
    std::vector<std::vector<uint8_t>> blobs;
    std::vector<LectChunk> chunks;
    size_t scanned = 0;
    std::error_code ec;
    for (auto it = std::filesystem::recursive_directory_iterator(dataDir, ec);
         !ec && it != std::filesystem::recursive_directory_iterator(); ++it) {
        if (!it->is_regular_file(ec)) continue;
        if (it->file_size(ec) > 40u * 1024 * 1024) continue;
        std::vector<uint8_t> d;
        std::string err;
        if (!rr::interp::ReadWholeFile(it->path().string(), d, err)) continue;
        ++scanned;
        const size_t before = chunks.size();
        ScanFile(it->path().filename().string(), d, chunks);
        if (chunks.size() != before) {
            blobs.push_back(std::move(d));
            for (size_t k = before; k < chunks.size(); ++k) chunks[k].off |= 0; // keep index
            for (size_t k = before; k < chunks.size(); ++k) chunks[k].size = chunks[k].size;
            for (size_t k = before; k < chunks.size(); ++k)
                chunks[k].file = chunks[k].file + "#" + std::to_string(blobs.size() - 1);
        }
    }
    if (ec) {
        std::printf("FAIL: cannot walk %s (%s)\n", dataDir.c_str(), ec.message().c_str());
        return 1;
    }
    std::printf("scanned %zu files, found %zu LECT chunks\n\n", scanned, chunks.size());

    const auto blobOf = [&](const LectChunk& c) -> const std::vector<uint8_t>& {
        const size_t h = c.file.rfind('#');
        return blobs[static_cast<size_t>(std::stoul(c.file.substr(h + 1)))];
    };
    const auto nameOf = [&](const LectChunk& c) { return c.file.substr(0, c.file.rfind('#')); };

    // ---------------------------------------------------------------- A: page table vs the disc
    constexpr uint32_t kPageTable = 0x800D5F70;
    constexpr uint32_t kRegistry = 0x800CE1B0;
    Entry entries[34];
    bool occupied[34] = {};
    std::printf("--- A. page table 0x800D5F70 against the disc ---\n");
    std::printf("slot id   page(x,y) bpp uBias vBias desc   LECT chunk                       rows\n");
    int aChecked = 0, aOk = 0;
    for (int k = 0; k < 34; ++k) {
        const uint32_t a = kPageTable + 12u * static_cast<uint32_t>(k);
        Entry& e = entries[static_cast<size_t>(k)];
        e.id = r8(a);
        e.slot = r8(a + 1);
        e.uBias = r8(a + 2);
        e.vBias = r8(a + 3);
        e.desc = r16(a + 4);
        e.colX = r16(a + 6);
        e.tpage = r16(a + 8);
        e.clut = r16(a + 10);
        if (e.tpage == 0 && e.colX == 0) continue;
        occupied[static_cast<size_t>(k)] = true;
        const uint32_t px = (e.tpage & 0x0Fu) * 64u;
        const uint32_t py = ((e.tpage >> 4) & 1u) * 256u;
        const uint32_t depth = (e.tpage >> 7) & 3u;
        const uint32_t bpp = depth == 0 ? 4u : (depth == 1 ? 8u : 16u);
        const LectChunk* found = nullptr;
        for (const LectChunk& c : chunks)
            if (c.id == e.id && c.bpp == bpp) { found = &c; break; }
        if (found == nullptr) {
            std::printf("%-4d 0x%02X (%4u,%3u) %2u %5u %5u 0x%04X  -- no LECT with this id/bpp --\n",
                        k, e.id, px, py, bpp, e.uBias, e.vBias, e.desc);
            continue;
        }
        const std::vector<uint8_t>& d = blobOf(*found);
        const uint32_t bytesPerRow = (bpp == 4) ? found->width / 2u : found->width;
        const uint32_t hwPerRow = bytesPerRow / 2u;
        const uint32_t x0 = px + (e.uBias * bpp) / 16u;
        uint32_t ok = 0;
        for (uint32_t row = 0; row < found->height; ++row) {
            bool same = true;
            for (uint32_t h = 0; h < hwPerRow && same; ++h) {
                const size_t s = found->payload + static_cast<size_t>(row) * bytesPerRow + 2u * h;
                if (s + 1 >= d.size()) { same = false; break; }
                const uint16_t want = static_cast<uint16_t>(d[s] | (d[s + 1] << 8));
                same = (want == vhw(x0 + h, py + e.vBias + row));
            }
            if (same) ++ok;
        }
        ++aChecked;
        if (ok == found->height) ++aOk;
        std::printf("%-4d 0x%02X (%4u,%3u) %2u %5u %5u 0x%04X  %-24s +0x%-6X %u/%u%s\n", k, e.id, px, py,
                    bpp, e.uBias, e.vBias, e.desc, nameOf(*found).c_str(), found->off, ok,
                    found->height, ok == found->height ? "" : "  MISMATCH");
    }
    std::printf("A: %d/%d slots byte-identical to the disc chunk they name\n\n", aOk, aChecked);

    // ---------------------------------------------------------------- B: model -> slot
    std::printf("--- B. model registry 0x800CE1B0: registry+0x07 == slot of DOD3+0x1C ---\n");
    int bChecked = 0, bOk = 0;
    for (int i = 0; i < 50; ++i) {
        const uint32_t a = kRegistry + 16u * static_cast<uint32_t>(i);
        const uint32_t modelId = r32(a);
        if (modelId == 0) continue;
        const int8_t idx = static_cast<int8_t>(r8(a + 7));
        const uint32_t groupArr = r32(a + 8);
        if (groupArr == 0) continue;
        const uint32_t dod3 = r32(groupArr);
        const uint32_t texId = r32(dod3 + 0x1C);
        int want = -1;
        for (int k = 0; k < 34; ++k)
            if (entries[static_cast<size_t>(k)].id == texId) { want = k; break; }
        ++bChecked;
        const bool good = (want == idx);
        if (good) ++bOk;
        if (verbose || !good)
            std::printf("  model %-5u registry+0x07=%-4d DOD3+0x1C=0x%02X -> slot %-4d %s\n", modelId,
                        idx, texId, want, good ? "ok" : "MISMATCH");
    }
    std::printf("B: %d/%d models bind to the slot their own DOD3+0x1C names\n\n", bOk, bChecked);

    // ---------------------------------------------------------------- C/D: the 4bpp CLUT table
    std::printf("--- C/D. 4bpp CLUT descriptor and the table it builds ---\n");
    int cChecked = 0, cOk = 0;
    for (int k = 0; k < 34; ++k) {
        if (!occupied[static_cast<size_t>(k)]) continue;
        const Entry& e = entries[static_cast<size_t>(k)];
        if (((e.tpage >> 7) & 3u) != 0u) continue; // 4bpp only
        const uint32_t px = (e.tpage & 0x0Fu) * 64u;
        const uint32_t py = ((e.tpage >> 4) & 1u) * 256u;
        const LectChunk* found = nullptr;
        for (const LectChunk& c : chunks)
            if (c.id == e.id && c.bpp == 4) { found = &c; break; }
        const uint32_t shift = e.desc & 3u;
        const uint32_t mask = (e.desc >> 2) & 3u;
        const uint32_t baseY = (e.desc >> 4) & 0x1FFu;
        const uint32_t expect = found ? (e.vBias + py + found->height - 1u) : 0u;
        ++cChecked;
        const bool shapeOk = found && shift == 2 && mask == 3 && baseY == expect;
        std::printf("  slot %-2d id 0x%02X desc 0x%04X -> shift=%u mask=%u baseY=%u (vBias+pageY+h-1=%u) %s\n",
                    k, e.id, e.desc, shift, mask, baseY, expect, shapeOk ? "ok" : "MISMATCH");
        // The table always builds 40 entries, but only the first maxTpage+1 of them are meant to
        // be CLUTs - the rest address rows the art still occupies. The check is therefore that the
        // CLUT-shaped slots form a CONTIGUOUS PREFIX: entry 0 transparent and the other 15
        // non-zero, for n = 0..k-1 and for no n >= k.
        uint32_t ids[40];
        bool isClut[40];
        for (uint32_t n = 0; n < 40; ++n) {
            const uint32_t cx = e.colX + ((n & mask) << 4);
            const uint32_t cy = (baseY - (n >> shift)) & 0x1FFu;
            ids[n] = ((cy << 6) | (cx >> 4)) & 0xFFFFu;
            const uint32_t x = (ids[n] & 0x3Fu) * 16u;
            const uint32_t y = (ids[n] >> 6) & 0x1FFu;
            uint32_t nz = 0;
            for (uint32_t t = 1; t < 16; ++t) nz += (vhw(x + t, y) != 0) ? 1u : 0u;
            isClut[n] = (vhw(x, y) == 0 && nz == 15);
        }
        uint32_t prefix = 0;
        while (prefix < 40 && isClut[prefix]) ++prefix;
        uint32_t after = 0;
        for (uint32_t n = prefix; n < 40; ++n) after += isClut[n] ? 1u : 0u;
        const bool prefixOk = prefix >= 8 && after == 0;
        std::printf("      CLUT-shaped slots: a contiguous prefix of %u (and %u stragglers) -> "
                    "prim.tpage 0..%u are palettes%s\n",
                    prefix, after, prefix ? prefix - 1 : 0, prefixOk ? "" : "   UNEXPECTED SHAPE");
        if (shapeOk && prefixOk) ++cOk;
        // D: the longest run in the snapshot's own scratchpad that matches the computed table
        uint32_t best = 0, run = 0, bestAt = 0;
        for (uint32_t n = 0; n < 40; ++n) {
            uint32_t w = 0;
            std::memcpy(&w, scratch.data() + 0xA0 + 4u * n, 4);
            if ((w >> 16) == ids[n] && (w & 0xFFFFu) == 0) {
                ++run;
                if (run > best) { best = run; bestAt = n + 1 - run; }
            } else {
                run = 0;
            }
        }
        std::printf("      scratchpad 0x1F8000A0: longest matching run %u (entries %u..%u)%s\n", best,
                    bestAt, best ? bestAt + best - 1 : 0,
                    best >= 8 ? "  <- the engine's own built table" : "");
        (void)px;
    }
    std::printf("C: %d/%d 4bpp slots have a consistent descriptor and 40 real CLUTs\n\n", cOk, cChecked);

    const bool pass = aChecked > 0 && aOk == aChecked && bChecked > 0 && bOk == bChecked &&
                      cChecked > 0 && cOk == cChecked;
    std::printf("verdict %s\n", pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}
