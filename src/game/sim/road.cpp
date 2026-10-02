#include "game/sim/road.h"

namespace rr::sim {
namespace {
int32_t LoadS32(const uint8_t* p) {
    return static_cast<int32_t>(static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
                                (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24));
}
int16_t LoadS16(const uint8_t* p) {
    return static_cast<int16_t>(static_cast<uint16_t>(static_cast<uint32_t>(p[0]) |
                                                      (static_cast<uint32_t>(p[1]) << 8)));
}
} // namespace

int32_t FindRoadPieceIndex(int16_t objKind, int16_t count, const uint8_t* entries, int32_t key) {
    if (entries == nullptr) return -1;   // 0x80039C90, `beqz a0`
    if (objKind != 1) return -1;         // 0x80039CA0
    // 0x80039CB8: `sltu a0,a3` with a3 = base + (count << 5). A negative count makes the end
    // pointer wrap below the base and the loop never runs, which this reproduces.
    const int32_t end = static_cast<int32_t>(static_cast<uint32_t>(count) << 5);
    if (end <= 0) return -1;
    for (int32_t i = 0; i < count; ++i) {
        const uint8_t* e = entries + static_cast<size_t>(i) * kRoadPieceEntrySize;
        const int32_t diff = static_cast<int32_t>(static_cast<uint32_t>(LoadS32(e + 12)) -
                                                  static_cast<uint32_t>(key));
        const int32_t fused = static_cast<int32_t>(static_cast<uint32_t>(LoadS16(e + 2)) |
                                                   static_cast<uint32_t>(diff));
        if (fused == 0) return i;
    }
    return -1;
}

} // namespace rr::sim
