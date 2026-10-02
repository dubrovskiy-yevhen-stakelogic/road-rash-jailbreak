// Bench rows for the race loader's starting grid (src\game\sim\grid_build.{h,cpp}),
// RASHCDI SHA-1 9a8b79d8739713c12b0a2dc4d75ef8f0a2cc0b06. Included LAST by verify_physics.cpp inside its
// anonymous namespace (rows_engine_note.inc's sf::InjectLeaf, rows_bike_step.inc's bs::ExactOracle,
// rows_population.inc's pop:: and rows_riders.inc's riders::LevelBi are visible).
//
//   spawn_bike    RASHCDI 0x80065A94  SpawnBike(entry, bi, slot, mask, flags)
//   start_record  RASHCDI 0x8006ACD0  StartRecord(road, along, dir)   (RoadGate / CursorSeat PORTED, native)
//   grid_lists    RASHCDI 0x80065468  GridLists()
//
// RASHCDI is not resident in the rr-race machine (RASHCDG is): every case lays the WHOLE overlay image
// (work\disc_us\RASHCDI.BIN) over 0x8005B5E8 on both machines, as the console has it while the race loads -
// SpawnBike's own leaves (AllocBike, AllocRider, PlayerColumn, 0x8006AD24, 0x8005DD9C, BikeName, the PORTED
// RiderRecordInit / RiderNameId) and data (the bike names, the class table 0x8006B8A0, ".ph") live there, and
// so do its oracle callees PlayerCopPlace 0x8006581C and SaveCopRecord 0x800644C8.
//
// spawn_bike's seams, executed by the ORACLE and witnessed: the BIOS string calls strcpy / strcat / strlen /
// strrchr / strncmp (SLUS stubs 0x800448E4 / 0x80044864 / 0x800448F4 / 0x80044964 / 0x80044874, allowBiosCalls),
// LoadBikePh 0x80065558, AnimBank 0x8005BCDC, AnimNoise 0x80063158, ModelBind SLUS 0x8002FAD4, the box SLUS
// 0x80012FC8, Attach SLUS 0x80012838, RoadWalk SLUS 0x80012C1C, RoadGate SLUS 0x80039DFC, CursorSeat SLUS
// 0x8003A700, RoadPosition SLUS 0x8003662C, RoadClass SLUS 0x8003DE28, RoadsideRun SLUS 0x8003DF54, RouteBind
// SLUS 0x8003AF9C, Progress SLUS 0x8003B61C, PlayerCopPlace 0x8006581C, SaveCopRecord 0x800644C8.
//
// INPUT FAMILIES (spawn_bike), over rr-race (race 4 of set 1, [START] = road 9 at 1057.0, dir -1):
//   dump-derived: an entry of the capture's own grid (STARTDFA.BIN block 3: slot, lateral, along), flags 0,
//     the capture's own game_state (race type 34, one player), the next pool slot;
//   randomised: the race type (0, 1, 4, 8, 0x10, 0x11, 0x20, 0x21, 33, 34, 36, 44, any byte), the players
//     1..2, the bike words gs+0x48 / +0x4C 0..26, gs+0x3A / +0x44 (the player column), the
//     seat bytes gs+0x0A.., the player records' +10 / +20, the session byte; flags 0 (a racer), 8 (player 2),
//     4 | index << 4 (an explicit pool slot); the grid slot 0..16 (a racer) and 17..18 (police); the class
//     mask; the pools' counts and capacity bytes (a full pool: the 0x440 store and v0 = 0); the entry's
//     lateral / along (+-24.0 / +-400.0, and |along| < 131/65536 for the clamp; the slot word 0 as BuildGrid
//     leaves it, now and then 1); the start record's direction +-1 and its cursor; LEVEL<n>.BI edited.
// NAMED BOUNDS (each keeps the case away from hardware the bench cannot answer or from a loop the original
// never leaves, never from a branch of SpawnBike's own):
//   * no flag 1 (player 1): its animation banks, ANIMNOIZ.DAT and the weapons' model file come off the CD,
//     which the bench does not model (AnimBank 0x8005BCDC's read would trap). Player 2 (flag 8) runs the
//     same player arm except that block; the block itself is straight-line calls and stores.
//   * *(0x8005AD40) has bits 0..15 set: every stat block is "already loaded", so a player's .PH read
//     (0x80065558, the CD) is never asked for.
//   * the mask leaves column 8 of each class free (the search starts at (slot + 1) / 2 <= 8 and only moves
//     up): with every column from there to 8 taken in both classes the original's search never ends (a grid
//     file holds at most 18 entries, one column each).
//   * the police slot is 17 or 18 (the files' only police slots): 19 / 20 index the AI maps past their end.
//   * the bank gs+0x3C is the capture's own: the model registry holds that bank's models only, and a class of
//     another bank finds no registry slot - ModelBind leaves the entity's +0x60 at 0 and the box 0x80012FC8
//     then loads through *(0 + 8) and traps, on the console as here.
//   * a player record's +10 is 0..4 and the bike words 0..26 (the front end's ranges): larger values make
//     RiderRecordInit read far outside LEVEL<n>.BI.

namespace gridr {

using pop::Chance;
using pop::Put8;

constexpr uint32_t kStrCpy = 0x800448E4u, kStrCat = 0x80044864u, kStrLen = 0x800448F4u, kStrRChr = 0x80044964u,
                   kStrNCmp = 0x80044874u, kLoadPh = 0x80065558u, kAnimBank = 0x8005BCDCu, kAnimNoise = 0x80063158u,
                   kModelBind = 0x8002FAD4u, kBox = 0x80012FC8u, kAttach = 0x80012838u, kRoadWalk = 0x80012C1Cu,
                   kRoadGate = 0x80039DFCu, kCursorSeat = 0x8003A700u, kRoadPos = 0x8003662Cu, kRoadClass = 0x8003DE28u,
                   kRoadside = 0x8003DF54u, kRouteBind = 0x8003AF9Cu, kProgress = 0x8003B61Cu, kCopPlace = 0x8006581Cu,
                   kSaveCop = 0x800644C8u;
constexpr uint32_t kGs = 0x8005B2F8u;

std::vector<OracleCallee> SpawnSeamCallees() {
    return {OracleCallee{kStrCpy, 2},     OracleCallee{kStrCat, 2},    OracleCallee{kStrLen, 1},
            OracleCallee{kStrRChr, 2},    OracleCallee{kStrNCmp, 3},   OracleCallee{kLoadPh, 2},
            OracleCallee{kAnimBank, 2},   OracleCallee{kAnimNoise, 0}, OracleCallee{kModelBind, 4},
            OracleCallee{kBox, 2},        OracleCallee{kAttach, 4},    OracleCallee{kRoadWalk, 3},
            OracleCallee{kRoadGate, 2},   OracleCallee{kCursorSeat, 3}, OracleCallee{kRoadPos, 3},
            OracleCallee{kRoadClass, 4},  OracleCallee{kRoadside, 3},  OracleCallee{kRouteBind, 3},
            OracleCallee{kProgress, 1},   OracleCallee{kCopPlace, 1},  OracleCallee{kSaveCop, 1}};
}

struct Seams final : rr::sim::SpawnCallees {
    NativeEnv& env;
    explicit Seams(NativeEnv& e) : env(e) {}
    bool Call(uint32_t f, std::initializer_list<uint32_t> args, uint32_t sp, uint32_t* v0 = nullptr) {
        uint32_t a[4] = {0, 0, 0, 0};
        int n = 0;
        for (uint32_t x : args) a[n++] = x;
        const uint32_t r = bs::ExactOracle(env, f, a, n, sp);
        if (v0 != nullptr) *v0 = r;
        return env.failure.empty();
    }
    bool StrCpy(uint32_t d, uint32_t s, uint32_t sp) override { return Call(kStrCpy, {d, s}, sp); }
    bool StrCat(uint32_t d, uint32_t s, uint32_t sp) override { return Call(kStrCat, {d, s}, sp); }
    bool StrLen(uint32_t s, uint32_t sp, uint32_t& v0) override { return Call(kStrLen, {s}, sp, &v0); }
    bool StrRChr(uint32_t s, uint32_t ch, uint32_t sp, uint32_t& v0) override { return Call(kStrRChr, {s, ch}, sp, &v0); }
    bool StrNCmp(uint32_t a, uint32_t b, uint32_t n, uint32_t sp, uint32_t& v0) override {
        return Call(kStrNCmp, {a, b, n}, sp, &v0);
    }
    bool LoadBikePh(uint32_t name, uint32_t dst, uint32_t sp) override { return Call(kLoadPh, {name, dst}, sp); }
    bool AnimBank(uint32_t desc, uint32_t name, uint32_t sp, uint32_t& v0) override {
        return Call(kAnimBank, {desc, name}, sp, &v0);
    }
    bool AnimNoise(uint32_t sp) override { return Call(kAnimNoise, {}, sp); }
    bool ModelBind(uint32_t obj, uint32_t pool, uint32_t cls, uint32_t alloc, uint32_t sp, uint32_t& v0) override {
        return Call(kModelBind, {obj, pool, cls, alloc}, sp, &v0);
    }
    bool BoxSetup(uint32_t e, uint32_t idx, uint32_t sp) override { return Call(kBox, {e, idx}, sp); }
    bool Attach(uint32_t b, uint32_t r, uint32_t kind, uint32_t seat, uint32_t sp) override {
        return Call(kAttach, {b, r, kind, seat}, sp);
    }
    bool RoadWalk(uint32_t from, uint32_t out, int32_t dist, uint32_t sp) override {
        return Call(kRoadWalk, {from, out, static_cast<uint32_t>(dist)}, sp);
    }
    bool RoadGate(uint32_t h, uint32_t key, uint32_t sp, uint32_t& v0) override { return Call(kRoadGate, {h, key}, sp, &v0); }
    bool CursorSeat(uint32_t obj, uint32_t key, uint32_t cur, uint32_t sp, uint32_t& v0) override {
        return Call(kCursorSeat, {obj, key, cur}, sp, &v0);
    }
    bool RoadPosition(uint32_t heading, uint32_t cur, uint32_t out, uint32_t sp) override {
        return Call(kRoadPos, {heading, cur, out}, sp);
    }
    bool RoadClass(uint32_t e, int32_t mode, uint32_t out, int32_t zone, uint32_t sp) override {
        return Call(kRoadClass, {e, static_cast<uint32_t>(mode), out, static_cast<uint32_t>(zone)}, sp);
    }
    bool RoadsideRun(uint32_t e, int32_t mode, int32_t zone, uint32_t sp) override {
        return Call(kRoadside, {e, static_cast<uint32_t>(mode), static_cast<uint32_t>(zone)}, sp);
    }
    bool RouteBind(uint32_t p, int32_t step, uint32_t x, uint32_t sp) override {
        return Call(kRouteBind, {p, static_cast<uint32_t>(step), x}, sp);
    }
    bool Progress(uint32_t p, uint32_t sp, uint32_t& v0) override { return Call(kProgress, {p}, sp, &v0); }
    bool PlayerCopPlace(uint32_t e, uint32_t sp, uint32_t& v0) override { return Call(kCopPlace, {e}, sp, &v0); }
    bool SaveCopRecord(uint32_t e, uint32_t sp) override { return Call(kSaveCop, {e}, sp); }
};

void InjectOverlay(CaseContext& c) {
    const std::vector<uint8_t>& img = sf::Rashcdi();
    if (!img.empty()) c.mem->WriteBlock(sf::kOverlayBase, img.data(), static_cast<uint32_t>(img.size()));
}

// The capture's own grid (race 4 of set 1): STARTDFA.BIN block 3, {slot, lateral, along} per entry.
const std::vector<int32_t>& Race4Grid() {
    static std::vector<int32_t> v;
    static bool loaded = false;
    if (!loaded) {
        loaded = true;
        std::vector<uint8_t> f;
        std::string error;
        if (rr::interp::ReadWholeFile("work\\disc_us\\DATA\\STARTDFA.BIN", f, error) && f.size() >= 4u * 292u) {
            const uint8_t* b = f.data() + 3u * 292u;
            int32_t n = 0;
            std::memcpy(&n, b, 4);
            for (int32_t i = 0; i < n && i < 18; ++i)
                for (int k = 0; k < 3; ++k) {
                    int32_t w = 0;
                    std::memcpy(&w, b + 4 + 12 * i + 4 * k, 4);
                    v.push_back(w);
                }
        } else {
            std::fprintf(stderr, "rows_grid: work\\disc_us\\DATA\\STARTDFA.BIN not readable; derived cases use slot 1\n");
        }
    }
    return v;
}

// ---------------------------------------------------------------------------- SpawnBike 0x80065A94
void SetupSpawn(CaseContext& c, Args& a) {
    InjectOverlay(c);
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    const uint32_t gs = m.PeekWord(kGs);
    const uint32_t entry = c.scratch, mask = c.scratch + 16u, bi = c.scratch + 64u, stats = c.scratch + 2048u;
    // the stat array: rr-race's four blocks, a fifth for player 2 (the product's two-player layout)
    {
        const uint32_t old = m.PeekWord(0x8005B248u);
        std::vector<uint8_t> blk(4u * 448u);
        m.ReadBlock(old, blk.data(), static_cast<uint32_t>(blk.size()));
        m.WriteBlock(stats, blk.data(), static_cast<uint32_t>(blk.size()));
        m.WriteBlock(stats + 4u * 448u, blk.data() + 3u * 448u, 448u);
        m.PokeWord(0x8005B248u, stats);
    }
    // NAMED BOUND: every stat block loaded (no .PH read off the CD)
    m.PokeWord(rr::sim::kGridStatLoaded, 0xFFFFu | (c.sample ? 0x04000000u : (r.U32() & 0xFFFF0000u)));
    const uint32_t bank = m.PeekWord(gs + 60u) % 3u;                    // NAMED BOUND: the capture's own bank
    const std::vector<uint8_t>& f = riders::LevelBi(bank);
    if (f.size() >= 1728u) m.WriteBlock(bi, f.data(), 1728u);
    uint32_t slot = 1, flags = 0;
    int32_t lat = 0, along = 0;
    const std::vector<int32_t>& grid = Race4Grid();
    if (c.sample) {
        const uint32_t n = static_cast<uint32_t>(grid.size() / 3u);
        const uint32_t k = n ? 1u + r.U32() % (n - 1u) : 0u;              // entry 0 is player 1's (flag 1)
        if (n) {
            slot = static_cast<uint32_t>(grid[3u * k]);
            lat = grid[3u * k + 1u];
            along = grid[3u * k + 2u];
        }
        m.PokeWord(mask, 0);
        for (uint32_t b = 0; b < k && b < 16u; ++b)                        // the columns earlier entries took
            m.PokeWord(mask, m.PeekWord(mask) | (1u << ((1u + b / 2u) + (b & 1u) * 10u)));
        const uint32_t used = k;
        Put8(m, rr::sim::kGridPoolBytes + 0u, 18);
        Put8(m, rr::sim::kGridPoolBytes + 1u, used);
        Put8(m, rr::sim::kGridPoolBytes + 2u, 18);
        Put8(m, rr::sim::kGridPoolBytes + 3u, used);
        m.PokeWord(0x8005B1F8u, used);
        m.PokeWord(0x8005B218u, used);
    } else {
        static const uint32_t kTypes[] = {0u, 1u, 4u, 8u, 0x10u, 0x11u, 0x20u, 0x21u, 33u, 34u, 36u, 44u, 5u, 0x18u};
        Put8(m, gs + 4u, Chance(r, 10) ? r.U32() & 0xFFu : kTypes[r.U32() % (sizeof(kTypes) / sizeof(kTypes[0]))]);
        m.PokeWord(gs + 48u, 1u + r.U32() % 2u);
        m.PokeWord(gs + 72u, r.U32() % 27u);
        m.PokeWord(gs + 76u, r.U32() % 27u);
        WriteU16Bench(m, gs + 58u, static_cast<uint16_t>(r.U32() % 4u));
        m.PokeWord(gs + 68u, Chance(r, 4) ? r.U32() : r.U32() % 40u);
        for (uint32_t k = 0; k < 20u; ++k) Put8(m, gs + 10u + k, Chance(r, 2) ? 1u : r.U32() % 3u);
        for (uint32_t p = 0; p < 2u; ++p) {
            Put8(m, rr::sim::kRrPlayerRecords + 36u * p + 10u, r.U32() % 5u);
            Put8(m, rr::sim::kRrPlayerRecords + 36u * p + 20u, r.U32() & 0xFFu);
        }
        Put8(m, 0x800D80F5u, r.U32() & 0xFFu);
        m.PokeWord(0x8005B254u, Chance(r, 2) ? 0u : 1u);
        // the pools: capacity 18, used / counts drawn (a full pool now and then)
        const uint32_t used = Chance(r, 12) ? 18u : r.U32() % 18u;
        Put8(m, rr::sim::kGridPoolBytes + 0u, 18);
        Put8(m, rr::sim::kGridPoolBytes + 1u, used);
        Put8(m, rr::sim::kGridPoolBytes + 2u, 18);
        Put8(m, rr::sim::kGridPoolBytes + 3u, Chance(r, 12) ? 18u : used);
        m.PokeWord(0x8005B1F8u, Chance(r, 4) ? r.U32() % 18u : used % 18u);
        m.PokeWord(0x8005B218u, m.PeekWord(0x8005B1F8u));
        // the flags: a racer, player 2, an explicit index (NAMED BOUND: never player 1)
        const uint32_t pick = r.U32() % 6u;
        if (pick == 0u) flags = 8u;
        else if (pick == 1u) flags = 4u | ((r.U32() % 18u) << 4);
        else if (pick == 2u) flags = 8u | 4u | ((r.U32() % 18u) << 4);
        slot = Chance(r, 5) ? 17u + r.U32() % 2u : r.U32() % 17u;
        // the mask (NAMED BOUND: a free column below 9 in each class)
        uint32_t mk = Chance(r, 3) ? 0u : r.U32() & 0x0007FDFFu;
        mk &= ~((1u << 8) | (1u << 18));                                  // column 8 of each class
        m.PokeWord(mask, mk);
        // the offsets
        lat = static_cast<int32_t>(r.U32() % 0x300000u) - 0x180000;
        along = static_cast<int32_t>(r.U32() % 0x3200000u) - 0x1900000;
        if (Chance(r, 8)) along = static_cast<int32_t>(r.U32() % 262u) - 131;
        if (Chance(r, 16)) along = static_cast<int32_t>(0x80000000u);
        // the classes of the records RiderRecordInit may keep, and player 1's
        if (Chance(r, 3)) {
            const uint32_t p1 = m.PeekWord(0x8005B38Cu);
            const uint32_t rd = m.PeekWord(p1 + 1084u);
            if (rd >= 0x80000000u && rd < 0x80200000u) Put8(m, rd + 1u, (m.PeekByte(rd + 1u) & 0xF0u) | r.U32() % 3u);
            m.PokeWord(p1 + 180u, r.U32() % 27u);
        }
        // LEVEL<n>.BI now and then edited (class nibbles, the appearance bytes 1206..1215)
        if (Chance(r, 3))
            for (uint32_t k = 0, n = 1u + r.U32() % 12u; k < n; ++k) {
                const uint32_t o = Chance(r, 2) ? 1206u + r.U32() % 10u : 64u * (r.U32() % 27u) + 5u;
                Put8(m, bi + o, r.U32() & 0xFFu);
            }
        // the start record's direction and the cursor copy
        if (Chance(r, 3)) m.PokeWord(rr::sim::kGridStartRecord + 4u, Chance(r, 2) ? 1u : 0xFFFFFFFFu);
        if (Chance(r, 3)) Put8(m, 0x8005AD48u, r.U32() & 0xFFu);
    }
    m.PokeWord(entry + 0u, static_cast<uint32_t>(lat));
    m.PokeWord(entry + 4u, static_cast<uint32_t>(along));
    m.PokeWord(entry + 8u, (!c.sample && Chance(r, 12)) ? 1u : 0u);
    a.a[0] = entry;
    a.a[1] = bi;
    a.a[2] = slot;
    a.a[3] = mask;
    a.a4 = flags;
}
uint32_t NativeSpawn(NativeEnv& env, const Args& a) {
    rr::sim::GuestRam g = pop::View(env);
    Seams s(env);
    bool ok = true;
    const uint32_t v = rr::sim::SpawnBike(g, a.a[0], a.a[1], a.a[2], a.a[3], a.a4, env.sp + rr::sim::kSpawnBikeFrame, s, ok);
    return pop::PopDone(env, g, ok, "SpawnBike", v);
}

// ---------------------------------------------------------------------------- StartRecord 0x8006ACD0
void SetupStart(CaseContext& c, Args& a) {
    InjectOverlay(c);
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    const uint32_t p1 = m.PeekWord(0x8005B38Cu);
    uint32_t road = 9, along = 1057u << 16, dir = 0xFFFFFFFFu;
    if (c.sample) {
        road = m.PeekWord(p1 + 360u) & 0xFFFFu;
        along = m.PeekWord(p1 + 368u);
        dir = m.PeekWord(p1 + 364u);
    } else {
        road = Chance(r, 2) ? (Chance(r, 2) ? 9u : m.PeekWord(p1 + 360u)) : (Chance(r, 4) ? r.U32() : r.U32() % 64u);
        along = Chance(r, 2) ? m.PeekWord(p1 + 368u) + (r.U32() % 0x1000000u) - 0x800000u : r.U32() % 0x10000000u;
        dir = Chance(r, 2) ? 1u : 0xFFFFFFFFu;
    }
    a.a[0] = road;
    a.a[1] = along;
    a.a[2] = dir;
}
uint32_t NativeStart(NativeEnv& env, const Args& a) {
    rr::sim::GuestRam g = pop::View(env);
    const int32_t v = rr::sim::StartRecord(g, a.a[0], a.a[1], a.a[2], env.sp);
    return pop::PopDone(env, g, true, "StartRecord", static_cast<uint32_t>(v));
}

// ---------------------------------------------------------------------------- GridLists 0x80065468
void SetupLists(CaseContext& c, Args&) {
    InjectOverlay(c);
    if (c.sample) return;
    Rng& r = *c.rng;
    for (uint32_t h : {0x8005B298u, 0x8005B350u, 0x8005B2D8u, 0x8005B378u, 0x8005B270u}) {
        c.mem->PokeWord(h, r.U32());
        c.mem->PokeWord(h + 4u, r.U32());
    }
}
uint32_t NativeLists(NativeEnv& env, const Args&) {
    rr::sim::GuestRam g = pop::View(env);
    rr::sim::GridLists(g);
    return pop::PopDone(env, g, true, "GridLists");
}

} // namespace gridr

void AddGridRows(std::vector<Row>& rows) {
    {
        Row r{"spawn_bike", "Bike *SpawnBike(Entry *e, u8 *bi, u32 slot, u32 *mask, u32 flags)  [RASHCDI 0x80065A94]",
              rr::sim::kSpawnBikeFn, gridr::SetupSpawn, nullptr, /*compareV0=*/true, gridr::NativeSpawn};
        r.oracleCallees = gridr::SpawnSeamCallees();
        r.oracleFrame = rr::sim::kSpawnBikeFrame;
        r.maxSteps = 60'000'000;
        r.allowBiosCalls = true;
        rows.push_back(std::move(r));
    }
    {
        Row r{"start_record", "void StartRecord(u32 road, s32 along, s32 dir)  [RASHCDI 0x8006ACD0]",
              rr::sim::kStartRecordFn, gridr::SetupStart, nullptr, /*compareV0=*/true, gridr::NativeStart};
        r.maxSteps = 20'000'000;
        rows.push_back(std::move(r));
    }
    {
        Row r{"grid_lists", "void GridLists(void)  [RASHCDI 0x80065468]", rr::sim::kGridListsFn, gridr::SetupLists,
              nullptr, /*compareV0=*/false, gridr::NativeLists};
        rows.push_back(std::move(r));
    }
}
