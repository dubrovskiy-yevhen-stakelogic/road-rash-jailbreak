// The rider animation objects: the race loader's descriptor and object initialisers and SLUS 0x800119C0's
// hand-out loop (anim_objects.h). Transcribed from our own disassembly; line comments carry the addresses.
#include "game/sim/anim_objects.h"

#include "game/sim/anim.h"
#include "game/sim/population.h" // ViewSlot SLUS 0x80012884

namespace rr::sim {

namespace {

inline uint32_t U(int32_t v) { return static_cast<uint32_t>(v); }
inline int32_t S(uint32_t v) { return static_cast<int32_t>(v); }

// BankSwitch needs no pose.
struct NoPoseSeam final : AnimPoseSeam {
    uint32_t TransitionCapture(uint32_t, uint32_t, uint32_t) override { return 0; }
    uint32_t ApplyFrame(uint32_t) override { return 0; }
};

} // namespace

// ============================================================================ RASHCDI 0x8005D130
void AnimDescInit(GuestRam& g, uint32_t desc) {
    g.W32(desc + 24u, 10);                                          // 0x8005D138
    const int32_t cap = g.S32(desc + 24u);
    g.W32(desc + 0u, 0);
    g.W32(desc + 4u, 0);
    g.W32(desc + 12u, 0);
    g.W32(desc + 8u, 0);
    g.W32(desc + 16u, kAoSlotRecords);                              // 0x8005D158
    g.W32(desc + 20u, 0);
    if (cap <= 0) return;
    int32_t k = 0;
    uint32_t off = 0;
    do {                                                            // 0x8005D168
        const uint32_t s = g.U32(desc + 16u) + off;
        ++k;
        g.W8(s + 1u, 0);
        g.W16(s + 2u, 0);
        g.W32(s + 4u, 0);
        g.W8(s + 0u, 0);
        off += 12u;
        if (g.Faulted()) return;
    } while (k < g.S32(desc + 24u));
}

// ============================================================================ RASHCDI 0x8005D1A0
bool AnimObjectsInit(GuestRam& g, uint32_t desc, int32_t n, uint32_t sp, AnimObjectsCallees& c) {
    const uint32_t F = sp - 32u;                                    // `addiu sp,sp,-32`
    uint32_t v0 = 0;
    if (!c.Malloc(U(n) * 2108u, 0, F, v0)) return false;            // 0x8005D1CC
    const uint32_t objects = v0;
    if (!c.Malloc(U(n) * 60u, 0, F, v0)) return false;              // 0x8005D1E4
    g.W32(desc + 0u, objects);                                      // the delay slot of the second call
    g.W32(desc + 12u, U(n));
    g.W32(desc + 4u, v0);
    g.W32(desc + 8u, 0);                                            // 0x8005D200
    if (n <= 0) return !g.Faulted();
    int32_t k = 0;
    uint32_t po = 0, oo = 0;
    do {                                                            // 0x8005D218
        ++k;
        const uint32_t prog = g.U32(desc + 4u) + po;
        const uint32_t obj = g.U32(desc + 0u) + oo;
        po += 60u;
        g.W32(obj + 36u, 0);
        g.W8(prog + 1u, 5);
        g.W32(obj + 4u, prog);
        g.W32(obj + 8u, 5);
        g.W32(obj + 1760u, 0xFFFFFFFFu);
        g.W32(obj + 1756u, 0);
        g.W32(obj + 44u, 0);
        oo += 2108u;
        if (g.Faulted()) return false;
    } while (k < g.S32(desc + 12u));
    return !g.Faulted();
}

// ============================================================================ RASHCDI 0x80063844..0x800638A0
int32_t AnimObjectCount(GuestRam& g) {
    const uint32_t gs = g.U32(kAnimGameStatePtr);
    const int32_t base = S(g.U32(kAoPool1Count) + (g.U32(kAoPedSwitch) << 2));
    int32_t n = base + 1;
    if (g.U8(gs + 4u) == 8u) n = base + 2;                          // 0x80063874
    if (g.U8(g.U32(kAnimGameStatePtr) + 4u) == 44u) n += 4;         // 0x80063894
    return n;
}

// ============================================================================ SLUS 0x800119C0
bool RaceStartBikes(GuestRam& g, uint32_t sp, RaceStartCallees& c, RaceStartCounts* counts) {
    const uint32_t F = sp - kRaceStartFrame;
    NoPoseSeam noPose;
    AnimMachine m(g, noPose);
    RaceStartCounts n;
    const uint32_t pool = kPopPoolTable;                            // s5
    int32_t left = g.S32(g.U32(pool + 12u));                        // 0x800119EC: *(*(pool + 12))
    uint32_t e = g.U32(pool);                                       // s1
    while (left >= 0) {                                             // 0x800119FC
        const uint32_t r = g.U32(e + 852u);                         // 0x80011A10
        const uint32_t obj = ViewSlot(g, kAnimDescriptor, r);
        g.W32(r + 540u, obj);                                       // 0x80011A20
        ++n.riders;
        if (obj == 0u) ++n.noObject;
        const uint32_t bank = g.U32(kAnimBankTable);
        if (bank != 0u) {                                           // 0x80011A2C
            m.BankSwitch(obj, bank);
            ++n.switched;
        }
        if (g.Faulted()) return false;
        const uint32_t ev = (g.U16(r + 172u) & 1u) ? 4u : 6u;       // 0x80011A44
        if (!c.StanceEvent(ev, r, 1, F)) return false;              // 0x80011A58
        if (g.S16(e + 320u) != 0) {                                 // 0x80011A68: live
            if (!c.EntityCell(e, F)) return false;                  // 0x80011A70
            if (!c.BuildObb(e, F)) return false;                    // 0x80011A78
            ++n.live;
        } else {
            if (!c.Transition(e, 1, F)) return false;               // 0x80011A88
            ++n.dormant;
        }
        const uint32_t cls = g.U32(e + 180u);                       // 0x80011A90
        if ((cls - 6u) < 3u || (cls - 15u) < 3u) {                  // a sidecar class
            const uint32_t pr = g.U32(g.U32(e + 856u) + 852u);      // 0x80011AB4: the passenger's rider
            const uint32_t po = ViewSlot(g, kAnimDescriptor, pr);
            g.W32(pr + 540u, po);                                   // 0x80011ACC
            ++n.passengers;
            if (po == 0u) ++n.noObject;
            const uint32_t pbank = g.U32(kAnimBankTable + 24u);
            if (pbank != 0u) m.BankSwitch(po, pbank);               // 0x80011AE0
            if (g.Faulted()) return false;
            if (g.U8(pr + 572u) & 0x20u)                            // 0x80011AE8
                if (!c.StanceEvent(77, pr, 1, F)) return false;     // 0x80011B00
        }
        --left;                                                     // 0x80011B08
        e += g.U32(pool + 4u);
        if (g.Faulted()) return false;
    }
    if (counts != nullptr) *counts = n;
    return !g.Faulted();
}

bool RaceStart(GuestRam& g, uint32_t sp, RaceStartCallees& c) {
    const uint32_t F = sp - kRaceStartFrame;
    if (!RaceStartBikes(g, sp, c)) return false;
    if (!c.Tail(0x800A41EC, 0, 0, F)) return false;                 // 0x80011B18
    if (!c.Tail(0x800A4774, 1, 0, F)) return false;                 // 0x80011B20
    if (!c.Tail(0x8008D89C, 0, 0, F)) return false;                 // 0x80011B28
    if (!c.Tail(0x8009C308, 0, 0, F)) return false;                 // 0x80011B30
    if (!(g.U8(g.U32(kAnimGameStatePtr) + 4u) & 1u))                // 0x80011B4C
        if (!c.Tail(0x800901A8, 1, 1, F)) return false;
    if (g.U32(0x8005B220u) != 0u) {                                 // 0x80011B6C
        const uint32_t p1 = g.U32(0x8005B38Cu);
        g.W32(p1 + 560u, g.U32(p1 + 560u) | 0x08000000u);          // the delay slot of 0x80011B8C
        if (!c.Tail(0x80012764, 1, 0, F)) return false;
    }
    if (!c.Tail(0x8001654C, 0, 0, F)) return false;                 // 0x80011B94
    if (g.U32(g.U32(kAnimGameStatePtr) + 48u) == 1u) {              // 0x80011BB0
        while (g.U32(0x800CD670u + 36u) == 2u) {                    // 0x80011BC4 / 0x80011BE8
            if (!c.Tail(0x80030608, 0, 0, F)) return false;
            if (!c.Tail(0x800247E8, 0, 0, F)) return false;
            if (g.Faulted()) return false;
        }
    } else {
        if (!c.Tail(0x800164B4, 0, 0, F)) return false;             // 0x80011BF8
    }
    if (!c.Tail(0x8001A424, 0, 0, F)) return false;                 // 0x80011C00
    if (g.U32(0x8005B220u) != 0u)                                   // 0x80011C14
        if (!c.Tail(0x80018C1C, 1, 0, F)) return false;
    return !g.Faulted();
}

} // namespace rr::sim
