// Bench rows for the race loader's rider-record functions (src\game\sim\rider_record.{h,cpp}), RASHCDI
// SHA-1 9a8b79d8739713c12b0a2dc4d75ef8f0a2cc0b06. Included LAST by verify_physics.cpp inside its
// anonymous namespace (rows_engine_note.inc's sf::InjectLeaf and rows_population.inc's pop:: are visible).
//
//   rider_record_init  RASHCDI 0x80064C0C  RiderRecordInit(bi, rec, e)
//   rider_name_id      RASHCDI 0x80066E1C  RiderNameId(e)
//   grid_rider_adjust  RASHCDI 0x800650A0  GridRiderAdjust(e)
//
// RASHCDI is not resident in the rr-race machine (RASHCDG is), so every case first copies the
// function's OWN bytes out of the player's work\disc_us\RASHCDI.BIN back to their own address on both
// machines (rows_engine_note.inc's records_init does the same). Sound for these three: their branches are
// relative, their only calls are to resident SLUS leaves (memset 0x8001E100, memcpy 0x8001E0B4, the byte
// copy 0x8001E08C, Rand 0x8001FC58 - and 0x80064C0C's printf 0x80044894, which no path reaches), and
// every data reference is an absolute address outside the overlay except RiderNameId's 0x8006B8A0 table,
// which the setups plant.
//
// INPUT FAMILIES
//   dump-derived: a bike of this machine given the captured sample's handle and rider-record pointer
//     (+0xAC, +0x43C - which AI index's record), the record its AI index names, LEVEL1.BI as the loader
//     reads it (the 27 records, 1728 bytes, 0x80064B44), the snapshot's own game_state;
//   randomised: every branch input drawn - the race type (the ones the functions test: 0, 1, 4, 8, 0x10,
//     0x18, 0x20, 33, 34, 36, 44, any byte), game_state +5, the player count 0..2, the bank -1..3, the
//     session byte 0x800D80DD, the handle as a player's, the record index -2..40 and a record whose first
//     byte does not name it, a null buffer, the class nibbles of the bike and of every record, the grudge
//     bytes, the grid class table 0x8006B8A0, the AI index maps, the player records' +9 / +10, player 1's
//     model +180, the class blocks' adjustment bytes, the Rand seed.
// NAMED BOUNDS: the rider-record pointer is one of the 20 records 0x800D5758 + 72k (the only place
// SpawnBike points it, 0x80065DE8), the rider pointer +0x354 the machine's own rider of that bike, the
// buffer 8-byte aligned (BuildGrid's stack frame, sp+256).

namespace riders {

constexpr uint32_t kInitFn = 0x80064C0Cu, kInitEnd = 0x800650A0u;
constexpr uint32_t kAdjustFn = 0x800650A0u, kAdjustEnd = 0x800653E8u;
constexpr uint32_t kNameFn = 0x80066E1Cu, kNameEnd = 0x800670FCu;
constexpr uint32_t kGsPtr = rr::sim::kRrGameStatePtr;

using pop::Chance;
using pop::Put8;

const std::vector<uint8_t>& LevelBi(uint32_t bank) {
    static std::vector<uint8_t> files[3];
    static bool loaded[3] = {false, false, false};
    bank %= 3u;
    if (!loaded[bank]) {
        loaded[bank] = true;
        std::string error;
        const std::string path = "work\\disc_us\\DATA\\LEVEL" + std::to_string(bank + 1u) + ".BI";
        if (!rr::interp::ReadWholeFile(path, files[bank], error)) {
            std::fprintf(stderr, "rows_riders: %s not readable (%s); the rows will fail\n", path.c_str(), error.c_str());
            files[bank].clear();
        }
    }
    return files[bank];
}

uint32_t Gs(const Memory& m) { return m.PeekWord(kGsPtr); }
uint32_t Record(uint32_t k) { return rr::sim::kRiderRecords + rr::sim::kRiderRecordBytes * k; }

uint32_t DrawType(Rng& r) {
    static const uint32_t kTypes[] = {0u, 1u, 4u, 5u, 8u, 9u, 0x10u, 0x11u, 0x18u, 0x20u, 0x21u, 33u, 34u, 36u, 44u, 17u};
    if (Chance(r, 8)) return r.U32() & 0xFFu;
    return kTypes[r.U32() % (sizeof(kTypes) / sizeof(kTypes[0]))];
}

// The game_state fields the three functions branch on, drawn; the session byte and the seed too.
void DrawGameState(CaseContext& c) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    const uint32_t gs = Gs(m);
    Put8(m, gs + 4u, DrawType(r));
    Put8(m, gs + 5u, r.U32() & 0xFFu);
    m.PokeWord(gs + 48u, r.U32() % 3u);
    static const int32_t kBanks[] = {0, 0, 1, 2, -1, 3};
    m.PokeWord(gs + 60u, static_cast<uint32_t>(kBanks[r.U32() % 6u]));
    Put8(m, rr::sim::kRrSessionFlags, r.U32() & 0xFFu);
    m.PokeWord(c.gp + 2076u, r.U32());
}

// The bike a case works on, and its rider-record pointer (NAMED BOUND).
uint32_t PlantBike(CaseContext& c) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    uint32_t slot, rd;
    if (c.sample != nullptr && c.sample->entity.size() == 1096) {
        const uint8_t* s = c.sample->entity.data();
        slot = (static_cast<uint32_t>(s[0xAC]) | (static_cast<uint32_t>(s[0xAD]) << 8)) % 18u;
        std::memcpy(&rd, s + 0x43C, 4);
        if (rd < rr::sim::kRiderRecords || rd >= Record(20) || (rd - rr::sim::kRiderRecords) % 72u != 0u)
            rd = Record(slot);
    } else {
        slot = r.U32() % 18u;
        rd = Record(r.U32() % 20u);
    }
    const uint32_t e = pop::Bike(m, slot);
    m.PokeWord(e + 1084u, rd);
    if (!c.sample && Chance(r, 3)) WriteU16Bench(m, e + 172u, static_cast<uint16_t>(r.U32() % 3u));
    return e;
}

// LEVEL<bank>.BI at scratch+0 (8-byte aligned, 1728 bytes), mutated now and then in a randomised case.
uint32_t PlantBi(CaseContext& c, uint32_t bank) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    const std::vector<uint8_t>& f = LevelBi(bank);
    if (f.size() < 1728u) return c.scratch;
    m.WriteBlock(c.scratch, f.data(), 1728u);
    if (!c.sample) {
        const uint32_t edits = Chance(r, 2) ? 0u : 1u + r.U32() % 40u;
        for (uint32_t k = 0; k < edits; ++k) Put8(m, c.scratch + r.U32() % 1728u, r.U32() & 0xFFu);
    }
    return c.scratch;
}

// Random class nibbles and grudge bytes in the 20 records, and the maps.
void DrawRecords(CaseContext& c) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    for (uint32_t k = 0; k < 20u; ++k) {
        if (Chance(r, 2)) Put8(m, Record(k) + 1u, (r.U32() & 0xF0u) | (Chance(r, 5) ? r.U32() % 16u : r.U32() % 3u));
        for (uint32_t j = 0; j < 20u; ++j)
            if (Chance(r, 3)) Put8(m, Record(k) + 16u + j, Chance(r, 2) ? r.U32() & 0xFFu : static_cast<uint32_t>(static_cast<int32_t>(r.U32() % 31u) - 15));
        if (Chance(r, 3)) Put8(m, Record(k) + 36u, r.U32() & 0xFFu);
    }
    for (uint32_t k = 0; k < 20u; ++k)
        if (Chance(r, 3)) Put8(m, rr::sim::kRrAiToHandle + k, Chance(r, 2) ? 31u : r.U32() % 18u);
    for (uint32_t h = 0; h < 18u; ++h)
        if (Chance(r, 3)) Put8(m, rr::sim::kRrHandleToAi + h, Chance(r, 8) ? r.U32() & 0xFFu : r.U32() % 20u);
}

// ---------------------------------------------------------------- RiderRecordInit 0x80064C0C
void SetupInit(CaseContext& c, Args& a) {
    sf::InjectLeaf(c, kInitFn, kInitEnd);
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    const uint32_t e = PlantBike(c);
    uint32_t bank = 0;
    int32_t rec;
    if (c.sample) {
        rec = static_cast<int32_t>((m.PeekWord(e + 1084u) - rr::sim::kRiderRecords) / 72u);
    } else {
        DrawGameState(c);
        bank = r.U32() % 3u;
        rec = Chance(r, 6) ? static_cast<int32_t>(r.U32() % 43u) - 2 : static_cast<int32_t>(r.U32() % 27u);
        const uint32_t R = m.PeekWord(e + 852u);
        if (Chance(r, 2)) m.PokeWord(e + 36u, r.U32());
        if (Chance(r, 2)) m.PokeWord(R + 36u, r.U32());
    }
    const uint32_t bi = PlantBi(c, bank);
    if (!c.sample && rec >= 0 && rec < 27 && Chance(r, 5)) Put8(m, bi + 64u * static_cast<uint32_t>(rec), r.U32() & 0xFFu);
    a.a[0] = (!c.sample && Chance(r, 10)) ? 0u : bi;
    a.a[1] = static_cast<uint32_t>(rec);
    a.a[2] = e;
}
uint32_t NativeInit(NativeEnv& env, const Args& a) {
    rr::sim::GuestRam g(env.clone->mem.ram().data(), env.gp);
    rr::sim::RiderRecordInit(g, a.a[0], static_cast<int32_t>(a.a[1]), a.a[2]);
    return RqDone(env, g, 0);
}

// ---------------------------------------------------------------- RiderNameId 0x80066E1C
void SetupName(CaseContext& c, Args& a) {
    sf::InjectLeaf(c, kNameFn, kNameEnd);
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    const uint32_t e = PlantBike(c);
    const uint32_t rd = m.PeekWord(e + 1084u);
    // the grid class table (RASHCDI's own data, planted: RASHCDG sits there in this machine)
    for (uint32_t h = 0; h < 24u; ++h)
        Put8(m, rr::sim::kRrSpawnClassTable + h, (!c.sample && Chance(r, 6)) ? r.U32() & 0xFFu : r.U32() % 2u);
    if (!c.sample) {
        DrawGameState(c);
        Put8(m, rd + 38u, Chance(r, 4) ? r.U32() & 0xFFu : r.U32() % 20u);
        Put8(m, rd + 1u, (r.U32() & 0xF0u) | (Chance(r, 6) ? r.U32() % 16u : r.U32() % 3u));
        for (uint32_t p = 0; p < 2u; ++p) {
            Put8(m, rr::sim::kRrPlayerRecords + 36u * p + 9u, Chance(r, 2) ? 0u : r.U32() & 0xFFu);
            Put8(m, rr::sim::kRrPlayerRecords + 36u * p + 10u, Chance(r, 3) ? r.U32() & 0xFFu : r.U32() % 12u);
        }
        const uint32_t p1 = m.PeekWord(rr::sim::kRrP1Bike);
        m.PokeWord(p1 + 180u, Chance(r, 4) ? r.U32() : r.U32() % 20u);
        DrawRecords(c);
        // the class nibbles the name reads: player 1's and pool slots 0 / 1
        for (uint32_t s = 0; s < 2u; ++s)
            if (Chance(r, 2)) Put8(m, m.PeekWord(pop::Bike(m, s) + 1084u) + 1u, r.U32() % 3u);
    }
    a.a[0] = e;
}
uint32_t NativeName(NativeEnv& env, const Args& a) {
    rr::sim::GuestRam g(env.clone->mem.ram().data(), env.gp);
    const uint32_t v = rr::sim::RiderNameId(g, a.a[0]);
    return RqDone(env, g, v);
}

// ---------------------------------------------------------------- GridRiderAdjust 0x800650A0
void SetupAdjust(CaseContext& c, Args& a) {
    sf::InjectLeaf(c, kAdjustFn, kAdjustEnd);
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    const uint32_t e = PlantBike(c);
    if (!c.sample) {
        DrawGameState(c);
        DrawRecords(c);
        for (uint32_t cls = 0; cls < 3u; ++cls)
            for (uint32_t off : {6u, 7u, 19u, 20u, 21u, 22u, 23u})
                if (Chance(r, 3)) Put8(m, rr::sim::kRrClassBlocks + 36u * cls + off, Chance(r, 2) ? r.U32() & 0xFFu : r.U32() % 7u);
    }
    a.a[0] = e;
}
uint32_t NativeAdjust(NativeEnv& env, const Args& a) {
    rr::sim::GuestRam g(env.clone->mem.ram().data(), env.gp);
    rr::sim::GridRiderAdjust(g, a.a[0]);
    return RqDone(env, g, 0);
}

} // namespace riders

// `--coverage`: the three functions' extents (each ends at its jr ra's delay slot).
const CoverageRange* RiderCoverage(const char* rowName) {
    struct Named {
        const char* name;
        CoverageRange range;
    };
    static const Named kRanges[] = {
        {"rider_record_init", {riders::kInitFn, riders::kInitEnd}},
        {"rider_name_id", {riders::kNameFn, riders::kNameEnd}},
        {"grid_rider_adjust", {riders::kAdjustFn, riders::kAdjustEnd}},
    };
    for (const Named& n : kRanges)
        if (std::strcmp(n.name, rowName) == 0) return &n.range;
    return nullptr;
}

void AddRiderRows(std::vector<Row>& rows) {
    {
        Row row{"rider_record_init", "void RiderRecordInit(u8 *bi, s32 rec, Bike *e)  [RASHCDI 0x80064C0C]",
                riders::kInitFn, riders::SetupInit, nullptr, /*compareV0=*/false};
        row.nativeEx = riders::NativeInit;
        rows.push_back(row);
    }
    {
        Row row{"rider_name_id", "u32 RiderNameId(Bike *e)  [RASHCDI 0x80066E1C]", riders::kNameFn, riders::SetupName,
                nullptr, /*compareV0=*/true};
        row.nativeEx = riders::NativeName;
        rows.push_back(row);
    }
    {
        Row row{"grid_rider_adjust", "void GridRiderAdjust(Bike *e)  [RASHCDI 0x800650A0]", riders::kAdjustFn,
                riders::SetupAdjust, nullptr, /*compareV0=*/false};
        row.nativeEx = riders::NativeAdjust;
        rows.push_back(row);
    }
}
