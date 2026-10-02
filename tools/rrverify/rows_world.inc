// Bench rows of the cell walker and what it spawns - the roadside props, the
// collision volumes and their lists (src\game\sim\world_pop.h). Included LAST by verify_physics.cpp
// inside its anonymous namespace (after rows_vis.inc), so the
// helpers of the earlier row files (bs::ExactOracle, rcv::Tables) are visible.
//
// The machine is a race capture (rr-race by default: one player on road 9, ten cells in view 0's slots,
// pools 4 / 5 / 6 empty; `--state work\oracle\state\rr-pack` has six props and eight volumes live).
// Every case stirs the world the walker reads: one resident cell is made the bike's own cell (+0xB0),
// view 0's eye is put within 0..400 units of one of its records, records are made fresh / spawned /
// cached, pools 4 and 6 are pre-filled or full, the two-player mode bit is set now and then, and
// entities are planted on the bike's own road for the passes. The seams (world_pop.h) are answered by
// the original code at the port's call-site sp and their call sequence compared.
//
// Named bounds (each a state the game builds or keeps):
//   * one player (the capture's; view 1 and bike 1 are not live there) - the two-player MODE bit is
//     stirred, the player count is not;
//   * a planted prop / volume stands on the bike's road with the bike's own cursor (a real prop seat);
//   * a record's cached piece +6 is -1 or the one the gate would cache (the table is not bounded).
namespace worldrows {

namespace s = rr::sim;

bool Chance(Rng& r, uint32_t n) { return r.U32() % n == 0u; }
void Put16(Memory& m, uint32_t a, uint16_t v) { m.WriteBlock(a, &v, 2); }
uint16_t Get16(Memory& m, uint32_t a) {
    uint16_t v = 0;
    m.ReadBlock(a, &v, 2);
    return v;
}
void Put8(Memory& m, uint32_t a, uint8_t v) { m.WriteBlock(a, &v, 1); }

constexpr uint32_t kView0 = 0x800CD898;

struct Cell {
    uint32_t slot = 0, id = 0, r0 = 0, ext = 0;
};
// The resident slots of player 0 (id not -1, a body).
std::vector<Cell> Cells(Memory& m) {
    std::vector<Cell> v;
    for (uint32_t k = 0; k < 12; ++k) {
        const uint32_t sl = s::kWpCellSlots + 112u * k;
        if (m.PeekWord(sl) == 0xFFFFFFFFu || m.PeekWord(sl + 4u) == 0) continue;
        Cell c;
        c.slot = sl;
        c.id = m.PeekWord(sl + 8u);
        c.r0 = m.PeekWord(m.PeekWord(sl + 4u) + 32u);
        c.ext = m.PeekWord(sl + 76u);
        v.push_back(c);
    }
    return v;
}
// Record `i` of kind-array `a` (0 kind 3, 1 kind 2, 2 kind 4, 3 kind 6, 4 kind 0) of region 0 `r0`.
uint32_t Record(Memory& m, uint32_t r0, uint32_t a, uint32_t i) {
    static const uint32_t kStride[5] = {68, 76, 64, 88, 64};
    return m.PeekWord(r0 + 36u + 4u * a) + kStride[a] * i;
}
uint32_t Count(Memory& m, uint32_t r0, uint32_t a) { return Get16(m, r0 + 2u * a); }

// A record's spawned word +4 and cached piece +6: fresh as on disc, released (0), spawned (1 or a
// volume handle), and the piece kept or forgotten.
void StirRecord(Rng& r, Memory& m, uint32_t rec) {
    switch (r.U32() % 6u) {
    case 0: Put16(m, rec + 4u, 0xFFFF); break;
    case 1: Put16(m, rec + 4u, 0); break;
    case 2: Put16(m, rec + 4u, 1); break;
    case 3: Put16(m, rec + 4u, static_cast<uint16_t>(0xC0u + r.U32() % 24u)); break;
    default: break;
    }
    if (Chance(r, 3)) Put16(m, rec + 6u, 0xFFFF);
}

// Pool 4 pre-filled with `k` taken slots (handles 0x80 + i, their store records taken) and the pool's
// counters and free bytes set as the allocator leaves them; or its free bytes cut to force the refusal.
void FillPool4(Rng& r, Memory& m) {
    const uint32_t base = m.PeekWord(s::kWpPool4Ctrl + 12u);
    const uint32_t k = r.U32() % 16u;
    for (uint32_t i = 0; i < k; ++i) {
        Put16(m, base + 596u * i + 172u, static_cast<uint16_t>(0x80u + i));
        if (i < 18) m.PokeWord(s::kWpStore + 24u * i, 0x80000000u | i);
    }
    const uint32_t hole = k > 2 && Chance(r, 3) ? r.U32() % k : k;
    if (hole < k) Put16(m, base + 596u * hole + 172u, 0);
    m.PokeWord(s::kWpPool4Ctrl, k);
    m.PokeWord(s::kWpPool4Ctrl + 4u, hole);
    m.PokeWord(s::kWpPool4Ctrl + 8u, k == 0 ? 0xFFFFFFFFu : k - 1u);
    m.PokeWord(s::kWpStoreLow, k < 18 ? k : 18u);
    m.PokeWord(s::kWpStoreFree, Chance(r, 4) ? 596u * (r.U32() % 2u) + r.U32() % 596u : 8344u - 596u * k);
}

// Pool 6 pre-filled with `k` volumes near or far from the eye, the per-player list of player 0 built
// as VolumeLists leaves it (bands by distance), or the pool full.
void FillPool6(Rng& r, Memory& m, uint32_t cellId) {
    const uint32_t base = m.PeekWord(s::kWpPool6Ptr);
    const uint32_t cap = m.PeekWord(s::kWpPool6Cap);
    const uint32_t k = Chance(r, 3) ? cap : r.U32() % (cap + 1u);
    const int32_t ex = static_cast<int32_t>(m.PeekWord(kView0 + 184u)), ez = static_cast<int32_t>(m.PeekWord(kView0 + 192u));
    const uint32_t lists = s::kWpVolumeLists;
    const uint32_t list = m.PeekWord(lists + 4u);
    for (uint32_t b = 0; b < 3; ++b) {
        m.PokeWord(lists + 8u + 4u * b, 0);
        m.PokeWord(lists + 20u + 4u * b, 0xFFFFFFFFu);
    }
    int32_t tail[3] = {-1, -1, -1};
    for (uint32_t i = 0; i < k; ++i) {
        const uint32_t v = base + 280u * i;
        Put16(m, v, static_cast<uint16_t>(0xC0u + i));
        Put8(m, v + 2u, static_cast<uint8_t>(r.U32() % 8u));
        Put8(m, v + 3u, 1);
        m.PokeWord(v + 4u, Chance(r, 3) ? cellId : 0x01800000u | (r.U32() & 0x3FFu));
        const int32_t d = static_cast<int32_t>(r.U32() % 120u) << 16;
        m.PokeWord(v + 12u, static_cast<uint32_t>(ex + d));
        m.PokeWord(v + 20u, static_cast<uint32_t>(ez + (static_cast<int32_t>(r.U32() % 20u) << 16)));
        const uint32_t band = d < (20 << 16) ? 0u : (d < (80 << 16) ? 1u : 2u);
        Put16(m, list + 8u * i, static_cast<uint16_t>(i));
        Put16(m, list + 8u * i + 2u, 0xFFFF);
        m.PokeWord(list + 8u * i + 4u, static_cast<uint32_t>(d >> 16));
        m.PokeWord(lists + 8u + 4u * band, m.PeekWord(lists + 8u + 4u * band) + 1u);
        if (tail[band] < 0) m.PokeWord(lists + 20u + 4u * band, i);
        else Put16(m, list + 8u * static_cast<uint32_t>(tail[band]) + 2u, static_cast<uint16_t>(i));
        tail[band] = static_cast<int32_t>(i);
    }
    m.PokeWord(s::kWpPool6Ctrl, k);
    m.PokeWord(s::kWpPool6Ctrl + 4u, k);
    m.PokeWord(s::kWpPool6Ctrl + 8u, k == 0 ? 0xFFFFFFFFu : k - 1u);
}

// View 0's eye within `reach` whole units of record `rec` (x and z).
void EyeNear(Rng& r, Memory& m, uint32_t rec, int32_t reach) {
    const int32_t dx = static_cast<int32_t>(r.U32() % static_cast<uint32_t>(2 * reach + 1)) - reach;
    const int32_t dz = static_cast<int32_t>(r.U32() % static_cast<uint32_t>(2 * reach + 1)) - reach;
    m.PokeWord(kView0 + 184u, m.PeekWord(rec + 20u) + (static_cast<uint32_t>(dx) << 16));
    m.PokeWord(kView0 + 192u, m.PeekWord(rec + 28u) + (static_cast<uint32_t>(dz) << 16));
}

// The common stir. Returns the chosen cell and record (kind array `a`, index `i`).
struct Pick {
    Cell cell;
    uint32_t a = 0, i = 0, rec = 0;
    bool ok = false;
};
Pick StirWorld(CaseContext& c) {
    Rng& r = *c.rng;
    Memory& m = *c.mem;
    Pick pk;
    const std::vector<Cell> cells = Cells(m);
    if (cells.empty()) return pk;
    pk.cell = cells[r.U32() % cells.size()];
    const uint32_t bike = m.PeekWord(s::kWpPlayerBikes);
    // the bike's own cell: this one (CellsAround's first entry), or as captured
    if (!Chance(r, 5)) m.PokeWord(bike + 176u, pk.cell.id);
    // the bike's road: as captured, another road, or in a junction (CellFar's and CellsAround's arms)
    switch (r.U32() % 6u) {
    case 0: m.PokeWord(bike + 360u, r.U32() % 37u); break;
    case 1: m.PokeWord(bike + 360u, 0x10000u | (r.U32() % 25u)); break;
    case 2: m.PokeWord(bike + 368u, m.PeekWord(bike + 368u) + ((r.U32() % 800u) << 16) - (400u << 16)); break;
    default: break;
    }
    if (Chance(r, 3)) m.PokeWord(bike + 364u, Chance(r, 2) ? 1u : 0xFFFFFFFFu);
    // the record the eye goes to: a prop, a volume or a pedestrian of the cell
    std::vector<std::pair<uint32_t, uint32_t>> recs;
    for (uint32_t a : {1u, 2u, 3u})
        for (uint32_t i = 0; i < Count(m, pk.cell.r0, a); ++i) recs.push_back({a, i});
    if (!recs.empty()) {
        const auto [a, i] = recs[r.U32() % recs.size()];
        pk.a = a;
        pk.i = i;
        pk.rec = Record(m, pk.cell.r0, a, i);
        pk.ok = true;
        const int32_t reach = static_cast<int32_t>(r.U32() % (Chance(r, 3) ? 400u : 220u));
        const int32_t dx = static_cast<int32_t>(r.U32() % static_cast<uint32_t>(2 * reach + 1)) - reach;
        const int32_t dz = static_cast<int32_t>(r.U32() % static_cast<uint32_t>(2 * reach + 1)) - reach;
        m.PokeWord(kView0 + 184u, m.PeekWord(pk.rec + 20u) + (static_cast<uint32_t>(dx) << 16));
        m.PokeWord(kView0 + 192u, m.PeekWord(pk.rec + 28u) + (static_cast<uint32_t>(dz) << 16));
    }
    // the records of the cell: most made fresh so that the walker spawns, some stirred
    for (uint32_t a = 0; a < 5; ++a)
        for (uint32_t i = 0; i < Count(m, pk.cell.r0, a); ++i) {
            const uint32_t rec = Record(m, pk.cell.r0, a, i);
            if (Chance(r, 2)) Put16(m, rec + 4u, 0xFFFF);
            if (Chance(r, 4)) StirRecord(r, m, rec);
        }
    if (Chance(r, 3)) FillPool4(r, m);
    if (Chance(r, 3)) FillPool6(r, m, pk.cell.id);
    const uint32_t gs = m.PeekWord(s::kWpGameStatePtr);
    if (Chance(r, 6)) Put8(m, gs + 4u, static_cast<uint8_t>(m.PeekByte(gs + 4u) ^ 0x10u));
    if (Chance(r, 5)) m.PokeWord(0x800D86F4u, r.U32() % 3u);       // cops available (the dead kind-0 arm)
    if (Chance(r, 5)) m.PokeWord(0x8005B314u, r.U32() % 4u);       // hazard objects out
    return pk;
}

// A prop planted in pool-4 slot `i` on the bike's own road (the bike's cursor, road coordinate and box),
// moving (+0x1E0 > 0) or at rest, with its flags +0x250 and rest point +0x1F8.
uint32_t PlantProp(Rng& r, Memory& m, uint32_t i) {
    const uint32_t bike = m.PeekWord(s::kWpPlayerBikes);
    const uint32_t base = m.PeekWord(s::kWpPool4Ctrl + 12u);
    const uint32_t e = base + 596u * i;
    std::vector<uint8_t> b(596);
    m.ReadBlock(bike, b.data(), 596);
    m.WriteBlock(e, b.data(), 596);
    Put16(m, e + 172u, static_cast<uint16_t>(0x80u + i));
    m.PokeWord(e + 4u, s::kWpStore + 24u * (i % 18u));
    Put16(m, e + 320u, static_cast<uint16_t>(Chance(r, 4) ? 0u : 1u));
    m.PokeWord(e + 480u, Chance(r, 3) ? 0u : r.U32() % 0x60000u);
    m.PokeWord(e + 592u, r.U32() & 0x87u);
    for (uint32_t k = 0; k < 3; ++k) {
        m.PokeWord(e + 504u + 4u * k, m.PeekWord(e + 184u + 4u * k) + (r.U32() % 0x40000u) - 0x20000u);
        m.PokeWord(e + 580u + 4u * k, m.PeekWord(e + 184u + 4u * k));
        m.PokeWord(e + 456u + 4u * k, (r.U32() % 0x80000u) - 0x40000u);
    }
    if (Chance(r, 3)) m.PokeWord(e + 388u, m.PeekWord(e + 388u) ^ 1u);
    return e;
}
void PlantProps(Rng& r, Memory& m) {
    const uint32_t k = 1u + r.U32() % 4u;
    for (uint32_t i = 0; i < k; ++i) PlantProp(r, m, i);
    m.PokeWord(s::kWpPool4Ctrl, k);
    m.PokeWord(s::kWpPool4Ctrl + 4u, k);
    m.PokeWord(s::kWpPool4Ctrl + 8u, k - 1u);
    m.PokeWord(s::kWpStoreLow, k);
    m.PokeWord(s::kWpStoreFree, 8344u - 596u * k);
    for (uint32_t i = 0; i < k; ++i) m.PokeWord(s::kWpStore + 24u * i, 0x80000000u | i);
}
// A volume planted in pool-6 slot `i` on the bike's road (the bike's cursor at +156, road word +188).
void PlantVolumes(Rng& r, Memory& m, uint32_t cellId) {
    const uint32_t bike = m.PeekWord(s::kWpPlayerBikes);
    const uint32_t base = m.PeekWord(s::kWpPool6Ptr);
    const uint32_t k = 1u + r.U32() % 5u;
    for (uint32_t i = 0; i < k; ++i) {
        const uint32_t v = base + 280u * i;
        std::vector<uint8_t> b(280, 0);
        m.WriteBlock(v, b.data(), 280);
        Put16(m, v, static_cast<uint16_t>(0xC0u + i));
        Put8(m, v + 2u, static_cast<uint8_t>(r.U32() % 4u));
        Put8(m, v + 3u, static_cast<uint8_t>(r.U32() % 2u)); // one player: no bit for player 1
        m.PokeWord(v + 4u, Chance(r, 2) ? cellId : 0x01800000u | (r.U32() & 0x3FFu));
        m.PokeWord(v + 8u, r.U32() % 2u);
        std::vector<uint8_t> cur(44);
        m.ReadBlock(bike + 0x148u, cur.data(), 44);
        m.WriteBlock(v + 156u, cur.data(), 44);                       // cursor +156..+187, road +188..+199
        if (Chance(r, 3)) m.PokeWord(v + 196u, m.PeekWord(v + 196u) + ((r.U32() % 600u) << 16) - (300u << 16));
        for (uint32_t j = 0; j < 3; ++j)
            m.PokeWord(v + 12u + 4u * j, m.PeekWord(bike + 184u + 4u * j) + (r.U32() % 0x1000000u) - 0x800000u);
        Put16(m, v + 148u, 1);
    }
    m.PokeWord(s::kWpPool6Ctrl, k);
    m.PokeWord(s::kWpPool6Ctrl + 4u, k);
    m.PokeWord(s::kWpPool6Ctrl + 8u, k - 1u);
}

// ---------------------------------------------------------------- the seam and the rows
std::vector<OracleCallee> Seams() {
    return {OracleCallee{s::kWpPedSpawnFn, 3},    OracleCallee{s::kWpHazardSpawnFn, 4, 1},
            OracleCallee{s::kWpResetBikeFn, 1},   OracleCallee{s::kWpTransitionFn, 2},
            OracleCallee{s::kWpBuildObbFn, 1},    OracleCallee{s::kWpGroundQueryFn, 4, 1}};
}

template <class F>
uint32_t RunWorld(NativeEnv& env, F&& f) {
    s::GuestRam g = RqView(env);
    rcv::RecoverOracle o(env);
    const s::BikeTables t = rcv::Tables(env, g);
    uint32_t v0 = 0;
    const bool ok = f(g, t, o, v0);
    if (!ok && env.failure.empty() && !g.Faulted()) env.failure = "the port refused";
    return RqDone(env, g, v0);
}

Row WRow(const char* name, const char* sig, uint32_t entry, std::function<void(CaseContext&, Args&)> setup,
         std::function<uint32_t(NativeEnv&, const Args&)> native, bool v0 = true, bool seams = false) {
    Row r{name, sig, entry, std::move(setup), nullptr, v0, std::move(native)};
    r.maxSteps = 40'000'000;
    if (seams) r.oracleCallees = Seams();
    return r;
}

} // namespace worldrows

void AddWorldRows(std::vector<Row>& rows) {
    using namespace worldrows;
    using G = s::GuestRam;
    using T = s::BikeTables;
    using C = rr::sim::RecoverCallees;
    // ---- the walker, whole (every function below it on the native side)
    rows.push_back(WRow("world_cell_walker", "void 0x8009C308()  [RASHCDG, the cell walker: peds, props, volumes]",
                        s::kCellWalkerFn, [](CaseContext& c, Args&) { StirWorld(c); },
                        [](NativeEnv& env, const Args&) {
                            return RunWorld(env, [&](G& g, const T& t, C& o, uint32_t&) {
                                return s::CellWalker(g, env.sp, t, o);
                            });
                        }, false, true));
    rows.push_back(WRow("world_cell_records", "void 0x8009CA88(id, region0, ext, onRoute, [p])  [RASHCDG, one cell's records]",
                        s::kCellRecordsFn,
                        [](CaseContext& c, Args& a) {
                            const Pick pk = StirWorld(c);
                            a.a[0] = pk.cell.id;
                            a.a[1] = Chance(*c.rng, 12) ? 0u : pk.cell.r0;
                            a.a[2] = Chance(*c.rng, 12) ? 0u : pk.cell.ext;
                            a.a[3] = c.rng->U32() % 2u;
                            a.a4 = 0;
                        },
                        [](NativeEnv& env, const Args& a) {
                            return RunWorld(env, [&](G& g, const T& t, C& o, uint32_t&) {
                                return s::CellRecords(g, a.a[0], a.a[1], a.a[2], static_cast<int32_t>(a.a[3]), a.a4, env.sp, t, o);
                            });
                        }, false, true));
    rows.push_back(WRow("world_record_spawn", "s16 0x8009C654(rec, cellId, p)  [RASHCDG, one record's spawn by kind]",
                        s::kRecordSpawnFn,
                        [](CaseContext& c, Args& a) {
                            const Pick pk = StirWorld(c);
                            a.a[0] = pk.rec != 0 ? pk.rec : Record(*c.mem, pk.cell.r0, 2, 0);
                            a.a[1] = pk.cell.id;
                            a.a[2] = 0;
                        },
                        [](NativeEnv& env, const Args& a) {
                            return RunWorld(env, [&](G& g, const T& t, C& o, uint32_t& v) {
                                return s::RecordSpawn(g, a.a[0], a.a[1], a.a[2], env.sp, t, o, v);
                            });
                        }, true, true));
    rows.push_back(WRow("world_cells_around", "s32 0x8009FAD8(s32 *out, p)  [RASHCDG, the cells around a player]",
                        s::kCellsAroundFn,
                        [](CaseContext& c, Args& a) {
                            StirWorld(c);
                            if (Chance(*c.rng, 4)) { // the rider off the bike, in another cell
                                const uint32_t bike = c.mem->PeekWord(s::kWpPlayerBikes);
                                const uint32_t rider = c.mem->PeekWord(bike + 852u);
                                c.mem->PokeWord(rider + 604u, 3u + c.rng->U32() % 3u);
                                c.mem->PokeWord(rider + 176u, Chance(*c.rng, 3) ? 0xFFFFFFFFu : c.mem->PeekWord(bike + 176u));
                            }
                            a.a[0] = c.scratch;
                            a.a[1] = 0;
                        },
                        [](NativeEnv& env, const Args& a) {
                            return RunWorld(env, [&](G& g, const T&, C&, uint32_t& v) {
                                v = static_cast<uint32_t>(s::CellsAround(g, a.a[0], a.a[1], env.sp));
                                return true;
                            });
                        }));
    rows.push_back(WRow("world_id_in_list", "s32 0x8009FC4C(id, s32 *list, n)  [RASHCDG, a leaf]", s::kIdInListFn,
                        [](CaseContext& c, Args& a) {
                            for (uint32_t k = 0; k < 6; ++k) c.mem->PokeWord(c.scratch + 4u * k, c.rng->U32() % 8u);
                            a.a[0] = c.rng->U32() % 9u;
                            a.a[1] = c.scratch;
                            a.a[2] = c.rng->U32() % 7u;
                        },
                        [](NativeEnv& env, const Args& a) {
                            return RunWorld(env, [&](G& g, const T&, C&, uint32_t& v) {
                                v = static_cast<uint32_t>(s::IdInList(g, a.a[0], a.a[1], static_cast<int32_t>(a.a[2])));
                                return true;
                            });
                        }));
    rows.push_back(WRow("world_cell_slot_for", "Slot *0x80013204(id, p)  [SLUS, player p's slot of a cell]", s::kCellSlotForFn,
                        [](CaseContext& c, Args& a) {
                            const Pick pk = StirWorld(c);
                            a.a[0] = Chance(*c.rng, 4) ? (0x01800000u | (c.rng->U32() & 0x3FFu)) : pk.cell.id;
                            a.a[1] = Chance(*c.rng, 4) ? 1u : 0u;
                        },
                        [](NativeEnv& env, const Args& a) {
                            return RunWorld(env, [&](G& g, const T&, C&, uint32_t& v) {
                                v = s::CellSlotFor(g, a.a[0], a.a[1]);
                                return true;
                            });
                        }));
    rows.push_back(WRow("world_other_slot", "Slot *0x80013360(id, p)  [SLUS, the other player's slot]", s::kOtherSlotFn,
                        [](CaseContext& c, Args& a) {
                            const Pick pk = StirWorld(c);
                            a.a[0] = pk.cell.id;
                            a.a[1] = c.rng->U32() % 2u;
                        },
                        [](NativeEnv& env, const Args& a) {
                            return RunWorld(env, [&](G& g, const T&, C&, uint32_t& v) {
                                v = s::OtherSlot(g, a.a[0], a.a[1]);
                                return true;
                            });
                        }));
    rows.push_back(WRow("world_cell_far", "s32 0x8009F054(ext, p)  [RASHCDG, a cell far along the player's road]", s::kCellFarFn,
                        [](CaseContext& c, Args& a) {
                            const Pick pk = StirWorld(c);
                            a.a[0] = Chance(*c.rng, 10) ? 0u : pk.cell.ext;
                            a.a[1] = 0;
                        },
                        [](NativeEnv& env, const Args& a) {
                            return RunWorld(env, [&](G& g, const T&, C&, uint32_t& v) {
                                v = static_cast<uint32_t>(s::CellFar(g, a.a[0], a.a[1]));
                                return true;
                            });
                        }));
    rows.push_back(WRow("world_resident_pieces", "s32 0x8003C42C(s32 *out, max)  [SLUS, the resident road pieces]",
                        s::kResidentPiecesFn,
                        [](CaseContext& c, Args& a) {
                            a.a[0] = Chance(*c.rng, 10) ? 0u : c.scratch;
                            a.a[1] = c.rng->U32() % 8u;
                        },
                        [](NativeEnv& env, const Args& a) {
                            return RunWorld(env, [&](G& g, const T&, C&, uint32_t& v) {
                                v = static_cast<uint32_t>(s::ResidentPieces(g, a.a[0], static_cast<int32_t>(a.a[1])));
                                return true;
                            });
                        }));
    rows.push_back(WRow("world_piece_in_cell", "s32 0x8009F11C(key, ext)  [RASHCDG, a road piece inside a cell]",
                        s::kPieceInCellFn,
                        [](CaseContext& c, Args& a) {
                            const Pick pk = StirWorld(c);
                            const int32_t last = static_cast<int32_t>(c.mem->PeekWord(s::kWpPiecesLast));
                            a.a[0] = last >= 0 ? c.mem->PeekWord(s::kWpPieces + 16u * (c.rng->U32() % static_cast<uint32_t>(last + 1))) : 0u;
                            if (Chance(*c.rng, 8)) a.a[0] = 0xFFFFFFFFu;
                            a.a[1] = Chance(*c.rng, 10) ? 0u : pk.cell.ext;
                        },
                        [](NativeEnv& env, const Args& a) {
                            return RunWorld(env, [&](G& g, const T&, C&, uint32_t& v) {
                                v = static_cast<uint32_t>(s::PieceInCell(g, a.a[0], a.a[1]));
                                return true;
                            });
                        }));
    rows.push_back(WRow("world_cell_pieces", "s32 0x8009EF8C(ext, s32 *out, max)  [RASHCDG, the pieces of a cell]",
                        s::kCellPiecesFn,
                        [](CaseContext& c, Args& a) {
                            const Pick pk = StirWorld(c);
                            a.a[0] = Chance(*c.rng, 10) ? 0u : pk.cell.ext;
                            a.a[1] = c.scratch;
                            a.a[2] = c.rng->U32() % 7u;
                        },
                        [](NativeEnv& env, const Args& a) {
                            return RunWorld(env, [&](G& g, const T&, C&, uint32_t& v) {
                                v = static_cast<uint32_t>(s::CellPieces(g, a.a[0], a.a[1], static_cast<int32_t>(a.a[2]), env.sp));
                                return true;
                            });
                        }));
    // the record tests: the list of the cell's pieces planted in the scratch block
    auto recordArgs = [](CaseContext& c, Args& a, bool piece) {
        const Pick pk = StirWorld(c);
        Memory& m = *c.mem;
        uint32_t n = 0;
        const int32_t last = static_cast<int32_t>(m.PeekWord(s::kWpPiecesLast));
        for (int32_t k = 0; k <= last && n < 6; ++k) {
            const uint32_t key = m.PeekWord(s::kWpPieces + 16u * static_cast<uint32_t>(k));
            if (key == 0xFFFFFFFFu || Chance(*c.rng, 3)) continue;
            m.PokeWord(c.scratch + 4u * n++, key);
        }
        uint32_t rec = pk.rec;
        if (rec == 0 || Chance(*c.rng, 3)) {
            const uint32_t a2 = c.rng->U32() % 5u;
            if (Count(m, pk.cell.r0, a2) != 0) rec = Record(m, pk.cell.r0, a2, c.rng->U32() % Count(m, pk.cell.r0, a2));
        }
        if (rec == 0) rec = Record(m, pk.cell.r0, 3, 0);
        if (piece && Chance(*c.rng, 2) && n > 0) { // a record keyed to a junction child of a listed piece
            s::GuestRam gg(m.ram().data(), c.gp);
            const uint32_t b = s::RoadBttRecord(gg, static_cast<int32_t>(m.PeekWord(c.scratch)));
            if (b != 0 && m.PeekWord(b + 12u) != 0) {
                const uint32_t r = m.PeekWord(b + 12u);
                const uint32_t nc = Get16(m, r + 30u);
                if (nc > 0 && nc < 64) m.PokeWord(rec + 8u, m.PeekWord(m.PeekWord(r + 72u) + 20u * (c.rng->U32() % nc)));
            }
            Put16(m, rec + 6u, 0xFFFF);
        }
        a.a[0] = rec;
        a.a[1] = c.scratch;
        a.a[2] = n;
        a.a[3] = 0;
    };
    rows.push_back(WRow("world_record_junction", "s32 0x8009F288(rec, s32 *list, n)  [RASHCDG, a record re-keyed off a junction]",
                        s::kRecordJunctionFn, [recordArgs](CaseContext& c, Args& a) { recordArgs(c, a, true); },
                        [](NativeEnv& env, const Args& a) {
                            return RunWorld(env, [&](G& g, const T&, C&, uint32_t& v) {
                                v = static_cast<uint32_t>(s::RecordJunction(g, a.a[0], a.a[1], static_cast<int32_t>(a.a[2])));
                                return true;
                            });
                        }));
    rows.push_back(WRow("world_record_window", "s32 0x8009C4FC(rec, s32 *list, n, p)  [RASHCDG, a record's road gate and window]",
                        s::kRecordWindowFn, [recordArgs](CaseContext& c, Args& a) { recordArgs(c, a, Chance(*c.rng, 3)); },
                        [](NativeEnv& env, const Args& a) {
                            return RunWorld(env, [&](G& g, const T&, C&, uint32_t& v) {
                                v = static_cast<uint32_t>(s::RecordWindow(g, a.a[0], a.a[1], static_cast<int32_t>(a.a[2]), a.a[3], env.sp));
                                return true;
                            });
                        }));
    rows.push_back(WRow("world_record_stamp", "void 0x8009C41C(slot, kind, idx, value)  [RASHCDG, the other player's record +4]",
                        s::kRecordStampFn,
                        [](CaseContext& c, Args& a) {
                            const Pick pk = StirWorld(c);
                            const uint32_t gs = c.mem->PeekWord(s::kWpGameStatePtr);
                            if (Chance(*c.rng, 2)) Put8(*c.mem, gs + 4u, static_cast<uint8_t>(c.mem->PeekByte(gs + 4u) | 0x10u));
                            static const uint32_t kKinds[5] = {3, 2, 4, 6, 0};
                            const uint32_t arr = c.rng->U32() % 5u;
                            const uint32_t n = Count(*c.mem, pk.cell.r0, arr);
                            a.a[0] = Chance(*c.rng, 8) ? 0u : pk.cell.slot;
                            a.a[1] = Chance(*c.rng, 6) ? c.rng->U32() % 9u : kKinds[arr];
                            a.a[2] = n != 0 ? c.rng->U32() % n : 0u;
                            if (n == 0) a.a[1] = 1;
                            a.a[3] = c.rng->U32() & 0xFFFFu;
                        },
                        [](NativeEnv& env, const Args& a) {
                            return RunWorld(env, [&](G& g, const T&, C&, uint32_t&) {
                                s::RecordStamp(g, a.a[0], a.a[1], a.a[2], a.a[3]);
                                return true;
                            });
                        }, false));
    rows.push_back(WRow("world_prop_kind", "s32 0x8009C5E4(cls)  [RASHCDG, a prop group's DOD3 kind code]", s::kPropKindFn,
                        [](CaseContext& c, Args& a) { a.a[0] = c.rng->U32() % 45u; },
                        [](NativeEnv& env, const Args& a) {
                            return RunWorld(env, [&](G& g, const T&, C&, uint32_t& v) {
                                v = static_cast<uint32_t>(s::PropKind(g, static_cast<int32_t>(a.a[0])));
                                return true;
                            });
                        }));
    rows.push_back(WRow("world_volume_room", "s32 0x8008D9E4(p)  [RASHCDG, pool 6 room for a player]", s::kVolumeRoomFn,
                        [](CaseContext& c, Args& a) {
                            if (Chance(*c.rng, 2)) c.mem->PokeWord(c.mem->PeekWord(s::kWpGameStatePtr) + 48u, 2u);
                            c.mem->PokeWord(s::kWpPool6Ctrl + 16u, c.rng->U32() % 20u);
                            c.mem->PokeWord(s::kWpPool6Ctrl + 20u, c.rng->U32() % 20u);
                            a.a[0] = c.rng->U32() % 2u;
                        },
                        [](NativeEnv& env, const Args& a) {
                            return RunWorld(env, [&](G& g, const T&, C&, uint32_t& v) {
                                v = static_cast<uint32_t>(s::VolumeRoom(g, a.a[0]));
                                return true;
                            });
                        }));
    rows.push_back(WRow("world_class50", "void 0x80012BA8(rec, cellId, p)  [SLUS, the class-50 record]", s::kClass50Fn,
                        [](CaseContext& c, Args& a) {
                            const Pick pk = StirWorld(c);
                            a.a[0] = pk.rec != 0 ? pk.rec : Record(*c.mem, pk.cell.r0, 3, 0);
                            a.a[1] = pk.cell.id;
                            a.a[2] = c.rng->U32() % 2u;
                        },
                        [](NativeEnv& env, const Args& a) {
                            return RunWorld(env, [&](G& g, const T&, C&, uint32_t&) {
                                s::Class50(g, a.a[0], a.a[1], a.a[2]);
                                return true;
                            });
                        }, false));
    // ---- props
    auto propArgs = [](CaseContext& c, Args& a) {
        const Pick pk = StirWorld(c);
        Memory& m = *c.mem;
        uint32_t rec = pk.rec;
        if (pk.a != 2 || Chance(*c.rng, 4)) { // a prop record of any resident cell
            for (const Cell& cl : Cells(m))
                if (Count(m, cl.r0, 2) != 0 && (rec == 0 || pk.a != 2 || Chance(*c.rng, 2)))
                    rec = Record(m, cl.r0, 2, c.rng->U32() % Count(m, cl.r0, 2));
        }
        if (rec == 0) rec = Record(m, pk.cell.r0, 3, 0);
        Put16(m, rec + 4u, 0xFFFF);
        if (!Chance(*c.rng, 5)) EyeNear(*c.rng, m, rec, 150); // mostly inside the window: the seat succeeds
        if (Chance(*c.rng, 3)) Put16(m, rec + 12u, static_cast<uint16_t>(c.rng->U32() % 4u)); // the frame flags
        if (Chance(*c.rng, 5)) Put16(m, rec + 2u, static_cast<uint16_t>(1u + c.rng->U32() % 38u));
        a.a[0] = rec;
        a.a[1] = m.PeekWord(s::kWpPlayerBikes);
    };
    rows.push_back(WRow("world_prop_alloc4", "Prop *0x800A2630(rec, bike)  [RASHCDG, a pool-4 prop]", s::kPropAlloc4Fn,
                        propArgs,
                        [](NativeEnv& env, const Args& a) {
                            return RunWorld(env, [&](G& g, const T& t, C& o, uint32_t& v) {
                                return s::PropAlloc4(g, a.a[0], a.a[1], env.sp, t, o, v);
                            });
                        }, true, true));
    rows.push_back(WRow("world_prop_alloc5", "Prop *0x800A2448(rec, bike)  [RASHCDG, a pool-5 prop]", s::kPropAlloc5Fn,
                        propArgs,
                        [](NativeEnv& env, const Args& a) {
                            return RunWorld(env, [&](G& g, const T& t, C& o, uint32_t& v) {
                                return s::PropAlloc5(g, a.a[0], a.a[1], env.sp, t, o, v);
                            });
                        }, true, true));
    rows.push_back(WRow("world_prop_init", "s32 0x800A07D0(rec, e, pool, bike)  [RASHCDG, a prop set up from its record]",
                        s::kPropInitFn,
                        [propArgs](CaseContext& c, Args& a) {
                            propArgs(c, a);
                            Memory& m = *c.mem;
                            const uint32_t e = m.PeekWord(s::kWpPool4Ctrl + 12u);
                            Put16(m, e + 172u, 0x80);
                            m.PokeWord(e + 4u, s::kWpStore);
                            m.PokeWord(s::kWpPool4Ctrl, 1);
                            m.PokeWord(s::kWpPool4Ctrl + 4u, 1);
                            m.PokeWord(s::kWpPool4Ctrl + 8u, 0);
                            m.PokeWord(s::kWpStore, 0x80000000u);
                            a.a[3] = a.a[1];
                            a.a[1] = e;
                            a.a[2] = 4;
                        },
                        [](NativeEnv& env, const Args& a) {
                            return RunWorld(env, [&](G& g, const T& t, C& o, uint32_t& v) {
                                return s::PropInit(g, a.a[0], a.a[1], a.a[2], a.a[3], env.sp, t, o, v);
                            });
                        }, true, true));
    rows.push_back(WRow("world_prop_rows", "void 0x800A3F14(rec, e)  [RASHCDG, a prop's rows]", s::kPropRowsFn,
                        [propArgs](CaseContext& c, Args& a) {
                            propArgs(c, a);
                            Memory& m = *c.mem;
                            const uint32_t bike = a.a[1];
                            Put16(m, a.a[0] + 12u, static_cast<uint16_t>(c.rng->U32() % 4u));
                            for (uint32_t k = 0; k < 3; ++k)
                                if (Chance(*c.rng, 2)) Put16(m, a.a[0] + 14u + 2u * k, static_cast<uint16_t>(c.rng->S16() >> 3));
                            a.a[1] = bike; // the bike as the entity: +0x154 its slice
                        },
                        [](NativeEnv& env, const Args& a) {
                            return RunWorld(env, [&](G& g, const T& t, C&, uint32_t&) { return s::PropRows(g, a.a[0], a.a[1], t); });
                        }, false));
    rows.push_back(WRow("world_matrix_quat", "void 0x800716C0(s16 m[9], s16 q[4])  [RASHCDG, a rotation's quaternion]",
                        s::kMatrixQuatFn,
                        [](CaseContext& c, Args& a) {
                            Memory& m = *c.mem;
                            const uint32_t bike = m.PeekWord(s::kWpPlayerBikes);
                            std::vector<uint8_t> rows(18);
                            m.ReadBlock(bike + 432u + (Chance(*c.rng, 2) ? 84u : 0u), rows.data(), 18);
                            m.WriteBlock(c.scratch, rows.data(), 18);
                            if (Chance(*c.rng, 2)) { // a turned frame: the diagonal's sign flipped in pairs
                                const uint32_t pick = c.rng->U32() % 3u;
                                for (uint32_t k = 0; k < 9; ++k)
                                    if ((k / 3) != pick && (k % 3) != pick)
                                        Put16(m, c.scratch + 2u * k, static_cast<uint16_t>(0u - Get16(m, c.scratch + 2u * k)));
                            }
                            if (Chance(*c.rng, 4))
                                for (uint32_t k = 0; k < 9; ++k) Put16(m, c.scratch + 2u * k, static_cast<uint16_t>(c.rng->S16() >> 3));
                            a.a[0] = c.scratch;
                            a.a[1] = c.scratch + 64u;
                        },
                        [](NativeEnv& env, const Args& a) {
                            return RunWorld(env, [&](G& g, const T& t, C&, uint32_t&) {
                                s::MatrixQuat(g, a.a[0], a.a[1], t);
                                return true;
                            });
                        }, false));
    rows.push_back(WRow("world_junction_slice", "Slice *0x8003C758(cursor, pos)  [SLUS, a junction cursor's nearest slice]",
                        s::kJunctionSliceFn,
                        [](CaseContext& c, Args& a) {
                            Memory& m = *c.mem;
                            const uint32_t bike = m.PeekWord(s::kWpPlayerBikes);
                            std::vector<uint8_t> cur(32);
                            m.ReadBlock(bike + 0x148u, cur.data(), 32);
                            m.WriteBlock(c.scratch, cur.data(), 32);
                            for (uint32_t k = 0; k < 3; ++k)
                                m.PokeWord(c.scratch + 64u + 4u * k, m.PeekWord(bike + 184u + 4u * k) + (c.rng->U32() % 0x4000000u) - 0x2000000u);
                            a.a[0] = Chance(*c.rng, 10) ? 0u : c.scratch;
                            a.a[1] = c.scratch + 64u;
                        },
                        [](NativeEnv& env, const Args& a) {
                            return RunWorld(env, [&](G& g, const T&, C&, uint32_t& v) {
                                v = s::JunctionSlice(g, a.a[0], a.a[1]);
                                return true;
                            });
                        }));
    rows.push_back(WRow("world_prop_settle", "void 0x800A3A84(e)  [RASHCDG, a moving prop's ground contact]", s::kPropSettleFn,
                        [](CaseContext& c, Args& a) {
                            StirWorld(c);
                            a.a[0] = PlantProp(*c.rng, *c.mem, 0);
                        },
                        [](NativeEnv& env, const Args& a) {
                            return RunWorld(env, [&](G& g, const T&, C& o, uint32_t&) { return s::PropSettle(g, a.a[0], env.sp, o); });
                        }, false, true));
    rows.push_back(WRow("world_prop_pass", "void 0x800A2898(dt)  [RASHCDG, every pool-4 prop]", s::kPropPassFn,
                        [](CaseContext& c, Args& a) {
                            StirWorld(c);
                            PlantProps(*c.rng, *c.mem);
                            a.a[0] = 0x884u + c.rng->U32() % 0x2000u;
                        },
                        [](NativeEnv& env, const Args& a) {
                            return RunWorld(env, [&](G& g, const T&, C& o, uint32_t&) {
                                return s::PropPass(g, static_cast<int32_t>(a.a[0]), env.sp, o);
                            });
                        }, false, true));
    rows.push_back(WRow("world_pool5_pass", "void 0x8009ACA4()  [RASHCDG, every pool-5 entity's window]", s::kPool5PassFn,
                        [](CaseContext& c, Args&) {
                            StirWorld(c);
                            Memory& m = *c.mem;
                            const uint32_t bike = m.PeekWord(s::kWpPlayerBikes);
                            const uint32_t base = m.PeekWord(s::kWpPool5Ctrl + 12u);
                            const uint32_t k = c.rng->U32() % 4u;
                            for (uint32_t i = 0; i < k; ++i) {
                                const uint32_t e = base - 452u * i;
                                std::vector<uint8_t> b(452);
                                m.ReadBlock(bike, b.data(), 452);
                                m.WriteBlock(e, b.data(), 452);
                                Put16(m, e + 172u, static_cast<uint16_t>(Chance(*c.rng, 5) ? 0u : 0xA0u + i));
                                m.PokeWord(e + 4u, s::kWpStore + 24u * i);
                                m.PokeWord(s::kWpStore + 24u * i, 0x80000000u | i);
                                if (Chance(*c.rng, 3)) m.PokeWord(e + 368u, m.PeekWord(e + 368u) + ((c.rng->U32() % 600u) << 16) - (300u << 16));
                            }
                            m.PokeWord(s::kWpPool5Ctrl, k);
                            m.PokeWord(s::kWpPool5Ctrl + 4u, k);
                            m.PokeWord(s::kWpPool5Ctrl + 8u, k - 1u);
                        },
                        [](NativeEnv& env, const Args&) {
                            return RunWorld(env, [&](G& g, const T&, C&, uint32_t&) { return s::Pool5Pass(g, env.sp); });
                        }, false));
    // ---- collision volumes
    rows.push_back(WRow("world_volume_spawn", "Vol *0x8009BB48(rec, bike)  [RASHCDG, a collision volume]", s::kVolumeSpawnFn,
                        [](CaseContext& c, Args& a) {
                            const Pick pk = StirWorld(c);
                            Memory& m = *c.mem;
                            uint32_t rec = pk.a == 3 ? pk.rec : 0u;
                            for (const Cell& cl : Cells(m))
                                if (Count(m, cl.r0, 3) != 0 && (rec == 0 || Chance(*c.rng, 3)))
                                    rec = Record(m, cl.r0, 3, c.rng->U32() % Count(m, cl.r0, 3));
                            if (Chance(*c.rng, 3)) Put16(m, rec + 12u, static_cast<uint16_t>(c.rng->U32() % 4u));
                            if (Chance(*c.rng, 4)) Put16(m, rec + 2u, static_cast<uint16_t>(c.rng->U32() % 2u));
                            if (!Chance(*c.rng, 5)) EyeNear(*c.rng, m, rec, 60);
                            a.a[0] = rec;
                            a.a[1] = m.PeekWord(s::kWpPlayerBikes);
                        },
                        [](NativeEnv& env, const Args& a) {
                            return RunWorld(env, [&](G& g, const T& t, C&, uint32_t& v) {
                                return s::VolumeSpawn(g, a.a[0], a.a[1], env.sp, t, v);
                            });
                        }));
    rows.push_back(WRow("world_volume_mask", "s32 0x80013294(kind, h, p)  [SLUS, a volume's per-player bit]", s::kVolumeMaskFn,
                        [](CaseContext& c, Args& a) {
                            const Pick pk = StirWorld(c);
                            PlantVolumes(*c.rng, *c.mem, pk.cell.id);
                            a.a[0] = 6;
                            a.a[1] = Chance(*c.rng, 6) ? c.rng->U32() & 0xFFFFu : 0xC0u + c.rng->U32() % 5u;
                            a.a[2] = c.rng->U32() % 2u;
                        },
                        [](NativeEnv& env, const Args& a) {
                            return RunWorld(env, [&](G& g, const T&, C&, uint32_t& v) {
                                v = static_cast<uint32_t>(s::VolumeMask(g, a.a[0], a.a[1], a.a[2]));
                                return true;
                            });
                        }));
    rows.push_back(WRow("world_volume_unstamp", "void 0x8009F3D0(vol, p)  [RASHCDG, a volume's record spawnable again]",
                        s::kVolumeUnstampFn,
                        [](CaseContext& c, Args& a) {
                            const Pick pk = StirWorld(c);
                            PlantVolumes(*c.rng, *c.mem, pk.cell.id);
                            a.a[0] = Chance(*c.rng, 10) ? 0u : c.mem->PeekWord(s::kWpPool6Ptr) + 280u * (c.rng->U32() % 5u);
                            a.a[1] = 0;
                        },
                        [](NativeEnv& env, const Args& a) {
                            return RunWorld(env, [&](G& g, const T&, C&, uint32_t&) {
                                s::VolumeUnstamp(g, a.a[0], a.a[1]);
                                return true;
                            });
                        }, false));
    rows.push_back(WRow("world_volume_lists", "void 0x8008D89C()  [RASHCDG, the volume lists rebuilt]", s::kVolumeListsFn,
                        [](CaseContext& c, Args&) {
                            const Pick pk = StirWorld(c);
                            PlantVolumes(*c.rng, *c.mem, pk.cell.id);
                        },
                        [](NativeEnv& env, const Args&) {
                            return RunWorld(env, [&](G& g, const T&, C&, uint32_t&) {
                                s::VolumeLists(g);
                                return true;
                            });
                        }, false));
    rows.push_back(WRow("world_volume_take", "u16 0x8008DECC(p, band)  [RASHCDG, a volume taken off a band]", s::kVolumeTakeFn,
                        [](CaseContext& c, Args& a) {
                            const Pick pk = StirWorld(c);
                            FillPool6(*c.rng, *c.mem, pk.cell.id);
                            a.a[0] = 0;
                            a.a[1] = c.rng->U32() % 3u;
                        },
                        [](NativeEnv& env, const Args& a) {
                            return RunWorld(env, [&](G& g, const T&, C&, uint32_t& v) {
                                v = s::VolumeTake(g, a.a[0], a.a[1]);
                                return true;
                            });
                        }));
    rows.push_back(WRow("world_volume_evict", "void 0x8008DA20(rec, p)  [RASHCDG, pool 6 full: a far volume released]",
                        s::kVolumeEvictFn,
                        [](CaseContext& c, Args& a) {
                            const Pick pk = StirWorld(c);
                            FillPool6(*c.rng, *c.mem, pk.cell.id);
                            a.a[0] = pk.rec != 0 ? pk.rec : Record(*c.mem, pk.cell.r0, 3, 0);
                            a.a[1] = 0;
                        },
                        [](NativeEnv& env, const Args& a) {
                            return RunWorld(env, [&](G& g, const T&, C&, uint32_t&) { return s::VolumeEvict(g, a.a[0], a.a[1]); });
                        }, false));
    rows.push_back(WRow("world_volume_pass", "void 0x8009AB60()  [RASHCDG, every volume's window]", s::kVolumePassFn,
                        [](CaseContext& c, Args&) {
                            const Pick pk = StirWorld(c);
                            PlantVolumes(*c.rng, *c.mem, pk.cell.id);
                        },
                        [](NativeEnv& env, const Args&) {
                            return RunWorld(env, [&](G& g, const T&, C&, uint32_t&) { return s::VolumePass(g, env.sp); });
                        }, false));
}
