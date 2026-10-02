#include "game/frame_ot.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace rr::game {

namespace {
uint32_t g_ot[4] = {};
uint32_t g_count = 0;
} // namespace

bool FrameOtsPorted() {
    const char* v = std::getenv("RRJB_FXOT");
    return v == nullptr || std::strcmp(v, "top") != 0;
}

std::string ReserveFrameOts(uint32_t& from, uint32_t limit, int players) {
    g_count = 0;
    if (!FrameOtsPorted()) return "the effects' ordering table is carved from the packet heap's top (RRJB_FXOT=top: the control)";
    const uint32_t n = players == 1 ? 2u : 4u; // 0x8001C1AC: +0xF4 = 2 with one player, else 4
    uint32_t at = (from + 15u) & ~15u;
    if (at + n * kFrameOtBytes > limit) return "the frame's ordering tables were NOT placed: no room in the bump region";
    for (uint32_t k = 0; k < n; ++k, at += kFrameOtBytes) g_ot[k] = at;
    from = at;
    g_count = n;
    char b[300];
    std::snprintf(b, sizeof(b),
                  "the frame's ordering tables as SLUS 0x8001C1AC makes them (frame_ot.h): %u x malloc(0x%X) "
                  "(0x%X entries) OURS at 0x%08X.., apart from the packet heap (rr-race: the OT at 0x800FB974, the heap's "
                  "end 0x800F0460)",
                  n, kFrameOtBytes, kFrameOtEntries, g_ot[0]);
    return b;
}

bool ApplyFrameOts(rr::sim::GuestRam& g, uint32_t record) {
    if (g_count == 0 || record == 0) return false;
    g.W8(record + 0xF4u, static_cast<uint8_t>(g_count));
    for (uint32_t k = 0; k < g_count; ++k) g.W32(record + 0xF8u + 4u * k, g_ot[k]);
    g.W32(0x8005AE00u, 0);
    g.W32(record + 0x108u, g_ot[0]);
    // ClearOTagR 0x80048CAC(ot, 0x514): each entry links to the one below it, entry 0 ends the list
    for (uint32_t i = 0; i < kFrameOtEntries; ++i)
        g.W32(g_ot[0] + 4u * i, i == 0 ? 0x00FFFFFFu : ((g_ot[0] + 4u * (i - 1u)) & 0x00FFFFFFu));
    return !g.Faulted();
}

} // namespace rr::game
