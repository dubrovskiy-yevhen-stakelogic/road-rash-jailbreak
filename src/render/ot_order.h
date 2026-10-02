#pragma once
// The ordering-table slot map on the CPU (race_scene.h SetOtOrder): the same map as
// shaders.cpp OtSlot, for the primitives drawn by programs of their own (fx_draw.h).
//
// The emitters (SLUS 0x800251E4 0x800256F8.., 0x80025EE0 0x80026784.., RASHCDG 0x8006D350 ..) map a depth key t0 (cell
// units, 1/64 world unit) through the scratchpad RASHCDG 0x800674D4 fills: near offset *(0x1F800004) =
// *(0x8005B4D4), shift *(0x1F800000) = *(0x8005B4D8), the bases 0 0 0x800 0x1000 at 0x1F800006.. and the slot offsets
// 0 0 0x200 0x300 at 0x1F80000E.. (read in rr-pack's scratchpad), clamped to [0, *(0x8005ADFC) - 2].
#include <algorithm>
#include <cstdint>

namespace rr::render {

inline int OtSlotOf(int32_t t0, int nearOffset, int shift, int maxSlot) {
    int32_t s = 0;
    if (nearOffset < 4096) {
        const int b = (t0 >> 11) > 0 ? 1 : 0;
        const int c = (t0 >> 12) <= 0 ? (b == 0 ? -3 : -2) : (b == 0 ? -2 : 0);
        const int a1 = shift + c;
        const int i = std::clamp(a1, 0, 3);
        const int32_t base = i == 2 ? 0x800 : (i == 3 ? 0x1000 : 0);
        const int32_t off = i == 2 ? 0x200 : (i == 3 ? 0x300 : 0);
        s = off + ((t0 - (base + nearOffset)) >> (a1 & 31));
    } else {
        s = (t0 - nearOffset) >> (shift & 31);
    }
    return std::clamp<int32_t>(s, 0, maxSlot);
}

// The depth (0..1) a primitive of slot `slot` and link rank `rank` (1024ths of a slot) is drawn at in a table pass
// drawn in [base, base + 0.5) - shaders.cpp's rule.
inline float OtDepthOf(int slot, int rank, int maxSlot, float base) {
    return base + 0.5f * (static_cast<float>(slot) + static_cast<float>(rank) / 1024.0f) / static_cast<float>(maxSlot + 2);
}

} // namespace rr::render
