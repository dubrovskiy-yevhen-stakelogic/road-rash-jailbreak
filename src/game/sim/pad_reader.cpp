#include "game/sim/pad_reader.h"

#include "game/sim/input.h"

namespace rr::sim {
namespace {

inline uint32_t U(int32_t v) { return static_cast<uint32_t>(v); }

// SLUS 0x8001CA58 AxisCurve(raw, curve) over the guest: the curve's six halfwords and ENV.EN's first
// 180 bytes (the dead zone +0xB0 and the segment width +0xB2 are what it reads).
int32_t GuestAxis(GuestRam& g, uint8_t raw, uint32_t curve) {
    uint16_t c[6];
    for (uint32_t k = 0; k < 6; ++k) c[k] = g.U16(curve + 2u * k);
    uint8_t cfg[180];
    g.ReadBlock(kPadEnvCfg, cfg, sizeof(cfg));
    return AxisCurve(raw, c, cfg);
}

} // namespace

bool PadRiderControls(GuestRam& g, uint32_t p, uint32_t rec, uint32_t bike, uint32_t live, uint32_t& exit) {
    const uint32_t axes = kPadAxesTable + 8u * p;
    auto stamp = [&](uint32_t slot) { return g.S32(rec + 0x14u + 8u * slot); };
    auto code = [&](uint32_t slot) { return g.S8(rec + 0x1Au + 8u * slot); };
    // 0x8001CFB0: the analogue axes.
    if (g.U32(rec + 0x10u) != 0) {
        g.W32(axes + 0u, U(GuestAxis(g, g.U8(rec + 9u), 0x800D3984u)));
        g.W32(axes + 4u, U(GuestAxis(g, g.U8(rec + 10u), 0x800D3978u)));
    }
    const uint32_t rider = g.U32(bike + 0x354u);                               // 0x8001CFF8
    if (g.S16(rider + 0x140u) == 0) {
        exit = kPadExitIdle;
        return !g.Faulted();
    }
    if (g.U32(rider + 0x25Cu) < 2u) {                                          // on the bike
        uint32_t t0 = 0, a2 = 0, a0 = 0, v0 = 0;
        if (g.U32(rec + 0x10u) != 0) {                                         // 0x8001D034: analogue
            const int8_t s15 = g.S8(rec + 0x92u);
            a2 = (0 < s15) ? 1u : 0u;
            if (g.S8(rec + 0x9Au) > 0) a2 |= 0x20u;
            if (!(g.S8(rec + 0xAAu) < 2)) a2 |= 4u;
            a0 = 0;
            if (!(s15 < 2)) {
                if (g.S8(bike + 0x350u) > 0) {
                    a0 = 1;
                    a2 |= 0xCu;
                }
            }
            v0 = a0 << 3;                                                      // every branch's delay slot
            if (!(g.S8(rec + 0x9Au) < 2) && g.S32(rec + 0xA4u) > 0) a2 |= 0x400u; // 0x8001D0B0
        } else {                                                               // 0x8001D0B4: digital
            uint32_t a1 = g.U32(rec + 0xB8u);
            const uint32_t a3 = g.U32(bike + 0x230u);
            a0 = (0 < stamp(g.U32(a1 + 12u))) ? 1u : 0u;                       // control 3
            if (!((a3 >> 1) & 1u)) a2 = (0u - a0) & 3u;
            t0 = ~(0u - a0) & 2u;
            a0 = (0 < stamp(g.U32(a1 + 16u))) ? 1u : 0u;                       // control 4
            if (!((a3 >> 6) & 1u)) a2 |= (0u - a0) & 0x60u;
            if (!a0) t0 |= 0x40u;
            // controls 5 and 6: the steering pairs, with rec+0xB4 == 2's override (0x8001D128, 0x8001D1F4)
            auto steer = [&](uint32_t word) -> uint32_t {
                if (g.U32(rec + 0xB4u) == 2u) {
                    if (stamp(g.U32(g.U32(rec + 0xB8u) + word)) > 0) {
                        if (g.S32(rec + 0x2Cu) > 0) return 0;
                        if (g.S32(rec + 0x24u) > 0) return 1;
                    }
                }
                return (0 < stamp(g.U32(g.U32(rec + 0xB8u) + word))) ? 1u : 0u;
            };
            for (uint32_t k = 0; k < 2; ++k) {
                a0 = steer(k == 0 ? 20u : 24u);
                const uint32_t bit = k == 0 ? 8u : 9u;
                const uint32_t v1 = (g.U32(bike + 0x230u) >> bit) & 1u;
                uint32_t b = 0;
                if (!v1) b = (0u - a0) & (k == 0 ? 0x180u : 0x280u);
                if (!a0) a2 |= b + (v1 << 7);
                else a2 |= b;
                t0 |= ~(0u - a0) & (k == 0 ? 0x100u : 0x200u);
            }
            a1 = g.U32(rec + 0xB8u);
            if (!(code(g.U32(a1 + 12u)) < 2)) a2 |= 4u;                         // 0x8001D2D0
            a0 = (0 < stamp(g.U32(a1 + 28u))) ? 1u : 0u;                       // control 7
            if (a0 && !(code(g.U32(a1 + 16u)) < 2)) a2 |= 0x400u;
            v0 = a0 << 3;
        }
        a2 |= v0;                                                              // 0x8001D32C
        if (!a0) t0 |= 8u;
        g.W32(bike + 0x230u, (g.U32(bike + 0x230u) & ~t0) | a2);               // 0x8001D34C
        exit = kPadExitMerged;
        return !g.Faulted();
    }
    // 0x8001D5B0: off the bike - the walk.
    exit = kPadExitCamera;
    if (g.U32(g.U32(bike + 0x43Cu) + 0x28u) != 0) return !g.Faulted();
    const uint32_t t0 = g.U32(rec + 0x10u);
    const uint16_t cat = g.U16(0x800541D4u + 8u * g.U16(rider + 0x220u) + 2u);
    uint32_t a1 = 0, a2 = 0;
    if (cat == 8u) {
        uint32_t a0 = ((g.S32(rec + 0x24u) > 0) || (g.S32(rec + 0x4Cu) > 0)) ? 0x100u : 0u;
        if (g.S32(rec + 0x14u) > 0) a0 |= 0x200u;
        uint32_t v0 = ((g.S32(rec + 0x2Cu) > 0) || (g.S32(rec + 0x34u) > 0)) ? 0x80u : 0u;
        if (g.S32(rec + 0x1Cu) > 0) v0 |= 0x400u;
        a1 = a0 | v0;
        if (a1 != 0) a2 = 1;
        else if (t0 != 0 && (g.S32(kPadAxesTable + 8u * p) != 0 || g.S32(kPadAxesTable + 8u * p + 4u) != 0)) a2 = 1;
        if (!(g.S32(rec + 0x44u) > 0)) g.W32(rider + 0x228u, g.U32(rider + 0x228u) & ~0x1000u);   // 0x8001D69C
    }
    if (g.S8(rec + 0x4Au) > 0) {                                               // 0x8001D6AC: slot 6's press
        a1 |= 0x1000u;
        g.W8(live + 72u, 0xFFu);
        g.W8(live + 74u, 1u);
    }
    g.W32(rider + 0x228u, (g.U32(rider + 0x228u) & 0xFFFFF07Fu) | a1 | ((0u - a2) & 0x1000800u));
    return !g.Faulted();
}

// ============================================================================ SLUS 0x8001D51C..0x8001D5AC
bool PadTauntPress(GuestRam& g, uint32_t rec, uint32_t bike, uint32_t& handle, uint32_t& v0) {
    const uint32_t slot = g.U32(g.U32(rec + 0xB8u) + 0x20u);                 // lw 184(s2); lw 32(v0)
    const int32_t code = g.S8(rec + (slot << 3) + 0x1Au);                    // lb 26(s2 + slot * 8)
    v0 = static_cast<uint32_t>(code);
    if (code <= 0) return false;                                             // blez -> 0x8001D550
    handle = g.U16(bike + 0xACu);                                            // lhu a0,172(s3)
    return true;
}

PadTauntRegs PadTauntHeld(GuestRam& g, uint32_t rec, uint32_t bike) {
    PadTauntRegs r;
    const uint32_t slot = g.U32(g.U32(rec + 0xB8u) + 0x20u);
    const int32_t held = g.S32(rec + (slot << 3) + 0x14u);                   // lw 20(s2 + slot * 8)
    const uint32_t rider = g.U32(bike + 0x354u);
    if (held > 0) {                                                          // 0x8001D578
        r.a0 = 0x800000u;
        r.v0 = rider;
        r.v1 = g.U32(rider + 0x228u) | 0x800000u;
        g.W32(rider + 0x228u, r.v1);
    } else {                                                                 // 0x8001D594
        r.a0 = rider;
        r.v1 = 0xFF7FFFFFu;
        r.v0 = g.U32(rider + 0x228u) & 0xFF7FFFFFu;
        g.W32(rider + 0x228u, r.v0);
    }
    return r;
}

} // namespace rr::sim
