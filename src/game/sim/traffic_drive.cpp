#include "game/sim/traffic_drive.h"

#include <utility>

#include "game/sim/ai.h"
#include "game/sim/fixed.h"
#include "game/sim/road_runtime.h"
#include "game/sim/vec.h"

// Every function below is transcribed from our own disassembly of SLUS_010.53 (SHA-1
// 67ed165a2c517d4e6106fb0dfa324d66dd9a76f1) and RASHCDG.BIN (SHA-1
// cfe43a7786759f2cb9c57751cf99e84d1074782c). Comments give the original's addresses. Loads that may
// fault are made in the original's order; arithmetic wraps in uint32_t wherever the original's does.

namespace rr::sim {

namespace {

inline uint32_t U(int32_t v) { return static_cast<uint32_t>(v); }
inline int32_t S(uint32_t v) { return static_cast<int32_t>(v); }
inline uint32_t SX(int16_t v) { return U(static_cast<int32_t>(v)); }
// The `sra / addu / xor` absolute value: INT32_MIN stays negative.
inline int32_t Abs(int32_t v) {
    const uint32_t sg = U(v >> 31);
    return S((U(v) + sg) ^ sg);
}
inline int32_t Neg(int32_t v) { return S(0u - U(v)); }
inline int32_t Add(int32_t a, int32_t b) { return S(U(a) + U(b)); }
inline int32_t Sub(int32_t a, int32_t b) { return S(U(a) - U(b)); }
// `(s32)(s16) << 4` with `sll`.
inline int32_t Promote(int16_t v) { return S(SX(v) << 4); }

constexpr uint32_t kGameStatePtr = 0x8005B2F8;
constexpr uint32_t kPool0Ptr = 0x8005B3A0;
constexpr uint32_t kPool1Ptr = 0x8005B3A4;
constexpr uint32_t kBikeCount = 0x8005B1F8;

// A walk / chain bound (see the rows file): the original would run on (or around a cycle) forever.
constexpr uint32_t kWalkBound = 1u << 20;

// SLUS 0x8002E698 DotLcm(v, m) over guest addresses: v by three `lh`, m by `lhu, lhu, lh`; its one
// store is MAC1 >> 8 spilled to its own sp-8 (as road_runtime.cpp's). `sp` is the stack pointer at the
// call.
int32_t DotLcmView(GuestRam& g, uint32_t v, uint32_t row, uint32_t sp) {
    const int16_t a[3] = {g.S16(v + 0), g.S16(v + 2), g.S16(v + 4)};
    const int16_t b0 = static_cast<int16_t>(g.U16(row + 0));
    const int16_t b1 = static_cast<int16_t>(g.U16(row + 2));
    const int16_t b[3] = {b0, b1, g.S16(row + 4)};
    const int32_t r = DotLcm(a, b);
    g.W32(sp - 8u, U(r));
    return r;
}

// RASHCDG 0x800B6AAC AiProject over guest addresses (a leaf, no stores).
int32_t AiProjectView(GuestRam& g, uint32_t p, uint32_t axis, uint32_t org) {
    const int32_t pp[3] = {g.S32(p + 0), g.S32(p + 4), g.S32(p + 8)};
    const int16_t ax[3] = {g.S16(axis + 0), g.S16(axis + 2), g.S16(axis + 4)};
    const int32_t o[3] = {g.S32(org + 0), g.S32(org + 4), g.S32(org + 8)};
    return AiProject(pp, ax, o);
}

// SLUS 0x80020018 RatAtan2 over the view: its table read out of guest memory (fixed.h).
int32_t RatAtan2View(GuestRam& g, int32_t y, int32_t x) {
    int32_t table[20];
    for (uint32_t i = 0; i < 20; ++i) table[i] = g.S32(0x8005285Cu + 4u * i);
    return RatAtan2(y, x, table);
}

// The engine's octagonal distance with the original's wrapping arithmetic (big / small already
// ordered by the caller's `slt`).
int32_t OctagonOrdered(int32_t big, int32_t small) {
    const int32_t s = Add(small, small >> 1);
    return Add(Add(Sub(Sub(big, big >> 5), big >> 7), s >> 2), s >> 6);
}

// The `mult` / `mfhi` / `mflo` product of the original, both halves (for the words it spills).
struct Prod {
    uint32_t lo, hi;
};
Prod Mult(int32_t a, int32_t b) {
    const int64_t p = static_cast<int64_t>(a) * static_cast<int64_t>(b);
    return Prod{static_cast<uint32_t>(p), static_cast<uint32_t>(static_cast<uint64_t>(p) >> 32)};
}
// (lo >> 16) | (hi << 16) - the same bits as FixMul.
inline uint32_t Mid(const Prod& p) { return (p.lo >> 16) | (p.hi << 16); }

// The slice index test the road step and the brake share: the cursor's slice is the first or the
// last of its sub-object (`lh 0(slice)` against `lh 10(sub) - 1`).
bool AtSubEnd(GuestRam& g, uint32_t cursor) {
    const int16_t idx = g.S16(g.U32(cursor + 12));
    if (idx == 0) return true;
    return static_cast<int32_t>(idx) == g.S16(g.U32(cursor + 8) + 10) - 1;
}

} // namespace

// ============================================================================ SLUS 0x8003775C

bool RoadStep(GuestRam& g, uint32_t p, uint32_t cur, int32_t dist, uint32_t sp, uint32_t& v0) {
    const uint32_t F = sp - 176u;                                   // `addiu sp,sp,-176`
    const uint32_t cands = F + 24, dirs = F + 120, dirW = F + 136;
    int32_t s4 = 0;
    v0 = 0;
    if (cur == 0) return true;                                      // 0x8003778C
    g.W32(F + 52, 0);                                               // 0x8003779C: cands[0] +0x1C
    uint32_t s0 = g.U32(cur + 12);
    v0 = s0;
    if (dist == 0) return true;                                     // 0x800377A4
    if (p != 0) g.W32(dirW, g.U32(p + 192));                        // 0x800377B4: e[+0x16C]
    else g.W32(dirW, dist > 0 ? 1u : 0xFFFFFFFFu);                  // 0x800377CC
    int32_t s2;
    if (g.S32(dirW) > 0) s2 = S(g.U32(s0 + 32) - g.U32(cur + 20));  // 0x800377E0: to the slice end
    else s2 = g.S32(cur + 20);
    const int32_t s1 = Abs(dist);
    const int32_t a0 = g.S32(dirW);
    g.W32(dirs, U(a0));                                             // 0x8003780C
    if (!(s2 < s1)) {                                               // 0x800379E8: inside this slice
        int32_t v = g.S32(cur + 20);
        v = (a0 > 0) ? Add(v, s1) : Sub(v, s1);
        g.W32(cur + 20, U(v));
        v0 = s0;
        return !g.Faulted();
    }
    // Across a sub-object end: the walker for the direction, then the pick (0x80037884 / 0x80037994).
    auto cross = [&]() -> uint32_t {
        g.W32(F + 16, 3);                                           // the walker's fifth argument
        const int32_t n = (g.S32(dirW) > 0) ? RoadNeighboursForward(g, p, cur, cands, dirs, 3, F)
                                            : RoadNeighboursBackward(g, p, cur, cands, dirs, 3, F);
        g.W32(F + 16, cur);                                         // the pick's fifth and sixth
        g.W32(F + 20, dirW);
        return RoadPickNeighbour(g, p, cands, dirs, n, cur, dirW);  // 0x800378BC / 0x800379C8
    };
    if (!AtSubEnd(g, cur)) {                                        // 0x80037844: the next slice
        s0 = (a0 > 0) ? s0 + 52u : s0 - 52u;
        g.W32(cur + 12, s0);
    } else {
        s0 = cross();
    }
    for (uint32_t guard = 0; s2 < s1; ++guard) {                    // 0x800378C8
        if (g.Faulted()) return false;
        if (guard >= kWalkBound) return false;                      // NAMED BOUND (rows file)
        const int32_t len = g.S32(s0 + 32);
        const int32_t rem = Sub(s1, s2);
        if (!(len < rem)) {                                         // 0x800378E8: it ends in this slice
            s4 = rem;
            if (g.S32(dirW) <= 0) s4 = Sub(len, s4);
            s2 = s1;
            continue;
        }
        const int16_t idx = g.S16(g.U32(cur + 12));                 // 0x80037904
        s2 = Add(s2, len);
        if (idx != 0 && static_cast<int32_t>(idx) != g.S16(g.U32(cur + 8) + 10) - 1) {
            s0 = (g.S32(dirW) > 0) ? s0 + 52u : s0 - 52u;           // 0x80037948
            g.W32(cur + 12, s0);
            continue;
        }
        if (RoadNextObjectMissing(g, cur, g.S32(dirW)) != 0) {      // 0x80037964: stop at the end
            g.W32(cur + 20, U(s4));
            v0 = s0;
            return !g.Faulted();
        }
        s0 = cross();
    }
    g.W32(cur + 20, U(s4));                                         // 0x800379E4
    v0 = s0;
    return !g.Faulted();
}

// ============================================================================ 0x8008AE94

bool GridQuery(GuestRam& g, uint32_t out, uint32_t cnt, uint32_t pos, int32_t radius, uint32_t sp) {
    const uint32_t F = sp - 56u;                                    // `addiu sp,sp,-56`
    uint32_t t0 = 0;
    const uint32_t oz0 = g.U32(kTdGridOriginZ);
    const uint32_t pz = g.U32(pos + 8), px = g.U32(pos + 0);
    const uint32_t ox0 = g.U32(kTdGridOriginX);
    const uint16_t self = g.U16(F + 76);                            // 0x8008AEEC: the sixth argument
    int32_t gx = (S(px - ox0) >> 21) + 11;
    int32_t gz = (S(pz - oz0) >> 21) + 11;
    const uint32_t np = g.U32(g.U32(kGameStatePtr) + 48);
    g.W16(F + 0, self);                                             // 0x8008AF1C
    if (np != 1 && !(U(gx) < 24u && U(gz) < 24u)) {                 // 0x8008AF34: the second grid's z
        gz = (S(pz - g.U32(kTdGridOriginZ + 4)) >> 21) + 11;
        gz = S(U(gz) + (24u << (U(gz >> 31) & 31u)));
    }
    const uint32_t hi = (gz < 24) ? 0u : 1u;                        // 0x8008AF5C
    const int32_t half = Add(radius, Add(radius, S(U(radius) >> 31)) >> 1);
    const uint32_t np2 = g.U32(g.U32(kGameStatePtr) + 48);
    const int32_t lim = S(24u << ((np2 - 1u) & 31u));
    const uint32_t s8 = hi & ((gz < lim) ? 1u : 0u);                // the grid
    const uint32_t ox = g.U32(kTdGridOriginX + 4u * s8);
    const uint32_t x = g.U32(pos + 0), z = g.U32(pos + 8);
    int32_t s7 = (S(x - U(half) - ox) >> 21) + 11;
    const uint32_t oz = g.U32(kTdGridOriginZ + 4u * s8);
    int32_t t8 = (S(x + U(half) - ox) >> 21) + 11;
    int32_t t1 = (S(z - U(half) - oz) >> 21) + 11;
    int32_t s6 = (S(z + U(half) - oz) >> 21) + 11;
    if (s7 < 0) s7 = 0;
    if (t1 < 0) t1 = 0;
    if (!(t8 < 24)) t8 = 23;
    if (!(s6 < 24)) s6 = 23;
    if (!(s7 < 24) || !(t1 < 24) || t8 < 0 || s6 < 0) {             // 0x8008B048
        g.W32(cnt, 0);
        return !g.Faulted();
    }
    const uint32_t base = (0u - s8) & 0x18u;                        // 0x8008B050
    uint32_t guard = 0;
    for (int32_t row = t1; !(s6 < row); ++row) {
        if (t8 < s7) continue;                                      // 0x8008B070
        g.W32(F + 8, g.U32(kPool0Ptr));                             // 0x8008B084
        const uint32_t t7 = base + U(row);
        for (int32_t col = s7; !(t8 < col); ++col) {
            uint32_t t2 = g.U8(kTdGridCells + U(col) + 24u * t7);   // 0x8008B0A4
            if (t2 == 128u) continue;
            const uint32_t pool1 = g.U32(kPool1Ptr);
            uint32_t wp = out + 2u * t0;
            const uint16_t skip = g.U16(F + 0);
            do {
                if (g.Faulted() || ++guard > kWalkBound) return false; // NAMED BOUND (rows file)
                const uint32_t hb = g.U8(kTdGridLinks + 2u * t2 + 1u);
                const uint32_t mask = g.U32(F + 72);                // the fifth argument
                const uint32_t kind = hb >> 5;
                if (mask & (1u << (kind & 31u))) {
                    uint32_t rec;                                   // where the handle and box are
                    uint32_t hOff, xOff, zOff;
                    if (kind == 0) {                                // 0x8008B100: a bike
                        rec = g.U32(F + 8) + 1096u * (hb & 0xFFFFu);
                        hOff = 172; xOff = 184; zOff = 192;
                    } else if (kind == 1) {                         // 0x8008B1DC: a rider
                        rec = pool1 + 628u * (hb & 0x1Fu);
                        hOff = 172; xOff = 184; zOff = 192;
                    } else {                                        // 0x8008B2BC: pools 2..7
                        const uint32_t tb = kTdPoolTable + (kind << 4);
                        const uint32_t stride = g.U32(tb + 4);
                        const uint32_t b = g.U32(tb);
                        const uint32_t off = g.U32(kTdHandleOffTab + 4u * (kind - 2u));
                        rec = b + stride * (hb & 0x1Fu) + off;
                        hOff = 0; xOff = 12; zOff = 20;
                    }
                    const uint16_t hv = g.U16(rec + hOff);
                    if (hv != skip) {
                        const int32_t dx = Abs(S(g.U32(rec + xOff) - g.U32(pos + 0)));
                        const int32_t dz = Abs(S(g.U32(rec + zOff) - g.U32(pos + 8)));
                        const int32_t mn = (dx < dz) ? dx : dz;
                        const int32_t metric = Sub(Add(dx, dz), Add(mn, S(U(mn) >> 31)) >> 1);
                        if (!(radius < metric)) {
                            uint32_t k = 0;
                            if (t0 != 0) {                          // 0x8008B198: already listed?
                                for (uint32_t a = out;; a += 2u) {
                                    if (g.U16(a) == hv) break;
                                    if (++k == t0) break;
                                }
                            }
                            if (k == t0) {
                                // pools 0 / 1 re-read the handle (0x8008B1C4); the rest store the one read
                                g.W16(wp, (kind < 2u) ? g.U16(rec + hOff) : hv);
                                wp += 2u;
                                ++t0;
                                if (t0 == g.U32(cnt)) return !g.Faulted();   // 0x8008B3B8: full
                            }
                        }
                    }
                }
                t2 = g.U8(kTdGridLinks + 2u * t2);                  // 0x8008B3C8
            } while (t2 != 128u);
        }
    }
    g.W32(cnt, t0);                                                 // 0x8008B3F8
    return !g.Faulted();
}

// ============================================================================ the car's callees

uint32_t CarAhead(GuestRam& g, uint32_t car) {
    uint32_t best = 0;
    int32_t bestD = S(0xFFFF0000u);
    int32_t n = g.S32(g.U32(kTdPool3Table + 12));
    uint32_t e = g.U32(kTdPool3Table);
    for (; n >= 0; --n, e += g.U32(kTdPool3Table + 4)) {
        if (g.U16(e + 172) == 0) continue;
        if (g.U32(e + 360) != g.U32(car + 360)) continue;
        const int32_t dir = g.S32(car + 364);
        if (S(g.U32(e + 364) ^ U(dir)) < 0) continue;              // 0x8009BAB4: the other way
        bool ahead = false;
        if (dir > 0 && g.S32(car + 368) < g.S32(e + 368)) ahead = true;
        else if (dir < 0 && g.S32(e + 368) < g.S32(car + 368)) ahead = true;
        if (!ahead) continue;
        const int32_t d = Abs(Sub(g.S32(car + 368), g.S32(e + 368)));   // 0x8009BAFC
        if (bestD < 0 || d < bestD) {
            best = e;
            bestD = d;
        }
    }
    return best;
}

uint32_t LanePlayer(GuestRam& g, uint32_t car, uint32_t sp) {
    const uint32_t F = sp - 8u;                                     // `addiu sp,sp,-8`
    const uint32_t gs = g.U32(kGameStatePtr);
    if (!(g.U8(gs + 4) & 0x10u)) return 0;
    const uint32_t a1 = g.U32(g.U32(kTdPlayer1Bike) + 360);
    if (a1 == g.U32(g.U32(kTdPlayer2Bike) + 360)) {                 // both players on one road key
        if (g.U32(car + 360) != a1) return 0;
        for (int32_t p = 0; p < g.S32(gs + 48); ++p) {              // 0x800A0528
            const uint32_t b = g.U32(kPopPlayerBikes + 4u * U(p));
            const int32_t d = Abs(Sub(g.S32(car + 368), g.S32(b + 368)));
            g.W32(F + 4u * U(p), U(d));
        }
        return (g.S32(F + 4) < g.S32(F + 0)) ? 1u : 0u;             // 0x800A0570
    }
    const int32_t np = g.S32(gs + 48);                              // 0x800A057C
    if (!(0 < np)) return 0;
    const uint32_t key = g.U32(car + 360);
    for (int32_t p = 0; p < np; ++p)
        if (key == g.U32(g.U32(kPopPlayerBikes + 4u * U(p)) + 360)) return U(p);
    return 0;
}

bool PlayerClose(GuestRam& g, uint32_t car, uint32_t sp, TrafficDriveCallees& c, int32_t& v0) {
    const uint32_t F = sp - 24u;                                    // `addiu sp,sp,-24`
    v0 = 0;
    const uint32_t idx = g.U8(car + 511);
    if (!(S(idx) < g.S32(kBikeCount))) return true;                 // 0x800A05F8
    const uint32_t b = g.U32(kPool0Ptr) + 1096u * idx;
    const uint32_t r = g.U32(b + 852);
    uint32_t e = b;
    if (!(g.U32(r + 604) < 3u)) e = r;                              // a downed rider: the rider
    const uint32_t key = g.U32(car + 360);
    if (key != g.U32(e + 360)) return true;
    const int32_t along = g.S32(car + 368);
    const int32_t d = Sub(g.S32(e + 368), along);
    if ((key >> 16) == 1) {                                         // at a node
        v0 = (0x2FFFF < Abs(d)) ? 0 : 1;
        return true;
    }
    if (S(U(d) ^ g.U32(car + 364)) >= 0) return true;               // 0x800A069C: not head-on
    if (0xC0000 < Abs(d)) {
        v0 = 1;
        return true;
    }
    if (!(0x2FFFF < along)) {                                       // 0x800A06CC
        v0 = 1;
        return true;
    }
    uint32_t end = 0;
    if (!c.RoadLength(car + 328, F, end)) return false;             // 0x800A06D4
    v0 = (Sub(S(end), 0x30000) < g.S32(car + 368)) ? 1 : 0;
    return true;
}

void CarBrake(GuestRam& g, uint32_t car, int32_t dt, uint32_t changed, uint32_t sp) {
    const uint32_t F = sp - 40u;                                    // `addiu sp,sp,-40`
    if (car != 0 && g.U16(car + 362) == 1) {                        // at a node
        g.W8(car + 509, static_cast<uint8_t>(g.U8(car + 509) & 0xBCu));
        return;
    }
    if (g.U8(car + 509) & 0x40u) return;                            // 0x8009B500: pulling away
    if (g.S32(car + 480) < 131) {
        const int32_t w = g.S32(car + 504);
        if (w >= 0) {                                               // 0x8009B524: the stop timer
            const int32_t t = Add(w, dt);
            g.W32(car + 504, U(t));
            if (!(0x1FFFF < t)) return;
            g.W32(car + 484, 0x26666);                              // 0x8009B548: pull away at 2.4
            g.W32(car + 504, 0xFFFF0000u);
            g.W8(car + 509, static_cast<uint8_t>((g.U8(car + 509) & 0xFCu) | 0x40u));
            return;
        }
    }
    const uint8_t fl = g.U8(car + 509);                             // 0x8009B564
    if (fl & 1u) {                                                  // braking for the end node
        const int32_t d = RoadEndNode(g, car + 360, F + 16, 1);
        if (!(g.S32(car + 480) < 131) && !(d < S(g.U32(car + 308) << 1))) return;
        g.W32(car + 504, 0);                                        // 0x8009B5B0
        return;
    }
    if (changed == 0 && !(fl & 2u)) {                               // 0x8009B5B8
        const int16_t idx = g.S16(g.U32(car + 340));
        if (idx != 0 && static_cast<int32_t>(idx) != g.S16(g.U32(car + 336) + 10) - 1) return;
    }
    if (g.U16(car + 362) != 0) {                                    // 0x8009B704
        g.W8(car + 509, static_cast<uint8_t>(g.U8(car + 509) & 0xFDu));
        return;
    }
    const int32_t d = RoadEndNode(g, car + 360, F + 16, 1);         // 0x8009B610
    if (g.S32(F + 16) == -1) return;
    if (0xEFFFF < d) return;
    if (g.S32(car + 480) < 132) return;
    const int32_t v = g.S32(car + 480);
    g.W8(car + 509, static_cast<uint8_t>(g.U8(car + 509) | 0x81u)); // 0x8009B664
    const int32_t nsq = Neg(FixMul(v, v));
    const int32_t d2 = S(U(d) << 1);
    uint32_t r;
    if (nsq > 0) {                                                  // 0x8009B674
        const int32_t n = Neg(FixMul(g.S32(car + 480), g.S32(car + 480)));
        r = (d2 > 0) ? FixDiv(U(n), U(d2)) : 0u - FixDiv(U(n), 0u - U(d2));
    } else {                                                        // 0x8009B6B0
        const int32_t n = FixMul(g.S32(car + 480), g.S32(car + 480));
        r = (d2 > 0) ? 0u - FixDiv(U(n), U(d2)) : FixDiv(U(n), 0u - U(d2));
    }
    g.W32(car + 484, r);                                            // 0x8009B700
}

void LaneChange(GuestRam& g, uint32_t car, int32_t dt, uint32_t sp) {
    const uint32_t F = sp - 64u;                                    // `addiu sp,sp,-64`
    if (g.U8(car + 510) & 1u) return;                               // already changing
    const int32_t acc = Add(g.S32(kTdLaneTimer), dt);
    g.W32(kTdLaneTimer, U(acc));                                    // 0x8009B75C
    if (!(0x20000 < acc)) return;
    if (g.S32(car + 484) > 0 && g.S32(car + 480) > 0) return;       // 0x8009B778
    const uint32_t p = LanePlayer(g, car, F);                       // 0x8009B790
    const uint32_t pb = g.U32(kPopPlayerBikes + 4u * p);
    const uint32_t view = kPopViewArray + kPopViewStride * p;
    const int32_t dx = Sub(g.S32(car + 184), g.S32(view + 184));
    g.W32(F + 16, U(dx));
    const int32_t dy = Sub(g.S32(car + 188), g.S32(view + 188));
    g.W32(F + 20, U(dy));
    const int32_t dz = Sub(g.S32(car + 192), g.S32(view + 192));
    g.W32(F + 24, U(dz));
    const Prod px = Mult(dx, Promote(g.S16(view + 444)));
    const Prod py = Mult(dy, Promote(g.S16(view + 446)));
    const Prod pz = Mult(dz, Promote(g.S16(view + 448)));
    g.W32(F + 40, pz.lo);                                           // 0x8009B878
    g.W32(F + 44, pz.hi);
    const int32_t front = S(Mid(pz) + (Mid(py) + Mid(px)));
    do {
        if (car == 0) break;                                        // 0x8009B890
        if (front < 0) break;                                       // behind the camera
        if (g.S32(car + 480) < 132) break;
        const uint32_t key = g.U32(car + 360);
        if ((key >> 16) != 0) break;
        if (key != g.U32(pb + 360)) break;
        if (g.U8(car + 509) & 0x13u) break;
        const uint32_t a = U(Abs(Sub(g.S32(car + 368), g.S32(pb + 368)))) + 0xFFD80000u;
        if (0x280000u < a) break;                                   // 2.5 .. 5.0 from him
        const uint32_t xs = g.U32(car + 372);
        if (xs == 0) break;
        const int32_t lanes = (g.S32(car + 364) > 0) ? g.S16(xs + 140) : g.S16(xs + 12);
        if (lanes < 2) break;
        if (!(0x640000 < RoadEndNode(g, car + 360, F + 32, 1))) break;   // 0x8009B968
        const uint32_t ah = CarAhead(g, car);                       // 0x8009B980
        if (ah != 0 && !(0x190000 < Abs(Sub(g.S32(ah + 368), g.S32(car + 368))))) break;
        if (!((GuestRand(g) & 0x64u) < 40u)) break;                 // 0x8009B9BC: `& 0x64`, the original's
        const int8_t lane = g.S8(car + 508);
        if (lanes < 3) {
            if (lane != 2) g.W8(car + 508, static_cast<uint8_t>(g.U8(car + 508) ^ 3u));
        } else if (lane != 2) {
            g.W8(car + 508, 2);                                     // 0x8009BA2C
        } else {
            g.W8(car + 508, (g.S16(car + 370) & 1) ? 3 : 1);        // 0x8009BA04
        }
        g.W8(car + 510, static_cast<uint8_t>(g.U8(car + 510) | 3u)); // 0x8009BA3C
    } while (false);
    g.W32(kTdLaneTimer, 0);                                         // 0x8009BA44
}

int32_t BlockGiveUp(GuestRam& g, uint32_t car, uint32_t h, uint32_t sp) {
    const uint32_t gs = g.U32(kGameStatePtr);                       // 0x8009F6FC, before the frame
    const uint32_t F = sp - 80u;                                    // `addiu sp,sp,-80`
    if (g.S32(gs + 48) > 0) {
        uint32_t s1 = F, t4 = kPopViewArray;
        for (int32_t p = 0;; ++p) {
            g.W32(s1, 0);                                           // 0x8009F74C
            const int32_t a3 = g.S32(t4 + 184), a2 = g.S32(car + 184);
            const int32_t vz = g.S16(t4 + 194), cz = g.S16(car + 194);
            int32_t big = Abs(Sub(a3 >> 16, a2 >> 16));
            int32_t small = Abs(Sub(vz, cz));
            if (big < small) std::swap(big, small);
            const int32_t dx = Sub(a2, a3);
            g.W32(F + 8, U(dx));
            const int32_t dy = Sub(g.S32(car + 188), g.S32(t4 + 188));
            g.W32(F + 12, U(dy));
            const int32_t dz = Sub(g.S32(car + 192), g.S32(t4 + 192));
            g.W32(F + 16, U(dz));
            const int32_t hx = Sub(g.S32(h + 12), g.S32(t4 + 184));
            g.W32(F + 24, U(hx));
            const int32_t hy = Sub(g.S32(h + 16), g.S32(t4 + 188));
            g.W32(F + 28, U(hy));
            const int32_t hz = Sub(g.S32(h + 20), g.S32(t4 + 192));
            g.W32(F + 32, U(hz));
            const int32_t ax = Promote(g.S16(t4 + 444)), ay = Promote(g.S16(t4 + 446)), az = Promote(g.S16(t4 + 448));
            const int32_t oct = OctagonOrdered(big, small);
            const Prod qz = Mult(dz, az);
            g.W32(F + 40, qz.lo);                                   // 0x8009F898
            g.W32(F + 44, qz.hi);
            const int32_t carFront = S(Mid(qz) + (Mid(Mult(dy, ay)) + Mid(Mult(dx, ax))));
            bool set = false, tail = true;
            if (carFront < 0) {                                     // the car is behind the camera
                const Prod qx = Mult(hx, ax);
                g.W32(F + 40, qx.lo);                               // 0x8009F8E8
                g.W32(F + 44, qx.hi);
                const int32_t hFront = S(Mid(Mult(hz, az)) + (Mid(Mult(hy, ay)) + Mid(qx)));
                if (hFront < 0) {                                   // and so is the blocker
                    tail = false;
                    if (!(oct < 160)) set = true;
                }
            }
            if (tail && !(oct < 160) && (g.U16(h) >> 5) == 3u) set = true;   // 0x8009F940
            if (set) g.W32(s1, 1);                                  // 0x8009F960
            s1 += 4u;
            t4 += kPopViewStride;
            if (!(p + 1 < g.S32(gs + 48))) break;
        }
    }
    const uint32_t gs2 = g.U32(kGameStatePtr);                      // 0x8009F97C
    if (g.U8(gs2 + 4) & 0x10u) {
        if (g.U32(F + 0) == 0) return 0;
        return (0u < g.U32(F + 4)) ? 1 : 0;
    }
    return g.S32(F + 0);
}

bool Horn(GuestRam& g, uint32_t car, uint32_t h, uint32_t sp, TrafficDriveCallees& c) {
    const uint32_t F = sp - 32u;                                    // `addiu sp,sp,-32`
    if (car == 0 || h == 0) return true;
    const uint32_t kind = g.U16(h) >> 5;
    if (!(kind < 2u)) return true;                                  // a bike or a rider only
    if (!(g.S32(car + 480) < 131)) return true;
    if (!(g.U8(car + 509) & 0x10u)) return true;
    if (kind == 0) {                                                // 0x8009FA54: not a finished bike
        const uint32_t b = g.U32(kPool0Ptr) + 1096u * g.U16(h);
        if (g.U32(g.U32(b + 1084) + 40) != 0) return true;
    }
    const int32_t id = S(99u - (g.U32(car + 180) & 1u));            // 0x8009FA90
    if ((GuestRand(g) & 7u) != 0) return true;                      // 0x8009FAA0
    return c.PlaySound3D(g.S32(car + 184), g.S32(car + 192), id, 0, F);   // 0x8009FABC
}

bool WarnSound(GuestRam& g, uint32_t idx, uint32_t bit, int32_t x, int32_t z, uint32_t sp, TrafficDriveCallees& c) {
    const uint32_t F = sp - 24u;                                    // `addiu sp,sp,-24`
    const uint32_t gs = g.U32(kGameStatePtr);
    const int32_t d = Sub(g.S32(g.gp() + kTdWarnClockGp), g.S32(gs + 12));
    if (Abs(d) < 76) return true;                                   // 0x80017D20
    const uint32_t tab = g.U32(g.gp() + kTdWarnPosGp);
    if (tab != 0) {                                                 // a quarter of the way from the listener
        const uint32_t e = tab + 72u * idx;
        const int32_t ex = g.S32(e + 4), ez = g.S32(e + 8);
        x = Add(ex, Sub(x, ex) >> 2);
        z = Add(ez, Sub(z, ez) >> 2);
    }
    if (!c.PlaySound3D(x, z, S(bit + 98u), 0, F)) return false;     // 0x80017D70
    g.W32(g.gp() + kTdWarnClockGp, g.U32(g.U32(kGameStatePtr) + 12));   // 0x80017D8C
    return true;
}

bool PassWarn(GuestRam& g, uint32_t car, uint32_t sp, TrafficDriveCallees& c, int32_t& v0) {
    const uint32_t F = sp - 40u;                                    // `addiu sp,sp,-40`
    v0 = 0;
    int32_t s3 = 0;
    if (g.U32(g.U32(kGameStatePtr) + 48) == 0) return true;         // 0x8009FD0C
    uint32_t p = 0, slot = kPopPlayerBikes;
    do {
        const uint32_t b = g.U32(slot);
        do {
            if (car == 0) break;
            if (!(g.U32(g.U32(b + 852) + 604) < 3u)) break;         // seated
            if (!(0x280000 < g.S32(b + 480))) break;                // faster than 40.0
            if (g.U32(b + 360) != g.U32(car + 360)) break;
            const int32_t a1 = g.S32(b + 364);
            if (!(S(U(a1) ^ g.U32(car + 364)) < 0)) break;          // the other way
            bool passed = false;
            if (a1 > 0 && g.S32(b + 368) < g.S32(car + 368)) passed = true;
            else if (a1 < 0 && g.S32(car + 368) < g.S32(b + 368)) passed = true;
            if (!passed) break;
            int32_t d;
            const uint32_t k = g.U32(b + 360);                      // 0x8009FDC4
            if (k == g.U32(car + 360) && ((k >> 16) == 0 || g.U32(b + 336) == g.U32(car + 336)))
                d = Sub(g.S32(b + 344), g.S32(car + 344));
            else
                d = AiProjectView(g, b + 184, car + 432, car + 184);   // 0x8009FE0C
            if (!(Abs(d) < g.S32(car + 304))) break;
            s3 = 1;
            if (!WarnSound(g, p, g.U32(car + 180) & 1u, g.S32(car + 184), g.S32(car + 192), F, c)) return false;
        } while (false);
        ++p;
        slot += 4u;
    } while (p < g.U32(g.U32(kGameStatePtr) + 48));                // 0x8009FE64 (`sltu`)
    v0 = s3;
    return true;
}

int32_t BlockerGap(GuestRam& g, uint32_t h, uint32_t car, uint32_t sp) {
    const uint32_t F = sp - 40u;                                    // `addiu sp,sp,-40`
    const uint32_t tb = kTdPoolTable + ((h >> 1) & 0x7FF0u);        // 0x800BF10C: 16 x (h >> 5)
    const uint32_t stride = g.U32(tb + 4);
    const uint32_t e = g.U32(tb) + stride * (h & 0x1Fu);
    int32_t s0;
    const uint32_t k = g.U32(e + 360);
    if (k == g.U32(car + 360) && ((k >> 16) == 0 || g.U32(e + 336) == g.U32(car + 336))) {
        s0 = Sub(g.S32(e + 344), g.S32(car + 344));                 // 0x800BF174: the lateral gap
        if (g.S32(car + 364) < 0) s0 = Neg(s0);
    } else {
        s0 = AiProjectView(g, e + 184, car + 432, car + 184);       // 0x800BF1A8
    }
    const uint32_t a3 = U(s0) >> 31;
    const uint32_t a1 = ((S(g.U32(e + 364) ^ g.U32(car + 364)) >> 31) == 0) ? 1u : 0u;
    if (((h & 0xFFFFu) >> 5) == 0) {                                // 0x800BF1D0: a bike with a passenger
        const uint32_t pas = g.U32(e + 856);
        if (pas != 0 && a1 == a3) {
            const uint32_t w = g.U32(e + 304) + g.U32(pas + 304);
            s0 = (a1 != 0) ? S(U(s0) + w) : S(U(s0) - w);
        }
    }
    s0 = Abs(s0);
    const int32_t t0 = DotLcmView(g, e + 444, car + 444, F);        // 0x800BF234: the headings
    const int32_t at = Abs(t0);
    const uint32_t b1 = (at < 16384) ? 1u : 0u;
    uint32_t b3 = 0;
    if (b1 == 0) b3 = (0xDDB1 < at) ? 0u : 1u;
    const int32_t a2 = g.S32(e + 304), a0 = g.S32(e + 308);
    int32_t v = S((U(a2) + U(a0)) * 3u);
    if (v < 0) v = Add(v, 3);
    v >>= 2;
    const uint32_t thr = U(a2) + ((0u - b3) & (U(v) - U(a2))) + ((0u - b1) & (U(a0) - U(a2))) + g.U32(car + 304);
    if (S(thr) < s0) return S(0x7FFF0000u);                         // 0x800BF2B8: not in the path
    int32_t s1;
    if ((g.U16(e + 172) >> 5) < 4u && b1 == 0) {                    // 0x800BF2C0: the closing speed
        s1 = (t0 < 0) ? Add(g.S32(car + 480), g.S32(e + 480)) : Sub(g.S32(car + 480), g.S32(e + 480));
    } else {
        s1 = g.S32(car + 480);
    }
    if (g.S32(car + 480) != 0 || g.S32(e + 480) != 0) {             // 0x800BF30C
        if (s1 < 131) return S(0x7FFF0000u);
    }
    int32_t d = AiProjectView(g, e + 184, car + 444, car + 184);    // 0x800BF33C: along the heading
    const int32_t sub = ((g.U16(e + 172) >> 5) != 0) ? S(2u * g.U32(car + 308) + g.U32(e + 308)) : g.S32(car + 308);
    d = Sub(d, sub);
    if (d < 0) return (g.S32(car + 308) < Abs(d)) ? S(0x7FFF0000u) : d;   // 0x800BF380
    if (s1 < 132) return d;                                         // 0x800BF3A8
    if (d > 0) return (s1 > 0) ? S(FixDiv(U(d), U(s1))) : S(0u - FixDiv(U(d), 0u - U(s1)));
    return (s1 > 0) ? S(0u - FixDiv(0u - U(d), U(s1))) : S(FixDiv(0u - U(d), 0u - U(s1)));   // 0x800BF3E4
}

bool Blocker(GuestRam& g, uint32_t car, uint32_t sp, uint32_t& v0) {
    const uint32_t F = sp - 88u;                                    // `addiu sp,sp,-88`
    v0 = 0;
    uint32_t s3 = 224;
    int32_t s2 = 0xA0000;
    g.W32(F + 56, 16);                                              // 0x800BE800: the list's room
    g.W32(F + 16, 59);                                              // the mask: pools 0, 1, 3, 4, 5
    g.W32(F + 20, g.U16(car + 172));                                // the handle to skip
    if (!GridQuery(g, F + 24, F + 56, car + 184, 0x3C5997, F)) return false;   // 0x800BE828
    for (int32_t i = 0; i < g.S32(F + 56); ++i) {                   // 0x800BE844
        const uint32_t a = F + 24u + 2u * U(i);
        const int32_t v = BlockerGap(g, g.U16(a), car, F);
        if (v < s2) {
            s2 = v;
            s3 = g.U16(a);
        }
    }
    if ((s3 & 0xFFFFu) == 224u) {                                   // 0x800BE884: nothing
        if (g.U8(car + 509) & 0x30u) g.W32(car + 484, 0x26666);
        g.W8(car + 509, static_cast<uint8_t>(g.U8(car + 509) & 0xCFu));
        return !g.Faulted();
    }
    const uint32_t kind = (s3 & 0xFFFFu) >> 5;
    uint32_t s0;
    if (kind == 6) {
        s0 = g.U32(kTdPool6Ptr) + 280u * (s3 & 0x1Fu);              // 0x800BE8C4
    } else {
        const uint32_t tb = kTdPoolTable + (kind << 4);
        const uint32_t stride = g.U32(tb + 4);
        s0 = g.U32(tb) + stride * (s3 & 0x1Fu) + 172u;              // 0x800BE910
    }
    const int32_t a1 = g.S32(car + 480);
    if (!(a1 < 132)) {                                              // 0x800BE928: brake to the gap
        int32_t n = S(g.U32(car + 308) << 1), d = a1;
        bool add;
        if (n > 0) {
            if (d > 0) add = false;
            else { d = Neg(d); add = true; }
        } else if (d <= 0) {
            n = Neg(n);
            d = Neg(d);
            add = false;
        } else {
            n = Neg(n);
            add = true;
        }
        const uint32_t q = FixDiv(U(n), U(d));
        s2 = add ? S(U(s2) + q) : S(U(s2) - q);
        if (s2 < 3276) {                                            // 0x800BE97C: stop
            g.W32(car + 484, 0);
            g.W32(car + 480, 0);
        } else {
            const int32_t v = g.S32(car + 480);
            const int32_t nv = Neg(v);
            uint32_t r;
            if (nv > 0) r = (s2 > 0) ? FixDiv(U(nv), U(s2)) : 0u - FixDiv(U(nv), 0u - U(s2));
            else r = (s2 > 0) ? 0u - FixDiv(U(v), U(s2)) : FixDiv(U(v), 0u - U(s2));
            g.W32(car + 484, r);                                    // 0x800BE9E0
        }
    }
    uint8_t f = g.U8(car + 509);                                    // 0x800BE9E4
    if (!(f & 0x10u)) {
        g.W8(car + 509, static_cast<uint8_t>(f | 0x80u));
        f = g.U8(car + 509);
    }
    g.W8(car + 509, static_cast<uint8_t>(f | 0x10u));
    v0 = s0;
    return !g.Faulted();
}

// ============================================================================ 0x8009A298

namespace {

// One pool-3 slot's frame of the pass (0x8009A2EC .. 0x8009AB18). False when a callee refused.
bool CarFrame(GuestRam& g, uint32_t car, uint32_t F, TrafficDriveCallees& c) {
    PopulationCallees& pc = c.Population();
    if (car == 0) return true;                                      // 0x8009A2EC
    if (g.U16(car + 172) == 0) return true;
    int32_t ok = 0;
    if (!CarCheck(g, car, F, pc, ok)) return false;                 // 0x8009A304
    if (ok == 0) return true;
    const uint32_t h = car + 172;
    const uint32_t lv = RoadWindow(g, h, F);                        // 0x8009A318
    g.W16(car + 320, static_cast<uint16_t>(lv));                    // 0x8009A320
    if ((lv & 0xFFFFu) == 0) return pc.Release(h, 3, F);            // 0x8009AA98: out of the window
    auto release = [&]() {                                          // 0x8009AA90
        g.W16(car + 320, 0);
        return pc.Release(h, 3, F);
    };
    const uint32_t bx = g.U32(car + 184), by = g.U32(car + 188), bz = g.U32(car + 192);
    g.W32(car + 468, bx);                                           // 0x8009A34C: the previous box
    g.W32(car + 472, by);
    g.W32(car + 476, bz);
    uint32_t s4 = 0, s6 = 0;
    if (g.U8(g.U32(kGameStatePtr) + 4) & 1u) {                      // 0x8009A358: an odd race type
        if (g.U32(car + 180) == 0) {                                // a class-0 car
            s6 = 1;
            int32_t close = 0;
            if (!PlayerClose(g, car, F, c, close)) return false;    // 0x8009A380
            if (close != 0) {
                g.W32(car + 484, 0);
                g.W32(car + 480, 0);
            }
        }
    }
    {
        const uint32_t gs = g.U32(kGameStatePtr);                   // 0x8009A39C: race phase 1 or 2
        if (U(g.U8(gs + 57)) - 1u < 2u) {
            const uint32_t cls = g.U32(car + 180);
            if (cls == 0 || cls == 15) {
                if (g.S32(car + 480) < 131 && g.S32(car + 484) < 131) {
                    g.W32(car + 484, 0);                            // 0x8009A3FC: parked
                    g.W32(car + 480, 0);
                    return true;
                }
            } else if (g.U32(g.U32(g.U32(kTdPlayer1Bike) + 1084) + 40) == 0) {   // 0x8009A408
                const uint32_t fin = U(g.U16(kTdFinishAlong)) << 16;
                const int32_t s0 = S(g.U32(car + 368) - fin);
                const int32_t x = S(g.U32(car + 364) ^ g.U32(kTdFinishDir));
                bool brake;
                if (x < 0) brake = (s0 > 0) && !(0x59FFFF < s0);    // 0x8009A474
                else brake = (S(0xFFEC0000u) < s0) && !(0x27FFFF < s0);
                if (brake) g.W32(car + 484, 0xFFFD999Au);           // 0x8009A490: -2.4 near the finish
            }
        }
    }
    // 0x8009A494: the speed
    const int32_t acc = g.S32(car + 484);
    const int32_t v1 = Add(g.S32(car + 480), FixMul(acc, g.S32(F + 72)));
    g.W32(car + 480, U(v1));                                        // 0x8009A4B4
    if (v1 < 0) {
        g.W32(car + 484, 0);
        g.W32(car + 480, 0);
    } else if (s6 == 0 && (g.U8(car + 509) & 8u) && 0x8F0D8 < v1) {  // 0x8009A4CC
        g.W32(car + 480, 0x8F0D8);
        g.W32(car + 484, 0);
    } else {
        int32_t cap = 0x141DDD;                                     // 0x8009A4FC
        if (s6 != 0) {                                              // the catch-up cap
            const uint32_t gs = g.U32(kGameStatePtr);
            const int32_t a1 = g.S32(kTdCatchupTab + 4u * g.U32(gs + 60));
            const int32_t q = (a1 > 0) ? S(FixDiv(0x9C0000u, U(a1))) : S(0u - FixDiv(0x9C0000u, 0u - U(a1)));
            const uint32_t p1 = g.U32(kTdPlayer1Bike);
            const uint32_t cls1 = g.U8(g.U32(p1 + 1084) + 1) & 0xFu;
            const uint32_t b = g.U32(kPopPlayerBikes + (((cls1 ^ 2u) != 0) ? 4u : 0u));   // 0x8009A578
            if (b != 0) {
                const int32_t d = Sub(q, g.S32(b + 480));
                cap = (0xFFFF < d) ? d : 0x10000;
            }
        }
        if (!(g.S32(car + 480) < cap)) {                            // 0x8009A5A8
            g.W32(car + 480, U(cap));
            g.W32(car + 484, 0);
        }
    }
    // 0x8009A5C4: the road step
    const int32_t dt = g.S32(F + 72);
    const int32_t speed = g.S32(car + 480);
    const uint32_t s5 = g.U32(car + 340);
    const int32_t s1 = g.S32(car + 348);
    const uint16_t s8 = g.U16(car + 362);
    const int32_t step = FixMul(dt, speed);
    uint32_t s3 = 0;
    if (!RoadStep(g, car + 172, car + 328, step, F, s3)) return false;   // 0x8009A5EC
    if (s5 == s3) {                                                 // 0x8009A780: the same slice
        const int32_t dir = g.S32(car + 364);
        if (dir > 0 && !(step < Sub(g.S32(s5 + 32), s1))) return release();   // a dead end
        if (dir < 0 && !(step < s1)) return release();
    } else {                                                        // 0x8009A600: a new slice's rows
        const int32_t dot = DotLcmView(g, s3 + 14, car + 450, F);
        g.W16(car + 444, g.U16(s3 + 14));
        g.W16(car + 446, g.U16(s3 + 16));
        g.W16(car + 448, g.U16(s3 + 18));
        g.W16(car + 432, g.U16(s3 + 2));
        g.W16(car + 434, g.U16(s3 + 4));
        g.W16(car + 436, g.U16(s3 + 6));
        g.W16(car + 450, g.U16(s3 + 14));
        g.W16(car + 452, g.U16(s3 + 16));
        g.W16(car + 454, g.U16(s3 + 18));
        if (dot < 0) {                                              // 0x8009A674: against the heading
            s4 |= 2u;
            for (uint32_t o : {454u, 450u, 446u, 452u, 432u, 444u, 448u, 436u, 434u})
                g.W16(car + o, static_cast<uint16_t>(0u - g.U16(car + o)));
        }
        g.W16(car + 438, g.U16(s3 + 8));                            // 0x8009A6EC
        g.W16(car + 440, g.U16(s3 + 10));
        {
            const uint16_t v438 = g.U16(car + 438), v12 = g.U16(s3 + 12);
            g.W16(car + 438, static_cast<uint16_t>(0u - v438));
            const uint16_t v440 = g.U16(car + 440);
            g.W16(car + 442, v12);
            g.W16(car + 442, static_cast<uint16_t>(0u - v12));
            g.W16(car + 440, static_cast<uint16_t>(0u - v440));
        }
        const int32_t ang = RatAtan2View(g, Promote(g.S16(car + 444)), Promote(g.S16(car + 448)));   // 0x8009A724
        g.W32(car + 292, U(ang));
        const int16_t sn = g.S16(kTdSinCos + ((U(ang) & 0xFFFu) << 2) + 2u);
        const uint32_t a2 = g.U32(car + 292);
        g.W32(car + 296, SX(sn) << 4);                              // 0x8009A768
        const int16_t cs = g.S16(kTdSinCos + ((a2 & 0xFFFu) << 2));
        g.W32(car + 300, SX(cs) << 4);                              // 0x8009A77C
    }
    // 0x8009A7B8
    int32_t still = 0;
    if (!CarCheck(g, car, F, pc, still)) return false;
    if (still == 0) return true;
    RoadPosition(g, car + 450, car + 328, car + 360, F);            // 0x8009A7D0
    const int32_t zone = NodeZone(g, car, F, pc.Road());            // 0x8009A7D8
    RoadClass(g, car, 0, 0, zone, F, pc.Road());                    // 0x8009A7F0
    RoadsideRun(g, car, 0, zone, F, pc.Road());                     // 0x8009A800
    RoadsideZones(g, car, F);                                       // 0x8009A808
    RouteBind(g, h, 1, 0, F, pc.Road());                            // 0x8009A81C
    const int32_t prog = ProgressScalar(g, h, F);                   // 0x8009A824
    const int32_t inSlice = g.S32(car + 348);
    g.W32(car + 324, U(prog));                                      // 0x8009A840
    MulAddView(g, s3 + 20, s3 + 14, inSlice, F + 16);               // 0x8009A83C: the slice point
    // 0x8009A844: the lane's lateral - the abs() macro over three calls
    int32_t la = 0, lb = 0, lc = 0;
    if (!LaneLateral(g, car, F, pc, la)) return false;
    if (!LaneLateral(g, car, F, pc, lb)) return false;
    if (!LaneLateral(g, car, F, pc, lc)) return false;
    int32_t target = S((U(la >> 31) + U(lb)) ^ U(lc >> 31));
    if (g.S8(car + 508) < 0) target = Neg(target);                  // 0x8009A880
    int32_t cur = Abs(g.S32(car + 344));                            // 0x8009A894
    if (target != cur) {                                            // 0x8009A898: steer toward it
        const int32_t spd = g.S32(car + 480);
        int32_t q;
        if ((g.U8(car + 509) & 0x10u) && !(spd < 132)) q = S(FixDiv(U(spd), 0x50000u));   // 0x8009A8D0
        else if (spd > 0) q = S(FixDiv(U(spd), 0xA0000u));          // 0x8009A8FC
        else q = Neg(S(FixDiv(0u - U(spd), 0xA0000u)));             // 0x8009A910
        const int32_t mv0 = FixMul(g.S32(F + 72), q);               // 0x8009A920
        const int32_t diff = Abs(Sub(target, Abs(cur)));
        const int32_t mv = (mv0 < diff) ? mv0 : diff;
        cur = (cur < target) ? Add(cur, mv) : Sub(cur, mv);         // 0x8009A958
        s4 |= 2u;
        g.W32(car + 344, U(cur));                                   // 0x8009A980
        uint32_t big = 0;
        if (g.U8(car + 510) & 2u) big = (Abs(mv) < 4097) ? 0u : 1u; // 0x8009A984: still changing lane
        s4 |= big << 2;
    }
    {
        uint8_t f = g.U8(car + 510);                                // 0x8009A9A0
        if (!(s4 & 4u)) f = static_cast<uint8_t>(f & 0xFDu);
        g.W8(car + 510, f);
    }
    if ((s4 & 2u) || s8 != g.U16(car + 362)) {                      // 0x8009A9CC: the sign by direction
        const int32_t v = Abs(g.S32(car + 344));
        g.W32(car + 344, g.S32(car + 364) > 0 ? U(v) : 0u - U(v));
    }
    if (g.S8(car + 508) < 0) g.W32(car + 344, 0u - U(cur));         // 0x8009AA20
    MulAddView(g, F + 16, s3 + 2, g.S32(car + 344), car + 184);     // 0x8009AA30: the box
    int32_t warned = 0;
    if (!PassWarn(g, car, F, c, warned)) return false;              // 0x8009AA38
    s4 |= U(warned);
    uint32_t blk = 0;
    if (!Blocker(g, car, F, blk)) return false;                     // 0x8009AA44
    if (blk != 0 && (g.U8(car + 509) & 0x10u)) {                    // 0x8009AA58
        if (!(s4 & 1u) && !Horn(g, car, blk, F, c)) return false;   // 0x8009AA74
        if (BlockGiveUp(g, car, blk, F) != 0) return release();     // 0x8009AA80
    }
    const int32_t lim = S(0x141DDDu << (s6 & 31u)) >> 1;            // 0x8009AAB0
    if (g.S32(car + 480) < lim && !(g.U8(car + 509) & 0x11u)) g.W32(car + 484, 0x26666);   // 0x8009AAE0
    CarBrake(g, car, g.S32(F + 72), (s5 != s3) ? 1u : 0u, F);       // 0x8009AAF0
    g.W8(car + 509, static_cast<uint8_t>(g.U8(car + 509) & 0x7Fu)); // 0x8009AB08
    if (!pc.BuildObb(car)) return false;                            // 0x8009AB04
    LaneChange(g, car, g.S32(F + 72), F);                           // 0x8009AB10
    return true;
}

} // namespace

bool TrafficPass(GuestRam& g, int32_t dt, uint32_t sp, TrafficDriveCallees& c) {
    const uint32_t F = sp - 72u;                                    // `addiu sp,sp,-72`
    g.W32(F + 72, U(dt));                                           // 0x8009A2D0: the a0 home slot
    int32_t n = g.S32(g.U32(kTdPool3Table + 12));                   // 0x8009A2DC: the high index
    uint32_t car = g.U32(kTdPool3Table);
    for (; n >= 0; --n) {
        if (!CarFrame(g, car, F, c)) return false;
        if (g.Faulted()) return false;
        car += g.U32(kTdPool3Table + 4);                            // 0x8009AB20: the stride
    }
    return !g.Faulted();
}

} // namespace rr::sim
