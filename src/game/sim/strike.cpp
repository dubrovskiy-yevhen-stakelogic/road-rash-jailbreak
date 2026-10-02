// RASHCDG 0x800C1370 Strike, ported from our own disassembly of RASHCDG.BIN - see strike.h.
#include "game/sim/strike.h"

#include "game/sim/ai.h"

namespace rr::sim::fight {
namespace {

constexpr uint32_t U(int32_t v) { return static_cast<uint32_t>(v); }
constexpr int32_t S(uint32_t v) { return static_cast<int32_t>(v); }
inline int32_t Iabs(int32_t x) { // `sra; addu; xor`
    const uint32_t s = U(x >> 31);
    return S((U(x) + s) ^ s);
}
int32_t Project(GuestRam& g, uint32_t p, uint32_t axis, uint32_t q) {
    int32_t a[3], o[3];
    int16_t n[3];
    for (uint32_t k = 0; k < 3; ++k) {
        a[k] = g.S32(p + 4u * k);
        o[k] = g.S32(q + 4u * k);
        n[k] = g.S16(axis + 2u * k);
    }
    return AiProject(a, n, o); // RASHCDG 0x800B6AAC, ported (ai.h)
}
uint32_t Category(GuestRam& g, uint32_t stance) { return g.U16(kStanceTab + 8u * (stance & 0xFFFFu) + 2u); }

} // namespace

bool Strike(GuestRam& g, Callees& c, uint32_t me, uint32_t t, StrikeTrace& tr) {
    uint32_t flags = 0;                                              // sw zero,24(sp)
    int32_t lat;                                                     // s0
    const uint32_t troad = g.U32(t + 0x168u);
    if (troad == g.U32(me + 0x168u) && ((troad >> 16) == 0 || g.U32(t + 0x150u) == g.U32(me + 0x150u))) {
        lat = S(g.U32(t + 0x158u) - g.U32(me + 0x158u));             // 0x800C13D4
        if (g.S32(me + 0x16Cu) < 0) lat = S(0u - U(lat));
    } else {
        lat = Project(g, t + 0xB8u, me + 0x1B0u, me + 0xB8u);        // 0x800C1400
    }
    const int32_t along = Project(g, me + 0x1F8u, t + 0x210u, t + 0x1F8u); // s4, 0x800C1414
    const uint32_t side = (lat > 0 ? 1u : 0u) << 8;                   // s2
    const int32_t alat = Iabs(lat);                                   // s0
    const uint32_t r = g.U32(me + 0x354u);
    if (Category(g, g.U16(r + 0x220u)) == 3) {                        // 0x800C1454
        g.W8(g.U32(me + 0x43Cu) + 0x3Cu, 71);                          // the delay slot of jal 0x800C09B0
        tr.stepped = true;
        uint32_t fired = 0;
        if (!FightStep(g, c, me, fired)) return false;
        if (fired != 0) flags = 32;
    } else if (Iabs(along) <= 0xFFFE) {                               // `slt v0,0xfffe,|along|`
        const uint32_t rd = g.U32(me + 0x43Cu);
        if (alat < S(g.U32(rd + 8u) + g.U32(me + 0x130u))) {          // 0x800C14A0
            g.W8(rd + 0x3Cu, 71);                                      // the delay slot of jal 0x800C122C
            tr.restarted = true;
            if (!FightRestart(g, c, me, t, side)) return false;
            flags = 48;
        }
    }
    if (g.Faulted()) return false;
    if (flags == 0) {                                                 // 0x800C1518
        const uint32_t r2 = g.U32(me + 0x354u);
        if (g.U32(r2 + 0x25Cu) != 1u) return !g.Faulted();
        const uint32_t st = g.U16(r2 + 0x220u);
        if (Category(g, st) == 3) return !g.Faulted();
        if (st == 16) {
            uint32_t done = 0;
            if (!c.ClipDone(g.U32(r2 + 0x21Cu), done)) return false;  // 0x8005BE58
            if (done == 0) return !g.Faulted();
        }
        uint32_t v0 = 0;
        tr.lean = true;
        if (!c.StanceEvent(16, g.U32(me + 0x354u), side | 2u, v0)) return false; // 0x800C4550
        return !g.Faulted();
    }
    tr.armed = true;
    if (ReachTest(g, me, t, alat, along, flags)) {                    // 0x800C159C(me, t, |lat|, along, &flags)
        tr.hit = true;
        uint32_t v0 = 0;
        if (!ApplyHit(g, c, me, t, side, flags, v0)) return false;     // 0x800C17B0
    }
    return !g.Faulted();
}

bool Strike(GuestRam& g, Callees& c, uint32_t me, uint32_t t) {
    StrikeTrace tr;
    return Strike(g, c, me, t, tr);
}

} // namespace rr::sim::fight
