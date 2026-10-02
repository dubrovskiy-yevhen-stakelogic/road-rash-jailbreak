#include "game/sim/cell_draw.h"

namespace rr::sim {
namespace {

constexpr uint32_t kSlotBytes = 112;

uint32_t SlotAt(int32_t i) { return kCellSlots + kSlotBytes * static_cast<uint32_t>(i); }

// The record test the draw list makes three times (0x8003605C.., 0x80036100.., 0x8003618C..): the
// record's road +0x0C is the player's and from +0x0E <= along <= to +0x10 (signed halfwords).
bool Holds(GuestRam& g, uint32_t rec, int32_t road, int32_t along) {
    if (g.S16(rec + 12u) != road) return false;
    if (along < g.S16(rec + 14u)) return false;
    return !(g.S16(rec + 16u) < along);
}

} // namespace

// ============================================================================ SLUS 0x800363F0
bool CellReady(GuestRam& g, uint32_t slot) {
    for (uint32_t k = 0; k < 2; ++k) {
        if (g.U32(slot + 88u + 4u * k) == 0xFFFFFFFFu) continue;
        if (g.U32(slot + 96u + 4u * k) != 0u) continue;
        return false;
    }
    return true;
}

// ============================================================================ SLUS 0x800329BC
int32_t CellSlotOf(GuestRam& g, uint32_t id, uint32_t p) {
    const int32_t lo = static_cast<int32_t>(12u * p);
    for (int32_t i = lo; i < lo + 12; ++i)
        if (g.U32(SlotAt(i)) == id) return i;
    return -1;
}

// ============================================================================ SLUS 0x80035E60
int32_t CellResidentList(GuestRam& g, uint32_t list, uint32_t p, const CellReadyFn& ready) {
    int32_t n = 0;
    const int32_t lo = static_cast<int32_t>(12u * p);
    for (int32_t i = lo; i < lo + 12; ++i) {
        const uint32_t s = SlotAt(i);
        if (g.U32(s) == 0xFFFFFFFFu || g.U32(s + 8u) == 0xFFFFFFFFu || g.U32(s + 4u) == 0u) continue;
        if (!ready(g, s)) continue;
        g.W32(list, static_cast<uint32_t>(i));
        list += 4u;
        ++n;
    }
    return n;
}

// ============================================================================ SLUS 0x80035F48
int32_t CellDrawList(GuestRam& g, uint32_t p, const CellReadyFn& ready) {
    const uint32_t gp = g.gp();
    const uint32_t cursor = gp + kGpRlsCursor + 4u * p;
    const uint32_t list = kCellDrawLists + 48u * p;
    if (g.U32(gp + kGpRlsTable) == 0) {                                      // 0x80035F78
        const int32_t n = CellResidentList(g, list, p, ready);
        g.W32(gp + kGpDrawCount + 4u * p, static_cast<uint32_t>(n));
        g.W32(gp + kGpDrawCount2 + 4u * p, static_cast<uint32_t>(n));
    }
    const uint32_t bike = g.U32(kCellViewBikes + 4u * p);                   // 0x80035FCC
    const uint32_t rider = g.U32(bike + 852u);
    const uint32_t e = (g.U32(rider + 604u) - 3u < 2u) ? rider : bike;      // off the bike: the rider
    if (g.Faulted()) return 0;
    const uint32_t word = g.U32(e + 360u);
    if ((word >> 16) == 0) {                                                // 0x80036020
        const int32_t road = static_cast<int32_t>(word & 0xFFFFu);
        const int32_t along = g.S16(e + 370u);
        bool found = false;
        uint32_t cur = g.U32(cursor);
        if (cur != 0) {
            found = Holds(g, cur, road, along);                             // the cursor as it is
            if (!found) {
                if (road == g.S16(cur + 12u)) g.W32(cursor, cur + (g.U32(e + 364u) << 5)); // one record on
                cur = g.U32(cursor);
                if (cur != 0) found = Holds(g, cur, road, along);
            }
        }
        if (!found) {                                                       // 0x80036140: the scan
            const uint32_t table = g.U32(gp + kGpRlsTable);
            g.W32(cursor, 0);
            uint32_t rec = g.U32(table + 4u * static_cast<uint32_t>(road));
            for (int32_t n = 0; n < 140; ++n, rec += 32u) {
                if (g.S16(rec + 12u) != road) break;
                if (Holds(g, rec, road, along)) {
                    g.W32(cursor, rec);                                     // 0x80035FFC
                    break;
                }
                if (g.Faulted()) return 0;
            }
        }
    }
    if (g.Faulted()) return 0;
    if (g.U32(cursor) == 0) return 0;                                       // 0x800361EC: the list stays
    for (uint32_t k = 0; k < 12; ++k) g.W32(list + 4u * k, 0xFFFFFFFFu);
    for (int32_t k = 5; k >= 0; --k) g.W32(kCellMissing + 4u * static_cast<uint32_t>(k), 0xFFFFFFFFu);
    g.W32(gp + kGpDrawCount + 4u * p, 0);
    g.W32(gp + kGpDrawTotal, 0);
    g.W32(gp + kGpMissingCount, 0);
    for (uint32_t k = 0; k < 6; ++k) {                                      // 0x80036284
        const uint32_t key = g.U16(g.U32(cursor) + 18u + 2u * k);
        if (key == 0xFFFFu) break;
        const uint32_t id = ((key & 0x8000u) << 16) | ((key & 0x7C00u) << 13) | (key & 0x3FFu);
        if (id == 0xFFFFFFFFu) continue;
        const int32_t slot = CellSlotOf(g, id & 0x0FFFFFFFu, p);
        bool drawn = false;
        if (slot != -1) {
            const uint32_t s = SlotAt(slot);
            drawn = g.U32(s + 8u) != 0xFFFFFFFFu && g.U32(s + 4u) != 0u && ready(g, s);
        }
        if (!drawn) {
            const uint32_t m = g.U32(gp + kGpMissingCount);
            g.W32(kCellMissing + 4u * m, id);
            g.W32(gp + kGpMissingCount, m + 1u);
        } else {
            const uint32_t c = g.U32(gp + kGpDrawCount + 4u * p);
            g.W32(list + 4u * c, static_cast<uint32_t>(slot));
            g.W32(gp + kGpDrawCount + 4u * p, c + 1u);
        }
    }
    const uint32_t n = g.U32(gp + kGpDrawCount + 4u * p);                   // 0x80036384
    g.W32(gp + kGpDrawCount2 + 4u * p, n);
    g.W32(gp + kGpDrawTotal, n + g.U32(gp + kGpMissingCount));
    return g.Faulted() ? 0 : static_cast<int32_t>(n);
}

// ============================================================================ SLUS 0x8002428C (fix-up)
void RlsRelocate(GuestRam& g, uint32_t file) {
    const uint32_t gp = g.gp();
    g.W32(gp + kGpRlsFile, file);                                           // 0x8002431C
    const uint32_t n = (static_cast<uint32_t>(g.U8(file + 4u)) - 8u) >> 2;
    g.W32(gp + kGpRlsTable, file + 8u);
    for (uint32_t k = 0; k < n; ++k) g.W32(file + 8u + 4u * k, g.U32(file + 8u + 4u * k) + file);
}

// ============================================================================ SLUS 0x80030500
int32_t CellAt(GuestRam& g, uint32_t road, int32_t d) {
    const uint32_t base = g.U32(kResourceListPtr);
    const int32_t n = g.S32(base + 0xA58u);
    uint32_t flags = base + 44u, rec = base + 64u;                          // +0x2C and +0x40 of record 0
    for (int32_t i = 0; i < n; ++i, flags += 36u, rec += 36u) {
        if ((g.U32(flags) & 0x12u) != 0x12u) continue;
        const uint32_t type = g.U32(g.U32(rec)) >> 28;
        if (type != 0 && type != 8) continue;
        uint32_t ext = g.U32(rec + 4u);
        for (uint32_t k = 0; k < 4; ++k, ext += 12u) {
            const uint32_t w = g.U32(ext);
            if (w == 0xFFFFFFFFu) break;
            if (road == w && !(d < g.S32(ext + 4u)) && d < g.S32(ext + 8u))
                return static_cast<int32_t>(g.U32(g.U32(rec)) & 0x0FFFFFFFu);
        }
        if (g.Faulted()) return -1;
    }
    return -1;
}

// ============================================================================ SLUS 0x80030410
int32_t PositionCell(GuestRam& g, uint32_t h, int32_t radius) {
    constexpr uint32_t kView0 = 0x800CD898;
    const uint32_t word = g.U32(h + 188u);
    const uint32_t camRoad = g.U16(kView0 + 360u);
    if ((word >> 16) == 0) {
        int32_t d = g.S32(h + 196u) >> 10;
        const int32_t r = static_cast<int16_t>(static_cast<uint16_t>(radius));
        if (r != 0 && (word & 0xFFFFu) == camRoad) {
            const int32_t c = g.S32(kView0 + 368u) >> 10;
            bool past;
            if (d < c) {
                d += r;
                past = c < d;
            } else {
                d -= r;
                past = d < c;
            }
            if (past) d = c;
        }
        return CellAt(g, g.U16(h + 188u), d);
    }
    const uint32_t graph = g.U32(g.gp() + 472u);
    const uint32_t jn = g.U32(graph + 20u) + 40u * (word & 0xFFFFu);       // 0x800245F4
    const uint32_t road = g.U32(jn + 8u);
    const uint32_t rec = g.U32(graph + 24u) + 16u * road;                    // 0x800245DC
    const int32_t d = g.S32(jn + 12u) > 0 ? 1 : static_cast<int32_t>(g.U32(rec + 4u) - 1u);
    return CellAt(g, road, d);
}

// ============================================================================ RASHCDG 0x8008B99C
int32_t EntityCell(GuestRam& g, uint32_t e) {
    const uint32_t h = g.U16(e + 172u);
    const uint32_t kind = h >> 5;
    int32_t r = 0;
    if (kind != 6u && (kind != 4u || (h & 0x1Fu) < 30u)) {
        const uint32_t sh = (g.U16(g.U32(e) + 14u) >> 12) & 31u;
        r = static_cast<int16_t>(static_cast<uint16_t>(static_cast<uint32_t>(g.S32(e + 40u) >> sh)));
    }
    const int32_t cell = PositionCell(g, e + 172u, r);
    g.W32(e + 176u, static_cast<uint32_t>(cell));
    return cell;
}

} // namespace rr::sim
