#include "game/sim/model_draw.h"

#include <algorithm>

#include "game/sim/effects.h"
#include "game/sim/traffic_bind.h"

namespace rr::sim::model {
namespace {

constexpr uint32_t kGameStatePtr = 0x8005B2F8;

// ---------------------------------------------------------------------------- GTE arithmetic
// src\interp\gte.cpp's: the 44-bit accumulator wraps, IR saturates, the UNR reciprocal divide.
struct UnrTable {
    uint8_t v[257];
    constexpr UnrTable() : v{} {
        for (int i = 0; i <= 256; ++i) {
            const int x = (0x40000 / (i + 0x100) + 1) / 2 - 0x101;
            v[i] = static_cast<uint8_t>(x < 0 ? 0 : x);
        }
    }
};
constexpr UnrTable kUnr{};

int64_t Mac44(int64_t v) { return static_cast<int64_t>(static_cast<uint64_t>(v) << 20) >> 20; }
int32_t ClampIr(int32_t v) { return v < -0x8000 ? -0x8000 : (v > 0x7FFF ? 0x7FFF : v); }
int32_t ClampSxy(int32_t v) { return v < -0x400 ? -0x400 : (v > 0x3FF ? 0x3FF : v); }

uint32_t GteDivide(uint32_t h, uint32_t sz3) {
    if (h < sz3 * 2) {
        uint32_t z = 0;
        for (int i = 15; i >= 0 && !(sz3 & (1u << i)); --i) ++z;
        const uint32_t n = h << z;
        uint32_t d = sz3 << z;
        const uint32_t u = kUnr.v[(d - 0x7FC0) >> 7] + 0x101u;
        d = static_cast<uint32_t>((0x2000080u - d * u) >> 8);
        d = static_cast<uint32_t>((0x0000080u + d * u) >> 8);
        const uint64_t r = (static_cast<uint64_t>(n) * d + 0x8000u) >> 16;
        return static_cast<uint32_t>(std::min<uint64_t>(0x1FFFFu, r));
    }
    return 0x1FFFF;
}

// 20-byte / 32-byte block copies in word order (the originals' lw / sw pairs).
void CopyWords(GuestRam& g, uint32_t dst, uint32_t src, uint32_t words) {
    for (uint32_t k = 0; k < words; ++k) g.W32(dst + 4u * k, g.U32(src + 4u * k));
}

uint32_t Kind(GuestRam& g, uint32_t obj) { return (g.U16(g.U32(obj) + 14u) & 0x78u) >> 3; }
uint32_t Shift(GuestRam& g, uint32_t obj) { return g.U16(g.U32(obj) + 14u) >> 12; }
uint32_t Stack(GuestRam& g) { return g.U32(kStackPtr); }
void SetStack(GuestRam& g, uint32_t v) { g.W32(kStackPtr, v); }

// The three column products the originals write out (0x80067214..0x800672E4, 0x800687F8..0x800688CC):
// RT loaded from `rt`, each column of the 3x3 at `src` (+0 / +2 / +4, rows 6 bytes apart) turned into
// the same column of `dst`. `src` is re-read per column through `srcOf()`.
template <class SrcOf>
void TurnColumns(GuestRam& g, ModelGte& gte, uint32_t dst, SrcOf srcOf) {
    for (uint32_t c = 0; c < 3; ++c) {
        const uint32_t s = srcOf() + 2u * c;
        const int16_t in[3] = {g.S16(s), g.S16(s + 6u), g.S16(s + 12u)};
        int16_t out[3];
        gte.MulIr(in, out);
        g.W16(dst + 2u * c, static_cast<uint16_t>(out[0]));
        g.W16(dst + 2u * c + 6u, static_cast<uint16_t>(out[1]));
        g.W16(dst + 2u * c + 12u, static_cast<uint16_t>(out[2]));
    }
}

void StoreMac(GuestRam& g, uint32_t a, const int32_t mac[3]) {
    for (uint32_t k = 0; k < 3; ++k) g.W32(a + 4u * k, static_cast<uint32_t>(mac[k]));
}

} // namespace

// ============================================================================ the GTE
ModelGte ModelGte::From(const uint32_t cr[32], const uint32_t dr[32]) {
    ModelGte t;
    for (int k = 0; k < 4; ++k) {
        t.rt[2 * k] = static_cast<int16_t>(cr[k] & 0xFFFFu);
        t.rt[2 * k + 1] = static_cast<int16_t>(cr[k] >> 16);
    }
    t.rt[8] = static_cast<int16_t>(cr[4] & 0xFFFFu);
    for (int k = 0; k < 3; ++k) t.tr[k] = static_cast<int32_t>(cr[5 + k]);
    t.l1[0] = static_cast<int16_t>(cr[16] & 0xFFFFu);
    t.l1[1] = static_cast<int16_t>(cr[16] >> 16);
    t.l1[2] = static_cast<int16_t>(cr[17] & 0xFFFFu);
    t.ofx = static_cast<int32_t>(cr[24]);
    t.ofy = static_cast<int32_t>(cr[25]);
    t.h = static_cast<uint16_t>(cr[26] & 0xFFFFu);
    t.sxy2 = dr[14];
    return t;
}

void ModelGte::LoadRt(GuestRam& g, uint32_t a) {
    for (uint32_t k = 0; k < 4; ++k) {
        const uint32_t w = g.U32(a + 4u * k);
        rt[2 * k] = static_cast<int16_t>(w & 0xFFFFu);
        rt[2 * k + 1] = static_cast<int16_t>(w >> 16);
    }
    rt[8] = static_cast<int16_t>(g.U32(a + 16u) & 0xFFFFu);
}

void ModelGte::LoadTr(GuestRam& g, uint32_t a) {
    for (uint32_t k = 0; k < 3; ++k) tr[k] = g.S32(a + 4u * k);
}

void ModelGte::MulIr(const int16_t in[3], int16_t out[3]) const {
    for (int n = 0; n < 3; ++n) {
        int64_t acc = 0;
        acc = Mac44(acc + static_cast<int64_t>(rt[3 * n + 0]) * in[0]);
        acc = Mac44(acc + static_cast<int64_t>(rt[3 * n + 1]) * in[1]);
        acc = Mac44(acc + static_cast<int64_t>(rt[3 * n + 2]) * in[2]);
        out[n] = static_cast<int16_t>(ClampIr(static_cast<int32_t>(acc >> 12)));
    }
}

void ModelGte::MulV0(const int16_t v[3], int32_t mac[3]) const {
    for (int n = 0; n < 3; ++n) {
        int64_t acc = 0;
        acc = Mac44(acc + static_cast<int64_t>(rt[3 * n + 0]) * v[0]);
        acc = Mac44(acc + static_cast<int64_t>(rt[3 * n + 1]) * v[1]);
        acc = Mac44(acc + static_cast<int64_t>(rt[3 * n + 2]) * v[2]);
        mac[n] = static_cast<int32_t>(acc >> 12);
    }
}

int32_t ModelGte::LightRow(const int16_t ir[3]) const {
    int64_t acc = 0;
    acc = Mac44(acc + static_cast<int64_t>(l1[0]) * ir[0]);
    acc = Mac44(acc + static_cast<int64_t>(l1[1]) * ir[1]);
    acc = Mac44(acc + static_cast<int64_t>(l1[2]) * ir[2]);
    return static_cast<int32_t>(acc);
}

uint32_t ModelGte::Rtps(const int16_t v[3], int32_t mac[3]) {
    int64_t a[3];
    for (int n = 0; n < 3; ++n) {
        int64_t acc = static_cast<int64_t>(tr[n]) << 12;
        acc = Mac44(acc + static_cast<int64_t>(rt[3 * n + 0]) * v[0]);
        acc = Mac44(acc + static_cast<int64_t>(rt[3 * n + 1]) * v[1]);
        acc = Mac44(acc + static_cast<int64_t>(rt[3 * n + 2]) * v[2]);
        a[n] = acc;
        mac[n] = static_cast<int32_t>(acc >> 12);
    }
    const int32_t ir1 = ClampIr(static_cast<int32_t>(a[0] >> 12));
    const int32_t ir2 = ClampIr(static_cast<int32_t>(a[1] >> 12));
    const int64_t z = a[2] >> 12;
    const uint32_t sz3 = static_cast<uint32_t>(z < 0 ? 0 : (z > 0xFFFF ? 0xFFFF : z));
    const uint32_t d = GteDivide(h, sz3);
    const int64_t sx = static_cast<int64_t>(d) * ir1 + ofx;
    const int64_t sy = static_cast<int64_t>(d) * ir2 + ofy;
    const int32_t x = ClampSxy(static_cast<int32_t>(sx >> 16));
    const int32_t y = ClampSxy(static_cast<int32_t>(sy >> 16));
    sxy2 = (static_cast<uint32_t>(y & 0xFFFF) << 16) | static_cast<uint32_t>(x & 0xFFFF);
    return sxy2;
}

// ============================================================================ SLUS 0x800220A4
// Software-pipelined: iteration k stores the SXY of vertex k-1 (the first stores the GTE's stale
// SXY2 and is overwritten), and the tail's X clip bits use the SECOND-TO-LAST vertex's SX (t2 is not
// reloaded after the loop) - both as the original.
void ModelVerts(GuestRam& g, ModelGte& gte, uint32_t entry, uint32_t count, uint32_t verts, uint32_t cam,
                uint32_t screen, uint32_t flags) {
    gte.LoadTr(g, entry + 20u);
    gte.LoadRt(g, entry);
    auto bits = [](uint32_t v1, int32_t sy, int32_t sx) {
        uint32_t a0 = ((v1 - 1024u) >> 31) << 5;
        a0 |= ((v1 - 40u) >> 31) << 4;
        a0 |= ((240u - static_cast<uint32_t>(sy)) >> 31) << 3;
        a0 |= (static_cast<uint32_t>(sy) >> 31) << 2;
        a0 |= (((384u - static_cast<uint32_t>(sx)) >> 31) << 1) | (static_cast<uint32_t>(sx) >> 31);
        return a0;
    };
    const uint32_t end = verts + count * 8u;
    uint32_t t1 = screen, t3 = flags, a3 = cam, v1 = 0;
    int32_t t2 = 0;
    for (uint32_t t0 = verts; t0 < end; t0 += 8u) {
        const int16_t v[3] = {g.S16(t0), g.S16(t0 + 2u), g.S16(t0 + 4u)};
        g.W32(t1, gte.sxy2);
        t2 = g.S16(t1);
        int32_t mac[3];
        gte.Rtps(v, mac);
        const int32_t sy = g.S16(t1 + 2u);
        g.W8(t3, static_cast<uint8_t>(bits(v1, sy, t2)));
        if (t0 != verts) {
            t1 += 4u;
            t3 += 1u;
        }
        g.W32(a3, static_cast<uint32_t>(mac[0]));
        g.W32(a3 + 4u, static_cast<uint32_t>(mac[1]));
        g.W32(a3 + 8u, static_cast<uint32_t>(mac[2]));
        v1 = g.U32(a3 + 8u);
        a3 += 16u;
        if (g.Faulted()) return;
    }
    g.W32(t1, gte.sxy2);
    const int32_t sy = g.S16(t1 + 2u);
    g.W8(t3, static_cast<uint8_t>(bits(v1, sy, t2)));
}

// ============================================================================ SLUS 0x8001FD24
void EulerMatrix(GuestRam& g, uint32_t x, uint32_t y, uint32_t z, uint32_t dst) {
    const uint32_t ax = kSinCos + 4u * (x & 0xFFFu), ay = kSinCos + 4u * (y & 0xFFFu), az = kSinCos + 4u * (z & 0xFFFu);
    const int32_t sx = g.S16(ax), cx = g.S16(ax + 2u), sy = g.S16(ay), cy = g.S16(ay + 2u);
    const int32_t sz = g.S16(az), cz = g.S16(az + 2u);
    auto h = [](int32_t v) { return static_cast<uint16_t>(static_cast<uint32_t>(v)); };
    g.W16(dst + 12u, h(-sy));
    g.W16(dst + 14u, h((sx * cy) >> 12));
    g.W16(dst + 16u, h((cx * cy) >> 12));
    const int32_t sxsy = (sx * sy) >> 12;
    const int32_t czcx = (cz * cx) >> 12;
    const int32_t szcx = (sz * cx) >> 12;
    g.W16(dst + 2u, h(((sxsy * cz) >> 12) - szcx));
    g.W16(dst + 0u, h((cz * cy) >> 12));
    g.W16(dst + 8u, h(czcx + ((sxsy * sz) >> 12)));
    g.W16(dst + 6u, h((cy * sz) >> 12));
    g.W16(dst + 4u, h(((sx * sz) >> 12) + ((czcx * sy) >> 12)));
    g.W16(dst + 10u, h(((szcx * sy) >> 12) - ((sx * cz) >> 12)));
}

// ============================================================================ SLUS 0x8004D014
void ScaleMatrix(GuestRam& g, uint32_t m, const int32_t s[3]) {
    // `multu` of sign-extended operands, `mflo`, `sra 12`: the low word of the product, shifted.
    auto mul12 = [](int32_t a, int32_t b) {
        return static_cast<int32_t>(static_cast<uint32_t>(a) * static_cast<uint32_t>(b)) >> 12;
    };
    auto part = [&](uint32_t w, int32_t fl, int32_t fh) {
        const uint32_t l = static_cast<uint32_t>(mul12(static_cast<int16_t>(w & 0xFFFFu), fl)) & 0xFFFFu;
        const uint32_t hi = static_cast<uint32_t>(mul12(static_cast<int32_t>(w) >> 16, fh)) << 16;
        return l | hi;
    };
    g.W32(m + 0u, part(g.U32(m + 0u), s[0], s[1]));
    g.W32(m + 4u, part(g.U32(m + 4u), s[2], s[0]));
    g.W32(m + 8u, part(g.U32(m + 8u), s[1], s[2]));
    g.W32(m + 12u, part(g.U32(m + 12u), s[0], s[1]));
    g.W32(m + 16u, static_cast<uint32_t>(mul12(static_cast<int16_t>(g.U32(m + 16u) & 0xFFFFu), s[2])));
}

// ============================================================================ RASHCDG 0x80066A84
uint32_t SeatVertex(GuestRam& g, uint32_t obj) {
    const int32_t lod = g.S8(obj + 8u);
    const uint32_t dod = g.U32(g.U32(g.U32(obj + 96u) + 8u) + static_cast<uint32_t>(lod * 12));
    const uint32_t n = g.U16(dod + 24u);
    uint32_t idx = 0;
    if (n == 3u) idx = 2;
    else if (n < 4u) return 0;
    else if (n == 5u) idx = 3;
    else if (n == 6u) idx = 4;
    else return 0;
    return g.U32(g.U32(obj) + 36u) + g.U32(g.U32(g.U32(obj + 4u)) + 16u) * 8u + 4u + idx * 8u;
}

// ============================================================================ RASHCDG 0x80066B98
void ChildPlace(GuestRam& g, ModelGte& gte, uint32_t obj, uint32_t k) {
    const uint32_t child = g.U32(obj + 56u + 8u * k);
    const uint32_t s2 = Shift(g, child);
    const uint32_t v1 = (4u - s2) & 31u;
    int16_t p[3] = {static_cast<int16_t>(g.S16(child + 28u) >> v1), static_cast<int16_t>(g.S16(child + 30u) >> v1),
                    static_cast<int16_t>(g.S16(child + 32u) >> v1)};
    gte.LoadRt(g, g.U32(obj + 4u) + 52u);
    int32_t mac[3];
    gte.MulV0(p, mac);
    StoreMac(g, child + 12u, mac);
    uint32_t a1 = SeatVertex(g, obj);
    uint32_t a2 = 0;
    if (a1 == 0) {
        const uint32_t dod = g.U32(g.U32(g.U32(obj + 96u) + 8u));
        a2 = g.U16(dod + 14u) >> 12;
        const uint32_t a0 = g.U32(dod + 36u) + 4u + g.U32(g.U32(g.U32(obj + 4u)) + 16u) * 8u;
        a1 = (g.U16(dod + 24u) < 6u) ? a0 + 24u : a0 + 32u;
    } else {
        a2 = Shift(g, obj);
    }
    const int32_t a0 = static_cast<int32_t>(s2 - a2);
    for (uint32_t c = 0; c < 3; ++c) {
        const int32_t s = g.S16(a1 + 2u * c);
        const int32_t add = a0 >= 0 ? static_cast<int32_t>(static_cast<uint32_t>(s) << (a0 & 31)) : (s >> ((-a0) & 31));
        p[c] = static_cast<int16_t>(static_cast<uint16_t>(g.U16(child + 12u + 4u * c) + static_cast<uint32_t>(add)));
    }
    if (g.S8(obj + 72u) == 3) gte.LoadRt(g, obj + 104u);
    else gte.LoadRt(g, g.U32(obj + 4u) + 4u);
    gte.MulV0(p, mac);
    StoreMac(g, child + 12u, mac);
    for (uint32_t c = 0; c < 3; ++c)
        g.W32(child + 12u + 4u * c, g.U32(child + 12u + 4u * c) + (g.U32(obj + 12u + 4u * c) << (s2 & 31u)));
    CopyWords(g, child + 104u, Stack(g), 8);
}

// ============================================================================ RASHCDG 0x80067064
void AttachWalk(GuestRam& g, ModelGte& gte, uint32_t prog, uint32_t obj) {
    const uint32_t rec = kProgRecords + 3u * prog;
    uint32_t s0 = g.U8(rec + 1u);
    const uint32_t first = g.U8(rec + 0u);
    int32_t s6 = g.U8(rec + 2u);
    const uint32_t s7 = g.U32(obj + 4u);
    uint32_t s3 = kProgWords + 4u * first;
    uint32_t s5 = s7 + 24u;
    if (s6 <= 0) return;
    do {
        int32_t s4 = static_cast<int32_t>(s0);
        if (static_cast<int32_t>(s0) > 0) {
            do {
                const uint32_t s1 = g.U32(s3);
                const uint32_t a3 = g.U32(kVertsPtr);
                uint32_t a1 = Stack(g);
                const uint32_t a0 = (s1 & 3u) << 5;
                const uint32_t parent = s7 + ((s1 >> 13) & 0x1Fu) * 24u;
                const uint32_t a2 = (s1 >> 6) & 0x70u;
                auto src = [&]() { return a2 + (g.U32(g.U32(parent) + 16u) << 4) + a3; };
                const uint32_t x = g.U32(src());
                a1 += a0;
                g.W32(a1 + 20u, x);
                s3 += 4u;
                g.W32(a1 + 24u, g.U32(src() + 4u));
                const uint32_t na0 = a1 + ((s1 << 3) & 0x60u);
                SetStack(g, a1);
                const uint32_t z = g.U32(src() + 8u);
                SetStack(g, na0);
                const uint32_t t3 = Stack(g);
                g.W32(a1 + 28u, z);
                CopyWords(g, t3, s7 + ((s1 >> 23) & 0x1Fu) * 24u + 4u, 5);
                const uint32_t pe = na0 - 32u;
                const uint32_t dst = na0 - ((s1 >> 3) & 0x60u);
                gte.LoadRt(g, pe);
                uint32_t col = 0;
                TurnColumns(g, gte, dst, [&]() { return col++ == 0 ? t3 : Stack(g); });
                const uint32_t dpd = g.U32(s5);
                s5 += 24u;
                s4 -= 1;
                SetStack(g, Stack(g) - ((s1 << 1) & 0x60u));
                s0 = g.U32(dpd + 16u);
                ModelVerts(g, gte, Stack(g), g.U16(dpd + 14u), g.U32(g.U32(obj) + 36u) + s0 * 8u + 4u,
                           g.U32(kVertsPtr) + s0 * 16u, g.U32(kScreenPtr) + s0 * 4u, g.U32(kFlagsPtr) + s0);
                SetStack(g, Stack(g) - ((s1 >> 1) & 0x60u));
                if (g.Faulted()) return;
            } while (s4 > 0);
        }
        const uint32_t w = g.U32(s3);
        if ((w & 3u) == 3u) {
            const uint32_t child = g.U32(obj + 56u);
            s3 += 4u;
            if (child != 0 && g.U32(obj + 60u) == ((w >> 18) & 0x1Fu)) CopyWords(g, child + 104u, Stack(g), 8);
            s0 = (w >> 13) & 0x1Fu;
            SetStack(g, Stack(g) - ((w >> 1) & 0x60u));
        }
        s6 -= 1;
        if (g.Faulted()) return;
    } while (s6 > 0);
}

// ============================================================================ RASHCDG 0x80066EC4
void BikeParts(GuestRam& g, ModelGte& gte, uint32_t obj) {
    if (g.U16(obj + 172u) < g.U32(g.U32(kGameStatePtr) + 48u)) CopyWords(g, kPlayerStack, Stack(g), 8);
    const uint32_t n = g.U16(g.U32(obj) + 24u);
    uint32_t prog = 5;
    bool walk = true;
    if (n < 5u) {
        if (n == 1u) walk = false;
    } else {
        const uint32_t s1 = (n < 6u) ? 0u : 1u;
        EulerMatrix(g, static_cast<uint32_t>(static_cast<int32_t>(g.S16(obj + 836u))), 0, 0, g.U32(obj + 4u) + 76u);
        EulerMatrix(g, static_cast<uint32_t>(static_cast<int32_t>(g.S16(obj + 838u))), 0, 0, g.U32(obj + 4u) + 100u);
        if (s1 != 0)
            EulerMatrix(g, static_cast<uint32_t>(static_cast<int32_t>(g.S16(g.U32(obj + 856u) + 836u))), 0, 0,
                        g.U32(obj + 4u) + 124u);
        prog = 4u - s1;
    }
    if (walk) AttachWalk(g, gte, prog, obj);
    for (uint32_t k = 0; k < 2; ++k)
        if (g.U32(obj + 56u + 8u * k) != 0) ChildPlace(g, gte, obj, k);
    if (g.U16(g.U32(obj) + 24u) != 1u) SetStack(g, Stack(g) - 32u);
    SetStack(g, Stack(g) - 32u);
}

// ============================================================================ RASHCDG 0x8006745C
void RiderParts(GuestRam& g, ModelGte& gte, uint32_t obj) {
    const uint32_t n = g.U16(g.U32(obj) + 24u);
    if (n == 12u) AttachWalk(g, gte, 1, obj);
    else if (n == 4u) AttachWalk(g, gte, 2, obj);
    else if (n == 17u) AttachWalk(g, gte, 0, obj);
}

// ============================================================================ RASHCDG 0x80066B28
void ObjectParts(GuestRam& g, ModelGte& gte, uint32_t obj) {
    const uint32_t n = g.U16(g.U32(obj) + 24u);
    if (n == 4u) AttachWalk(g, gte, 6, obj);
    else if (n == 3u) AttachWalk(g, gte, 7, obj);
    else SetStack(g, Stack(g) - 32u);
}

// ============================================================================ RASHCDG 0x80067AC4
uint32_t ModelVisible(GuestRam& g, ModelGte& gte, uint32_t obj, uint32_t view) {
    const uint32_t bit = 1u << (view & 31u);
    const uint32_t camPtr = kRenderCamPtrs + 4u * view;
    const uint32_t s8 = g.U32(camPtr) + 92u;
    const uint32_t s4 = obj + 80u;
    const uint32_t s7 = Kind(g, obj);
    if (g.U32(obj + 52u) == 0 && g.S32(obj + 176u) == -1) return 0;
    SetStack(g, 0x1F8002E0u);
    auto invisible = [&]() {
        const uint32_t v = g.U8(obj + 9u) & ~bit;
        g.W8(obj + 9u, static_cast<uint8_t>(v));
        return (v & 0xFFu) & bit;
    };
    auto link = [&]() {
        if (g.U32(obj + 52u) == 0) {
            const uint32_t head = g.U32(kDrawList);
            if (head != 0) {
                g.W32(obj + 168u, head);
                g.W32(kDrawList, obj);
            } else {
                g.W32(kDrawList, obj);
                g.W32(obj + 168u, 0);
            }
        }
        return g.U8(obj + 9u) & bit;
    };
    if (g.S8(obj + 72u) == 1) {
        g.W8(obj + 9u, static_cast<uint8_t>(g.U8(obj + 9u) | bit));
        const uint32_t a3 = Shift(g, obj);
        CopyWords(g, obj + 136u, obj + 104u, 8);
        if (s7 == 5u) {
            g.W32(s4, g.U32(obj + 156u));
            g.W32(s4 + 4u, g.U32(obj + 160u));
            g.W32(s4 + 8u, g.U32(obj + 164u));
            g.W32(obj + 164u, 0);
            g.W32(obj + 160u, 0);
            g.W32(obj + 156u, 0);
        } else {
            const uint32_t a1 = (10u - a3) & 31u;
            int16_t p[3];
            for (uint32_t c = 0; c < 3; ++c)
                p[c] = static_cast<int16_t>(static_cast<uint16_t>(
                    g.U16(obj + 12u + 4u * c) - static_cast<uint32_t>(g.S32(g.U32(camPtr) + 16u + 4u * c) >> a1)));
            gte.LoadRt(g, s8);
            int32_t mac[3];
            gte.MulV0(p, mac);
            StoreMac(g, s4, mac);
        }
        return link();
    }
    {
        const int32_t range = g.S32(kDrawRanges + 4u * s7);
        const int32_t dist = g.S32(obj + 44u + 4u * view);
        bool far = range < dist;
        if (s7 == 6u) far = (g.S8(obj + 8u) != 0) ? (range < dist) : (range + 32000 < dist);
        if (far) return invisible();
        if (dist < 0) return invisible();
    }
    if (g.U32(obj + 100u) != 0) {
        LodSelect(g, obj, static_cast<uint32_t>(static_cast<int32_t>(g.S8(obj + 10u + view))));
        g.W32(obj + 36u, g.U32(obj + 36u) & ~0x80u);
    }
    for (uint32_t k = 0; k < 2; ++k) {
        const uint32_t c = g.U32(obj + 56u + 8u * k);
        if (c != 0 && g.U32(c + 100u) != 0) {
            LodSelect(g, c, static_cast<uint32_t>(static_cast<int32_t>(g.S8(c + 10u + view))));
            g.W32(c + 36u, g.U32(c + 36u) & ~0x80u);
        }
    }
    {
        const uint32_t a0 = g.U32(obj + 36u);
        if (((a0 >> 5) & 3u) != 1u &&
            static_cast<uint32_t>(g.S32(obj + 44u + 4u * view) - 129) < 639u) {
            if (g.U32(g.U32(kGameStatePtr) + 48u) == 1u && s7 != 3u) {
                uint32_t v1 = 0;
                if (g.U32(g.U32(obj) + 44u) != 0) {
                    g.W32(obj + 36u, a0 | 0x80u);
                    v1 = 1;
                }
                for (uint32_t k = 0; k < 2; ++k) {
                    const uint32_t c = g.U32(obj + 56u + 8u * k);
                    if (c != 0 && g.U32(c + 100u) != 0 && g.U32(g.U32(c) + 44u) != 0) {
                        ++v1;
                        g.W32(c + 36u, g.U32(c + 36u) | 0x80u);
                    }
                }
                g.W8(obj + 9u, static_cast<uint8_t>((v1 << 4) | (g.U8(obj + 9u) & 0xCFu)));
            } else {
                g.W32(obj + 36u, (g.U32(obj + 36u) & ~0x80u) | ((s7 - 1u < 2u ? 1u : 0u) << 7));
            }
        }
    }
    const uint32_t a3 = Shift(g, obj);
    if (g.S8(obj + 72u) == 3) {
        int32_t w[3] = {static_cast<int32_t>(g.U32(obj + 12u) << a3), static_cast<int32_t>(g.U32(obj + 16u) << a3),
                        static_cast<int32_t>(g.U32(obj + 20u) << a3)};
        if (g.U8(obj + 9u) & 4u) {
            int32_t mac[3];
            gte.LoadRt(g, obj + 104u);
            if (a3 == 4u) {
                const int16_t v[3] = {g.S16(obj + 28u), g.S16(obj + 30u), g.S16(obj + 32u)};
                gte.MulV0(v, mac);
            } else {
                const uint32_t sh = (4u - a3) & 31u;
                const int16_t v[3] = {static_cast<int16_t>(g.S16(obj + 28u) >> sh),
                                      static_cast<int16_t>(g.S16(obj + 30u) >> sh),
                                      static_cast<int16_t>(g.S16(obj + 32u) >> sh)};
                gte.MulV0(v, mac);
            }
            for (int c = 0; c < 3; ++c) w[c] = static_cast<int32_t>(static_cast<uint32_t>(w[c]) + static_cast<uint32_t>(mac[c]));
            for (uint32_t c = 0; c < 3; ++c)
                g.W32(obj + 12u + 4u * c, g.U32(obj + 12u + 4u * c) + static_cast<uint32_t>(mac[c] >> a3));
        }
        SetStack(g, Stack(g) + 32u);
        const uint32_t t2 = Stack(g);
        CopyWords(g, t2, s8, 8);
        const uint32_t a1 = (10u - a3) & 31u;
        int16_t p[3];
        for (uint32_t c = 0; c < 3; ++c)
            p[c] = static_cast<int16_t>(static_cast<uint16_t>(static_cast<uint32_t>(w[c]) & 0xFFFFu) -
                                        static_cast<uint32_t>(g.S32(g.U32(camPtr) + 16u + 4u * c) >> a1));
        gte.LoadRt(g, t2);
        int32_t mac[3];
        gte.MulV0(p, mac);
        StoreMac(g, s4, mac);
    } else {
        const uint32_t rc = g.U32(camPtr);
        int16_t p[3];
        p[0] = static_cast<int16_t>((g.S32(obj + 184u) - g.S32(rc + 16u)) >> 10);
        p[1] = static_cast<int16_t>((g.S32(obj + 188u) - g.S32(g.U32(camPtr) + 20u)) >> 10);
        SetStack(g, Stack(g) + 32u);
        const uint32_t t2 = Stack(g);
        p[2] = static_cast<int16_t>((g.S32(obj + 192u) - g.S32(g.U32(camPtr) + 24u)) >> 10);
        CopyWords(g, t2, s8, 8);
        gte.LoadRt(g, t2);
        int32_t mac[3];
        gte.MulV0(p, mac);
        StoreMac(g, s4, mac);
        g.W32(s4, g.U32(s4) << a3);
        g.W32(s4 + 8u, g.U32(s4 + 8u) << a3);
        g.W32(s4 + 4u, g.U32(s4 + 4u) << a3);
    }
    // 0x800682B4: the frustum test on the camera-space centre, widened by the radius +0x28.
    int32_t x = g.S32(s4), y = g.S32(s4 + 4u);
    const uint32_t r = g.U32(obj + 40u);
    const uint32_t sh = a3 & 31u;
    int32_t z;
    if (s7 == 3u) {
        x >>= sh;
        y >>= sh;
        z = g.S32(s4 + 8u) >> sh;
    } else {
        x = static_cast<int32_t>(x >= 0 ? static_cast<uint32_t>(x) - r : static_cast<uint32_t>(x) + r) >> sh;
        y = static_cast<int32_t>(y >= 0 ? static_cast<uint32_t>(y) - r : static_cast<uint32_t>(y) + r) >> sh;
        z = static_cast<int32_t>(g.U32(s4 + 8u) + r) >> sh;
    }
    auto neg = [](int32_t v) { return static_cast<int32_t>(0u - static_cast<uint32_t>(v)); }; // negu
    if (z < x || z < neg(x) || z < y || z < neg(y) || z < 40) {
        SetStack(g, Stack(g) - 32u);
        return invisible();
    }
    g.W8(obj + 9u, static_cast<uint8_t>(g.U8(obj + 9u) | bit));
    const uint32_t t2 = Stack(g);
    CopyWords(g, obj + 136u, t2, 8);
    SetStack(g, t2 - 32u);
    return link();
}

// ============================================================================ RASHCDG 0x800667C4
void ModelLod(GuestRam& g, uint32_t obj, uint32_t views) {
    const uint32_t kind = Kind(g, obj);
    uint32_t t2 = 0;
    do { // a do-while: the first view is always walked (`slt t2,a1` at the bottom)
        const uint32_t table = g.U32(obj + 100u);
        const int32_t dist = g.S32(obj + 44u + 4u * t2);
        if (table != 0) {
            int32_t a2 = g.S8(obj + 10u + t2);
            const int32_t top = static_cast<int32_t>(g.U8(g.U32(obj + 96u) + 4u)) - 1;
            if (a2 < top && g.S32(table + static_cast<uint32_t>(a2) * 8u) < dist) {
                uint32_t p = table + static_cast<uint32_t>(a2) * 8u;
                do {
                    p += 8u;
                    a2 += 1;
                } while (g.S32(p) < dist);
            } else if (a2 != 0) {
                uint32_t p = g.U32(obj + 100u) + static_cast<uint32_t>(a2) * 8u;
                if (dist < g.S32(p + 4u)) {
                    do {
                        p -= 8u;
                        a2 -= 1;
                    } while (dist < g.S32(p + 4u));
                }
            }
            const int32_t m = static_cast<int32_t>(g.U8(g.U32(obj + 96u) + 4u)) - 1;
            g.W8(obj + 10u + t2, static_cast<uint8_t>(a2 < m ? a2 : m));
        }
        if (g.Faulted()) return;
        ++t2;
    } while (static_cast<int32_t>(t2) < static_cast<int32_t>(views));
    int32_t lod;
    if (static_cast<int32_t>(views) < 2) {
        lod = g.S8(obj + 10u);
    } else {
        const int8_t a = g.S8(obj + 10u), b = g.S8(obj + 11u);
        lod = a < b ? a : b;
    }
    if (kind == 1u || kind == 4u) {
        uint32_t interp = 0;
        if (lod == 0) {
            interp = 1;
        } else if (lod == 1 && kind == 1u) {
            if (static_cast<int32_t>(g.U16(obj + 172u) & 0x1Fu) < g.S32(g.U32(kGameStatePtr) + 48u)) interp = 1;
            else if (g.U8(obj + 572u) & 0x20u) interp = 1;
        }
        const uint32_t k2 = Kind(g, obj);
        const uint32_t anim = g.U32(obj + 540u);
        if (k2 == 1u || k2 == 4u) {
            const uint32_t idx = k2 == 4u ? static_cast<uint32_t>(lod + 1) * 4u : static_cast<uint32_t>(lod) * 4u;
            g.W32(anim + 1760u, g.U32(0x80052390u + idx));
            g.W32(anim + 36u, (g.U32(anim + 36u) & ~4u) | (interp << 2));
        }
    }
    g.W8(obj + 9u, static_cast<uint8_t>(g.U8(obj + 9u) & 0xF7u));
}

// ============================================================================ RASHCDG 0x80068468
bool ModelDraw(GuestRam& g, ModelGte& gte, uint32_t obj, uint32_t view, DrawCallees& callees) {
    const uint32_t s3 = Kind(g, obj);
    SetStack(g, 0x1F800300u);
    CopyWords(g, Stack(g), obj + 136u, 8);
    const uint32_t e = 0x1F800320u;
    g.W32(e + 20u, g.U32(obj + 80u));
    g.W32(e + 24u, g.U32(obj + 84u));
    const uint32_t tz = g.U32(obj + 88u);
    SetStack(g, e);
    g.W32(e + 28u, tz);
    if (g.S8(obj + 72u) != 3) {
        CopyWords(g, Stack(g), g.U32(obj + 4u) + 4u, 5);
    } else {
        gte.LoadRt(g, obj + 104u);
        uint32_t col = 0;
        (void)col;
        for (uint32_t c = 0; c < 3; ++c) {
            const uint32_t s = g.U32(obj + 4u) + 4u + 2u * c;
            const int16_t in[3] = {g.S16(s), g.S16(s + 6u), g.S16(s + 12u)};
            int16_t out[3];
            gte.MulIr(in, out);
            g.W16(obj + 104u + 2u * c, static_cast<uint16_t>(out[0]));
            g.W16(obj + 110u + 2u * c, static_cast<uint16_t>(out[1]));
            g.W16(obj + 116u + 2u * c, static_cast<uint16_t>(out[2]));
        }
        CopyWords(g, Stack(g), obj + 104u, 5);
        if ((s3 == 1u || s3 == 4u) && g.U32(obj + 52u) == 0 && g.S32(obj + 44u + 4u * view) < 1280) {
            // 0x800669E8: the object's position in its own unit
            const uint32_t sh = Shift(g, obj);
            int32_t p[3];
            for (uint32_t c = 0; c < 3; ++c)
                p[c] = g.S8(obj + 72u) == 1 ? (g.S32(obj + 12u + 4u * c) >> sh) : g.S32(obj + 12u + 4u * c);
            for (uint32_t c = 0; c < 3; ++c)
                g.W32(obj + 244u + 4u * c,
                      (static_cast<uint32_t>(p[c]) << 10) - g.U32(obj + 184u + 4u * c));
            static constexpr uint32_t kNeg[6][2] = {{262, 104}, {264, 110}, {266, 116}, {268, 106}, {270, 112}, {272, 118}};
            for (const auto& n : kNeg) g.W16(obj + n[0], static_cast<uint16_t>(-static_cast<int32_t>(g.S16(obj + n[1]))));
            g.W16(obj + 256u, static_cast<uint16_t>(g.S16(obj + 108u)));
            g.W16(obj + 258u, static_cast<uint16_t>(g.S16(obj + 114u)));
            g.W16(obj + 260u, static_cast<uint16_t>(g.S16(obj + 120u)));
            g.W8(obj + 547u, 1);
        }
    }
    // 0x800687C0: the object's matrix through the parent entry (the camera)
    gte.LoadRt(g, Stack(g) - 32u);
    TurnColumns(g, gte, Stack(g), [&]() { return Stack(g); });
    if (g.U32(obj + 76u) != 0x10000u) {
        const int32_t s[3] = {static_cast<int32_t>(g.U32(obj + 76u) << 12) >> 16, 4096, 4096};
        ScaleMatrix(g, Stack(g), s);
    }
    {
        const uint32_t dpd = g.U32(g.U32(obj + 4u));
        const uint32_t vb = g.U32(dpd + 16u);
        ModelVerts(g, gte, Stack(g), g.U16(dpd + 14u), g.U32(g.U32(obj) + 36u) + vb * 8u + 4u,
                   g.U32(kVertsPtr) + vb * 16u, g.U32(kScreenPtr) + vb * 4u, g.U32(kFlagsPtr) + vb);
    }
    if (g.Faulted()) return false;
    if ((g.U8(g.U32(kGameStatePtr) + 4u) & 0x10u) == 0 && g.U32(g.U32(obj) + 40u) != 0) {
        // the light: the level's light vector into the camera's frame (RT = the render camera
        // transposed by SLUS 0x8004D264), then one byte per model normal
        const uint32_t rc = g.U32(kRenderCamPtrs + 4u * view) + 92u;
        int16_t m[9];
        for (uint32_t k = 0; k < 9; ++k) m[k] = g.S16(rc + 2u * k);
        static constexpr int kT[9] = {0, 3, 6, 1, 4, 7, 2, 5, 8};
        for (int k = 0; k < 9; ++k) gte.rt[k] = m[kT[k]];
        const int16_t lv[3] = {g.S16(kLightVector), g.S16(kLightVector + 2u), g.S16(kLightVector + 4u)};
        int32_t mac[3];
        gte.MulV0(lv, mac);
        gte.l1[0] = static_cast<int16_t>(mac[0]);
        gte.l1[1] = static_cast<int16_t>(mac[1]);
        gte.l1[2] = static_cast<int16_t>(mac[2]);
        auto normal = [&](uint32_t off) {
            const uint32_t nb = g.U32(g.U32(obj) + 40u) + off;
            const int16_t ir[3] = {g.S16(nb), g.S16(nb + 2u), g.S16(nb + 4u)};
            return gte.LightRow(ir) >> 8;
        };
        auto put = [&](int32_t v, int32_t a0) {
            if (v < 0) v = 0;
            v >>= 11;
            if (v >= 32) v = 31;
            g.W8(0x1F80013Fu + static_cast<uint32_t>(a0), static_cast<uint8_t>(v));
        };
        int32_t sp96 = normal(4u);
        const int32_t count = g.S32(g.U32(g.U32(obj) + 40u));
        int32_t a0 = 1;
        uint32_t a1 = 12;
        while (a0 < count) {
            const int32_t next = normal(a1);
            put(sp96, a0);
            a1 += 8u;
            a0 += 1;
            sp96 = next;
        }
        put(sp96, a0);
    }
    switch (s3) {
    case 1:
    case 4:
        RiderParts(g, gte, obj);
        break;
    case 2: {
        const uint32_t a0 = g.U32(obj + 596u);
        bool set = false;
        if (a0 != 0) set = (g.U32(obj + 560u) & 0x08000000u) == 0 || g.S32(obj + 600u) < static_cast<int32_t>(a0 << 1);
        g.W32(obj + 36u, set ? (g.U32(obj + 36u) | 0x40000u) : (g.U32(obj + 36u) & 0xFFFBFFFFu));
        BikeParts(g, gte, obj);
        break;
    }
    case 3:
    case 5:
    case 6:
        ObjectParts(g, gte, obj);
        break;
    default:
        break;
    }
    if (g.Faulted()) return false;
    SetStack(g, Stack(g) - 32u);
    if (!callees.Emit(obj, view)) return false;
    for (uint32_t k = 0; k < 2; ++k) {
        const uint32_t c = g.U32(obj + 56u + 8u * k);
        if (c == 0) continue;
        if ((s3 != 1u && s3 != 4u) || g.U16(g.U32(obj) + 24u) == 17u) {
            ModelVisible(g, gte, c, view);
            if (!ModelDraw(g, gte, c, view, callees)) return false;
        }
    }
    return !g.Faulted();
}

// ============================================================================ RASHCDG 0x800674D4
bool RenderModels(GuestRam& g, ModelGte& gte, rr::sim::FxEnv& fx, uint32_t cells, int32_t count, uint32_t view,
                  DrawCallees& callees) {
    // the scratchpad's depth ranges (0x800674FC..0x800675B8, in the original's order)
    g.W16(0x1F80000Au, 0x800);
    g.W16(0x1F80000Cu, 0x1000);
    g.W16(0x1F800012u, 0x200);
    g.W16(0x1F800014u, 0x300);
    g.W8(0x1F800018u, 0xFD);
    const uint32_t shift = g.U32(0x8005B4D8u);
    g.W8(0x1F800019u, 0xFE);
    const uint32_t nearZ = g.U32(0x8005B4D4u);
    const uint32_t otLen = g.U32(0x8005ADFCu);
    g.W16(0x1F800008u, 0);
    g.W16(0x1F800010u, 0);
    g.W8(0x1F80001Au, 0);
    g.W32(0x1F800004u, nearZ);
    const uint32_t screen = g.U32(kScreenPtr);
    g.W32(0x1F800000u, shift);
    g.W32(0x1F80001Cu, otLen - 2u);
    g.W32(0x1F800208u, screen);
    for (uint32_t k = 0; k < 32; ++k) g.W32(0x1F800020u + 4u * k, g.U32(0x800D50A8u + 4u * k)); // the tpage words
    auto cellAt = [&](int32_t i) { return g.U32(kCellRecords + 8u + 112u * g.U32(cells + 4u * static_cast<uint32_t>(i))); };
    // 0x80067690: every draw-list entry of the cell (LOD > 0, or a DOD3 halfword +0x0E either way: the
    // 0x8006780C arm is unreachable - its selector is set in a delay slot)
    for (int32_t i = 0; i < count; ++i) {
        const uint32_t cell = cellAt(i);
        for (uint32_t p = g.U32(kDrawList); p != 0; p = g.U32(p + 168u)) {
            if (g.U32(p + 176u) != cell) continue;
            const uint32_t sel = (g.U8(p + 9u) & 0x30u) >> 4;
            g.W32(0x800CCD78u, sel);
            g.W32(0x800CCDA0u, sel);
            if (!ModelDraw(g, gte, p, view, callees)) return false;
        }
    }
    // 0x80067770: the effect pass of the cell's entries; `prev` is the element made current last (the
    // original's `move s1,s0` sits in the loop's delay slot), so only the list's head is ever unlinked
    for (int32_t i = 0; i < count; ++i) {
        const uint32_t cell = cellAt(i);
        uint32_t prev = 0;
        uint32_t p = g.U32(kDrawList);
        while (p != 0) {
            if (g.U32(p + 176u) == cell) {
                rr::sim::EffectPass(g, fx, p, view);
                if (fx.refused || g.Faulted()) return false;
                p = g.U32(p + 168u);
                if (prev != 0) g.W32(prev + 168u, p);
                else g.W32(kDrawList, p);
            } else {
                p = g.U32(p + 168u);
            }
            prev = p;
        }
    }
    return !g.Faulted();
}

} // namespace rr::sim::model
