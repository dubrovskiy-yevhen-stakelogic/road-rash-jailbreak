#include "game/sim/police.h"

#include "game/sim/hud.h"          // HudRouteOff, RASHCDG 0x80095410
#include "game/sim/road_runtime.h" // RoadEndNode, SLUS 0x8003A5F4
#include "game/sim/vec.h"          // DotLcm, SLUS 0x8002E698

// Every function below is transcribed from our own disassembly of SLUS_010.53 (SHA-1
// 67ed165a2c517d4e6106fb0dfa324d66dd9a76f1) and RASHCDG.BIN (SHA-1
// cfe43a7786759f2cb9c57751cf99e84d1074782c). Comments give the original's addresses. Loads that may
// fault are made in the original's order; arithmetic wraps in uint32_t wherever the original's does.

namespace rr::sim {

namespace {

inline uint32_t U(int32_t v) { return static_cast<uint32_t>(v); }
inline int32_t S(uint32_t v) { return static_cast<int32_t>(v); }

// The `sra / addu / xor` absolute value.
inline int32_t Abs(int32_t v) {
    const uint32_t sg = U(v >> 31);
    return S((U(v) + sg) ^ sg);
}

// The row of the threshold / percentage tables: the bank, + 3 with a cop out (0x8009EAD4 and its
// four copies). The game-state pointer and the count are re-read every time, as the original does.
uint32_t PoliceRow(GuestRam& g) {
    const uint32_t gs = g.U32(kPopGameStatePtr);
    const int32_t out = g.S32(kPopCopsOut);
    uint32_t row = g.U32(gs + 60);
    if (out > 0) row += 3u;
    return row << 2;
}

// *(table + 4 * bank), the bank re-read through the game-state pointer.
int32_t ByBank(GuestRam& g, uint32_t table) {
    const uint32_t gs = g.U32(kPopGameStatePtr);
    return g.S32(table + (g.U32(gs + 60) << 2));
}

} // namespace

// ============================================================================ 0x80095848

uint32_t FindFreeCop(GuestRam& g) {
    const uint32_t ctl = kPopPoolTable;                              // 0x8009584C
    int32_t i = g.S32(g.U32(ctl + 12));
    uint32_t e = g.U32(ctl);
    if (i < 0) return 0;                                             // 0x80095860
    const uint32_t gs = g.U32(kPopGameStatePtr);
    for (;;) {
        do {
            if (e == 0) break;                                       // 0x80095870
            if (g.U16(e + 172) < g.U32(gs + 48)) break;              // a player (`sltu`)
            if ((g.U8(g.U32(e + 1084) + 1) & 0xFu) != 2u) break;     // not a cop
            if (g.S16(e + 320) != 0) break;                          // live
            if ((g.U8(e + 928) & 0x10u) == 0) return e;              // 0x800958C8: not in play
        } while (false);
        const uint32_t stride = g.U32(ctl + 4);                      // 0x800958D0
        --i;
        e += stride;
        if (i < 0) return 0;
        if (g.Faulted()) return 0;
    }
}

// ============================================================================ 0x80094184

void CopSetup(GuestRam& g, uint32_t e) {
    if (e == 0) return;                                              // 0x8009419C
    uint32_t rd = g.U32(e + 1084);
    const uint32_t cls = g.U8(rd + 1) & 0xFu;
    if (cls != 2u) return;                                           // 0x800941BC
    const uint32_t rec = kPoliceRecord;
    if (g.U8(rec + 19) == 0) return;                                 // 0x800941D0: no saved record
    g.W8(rd + 12, g.U8(rec + 12));                                   // 0x800941E0
    rd = g.U32(e + 1084);
    g.W8(rd + 13, g.U8(rec + 13));
    rd = g.U32(e + 1084);
    g.W8(rd + 15, g.U8(rec + 15));
    rd = g.U32(e + 1084);
    g.W8(rd + 14, g.U8(rec + 14));
    rd = g.U32(e + 1084);
    g.W8(rd + 2, g.U8(rec + 16));
    rd = g.U32(e + 1084);
    g.W8(rd + 61, g.U8(rec + 17));
    rd = g.U32(e + 1084);
    g.W8(rd + 36, g.U8(rec + 18));
    rd = g.U32(e + 1084);
    g.W8(rd + 37, g.U8(rec + 18));
    rd = g.U32(e + 1084);
    g.W16(rd + 44, g.U16(rec + 20));                                 // 0x80094260
    rd = g.U32(e + 1084);
    g.W8(rd + 46, g.U8(rec + 22));
    g.W8(g.U32(e + 1084) + 47, 0);                                   // 0x8009427C
    g.W32(g.U32(e + 1084) + 48, 0);                                  // 0x80094288
    {
        const uint32_t st = g.U32(e + 556);                          // 0x8009428C
        g.W32(st + 224, g.U32(rec + 8));                             // 0x80094298
    }
    const uint32_t gs = g.U32(kPopGameStatePtr);
    rd = g.U32(e + 1084);
    const uint32_t bank = g.U32(gs + 60);
    const uint8_t old = g.U8(rd + 38);                               // s3
    // clamp(bank, 0, cls) with cls == 2: max(bank, 0) + min(cls - bank, 0), in the original's wrap
    const uint32_t lo = ~U(S(bank) >> 31) & bank;
    const uint32_t d = cls - bank;
    const uint32_t base = ((lo + (U(S(d) >> 31) & d)) << 3) + 66u;   // s2
    int32_t n = 0;
    do {
        const uint32_t r = GuestRand(g);                             // 0x800942D4: RAND
        g.W8(g.U32(e + 1084) + 38, static_cast<uint8_t>(base + (r & 7u)));   // 0x800942E8
        const uint8_t now = g.U8(g.U32(e + 1084) + 38);
        ++n;
        if (now != old) return;                                      // 0x800942FC
        if (g.Faulted()) return;
    } while (n < 8);
}

// ============================================================================ 0x800A01CC

void PoliceShare(GuestRam& g, uint32_t flags, uint32_t sp) {
    const uint32_t F = sp - 56u;                                     // `addiu sp,sp,-56`
    const uint32_t gs = g.U32(kPopGameStatePtr);                     // a3 / t5, kept
    if ((g.U8(gs + 4) & 0x10u) == 0) return;                         // 0x800A01EC
    if (g.U32(flags) == 0) return;
    if (g.U32(flags + 4) == 0) return;
    {
        const uint32_t p1 = g.U32(kPoliceP1Bike), p2 = g.U32(kPoliceP2Bike);   // 0x800A0218
        int32_t big = Abs(g.S16(p1 + 186) - g.S16(p2 + 186));
        int32_t small = Abs(g.S16(p1 + 194) - g.S16(p2 + 194));
        if (big < small) {                                           // 0x800A0254
            const int32_t t = big;
            big = small;
            small = t;
        }
        const int32_t s = small + (small >> 1);
        const int32_t oct = S(U(big) - U(big >> 5) - U(big >> 7) + U(s >> 2) + U(s >> 6));
        const int32_t lim = (g.S32(kPoliceDistTab + (g.U32(gs + 60) << 2)) >> 1) + 100;
        if (!(oct < lim)) return;                                    // 0x800A02B0: too far apart
    }
    if (g.S32(gs + 48) > 0) {
        uint32_t p = 0;
        uint32_t pb = kPopPlayerBikes;
        for (;;) {                                                   // 0x800A02DC
            const uint32_t slot = F + 16u + (p << 2);
            const uint32_t other = p ^ 1u;
            const uint32_t v = kPopViewArray + other * kPopViewStride;
            g.W32(slot, 0);
            const uint32_t b = g.U32(pb);
            const uint32_t dx = g.U32(b + 184) - g.U32(v + 184);
            g.W32(F + 24, dx);
            const uint32_t dy = g.U32(b + 188) - g.U32(v + 188);
            g.W32(F + 28, dy);
            const uint32_t dz = g.U32(b + 192) - g.U32(v + 192);
            g.W32(F + 32, dz);
            const int64_t m0 = static_cast<int64_t>(S(dx)) * static_cast<int64_t>(S(U(static_cast<int32_t>(g.S16(v + 444))) << 4));
            const int64_t m1 = static_cast<int64_t>(S(dy)) * static_cast<int64_t>(S(U(static_cast<int32_t>(g.S16(v + 446))) << 4));
            const int64_t m2 = static_cast<int64_t>(S(dz)) * static_cast<int64_t>(S(U(static_cast<int32_t>(g.S16(v + 448))) << 4));
            const uint64_t u2 = static_cast<uint64_t>(m2);
            g.W32(F + 40, static_cast<uint32_t>(u2));                // 0x800A03BC: lo / hi of the third
            g.W32(F + 44, static_cast<uint32_t>(u2 >> 32));
            const uint32_t sum = static_cast<uint32_t>(static_cast<uint64_t>(m2) >> 16) +
                                 (static_cast<uint32_t>(static_cast<uint64_t>(m1) >> 16) +
                                  static_cast<uint32_t>(static_cast<uint64_t>(m0) >> 16));
            if (S(sum) >= 0) g.W32(slot, 1);                         // 0x800A03E0: in front of his camera
            ++p;
            pb += 4;
            if (!(S(p) < g.S32(gs + 48))) break;
            if (g.Faulted()) return;
        }
    }
    const uint32_t a = g.U32(F + 16), b = g.U32(F + 20);             // 0x800A03F8
    if (a != b) {
        // the player in view of the other's camera loses his cop
        if (a == 0) {
            if (g.U32(flags + 4) != 0) g.W32(flags + 4, 0);          // 0x800A049C
        } else {
            if (g.U32(flags) != 0) g.W32(flags, 0);                  // 0x800A0488
        }
        return;
    }
    int16_t va[3], vb[3];
    for (uint32_t k = 0; k < 3; ++k) {
        va[k] = g.S16(kPopViewArray + 0x1BCu + 2u * k);
        vb[k] = g.S16(kPopViewArray + kPopViewStride + 0x1BCu + 2u * k);
    }
    if (DotLcm(va, vb) >= 0) return;                                 // 0x800A0410 / 0x800A0418
    const uint32_t gs2 = g.U32(kPopGameStatePtr);
    if (g.S32(gs2 + 48) <= 0) return;
    uint32_t f = flags;
    for (int32_t i = 0;;) {                                          // 0x800A043C
        if (g.U32(f) != 0) g.W32(f, 0);
        ++i;
        f += 4;
        if (!(i < g.S32(gs2 + 48))) break;
        if (g.Faulted()) return;
    }
}

// ============================================================================ 0x8009E89C

bool PoliceSched(GuestRam& g, int32_t acc, uint32_t sp, PoliceCallees& pc, PopulationCallees& pop) {
    const uint32_t F = sp - 80u;                                     // `addiu sp,sp,-80`
    const uint32_t on = g.U32(kPoliceOn);                            // 0x8009E8A0
    uint32_t force = 0;                                              // s7, never cleared per player
    g.W32(sp, U(acc));                                               // 0x8009E8D8: into the caller's arg slot
    if (on == 0) return !g.Faulted();                                // the police switch
    {
        const uint32_t gs = g.U32(kPopGameStatePtr);                 // a3, kept for the race-type byte
        if (g.S32(gs + 48) > 0) {
            uint32_t flag = F + 24, pb = kPopPlayerBikes;
            for (int32_t p = 0;;) {                                  // 0x8009E910: pass 1, eligible players
                g.W32(flag, 0);
                const uint32_t b = g.U32(pb);
                const uint32_t rd = g.U32(b + 1084);
                do {
                    if (g.U32(rd + 40) != 0) break;                  // finished
                    if (g.U8(gs + 4) & 1u) break;
                    if (g.U8(rd) & 0x40u) break;                     // frozen on the grid
                    if (g.S16(b + 320) == 0) {                       // 0x8009E95C: the bike dormant
                        const uint32_t R = g.U32(b + 852);
                        if (g.U32(R + 604) < 3u) break;              // the rider seated
                        if (g.S16(R + 320) == 0) break;              // the rider dormant too
                    }
                    if (g.S32(kPoliceAvail) > 0) g.W32(flag, 1);     // 0x8009E9A8
                } while (false);
                flag += 4;
                ++p;
                pb += 4;
                if (!(p < g.S32(g.U32(kPopGameStatePtr) + 48))) break;
                if (g.Faulted()) return false;
            }
        }
    }
    PoliceShare(g, F + 24, F);                                       // 0x8009E9D0
    if (g.Faulted()) return false;
    if (!(g.S32(g.U32(kPopGameStatePtr) + 48) > 0)) return !g.Faulted();
    for (int32_t p = 0;;) {                                          // 0x8009EA04: pass 2
        const uint32_t four = U(p) << 2;                             // s5
        const uint32_t accB = kPoliceAccB + four;                    // s2
        bool store = true;                                           // 0x8009EF38: accB = 0xFFFF0000
        do {
            if (g.U32(F + 24 + four) == 0) break;                    // not eligible
            const uint32_t b = g.U32(kPopPlayerBikes + four);        // s0
            uint32_t slow = 0;                                       // s1
            const uint32_t off = U(HudRouteOff(g, b));               // 0x8009EA28 -> 0x80095410; s8
            const uint32_t rd = g.U32(b + 1084);
            const uint32_t b0 = g.U8(rd);
            const uint32_t place = g.U8(rd + 39);
            const int32_t avail = g.S32(kPoliceAvail);
            const uint32_t wanted = b0 >> 7;                         // s6
            g.W32(F + 36, place == 1u ? 1u : 0u);                    // 0x8009EA6C: the leader
            if (!(S(place) < S(g.U32(kPoliceField) - U(avail))))     // near the back
                slow = (0x165A11 < g.S32(b + 480)) ? 0u : 1u;        // 0x8009EA80
            {
                const uint32_t gs = g.U32(kPopGameStatePtr);
                if (g.U8(gs + 4) == 44 && g.U8(gs + 57) == 3) force = 1;   // 0x8009EAAC
            }
            const uint32_t accA = kPoliceAccA + four;                // 0x8009EAB0
            g.W32(accA, g.U32(accA) + g.U32(sp));
            {
                const uint32_t row = PoliceRow(g);
                if (!(g.S32(accA) < g.S32(kPoliceThrTab + row))) {   // 0x8009EB08
                    const uint32_t r = GuestRand(g);                 // 0x8009EB10: RAND, the police roll
                    const uint32_t r100 = r % 100u;
                    const uint32_t row2 = PoliceRow(g);
                    if (r100 < g.U32(kPolicePctTab + row2)) {        // 0x8009EB78 (`sltu`)
                        const int32_t d = RoadEndNode(g, b + 360, F + 32, 1);   // 0x8009EB88 -> SLUS 0x8003A5F4
                        if (ByBank(g, kPoliceDistTab) < d) force = 1;           // 0x8009EBBC
                    }
                    g.W32(accA, 0);                                  // 0x8009EBCC
                }
            }
            {
                const uint32_t R = g.U32(b + 852);                   // 0x8009EBD0
                if (g.U32(R + 604) == 1u) {                          // seated and riding
                    const int32_t fast = ByBank(g, kPoliceSpeedTab);
                    if (fast < g.S32(b + 480) && wanted == 0 && off == 0 && g.U32(F + 36) == 0 && slow == 0 &&
                        force == 0)
                        break;                                       // 0x8009EC40: nothing to fear
                }
            }
            {
                const int32_t v = g.S32(accB);                       // 0x8009EC48
                if (v < 0) g.W32(accB, 0);
                else g.W32(accB, U(v) + g.U32(sp));
            }
            bool release = force != 0;                               // 0x8009EC70
            if (!release) {
                const uint32_t row = PoliceRow(g);
                release = g.S32(kPoliceThrTab + row) < g.S32(accB);  // 0x8009ECAC
            }
            if (!release && wanted != 0) {
                const uint32_t row = PoliceRow(g);                   // 0x8009ECC0
                release = (g.S32(kPoliceThrTab + row) >> 1) < g.S32(accB);
            }
            if (!release) {
                if (off == 0) {                                      // 0x8009ED00
                    store = false;
                    break;
                }
                const uint32_t row = PoliceRow(g);
                release = (g.S32(kPoliceThrTab + row) >> 1) < g.S32(accB);
                if (!release) {
                    store = false;                                   // 0x8009ED40
                    break;
                }
            }
            // ---- the release, 0x8009ED48
            const uint32_t cop = FindFreeCop(g);                     // s1
            if (g.Faulted()) return false;
            if (cop == 0) break;                                     // 0x8009ED54
            {
                const uint32_t gs = g.U32(kPopGameStatePtr);         // a3, kept
                const int32_t half = g.S32(kPoliceDistTab + (g.U32(gs + 60) << 2)) >> 1;   // a1
                const uint32_t R = g.U32(b + 852);
                if (g.U32(R + 604) < 3u) {                           // 0x8009ED8C: the player's rider seated
                    if (force == 0) {
                        if (!pc.RoadWalk(b + 360, cop + 360, S(0u - U(half)), F)) return false;   // 60.0 behind
                    } else {
                        g.W32(cop + 360, g.U32(b + 360));            // 0x8009EDA4: the player's road
                        const int32_t dir = (g.S32(b + 364) > 0) ? 1 : -1;
                        g.W32(cop + 364, U(dir));                    // 0x8009EDBC
                        const uint32_t along = g.U32(b + 368);
                        const uint32_t dist = g.U32(kPoliceDistTab + (g.U32(gs + 60) << 2));
                        g.W32(cop + 368, dir > 0 ? along + dist : along - dist);   // 0x8009EE10: 120.0 ahead
                        uint32_t first = 0, second = 0;
                        if (!pc.RoadLength(b + 328, F, first)) return false;   // 0x8009EE0C -> SLUS 0x8003C520
                        if (!pc.RoadLength(b + 328, F, second)) return false;  // 0x8009EE18
                        const uint32_t a0 = g.U32(cop + 368);
                        const uint32_t pos = ~U(S(a0) >> 31) & a0;   // max(along, 0)
                        const uint32_t over = U(S(first - a0) >> 31) & (second - a0);
                        g.W32(cop + 368, pos + over);                // 0x8009EE4C: clamped to the road
                    }
                } else {                                             // 0x8009EE50: from the downed rider
                    int32_t dist = S(0u - U(half));
                    if (S(g.U32(b + 364) ^ g.U32(R + 364)) < 0) dist = half;
                    if (!pc.RoadWalk(R + 360, cop + 360, dist, F)) return false;   // 0x8009EE70
                }
            }
            if (g.U16(cop + 362) != 0) break;                        // 0x8009EE80: the walk ended on a node
            if (!pop.ResetBike(cop, F)) return false;                // 0x8009EE88 -> SLUS 0x8002090C
            g.W32(cop + 924, U(ByBank(g, kPoliceSpeedTab)));         // 0x8009EEB0
            g.W32(cop + 480, U(ByBank(g, kPoliceSpeedTab)));         // 0x8009EED0 (delay slot)
            CopSetup(g, cop);                                        // 0x8009EECC
            if (g.Faulted()) return false;
            {
                const uint32_t R = g.U32(cop + 852);
                g.W16(cop + 320, 1);                                 // 0x8009EEE4: live
                g.W32(R + 604, 1);                                   // 0x8009EEF0: seated
            }
            if (!Transition(g, cop, 0, F, pop)) return false;        // 0x8009EEEC -> 0x80093F94: placement
            if (!pop.ClearCommands(cop, F)) return false;            // 0x8009EEF4 -> 0x800BCD10
            g.W16(F + 16, 4);                                        // 0x8009EF0C
            g.W16(F + 18, 224);                                      // 0x8009EF18
            if (!pop.PushCommand(F + 16, 1, cop, F)) return false;   // 0x8009EF14 -> 0x800BCA68
            {
                const uint32_t gs = g.U32(kPopGameStatePtr);
                const int32_t clk = g.S32(gs + 16);
                const uint32_t rd2 = g.U32(cop + 1084);
                g.W16(rd2 + 62, static_cast<uint16_t>(clk >> 8));    // 0x8009EF30
            }
        } while (false);
        if (store) g.W32(accB, 0xFFFF0000u);                         // 0x8009EF38
        ++p;                                                         // 0x8009EF3C
        if (!(p < g.S32(g.U32(kPopGameStatePtr) + 48))) break;
        if (g.Faulted()) return false;
    }
    return !g.Faulted();
}

} // namespace rr::sim
