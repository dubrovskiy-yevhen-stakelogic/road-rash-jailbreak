#include "game/sim/pad_motor.h"

#include "game/sim/fixed.h"

namespace rr::sim {

namespace {
constexpr uint32_t kGameStatePtr = 0x8005B2F8;
constexpr uint32_t kAttract = 0x8005B220;
int32_t Clock(GuestRam& g) { return g.S32(g.U32(kGameStatePtr) + 0x0Cu); }
uint32_t Rec(uint32_t pad) { return kPadPorts + kPadPortBytes * pad; } // sll 1; addu; sll 3 = 24 pad
} // namespace

// SLUS 0x8001DC94..0x8001DD04.
void PadMotorSmall(GuestRam& g, uint32_t pad, int32_t frames) {
    const uint32_t r = Rec(pad);
    if (g.U32(r + 8u) != 0 && frames >= 0) {                      // beqz v0 / bltz a1
        g.W8(r + 0x0Cu, 1);
        if (frames == -1) {                                       // (unreachable past the bltz, kept as written)
            g.W32(r + 0x10u, 0);
            return;
        }
        g.W32(r + 0x10u, static_cast<uint32_t>(Clock(g)) + static_cast<uint32_t>(frames));
        return;
    }
    g.W8(r + 0x0Cu, 0);                                           // 0x8001DCFC
    g.W32(r + 0x10u, 0);
}

// SLUS 0x8001DD08..0x8001DD70.
void PadMotorLarge(GuestRam& g, uint32_t pad, int32_t frames, int32_t strength) {
    const uint32_t r = Rec(pad);
    if (g.U32(r + 8u) != 0 && strength != 0) {                   // beqz v0 / beqz a2
        g.W8(r + 0x0Du, static_cast<uint8_t>(strength));          // the delay slot of `bne a1,-1`
        if (frames == -1) {
            g.W32(r + 0x14u, 0);
            return;
        }
        g.W32(r + 0x14u, static_cast<uint32_t>(Clock(g)) + static_cast<uint32_t>(frames));
        return;
    }
    g.W8(r + 0x0Du, 0);                                           // 0x8001DD68
    g.W32(r + 0x14u, 0);
}

// SLUS 0x8001DD74..0x8001DDC0: DC94(a0, a1, 1) - the third argument is not read - then DD08(a0, a2, a3).
void PadMotor(GuestRam& g, uint32_t pad, int32_t smallFrames, int32_t largeFrames, int32_t strength) {
    PadMotorSmall(g, pad, smallFrames);
    PadMotorLarge(g, pad, largeFrames, strength);
}

// SLUS 0x8001DDC4..0x8001DF60.
bool PadActuatorService(GuestRam& g, uint32_t pad, uint32_t single, LibPad& lp) {
    if (pad == 1) g.W32(kPadPorts + kPadPortBytes, single == 0 ? 0x10u : 1u); // 0x8001DDD8..0x8001DDF8
    const uint32_t s0 = Rec(pad);
    const uint32_t port = g.U32(s0);
    for (uint32_t i = 0; i < 2; ++i) {                            // 0x8001DE20..0x8001DE60
        const uint32_t stop = g.U32(s0 + 0x10u + 4u * i);
        if (stop != 0 && static_cast<int32_t>(stop) < Clock(g)) {
            g.W8(s0 + 0x0Cu + i, 0);
            g.W32(s0 + 0x10u + 4u * i, 0);
        }
    }
    const int32_t st = lp.GetState(port);                         // 0x8001DE68
    if (st == 0) {
        g.W32(s0 + 4u, 0);
        return !g.Faulted();
    }
    if (st == 1) {                                                // 0x8001DE8C..0x8001DEB8
        g.W32(s0 + 4u, 0);
        g.W8(s0 + 0x0Du, 0);
        g.W8(s0 + 0x0Cu, 0);
        g.W32(s0 + 0x14u, 0);
        g.W32(s0 + 0x10u, 0);
    }
    if (g.U32(s0 + 4u) == 0) {                                    // 0x8001DEBC
        lp.SetAct(port, s0 + 0x0Cu, 2);
        if (st == 2) {
            g.W32(s0 + 4u, 2);
            return !g.Faulted();
        }
        if (st != 6) return !g.Faulted();
        if (lp.SetActAlign(port, g.gp() + kPadAlignGp) != 0) g.W32(s0 + 4u, 1);
        return !g.Faulted();
    }
    if (lp.InfoMode(port, 2, 0) != 0) return !g.Faulted();        // 0x8001DF10
    g.W8(s0 + 0x0Cu, 0x40);
    g.W8(s0 + 0x0Du, g.U32(s0 + 0x10u) != 0 ? 1u : 0u);
    return !g.Faulted();
}

// SLUS 0x8001CB3C, [0x8001D7DC, 0x8001DA08).
PadRumbleRegs PadEngineRumble(GuestRam& g, uint32_t p, uint32_t bike, uint32_t cnt, uint32_t s0In) {
    PadRumbleRegs r;
    r.s0 = s0In;
    if (g.U32(kAttract) != 0) return r;                           // 0x8001D7E8 -> 0x8001DA2C
    if (g.U32(Rec(p) + 0x14u) != 0) {                             // 0x8001D808 -> 0x8001DA28
        g.W32(cnt, 0);
        return r;
    }
    const uint32_t slow = (0x23C35 < g.S32(bike + 0x1E0u)) ? 0u : 1u; // s0 (the delay slot of 0x8001D824)
    uint32_t s0 = slow;
    int32_t a1 = 0;
    int32_t a0 = g.S32(bike + 0x2A4u);
    if (a0 != 0) {
        const int32_t sg = a0 >> 31;
        a0 = static_cast<int32_t>((static_cast<uint32_t>(sg) + static_cast<uint32_t>(a0)) ^ static_cast<uint32_t>(sg));
        const int32_t k = g.S32(g.U32(bike + 0x22Cu) + 0xCCu);
        auto neg = [](int32_t v) { return static_cast<int32_t>(0u - static_cast<uint32_t>(v)); };
        if (a0 > 0) {                                             // 0x8001D840
            if (k > 0) a1 = static_cast<int32_t>(FixDiv(static_cast<uint32_t>(a0), static_cast<uint32_t>(k)));
            else a1 = neg(static_cast<int32_t>(FixDiv(static_cast<uint32_t>(a0), static_cast<uint32_t>(neg(k)))));
        } else {                                                  // 0x8001D860 (|INT_MIN|)
            if (k <= 0)
                a1 = static_cast<int32_t>(FixDiv(static_cast<uint32_t>(neg(a0)), static_cast<uint32_t>(neg(k))));
            else a1 = neg(static_cast<int32_t>(FixDiv(static_cast<uint32_t>(neg(a0)), static_cast<uint32_t>(k))));
        }
    }
    if (0x11FFF < g.S32(bike + 0x2BCu) && a1 < 0x8000) a1 = 0x8000; // 0x8001D8A0..0x8001D8C4
    auto off = [&]() {                                            // 0x8001D9F0..0x8001DA04
        PadMotorLarge(g, p, -1, 0);
        g.W32(cnt, 0);
        r.s0 = s0;
        r.strength = 0;
        return r;
    };
    if (g.S16(bike + 0x140u) == 0) return off();
    if (!(g.U32(g.U32(bike + 0x354u) + 0x25Cu) < 2u)) return off();
    if (a1 != 0) {                                                // 0x8001D93C
        uint32_t v = static_cast<uint32_t>(a1) * 145u;
        if (static_cast<int32_t>(v) < 0) v += 0xFFFFu;
        s0 = static_cast<uint32_t>((static_cast<int32_t>(v) >> 16) + 110);
        g.W32(cnt, 0);
    } else {
        const bool flag = (g.U32(bike + 0x184u) & 1u) != 0;
        if (!(flag && s0 == 0)) {                                 // 0x8001D8FC..0x8001D910
            if (!(g.U32(bike + 0xB4u) < 9u)) return off();        // 0x8001D918
            if (s0 == 0) return off();
        }
        if (s0 == 0) {                                            // 0x8001D968 (the delay slot's s0 = 0)
            s0 = 0;
        } else {
            const uint32_t n = g.U32(cnt) + 1u;
            g.W32(cnt, n);
            s0 = static_cast<int32_t>(n) < 601 ? 50u : 0u;
        }
    }
    if (g.U32(bike + 0x184u) & 1u) {                              // 0x8001D990
        int32_t v1 = FixMul(g.S32(bike + 0x1E0u), 0x78000) >> 16;
        if (!(v1 < 151)) v1 = 150;
        int32_t a2 = static_cast<int32_t>(s0);
        if (!(v1 < a2)) a2 = v1;
        g.W32(cnt, 0);
        s0 = static_cast<uint32_t>(a2);
    }
    PadMotorLarge(g, p, -1, static_cast<int32_t>(s0));            // 0x8001D9E0
    r.s0 = s0;
    r.strength = static_cast<int32_t>(s0);
    return r;
}

// SLUS 0x8001CB3C, [0x8001DA08, 0x8001DA2C).
PadRumbleRegs PadMotorsOff(GuestRam& g, uint32_t p, uint32_t cnt, uint32_t s0) {
    PadRumbleRegs r;
    r.s0 = s0;
    PadMotorLarge(g, p, -1, 0);
    PadMotorSmall(g, p, -1);
    g.W32(cnt, 0);
    r.strength = 0;
    return r;
}

} // namespace rr::sim
