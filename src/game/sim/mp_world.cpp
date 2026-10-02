// Two players: the cell-state copy SLUS 0x8001339C and the class-50 release RASHCDG 0x800A3ECC (mp_world.h).
#include "game/sim/mp_world.h"

#include "game/sim/world_pop.h" // CellSlotFor 0x80013204, OtherSlot 0x80013360, VolumeMask 0x80013294, Class50 0x80012BA8

namespace rr::sim::mp {

void CellStateCopy(GuestRam& g, uint32_t id, uint32_t p) {
    const uint32_t gsp = g.gp() + 1644u;                                        // 0x8005B2F8
    if ((g.U8(g.U32(gsp) + 4u) & 0x10u) == 0) return;                           // 0x8001339C..0x800133D0
    const uint32_t mine = CellSlotFor(g, id, p);                               // 0x800133D8
    const uint32_t other = OtherSlot(g, id, p);                                 // 0x800133E8
    if (mine == 0 || other == 0) return;                                        // 0x800133F0 / 0x800133F8
    const uint32_t s2 = g.U32(g.U32(mine + 4u) + 32u);                          // 0x80013400..0x80013408
    const uint32_t s3 = g.U32(g.U32(other + 4u) + 32u);                         // 0x8001340C..0x80013414
    // the kind-2 array +0x28 (count +2, 76 bytes)                                 0x80013410..0x80013450
    for (uint32_t i = 0, o = 0; i < g.U16(s2 + 2u); ++i, o += 76u)
        g.W16(g.U32(s3 + 40u) + o + 4u, g.U16(g.U32(s2 + 40u) + o + 4u));
    // the kind-3 array +0x24 (count +0, 68 bytes)                                 0x80013454..0x80013494
    for (uint32_t i = 0, o = 0; i < g.U16(s2); ++i, o += 68u)
        g.W16(g.U32(s3 + 36u) + o + 4u, g.U16(g.U32(s2 + 36u) + o + 4u));
    // the kind-6 array +0x30 (count +6, 88 bytes): a volume handle gets the other player's bit
    for (uint32_t i = 0, o = 0; i < g.U16(s2 + 6u); ++i, o += 88u) {           // 0x80013498..0x800134FC
        g.W16(g.U32(s3 + 48u) + o + 4u, g.U16(g.U32(s2 + 48u) + o + 4u));
        const uint32_t rec = g.U32(s2 + 48u) + o;
        if (g.S16(rec + 4u) > 0) VolumeMask(g, 6u, g.U16(rec + 4u), p ^ 1u);   // 0x800134D8..0x800134E8
    }
    // the kind-4 array +0x2C (count +4, 64 bytes): class 50 in the other copy re-stamps p's class-50 record
    for (uint32_t i = 0; i < g.U16(s2 + 4u); ++i) {                            // 0x80013500..0x80013580
        const uint32_t o = i << 6;
        g.W16(g.U32(s3 + 44u) + o + 4u, g.U16(g.U32(s2 + 44u) + o + 4u));
        if ((g.U8(g.U32(gsp) + 4u) & 0x10u) != 0 && g.U16(g.U32(s3 + 44u) + o + 2u) == 50u)
            Class50(g, s3, id, p);                                              // 0x80013568: a0 = s3 (the header)
    }
    // the kind-0 array +0x34 (count +8, 64 bytes)                                 0x80013584..0x800135C0
    for (uint32_t i = 0; i < g.U16(s2 + 8u); ++i) {
        const uint32_t o = i << 6;
        g.W16(g.U32(s3 + 52u) + o + 4u, g.U16(g.U32(s2 + 52u) + o + 4u));
    }
}

void Class50Release(GuestRam& g, uint32_t id, uint32_t p) {
    const uint32_t r = 0x800D43C0u + 28u * p;                                   // 0x800A3ECC..0x800A3EE0
    if (g.S16(r + 22u) == 0) return;                                            // 0x800A3EE4..0x800A3EEC
    if (g.U32(r + 24u) != id) return;                                           // 0x800A3EF4..0x800A3EFC
    g.W16(r + 22u, 0);                                                          // 0x800A3F04
    g.W32(r + 24u, 0);                                                          // 0x800A3F08
}

} // namespace rr::sim::mp
