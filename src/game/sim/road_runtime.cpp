#include "game/sim/road_runtime.h"

#include "game/sim/ai.h"
#include "game/sim/fixed.h"
#include "game/sim/vec.h"

// Every function below is transcribed from our own disassembly of SLUS_010.53
// (SHA-1 67ed165a2c517d4e6106fb0dfa324d66dd9a76f1). Comments give the original's addresses. Loads
// that may fault (pointer chases) are made in the original's order; arithmetic wraps in uint32_t
// wherever the original's does.

namespace rr::sim {

namespace {

inline uint32_t U(int32_t v) { return static_cast<uint32_t>(v); }
inline int32_t S(uint32_t v) { return static_cast<int32_t>(v); }
inline int32_t Add(int32_t a, int32_t b) { return S(U(a) + U(b)); }
inline int32_t Sub(int32_t a, int32_t b) { return S(U(a) - U(b)); }
// `(s32)(s16) << 4` with `sll` (a direction in 4096 = 1.0 promoted to 16.16).
inline int32_t Promote(int16_t v) { return S(U(static_cast<int32_t>(v)) << 4); }
// The `sra / addu / xor` absolute value: INT32_MIN stays negative.
inline int32_t Abs(int32_t v) {
    const uint32_t sg = U(v >> 31);
    return S((U(v) + sg) ^ sg);
}
// `divu` remainder; the R3000 leaves the dividend in hi for a zero divisor, as src\interp does.
inline uint32_t URem(uint32_t n, uint32_t d) { return (d == 0u) ? n : (n % d); }

// SLUS 0x8002E698 DotLcm over guest addresses: v by three `lh`, m by `lhu, lhu, lh`; its one store is
// MAC1 >> 8 spilled to its own sp-8 (0x8002E6E8). `sp` is the stack pointer at the call.
int32_t DotLcmView(GuestRam& m, uint32_t v, uint32_t row, uint32_t sp) {
    const int16_t a[3] = {m.S16(v + 0), m.S16(v + 2), m.S16(v + 4)};
    const int16_t b0 = static_cast<int16_t>(m.U16(row + 0));
    const int16_t b1 = static_cast<int16_t>(m.U16(row + 2));
    const int16_t b[3] = {b0, b1, m.S16(row + 4)};
    const int32_t r = DotLcm(a, b);
    m.W32(sp - 8u, U(r));
    return r;
}

// RASHCDG 0x800B6AAC AiProject over guest addresses (a leaf, no stores).
int32_t AiProjectView(GuestRam& m, uint32_t p, uint32_t axis, uint32_t org) {
    const int32_t pp[3] = {m.S32(p + 0), m.S32(p + 4), m.S32(p + 8)};
    const int16_t ax[3] = {m.S16(axis + 0), m.S16(axis + 2), m.S16(axis + 4)};
    const int32_t o[3] = {m.S32(org + 0), m.S32(org + 4), m.S32(org + 8)};
    return AiProject(pp, ax, o);
}

// The `+-5` rewrite of +0x16C that RoadTrack (0x80037064) and RoadReseat (0x800399A4) share: the
// sub-object tag at `sub + 6` compared with the one saved before, done branch-free in the original.
void TagRewrite(GuestRam& m, uint32_t e, uint16_t savedTag) {
    const int16_t now = m.S16(m.U32(e + 0x150) + 6);
    const int32_t dir = m.S32(e + 0x16C);
    const uint32_t mask = (now == static_cast<int16_t>(savedTag)) ? 0xFFFFFFFFu : 0u;
    int32_t a0, v0;
    if (dir > 0) {
        a0 = 5;
        v0 = Sub(dir, 5);
    } else {
        a0 = -5;
        v0 = Add(dir, 5);
    }
    m.W32(e + 0x16C, U(a0) + (mask & U(v0)));
}

} // namespace

// ============================================================================ the leaves

int32_t RoadEdgeClass(GuestRam& m, uint32_t group, int32_t lateral, int32_t side, uint32_t out) {
    const int32_t lat = Abs(lateral);                               // 0x8003E67C
    const uint32_t h = group + ((side == 2) ? 136u : 8u);
    const int32_t outer = m.S32(h + 8);
    const int16_t band = m.S16(h + 6);
    const int32_t edge5 = m.S32(h + 72);
    if (band > 0 && lat < Abs(edge5)) {                             // class 5: +0 then +2
        const uint16_t sub = m.U16(h + 82);
        m.W16(out + 0, 5);
        m.W16(out + 2, sub);
        return 0;
    }
    int32_t inner = outer;                                          // 0x8003E6D8
    if (m.S16(h + 2) > 0) inner = m.S32(h + 24);
    if (lat < Abs(inner)) {                                         // tarmac: +2 then +0
        m.W16(out + 2, 1);
        m.W16(out + 0, 4);
        return 0;
    }
    if (lat < Abs(outer)) {                                         // the shoulder: +0 then +2
        const uint16_t sub = m.U16(h + 18);
        m.W16(out + 0, 1);
        m.W16(out + 2, sub);
        return 0;
    }
    m.W16(out + 2, 0);                                              // off the road: +2 then +0
    m.W16(out + 0, 0);
    return 1;
}

int32_t SegmentStraddle(GuestRam& m, uint32_t e, uint32_t r, uint32_t sp) {
    const uint32_t F = sp - 16u;                                    // `addiu sp,sp,-16`
    const int32_t tx = Promote(m.S16(m.U32(e + 0x154) + 14));
    m.W32(F + 0, U(tx));                                            // dead spill
    const int32_t tz = Promote(m.S16(m.U32(e + 0x154) + 18));
    m.W32(F + 4, U(tz));                                            // dead spill
    const int32_t px = m.S32(e + 0xB8);
    const int32_t dx1 = Sub(px, m.S32(r + 0x14));
    const int32_t pz = m.S32(e + 0xC0);
    const int32_t dz1 = Sub(pz, m.S32(r + 0x1C));
    const int32_t dx0 = Sub(px, m.S32(r + 0x08));
    const int32_t dz0 = Sub(pz, m.S32(r + 0x10));
    const int32_t a = Add(FixMul(tz, dz1), FixMul(tx, dx1));
    const int64_t last = static_cast<int64_t>(tz) * static_cast<int64_t>(dz0);
    m.W32(F + 8, static_cast<uint32_t>(static_cast<uint64_t>(last)));          // dead spill, lo
    m.W32(F + 12, static_cast<uint32_t>(static_cast<uint64_t>(last) >> 32));   // dead spill, hi
    const int32_t b = Add(FixMul(tz, dz0), FixMul(tx, dx0));
    return ((a ^ b) < 0) ? 1 : 0;                                   // 0x8003DDA4 `slti v0,v0,0`
}

uint32_t RoadsideRunOfSlice(GuestRam& m, uint32_t e) {
    const uint32_t v = m.U32(e + 0x154) + 0x2Cu;                    // 0x8003F1F0
    m.W32(e + 0x1EC, v);                                            // the delay slot of `jr ra`
    return v;
}

uint32_t RouteFindLegView(GuestRam& m, uint32_t o, uint32_t key) {
    if (m.S16(kRouteRecordCount) == -1) return 0;                  // 0x8003B4B0
    if (o == 0) return 0;
    const int32_t n = m.S32(o + 12);
    for (int32_t i = 0; i < n; ++i) {
        const uint32_t leg = o + 20u + 16u * U(i);
        if (m.U32(leg) == key) return leg;
    }
    return 0;
}

void RoadProjectView(GuestRam& m, uint32_t p, uint32_t slice, uint32_t lateralOut,
                     uint32_t alongOut, uint32_t sp) {
    const uint32_t F = sp - 8u;                                     // `addiu sp,sp,-8`
    // One dot product: row `rowOff` (s16 x3) promoted, against p - pos, three truncated products
    // summed as words; the last product's lo/hi are spilled to F+0/F+4 (0x800368A4).
    const auto dot = [&m, p, slice, F](uint32_t rowOff) {
        uint32_t sum = 0;
        for (uint32_t k = 0; k < 3; ++k) {
            const int32_t axis = Promote(m.S16(slice + rowOff + 2u * k));
            const int32_t d = Sub(m.S32(p + 4u * k), m.S32(slice + 20u + 4u * k));
            if (k == 2) {
                const uint64_t prod = static_cast<uint64_t>(static_cast<int64_t>(axis) * d);
                m.W32(F + 0, static_cast<uint32_t>(prod));
                m.W32(F + 4, static_cast<uint32_t>(prod >> 32));
            }
            sum += U(FixMul(axis, d));
        }
        return sum;
    };
    if (lateralOut != 0) m.W32(lateralOut, dot(2));                 // row 0, SLCT +0x02
    if (alongOut != 0) m.W32(alongOut, dot(14));                    // row 2, SLCT +0x0E
}

void MulAddView(GuestRam& m, uint32_t base, uint32_t dir, int32_t t, uint32_t out) {
    for (uint32_t k = 0; k < 3; ++k) {                              // 0x8002EAD8
        const int32_t d = Promote(m.S16(dir + 2u * k));
        const int32_t b = m.S32(base + 4u * k);
        m.W32(out + 4u * k, U(Add(FixMul(d, t), b)));
    }
}

// ============================================================================ the classifier

int32_t RoadCrossSection(GuestRam& m, uint32_t cursor, uint32_t pos, uint32_t out) {
    int32_t n = 0;                                                  // t0
    const uint32_t obj = m.U32(cursor + 0);
    const uint32_t piece = m.U32(cursor + 4);
    const uint32_t sub = m.U32(cursor + 8);
    const int16_t core = m.S16(piece + 2);
    const uint32_t slice = m.U32(cursor + 12);
    if (core == 0) {                                                // 0x8003EF78
        n = m.S16(sub + 16);
        if (n < 0) n = 0;
    }
    uint32_t prev = m.U32(out + 4);                                 // s0, the cached XSIH record
    const int32_t first = m.S16(sub + 8);
    const int32_t count = m.S16(sub + 10);
    int32_t a = m.S32(pos + 8);                                     // a3, the along-distance
    const uint32_t last = m.U32(obj + 0x34) + U(first + count) * 52u - 52u;
    const int16_t idx = m.S16(slice + 0);
    const int32_t lateral = m.S32(cursor + 16);                     // t3
    bool clamp = false;
    if (idx == 0 && a < m.S32(slice + 40)) clamp = true;            // before the first slice
    if (!clamp && slice == last) {                                  // after the last one
        const int32_t d = m.S32(slice + 40);
        const int32_t c = m.S32(slice + 32);
        if (Add(d, c) < a) clamp = true;
    }
    if (clamp) {                                                    // 0x8003F008: upper bound first
        const int32_t lo = m.S32(slice + 40);
        const int32_t ch = m.S32(slice + 32);
        a = m.S32(pos + 8);
        const int32_t hi = Add(lo, ch);
        if (hi < a) a = hi;
        if (!(lo < a)) a = lo;
    }
    if (n == 0 || m.U32(out + 0) == 0) prev = 0;                    // 0x8003F040
    uint32_t xs = 0;                                                // s1
    int32_t searched = 0;                                           // s3
    if (prev != 0) {                                                // the cache: UNCLAMPED, 32 bits
        const int32_t from = m.S32(prev + 8);
        const int32_t along = m.S32(pos + 8);
        if (along < from || m.S32(prev + 12) < along || m.S16(prev + 4) < 0) prev = 0;
        else xs = m.U32(out + 0);
    }
    if (n > 0 && prev == 0) {                                       // 0x8003F0C8: the search
        const int32_t hiA = a >> 16;
        const int32_t firstX = m.S16(sub + 18);
        const uint32_t xsih = m.U32(obj + 0x38);
        for (int32_t i = 0; i < n; ++i) {
            const uint32_t r = xsih + (U(firstX + i) << 4);
            if (hiA < m.S16(r + 10)) continue;
            if (m.S16(r + 14) < hiA) continue;
            prev = r;
            const int16_t g = m.S16(r + 4);
            xs = 0;
            if (g >= 0) xs = m.U32(obj + 0x3C) + U(static_cast<int32_t>(g)) * 264u;
            break;
        }
        searched = 1;
    }
    int32_t nib = 0;                                                // t4
    if (xs != 0) {                                                  // 0x8003F158
        m.W32(out + 0x34, m.U32(xs + 4));
        m.W32(out + 0x28, m.U32(xs + 0x90));
        m.W16(out + 0x30, m.U16(xs + 0x8C));
        m.W32(out + 0x1C, m.U32(xs + 0x10));
        m.W16(out + 0x24, m.U16(xs + 0x0C));
        nib = RoadEdgeClass(m, xs, lateral, (lateral >= 0) ? 2 : 1, out + 20);
    }
    const uint32_t w = m.U32(out + 16);                             // 0x8003F1B0
    m.W32(out + 0, xs);
    m.W32(out + 4, prev);
    m.W32(out + 8, 0);
    m.W32(out + 12, 0);
    m.W32(out + 16, (w & ~0xFu) | U(nib));
    return searched;
}

uint32_t RoadClassNode(GuestRam& m, uint32_t e, uint32_t out, uint32_t cursorOut, int32_t zone,
                       uint32_t sp, RoadRuntimeCallees& calls) {
    const uint32_t F = sp - 56u;                                    // `addiu sp,sp,-56`
    uint32_t r = 0;                                                 // s4
    uint32_t xs = 0;                                                // s1
    const uint32_t piece = m.U32(e + 0x14C);
    const int16_t core = m.S16(piece + 2);
    const uint32_t obj = m.U32(e + 0x148);
    if (core == 1) {
        const uint32_t node = RoadNodeRecord(m, m.S32(obj + 0));    // 0x8003EEB8
        if (node != 0) {
            m.W32(F + 16, U(zone));                                 // 0x8003EED8, the fifth argument
            r = calls.RoadClassCore(m, e, node, out, cursorOut, zone, F);   // 0x8003EED4
            xs = m.U32(out + 0);
        }
    }
    if (xs == 0) {                                                  // 0x8003EEF0
        const uint32_t w = m.U32(out + 16);
        m.W32(out + 0, 0);
        m.W32(out + 16, w & ~0xFu);
        if (cursorOut != 0) m.W32(cursorOut + 12, 0);
    }
    return r & 0xFFu;
}

uint32_t RoadClass(GuestRam& m, uint32_t e, int32_t mode, uint32_t cursorOut, int32_t zone,
                   uint32_t sp, RoadRuntimeCallees& calls) {
    const uint32_t F = sp - 32u;                                    // `addiu sp,sp,-32`
    uint32_t was = 0;                                               // s2
    if (m.U32(e + 0x174) != 0) was = m.U32(e + 0x184) & 1u;
    if (mode == 1) {                                                // 0x8003DE6C
        m.W32(e + 0x174, 0);
        m.W32(e + 0x178, 0);
        m.W32(e + 0x17C, 0);
        m.W32(e + 0x180, 0);
    }
    uint32_t r;
    if (m.U16(e + 0x16A) == 1) {                                    // a junction core piece
        const uint32_t c = m.U32(e + 0x178);
        if (c != 0 && m.S16(c + 4) >= 0) m.W32(e + 0x178, 0);
        r = RoadClassNode(m, e, e + 0x174, cursorOut, zone, F, calls);
    } else {                                                        // open road
        const uint32_t c = m.U32(e + 0x178);
        if (c != 0 && m.S16(c + 4) < 0) m.W32(e + 0x178, 0);
        r = U(RoadCrossSection(m, e + 0x148, e + 0x168, e + 0x174));
        if (cursorOut != 0) m.W32(cursorOut + 12, 0);
    }
    if (m.U32(e + 0x174) != 0 && ((m.U32(e + 0x184) & 1u) != 0 || was != 0)) r = 1;
    return r & 0xFFu;
}

int32_t NodeZone(GuestRam& m, uint32_t e, uint32_t sp, RoadRuntimeCallees& calls) {
    const uint32_t F = sp - 40u;                                    // `addiu sp,sp,-40`
    if (e == 0) return -1;
    if (m.U16(e + 0x16A) != 1) return -1;
    const uint32_t node = RoadNodeRecord(m, m.S32(m.U32(e + 0x148) + 0));
    if (node == 0) return -1;
    m.W32(F + 16, 0xFFFFFFFFu);                                     // hint -1 at sp+16 (0x8003DDF8)
    return S(calls.NodeWedge(m, e + 0xB8, node, 0, 0, -1, F));
}

uint32_t RoadsideRun(GuestRam& m, uint32_t e, int32_t mode, int32_t zone, uint32_t sp,
                     RoadRuntimeCallees& calls) {
    const uint32_t F = sp - 32u;                                    // `addiu sp,sp,-32`
    if (mode == 1)                                                  // MemSet32(e + 0x1EC, 0, 12)
        for (uint32_t i = 0; i < 12; i += 4) m.W32(e + 0x1EC + i, 0);
    if (m.U16(e + 0x16A) == 1) {
        const uint32_t v = m.U32(e + 0x1F0);
        m.W32(e + 0x1EC, 0);                                        // the delay slot, unconditional
        if (v == 0xFFFFFFFFu) m.W32(e + 0x1F0, 0);
        return calls.RoadsideRunNode(m, e, zone, F);
    }
    const uint32_t v = m.U32(e + 0x1EC);
    m.W32(e + 0x1F0, 0);
    if (v == 0xFFFFFFFFu) m.W32(e + 0x1EC, 0);
    return RoadsideRunOfSlice(m, e);
}

void RoadPosition(GuestRam& m, uint32_t heading, uint32_t cursor, uint32_t out, uint32_t sp) {
    const uint32_t F = sp - 32u;                                    // `addiu sp,sp,-32`
    {
        const uint32_t piece = m.U32(cursor + 4);
        const uint32_t hi = m.U16(piece + 2);
        const uint32_t lo = m.U16(piece + 12);
        m.W32(out + 0, (hi << 16) | lo);
    }
    const int32_t d = DotLcmView(m, heading, m.U32(cursor + 12) + 14u, F);
    const int32_t old = m.S32(out + 4);
    int32_t v;
    if (d >= 0) {
        const int32_t a1 = (Abs(old) == 5) ? 3 : 1;
        v = a1 + ((old < 1) ? 1 : 0);
    } else {
        const int32_t a1 = (Abs(old) == 5) ? -3 : -1;
        v = a1 - S((~U(old)) >> 31);                                // `nor; srl 31`: old >= 0
    }
    m.W32(out + 4, U(v));
    const int32_t along = Add(m.S32(m.U32(cursor + 12) + 40), m.S32(cursor + 20));
    m.W32(out + 8, U(along));
    if (m.S16(m.U32(cursor + 4) + 2) != 0) return;                  // a core piece: done
    const uint32_t obj = m.U32(cursor + 0);
    if (m.S16(obj + 16) == 1 && m.U32(obj + 12) == 0) {             // a stub of a core object
        int32_t v1 = along;
        if (v1 < 0) v1 = 0;
        m.W32(out + 8, U(v1));
        const uint32_t pc = m.U32(cursor + 4);
        if (m.U32(pc + 24) != 0) {
            const int32_t lim = m.S32(pc + 28);
            if (lim < v1) m.W32(out + 8, U(lim));
        }
    }
    const int16_t idx = m.S16(m.U32(cursor + 12) + 0);              // 0x80036770
    if (idx != 0) {
        const int32_t lastIdx = m.S16(m.U32(cursor + 8) + 10) - 1;
        if (idx != lastIdx) return;
    }
    if (RoadNextObjectMissing(m, cursor, m.S32(out + 4)) == 0) return;
    const uint32_t pc = m.U32(cursor + 4);                          // 0x800367B8: two stores
    int32_t v1 = m.S32(out + 8);
    const int32_t lim = m.S32(pc + 28);
    if (lim < v1) v1 = lim;
    m.W32(out + 8, U(v1));
    if (v1 < 0) v1 = 0;
    m.W32(out + 8, U(v1));
}

uint32_t RoadClassify(GuestRam& m, uint32_t e, uint32_t sp, RoadRuntimeCallees& calls) {
    const uint32_t F = sp - 72u;                                    // `addiu sp,sp,-72`
    const uint32_t x = F + 16u;                                     // the 32-byte cursor, sp+16
    const int32_t zone = NodeZone(m, e, F, calls);
    uint32_t r = RoadClass(m, e, 0, x, zone, F, calls) & 0xFFu;     // s2
    if ((m.U16(e + 0xAC) >> 5) < 2) {                               // bikes and riders
        const uint32_t f = m.U32(e + 0x184);
        bool go = (f & 0x20u) != 0;
        if (!go) go = m.U32(e + 0x174) != 0 && (f & 1u) != 0;
        if (go && m.U16(e + 0x16A) == 1 && m.S16(m.U32(e + 0x14C) + 2) == 1 &&
            m.U32(x + 12) != 0) {
            const uint32_t mine = m.U32(e + 0x150);
            if (mine != m.U32(x + 8)) {
                const uint32_t xp = m.U32(x + 4);
                if (xp != 0 && m.S16(xp + 2) == 1) {                // adopt the node arm's cursor
                    GuestCopyWords(m, e + 0x148, x, 32);
                    RoadPosition(m, e + 0x1C2, e + 0x148, e + 0x168, F);
                }
            }
        }
    }
    // 0x8003E0FC. Dead in effect (`dff4.bit6`), kept as the original has it.
    if ((m.U32(e + 0x184) & 0x41u) != 0) r = 1;
    RoadsideRun(m, e, 0, zone, F, calls);
    const uint32_t bit = (0u - r) & 0x40u;
    m.W32(e + 0x184, m.U32(e + 0x184) | bit);
    return bit;
}

// ============================================================================ progress and route

uint32_t TurnSubObject(GuestRam& m, uint32_t cursor, int32_t len, int32_t road, uint32_t dirOut) {
    if (cursor == 0) return 0;                                      // 0x8003BE1C
    if (road == -1) return 0;
    const uint32_t piece = m.U32(cursor + 4);                       // s2
    int32_t subCount = m.S16(piece + 22);                           // GRPT +0x16, re-read per turn
    const uint32_t o = m.U32(cursor + 0);                           // s3, the object
    for (int32_t i = 0; i < subCount; ++i, subCount = m.S16(piece + 22)) {
        const int32_t firstSubt = m.S16(piece + 20);
        const uint32_t sub = m.U32(o + 0x30) + U(firstSubt + i) * 28u;
        if ((Add(m.S32(sub + 12), 0x8000) >> 16) != (len >> 12)) continue;
        const uint32_t ipt = RoadJunctionIndex(m, m.S32(o + 0));
        if (ipt == 0) continue;
        if (m.S16(ipt + 8) <= 0) continue;
        for (int32_t k = 0; k < m.S16(ipt + 8); ++k) {
            const uint32_t g = m.U32(kRoadGraphPtr);
            const uint32_t t = m.U32(g + 0x34) + U(m.S16(ipt + 6) + k) * 12u;
            if (m.S16(t + 10) != road) continue;
            const uint32_t gp = m.U32(g + 0x38) + U(static_cast<int32_t>(m.S16(t + 2))) * 12u;
            const int32_t off = m.S16(piece + 20) + m.S16(gp + 4);
            if (sub != m.U32(o + 0x30) + U(off) * 28u) continue;
            if (m.S16(t + 6) > 0) m.W32(dirOut, U(static_cast<int32_t>(m.S16(gp + 8))));
            else m.W32(dirOut, U(-static_cast<int32_t>(m.S16(gp + 8))));
            return sub;
        }
    }
    return 0;
}

int32_t ProgressScalar(GuestRam& m, uint32_t p, uint32_t sp) {
    const uint32_t F = sp - 80u;                                    // `addiu sp,sp,-80`
    const int16_t count = m.S16(kRouteRecordCount);
    const uint32_t R = m.U32(p + 0x100);                            // e[+0x1AC]
    if (count == -1) return 0;
    if (R == 0) return m.S32(p + 0xC4);                             // e[+0x170], NOT shifted
    const uint32_t key = m.U32(p + 0xBC);                           // e[+0x168]
    const uint32_t kind = key >> 16;
    int32_t d;
    if (kind == 1) {                                                // on a junction core piece
        const uint32_t rec = RouteLegFor(m, R, S(key & 0xFFFFu));
        if (rec == 0) {
            d = m.S32(p + 0xC4) >> 4;
        } else {
            const int32_t exits = m.S32(rec + 16);
            d = m.S32(rec + 4);
            if (exits > 0) {
                const uint32_t dir = F + 48u;
                const uint32_t sub = TurnSubObject(m, p + 0x9C, m.S32(rec + 8), m.S32(rec + 0x54), dir);
                if (sub != 0) {
                    int32_t a;
                    if (sub == m.U32(p + 0xA4)) {
                        a = m.S32(p + 0xC4);
                    } else {                                        // 0x8003B6E0
                        const uint32_t c = F + 16u;
                        m.W32(c + 0, m.U32(p + 0x9C));
                        const uint32_t piece = m.U32(p + 0xA0);
                        m.W32(c + 8, sub);
                        m.W32(c + 4, piece);
                        const uint32_t obj = m.U32(p + 0x9C);
                        const int32_t first = m.S16(sub + 8);
                        const uint32_t slct = m.U32(obj + 0x34);
                        m.W32(c + 24, 0);
                        m.W32(c + 28, 0);
                        m.W32(c + 20, 0);
                        m.W32(c + 16, 0);
                        m.W32(c + 12, slct + U(first) * 52u);
                        const uint32_t s = RoadSliceSearch(m, p, c, p + 12u, F);
                        const int32_t proj = AiProjectView(m, p + 12u, s + 14u, s + 20u);
                        a = Add(m.S32(s + 40), proj);
                    }
                    if (m.S32(dir) > 0) {
                        const int32_t base = Sub(d, m.S32(rec + 8));
                        d = Add(base, Sub(m.S32(sub + 12), a) >> 4);
                    } else {
                        const int32_t base = Sub(d, m.S32(rec + 8));
                        d = Add(base, a >> 4);
                    }
                }
            }
        }
    } else {
        const uint32_t fin = m.U32(kRouteFinishPtr);                // 0x8003B79C
        const uint32_t r0 = m.U32(R + 0);
        const int32_t fKey = m.S32(fin + 12);
        bool finishRoad = false;
        if ((U(fKey) == r0 || fKey == -1) && kind == 0) finishRoad = (key & 0xFFFFu) == m.U32(fin + 0);
        if (finishRoad) {
            const int32_t fdir = m.S32(fin + 8);
            bool before;
            if (fdir > 0) before = m.S32(p + 0xC4) < m.S32(fin + 4);
            else if (fdir == 0) before = true;
            else before = m.S32(fin + 4) < m.S32(p + 0xC4);
            if (before) {
                const uint32_t f2 = m.U32(kRouteFinishPtr);
                const int32_t along = m.S32(p + 0xC4);
                d = Abs(Sub(m.S32(f2 + 4), along)) >> 4;
            } else {
                d = 0;
            }
        } else {
            const uint32_t leg = RouteFindLegView(m, R, m.U16(p + 0xBC));   // 0x8003B84C
            if (leg == 0) {
                d = m.S32(p + 0xC4) >> 4;
            } else {
                int32_t v, next;
                if (m.S32(leg + 4) > 0) {
                    const int32_t end = m.S32(leg + 8);
                    v = Sub(end, m.S32(p + 0xC4));
                    next = m.S16(leg + 14);
                } else {
                    v = m.S32(p + 0xC4);
                    next = m.S16(leg + 12);
                }
                d = v >> 4;
                if (next != -1) {
                    const uint32_t r2 = RouteLegFor(m, R, next);
                    if (r2 != 0) d = Add(d, m.S32(r2 + 4));
                }
            }
        }
    }
    return (d < 0) ? 0 : d;
}

uint32_t RouteBind(GuestRam& m, uint32_t p, int32_t step, uint32_t x, uint32_t sp,
                   RoadRuntimeCallees& calls) {
    const uint32_t F = sp - 24u;                                    // `addiu sp,sp,-24`
    if (p == 0) return 0;
    const uint32_t h = m.U16(p + 0);
    const uint32_t pool = h >> 5;
    uint32_t e = 0;
    if (pool == 0) e = m.U32(kRoadPool0Ptr) + 1096u * h;
    if (m.U32(p + 0x100) == 0 || step == 0) return calls.RouteBindFirst(m, p, e, pool, x, F);
    return calls.RouteBindStep(m, p, pool, e, x, F);                // note the argument order
}

uint32_t RoadsideZones(GuestRam& m, uint32_t e, uint32_t sp) {
    const uint32_t F = sp - 96u;                                    // `addiu sp,sp,-96`
    const uint32_t kA2 = 0xFFFFFF9Fu;                               // ~0x60, the zone state
    const uint32_t kS8 = 0xFFFFFFE1u;                               // ~0x1E, the counter
    for (uint32_t half = 0; half < 2; ++half) {                     // the period table, 3 + 3
        uint32_t w[3];
        for (uint32_t k = 0; k < 3; ++k) w[k] = m.U32(kZonePeriodTable + 12u * half + 4u * k);
        for (uint32_t k = 0; k < 3; ++k) m.W32(F + 16u + 12u * half + 4u * k, w[k]);
    }
    uint32_t w24 = m.U32(e + 0x24);
    const uint32_t node = m.U32(e + 0x1F0);
    const uint32_t was = (w24 >> 5) & 3u;                           // s7
    w24 &= ~0x360u;
    m.W32(e + 0x24, w24);
    uint32_t list = 0, run = 0;                                     // s6, a1
    if (node != 0) {
        list = node;
        m.W32(F + 40, 1);
    } else {
        const uint32_t r = m.U32(e + 0x1EC);
        if (r == 0) return w24;                                     // NO propagation on this exit
        run = r;
        if (m.S32(e + 0x158) >= 0) run += 2;
        m.W32(F + 40, m.U8(run + 1));
    }
    if (m.S32(F + 40) > 0) {
        if (list == 0 && run != 0) {
            const uint32_t obj = m.U32(e + 0x148);
            const uint32_t first = m.U8(run + 0);
            list = m.U32(obj + 0x64) + 4u * first;
        }
        if (m.S32(F + 40) != 0) {
            uint32_t a2 = kA2;
            for (int32_t i = 0;;) {
                const int32_t firstSeg = m.S16(list + 0);
                const uint32_t obj = m.U32(e + 0x148);
                const uint32_t cnt = m.U8(list + 3);
                uint32_t r = m.U32(obj + 0x60) + U(firstSeg * 5) * 8u;   // 40-byte BSDT records
                if (cnt != 0) {
                    uint32_t s2 = r + 2u;
                    for (uint32_t j = 0;;) {
                        m.W32(F + 48, a2);
                        const int32_t st = SegmentStraddle(m, e, r, F);
                        a2 = m.U32(F + 48);
                        if (st != 0) {
                            const uint32_t kindM1 = (m.U16(s2) & 0xFu) - 1u;
                            if (kindM1 < 8u) {
                                const uint32_t target = m.U32(kZoneJumpTable + 4u * kindM1);
                                if (target == 0x8003AB80u) {                    // kinds 1, 2
                                    const uint32_t w = m.U32(e + 0x24);
                                    const uint32_t b = m.U16(s2) & 0x200u;
                                    m.W32(e + 0x24, (w & ~0x200u) | b);
                                } else if (target == 0x8003ABA0u) {             // kind 5
                                    m.W32(e + 0x24, (m.U32(e + 0x24) & a2) | 0x320u);
                                } else if (target == 0x8003AD2Cu) {             // kind 3
                                    m.W32(e + 0x24, m.U32(e + 0x24) | 0x300u);
                                } else if (target == 0x8003ABB4u) {             // kinds 4, 6, 7, 8
                                    const uint32_t h = m.U16(e + 0xAC);
                                    const bool runs = ((h >> 5) != 4u) || ((h & 0x1Fu) < 30u);
                                    if (runs) {
                                        const int32_t spd = m.S32(e + 0x1E0);
                                        if ((spd >> 16) != 0) {
                                            const int32_t k = spd >> 20;
                                            const uint32_t lo = (~U(spd >> 31)) & U(k);
                                            const int32_t hk = Sub(5, k);
                                            const uint32_t idx = lo + (U(hk >> 31) & U(hk));
                                            const uint32_t period = m.U32(F + 16u + 4u * idx);
                                            const uint32_t a1 = m.U32(e + 0x24);
                                            const uint32_t c = (a1 >> 1) & 0xFu;
                                            if (c < period) {
                                                m.W32(e + 0x24, (a1 & a2) | ((was != 0) ? 0x40u : 0u));
                                            } else if (was == 0) {
                                                m.W32(e + 0x24, ((a1 & kS8) & a2) | 0x40u);
                                            } else {
                                                m.W32(e + 0x24, a1 & kS8);
                                                m.W32(F + 48, a2);
                                                const uint32_t r1 = GuestRand(m);
                                                a2 = m.U32(F + 48);
                                                if ((r1 & period) != 0) {
                                                    m.W32(e + 0x24, m.U32(e + 0x24) & a2);
                                                    m.W32(F + 48, a2);
                                                    const uint32_t r2 = GuestRand(m);
                                                    const uint32_t rem = URem(r2, period);
                                                    const uint32_t w = m.U32(e + 0x24);
                                                    m.W32(e + 0x24, (w & kS8) | ((rem & 0xFu) << 1));
                                                    a2 = m.U32(F + 48);
                                                }
                                            }
                                            const uint32_t w = m.U32(e + 0x24);      // 0x8003ACC0
                                            m.W32(e + 0x24, (w & kS8) | (((((w >> 1) & 0xFu) + 1u) & 0xFu) << 1));
                                        }
                                    }
                                    const uint32_t a1 = m.U32(e + 0x24) | 0x200u;   // 0x8003ACE8
                                    m.W32(e + 0x24, a1);
                                    const uint32_t kind = m.U16(s2) & 0xFu;
                                    const uint32_t bit8 = (kind == 6u) ? 0u : ((kind ^ 8u) != 0u ? 1u : 0u);
                                    m.W32(e + 0x24, (a1 & ~0x100u) | (bit8 << 8));
                                } else {
                                    // A jump target this transcription does not know: the table
                                    // at 0x80010E14 was changed. Not emulated - fault the call.
                                    (void)m.U32(0xFFFFFFFFu);
                                }
                            }
                        }
                        s2 += 40u;
                        const bool stateSet = ((m.U32(e + 0x24) >> 5) & 3u) != 0;
                        r += 40u;
                        if (stateSet) break;
                        ++j;
                        if (!(j < m.U8(list + 3))) break;
                    }
                }
                const bool stateSet = ((m.U32(e + 0x24) >> 5) & 3u) != 0;   // 0x8003AD6C
                list += 4u;
                if (stateSet) break;
                ++i;
                if (!(i < m.S32(F + 40))) break;
            }
        }
    }
    for (uint32_t k = 0; k < 2; ++k) {                              // 0x8003AD9C: propagate
        uint32_t o = m.U32(e + 8u * k + 0x38u);
        while (o != 0) {
            const uint32_t ow = m.U32(o + 0x24);
            const uint32_t ew = m.U32(e + 0x24);
            m.W32(o + 0x24, (ow & kA2) | (ew & 0x60u));
            o = m.U32(o + 8u * k + 0x38u);
        }
    }
    return 0;
}

// ============================================================================ tracking

uint32_t RoadTrack(GuestRam& m, uint32_t e, uint32_t sp) {
    const uint32_t F = sp - 40u;                                    // `addiu sp,sp,-40`
    const uint32_t sub = m.U32(e + 0x150);
    const uint32_t old = m.U32(e + 0x154);                          // s3
    const uint16_t tag = m.U16(sub + 6);                            // s0
    const uint32_t s = RoadSliceSearch(m, e + 0xAC, e + 0x148, e + 0xB8, F);
    TagRewrite(m, e, tag);                                          // 0x80037058
    RoadProjectView(m, e + 0xB8, s, e + 0x158, e + 0x15C, F);
    RoadPosition(m, e + 0x1C2, e + 0x148, e + 0x168, F);
    uint32_t w = m.U32(e + 0x184);
    w = (s == old) ? (w & ~0x40u) : (w | 0x40u);
    m.W32(e + 0x184, w);
    return w;
}

uint32_t RoadReseat(GuestRam& m, uint32_t e, uint32_t sp, RoadRuntimeCallees& calls) {
    const uint32_t F = sp - 88u;                                    // `addiu sp,sp,-88`
    const uint32_t cur = e + 0x148;                                 // s4
    const uint16_t tag = m.U16(m.U32(e + 0x150) + 6);               // s6
    if (m.U16(e + 0x16A) == 0) {                                    // on open road
        calls.RoadShortcut(m, e, F);                                // 0x800396E4
    } else {
        const uint32_t piece = m.U32(cur + 4);
        const int16_t core = m.S16(piece + 2);
        const uint32_t obj = m.U32(e + 0x148);                      // s5
        if (core == 1 && m.S16(piece + 0) == 0) {
            const uint32_t node = RoadNodeRecord(m, m.S32(obj + 0));
            if (node == 0) return 0;
            uint32_t best = 0;                                      // s0
            uint32_t metric = 0xFFFF0000u;                          // a3: `lui a3,0xffff`, a sentinel
            const int16_t n0 = m.S16(node + 2);
            if (n0 <= 0) return U(static_cast<int32_t>(n0));        // v0 = the count, <= 0
            const int32_t kFar = 0x5A8000;
            for (int32_t t = 0; t < m.S16(node + 2); ++t) {
                const uint32_t arm = node + 8u + 24u * U(t);
                const int32_t dx = Sub(m.S32(e + 0xB8), m.S32(arm + 12));
                m.W32(F + 16, U(dx));
                const int32_t dy = Sub(m.S32(e + 0xBC), m.S32(arm + 16));
                m.W32(F + 20, U(dy));
                const int32_t dz = Sub(m.S32(e + 0xC0), m.S32(arm + 20));
                m.W32(F + 24, U(dz));
                int32_t a1;
                if (kFar < Abs(dx) || kFar < Abs(dz)) {
                    a1 = 0x7FFF0000;
                } else {
                    const int32_t ax = Abs(Sub(m.S32(e + 0xB8), m.S32(arm + 12)));
                    const int32_t az = Abs(Sub(m.S32(e + 0xC0), m.S32(arm + 20)));
                    const int32_t mn = (ax < az) ? ax : az;
                    const int32_t sum = Add(ax, az);
                    const int32_t half = S(U(mn) + (U(mn) >> 31)) >> 1;
                    a1 = Sub(sum, half);
                }
                if (metric == 0xFFFF0000u || a1 < S(metric)) {
                    best = arm;
                    const uint32_t w0 = m.U32(F + 16), w1 = m.U32(F + 20), w2 = m.U32(F + 24);
                    metric = U(a1);
                    m.W32(F + 32, w0);
                    m.W32(F + 36, w1);
                    m.W32(F + 40, w2);
                }
            }
            if (best == 0) return 0;
            // 0x80039870: the signed distance outside the arm's plane, three truncated products
            const int32_t p0 = FixMul(m.S32(F + 32), Promote(m.S16(best + 6)));
            const int32_t p1 = FixMul(m.S32(F + 36), Promote(m.S16(best + 8)));
            const int32_t z = m.S32(F + 40);
            const int32_t v2 = Promote(m.S16(best + 10));
            const uint64_t prod = static_cast<uint64_t>(static_cast<int64_t>(z) * v2);
            m.W32(F + 48, static_cast<uint32_t>(prod));             // dead spills, lo / hi
            m.W32(F + 52, static_cast<uint32_t>(prod >> 32));
            const int32_t plane = Add(FixMul(z, v2), Add(p1, p0));
            if (plane < -131) {
                const uint32_t pc = RoadFindPiece(m, obj, m.S16(best + 4));
                if (pc == 0) return 0;
                const int32_t firstSubt = m.S16(pc + 20);
                const uint32_t slct = m.U32(obj + 0x34);
                const uint32_t sub = m.U32(obj + 0x30) + U(firstSubt) * 28u;   // s2
                const int16_t dir = m.S16(best + 2);
                const int32_t first = m.S16(sub + 8);
                int32_t idx = first;
                if (dir <= 0) idx = Add(first - 1, m.S16(sub + 10));
                const uint32_t s = slct + U(idx) * 52u;                          // s0
                RoadProjectView(m, e + 0xB8, s, cur + 16, cur + 20, F);
                m.W32(cur + 4, pc);
                m.W32(cur + 8, sub);
                m.W32(cur + 12, s);
                m.W32(cur + 24, 0);
                m.W32(cur + 28, 0);
            }
        }
    }
    TagRewrite(m, e, tag);                                          // 0x80039998
    return m.U32(e + 0x16C);
}

uint32_t RoadProbeAhead(GuestRam& m, uint32_t e, uint32_t sp, RoadRuntimeCallees& calls) {
    const uint32_t F = sp - 64u;                                    // `addiu sp,sp,-64`
    const uint32_t c = F + 16u;                                     // the private cursor
    m.W32(c + 0, m.U32(e + 0x148));
    m.W32(c + 4, m.U32(e + 0x14C));
    m.W32(c + 8, m.U32(e + 0x150));
    const uint32_t slice = m.U32(e + 0x154);
    m.W32(c + 24, 0);
    m.W32(c + 28, 0);
    m.W32(c + 16, 0);
    m.W32(c + 20, 0);
    m.W32(c + 12, slice);
    const bool crash = (m.U32(e + 0x238) & 0x600u) != 0;           // s0
    int32_t len;
    uint32_t base;
    int16_t h0;
    if (crash) {                                                    // from the box centre
        h0 = m.S16(e + 0x210);
        len = m.S32(e + 0x134);
        base = e + 0xB8;
    } else {                                                        // from the contact point, 2x
        len = S(U(m.S32(e + 0x134)) << 1);
        h0 = m.S16(e + 0x210);
        base = e + 0x1F8;
    }
    m.W32(e + 0xF4, U(Add(FixMul(Promote(h0), len), m.S32(base + 0))));
    for (uint32_t k = 1; k < 3; ++k) {
        const int32_t h = Promote(m.S16(e + 0x210 + 2u * k));
        m.W32(e + 0xF4 + 4u * k, U(Add(FixMul(h, len), m.S32(base + 4u * k))));
    }
    RoadSliceSearch(m, e + 0xAC, c, e + 0xF4, F);                  // 0x80037270
    const uint32_t cs = m.U32(c + 12);
    const uint32_t q = m.U32(e + 0x358);                            // the passenger
    m.W32(e + 0x100, cs);
    if (q == 0) return cs;
    if (m.U32(e + 0x440) == 0) return 0;
    uint32_t from;
    if (!crash) {
        MulAddView(m, e + 0x1F8, e + 0x210, m.S32(e + 0x134), q + 0x1F8);
        from = q + 0x1F8;
    } else {
        from = e + 0xB8;
    }
    const int32_t t = Add(m.S32(e + 0x130), m.S32(q + 0x130));
    MulAddView(m, from, e + 0x204, t, q + 0xB8);
    if ((m.U32(e + 0x230) & 0x08000000u) != 0) {                    // the BIKE's bit 27 decides
        m.W32(q + 0x184, m.U32(q + 0x184) & ~0x20u);
    } else {
        RoadReseat(m, q, F, calls);
        m.W32(q + 0x184, m.U32(q + 0x184) | 0x20u);
    }
    return RoadTrack(m, q, F);
}

// ============================================================================ pass H and the re-bind

uint32_t RoadTrackPass(GuestRam& m, uint32_t sp, RoadRuntimeCallees& calls) {
    const uint32_t F = sp - 32u;                                    // `addiu sp,sp,-32`
    uint32_t p = kRoadListFirst;
    uint32_t end = m.U32(kRoadListHead);
    uint32_t e = m.U32(p);
    while (p < end) {
        if (m.U32(e + 0x33C) != 0) {                                // tag latched: disarm
            const uint32_t a = m.U32(e + 0x184);
            const uint32_t b = m.U32(e + 0x234);
            m.W32(e + 0x184, a & ~0x80u);
            m.W32(e + 0x234, b & 0xFBFFFFFFu);
        } else {                                                    // arm the latch
            m.W32(e + 0x184, m.U32(e + 0x184) | 0x80u);
            m.W32(e + 0x234, m.U32(e + 0x234) | 0x04000000u);
        }
        if ((m.U32(e + 0x230) & 0x08000000u) != 0) {
            m.W32(e + 0x184, m.U32(e + 0x184) & ~0x20u);
        } else {
            RoadReseat(m, e, F, calls);
            m.W32(e + 0x184, m.U32(e + 0x184) | 0x20u);
        }
        RoadTrack(m, e, F);
        if ((m.U32(e + 0x234) & 0x40000u) != 0) m.W32(e + 0x16C, m.U32(m.U32(e + 0x340) + 0xC0));
        RoadProbeAhead(m, e, F, calls);
        p += 4;
        end = m.U32(kRoadListHead);
        e = m.U32(p);
    }
    return 0;
}

uint32_t RoadClassPass(GuestRam& m, uint32_t sp, RoadRuntimeCallees& calls) {
    const uint32_t F = sp - 32u;                                    // `addiu sp,sp,-32`
    uint32_t p = kRoadListFirst;
    uint32_t end = m.U32(kRoadListHead);
    uint32_t e = m.U32(p);
    while (p < end) {
        RoadClassify(m, e, F, calls);
        const uint32_t q = m.U32(e + 0x358);
        if (q != 0) RoadClassify(m, q, F, calls);
        if ((m.U32(e + 0x238) & 0x800u) != 0) m.W32(e + 0x184, m.U32(e + 0x184) | 0x40u);
        p += 4;
        end = m.U32(kRoadListHead);
        e = m.U32(p);
    }
    return 0;
}

uint32_t RouteCheckPass(GuestRam& m, uint32_t sp, RoadRuntimeCallees& calls) {
    const uint32_t F = sp - 32u;                                    // `addiu sp,sp,-32`
    uint32_t p = kRoadListFirst;
    uint32_t end = m.U32(kRoadListHead);
    uint32_t e = m.U32(p);
    while (p < end) {
        RouteBind(m, e + 0xAC, 1, 0, F, calls);
        const uint32_t q = m.U32(e + 0x358);
        if (q != 0) m.W32(q + 0x1AC, m.U32(e + 0x1AC));
        {
            const uint32_t rd = m.U32(e + 0x43C);
            m.W8(rd, static_cast<uint8_t>(m.U8(rd) & 0x7Fu));
        }
        if (m.S16(kRouteRecordCount) != -1 && m.U32(m.U32(e + 0x354) + 0x25C) < 3u) {
            const uint32_t gs = m.U32(kRoadGameState);
            const uint32_t h = m.U16(e + 0xAC);
            const uint32_t players = m.U32(gs + 0x30);
            bool test = h < players;                                // a player
            if (!test) test = (m.U8(m.U32(e + 0x43C) + 1) & 0xFu) != 2u;   // or not police
            if (test) {
                const uint32_t key = m.U32(e + 0x168);
                if ((key >> 16) == 0) {
                    const uint32_t leg = RouteFindLegView(m, m.U32(e + 0x1AC), key & 0xFFFFu);
                    if (leg != 0 && (m.S32(leg + 4) ^ m.S32(e + 0x16C)) < 0) {
                        const uint32_t rd = m.U32(e + 0x43C);       // WRONG WAY
                        m.W8(rd, static_cast<uint8_t>(m.U8(rd) | 0x80u));
                    }
                }
            }
        }
        RoadsideZones(m, e, F);
        p += 4;
        end = m.U32(kRoadListHead);
        e = m.U32(p);
    }
    return 0;
}

uint32_t ProgressPass(GuestRam& m, uint32_t sp) {
    const uint32_t F = sp - 40u;                                    // `addiu sp,sp,-40`
    uint32_t p = kRoadListFirst;
    uint32_t end = m.U32(kRoadListHead);
    uint32_t e = m.U32(p);
    const int32_t kStep = S(0xFFE6DAA0u);                           // -0x192560, 0x8003B554
    while (p < end) {
        const int32_t old = m.S32(e + 0x144);
        int32_t val = ProgressScalar(m, e + 0xAC, F);
        int32_t t = 0x649581;
        m.W32(e + 0x144, U(val));
        while (val < t) {
            const uint32_t rd = m.U32(e + 0x43C);
            if ((m.U8(rd + 0x44) & 1u) != 0) break;                 // latched
            if (t < old) m.W8(rd + 0x44, static_cast<uint8_t>(m.U8(rd + 0x44) | 1u));
            else t = Add(t, kStep);
            val = m.S32(e + 0x144);
        }
        const uint32_t q = m.U32(e + 0x358);
        if (q != 0) m.W32(q + 0x144, m.U32(e + 0x144));
        p += 4;
        end = m.U32(kRoadListHead);
        e = m.U32(p);
    }
    return 0;
}

uint32_t RoadRebindBody(GuestRam& m, uint32_t e, uint32_t sp, RoadRuntimeCallees& calls) {
    const uint32_t F = sp - 32u;                                    // `addiu sp,sp,-32`
    RoadTrack(m, e, F);
    if ((m.U16(e + 0xAC) >> 5) == 0) RoadProbeAhead(m, e, F, calls);   // pool 0 only
    RoadClassify(m, e, F, calls);
    RouteBind(m, e + 0xAC, 1, 0, F, calls);
    const int32_t v = ProgressScalar(m, e + 0xAC, F);
    m.W32(e + 0x144, U(v));                                         // the delay slot of the next jal
    RoadsideZones(m, e, F);
    return (m.U32(e + 0x184) >> 6) & 1u;
}

uint32_t RoadRebind(GuestRam& m, uint32_t e, int32_t flag, uint32_t sp, RoadRuntimeCallees& calls) {
    const uint32_t F = sp - 24u;                                    // `addiu sp,sp,-24`
    if (flag != 0) {
        RoadReseat(m, e, F, calls);
        m.W32(e + 0x184, m.U32(e + 0x184) | 0x20u);
    } else {
        m.W32(e + 0x184, m.U32(e + 0x184) & ~0x20u);
    }
    return RoadRebindBody(m, e, F, calls);
}

// ============================================================================ under the re-bind
// Transcribed from our own disassembly of SLUS_010.53 (SHA-1
// 67ed165a2c517d4e6106fb0dfa324d66dd9a76f1); comments give the original's addresses.

uint32_t GraphRoad(GuestRam& m, uint32_t id) {
    const uint32_t g = m.U32(m.gp() + kRaceGraphGp);               // 0x800245DC
    return m.U32(g + 24) + (id << 4);
}

uint32_t GraphNode(GuestRam& m, uint32_t id) {
    const uint32_t g = m.U32(m.gp() + kRaceGraphGp);               // 0x800245F4
    return m.U32(g + 20) + id * 40u;
}

int32_t RoadEndNode(GuestRam& m, uint32_t pos, uint32_t nodeOut, int32_t mode) {
    int32_t d = 0;                                                  // s1
    uint32_t n = 0xFFFFFFFFu;                                       // s0
    const uint32_t key = m.U32(pos + 0);                            // 0x8003A624
    if ((key >> 16) == 1) {
        n = key & 0xFFFFu;                                          // a junction core: d = 0
    } else {
        const uint32_t r = GraphRoad(m, key & 0xFFFFu);
        if (r == 0) {                                               // 0x8003A64C
            m.W32(nodeOut, n);
            return 0;
        }
        const uint32_t len = (m.U32(r + 4) >> 6) << 16;             // whole world units, 16.16
        n = m.U32(r + 12);                                          // the end node
        const int32_t along = m.S32(pos + 8);
        d = Sub(S(len), along);
        bool start = (mode == 0) && along < d;                      // mode 0: the NEARER end
        if (!start) {
            const int32_t dir = m.S32(pos + 4);                     // 0x8003A67C
            start = dir < 0 && mode == 1;
        }
        if (start) {
            d = m.S32(pos + 8);
            n = m.U32(r + 8);
        }
    }
    if (n != 0xFFFFFFFFu) {                                         // 0x8003A6A0
        const uint32_t g = GraphNode(m, n);
        if (g != 0) n = (m.U32(g + 4) < 3u) ? 0xFFFFFFFFu : m.U32(g + 0);   // `sltiu`: degree >= 3
    }
    m.W32(nodeOut, n);                                              // 0x8003A6D8
    return d;
}

uint32_t RouteRecordOfRoad(GuestRam& m, uint32_t road) {
    if (m.S16(kRouteRecordCount) <= 0) return 0;                    // 0x8003F508
    uint32_t off = 0;
    for (int32_t i = 0;;) {
        const uint32_t rec = m.U32(kRouteTable + 36) + off;         // re-read every pass
        const uint32_t leg = RouteFindLegView(m, rec, road);
        ++i;
        if (leg != 0) return rec;
        if (!(i < m.S16(kRouteRecordCount))) return 0;
        off += kRouteLegBytes;
    }
}

namespace {
// The slot bit of route record `rec` (+0x76), set; returns the `or` result, the original's v0 on the
// exits that end with it.
uint32_t SlotBitSet(GuestRam& m, uint32_t rec, uint32_t slot) {
    const uint32_t v = U(m.U16(rec + 118)) | (1u << (slot & 31u));
    m.W16(rec + 118, static_cast<uint16_t>(v));
    return v;
}
} // namespace

uint32_t RouteBindFirst(GuestRam& m, uint32_t p, uint32_t e, uint32_t pool, uint32_t x,
                        uint32_t callerV0) {
    if (p == 0) return callerV0;                                    // 0x8003B044: v0 never set
    if (e != 0) {                                                   // pool 0
        const uint32_t gs = m.U32(kRoadGameState);                  // 0x8003B054
        const uint32_t h = m.U16(e + 0xAC);
        const uint32_t players = m.U32(gs + 48);
        bool police = false;
        if (!(h < players)) police = (m.U8(m.U32(e + 0x43C) + 1) & 0xFu) == 2u;
        if (!police) {                                              // 0x8003B090: the START record
            const int32_t count = m.S16(kRouteRecordCount);
            const uint32_t base = m.U32(kRouteTable + 36);
            m.W32(p + 256, base + U(count) * kRouteLegBytes);
        } else if (x != 0) {                                        // 0x8003B0BC: x's record
            m.W32(p + 256, m.U32(x + 0x1AC));
        } else {
            const uint32_t r = RouteRecordOfRoad(m, m.U16(p + 188));
            if (r != 0) m.W32(p + 256, r);
        }
        const uint32_t rec = m.U32(e + 0x1AC);                      // 0x8003B0E4: ALWAYS
        return SlotBitSet(m, rec, m.U16(e + 0xAC));
    }
    if (pool == 1) {                                                // 0x8003B108: a rider
        const uint32_t r = m.U32(kRoadPool1Ptr) + 628u * (m.U16(p + 0) & 0x1Fu);
        if (r == 0) {
            m.W32(p + 256, 0);
            return 0;
        }
        const uint32_t v = m.U32(m.U32(r + 596) + 0x1AC);           // its bike's record, no bit
        m.W32(p + 256, v);
        return v;
    }
    uint32_t leg = 0;                                               // 0x8003B160: other pools
    if (x != 0) leg = RouteFindLegView(m, m.U32(x + 0x1AC), m.U16(p + 188));
    if (leg == 0) {
        const uint32_t r = RouteRecordOfRoad(m, m.U16(p + 188));
        if (r != 0) {
            m.W32(p + 256, r);
            return r;
        }
        if (x == 0) return 0;
    }
    const uint32_t v = m.U32(x + 0x1AC);                            // 0x8003B1A0
    m.W32(p + 256, v);
    return v;
}

uint32_t RouteBindStep(GuestRam& m, uint32_t p, uint32_t pool, uint32_t e, uint32_t x, uint32_t sp) {
    (void)x;                                                        // a3 is never read
    const uint32_t F = sp - 48u;                                    // `addiu sp,sp,-48`
    const uint32_t key = m.U32(p + 188);                            // 0x8003B1E4
    if ((key >> 16) == 1) {                                         // on a junction core
        if (!(pool == 1 || (pool == 0 && e != 0))) {                // 0x8003B318: other pools
            const uint32_t N = RouteLegOfRoad(m, S(U(m.U16(p + 188))));
            if (N != 0) m.W32(p + 256, N);
            return N;
        }
        const uint32_t R = m.U32(p + 256);                          // 0x8003B210
        if (R == 0) return 1;
        if (m.S32(R + 16) <= 0) return 1;                           // no exits (the finish)
        const uint32_t k = key & 0xFFFFu;
        const uint32_t r0 = m.U32(R + 0);
        if (r0 == k) {                                              // its own node
            if (e == 0) return r0;
            const uint32_t h = m.U16(e + 0xAC);
            SlotBitSet(m, R, h);
            return 1u << (h & 31u);                                 // v0 = the `sllv`
        }
        int32_t i = 0;
        for (;;) {                                                  // 0x8003B270
            const uint32_t a0 = m.U32(R + 100u + 4u * U(i));
            if (a0 != 0xFFFFFFFFu && a0 == k) {
                const uint32_t N = RouteLegOfRoad(m, S(a0));
                if (N != 0) m.W32(p + 256, N);
                if (e == 0) return N;
                if (N != 0) {                                       // 0x8003B3B4
                    const uint32_t rec = m.U32(p + 256);
                    return SlotBitSet(m, rec, m.U16(e + 0xAC));
                }
                const uint32_t h = m.U16(e + 0xAC);                 // 0x8003B2B8
                const uint32_t rec = m.U32(p + 256);
                const uint32_t nb = ~(1u << (h & 31u));
                m.W16(rec + 118, static_cast<uint16_t>(m.U16(rec + 118) & nb));
                return nb;                                          // v0 = the `nor`
            }
            ++i;
            if (!(i < m.S32(R + 16))) break;                        // re-read each pass
        }
        // 0x8003B2EC: not a successor. v0 is the loop branch's delay slot, `sll v0,v1,2`.
        if (e == 0) return U(i) << 2;
        const uint32_t h = m.U16(e + 0xAC);
        const uint32_t rec = m.U32(p + 256);
        const uint32_t v = m.U16(rec + 118) & ~(1u << (h & 31u));
        m.W16(rec + 118, static_cast<uint16_t>(v));
        return v;
    }
    if (RouteFindLegView(m, m.U32(p + 256), key & 0xFFFFu) != 0)   // 0x8003B32C: still on a leg
        return (pool < 2u) ? 1u : 0u;                               // the `sltiu` in the delay slot
    uint32_t v0 = (pool < 2u) ? 1u : 0u;
    bool follow;                                                    // players, police, pool 1
    if (!(pool < 2u)) {
        follow = false;
    } else if (e == 0 || pool != 0) {
        follow = true;
    } else {
        v0 = m.U8(m.U32(e + 0x43C) + 1) & 0xFu;                     // 0x8003B358
        if (v0 == 2u) {
            follow = true;
        } else {
            const uint32_t gs = m.U32(kRoadGameState);
            const uint32_t h = m.U16(e + 0xAC);
            v0 = m.U32(gs + 48);
            follow = h < v0;
        }
    }
    if (!follow) {                                                  // 0x8003B394: AI, pools >= 2
        const uint32_t N = RouteRecordOfRoad(m, m.U16(p + 188));
        if (N != 0) m.W32(p + 256, N);
        return N;
    }
    const uint32_t R = m.U32(p + 256);                              // 0x8003B3D0
    if (R == 0 || e == 0) return v0;
    {
        const int32_t c = m.S32(R + 16);
        if (c <= 0) return U(c);
    }
    const int32_t d = RoadEndNode(m, e + 0x168, F + 16, 0);
    if (0x320000 < d) return U(d);                                  // 50.0 units, `slt`
    {
        const int32_t c = m.S32(R + 16);
        if (c <= 0) return U(c);
    }
    for (int32_t i = 0;;) {                                         // 0x8003B42C
        const uint32_t a0 = m.U32(R + 100u + 4u * U(i));
        if (a0 != 0xFFFFFFFFu && a0 == m.U32(F + 16)) {
            const uint32_t N = RouteLegOfRoad(m, S(a0));
            if (N == 0) return 0;
            const uint32_t has = U(RouteLegHasRoad(m, N, S(U(m.U16(e + 0x168)))));
            if (has != 0) m.W32(p + 256, N);
            return has;                                             // first match only
        }
        ++i;
        if (!(i < m.S32(R + 16))) return U(i) << 2;                 // the delay slot's `sll v0,v1,2`
    }
}

uint32_t TurnByRoads(GuestRam& m, uint32_t cursor, int32_t from, int32_t to, uint32_t dirOut,
                     uint32_t turnOut) {
    if (cursor == 0) return 0;                                      // 0x8003BD34
    uint32_t j = m.U32(cursor + 24);                                // the junction (IPT_)
    if (j == 0) return 0;
    if (!(0 < m.S16(j + 8))) return 0;
    for (int32_t i = 0;;) {
        const int32_t first = m.S16(j + 6);                         // 0x8003BD60
        const uint32_t G = m.U32(kRoadGraphPtr);                    // re-read
        const uint32_t t = m.U32(G + 52) + U(Add(first, i)) * 12u;
        if (m.S16(t + 8) == from && m.S16(t + 10) == to) {
            const int32_t gi = m.S16(t + 2);                        // 0x8003BDA4
            const uint32_t g = m.U32(G + 56) + U(gi) * 12u;
            const uint32_t piece = m.U32(cursor + 4);
            const int32_t off = m.S16(g + 4);
            const int32_t fs = m.S16(piece + 20);
            const uint32_t obj = m.U32(cursor + 0);
            const int32_t d = m.S16(g + 8);                         // NOT negated by PDT_ +6
            const uint32_t subt = m.U32(obj + 48);
            m.W32(dirOut, U(d));
            m.W32(turnOut, t);
            return subt + U(Add(fs, off)) * 28u;
        }
        j = m.U32(cursor + 24);                                     // 0x8003BDF8, re-read
        ++i;
        if (!(i < m.S16(j + 8))) return 0;
    }
}

uint32_t Lerp16(GuestRam& m, uint32_t t, uint32_t a, uint32_t b, uint32_t sp) {
    const uint32_t w = 0x10000u - t;                                // 0x8003E61C
    const int64_t p0 = static_cast<int64_t>(S(w)) * static_cast<int64_t>(S(a));
    const int64_t p1 = static_cast<int64_t>(S(t)) * static_cast<int64_t>(S(b));
    m.W32(sp - 8u, static_cast<uint32_t>(static_cast<uint64_t>(p0)));          // dead spill, lo
    m.W32(sp - 4u, static_cast<uint32_t>(static_cast<uint64_t>(p0) >> 32));    // dead spill, hi
    return static_cast<uint32_t>(static_cast<uint64_t>(p0 >> 16)) +
           static_cast<uint32_t>(static_cast<uint64_t>(p1 >> 16));
}

int32_t CoreEdgeClass(GuestRam& m, uint32_t gA, uint32_t gB, uint32_t f, int32_t lat, int32_t sideA,
                      int32_t sideB, int32_t dir, uint32_t out, uint32_t sp) {
    const uint32_t F = sp - 48u;                                    // `addiu sp,sp,-48`
    int32_t l = lat;
    if ((l ^ dir) < 0) l = 0;                                       // 0x8003E498: signs differ
    l = Abs(l);
    const uint32_t hA = gA + ((sideA == 2) ? 136u : 8u);
    const uint32_t hB = gB + ((sideB == 2) ? 136u : 8u);
    const uint32_t outer = Lerp16(m, f, U(Abs(m.S32(hA + 8))), U(Abs(m.S32(hB + 8))), F);
    uint32_t a1 = U(Abs(m.S32(hA + 8)));                            // 0x8003E4FC, re-read
    uint32_t a2 = U(Abs(m.S32(hB + 8)));
    uint32_t inner = outer;
    const int16_t va = m.S16(hA + 2);
    bool lerp = true;
    int16_t vb;
    if (va > 0) {
        a1 = U(Abs(m.S32(hA + 24)));
        vb = m.S16(hB + 2);
    } else {
        vb = m.S16(hB + 2);
        if (vb <= 0) {
            lerp = false;
        } else if (va != 0) {                                       // "in use" is != 0 here
            a1 = U(Abs(m.S32(hA + 24)));
            vb = m.S16(hB + 2);
        }
    }
    if (lerp) {                                                     // 0x8003E55C
        if (vb != 0) a2 = U(Abs(m.S32(hB + 24)));
        inner = Lerp16(m, f, a1, a2, F);
    }
    if (l < Abs(S(inner))) {                                        // class 4
        m.W16(out + 2, 1);
        m.W32(out + 4, 0);
        m.W16(out + 0, 4);
        return 0;
    }
    if (l < Abs(S(outer))) {                                        // class 1
        m.W32(out + 4, inner);
        const uint16_t sub = m.U16(hA + 18);
        m.W16(out + 0, 1);
        m.W16(out + 2, sub);
        return 0;
    }
    m.W32(out + 4, outer);                                          // off the road
    m.W16(out + 2, 0);
    m.W16(out + 0, 0);
    return 1;
}

uint32_t CoreCrossSection(GuestRam& m, uint32_t gA, uint32_t xsih, int32_t dA, int32_t dB, uint32_t gB,
                          int32_t lat, uint32_t f, uint32_t out, int32_t dir, uint32_t sp) {
    const uint32_t F = sp - 88u;                                    // `addiu sp,sp,-88`
    const uint32_t hA = gA + ((dA > 0) ? 8u : 136u);
    const int32_t sideA = (dA > 0) ? 1 : 2;
    const uint32_t hB = gB + ((dB > 0) ? 136u : 8u);                // the OPPOSITE convention
    const int32_t sideB = (dB > 0) ? 2 : 1;
    const uint32_t outer = Lerp16(m, f, U(Abs(m.S32(hA + 8))), U(Abs(m.S32(hB + 8))), F);
    m.W32(F + 32, outer);                                           // 0x8003EDAC
    const uint16_t h4 = m.U16(hA + 4);
    m.W16(F + 40, h4);
    m.W32(F + 16, U(sideA));                                        // CoreEdgeClass' stack arguments
    m.W32(F + 20, U(sideB));
    m.W32(F + 24, U(dir));
    m.W32(F + 28, out + 20u);
    const int32_t r = CoreEdgeClass(m, gA, gB, f, lat, sideA, sideB, dir, out + 20u, F);
    const uint32_t w = Lerp16(m, f, m.U32(gA + 4), m.U32(gB + 4), F);
    m.W32(out + 28, 0);                                             // 0x8003EDF4
    m.W16(out + 36, 0);
    m.W32(out + 40, m.U32(F + 32));
    const uint16_t a1 = m.U16(F + 40);
    const uint32_t v1 = m.U32(out + 16);
    m.W32(out + 52, w);
    m.W32(out + 0, gA);
    m.W32(out + 4, xsih);
    m.W32(out + 8, gB);
    m.W32(out + 12, xsih);
    m.W32(out + 16, (v1 & ~0xFu) | U(r));
    m.W16(out + 48, a1);
    return 0;
}

int32_t NodeWedge(GuestRam& m, uint32_t point, uint32_t node, uint32_t armA, uint32_t armB,
                  int32_t hint, uint32_t sp) {
    const uint32_t F = sp - 80u;                                    // `addiu sp,sp,-80`
    int32_t w = -1, i = 0, j = 0;                                   // s4, s0, s3
    bool done = false;
    if (hint >= 0) {                                                // 0x8003EB9C
        const int32_t n = m.S16(node + 2);
        if (hint < n) {
            w = hint;
            i = hint;
            if (hint < n - 1) j = hint + 1;
            done = true;
        }
    }
    if (!done && m.S16(node + 2) > 0) {                             // 0x8003EBD0
        // the distance of the point to each arm's plane, kept in memory at sp+16 + 4k; the turned
        // vector (-v2, v1, v0) at sp+32 (three halfwords) - a fifth arm's distance lands on it
        uint32_t k = 0;
        do {
            const uint32_t arm = node + 8u + 24u * k;
            ++k;
            m.W16(F + 32, static_cast<uint16_t>(0u - m.U16(arm + 10)));
            m.W16(F + 34, m.U16(arm + 8));
            m.W16(F + 36, m.U16(arm + 6));
            const int32_t d = AiProjectView(m, point, F + 32, arm + 12);
            m.W32(F + 16u + 4u * (k - 1u), U(d));
        } while (S(k) < m.S16(node + 2));
        const int32_t n = m.S16(node + 2);                          // 0x8003EC38
        if (n > 0) {
            for (i = 0;;) {
                j = (i < n - 1) ? i + 1 : 0;
                const int32_t di = m.S32(F + 16u + 4u * U(i));
                ++w;
                if (di <= 0 && m.S32(F + 16u + 4u * U(j)) > 0) break;   // the wedge i, i+1
                ++i;
                if (!(i < n)) break;                                // NOT FOUND: i = n, w = n - 1
            }
        } else {
            i = 0;
        }
    }
    if (w < 0) return w;                                            // 0x8003ECA0
    if (armA != 0) m.W32(armA, node + 8u + 24u * U(i));
    if (armB != 0) m.W32(armB, node + 8u + 24u * U(j));
    return w;
}

uint32_t RoadClassCore(GuestRam& m, uint32_t e, uint32_t node, uint32_t out, uint32_t cursorOut,
                       int32_t zone, uint32_t sp, RoadRuntimeCallees& calls) {
    const uint32_t F = sp - 176u;                                   // `addiu sp,sp,-176`
    uint32_t sub = 0, slice = 0, gA = 0, frac = 0;                  // s7, s6, s8, s4
    m.W32(sp + 4, node);                                            // a1, a2 to their home slots
    m.W32(sp + 8, out);
    m.W32(F + 120, 0);                                              // CoreCrossSection's arguments
    m.W32(F + 124, 0);
    m.W32(F + 128, 0);
    m.W32(F + 132, 0);
    m.W32(F + 104, 1);                                              // the turn direction, primed
    const uint32_t obj = m.U32(e + 0x148);                          // s2
    if (node != 0 && m.U32(obj + 0x40) != 0) {                      // a node and an XSAI block
        const uint32_t P = e + 0xB8;                                // s1
        m.W32(F + 16, U(zone));                                     // 0x8003E7E8
        const int32_t w = S(calls.NodeWedge(m, P, node, F + 96, F + 100, zone, F));
        if (w >= 0) {
            const uint32_t A = m.U32(F + 96);
            const uint32_t B = m.U32(F + 100);
            const int32_t ra = m.S16(A + 4), rb = m.S16(B + 4);
            m.W32(F + 16, F + 108);                                 // 0x8003E814
            sub = TurnByRoads(m, e + 0x148, ra, rb, F + 104, F + 108);
            // NO NULL CHECK: with sub == 0 these read guest 0x0C and 0x08.
            const int32_t len = m.S32(sub + 12);                    // s0
            GuestCopyWords(m, F + 40, e + 0x148, 32);               // the local cursor at sp+40
            m.W32(F + 48, sub);
            const int32_t first = m.S16(sub + 8);
            m.W32(F + 52, m.U32(obj + 0x34) + U(first) * 52u);
            slice = RoadSliceSearch(m, e + 0xAC, F + 40, P, F);
            RoadProjectView(m, P, slice, F + 112, F + 116, F);
            const uint32_t nd = m.U32(sp + 4);                      // 0x8003E880, from the home slot
            if (m.S16(nd + 2) == 2) m.W32(F + 112, U(Abs(m.S32(F + 112))));
            m.W32(F + 56, m.U32(F + 112));                          // cursor lateral / along
            m.W32(F + 60, m.U32(F + 116));
            m.W16(F + 72, m.U16(slice + 14));                       // the heading, slice row 2
            m.W16(F + 74, m.U16(slice + 16));
            m.W16(F + 76, m.U16(slice + 18));
            RoadPosition(m, F + 72, F + 40, F + 80, F);
            const int32_t k = m.S16(sub + 18);                      // the turn's first XSIH
            if (k >= 0) {
                m.W32(F + 124, m.U32(obj + 0x38) + U(k) * 16u);
                const uint32_t L = m.U32(obj + 0x40) + U(k) * 20u;  // XSAI, parallel to XSIH
                const int32_t g1 = m.S16(L + 4);
                if (g1 != -1) {
                    const int32_t g2 = m.S16(L + 6);
                    if (g2 != -1) {
                        const uint32_t Ap = m.U32(F + 96);          // 0x8003E93C
                        bool fwd = m.S16(Ap + 4) == m.S16(L + 10);
                        uint32_t Bp = 0;
                        if (fwd) {
                            Bp = m.U32(F + 100);
                            fwd = m.S16(Bp + 4) == m.S16(L + 12);
                        }
                        if (fwd) {                                  // the record's order
                            m.W32(F + 128, U(static_cast<int32_t>(m.S16(Ap + 2))));
                            m.W32(F + 132, U(static_cast<int32_t>(m.S16(Bp + 2))));
                            const uint32_t xsdh = m.U32(obj + 0x3C);
                            gA = xsdh + U(g1) * 264u;
                            m.W32(F + 120, xsdh + U(g2) * 264u);
                        } else {                                    // 0x8003E9A8: reversed
                            const int32_t h6 = m.S16(L + 6);
                            const uint32_t xsdh = m.U32(obj + 0x3C);
                            gA = xsdh + U(h6) * 264u;
                            const int32_t h4 = m.S16(L + 4);
                            const uint32_t gB = xsdh + U(h4) * 264u;
                            const uint32_t B2 = m.U32(F + 100);
                            const uint32_t A2 = m.U32(F + 96);
                            m.W32(F + 120, gB);
                            m.W32(F + 128, U(static_cast<int32_t>(m.S16(B2 + 2))));
                            m.W32(F + 132, U(static_cast<int32_t>(m.S16(A2 + 2))));
                        }
                        // 0x8003E9F8: along the turn clamped to [0, len], branch-free
                        const int32_t a = m.S32(F + 88);
                        const int32_t lo = S(~U(a >> 31) & U(a));
                        const int32_t rest = Sub(len, a);
                        const int32_t t = Add(lo, S(U(rest >> 31) & U(rest)));
                        if (t > 0) {                                // SDiv, the sign dance
                            if (len > 0) frac = FixDiv(U(t), U(len));
                            else frac = 0u - FixDiv(U(t), 0u - U(len));
                        } else if (len > 0) {
                            frac = 0u - FixDiv(0u - U(t), U(len));
                        } else {
                            frac = FixDiv(0u - U(t), 0u - U(len));
                        }
                    }
                }
            }
        }
    }
    if (gA != 0) {                                                  // 0x8003EA64
        const uint32_t xs = m.U32(F + 124);
        const uint32_t dA = m.U32(F + 128);
        const uint32_t dB = m.U32(F + 132);
        const uint32_t gB = m.U32(F + 120);
        m.W32(F + 16, gB);
        const uint32_t lat = m.U32(F + 112);
        const uint32_t o = m.U32(sp + 8);                           // from the home slot
        m.W32(F + 24, frac);
        m.W32(F + 28, o);
        m.W32(F + 20, lat);
        const uint32_t dir = m.U32(F + 104);
        m.W32(F + 32, dir);
        const uint32_t r = CoreCrossSection(m, gA, xs, S(dA), S(dB), gB, S(lat), frac, o, S(dir), F);
        if (cursorOut != 0) {
            const uint32_t pc = m.U32(F + 44);
            if (m.S16(pc + 2) == 1) {                               // still on the core
                m.W32(cursorOut + 0, m.U32(F + 40));
                const uint32_t piece = m.U32(F + 44);
                const uint32_t l = m.U32(F + 112), al = m.U32(F + 116);
                m.W32(cursorOut + 8, sub);
                m.W32(cursorOut + 12, slice);
                m.W32(cursorOut + 16, l);
                m.W32(cursorOut + 20, al);
                m.W32(cursorOut + 4, piece);
                const uint32_t turn = m.U32(F + 108), junction = m.U32(F + 64);
                m.W32(cursorOut + 28, turn);
                m.W32(cursorOut + 24, junction);
            } else {
                m.W32(cursorOut + 12, 0);                           // the search left the core
            }
        }
        return r & 0xFFu;
    }
    const uint32_t o = m.U32(sp + 8);                               // 0x8003EB00: no cross-section
    const uint32_t v = m.U32(o + 16);
    m.W32(o + 0, 0);
    m.W32(o + 16, v & ~0xFu);
    if (cursorOut != 0) m.W32(cursorOut + 12, 0);
    return 1;
}

uint32_t RoadsideRunNode(GuestRam& m, uint32_t e, int32_t zone, uint32_t sp, RoadRuntimeCallees& calls,
                         uint32_t callerV0) {
    const uint32_t F = sp - 56u;                                    // `addiu sp,sp,-56`
    if (e == 0) return callerV0;                                    // 0x8003F22C: v0 never set
    const uint32_t s1 = e + 0x1EC;
    int32_t w = -1;
    const uint32_t nd = RoadNodeRecord(m, m.S32(m.U32(e + 0x148)));
    if (nd != 0) {
        m.W32(F + 16, U(zone));                                     // 0x8003F250
        w = S(calls.NodeWedge(m, e + 0xB8, nd, F + 24, F + 28, zone, F));
    }
    if (w < 0) return (nd == 0) ? 0u : U(w);                        // NOTHING written
    const uint32_t obj = m.U32(e + 0x148);                          // 0x8003F270
    const int32_t cnt = m.S16(obj + 78);                            // the BGDT record count
    const int32_t n = (cnt < 4) ? cnt : 3;
    const uint32_t bg = m.U32(obj + 92);                            // the BGDT block
    uint32_t found = 0;
    if (bg != 0 && n > 0) {
        const uint32_t A = m.U32(F + 24), B = m.U32(F + 28);        // read once
        uint32_t r = bg;
        for (int32_t i = 0; i < n; ++i, r += 20u) {
            if (m.U8(r + 2) != 1u) continue;
            const uint32_t r8 = m.U8(r + 8);
            if (r8 == 255u) {                                       // by the wedge index
                if (U(w) == m.U8(r + 3)) {
                    found = r;
                    break;
                }
                continue;
            }
            const int32_t ra = m.S16(A + 4);                        // by the road pair
            if (S(r8) == ra && S(U(m.U8(r + 9))) == m.S16(B + 4)) {
                found = r;
                break;
            }
            if (S(U(m.U8(r + 9))) != ra) continue;                  // ... or the reversed pair
            if (S(r8) != m.S16(B + 4)) continue;
            found = r;
            break;
        }
    }
    if (found == 0) {                                               // 0x8003F35C
        m.W32(s1 + 4, 0);
        return s1 + 8u;                                             // the `addiu` in the delay slot
    }
    m.W32(s1 + 4, s1 + 8u);                                         // 0x8003F36C: +0x1F0 -> +0x1F4
    m.W8(s1 + 10, m.U8(found + 0));
    m.W16(s1 + 8, m.U16(found + 4));
    const uint8_t b6 = m.U8(found + 6);
    m.W8(s1 + 11, b6);
    return b6;
}

namespace {
// RoadShortcut's distance (0x8003C0A8 and 0x8003C27C): ApproxLen3 of (e+0xB8 - P) >> 16 per axis,
// P at sp+64; the three differences are stored at sp+80..88 on the way.
int32_t ShortcutDistance(GuestRam& m, uint32_t e, uint32_t F) {
    const int32_t dx = Sub(m.S32(e + 0xB8), m.S32(F + 64));
    const int32_t pz = m.S32(F + 72);
    m.W32(F + 80, U(dx));
    const int32_t dy = Sub(m.S32(e + 0xBC), m.S32(F + 68));
    m.W32(F + 84, U(dy));
    const int32_t dz = Sub(m.S32(e + 0xC0), pz);
    m.W32(F + 88, U(dz));
    return ApproxLen3(dx >> 16, dy >> 16, dz >> 16);
}
} // namespace

uint32_t RoadShortcut(GuestRam& m, uint32_t e, uint32_t sp, RoadRuntimeCallees& calls) {
    const uint32_t F = sp - 144u;                                   // `addiu sp,sp,-144`
    {
        const uint32_t v = m.U16(e + 0x16A);                        // on a core piece
        if (v != 0) return v;
    }
    if (m.U32(e + 0x174) == 0) return 0;
    if ((m.U32(e + 0x184) & 1u) == 0) return 0;                     // on the road
    const uint32_t obj = m.U32(e + 0x148);                          // s4
    for (uint32_t i = 0; i < 32; i += 4) m.W32(F + 16 + i, 0);      // MemSet32(sp+16, 0, 32)
    m.W32(F + 16, obj);                                             // cursor.object, the delay slot
    const int32_t d = RoadEndNode(m, e + 0x168, F + 96, 0);
    if (m.U32(F + 96) == 0xFFFFFFFFu) return 0x310000u;             // the `lui` in the delay slot
    if (0x31FFFF < d) return 1;                                     // 50.0 units or more
    const uint32_t sl = m.U32(e + 0x154);                           // 0x8003C094
    MulAddView(m, sl + 20, sl + 14, m.S32(e + 0x15C), F + 64);      // its own centre-line point
    const int32_t own = ShortcutDistance(m, e, F);                  // s8
    if (m.S16(obj + 16) != 1) return 1;                             // not a junction object
    {
        const uint32_t half = m.U32(obj + 12);
        if (half != 0) return half;                                 // not the CORE object
    }
    const uint32_t nd = RoadNodeRecord(m, m.S32(obj + 0));          // s5
    if (nd == 0) return 0;
    {
        const int32_t n = m.S16(nd + 2);
        if (n <= 0) return U(n);
    }
    for (int32_t i = 0;;) {                                         // 0x8003C13C, armCount re-read
        const uint32_t arm = nd + 8u + 24u * U(i);
        const uint32_t key = m.U32(e + 0x168);
        const bool ownRoad =
            (key >> 16) == 0 && (key & 0xFFFFu) == U(static_cast<int32_t>(m.S16(arm + 4)));
        if (!ownRoad) {
            const uint32_t pc = RoadFindPiece(m, obj, m.S16(arm + 4));
            if (pc == 0) return 0;                                  // an arm road without a piece
            const int32_t fs = m.S16(pc + 20);
            const int32_t dir = m.S16(arm + 2);
            const uint32_t sub = m.U32(obj + 0x30) + U(fs) * 28u;
            uint32_t s;
            int32_t along;
            if (dir > 0) {                                          // enter at the first slice
                const int32_t f = m.S16(sub + 8);
                along = 0;
                s = m.U32(obj + 0x34) + U(f) * 52u;
            } else {                                                // at the last, its chord along
                const int32_t f = m.S16(sub + 8);
                const int32_t c = m.S16(sub + 10);
                s = m.U32(obj + 0x34) + U(Add(f, c)) * 52u - 52u;
                along = m.S32(s + 32);
            }
            // The cursor at sp+16: +4, +8, +12 and +20 only - the object (+0x00), the lateral
            // (+0x10) and the words +0x18/+0x1C keep what the memset or the previous arm's search
            // left (+0x00 can be ANOTHER object after a search).
            m.W32(F + 20, pc);
            m.W32(F + 24, sub);
            m.W32(F + 28, s);
            m.W32(F + 36, U(along));
            const uint32_t found = RoadSliceSearch(m, e + 0xAC, F + 16, e + 0xB8, F);
            const uint32_t org = found + 20u;
            const int32_t lat = AiProjectView(m, e + 0xB8, found + 2u, org);
            m.W32(F + 32, U(lat));
            const int32_t al = AiProjectView(m, e + 0xB8, found + 14u, org);
            m.W32(F + 36, U(al));
            RoadPosition(m, e + 0x1C2, F + 16, F + 48, F);
            MulAddView(m, org, found + 14u, m.S32(F + 36), F + 64);
            const int32_t there = ShortcutDistance(m, e, F);
            if (there < own) {                                      // strictly nearer only
                const uint32_t k = m.U32(e + 0x168);
                const uint32_t nk = m.U32(F + 48);
                if ((k >> 16) == 0 && (nk >> 16) == 0 && k != nk) { // 0x8003C2F0: the re-bind
                    GuestCopyWords(m, e + 0x148, F + 16, 32);
                    GuestCopyWords(m, e + 0x168, F + 48, 12);
                    RoadClass(m, e, 1, 0, -1, F, calls);
                    RoadsideRun(m, e, 1, -1, F, calls);
                    const uint32_t r = RouteLegOfRoad(m, m.S32(F + 96));   // RouteRecordByKey(node)
                    if (r != 0) m.W32(e + 0x1AC, r);
                    const uint32_t R = m.U32(e + 0x1AC);
                    if (R != 0) {
                        const uint32_t h = m.U16(e + 0xAC);
                        if ((h >> 5) < 2u) {                        // pools 0 and 1
                            uint32_t slot = h & 0x1Fu;
                            const int32_t nb = m.S32(kRoadBikeCount);
                            if (!(S(slot) < nb)) slot -= U(nb);
                            const uint32_t bit = 1u << (slot & 31u);
                            if (r != 0) m.W16(R + 118, static_cast<uint16_t>(m.U16(R + 118) | bit));
                            else m.W16(R + 118, static_cast<uint16_t>(m.U16(R + 118) & ~bit));
                        }
                    }
                    RouteBind(m, e + 0xAC, 1, 0, F, calls);
                    const int32_t v = ProgressScalar(m, e + 0xAC, F);
                    m.W32(e + 0x144, U(v));                         // the delay slot
                    return U(v);
                }
            }
        }
        ++i;                                                        // 0x8003C3E8
        if (!(i < m.S16(nd + 2))) return U(i) << 1;                 // the delay slot's `sll v0,s3,1`
    }
}

uint32_t RoadRuntimeNative::RoadClassCore(GuestRam& m, uint32_t e, uint32_t node, uint32_t out,
                                          uint32_t cursorOut, int32_t zone, uint32_t sp) {
    return rr::sim::RoadClassCore(m, e, node, out, cursorOut, zone, sp, *this);
}
uint32_t RoadRuntimeNative::NodeWedge(GuestRam& m, uint32_t point, uint32_t node, uint32_t armA,
                                      uint32_t armB, int32_t hint, uint32_t sp) {
    return U(rr::sim::NodeWedge(m, point, node, armA, armB, hint, sp));
}
uint32_t RoadRuntimeNative::RoadsideRunNode(GuestRam& m, uint32_t e, int32_t zone, uint32_t sp) {
    // The one caller, 0x8003DFB0, holds -1 in v0 at the call (`li v0,-1` at 0x8003DF94).
    return rr::sim::RoadsideRunNode(m, e, zone, sp, *this, 0xFFFFFFFFu);
}
uint32_t RoadRuntimeNative::RouteBindFirst(GuestRam& m, uint32_t p, uint32_t e, uint32_t pool, uint32_t x,
                                           uint32_t sp) {
    (void)sp;                                                       // no local the port needs
    return rr::sim::RouteBindFirst(m, p, e, pool, x, 0);            // RouteBind never passes p == 0
}
uint32_t RoadRuntimeNative::RouteBindStep(GuestRam& m, uint32_t p, uint32_t pool, uint32_t e, uint32_t x,
                                          uint32_t sp) {
    return rr::sim::RouteBindStep(m, p, pool, e, x, sp);
}
uint32_t RoadRuntimeNative::RoadShortcut(GuestRam& m, uint32_t e, uint32_t sp) {
    return rr::sim::RoadShortcut(m, e, sp, *this);
}

} // namespace rr::sim
