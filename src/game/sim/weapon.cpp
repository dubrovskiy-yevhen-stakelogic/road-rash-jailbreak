// Weapons in the hand (weapon.h), transcribed from our own listings of RASHCDG.BIN and
// SLUS_010.53; each store in the original's order.
#include "game/sim/weapon.h"

#include "game/sim/population.h"
#include "game/sim/traffic_bind.h"

namespace rr::sim::weapon {
namespace {
constexpr uint32_t kGameStatePtr = 0x8005B2F8;
constexpr uint32_t kRiderBike = 0x254, kBikeDef = 0x43C;
// BankSwitch (SLUS 0x80012858) is six stores and never reaches the animation machine's pose side; the
// machine still wants a seam object. Reaching either member would be a port error: it faults the view.
struct NoPoseSide final : AnimPoseSeam {
    explicit NoPoseSide(GuestRam& g) : g_(g) {}
    uint32_t TransitionCapture(uint32_t, uint32_t, uint32_t) override { return Fault(); }
    uint32_t ApplyFrame(uint32_t) override { return Fault(); }
    uint32_t Fault() {
        (void)g_.U32(0x1F801000u); // I/O space: a fault in this view
        return 0;
    }
    GuestRam& g_;
};
}

bool WeaponObject(GuestRam& g, WeaponCallees& c, uint32_t r, uint32_t side, const SpineIo& io, uint32_t& v0) {
    v0 = 0;
    if (g.U32(kObjBits) == 0xFFu) return !g.Faulted();                     // 0x80095918
    uint32_t s0 = kObjSlots;
    for (uint32_t i = 0; i < 8u; ++i, s0 += kObjBytes) {
        const uint32_t bits = g.U32(kObjBits);                             // 0x80095974, reloaded each pass
        const uint32_t bit = 1u << i;
        if (bits & bit) continue;
        const uint32_t bike = g.U32(r + kRiderBike);                       // 0x80095988
        g.W32(kObjBits, bits | bit);
        g.W8(r + 0x23Bu, static_cast<uint8_t>(i));
        const uint32_t w = g.U8(g.U32(bike + kBikeDef) + 46u);
        if (w == 0u || w == 4u) {                                          // 0x800959A4: the two with moving parts
            if (g.U32(kFreeFarFlag) != 0u) {
                const uint32_t players = g.U32(g.U32(kGameStatePtr) + 0x30u);
                if (g.U16(bike + 0xACu) < players && g.U32(kAnimDesc + 8u) == g.U32(kAnimDesc + 12u)) {
                    uint32_t ignored = 0;
                    if (!c.FreeFarRider(ignored)) return false;            // 0x800959F8
                }
            }
            const uint32_t o = ViewSlot(g, kAnimDesc, s0);                 // 0x80095A04
            g.W32(r + 0x22Cu, o);
            if (o == 0u) {                                                 // 0x80095928
                const uint32_t sh = static_cast<uint32_t>(static_cast<int32_t>(g.S8(r + 0x23Bu))) & 31u;
                g.W32(kObjBits, g.U32(kObjBits) & ~(1u << sh));
                g.W8(r + 0x23Bu, 0xFF);
                v0 = 0;
                return !g.Faulted();
            }
            NoPoseSide none(g);
            AnimMachine m(g, none);
            m.BankSwitch(o, g.U32(kWeaponBank));                           // 0x80095A20
        }
        const uint32_t w2 = g.U8(g.U32(g.U32(r + kRiderBike) + kBikeDef) + 46u); // 0x80095A38
        if (static_cast<int32_t>(w2) != static_cast<int32_t>(g.S8(s0 + 8u))) LodSelect(g, s0, w2);
        Attach(g, r, s0, side != 0u ? 7 : 10, 0);                          // 0x80095A68
        if (g.U8(g.U32(g.U32(r + kRiderBike) + kBikeDef) + 47u) != 0u) {   // 0x80095A80: swings left
            ObjectEffect(g, s0, 0, 1500, 6, 0, io);
            g.W8(r + 0x23Cu, static_cast<uint8_t>(g.U8(r + 0x23Cu) | 0x80u));
        }
        v0 = 1;
        return !g.Faulted();
    }
    v0 = 0;
    return !g.Faulted();
}

bool OverlayClip(AnimMachine& anim, uint32_t ev, uint32_t r, uint32_t flags, uint32_t rate, uint32_t& v0) {
    GuestRam& g = anim.ram();
    const uint32_t o = g.U32(r + 0x22Cu);                                  // 0x800C2F94
    if (o == 0u) return !g.Faulted();                                      // v0 untouched
    const uint32_t e = ev & 0xFFFFu;
    if (g.U16(r + 0x220u) == e) {                                          // 0x800C2FAC
        v0 = anim.Restart(o);
    } else if (((ev - 145u) & 0xFFFFu) < 41u) {                            // 0x800C2FC8
        v0 = anim.HardStart(g.U32(r + 0x22Cu), e - 145u, flags & 0xFFu, rate, 0);
    } else {
        v0 = 0;                                                            // the sltiu's 0
    }
    return !anim.Failed();
}

void ObjectEffect(GuestRam& g, uint32_t e, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t flags, const SpineIo& io) {
    if (flags & 2u) {                                                      // 0x80027420
        const uint32_t h = g.U16(e + 0xACu);
        const int32_t players = g.S32(g.U32(kGameStatePtr) + 0x30u);
        if (!((h >> 5) < 2u) || !(static_cast<int32_t>(h & 31u) < players)) return;
    }
    const int32_t idx = EffectFindFree(g);                                 // 0x80027460
    if (idx == -1) return;
    const uint32_t rec = kEffectPool + kEffectRecordBytes * static_cast<uint32_t>(idx);
    uint32_t w = g.U32(rec);
    g.W8(rec + 60u, static_cast<uint8_t>(flags));                          // 0x800274A0
    w = (w & 0xFFFFFC3Fu) | ((a3 & 15u) << 6);
    g.W32(rec, w);                                                         // 0x800274B4
    w = (w & 0xFFC03FFFu) | ((a1 & 0xFFu) << 14);
    w = (w & 0xFFFFC3FFu) | 0x400u;
    g.W32(rec, w);                                                         // 0x800274DC
    g.W32(rec + 52u, a2);                                                  // 0x800274E8
    g.W32(rec + 48u, g.U32(g.U32(kGameStatePtr) + 0x10u));                 // 0x800274F0 (delay slot)
    EffectJitterSpray(g, rec, 120, io);                                    // 0x800274EC
    g.W32(rec + 36u, 0);
    g.W32(rec + 44u, 0);
    g.W32(rec + 40u, 0);
    g.W8(rec + 61u, 0);
    g.W16(rec + 62u, 30);                                                  // 0x80027514 (delay slot)
    EffectLink(g, e, static_cast<uint32_t>(idx));                          // 0x80027510
}

} // namespace rr::sim::weapon
