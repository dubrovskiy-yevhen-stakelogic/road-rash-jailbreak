// The starting grid in the product: BuildGrid RASHCDI 0x80067B00 and SpawnBike
// 0x80065A94, PORTED in src\game\sim\grid_build.{h,cpp} (spawn_bike's bench row; BuildGrid itself cannot be
// benched - its first callee and player 1's SpawnBike read the CD), run on the session's arena with the
// session's callees: the ported SLUS road, route, place and seat functions; the loaders read the player's disc.
// Then BuildRace's next call RASHCDI 0x80067784 (every bike's +0x340 = 0 and ResetBikeState SLUS 0x8002090C).
//
// What is OURS here, named:
//   * the three mallocs return the arena's fixed addresses (rr-race's stat array / pool 0 / pool 1; with two
//     players the stat array's block ends where pool 0's starts, the original's heap order - mp_arena.h);
//   * ModelBind SLUS 0x8002FAD4 is answered with the class (its v0 for pools 1 / 2) and the bike's +0x4C = 1.0
//     (RegistryBind's store 0x8003006C); the registry binding itself is the session's BindModels, after;
//   * the box SLUS 0x80012FC8 reads the model's LOD-0 half extents from the level bundle's .GEO (the model
//     the class names, SLUS 0x8002FB60 / 0x8002FB84) instead of the registry;
//   * player 1's animation banks (0x8005BCDC) are the session's BuildAnimArena, ANIMNOIZ.DAT (0x80063158) is
//     loaded by the PORTED AnimNoise (RRJB_LOADER2=off: not loaded), the weapon objects (ModelBind
//     pool 5) are the weapon arena's; SpawnPassenger 0x800670FC is the session's JailSpawn, after the cursors;
//   * BuildRace RASHCDI 0x8006982C PORTED runs as the grid's orchestrator (loader2.h): BuildGrid and
//     0x80067784 in its site; its pools (0x80069000 / 0x80068AA4) at the world arena's point, the camera 0x80067564 at
//     the camera arena's, RaceBlockCopy 0x80068470 at the hazard set's (answered, named in the LOADER2 line);
//   * the resident piece list RoadGate reads is the session's streaming rule applied at the [START] record.
#include "game/race_session.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <string>
#include <vector>

#include "game/grid_loader.h"        // SaveCopRecord RASHCDI 0x800644C8
#include "game/race_modes.h"         // PlayerCopPlace RASHCDI 0x8006581C
#include "game/sim/grid_build.h"
#include "game/loader_product.h"     // ModelBind PORTED
#include "game/mp_arena.h"           // the two-player stat array by BuildGrid's heap order
#include "game/audio/root_counter.h"  // a scripted run keeps its LCG seed
#include "game/sim/coll_util.h"      // GScale SLUS 0x8002EE50 (TrialLimits)
#include "game/sim/loader2.h"        // BuildRace RASHCDI 0x8006982C
#include "game/sim/spine.h"          // kHeapHeads (SpineMalloc)
#include "game/sim/traffic_bind.h"   // ModelBind SLUS 0x8002FAD4
#include "game/sim/population.h"     // RoadGate, CursorSeat, Attach
#include "game/sim/race.h"           // ComputePlace SLUS 0x800138E8
#include "game/sim/rider_record.h"   // GridRiderAdjust RASHCDI 0x800650A0
#include "game/sim/road_runtime.h"   // RoadPosition, RoadClass, RoadsideRun, RouteBind, ProgressScalar
#include "game/sim/spine.h"          // ResetBikeState SLUS 0x8002090C
#include "game/sim/traffic_leaves.h" // RoadWalk SLUS 0x80012C1C
#include "rrformats/level_bank.h"
#include "rrformats/rmd3.h"

namespace rr::game {

namespace {

using rr::sim::GuestRam;

constexpr uint32_t kGp = 0x8005AC8C;               // SLUS_010.53's gp (race_session.cpp kArenaGp)
constexpr uint32_t kGsPtr = 0x8005B2F8;
constexpr uint32_t kArenaPool0 = 0x801B65D4;       // race_session.cpp: pool 0, stride 1096
constexpr uint32_t kArenaPool1 = 0x801BB2EC;       // race_session.cpp: pool 1, stride 628
constexpr uint32_t kArenaStatArray = 0x801B5ECC;   // race_session.cpp: the stat array (four blocks)
constexpr uint32_t kArenaAnimFiles = 0x801BE13C;   // race_session.cpp: the first bank file (pool 1's end bound)
constexpr uint32_t kGridSp = 0x801FF000;           // race_session.cpp kArenaStepSp: BuildGrid's sp (its frame
                                                   // 0x801FE6C8, the LEVEL<n>.BI buffer 0x801FE7C8 = kRiderBiAt)
constexpr uint32_t kBlockAt = kGridSp + 24u;       // BuildRace's sp+24: the grid block (OURS: BuildRace's frame)
constexpr uint32_t kRouteHeader = 0x800D6170;      // +0x14 -> the start record {road, along, dir}
constexpr uint32_t kPool0Count = 0x8005B1F8, kPool0High = 0x8005AD38;
constexpr uint32_t kPool1Count = 0x8005B218, kPool1High = 0x8005AD3C;
constexpr uint32_t kPoolTable = 0x800CE4D0;
constexpr uint32_t kRouteArmed = 0x800D6182;
constexpr uint32_t kLevelBiBytes = 1728;           // what 0x80064B44 reads (rows_riders.inc)
constexpr uint32_t kLevelPhBytes = 1344;           // 0x800654BC: li a2,1344
constexpr uint32_t kBikePhBytes = 448;             // 0x80065558: li a2,448

std::string Hex(uint32_t v) {
    char b[12];
    std::snprintf(b, sizeof(b), "0x%08X", v);
    return b;
}

std::string GuestString(GuestRam& g, uint32_t a) {
    std::string s;
    for (uint32_t k = 0; k < 256u; ++k) {
        const uint8_t c = g.U8(a + k);
        if (c == 0) break;
        s.push_back(static_cast<char>(c));
    }
    return s;
}

// The level bundle's models, for the boxes (SLUS 0x80012FC8 on LOD 0).
struct GeoModels {
    bool tried = false;
    std::string file, error;
    std::vector<rr::Model> models;
    const rr::ModelGroup* Lod0(uint32_t id) const {
        for (const rr::Model& m : models)
            if (m.id == id && !m.groups.empty()) return &m.groups.front();
        return nullptr;
    }
};

// SpawnBike's callees in the product.
class ProductSpawn final : public rr::sim::SpawnCallees {
public:
    ProductSpawn(RaceSession& s, GuestRam& g, const DiscImage& disc, const rr::sim::BikeTables& t)
        : s_(s), g_(g), disc_(disc), t_(t) {}
    size_t phLoads = 0, phMissing = 0, boxesFromGeo = 0, boxesMissing = 0, animRequests = 0;
    std::string phNames;

    bool StrCpy(uint32_t d, uint32_t src, uint32_t) override {                  // BIOS A(19h) strcpy
        for (uint32_t k = 0;; ++k) {
            const uint8_t c = g_.U8(src + k);
            g_.W8(d + k, c);
            if (c == 0 || g_.Faulted() || k > 4096u) break;
        }
        return !g_.Faulted();
    }
    bool StrCat(uint32_t d, uint32_t src, uint32_t sp) override {               // BIOS strcat
        uint32_t n = 0;
        StrLen(d, sp, n);
        return StrCpy(d + n, src, sp);
    }
    bool StrLen(uint32_t s, uint32_t, uint32_t& v0) override {
        v0 = 0;
        while (g_.U8(s + v0) != 0 && !g_.Faulted() && v0 < 4096u) ++v0;
        return !g_.Faulted();
    }
    bool StrRChr(uint32_t s, uint32_t c, uint32_t, uint32_t& v0) override {
        v0 = 0;
        for (uint32_t k = 0; k < 4096u; ++k) {
            const uint8_t ch = g_.U8(s + k);
            if (ch == static_cast<uint8_t>(c)) v0 = s + k;
            if (ch == 0) break;
        }
        return !g_.Faulted();
    }
    bool StrNCmp(uint32_t a, uint32_t b, uint32_t n, uint32_t, uint32_t& v0) override {
        v0 = 0;
        for (uint32_t k = 0; k < n; ++k) {
            const int32_t x = g_.U8(a + k), y = g_.U8(b + k);
            if (x != y) {
                v0 = static_cast<uint32_t>(x - y);
                break;
            }
            if (x == 0) break;
        }
        return !g_.Faulted();
    }
    bool LoadBikePh(uint32_t name, uint32_t dst, uint32_t) override {          // RASHCDI 0x80065558
        std::string file = GuestString(g_, name);
        for (char& ch : file)
            if (ch == '\\') ch = '/';
        std::vector<uint8_t> bytes;
        if (const auto f = disc_.Find(file)) bytes = disc_.ReadFile(*f);
        phNames += (phNames.empty() ? "" : ", ") + file;
        if (bytes.size() < kBikePhBytes) {                                      // the open fails: nothing read
            ++phMissing;
            return true;
        }
        g_.WriteBlock(dst, bytes.data(), kBikePhBytes);
        ++phLoads;
        return !g_.Faulted();
    }
    bool AnimBank(uint32_t, uint32_t, uint32_t, uint32_t& v0) override {       // the session's BuildAnimArena
        ++animRequests;
        v0 = 0;
        return true;
    }
    bool AnimNoise(uint32_t sp) override {                                      // RASHCDI 0x80063158
        if (!Loader2On() || heapFrom == nullptr) return true;                       // RRJB_LOADER2=off: not loaded
        // PORTED; DATA\ANIMNOIZ.DAT into a block of the session's bump region (OURS: where)
        ProductLoaderCallees c(g_, nullptr, &disc_, heapFrom, heapLimit);
        if (!rr::sim::AnimNoise(g_, sp, c) || g_.Faulted()) {
            Loader2Totals().refused = "AnimNoise: " + (c.error.empty() ? std::string("a fault") : c.error);
            return false;
        }
        ++Loader2Totals().animNoise;
        const uint32_t recs = g_.U32(rr::sim::kL2NoiseRecords), ev = g_.U32(rr::sim::kL2NoiseEvents);
        Loader2Totals().noiseRecords = (recs != 0u && ev > recs + 8u) ? (ev - 8u - recs) / 12u : 0u;
        return true;
    }
    uint32_t* heapFrom = nullptr; // the session's bump region (AnimNoise's file)
    uint32_t heapLimit = 0;
    bool ModelBind(uint32_t obj, uint32_t pool, uint32_t cls, uint32_t alloc, uint32_t, uint32_t& v0) override {
        if (LoaderPorted() && std::getenv("RRJB_LOADER_BIND") == nullptr) { // SLUS 0x8002FAD4 PORTED (traffic_bind.h) on the registry the PORTED
            // LoadBikeBank filled - RegistryBind's stores (+0x60 the record, the part array from the heap below, the
            // +0x24 bits, +0x4C = 1.0 for a bike 0x8003006C, the table key), pools 1 / 2 / 5
            ++LoaderTotals().modelBinds;
            return rr::sim::ModelBind(g_, obj, static_cast<int32_t>(pool), cls, alloc, v0) && !g_.Faulted();
        }
        v0 = cls;                                                               // pools 1 / 2: the class
        if (pool == 2u) g_.W32(obj + 76u, 0x10000u);                            // RegistryBind 0x8003006C
        if (pool == 5u) v0 = 0;                                                 // the weapon arena's objects
        return !g_.Faulted();
    }
    bool BoxSetup(uint32_t e, uint32_t, uint32_t) override {                    // SLUS 0x80012FC8 (idx 0)
        Geo();
        const uint32_t cls = g_.U32(e + 180u);
        const uint32_t pool = g_.U16(e + 172u) >> 5;
        uint32_t id;
        if (pool == 0u) {
            const uint32_t v1 = ((cls - 3u) < 3u || (cls - 12u) < 3u) ? 1u : 0u; // SLUS 0x8002FB84
            id = 100u + cls - 3u * v1;
        } else {
            id = 150u + g_.U32(kGp + 572u + 4u * ((cls - 9u) < 9u ? 1u : 0u));  // SLUS 0x8002FB60
        }
        const rr::ModelGroup* lod0 = geo_.Lod0(id);
        if (lod0 == nullptr) {                                                  // no model: the box stays
            ++boxesMissing;
            return true;
        }
        ++boxesFromGeo;
        const uint32_t shift = ((lod0->flags >> 16) >> 12) & 31u;               // SLUS 0x80012AEC
        int32_t x[3];
        for (int k = 0; k < 3; ++k) x[k] = static_cast<int32_t>(static_cast<uint32_t>(static_cast<int32_t>(lod0->bbox.half[k]) >> shift) << 10);
        if (pool - 1u < 2u) {                                                   // 0x80012FF0: pools 1 / 2
            g_.W32(e + 312u, static_cast<uint32_t>(x[0]) << 1);
            g_.W32(e + 308u, static_cast<uint32_t>(x[1]));
            g_.W32(e + 304u, static_cast<uint32_t>(x[2]));
            return !g_.Faulted();
        }
        g_.W32(e + 304u, static_cast<uint32_t>(x[0]));
        g_.W32(e + 312u, static_cast<uint32_t>(x[1]) << 1);
        g_.W32(e + 308u, static_cast<uint32_t>(x[2]));
        if (pool != 0u) return !g_.Faulted();
        const uint32_t gs = g_.U32(kGsPtr);
        if (cls < 18u) {                                                        // a bike: 9/8 or 11/8 of the length
            const int32_t k = (cls < 9u || g_.U32(gs + 60u) == 2u) ? 2 : 0;
            int32_t v = (k + 9) * g_.S32(e + 312u);
            if (v < 0) v += 7;
            v >>= 3;
            g_.W32(e + 312u, static_cast<uint32_t>(v));
            if (g_.U32(gs + 48u) <= g_.U16(e + 172u)) return !g_.Faulted();
            g_.W32(e + 312u, static_cast<uint32_t>(v - 0x1000));                // a player's: less 1/16
        } else {
            int32_t v = g_.S32(e + 312u) * 11;
            if (v < 0) v += 15;
            g_.W32(e + 312u, static_cast<uint32_t>(v >> 4));
        }
        return !g_.Faulted();
    }
    bool Attach(uint32_t b, uint32_t r, uint32_t kind, uint32_t seat, uint32_t) override {
        rr::sim::Attach(g_, b, r, static_cast<int32_t>(kind), static_cast<int32_t>(seat));
        return !g_.Faulted();
    }
    bool RoadWalk(uint32_t from, uint32_t out, int32_t dist, uint32_t sp) override {
        rr::sim::RoadWalk(g_, from, out, dist, sp);
        return !g_.Faulted();
    }
    bool RoadGate(uint32_t h, uint32_t key, uint32_t, uint32_t& v0) override {
        v0 = rr::sim::RoadGate(g_, h, key);
        return !g_.Faulted();
    }
    bool CursorSeat(uint32_t obj, uint32_t key, uint32_t cur, uint32_t sp, uint32_t& v0) override {
        v0 = static_cast<uint32_t>(rr::sim::CursorSeat(g_, obj, key, cur, sp));
        return !g_.Faulted();
    }
    bool RoadPosition(uint32_t heading, uint32_t cur, uint32_t out, uint32_t sp) override {
        rr::sim::RoadPosition(g_, heading, cur, out, sp);
        return !g_.Faulted();
    }
    bool RoadClass(uint32_t e, int32_t mode, uint32_t out, int32_t zone, uint32_t sp) override {
        rr::sim::RoadClass(g_, e, mode, out, zone, sp, road_);
        return !g_.Faulted();
    }
    bool RoadsideRun(uint32_t e, int32_t mode, int32_t zone, uint32_t sp) override {
        rr::sim::RoadsideRun(g_, e, mode, zone, sp, road_);
        return !g_.Faulted();
    }
    bool RouteBind(uint32_t p, int32_t step, uint32_t x, uint32_t sp) override {
        rr::sim::RouteBind(g_, p, step, x, sp, road_);
        return !g_.Faulted();
    }
    bool Progress(uint32_t p, uint32_t sp, uint32_t& v0) override {
        v0 = static_cast<uint32_t>(rr::sim::ProgressScalar(g_, p, sp));
        return !g_.Faulted();
    }
    bool PlayerCopPlace(uint32_t e, uint32_t, uint32_t& v0) override {
        v0 = rr::game::PlayerCopPlace(g_, e, t_);
        return !g_.Faulted();
    }
    bool SaveCopRecord(uint32_t e, uint32_t) override {
        rr::game::SaveCopRecord(g_, e);
        return !g_.Faulted();
    }
    const GeoModels& GeoState() const { return geo_; }

private:
    void Geo() {
        if (geo_.tried) return;
        geo_.tried = true;
        const uint32_t gs = g_.U32(kGsPtr);
        geo_.file = rr::LevelBankFile(rr::LevelBankIndex(g_.S32(gs + 60u), g_.U8(gs + 4u), g_.S32(gs + 72u)), ".GEO");
        const auto f = disc_.Find(geo_.file);
        if (!f) {
            geo_.error = "not on the disc";
            return;
        }
        try {
            geo_.models = rr::ParseGeo(disc_.ReadFile(*f));
        } catch (const std::exception& ex) {
            geo_.error = ex.what();
        }
    }
    RaceSession& s_;
    GuestRam& g_;
    const DiscImage& disc_;
    const rr::sim::BikeTables& t_;
    rr::sim::RoadRuntimeNative road_;
    GeoModels geo_;
};

// BuildGrid's callees in the product.
class ProductGrid final : public rr::sim::GridCallees {
public:
    ProductGrid(GuestRam& g, uint8_t* ram, const DiscImage& disc, ProductSpawn& spawn, uint32_t statArray)
        : g_(g), ram_(ram), disc_(disc), spawn_(spawn), statArray_(statArray) {}
    int mallocs = 0;
    std::string refused;
    size_t spawns = 0, spawnFailed = 0, passengers = 0;
    bool Malloc(uint32_t bytes, uint32_t, uint32_t, uint32_t& v0) override {  // SLUS 0x8001447C: the arena's
        switch (mallocs++) {
        case 0: v0 = statArray_; return true;
        case 1:
            v0 = kArenaPool0;
            if (kArenaPool0 + bytes > kArenaPool1) return Refuse("pool 0 does not fit below pool 1");
            return true;
        case 2:
            v0 = kArenaPool1;
            if (kArenaPool1 + bytes > kArenaAnimFiles) return Refuse("pool 1 does not fit below the bank files");
            return true;
        default: return Refuse("a fourth malloc");
        }
    }
    bool LoadLevelBi(uint32_t buf, uint32_t, uint32_t) override {             // RASHCDI 0x80064B44
        return Load("DATA/LEVEL" + std::to_string(g_.S32(g_.U32(kGsPtr) + 60u) + 1) + ".BI", buf, kLevelBiBytes);
    }
    bool LoadLevelPh(uint32_t dst, uint32_t) override {                        // RASHCDI 0x800654BC
        return Load("DATA/LEVEL" + std::to_string(g_.S32(g_.U32(kGsPtr) + 60u) + 1) + ".PH", dst, kLevelPhBytes);
    }
    bool SpawnBike(uint32_t entry, uint32_t bi, uint32_t slot, uint32_t mask, uint32_t flags, uint32_t sp,
                   uint32_t& v0) override {
        bool ok = true;
        v0 = rr::sim::SpawnBike(g_, entry, bi, slot, mask, flags, sp, spawn_, ok);
        ++spawns;
        if (!ok) {
            ++spawnFailed;
            return Refuse("SpawnBike refused (a callee or the view)");
        }
        return true;
    }
    bool ComputePlace(uint32_t e, int32_t mode, uint32_t, uint32_t& v0) override {   // SLUS 0x800138E8, PORTED
        const int32_t high = g_.S32(kPool0High);
        std::vector<rr::sim::PlaceNode> nodes(static_cast<size_t>(high < 0 ? 0 : high + 1));
        const uint32_t base = g_.U32(kPoolTable);
        for (size_t i = 0; i < nodes.size(); ++i) {
            const uint32_t b = base + 1096u * static_cast<uint32_t>(i);
            nodes[i].entity = Raw(b, 1096);
            nodes[i].riderDef = Raw(g_.U32(b + 1084u), 72);
            nodes[i].binding = Binding(b);
        }
        rr::sim::ComputePlaceEnv env;
        env.gameState = Raw(g_.U32(kGsPtr), 0x50);
        env.liveBikes = g_.S32(kPool0Count);
        env.raceFlags = g_.U32(0x8005AD48u);
        env.pool = nodes.data();
        env.poolHigh = high;
        env.poolCount = static_cast<int32_t>(nodes.size());
        const uint8_t* ep = Raw(e, 1096);
        const uint8_t* rd = Raw(g_.U32(e + 1084u), 72);
        int32_t place = 0;
        if (ep == nullptr || rd == nullptr || env.gameState == nullptr ||
            !rr::sim::ComputePlace(ep, rd, Binding(e), mode, env, &place))
            return Refuse("ComputePlace declined");
        v0 = static_cast<uint32_t>(place);
        return true;
    }
    bool RiderAdjust(uint32_t e, uint32_t) override {                         // RASHCDI 0x800650A0, PORTED
        rr::sim::GridRiderAdjust(g_, e);
        return !g_.Faulted();
    }
    bool SpawnPassenger(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) override {   // the session's JailSpawn
        ++passengers;
        return true;
    }

private:
    bool Refuse(const std::string& why) {
        if (refused.empty()) refused = why;
        return false;
    }
    bool Load(const std::string& name, uint32_t dst, uint32_t bytes) {
        std::vector<uint8_t> f;
        if (const auto e = disc_.Find(name)) f = disc_.ReadFile(*e);
        if (f.size() < bytes) return Refuse(name + " is missing or short");
        g_.WriteBlock(dst, f.data(), bytes);
        return !g_.Faulted();
    }
    const uint8_t* Raw(uint32_t a, uint32_t n) const {
        if (a < 0x80000000u || static_cast<uint64_t>(a) + n > 0x80200000ull) return nullptr;
        return ram_ + (a - 0x80000000u);
    }
    rr::sim::RouteBinding Binding(uint32_t e) const {
        rr::sim::RouteBinding b;
        const uint32_t ro = g_.U32(e + 0x1ACu);
        b.routeObject = Raw(ro, 120);
        if (b.routeObject != nullptr) {
            std::memcpy(&b.firstWord, b.routeObject, 4);
            b.legs = b.routeObject + 20;
            std::memcpy(&b.legCount, b.routeObject + 0x0C, 4);
        }
        b.routeArmed = g_.S16(kRouteArmed);
        return b;
    }
    GuestRam& g_;
    uint8_t* ram_;
    const DiscImage& disc_;
    ProductSpawn& spawn_;
    uint32_t statArray_;
};

} // namespace

bool RaceSession::OriginalBind() const {
    return gridPorted_ && Loader2On() && std::getenv("RRJB_LOADER_BIND") == nullptr;
}

bool RaceSession::BuildGridPorted(const DiscImage& disc) {
    if (!gridPorted_) return false;
    GuestRam g(arena_.Ram(), kGp);
    g.SetScratchpad(scratchpad_.data());
    const uint32_t gs = g.U32(kGsPtr);
    // ---- RASHCDI over RASHCDG for the call (the console has the loader overlay resident while the race loads)
    std::vector<uint8_t> overlay;
    if (LoaderPorted() && !LoaderOverlayImage().empty()) overlay = LoaderOverlayImage(); // with the loader's writes
    else if (const auto f = disc.Find("RASHCDI.BIN")) overlay = disc.ReadFile(*f);
    if (overlay.empty()) {
        NoteSeam("the ported grid was NOT run: RASHCDI.BIN is not on the disc (the session's own layout stands)");
        gridPorted_ = false;
        return false;
    }
    const uint32_t ovlBytes = static_cast<uint32_t>(overlay.size());
    std::vector<uint8_t> saved(ovlBytes);
    g.ReadBlock(rr::sim::kRashcdiLoad, saved.data(), ovlBytes);
    std::vector<uint8_t> arenaCopy(At(0x80000000u), At(0x80000000u) + rr::sim::GuestRam::kRamSize);
    g.WriteBlock(rr::sim::kRashcdiLoad, overlay.data(), ovlBytes);

    // ---- SetUpRace's 0x8006ACD0 (the [START] record and its cursor), on the resident piece list of the start
    const uint32_t start = g.U32(kRouteHeader + 0x14u);
    const uint32_t road = g.U32(start), along = g.U32(start + 4u), dir = g.U32(start + 8u);
    if (!streamPorted_) { // OURS: the session's streaming rule at [START] (else the PORTED stream start ran)
        g.W32(ArenaEntity(0) + 0x168u, road & 0xFFFFu);                  // the streaming rule's coordinate (OURS)
        g.W32(ArenaEntity(0) + 0x16Cu, dir);
        g.W32(ArenaEntity(0) + 0x170u, along);
        for (int32_t& s : pieceSlots_) s = -1;
        pieceHigh_ = -1;
        RoadStreamPass(g);
    }
    const int32_t seated = rr::sim::StartRecord(g, road, along, dir, kGridSp);

    // ---- the pools' counters as the loader finds them (OURS: their clearing is not read), the escape flag
    g.W32(kPool0Count, 0);
    g.W32(kPool1Count, 0);
    g.W32(kPool0High, 0);
    g.W32(kPool1High, 0);
    for (uint32_t k = 0; k < 4u; ++k) g.W8(rr::sim::kGridPoolBytes + k, 0);
    g.W8(rr::sim::kGridEscapeFlag, 0);

    // ---- BuildRace's grid block: STARTDF<A|B>.BIN block raceId - 1 on its frame (sp+24), size 292 or 0
    std::vector<uint8_t> file;
    const std::string blockFile = std::string("DATA/STARTDF") + (world_.set == 2 ? "B" : "A") + ".BIN";
    if (const auto f = disc.Find(blockFile)) file = disc.ReadFile(*f);
    const size_t at = 292u * static_cast<size_t>(world_.raceId - 1);
    int32_t size = 0;
    if (world_.raceId >= 1 && file.size() >= at + 292u) {
        g.WriteBlock(kBlockAt, file.data() + at, 292u);
        size = 292;
    }

    // ---- the stat array: rr-race's; with two players (five blocks) its block ends where pool 0's starts, as
    // BuildGrid's mallocs leave it (mp_arena.h); RRJB_MPARENA=off: a free cell buffer (the old placement)
    uint32_t statArray = kArenaStatArray;
    if (g.U32(gs + 48u) == 2u && MpArenaOriginal()) statArray = StatArrayAt(2u);
    else if (g.U32(gs + 48u) == 2u && !freeCellBuffers_.empty()) {
        statArray = freeCellBuffers_.back();
        freeCellBuffers_.pop_back();
        --cellBuffersTotal_;
    }
    const rr::sim::BikeTables t = SessionTables();
    ProductSpawn spawn(*this, g, disc, t);
    spawn.heapFrom = &Loader2Totals().heapNext; // BuildRace's reserve (race_session.cpp, before the cell buffers)
    spawn.heapLimit = Loader2Totals().heapEnd;
    ProductGrid grid(g, arena_.Ram(), disc, spawn, statArray);
    // BuildRace's next call, RASHCDI 0x80067784: pool 0 from its high index down, +0x340 = 0 and ResetBikeState
    const auto resetBikes = [&] {
        const int32_t high = g.S32(g.U32(kPoolTable + 0x0Cu));
        uint32_t e = g.U32(kPoolTable);
        for (int32_t i = high; i >= 0; --i, e += g.U32(kPoolTable + 4u)) {
            g.W32(e + 0x340u, 0);
            rr::sim::ResetBikeState(g, e);
        }
    };
    bool built = false, resetRan = false;
    Loader2Totals().buildRaceRan = false;
    if (Loader2On()) {
        // BuildRace RASHCDI 0x8006982C PORTED as the orchestrator (sim\loader2.h); its sp puts its
        // frame at kGridSp, so its sp+24 is the grid block (kBlockAt, above) and BuildGrid runs at kGridSp
        const uint32_t seed = g.U32(kGp + 2076u);
        ProductLoaderCallees* self = nullptr;
        ProductLoaderCallees c(g, arena_.Ram(), &disc, &Loader2Totals().heapNext, Loader2Totals().heapEnd,
                               [&](uint32_t fn, const uint32_t* a, int, uint32_t csp, uint32_t& v0, bool& handled) {
            namespace s = rr::sim;
            handled = true;
            Loader2Counts& L = Loader2Totals();
            switch (fn) {
            case s::kL2WorldPoolsFn:
            case s::kL2PoolTableFn:
                Loader2Answered("WorldPoolsInit 0x80069000 / PoolTableInit 0x80068AA4 at the world arena (PORTED there)");
                return true;
            case s::kLdLoadFile: {
                // OURS, named: a read file is laid where its caller copies it (no transient block): the grid block -
                // the session's STARTDF file and race (a direct start leaves game_state+0x40 = 0, set 2 runs with one
                // player) - at BuildRace's sp+24 with the buffer placed so that its copy of block +0x40 - 1 is that
                // block; STARTJBA.BIN at EscapeLoad's sp+48
                const std::string name = GuestString(g, a[0]);
                if (name.find("STARTDF") != std::string::npos) {
                    if (size == 0) {
                        v0 = 0xFFFFFFFFu;
                        return true;
                    }
                    const uint32_t idx = g.U32(gs + 64u) - 1u;
                    g.W32(a[2], kBlockAt - 292u * idx);
                    g.W32(a[3], static_cast<uint32_t>(file.size()));
                    v0 = static_cast<uint32_t>(file.size());
                    return !g.Faulted();
                }
                std::string path = name;
                for (char& ch : path)
                    if (ch == '\\') ch = '/';
                std::vector<uint8_t> bytes;
                if (const auto f = disc.Find(path)) bytes = disc.ReadFile(*f);
                if (bytes.empty() || bytes.size() > 640u) {
                    v0 = 0xFFFFFFFFu;
                    return true;
                }
                g.WriteBlock(csp + 48u, bytes.data(), static_cast<uint32_t>(bytes.size()));
                g.W32(a[2], csp + 48u);
                g.W32(a[3], static_cast<uint32_t>(bytes.size()));
                v0 = static_cast<uint32_t>(bytes.size());
                return !g.Faulted();
            }
            case s::kL2BuildGrid:
                // OURS, named: a scripted run keeps the session's fixed LCG seed (BuildRace's SRand of root counter 2
                // is the console's timing; a live run takes it, as the original)
                if (!LiveRootCounter()) g.W32(kGp + 2076u, seed);
                built = rr::sim::BuildGrid(g, a[0], a[1], csp, grid) && !g.Faulted();
                return built;
            case s::kL2ResetBikes:
                resetBikes();
                resetRan = true;
                return !g.Faulted();
            case s::kL2CameraSetUp:
                Loader2Answered("CameraSetUp 0x80067564 at the camera arena");
                return true;
            case s::kL2TrialLimitsFn:
                ++L.trialLimits;
                return s::TrialLimits(g, csp, *self);
            case s::kL2Scale:
                s::cu::GScale(g, static_cast<int32_t>(a[0]), a[1], a[2]);
                return !g.Faulted();
            case s::kL2PopBlockFn:
                ++L.popBlock;
                return s::PopBlockInit(g, a[0], a[1], csp, *self);
            case s::kL2PedTablesFn:
                ++L.pedTables;
                s::PedTablesInit(g);
                return !g.Faulted();
            case s::kL2PartSlotsFn:
                ++L.partSlots;
                return s::PartSlotsInit(g, csp, *self);
            case s::kL2CensusFn:
                ++L.census;
                return s::CensusInit(g, csp, *self);
            case s::kLdBlockCopyFn:
                Loader2Answered("RaceBlockCopy 0x80068470 at the hazard set (PORTED there)");
                return true;
            case s::kL2SpeedClassFn:
                ++L.speedClass;
                return s::SpeedClassCopy(g, a[0], a[1], csp, *self);
            case s::kL2EscapeLoadFn:
                ++L.escapeLoad;
                return s::EscapeLoad(g, csp, *self);
            default:
                handled = false;
                return true;
            }
        });
        self = &c;
        ++Loader2Totals().buildRace;
        const bool ok = rr::sim::BuildRace(g, kGridSp + rr::sim::kL2BuildRaceFrame, c) && !g.Faulted();
        if (ok) {
            Loader2Totals().buildRaceRan = true;
            for (uint32_t k = 0; k < 3u; ++k) Loader2Totals().speedClasses[k] = g.U32(rr::sim::kL2SpeedClasses + 4u * k);
            for (uint32_t k = 0; k < 2u; ++k) Loader2Totals().jailRecords[k] = g.U32(rr::sim::kL2JailCounts + 4u * k);
        } else if (built) {
            Loader2Totals().refused = "BuildRace: " + (c.error.empty() ? std::string("a callee or the view") : c.error);
            built = false;
        }
    } else {
        built = rr::sim::BuildGrid(g, kBlockAt, size, kGridSp, grid) && !g.Faulted();
    }
    const bool faulted = g.Faulted();
    g.ClearFault();

    // ---- RASHCDG back over the loader overlay; the frame's LEVEL<n>.BI buffer cleared (the product's stack)
    g.WriteBlock(rr::sim::kRashcdiLoad, saved.data(), ovlBytes);
    for (uint32_t k = 0; k < 2048u; k += 4) g.W32(kGridSp - rr::sim::kBuildGridFrame + 256u + k, 0);
    if (!built) {
        std::memcpy(At(0x80000000u), arenaCopy.data(), arenaCopy.size());
        NoteSeam("the PORTED BuildGrid RASHCDI 0x80067B00 declined (" +
                 (grid.refused.empty() ? std::string(faulted ? "an address the console would fault on" : "?") : grid.refused) +
                 "): the session's own layout stands");
        gridPorted_ = false;
        return false;
    }

    // ---- BuildRace's next call, RASHCDI 0x80067784 (run in BuildRace's site when BuildRace is ported)
    {
        if (!resetRan) resetBikes();
        if (g.Faulted()) {
            g.ClearFault();
            NoteSeam("RASHCDI 0x80067784 (ResetBikeState SLUS 0x8002090C per bike, PORTED) met an address the console would fault on");
        }
    }

    // ---- the session's view of the field, from the pools BuildGrid filled (pool-0 slot i = grid entry i)
    const uint32_t n = g.U32(kPool0Count);
    const size_t planned = bikes_.size();
    const uint32_t players = g.U32(gs + 48u);
    bikes_.assign(n, RaceBike{});
    size_t cops = 0;
    for (uint32_t i = 0; i < n; ++i) {
        RaceBike& b = bikes_[i];
        const uint32_t e = ArenaEntity(i);
        b.entityAddress = e;
        b.ownerAddress = g.U32(e + 0x354u);
        b.riderDefAddress = g.U32(e + 0x43Cu);
        b.entity = ArenaBytes{At(e), 1096};
        b.owner = ArenaBytes{At(b.ownerAddress), 628};
        b.riderDef = ArenaBytes{At(b.riderDefAddress), 72};
        b.isPlayer = g.U16(e + 0xACu) < players;
        b.isCop = !b.isPlayer && (g.U8(b.riderDefAddress + 1u) & 0xFu) == 2u;
        cops += b.isCop ? 1u : 0u;
        if (size != 0 && i < 18u) {
            int32_t w[3];
            std::memcpy(w, file.data() + at + 4u + 12u * i, 12);
            b.gridSlot = w[0];
            b.gridAlong = w[2];
        }
        b.statsAddress = g.U32(e + 0x22Cu);
        b.stats = At(b.statsAddress);
        b.statsBlock = static_cast<int32_t>((b.statsAddress - statArray) / 448u);
        // OURS, display only: the nearest slice of world.cpp's route to the box centre
        double best = 1e300;
        for (size_t s = 0; s < world_.path.size(); ++s) {
            double d = 0.0;
            for (uint32_t k = 0; k < 3; ++k) {
                const double v = static_cast<double>(world_.path[s].pos[k]) - g.S32(e + 0xB8u + 4u * k);
                d += v * v;
            }
            if (d < best) {
                best = d;
                b.pathHint = s;
            }
        }
    }
    for (int32_t& s : pieceSlots_) s = -1;                               // BuildPopulationArena empties the list
    pieceHigh_ = -1;

    char line[1000];
    std::snprintf(line, sizeof(line),
                  "the grid is the ORIGINAL's: BuildGrid RASHCDI 0x80067B00 and SpawnBike 0x80065A94 PORTED (grid_build.h, "
                  "bench row spawn_bike), then 0x80067784 (ResetBikeState per bike): %s block %d (%d byte(s)), %u bike(s) "
                  "(the session planned %zu), %zu police, racers *(0x8005B1FC) = %d, [START] road %u dir %d along %.3f "
                  "(cursor %s), stat array %s (%s), bike .PH %s (%zu read, %zu missing), boxes from %s: %zu (%zu without a "
                  "model), %zu animation-bank request(s) left to BuildAnimArena, %zu sidecar passenger(s) left to JailSpawn; "
                  "RRJB_GRID=ours restores the session's layout",
                  blockFile.c_str(), world_.raceId - 1, size, n, planned, cops, g.S32(rr::sim::kGridRacers),
                  road & 0xFFFFu, static_cast<int32_t>(dir), static_cast<int32_t>(along) / 65536.0,
                  seated ? "seated" : "NOT seated", Hex(statArray).c_str(),
                  statArray == kArenaStatArray  ? "rr-race's"
                  : statArray == StatArrayAt(2u) ? "the original's order: its block ends at pool 0's, mp_arena.h"
                                                 : "OURS: a free cell buffer, RRJB_MPARENA=off", spawn.phNames.c_str(),
                  spawn.phLoads, spawn.phMissing, spawn.GeoState().file.c_str(), spawn.boxesFromGeo, spawn.boxesMissing,
                  spawn.animRequests, grid.passengers);
    NoteSeam(line);
    if (n != planned)
        NoteSeam("the ported grid spawned " + std::to_string(n) + " bike(s) where the session had planned " +
                 std::to_string(planned) + " (the passengers' pool slots were planned before it)");
    return true;
}

// One line per bike: where the grid put it (the road coordinate +0x168..+0x173, the lateral +0x158, the box
// centre +0xB8) - the same line for the session's own layout and for the original's.
void RaceSession::NoteGridLayout(const char* who) {
    GuestRam g(arena_.Ram(), kGp);
    for (size_t i = 0; i < bikes_.size(); ++i) {
        const uint32_t e = bikes_[i].entityAddress;
        char b[420];
        std::snprintf(b, sizeof(b),
                      "grid (%s): bike %2zu slot %2d %s class %u live %d road %u%s dir %d along %9.3f lateral %8.3f "
                      "pos (%.3f, %.3f, %.3f); rd+1 0x%02X +0xB4 %u box 0x%X 0x%X 0x%X mass 0x%X stats +%u",
                      who, i, bikes_[i].gridSlot, bikes_[i].isPlayer ? "player" : (bikes_[i].isCop ? "police" : "racer "),
                      g.U8(bikes_[i].riderDefAddress + 1u) & 0xFu, g.S16(e + 0x140u), g.U32(e + 0x168u) & 0xFFFFu,
                      (g.U32(e + 0x168u) >> 16) ? " (node)" : "", g.S32(e + 0x16Cu), g.S32(e + 0x170u) / 65536.0,
                      g.S32(e + 0x158u) / 65536.0, g.S32(e + 0xB8u) / 65536.0, g.S32(e + 0xBCu) / 65536.0,
                      g.S32(e + 0xC0u) / 65536.0, g.U8(bikes_[i].riderDefAddress + 1u), g.U32(e + 0xB4u),
                      g.U32(e + 0x130u), g.U32(e + 0x134u), g.U32(e + 0x138u), g.U32(e + 0x13Cu),
                      (g.U32(e + 0x22Cu) - g.U32(0x8005B248u)) / 448u);
        NoteSeam(b);
    }
}

} // namespace rr::game
