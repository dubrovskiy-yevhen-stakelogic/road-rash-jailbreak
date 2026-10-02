#include "game/bike_pose_product.h"

#include "game/sim/bike_parts.h"

namespace rr::game {
namespace {

// The draw 0x80067AC4 is the renderer's job in the product: answered without running anything.
struct NoDraw final : rr::sim::BikePartCallees {
    bool Call(uint32_t, uint32_t, uint32_t, uint32_t) override { return true; }
};
constexpr uint32_t kBikeDrawSp = 0x801FE000u; // ours: a stack depth nothing else uses at this point

} // namespace

bool RunBikeParts(uint8_t* ram, uint32_t gp, uint32_t bike, const uint16_t* asinTable) {
    rr::sim::GuestRam g(ram, gp);
    const uint32_t parts = g.U32(bike + 4u);
    if (parts < 0x80000000u || parts >= 0x80200000u) return true;          // no part slots: nothing drawn
    NoDraw none;
    if (!rr::sim::BikeInstance(g, bike, 0, kBikeDrawSp, asinTable, none)) return false;
    rr::sim::WheelSlots(g, bike);
    return !g.Faulted();
}

bool ReadBikeParts(const uint8_t* ram, uint32_t bike, rr::PartMatrix out[5]) {
    auto u32 = [&](uint32_t a) {
        const uint32_t o = a & 0x1FFFFFu;
        return static_cast<uint32_t>(ram[o]) | (static_cast<uint32_t>(ram[o + 1]) << 8) |
               (static_cast<uint32_t>(ram[o + 2]) << 16) | (static_cast<uint32_t>(ram[o + 3]) << 24);
    };
    auto s16 = [&](uint32_t a) {
        const uint32_t o = a & 0x1FFFFFu;
        return static_cast<int16_t>(static_cast<uint16_t>(ram[o] | (ram[o + 1] << 8)));
    };
    const uint32_t parts = u32(bike + 4u);
    if (parts < 0x80000000u || (parts & 0x1FFFFFu) + 24u * 5u > 0x200000u) return false;
    out[0] = rr::PartMatrix{};
    for (uint32_t k = 1; k < 5; ++k)
        for (uint32_t e = 0; e < 9; ++e) out[k].m[e] = s16(parts + 24u * k + 4u + 2u * e);
    return true;
}

int ReadMachineParts(const uint8_t* ram, uint32_t bike, rr::PartMatrix* out, int max) {
    auto u32 = [&](uint32_t a) {
        const uint32_t o = a & 0x1FFFFFu;
        if (o + 4u > 0x200000u) return 0u;
        return static_cast<uint32_t>(ram[o]) | (static_cast<uint32_t>(ram[o + 1]) << 8) |
               (static_cast<uint32_t>(ram[o + 2]) << 16) | (static_cast<uint32_t>(ram[o + 3]) << 24);
    };
    const uint32_t model = u32(bike);
    const uint32_t parts = u32(bike + 4u);
    if (model < 0x80000000u || parts < 0x80000000u) return 0;
    const uint32_t n = u32(model + 24u) & 0xFFFFu;
    if (n == 0u || static_cast<int>(n) > max || (parts & 0x1FFFFFu) + 24u * n > 0x200000u) return 0;
    out[0] = rr::PartMatrix{};
    for (uint32_t k = 1; k < n; ++k)
        for (uint32_t e = 0; e < 9; ++e) {
            const uint32_t o = (parts + 24u * k + 4u + 2u * e) & 0x1FFFFFu;
            out[k].m[e] = static_cast<int16_t>(static_cast<uint16_t>(ram[o] | (ram[o + 1] << 8)));
        }
    return static_cast<int>(n);
}

} // namespace rr::game
