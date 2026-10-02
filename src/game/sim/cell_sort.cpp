// The cells' view sort (cell_sort.h), transcribed from SLUS_010.53 (SHA-1 67ed165a2c517d4e6106fb0dfa324d66dd9a76f1).
#include "game/sim/cell_sort.h"

#include "game/sim/effects.h" // FxGte: MVMVA, bit for bit with the interpreter's GTE

namespace rr::sim {
namespace {

using U = uint32_t;

uint32_t CtxOf(int32_t slot) { return kCellCtx + static_cast<U>(slot) * kCellCtxStride; }

// The GTE of one cell: RT = ctx +0x0C.. (five control words), TR = ctx +0x20.. (three).
FxGte CellGte(GuestRam& g, uint32_t ctx) {
    FxGte gte;
    uint32_t w[8];
    for (U k = 0; k < 8; ++k) w[k] = g.U32(ctx + 0x0Cu + 4u * k);
    gte.rt[0] = static_cast<int16_t>(w[0] & 0xFFFFu);
    gte.rt[1] = static_cast<int16_t>(w[0] >> 16);
    gte.rt[2] = static_cast<int16_t>(w[1] & 0xFFFFu);
    gte.rt[3] = static_cast<int16_t>(w[1] >> 16);
    gte.rt[4] = static_cast<int16_t>(w[2] & 0xFFFFu);
    gte.rt[5] = static_cast<int16_t>(w[2] >> 16);
    gte.rt[6] = static_cast<int16_t>(w[3] & 0xFFFFu);
    gte.rt[7] = static_cast<int16_t>(w[3] >> 16);
    gte.rt[8] = static_cast<int16_t>(w[4] & 0xFFFFu);
    for (int k = 0; k < 3; ++k) gte.tr[k] = static_cast<int32_t>(w[5 + k]);
    return gte;
}

// The side codes of one vertex (0x80035150..0x800351BC / 0x8003530C..0x80035378): the AND code against the planes
// |16 z| = 21 x, the OR code against |16 z| = 16 x, both with "behind" (16 z < -640) as bit 2. MIPS arithmetic wraps.
int32_t Widened(int32_t x, int32_t widen) {
    if (widen == kWidenUnit || widen <= 0) return x;
    return static_cast<int32_t>(static_cast<int64_t>(x) * kWidenUnit / widen); // OURS: the wide window (cell_sort.h)
}
uint32_t SideCode(int32_t x, int32_t z, uint32_t slope, int32_t widen) {
    x = Widened(x, widen);
    const int32_t z16 = static_cast<int32_t>(static_cast<U>(z) << 4);
    const int32_t sign = z16 >> 31;
    const int32_t az = static_cast<int32_t>((static_cast<U>(sign) + static_cast<U>(z16)) ^ static_cast<U>(sign));
    const int32_t naz = static_cast<int32_t>(0u - static_cast<U>(az));
    const int32_t xs = static_cast<int32_t>(static_cast<U>(x) * slope);
    U c = z16 < -640 ? 4u : 0u;
    if (az < xs) c |= 2u;
    if (xs < naz) c |= 1u;
    return c;
}

int32_t Verdict(uint32_t andCode, uint32_t orCode) { return andCode != 0 ? 0 : (orCode == 0 ? 2 : 1); }

} // namespace

// ============================================================================ SLUS 0x80035040
int32_t CellDepthRange(GuestRam& g, uint32_t ctx, uint32_t poly, int32_t n, int32_t widen) {
    uint32_t andCode = 0xFFu, orCode = 0;
    const FxGte gte = CellGte(g, ctx);
    g.W32(ctx + 0x2Cu, static_cast<U>(-641));
    g.W32(ctx + 0x30u, 0xFFFFu);
    for (int32_t i = 0; i < n; ++i) {
        const int32_t index = g.S16(poly + 2u * static_cast<U>(i));
        const uint32_t vtx = g.U32(g.U32(ctx) + 0x34u) + (static_cast<U>(index) << 3) + 4u;
        const int16_t v[3] = {g.S16(vtx), g.S16(vtx + 2u), g.S16(vtx + 4u)};
        int32_t mac[3];
        gte.Mvmva(v, mac);
        const int32_t z = mac[2];
        if (z < g.S32(ctx + 0x30u)) g.W32(ctx + 0x30u, static_cast<U>(z));
        if (g.S32(ctx + 0x2Cu) < z) g.W32(ctx + 0x2Cu, static_cast<U>(z));
        andCode &= SideCode(mac[0], z, 21u, widen);
        orCode |= SideCode(mac[0], z, 16u, widen);
        if (g.Faulted()) return 0;
    }
    return Verdict(andCode, orCode);
}

namespace {
// 0x800351EC with its out-words kept by value (the LOD pass's caller-frame use).
int32_t RegionRange(GuestRam& g, uint32_t ctx, uint32_t poly, int32_t n, int32_t widen, int32_t& mean, int32_t& min) {
    uint32_t andCode = 7u, orCode = 0;
    const FxGte gte = CellGte(g, ctx);
    uint32_t out = g.U32(kCellVertexBuffer);
    int32_t sum = 0;
    min = 0x00FFFFFF;
    for (int32_t i = 0; i < n; ++i) {
        const int32_t index = g.S16(poly + 2u * static_cast<U>(i));
        const uint32_t vtx = g.U32(g.U32(ctx) + 0x34u) + (static_cast<U>(index) << 3) + 4u;
        const int16_t v[3] = {g.S16(vtx), g.S16(vtx + 2u), g.S16(vtx + 4u)};
        int32_t mac[3];
        gte.Mvmva(v, mac);
        for (U k = 0; k < 3; ++k) g.W32(out + 4u * k, static_cast<U>(mac[k]));
        const int32_t z = g.S32(out + 8u), x = g.S32(out);
        if (z < min) min = z;
        if (i < 4) sum = static_cast<int32_t>(static_cast<U>(sum) + static_cast<U>(z));
        andCode &= SideCode(x, z, 21u, widen);
        orCode |= SideCode(x, z, 16u, widen);
        out += 16u;
        if (g.Faulted()) return 0;
    }
    mean = sum >> 2;
    return Verdict(andCode, orCode);
}
} // namespace

// ============================================================================ SLUS 0x800351EC
int32_t CellRegionRange(GuestRam& g, uint32_t ctx, uint32_t poly, int32_t n, uint32_t mean, uint32_t min, uint32_t max,
                        int32_t widen) {
    uint32_t andCode = 7u, orCode = 0;
    const FxGte gte = CellGte(g, ctx);
    uint32_t out = g.U32(kCellVertexBuffer);
    g.W32(min, 0x00FFFFFFu);
    g.W32(max, static_cast<U>(-641));
    g.W32(mean, 0);
    for (int32_t i = 0; i < n; ++i) {
        const int32_t index = g.S16(poly + 2u * static_cast<U>(i));
        const uint32_t vtx = g.U32(g.U32(ctx) + 0x34u) + (static_cast<U>(index) << 3) + 4u;
        const int16_t v[3] = {g.S16(vtx), g.S16(vtx + 2u), g.S16(vtx + 4u)};
        int32_t mac[3];
        gte.Mvmva(v, mac);
        for (U k = 0; k < 3; ++k) g.W32(out + 4u * k, static_cast<U>(mac[k]));
        const int32_t z = g.S32(out + 8u), x = g.S32(out);
        if (z < g.S32(min)) g.W32(min, static_cast<U>(z));
        if (g.S32(max) < z) g.W32(max, static_cast<U>(z));
        if (i < 4) g.W32(mean, g.U32(mean) + static_cast<U>(z));
        andCode &= SideCode(x, z, 21u, widen);
        orCode |= SideCode(g.S32(out), z, 16u, widen); // x re-read from the buffer (0x80035354)
        out += 16u;
        if (g.Faulted()) return 0;
    }
    g.W32(mean, static_cast<U>(g.S32(mean) >> 2));
    return Verdict(andCode, orCode);
}

// ============================================================================ SLUS 0x800106DC
void ApplyRows(GuestRam& g, uint32_t v, uint32_t out, uint32_t m) {
    const int32_t x = g.S32(v), y = g.S32(v + 4u), z = g.S32(v + 8u);
    for (U r = 0; r < 3; ++r) {
        const int32_t a = static_cast<int32_t>(static_cast<U>(g.S16(m + 6u * r)) * static_cast<U>(x)) >> 12;
        const int32_t b = static_cast<int32_t>(static_cast<U>(g.S16(m + 6u * r + 2u)) * static_cast<U>(y)) >> 12;
        const int32_t c = static_cast<int32_t>(static_cast<U>(g.S16(m + 6u * r + 4u)) * static_cast<U>(z)) >> 12;
        g.W32(out + 4u * r, static_cast<U>(a) + static_cast<U>(b) + static_cast<U>(c));
    }
}

// ============================================================================ SLUS 0x800353C4
void CellViewCull(GuestRam& g, uint32_t count, int32_t n, uint32_t list, uint32_t p, int32_t widen) {
    const uint32_t gp = g.gp();
    int32_t cameraAt = -1;
    const uint32_t cameraCell = g.U32(kCellViewRecords + kCellViewStride * p + 0xB0u);
    g.W32(gp + kGpCameraSlot, 0xFFFFFFFFu);
    const uint32_t rcPtr = kCellRenderCams + 4u * p;
    for (int32_t i = 0; i < n; ++i) {
        const uint32_t at = list + 4u * static_cast<U>(i);
        const int32_t slot = g.S32(at);
        if (slot == -1) continue;
        const uint32_t ctx = CtxOf(slot);
        { // the render camera's rows +0x5C..+0x78 into ctx +0x0C.. (0x80035480..0x800354C8, in its order)
            const uint32_t src = g.U32(rcPtr) + 0x5Cu, dst = ctx + 0x0Cu;
            uint32_t t0 = g.U32(src), t1 = g.U32(src + 4u);
            g.W32(dst, t0);
            t0 = g.U32(src + 8u);
            g.W32(dst + 4u, t1);
            g.W32(dst + 8u, t0);
            t0 = g.U32(src + 12u);
            t1 = g.U32(src + 16u);
            g.W32(dst + 12u, t0);
            t0 = g.U32(src + 20u);
            g.W32(dst + 16u, t1);
            g.W32(dst + 20u, t0);
            t0 = g.U32(src + 24u);
            t1 = g.U32(src + 28u);
            g.W32(dst + 24u, t0);
            g.W32(dst + 28u, t1);
        }
        // the cell's origin less the eye, turned by the rows (0x800106DC; the caller's frame, here by value)
        const int32_t ox = g.S32(g.U32(ctx) + 8u), oy = g.S32(g.U32(ctx) + 12u), oz = g.S32(g.U32(ctx) + 16u);
        const int32_t d[3] = {static_cast<int32_t>(static_cast<U>(ox) - g.U32(g.U32(rcPtr) + 0x1Cu)),
                              static_cast<int32_t>(static_cast<U>(oy) - g.U32(g.U32(rcPtr) + 0x20u)),
                              static_cast<int32_t>(static_cast<U>(oz) - g.U32(g.U32(rcPtr) + 0x24u))};
        int32_t t[3];
        for (U r = 0; r < 3; ++r) {
            int32_t s = 0;
            for (U c = 0; c < 3; ++c)
                s = static_cast<int32_t>(static_cast<U>(s) +
                                         static_cast<U>(static_cast<int32_t>(static_cast<U>(g.S16(ctx + 0x0Cu + 6u * r + 2u * c)) *
                                                                             static_cast<U>(d[c])) >> 12));
            t[r] = s;
        }
        const uint32_t body = g.U32(ctx);
        for (U k = 0; k < 3; ++k) g.W32(ctx + 0x20u + 4u * k, static_cast<U>(t[k]));
        const uint32_t poly = g.U32(body + 0x2Cu);
        const int32_t from = g.S16(poly), to = g.S16(poly + 2u);
        const int32_t code = CellDepthRange(g, ctx, poly + 2u * static_cast<U>(from), to - from, widen) & 0xFF;
        const int32_t again = g.S32(at);
        if (g.U32(kCellCtx - 4u + static_cast<U>(again) * kCellCtxStride + 8u) == cameraCell) {
            if (g.S32(ctx + 0x2Cu) < 0) g.W32(ctx + 0x2Cu, 0x500u);
            g.W32(gp + kGpCameraSlot, g.U32(at));
            cameraAt = i;
        } else if (code == 0) {
            g.W32(at, 0xFFFFFFFFu);
            g.W32(count, g.U32(count) - 1u);
        }
        if (g.Faulted()) return;
    }
    const int32_t camera = g.S32(gp + kGpCameraSlot);
    if (camera != -1 && cameraAt != -1) {
        g.W32(list + 4u * static_cast<U>(cameraAt), g.U32(list));
        g.W32(list, static_cast<U>(camera));
    }
}

// ============================================================================ SLUS 0x80036438
int32_t CellListSort(GuestRam& g, int32_t n, uint32_t list, uint32_t /*p*/, bool* settled, int32_t maxPasses) {
    // 0x8002E698 (the view's light row times its vector, a GTE-only product whose value is dropped) is not run: it
    // leaves no byte in RAM.
    const uint32_t gp = g.gp();
    const auto rec = [](int32_t slot) { return kCellCtx - 4u + static_cast<U>(slot) * kCellCtxStride; };
    uint32_t base = list;
    int32_t k = 1;
    const int32_t camera = g.S32(gp + kGpCameraSlot);
    if (camera != -1) {
        g.W32(base, static_cast<U>(camera));
        base += 4u;
        k = 2;
    }
    const int32_t m = n - k;
    int32_t passes = 0;
    if (settled) *settled = true;
    for (int32_t swaps = 1; swaps > 0;) {
        if (passes == maxPasses) {
            if (settled) *settled = false;
            break;
        }
        ++passes;
        swaps = 0;
        for (int32_t i = 0; i < m; ++i) {
            const uint32_t at = base + 4u * static_cast<U>(i);
            const int32_t cur = g.S32(at);
            if (cur == -1) continue;
            int32_t j = i + 1;
            const int32_t curMax = g.S32(rec(cur) + 0x30u);
            if (g.S32(base + 4u * static_cast<U>(j)) == -1) {
                while (j < m) {
                    ++j;
                    if (g.S32(base + 4u * static_cast<U>(j)) != -1) break;
                }
                if (g.S32(base + 4u * static_cast<U>(j)) == -1) continue;
            }
            const uint32_t other = base + 4u * static_cast<U>(j);
            const int32_t next = g.S32(other);
            const int32_t nextMax = g.S32(rec(next) + 0x30u);
            const int32_t d = static_cast<int32_t>(static_cast<U>(curMax) - static_cast<U>(nextMax));
            const int32_t sign = d >> 31;
            const int32_t ad = static_cast<int32_t>((static_cast<U>(sign) + static_cast<U>(d)) ^ static_cast<U>(sign));
            bool swap = nextMax < curMax;
            if (ad < 0x280) swap = g.S32(rec(next) + 0x40u) < g.S32(rec(g.S32(at)) + 0x40u);
            if (swap) {
                const uint32_t v = g.U32(at);
                ++swaps;
                g.W32(at, static_cast<U>(next));
                g.W32(other, v);
            }
            if (g.Faulted()) return passes;
        }
    }
    return passes;
}

// ============================================================================ SLUS 0x80035680 (and 0x80036614)
void CellLodPass(GuestRam& g, int32_t n, uint32_t list, uint32_t /*p*/, int32_t widen) {
    const int32_t fineDepth = g.S32(g.U32(kCellGameState) + 0x30u) == 2 ? 0x12C0 : 0x2580;
    const auto set = [&](uint32_t ctx, int32_t k, int32_t v) {
        g.W32(ctx + 0x34u, g.U32(ctx + 0x34u) | (static_cast<U>(v) << ((static_cast<U>(k) << 2) & 31u)));
    };
    int32_t fineCells = 0;
    for (int32_t s = 0; s < n; ++s) {
        const int32_t slot = g.S32(list + 4u * static_cast<U>(s));
        if (slot == -1) continue;
        const uint32_t ctx = CtxOf(slot);
        g.W32(ctx + 0x34u, 0);
        bool fine = false;
        for (int32_t k = 0; k < static_cast<int32_t>(g.U16(g.U32(ctx) + 6u)); ++k) {
            const uint32_t polys = g.U32(g.U32(ctx) + 0x2Cu);
            const int32_t from = g.S16(polys + 2u * static_cast<U>(k) + 2u), to = g.S16(polys + 2u * static_cast<U>(k) + 4u);
            // 0x800351EC with its three out-words in the caller's frame (sp+32 mean, +36 min, +40 max): by value here
            int32_t mean = 0, mn = 0;
            const int32_t code = RegionRange(g, ctx, polys + 2u * static_cast<U>(from), to - from, widen, mean, mn);
            if (k == 1) g.W32(ctx + 0x3Cu, static_cast<U>(mean));
            if (code == 0) {
                set(ctx, k, 0);
            } else {
                int32_t v = 0;
                if (g.U32(g.U32(ctx) + 0x3Cu) != 0 && mn < fineDepth && fineCells < 4) {
                    fine = true;
                    v = (code == 1 || mean < 0x800) ? 7 : 3;
                } else {
                    v = (code == 1 || mean < 0x800) ? 5 : 1;
                }
                set(ctx, k, v);
                if (mean < fineDepth) set(ctx, k, 12);
            }
            if (g.Faulted()) return;
        }
        if (fine) ++fineCells;
    }
}

// ============================================================================ SLUS 0x80021988 (its map)
OtTableMap OtTableMapOf(GuestRam& g, int32_t min, int32_t max) {
    OtTableMap m;
    int32_t span = 0;
    if (max < 0) {
        m.nearOffset = 0;
        span = 0x780;
        m.shift = g.S32(kCellOtShiftTable);
    } else if (min < 0) {
        m.nearOffset = 0;
        span = max + 0x780;
        m.shift = g.S32(kCellOtShiftTable);
    } else {
        span = max - min + 0x780;
        int32_t i = min >> 11;
        m.nearOffset = min - 0x3C0;
        if (!(i < 10)) i = 9;
        m.shift = g.S32(kCellOtShiftTable + 4u * static_cast<U>(i));
    }
    const U sh = static_cast<U>(m.shift) & 31u;
    m.length = static_cast<int32_t>(static_cast<U>(span) + (1u << sh)) >> sh;
    if (m.nearOffset < 0x1000) m.length += 0x300;
    if (!(m.length < 1300)) m.length = 1299;
    return m;
}

} // namespace rr::sim
