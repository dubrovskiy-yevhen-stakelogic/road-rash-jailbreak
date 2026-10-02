#include "game/cell_view.h"

#include <algorithm>
#include <cstdio>

#include "game/sim/bike_parts.h"
#include "game/sim/cell_draw.h"
#include "game/stream_product.h" // StreamCellReadyPorted
#include "game/cell_sort_product.h" // CellSortOn

namespace rr::game {
namespace {

constexpr uint32_t kStreamRecords = 0x80053478; // SLUS 0x8002379C: 0x80 bytes a view, +4 the entity

struct Totals {
    size_t passes = 0, listed = 0, missing = 0, emptyPasses = 0, noRecord = 0, notReady = 0;
};
Totals& T() {
    static Totals t;
    return t;
}

uint32_t Read32(const uint8_t* ram, uint32_t a) {
    const uint32_t o = a & 0x1FFFFFu;
    return static_cast<uint32_t>(ram[o]) | (static_cast<uint32_t>(ram[o + 1]) << 8) |
           (static_cast<uint32_t>(ram[o + 2]) << 16) | (static_cast<uint32_t>(ram[o + 3]) << 24);
}

} // namespace

std::string BuildRlsArena(rr::sim::GuestRam& g, const rr::DiscImage& disc, int set, uint32_t& from, uint32_t limit) {
    const std::string name = "DATA/STREAM" + std::to_string(set) + ".RLS";
    const auto f = disc.Find(name);
    if (!f) return "the cell release list was NOT loaded: " + name + " is not on the disc (every cell resident is drawn)";
    const std::vector<uint8_t> file = disc.ReadFile(*f);
    uint32_t at = (from + 3u) & ~3u;
    bool own = false;
    if (file.size() < 12) return "the cell release list was NOT loaded: " + name + " is too short";
    if (at + file.size() > limit) {
        // the object area is spent by the loader ports' arenas - the file goes where every capture holds it
        // (gp+0x1A4 = 0x801AC45C: 0x80023498's block after the route heap's GRF, route block and ROAD<n>.MAP, which
        // the route port places as rr-race has them), when nothing of the arena is there. OURS: the place.
        // RRJB_CELL_SORT=off: no release list (the renderer's residency windows).
        constexpr uint32_t kRlsAt = 0x801AC45Cu;
        bool unused = CellSortOn() && g.U32(g.gp() + rr::sim::kGpRlsTable) == 0;
        for (uint32_t k = 0; unused && k < file.size(); k += 4) unused = g.U32(kRlsAt + k) == 0;
        if (!unused) return "the cell release list was NOT loaded: " + name + " does not fit the arena";
        at = kRlsAt;
        own = true;
    }
    g.WriteBlock(at, file.data(), static_cast<uint32_t>(file.size()));
    rr::sim::RlsRelocate(g, at);                                // SLUS 0x8002428C's fix-up, PORTED
    g.W32(g.gp() + rr::sim::kGpRlsCursor, 0);                  // 0x80023498: *(0x8005B508) = 0
    g.W32(g.gp() + rr::sim::kGpRlsCursor + 4u, 0);             // *(0x8005B50C) = 0
    if (!own) from = at + static_cast<uint32_t>(file.size());
    char b[420];
    std::snprintf(b, sizeof(b),
                  "the cell release list %s (%zu bytes) is loaded as SLUS 0x80023498 / 0x8002428C load it, OURS at "
                  "0x%08X%s: each view draws the cells the record holding its player names (SLUS 0x80035F48, "
                  "PORTED, cell_view.h)",
                  name.c_str(), file.size(), at, own ? " (every capture's own address: the object area is spent)" : "");
    return b;
}

uint32_t StreamTarget(rr::sim::GuestRam& g, int p, uint32_t bike) {
    const uint32_t e = g.U32(kStreamRecords + 0x80u * static_cast<uint32_t>(p) + 4u);
    return e != 0 ? e : bike;
}

void CellDrawPass(rr::sim::GuestRam& g, int players) {
    if (g.U32(g.gp() + rr::sim::kGpRlsTable) == 0) return;
    // The page test 0x800363F0 (PORTED, cell_draw.h): a cell whose texture pages the streamer has not bound yet
    // (0x800325BC / 0x800324CC, stream_cd.h) is left out of the draw, as on the console. RRJB_CELLREADY=off or
    // RRJB_STREAM=ours (no texture bookkeeping): answered "resolved" (every cell texture is the renderer's already).
    static const bool ported = StreamCellReadyPorted();
    const rr::sim::CellReadyFn ready = [](rr::sim::GuestRam& gg, uint32_t sl) {
        if (!ported || rr::sim::CellReady(gg, sl)) return true;
        ++T().notReady;
        return false;
    };
    for (int p = 0; p < players && p < 2; ++p) {
        const int32_t n = rr::sim::CellDrawList(g, static_cast<uint32_t>(p), ready);
        if (g.Faulted()) {
            g.ClearFault();
            continue;
        }
        ++T().passes;
        if (g.U32(g.gp() + rr::sim::kGpRlsCursor + 4u * static_cast<uint32_t>(p)) == 0) {
            ++T().noRecord;
            continue;
        }
        T().listed += static_cast<size_t>(n);
        T().missing += g.U32(g.gp() + rr::sim::kGpMissingCount);
        if (n == 0) ++T().emptyPasses;
    }
}

std::vector<uint32_t> CellDrawIds(const uint8_t* ram, int p, bool& ok) {
    constexpr uint32_t gp = 0x8005AC8C; // SLUS_010.53's gp (the arena's)
    std::vector<uint32_t> ids;
    ok = Read32(ram, gp + rr::sim::kGpRlsTable) != 0;
    if (!ok) return ids;
    const uint32_t pp = static_cast<uint32_t>(p);
    const uint32_t n = Read32(ram, gp + rr::sim::kGpDrawCount + 4u * pp);
    for (uint32_t k = 0; k < n && k < 12u; ++k) {
        const uint32_t slot = Read32(ram, rr::sim::kCellDrawLists + 48u * pp + 4u * k);
        if (slot >= 24u) continue;
        // a cell whose largest depth +0x2C (slot +0x30) the ported sort left not positive is not drawn
        // by 0x80035958, nor anything filed in it (cell_sort_product.h)
        if (CellSortOn() && static_cast<int32_t>(Read32(ram, rr::sim::kCellSlots + 112u * slot + 0x30u)) <= 0) continue;
        ids.push_back(Read32(ram, rr::sim::kCellSlots + 112u * slot) & 0x0FFFFFFFu);
    }
    return ids;
}

int DrawLod(const uint8_t* ram, uint32_t obj, int p) {
    if (obj == 0 || p < 0 || p > 1) return 0;
    const int v = static_cast<int8_t>(ram[(obj + 10u + static_cast<uint32_t>(p)) & 0x1FFFFFu]);
    return v >= 0 && v < 4 ? v : 0;
}

void WriteResourceList(rr::sim::GuestRam& g, std::vector<LoadedCell> cells) {
    std::sort(cells.begin(), cells.end(), [](const LoadedCell& x, const LoadedCell& y) { return x.at < y.at; });
    const uint32_t base = g.U32(rr::sim::kResourceListPtr);
    if (base == 0) return;
    constexpr uint32_t kMax = 32; // the captures' 32 stream buffers
    uint32_t n = 0;
    for (const LoadedCell& c : cells) {
        if (n == kMax) break;
        const uint32_t rec = base + 0x2Cu + 36u * n;
        g.W32(rec + 0x00u, 0x33u);
        g.W32(rec + 0x04u, n);
        g.W32(rec + 0x08u, g.U32(c.at));
        g.W32(rec + 0x0Cu, c.at);
        g.W32(rec + 0x10u, c.at + 0x20u);
        g.W32(rec + 0x14u, c.at);
        g.W32(rec + 0x18u, c.body - 48u);
        g.W32(rec + 0x1Cu, 0);
        g.W32(rec + 0x20u, 0);
        ++n;
    }
    g.W32(base + 0xA58u, n);
}

void DrawLoopCells(rr::sim::GuestRam& g, int players) {
    constexpr uint32_t kPools = 0x800CE4D0, kViews = 0x800CD898;
    for (int p = 0; p < players && p < 2; ++p) {
        rr::sim::EntityCell(g, kViews + 1132u * static_cast<uint32_t>(p)); // 0x8008D580
        int32_t n = g.S32(g.U32(kPools + 12u));
        uint32_t e = g.U32(kPools);
        for (; n >= 0; --n, e += g.U32(kPools + 4u)) {
            if (g.S16(e + 320u) != 0) rr::sim::EntityCell(g, e);          // 0x8008D5B4
            const uint32_t rider = g.U32(e + 852u);
            if (g.U32(rider + 604u) < 3u) g.W32(rider + 176u, g.U32(e + 176u)); // 0x8008D5F4
            else if (g.S16(rider + 320u) != 0 && rr::sim::EntityCell(g, rider) > 0)
                rr::sim::RiderInstanceStores(g, g.U32(e + 852u));              // 0x8008D644 (its draw: the model pass)
            if (g.Faulted()) {
                g.ClearFault();
                return;
            }
        }
        // 0x8008D670 / 0x8008D6E0: the passenger of player 1's bike (*(0x8005B38C)) and of *(0x8005B21C), when the
        // bike's rider has +0x23C bit 4 and the passenger's rider is off (+0x25C >= 3): the cell test (its
        // result not read) and RiderInstance, with no live test.
        for (const uint32_t ptr : {0x8005B38Cu, 0x8005B21Cu}) {
            const uint32_t b = g.U32(ptr);
            if (ptr == 0x8005B21Cu && b == 0) continue;                        // 0x8008D6EC (the first is not tested)
            if ((g.U8(g.U32(b + 852u) + 572u) & 0x10u) == 0) continue;
            const uint32_t pr = g.U32(g.U32(b + 856u) + 852u);
            if (g.U32(pr + 604u) < 3u) continue;
            rr::sim::EntityCell(g, pr);
            rr::sim::RiderInstanceStores(g, g.U32(g.U32(g.U32(ptr) + 856u) + 852u));
            if (g.Faulted()) {
                g.ClearFault();
                return;
            }
        }
    }
}

bool InDrawnCell(const uint8_t* ram, uint32_t obj, int p) {
    bool ok = false;
    const std::vector<uint32_t> ids = CellDrawIds(ram, p, ok);
    if (!ok) return true;
    const uint32_t cell = Read32(ram, obj + 176u);
    for (uint32_t id : ids)
        if (id == cell) return true;
    return false;
}

bool InDrawRange(const uint8_t* ram, uint32_t obj, int p) {
    if (obj == 0 || p < 0 || p > 1) return false;
    const uint32_t dod = Read32(ram, obj);
    if (dod < 0x80000000u || dod >= 0x80200000u) return false;
    const uint32_t w = static_cast<uint32_t>(ram[(dod + 14u) & 0x1FFFFFu]) |
                       (static_cast<uint32_t>(ram[(dod + 15u) & 0x1FFFFFu]) << 8);
    const uint32_t kind = (w & 0x78u) >> 3;
    const int32_t range = static_cast<int32_t>(Read32(ram, 0x800CC6A4u + 4u * kind));
    const int32_t dist = static_cast<int32_t>(Read32(ram, obj + 44u + 4u * static_cast<uint32_t>(p)));
    bool far = range < dist;
    if (kind == 6u)
        far = static_cast<int8_t>(ram[(obj + 8u) & 0x1FFFFFu]) != 0 ? range < dist : range + 32000 < dist;
    return !far && dist >= 0;
}

std::string CellDrawTotals() {
    char b[520];
    std::snprintf(b, sizeof(b),
                  "cell draw list (SLUS 0x80035F48, PORTED, cell_view.h): %zu pass(es), %zu cell(s) listed, %zu named "
                  "by the release list but not resident (drawn by the original only once streamed in), %zu pass(es) "
                  "with an empty list, %zu with no record holding the player (the previous list kept); %zu time(s) a "
                  "resident cell was left out because its texture pages were not bound yet (0x800363F0)\n",
                  T().passes, T().listed, T().missing, T().emptyPasses, T().noRecord, T().notReady);
    return b;
}

} // namespace rr::game
