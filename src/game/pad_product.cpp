#include "game/pad_product.h"

#include "game/sim/pad_reader.h"
#include "game/pause_product.h" // PauseOn

#include <cmath>
#include <cstdlib>

namespace rr::game {

using rr::sim::GuestRam;

namespace {

// SLUS 0x8001C8F4..0x8001C9FC, one slot: the hold stamp +0, the frame counter +5, the press code
// +6, the state byte +4 and its timer +7, against the record's clock +0x00.
void StampSlot(GuestRam& g, uint32_t rec, uint32_t slot, bool held) {
    const uint32_t a3 = rec + 0x14u + 8u * slot;
    const uint32_t a0 = a3 + 5u;
    if (held) {
        if (!(g.S32(a3) > 0)) {                                          // 0x8001C90C
            g.W32(a3, g.U32(rec));
            g.W8(a0 + 1u, g.U8(a0) < 11u ? 2u : 1u);
            g.W8(a0, 0);
            g.W8(a0 + 2u, 0);
            g.W8(a0 - 1u, 1);
        } else {
            const int8_t v1 = g.S8(a0 - 1u);
            if (v1 >= 0) {
                const uint8_t v0 = g.U8(a0 + 2u);
                if (v1 == 0 ? !(v0 < 3u) : !(v0 < 31u)) {                 // 0x8001C964..0x8001C994
                    g.W8(a0 - 1u, 0);
                    g.W8(a0 + 1u, 0xFFu);
                    g.W8(a0 + 2u, 0);
                }
            }
            g.W8(a0 + 2u, static_cast<uint8_t>(g.U8(a0 + 2u) + 1u));
        }
    } else {                                                             // 0x8001C9AC
        g.W8(a0 - 1u, 0);
        const int32_t s = g.S32(a3);
        if (s > 0) g.W32(a3, static_cast<uint32_t>(s) - g.U32(rec));
    }
    if (g.U8(a0) < 31u) g.W8(a0, static_cast<uint8_t>(g.U8(a0) + 1u)); // 0x8001C9D0
}

} // namespace

PadControlsResult RunPadControls(uint8_t* ram, uint32_t gp, uint32_t bike, const PadDevice& dev, uint32_t p) {
    PadControlsResult r;
    GuestRam g(ram, gp);
    const uint32_t rec = rr::sim::kPadRecordsFrame + 192u * p;
    g.W32(rec + 0x0Cu, dev.analog ? 0x73u : 0x41u);
    g.W32(rec + 0x10u, dev.analog ? 1u : 0u);
    g.W8(rec + 0x08u, dev.ly);
    g.W8(rec + 0x09u, dev.ry);
    g.W8(rec + 0x0Au, dev.lx);
    if (dev.analog) {
        StampSlot(g, rec, 15, dev.ry == 0x00u);
        StampSlot(g, rec, 16, dev.ry == 0xFFu);
        StampSlot(g, rec, 17, dev.ly == 0x00u);
        StampSlot(g, rec, 18, dev.ly == 0xFFu);
    }
    // 0x8001CF8C: racing (the state byte 1 - not paused, not in the results; RRJB_PAUSE=off: any state) and
    // 0x8001CF00: under pad control
    const bool racing = g.U8(g.U32(0x8005B2F8u)) == 1u || !PauseOn();
    if (racing && !(g.U32(bike + 0x230u) & 0x08000000u)) {
        r.ran = true;
        r.ok = rr::sim::PadRiderControls(g, p, rec, bike, rec, r.exit);
    }
    const uint32_t rider = g.U32(bike + 0x354u);
    r.riderFlags = g.U32(rider + 0x228u);
    r.mount = g.U32(rider + 0x25Cu);
    const double dx = (g.S32(rider + 0xB8u) - static_cast<double>(g.S32(bike + 0xB8u))) / 65536.0;
    const double dz = (g.S32(rider + 0xC0u) - static_cast<double>(g.S32(bike + 0xC0u))) / 65536.0;
    r.riderToBike = std::sqrt(dx * dx + dz * dz);
    // The pause: the clear moves to the next frame's stamp (fight_session.cpp FightPadStamp), so the frame's
    // press codes live the whole frame, as the original's frame copy does - the pause menu reads them at the end
    // of GameFrame. RRJB_PAUSE=off: cleared here.
    for (uint32_t k = 0; k < 19 && !PauseOn(); ++k) {                    // 0x8001CBB4..0x8001CBD4
        const uint32_t s = rec + 0x14u + 8u * k;
        g.W8(s + 6u, 0);
        if (g.S32(s) < 0) g.W32(s, 0);
    }
    if (g.Faulted()) r.ok = false;
    return r;
}

size_t g_walkFreeFrames = 0;
size_t WalkFreeFrames() { return g_walkFreeFrames; }

bool WalkToBike(const uint8_t* ram, uint32_t bike, bool& forward, int& turn) {
    GuestRam g(const_cast<uint8_t*>(ram), 0);
    const uint32_t rider = g.U32(bike + 0x354u);
    if (g.U32(rider + 0x25Cu) < 2u) return false;                     // on the bike: the caller's own pad
    forward = false;
    turn = 0;
    // off the bike and NOT walking by hand: hold nothing. RiderRecover 0x80092E04 walks a rider that is
    // not "far" back by itself (WalkTarget / WalkStep -> ReSeat); any held direction would make the pad
    // reader's walk arm switch him to the manual walk (+0x228 |= 0x1000800) every other frame, and near
    // the bike that toggling kept him circling it
    static const bool legacy = [] {                                   // RRJB_WALKFREE=off: the old script
        const char* v = std::getenv("RRJB_WALKFREE");
        return v != nullptr && v[0] == 'o' && v[1] == 'f';
    }();
    if (!(g.U32(rider + 0x228u) & 0x800u)) {
        if (legacy) return false;
        ++g_walkFreeFrames;
        return true;
    }
    const double vx = g.S32(bike + 0xB8u) - static_cast<double>(g.S32(rider + 0xB8u));
    const double vz = g.S32(bike + 0xC0u) - static_cast<double>(g.S32(rider + 0xC0u));
    const double hx = g.S16(rider + 0x1C2u), hz = g.S16(rider + 0x1C6u);
    const double vl = std::sqrt(vx * vx + vz * vz), hl = std::sqrt(hx * hx + hz * hz);
    forward = true;
    turn = 0;
    // near the bike the script lets go, as a player would: with no key held the pad reader clears the
    // manual-walk pair and RiderRecover's own walk (WalkStep) takes the rider to the seat
    if (vl < 3.0 * 65536.0) {
        forward = false;
        return true;
    }
    if (hl < 1.0) return true;
    const double c = (hz * vx - hx * vz) / (vl * hl), dotv = (hx * vx + hz * vz) / (vl * hl);
    // the walk turns only while it moves (GroundWalkControl 0x8009926C: the turn rate +0x1E8 rides on the
    // speed +0x1E0), so the script always walks and steers toward the bike
    forward = true;
    if (dotv < 0.98) turn = c > 0 ? 1 : -1;
    return true;
}

bool StandingStart(const uint8_t* ram, uint32_t bike) {
    GuestRam g(const_cast<uint8_t*>(ram), 0);
    const uint32_t rider = g.U32(bike + 0x354u);
    const int32_t s = g.S32(bike + 0x1E0u);
    return g.U32(rider + 0x25Cu) < 2u && s < 0x10000 && s > -0x10000 && g.S32(0x8005B230u) <= 0;
}

} // namespace rr::game
