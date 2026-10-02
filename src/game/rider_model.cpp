#include "game/rider_model.h"

#include <cstdio>
#include <exception>

#include "rrformats/level_bank.h"
#include "rrformats/rmd3.h"

namespace rr::game {
namespace {
constexpr uint32_t kGameStatePtr = 0x8005B2F8; // -> game_state: +4 race type, +0x30 players, +0x3C bank
constexpr uint32_t kP1Bike = 0x8005B38C;
constexpr uint32_t kPool1BaseId = 150;         // *(0x800D4C38 + 16 + 8), pool 1's base model id (rr-race)
constexpr uint32_t kClassOffsets = 572;        // gp + 572 / gp + 576: SLUS data words 0 / 9
} // namespace

uint32_t RiderModelClass(rr::sim::GuestRam& g, uint32_t bike) {
    const uint32_t gs = g.U32(kGameStatePtr);
    const uint32_t rec = g.U32(bike + 1084u);
    const uint32_t b1 = g.U8(rec + 1u);
    // 0x80065E00..0x80065E50: s7 = 1 in race types 33 / 44 for a rider of another class than player 1's
    uint32_t s7 = 0;
    const uint32_t type = g.U8(gs + 4u);
    if ((type == 33u || type == 44u) && (b1 & 0xFu) != (g.U8(g.U32(g.U32(kP1Bike) + 1084u) + 1u) & 0xFu)) s7 = 1;
    if (g.U16(bike + 172u) < g.U32(gs + 48u)) s7 = 0;                  // 0x80066350..0x80066368: a player
    const uint32_t s1 = s7 ? 2u : (b1 & 0xFu);                         // 0x80066380..0x80066390
    const uint32_t sh = (32u - ((b1 >> 4) << 2)) & 31u;                 // 0x80066394..0x800663A4 (srav)
    const uint32_t a0 = (0x01112222u >> sh) & 0xFu;
    const uint32_t s4 = s1 < 2u ? 2u - a0 : 0u;                         // 0x800663AC..0x800663B8
    const uint32_t cls = 9u * s1 + 3u * s4 + g.U32(gs + 60u);           // 0x800663C8..0x800663EC
    g.W32(g.U32(bike + 852u) + 180u, cls);                              // 0x800663FC: sw v0,180(s5)
    return cls;
}

std::string SetRiderBoxes(rr::sim::GuestRam& g, const rr::DiscImage& disc, int bank,
                          const std::vector<uint32_t>& riders) {
    const uint32_t gsp = g.U32(kGameStatePtr); // RASHCDI 0x8005C45C's bundle (rrformats/level_bank.h)
    const std::string file = rr::LevelBankFile(rr::LevelBankIndex(bank, g.U8(gsp + 4u), g.S32(gsp + 0x48u)), ".GEO");
    const auto f = disc.Find(file);
    if (!f) return "the rider boxes (SLUS 0x80012FC8) were NOT set: " + file + " is not on the disc";
    std::vector<rr::Model> models;
    try {
        models = rr::ParseGeo(disc.ReadFile(*f));
    } catch (const std::exception& e) {
        return "the rider boxes (SLUS 0x80012FC8) were NOT set: " + file + ": " + e.what();
    }
    std::string done, missing;
    for (const uint32_t r : riders) {
        const uint32_t cls = g.U32(r + 180u);
        const uint32_t id = kPool1BaseId + g.U32(g.gp() + kClassOffsets + 4u * ((cls - 9u) < 9u ? 1u : 0u));
        const rr::ModelGroup* lod0 = nullptr;
        for (const rr::Model& m : models)
            if (m.id == id && !m.groups.empty()) lod0 = &m.groups.front();
        char one[48];
        if (lod0 == nullptr) { // ModelBind finds no registry slot: nothing is bound and the box stays
            std::snprintf(one, sizeof(one), "%s0x%08X (model %u)", missing.empty() ? "" : ", ", r, id);
            missing += one;
            continue;
        }
        // SLUS 0x80012AEC: (s16 half >> ((u16 DOD3 +14 >> 12) & 31)) << 10 per axis
        const uint32_t shift = ((lod0->flags >> 16) >> 12) & 31u;
        uint32_t ext[3];
        for (int k = 0; k < 3; ++k) ext[k] = static_cast<uint32_t>(static_cast<int32_t>(lod0->bbox.half[k]) >> shift) << 10;
        g.W32(r + 312u, ext[0] << 1); // 0x80013008..0x80013020, a pool-1 / pool-2 entity
        g.W32(r + 308u, ext[1]);
        g.W32(r + 304u, ext[2]);
        std::snprintf(one, sizeof(one), "%s%u:%u", done.empty() ? "" : ", ", cls, id);
        done += one;
    }
    std::string note = "the rider boxes (SpawnBike RASHCDI 0x80066350..0x800663FC: the class at +0xB4, SLUS "
                       "0x80012FC8 on the model's LOD 0 from " + file + "): class:model " + done;
    if (!missing.empty()) note += "; NOT set (the model is not in the bundle, ModelBind binds nothing): " + missing;
    return note;
}

} // namespace rr::game
