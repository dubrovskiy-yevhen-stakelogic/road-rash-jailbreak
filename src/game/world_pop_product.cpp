// The world around the road in the product (world_pop_product.h).
#include "game/world_pop_product.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "game/sim/cell_draw.h"
#include "game/sim/bike.h"
#include "game/sim/ground.h"
#include "game/sim/world_pop.h"
#include "game/traffic_arena.h"
#include "game/loader_product.h" // the ported loader
#include "game/hazard_product.h" // the hazard spawner 0x800A0A20, PORTED

namespace rr::game {
namespace {

using rr::sim::GuestRam;
namespace s = rr::sim;

constexpr uint32_t kGameStatePtr = 0x8005B2F8;
constexpr uint32_t kPedBlock = 0x800D4B70;     // pool 2's control: +0 live, +4 next, +8 high, +12, +16 base
constexpr uint32_t kPedParams = 0x800D8740;    // the pedestrian parameters (+4 the live cap, read by Budget(2))
constexpr uint32_t kPedSwitch = 0x8005B254;    // PedPass / PedRelease run when non-zero

// OURS: the heap the loader's mallocs come from (the console's is SLUS 0x800142B4).
uint32_t Alloc(uint32_t& next, uint32_t limit, uint32_t n) {
    const uint32_t size = (n + 11u) & ~7u;
    if (next + size > limit) return 0;
    const uint32_t user = next + 4u;
    next += size;
    return user;
}

WorldTotals g_totals;

} // namespace

WorldTotals& WorldRunTotals() { return g_totals; }

int HazardSetFor(const rr::DiscImage& disc, uint8_t mode, uint32_t seed) {
    if ((mode & 0x18u) == 8u) return 0;                 // 0x8005C82C
    if (mode & 0x10u) return 1;                         // 0x80063814
    const auto env = disc.Find("DATA/ENV.EN");
    if (!env) return -1;
    const std::vector<uint8_t> bytes = disc.ReadFile(*env);
    if (bytes.size() < 0xCDu) return -1;
    const uint32_t count = static_cast<int8_t>(bytes[0xCC]) > 0 ? bytes[0xCC] : 0u; // `lb` at 0x8006AD60
    uint32_t sel = 0;
    if (count != 0) {
        seed = seed * 0x0019660Du + 0x3C6EF35Fu;        // Rand, SLUS 0x8001FC58
        sel = seed % count;                             // `divu` at 0x8006AD94
    }
    const size_t at = 0xCDu + 2u * sel;
    if (at >= bytes.size()) return -1;
    const int v = static_cast<int8_t>(bytes[at]);
    return v < 1 ? 1 : (v > 5 ? 5 : v);                 // 0x8006AEC0..0x8006AEE8
}

std::string BuildWorldArenaLine(GuestRam& g, const rr::DiscImage& disc, int hazardSet, uint32_t& from,
                                uint32_t limit) {
    const uint32_t gs = g.U32(kGameStatePtr);
    const uint32_t mode = g.U8(gs + 4u);
    const uint32_t players = g.U32(gs + 48u);
    uint32_t next = (from + 7u) & ~7u;
    std::string fail;
    // RASHCDI 0x80068D54 (the population reset), pools 2 / 4 / 5 - PORTED under the ported loader (the traffic arena
    // runs PopulationReset whole, loader_product.h); the transcription below is RRJB_LOADER=off's
    if (!LoaderPorted()) {
    for (uint32_t k = 0; k < 20; k += 4) g.W32(kPedBlock + k, 0);
    g.W32(kPedBlock + 8u, 0xFFFFFFFFu);                                       // 0x80068EB4
    g.W32(kPedParams + 8u, 10);                                               // 0x80068EC8..0x80068EF0
    g.W32(kPedParams + 16u, 10);
    g.W32(kPedParams + 20u, 150);
    g.W32(kPedParams + 24u, 300);
    g.W32(kPedParams + 4u, 4);
    g.W32(kPedParams + 12u, 4);
    g.W32(kPedParams + 28u, 5);
    g.W32(s::kWpPool4Ctrl + 12u, 0x800D1818u);                                // 0x80068F00
    g.W32(s::kWpPool4Ctrl, 0);
    g.W32(s::kWpPool4Ctrl + 4u, 0);
    g.W32(s::kWpPool4Ctrl + 8u, 0xFFFFFFFFu);
    g.W32(s::kWpPool5Ctrl + 12u, 0x800D36ECu);                                // 0x80068F24
    g.W32(s::kWpPool5Ctrl, 0);
    g.W32(s::kWpPool5Ctrl + 4u, 0);
    g.W32(s::kWpPool5Ctrl + 8u, 0xFFFFFFFFu);
    g.W32(s::kWpStoreFree, 8344);                                             // 0x80068FA8
    g.W32(s::kWpStoreLow, 0);                                                 // 0x80068FD0
    }
    for (uint32_t a = 0x800D1818u; a < 0x800D1818u + 8344u; a += 4) g.W32(a, 0);   // the store the pools share
    for (uint32_t a = s::kWpStore; a < s::kWpStore + 18u * 24u; a += 4) g.W32(a, 0);
    // loader2: BuildRace's first two calls PORTED here, where the session builds the world's pools -
    // WorldPoolsInit RASHCDI 0x80069000 and PoolTableInit 0x80068AA4 (its PopulationReset 0x80068D54 PORTED under it:
    // the ped block and pool 6 exist now, as in the original's order); the mallocs are blocks of the bump region (OURS:
    // where). Only when the session will run the ported grid (the rider records are cleared here, BuildGrid loads them).
    const bool l2 = Loader2On() && Loader2Totals().gridPlanned;
    uint32_t cap = (mode & 0x10u) ? 32u : 24u;                                // 0x8006903C..0x8006905C
    if (l2) {
        ProductLoaderCallees* self = nullptr;
        ProductLoaderCallees c(g, nullptr, &disc, &next, limit,
                               [&](uint32_t fn, const uint32_t*, int, uint32_t csp, uint32_t&, bool& handled) {
                                   handled = fn == s::kLdPopResetFn;
                                   return handled ? s::PopulationReset(g, csp, *self) : true;
                               });
        self = &c;
        if (!s::WorldPoolsInit(g, kLoaderSp, c) || g.Faulted()) fail += " (WorldPoolsInit refused: " + c.error + ")";
        else ++Loader2Totals().worldPools;
        if (!s::PoolTableInit(g, kLoaderSp, c) || g.Faulted()) fail += " (PoolTableInit refused: " + c.error + ")";
        else ++Loader2Totals().poolTable;
        g.ClearFault();
        cap = g.U32(0x8005B214u);
    }
    if (!l2) {
    // RASHCDI 0x80069000: pool 6 and the per-player volume lists
    g.W32(s::kWpPool6Cap, cap);
    g.W32(s::kWpPool6Ctrl + 28u, 0);
    g.W32(s::kWpPool6Ctrl + 12u, 0);
    const uint32_t vols = Alloc(next, limit, cap * 280u);                     // 0x8006907C
    g.W32(s::kWpPool6Ctrl + 28u, vols);
    if (vols != 0) {
        for (uint32_t a = 0; a < cap * 280u; a += 4) g.W32(vols + a, 0);
        g.W32(s::kWpPool6Ctrl + 12u, cap - 1u);
    } else {
        fail += " (no room for pool 6)";
    }
    g.W32(s::kWpVolumeLists, 0);                                              // 0x800690D4..0x800690E4
    g.W32(s::kWpVolumeLists + 4u, 0);
    g.W32(s::kWpVolumeLists + 32u, 0);
    g.W32(s::kWpVolumeLists + 36u, 0);
    for (uint32_t p = 0; p < players && p < 2; ++p) {
        const uint32_t b = s::kWpVolumeLists + 32u * p;
        g.W32(b, cap);
        const uint32_t list = Alloc(next, limit, cap * 8u);                  // 0x8006911C
        g.W32(b + 4u, list);
        if (list == 0) {
            fail += " (no room for a volume list)";
            continue;
        }
        for (uint32_t i = 0; i < cap; ++i) {
            g.W16(list + 8u * i, static_cast<uint16_t>(i));
            g.W16(list + 8u * i + 2u, 0xFFFF);
            g.W32(list + 8u * i + 4u, 0xFFFFFFFFu);
        }
        for (uint32_t k = 0; k < 3; ++k) {
            g.W32(b + 8u + 4u * k, 0);
            g.W32(b + 20u + 4u * k, 0xFFFFFFFFu);
        }
    }
    g.W32(s::kWpPool6Ctrl, 0);                                                // 0x80068F88..0x80068FA0
    g.W32(s::kWpPool6Ctrl + 4u, 0);
    g.W32(s::kWpPool6Ctrl + 8u, 0xFFFFFFFFu);
    // The pedestrian block's base (0x800691C8..0x8006920C): only with the pedestrian switch on. The
    // product runs no pedestrian (their spawner is not ported), so the switch stays as the arena has it.
    if (g.U32(kPedSwitch) != 0) {
        const uint32_t peds = Alloc(next, limit, 2288u);
        g.W32(kPedBlock + 16u, peds);
        if (peds != 0) {
            for (uint32_t a = 0; a < 2288u; a += 4) g.W32(peds + a, 0);
            g.W32(kPedBlock + 12u, 3);
        }
    }
    // RASHCDI 0x80068C3C..: the pool table's pools 2, 4, 5, 6
    g.W32(s::kWpPoolTable + 32u, g.U32(kPedBlock + 16u));
    g.W32(s::kWpPoolTable + 36u, 572);
    g.W32(s::kWpPoolTable + 40u, kPedBlock);
    g.W32(s::kWpPoolTable + 44u, kPedBlock + 8u);
    g.W32(s::kWpPoolTable + 64u, g.U32(s::kWpPool4Ctrl + 12u));
    g.W32(s::kWpPoolTable + 68u, 596);
    g.W32(s::kWpPoolTable + 72u, s::kWpPool4Ctrl);
    g.W32(s::kWpPoolTable + 76u, s::kWpPool4Ctrl + 8u);
    g.W32(s::kWpPoolTable + 80u, g.U32(s::kWpPool5Ctrl + 12u));
    g.W32(s::kWpPoolTable + 84u, 0xFFFFFE3Cu);                                 // -452
    g.W32(s::kWpPoolTable + 88u, s::kWpPool5Ctrl);
    g.W32(s::kWpPoolTable + 92u, s::kWpPool5Ctrl + 8u);
    g.W32(s::kWpPoolTable + 96u, g.U32(s::kWpPool6Ctrl + 28u));
    g.W32(s::kWpPoolTable + 100u, 280);
    g.W32(s::kWpPoolTable + 104u, s::kWpPool6Ctrl);
    g.W32(s::kWpPoolTable + 108u, s::kWpPool6Ctrl + 8u);
    } // !l2
    // The prop model: DATA\HAZARD<n>.GEO (RASHCDI 0x8006383C -> 0x8005C7F0 -> 0x8005CA10)
    const int set = hazardSet < 0 ? 0 : hazardSet;
    const std::string file = "DATA/HAZARD" + std::to_string(set) + ".GEO";
    std::vector<uint32_t> models;
    uint32_t at = 0;
    const auto f = disc.Find(file);
    bool loaded = false;
    if (LoaderPorted()) { // SetUpRace's HazardModels RASHCDI 0x8005C7F0 PORTED (the .GEO and its .TEX)
        std::string error;
        loaded = LoaderHazardModels(g, disc, set, next, limit, models, &at, error);
    } else if (f) {
        loaded = LoadGeoIntoArena(g, disc.ReadFile(*f), next, limit, models, &at);
    }
    if (!loaded) fail += " (" + file + " was not loaded)";
    from = next;
    char b[400];
    std::snprintf(b, sizeof(b),
                  "the world arena (world_pop_product.h: RASHCDI 0x80068AA4 / 0x80068D54 / 0x80069000 %s): "
                  "pools 4 / 5 at 0x800D1818 / 0x800D36EC, pool 6 of %u at 0x%08X (OURS: where), the volume lists, "
                  "the prop model %s (%zu model(s), family-6 slot %d) at 0x%08X%s",
                  l2 ? "PORTED: WorldPoolsInit / PoolTableInit with PopulationReset under it" : "transcribed",
                  cap, g.U32(s::kWpPool6Ctrl + 28u), file.c_str(), models.size(),
                  static_cast<int>(g.S16(s::kWpPropClass)), at, fail.empty() ? "" : (" - FAILED:" + fail).c_str());
    return b;
}

uint32_t SpawnBikeClass(GuestRam& g, uint32_t bike, int player) {
    const uint32_t gs = g.U32(kGameStatePtr);
    uint32_t cls;
    if (player >= 0) {
        cls = g.U32(gs + 0x48u + 4u * (player == 0 ? 0u : 1u));                   // 0x80065FD8..0x80065FEC
    } else {
        const uint32_t b1 = g.U8(g.U32(bike + 1084u) + 1u);
        const uint32_t type = g.U8(gs + 4u);
        uint32_t s7 = 0;                                                        // 0x80065E00..0x80065E50
        if ((type == 33u || type == 44u) && (b1 & 0xFu) != (g.U8(g.U32(g.U32(0x8005B38Cu) + 1084u) + 1u) & 0xFu)) s7 = 1;
        const uint32_t s1 = s7 ? 2u : (b1 & 0xFu);                              // 0x80066144..0x80066154
        const uint32_t s4 = s1 < 2u ? ((b1 >> 4) < 2u ? 0u : 1u) : 0u;          // 0x80066158..0x8006616C
        cls = 9u * s1 + 3u * s4 + g.U32(gs + 60u);                              // 0x80066170..0x80066194
    }
    g.W32(bike + 180u, cls);                                                    // 0x80066328
    return cls;
}

namespace {
bool InRegistry(GuestRam& g, uint32_t id) {
    for (uint32_t k = 0; k < 50; ++k)
        if (g.U32(s::kWpRegistry + 16u * k) == id) return true;
    return false;
}
} // namespace

uint32_t BikeClassModel(GuestRam& g, uint32_t cls, uint32_t fallback) {
    const uint32_t v1 = ((cls - 3u) < 3u || (cls - 12u) < 3u) ? 1u : 0u;     // SLUS 0x8002FB84
    const uint32_t id = 100u + cls - 3u * v1;
    return InRegistry(g, id) ? id : fallback;
}

uint32_t RiderClassModel(GuestRam& g, uint32_t cls, uint32_t fallback) {
    const uint32_t id = 150u + g.U32(g.gp() + 572u + 4u * ((cls - 9u) < 9u ? 1u : 0u)); // SLUS 0x8002FB60
    return InRegistry(g, id) ? id : fallback;
}

void RelocateRegionZero(GuestRam& g, uint32_t body) {
    const uint32_t r0 = g.U32(body + 0x20u);                                  // SLUS 0x800135E8
    for (uint32_t k = 0; k < 5; ++k) {
        const uint32_t w = r0 + 0x24u + 4u * k;
        g.W32(w, g.U16(r0 + 2u * k) != 0 ? r0 + g.U32(w) : 0u);
    }
}

bool WorldProductCallees::Call(uint32_t fn, const uint32_t* a, int n, uint32_t sp, uint32_t& v0) {
    v0 = 0;
    switch (fn) {
    case s::kWpBuildObbFn:
        ++obbs;
        return pop.BuildObb(a[0]);
    case s::kWpGroundQueryFn: {
        ++grounds;
        const s::GroundResult r = s::GroundQuery(g, a[0], a[1], a[2], a[3], n > 4 ? a[4] : 0u, t.rsqrt);
        if (r.declined) {
            ++refused;
            if (note) note("GroundQuery 0x800A7BF8 (PORTED) declined a polygon under a moving prop: the prop pass refused");
            return false;
        }
        v0 = r.value;
        return true;
    }
    case s::kWpResetBikeFn: return pop.ResetBike(a[0], sp);
    case s::kWpTransitionFn: return s::Transition(g, a[0], a[1], sp, pop);
    case s::kWpPedSpawnFn:
        ++peds;
        ++g_totals.peds;
        if (pedSpawn) return pedSpawn(a, sp, v0); // PORTED (peds.h), run by the session
        if (note)
            note("RASHCDG 0x800CB8C8 the pedestrian spawner is not ported: the cell walker asks for it and it is NOT "
                 "RUN (v0 = 0) - no pedestrian appears, its record stays unspawned");
        return true;
    case s::kWpHazardSpawnFn:
        ++hazards;
        ++g_totals.hazards;
        if (HazardsOn()) return HazardSpawnSeam(g, a, n, sp, t, v0); // PORTED (hazard_product.h)
        if (note)
            note("RASHCDG 0x800A0A20 the hazard-object spawner (kind-4 classes 0 / 9) is not ported: the cell walker "
                 "asks for it and it is NOT RUN - the object is not made (its pool slot stays marked, as the arm leaves it)");
        return true;
    default:
        ++refused;
        return false;
    }
}

bool RunCellWalker(GuestRam& g, uint32_t sp, WorldProductCallees& c) {
    ++g_totals.walks;
    const uint32_t props0 = g.U32(s::kWpPool4Ctrl) + g.U32(s::kWpPool5Ctrl), vols0 = g.U32(s::kWpPool6Ctrl);
    const bool ok = s::CellWalker(g, sp, c.t, c) && !g.Faulted();
    if (!ok) {
        ++g_totals.walkRefused;
        return false;
    }
    const uint32_t props1 = g.U32(s::kWpPool4Ctrl) + g.U32(s::kWpPool5Ctrl), vols1 = g.U32(s::kWpPool6Ctrl);
    if (props1 > props0) g_totals.propSpawns += props1 - props0;
    if (vols1 > vols0) g_totals.volumeSpawns += vols1 - vols0;
    if (props1 > g_totals.maxProps) g_totals.maxProps = props1;
    if (vols1 > g_totals.maxVolumes) g_totals.maxVolumes = vols1;
    return true;
}

bool RunWorldPasses(GuestRam& g, int32_t dt, uint32_t sp, WorldProductCallees& c) {
    ++g_totals.passes;
    bool ok = s::PropPass(g, dt, sp, c) && !g.Faulted();                       // 0x8008ACB0
    if (ok && c.pedPass && g.U32(0x8005B254u) != 0 && !c.pedPass(dt, sp)) g.ClearFault(); // 0x8008ACC0: PedPass (peds.h)
    ok = ok && s::Pool5Pass(g, sp) && !g.Faulted();                            // 0x8008ACC8
    ok = ok && s::VolumePass(g, sp) && !g.Faulted();                           // 0x8008ACD0
    if (!ok) ++g_totals.passRefused;
    return ok;
}

bool RunWorldChild(GuestRam& g, uint32_t fn, int32_t dt, uint32_t sp, WorldProductCallees& c) {
    bool ok = false;
    if (fn == 0x800A2898u) {
        ++g_totals.passes;
        ok = s::PropPass(g, dt, sp, c) && !g.Faulted();
    } else if (fn == 0x8009ACA4u) {
        ok = s::Pool5Pass(g, sp) && !g.Faulted();
    } else if (fn == 0x8009AB60u) {
        ok = s::VolumePass(g, sp) && !g.Faulted();
    }
    if (!ok) ++g_totals.passRefused;
    return ok;
}

void DrawLoopPools(GuestRam& g, int players) {
    for (int p = 0; p < players; ++p) {
        (void)p; // the view index only reaches ModelVisible, which the model runtime runs
        for (uint32_t pool = 2; pool < 6; ++pool) {                           // 0x8008D6F8..0x8008D7FC
            const uint32_t rec = s::kWpPoolTable + 16u * pool;
            if (g.U32(rec + 12u) == 0) continue; // OURS: a pool the arena has no record of
            int32_t n = g.S32(g.U32(rec + 12u));
            uint32_t e = g.U32(rec);
            for (; n >= 0; --n, e += g.U32(rec + 4u)) {
                if (g.U16(e + 0xACu) == 0) continue;
                if (s::EntityCell(g, e) <= 0) continue;                        // 0x8008B99C
                g.W32(e + 0x0Cu, static_cast<uint32_t>(g.S32(e + 0xB8u) >> 10));
                g.W32(e + 0x14u, static_cast<uint32_t>(g.S32(e + 0xC0u) >> 10));
                g.W32(e + 0x10u, static_cast<uint32_t>(g.S32(e + 0xBCu) >> 10));
                const uint32_t m = g.U8(e + 0x48u) != 3u ? g.U32(e + 4u) + 4u : e + 0x68u;
                g.W16(m + 0u, g.U16(e + 0x1B0u));
                g.W16(m + 6u, g.U16(e + 0x1B2u));
                g.W16(m + 12u, g.U16(e + 0x1B4u));
                g.W16(m + 2u, g.U16(e + 0x1B6u));
                g.W16(m + 8u, g.U16(e + 0x1B8u));
                g.W16(m + 14u, g.U16(e + 0x1BAu));
                g.W16(m + 4u, g.U16(e + 0x1BCu));
                g.W16(m + 10u, g.U16(e + 0x1BEu));
                g.W16(m + 16u, g.U16(e + 0x1C0u));
                if (g.Faulted()) {
                    g.ClearFault();
                    break;
                }
            }
        }
    }
}

std::vector<LiveProp> LiveProps(const uint8_t* ram) {
    auto u32 = [ram](uint32_t a) {
        uint32_t v;
        std::memcpy(&v, ram + (a & 0x1FFFFFu), 4);
        return v;
    };
    auto u16 = [ram](uint32_t a) {
        uint16_t v;
        std::memcpy(&v, ram + (a & 0x1FFFFFu), 2);
        return v;
    };
    std::vector<LiveProp> out;
    for (uint32_t pool = 4; pool <= 5; ++pool) {
        const uint32_t rec = s::kWpPoolTable + 16u * pool;
        if (u32(rec + 12u) == 0 || u32(rec) == 0) continue;
        int32_t n = static_cast<int32_t>(u32(u32(rec + 12u)));
        uint32_t e = u32(rec);
        const uint32_t stride = u32(rec + 4u);
        for (int32_t k = 0; k <= n && k < 64; ++k, e += stride) {
            if ((e & 0xFF000000u) != 0x80000000u) break;
            if (u16(e + 0xACu) == 0) continue;
            LiveProp lp;
            lp.entity = e;
            lp.cls = u32(e + 0xB4u);
            for (uint32_t j = 0; j < 3; ++j) lp.pos[j] = static_cast<int32_t>(u32(e + 0xB8u + 4u * j));
            for (uint32_t j = 0; j < 9; ++j) lp.rows[j] = static_cast<int16_t>(u16(e + 0x1B0u + 2u * j));
            lp.cell = static_cast<int32_t>(u32(e + 0xB0u));
            out.push_back(lp);
        }
    }
    return out;
}

std::string WorldTotalsLine() {
    const WorldTotals& t = g_totals;
    char b[640];
    std::snprintf(b, sizeof(b),
                  "the world population (world_pop_product.h): the PORTED cell walker 0x8009C308 ran %zu time(s), %zu refused; the "
                  "PORTED prop / pool-5 / volume passes %zu frame(s), %zu refused; props spawned %zu (at most %zu live), "
                  "collision volumes spawned %zu (at most %zu live); pedestrian spawns asked %zu (run by the "
                  "pedestrian port while its switch is on - see PEDS), hazard objects asked %zu (run by hazard_product.h)\n",
                  t.walks, t.walkRefused, t.passes, t.passRefused, t.propSpawns, t.maxProps, t.volumeSpawns, t.maxVolumes,
                  t.peds, t.hazards);
    return b;
}

namespace {

// The seams for the capture check: BuildObb natively on the image; a pedestrian / hazard spawn is not run.
struct CheckCallees final : rr::sim::RecoverCallees {
    GuestRam& g;
    uint8_t* ram;
    size_t other = 0;
    CheckCallees(GuestRam& gg, uint8_t* r) : g(gg), ram(r) {}
    bool Call(uint32_t fn, const uint32_t* a, int, uint32_t, uint32_t& v0) override {
        v0 = 0;
        if (fn == s::kWpBuildObbFn) {
            const uint32_t e = a[0];
            if (e < 0x80000000u || e + 1096u > 0x80200000u) return false;
            rr::sim::EntityView v(ram + (e - 0x80000000u));
            int32_t rev = 0;
            if ((g.U16(e + 0xACu) >> 5) == 0) {
                const uint32_t owner = g.U32(e + 852u);
                rev = owner != 0 ? g.S32(owner + 604u) : 0;
            }
            rr::sim::BuildObb(v, rev);
            return true;
        }
        ++other;
        return true;
    }
};

struct Span {
    uint32_t off, len;
    const char* what;
};

} // namespace

int CheckWorldSpawn(const std::string& ramPathIn, bool mutate) {
    std::string ramPath = ramPathIn;
    if (ramPath.size() < 4 || ramPath.substr(ramPath.size() - 4) != ".bin") ramPath += "\\ram.bin";
    std::vector<uint8_t> A(GuestRam::kRamSize), B;
    if (FILE* f = std::fopen(ramPath.c_str(), "rb")) {
        const size_t got = std::fread(A.data(), 1, A.size(), f);
        std::fclose(f);
        if (got != A.size()) {
            std::printf("worldcheck: %s is not a 2 MiB RAM image\n", ramPath.c_str());
            return 2;
        }
    } else {
        std::printf("worldcheck: cannot open %s\n", ramPath.c_str());
        return 2;
    }
    constexpr uint32_t kGp = 0x8005AC8C;
    GuestRam ga(A.data(), kGp);
    B = A;
    GuestRam gb(B.data(), kGp);
    size_t propsA = 0, volsA = 0, recReset = 0;
    for (uint32_t pool = 4; pool <= 5; ++pool) { // the capture's live props out of B
        const uint32_t rec = s::kWpPoolTable + 16u * pool;
        const int32_t n = ga.S32(ga.U32(rec + 12u));
        const uint32_t stride = ga.U32(rec + 4u);
        for (int32_t k = 0; k <= n; ++k) {
            const uint32_t e = ga.U32(rec) + stride * static_cast<uint32_t>(k);
            if (ga.U16(e + 0xACu) == 0) continue;
            ++propsA;
            for (uint32_t o = 0; o < (pool == 4 ? 596u : 452u); o += 4) gb.W32(e + o, 0);
        }
    }
    for (uint32_t c : {s::kWpPool4Ctrl, s::kWpPool5Ctrl}) {
        gb.W32(c, 0);
        gb.W32(c + 4u, 0);
        gb.W32(c + 8u, 0xFFFFFFFFu);
    }
    for (uint32_t a = s::kWpStore; a < s::kWpStore + 18u * 24u; a += 4) gb.W32(a, 0);
    gb.W32(s::kWpStoreLow, 0);
    gb.W32(s::kWpStoreFree, 8344);
    for (int32_t k = 0; k <= ga.S32(s::kWpPool6Ctrl + 8u); ++k) { // and its volumes
        const uint32_t v = ga.U32(s::kWpPool6Ptr) + 280u * static_cast<uint32_t>(k);
        if (ga.U16(v) == 0) continue;
        ++volsA;
        for (uint32_t o = 0; o < 280u; o += 4) gb.W32(v + o, 0);
    }
    gb.W32(s::kWpPool6Ctrl, 0);
    gb.W32(s::kWpPool6Ctrl + 4u, 0);
    gb.W32(s::kWpPool6Ctrl + 8u, 0xFFFFFFFFu);
    for (uint32_t k = 0; k < 12; ++k) { // the records that made them: fresh again (kinds 4 and 6, player 0)
        const uint32_t sl = s::kWpCellSlots + 112u * k;
        if (gb.U32(sl) == 0xFFFFFFFFu || gb.U32(sl + 4u) == 0) continue;
        const uint32_t r0 = gb.U32(gb.U32(sl + 4u) + 32u);
        for (uint32_t arr : {2u, 3u}) {
            const uint32_t stride = arr == 2 ? 64u : 88u;
            const uint32_t base = gb.U32(r0 + 36u + 4u * arr);
            for (uint32_t i = 0; i < gb.U16(r0 + 2u * arr); ++i) {
                const uint32_t rec = base + stride * i;
                if (mutate) gb.W32(rec + 20u, gb.U32(rec + 20u) + 0x400u);
                if (gb.S16(rec + 4u) <= 0) continue;
                gb.W16(rec + 4u, 0xFFFF);
                gb.W16(rec + 6u, 0xFFFF);
                ++recReset;
            }
        }
    }
    const s::BikeTables t = s::RecoverTables(B.data(), gb);
    // the cells the walker visits for player 0 now (0x8009FAD8, on a copy: it writes the stack)
    std::vector<uint32_t> walked;
    {
        std::vector<uint8_t> C = B;
        GuestRam gc(C.data(), kGp);
        const int32_t n = s::CellsAround(gc, 0x801FF000u, 0, 0x801FFE00u);
        for (int32_t k = 0; k < n && k < 8; ++k) walked.push_back(gc.U32(0x801FF000u + 4u * static_cast<uint32_t>(k)));
    }
    CheckCallees cc(gb, B.data());
    const bool ran = s::CellWalker(gb, 0x801FFE00u, t, cc) && !gb.Faulted();
    static const Span kProp[] = {{0xB4, 4, "class"},          {0xB8, 12, "settled position"}, {0x130, 16, "box, mass"},
                                 {0x148, 44, "cursor, road"}, {0x1B0, 18, "rows"},            {0x124, 12, "heading"},
                                 {0x1F8, 12, "rest point"},   {0x204, 18, "rows copy"},       {0x230, 8, "quaternion"}};
    static const Span kVol[] = {{0x08, 4, "class"},    {0x0C, 12, "centre"},     {0x18, 96, "corners"},
                                {0x78, 12, "heading"}, {0x84, 16, "size, mass"}, {0x9C, 44, "cursor, road"},
                                {0x104, 18, "frame"}};
    size_t matched = 0, compared = 0, differ = 0, missing = 0, extra = 0;
    std::string first;
    auto compare = [&](uint32_t ea, uint32_t eb, const Span* sp, size_t n, const char* kind) {
        for (size_t i = 0; i < n; ++i)
            for (uint32_t o = 0; o < sp[i].len; ++o) {
                ++compared;
                if (ga.U8(ea + sp[i].off + o) == gb.U8(eb + sp[i].off + o)) continue;
                ++differ;
                if (!first.empty()) continue;
                char b[200];
                std::snprintf(b, sizeof(b), "%s at 0x%08X: %s +0x%X differs (capture 0x%02X, port 0x%02X)", kind, ea,
                              sp[i].what, sp[i].off + o, ga.U8(ea + sp[i].off + o), gb.U8(eb + sp[i].off + o));
                first = b;
            }
    };
    // matched by the record-derived road coordinate and class
    auto sameKey = [&](uint32_t ea, uint32_t eb, uint32_t road, uint32_t clsOff) {
        for (uint32_t o = 0; o < 12; o += 4)
            if (ga.U32(ea + road + o) != gb.U32(eb + road + o)) return false;
        return ga.U32(ea + clsOff) == gb.U32(eb + clsOff);
    };
    // A capture entity the walker does not make again is explained when it stands outside the spawn window
    // of the capture's camera (0x80013110: whole-unit octagonal distance, props 200, volumes gp+204 = 80):
    // the original spawned it from nearer and keeps it until the release radius (230 / 100).
    size_t kept = 0;
    auto cellWalked = [&](uint32_t e) {
        return std::find(walked.begin(), walked.end(), ga.U32(e)) != walked.end();
    };
    auto outsideSpawn = [&](uint32_t pos, int32_t radius) {
        int32_t a = (ga.S32(pos) >> 16) - (ga.S32(0x800CD898u + 184u) >> 16);
        int32_t b = (ga.S32(pos + 8u) >> 16) - (ga.S32(0x800CD898u + 192u) >> 16);
        a = a < 0 ? -a : a;
        b = b < 0 ? -b : b;
        if (a < b) std::swap(a, b);
        const int32_t m = b + (b >> 1);
        return a - (a >> 5) - (a >> 7) + (m >> 2) + (m >> 6) >= radius;
    };
    std::vector<uint32_t> bProps, bVols;
    for (uint32_t pool = 4; pool <= 5; ++pool) {
        const uint32_t rec = s::kWpPoolTable + 16u * pool;
        const int32_t n = gb.S32(gb.U32(rec + 12u));
        for (int32_t k = 0; k <= n; ++k) {
            const uint32_t e = gb.U32(rec) + gb.U32(rec + 4u) * static_cast<uint32_t>(k);
            if (gb.U16(e + 0xACu) != 0) bProps.push_back(e);
        }
    }
    for (int32_t k = 0; k <= gb.S32(s::kWpPool6Ctrl + 8u); ++k) {
        const uint32_t v = gb.U32(s::kWpPool6Ptr) + 280u * static_cast<uint32_t>(k);
        if (gb.U16(v) != 0) bVols.push_back(v);
    }
    std::vector<bool> usedP(bProps.size(), false), usedV(bVols.size(), false);
    for (uint32_t pool = 4; pool <= 5; ++pool) {
        const uint32_t rec = s::kWpPoolTable + 16u * pool;
        const int32_t n = ga.S32(ga.U32(rec + 12u));
        for (int32_t k = 0; k <= n; ++k) {
            const uint32_t e = ga.U32(rec) + ga.U32(rec + 4u) * static_cast<uint32_t>(k);
            if (ga.U16(e + 0xACu) == 0) continue;
            size_t hit = bProps.size();
            for (size_t j = 0; j < bProps.size() && hit == bProps.size(); ++j)
                if (!usedP[j] && sameKey(e, bProps[j], 0x168u, 0xB4u)) hit = j;
            if (hit == bProps.size()) {
                if (outsideSpawn(e + 0xB8u, 200) || !cellWalked(e + 0xB0u)) ++kept;
                else ++missing;
                continue;
            }
            usedP[hit] = true;
            ++matched;
            compare(e, bProps[hit], kProp, sizeof(kProp) / sizeof(kProp[0]), "prop");
        }
    }
    for (int32_t k = 0; k <= ga.S32(s::kWpPool6Ctrl + 8u); ++k) {
        const uint32_t v = ga.U32(s::kWpPool6Ptr) + 280u * static_cast<uint32_t>(k);
        if (ga.U16(v) == 0) continue;
        size_t hit = bVols.size();
        for (size_t j = 0; j < bVols.size() && hit == bVols.size(); ++j)
            if (!usedV[j] && sameKey(v, bVols[j], 0xBCu, 0x08u)) hit = j;
        if (hit == bVols.size()) {
            if (outsideSpawn(v + 12u, ga.S32(s::kWpVolumeRadius) >> 16)) ++kept; // a volume keeps no cell: +4 the cell id
            else if (std::find(walked.begin(), walked.end(), ga.U32(v + 4u)) == walked.end()) ++kept;
            else ++missing;
            continue;
        }
        usedV[hit] = true;
        ++matched;
        // a pole (class 1) has four footprint corners only (0x8009BEB0..0x8009BFDC); a box has the four tops
        // +0x48..+0x6B too (0x8009C1FC). The pole's +0x48.. is whatever the slot held before.
        Span spans[sizeof(kVol) / sizeof(kVol[0])];
        std::memcpy(spans, kVol, sizeof(kVol));
        if (ga.U32(v + 8u) == 1u) spans[2].len = 48;
        compare(v, bVols[hit], spans, sizeof(kVol) / sizeof(kVol[0]), "volume");
    }
    for (bool u : usedP) extra += u ? 0u : 1u;
    for (bool u : usedV) extra += u ? 0u : 1u;
    const bool pass = ran && matched > 0 && missing == 0 && differ == 0; // nothing to compare is no evidence
    std::printf("worldcheck %s%s: capture props %zu, volumes %zu; %zu record(s) made fresh; the PORTED walker %s, made "
                "props %zu, volumes %zu (other seams asked %zu); matched %zu, kept outside the spawn window or the walked cells %zu, missing %zu, "
                "extra %zu; %zu byte(s) "
                "compared, %zu differ%s%s -> %s\n",
                ramPath.c_str(), mutate ? " (MUTATED: every kind-4 / kind-6 record moved by 1/64 unit)" : "", propsA, volsA,
                recReset, ran ? "ran" : "REFUSED", bProps.size(), bVols.size(), cc.other, matched, kept, missing, extra, compared,
                differ, first.empty() ? "" : "; first: ", first.c_str(),
                pass ? "PASS" : (matched == 0 && missing == 0 && ran ? "NOTHING TO COMPARE" : "FAIL"));
    return pass ? 0 : 1;
}

} // namespace rr::game
