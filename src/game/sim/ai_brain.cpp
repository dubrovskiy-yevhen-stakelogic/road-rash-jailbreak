// The AI's per-frame brain, ported from our own disassembly of RASHCDG.BIN (SHA-1
// cfe43a7786759f2cb9c57751cf99e84d1074782c). See ai_brain.h; every block below names the original
// addresses it transcribes. Arithmetic is the R3000's: 32-bit wrapping add/sub/shift, `mult` low
// words, FixMul/FixDiv (SLUS 0x8001FC90 / 0x80010028) and the sign-magnitude divide idiom.
#include "game/sim/ai_brain.h"

#include "game/sim/coll_util.h"

namespace rr::sim {

using cu::Add;
using cu::Div16;
using cu::Div4;
using cu::GDot;
using cu::GProject;
using cu::Half;
using cu::Iabs;
using cu::Mid;
using cu::MulLo;
using cu::Neg;
using cu::S;
using cu::SDiv;
using cu::Shl;
using cu::Sub;
using cu::U;

namespace {

constexpr int32_t kNoCandidate = 0x7FFF0000;

// e + 0x3B4 + 8 * (s8)e[+0x3B2]: the top slot of the command stack (0x800BD67C..0x800BD690).
uint32_t TopSlot(GuestRam& g, uint32_t e) {
    return e + 0x3B4u + U(8 * static_cast<int32_t>(g.S8(e + 0x3B2u)));
}

// One object of the query's chain: the octagonal distance test, the de-duplication against the list
// so far and the store (0x8008B130..0x8008B1D4 and its two copies). Returns true when the list is full.
bool QueryTake(GuestRam& g, uint32_t list, uint32_t countPtr, uint32_t pos, int32_t radius, uint32_t h,
               uint32_t px, uint32_t pz, int32_t& found) {
    const int32_t ox = g.S32(px), x = g.S32(pos), oz = g.S32(pz), z = g.S32(pos + 8u);
    const int32_t dx = Iabs(Sub(ox, x)), dz = Iabs(Sub(oz, z));
    const int32_t mn = dx < dz ? dx : dz;
    if (radius < Sub(Add(dx, dz), Half(mn))) return false;
    for (int32_t k = 0; k < found; ++k)
        if (g.U16(list + 2u * U(k)) == h) return false;
    g.W16(list + 2u * U(found), static_cast<uint16_t>(h));
    ++found;
    return found == g.S32(countPtr);
}

} // namespace

// ============================================================================ RASHCDG 0x8008AE94
void AiSpatialQuery(GuestRam& g, uint32_t list, uint32_t countPtr, uint32_t pos, int32_t radius,
                    uint32_t mask, uint32_t handleArg) {
    const uint32_t handle = handleArg & 0xFFFFu; // lhu 76(sp)
    int32_t found = 0;
    // 0x8008AEB4..0x8008AF54: which grid. With other than one player and the point outside grid 0,
    // the row is measured against grid 1's z origin and moved into rows 24..47.
    const int32_t oz0 = g.S32(kAiBrainGridOrgZ);
    const int32_t pz0 = g.S32(pos + 8u), px0 = g.S32(pos);
    const int32_t ox0 = g.S32(kAiBrainGridOrgX);
    int32_t row = (Sub(pz0, oz0) >> 21) + 11;
    const int32_t col = (Sub(px0, ox0) >> 21) + 11;
    uint32_t gs = g.U32(kAiBrainGameState);
    if (g.S32(gs + 48u) != 1) {
        if (!(U(col) < 24u && U(row) < 24u)) {
            row = (Sub(pz0, g.S32(kAiBrainGridOrgZ + 4u)) >> 21) + 11;
            row = Add(row, S(24u << (U(row >> 31) & 31u)));
        }
    }
    const int32_t reach = Add(radius, Half(radius));
    gs = g.U32(kAiBrainGameState);
    const uint32_t lim = 24u << (U(Sub(g.S32(gs + 48u), 1)) & 31u);
    const uint32_t sel = (row >= 24 ? 1u : 0u) & (row < S(lim) ? 1u : 0u);
    const int32_t px = g.S32(pos), ox = g.S32(kAiBrainGridOrgX + 4u * sel);
    const int32_t pz = g.S32(pos + 8u), oz = g.S32(kAiBrainGridOrgZ + 4u * sel);
    int32_t c0 = (Sub(Sub(px, reach), ox) >> 21) + 11;
    int32_t c1 = (Sub(Add(px, reach), ox) >> 21) + 11;
    int32_t r0 = (Sub(Sub(pz, reach), oz) >> 21) + 11;
    int32_t r1 = (Sub(Add(pz, reach), oz) >> 21) + 11;
    if (c0 < 0) c0 = 0;
    if (r0 < 0) r0 = 0;
    if (c1 >= 24) c1 = 23;
    if (r1 >= 24) r1 = 23;
    if (!(c0 < 24) || !(r0 < 24) || c1 < 0 || r1 < 0) {
        g.W32(countPtr, 0);
        return;
    }
    const int32_t rowOff = S((0u - sel) & 0x18u);
    for (int32_t r = r0; r <= r1; ++r) {
        if (c1 < c0) continue;
        const uint32_t pool0 = g.U32(kAiBrainPool0Ptr);
        const int32_t rr = rowOff + r;
        for (int32_t cc = c0;;) {
            uint32_t link = g.U8(kAiBrainGridCells + U(cc + rr * 24));
            if (link != 128u) {
                const uint32_t pool1 = g.U32(kAiBrainPool1Ptr);
                do {
                    const uint32_t code = g.U8(kAiBrainGridLinks + 2u * link + 1u);
                    const uint32_t pool = code >> 5;
                    if ((mask & (1u << pool)) != 0u) {
                        uint32_t ha;  // the address of the object's handle (x at +12, z at +20)
                        if (pool == 0) ha = pool0 + code * 1096u + 0xACu;
                        else if (pool == 1) ha = pool1 + (code & 31u) * 628u + 0xACu;
                        else {
                            const uint32_t rec = kAiBrainPoolTable + pool * 16u;
                            const uint32_t stride = g.U32(rec + 4u);
                            const uint32_t base = g.U32(rec);
                            const uint32_t off = g.U32(kAiBrainHandleOff + (pool - 2u) * 4u);
                            ha = base + U(MulLo(S(stride), S(code & 31u))) + off;
                        }
                        const uint32_t h = g.U16(ha);
                        if (h != handle &&
                            QueryTake(g, list, countPtr, pos, radius, h, ha + 12u, ha + 20u, found))
                            return;
                    }
                    link = g.U8(kAiBrainGridLinks + 2u * link);
                } while (link != 128u);
            }
            ++cc;
            if (c1 < cc) break;
        }
    }
    g.W32(countPtr, U(found));
}

// ============================================================================ RASHCDG 0x800BEBA4
int32_t AiBrainProbe(GuestRam& g, uint32_t h, uint32_t e) {
    uint32_t obj = 0, s1, wAddr;
    const uint32_t pool = (h & 0xFFFFu) >> 5;
    if (pool == 6) {                                                           // 0x800BEBDC
        s1 = g.U32(kAiBrainPool6Ptr) + (h & 31u) * 280u;
        wAddr = kAiBrainWeights + (g.S32(s1 + 8u) == 1 ? 4u : 6u);
    } else {                                                                   // 0x800BEC28
        const uint32_t rec = kAiBrainPoolTable + pool * 16u;
        const uint32_t stride = g.U32(rec + 4u);
        obj = g.U32(rec) + U(MulLo(S(stride), S(h & 31u)));
        s1 = obj + 172u;
        wAddr = kAiBrainWeights + (g.U16(s1) >> 5);
    }
    const uint32_t weight = g.U8(wAddr);
    // The lateral distance (0x800BEC6C..0x800BED7C): the road-lateral difference on the same road
    // key, the projection on e's lateral axis +0x1B0 otherwise; with +0x3A0 bit 0 clear, on +0x204.
    auto sameRoad = [&]() {
        const uint32_t k = g.U32(s1 + 188u);
        return k == g.U32(e + 0x168u) && ((k >> 16) == 0u || g.U32(s1 + 164u) == g.U32(e + 0x150u));
    };
    int32_t d;
    if (g.U8(e + 0x3A0u) & 1u) {
        d = sameRoad() ? Sub(g.S32(s1 + 172u), g.S32(e + 0x158u)) : GProject(g, s1 + 12u, e + 432u, e + 184u);
    } else {
        d = GProject(g, s1 + 12u, e + 0x204u, e + 0x1F8u);
    }
    if (Iabs(d) > 0x20000) return kNoCandidate;
    // The distance ahead (0x800BED9C..0x800BEE80).
    int32_t ahead;
    if ((g.U8(e + 0x3A0u) & 1u) && g.S32(e + 0x144u) > 0x3C000)
        ahead = Shl(Sub(g.S32(e + 0x144u), g.S32(s1 + 152u)), 4);
    else
        ahead = GProject(g, s1 + 12u, e + 0x210u, e + 0x1F8u);
    if (ahead < Neg(g.S32(s1 + 136u))) return kNoCandidate;
    // The closing speed (0x800BEEA4..0x800BEF60): against a bike or rider (pool < 4) moving along
    // e's heading, the speed difference.
    int32_t speed;
    if ((g.U16(s1) >> 5) < 4u) {
        const int32_t p0 = MulLo(g.S16(obj + 444u), g.S16(e + 528u));
        const int32_t p1 = MulLo(g.S16(obj + 446u), g.S16(e + 530u));
        const int32_t p2 = MulLo(g.S16(obj + 448u), g.S16(e + 532u));
        const int32_t dot = Shl(Add(Add(Shl(p0, 4) >> 16, Shl(p1, 4) >> 16), Shl(p2, 4) >> 16), 4);
        if (Iabs(dot) < 16384) speed = g.S32(e + 0x1E0u);
        else speed = dot >= 0 ? Sub(g.S32(e + 0x1E0u), g.S32(obj + 0x1E0u)) : Add(g.S32(e + 0x1E0u), g.S32(obj + 0x1E0u));
    } else {
        speed = g.S32(e + 0x1E0u);
    }
    if (speed < 524) return kNoCandidate;
    if (ahead > 0xBF0000) return kNoCandidate;
    const int32_t t = SDiv(ahead, speed);                                       // 0x800BEF84
    int32_t w = S(weight << 12);
    const int32_t lat = Iabs(sameRoad() ? Sub(g.S32(s1 + 172u), g.S32(e + 0x158u))
                                        : GProject(g, s1 + 12u, e + 432u, e + 184u));
    if (lat > 0x1547A) w = Mid(w, Div16(MulLo(Mid(lat, lat), 9)));              // 0x800BF03C
    if ((g.U16(s1) >> 5) == 0u) {
        if (g.U32(g.U32(obj + 852u) + 552u) & 0x8000u) return kNoCandidate;
    }
    return Mid(w, t);
}

// ============================================================================ RASHCDG 0x800BD4D4
bool AiBrain(GuestRam& g, uint32_t e, uint32_t sp, AiBrainCallees& c, int32_t& v0) {
    const uint32_t fp = sp - kAiBrainFrame;
    v0 = 0;
    uint32_t s5 = 0;   // the flags: 1 pass on the left, 2 same road as a car, 4 lean added, 8 target
                       // left of the road, 0x10 pool-6 arm, 0x20 hold, 0x40 double the gap limit
    uint32_t other = 0;
    g.W32(fp + 132u, 0);
    auto done = [&]() { return !g.Faulted(); };
    auto popDone = [&]() { return c.PopCommand(e, fp) && !g.Faulted(); };
    if (e == 0) return done();

    if (g.S32(e + 0x1E0u) < 131) {                                             // 0x800BD514
        if (g.U16(TopSlot(g, e)) != 3u) return done();
        return popDone();
    }
    // 0x800BD554: the query, radius (speed + 20.1) * 2.
    const int32_t reach = Mid(Add(g.S32(e + 0x1E0u), 0x141DDD), 0x20000);
    g.W32(fp + 104u, 16);
    g.W32(fp + 16u, 73);
    const uint32_t hnd = g.U16(e + 0xACu);
    g.W16(fp + 112u, 224);
    g.W32(fp + 20u, hnd);
    AiSpatialQuery(g, fp + 24u, fp + 104u, e + 0xB8u, reach, 73u, hnd);
    int32_t s7 = reach;
    uint32_t kind = 7;
    if (g.S32(fp + 104u) != 0) {                                               // 0x800BD5C8
        const uint8_t b = g.U8(e + 0x3A0u);
        const uint32_t f = g.U32(e + 0x184u);
        g.W8(e + 0x3A0u, static_cast<uint8_t>(b & 0xFEu));
        if (!(f & 1u)) {
            const int32_t d = GDot(g, e + 528u, g.U32(e + 340u) + 14u);
            g.W32(fp + 120u, U(d));
            g.W32(fp + 120u, U(Iabs(d)));
            if (g.S32(fp + 120u) > 0xD1EB) g.W8(e + 0x3A0u, static_cast<uint8_t>(g.U8(e + 0x3A0u) | 1u));
        }
    }
    for (int32_t k = 0; k < g.S32(fp + 104u); ++k) {                           // 0x800BD62C
        const int32_t sc = AiBrainProbe(g, g.U16(fp + 24u + 2u * U(k)), e);
        if (sc < s7) {
            const uint16_t h2 = g.U16(fp + 24u + 2u * U(k));
            s7 = sc;
            kind = h2 >> 5;
            g.W16(fp + 112u, h2);
        }
    }
    // 0x800BD67C: nothing chosen - keep a command 3 on a car only while the car is still ahead.
    const uint32_t top = TopSlot(g, e);
    g.W32(fp + 128u, top);
    const uint32_t best = g.U16(fp + 112u);
    if (best == 224u) {
        if (top == 0) return done();
        const uint32_t op = g.U16(top);
        if (op != 3u) return done();
        if (g.U8(e + 0x3A0u) & 1u) {
            const uint32_t tg = g.U16(top + 2u);
            if ((tg >> 5) == op) {
                const uint32_t car = kAiBrainCarPool + (tg & 31u) * 512u;
                uint32_t hold = 0;
                if (g.S16(car + 320u) != 0)
                    hold = Sub(g.S32(car + 324u), g.S32(car + 308u) >> 4) < g.S32(e + 324u) ? 1u : 0u;
                s5 |= hold << 5;
            }
        }
        if (!(s5 & 0x20u)) return popDone();
        const uint16_t t2 = g.U16(g.U32(fp + 128u) + 2u);
        kind = t2 >> 5;
        g.W16(fp + 112u, t2);
    } else if (top != 0 && g.U16(top + 2u) == best && g.U16(top) >= 5u) {    // 0x800BD760
        return done();
    }
    // 0x800BD794: the target by pool; +120 = the closing speed.
    uint32_t tgt = 0;   // s6: the target's handle address (a pool-6 record itself)
    const uint32_t bh = g.U16(fp + 112u);
    if (kind == 0) {
        other = g.U32(kAiBrainPool0Ptr) + (bh & 31u) * 1096u;
        tgt = other + 172u;
        const int32_t d = GDot(g, other + 528u, e + 528u);
        if (Iabs(d) < 16384) g.W32(fp + 120u, g.U32(e + 480u));
        else g.W32(fp + 120u, U(d < 0 ? Add(g.S32(e + 480u), g.S32(other + 480u)) : Sub(g.S32(e + 480u), g.S32(other + 480u))));
    } else if (kind == 3) {
        const uint32_t car = kAiBrainCarPool + (bh & 31u) * 512u;
        tgt = car + 172u;
        const int32_t d = GDot(g, car + 444u, e + 528u);
        if (Iabs(d) < 16384) {
            g.W32(fp + 120u, g.U32(e + 480u));
        } else {
            g.W32(fp + 120u, U(d < 0 ? Add(g.S32(e + 480u), g.S32(car + 480u)) : Sub(g.S32(e + 480u), g.S32(car + 480u))));
            const uint32_t k = g.U32(e + 360u);
            if (k == g.U32(car + 360u) && (k >> 16) == 0u) {
                s5 |= 2u;
                if (g.U8(car + 510u) & 2u) s5 |= 0x40u;
            }
        }
    } else if (kind == 6) {
        tgt = g.U32(kAiBrainPool6Ptr) + (bh & 31u) * 280u;
        if (g.S32(tgt + 8u) == 1) s5 |= 0x10u;
        g.W32(fp + 120u, g.U32(e + 480u));
    } else {
        return done();
    }
    // 0x800BD928: the gap ahead (s7).
    if ((g.U8(e + 0x3A0u) & 1u) && g.U32(e + 360u) == g.U32(tgt + 188u)) {
        s7 = Sub(Shl(Sub(g.S32(e + 324u), g.S32(tgt + 152u)), 4), Add(g.S32(e + 308u), g.S32(tgt + 136u)));
    } else {
        s7 = Sub(GProject(g, tgt + 12u, e + 528u, e + 504u), g.S32(e + 308u));
        if (!(s5 & 2u)) s7 = Sub(s7, Mid(0xB4FD, Add(g.S32(tgt + 132u), g.S32(tgt + 136u))));
    }
    // 0x800BD9AC: outside [own half length, 2 * closing speed] - drop (or keep) a command 3.
    if (s7 < g.S32(e + 308u) || Mid(g.S32(fp + 120u), 0x20000) < s7) {
        const uint32_t t3 = g.U32(fp + 128u);
        if (g.U16(t3) != 3u) {
            if (!(s5 & 0x20u)) return done();
        } else {
            if (g.U16(t3 + 2u) != g.U16(fp + 112u)) return popDone();
            uint32_t hold = 0;
            if (g.U8(e + 0x3A0u) & 1u) hold = kind == 3u ? 1u : 0u;
            s5 |= hold << 5;
            if (!(s5 & 0x20u)) return popDone();
        }
    }
    // 0x800BDA64: a bike close enough - command 5 / 7 / 12 against it.
    if (kind == 0 && g.S32(fp + 120u) <= 0x13FFFF) {
        if (g.U16(g.U32(fp + 128u)) >= 5u) {                                   // 0x800BDA9C
            int32_t lat;
            const uint32_t k = g.U32(other + 360u);
            if (k == g.U32(e + 360u) && ((k >> 16) == 0u || g.U32(other + 336u) == g.U32(e + 336u))) {
                lat = Sub(g.S32(other + 344u), g.S32(e + 344u));
                if (g.S32(e + 364u) < 0) lat = Neg(lat);
            } else {
                lat = GProject(g, other + 184u, e + 432u, e + 184u);
            }
            const uint32_t q = g.U32(other + 856u);
            if (q != 0 && lat < 0) lat = Add(lat, Add(g.S32(other + 304u), g.S32(q + 304u)));
            if (Iabs(lat) > 0x18000) return done();
            if (s7 > 0xA0000) return done();
        }
        // 0x800BDB68
        const uint32_t oTop = other + 0x3B4u + U(8 * static_cast<int32_t>(g.S8(other + 946u)));
        const uint32_t rdE = g.U32(e + 1084u);
        const uint32_t rdO = g.U32(other + 1084u);
        uint16_t cmd;
        if ((g.U8(rdO + 1u) & 0xFu) != (g.U8(rdE + 1u) & 0xFu)) {
            cmd = 7;
        } else if (g.U16(oTop) == 10u && g.U16(fp + 98u) != g.U16(e + 172u)) {  // the stale read
            cmd = 12;
        } else if (g.U16(oTop + 2u) == g.U16(e + 172u)) {
            cmd = 7;
        } else {
            cmd = Shl(g.S32(e + 308u), 2) < s7 ? 5 : 7;
        }
        g.W16(fp + 96u, cmd);
        const uint32_t cr = g.U16(fp + 96u);
        if (cr == 7u || cr == 5u) {
            uint32_t ok = 0;
            if (!(cr == 7u ? c.CloseTest(e, other, s7, fp, ok) : c.GapTest(e, other, s7, fp, ok))) return false;
            if (ok == 0) return done();
        }
        if (g.U16(g.U32(fp + 128u)) == 3u && !c.PopCommand(e, fp)) return false;   // 0x800BDC38
        g.W16(fp + 98u, g.U16(fp + 112u));
        if (!c.PushCommand(fp + 96u, 2, e, fp)) return false;
        if (g.U16(fp + 96u) != 7u) return done();
        if (U(g.U16(oTop)) - 6u >= 2u) return done();
        uint32_t ok = 0;
        if (!c.GapTest(e, other, s7, fp, ok)) return false;
        if (ok == 0) return done();
        g.W16(fp + 96u, 5);
        if (!c.PushCommand(fp + 96u, 2, e, fp)) return false;
        return done();
    }
    // 0x800BDCCC: command 3 on the target (re-aimed and re-stamped when it is already on top).
    {
        const uint32_t t3 = g.U32(fp + 128u);
        if (t3 != 0 && g.U16(t3) == 3u) {
            const uint16_t st = g.U16(t3 + 6u);
            g.W16(t3 + 2u, g.U16(fp + 112u));
            if (!(st & 0x8000u)) {
                const int32_t a1 = MulLo(g.S32(g.U32(kAiBrainGameState) + 16u), 0x884);
                if (0x78000 < Sub(a1, Shl(st & 0x3FFF, 16)))
                    g.W16(t3 + 6u, static_cast<uint16_t>(st | 0x8000u | (U(Add(a1, 0x8000) >> 16) & 0x3FFFu)));
            }
        } else {
            g.W16(fp + 96u, 3);
            g.W16(fp + 98u, g.U16(fp + 112u));
            if (!c.PushCommand(fp + 96u, 2, e, fp)) return false;
            g.W32(fp + 132u, 1);
            g.W32(fp + 128u, e + 948u + U(8 * static_cast<int32_t>(g.S8(e + 946u))));
        }
    }
    // 0x800BDD98: the two lateral edges of the target measured across e's heading (s1 left, s0 right).
    int32_t s0, s1, s2 = 0;
    if (s5 & 2u) {
        int32_t d = Sub(g.S32(tgt + 172u), g.S32(e + 344u));
        if (g.S32(e + 364u) < 0) d = Neg(d);
        const int32_t w = g.S32(tgt + 132u);
        s1 = Sub(d, w);
        s0 = Add(d, w);
        g.W32(fp + 124u, U(w));
    } else {
        g.W32(fp + 56u, g.U32(e + 296u));
        g.W32(fp + 60u, U(Neg(g.S32(e + 300u))));
        if (s5 & 0x10u) {
            g.W32(fp + 124u, Sub(g.S32(e + 188u), g.S32(e + 312u)) < g.S32(tgt + 88u) ? g.U32(tgt + 132u) : g.U32(tgt + 84u));
        } else {
            const uint32_t pool = g.U16(tgt) >> 5;
            g.W32(fp + 124u, g.U32(tgt + 132u));
            if (pool == 0) {
                const uint32_t q = g.U32(other + 856u);
                if (q != 0) s2 = Shl(g.S32(q + 304u), 1);
            }
        }
        s0 = Add(Mid(g.S32(e + 296u), g.S32(tgt + 124u)), Mid(g.S32(e + 300u), g.S32(tgt + 128u)));
        int32_t p80, p84, p88, p92;
        if (s0 < 0) {
            p80 = Add(g.S32(tgt + 48u), Mid(s2, g.S32(tgt + 124u)));
            p84 = Sub(g.S32(tgt + 56u), Mid(s2, g.S32(tgt + 128u)));
            const int32_t w2 = Shl(g.S32(fp + 124u), 1);
            p88 = Sub(g.S32(tgt + 48u), Mid(w2, g.S32(tgt + 124u)));
            p92 = Add(g.S32(tgt + 56u), Mid(w2, g.S32(tgt + 128u)));
        } else {
            p80 = g.S32(tgt + 24u);
            p84 = g.S32(tgt + 32u);
            const int32_t w2 = Add(Shl(g.S32(fp + 124u), 1), s2);
            p88 = Add(g.S32(tgt + 24u), Mid(w2, g.S32(tgt + 124u)));
            p92 = Sub(g.S32(tgt + 32u), Mid(w2, g.S32(tgt + 128u)));
        }
        g.W32(fp + 80u, U(p80));
        g.W32(fp + 84u, U(p84));
        g.W32(fp + 88u, U(p88));
        g.W32(fp + 92u, U(p92));
        const int32_t hx = g.S32(fp + 56u), hz = g.S32(fp + 60u);
        s1 = Add(Mid(hx, Sub(p80, g.S32(e + 184u))), Mid(hz, Sub(p84, g.S32(e + 192u))));
        s0 = Add(Mid(hx, Sub(p88, g.S32(e + 184u))), Mid(hz, Sub(p92, g.S32(e + 192u))));
    }
    // 0x800BDFBC: the lean term widens the side the bike leans to.
    {
        const int32_t steer = g.S32(e + 652u);
        const uint32_t idx = (U(MulLo(steer, 0xA3)) >> 12) & 0x3FFCu;
        s2 = Iabs(Mid(Shl(g.S16(kAiBrainSteerSin + idx), 4), g.S32(e + 312u)));
    }
    {
        const int32_t steer = g.S32(e + 652u);
        if (steer < -1143) s0 = Add(s0, s2);
        else if (steer >= 1144) s1 = Sub(s1, s2);
    }
    const int32_t a1abs = Iabs(s1), a0abs = Iabs(s0);
    const bool oneSide = (s1 >= 0) != (s0 < 0);   // both edges on one side of e's line
    const int32_t wide = Add(Shl(g.S32(e + 304u), 1), g.S32(e + 304u));
    if (a1abs < a0abs) {                                                       // 0x800BE054
        if (oneSide && wide < a1abs) {
            s5 |= 0x20u;
            if (!(s5 & 2u)) {
                v0 = g.S32(fp + 132u);
                return done();
            }
        }
        s5 |= 1u;
    } else if (oneSide && wide < a0abs) {                                     // 0x800BE090
        s5 |= 0x20u;
        if (!(s5 & 2u)) {
            v0 = g.S32(fp + 132u);
            return done();
        }
    }
    // 0x800BE0C4: a command not fresh this pass keeps the side it chose before (+0x3A0 bit 2).
    if (!(g.U16(g.U32(fp + 128u) + 6u) & 0x8000u)) s5 = (s5 & ~1u) | ((g.U8(e + 0x3A0u) >> 2) & 1u);
    if (s5 & 2u) {
        // 0x800BE104: behind a car on the same road - a lateral aim delta.
        int32_t need = Add(Add(s2, g.S32(fp + 124u)), g.S32(e + 304u));
        const int32_t steer = g.S32(e + 652u);
        if ((steer < -1143 && !(s5 & 1u)) || (steer >= 1144 && (s5 & 1u))) {
            s5 |= 4u;
            need = Add(need, s2);
        }
        const int32_t o158 = g.S32(tgt + 172u);
        if (need < Iabs(Sub(g.S32(e + 344u), o158))) s5 |= 0x20u;
        if (o158 < 0) s5 |= 8u;
        const int32_t lanes = g.S16(e + ((s5 & 8u) ? 408u : 420u));
        g.W32(fp + 108u, 0);
        if (lanes >= 2) {
            const int32_t lw = g.S32(e + 424u);
            const int32_t q = SDiv(Iabs(g.S32(tgt + 172u)), lw) >> 16;
            const int32_t m = MulLo(lw, q);
            g.W32(fp + 108u, U((s5 & 8u) ? Sub(g.S32(fp + 108u), m) : Add(g.S32(fp + 108u), m)));
        }
        const uint32_t flip = ((s5 ^ 1u) & 1u) ^ (g.S32(e + 364u) > 0 ? 1u : 0u);
        if ((g.S32(tgt + 172u) >= 0 ? 1u : 0u) != flip) {
            const int32_t lw = g.S32(e + 424u);
            g.W32(fp + 108u, U((s5 & 8u) ? Sub(g.S32(fp + 108u), lw) : Add(g.S32(fp + 108u), lw)));
        }
        if (s5 & 4u) {
            const uint32_t m = 0u - (((g.S32(e + 652u) >= 0) ? 1u : 0u) ^ (g.S32(e + 364u) > 0 ? 1u : 0u));
            const int32_t a = Add(Neg(s2), S(m & U(Sub(s2, Neg(s2)))));
            g.W32(fp + 108u, U(Add(g.S32(fp + 108u), Shl(a, 1))));
        }
        const uint32_t sl = g.U32(e + 340u);
        const int32_t p1 = MulLo(g.S16(sl + 2u), g.S16(e + 872u));
        const int32_t p2 = MulLo(g.S16(sl + 6u), g.S16(e + 876u));
        int32_t t0 = Sub(g.S32(fp + 108u), Shl(g.S16(e + 878u), 5));
        const int32_t sum = Add(Shl(p1, 4) >> 16, Shl(p2, 4) >> 16);
        g.W32(fp + 108u, U(t0));
        if (sum < 0) t0 = Neg(t0);
        g.W32(fp + 108u, U(t0));
        g.W32(fp + 16u, 0);
        if (!c.SetAimDelta(e, 0, e + 872u, fp + 108u, 0, fp)) return false;
        g.W16(e + 944u, 1);
    } else {
        // 0x800BE350: an aim point beside the target, on the side chosen.
        g.W32(fp + 108u, U(GProject(g, e + 880u, e + 516u, e + 504u)));
        const int32_t fwd = GProject(g, e + 880u, e + 528u, e + 504u);
        const int32_t side = g.S32(fp + 108u);
        if ((side < 1 ? 1u : 0u) != (s5 & 1u)) {                               // 0x800BE390
            const int32_t slope = SDiv(Iabs(side), fwd);
            const int32_t need = SDiv((s5 & 1u) ? a0abs : a1abs, s7);
            if (need < Shl(slope, 1)) s5 ^= 1u;
        }
        int32_t off = s7 > 0x2FFFF ? Shl(g.S32(e + 304u), 1) : Mid(Div4(s7), g.S32(e + 304u));
        off = (s5 & 1u) ? Sub(Neg(a1abs), off) : Add(off, a0abs);
        g.W32(fp + 108u, U(SDiv(Mid(off, fwd), s7)));
        g.W32(fp + 64u, U(Add(Add(Mid(fwd, g.S32(e + 300u)), Mid(g.S32(fp + 108u), g.S32(fp + 56u))), g.S32(e + 184u))));
        g.W32(fp + 72u, U(Add(Add(Mid(fwd, g.S32(e + 296u)), Mid(g.S32(fp + 108u), g.S32(fp + 60u))), g.S32(e + 192u))));
        g.W32(fp + 68u, g.U32(e + 884u));
        g.W32(fp + 16u, 0);
        if (!c.SetAimDelta(e, fp + 64u, 0, 0, 0, fp)) return false;
    }
    // 0x800BE5F4: remember the side; slow down toward a target inside the gap limit.
    g.W8(e + 928u, static_cast<uint8_t>((g.U8(e + 928u) & 0xFBu) | ((s5 & 1u) << 2)));
    const uint32_t gs = g.U32(kAiBrainGameState);
    const int32_t lvl = g.S32(gs + 60u);
    int32_t lim = g.S32(kAiBrainGapLimit + U(lvl) * 4u);
    if (s5 & 0x40u) lim = Shl(lim, 1);
    if (!(s5 & 0x20u) && s7 < lim && 0x30000 < s7) {
        const uint32_t pool = g.U16(tgt) >> 5;
        if (pool == 3u && !(0x19998 < g.S32(tgt + 140u)) && 0xE0000 < g.S32(fp + 120u) &&
            s7 < Sub(lim, Shl(MulLo(Sub(S(pool), lvl), 15), 15))) {
            g.W32(e + 560u, g.U32(e + 560u) | 0x800u);                        // 0x800BE6B4
            const uint32_t t3 = g.U32(fp + 128u);
            const uint32_t w = g.U16(t3 + 4u);
            if (w - 1u < 300u) {
                g.W16(t3 + 4u, static_cast<uint16_t>(w + g.U16(gs + 28u)));
            } else {
                g.W16(t3 + 4u, 1);
                g.W32(e + 560u, g.U32(e + 560u) | 0x1000u);
            }
        } else {
            g.W32(fp + 108u, U(SDiv(Sub(s7, 0x30000), Sub(lim, 0x30000))));    // 0x800BE710
            g.W32(e + 924u, U(Mid(g.S32(e + 924u), g.S32(fp + 108u))));
            g.W32(e + 916u, 0);
            g.W32(e + 920u, 0);
            g.W32(e + 564u, g.U32(e + 564u) & 0xFFF7FFFFu);
        }
    }
    v0 = g.S32(fp + 132u);
    return done();
}

// ============================================================================ RASHCDG 0x800BCD48
bool AiBrainPass(GuestRam& g, int32_t dt, uint32_t skip, uint32_t maskC, uint32_t sp, AiBrainCallees& c) {
    const uint32_t fp = sp - kAiBrainPassFrame;
    uint32_t e = g.U32(kAiBrainPool0Ptr);
    for (int32_t i = 0; i < g.S32(kAiBrainLiveCount); ++i, e += 1096u) {
        const uint32_t bit = 1u << (U(i) & 31u);
        if (skip & bit) continue;
        const uint32_t op = g.U16(e + 956u + U(8 * (static_cast<int32_t>(g.S8(e + 946u)) - 1)));
        if (g.S16(e + 320u) == 0) continue;
        if ((g.U32(e + 560u) & 0x18000000u) != 0x08000000u) continue;
        if (g.U8(g.U32(e + 1084u)) & 1u) continue;
        if (op < 3u || op >= 18u) continue;
        int32_t keep = 0;
        if (!AiBrain(g, e, fp, c, keep)) return false;
        const uint32_t f = g.U32(e + 564u);
        if (!(f & 0x200u)) continue;
        if (keep != 0 || Iabs(g.S32(e + 676u)) >= 2130) {
            g.W32(e + 564u, f & ~0x200u);
            g.W32(e + 704u, 0);
        } else {
            g.W32(e + 704u, (maskC & bit) ? U(Sub(g.S32(e + 704u), dt)) : 0xC000u);
        }
        if (g.S32(e + 704u) < 0) {
            const uint32_t f2 = g.U32(e + 564u);
            g.W32(e + 704u, 0);
            g.W32(e + 564u, f2 & ~0x200u);
        }
    }
    return !g.Faulted();
}

} // namespace rr::sim
