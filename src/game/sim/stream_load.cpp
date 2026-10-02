// The streamer's loaders (stream.h): cells, road pieces, textures, panoramas, and the loader's set-up.
// Transcribed from our own disassembly of SLUS_010.53 (SHA-1 67ed165a2c517d4e6106fb0dfa324d66dd9a76f1),
// RASHCDG.BIN (SHA-1 cfe43a7786759f2cb9c57751cf99e84d1074782c: CellRelease 0x8008C45C) and RASHCDI.BIN (the
// set-up); each function accepted by its rrverify row (tools\rrverify\rows_stream.inc).
#include "game/sim/stream.h"

#include "game/sim/cell_draw.h"

namespace rr::sim {

namespace {

using rc::S;
using rc::U;

uint32_t Cur(GuestRam& g) { return g.U32(g.gp() + kGpStCur); }
uint32_t Gs(GuestRam& g) { return g.U32(kStGameState); }
uint32_t Players(GuestRam& g) { return g.U32(Gs(g) + 0x30u); }
bool Call(RecoverCallees& c, uint32_t fn, std::initializer_list<uint32_t> a, uint32_t sp, uint32_t* v0 = nullptr) {
    return rc::Call(c, fn, a, sp, v0);
}

constexpr uint32_t kTagTable = 0x8005AEEC; // SLUS data: GRPT, SUBT, SLCT, XSIH, ... NMBD, 8 bytes apart
constexpr uint32_t kTagField[12] = {0x2C, 0x30, 0x34, 0x38, 0x3C, 0x40, 0x44, 0x48, 0x5C, 0x60, 0x64, 0x68};

// SLUS 0x8003D7C4(h, d) / 0x8003D7A8(e, d): a cursor's / an entity's pointers into a moved object rebased.
void RebaseCursor(GuestRam& g, uint32_t h, uint32_t d) {
    const uint32_t a = g.U32(h + 200u);
    for (uint32_t o = 0x9Cu; o <= 0xA8u; o += 4u) g.W32(h + o, g.U32(h + o) + d);
    g.W32(h + 200u, a != 0 ? a + d : 0u);
    for (uint32_t o = 0xCCu; o <= 0xD4u; o += 4u) {
        const uint32_t v = g.U32(h + o);
        g.W32(h + o, v != 0 ? v + d : 0u);
    }
}
void RebaseEntity(GuestRam& g, uint32_t e, uint32_t d) {
    const uint32_t v = g.U32(e + 0x1ECu);
    g.W32(e + 0x1ECu, v != 0 ? v + d : 0u);
}

uint32_t PoolBase(GuestRam& g, uint32_t k) { return g.U32(0x800CE4D0u + 16u * k); }
uint32_t PoolStride(GuestRam& g, uint32_t k) { return g.U32(0x800CE4D4u + 16u * k); }
int32_t PoolHigh(GuestRam& g, uint32_t k) { return g.S32(g.U32(0x800CE4DCu + 16u * k)); }

} // namespace

// ============================================================================ cells
uint32_t CellLoad(GuestRam& g, uint32_t payload, uint32_t id, uint32_t type, uint32_t ext, uint32_t entry,
                  uint32_t p, uint32_t sp, RecoverCallees& c) {
    const uint32_t csp = sp - 56u;
    const uint32_t id28 = id & 0x0FFFFFFFu;
    if (id28 == 0xFFFFFFFFu) return 2;
    if (!(g.S32(g.gp() + kGpStCellCount) < 24)) return 0;
    int32_t i = 0;
    if (type == 8 || type == 0) {
        if (CellSlotOf(g, id28, p) != -1) return 3;
        i = CellSlotOf(g, 0xFFFFFFFFu, p);
        if (i == -1) return 0;
        const uint32_t slot = kStCellSlots + 0x70u * U(i);
        g.W32(slot, id);
        if (g.U32(slot + 8u) == 0xFFFFFFFFu) {
            g.W32(slot + 8u, id);
            g.W32(slot + 12u, 0);
            g.W32(slot + 0x4Cu, ext);
            CellFix(g, slot, payload);
            g.W32(g.gp() + kGpStCellCount, g.U32(g.gp() + kGpStCellCount) + 1u);
            if (!Call(c, kStRegionZeroFn, {slot + 4u, p}, csp)) return 0;
            const uint32_t f = g.U32(slot + 12u);
            g.W32(slot + 12u, f | 1u);
            if (type == 0) g.W32(slot + 12u, f | 3u);
            else g.W32(g.U32(slot + 4u) + 0x3Cu, 0);
            g.W32(slot + 0x44u, entry);
        }
    } else {
        i = CellSlotOf(g, id28, p);
        if (i == -1) return 0;
        const uint32_t slot = kStCellSlots + 0x70u * U(i);
        if (g.U32(slot + 4u) == 0) return 0;
        if ((g.U32(slot + 12u) & 2u) != 0) return 3;
        g.W32(g.U32(slot + 4u) + 0x3Cu, payload); // 0x80032338
        g.W32(slot + 12u, g.U32(slot + 12u) | 2u);
    }
    const uint32_t slot = kStCellSlots + 0x70u * U(i);
    Call(c, kStCellTexFn, {slot, p}, csp);
    Call(c, kStCellTexPassFn, {slot, 0, p}, csp);
    Call(c, kStCellTexPassFn, {slot, 1, p}, csp);
    return 1;
}

void CellFix(GuestRam& g, uint32_t slot, uint32_t payload) {
    const int32_t n = S((g.U32(payload) - 13u) * 2u);
    int32_t a = 0, b = 0;
    uint32_t pa = slot, pb = slot, w = payload + 4u;
    for (int32_t k = 0; k < n; ++k, w += 2u) {
        const int32_t v = g.S16(w);
        if (v == -1) continue;
        if ((v & 0x8000) != 0) {
            if (a < 2) {
                g.W32(pa + 0x58u, g.U16(w));
                pa += 4u;
                ++a;
            }
        } else if (b < 2) {
            g.W32(pb + 0x50u, g.U16(w));
            pb += 4u;
            ++b;
        }
    }
    const uint32_t body = payload + g.U32(payload) * 4u;
    g.W32(slot + 4u, body);
    g.W8(slot + 0x48u, static_cast<uint8_t>(g.U8(body + 6u) + 1u));
    g.W8(slot + 0x49u, static_cast<uint8_t>(g.U8(body + 4u) + g.U16(body + 6u) * 2u));
    g.W32(body + 0x24u, payload + g.U32(body + 0x24u));
    for (uint32_t o : {0x20u, 0x28u, 0x2Cu, 0x30u, 0x34u, 0x38u, 0x3Cu}) {
        const uint32_t b2 = g.U32(slot + 4u);
        g.W32(b2 + o, payload + g.U32(b2 + o));
    }
}

uint32_t CellUnload(GuestRam& g, uint32_t id, uint32_t type, uint32_t p, uint32_t sp, RecoverCallees& c) {
    const uint32_t csp = sp - 40u;
    for (int32_t i = S(12u * p); i < S(12u * p + 12u); ++i) {
        const uint32_t slot = kStCellSlots + 0x70u * U(i);
        if (id != g.U32(slot + 8u)) continue;
        if (type == 9) {
            g.W32(g.U32(slot + 4u) + 0x3Cu, 0);
            g.W32(slot + 12u, g.U32(slot + 12u) & 0xFFFFFFA9u);
            continue;
        }
        CellRelease(g, id, p, csp, c);
        g.W32(slot + 4u, 0);
        g.W32(slot + 8u, 0xFFFFFFFFu);
        g.W32(slot + 12u, 0);
        g.W32(slot + 0x38u, 0);
        g.W32(slot, 0xFFFFFFFFu);
        g.W32(slot + 0x4Cu, 0);
        for (uint32_t j = 0; j < 2u; ++j) {
            g.W32(slot + 0x58u + 4u * j, 0xFFFFFFFFu);
            g.W32(slot + 0x50u + 4u * j, 0xFFFFFFFFu);
            g.W32(slot + 0x60u + 4u * j, 0);
            g.W32(slot + 0x68u + 4u * j, 0);
        }
        g.W32(g.gp() + kGpStCellCount, g.U32(g.gp() + kGpStCellCount) - 1u);
        return 1;
    }
    return 0;
}

void CellRelease(GuestRam& g, uint32_t id, uint32_t p, uint32_t sp, RecoverCallees& c) {
    const uint32_t csp = sp - 64u;
    uint32_t other = 0;
    if ((g.U8(Gs(g) + 4u) & 0x10u) != 0) {
        if (!Call(c, kStOtherSlotFn, {id, p}, csp, &other)) return;
        Call(c, kStJailCellFn, {id, p}, csp);
    }
    if (other == 0) {
        // pool 0: the bikes in the cell go to sleep, then the downed riders (and passengers)
        uint32_t e = PoolBase(g, 0);
        for (int32_t n = PoolHigh(g, 0); n >= 0; --n) {
            if (!(g.U16(e + 0xACu) < Players(g)) && g.U32(e + 0xB0u) == id && g.S16(e + 0x140u) != 0)
                Call(c, kStSleepFn, {e, 1}, csp);
            e += PoolStride(g, 0);
        }
        e = PoolBase(g, 0);
        for (int32_t n = PoolHigh(g, 0); n >= 0; --n) {
            const uint32_t h = g.U16(e + 0xACu);
            uint32_t r = g.U32(e + 0x354u);
            if (((h >> 5) != 1u || !(S(h & 0x1Fu) < S(Players(g)))) && g.U32(r + 0x25Cu) - 3u < 2u &&
                g.U32(r + 0xB0u) == id && g.S16(r + 0x140u) != 0)
                Call(c, kStRiderSleepFn, {r, 1}, csp);
            if ((g.U8(g.U32(e + 0x354u) + 0x23Cu) & 0x10u) != 0) {
                r = g.U32(g.U32(e + 0x358u) + 0x354u);
                if (g.U32(r + 0x25Cu) - 3u < 2u && g.U32(r + 0xB0u) == id && g.S16(r + 0x140u) != 0)
                    Call(c, kStRiderSleepFn, {r, 1}, csp);
            }
            e += PoolStride(g, 0);
        }
        // the traffic (512-byte records from 0x800CF660; control 0x800CF650: live, next free, high)
        constexpr uint32_t kCars = 0x800CF660, kCarCtl = 0x800CF650;
        uint32_t car = kCars;
        for (int32_t n = g.S32(kCarCtl + 8u); n >= 0; --n, car += 512u) {
            const uint32_t h = car + 0xACu;
            if (g.U16(h) == 0 || g.U32(h + 4u) != id) continue;
            if (g.U32(h + 8u) == 0 && ((g.U32(car + 0x24u) >> 27) & 1u) != 0) {
                Call(c, kStFxStopFn, {car}, csp);
                Call(c, kStFxFreeFn, {car}, csp);
            }
            const uint32_t slot = g.U16(h) & 0x1Fu;
            g.W16(h + 0x94u, 0);
            if (g.S32(kCarCtl) > 0) g.W32(kCarCtl, g.U32(kCarCtl) - 1u);
            if (g.U32(kCarCtl + 8u) == slot) {
                do {
                    g.W32(kCarCtl + 8u, g.U32(kCarCtl + 8u) - 1u);
                    if (g.S32(kCarCtl + 8u) < 0) break;
                } while (g.U16(kCars + 0xACu + 512u * g.U32(kCarCtl + 8u)) == 0);
            }
            if (S(slot) < g.S32(kCarCtl + 4u)) g.W32(kCarCtl + 4u, slot);
        }
        // the pedestrians (*(0x800D4B80), 0x23C bytes; control 0x800D4B70)
        constexpr uint32_t kPedCtl = 0x800D4B70;
        if (g.U32(kPedCtl + 16u) != 0 && g.S32(kPedCtl + 8u) >= 0) {
            uint32_t ped = g.U32(kPedCtl + 16u);
            for (int32_t n = g.S32(kPedCtl + 8u); n >= 0; --n, ped += 0x23Cu) {
                const uint32_t h = ped + 0xACu;
                if (g.U16(h) == 0 || g.U32(h + 4u) != id) continue;
                g.W16(h + 0x94u, 0);
                Call(c, kStPedFreeFn, {ped}, csp);
                const uint32_t slot = g.U16(h) & 0x1Fu;
                if (g.S32(kPedCtl) > 0) g.W32(kPedCtl, g.U32(kPedCtl) - 1u);
                if (g.U32(kPedCtl + 8u) == slot) {
                    do {
                        g.W32(kPedCtl + 8u, g.U32(kPedCtl + 8u) - 1u);
                        if (g.S32(kPedCtl + 8u) < 0) break;
                    } while (g.S16(g.U32(kPedCtl + 16u) + g.U32(kPedCtl + 8u) * 0x23Cu + 0xACu) == 0);
                }
                if (S(slot) < g.S32(kPedCtl + 4u)) g.W32(kPedCtl + 4u, slot);
                g.W16(h, 0);
            }
        }
        // pools 4 and 5, the props (their store records freed too)
        auto props = [&](uint32_t ctl, int32_t stride) {
            if (g.S32(ctl + 8u) < 0) return;
            uint32_t h = g.U32(ctl + 12u) + 0xACu;
            for (int32_t n = g.S32(ctl + 8u); n >= 0; --n, h += U(stride)) {
                if (g.U16(h) == 0 || g.U32(h + 4u) != id) continue;
                g.W16(h + 0x94u, 0);
                const uint32_t slot = g.U16(h) & 0x1Fu;
                if (g.S32(ctl) > 0) g.W32(ctl, g.U32(ctl) - 1u);
                if (g.U32(ctl + 8u) == slot) {
                    do {
                        g.W32(ctl + 8u, g.U32(ctl + 8u) - 1u);
                        g.W32(0x800D1814u, g.U32(0x800D1814u) + U(stride < 0 ? -stride : stride));
                        if (g.S32(ctl + 8u) < 0) break;
                    } while (g.S16(g.U32(ctl + 12u) + g.U32(ctl + 8u) * U(stride) + 0xACu) == 0);
                }
                if (S(slot) < g.S32(ctl + 4u)) g.W32(ctl + 4u, slot);
                const uint32_t store = g.U32(h - 0xA8u);
                g.W32(store, 0);
                const int32_t idx = S((store + 0x7FF2E99Cu) * 0xAAAAAAABu) >> 3;
                if (idx < g.S32(0x800D1660u)) g.W32(0x800D1660u, U(idx));
                g.W16(h, 0);
            }
        };
        props(0x800CD6C8u, 0x254);
        props(0x800CE598u, -0x1C4);
    }
    // pool 6, the collision volumes: player p's bit dropped, the volume freed with the last bit
    constexpr uint32_t kVolCtl = 0x800CD6A8;
    if (g.U32(kVolCtl + 28u) != 0 && g.S32(kVolCtl + 8u) >= 0) {
        const uint32_t bit = 1u << (p & 31u);
        uint32_t v = g.U32(kVolCtl + 28u);
        for (int32_t n = g.S32(kVolCtl + 8u); n >= 0; --n, v += 0x118u) {
            if (g.U16(v) == 0 || g.U32(v + 4u) != id) continue;
            const uint32_t m = g.U8(v + 3u);
            if ((m & bit) == 0) continue;
            if (m == (bit & 0xFFu)) {
                if (other != 0) Call(c, kStStampFn, {other, 6, g.U8(v + 2u), 0}, csp);
                g.W8(v + 0x94u, 0);
                g.W8(v + 0x95u, 0);
                const uint32_t slot = g.U16(v) & 0x1Fu;
                if (g.S32(kVolCtl) > 0) g.W32(kVolCtl, g.U32(kVolCtl) - 1u);
                if (g.U32(kVolCtl + 8u) == slot) {
                    do {
                        g.W32(kVolCtl + 8u, g.U32(kVolCtl + 8u) - 1u);
                        if (g.S32(kVolCtl + 8u) < 0) break;
                    } while (g.S16(g.U32(kVolCtl + 28u) + g.U32(kVolCtl + 8u) * 0x118u) == 0);
                }
                if (S(slot) < g.S32(kVolCtl + 4u)) g.W32(kVolCtl + 4u, slot);
                g.W16(v, 0);
            } else {
                g.W8(v + 3u, static_cast<uint8_t>(m & (1u << ((p ^ 1u) & 31u))));
            }
        }
    }
}

// ============================================================================ road pieces
uint32_t PieceLoad(GuestRam& g, uint32_t payload, uint32_t id, uint32_t p, uint32_t sp, RecoverCallees& c) {
    const uint32_t csp = sp - 32u;
    id &= 0xFFFFu;
    if (!(S(id) < g.S16(g.U32(kRoadGraphPtr) + 0x28u))) return 2;
    const int32_t v = PieceAdd(g, id, payload, p);
    if (v < 1) return U(v < 0 ? -v : v);
    if (PieceBind(g, payload, id) == 0) return 0;
    const uint32_t p1 = g.U32(kStPlayer1);
    uint32_t p2 = 0;
    if (p1 == 0) {
        p2 = g.U32(kStPlayer2);
        if (p2 == 0) return 1;
    } else {
        if (g.U32(g.U32(p1 + 0x43Cu) + 0x28u) == 0) goto passes;
        p2 = g.U32(kStPlayer2);
        if (p2 == 0) return 1;
    }
    if (g.U32(g.U32(p2 + 0x43Cu) + 0x28u) != 0) return 1;
passes:
    Call(c, kStActivateFn, {}, csp);
    Call(c, kStDownedFn, {}, csp);
    return 1;
}

int32_t PieceAdd(GuestRam& g, uint32_t id, uint32_t obj, uint32_t half) {
    int32_t free = -1;
    for (int32_t k = 0; k < 6; ++k) {
        const uint32_t slot = kStPieces + 16u * U(k);
        if (g.U32(slot) == id) {
            if ((g.U8(Gs(g) + 4u) & 0x10u) != 0 && half != g.U32(slot + 4u)) {
                g.W32(slot + 8u + 4u * half, obj);
                return -1;
            }
            return -3;
        }
        if (free == -1 && g.U32(slot) == 0xFFFFFFFFu) free = k;
    }
    if (free == -1) return 0;
    const uint32_t slot = kStPieces + 16u * U(free);
    g.W32(slot, id);
    g.W32(slot + 4u, half);
    g.W32(slot + 8u + 4u * half, obj);
    g.W32(slot + 8u + 4u * (half ^ 1u), 0);
    g.W32(kStPiecesUsed, g.U32(kStPiecesUsed) + 1u);
    if (g.S32(kStPiecesLast) <= free) g.W32(kStPiecesLast, U(free));
    return 1;
}

uint32_t PieceBind(GuestRam& g, uint32_t obj, uint32_t id) {
    const uint32_t btt = RoadBttRecord(g, static_cast<int16_t>(id));
    if (btt == 0) return 0;
    for (uint32_t blk = obj + 0x6Cu; blk < obj + 0x4000u; blk += g.U32(blk + 4u)) {
        int32_t hit = -1;
        for (int32_t k = 0; k < 12 && hit < 0; ++k) {
            bool same = true;
            for (uint32_t b = 0; b < 4u; ++b) { // SLUS 0x80044874 strncmp(blk, tag, 4)
                const uint8_t x = g.U8(blk + b), y = g.U8(kTagTable + 8u * U(k) + b);
                if (x != y) {
                    same = false;
                    break;
                }
                if (x == 0) break;
            }
            if (same) hit = k;
        }
        if (hit < 0) break;
        g.W32(obj + kTagField[hit], blk + 8u);
    }
    g.W32(btt + 12u, obj);
    if (g.U32(kStFirstPiece) == 0) g.W32(kStFirstPiece, obj);
    if ((g.U8(Gs(g) + 4u) & 0x10u) != 0) {
        if (obj == 0) return 1;
        if (g.U32(obj) == 0xBu) {
            uint32_t b = g.U32(obj + 0x60u);
            g.W32(b + 0x620u, 0x0B897B7Du); g.W32(b + 0x624u, 0xFFF0DC54u); g.W32(b + 0x628u, 0x1538BC84u);
            b = g.U32(obj + 0x60u);
            g.W32(b + 0x198u, 0x0B775E29u); g.W32(b + 0x19Cu, 0xFFF0BB49u); g.W32(b + 0x1A0u, 0x1515ACCFu);
            b = g.U32(obj + 0x60u);
            g.W32(b + 0x3F0u, 0x0B38FCECu); g.W32(b + 0x3F4u, 0xFFF0BB49u); g.W32(b + 0x3F8u, 0x15361E79u);
            g.W32(g.U32(obj + 0x60u) + 0x55Cu, g.U32(g.U32(obj + 0x60u) + 0x568u));
        }
    }
    if (obj == 0 || g.U32(obj) != 0x69u) return 1;
    uint32_t b = g.U32(obj + 0x60u);
    g.W32(b + 0x9E0u, 0xED2431ADu); g.W32(b + 0x9E4u, 0xFE062800u); g.W32(b + 0x9E8u, 0xEBC76784u);
    b = g.U32(obj + 0x60u);
    g.W32(b + 0x9C4u, 0xED2431ADu); g.W32(b + 0x9C8u, 0xFE062800u); g.W32(b + 0x9CCu, 0xEBC76784u);
    g.W32(b + 0x9B8u, 0xED08D8CAu); g.W32(b + 0x9BCu, 0xFE0A3D4Du); g.W32(b + 0x9C0u, 0xEBC6469Du);
    b = g.U32(obj + 0x60u);
    g.W32(b + 0x99Cu, 0xED08D8CAu); g.W32(b + 0x9A0u, 0xFE0A3D4Du); g.W32(b + 0x9A4u, 0xEBC6469Du);
    g.W32(b + 0x990u, 0xECED651Du); g.W32(b + 0x994u, 0xFE09E617u); g.W32(b + 0x998u, 0xEBBF1AECu);
    return 1;
}

int32_t PieceDrop(GuestRam& g, uint32_t id, uint32_t half) {
    const int32_t last = g.S32(kStPiecesLast);
    if (last < 0) return -1;
    for (int32_t k = 0; k <= g.S32(kStPiecesLast); ++k) {
        const uint32_t slot = kStPieces + 16u * U(k);
        if (g.U32(slot) == 0xFFFFFFFFu || g.U32(slot) != id) continue;
        if (g.U32(slot + 8u + 4u * (half ^ 1u)) != 0) {
            g.W32(slot + 8u + 4u * half, 0);
            if (g.U32(slot + 4u) != half) return 6;
            g.W32(slot + 4u, half ^ 1u);
            return k;
        }
        g.W32(slot, 0xFFFFFFFFu);
        g.W32(slot + 8u, 0);
        g.W32(slot + 12u, 0);
        g.W32(kStPiecesUsed, g.U32(kStPiecesUsed) - 1u);
        if (last <= k) {
            if (last == -1) return -1;
            int32_t n = 0;
            do {
                n = g.S32(kStPiecesLast) - 1;
                if (g.U32(kStPieces + 16u * g.U32(kStPiecesLast)) != 0xFFFFFFFFu) return -1;
                g.W32(kStPiecesLast, U(n));
            } while (n != -1);
        }
        return -1;
    }
    return -1;
}

void PieceRebase(GuestRam& g, uint32_t newObj, uint32_t oldObj) {
    const uint32_t d = newObj - oldObj;
    for (uint32_t p = 0; S(p) < S(Players(g)); ++p) {
        const uint32_t v = kStViews + 1132u * p;
        if (g.U32(v + 0x148u) == oldObj) {
            RebaseCursor(g, v + 0xACu, d);
            RebaseEntity(g, v, d);
        }
    }
    uint32_t e = PoolBase(g, 0);
    for (int32_t n = PoolHigh(g, 0); n >= 0; --n, e += PoolStride(g, 0)) {
        if (g.S16(e + 0x140u) != 0 && g.U32(e + 0x148u) == oldObj) {
            RebaseCursor(g, e + 0xACu, d);
            RebaseEntity(g, e, d);
            if (g.U32(e + 0x358u) != 0 && g.U32(e + 0x440u) != 0) {
                RebaseCursor(g, g.U32(e + 0x358u) + 0xACu, d);
                RebaseEntity(g, g.U32(e + 0x358u), d);
            }
        }
        const uint32_t r = g.U32(e + 0x354u);
        if (g.U32(r + 0x25Cu) - 3u < 2u && g.S16(r + 0x140u) != 0 && g.U32(r + 0x148u) == oldObj) {
            RebaseCursor(g, r + 0xACu, d);
            RebaseEntity(g, r, d);
        }
        if (g.U32(e + 0x358u) != 0 && g.U32(e + 0x440u) != 0 && (g.U8(g.U32(e + 0x354u) + 0x23Cu) & 0x10u) != 0) {
            const uint32_t r2 = g.U32(g.U32(e + 0x358u) + 0x354u);
            if (g.U32(r2 + 0x25Cu) - 3u < 2u && g.S16(r2 + 0x140u) != 0 && g.U32(r2 + 0x148u) == oldObj) {
                RebaseCursor(g, r2 + 0xACu, d);
                RebaseEntity(g, r2, d);
            }
        }
    }
    for (uint32_t k = 2; k <= 4; ++k) {
        uint32_t o = PoolBase(g, k);
        for (int32_t n = PoolHigh(g, k); n >= 0; --n, o += PoolStride(g, k))
            if (g.S16(o + 0xACu) != 0 && g.U32(o + 0x148u) == oldObj) {
                RebaseCursor(g, o + 0xACu, d);
                RebaseEntity(g, o, d);
            }
    }
    uint32_t o = PoolBase(g, 5);
    for (int32_t n = PoolHigh(g, 5); n >= 0; --n, o += PoolStride(g, 5))
        if (g.S16(o + 0xACu) != 0 && g.U32(o + 0x148u) == oldObj) RebaseCursor(g, o + 0xACu, d);
    o = PoolBase(g, 6);
    for (int32_t n = PoolHigh(g, 6); n >= 0; --n, o += PoolStride(g, 6))
        if (g.S16(o) != 0 && g.U32(o + 0x9Cu) == oldObj) RebaseCursor(g, o, d);
}

uint32_t PieceUnload(GuestRam& g, uint32_t id, uint32_t p, uint32_t sp, RecoverCallees& c) {
    const uint32_t csp = sp - 40u;
    const uint32_t key = id & 0xFFFFu;
    if (!(S(key) < g.S16(g.U32(kRoadGraphPtr) + 0x28u))) return 2;
    const uint32_t btt = RoadBttRecord(g, S(key));
    if (btt == 0) return 2;
    if (g.U32(btt + 12u) == 0) return 2;
    const uint32_t bike = g.U32(kStRiderPtrs + 4u * p);
    const uint32_t view = kStViews + 1132u * p;
    if (g.U32(view + 0x148u) != 0 && g.U32(g.U32(view + 0x148u)) == key) return 2;
    if (bike != 0) {
        if (g.U32(g.U32(bike + 0x354u) + 0x25Cu) < 3u && g.U32(bike + 0x148u) != 0 && g.U32(g.U32(bike + 0x148u)) == key)
            return 2;
        const uint32_t r = g.U32(bike + 0x354u);
        if (g.U32(r + 0x25Cu) - 3u < 2u && g.U32(r + 0x148u) != 0 && g.U32(g.U32(r + 0x148u)) == key) return 2;
    }
    if ((g.U8(Gs(g) + 4u) & 1u) != 0 && g.U32(view + 0x304u) != 0 && g.U32(0x8005AD48u) == 1u) return 2;
    const int32_t k = PieceDrop(g, key, p);
    if (k >= 0 && Players(g) == 2) {
        if (k != 6) {
            const uint32_t slot = kStPieces + 16u * U(k);
            const uint32_t obj = g.U32(slot + 8u + 4u * g.U32(slot + 4u));
            const uint32_t old = g.U32(btt + 12u);
            if (PieceBind(g, obj, key) == 0) return 0;
            PieceRebase(g, obj, old);
        }
        return 1;
    }
    g.W32(btt + 12u, 0);
    if (g.U32(kStPlayer1) == 0 && g.U32(kStPlayer2) == 0) return 1;
    Call(c, kStActivateFn, {}, csp);
    Call(c, kStDownedFn, {}, csp);
    for (uint32_t pool = 2; pool <= 5; ++pool) {
        uint32_t o = PoolBase(g, pool);
        for (int32_t n = PoolHigh(g, pool); n >= 0; --n, o += PoolStride(g, pool)) {
            if (g.S16(o + 0xACu) == 0) continue;
            uint32_t v = 0;
            if (!Call(c, kStNearPieceFn, {o + 0xACu}, csp, &v)) return 1;
            g.W16(o + 0x140u, static_cast<uint16_t>(v));
            if (static_cast<int16_t>(v) == 0) Call(c, kStPoolDropFn, {o + 0xACu, pool}, csp);
        }
    }
    uint32_t o = PoolBase(g, 6);
    for (int32_t n = PoolHigh(g, 6); n >= 0; --n, o += PoolStride(g, 6)) {
        if (g.S16(o) == 0) continue;
        uint32_t v = 0;
        if (!Call(c, kStNearPieceFn, {o}, csp, &v)) return 1;
        g.W16(o + 0x94u, static_cast<uint16_t>(v));
        if (static_cast<int16_t>(v) == 0) Call(c, kStPoolDropFn, {o, 6}, csp);
    }
    return 1;
}

// ============================================================================ textures
int32_t TexFind(GuestRam& g, uint32_t key, uint32_t p) {
    for (int32_t i = S(p * 24u); i < S(p * 24u + 24u); ++i)
        if (key == g.U32(kStTexSlots + 0x30u * U(i) + 8u)) return i;
    return -1;
}

void TexLink(GuestRam& g, uint32_t slot, uint32_t kind, uint32_t p) {
    for (int32_t i = S(p * 12u); i < S(p * 12u + 12u); ++i) {
        const uint32_t cs = kStCellSlots + 0x70u * U(i);
        for (uint32_t j = 0; j < 2u; ++j) {
            if (kind == 0) {
                if (g.U32(cs + 0x50u + 4u * j) == g.U32(slot + 8u)) g.W32(cs + 0x68u + 4u * j, slot);
            } else if (g.U32(cs + 0x58u + 4u * j) == g.U32(slot + 8u)) {
                g.W32(cs + 0x60u + 4u * j, slot);
            }
        }
    }
}

int32_t VramFind(GuestRam& g, uint32_t kind, uint32_t key, uint32_t p) {
    const int32_t n = kind == 0 ? g.S32(kStVramCountB) : g.S32(kStVramCountA);
    uint32_t e = kind == 0 ? kStVramB + p * 0x18u : kStVramA + (p << 5);
    for (int32_t i = 0; i < n; ++i, e += 8u)
        if (g.U32(e) == key) return i;
    return -1;
}

void VramUpload(GuestRam& g, uint32_t page, uint32_t key, uint32_t a, uint32_t b, uint32_t p, uint32_t sp,
                RecoverCallees& c) {
    const uint32_t csp = sp - 64u;
    const uint32_t rect = csp + 16u;
    const uint32_t n = Players(g);
    int32_t i = TexFind(g, key, p);
    if (i == -1) i = TexFind(g, 0xFFFFFFFFu, p);
    const uint32_t slot = kStTexSlots + 0x30u * U(i);
    g.W32(slot + 8u, key);
    const uint32_t kind = key >> 15;
    if (kind == 0) {
        const uint32_t lay = 0x800533B4u + 4u * p + (n - 1u) * 0x58u;
        g.W16(rect + 4u, 0x40);
        g.W16(rect + 6u, 0x80);
        g.W16(rect + 2u, 0);
        g.W16(rect + 0u, static_cast<uint16_t>(((page & 0xFu) + (g.U8(lay + 2u) & 0xFu)) << 6));
        Call(c, kStLoadImageFn, {rect, a}, csp);
        g.W16(rect + 2u, static_cast<uint16_t>(g.U16(rect + 2u) + 0x80u));
        Call(c, kStLoadImageFn, {rect, b}, csp);
        g.W32(slot, page);
    } else if (kind == 1) {
        const uint32_t lay = 0x800533B4u + 4u * (p + 2u) + (n - 1u) * 0x58u;
        const uint32_t hi = (page & 1u) << 7;
        g.W16(rect + 0u, static_cast<uint16_t>(((g.U8(lay + 2u) & 0xFu) << 6) + ((page << 5) & 0x3C0u)));
        g.W16(rect + 4u, 0x40);
        g.W16(rect + 6u, 0x80);
        g.W16(rect + 2u, static_cast<uint16_t>(g.U8(lay + 1u) + ((g.U8(lay + 2u) & 0x10u) << 4) + hi));
        Call(c, kStLoadImageFn, {rect, a}, csp);
        const uint32_t e = kStVramA + page * 8u + (p << 5);
        g.W8(e + 6u, 0);
        g.W8(e + 7u, static_cast<uint8_t>(hi));
        g.W32(slot, page);
    }
}

uint32_t VramBind(GuestRam& g, uint32_t page, uint32_t slotIndex, uint32_t kind, uint32_t p, uint32_t sp,
                  RecoverCallees& c) {
    const uint32_t csp = sp - 40u;
    const uint32_t s = kStTexSlots + slotIndex * 0x30u;
    if (g.U32(s + 12u) == 0) return 1;
    uint32_t r = 0;
    if (g.U32(s + 16u) == 0 || kind != 0) {
        if (g.U32(s + 12u) == 0) return 1;
        if (kind != 1) return 1;
        VramUpload(g, page, g.U32(s + 8u), g.U32(s + 12u), g.U32(s + 16u), p, csp, c);
        ResFreeByBuffer(g, g.U32(s + 12u), csp, c);
        g.W32(s + 12u, 0);
        g.W32(s + 16u, 0);
        g.W32(s, page);
        const uint32_t e = page * 8u + (p << 5);
        const uint32_t tp = kStTpageTable + (g.U16(kStVramA + 4u + e) & 0x1Fu) * 4u;
        if ((page & 1u) == 0) {
            g.W32(tp, (g.U32(tp) & 0xFFFFu) | (g.U32(s + 8u) << 16));
            g.W16(kStVramA + 4u + e, static_cast<uint16_t>(g.U16(kStVramA + 4u + e) | 0x800u));
        } else {
            g.W32(tp, (g.U32(tp) & 0xFFFF0000u) | g.U32(s + 8u));
        }
        r = 1;
        g.W32(kStVramA + e, g.U32(s + 8u));
    } else {
        VramUpload(g, page, g.U32(s + 8u), g.U32(s + 12u), g.U32(s + 16u), p, csp, c);
        ResFreeByBuffer(g, g.U32(s + 12u), csp, c);
        ResFreeByBuffer(g, g.U32(s + 16u), csp, c);
        r = 0;
        const uint32_t e = (p * 3u + page) * 8u;
        g.W32(s + 12u, 0);
        g.W32(s + 16u, 0);
        g.W32(s, page);
        g.W32(kStTpageTable + (g.U16(kStVramB + 4u + e) & 0x1Fu) * 4u, g.U32(s + 8u));
        g.W32(kStVramB + e, g.U32(s + 8u));
    }
    Call(c, kStTexPassAllFn, {r, p}, csp);
    return 1;
}

uint32_t TexLoad(GuestRam& g, uint32_t key, uint32_t buf, uint32_t kind, uint32_t half, uint32_t v,
                 uint32_t windows, uint32_t p, uint32_t sp, RecoverCallees& c) {
    const uint32_t csp = sp - 56u;
    int32_t i = TexFind(g, key, p);
    if (i == -1) {
        i = TexFind(g, 0xFFFFFFFFu, p);
    } else {
        const uint32_t s = kStTexSlots + 0x30u * U(i);
        if (kind == 0) {
            if (half == 0 && g.U32(s + 12u) != 0) return 3;
            if (half == 1 && g.U32(s + 16u) != 0) return 3;
        }
        if (kind == 1 && g.U32(s + 12u) != 0) return 3;
        if (g.U32(s) != 0xFFFFFFFFu) return 3;
    }
    const uint32_t s = kStTexSlots + 0x30u * U(i);
    if (kind == 0) {
        g.W32(s + 8u, key);
        g.W32(s + 4u, 0);
        g.W32(half == 0 ? s + 12u : s + 16u, buf);
    } else {
        g.W32(s + 8u, key);
        g.W32(s + 12u, buf);
        g.W32(s + 4u, kind);
    }
    g.W32(s + 20u, v);
    GuestCopyWords(g, s + 24u, windows, 0x18u); // SLUS 0x8001E0B4
    TexLink(g, s, kind, p);
    if ((kind == 0 && g.U32(s + 12u) != 0 && g.U32(s + 16u) != 0) || kind == 1) {
        int32_t pg = VramFind(g, kind, key, p);
        if (pg < 0) {
            pg = VramFind(g, kind, 0xFFFFFFFFu, p);
            if (pg != -1) VramBind(g, U(pg), U(i), kind, p, csp, c);
        }
    }
    return 1;
}

void VramFree(GuestRam& g, int32_t page, uint32_t kind, uint32_t p) {
    const uint32_t t = kind == 0 ? kStVramB + p * 0x18u : kStVramA + (p << 5);
    if (page >= 0) g.W32(t + U(page) * 8u, 0xFFFFFFFFu);
}

void TexUnlinkAll(GuestRam& g, uint32_t key, uint32_t p, uint32_t sp, RecoverCallees& c) {
    const uint32_t csp = sp - 64u;
    const int32_t kind = S(key) >> 15;
    for (int32_t i = S(p * 12u); i < S(p * 12u + 12u); ++i) {
        const uint32_t cs = kStCellSlots + 0x70u * U(i);
        for (uint32_t j = 0; j < 2u; ++j) {
            const uint32_t at = cs + (kind == 0 ? 0x68u : 0x60u) + 4u * j;
            if (g.U32(at) != 0 && g.U32(g.U32(at) + 8u) == key) {
                Call(c, kStTexUnlinkFn, {cs, U(kind)}, csp);
                g.W32(at, 0);
            }
        }
    }
}

int32_t TexFree(GuestRam& g, uint32_t key, uint32_t /*kind: not read*/, uint32_t p, uint32_t sp, RecoverCallees& c) {
    const uint32_t csp = sp - 40u;
    const int32_t i = TexFind(g, key, p);
    if (i == -1) return -1;
    TexUnlinkAll(g, key, p, csp, c);
    const uint32_t s = kStTexSlots + 0x30u * U(i);
    VramFree(g, g.S32(s), g.U32(s + 4u), p);
    g.W32(s + 8u, 0xFFFFFFFFu);
    g.W32(s + 12u, 0);
    g.W32(s + 16u, 0);
    g.W32(s, 0xFFFFFFFFu);
    g.W32(s + 20u, 0xFFFFFFFFu);
    for (uint32_t k = 0; k < 0x18u; k += 4u) g.W32(s + 24u + k, 0xFFFFFFFFu); // SLUS 0x8001E100
    return 1;
}

void TexSweep(GuestRam& g, uint32_t p, uint32_t sp, RecoverCallees& c) {
    const uint32_t csp = sp - 40u;
    uint32_t s = kStTexSlots + p * 0x480u;
    for (int32_t k = 0; k < 24; ++k, s += 0x30u) {
        const uint32_t key = g.U32(s + 8u);
        if (S(key) < 0) continue;
        if (ResWindow(g, key, s + 24u, p) >= 0) continue;
        const uint32_t id = ((key & 0x8000u) << 16) | ((key & 0x7C00u) << 13) | (key & 0x3FFu);
        if (g.U32(s + 4u) == 0) TexFree(g, ((id >> 16) & 0x8000u) | ((id >> 13) & 0x7C00u) | (id & 0x3FFu), 0, p, csp - 24u, c);
        else TexFree(g, ((id >> 13) & 0x7C00u) | 0x8000u | (id & 0x3FFu), 1, p, csp - 24u, c);
    }
}

// ============================================================================ panoramas
uint32_t PanoLoad(GuestRam& g, uint32_t payload, uint32_t windows, uint32_t key) {
    const uint32_t sky = g.U32(kStSkyPtr);
    if (g.U32(g.gp() + kGpStSkyBusy) != 0 || g.U32(payload + 12u) != 0x50414E4Fu) return 2;
    if (!(g.S16(sky + 0x22u) < 20)) return 0;
    uint32_t e = sky + 0x2E4u;
    for (int32_t i = 0; i < 20; ++i, e += 12u) {
        if (g.U32(e) == 0) continue;
        bool same = true;
        uint32_t w = g.U32(e + 4u);
        for (int32_t k = 0; k < 4; ++k, w += 6u) {
            if (g.S16(w) != g.S16(windows) || g.S16(w + 2u) != g.S16(windows + 2u) || g.S16(w + 4u) != g.S16(windows + 4u)) {
                same = false;
                break;
            }
        }
        if (same) return 3;
    }
    e = sky + 0x2E4u;
    bool found = false;
    for (int32_t n = 20; n != 0 && !found;) {
        if (g.U32(e) == 0) found = true;
        else {
            e += 12u;
            --n;
        }
    }
    if (!found) return 0;
    g.W32(payload + 0x18u, g.U16(payload + 4u));
    g.W32(payload + 0x24u, payload);
    g.W32(payload + 0x1Cu, g.U32(payload + 8u));
    g.W16(sky + 0x22u, static_cast<uint16_t>(g.S16(sky + 0x22u) + 1));
    g.W32(e, payload);
    g.W32(e + 4u, windows);
    g.W32(e + 8u, key);
    return 1;
}

uint32_t PanoFree(GuestRam& g, uint32_t payload) {
    if (g.U32(g.gp() + kGpStSkyBusy) != 0 || g.U32(payload + 12u) != 0x50414E4Fu) return 2;
    const uint32_t sky = g.U32(kStSkyPtr);
    uint32_t e = sky + 0x2E4u;
    bool found = false;
    int32_t i = 0;
    for (int32_t n = 20; n != 0 && !found;) {
        if (g.U32(e) == payload) found = true;
        else {
            e += 12u;
            --n;
            ++i;
        }
    }
    if (!found) return 2;
    const uint32_t sky2 = g.U32(kStSkyPtr);
    if (i == g.S16(sky2 + 0x10u) || i == g.S16(sky2 + 0x12u)) return 2;
    g.W32(e, 0);
    g.W32(e + 4u, 0);
    g.W32(e + 8u, 0);
    g.W16(sky2 + 0x22u, static_cast<uint16_t>(g.S16(sky2 + 0x22u) - 1));
    return 1;
}

// ============================================================================ the loader's set-up
void ResInit(GuestRam& g, uint32_t buffers) {
    const uint32_t r = g.U32(kStResList);
    if (Players(g) == 1) {
        g.W32(r + 0xA44u, 0x1D);
        g.W32(r + 0xA50u, 0x1E);
        g.W32(r + 0xA54u, 0x1F);
        g.W32(r + 0xA58u, 0x20);
        g.W32(r + 0xA40u, 0);
        g.W32(r + 0xA4Cu, 0);
        g.W32(r + 0xA48u, 0);
        g.W32(r + 0xA5Cu, 2);
    } else {
        g.W32(r + 0xA44u, 0xF);
        g.W32(r + 0xA48u, 0x10);
        g.W32(r + 0xA4Cu, 0x1F);
        g.W32(r + 0xA40u, 0);
        g.W32(r + 0xA54u, 0);
        g.W32(r + 0xA50u, 0);
        g.W32(r + 0xA58u, 0x20);
        g.W32(r + 0xA5Cu, 0);
    }
    const uint32_t n = g.U32(r + 0xA58u);
    for (uint32_t o : {0x00u, 0x04u, 0x08u, 0x0Cu, 0x14u, 0x18u, 0x20u, 0x24u, 0x28u, 0x1Cu}) g.W32(r + o, 0);
    g.W32(r + 0x10u, n);
    // 0x8005D338: the heap block, once
    if (g.U32(r + 0xA3Cu) == 0) {
        g.W32(r + 0xA3Cu, 1);
        g.W32(r + 0xA38u, buffers);
    }
    // 0x8005D38C: the records
    uint32_t buf = g.U32(r + 0xA38u);
    for (uint32_t i = 0; S(i) < g.S32(r + 0xA58u); ++i, buf += 0x4000u) {
        const uint32_t rec = r + 0x2Cu + 36u * i;
        g.W32(rec, 0);
        g.W32(rec + 12u, buf);
        g.W32(rec + 4u, i);
        g.W32(rec + 16u, 0);
        g.W32(rec + 20u, 0);
        g.W32(rec + 24u, 0);
        g.W32(rec + 32u, 0);
    }
    // 0x8005D3F4: the new-block queue
    g.W32(r + 0x930u, 0);
    g.W32(r + 0x92Cu, 0);
    g.W32(r + 0x934u, 0);
}

void TexInit(GuestRam& g, int32_t players) {
    for (uint32_t slot = kStCellSlots; slot < 0x800D9268u; slot += 0x70u) {
        g.W32(slot, 0xFFFFFFFFu);
        for (uint32_t j = 0; j < 2u; ++j) {
            g.W32(slot + 0x68u + 4u * j, 0);
            g.W32(slot + 0x60u + 4u * j, 0);
            g.W32(slot + 0x50u + 4u * j, 0xFFFFFFFFu);
            g.W32(slot + 0x58u + 4u * j, 0xFFFFFFFFu);
        }
        g.W32(slot + 8u, 0xFFFFFFFFu);
        g.W32(slot + 12u, 0);
        g.W32(slot + 0x4Cu, 0);
        g.W32(slot + 0x38u, 0);
    }
    int32_t j = 0;
    for (int32_t k = 0; k < 12; ++k)
        for (j = 0; j < players; ++j) g.W32(0x800D9B80u + 4u * U(k) + 0x30u * U(j), 0xFFFFFFFFu);
    g.W32(0x8005B568u + 4u * U(j), 0);
    for (uint32_t s = kStTexSlots, k = 0; k < 0x30u; ++k, s += 0x30u) {
        g.W32(s + 8u, 0xFFFFFFFFu);
        g.W32(s + 12u, 0);
        g.W32(s + 16u, 0);
        g.W32(s, 0xFFFFFFFFu);
        g.W32(s + 20u, 0xFFFFFFFFu);
        for (uint32_t b = 0; b < 0x18u; b += 4u) g.W32(s + 24u + b, 0xFFFFFFFFu);
    }
    for (uint32_t u = 0; u < 3u; ++u)
        for (int32_t k = 0; k < players; ++k) {
            const uint32_t at = kStVramB + (u << 3) + 0x18u * U(k);
            g.W32(at, 0xFFFFFFFFu);
            const uint32_t lay = g.U8(0x800533B6u + U((players - 1) * 0x58 + 4 * k));
            g.W16(at + 4u, static_cast<uint16_t>(S((((lay & 0xFu) << 6) + ((u & 0xFu) << 6)) & 0x3FFu) >> 6));
        }
    for (uint32_t u = 0; u < 4u; ++u)
        for (int32_t k = 0; k < players; ++k) {
            const uint32_t at = kStVramA + (u << 3) + 0x20u * U(k);
            g.W32(at, 0xFFFFFFFFu);
            const uint32_t lay = g.U8(0x800533B6u + U(8 + 4 * k + (players - 1) * 0x58));
            g.W16(at + 4u, static_cast<uint16_t>(S((((lay & 0xFu) << 6) + ((u & 0x1Eu) << 5)) & 0x3FFu) >> 6));
        }
    g.W32(0x8005AEDCu, 0);
    g.W16(0x800D6162u, 0x7800);
    g.W32(kStVramCountB, players == 1 ? 3u : 2u);
    g.W32(kStVramCountA, 4u);
}

} // namespace rr::sim
