// The whole-frame parity hook: the product's session takes a captured
// console state as its arena, so the product's own frame assembly and renderer draw the state the
// ORIGINAL's GPU packets of that frame were built from (rrgame --parity, tests\run_gates.ps1).
//
// The arena is laid out like the console's RAM at rr-race's own addresses, so a
// capture's 2 MiB image IS an arena: the view records, the bikes of pool 0, the riders of pool 1, the
// props / cars / pedestrians of pools 2..5, the cell slot table and the draw list are all where the
// product's draw reads them. What is host-side is re-derived from the image here: each bike's rider
// (+0x354) and runtime rider record (+0x43C) and the display-only route distance the sky choice uses.
#include "game/race_session.h"

#include <cstring>

namespace rr::game {
namespace {
constexpr uint32_t kPool0 = 0x801B65D4;  // pool 0, stride 1096 (race_session.cpp kArenaPool0)
constexpr uint32_t kPool1 = 0x801BB2EC;  // pool 1 follows pool 0's 18 slots exactly: 0x4D18 = 18 x 1096
constexpr uint32_t kEntityBytes = 1096;
constexpr uint32_t kRiderRecordBytes = 628;
constexpr uint32_t kRiderDefRecordBytes = 72;
constexpr size_t kPool0Slots = (kPool1 - kPool0) / kEntityBytes;
} // namespace

std::string RaceSession::AdoptCapture(const std::vector<uint8_t>& ram) {
    if (ram.size() != 0x200000u) return "parity: the capture is not a 2 MiB guest RAM image";
    std::memcpy(arena_.Ram(), ram.data(), ram.size());
    const auto word = [this](uint32_t a) {
        uint32_t v = 0;
        std::memcpy(&v, At(a), 4);
        return v;
    };
    const auto inRam = [](uint32_t a) { return a >= 0x80000000u && a < 0x80200000u; };
    // Every slot of pool 0 is a bike in the capture; the ones with +0x140 == 0 are dormant and the draw skips
    // them (BikeLive), as the console's model draw does.
    if (bikes_.size() < kPool0Slots) bikes_.resize(kPool0Slots);
    size_t live = 0;
    for (size_t i = 0; i < bikes_.size(); ++i) {
        RaceBike& b = bikes_[i];
        b.entityAddress = kPool0 + kEntityBytes * static_cast<uint32_t>(i);
        b.entity = ArenaBytes{At(b.entityAddress), kEntityBytes};
        const uint32_t owner = word(b.entityAddress + 0x354u), def = word(b.entityAddress + 0x43Cu);
        b.ownerAddress = inRam(owner) ? owner : kPool1 + kRiderRecordBytes * static_cast<uint32_t>(i);
        b.owner = ArenaBytes{At(b.ownerAddress), kRiderRecordBytes};
        if (inRam(def)) {
            b.riderDefAddress = def;
            b.riderDef = ArenaBytes{At(def), kRiderDefRecordBytes};
        }
        // the display-only route distance (UpdateDisplayDistance) from a whole-path search
        const std::vector<rr::RoadSlice>& path = world_.path;
        double best = 1e300;
        for (size_t s = 0; s < path.size(); ++s) {
            double d = 0.0;
            for (uint32_t k = 0; k < 3; ++k) {
                int32_t c = 0;
                std::memcpy(&c, At(b.entityAddress + 0xB8u + 4u * k), 4);
                const double v = static_cast<double>(path[s].pos[k]) - static_cast<double>(c);
                d += v * v;
            }
            if (d < best) {
                best = d;
                b.pathHint = s;
            }
        }
        UpdateDisplayDistance(i);
        if (BikeLive(i)) ++live;
    }
    char note[160];
    std::snprintf(note, sizeof(note), "parity: the arena is the capture's RAM; %zu bikes of pool 0 bound, %zu live",
                  bikes_.size(), live);
    return note;
}

} // namespace rr::game
