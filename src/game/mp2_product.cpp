// The two-player leftovers in the product (mp2_product.h).
#include "game/mp2_product.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "game/shadow_product.h"
#include "game/sim/mp_world.h"
#include "game/sim/shadow2p.h"
#include "game/sim/world_pop.h" // CellSlotFor, OtherSlot (the counter only)

namespace rr::game {

bool Mp2On() {
    static const bool on = !(std::getenv("RRJB_MP2") != nullptr && std::strcmp(std::getenv("RRJB_MP2"), "off") == 0);
    return on;
}

Mp2Totals& Mp2() {
    static Mp2Totals t;
    return t;
}

bool Mp2CellCopy(rr::sim::GuestRam& g, uint32_t slotPlus4, uint32_t p) {
    if (!Mp2On()) return false;
    Mp2Totals& t = Mp2();
    ++t.cellCopyCalls;
    const uint32_t id = g.U32(slotPlus4 + 4u);
    if ((g.U8(g.U32(g.gp() + 1644u) + 4u) & 0x10u) != 0 && rr::sim::CellSlotFor(g, id, p) != 0 &&
        rr::sim::OtherSlot(g, id, p) != 0)
        ++t.cellCopyRan;
    rr::sim::mp::CellStateCopy(g, id, p);
    return true;
}

bool Mp2Class50Release(rr::sim::GuestRam& g, uint32_t id, uint32_t p) {
    if (!Mp2On()) return false;
    Mp2Totals& t = Mp2();
    ++t.class50Calls;
    const uint32_t r = 0x800D43C0u + 28u * p;
    const bool live = g.S16(r + 22u) != 0 && g.U32(r + 24u) == id;
    rr::sim::mp::Class50Release(g, id, p);
    if (live) ++t.class50Freed;
    return true;
}

namespace {
struct SpriteSink final : rr::sim::shadow::Sink2p {
    rr::sim::GuestRam& g;
    ShadowFrame& out;
    uint32_t view;
    SpriteSink(rr::sim::GuestRam& gg, ShadowFrame& o, uint32_t v) : g(gg), out(o), view(v) {}
    void Packet(uint32_t address, int32_t otz4) override {
        FxPacket p;
        p.address = address;
        p.function = rr::sim::shadow::kShadow2pFn;
        p.word1 = g.U32(address + 4u);
        for (uint32_t v = 0; v < 4; ++v) {
            const uint32_t xy = g.U32(address + 8u + 8u * v), uv = g.U32(address + 12u + 8u * v);
            p.x[v] = static_cast<int16_t>(xy & 0xFFFFu);
            p.y[v] = static_cast<int16_t>(xy >> 16);
            p.u[v] = static_cast<uint8_t>(uv);
            p.v[v] = static_cast<uint8_t>(uv >> 8);
            if (v == 0) p.clut = static_cast<uint16_t>(uv >> 16);
            if (v == 1) p.tpage = static_cast<uint16_t>(uv >> 16);
        }
        p.depth = otz4;
        p.view = view;
        static const bool trace = std::getenv("RRJB_MP2_TRACE") != nullptr; // DEVELOPMENT: where the sprite lands
        if (trace)
            std::printf("mp2 shadow: view %u packet 0x%08X otz4 %d xy (%d,%d) (%d,%d) (%d,%d) (%d,%d) uv %u,%u tpage %04X "
                        "clut %04X\n", view, address, otz4, p.x[0], p.y[0], p.x[1], p.y[1], p.x[2], p.y[2], p.x[3], p.y[3],
                        p.u[0], p.v[0], p.tpage, p.clut);
        out.sprites.push_back(p);
        ++Mp2().shadowPackets;
    }
};
} // namespace

void Mp2ShadowTail(rr::sim::GuestRam& g, const rr::sim::shadow::Gte& gte, uint32_t obj, uint32_t view, uint32_t w24,
                   ShadowFrame& out) {
    if (!Mp2On() || !ShadowPortOn()) return;
    SpriteSink sink(g, out, view);
    bool ran = false;
    const bool ok = rr::sim::shadow::EmitterShadow2p(g, gte, obj, view, w24, &sink, ran);
    if (ran) ++Mp2().shadowCalls;
    if (!ok || g.Faulted()) {
        ++Mp2().shadowRefused;
        g.ClearFault();
    }
}

std::string Mp2Report() {
    const Mp2Totals& t = Mp2();
    char b[700];
    std::snprintf(b, sizeof(b),
                  "mp2: the two-player leftovers%s: cell-state copy SLUS 0x8001339C (PORTED) %zu call(s), %zu with both "
                  "players' slots; class-50 release RASHCDG 0x800A3ECC (PORTED) %zu call(s), %zu record(s) released; "
                  "two-player shadow SLUS 0x80026960 (PORTED) %zu call(s), %zu packet(s), %zu refused, %zu drawn; unlit "
                  "ramp (game_state+4 bit 4) in %zu view-frame(s); player 2's own sidecar rig in %zu frame(s)",
                  Mp2On() ? "" : " SWITCHED OFF (RRJB_MP2=off)", t.cellCopyCalls, t.cellCopyRan, t.class50Calls,
                  t.class50Freed, t.shadowCalls, t.shadowPackets, t.shadowRefused, t.shadowDrawn, t.unlitViews,
                  t.rig2Frames);
    return b;
}

} // namespace rr::game
