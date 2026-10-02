#include "game/sim/population.h"

#include "game/sim/fixed.h"
#include "game/sim/integrator.h"

// Every function below is transcribed from our own disassembly of SLUS_010.53 (SHA-1
// 67ed165a2c517d4e6106fb0dfa324d66dd9a76f1) and RASHCDG.BIN (SHA-1
// cfe43a7786759f2cb9c57751cf99e84d1074782c). Comments give the original's addresses. Loads that may
// fault are made in the original's order; arithmetic wraps in uint32_t wherever the original's does.

namespace rr::sim {

namespace {

inline uint32_t U(int32_t v) { return static_cast<uint32_t>(v); }
inline int32_t S(uint32_t v) { return static_cast<int32_t>(v); }
inline uint32_t SX(int16_t v) { return U(static_cast<int32_t>(v)); }
// The `sra / addu / xor` absolute value.
inline int32_t Abs(int32_t v) {
    const uint32_t sg = U(v >> 31);
    return S((U(v) + sg) ^ sg);
}

// The engine's octagonal distance of the integer differences (SLUS 0x80013150..0x800131A8, and the
// same arithmetic inlined at 0x8003A0D8 and in RASHCDG).
int32_t Octagon(int32_t dx, int32_t dz) {
    dx = Abs(dx);
    dz = Abs(dz);
    int32_t big = dx, small = dz;
    if (dx < dz) {
        big = dz;
        small = dx;
    }
    const int32_t s = small + (small >> 1);
    return big - (big >> 5) - (big >> 7) + (s >> 2) + (s >> 6);
}

} // namespace

// ============================================================================ the window test

int32_t WindowPred(GuestRam& g, uint32_t kind, uint32_t pos, uint32_t p, uint32_t extra, uint32_t sticky) {
    const uint32_t v = kPopViewArray + kPopViewStride * p;          // 0x80013114 (1132 p, wrapping)
    const int32_t vx = g.S16(v + 0xBA);                             // 0x8001313C
    const int32_t px = g.S16(pos + 2);
    const int32_t vz = g.S16(v + 0xC2);
    const int32_t pz = g.S16(pos + 10);
    const int32_t d = Octagon(vx - px, vz - pz);                    // integer parts only
    if (kind == 6u) {                                               // 0x800131AC: collision volumes
        const uint32_t r = g.U32(g.gp() + kPopVolumeRadiusGp) + extra;
        return (d < (S(r) >> 16)) ? 1 : 0;
    }
    if (kind < 2u) return (d < (sticky != 0 ? 350 : 300)) ? 1 : 0;  // 0x800131F0 / 0x800131F8
    return (d < (sticky != 0 ? 230 : 200)) ? 1 : 0;                  // 0x800131E0 / 0x800131E8
}

int32_t PieceContains(GuestRam& g, uint32_t btt, uint32_t key) {
    if (key == 0) return 0;                                         // 0x80039D08
    if (btt == 0) return 0;
    const uint32_t obj = g.U32(btt + 12);                           // BTT_ +0x0C, the resident object
    if (obj == 0) return 0;
    const int16_t junction = g.S16(btt + 4);
    const uint32_t own = (SX(junction) << 16) | g.U16(btt + 16);    // 0x80039D34
    const int16_t hi = g.S16(key + 10);                             // the along's integer half
    if (junction == 0) {                                            // a road piece
        if (own != g.U32(key)) return 0;                            // 0x80039D48
        if (hi < g.S16(btt + 22)) return 0;
        if (g.S16(btt + 26) < hi) return 0;
        return 1;
    }
    const uint32_t k = g.U32(key);                                  // 0x80039D80
    if (own == k) return (g.U32(btt + 28) == 0) ? 1 : 0;            // the node's own key
    const uint32_t rec = RoadFindPiece(g, obj, S(k & 0xFFFFu));     // 0x80039D9C
    if (rec == 0) return 0;
    if (g.S16(rec + 2) != 0) return 0;                              // a core piece holds no road
    if (hi < g.S16(rec + 26)) return 0;
    return (g.S16(rec + 30) < hi) ? 0 : 1;
}

uint32_t RoadGate(GuestRam& g, uint32_t h, uint32_t key) {
    uint32_t own = 0xFFFFFFFFu;                                     // s3: the live entity's object id
    uint32_t ownObj = 0;                                            // s4: ... and the object
    if (h != 0 && g.S16(h + 148) != 0) {                            // 0x80039E34: live
        const uint32_t id = g.U32(g.U32(h + 156));                  // cursor object's id
        const uint32_t rec = RoadBttRecord(g, S(id));               // 0x80039E50
        if (rec == 0) return 0;
        const uint16_t hd = g.U16(h);
        const uint32_t gs = g.U32(kPopGameStatePtr);
        const int32_t np = g.S32(gs + 48);
        if ((hd >> 5) < 2 && static_cast<int32_t>(hd & 0x1Fu) < np) return g.U32(rec + 12);   // a player
        if (PieceContains(g, rec, key) != 0) return g.U32(rec + 12);                           // 0x80039E90
        ownObj = g.U32(rec + 12);                                   // 0x80039EB0
        own = g.U32(g.U32(h + 156));
    }
    if (g.S32(kPopPieceLast) < 0) return 0;                         // 0x80039EC4
    uint32_t entry = kPopPieceList;
    for (int32_t i = 0;; entry += 16u) {                            // 0x80039ED8
        const uint32_t id = g.U32(entry);
        if (id != 0xFFFFFFFFu && own != id) {
            const uint32_t rec = RoadBttRecord(g, S(id));
            if (rec != 0 && PieceContains(g, rec, key) != 0)        // 0x80039F04
                return (own == 0xFFFFFFFFu) ? g.U32(rec + 12) : ownObj;
        }
        if (g.Faulted()) return 0;
        ++i;
        if (g.S32(kPopPieceLast) < i) return 0;                     // re-read per entry
    }
}

uint32_t SeatAlong(GuestRam& g, uint32_t cur, int32_t along) {
    if (cur == 0) return 0;                                         // 0x80037528
    const uint32_t sub = g.U32(cur + 8);
    const uint32_t obj = g.U32(cur + 0);
    const uint32_t piece = g.U32(cur + 4);
    const int32_t first = g.S16(sub + 8);
    const int32_t count = g.S16(sub + 10);
    const uint32_t dist = g.U32(obj + 0x44);                        // t4, the DIST table
    const uint32_t slices = g.U32(obj + 0x34);
    const uint32_t last = slices + U(first + count) * 52u - 52u;    // t3, the sub-object's last slice
    const uint32_t slice = g.U32(cur + 12);
    const int16_t nDist = g.S16(obj + 0x1C);
    const uint32_t cum = g.U32(slice + 40);                         // the running distance +0x28
    const uint32_t a = (along > 0) ? cum + U(along) : cum - U(along);   // 0x80037580
    if (g.S16(piece + 2) != 0) return 0;                            // 0x800375A8: a core piece
    if (nDist <= 0) return 0;
    if (dist == 0) return 0;
    const int32_t hi = S(a) >> 16;
    const int32_t start = g.S32(piece + 24);
    if (hi < (start >> 16) || g.S16(piece + 30) < hi) {             // 0x800375D0 / 0x800375E4
        if ((S(a) >> 16) < g.S16(piece + 30)) {                     // 0x80037708: before the piece
            const int32_t f = g.S16(sub + 8);                       // 0x8003772C
            const uint32_t base = g.U32(obj + 0x34);
            g.W32(cur + 20, 0);
            const uint32_t s = base + U(f) * 52u;
            g.W32(cur + 12, s);
            return s;
        }
        g.W32(cur + 12, last);                                      // 0x8003771C: past it
        g.W32(cur + 20, g.U32(last + 32));
        return 0;
    }
    const int32_t d = S(a - U(start));                              // 0x800375E8
    const int32_t q = static_cast<int32_t>((static_cast<int64_t>(d >> 16) * 0x51EB851FLL) >> 32);
    int32_t idx = ((q >> 4) - (d >> 31)) - 1;                       // (d >> 16) / 50 - 1, truncated
    const int32_t lastIdx = g.S16(sub + 22) - 1;
    if (lastIdx < idx) idx = lastIdx;
    uint32_t s;
    if (idx < 0) {                                                  // 0x80037670
        const int32_t f = g.S16(sub + 8);
        s = g.U32(obj + 0x34) + U(f) * 52u;
    } else {                                                        // 0x80037630
        const int32_t base = g.S16(sub + 20);
        const uint32_t de = dist + U(base + idx) * 4u;
        const int32_t f = g.S16(sub + 8);
        const int32_t off = g.S16(de + 2);
        s = g.U32(obj + 0x34) + U(f + off) * 52u;
    }
    const int16_t cnt = g.S16(sub + 10);                            // 0x80037698
    if (g.S16(s) < cnt) {
        for (;;) {                                                  // 0x800376B4
            const uint32_t sc = g.U32(s + 40);
            const int32_t chord = g.S32(s + 32);
            if (!(chord < S(a - sc))) break;
            s += 52u;
            if (g.Faulted()) return 0;
            if (!(g.S16(s) < cnt)) break;
        }
    }
    if (last < s) {                                                 // 0x800376E8 -> 0x8003771C
        g.W32(cur + 12, last);
        g.W32(cur + 20, g.U32(last + 32));
        return s;
    }
    g.W32(cur + 12, s);                                             // 0x800376F0
    g.W32(cur + 20, a - g.U32(s + 40));
    return s;
}

int32_t CursorSeat(GuestRam& g, uint32_t obj, uint32_t key, uint32_t cur, uint32_t sp) {
    const uint32_t F = sp - 48u;                                    // `addiu sp,sp,-48`
    if (obj == 0) return 0;                                         // 0x8003A728
    const uint32_t k = g.U32(key);
    const uint32_t kind = k >> 16;
    if (kind == 0) {                                                // on a road
        uint32_t piece;
        if (g.S16(obj + 16) == 0) piece = g.U32(obj + 0x2C);       // a road piece: its GRPT
        else piece = RoadFindPiece(g, obj, S(k & 0xFFFFu));        // 0x8003A760: the junction's arm
        if (piece == 0) return 0;
        const int32_t si = g.S16(piece + 20);                       // 0x8003A774
        const uint32_t subs = g.U32(obj + 0x30);
        g.W32(cur + 0, obj);
        g.W32(cur + 4, piece);
        const uint32_t sub = subs + U(si) * 28u;
        g.W32(cur + 8, sub);
        const int32_t first = g.S16(sub + 8);
        const uint32_t slices = g.U32(obj + 0x34);
        g.W32(cur + 16, 0);
        g.W32(cur + 20, 0);
        g.W32(cur + 24, 0);
        g.W32(cur + 28, 0);
        g.W32(cur + 12, slices + U(first) * 52u);
        const uint32_t along = g.U32(key + 8) - g.U32(piece + 24);  // 0x8003A7CC
        SeatAlong(g, cur, S(along));
        return 1;
    }
    if (SX(g.S16(obj + 16)) != kind) return 0;                      // 0x8003A7E8: a node key
    const uint32_t node = g.U32(obj + 8);
    if (node != (k & 0xFFFFu)) return 0;
    if (g.U32(obj + 12) != 0) return 0;                             // the junction's core object only
    const uint32_t piece = g.U32(obj + 0x2C);                       // 0x8003A818
    uint32_t sub = g.U32(obj + 0x30);                               // s3: the first SUBT, unless ...
    const uint32_t leg = RouteLegOfRoad(g, S(node));                // 0x8003A820
    if (leg == 0) return 0;
    if (g.S32(leg + 16) > 0) {                                      // ... the route turns here
        g.W32(cur + 0, obj);
        g.W32(cur + 4, piece);
        const int32_t len = g.S32(leg + 8);
        const int32_t road = g.S32(leg + 84);
        sub = TurnSubObject(g, cur, len, road, F + 16);             // 0x8003A854, dir to sp+16
    }
    if (sub == 0) return 0;
    g.W32(cur + 0, obj);                                            // 0x8003A868
    g.W32(cur + 4, piece);
    g.W32(cur + 8, sub);
    const int32_t first = g.S16(sub + 8);
    g.W32(cur + 12, g.U32(obj + 0x34) + U(first) * 52u);
    const uint32_t ipt = RoadJunctionIndex(g, g.S32(obj + 0));      // 0x8003A8A0
    if (ipt != 0) {
        uint32_t turn = 0;                                          // s4
        const int32_t cnt = g.S16(ipt + 8);
        if (0 < cnt) {
            const int32_t base = g.S16(ipt + 6);
            const uint32_t G = g.U32(kRoadGraphPtr);
            const uint32_t subs = g.U32(obj + 0x30);
            const uint32_t pdt = g.U32(G + 0x34);
            const uint32_t gpdt = g.U32(G + 0x38);
            for (int32_t i = 0; i < cnt; ++i) {                     // 0x8003A8E8
                turn = pdt + U(base + i) * 12u;
                const uint32_t gr = gpdt + SX(g.S16(turn + 2)) * 12u;
                const int16_t off = g.S16(gr + 4);
                const int16_t live = g.S16(gr + 6);
                if (live != 0 && subs + SX(off) * 28u == sub) break;
            }
        }
        g.W32(cur + 24, ipt);                                       // 0x8003A944
        g.W32(cur + 28, turn);
    } else {
        g.W32(cur + 24, 0);                                         // 0x8003A950
        g.W32(cur + 28, 0);
    }
    g.W32(cur + 20, 0);                                             // 0x8003A95C
    g.W32(cur + 16, 0);
    return 1;
}

int32_t CursorReseat(GuestRam& g, uint32_t key, uint32_t cur) {
    const uint32_t obj = RoadGate(g, 0, key);                       // 0x8003A494
    if (obj == 0) return 0;
    uint32_t piece = 0;                                             // s0
    if (g.S16(obj + 16) == 0) {
        piece = g.U32(obj + 0x2C);                                  // 0x8003A4B8
    } else {
        const int32_t n = g.S16(obj + 18);                          // 0x8003A4C4
        if (0 < n) {
            const uint32_t k = g.U32(key);
            const uint32_t recs = g.U32(obj + 0x2C);
            const uint32_t kind = k >> 16, road = k & 0xFFFFu;
            for (int32_t i = 0; i < n; ++i) {                       // 0x8003A4F0
                piece = recs + U(i) * 32u;
                if (SX(g.S16(piece + 2)) == kind && g.U32(piece + 12) == road) break;
            }
        }
        if (g.S16(piece + 2) != 0) return 0;                        // 0x8003A524 (with no record: address 2)
    }
    const int32_t si = g.S16(piece + 20);                           // 0x8003A534
    const uint32_t sub = g.U32(obj + 0x30) + U(si) * 28u;
    const int32_t first = g.S16(sub + 8);
    const uint32_t slice = g.U32(obj + 0x34) + U(first) * 52u;
    g.W32(cur + 0, obj);                                            // 0x8003A574
    g.W32(cur + 4, piece);
    g.W32(cur + 8, sub);
    g.W32(cur + 12, slice);
    g.W32(cur + 20, 0);
    g.W32(cur + 16, 0);
    g.W32(cur + 24, 0);
    g.W32(cur + 28, 0);
    int32_t a = S(g.U32(key + 8) - g.U32(piece + 24));              // 0x8003A594
    if (a < 0) a = 0;
    const int32_t top = g.S32(piece + 28);
    if (top < a) a = top;
    SeatAlong(g, cur, a);                                           // 0x8003A5C8
    return 1;
}

uint32_t RoadWindow(GuestRam& g, uint32_t h, uint32_t sp) {
    const uint32_t F = sp - 56u;                                    // `addiu sp,sp,-56`
    const uint32_t key = h + 188;                                   // e + 0x168
    uint32_t s3 = 0;
    const uint32_t obj = RoadGate(g, h, key);                       // 0x80039F94
    if (obj == 0) return 0;
    const uint16_t hd0 = g.U16(h);
    uint32_t s4 = SX(g.S16(h + 148));                               // the live word
    if ((hd0 >> 5) < 2 && s4 == 0) {                                // a dormant bike or rider
        if (CursorSeat(g, obj, key, h + 156, F) == 0) return 0;     // 0x80039FC8
        const uint32_t box = h + 12;                                // e + 0xB8
        const uint32_t sl = g.U32(h + 168);
        MulAddView(g, sl + 20, sl + 14, g.S32(h + 176), box);       // 0x80039FF0
        MulAddView(g, box, sl + 2, g.S32(h + 172), box);            // 0x8003A004
    }
    const uint16_t hd = g.U16(h);                                   // 0x8003A00C
    uint32_t gs = g.U32(kPopGameStatePtr);
    const int32_t np0 = g.S32(gs + 48);
    if ((hd >> 5) < 2 && static_cast<int32_t>(hd & 0x1Fu) < np0) { // a player's own bike or rider
        if ((hd >> 5) != 0) return 1;                               // 0x8003A0AC: his rider
        const uint32_t slot = g.U16(h);
        uint32_t b = g.U32(kPopPool0Ptr) + 1096u * slot;
        if (b != 0) {
            if (g.U32(b + 1088) == 0) b = g.U32(b + 856);           // no list node: the partner
            if (b != 0 && g.U32(g.U32(b + 852) + 604) < 3u) s4 = 1; // rider seated
        }
        if (s4 != 0) return s4;                                     // 0x8003A0B0
    }
    // 0x8003A0BC..0x8003A164: the octagonal distance to every player's view, clamped at 999 - never
    // used. Its loads are made (a player count that walks the view array off RAM faults here, as the
    // console would); it runs at least once.
    (void)g.S16(h + 14);
    (void)g.S16(h + 22);
    uint32_t npU;
    {
        uint32_t t1 = 0, v = kPopViewArray;
        do {
            (void)g.S16(v + 0xBA);
            (void)g.S16(v + 0xC2);
            gs = g.U32(kPopGameStatePtr);
            npU = g.U32(gs + 48);
            ++t1;
            v += kPopViewStride;
            if (g.Faulted()) return 0;
        } while (t1 < npU);
    }
    if (s4 != 0) {                                                  // 0x8003A168: live - the sticky radii
        if (S(npU) > 0) {
            for (int32_t p = 0;;) {
                const uint32_t kind = g.U16(h) >> 5;
                g.W32(F + 16, 1);                                   // the fifth argument
                s3 |= U(WindowPred(g, kind, h + 12, U(p), 0x140000u, 1));
                gs = g.U32(kPopGameStatePtr);
                ++p;
                if (!(p < g.S32(gs + 48))) break;
            }
        }
        s4 = s3;
    }
    if (g.S16(h + 148) == 0) {                                      // 0x8003A1BC: dormant - the entry radii
        gs = g.U32(kPopGameStatePtr);
        if (g.S32(gs + 48) > 0) {
            for (int32_t p = 0;;) {
                const uint32_t kind = g.U16(h) >> 5;
                g.W32(F + 16, 0);
                s3 |= U(WindowPred(g, kind, h + 12, U(p), 0, 0));
                gs = g.U32(kPopGameStatePtr);
                ++p;
                if (!(p < g.S32(gs + 48))) break;
            }
        }
        s4 = s3;
    }
    if (s4 == 0) return 0;                                          // 0x8003A228
    if ((g.U16(h) >> 5) != 0) return s4;
    // A live pool-0 bike: the cop range bit.
    const uint32_t slot = g.U16(h);                                 // 0x8003A244
    const uint32_t gsp = g.U32(kPopGameStatePtr);
    const uint32_t b = g.U32(kPopPool0Ptr) + 1096u * slot;
    const uint32_t np = g.U32(gsp + 48);
    if (g.U16(b + 172) < np) return s4;                             // a player
    if ((g.U8(g.U32(b + 1084) + 1) & 0xFu) != 2u) return s4;        // not a cop
    if (S(np) > 0) {
        uint32_t out = F + 24, pp = kPopPlayerBikes;
        for (int32_t p = 0;;) {                                     // 0x8003A2B8
            const uint32_t pb = g.U32(pp);
            pp += 4;
            const int32_t mine = g.S32(b + 324);
            const int32_t theirs = g.S32(pb + 324);
            ++p;
            int32_t d = S(U(mine) - U(theirs)) >> 12;
            if (d < 0) d = S(0u - U(d));
            g.W32(out, U(d));                                       // far[p] in the frame
            out += 4;
            if (!(p < g.S32(gsp + 48))) break;
        }
    }
    gs = g.U32(kPopGameStatePtr);                                   // 0x8003A2F8
    bool near;
    if (g.S32(gs + 48) == 2) {
        if (g.S32(F + 24) < 201) return s4;
        near = g.S32(F + 28) < 201;
    } else {
        near = g.S32(F + 24) < 201;
    }
    if (near) return s4;
    g.W8(b + 928, static_cast<uint8_t>(g.U8(b + 928) | 0x20u));    // 0x8003A354: "cop left behind"
    return s4;
}

// ============================================================================ three SLUS leaves

void Attach(GuestRam& g, uint32_t b, uint32_t r, int32_t kind, int32_t seat) {
    g.W8(r + 72, 1);                                                // 0x80012844
    const uint32_t at = b + (U(seat) << 3);
    g.W32(at + 56, r);
    g.W32(at + 60, U(kind));
    g.W32(r + 52, b);                                               // 0x80012854
}

uint32_t ViewSlot(GuestRam& g, uint32_t desc, uint32_t r) {
    const uint32_t used = g.U32(desc + 8);                          // 0x80012884
    const uint32_t cap = g.U32(desc + 12);
    if (used == cap) return 0;                                      // none free
    int32_t i = 0;
    const int32_t n = S(cap);
    if (n > 0) {
        uint32_t o = g.U32(desc + 0);
        for (; i < n; ++i, o += 2108u) {                            // 0x800128A8: the first free object
            if (g.U32(o + 36) == 0) break;
            if (g.Faulted()) return 0;
        }
    }
    const uint32_t obj = g.U32(desc + 0) + 2108u * U(i);            // 0x800128D0
    const uint32_t flags = g.U32(obj + 36);
    g.W32(obj + 0, r);
    g.W32(obj + 1760, 0xFFFFFFFFu);
    g.W32(obj + 36, flags | 5u);
    const uint32_t kind = (g.U16(g.U32(r + 0) + 14) & 0x78u) >> 3;  // 0x80012900
    if (kind == 1 || kind == 4) {
        const uint32_t v = g.U32(0x80052390u + ((kind == 4) ? 4u : 0u));
        const uint32_t f2 = g.U32(obj + 36);
        g.W32(obj + 1760, v);
        g.W32(obj + 36, (f2 & 0xFFFFFFFBu) | 4u);
    }
    g.W8(g.U32(obj + 4) + 1, 5);                                    // 0x8001296C
    g.W32(desc + 8, g.U32(desc + 8) + 1u);
    return obj;
}

void CopDrop(GuestRam& g, uint32_t e) {
    g.W32(e + 36, g.U32(e + 36) & 0xF7FFFFFFu & 0xDFFFFFFFu & 0xEFFFFFFFu);   // 0x80028238
}

// ============================================================================ bikes: the activation pass

int32_t CopCount(GuestRam& g, int32_t d) {
    uint32_t v = g.U32(kPopCopsOut);                                // 0x8009DAF8
    v = (d > 0) ? v + 1u : v - 1u;
    g.W32(kPopCopsOut, v);
    const int32_t cap = g.S32(kPopCopsOut + 4);
    const int32_t now = g.S32(kPopCopsOut);
    int32_t lo = cap;
    if (now < cap) lo = now;                                        // 0x8009DB30
    g.W32(kPopCopsOut, U(lo));                                      // 0x8009DB48
    const int32_t r = (lo < 0) ? 0 : lo;
    g.W32(kPopCopsOut, U(r));                                       // 0x8009DB54
    return r;
}

namespace {

inline bool IsPlayer(GuestRam& g, uint32_t e) {
    const uint32_t gs = g.U32(kPopGameStatePtr);
    const uint16_t hd = g.U16(e + 172);
    return hd < g.U32(gs + 48);                                     // `sltu`: the handle below the count
}
inline uint32_t RiderClass(GuestRam& g, uint32_t e) {
    return g.U8(g.U32(e + 1084) + 1) & 0xFu;                        // riderDef +1, low nibble
}

// SLUS 0x80020018 RatAtan2 over the view: its table read out of guest memory (fixed.h).
int32_t RatAtan2View(GuestRam& g, int32_t y, int32_t x) {
    int32_t table[20];
    for (uint32_t i = 0; i < 20; ++i) table[i] = g.S32(0x8005285Cu + 4u * i);
    return RatAtan2(y, x, table);
}

// SLUS 0x8003FA40 MulMatrix0(a, b, out) over the view (integrator.h): `a` is loaded into the GTE
// before anything is written, the columns of `b` one at a time.
void MulMatrix0View(GuestRam& g, uint32_t a, uint32_t b, uint32_t out) {
    int16_t ma[9], mb[9], mo[9];
    for (uint32_t i = 0; i < 9; ++i) ma[i] = g.S16(a + 2u * i);
    for (uint32_t i = 0; i < 9; ++i) mb[i] = g.S16(b + 2u * i);
    MulMatrix0(ma, mb, mo);
    for (uint32_t i = 0; i < 9; ++i) g.W16(out + 2u * i, static_cast<uint16_t>(mo[i]));
}

void NegH(GuestRam& g, uint32_t a) { g.W16(a, static_cast<uint16_t>(0u - g.U16(a))); }

// e[+0x351] = (+0x1E0 > 10.0) ? stats[+0x1BC] - 1 : 0 (0x80094CF8 / 0x80094E1C).
uint8_t GearByte(GuestRam& g, uint32_t e, int32_t speed) {
    if (0xA0000 < speed) return static_cast<uint8_t>(g.U8(g.U32(e + 556) + 444) - 1u);
    return 0;
}

} // namespace

bool Retire(GuestRam& g, uint32_t e, uint32_t sp, PopulationCallees& c) {
    const uint32_t F = sp - 24u;                                    // `addiu sp,sp,-24`
    if (e == 0) return true;                                        // 0x80093FF0
    if (g.S16(e + 320) != 0) return true;
    bool body = IsPlayer(g, e);                                     // 0x80094018
    if (!body) {
        const uint32_t gs = g.U32(kPopGameStatePtr);
        body = !((g.U8(gs + 57) - 1u) < 2u);                        // not in game_state +0x39 1..2
    }
    if (body) {
        if (!c.ResetBike(e, F)) return false;                       // 0x8009403C
        const uint32_t R = g.U32(e + 852);
        if (g.U32(R + 604) < 3u || g.S16(R + 320) == 0) {           // rider seated, or dormant
            g.W16(R + 320, 0);                                      // 0x80094070
            if (!c.Remount(e, 1, F)) return false;
        } else {
            g.W32(e + 568, g.U32(e + 568) | 0x100u);                // 0x80094094
        }
    }
    const uint32_t fc = g.U32(e + 568);                             // 0x8009409C
    const uint32_t slot = g.U32(e + 540);
    g.W32(e + 568, fc | 0x08000000u);                               // "re-file me"
    if (slot != 0) {                                                // free the view slot
        g.W32(slot + 36, 0);
        g.W32(0x800CE178u, g.U32(0x800CE178u) - 1u);
        g.W32(e + 540, 0);
    }
    if (IsPlayer(g, e)) return true;                                // 0x800940E8
    if (RiderClass(g, e) != 2u) return true;
    const uint8_t f = g.U8(e + 928);
    if (f & 0x10u) {                                                // a cop in play
        g.W8(e + 928, static_cast<uint8_t>(f & 0xEFu));
        g.W32(e + 324, g.U32(0x8005B244u) << 12);                   // 0x80094138, before the call
        CopCount(g, -1);
        if ((g.U32(e + 36) >> 27) & 1u) {
            if (!c.CopLeave(e, F)) return false;                    // 0x80094154
            CopDrop(g, e);                                          // 0x8009415C
        }
    }
    g.W8(e + 928, static_cast<uint8_t>(g.U8(e + 928) & 0xDFu));     // 0x80094170
    return true;
}

bool Placement(GuestRam& g, uint32_t e, uint32_t sp, PopulationCallees& c) {
    const uint32_t F = sp - 88u;                                    // `addiu sp,sp,-88`
    int32_t s6 = FixMul(0x3C0000, 11);                              // 0x8009435C
    uint32_t s2 = 0, s4 = 0, s0 = 0;
    if (e == 0) return true;
    uint32_t gs = g.U32(kPopGameStatePtr);
    if (g.U8(gs + 4) == 44 && g.U8(gs + 57) == 0) g.W16(e + 320, 0); // 0x800943A0
    if (g.S16(e + 320) == 0) return true;
    uint32_t s3 = 0;
    bool chosen = false;
    gs = g.U32(kPopGameStatePtr);
    if ((g.U8(gs + 4) & 1u) && !(g.U16(e + 172) < g.U32(gs + 48)) &&
        g.U8(g.U32(e + 1084) + 39) == 254) {                         // 0x80094400
        s3 = 61;
        s6 = FixMul(0xA0000, 11);
        g.W32(e + 560, g.U32(e + 560) | 0x20000000u);
        chosen = true;
    }
    if (!chosen) {
        gs = g.U32(kPopGameStatePtr);                               // 0x80094428
        const uint8_t phase = g.U8(gs + 57);
        if (phase == 1) {
            const uint32_t p1 = g.U32(0x8005B38Cu);
            s3 = (g.U32(g.U32(p1 + 1084) + 40) != 0) ? 67u : 123u;
            s2 = g.U32(e + 344);                                    // 0x80094468
            s6 = g.S32(e + 292);
            g.W32(e + 924, 0);
            chosen = true;
        } else if (!(phase < 3) && !(g.U16(e + 172) < g.U32(gs + 48))) {
            const uint32_t p1 = g.U32(0x8005B38Cu);                 // 0x80094498
            const uint32_t rd = g.U32(e + 1084);
            const uint32_t p1rd = g.U32(p1 + 1084);
            if ((g.U8(rd + 1) & 0xFu) == (g.U8(p1rd + 1) & 0xFu)) {  // the same class as player 1
                s3 = 85;
                g.W8(rd, static_cast<uint8_t>(g.U8(rd) | 0x10u));   // 0x800944D0
                s6 = FixMul(0x9B0000, 11);
                chosen = true;
            }
        }
    }
    if (!chosen) {
        const uint32_t R = g.U32(e + 852);                          // 0x800944DC
        s3 = 1;
        if (!(g.U32(R + 604) < 3u) && g.S16(R + 320) != 0) s3 = 161;
        gs = g.U32(kPopGameStatePtr);                               // 0x80094510
        const uint32_t np = g.U32(gs + 48);
        if (!(g.U16(e + 172) < np)) {                               // not a player: the reference
            if (np == 2) {                                          // 0x80094534
                const uint32_t pb1 = g.U32(kPopPlayerBikes + 4);
                const uint32_t pb0 = g.U32(kPopPlayerBikes);
                const int32_t a = g.S32(pb1 + 324), b = g.S32(pb0 + 324);
                s4 = g.U32(kPopPlayerBikes + ((a < b) ? 4u : 0u));
            } else {
                s4 = g.U32(0x8005B38Cu);
            }
            const uint8_t refPlace = g.U8(g.U32(s4 + 1084) + 39);   // 0x80094570
            if (!(g.S32(0x8005B1F8u) < static_cast<int32_t>(refPlace))) {
                const uint32_t rd = g.U32(e + 1084);                // 0x80094590
                if (g.U32(rd + 40) != 0) {                          // this rider has finished
                    gs = g.U32(kPopGameStatePtr);
                    const uint8_t place = g.U8(rd + 39);
                    const uint32_t n = g.U32(gs + 48);
                    const int32_t lim = 8 >> ((n - 1u) & 31u);       // `srav`
                    if (lim < static_cast<int32_t>(place)) {
                        s3 = 0;                                     // 0x8009462C: not placed at all
                    } else {
                        const uint32_t fin = g.U32(0x800D6188u);    // 0x800945CC
                        const uint32_t along = g.U32(fin + 4);
                        const uint32_t rd2 = g.U32(e + 1084);
                        g.W32(e + 368, along);
                        const uint8_t pl = g.U8(rd2 + 39);
                        const int32_t lo = static_cast<int32_t>(pl >> 1) * (1 - static_cast<int32_t>((pl & 1u) << 1));
                        s3 |= 0x3Cu;
                        g.W32(e + 560, g.U32(e + 560) | 0x20000000u);
                        g.W32(e + 368, along + (U(lo * 3) << 16));  // 0x80094628
                    }
                }
            }
        }
    }
    // 0x80094630
    if (s3 != 0) s0 = U(CursorReseat(g, e + 360, e + 328));
    if (s0 == 0 || s3 == 0) {
        gs = g.U32(kPopGameStatePtr);                               // 0x80094FA4
        const uint16_t hd = g.U16(e + 172);
        g.W16(e + 320, 0);
        if (hd < g.U32(gs + 48)) return true;
    } else {
        const uint32_t piece = g.U32(e + 332);                      // 0x80094654
        const uint32_t route = g.U32(e + 428);
        const uint32_t road = g.U32(piece + 12);
        const uint32_t s5 = RouteFindLegView(g, route, road);       // the route leg of this road
        RoadClass(g, e, 1, 0, -1, F, c.Road());                     // 0x80094678
        if (s3 & 4u) {                                              // the road-edge lateral
            gs = g.U32(kPopGameStatePtr);
            uint32_t side;
            if (!(g.U8(gs + 57) < 3)) {
                side = ((static_cast<uint32_t>(g.U16(e + 172)) >> 1) - 1u) & 1u;
                if (side == 0) s6 = S(0u - U(s6));                  // 0x800946C8
            } else {
                side = 0;
                if (g.U32(gs + 48) == 2) side = (g.U32(g.U32(0x800D6188u)) == 0x20u) ? 1u : 0u;
            }
            uint32_t v;
            if (side != 0) v = g.U32(e + 412) - (g.U32(e + 308) << 1);   // 0x80094700
            else v = g.U32(e + 400) + (g.U32(e + 308) << 1);            // 0x80094714
            g.W32(e + 344, v);
            g.W32(e + 924, 0);
        }
        const int8_t depth = g.S8(e + 946);                         // 0x8009472C: the top AI command
        const uint16_t top = g.U16(e + 956 + 8u * U(static_cast<int32_t>(depth) - 1));
        g.W16(F + 16, top);
        if (s3 & 2u) {
            g.W32(e + 344, s2);                                     // keep the old lateral
        } else {
            bool edge = (s3 & 4u) != 0;
            if (!edge) {
                if (!IsPlayer(g, e) && RiderClass(g, e) == 2u) edge = true;
                else if (top == 1) edge = true;
                else g.W32(e + 344, 0);                             // an ordinary AI: the centre line
            }
            if (edge && !IsPlayer(g, e) && RiderClass(g, e) == 2u && top == 1)
                g.W32(e + 344, (g.S32(e + 364) > 0) ? g.U32(e + 416) : g.U32(e + 404));   // 0x8009480C
        }
        s2 = g.U32(e + 340);                                        // 0x8009482C: the slice
        MulAddView(g, s2 + 20, s2 + 14, g.S32(e + 348), e + 184);
        MulAddView(g, e + 184, s2 + 2, g.S32(e + 344), e + 184);
        const uint32_t x = g.U32(e + 184), y = g.U32(e + 188), z = g.U32(e + 192);
        g.W32(e + 504, x);                                          // 0x80094870
        g.W32(e + 508, y);
        g.W32(e + 512, z);
        g.W32(e + 468, x);
        g.W32(e + 472, y);
        g.W32(e + 476, z);
        RoadRebindBody(g, e, F, c.Road());                          // 0x80094884
        RoadsideRun(g, e, 1, -1, F, c.Road());                      // 0x80094894
        for (uint32_t k = 0; k < 5; ++k) g.W16(e + 432 + 2u * k, g.U16(s2 + 2 + 2u * k));   // 0x8009489C
        {
            const uint16_t v438 = g.U16(e + 438), v12 = g.U16(s2 + 12);
            g.W16(e + 438, static_cast<uint16_t>(0u - v438));
            const uint16_t v440 = g.U16(e + 440);
            g.W16(e + 442, v12);
            g.W16(e + 442, static_cast<uint16_t>(0u - v12));
            g.W16(e + 440, static_cast<uint16_t>(0u - v440));
        }
        for (uint32_t k = 0; k < 3; ++k) g.W16(e + 444 + 2u * k, g.U16(s2 + 14 + 2u * k));  // 0x80094900
        if (s5 != 0) {
            g.W32(e + 364, g.U32(s5 + 4));                          // the leg's direction
            if (g.S32(s5 + 4) < 0)
                for (uint32_t o : {444u, 446u, 448u, 432u, 434u, 436u}) NegH(g, e + o);
        } else {
            g.W32(e + 364, 1);
        }
        if (!IsPlayer(g, e) && RiderClass(g, e) == 2u) {            // 0x80094994: a cop
            gs = g.U32(kPopGameStatePtr);
            const uint32_t np = g.U32(gs + 48);
            if (np != 2) {
                s4 = g.U32(0x8005B38Cu);                            // 0x80094AA4
            } else {
                uint32_t pp = kPopPlayerBikes, out = F + 48;        // 0x800949DC: the nearer player
                for (uint32_t t = 0;;) {
                    const uint32_t pb = g.U32(pp);
                    const int32_t d = Octagon(g.S16(e + 186) - g.S16(pb + 186), g.S16(e + 194) - g.S16(pb + 194));
                    g.W32(out, U(d));
                    out += 4;
                    ++t;
                    pp += 4;
                    if (!(S(t) < g.S32(gs + 48))) break;
                    if (g.Faulted()) return false;
                }
                const int32_t d1 = g.S32(F + 52), d0 = g.S32(F + 48);
                s4 = g.U32(kPopPlayerBikes + ((d1 < d0) ? 4u : 0u));
            }
            if (g.U32(s4 + 360) == g.U32(e + 360) && S(g.U32(e + 364) ^ g.U32(s4 + 364)) < 0) {
                for (uint32_t o : {444u, 448u, 446u, 434u, 432u}) NegH(g, e + o);   // 0x80094AD8: turn round
                g.W32(e + 364, 0u - g.U32(e + 364));
                NegH(g, e + 436);
            }
        }
        if (s3 & 0x10u) {                                           // 0x80094B38
            if (!c.AxisRotation(s2 + 8, s6, F + 24, F)) return false;
            MulMatrix0View(g, e + 432, F + 24, e + 432);
        }
        for (uint32_t k = 0; k < 9; ++k) g.W16(e + 516 + 2u * k, g.U16(e + 432 + 2u * k));  // 0x80094B58
        if (!c.ResetBike(e, F)) return false;                       // 0x80094BA0
        if (s3 & 0x88u) {
            if (s3 & 8u) {
                const uint32_t v = g.U32(e + 668);                  // 0x80094BBC
                g.W32(e + 636, 28595);
                g.W32(e + 652, v + 28595u);
            } else {
                const uint32_t v = g.U32(e + 668);                  // 0x80094BD0
                g.W32(e + 652, 0x1921Fu);
                g.W32(e + 636, 0x1921Fu - v);
            }
            const int32_t ang = S(g.U32(e + 652) * 652u) >> 16;     // 0x80094BF0
            if (!c.AxisRotation(e + 444, ang, F + 24, F)) return false;
            MulMatrix0View(g, e + 432, F + 24, e + 432);
            g.W32(e + 568, g.U32(e + 568) | 0x100u);
        }
        if (s3 & 0x20u) {
            if (!c.SeatRelease(e, g.U32(e + 852), 0, F)) return false;   // 0x80094C4C
        }
        if (!c.BuildObb(e)) return false;                           // 0x80094C54
        const int32_t ang = RatAtan2View(g, S(SX(g.S16(e + 444)) << 4), S(SX(g.S16(e + 448)) << 4));
        g.W32(e + 292, U(ang));                                     // the heading
        const int16_t sn = g.S16(0x8005624Cu + ((U(ang) & 0xFFFu) << 2) + 2u);
        const uint32_t a2 = g.U32(e + 292);
        const uint32_t t308 = g.U32(e + 308);
        g.W32(e + 296, SX(sn) << 4);
        const int16_t cs = g.S16(0x8005624Cu + ((a2 & 0xFFFu) << 2));
        g.W32(e + 300, SX(cs) << 4);
        MulAddView(g, e + 184, e + 528, S(t308 << 1), e + 880);    // 0x80094CC4
        {
            const uint16_t h444 = g.U16(e + 444), h446 = g.U16(e + 446), h448 = g.U16(e + 448);
            const uint16_t h432 = g.U16(e + 432), h434 = g.U16(e + 434), h436 = g.U16(e + 436);
            const uint32_t speed = g.U32(e + 924);
            g.W16(e + 450, h444);                                   // 0x80094CE8
            g.W32(e + 480, speed);
            g.W32(e + 576, speed);
            g.W16(e + 452, h446);
            g.W16(e + 454, h448);
            g.W16(e + 814, h432);
            g.W16(e + 816, h434);
            g.W16(e + 818, h436);
            g.W8(e + 849, GearByte(g, e, S(speed)));
        }
        if ((s3 & 0x40u) && g.U16(e + 956) == 1) {                  // 0x80094D40
            const uint32_t fb = g.U32(e + 564);
            g.W16(e + 960, 1);
            g.W32(e + 564, fb & 0xFFFFFDFFu);
        }
        if (!(s3 & 0x20u)) {                                        // 0x80094D70: seat the rider
            const uint32_t R = g.U32(e + 852);
            GuestCopyWords(g, R + 328, e + 328, 32);
            g.W32(R + 184, g.U32(e + 184));
            g.W32(R + 188, g.U32(e + 188));
            g.W32(R + 192, g.U32(e + 192));
            g.W32(R + 468, g.U32(e + 184));
            g.W32(R + 472, g.U32(e + 188));
            g.W32(R + 476, g.U32(e + 192));
            const uint16_t live = g.U16(e + 320);                   // 0x80094DC8
            g.W32(R + 552, 0);
            g.W16(R + 320, live);
            if (!(s3 & 8u)) {
                if (!c.RowsFromHeading(e, F)) return false;         // 0x80094DE0
            }
            Attach(g, e, R, 2, 0);                                  // 0x80094DF4
            const uint32_t speed = g.U32(e + 924);
            g.W8(e + 72, 0);
            g.W32(e + 480, speed);
            g.W32(e + 576, speed);
            g.W32(e + 580, U(FixMul(S(speed), S(speed))));         // 0x80094E0C
            const uint8_t gear = GearByte(g, e, g.S32(e + 480));
            const uint16_t h444 = g.U16(e + 444);
            g.W8(e + 849, gear);
            g.W16(R + 444, h444);
            for (uint32_t o : {446u, 448u, 438u, 440u, 442u, 432u, 434u, 436u}) g.W16(R + o, g.U16(e + o));
            if (!IsPlayer(g, e) && RiderClass(g, e) == 2u && g.U32(R + 540) == 0) {   // 0x80094EAC
                g.W32(R + 540, ViewSlot(g, 0x800CE170u, R));        // 0x80094F00
            }
            const uint32_t obj = g.U32(R + 540);                    // 0x80094F10
            const uint32_t bank = g.U32(0x800CE190u);
            if (!c.BankSwitch(obj, bank, F)) return false;
            uint32_t ev;
            if (!IsPlayer(g, e) && RiderClass(g, e) == 2u && !(g.U8(e + 928) & 0x10u)) {
                g.W32(R + 604, 0);                                  // 0x80094F70: a parked cop's rider
                ev = 4;
            } else {
                g.W32(R + 604, 1);
                ev = 11;
            }
            if (!c.StanceEvent(ev, R, 1, F)) return false;          // 0x80094F88
        }
        g.W32(e + 568, g.U32(e + 568) | 0x08000000u);               // 0x80094F90: "re-file me"
    }
    // 0x80094FC4: the cop tail
    if (IsPlayer(g, e)) return true;
    if (RiderClass(g, e) != 2u) return true;
    if (g.S16(e + 320) == 0 || !(g.U8(e + 928) & 0x10u)) {
        CopCount(g, 1);                                             // 0x80095024
        const uint8_t f = g.U8(e + 928);
        g.W8(e + 928, static_cast<uint8_t>((f | 0x10u) & 0xDFu));  // "cop in play"
        RouteBind(g, e + 172, 0, s4, F, c.Road());                  // 0x80095044
        g.W32(g.U32(e + 1084) + 40, 0);                             // 0x80095058
        if (!c.ClearCommands(e, F)) return false;
        g.W16(F + 16, (g.S16(e + 320) != 0) ? 1u : 4u);             // {1 or 4, 224}
        g.W16(F + 18, 224);
    } else {
        if (!c.ClearCommands(e, F)) return false;                   // 0x8009508C
        g.W16(F + 16, 4);
        g.W16(F + 18, 224);
    }
    if (!c.PushCommand(F + 16, 1, e, F)) return false;              // 0x800950B0
    if (!c.CopJoin(e, F)) return false;                             // 0x800950B8
    return true;
}

bool Transition(GuestRam& g, uint32_t e, uint32_t old, uint32_t sp, PopulationCallees& c) {
    const uint32_t F = sp - 24u;                                    // `addiu sp,sp,-24`
    if (e == 0) return true;                                        // 0x80093F98
    const uint16_t live = g.U16(e + 320);
    if ((live & 1u) == (old & 1u)) return true;
    if (live == 0) return Retire(g, e, F, c);                       // the WHOLE word (correction 9)
    return Placement(g, e, F, c);
}

bool Activate(GuestRam& g, uint32_t e, uint32_t force, uint32_t sp, PopulationCallees& c) {
    const uint32_t F = sp - 32u;                                    // `addiu sp,sp,-32`
    if (g.S16(e + 320) == 0 && !IsPlayer(g, e) && RiderClass(g, e) == 2u && !(g.U8(e + 928) & 0x10u))
        return true;                                                // a parked cop
    const int16_t old = g.S16(e + 320);                             // 0x80093F44
    uint32_t v = 0;
    if (force == 0) v = RoadWindow(g, e + 172, F);                  // 0x80093F50
    const uint16_t now = g.U16(e + 320);
    g.W16(e + 320, static_cast<uint16_t>((0u - v) & ((now & 2u) | 1u)));   // 0x80093F7C
    return Transition(g, e, SX(old), F, c);
}

bool ActivationPass(GuestRam& g, uint32_t sp, PopulationCallees& c) {
    const uint32_t F = sp - 32u;                                    // `addiu sp,sp,-32`
    int32_t n = g.S32(g.U32(kPopPoolTable + 12));                   // the high index
    uint32_t e = g.U32(kPopPoolTable);
    while (n >= 0) {                                                // 0x80093EA0
        if (!Activate(g, e, 0, F, c)) return false;
        if (g.Faulted()) return false;
        e += g.U32(kPopPoolTable + 4);                              // the stride, re-read per slot
        --n;
    }
    return true;
}

// ============================================================================ riders on the ground

void RiderChase(GuestRam& g, uint32_t r) {
    constexpr uint32_t kStep = 0x50000u, kBack = 0xFFFB0000u;       // +-5.0 (`lui 0x5` / `lui 0xfffb`)
    uint32_t s4 = 0;                                                // the road record the rider ends on
    const uint32_t key = g.U32(r + 360);
    const uint32_t B = g.U32(r + 596);
    const uint32_t s5 = key & 0xFFFFu;
    const uint32_t s3 = g.U16(B + 360);
    uint32_t s1 = 0;                                                // the step
    if (s3 == s5) {                                                 // 0x80099D8C: the same road id
        if ((key >> 16) == 0) s1 = (g.S32(r + 368) < g.S32(B + 368)) ? kStep : kBack;
    } else {
        uint32_t s0 = 0;                                            // the rider's node record
        if ((key >> 16) == 0) s4 = GraphRoad(g, s5);               // 0x80099DC4
        else s0 = GraphNode(g, s5);
        uint32_t a1 = 0, s2 = 0;                                    // the bike's road / node record
        if (g.U16(B + 362) == 0) a1 = GraphRoad(g, s3);             // 0x80099DF4
        else s2 = GraphNode(g, s3);
        uint32_t a2 = 1;
        if (a1 != 0) {                                              // the bike is on a road
            const uint32_t t0 = (g.U32(a1 + 4) >> 6) << 16;         // its length, 16.16
            uint32_t a3 = 0;
            if (s0 != 0) {                                          // the rider is at a node
                uint32_t v1 = s0 + 8;
                const uint32_t end = v1 + 8u * g.U32(s0 + 4);
                while (v1 < end) {                                  // 0x80099E54
                    if (g.U32(v1) == s3) {                          // the node joins the bike's road
                        s4 = a1;
                        g.W32(r + 360, s3);
                        a2 = 0;
                        a3 = (0 < g.S32(v1 + 4)) ? 1u : 0u;
                        break;
                    }
                    v1 += 8;
                    if (g.Faulted()) return;
                }
            } else {                                                // the rider is on another road
                const uint32_t v0 = g.U32(s4 + 8), a0 = g.U32(a1 + 8);   // 0x80099E7C
                bool done = false;
                if (v0 == a0) done = true;
                else {
                    const uint32_t v1 = g.U32(a1 + 12);
                    if (v0 == v1) done = true;
                    else {
                        const uint32_t w = g.U32(s4 + 12);          // 0x80099EC4
                        if (w == a0 || w == v1) {
                            a2 = 0;
                            s1 = kStep;
                        }
                    }
                }
                if (done) {
                    a2 = 0;
                    s1 = kBack;
                }
            }
            if (a2 != 0) {                                          // 0x80099EEC: onto the bike's road
                s4 = a1;
                g.W32(r + 360, s3);
                a3 = ((S(t0) >> 1) < g.S32(B + 368)) ? 1u : 0u;
            }
            if (s1 == 0) g.W32(r + 368, a3 != 0 ? 0u : t0);         // 0x80099F08
        } else {                                                    // the bike is at a node
            uint32_t v1 = s2 + 8;
            if (s4 != 0) {
                const uint32_t end = v1 + 8u * g.U32(s2 + 4);
                while (v1 < end) {                                  // 0x80099F3C
                    if (g.U32(v1) == s5) {
                        s1 = (g.S32(v1 + 4) >= 0) ? kBack : kStep;
                        a2 = 0;
                        break;
                    }
                    v1 += 8;
                    if (g.Faulted()) return;
                }
            }
            if (a2 != 0) {                                          // 0x80099F80: the node's first road
                const uint32_t road = g.U32(s2 + 8);
                g.W32(r + 360, road);
                s4 = GraphRoad(g, road);
                if (g.S32(s2 + 12) < 0) g.W32(r + 368, 0);
                else g.W32(r + 368, (g.U32(s4 + 4) >> 6) << 16);
            }
        }
    }
    const uint32_t v1 = g.U32(r + 368) + s1;                        // 0x80099FB8
    g.W32(r + 368, v1);
    if (s4 == 0) return;
    if (S(v1) < 0) {
        g.W32(r + 360, g.U16(s4 + 8) | 0x10000u);                   // past the start: its start node
    } else if (S((g.U32(s4 + 4) >> 6) << 16) < S(v1)) {
        g.W32(r + 360, g.U16(s4 + 12) | 0x10000u);                  // past the end: its end node
    }
}

bool RiderTransition(GuestRam& g, uint32_t r, uint32_t old, uint32_t sp, PopulationCallees& c) {
    const uint32_t F = sp - 40u;                                    // `addiu sp,sp,-40`
    if (r == 0) return true;                                        // 0x800952BC
    const uint16_t live = g.U16(r + 320);
    if ((live & 1u) == (old & 1u)) return true;
    if (live != 0) {                                                // back in the window
        if (CursorReseat(g, r + 360, r + 328) == 0) {               // 0x800952E0
            g.W16(r + 320, 0);
            return true;
        }
        g.W32(r + 600, 0xFFFF0000u);                                // 0x8009530C, before the call
        RoadClass(g, r, 1, 0, -1, F, c.Road());
        RoadsideRun(g, r, 1, -1, F, c.Road());                      // 0x80095318
        const uint32_t sl = g.U32(r + 340);
        MulAddView(g, sl + 20, sl + 14, g.S32(r + 348), r + 184);   // 0x80095334
        const uint32_t sl2 = g.U32(r + 340);
        MulAddView(g, r + 184, sl2 + 2, g.S32(r + 344), r + 184);   // 0x8009534C
        g.W32(r + 508, 0);
        g.W32(r + 552, 0);
        g.W16(r + 544, 224);
        g.W32(r + 604, 0);
        return c.StanceEvent(73, r, 17, F);                         // 0x80095370
    }
    const uint32_t B = g.U32(r + 596);                              // left the window
    if (g.S16(B + 320) != 0) {
        if (!c.RiderDismount(r, 0, F)) return false;                // 0x8009539C
        if (!c.ClearCommands(g.U32(r + 596), F)) return false;      // 0x800953A8
        const uint16_t h = g.U16(g.U32(r + 596) + 172);
        g.W16(F + 16, 4);                                           // {4, the bike's handle}
        g.W16(F + 18, h);
        if (!c.PushCommand(F + 16, 0, g.U32(r + 596), F)) return false;
        g.W16(F + 16, 18);                                          // {18, the same handle}
        return c.PushCommand(F + 16, 0, g.U32(r + 596), F);         // 0x800953E4
    }
    return c.Remount(B, 1, F);                                      // 0x800953F4: the BIKE
}

bool Downed(GuestRam& g, uint32_t r, uint32_t off, uint32_t sp, PopulationCallees& c) {
    const uint32_t F = sp - 32u;                                    // `addiu sp,sp,-32`
    const int16_t old = g.S16(r + 320);
    uint32_t s1 = off;
    if (old == 0) {
        RiderChase(g, r);                                           // 0x800951E0
    } else if (g.U32(r + 552) & 0x04000000u) {                      // 0x800951F0
        const uint16_t hd = g.U16(r + 172);
        const uint32_t gs = g.U32(kPopGameStatePtr);
        const bool notPool1 = ((hd >> 5) ^ 1u) != 0;
        const bool notPlayer = !(static_cast<int32_t>(hd & 0x1Fu) < g.S32(gs + 48));
        if ((notPool1 || notPlayer) && !(g.U8(r + 572) & 0x20u)) {
            s1 = 1;                                                 // 0x80095248
            const uint32_t B = g.U32(r + 596);
            const uint32_t f = g.U32(r + 552);
            const uint32_t bk = g.U32(B + 360);
            g.W32(r + 552, f & 0xFBFFFFFFu);
            g.W32(r + 360, bk + 1u);                                // the bike's +0x168 + 1, as read
        }
    }
    if (s1 != 0) g.W16(r + 320, 0);                                 // 0x80095278
    else g.W16(r + 320, static_cast<uint16_t>(RoadWindow(g, r + 172, F)));   // NOT masked
    return RiderTransition(g, r, SX(old), F, c);
}

bool DownedRiderPass(GuestRam& g, uint32_t sp, PopulationCallees& c) {
    const uint32_t F = sp - 32u;                                    // `addiu sp,sp,-32`
    int32_t n = g.S32(g.U32(kPopPoolTable + 12));
    uint32_t e = g.U32(kPopPoolTable);
    while (n >= 0) {                                                // 0x8009511C
        const uint32_t r = g.U32(e + 852);
        if ((g.U32(r + 604) - 3u) < 2u) {
            if (!Downed(g, r, 0, F, c)) return false;
        }
        if (g.U8(g.U32(e + 852) + 572) & 0x10u) {                   // the passenger's rider
            const uint32_t pr = g.U32(g.U32(e + 856) + 852);
            if ((g.U32(pr + 604) - 3u) < 2u) {
                if (!Downed(g, pr, 0, F, c)) return false;
            }
        }
        if (g.Faulted()) return false;
        e += g.U32(kPopPoolTable + 4);
        --n;
    }
    return true;
}

// ============================================================================ the dormant racers

int32_t RouteChoice(GuestRam& g, uint32_t e, uint32_t node) {
    if (g.S16(kRouteRecordCount) == -1) return -1;                  // 0x8003A3EC
    const uint32_t rec = RouteLegFor(g, g.U32(e + 428), S(node));   // 0x8003A3F8
    if (rec == 0) return -1;
    uint32_t idx = 0;
    if (!(g.S32(rec + 16) < 2)) {
        const uint32_t r = GuestRand(g);                            // 0x8003A420
        const uint32_t n = g.U32(rec + 16);
        idx = (n == 0) ? r : r % n;                                 // `divu`, the remainder
    }
    return g.S32(rec + 84 + (idx << 2));
}

bool RouteWalk(GuestRam& g, uint32_t e, int32_t dist, uint32_t sp, PopulationCallees& c) {
    const uint32_t F = sp - 40u;                                    // `addiu sp,sp,-40`
    if (e == 0) return true;                                        // 0x80097538
    if (dist <= 0) return true;
    if (g.S16(e + 320) != 0) return true;
    const uint32_t pos = e + 360;
    // At the end of a route (a leg whose end node is -1) the original keeps the remaining distance
    // and walks the same stretch again, for ever (0x80097644 / 0x80097674). The port refuses instead.
    for (uint32_t stretch = 0;; ++stretch) {
        if (stretch == 65536) return false;
        RouteBind(g, e + 172, 1, 0, F, c.Road());                   // 0x80097564
        const uint32_t key = g.U32(pos);
        uint32_t leg;
        if ((key >> 16) == 1) {                                     // at a node: the route's exit
            const int32_t road = RouteChoice(g, e, key & 0xFFFFu);  // 0x80097580
            if (road == -1) return true;
            leg = RouteFindLegView(g, g.U32(e + 428), U(road));
            if (leg == 0) return true;
            g.W32(pos, U(road) & 0xFFFFu);                          // 0x800975AC
            g.W32(pos + 4, g.U32(leg + 4));
            if (g.S32(leg + 4) > 0) g.W32(pos + 8, 0);
            else g.W32(pos + 8, g.U32(leg + 8));
        } else {
            leg = RouteFindLegView(g, g.U32(e + 428), key & 0xFFFFu);   // 0x800975E4
            if (leg == 0) return true;
        }
        int32_t next = -1;                                          // a2
        if (g.S32(leg + 4) > 0) {                                   // 0x80097610: forward
            const uint32_t a = g.U32(pos + 8) + U(dist);
            g.W32(pos + 8, a);
            const uint32_t len = g.U32(leg + 8);
            dist = 0;
            if (S(len) < S(a)) {                                    // past the end
                next = g.S16(leg + 14);
                dist = S(a - len);
                if (next == -1) g.W32(pos + 8, len);                // 0x80097648: stop at the end
            }
        } else {                                                    // 0x8009764C: backward
            const uint32_t a = g.U32(pos + 8) - U(dist);
            g.W32(pos + 8, a);
            if (S(a) >= 0) {
                dist = 0;
            } else {
                next = g.S16(leg + 12);
                dist = S((a + U(S(a) >> 31)) ^ U(S(a) >> 31));     // |a|
                if (next == -1) g.W32(pos + 8, 0);                  // 0x80097678
            }
        }
        if (next != -1) {                                           // 0x80097688: onto the node
            g.W32(pos, (U(next) & 0xFFFFu) | 0x10000u);
            g.W32(pos + 8, 0);
        }
        if (g.Faulted()) return false;
        if (!(dist > 0)) break;                                     // 0x8009769C
    }
    return true;
}

bool DormantDrive(GuestRam& g, uint32_t e, int32_t dt, uint32_t sp, PopulationCallees& c) {
    const uint32_t F = sp - 32u;                                    // `addiu sp,sp,-32`
    if (e == 0) return true;                                        // 0x80095738
    if (g.S16(e + 320) != 0) {                                      // a live bike
        if (g.U32(kPopLiveSeen) == 0) g.W32(kPopLiveSeen, 1);       // 0x80095764
        return true;
    }
    g.W32(e + 568, g.U32(e + 568) & 0xFFFFF7FFu);                   // 0x80095774
    if (g.S16(kRouteRecordCount) == -1) return true;                // no route loaded
    const uint32_t rd = g.U32(e + 1084);
    if ((g.U8(rd + 1) & 0xFu) == 2u && !(g.U8(e + 928) & 0x10u)) return true;   // a parked cop
    if (g.U8(rd) & 0x10u) return true;                              // frozen
    if (g.U32(rd + 40) != 0) return true;                           // finished
    if (!(g.U32(g.U32(e + 852) + 604) < 3u)) return true;           // rider not seated
    int32_t speed = 0;
    if (!c.TargetSpeed(e, dt, F, speed)) return false;              // 0x80095800
    g.W32(e + 924, U(speed));
    g.W32(e + 480, U(speed));                                       // 0x80095818, before FixMul
    const int32_t d = FixMul(dt, speed);
    if (!RouteWalk(g, e, d, F, c)) return false;                    // 0x80095820
    g.W32(e + 324, U(ProgressScalar(g, e + 172, F)));               // 0x80095828
    return true;
}

// ============================================================================ the spawner and the traffic

void Density(GuestRam& g, uint32_t p) {
    const uint32_t b = g.U32(kPopPlayerBikes + 4u * p);             // 0x8009F460
    const uint32_t key = g.U32(b + 360);
    if ((key >> 16) != 0) return;                                   // at a node
    if (g.U32(b + 372) == 0) return;                                // no road data
    int32_t r = S(key & 0xFFFFu);
    const int32_t d = 36 - r;
    r += (d >> 31) & d;                                             // min(road, 36), branch-free
    const uint32_t t = kPopDensityTab + U(r) * 2u;
    const uint32_t gs = g.U32(kPopGameStatePtr);
    int32_t cap = g.U8(t);
    const int32_t bank = g.S32(gs + 60);
    int32_t intv = g.U8(t + 1);
    if (bank > 0) {                                                 // 0x8009F4C4
        if (bank == 1) { cap -= 2; intv += 2; }
        if (bank == 2) { cap -= 1; intv += 1; }
        if (bank == 3) intv += 1;
    }
    const uint8_t rt = g.U8(g.U32(kPopGameStatePtr) + 4);           // 0x8009F4FC
    if (rt & 0x10u) { cap -= 1; intv += 4; }
    if (rt & 0x08u) { cap -= 3; intv += 2; }
    const uint32_t out = kPopTrafficBlk + 4u * p;
    const int32_t over = 15 - cap;
    g.W32(out + 4, U((cap & ~(cap >> 31)) + (over & (over >> 31))));   // clamp(cap, 0, 15)
    g.W32(out + 12, U(intv & ~(intv >> 31)));                       // max(interval, 0)
}

int32_t CapTest(GuestRam& g, uint32_t p) {
    const int32_t live = g.S32(kPopPool3Ctrl);                      // 0x8008DBBC
    return (live < g.S32(kPopTrafficBlk + 4 + 4u * p)) ? 1 : 0;
}

int32_t DirCoin(GuestRam& g, int32_t dir, uint32_t player) {
    if (g.S32(player + 480) < 131) return S(0u - U(dir));           // 0x8003A9A0: slow - oncoming
    const uint32_t r = GuestRand(g);                                // 0x8003A9AC
    if ((r & 1u) == 0) return dir;
    return S(0u - U(dir));
}

int32_t LaneOffset(GuestRam& g, int32_t spacing, int32_t lanes, uint32_t sel) {
    const int8_t s8 = static_cast<int8_t>(sel & 0xFFu);
    int32_t a = s8;
    if (a < 0) a = -a;                                              // 0x8009CF40
    int32_t k = 0;
    if (a == 2) {                                                   // 0x8009CF78: a middle lane
        const int32_t m = S(U(lanes) - 1u);
        k = (m + S(U(m) >> 31)) >> 1;
        if (!(lanes < 3)) {
            const uint32_t r = GuestRand(g);                        // 0x8009CF94
            const uint32_t d = U(m) - U(k);
            k = S(U(k) + ((d == 0) ? r : r % d));                   // `divu`, the remainder
        }
    } else if (a == 3) {
        k = S(U(lanes) - 1u);                                       // 0x8009CFB0: the outer lane
    }
    const uint32_t v = U(spacing) * U(k) + U(spacing >> 1);         // 0x8009CFB4
    return (s8 < 0) ? S(0u - v) : S(v);
}

bool LaneLateral(GuestRam& g, uint32_t car, uint32_t sp, PopulationCallees& c, int32_t& v0) {
    const uint32_t F = sp - 40u;                                    // `addiu sp,sp,-40`
    int32_t s1 = 0;
    uint32_t rec = 0;
    int32_t spacing = 0, lanes = 0, edge = 0;
    bool fwd = false;
    if (g.U16(car + 362) == 0) {                                    // on a road: its own cross-section
        rec = g.U32(car + 372);
        if (rec == 0) {
            v0 = (g.S32(car + 364) >= 0) ? 0x1CCCC : S(0xFFFE3334u);
            return true;
        }
        const int32_t dir = g.S32(car + 364);
        spacing = g.S32(car + 424);
        if (dir > 0) {
            edge = g.S16(rec + 142);
            lanes = g.S16(car + 420);
            fwd = true;
        } else {
            edge = g.S16(rec + 14);
            lanes = g.S16(car + 408);
            spacing = S(0u - U(spacing));
        }
    } else {                                                        // 0x8009E7D8: at a node
        const uint32_t turn = g.U32(car + 356);
        const uint32_t obj = g.U32(car + 328);
        const int16_t road = g.S16(turn + 10);
        if (!c.NodeLanes(obj, road, F + 16, F, rec)) return false;
        if (rec == 0) {
            v0 = (g.S32(car + 364) >= 0) ? 0x1CCCC : S(0xFFFE3334u);
            return true;
        }
        const int32_t dir = g.S32(F + 16);
        spacing = g.S32(rec + 4);
        if (dir > 0) {
            edge = g.S16(rec + 142);
            lanes = g.S16(rec + 140);
            fwd = true;
        } else {
            edge = g.S16(rec + 14);
            lanes = g.S16(rec + 12);
        }
    }
    if (edge > 0) s1 = g.S32(rec + (fwd ? 208u : 80u));             // 0x8009E818 / 0x8009E834
    const int32_t lane = LaneOffset(g, spacing, lanes, U(static_cast<int32_t>(g.S8(car + 508))));
    v0 = S(U(Abs(s1)) + U(Abs(lane)));                              // 0x8009E848
    return true;
}

bool CarCheck(GuestRam& g, uint32_t car, uint32_t sp, PopulationCallees& c, int32_t& v0) {
    const uint32_t F = sp - 24u;                                    // `addiu sp,sp,-24`
    v0 = 0;
    if (car == 0) return true;                                      // 0x8009FE9C
    if (g.U16(car + 172) == 0) return true;
    if (g.S16(car + 320) == 0) return c.Release(car + 172, 3, F);   // a dormant car: released
    const uint32_t b = RoadBttRecord(g, g.S32(g.U32(car + 328)));   // 0x8009FED0
    if (b != 0) {
        const uint32_t o = g.U32(b + 12);
        if (o != 0 && g.U32(car + 328) == o) {                      // its object still resident
            v0 = 1;
            return true;
        }
    }
    g.W16(car + 320, 0);                                            // 0x8009FF00
    return c.Release(car + 172, 3, F);
}

bool CarSpawn(GuestRam& g, uint32_t rec, uint32_t pl, uint32_t sp, PopulationCallees& c, uint32_t& out) {
    const uint32_t F = sp - 112u;                                   // `addiu sp,sp,-112`
    out = 0;
    if (g.S16(kPopCarGate) == 0) return true;                       // 0x8009AD78
    const uint32_t key = g.U32(rec + 8);                            // the road coordinate at sp+16
    g.W32(F + 16, key);
    g.W32(F + 24, g.U32(rec + 36));
    const int16_t dir0 = g.S16(rec + 60);
    g.W32(F + 20, SX(dir0));
    if ((key >> 16) == 1) return true;                              // a node
    if (dir0 == 0) {
        const uint32_t v = GuestRand(g) & 1u;                       // 0x8009ADB0
        g.W32(F + 20, v);
        if (v == 0) g.W32(F + 20, 0xFFFFFFFFu);
    }
    const uint32_t obj = RoadGate(g, 0, F + 16);                    // 0x8009ADD0
    if (obj == 0) return true;
    const uint32_t ctrl = kPopPool3Ctrl;                            // allocate a slot
    const int32_t slot = g.S32(ctrl + 4);
    if (!(slot < 16)) return true;
    int32_t next = slot + 1;
    for (uint32_t w = ctrl + 16u + 512u * U(next) + 4u; next < 16; ++next, w += 512u) {   // 0x8009AE1C
        if (g.U16(w + 168) == 0 && g.U32(w) != 0) break;            // no handle, and word +4 set
    }
    const uint32_t car = ctrl + 16u + 512u * U(slot);
    g.W32(ctrl + 4, U(next));
    g.W16(car + 172, static_cast<uint16_t>(slot + 96));             // the handle
    const uint32_t live = g.U32(ctrl);
    const int32_t high = g.S32(ctrl + 8);
    g.W32(ctrl, live + 1u);
    if (high < slot) g.W32(ctrl + 8, U(slot));
    const uint32_t h = car + 172;
    g.W32(car + 180, g.U16(rec + 2));                               // 0x8009AEA8
    g.W16(car + 322, g.U16(rec + 62));
    if (CursorSeat(g, obj, F + 16, F + 32, F) == 0 ||              // 0x8009AEB4
        g.S16(g.U32(F + 36) + 2) == 1)                              // a junction core piece
        return c.Release(h, 3, F);
    GuestCopyWords(g, car + 328, F + 32, 32);                       // 0x8009AEE4
    const uint32_t sl = g.U32(car + 340);
    const int32_t inSlice = g.S32(car + 348);
    g.W32(car + 184, g.U32(sl + 20));
    g.W32(car + 188, g.U32(sl + 24));
    g.W32(car + 192, g.U32(sl + 28));
    MulAddView(g, sl + 20, sl + 14, inSlice, car + 184);            // 0x8009AF14
    g.W16(car + 454, 0);
    g.W16(car + 452, 0);
    g.W16(car + 450, 0);
    RoadPosition(g, car + 450, car + 328, car + 360, F);            // 0x8009AF30
    uint32_t refRoad, refAlong;                                     // the player, or his rider when down
    const uint32_t R = g.U32(pl + 852);
    if (g.U32(R + 604) < 3u) {
        g.W32(F + 64, g.U32(pl + 184));
        g.W32(F + 68, g.U32(pl + 188));
        g.W32(F + 72, g.U32(pl + 192));
        refRoad = g.U32(pl + 360);
        refAlong = g.U32(pl + 368);
    } else {
        g.W32(F + 64, g.U32(R + 184));                              // 0x8009AF88
        g.W32(F + 68, g.U32(g.U32(pl + 852) + 188));
        g.W32(F + 72, g.U32(g.U32(pl + 852) + 192));
        const uint32_t R2 = g.U32(pl + 852);
        refRoad = g.U32(R2 + 360);
        refAlong = g.U32(R2 + 368);
    }
    int32_t d;
    if (g.U32(car + 360) == refRoad) d = Abs(S(refAlong - g.U32(car + 368))) >> 16;   // 0x8009AFDC
    else d = Octagon(g.S16(F + 66) - g.S16(car + 186), g.S16(F + 74) - g.S16(car + 194));
    const uint32_t gs = g.U32(kPopGameStatePtr);                    // 0x8009B068
    if (g.U8(gs + 4) != 44 && g.S32(gs + 16) > 0 && !(0x77FFFF < S(U(d) << 16)))
        return c.Release(h, 3, F);                                  // within 120.0 once the race runs
    g.W32(car + 364, g.U32(F + 20));                                // 0x8009B0B0: the direction
    const int16_t sel = g.S16(rec + 64);
    const int32_t mag = (sel < 0) ? -sel : sel;
    if (mag == 4) {                                                 // a random lane, not the last one's
        uint32_t lane = 0;
        for (int32_t n = 0;;) {                                     // 0x8009B0D4
            lane = GuestRand(g) & 3u;
            g.W8(car + 508, static_cast<uint8_t>(lane));
            const int8_t last = g.S8(kPopLastLane);
            ++n;
            if (static_cast<int32_t>(lane) != last) break;
            if (!(n < 5)) break;
        }
        g.W8(kPopLastLane, static_cast<uint8_t>(lane));
    } else {
        g.W8(car + 508, g.U8(rec + 64));
    }
    if (g.S8(car + 508) == 0) g.W8(car + 508, 1);                   // 0x8009B114
    if (sel < 0) g.W8(car + 508, static_cast<uint8_t>(0u - g.U8(car + 508)));
    RoadClass(g, car, 1, 0, -1, F, c.Road());                       // 0x8009B148
    if (g.U32(car + 372) != 0) {                                    // the abs() macro: three calls
        int32_t a = 0, b = 0, cc = 0;
        if (!LaneLateral(g, car, F, c, a)) return false;            // 0x8009B160
        if (!LaneLateral(g, car, F, c, b)) return false;
        if (!LaneLateral(g, car, F, c, cc)) return false;
        g.W32(car + 344, U((a >> 31) + b) ^ U(cc >> 31));
    } else {
        g.W32(car + 344, 0x1CCCC);                                  // 1.8
    }
    if (g.S32(F + 20) < 0) g.W32(car + 344, 0u - g.U32(car + 344));
    if (g.S8(car + 508) < 0) g.W32(car + 344, 0u - g.U32(car + 344));
    MulAddView(g, car + 184, sl + 2, g.S32(car + 344), car + 184);  // 0x8009B1E8
    RoadsideRun(g, car, 1, -1, F, c.Road());
    g.W32(car + 492, 0);
    g.W32(car + 496, 0);
    g.W16(car + 438, g.U16(sl + 8));                                // 0x8009B208: the rows
    g.W16(car + 440, g.U16(sl + 10));
    {
        const uint16_t v438 = g.U16(car + 438), v12 = g.U16(sl + 12);
        g.W16(car + 438, static_cast<uint16_t>(0u - v438));
        const uint16_t v440 = g.U16(car + 440);
        g.W16(car + 442, v12);
        g.W16(car + 442, static_cast<uint16_t>(0u - v12));
        g.W16(car + 440, static_cast<uint16_t>(0u - v440));
    }
    for (uint32_t k = 0; k < 3; ++k) g.W16(car + 444 + 2u * k, g.U16(sl + 14 + 2u * k));
    for (uint32_t k = 0; k < 3; ++k) g.W16(car + 432 + 2u * k, g.U16(sl + 2 + 2u * k));
    if (g.S32(F + 20) < 0)
        for (uint32_t o : {432u, 434u, 436u, 444u, 446u, 448u}) NegH(g, car + o);
    RouteBind(g, h, 0, pl, F, c.Road());                            // 0x8009B2F0
    g.W32(car + 324, U(ProgressScalar(g, h, F)));
    uint32_t cls = 0;
    if (!c.ModelBind(car, 3, g.U16(rec + 2), F, cls)) return false; // 0x8009B310
    g.W32(car + 180, cls);
    if (cls == 0xFFFFu) return c.Release(h, 3, F);
    if (!c.CarSetup(car, F)) return false;                          // 0x8009B33C
    if (!c.BuildObb(car)) return false;
    const int32_t ang = RatAtan2View(g, g.S16(car + 444), g.S16(car + 448));   // 0x8009B354
    g.W32(car + 292, U(ang));
    const int16_t sn = g.S16(0x8005624Cu + ((U(ang) & 0xFFFu) << 2) + 2u);
    const uint32_t a2 = g.U32(car + 292);
    const uint16_t h446 = g.U16(car + 446), h448 = g.U16(car + 448);
    g.W32(car + 296, SX(sn) << 4);
    const int16_t cs = g.S16(0x8005624Cu + ((a2 & 0xFFFu) << 2));
    const uint16_t h444 = g.U16(car + 444);
    g.W32(car + 484, 0x26666);                                      // 2.4
    g.W16(car + 452, h446);
    g.W16(car + 454, h448);
    g.W8(car + 509, 0);
    g.W8(car + 510, 0);
    g.W16(car + 450, h444);
    g.W32(car + 316, 0x05500000u);                                  // 1360.0
    g.W32(car + 504, 0xFFFF0000u);                                  // -1.0
    g.W8(car + 511, 255);
    g.W32(car + 300, SX(cs) << 4);
    const uint32_t lv = RoadWindow(g, h, F);                        // 0x8009B3E4
    g.W16(car + 320, static_cast<uint16_t>(lv));
    const int16_t l2 = g.S16(car + 320);
    g.W32(car + 176, 0xFFFFFFFFu);
    uint32_t s2 = car;
    if (l2 == 0) {
        if (!c.Release(h, 3, F)) return false;                      // 0x8009B404
        s2 = 0;
    }
    const uint32_t gs2 = g.U32(kPopGameStatePtr);
    out = s2;
    if (!(g.U8(gs2 + 4) & 1u)) return true;
    if (g.U32(s2 + 180) != 0) return true;                          // (a released car: guest word 180)
    return c.CopJoin(s2, F);                                        // 0x8009B440
}

bool TrafficSched(GuestRam& g, int32_t acc, uint32_t sp, PopulationCallees& c) {
    const uint32_t F = sp - 160u;                                   // `addiu sp,sp,-160`
    uint32_t dist = g.U32(kPopTrafficBlk + 0x1C) << 16;             // s4: 180.0 ahead
    const uint32_t on = g.U32(kPopTrafficOn);
    uint32_t early = 0;                                             // s8
    g.W32(F + 112, 0);                                              // "spawned"
    if (on == 0) return true;                                       // 0x8009D03C: the traffic switch
    uint32_t gs = g.U32(kPopGameStatePtr);
    uint32_t p = g.U32(F + 112);
    if (p < g.U32(gs + 48)) {
        uint32_t flag = F + 104, pb = kPopPlayerBikes;
        do {                                                        // 0x8009D070
            const uint32_t b = g.U32(pb);
            g.W32(flag, 1);
            Density(g, p);
            uint32_t ok = 0;
            if (!c.Budget(3, F, ok)) return false;                  // 0x8009D084
            bool clear = (ok == 0);
            if (!clear) clear = (CapTest(g, p) == 0);
            if (!clear) clear = (g.U32(g.U32(b + 1084) + 40) != 0); // finished
            if (clear) g.W32(flag, 0);
            if (g.U8(g.U32(b + 1084)) & 0x40u) g.W32(flag, 0);      // frozen on the grid
            flag += 4;
            gs = g.U32(kPopGameStatePtr);
            ++p;
            pb += 4;
            if (g.Faulted()) return false;
        } while (p < g.U32(gs + 48));
    }
    gs = g.U32(kPopGameStatePtr);                                   // 0x8009D104
    if (g.U32(gs + 48) == 2 && g.U32(F + 104) == 0 && g.U32(F + 108) == 0) return true;
    gs = g.U32(kPopGameStatePtr);
    if (g.U32(gs + 48) == 1 && g.U32(F + 104) == 0) return true;
    auto clockLimit = [&](uint32_t gsx, uint32_t bank, uint32_t mul) {
        const uint8_t rt = g.U8(gsx + 4);
        const uint32_t k = (rt & 4u) ? bank + 6u : bank + ((0u - (rt & 1u)) & 3u);
        return S(g.U32(kPopClockTab + 4u * k) * mul) >> 16;
    };
    {
        const uint32_t acc0 = g.U32(kPopTrafficAcc);                // 0x8009D16C
        const uint32_t bank = g.U32(gs + 60);
        g.W32(kPopTrafficAcc, acc0 + U(acc));                       // time, not distance
        if (!(clockLimit(gs, bank, 300u) < g.S32(gs + 16))) return true;   // traffic has not started
    }
    {
        const uint32_t gs2 = g.U32(kPopGameStatePtr);               // 0x8009D1E0
        if (!(clockLimit(gs2, g.U32(gs2 + 60), 600u) < g.S32(gs2 + 16))) early = 1;
    }
    if (!c.ShareFlags(F + 104, F)) return false;                    // 0x8009D250
    gs = g.U32(kPopGameStatePtr);
    if (g.U32(gs + 48) != 0) {
        for (uint32_t pl = 0;;) {                                   // 0x8009D284
            do {
                if (g.U32(F + 104 + 4u * pl) == 0) break;
                const uint32_t b = g.U32(kPopPlayerBikes + 4u * pl);
                const uint32_t intv = g.U32(kPopTrafficBlk + 12 + 4u * pl);
                if (!(S(intv << 16) < g.S32(kPopTrafficAcc))) break;
                const uint32_t pos = b + 360;
                const uint32_t fl = g.U32(F + 104 + 4u * pl);
                if (fl == 1) {
                    const uint32_t r100 = GuestRand(g) % 100u;      // 0x8009D2D4: RAND #1
                    const int32_t spd = g.S32(b + 480);
                    int32_t pct;
                    if (0x141DDD < spd) {
                        pct = g.S16(kPopTrafficBlk + 36);
                    } else {
                        const uint32_t gsx = g.U32(kPopGameStatePtr);
                        if (g.U8(gsx + 4) == 44) pct = 90;
                        else if (0x50000 < spd) pct = g.S16(kPopTrafficBlk + 38);
                        else pct = g.S16(kPopTrafficBlk + 40);
                    }
                    int32_t lim = 100;
                    if (pct < 101) lim = pct;
                    const int32_t v = 100 - lim;
                    if (v > 0 && !(v < S(r100))) dist = 0u - dist;  // "ahead" flipped
                } else if (fl == 3) {
                    dist = 0u - dist;
                }
                if (S(dist) < 0) dist = 0u - (g.U32(kPopTrafficBlk + 0x20) << 16);   // 145.0 behind
                if (!c.RoadWalk(pos, F + 88, S(dist), F)) return false;             // 0x8009D3C0
                const uint32_t road = g.U32(F + 88);
                if ((road >> 16) != 0) break;                       // the walk ended on a node
                {
                    const uint32_t gsx = g.U32(kPopGameStatePtr);
                    if ((g.U8(gsx + 4) & 0x10u) && ((road - 11u) < 2u || road == 20u || road == 26u)) break;
                }
                const uint32_t pk = g.U32(b + 360);                 // 0x8009D414
                if ((pk >> 16) != 0) {                              // the player at a node
                    if (early) break;
                    const uint32_t leg = RouteFindLegView(g, g.U32(b + 428), road & 0xFFFFu);
                    if (leg != 0) {
                        if (RouteLegHasRoad(g, g.U32(b + 428), g.U16(F + 88)) != 0)
                            g.W32(F + 92, 0u - g.U32(leg + 4));
                        else
                            g.W32(F + 92, g.U32(leg + 4));
                    } else {
                        g.W32(F + 92, g.U32(pos + 4));
                    }
                } else {
                    if (pk == road) {
                        g.W32(F + 92, (g.S32(b + 364) > 0) ? 1u : 0xFFFFFFFFu);
                    } else {
                        const uint32_t leg = RouteFindLegView(g, g.U32(b + 428), road & 0xFFFFu);
                        g.W32(F + 92, leg != 0 ? g.U32(leg + 4) : 1u);
                    }
                    if (S(dist) > 0) {                              // 0x8009D474: spawning ahead
                        const uint32_t gsx = g.U32(kPopGameStatePtr);
                        if (g.U8(gsx + 4) == 44) {
                            const uint32_t r100 = GuestRand(g) % 100u;       // 0x8009D494
                            int32_t d = g.S32(F + 92);
                            if (S(r100) < 76) d = S(0u - U(d));
                            g.W32(F + 92, U(d));
                        } else if (early) {
                            g.W32(F + 92, 0u - g.U32(F + 92));
                        } else {
                            g.W32(F + 92, U(DirCoin(g, g.S32(F + 92), b)));  // 0x8009D4FC
                        }
                    }
                }
                const int16_t rh = g.S16(F + 88);                   // 0x8009D56C: the record at sp+16
                const uint32_t al = g.U32(F + 96);
                const uint16_t dh = g.U16(F + 92);
                g.W16(F + 18, 0xFFFFu);
                g.W16(F + 80, 4);
                g.W32(F + 24, SX(rh));
                g.W32(F + 52, al);
                g.W16(F + 76, dh);
                uint32_t v = 0;
                if (!c.Spacing(F + 16, F, v)) return false;         // 0x8009D590
                if (g.U32(F + 24) == g.U32(b + 360) &&
                    !(0x780000 < Abs(S(g.U32(b + 368) - g.U32(F + 52)))))
                    break;                                          // within 120.0 of the player
                if (v == 1) break;
                uint32_t car = 0;
                if (!CarSpawn(g, F + 16, b, F, c, car)) return false;   // 0x8009D5E8
                int32_t kept = 0;
                if (!CarCheck(g, car, F, c, kept)) return false;
                if (kept != 0) g.W32(F + 112, 1);
            } while (false);
            gs = g.U32(kPopGameStatePtr);                           // 0x8009D604
            ++pl;
            if (!(pl < g.U32(gs + 48))) break;
            if (g.Faulted()) return false;
        }
    }
    if (g.U32(F + 112) != 0) g.W32(kPopTrafficAcc, 0);              // 0x8009D630
    return true;
}

bool SchedDispatch(GuestRam& g, int32_t kind, int32_t acc, uint32_t sp, PopulationCallees& c) {
    const uint32_t F = sp - 24u;                                    // `addiu sp,sp,-24`
    if (kind == 3) return TrafficSched(g, acc, F, c);               // 0x8009B4A8
    if (kind < 4 && kind == 0) return c.PoliceSched(acc, F);        // 0x8009B498
    return true;
}

bool SpawnerPass(GuestRam& g, int32_t dt, uint32_t sp, PopulationCallees& c) {
    const uint32_t F = sp - 24u;                                    // `addiu sp,sp,-24`
    const uint32_t acc = g.U32(kPopSpawnAcc) + U(dt);               // 0x8008CD94
    g.W32(kPopSpawnAcc, acc);
    if (!(S(acc) < 16384)) {
        if (!c.CellWalker(F)) return false;                         // 0x8008CDB0
    }
    const uint32_t a = g.U32(kPopSpawnAcc);
    if (0xFFFF < S(a)) {                                            // a round
        if (!SchedDispatch(g, 0, S(a), F, c)) return false;         // the police first
        if (!SchedDispatch(g, 3, g.S32(kPopSpawnAcc), F, c)) return false;   // then the traffic
        g.W32(kPopSpawnAcc, 0);
    }
    return true;
}

} // namespace rr::sim
