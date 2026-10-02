// rrverify cellbind - which texture page every scene-cell primitive samples, checked primitive by
// primitive against the pages the ORIGINAL resolved in a captured console state.
//
// The cell loader's two fix-up passes (`SLUS_010.53 0x80033F14`, docs\formats\scene_cell.md 12)
// overwrite each band-0 / band-1 primitive's `texRef` at `+0x0A` with the tpage of the texture
// record its key resolved to. So a captured RAM image holds, for every resident cell whose passes
// have run (slot `+0x0C` bit 3 = band 0, bit 2 = band 1), the original's own answer to "which page
// does this primitive sample". This subcommand re-derives that answer from the DISC with the
// product's rule (`rr::CellTextureSlot` over the parser's key lists, rrformats/cell.h) and diffs it:
//
//   for every resident, fixed-up cell slot of `0x800D87E8`:
//     find the cell's chunk on the disc (same resource id, same vertex array as RAM), parse it with
//     `rr::ParseCellChunk` (+ `rr::AttachCellRegion7` for a type-8 cell), and check that the
//     parser's key lists are the slot's keys;
//     for every primitive of the fixed-up bands (the RAM copy at the relocated region 6 / region 7
//     pointer, walked group by group as the pass walks it):
//       texRef 0x7800 -> the fixed page `*(u16*)0x800D6160`;
//       otherwise      -> key = keys[CellTextureSlot(keys, texRef)], the slot's record for that key,
//                         its page index w0, and the tpage the resolver reads for it
//                         (band 1 `0x800D7710 + (bank*3 + w0)*8 + 4`, band 0 `0x800D76D0 + (bank*4 +
//                         w0)*8 + 4`, `0x80022758` / `0x8002289C`);
//       it must equal the tpage the console wrote at `+0x0A`.
//
// `--mutate` is the negative control: it resolves with the search over the keys in DISC order,
// i.e. without the descending sort `0x80033DAC` applies first - a rule that paints race 1/20's tree
// canopy with the neighbouring cell's houses. The gate requires the control to be caught.
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#include "interp/snapshot.h"
#include "rrformats/cell.h"
#include "rrformats/chunk.h"

namespace {

struct DiscCell {
    std::string file;
    size_t fileIndex = 0;
    std::vector<uint8_t> chunk;
};

// The resolver's search the way the renderer ran it before the fix: over the disc order.
int DiscOrderSlot(const std::vector<uint16_t>& keys, uint16_t texRef) {
    if (keys.empty()) return -1;
    size_t at = 0;
    while (at + 1 < keys.size() && texRef < keys[at]) ++at;
    if (keys[at] != texRef) at = 0;
    return static_cast<int>(at);
}

} // namespace

int CmdCellBind(int argc, char** argv) {
    std::string stateDir = "work\\oracle\\state\\rr-race";
    std::string dataDir = "work\\disc_us\\DATA";
    bool mutate = false, verbose = false;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--state" && i + 1 < argc) stateDir = argv[++i];
        else if (a == "--data" && i + 1 < argc) dataDir = argv[++i];
        else if (a == "--mutate") mutate = true;
        else if (a == "--verbose") verbose = true;
        else { std::printf("cellbind: unknown option %s\n", a.c_str()); return 2; }
    }
    std::vector<uint8_t> ram;
    std::string error;
    if (!rr::interp::ReadWholeFile((std::filesystem::path(stateDir) / "ram.bin").string(), ram, error) ||
        ram.size() != 2u * 1024 * 1024) {
        std::printf("FAIL: %s\n", error.empty() ? "ram.bin is not 2 MiB" : error.c_str());
        return 1;
    }
    const auto r8 = [&](uint32_t a) { return ram[a & 0x1FFFFF]; };
    const auto r16 = [&](uint32_t a) { return static_cast<uint16_t>(r8(a) | (r8(a + 1) << 8)); };
    const auto r32 = [&](uint32_t a) { return static_cast<uint32_t>(r16(a)) | (static_cast<uint32_t>(r16(a + 2)) << 16); };

    std::printf("=== rrverify cellbind%s ===\nstate %s\ndisc  %s\n", mutate ? " (negative control: disc-order search)" : "",
                stateDir.c_str(), dataDir.c_str());

    // ---------------------------------------------------------------- the resident, fixed-up slots
    constexpr uint32_t kSlots = 0x800D87E8, kSlotSize = 112, kRecords = 0x800D9268;
    std::map<uint32_t, std::vector<int>> slotsById;
    for (int s = 0; s < 24; ++s) {
        const uint32_t a = kSlots + kSlotSize * static_cast<uint32_t>(s);
        if (r32(a + 4) == 0 || (r32(a + 0x0C) & 0xCu) == 0) continue;
        slotsById[r32(a) & 0x0FFFFFFFu].push_back(s);
    }

    // ---------------------------------------------------------------- the disc: those ids' chunks
    // Every .STR and .STP: chunks of 0x4000, from 0 in a stream and from 0x800 in a race file.
    std::vector<std::string> files;
    std::map<uint32_t, std::vector<DiscCell>> cellsById, nineById;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(dataDir, ec)) {
        const std::string ext = entry.path().extension().string();
        const bool str = ext == ".STR" || ext == ".str", stp = ext == ".STP" || ext == ".stp";
        if (!str && !stp) continue;
        std::vector<uint8_t> d;
        std::string err;
        if (!rr::interp::ReadWholeFile(entry.path().string(), d, err)) continue;
        files.push_back(entry.path().filename().string());
        for (size_t off = stp ? 0x800 : 0; off + rr::kChunkSize <= d.size(); off += rr::kChunkSize) {
            const uint32_t key = static_cast<uint32_t>(d[off]) | (static_cast<uint32_t>(d[off + 1]) << 8) |
                                 (static_cast<uint32_t>(d[off + 2]) << 16) | (static_cast<uint32_t>(d[off + 3]) << 24);
            const uint32_t id = key & 0x0FFFFFFFu, type = key >> 28;
            if (!slotsById.count(id) || (type != 0 && type != 8 && type != 9)) continue;
            DiscCell c;
            c.file = files.back();
            c.fileIndex = files.size() - 1;
            c.chunk.assign(d.begin() + static_cast<std::ptrdiff_t>(off),
                           d.begin() + static_cast<std::ptrdiff_t>(off + rr::kChunkSize));
            (type == 9 ? nineById : cellsById)[id].push_back(std::move(c));
        }
    }
    if (ec) {
        std::printf("FAIL: cannot list %s (%s)\n", dataDir.c_str(), ec.message().c_str());
        return 1;
    }
    std::printf("resident fixed-up cells: %zu ids; scanned %zu stream files\n\n", slotsById.size(), files.size());

    size_t cellsChecked = 0, cellsUnmatched = 0, keyListDiffer = 0;
    size_t prims[2] = {}, bad[2] = {}, fixedPage[2] = {}, discriminating[2] = {};
    for (const auto& [id, slots] : slotsById) {
        for (int s : slots) {
            const uint32_t slot = kSlots + kSlotSize * static_cast<uint32_t>(s);
            const uint32_t body = r32(slot + 4), flags = r32(slot + 0x0C);
            // The chunk this slot was loaded from: same id and a vertex array equal to RAM's.
            const uint32_t ramVerts = r32(body + 0x34) + 4;
            const rr::CellData* found = nullptr;
            rr::CellData parsed;
            std::string from;
            for (const DiscCell& c : cellsById[id]) {
                try {
                    rr::CellData cell = rr::ParseCellChunk(c.chunk);
                    bool same = cell.countA == r16(body + 4) && cell.countB == r16(body + 6);
                    for (size_t v = 0; same && v < cell.vertexCount; ++v) {
                        const uint32_t at = ramVerts + 8u * static_cast<uint32_t>(v);
                        same = static_cast<int16_t>(r16(at)) == cell.vertexX[v] &&
                               static_cast<int16_t>(r16(at + 2)) == cell.vertexY[v] &&
                               static_cast<int16_t>(r16(at + 4)) == cell.vertexZ[v] &&
                               static_cast<int16_t>(r16(at + 6)) == cell.vertexW[v];
                    }
                    if (!same) continue;
                    if (cell.header.type == 8 && (flags & 4u))
                        for (const DiscCell& nine : nineById[id])
                            if (nine.fileIndex == c.fileIndex) {
                                rr::AttachCellRegion7(cell, nine.chunk);
                                break;
                            }
                    parsed = std::move(cell);
                    found = &parsed;
                    from = c.file;
                    break;
                } catch (const std::exception&) {
                }
            }
            if (!found) {
                ++cellsUnmatched;
                std::printf("slot %2d id %08X: no chunk on the disc with this id and RAM's vertex array\n", s, id);
                continue;
            }
            ++cellsChecked;
            for (int band : {0, 1}) {
                if (!(flags & (band == 0 ? 8u : 4u))) continue;
                if (band == 1 && !found->region7Present) {
                    std::printf("slot %2d id %08X: band 1 fixed up but its region 7 was not found in %s\n", s, id,
                                from.c_str());
                    ++cellsUnmatched;
                    continue;
                }
                // The slot's keys and records: band 1 +0x50/+0x54 -> +0x68/+0x6C, band 0 +0x58/+0x5C -> +0x60/+0x64.
                const uint32_t keyOff = band == 0 ? 0x58 : 0x50, recOff = band == 0 ? 0x60 : 0x68;
                const std::vector<uint16_t>& keys = band == 0 ? found->texKeyBand0 : found->texKeyBand1;
                std::vector<uint16_t> slotKeys;
                for (uint32_t k = 0; k < 2; ++k) {
                    const uint32_t v = r32(slot + keyOff + 4 * k);
                    if (v != 0 && v != 0xFFFFFFFFu) slotKeys.push_back(static_cast<uint16_t>(v));
                }
                if (slotKeys != keys) {
                    ++keyListDiffer;
                    std::printf("slot %2d id %08X band %d: the parser's keys are not the slot's\n", s, id, band);
                }
                const auto pageOfKey = [&](uint16_t key, bool& ok) -> uint16_t {
                    ok = false;
                    for (uint32_t k = 0; k < 2; ++k) {
                        if (r32(slot + keyOff + 4 * k) != key) continue;
                        const uint32_t rec = r32(slot + recOff + 4 * k);
                        if (rec < kRecords) return 0;
                        const int32_t w0 = static_cast<int32_t>(r32(rec));
                        if (w0 < 0) return 0;
                        const uint32_t bank = (rec - kRecords) / 1152u;
                        ok = true;
                        return band == 1 ? r16(0x800D7710u + (bank * 3u + static_cast<uint32_t>(w0)) * 8u + 4u)
                                         : r16(0x800D76D0u + (bank * 4u + static_cast<uint32_t>(w0)) * 8u + 4u);
                    }
                    return 0;
                };
                const std::vector<rr::CellPrimitive>& list = band == 0 ? found->band0 : found->band1;
                const uint32_t listBase = r32(body + (band == 0 ? 0x38u : 0x3Cu));
                size_t n = 0;
                size_t cellBad = 0, cellPrims = 0;
                for (size_t g = 0; g < found->groups.size(); ++g) {
                    const rr::CellPrimitiveGroup& group = found->groups[g];
                    if (group.band != band) continue;
                    uint32_t at = listBase + group.byteOffset;
                    const uint32_t count = group.triCount + group.quadCount;
                    for (uint32_t i = 0; i < count; ++i, ++n) {
                        const bool quad = i >= group.triCount;
                        const rr::CellPrimitive& prim = list.at(n);
                        const uint16_t console = r16(at + 0x0A);
                        at += quad ? 24u : 20u;
                        ++prims[band];
                        ++cellPrims;
                        uint16_t want = 0;
                        bool ok = true;
                        if (((prim.texRef >> 10) & 0x1Fu) == 30u) {
                            want = r16(0x800D6160u);
                            ++fixedPage[band];
                        } else {
                            const int product = rr::CellTextureSlot(keys, prim.texRef);
                            const int old = DiscOrderSlot(keys, prim.texRef);
                            if (product != old) ++discriminating[band];
                            const int chosen = mutate ? old : product;
                            want = chosen < 0 ? 0 : pageOfKey(keys[static_cast<size_t>(chosen)], ok);
                        }
                        if (!ok || want != console) {
                            ++bad[band];
                            ++cellBad;
                            if (verbose && cellBad <= 4)
                                std::printf("  slot %2d band %d prim %zu texRef %04X: ours tpage %04X, console %04X\n", s,
                                            band, n, prim.texRef, want, console);
                        }
                    }
                }
                std::printf("slot %2d id %08X (%s) band %d keys", s, id, from.c_str(), band);
                for (uint16_t k : keys) std::printf(" %04X", k);
                std::printf(": %zu primitives, %zu differ\n", cellPrims, cellBad);
            }
        }
    }
    const size_t total = prims[0] + prims[1], wrong = bad[0] + bad[1];
    std::printf("\ncells checked %zu, unmatched %zu, key lists differing %zu\n", cellsChecked, cellsUnmatched, keyListDiffer);
    for (int band : {0, 1})
        std::printf("band %d: %zu primitives (%zu on the fixed page 0x7800), %zu where the sorted and the disc-order "
                    "search disagree, %zu differ from the console\n",
                    band, prims[band], fixedPage[band], discriminating[band], bad[band]);
    const bool structural = cellsChecked > 0 && cellsUnmatched == 0 && keyListDiffer == 0 && total > 0;
    if (mutate) {
        const bool caught = structural && wrong > 0;
        std::printf("cellbind verdict %s\n", caught ? "PASS (negative control caught)" : "FAIL (negative control NOT caught)");
        return caught ? 0 : 1;
    }
    const bool pass = structural && wrong == 0;
    std::printf("cellbind verdict %s\n", pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}
