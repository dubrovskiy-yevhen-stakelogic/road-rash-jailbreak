// RASHCDG 0x8007EC30 RowsFromHeading and 0x800C3104 RiderDismount (traffic.h), line by line from
// our own listing of RASHCDG.BIN (SHA-1 cfe43a7786759f2cb9c57751cf99e84d1074782c).
#include "game/sim/traffic.h"

#include "game/sim/integrator.h" // OuterProduct: the GTE OP (sf = 1, lm = 0)

namespace rr::sim {

// ============================================================================ RASHCDG 0x8007EC30
uint32_t RowsFromHeading(GuestRam& g, uint32_t e) {
    const uint16_t h0 = g.U16(e + 450);                                        // 0x8007EC30
    const uint32_t sl = g.U32(e + 340);                                        // 0x8007EC34
    const uint16_t h1 = g.U16(e + 452), h2 = g.U16(e + 454);
    g.W16(e + 528, h0);                                                        // 0x8007EC40
    g.W16(e + 530, h1);
    g.W16(e + 532, h2);                                                        // 0x8007EC48
    g.W16(e + 522, static_cast<uint16_t>(0u - g.U16(sl + 8)));                 // 0x8007EC58
    g.W16(e + 524, static_cast<uint16_t>(0u - g.U16(sl + 10)));                // 0x8007EC68
    g.W16(e + 526, static_cast<uint16_t>(0u - g.U16(sl + 12)));                // 0x8007EC78
    // 0x8007EC80..0x8007ECD8: R11/R22/R33 = the up row, IR = the forward row, OP, IR -> +0x204.
    const int16_t d[3] = {g.S16(e + 522), g.S16(e + 524), g.S16(e + 526)};
    const int16_t ir[3] = {g.S16(e + 528), g.S16(e + 530), g.S16(e + 532)};
    int16_t o[3];
    OuterProduct(d, ir, o);
    g.W16(e + 516, static_cast<uint16_t>(o[0]));                              // 0x8007ECD0
    g.W16(e + 518, static_cast<uint16_t>(o[1]));
    g.W16(e + 520, static_cast<uint16_t>(o[2]));                              // 0x8007ECD8
    uint16_t row[9];
    for (uint32_t k = 0; k < 9; ++k) row[k] = g.U16(e + 516 + 2u * k);         // 0x8007ECDC..0x8007ECFC
    const uint32_t second = g.U32(e + 856);                                    // 0x8007ED00
    g.W32(e + 676, 0);                                                         // 0x8007ED04
    g.W32(e + 616, 0);
    g.W32(e + 636, 0);
    g.W32(e + 672, 0);
    g.W32(e + 488, 0);                                                         // 0x8007ED14
    for (uint32_t k = 0; k < 9; ++k) g.W16(e + 432 + 2u * k, row[k]);          // 0x8007ED24..0x8007ED44
    g.W16(e + 814, row[0]);                                                    // 0x8007ED48
    g.W16(e + 816, row[1]);
    g.W16(e + 818, row[2]);                                                    // 0x8007ED54 (delay slot)
    if (second != 0) g.W32(second + 488, 0);                                   // 0x8007ED58
    return row[0];                                                             // v0: lhu 516(a0)
}

// ============================================================================ RASHCDG 0x800C3104
bool RiderDismount(GuestRam& g, uint32_t r, int32_t how, uint32_t sp, DismountCallees& c) {
    const uint32_t F = sp - 32;                                                // addiu sp,sp,-32
    const int16_t live = g.S16(r + 320);                                       // 0x800C3118
    g.W32(r + 604, 0);                                                         // 0x800C3120
    g.W16(r + 608, 0);                                                         // 0x800C3128 (delay slot)
    if (g.Faulted()) return false;
    if (live != 0) {
        const uint16_t cur = g.U16(r + 544);                                   // 0x800C312C
        if (cur == 6 || cur == 77) return !g.Faulted();                        // 0x800C3134 / 0x800C313C
        const uint32_t ev = (g.U8(r + 572) & 0x20u) ? 77u : 6u;               // 0x800C3144..0x800C3158
        if (g.Faulted() || !c.StanceEvent(ev, r, 16, F)) return false;         // 0x800C3160
        return !g.Faulted();
    }
    if (!c.StanceLeave(224, r, 0, F)) return false;                            // 0x800C3178
    const uint32_t a = g.U32(r + 540);                                         // 0x800C3180
    if (a != 0 && (g.Faulted() || !c.AnimStop(a, F))) return false;            // 0x800C3190
    if (how != 0) {
        g.W16(r + 544, 224);                                                   // 0x800C31A4 (delay slot)
    } else {
        g.W16(r + 544, 73);                                                    // 0x800C31AC
        g.W32(r + 604, 4);                                                     // 0x800C31B4
    }
    return !g.Faulted();
}

} // namespace rr::sim
