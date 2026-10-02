// The hazard objects in the product (hazard_product.h). BuildRace's block copy is transcribed from our own
// listing of RASHCDI.BIN (SHA-1 9a8b79d8739713c12b0a2dc4d75ef8f0a2cc0b06); everything else is hazard.h's.
#include "game/hazard_product.h"
#include "game/loader_product.h" // LoaderPorted, LoaderTotals

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace rr::game {
namespace {

using rr::sim::GuestRam;
namespace s = rr::sim;

struct Totals {
    bool setup = false;
    uint32_t events = 0, objects = 0;
    int set = -1;
    size_t walkerSpawns = 0, walkerRefused = 0, eventSpawns = 0, releases = 0, maxLive = 0, passes = 0,
           passRefused = 0, draws = 0, drawRefused = 0, framesDrawn = 0, maxDrawn = 0;
    int32_t nearest = 0x7FFFFFFF; // whole units, octagonal, to player 1's bike
    int32_t highest = 0;          // whole units above player 1's bike (-y)
    size_t firstSpawn = 0, firstDrawn = 0, lastDrawn = 0; // the pass count then (~ the race frame)
    int32_t eventGap = 0x7FFFFFFF; // whole units: player 1's nearest approach to an armed event on its road
    size_t onEventRoad = 0;        // passes player 1 spent on an armed event's road
};
Totals g_t;

// OURS: the loader's mallocs from the session's bump region (SLUS 0x8001447C's block rule).
struct ArenaMalloc final : s::RecoverCallees {
    uint32_t& next;
    uint32_t limit;
    size_t refused = 0;
    ArenaMalloc(uint32_t& n, uint32_t l) : next(n), limit(l) {}
    bool Call(uint32_t fn, const uint32_t* a, int, uint32_t, uint32_t& v0) override {
        v0 = 0;
        if (fn != s::kHzMallocFn) return false;
        const uint32_t size = (a[0] + 11u) & ~7u;
        if (a[0] == 0 || next + size > limit) {
            ++refused;
            return false;
        }
        v0 = next + 4u;
        next += size;
        return true;
    }
};
// HazardDraw's ModelVisible: not run (hazard_product.h).
struct NoVisible final : s::RecoverCallees {
    bool Call(uint32_t fn, const uint32_t*, int, uint32_t, uint32_t& v0) override {
        v0 = 0;
        return fn == s::kHzModelVisibleFn;
    }
};

void Watch(GuestRam& g) {
    const uint32_t live = g.U32(s::kHzOut);
    g_t.maxLive = std::max<size_t>(g_t.maxLive, live);
    const uint32_t bike = g.U32(s::kHzBikePtr);
    const uint32_t base = g.U32(s::kHzRecordsPtr);
    if (base == 0 || bike == 0) return;
    for (uint32_t i = 0; i < 3; ++i) {
        const uint32_t r = base + 280u * i;
        if (g.U16(r + 172u) == 0) continue;
        int32_t a = (g.S32(r + 184u) - g.S32(bike + 184u)) >> 16, b = (g.S32(r + 192u) - g.S32(bike + 192u)) >> 16;
        a = a < 0 ? -a : a;
        b = b < 0 ? -b : b;
        if (a < b) std::swap(a, b);
        const int32_t m = b + (b >> 1);
        g_t.nearest = std::min(g_t.nearest, a - (a >> 5) - (a >> 7) + (m >> 2) + (m >> 6));
        g_t.highest = std::max(g_t.highest, (g.S32(bike + 188u) - g.S32(r + 188u)) >> 16);
    }
}

} // namespace

bool HazardsOn() {
    static const bool on = [] {
        const char* v = std::getenv("RRJB_HAZARDS");
        return !(v != nullptr && std::strcmp(v, "off") == 0);
    }();
    return on;
}

int HazardSetForRace(GuestRam& g, const rr::DiscImage& disc, int set, int raceId, uint8_t mode, HazardRace& h,
                     std::string& line) {
    // RASHCDI 0x80068470 (BuildRace, via 0x800689D0): block + 220 -> 0x8005B328 (8 bytes), + 228 -> the event
    // count 0x8006B8B4, + 232 -> the three templates 0x8006EB10 (the last two host-side, hazard_product.h)
    const std::string name = std::string("DATA/STARTDF") + (set == 2 ? "B" : "A") + ".BIN";
    std::vector<uint8_t> f;
    if (const auto e = disc.Find(name)) f = disc.ReadFile(*e);
    const size_t at = 292u * static_cast<size_t>(raceId > 0 ? raceId - 1 : 0);
    if (raceId >= 1 && f.size() >= at + 292u && LoaderPorted()) { // RaceBlockCopy PORTED
        std::string error;
        uint8_t events[48];
        if (!LoaderBlockCopy(g, nullptr, disc, f.data() + at, kLoaderSp + 24u, kLoaderSp, h.data.eventCount, events, error)) {
            line = "the hazard set: " + error;
            return -1;
        }
        std::memcpy(h.data.events, events, 48);
    } else if (raceId >= 1 && f.size() >= at + 292u) {
        g.WriteBlock(s::kHzTable, f.data() + at + 220u, 8);
        std::memcpy(&h.data.eventCount, f.data() + at + 228u, 4);
        std::memcpy(h.data.events, f.data() + at + 232u, 48);
    }
    // RASHCDI 0x800655D4: ENV.EN whole (12888 bytes) into the loader's BSS 0x8006B8B8
    if (const auto e = disc.Find("DATA/ENV.EN")) h.env = disc.ReadFile(*e);
    h.data.env = h.env.data();
    h.data.envSize = h.env.size();
    char b[300];
    if (h.env.size() < 12888u) {
        line = "the hazard set: DATA\\ENV.EN is not on this disc - HazardPick not run";
        return -1;
    }
    int result;
    if ((mode & 0x10u) != 0) {
        h.pick = 1;                                              // 0x80063814: s0 = 1, HazardPick not called
    } else {
        int32_t sel = 0, v0 = 0;
        if (!s::HazardPick(g, h.data.env, h.data.envSize, sel, v0)) {
            line = "RASHCDI 0x8006AD4C HazardPick (PORTED) refused";
            return -1;
        }
        h.data.sel = sel;
        h.pick = v0;
    }
    result = (mode & 0x18u) == 8u ? 0 : h.pick % 10;             // 0x8005C7F0's digit
    g_t.set = result;
    LoaderTotals().hazardPick = h.pick; // SetUpRace's 0x8006AD4C
    std::snprintf(b, sizeof(b),
                  "the hazard set (hazard_product.h): %s block %d's class table {%d, %d} {%d, %d} (RASHCDI 0x80068470 "
                  "%s), %d event(s); HazardPick 0x8006AD4C (PORTED) pair %d -> HAZARD%d",
                  name.c_str(), raceId - 1, g.S8(s::kHzTable), g.S8(s::kHzTable + 1u), g.S8(s::kHzTable + 4u),
                  g.S8(s::kHzTable + 5u), LoaderPorted() ? "PORTED, the race loader" : "transcribed", h.data.eventCount, h.data.sel, result);
    line = b;
    return result;
}

std::string BuildHazardArenaLine(GuestRam& g, const HazardRace& h, uint8_t mode, uint32_t& from, uint32_t limit) {
    if (!HazardsOn()) return "the hazard objects: RRJB_HAZARDS=off - HazardSetup 0x8006AEF4 not run (the negative control)";
    if (mode & 0x10u) return "the hazard objects: two-player mode - HazardSetup 0x8006AEF4 is not called (0x800639AC)";
    if (h.env.size() < 12888u) return "the hazard objects: no ENV.EN - HazardSetup 0x8006AEF4 not run";
    uint32_t next = (from + 7u) & ~7u;
    ArenaMalloc mc(next, limit);
    const bool ok = s::HazardSetup(g, h.data, 0x801FF000u, mc) && !g.Faulted();
    g.ClearFault();
    from = next;
    g_t.setup = ok;
    g_t.events = g.U32(s::kHzEventCount);
    g_t.objects = g.U32(s::kHzObjectCount);
    char b[400];
    std::snprintf(b, sizeof(b),
                  "the hazard objects (hazard_product.h): RASHCDI 0x8006AEF4 HazardSetup (PORTED)%s: %u event(s) at "
                  "0x%08X, %u object(s) listed of 6 at 0x%08X, 3 records at 0x%08X, class table {%d, %d} {%d, %d} "
                  "(OURS: where the mallocs sit)",
                  ok ? "" : " REFUSED", g_t.events, g.U32(s::kHzEventsPtr), g_t.objects, g.U32(s::kHzObjectsPtr),
                  g.U32(s::kHzRecordsPtr), g.S8(s::kHzTable), g.S8(s::kHzTable + 1u), g.S8(s::kHzTable + 4u),
                  g.S8(s::kHzTable + 5u));
    return b;
}

bool HazardSpawnSeam(GuestRam& g, const uint32_t* a, int n, uint32_t sp, const s::BikeTables& t, uint32_t& v0) {
    v0 = 0;
    if (n < 5 || g.U32(s::kHzRecordsPtr) == 0) { // no set-up (two-player mode never gets here: the arm is gated)
        ++g_t.walkerRefused;
        return true;
    }
    const bool ok = s::HazardSpawn(g, a[0], a[1], a[2], a[3], a[4], sp, t, v0) && !g.Faulted();
    if (!ok) {
        ++g_t.walkerRefused;
        return false;
    }
    if (v0 != 0) ++g_t.walkerSpawns;
    if (v0 != 0 && g_t.firstSpawn == 0) g_t.firstSpawn = g_t.passes;
    Watch(g);
    return true;
}

bool RunHazardPass(GuestRam& g, int32_t dt, uint32_t sp, const s::BikeTables& t) {
    if (g.U32(s::kHzRecordsPtr) == 0) return true; // the product's two-player races: no blocks
    ++g_t.passes;
    const uint32_t before = g.U32(s::kHzOut);
    {   // where player 1 is against the armed events (the pass's own test reads the same words)
        const uint32_t bike = g.U32(s::kHzBikePtr), ev = g.U32(s::kHzEventsPtr);
        for (int32_t k = 0; ev != 0 && k < g.S32(s::kHzEventCount) && k < 8; ++k) {
            const uint32_t e = ev + 16u * static_cast<uint32_t>(k);
            if (g.S8(e + 12u) == 0 || g.U32(e) != g.U32(bike + 360u)) continue;
            ++g_t.onEventRoad;
            int32_t d = (g.S32(e + 4u) - g.S32(bike + 368u)) >> 16;
            d = d < 0 ? -d : d;
            g_t.eventGap = std::min(g_t.eventGap, d);
        }
    }
    const bool ok = s::HazardPass(g, dt, sp, t) && !g.Faulted();
    if (!ok) {
        ++g_t.passRefused;
        g.ClearFault();
        return false;
    }
    const uint32_t after = g.U32(s::kHzOut);
    // the pass spawns only from zero (the events) and otherwise only releases
    if (before == 0 && after > 0) g_t.eventSpawns += after;
    if (before == 0 && after > 0 && g_t.firstSpawn == 0) g_t.firstSpawn = g_t.passes;
    else if (after < before) g_t.releases += before - after;
    Watch(g);
    return true;
}

bool RunHazardDraw(GuestRam& g, int players, uint32_t sp) {
    if (g.U32(s::kHzOut) == 0 || g.U32(s::kHzRecordsPtr) == 0) return true; // SLUS 0x80012010
    NoVisible nv;
    bool ok = true;
    for (int p = 0; p < players && ok; ++p) {
        ++g_t.draws;
        ok = s::HazardDraw(g, static_cast<uint32_t>(p), sp, nv) && !g.Faulted();
    }
    if (!ok) {
        ++g_t.drawRefused;
        g.ClearFault();
    }
    return ok;
}

std::vector<LiveHazard> LiveHazards(const uint8_t* ram) {
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
    std::vector<LiveHazard> out;
    const uint32_t base = u32(s::kHzRecordsPtr);
    if (u32(s::kHzOut) == 0 || (base & 0xFF000000u) != 0x80000000u) return out;
    for (uint32_t i = 0; i < 3; ++i) {
        const uint32_t r = base + 280u * i;
        if (u16(r + 172u) == 0) continue;
        LiveHazard h;
        h.record = r;
        h.group = static_cast<int8_t>(ram[(r + 8u) & 0x1FFFFFu]) < 0 ? 0u : ram[(r + 8u) & 0x1FFFFFu]; // LodSelect's +8
        for (uint32_t k = 0; k < 3; ++k) h.pos[k] = static_cast<int32_t>(u32(r + 184u + 4u * k));
        for (uint32_t k = 0; k < 9; ++k) h.rows[k] = static_cast<int16_t>(u16(r + 196u + 2u * k));
        h.cell = static_cast<int32_t>(u32(r + 176u));
        h.viewBits = ram[(r + 9u) & 0x1FFFFFu];
        out.push_back(h);
    }
    return out;
}

void AppendHazardRecords(const uint8_t* ram, std::vector<uint32_t>& entities) {
    if (!HazardsOn()) return;
    for (const LiveHazard& h : LiveHazards(ram)) entities.push_back(h.record);
}

void NoteHazardDrawn(size_t n) {
    if (n == 0) return;
    ++g_t.framesDrawn;
    if (g_t.firstDrawn == 0) g_t.firstDrawn = g_t.passes;
    g_t.lastDrawn = g_t.passes;
    g_t.maxDrawn = std::max(g_t.maxDrawn, n);
}

std::string HazardTotals() {
    const Totals& t = g_t;
    char b[900];
    std::snprintf(b, sizeof(b),
                  "the hazard objects (hazard_product.h)%s: HAZARD%d, set-up %s (%u event(s), %u object(s) listed); spawned %zu "
                  "by the cell walker's class-9 arm (%zu refused) and %zu by the events (the PORTED pass 0x800A13C4), "
                  "released %zu, at most %zu live; the pass ran %zu frame(s) (%zu refused), HazardDraw 0x800A2138 %zu "
                  "view(s) (%zu refused); first spawn at pass %zu; drawn in %zu frame(s) (at most %zu, passes %zu..%zu); nearest approach to player 1 %s units, "
                  "highest %d units above; player 1 on an armed event's road %zu pass(es), nearest %s units along; collisions: none (no reader of the records in the collision code)\n",
                  HazardsOn() ? "" : " - RRJB_HAZARDS=off", t.set, t.setup ? "ran" : "NOT RUN", t.events, t.objects,
                  t.walkerSpawns, t.walkerRefused, t.eventSpawns, t.releases, t.maxLive, t.passes, t.passRefused,
                  t.draws, t.drawRefused, t.firstSpawn, t.framesDrawn, t.maxDrawn, t.firstDrawn, t.lastDrawn,
                  t.nearest == 0x7FFFFFFF ? "-" : std::to_string(t.nearest).c_str(), t.highest, t.onEventRoad,
                  t.eventGap == 0x7FFFFFFF ? "-" : std::to_string(t.eventGap).c_str());
    return b;
}

} // namespace rr::game
