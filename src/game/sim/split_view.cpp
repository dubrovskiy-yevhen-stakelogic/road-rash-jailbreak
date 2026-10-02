#include "game/sim/split_view.h"

namespace rr::sim {

// SLUS 0x8001B670: lui v0,0x800D; jr ra; sw a0,0x6C68(v0).
void ViewModeWrite(GuestRam& g, uint32_t mode) { g.W32(kViewsFile, mode); }

// SLUS 0x8001B67C..0x8001B6A4: sw a0,0x6C68(v1); v0 = ((a0 << 1) + a0) << 3; sw 0x800D6C6C + v0, 2024(gp).
void ViewModeSet(GuestRam& g, uint32_t mode) {
    g.W32(kViewsFile, mode);
    g.W32(g.gp() + kViewsPtrGp, kViewsRecords + ((mode << 1) + mode) * 8u);
}

// SLUS 0x8001C498: lui v0,0x800D; jr ra; lw v0,0x6C68(v0).
uint32_t ViewMode(GuestRam& g) { return g.U32(kViewsFile); }

bool SplitViewRect(GuestRam& g, uint32_t p, SplitRect& out) {
    const uint32_t rec = g.U32(g.gp() + kViewsPtrGp);
    if (rec == 0) return false;
    const uint32_t r = rec + 8u * p;
    out.x = g.S16(r + 0u);
    out.y = g.S16(r + 2u);
    out.w = g.S16(r + 4u);
    out.h = g.S16(r + 6u);
    // SLUS 0x80011C4C: x + ((s32)((u32)(u16)w << 16) >> 17) + nudge.x (and the same for y)
    const auto half = [](int16_t v) { return static_cast<int32_t>(static_cast<uint32_t>(static_cast<uint16_t>(v)) << 16) >> 17; };
    out.cx = out.x + half(out.w) + g.S16(rec + 0x10u + 4u * p);
    out.cy = out.y + half(out.h) + g.S16(rec + 0x12u + 4u * p);
    return !g.Faulted();
}

} // namespace rr::sim
