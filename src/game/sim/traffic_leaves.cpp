#include "game/sim/traffic_leaves.h"

#include "game/sim/road_runtime.h"

// Every function below is transcribed from our own disassembly of SLUS_010.53 (SHA-1
// 67ed165a2c517d4e6106fb0dfa324d66dd9a76f1) and RASHCDG.BIN (SHA-1
// cfe43a7786759f2cb9c57751cf99e84d1074782c). Comments give the original's addresses. Loads that may
// fault are made in the original's order; arithmetic wraps in uint32_t wherever the original's does.

namespace rr::sim {

namespace {

inline uint32_t U(int32_t v) { return static_cast<uint32_t>(v); }
inline int32_t S(uint32_t v) { return static_cast<int32_t>(v); }
// The `sra / addu / xor` absolute value (INT_MIN stays INT_MIN).
inline int32_t Abs(int32_t v) {
    const uint32_t sg = U(v >> 31);
    return S((U(v) + sg) ^ sg);
}
// `mult a, b` then `(lo >> 16) | (hi << 16)`: the 64-bit product's bits 16..47.
inline uint32_t MulShr16(int32_t a, int32_t b) {
    const uint64_t p = static_cast<uint64_t>(static_cast<int64_t>(a) * static_cast<int64_t>(b));
    return static_cast<uint32_t>(p >> 16);
}

constexpr uint32_t kRouteRecordCountAddr = 0x800D6182; // s16, -1 = no route (kRouteTable + 0x12)

} // namespace

// ============================================================================ SLUS 0x80012C1C
void RoadWalk(GuestRam& g, uint32_t from, uint32_t out, int32_t dist, uint32_t sp) {
    g.W32(sp + 0, from);                                            // 0x80012C38: a0 to its home slot
    uint32_t pick = 0;                                              // s3 = 0 (0x80012C34)
    GuestCopyWords(g, out, from, 12);                               // 0x80012C60
    int32_t left = Abs(dist);                                       // s6 (0x80012C68)
    if (dist < 0) g.W32(out + 4, U(-g.S32(out + 4)));               // 0x80012C84
    uint32_t node = g.U16(out + 0);                                 // s7 = lhu (0x80012C88)
    while (left > 0) {                                              // 0x80012C8C / 0x80012F24
        const uint32_t key = g.U32(out + 0);                        // 0x80012C94
        if ((key >> 16) == 0) {                                     // a road
            const uint32_t rd = GraphRoad(g, key & 0xFFFFu);        // 0x80012CA8
            const uint32_t len = (g.U32(rd + 4) >> 6) << 16;        // a1
            const int32_t dir = g.S32(out + 4);
            const uint32_t along = g.U32(out + 8);
            g.W32(out + 8, dir > 0 ? along + U(left) : along - U(left));   // 0x80012CE0
            const int32_t v = g.S32(out + 8);                       // reloaded (0x80012CE4)
            if (v < 0) {                                            // past the start
                node = g.U32(rd + 8);                               // 0x80012D04
                left = -v;
            } else {
                left = S(U(v) - len);                               // 0x80012CF8 (delay slot)
                if (!(S(len) < v)) {                                // still on the road
                    left = 0;                                       // 0x80012F20
                    continue;
                }
                node = g.U32(rd + 12);                              // 0x80012D10: past the end
            }
            g.W32(out + 0, (node & 0xFFFFu) | 0x10000u);            // 0x80012D24
            g.W32(out + 8, 0);                                      // 0x80012D2C
            continue;
        }
        // At a node (any kind != 0): s7 is the node the last road ended on, or from's own low half.
        const uint32_t nd = GraphNode(g, node);                     // 0x80012D30
        if (nd == 0) {
            left = 0;
            continue;
        }
        const uint32_t degree = g.U32(nd + 4);
        if (degree < 2u) {                                          // 0x80012D4C (sltiu)
            left = 0;
            continue;
        }
        bool found = false;                                         // s5
        if (g.S16(kRouteRecordCountAddr) != -1) {                   // 0x80012D58
            if (dist > 0) {                                         // 0x80012D68
                // a0 = s5 = 0: the route record handed to 0x8003F408 is NULL, so this is 0.
                const uint32_t leg = RouteLegFor(g, 0, S(node));    // 0x80012D74
                if (leg != 0) {
                    uint32_t want;
                    if (g.S32(leg + 16) < 2) {                      // 0x80012D90 (slti)
                        want = g.U32(leg + 84);
                    } else {
                        const uint32_t r = GuestRand(g);            // 0x80012D9C
                        want = g.U32(leg + 84 + 4u * (r % g.U32(leg + 16)));
                    }
                    uint32_t k = 0;                                 // s0
                    const uint32_t n = g.U32(nd + 4);               // 0x80012DD8
                    for (; k < n; ++k) {
                        pick = nd + 8u + 8u * k;                    // s3 (0x80012DF0)
                        if (g.U32(pick) == want) {
                            found = true;                           // 0x80012DCC
                            break;
                        }
                    }
                    if (!(k < g.U32(nd + 4))) found = false;        // 0x80012E14 / 0x80012E28
                }
            } else {
                // 0x80012E30: an exit whose leg direction opposes the exit's - again with a NULL record.
                for (uint32_t k = 0; k < g.U32(nd + 4); ++k) {      // sltu, count reloaded (0x80012E94)
                    pick = nd + 8u + 8u * k;                        // 0x80012E3C
                    const uint32_t leg = RouteFindLegView(g, 0, g.U32(pick));   // 0x80012E44
                    if (leg == 0) continue;
                    const int32_t ld = g.S32(leg + 4);
                    if (ld < 0 && g.S32(pick + 4) > 0) {
                        found = true;
                        break;
                    }
                    if (ld > 0 && g.S32(pick + 4) < 0) {
                        found = true;
                        break;
                    }
                }
            }
        }
        if (!found) {                                               // 0x80012EA8
            const uint32_t r = GuestRand(g);                        // 0x80012EB0
            pick = nd + 8u * (r % g.U32(nd + 4)) + 8u;              // divu / mfhi
        }
        g.W32(out + 0, g.U16(pick));                                // 0x80012EE0: lhu, the exit's road
        const int32_t edir = g.S32(pick + 4);
        g.W32(out + 4, U(edir));                                    // 0x80012EF0
        if (edir > 0) {
            g.W32(out + 8, 0);                                      // 0x80012EF8
        } else {
            const uint32_t rd = GraphRoad(g, g.U16(out + 0));       // 0x80012F00
            g.W32(out + 8, (g.U32(rd + 4) >> 6) << 16);             // 0x80012F1C
        }
    }
    g.W32(out + 4, 1);                                              // 0x80012F2C
    if (g.U32(out + 0) == g.U32(from + 0)) g.W32(out + 4, g.U32(from + 4));   // 0x80012F50
    if (g.S16(kRouteRecordCountAddr) != -1) {                       // 0x80012F58
        const uint32_t key = g.U32(out + 0);
        if ((key >> 16) == 0) {
            const uint32_t leg = RouteFindLegView(g, 0, key & 0xFFFFu);   // 0x80012F7C, a0 = 0 again
            if (leg != 0) g.W32(out + 4, g.U32(leg + 4));           // 0x80012F94
        }
    }
}

// ============================================================================ RASHCDG 0x8008CDF4
uint32_t Budget(GuestRam& g, uint32_t kind) {
    switch (kind) {                                                 // sltiu 7, table 0x8005B680
        case 0:                                                     // 0x8008CE1C
            return (g.S8(0x8005B2B9) < g.S8(0x8005B2B8)) ? 1u : 0u;
        case 2:                                                     // 0x8008CE34
            if (g.U32(0x800D4B80) == 0) return 0;
            if (!(g.S32(0x800D4B74) < 4)) return 0;
            if (!(g.S32(0x800D4B70) < g.S32(0x800D8744))) return 0;
            return (0 < g.S32(0x800D4C7C)) ? 1u : 0u;
        case 3:                                                     // 0x8008CE8C: pool 3
            if (!(g.S32(0x800CF654) < 16)) return 0;
            return (0 < g.S32(0x800D4C6C)) ? 1u : 0u;
        case 4:                                                     // 0x8008CEBC
        case 5: {                                                   // 0x8008CF10
            const uint32_t b = (kind == 4) ? 0x800CD6C8u : 0x800CE598u;
            const int32_t used = g.S32(b + 4), cap = g.S32(b + 8);
            if (!(used < cap)) {
                const uint32_t load = g.U32(0x800D1814);
                if (load < (kind == 4 ? 596u : 452u)) return 0;     // 0x8008CEE8 / 0x8008CF3C
            }
            return (g.S32(0x800D4C9C) > 0) ? 1u : 0u;               // 0x8008CEF4
        }
        case 6:                                                     // 0x8008CF40
            if (g.U32(0x800CD6C4) == 0) return 0;
            return (g.S32(0x800CD6AC) < g.S32(0x8005B214)) ? 1u : 0u;
        default:                                                    // 1 and >= 7: 0x8008CF6C
            return 0;
    }
}

// ============================================================================ RASHCDG 0x8009F578
uint32_t Spacing(GuestRam& g, uint32_t rec) {
    constexpr uint32_t kPool3 = 0x800CE500;                         // pool 3's control in the table
    int32_t best = 0x3E70000;                                       // t1 = 999.0
    uint32_t near = 0;                                              // t0
    int32_t i = g.S32(g.U32(kPool3 + 12));                          // a3 = *(high index pointer)
    uint32_t car = g.U32(kPool3);                                   // a1 = base
    for (; i >= 0; --i) {                                           // 0x8009F594 / 0x8009F630
        if (g.U16(car + 0xAC) != 0 && g.U32(car + 360) == g.U32(rec + 8)) {   // 0x8009F5A0 / 0x8009F5BC
            const int32_t d = Abs(S(g.U32(car + 368) - g.U32(rec + 36)));     // 0x8009F5D0
            if (!(0xA0000 < d)) return 1;                           // within 10.0 (0x8009F650)
            if ((g.U8(car + 509) & 0x11u) != 0) {                   // 0x8009F5EC
                const int32_t rdir = g.S16(rec + 60);
                if (S(g.U32(car + 364) ^ U(rdir)) >= 0 && d < best) {   // same direction, nearer
                    best = d;                                       // 0x8009F620
                    near = car;
                }
            }
        }
        car += g.U32(kPool3 + 4);                                   // stride, reloaded (0x8009F628)
    }
    if (near == 0) return 0;                                        // 0x8009F638
    const uint32_t xs = g.U32(near + 372);
    if (xs == 0) return 1;                                          // 0x8009F648
    const int32_t dir = g.S32(near + 364);                          // 0x8009F658
    const int32_t width = g.S32(xs + 4);                            // a3
    const int32_t lanes = dir > 0 ? g.S16(near + 420) : g.S16(near + 408);
    if (lanes == 1) return 1;                                       // 0x8009F67C
    const int32_t lane = g.S8(near + 508);                          // a1 (lb)
    int32_t nl;
    if (lanes == 2) {
        if (lane == 2) nl = (Abs(g.S32(near + 344)) < width) ? 3 : 1;   // 0x8009F698
        else nl = (lane == 3) ? 1 : 3;                              // 0x8009F6C0
    } else {
        nl = lane + 1;                                              // 0x8009F6D4
        if (!(nl < 4)) nl = 1;
    }
    g.W16(rec + 64, static_cast<uint16_t>(nl));                     // 0x8009F6E8
    return 2;
}

// ============================================================================ RASHCDG 0x8009FF24
void ShareFlags(GuestRam& g, uint32_t flags, uint32_t sp) {
    const uint32_t F = sp - 32u;                                    // `addiu sp,sp,-32`
    const uint32_t gs = g.U32(0x8005B2F8);                          // 0x8009FF28
    if ((g.U8(gs + 4) & 0x10u) == 0) return;                        // 0x8009FF3C
    const uint32_t p1 = g.U32(0x8005B38C), p2 = g.U32(0x8005B21C);  // 0x8009FF4C
    int32_t a = g.S16(p1 + 186) - g.S16(p2 + 186);
    int32_t b = g.S16(p1 + 194) - g.S16(p2 + 194);
    a = Abs(a);
    b = Abs(b);
    int32_t big = a, small = b;
    if (a < b) {                                                    // 0x8009FF84
        big = b;
        small = a;
    }
    const int32_t s = small + (small >> 1);
    if (!((big - (big >> 5) - (big >> 7) + (s >> 2) + (s >> 6)) < 260)) return;   // 0x8009FFC0
    uint32_t slot = F;                                              // t3
    for (int32_t p = 0; p < g.S32(gs + 48); ++p, slot += 4u) {      // 0x8009FFD4 / 0x800A00FC
        const uint32_t v = 0x800CD898u + 1132u * (U(p) ^ 1u);       // the OTHER player's view
        g.W32(slot, 0);                                             // 0x800A0018
        const uint32_t bike = g.U32(0x8005B268u + 4u * U(p));       // 0x800A001C
        const int32_t dx = S(g.U32(bike + 184) - g.U32(v + 184));
        g.W32(F + 8, U(dx));                                        // 0x800A0030
        const int32_t dy = S(g.U32(bike + 188) - g.U32(v + 188));
        g.W32(F + 12, U(dy));                                       // 0x800A0044
        const int32_t dz = S(g.U32(bike + 192) - g.U32(v + 192));
        g.W32(F + 16, U(dz));                                       // 0x800A0058
        const uint32_t tx = MulShr16(dx, S(U(g.S16(v + 444)) << 4));
        const uint32_t ty = MulShr16(dy, S(U(g.S16(v + 446)) << 4));
        const int32_t mz = S(U(g.S16(v + 448)) << 4);
        const uint64_t pz = static_cast<uint64_t>(static_cast<int64_t>(dz) * static_cast<int64_t>(mz));
        g.W32(F + 24, static_cast<uint32_t>(pz));                   // 0x800A00C8: lo
        g.W32(F + 28, static_cast<uint32_t>(pz >> 32));             // 0x800A00CC: hi
        const uint32_t sum = static_cast<uint32_t>(pz >> 16) + (ty + tx);
        if (S(sum) >= 0) g.W32(slot, 1);                            // 0x800A00EC: ahead of that view
    }
    const uint32_t r0 = g.U32(F + 0), r1 = g.U32(F + 4);            // 0x800A0108 (r1 stale for np < 2)
    if (r0 == r1) {
        const uint32_t gs2 = g.U32(0x8005B2F8);                     // 0x800A011C
        uint32_t f = flags;
        for (int32_t p = 0; p < g.S32(gs2 + 48); ++p, f += 4u)      // 0x800A012C / 0x800A0154
            if (g.U32(f) != 0) g.W32(f, U(p + 2));                  // 0x800A0148
    } else if (r0 != 0) {
        if (g.U32(flags + 0) != 0) g.W32(flags + 0, 2);             // 0x800A0180
        if (g.U32(flags + 4) != 0) g.W32(flags + 4, 3);             // 0x800A0198
    } else {
        if (g.U32(flags + 4) != 0) g.W32(flags + 4, 2);             // 0x800A01AC
        if (g.U32(flags + 0) != 0) g.W32(flags + 0, 3);             // 0x800A01C0
    }
}

// ============================================================================ SLUS 0x8003E1E8
uint32_t NodeLanes(GuestRam& g, uint32_t obj, int32_t road, uint32_t dirOut) {
    uint32_t node = 0;
    if (obj != 0 && g.S16(obj + 16) == 1 && g.U32(obj + 12) == 0)   // 0x8003E214..0x8003E234
        node = RoadNodeRecord(g, g.S32(obj + 0));                   // 0x8003E240
    if (node == 0) return 0;
    const uint32_t arm = RoadNodeArm(g, node, road);                // 0x8003E254
    if (arm == 0) return 0;
    const uint32_t piece = RoadFindPiece(g, obj, road);             // 0x8003E268
    if (piece == 0) return 0;
    const uint32_t sub = g.U32(obj + 48) + U(g.S16(piece + 20)) * 28u;   // 0x8003E290
    if (sub == 0) return 0;
    uint32_t off;
    if (g.S16(arm + 2) > 0) {                                       // 0x8003E2A4
        off = U(g.S16(sub + 18)) << 4;                              // the first slice
    } else {
        off = (U(g.S16(sub + 18) + g.S16(sub + 16)) << 4) - 16u;    // the last slice
    }
    const uint32_t sl = g.U32(obj + 56) + off;                      // 0x8003E2D8
    if (g.S16(sl + 4) < 0) return 0;                                // 0x8003E2E4
    g.W32(dirOut, U(g.S16(arm + 2)));                               // 0x8003E2F4
    return g.U32(obj + 60) + U(g.S16(sl + 4)) * 264u;               // 0x8003E310
}

// ============================================================================ SLUS 0x80012AEC
int32_t ModelExtent(GuestRam& g, uint32_t e, int32_t idx, uint32_t out) {
    const uint32_t off = U(idx) * 12u;                              // a3 (0x80012AFC)
    const uint32_t ent = off + g.U32(g.U32(e + 96) + 8);
    const uint32_t box = g.U32(ent + 8);                            // a1
    if (box == 0) return -1;                                        // 0x80012B0C
    for (uint32_t k = 0; k < 3; ++k) {                              // 0x80012B1C / 0x80012B3C / 0x80012B70
        const uint32_t head = g.U32(off + g.U32(g.U32(e + 96) + 8)); // the chain re-read per axis
        const uint32_t sh = (g.U16(head + 14) >> 12) & 31u;
        const int32_t v = g.S16(box + 24 + 2u * k);
        g.W32(out + 4u * k, U(v >> sh) << 10);                      // srav, sll 10
    }
    return 0;
}

// ============================================================================ SLUS 0x80012FC8
void CarSetup(GuestRam& g, uint32_t e, int32_t idx, uint32_t sp) {
    const uint32_t F = sp - 40u;                                    // `addiu sp,sp,-40`
    ModelExtent(g, e, idx, F + 16);                                 // 0x80012FD8; v0 not tested
    const uint32_t pool = static_cast<uint32_t>(g.U16(e + 0xAC)) >> 5;
    if (pool - 1u < 2u) {                                           // pools 1, 2 (0x80012FF0)
        g.W32(e + 312, g.U32(F + 16) << 1);                         // 0x80013008
        g.W32(e + 308, g.U32(F + 20));                              // 0x80013014
        g.W32(e + 304, g.U32(F + 24));                              // 0x80013020
        return;
    }
    g.W32(e + 304, g.U32(F + 16));                                  // 0x8001302C
    g.W32(e + 312, g.U32(F + 20) << 1);                             // 0x8001303C
    const uint32_t pool2 = static_cast<uint32_t>(g.U16(e + 0xAC)) >> 5;
    g.W32(e + 308, g.U32(F + 24));                                  // 0x80013050 (delay slot)
    if (pool2 != 0) return;                                         // 0x8001304C
    const uint32_t cls = g.U32(e + 180);                            // 0x80013054
    if (!(cls < 18u)) {                                             // class 18 and up: 11/16
        int32_t v = S(g.U32(e + 312) * 11u);                        // 0x80013070..0x8001307C
        if (v < 0) v += 15;
        g.W32(e + 312, U(v >> 4));                                  // 0x800130FC
        return;
    }
    int32_t k = 0;
    if (cls < 9u) {
        k = 1;
    } else {
        const uint32_t gs = g.U32(g.gp() + 1644);                   // 0x8001309C: game_state
        if (g.S32(gs + 60) == 2) k = 1;                             // the bank
    }
    int32_t v = S(U(k * 2 + 9) * g.U32(e + 312));                   // 0x800130C4: mult, lo
    if (v < 0) v += 7;
    const int32_t a = v >> 3;
    const uint32_t gs = g.U32(g.gp() + 1644);                       // 0x800130DC
    const uint32_t h = g.U16(e + 0xAC);
    g.W32(e + 312, U(a));                                           // 0x800130E4
    if (h < g.U32(gs + 48)) g.W32(e + 312, U(a) - 4096u);           // 0x800130FC: a player's bike
}

// ============================================================================ SLUS 0x8003C520
int32_t CursorRoadEnd(GuestRam& g, uint32_t cursor) {
    const uint32_t piece = g.U32(cursor + 4);                       // 0x8003C52C
    if (g.S16(piece + 2) == 1) return g.S32(g.U32(cursor + 8) + 12);   // 0x8003C54C: the sub-object's
    const uint32_t rd = GraphRoad(g, g.U32(piece + 12));            // 0x8003C55C
    if (rd == 0) return 0;
    return S((g.U32(rd + 4) >> 6) << 16);                           // 0x8003C578
}

} // namespace rr::sim
