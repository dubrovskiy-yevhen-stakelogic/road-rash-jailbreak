// The pedestrians in the product (peds_product.h).
#include "game/peds_product.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "game/sim/peds.h"
#include "game/traffic_arena.h"
#include "game/loader_product.h" // LoaderPorted, Loader2On
#include "rrformats/level_bundle.h"

namespace rr::game {
namespace {

using rr::sim::GuestRam;
namespace s = rr::sim;

constexpr uint32_t kGameStatePtr = 0x8005B2F8;
constexpr uint32_t kPlayerBikePtr = 0x8005B38C;

// OURS: the heap the loader's mallocs come from (the console's is SLUS 0x800142B4).
uint32_t Alloc(uint32_t& next, uint32_t limit, uint32_t n) {
    const uint32_t size = (n + 11u) & ~7u;
    if (next + size > limit) return 0;
    const uint32_t user = next + 4u;
    next += size;
    return user;
}

PedTotals g_totals;
bool g_knocked[4] = {};

constexpr uint32_t kSheetTable = 0x800D5F70;    // the texture sheets' records, 12 bytes; *(0x8005B2E4) -> it
constexpr uint32_t kSheetTablePtr = 0x8005B2E4;
constexpr uint32_t kTagLect = 0x5443454C;

// RASHCDI 0x8005E848(players), the pedestrian sheets' part: the counts +408..+424 cleared, the table's
// pointer, the kind-4 array +312 (cap *(0x8005AE10 + 4 (players - 1))) and records 0 / 1 keyed 255. Then
// 0x8005DDB8's kind-4 arm (0x8005E090..0x8005E2C0) for each LECT of kind 4 in the level bundle's
// sections, in the loader's order (the same sections traffic_arena.cpp walks for the CTKP): a record
// keyed by the LECT's +16 unless one has the key or the array is full, its texture page words, and its
// CLUT id from the running counter +424. NOT run: the VRAM uploads (LoadImage SLUS 0x80048A6C) and the
// other kinds' arms. OURS, named: +424 is 32 when the first kind-4 sheet comes (the KNBP handler
// 0x8005DA38 sets it from the bank's +8 and 0x8005DBB8 advances it; neither is transcribed - 32 is what all
// four captures hold before the pedestrian sheets).
size_t BuildPedKeys(GuestRam& g, const rr::DiscImage& disc, int raceId, std::string& fail) {
    // the page table is the PORTED TexSetUp / LectSheet's (loader_product.h) - its kind-4 records
    if (LoaderPorted()) return g.U32(kSheetTable + 412u);
    const uint32_t gs = g.U32(kGameStatePtr);
    const uint32_t players = g.U32(gs + 48u);
    if (players < 1u || players > 2u) {
        fail += " (no player count for the sheet table)";
        return 0;
    }
    const uint32_t p1 = players - 1u;
    for (uint32_t o = 408; o <= 424; o += 4) g.W32(kSheetTable + o, 0);
    g.W32(kSheetTablePtr, kSheetTable);
    const int32_t cap = g.S32(0x8005AE10u + 4u * p1);
    for (int32_t i = 0; i < cap; ++i) g.W8(kSheetTable + 312u + 12u * static_cast<uint32_t>(i), 0xFF);
    g.W8(kSheetTable + 0u, 0xFF);
    g.W8(kSheetTable + 12u, 0xFF);
    g.W32(kSheetTable + 424u, 32); // OURS (see above)
    const auto f = disc.Find("DATA/GAMEBIN1.DAT");
    if (!f) {
        fail += " (no DATA/GAMEBIN1.DAT)";
        return 0;
    }
    const std::vector<uint8_t> bin = disc.ReadFile(*f);
    size_t made = 0;
    try {
        const rr::LevelBundle bundle =
            rr::ParseLevelBundle(bin, rr::LevelBundleIndexForRace(raceId));
        const uint32_t flags = g.U32(gs + 4u) & 0x18u;
        const uint32_t raceType = g.U8(gs + 4u);
        const uint32_t tpTable = 0x800533B4u + 88u * p1, clutTable = 0x80053254u + 176u * p1;
        for (const rr::LevelBundleSection& sec : bundle.sections) {
            const bool walk8 = sec.type == 8 && flags == 0;
            const bool walk9 = sec.type == 9 && !(flags == 8 && raceType != 44);
            if (!walk8 && !walk9) continue;
            const size_t len = static_cast<size_t>(sec.tag) >= 4 ? static_cast<size_t>(sec.tag) - 4 : 0;
            if (sec.payload + len > bin.size()) continue;
            const uint8_t* p = bin.data() + sec.payload;
            size_t at = 0;
            while (at + 20 <= len) {
                uint32_t tag, clen;
                std::memcpy(&tag, p + at, 4);
                std::memcpy(&clen, p + at + 4, 4);
                if (tag == kTagLect && p[at + 12] == 4u) {                                   // the kind-4 arm
                    const uint32_t key = static_cast<uint32_t>(p[at + 16] | (p[at + 17] << 8));
                    const int32_t count = g.S32(kSheetTable + 412u);
                    bool dup = false;
                    for (int32_t i = 0; i < count; ++i)                                      // 0x8005E0B4
                        if (g.U8(kSheetTable + 312u + 12u * static_cast<uint32_t>(i)) == key) dup = true;
                    if (!dup && count < cap) {                                               // 0x8005E0F8
                        const uint32_t c = static_cast<uint32_t>(count);
                        const uint32_t rec = kSheetTable + 312u + 12u * c;
                        const uint32_t b46 = g.U8(tpTable + 46u), b45 = g.U8(tpTable + 45u);
                        const uint32_t a3 = c & 7u, a2 = c & 1u, a1 = (a3 >> 1) << 6;
                        const uint32_t x = ((b46 & 0xFu) << 6) + ((c << 3) & 0x3C0u) + (a2 << 5);     // sp+16
                        const uint32_t y = (b45 + ((b46 & 0x10u) << 4) + a1) & 0xFFFFu;             // sp+18
                        g.W8(rec + 0u, static_cast<uint8_t>(key));
                        g.W8(rec + 1u, static_cast<uint8_t>(a3));
                        g.W8(rec + 2u, static_cast<uint8_t>(a2 << 6));
                        g.W8(rec + 3u, static_cast<uint8_t>(a1));
                        const uint32_t tp = ((y & 0x100u) >> 4) | ((x & 0x3FFu) >> 6) | 0x80u | ((y & 0x200u) << 2);
                        g.W16(rec + 8u, static_cast<uint16_t>(tp));
                        g.W16(rec + 6u, static_cast<uint16_t>(x));
                        g.W32(kSheetTable + 412u, c + 1u);
                        const int32_t n = g.S32(kSheetTable + 424u);                          // 0x8005E214
                        const int32_t cols = g.U8(clutTable + 116u);
                        const int32_t col = cols != 0 ? n % cols : n;                         // `div`: hi = n on 0
                        const int32_t row = n / 3;
                        const uint32_t cx = (g.U16(clutTable + 112u) + static_cast<uint32_t>(col) * g.U8(clutTable + 118u) + 640u) & 0xFFFFu;
                        const uint32_t cy = static_cast<uint32_t>(511 - row) & 0xFFFFu;
                        g.W16(rec + 10u, static_cast<uint16_t>((cy << 6) | ((cx >> 4) & 0x3Fu)));
                        g.W32(kSheetTable + 424u, static_cast<uint32_t>(n + 1));
                        ++made;
                    }
                }
                if (clen == 0) break;
                at += clen;
            }
        }
    } catch (const std::exception& e) {
        fail += std::string(" (GAMEBIN1.DAT: ") + e.what() + ")";
    }
    return made;
}

} // namespace

bool PedsEnabled() {
    const char* v = std::getenv("RRJB_PEDS");
    return v == nullptr || std::strcmp(v, "off") != 0;
}

uint32_t PedSwitchFor(GuestRam& g) {
    if (!PedsEnabled()) return 0;
    return (g.U8(g.U32(kGameStatePtr) + 4u) & 0x18u) == 0 ? 1u : 0u;               // RASHCDI 0x80063BB8
}

PedTotals& PedRunTotals() { return g_totals; }

std::string BuildPedArena(GuestRam& g, const rr::DiscImage& disc, int raceId, uint32_t& from, uint32_t limit) {
    g_totals = PedTotals{};
    for (bool& k : g_knocked) k = false;
    if (!PedsEnabled()) return "the pedestrians (peds_product.h) are SWITCHED OFF (RRJB_PEDS=off): 0x8005B254 stays 0";
    if (g.U32(s::kPedSwitch) == 0)
        return "the pedestrians (peds_product.h): the switch 0x8005B254 is 0 in this race type (game_state+4 & 0x18)";
    std::string fail;
    // with the ported grid, BuildRace's population block runs PedTablesInit 0x80068500 and
    // PartSlotsInit 0x80069394 PORTED (loader2.h) after BuildGrid, as the original; the transcription below is the rest's
    const bool l2 = Loader2On() && Loader2Totals().gridPlanned;
    uint32_t next = (from + 7u) & ~7u;
    size_t slots = 0;
    if (!l2) {
    // RASHCDI 0x80068500: the clip ids 224..236 of bank 5 and the six states
    for (uint32_t k = 0; k < 26; k += 2) g.W16(s::kPedClipIds + k, 0);
    for (uint32_t i = 0; i < 13; ++i) g.W16(s::kPedClipIds + 2u * i, static_cast<uint16_t>(0x5000u + i));
    const uint32_t st = s::kPedStates;
    g.W16(st + 0u, 73);
    g.W16(st + 6u, 230);
    g.W16(st + 12u, 230);
    g.W16(st + 18u, 50);
    g.W16(st + 20u, 4);
    g.W16(st + 24u, 70);
    g.W16(st + 2u, 0);
    g.W8(st + 4u, 0);
    g.W16(st + 8u, 0);
    g.W8(st + 10u, 0);
    g.W16(st + 14u, 0);
    g.W8(st + 16u, 0);
    g.W8(st + 22u, 0);
    g.W16(st + 26u, 0);
    g.W8(st + 28u, 0);
    g.W16(st + 30u, 69);
    g.W16(st + 32u, 0);
    g.W8(st + 34u, 0);
    // RASHCDI 0x80069394, pool 2's part: the 17 part slots of each of the four slots
    const uint32_t base = g.U32(s::kPedBlock + 16u);
    if (base != 0) {
        for (uint32_t i = 0; i < 4; ++i) {
            const uint32_t e = base + 572u * i;
            const uint32_t parts = Alloc(next, limit, 408u);                        // 0x800693D4
            g.W32(e + 4u, parts);
            if (parts == 0) {
                fail += " (no room for part slots)";
                continue;
            }
            for (uint32_t k = 0; k < 408u; k += 4) g.W32(parts + k, 0);             // OURS: zeroed
            g.W32(e + 0u, 0);
            g.W32(e + 96u, 0);
            g.W32(e + 540u, 0);
            g.W32(s::kPedBlock + 12u, g.U32(s::kPedBlock + 12u) + 1u);
            ++slots;
        }
    } else {
        fail += " (no pedestrian block)";
    }
    } // !l2
    // The pedestrians' sheet keys: RASHCDI 0x8005E848's kind-4 part and 0x8005DDB8's kind-4 arm (see the header)
    const size_t keys = BuildPedKeys(g, disc, raceId, fail);
    // RASHCDI 0x8005C630's first load: DATA\PED01A.GEO (models 400 / 430)
    std::vector<uint32_t> models;
    uint32_t at = 0;
    bool loaded = false;
    if (LoaderPorted() && rr::sim::RegistryFind(g, 400) >= 0) { // the PORTED CarModels loaded it
        for (uint32_t id : {400u, 430u})
            if (rr::sim::RegistryFind(g, id) >= 0) models.push_back(id);
        loaded = true;
    } else if (const auto f = disc.Find("DATA/PED01A.GEO")) {
        loaded = LoadGeoIntoArena(g, disc.ReadFile(*f), next, limit, models, &at);
    }
    if (!loaded) fail += " (DATA/PED01A.GEO was not loaded)";
    from = next;
    std::string ids;
    for (uint32_t m : models) ids += (ids.empty() ? "" : " / ") + std::to_string(m);
    char b[700];
    std::snprintf(b, sizeof(b),
                  "the pedestrian arena (peds_product.h: RASHCDI 0x80063BB8 / 0x80068500 / 0x80069394 / 0x8005E848 / "
                  "0x8005DDB8 / 0x8005C630 %s): the switch 0x8005B254 = 1, the clip-id and state tables, %zu "
                  "slot(s) with part slots (block +12 = %u), %zu pedestrian sheet key(s) (the kind-4 LECTs; OURS: the CLUT "
                  "counter from 32), DATA\\PED01A.GEO model(s) %s at 0x%08X (OURS: after the car file)%s",
                  l2 ? "PORTED in BuildRace: the tables and part slots after the grid" : "transcribed", slots, g.U32(s::kPedBlock + 12u), keys, ids.c_str(), at, fail.empty() ? "" : (" - FAILED:" + fail).c_str());
    return b;
}

void PedsFrame(GuestRam& g) {
    if (g.U32(s::kPedSwitch) == 0) return;
    const uint32_t base = g.U32(s::kPedBlock + 16u);
    if (base == 0) return;
    const size_t live = static_cast<size_t>(g.S32(s::kPedBlock) < 0 ? 0 : g.S32(s::kPedBlock));
    if (live > g_totals.maxLive) g_totals.maxLive = live;
    const uint32_t bike = g.U32(kPlayerBikePtr);
    for (uint32_t i = 0; i < 4; ++i) {
        const uint32_t e = base + 572u * i;
        const bool down = g.U16(e + 172u) != 0 && (g.U32(e + 552u) & 0x20000000u) != 0;
        if (down && !g_knocked[i]) {
            ++g_totals.knocked;
            if (std::getenv("RRJB_PEDS_TRACE")) std::fprintf(stderr, "PEDTRACE frame %zu ped %u knocked flags %08X clip %u\n", g_totals.releases, i, g.U32(e + 552u), g.U16(e + 544u)); // development trace, one line per knock-down
            int32_t dx = (g.S32(e + 184u) - g.S32(bike + 184u)) >> 16, dz = (g.S32(e + 192u) - g.S32(bike + 192u)) >> 16;
            if (dx * dx + dz * dz <= 36) ++g_totals.byPlayer;
        }
        g_knocked[i] = down;
    }
    g.ClearFault();
}

std::string PedTotalsLine() {
    const PedTotals& t = g_totals;
    char b[600];
    std::snprintf(b, sizeof(b),
                  "PEDS (peds.h / peds_product.h)%s: spawns asked %zu, made %zu (refused %zu); at most %zu "
                  "live; PedPass 0x800CB304 %zu frame(s), %zu refused; PedRelease 0x800CB4F8 %zu frame(s), %zu refused; "
                  "knocked down %zu (%zu with the player's bike within 6 units); drawn %zu pedestrian-frame(s) in %zu frame(s)\n",
                  PedsEnabled() ? "" : " SWITCHED OFF (RRJB_PEDS=off)", t.asked, t.spawned, t.spawnRefused, t.maxLive,
                  t.passes, t.passRefused, t.releases, t.releaseRefused, t.knocked, t.byPlayer, t.drawn, t.drawFrames);
    return b;
}

std::vector<LivePed> LivePeds(const uint8_t* ram) {
    auto u32 = [ram](uint32_t a) {
        uint32_t v;
        std::memcpy(&v, ram + (a & 0x1FFFFFu), 4);
        return v;
    };
    auto s16 = [ram](uint32_t a) {
        int16_t v;
        std::memcpy(&v, ram + (a & 0x1FFFFFu), 2);
        return v;
    };
    std::vector<LivePed> out;
    if (u32(s::kPedSwitch) == 0) return out;
    const uint32_t base = u32(s::kPedBlock + 16u);
    if ((base & 0xFF000000u) != 0x80000000u) return out;
    for (uint32_t i = 0; i < 4; ++i) {
        const uint32_t e = base + 572u * i;
        if (static_cast<uint16_t>(s16(e + 172u)) == 0 || s16(e + 320u) == 0) continue;
        const uint32_t reg = u32(e + 96u), parts = u32(e + 4u);
        if ((reg & 0xFF000000u) != 0x80000000u || (parts & 0xFF000000u) != 0x80000000u) continue;
        LivePed p;
        p.entity = e;
        p.model = u32(reg);
        p.lod = static_cast<uint32_t>(static_cast<int8_t>(ram[(e + 8u) & 0x1FFFFFu]));
        for (uint32_t k = 0; k < 3; ++k) p.pos[k] = static_cast<int32_t>(u32(e + 184u + 4u * k));
        for (uint32_t k = 0; k < 9; ++k) p.rows[k] = s16(e + 432u + 2u * k);
        for (uint32_t j = 0; j < 17; ++j)
            for (uint32_t k = 0; k < 9; ++k) p.parts[j][k] = s16(parts + 24u * j + 4u + 2u * k);
        for (uint32_t k = 0; k < 3; ++k) p.root[k] = s16(e + 0x1Cu + 2u * k);
        p.cell = static_cast<int32_t>(u32(e + 176u));
        const int16_t k = s16(e + 74u);
        const uint32_t table = u32(kSheetTablePtr);
        if (k >= 0 && k < 34 && (table & 0xFF000000u) == 0x80000000u) p.sheet = ram[(table + 12u * static_cast<uint32_t>(k)) & 0x1FFFFFu];
        out.push_back(p);
    }
    return out;
}

} // namespace rr::game
