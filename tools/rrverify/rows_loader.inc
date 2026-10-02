// Bench rows of the race loader's set-up path (src\game\sim\loader.{h,cpp}),
// RASHCDI SHA-1 9a8b79d8739713c12b0a2dc4d75ef8f0a2cc0b06 and SLUS_010.53. Included LAST by verify_physics.cpp
// inside its anonymous namespace (rows_grid.inc's gridr::InjectOverlay, rows_stream.inc's strows::Rec, rows_population.inc's
// pop:: and rows_bike_step.inc's bs::ExactOracle are visible).
//
// The machine is a race capture (rr-race by default). RASHCDI is not resident there (RASHCDG is): every case lays the
// WHOLE overlay image over 0x8005B5E8 on both machines first, as the console has it while the race loads.
//
// NAMED BOUNDS (the same on both machines, compared as seams where the port makes the call):
//   * the ORCHESTRATORS' children (EnterRace's six, RaceReset's fourteen, SetUpRace's eighteen) are stubbed
//     (`lui v0 / jr ra / ori v0`, v0 drawn per case where the parent stores it): each is ported and benched on its
//     own row where it is ported, and the rest load the race off the CD;
//   * the CD layer is the host's: LoadFile SLUS 0x80014654 is a stub that hands back a planted copy of the player's own
//     file (work\disc_us) and its length (or -1), and Free SLUS 0x800144B8 a `jr ra` (the planted copy is no heap
//     block); the file open / read / close of the sound loader likewise (the directory planted);
//   * the GPU / SPU library and the interrupt set-up (LoadImage 0x80048A6C, 0x8004D9A8 / 0x8004D9E4 / 0x8004DA44, the
//     critical-section stubs, the bank reader 0x80062D84, the SPU upload 0x8001E938, the sound resets) are `jr ra`;
//   * a malloc whose block the function writes more than 8 KiB into returns a planted block (the stream records'
//     buffers 28..31, 64 KiB, as the capture holds them) - the heap is the host's in the product too; the small
//     mallocs of the handlers run the original's heap;
//   * bzero (BIOS A(28h)) is a `jr ra`: the product's is the host's.
namespace ldr {

namespace s = rr::sim;
using pop::Chance;
using pop::Put8;

// `lui v0,hi; jr ra; ori v0,v0,lo`
void StubV0(Memory& m, uint32_t fn, uint32_t v0) {
    m.PokeWord(fn, 0x3C020000u | (v0 >> 16));
    m.PokeWord(fn + 4u, 0x03E00008u);
    m.PokeWord(fn + 8u, 0x34420000u | (v0 & 0xFFFFu));
}
// LoadFile(name, mode, &buf, &len, 0): *a2 = buf, *a3 = len, v0 = 0 (or -1: nothing stored).
void StubLoadFile(Memory& m, uint32_t buf, uint32_t len, bool fail) {
    const uint32_t fn = s::kLdLoadFile;
    if (fail) {
        m.PokeWord(fn, 0x03E00008u);
        m.PokeWord(fn + 4u, 0x2402FFFFu);
        return;
    }
    const uint32_t w[8] = {0x3C080000u | (buf >> 16), 0x35080000u | (buf & 0xFFFFu), 0xACC80000u,
                           0x3C080000u | (len >> 16), 0x35080000u | (len & 0xFFFFu), 0xACE80000u,
                           0x03E00008u, 0x00001021u};
    for (uint32_t k = 0; k < 8u; ++k) m.PokeWord(fn + 4u * k, w[k]);
}
// read(handle, dst, n, off): copies 96 bytes from `src` to a1, v0 = 96.
void StubRead96(Memory& m, uint32_t fn, uint32_t src) {
    const uint32_t w[11] = {0x3C080000u | (src >> 16), 0x35080000u | (src & 0xFFFFu), 0x24090018u, // li t1,24
                            0x8D0A0000u,  // lw t2,0(t0)
                            0x25080004u,  // addiu t0,t0,4
                            0xACAA0000u,  // sw t2,0(a1)
                            0x2529FFFFu,  // addiu t1,t1,-1
                            0x1520FFFBu,  // bnez t1,-5
                            0x24A50004u,  // addiu a1,a1,4 (delay)
                            0x03E00008u, 0x24020060u};
    for (uint32_t k = 0; k < 11u; ++k) m.PokeWord(fn + 4u * k, w[k]);
}
void JrRa(Memory& m, uint32_t fn) {
    m.PokeWord(fn, 0x03E00008u);
    m.PokeWord(fn + 4u, 0);
}

const std::vector<uint8_t>& DiscFile(const char* name) {
    struct Entry {
        std::string name;
        std::vector<uint8_t> bytes;
    };
    static std::vector<std::unique_ptr<Entry>> cache; // entries never move: the references stay valid
    for (const auto& e : cache)
        if (e->name == name) return e->bytes;
    auto e = std::make_unique<Entry>();
    e->name = name;
    std::string error;
    const std::string path = std::string("work\\disc_us\\") + name;
    if (!rr::interp::ReadWholeFile(path, e->bytes, error)) std::fprintf(stderr, "rows_loader: %s not readable\n", path.c_str());
    cache.push_back(std::move(e));
    return cache.back()->bytes;
}

// The planted-file area: the stream records' buffers 28..31 (64 KiB, one malloc in the capture).
uint32_t Area(Memory& m) { return m.PeekWord(strows::Rec(m, 28) + 12u); }
constexpr uint32_t kAreaBytes = 0x10000;

uint32_t Plant(Memory& m, const std::vector<uint8_t>& f, uint32_t at, uint32_t max) {
    const uint32_t n = static_cast<uint32_t>(std::min<size_t>(f.size(), max));
    if (n) m.WriteBlock(at, f.data(), n);
    return n;
}

struct Seams final : s::LoaderCallees {
    NativeEnv& env;
    explicit Seams(NativeEnv& e) : env(e) {}
    bool Call(uint32_t fn, const uint32_t* a, int n, uint32_t sp, uint32_t& v0) override {
        v0 = bs::ExactOracle(env, fn, a, n, sp);
        return env.failure.empty();
    }
};

template <typename F>
uint32_t Run(NativeEnv& env, const char* what, F f) {
    s::GuestRam g = pop::View(env);
    Seams c(env);
    uint32_t v0 = 0;
    const bool ok = f(g, c, v0);
    return pop::PopDone(env, g, ok, what, v0);
}

std::vector<OracleCallee> Callees(std::initializer_list<std::pair<uint32_t, int>> list) {
    std::vector<OracleCallee> v;
    for (const auto& [a, n] : list) v.push_back(OracleCallee{a, n});
    return v;
}

uint32_t Gs(Memory& m) { return m.PeekWord(s::kLdGameStatePtr); }
const uint32_t kTypes[] = {0u, 1u, 4u, 5u, 8u, 0x10u, 0x11u, 0x18u, 0x20u, 0x21u, 33u, 34u, 36u, 44u};
uint32_t AnyType(Rng& r) { return Chance(r, 8) ? r.U32() & 0xFFu : kTypes[r.U32() % (sizeof(kTypes) / sizeof(kTypes[0]))]; }

// ---------------------------------------------------------------------------- EnterRace 0x80063B90
const uint32_t kEnterKids[] = {0x8001B6A8u, s::kLdRaceResetFn, 0x800635D0u, s::kLdSetUpRaceFn, 0x80060088u, 0x8001BDB0u};
void SetupEnter(CaseContext& c, Args&) {
    gridr::InjectOverlay(c);
    for (uint32_t fn : kEnterKids) StubV0(*c.mem, fn, 0);
    if (c.sample) return;
    Put8(*c.mem, Gs(*c.mem) + 4u, AnyType(*c.rng));
    Put8(*c.mem, Gs(*c.mem) + 3u, c.rng->U32());
    c.mem->PokeWord(s::kLdPedSwitch, c.rng->U32());
}

// ---------------------------------------------------------------------------- RaceReset 0x80063500
const std::pair<uint32_t, int> kResetKids[] = {{0x80018C1Cu, 1}, {0x80020E30u, 1}, {0x8005D9ECu, 1}, {s::kLdModelTablesFn, 0},
                                               {0x8005D5E4u, 0}, {0x8001C1ACu, 0}, {0x8005D8D0u, 0}, {0x80063E48u, 0},
                                               {0x8005ED94u, 0}, {0x80063C20u, 0}, {0x8002D2E0u, 0}, {0x80063C00u, 0},
                                               {0x8005D330u, 0}, {0x8005D500u, 0}};
void SetupReset(CaseContext& c, Args&) {
    gridr::InjectOverlay(c);
    for (const auto& k : kResetKids) StubV0(*c.mem, k.first, 0);
    if (c.sample) return;
    for (uint32_t k = 0; k < 6u; ++k) {
        c.mem->PokeWord(s::kLdPlayerBlocks + 36u * k, c.rng->U32());
        c.mem->PokeWord(s::kLdPlayerBlocks + 36u * k + 4u, c.rng->U32());
    }
    Put8(*c.mem, Gs(*c.mem), c.rng->U32());
}

// ---------------------------------------------------------------------------- SetUpRace 0x80063670
const std::pair<uint32_t, int> kSetUpKids[] = {{0x8005D130u, 1}, {0x800653E8u, 1}, {0x800244E0u, 0}, {0x8006AC6Cu, 1},
                                               {0x80022F78u, 0}, {0x80023020u, 0}, {0x8006ACD0u, 3}, {0x8006982Cu, 0},
                                               {0x8006AD4Cu, 0}, {0x80018DC8u, 1}, {0x8005C7F0u, 1}, {0x8005D1A0u, 2},
                                               {0x8006AEF4u, 0}, {0x800235B0u, 2}, {0x8002B83Cu, 0}, {0x8005CEE0u, 1}};
void SetupSetUp(CaseContext& c, Args&) {
    gridr::InjectOverlay(c);
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    for (const auto& k : kSetUpKids) StubV0(m, k.first, 0);
    StubV0(m, 0x800653E8u, c.sample ? 0x801A6B5Cu : r.U32());          // the fight table (rr-race's own)
    StubV0(m, 0x8006AD4Cu, c.sample ? 4u : (Chance(r, 4) ? r.U32() : r.U32() % 12u));
    const uint32_t gs = Gs(m);
    const uint32_t p1 = m.PeekWord(0x8005B38Cu);
    // NAMED BOUND: a bike's +0x358 (the ridden bike's partner) is a bike of pool 0 - the capture's own when it is one
    const uint32_t partner = m.PeekWord(p1 + 856u);
    if (partner < 0x80000000u || partner >= 0x80200000u) m.PokeWord(p1 + 856u, p1);
    const uint32_t rec = c.scratch + 256u;
    for (uint32_t k = 0; k < 3u; ++k) m.PokeWord(rec + 4u * k, r.U32());
    m.PokeWord(0x800D6184u, rec);
    if (c.sample) return;
    Put8(m, gs + 4u, AnyType(r));
    WriteU16Bench(m, gs + 8u, static_cast<uint16_t>(r.U32()));
    m.PokeWord(gs + 60u, r.U32() % 3u);                                   // NAMED BOUND: the bank 0..2 (a 3-entry table)
    m.PokeWord(gs + 48u, 1u + r.U32() % 2u);
    m.PokeWord(gs + 16u, r.U32());
    m.PokeWord(0x8005ADE0u, r.U32() % 1000u);
    m.PokeWord(0x8005ADE4u, r.U32() % 1000u);
    m.PokeWord(0x8005B218u, r.U32() % 19u);
    m.PokeWord(s::kLdPedSwitch, r.U32() % 2u);
    m.PokeWord(0x8005B220u, Chance(r, 3) ? 1u : 0u);
    for (uint32_t a : {s::kLdClock0, s::kLdClock1, s::kLdTimeLimit, s::kLdFightPtr}) m.PokeWord(a, r.U32());
    // the riders' +0x23C bits of player 1 and of its partner's rider, and player 2 (0 or a bike of pool 0)
    const uint32_t base = m.PeekWord(0x8005B3A0u);
    const uint32_t p2 = Chance(r, 2) ? 0u : base + 1096u * (1u + r.U32() % 3u);
    m.PokeWord(0x8005B21Cu, p2);
    for (uint32_t b : {p1, m.PeekWord(p1 + 856u), p2}) {
        if (b == 0u) continue;
        const uint32_t rd = m.PeekWord(b + 852u);
        if (rd >= 0x80000000u && rd < 0x80200000u) Put8(m, rd + 572u, r.U32() & 0xFFu);
    }
    if (p2 != 0u) {
        const uint32_t pp = m.PeekWord(p2 + 856u);
        if (pp < 0x80000000u || pp >= 0x80200000u) m.PokeWord(p2 + 856u, p2);
    }
}

// ---------------------------------------------------------------------------- FinishTableInit 0x8006A7C0
void SetupFinish(CaseContext& c, Args&) {
    gridr::InjectOverlay(c);
    if (c.sample) return;
    for (uint32_t k = 0; k < 18u * 16u; k += 4) c.mem->PokeWord(s::kLdFinishTable + k, c.rng->U32());
}

// ---------------------------------------------------------------------------- RaceBlockCopy 0x80068470
void SetupBlock(CaseContext& c, Args& a) {
    gridr::InjectOverlay(c);
    const std::vector<uint8_t>& f = DiscFile("DATA\\STARTDFA.BIN");
    const uint32_t blk = c.scratch;
    if (c.sample && f.size() >= 4u * 292u) {
        c.mem->WriteBlock(blk, f.data() + 3u * 292u, 292u);
    } else {
        for (uint32_t k = 0; k < 292u; k += 4) c.mem->PokeWord(blk + k, c.rng->U32());
        if (!f.empty() && Chance(*c.rng, 2)) c.mem->WriteBlock(blk, f.data() + 292u * (c.rng->U32() % (f.size() / 292u)), 292u);
    }
    a.a[0] = blk;
}

// ---------------------------------------------------------------------------- PieceListInit 0x8006A8FC
void SetupPieces(CaseContext& c, Args&) {
    gridr::InjectOverlay(c);
    if (c.sample) return;
    for (uint32_t k = 0; k < 96u; k += 4) c.mem->PokeWord(s::kLdPieceList + k, c.rng->U32());
    c.mem->PokeWord(s::kLdPiecesUsed, c.rng->U32());
    c.mem->PokeWord(s::kLdPiecesLast, c.rng->U32());
}

// ---------------------------------------------------------------------------- SkyInit 0x800609B0
const uint32_t kSkyKids[] = {s::kLdBzero, 0x8004D9A8u, 0x8004D9E4u, 0x80043DA4u, 0x8004DA44u, 0x80043DB4u};
void SetupSky(CaseContext& c, Args&) {
    gridr::InjectOverlay(c);
    Memory& m = *c.mem;
    for (uint32_t fn : kSkyKids) JrRa(m, fn);
    StubV0(m, s::kLdMalloc, Area(m) + (c.sample ? 0u : 8u * (c.rng->U32() % 512u)));
    if (c.sample) return;
    WriteU16Bench(m, 0x8005B394u, static_cast<uint16_t>(c.rng->U32()));
    WriteU16Bench(m, 0x8005B390u, static_cast<uint16_t>(c.rng->U32()));
    for (uint32_t k = 0; k < 0x3048u; k += 4) m.PokeWord(Area(m) + k, c.rng->U32());
}

// ---------------------------------------------------------------------------- ResTableInit 0x8005D410
void SetupRes(CaseContext& c, Args&) {
    gridr::InjectOverlay(c);
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    const uint32_t res = m.PeekWord(s::kLdResListPtr);
    StubV0(m, s::kLdMalloc, c.sample ? m.PeekWord(res + 2616u) : (Chance(r, 4) ? 0u : r.U32() & 0x801FFFF0u));
    if (c.sample) return;
    m.PokeWord(Gs(m) + 48u, 1u + r.U32() % 2u);
    m.PokeWord(res + 2620u, Chance(r, 2) ? 0u : r.U32());
    if (Chance(r, 3)) m.PokeWord(res + 2616u, r.U32() & 0x801FFFF0u);
    for (uint32_t o = 0; o < 48u; o += 4) m.PokeWord(res + o, r.U32());
    for (uint32_t o = 2348u; o < 2360u; o += 4) m.PokeWord(res + o, r.U32());
    for (uint32_t o = 2624u; o < 2656u; o += 4) m.PokeWord(res + o, r.U32());
}

// ---------------------------------------------------------------------------- ModelTablesInit 0x8005BE40
void SetupTables(CaseContext& c, Args&) {
    gridr::InjectOverlay(c);
    if (c.sample) return;
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    for (uint32_t k = 0; k < 218u; k += 4) m.PokeWord(s::kLdSlotById + k, r.U32());
    for (uint32_t k = 0; k < 112u; k += 4) m.PokeWord(s::kLdFamilies + k, r.U32());
    for (uint32_t k = 0; k < 56u; k += 4) m.PokeWord(s::kLdClassLists + k, r.U32());
    if (Chance(r, 3))
        for (uint32_t k = 0; k < 7u; ++k) Put8(m, s::kLdClassCaps + k, r.U32() % 40u);
}

// ---------------------------------------------------------------------------- the registry's leaves
uint32_t LiveSlot(Memory& m, Rng& r) {
    std::vector<uint32_t> v;
    for (uint32_t k = 0; k < 50u; ++k)
        if (m.PeekWord(s::kLdRegistry + 16u * k) != 0u) v.push_back(k);
    return v.empty() ? 0u : v[r.U32() % v.size()];
}
void SetupFind(CaseContext& c, Args& a) {
    gridr::InjectOverlay(c);
    Memory& m = *c.mem;
    const uint32_t k = LiveSlot(m, *c.rng);
    a.a[0] = (c.sample || Chance(*c.rng, 2)) ? m.PeekWord(s::kLdRegistry + 16u * k) : (Chance(*c.rng, 2) ? 0u : c.rng->U32() % 1000u);
}
void SetupTexKey(CaseContext& c, Args& a) {
    gridr::InjectOverlay(c);
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    const uint32_t k = LiveSlot(m, r);
    a.a[0] = k;
    if (c.sample) return;
    const uint32_t dod = m.PeekWord(m.PeekWord(s::kLdRegistry + 16u * k + 8u));
    const uint32_t pt = m.PeekWord(s::kLdPageTablePtr);
    if (Chance(r, 2)) m.PokeWord(dod + 28u, Chance(r, 2) ? m.PeekByte(pt + 12u * (r.U32() % 34u)) : r.U32() % 300u);
    if (Chance(r, 3)) Put8(m, pt + 12u * (r.U32() % 34u), r.U32());
    Put8(m, s::kLdRegistry + 16u * k + 7u, r.U32());
}
void SetupClassFirst(CaseContext& c, Args& a) {
    gridr::InjectOverlay(c);
    Rng& r = *c.rng;
    a.a[0] = r.U32() % 7u;
    a.a[1] = r.U32() % 50u;
    if (!c.sample && Chance(r, 2)) WriteU16Bench(*c.mem, s::kLdClassLists + 8u * a.a[0] + 2u, Chance(r, 2) ? 0xFFFFu : static_cast<uint16_t>(r.U32()));
}
void SetupFamily(CaseContext& c, Args& a) {
    gridr::InjectOverlay(c);
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    const uint32_t kind = 1u + r.U32() % 6u;
    const uint32_t e = s::kLdFamilies + 16u * kind;
    a.a[0] = kind;
    a.a[1] = m.PeekWord(e + 8u) + r.U32() % std::max<uint32_t>(1u, m.PeekWord(e));
    a.a[2] = r.U32() % 50u;
    if (!c.sample && Chance(r, 2))
        WriteU16Bench(m, m.PeekWord(e + 12u) + 2u * (a.a[1] - m.PeekWord(e + 8u)), Chance(r, 2) ? 0xFFFFu : static_cast<uint16_t>(r.U32() % 50u));
}

// ---------------------------------------------------------------------------- the handlers
// A model's RMD3 / DOD3 / DPD3 chunk from the player's BBLEVEL1.GEO.
struct GeoChunks {
    std::vector<size_t> rmd, dod, dpd, bbd;
};
const GeoChunks& Chunks() {
    static GeoChunks x = [] {
        GeoChunks g;
        const std::vector<uint8_t>& f = DiscFile("DATA\\BBLEVEL1.GEO");
        size_t at = 0;
        auto u32 = [&](size_t o) {
            uint32_t v = 0;
            std::memcpy(&v, f.data() + o, 4);
            return v;
        };
        while (at + 16 <= f.size()) {
            const uint32_t len = u32(at + 4);
            if (len == 0) break;
            if (u32(at) == 0x33444D52u) {
                g.rmd.push_back(at);
                size_t p = at + 16;
                while (p + 16 <= at + len && p + 16 <= f.size()) {
                    const uint32_t t = u32(p), l = u32(p + 4);
                    if (l == 0) break;
                    if (t == 0x33444F44u) g.dod.push_back(p);
                    if (t == 0x33445044u) g.dpd.push_back(p);
                    if (t == 0x33444242u) g.bbd.push_back(p);
                    p += l;
                }
            }
            at += len;
        }
        return g;
    }();
    return x;
}
void PlantChunk(CaseContext& c, size_t at, uint32_t to, uint32_t max) {
    const std::vector<uint8_t>& f = DiscFile("DATA\\BBLEVEL1.GEO");
    uint32_t len = 0;
    if (at + 8 <= f.size()) std::memcpy(&len, f.data() + at + 4, 4);
    if (at < f.size()) c.mem->WriteBlock(to, f.data() + at, std::min<size_t>({len, max, f.size() - at}));
}
void FreeSomeSlots(CaseContext& c) {
    for (uint32_t k = 0; k < 50u; ++k)
        if (Chance(*c.rng, 3)) c.mem->PokeWord(s::kLdRegistry + 16u * k, 0);
}
void SetupRmd(CaseContext& c, Args& a) {
    gridr::InjectOverlay(c);
    Rng& r = *c.rng;
    const GeoChunks& g = Chunks();
    const uint32_t chunk = c.scratch;
    if (!g.rmd.empty()) PlantChunk(c, g.rmd[r.U32() % g.rmd.size()], chunk, 16);
    a.a[0] = c.sample ? 100u : r.U32() % 1000u;
    a.a[1] = chunk;
    if (c.sample) return;
    if (Chance(r, 2)) Put8(*c.mem, chunk + 12u, 1u + r.U32() % 6u);
    if (Chance(r, 8)) {
        for (uint32_t k = 0; k < 50u; ++k)
            if (c.mem->PeekWord(s::kLdRegistry + 16u * k) == 0u) c.mem->PokeWord(s::kLdRegistry + 16u * k, 1u + r.U32() % 900u);
    } else {
        FreeSomeSlots(c);
    }
}
// a registry record made fresh for model `id` with `lods` LOD records (the handler's own work before this one)
void FreshModel(CaseContext& c, uint32_t id, uint32_t lods, uint32_t lodArray) {
    Memory& m = *c.mem;
    const uint32_t k = c.rng->U32() % 50u;
    m.PokeWord(s::kLdRegistry + 16u * k, id);
    Put8(m, s::kLdRegistry + 16u * k + 4u, lods);
    m.PokeWord(s::kLdRegistry + 16u * k + 8u, lodArray);
    for (uint32_t j = 0; j < k; ++j)
        if (m.PeekWord(s::kLdRegistry + 16u * j) == id) m.PokeWord(s::kLdRegistry + 16u * j, id + 1u);
}
void SetupDod(CaseContext& c, Args& a) {
    gridr::InjectOverlay(c);
    Rng& r = *c.rng;
    const GeoChunks& g = Chunks();
    const uint32_t chunk = c.scratch + 64u, lods = c.scratch;
    if (!g.dod.empty()) PlantChunk(c, g.dod[r.U32() % g.dod.size()], chunk, 64);
    const uint32_t id = 100u + r.U32() % 60u;
    const uint32_t n = 1u + r.U32() % 4u;
    for (uint32_t k = 0; k < 12u * n; k += 4) c.mem->PokeWord(lods + k, r.U32());
    if (!Chance(r, 8)) FreshModel(c, id, n, lods);
    a.a[0] = id;
    a.a[1] = chunk;
    a.a[2] = r.U32() % n;
    if (!c.sample && Chance(r, 3)) WriteU16Bench(*c.mem, chunk + 24u, static_cast<uint16_t>(r.U32() % 64u));
    if (!c.sample && Chance(r, 3))
        for (uint32_t o : {32u, 36u, 40u, 44u, 48u}) c.mem->PokeWord(chunk + o, Chance(r, 3) ? 0u : r.U32() % 0x4000u);
}
void SetupDpd(CaseContext& c, Args& a) {
    gridr::InjectOverlay(c);
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    const GeoChunks& g = Chunks();
    const uint32_t chunk = c.scratch + 1024u, lods = c.scratch, parts = c.scratch + 256u;
    if (!g.dpd.empty()) PlantChunk(c, g.dpd[r.U32() % g.dpd.size()], chunk, 64);
    const uint32_t id = 100u + r.U32() % 60u;
    const uint32_t n = 1u + r.U32() % 4u;
    const uint32_t dod = c.scratch + 512u;
    WriteU16Bench(m, dod + 24u, static_cast<uint16_t>(1u + r.U32() % 16u));
    for (uint32_t k = 0; k < n; ++k) {
        m.PokeWord(lods + 12u * k, dod);
        m.PokeWord(lods + 12u * k + 4u, parts);
        m.PokeWord(lods + 12u * k + 8u, 0);
    }
    if (!Chance(r, 8)) FreshModel(c, id, n, lods);
    Put8(m, chunk + 13u, r.U32() % 32u);                                  // NAMED BOUND: a part index < 32 (the part array's room)
    if (Chance(r, 3)) m.PokeWord(chunk + 20u, Chance(r, 3) ? 0u : r.U32() % 0x4000u);
    a.a[0] = id;
    a.a[1] = chunk;
    a.a[2] = r.U32() % n;
    a.a[3] = Chance(r, 2) ? m.PeekByte(dod + 24u) - 1u : r.U32() % 16u;
}
void SetupBbd(CaseContext& c, Args& a) {
    gridr::InjectOverlay(c);
    Rng& r = *c.rng;
    const uint32_t lods = c.scratch;
    const uint32_t id = 100u + r.U32() % 60u;
    const uint32_t n = 1u + r.U32() % 4u;
    for (uint32_t k = 0; k < 12u * n; k += 4) c.mem->PokeWord(lods + k, r.U32());
    if (!Chance(r, 8)) FreshModel(c, id, n, lods);
    a.a[0] = id;
    a.a[1] = r.U32();
    a.a[2] = r.U32() % n;
}

// ---------------------------------------------------------------------------- ChunkWalk 0x8005C0C4
void SetupWalk(CaseContext& c, Args& a) {
    gridr::InjectOverlay(c);
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    const GeoChunks& g = Chunks();
    const std::vector<uint8_t>& f = DiscFile("DATA\\BBLEVEL1.GEO");
    const uint32_t area = Area(m);
    size_t at = g.rmd.empty() ? 0 : g.rmd[c.sample ? 0u : r.U32() % g.rmd.size()];
    uint32_t len = 0;
    if (at + 8 <= f.size()) std::memcpy(&len, f.data() + at + 4, 4);
    len = std::min<uint32_t>(len, kAreaBytes);
    if (at < f.size()) m.WriteBlock(area, f.data() + at, std::min<size_t>(len, f.size() - at));
    FreeSomeSlots(c);
    const uint32_t id = m.PeekWord(area + 8u);
    for (uint32_t k = 0; k < 50u; ++k)                                    // a model the file registers is not yet there
        if (m.PeekWord(s::kLdRegistry + 16u * k) == id) m.PokeWord(s::kLdRegistry + 16u * k, 0);
    if (!c.sample && Chance(r, 6)) m.PokeWord(s::kLdRegistry + 16u * (r.U32() % 50u), id);
    a.a[0] = c.scratch;
    a.a[1] = area;
    a.a[2] = (!c.sample && Chance(r, 8)) ? r.U32() % len : len;
    a.a[3] = Chance(r, 2) ? 1u : 0u;
}

// ---------------------------------------------------------------------------- GeoLoad 0x8005CA10 / CarModels 0x8005C630
const char* const kGeoFiles[] = {"DATA\\CAR01A.GEO", "DATA\\PED01A.GEO", "DATA\\HAZARD5.GEO", "DATA\\CARSC.GEO"};
uint32_t PlantGeo(CaseContext& c, const char* name) {
    Memory& m = *c.mem;
    const uint32_t len = Plant(m, DiscFile(name), Area(m), kAreaBytes);
    for (uint32_t k = 0; k < 50u; ++k)                                    // the file's models are not yet registered
        if (Chance(*c.rng, 2) || m.PeekWord(s::kLdRegistry + 16u * k) >= 300u) m.PokeWord(s::kLdRegistry + 16u * k, 0);
    return len;
}
void GeoStubs(CaseContext& c) {
    JrRa(*c.mem, s::kLdFree);
    // TexFile's TSLP arm (the appearance bytes and CLUT rows, not ported) and the GPU: LoadImage, DrawSync
    for (uint32_t fn : {s::kLdLoadImage, s::kLdDrawSync}) JrRa(*c.mem, fn);
}
void SetupGeo(CaseContext& c, Args& a) {
    gridr::InjectOverlay(c);
    Rng& r = *c.rng;
    GeoStubs(c);
    const uint32_t len = PlantGeo(c, kGeoFiles[c.sample ? 0u : r.U32() % 4u]);
    StubLoadFile(*c.mem, Area(*c.mem), (!c.sample && Chance(r, 10)) ? r.U32() % len : len, !c.sample && Chance(r, 12));
    a.a[0] = (!c.sample && Chance(r, 10)) ? 0u : c.scratch;               // the name (read by the stub only)
    a.a[1] = (!c.sample && Chance(r, 4)) ? c.scratch + 64u : 0u;
}
void SetupCars(CaseContext& c, Args& a) {
    gridr::InjectOverlay(c);
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    GeoStubs(c);
    const uint32_t len = PlantGeo(c, kGeoFiles[c.sample ? 0u : r.U32() % 4u]);
    StubLoadFile(m, Area(m), len, !c.sample && Chance(r, 12));
    a.a[0] = c.sample ? 4u : (Chance(r, 6) ? r.U32() % 200u : 1u + r.U32() % 99u);
    if (c.sample) return;
    Put8(m, Gs(m) + 4u, AnyType(r));
    m.PokeWord(Gs(m) + 48u, 1u + r.U32() % 2u);                           // NAMED BOUND: players 1..2 (the name table's two)
}

// ---------------------------------------------------------------------------- PopulationReset 0x80068D54
void SetupPop(CaseContext& c, Args&) {
    gridr::InjectOverlay(c);
    if (c.sample) return;
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    for (uint32_t k = 0; k < 16u; ++k)
        for (uint32_t o = 0; o < 512u; o += 64) m.PokeWord(0x800CF660u + 512u * k + o, r.U32());
    for (uint32_t o = 0; o < 64u; o += 4) m.PokeWord(0x800D8710u + o, r.U32());
    m.PokeWord(Gs(m) + 48u, 1u + r.U32() % 2u);
    const uint32_t cnt = m.PeekWord(0x8005B214u);
    if (Chance(r, 3)) m.PokeWord(0x8005B214u, r.U32() % (cnt + 1u)); // NAMED BOUND: not past the capture's pool-6 records
    if (Chance(r, 4)) m.PokeWord(0x800D4B80u, 0);
    for (uint32_t a : {0x8005B1FCu, 0x8005B1F8u, 0x8005B218u, 0x8005AD38u, 0x800D1660u, 0x8005AD3Cu, 0x8005B2B8u})
        m.PokeWord(a, r.U32());
}

// ---------------------------------------------------------------------------- EffectSheet 0x80061FAC
struct FxPayload {
    std::vector<uint8_t> bytes;
};
const FxPayload& Fx(int raceId) {
    static std::vector<std::unique_ptr<std::pair<int, FxPayload>>> cache; // entries never move
    for (const auto& e : cache)
        if (e->first == raceId) return e->second;
    FxPayload p;
    const std::vector<uint8_t>& bin = DiscFile("DATA\\GAMEBIN1.DAT");
    try {
        const rr::LevelBundle b = rr::ParseLevelBundle(bin, static_cast<size_t>(raceId - 1));
        if (const rr::LevelBundleSection* s5 = b.Find(5)) {
            const size_t n = std::min<size_t>(s5->limit - s5->payload, kAreaBytes);
            p.bytes.assign(bin.begin() + static_cast<std::ptrdiff_t>(s5->payload),
                           bin.begin() + static_cast<std::ptrdiff_t>(s5->payload + n));
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "rows_loader: GAMEBIN1.DAT race %d: %s\n", raceId, e.what());
    }
    cache.push_back(std::make_unique<std::pair<int, FxPayload>>(raceId, std::move(p)));
    return cache.back()->second;
}
void SetupFx(CaseContext& c, Args& a) {
    gridr::InjectOverlay(c);
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    JrRa(m, s::kLdLoadImage);
    int race = c.sample ? 4 : 1 + static_cast<int>(r.U32() % 40u);
    if (Fx(race).bytes.empty()) race = 4;                                 // NAMED BOUND: a race whose bundle has the section
    Plant(m, Fx(race).bytes, Area(m), kAreaBytes);
    a.a[0] = r.U32();
    a.a[1] = Area(m);
    if (c.sample) return;
    m.PokeWord(Gs(m) + 48u, 1u + r.U32() % 2u);                           // NAMED BOUND: players 1..2 (two config rows)
}

// ---------------------------------------------------------------------------- SoundInit SLUS 0x8001E614
void SetupSoundInit(CaseContext& c, Args& a) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    JrRa(m, 0x8001E738u);
    const uint32_t list = c.scratch;
    uint32_t w[9] = {1, 0x01002001u, 4, 12, 5, 24, 0, 0, 0};
    if (!c.sample) {
        w[3] = 1u + r.U32() % 16u;
        w[5] = r.U32() % 32u;
        if (Chance(r, 4)) std::swap(w[0], w[2]), std::swap(w[1], w[3]);
        if (Chance(r, 6)) w[2] = 9;                                       // no bank count: the default 1
        if (Chance(r, 6)) w[4] = 0;                                       // the list ends before the voices
    }
    for (uint32_t k = 0; k < 9u; ++k) m.PokeWord(list + 4u * k, w[k]);
    m.PokeWord(c.gp + 272u, 0);                                           // NAMED BOUND: no allocator hook (the game sets none)
    a.a[0] = Chance(r, 16) && !c.sample ? 0u : list;
    for (uint32_t o = 0; o < 16u; o += 4) m.PokeWord(0x800D6870u + o, r.U32());
}

// ---------------------------------------------------------------------------- SoundLoad 0x800627F8
const uint32_t kSoundJr[] = {0x8001F054u, 0x8001F080u, 0x8001F0D4u, 0x8001FBD4u, 0x80063448u, 0x8001EFE8u, 0x8001460Cu,
                             s::kLdFree};
void SetupSoundLoad(CaseContext& c, Args&) {
    gridr::InjectOverlay(c);
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    for (uint32_t fn : kSoundJr) JrRa(m, fn);
    StubV0(m, 0x8001458Cu, (!c.sample && Chance(r, 10)) ? 0xFFFFFFFFu : 3u + r.U32() % 4u);
    StubV0(m, 0x80043F00u, r.U32());
    StubV0(m, 0x80062D84u, c.sample ? 1u : r.U32() % 12u);
    StubV0(m, 0x800148BCu, r.U32());
    StubV0(m, s::kLdMalloc, Area(m) + 16u * (r.U32() % 256u));            // NAMED BOUND: the host's blocks (256 KiB asked)
    const std::vector<uint8_t>& f = DiscFile("DATA\\RASHNZ_E.DAT");
    const uint32_t dir = c.scratch + 1024u;
    if (f.size() >= 96u) m.WriteBlock(dir, f.data(), 96u);
    StubRead96(m, 0x80014780u, dir);
    const uint32_t gp = c.gp;
    if (c.sample) return;
    m.PokeWord(Gs(m) + 48u, 1u + r.U32() % 2u);
    m.PokeWord(Gs(m) + 72u, r.U32() % 27u);                               // NAMED BOUND: the bikes 0..26 (the directory's rows)
    m.PokeWord(Gs(m) + 76u, r.U32() % 27u);
    Put8(m, Gs(m) + 4u, AnyType(r));
    for (uint32_t o : {1920u, 1952u, 1924u, 1928u})                       // the blocks made or not yet
        if (Chance(r, 3)) m.PokeWord(gp + o, 0);
}

// ---------------------------------------------------------------------------- SoundSpuAttr SLUS 0x8001E8C4
// NAMED BOUND: libspu's SpuSetCommonAttr reads SPUCNT (the bench has no SPU to read); its stand-in copies the
// attribute record it is handed (40 bytes) to the scratch block, where the RAM diff compares it word for word.
void SetupSpuAttr(CaseContext& c, Args&) {
    Memory& m = *c.mem;
    const uint32_t dst = c.scratch;
    const uint32_t w[11] = {0x3C080000u | (dst >> 16), 0x35080000u | (dst & 0xFFFFu), 0x2409000Au, // li t1,10
                            0x8C8A0000u,  // lw t2,0(a0)
                            0x24840004u,  // addiu a0,a0,4
                            0xAD0A0000u,  // sw t2,0(t0)
                            0x2529FFFFu,  // addiu t1,t1,-1
                            0x1520FFFBu,  // bnez t1,-5
                            0x25080004u,  // addiu t0,t0,4 (delay)
                            0x03E00008u, 0x00000000u};
    for (uint32_t k = 0; k < 11u; ++k) m.PokeWord(s::kLdSpuCommonAttr + 4u * k, w[k]);
    if (c.sample) return;
    for (uint32_t k = 0; k < 64u; k += 4) m.PokeWord(dst + k, c.rng->U32());
}

// ---------------------------------------------------------------------------- TexInit 0x8005D63C
void SetupTexInit(CaseContext& c, Args& a) {
    gridr::InjectOverlay(c);
    a.a[0] = c.sample ? 1u : 1u + c.rng->U32() % 2u;                      // NAMED BOUND: players 1..2
    if (c.sample) return;
    for (uint32_t k = 0; k < 2688u; k += 4) c.mem->PokeWord(0x800D87E8u + k, c.rng->U32());
    for (uint32_t k = 0; k < 48u * 48u; k += 4) c.mem->PokeWord(0x800D9268u + k, c.rng->U32());
}

// ---------------------------------------------------------------------------- TexFile 0x8005C920 / CtkpList 0x8005BDAC
const char* const kTexFiles[] = {"DATA\\CARSC.TEX", "DATA\\HAZARD0.TEX", "DATA\\HAZARD3.TEX", "DATA\\HAZARD5.TEX",
                                 "DATA\\BBLEVEL1.TEX", "DATA\\BBLEVEL2.TEX"};
// the file's chunks up to 64 KiB (whole chunks), planted at the area; its length in `len`
void PlantTex(CaseContext& c, const char* name, uint32_t& len) {
    const std::vector<uint8_t>& f = DiscFile(name);
    uint32_t at = 0;
    while (at + 8u <= f.size()) {
        uint32_t l = 0;
        std::memcpy(&l, f.data() + at + 4, 4);
        if (l == 0 || at + l > kAreaBytes || at + l > f.size()) break;
        at += l;
    }
    len = at;
    if (at) c.mem->WriteBlock(Area(*c.mem), f.data(), at);
}
void SetupTexFile(CaseContext& c, Args& a) {
    gridr::InjectOverlay(c);
    GeoStubs(c);
    Rng& r = *c.rng;
    uint32_t len = 0;
    PlantTex(c, kTexFiles[c.sample ? 0u : r.U32() % 6u], len);
    a.a[0] = Area(*c.mem);
    a.a[1] = (!c.sample && Chance(r, 8)) ? r.U32() % (len + 1u) : len;
}
void SetupCtkp(CaseContext& c, Args& a) {
    gridr::InjectOverlay(c);
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    const uint32_t chunk = c.scratch;
    const uint32_t n = c.sample ? 4u : r.U32() % 12u;
    m.PokeWord(chunk, 0x504B5443u);
    m.PokeWord(chunk + 4u, 12u + 8u * n);
    m.PokeWord(chunk + 8u, n);
    for (uint32_t i = 0; i < n; ++i) {                                    // NAMED BOUND: kinds 0..6 (the lists' seven)
        WriteU16Bench(m, chunk + 12u + 8u * i, static_cast<uint16_t>(r.U32() % 7u));
        WriteU16Bench(m, chunk + 14u + 8u * i, static_cast<uint16_t>(r.U32()));
        m.PokeWord(chunk + 16u + 8u * i, r.U32());
    }
    if (!c.sample)
        for (uint32_t k = 0; k < 7u; ++k) WriteU16Bench(m, s::kLdClassLists + 8u * k, static_cast<uint16_t>(r.U32() % 8u));
    a.a[0] = chunk;
}

// ---------------------------------------------------------------------------- RigLoad 0x8005C30C / LoadBikeBank 0x8005C45C
const char* const kRigFiles[] = {"DATA\\CRUISES1.MRO", "DATA\\CRUISES2.MRO", "DATA\\CRUISES3.MRO", "DATA\\SPORTS1.MRO",
                                 "DATA\\SPORTS2.MRO", "DATA\\SPORTS3.MRO"};
// read(name, mode, buf, &len): copies the words from `src` to a2, *a3 = len, v0 = 0 (or -1)
void StubReadInto(Memory& m, uint32_t src, uint32_t len, bool fail) {
    const uint32_t fn = s::kLdReadInto;
    if (fail) {
        m.PokeWord(fn, 0x03E00008u);
        m.PokeWord(fn + 4u, 0x2402FFFFu);
        return;
    }
    const uint32_t words = std::max<uint32_t>(1u, (len + 3u) / 4u);
    const uint32_t w[15] = {0x3C080000u | (src >> 16), 0x35080000u | (src & 0xFFFFu), 0x3C090000u | (words >> 16),
                            0x35290000u | (words & 0xFFFFu), 0x8D0A0000u, 0x25080004u, 0xACCA0000u, 0x2529FFFFu,
                            0x1520FFFBu, 0x24C60004u, 0x3C0B0000u | (len >> 16), 0x356B0000u | (len & 0xFFFFu),
                            0xACEB0000u, 0x03E00008u, 0x00001021u};
    for (uint32_t k = 0; k < 15u; ++k) m.PokeWord(fn + 4u * k, w[k]);
}
// The rig file planted at the area's second half (the read copies it to the stream buffers' block)
uint32_t PlantRig(CaseContext& c, const char* name) {
    return Plant(*c.mem, DiscFile(name), Area(*c.mem) + 0x8000u, 0x8000u);
}
void ClearBank(CaseContext& c) {                                          // the bank's models not yet registered
    Memory& m = *c.mem;
    for (uint32_t k = 0; k < 50u; ++k) {
        const uint32_t id = m.PeekWord(s::kLdRegistry + 16u * k);
        if ((id >= 100u && id < 200u) || id == 800u || Chance(*c.rng, 4)) m.PokeWord(s::kLdRegistry + 16u * k, 0);
    }
}
void SetupRig(CaseContext& c, Args& a) {
    gridr::InjectOverlay(c);
    GeoStubs(c);
    Rng& r = *c.rng;
    const uint32_t len = PlantRig(c, kRigFiles[c.sample ? 0u : r.U32() % 6u]);
    StubReadInto(*c.mem, Area(*c.mem) + 0x8000u, len, !c.sample && Chance(r, 12));
    // NAMED BOUND: the rig's block (~24 KiB) is the host's (the capture's heap has no such block left): the area's
    // first half, or 0 now and then (the refusal arm)
    StubV0(*c.mem, s::kLdMalloc, (!c.sample && Chance(r, 10)) ? 0u : Area(*c.mem));
    ClearBank(c);
    a.a[0] = c.scratch;
    a.a[1] = c.scratch + 256u;
}
void SetupBank(CaseContext& c, Args& a) {
    gridr::InjectOverlay(c);
    GeoStubs(c);
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    // NAMED BOUND: the bank file (BBLEVEL<n>.GEO, 68 KiB) is planted over the stream records' buffers 24..31
    // (128 KiB, one block in the capture); the rig file at 28's second half, read by the stub into record 0's
    const uint32_t area = m.PeekWord(strows::Rec(m, 24) + 12u);
    const char* files[] = {"DATA\\BBLEVEL1.GEO", "DATA\\BBLEVEL2.GEO", "DATA\\BBLEVEL3.GEO"};
    const uint32_t len = Plant(m, DiscFile(files[c.sample ? 0u : r.U32() % 3u]), area, 0x12000u);
    StubLoadFile(m, area, len, !c.sample && Chance(r, 16));
    const uint32_t rig = Plant(m, DiscFile(kRigFiles[r.U32() % 6u]), area + 0x14000u, 0x8000u);
    StubReadInto(m, area + 0x14000u, rig, false);
    ClearBank(c);
    a.a[0] = c.sample ? 0u : (Chance(r, 6) ? r.U32() % 8u : r.U32() % 3u);
    if (c.sample) return;
    Put8(m, Gs(m) + 4u, Chance(r, 4) ? 44u : AnyType(r));
    m.PokeWord(Gs(m) + 48u, 1u + r.U32() % 2u);
    m.PokeWord(Gs(m) + 72u, r.U32() % 27u);                               // NAMED BOUND: the bikes 0..26
    m.PokeWord(Gs(m) + 76u, r.U32() % 27u);
}

// ---------------------------------------------------------------------------- the texture page table
// LECT / KNBP chunks of the player's .TEX files (the first 64 KiB of each)
struct TexChunk {
    const char* file;
    size_t at;
    uint32_t len;
};
const std::vector<TexChunk>& TexChunks(uint32_t tag) {
    static std::vector<TexChunk> lect, knbp;
    static bool done = false;
    if (!done) {
        done = true;
        for (const char* name : kTexFiles) {
            const std::vector<uint8_t>& f = DiscFile(name);
            size_t at = 0;
            while (at + 16 <= f.size()) {
                uint32_t t = 0, l = 0;
                std::memcpy(&t, f.data() + at, 4);
                std::memcpy(&l, f.data() + at + 4, 4);
                if (l == 0 || at + l > f.size()) break;
                if (l <= 0x8000u && t == 0x5443454Cu) lect.push_back({name, at, l});
                if (l <= 0x8000u && t == 0x50424E4Bu) knbp.push_back({name, at, l});
                at += l;
            }
        }
    }
    return tag == 0x5443454Cu ? lect : knbp;
}
uint32_t PlantChunkOf(CaseContext& c, const TexChunk& t) {
    const std::vector<uint8_t>& f = DiscFile(t.file);
    c.mem->WriteBlock(Area(*c.mem), f.data() + t.at, t.len);
    return Area(*c.mem);
}
void StirPages(CaseContext& c, uint32_t players) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    const uint32_t pages = 0x800D5F70u;
    const int32_t cap = static_cast<int32_t>(m.PeekWord(0x8005AE08u + 4u * (players - 1u)));
    const int32_t cap4 = static_cast<int32_t>(m.PeekWord(0x8005AE10u + 4u * (players - 1u)));
    m.PokeWord(pages + 408u, r.U32() % static_cast<uint32_t>(std::max(1, cap + 1)));
    m.PokeWord(pages + 412u, r.U32() % static_cast<uint32_t>(std::max(1, cap4 + 1)));
    m.PokeWord(pages + 424u, r.U32() % 64u);
    for (uint32_t k = 0; k < 34u; ++k)
        if (Chance(r, 3)) Put8(m, pages + 12u * k, Chance(r, 2) ? 0xFFu : r.U32() & 0xFFu);
}
void GpuStubs(CaseContext& c) {
    for (uint32_t fn : {s::kLdLoadImage, s::kLdDrawSync, s::kLdFree}) JrRa(*c.mem, fn);
}
void SetupLect(CaseContext& c, Args& a) {
    gridr::InjectOverlay(c);
    GpuStubs(c);
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    const std::vector<TexChunk>& v = TexChunks(0x5443454Cu);
    uint32_t chunk = c.scratch;
    if (!v.empty()) chunk = PlantChunkOf(c, v[c.sample ? 0u : r.U32() % v.size()]);
    a.a[0] = chunk;
    if (c.sample) return;
    const uint32_t players = 1u + r.U32() % 2u;                           // NAMED BOUND: players 1..2
    m.PokeWord(Gs(m) + 48u, players);
    StirPages(c, players);
    if (Chance(r, 3)) Put8(m, chunk + 12u, r.U32() % 8u);                 // another sheet kind (0 and 7: none)
    if (Chance(r, 4)) WriteU16Bench(m, chunk + 16u, static_cast<uint16_t>(Chance(r, 2) ? (Chance(r, 2) ? 116u : 149u) : 151u));
}
void SetupTslp(CaseContext& c, Args& a) {
    gridr::InjectOverlay(c);
    GpuStubs(c);
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    const uint32_t chunk = c.scratch;                                     // 20 + 0x1514 bytes of rows
    for (uint32_t k = 0; k < 0x1540u; k += 4) m.PokeWord(chunk + k, r.U32());
    a.a[0] = chunk;
    const uint32_t players = c.sample ? 1u : 1u + r.U32() % 2u;          // NAMED BOUND: players 1..2
    m.PokeWord(Gs(m) + 48u, players);
    m.PokeWord(0x800D5F70u + 424u, r.U32() % 64u);
    for (uint32_t p = 0; p < 2u; ++p) {
        m.PokeWord(Gs(m) + 72u + 4u * p, c.sample ? m.PeekWord(Gs(m) + 72u + 4u * p) : (Chance(r, 8) ? r.U32() : r.U32() % 27u));
        Put8(m, 0x800D81D8u + 36u * p + 10u, r.U32() % 5u);             // NAMED BOUND: appearance 0..4 (the rows)
    }
}
void SetupKnbp(CaseContext& c, Args& a) {
    gridr::InjectOverlay(c);
    GpuStubs(c);
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    const std::vector<TexChunk>& v = TexChunks(0x50424E4Bu);
    uint32_t chunk = c.scratch;
    if (!v.empty()) chunk = PlantChunkOf(c, v[c.sample ? 0u : r.U32() % v.size()]);
    else for (uint32_t k = 0; k < 64u; k += 4) m.PokeWord(chunk + k, r.U32());
    a.a[0] = chunk;
    if (c.sample) return;
    m.PokeWord(Gs(m) + 48u, 1u + r.U32() % 2u);
    if (Chance(r, 3)) WriteU16Bench(m, chunk + 8u, static_cast<uint16_t>(r.U32()));
    if (Chance(r, 3)) WriteU16Bench(m, chunk + 14u, static_cast<uint16_t>(r.U32() % 512u));
}
// the TIM file read 0x8001408C(name, &buf, tim): *a1 = the buffer, tim +12 -> a rect, +16 -> the pixels, v0 0 (or -1)
void StubTimFile(CaseContext& c, bool fail) {
    Memory& m = *c.mem;
    const uint32_t fn = s::kLdTimFile, buf = c.scratch + 1024u, rect = c.scratch + 2048u, pix = c.scratch + 2064u;
    if (fail) {
        m.PokeWord(fn, 0x03E00008u);
        m.PokeWord(fn + 4u, 0x2402FFFFu);
        return;
    }
    const uint32_t w[11] = {0x3C080000u | (buf >> 16), 0x35080000u | (buf & 0xFFFFu), 0xACA80000u,
                            0x3C080000u | (rect >> 16), 0x35080000u | (rect & 0xFFFFu), 0xACC8000Cu,
                            0x3C080000u | (pix >> 16), 0x35080000u | (pix & 0xFFFFu), 0xACC80010u,
                            0x03E00008u, 0x00001021u};
    for (uint32_t k = 0; k < 11u; ++k) m.PokeWord(fn + 4u * k, w[k]);
    for (uint32_t k = 0; k < 8u; k += 2) WriteU16Bench(m, rect + k, static_cast<uint16_t>(c.rng->U32()));
}
void SetupRim(CaseContext& c, Args& a) {
    gridr::InjectOverlay(c);
    GpuStubs(c);
    Rng& r = *c.rng;
    StubTimFile(c, !c.sample && Chance(r, 8));
    a.a[0] = 0x8005B6D0u;
    a.a[1] = c.sample ? 1u : 1u + r.U32() % 2u;
    if (!c.sample) StirPages(c, a.a[1]);
}
void SetupTexTables(CaseContext& c, Args& a) {
    gridr::InjectOverlay(c);
    GpuStubs(c);
    Rng& r = *c.rng;
    StubTimFile(c, !c.sample && Chance(r, 8));
    a.a[0] = c.sample ? 1u : 1u + r.U32() % 2u;
    if (!c.sample) StirPages(c, a.a[0]);
}
void SetupGtp(CaseContext& c, Args& a) {
    gridr::InjectOverlay(c);
    GpuStubs(c);
    Rng& r = *c.rng;
    StubLoadFile(*c.mem, c.scratch + 1024u, 4096u, !c.sample && Chance(r, 8));
    a.a[0] = c.sample ? 1u : 1u + r.U32() % 2u;
}
void SetupTexSetUp(CaseContext& c, Args&) {
    gridr::InjectOverlay(c);
    GpuStubs(c);
    Rng& r = *c.rng;
    StubTimFile(c, !c.sample && Chance(r, 8));
    StubLoadFile(*c.mem, c.scratch + 4096u, 4096u, !c.sample && Chance(r, 8));
    if (c.sample) return;
    const uint32_t players = 1u + r.U32() % 2u;
    c.mem->PokeWord(Gs(*c.mem) + 48u, players);
    StirPages(c, players);
}

Row Make(const char* name, const char* sig, uint32_t entry, std::function<void(CaseContext&, Args&)> setup,
         std::function<uint32_t(NativeEnv&, const Args&)> native, bool v0, std::vector<OracleCallee> callees = {}) {
    Row r{name, sig, entry, std::move(setup), nullptr, v0, std::move(native)};
    r.oracleCallees = std::move(callees);
    r.maxSteps = 40'000'000;
    return r;
}

} // namespace ldr

void AddLoaderRows(std::vector<Row>& rows) {
    namespace s = rr::sim;
    using ldr::Callees;
    using ldr::Make;
    using ldr::Run;
    // EnterRace / RaceReset / SetUpRace
    {
        std::vector<OracleCallee> k;
        for (uint32_t fn : ldr::kEnterKids) k.push_back(OracleCallee{fn, 0});
        rows.push_back(Make("enter_race", "void EnterRace(void)  [RASHCDI 0x80063B90]", s::kLdEnterRaceFn, ldr::SetupEnter,
                            [](NativeEnv& env, const Args&) {
                                return Run(env, "EnterRace", [&](s::GuestRam& g, ldr::Seams& c, uint32_t&) {
                                    return s::EnterRace(g, env.sp, c);
                                });
                            }, false, k));
    }
    {
        std::vector<OracleCallee> k;
        for (const auto& p : ldr::kResetKids) k.push_back(OracleCallee{p.first, p.second});
        k.push_back(OracleCallee{s::kLdMemset, 3});
        rows.push_back(Make("race_reset", "void RaceReset(void)  [RASHCDI 0x80063500]", s::kLdRaceResetFn, ldr::SetupReset,
                            [](NativeEnv& env, const Args&) {
                                return Run(env, "RaceReset", [&](s::GuestRam& g, ldr::Seams& c, uint32_t&) {
                                    return s::RaceReset(g, env.sp, c);
                                });
                            }, false, k));
    }
    {
        std::vector<OracleCallee> k;
        for (const auto& p : ldr::kSetUpKids) k.push_back(OracleCallee{p.first, p.second});
        rows.push_back(Make("set_up_race", "void SetUpRace(void)  [RASHCDI 0x80063670]", s::kLdSetUpRaceFn, ldr::SetupSetUp,
                            [](NativeEnv& env, const Args&) {
                                return Run(env, "SetUpRace", [&](s::GuestRam& g, ldr::Seams& c, uint32_t&) {
                                    return s::SetUpRace(g, env.sp, c);
                                });
                            }, false, k));
    }
    rows.push_back(Make("finish_table_init", "void FinishTableInit(void)  [RASHCDI 0x8006A7C0]", s::kLdFinishInitFn,
                        ldr::SetupFinish, [](NativeEnv& env, const Args&) {
                            return Run(env, "FinishTableInit", [&](s::GuestRam& g, ldr::Seams&, uint32_t&) {
                                s::FinishTableInit(g);
                                return true;
                            });
                        }, false));
    rows.push_back(Make("race_block_copy", "void RaceBlockCopy(u8 *block)  [RASHCDI 0x80068470]", s::kLdBlockCopyFn,
                        ldr::SetupBlock, [](NativeEnv& env, const Args& a) {
                            return Run(env, "RaceBlockCopy", [&](s::GuestRam& g, ldr::Seams& c, uint32_t&) {
                                return s::RaceBlockCopy(g, a.a[0], env.sp, c);
                            });
                        }, false, Callees({{s::kLdMemcpy, 3}})));
    rows.push_back(Make("piece_list_init", "void PieceListInit(void)  [RASHCDI 0x8006A8FC]", s::kLdPieceListFn,
                        ldr::SetupPieces, [](NativeEnv& env, const Args&) {
                            return Run(env, "PieceListInit", [&](s::GuestRam& g, ldr::Seams&, uint32_t&) {
                                s::PieceListInit(g);
                                return true;
                            });
                        }, false));
    {
        std::vector<OracleCallee> k = Callees({{s::kLdMalloc, 2}, {s::kLdMemset, 3}});
        const int ar[] = {2, 1, 1, 0, 1, 0};
        for (size_t i = 0; i < 6; ++i) k.push_back(OracleCallee{ldr::kSkyKids[i], ar[i]});
        rows.push_back(Make("sky_init", "void SkyInit(void)  [RASHCDI 0x800609B0]", s::kLdSkyInitFn, ldr::SetupSky,
                            [](NativeEnv& env, const Args&) {
                                return Run(env, "SkyInit", [&](s::GuestRam& g, ldr::Seams& c, uint32_t&) {
                                    return s::SkyInit(g, env.sp, c);
                                });
                            }, false, k));
    }
    rows.push_back(Make("res_table_init", "void ResTableInit(void)  [RASHCDI 0x8005D410]", s::kLdResTableFn, ldr::SetupRes,
                        [](NativeEnv& env, const Args&) {
                            return Run(env, "ResTableInit", [&](s::GuestRam& g, ldr::Seams& c, uint32_t&) {
                                return s::ResTableInit(g, env.sp, c);
                            });
                        }, false, Callees({{s::kLdMalloc, 2}})));
    rows.push_back(Make("tex_init", "void TexInit(s32 players)  [RASHCDI 0x8005D63C]", 0x8005D63Cu, ldr::SetupTexInit,
                        [](NativeEnv& env, const Args& a) {
                            return Run(env, "TexInit", [&](s::GuestRam& g, ldr::Seams&, uint32_t&) {
                                s::TexInit(g, static_cast<int32_t>(a.a[0]));
                                return true;
                            });
                        }, false));
    rows.push_back(Make("model_tables_init", "void ModelTablesInit(void)  [RASHCDI 0x8005BE40]", s::kLdModelTablesFn,
                        ldr::SetupTables, [](NativeEnv& env, const Args&) {
                            return Run(env, "ModelTablesInit", [&](s::GuestRam& g, ldr::Seams& c, uint32_t&) {
                                return s::ModelTablesInit(g, env.sp, c);
                            });
                        }, false, Callees({{s::kLdMemset, 3}})));
    rows.push_back(Make("registry_find", "s32 RegistryFind(u32 id)  [RASHCDI 0x8005C010]", s::kLdRegistryFindFn,
                        ldr::SetupFind, [](NativeEnv& env, const Args& a) {
                            return Run(env, "RegistryFind", [&](s::GuestRam& g, ldr::Seams&, uint32_t& v0) {
                                v0 = static_cast<uint32_t>(s::RegistryFind(g, a.a[0]));
                                return true;
                            });
                        }, true));
    rows.push_back(Make("tex_key", "void TexKey(s32 slot)  [RASHCDI 0x8005C054]", s::kLdTexKeyFn, ldr::SetupTexKey,
                        [](NativeEnv& env, const Args& a) {
                            return Run(env, "TexKey", [&](s::GuestRam& g, ldr::Seams&, uint32_t&) {
                                s::TexKey(g, a.a[0]);
                                return true;
                            });
                        }, false));
    rows.push_back(Make("class_first", "void ClassFirst(u32 kind, u16 slot)  [RASHCDI 0x8005BD80]", s::kLdClassFirstFn,
                        ldr::SetupClassFirst, [](NativeEnv& env, const Args& a) {
                            return Run(env, "ClassFirst", [&](s::GuestRam& g, ldr::Seams&, uint32_t&) {
                                s::ClassFirst(g, a.a[0], a.a[1]);
                                return true;
                            });
                        }, false));
    rows.push_back(Make("family_register", "void FamilyRegister(u32 kind, u32 id, u16 slot)  [SLUS 0x800303BC]",
                        s::kLdFamilyRegisterFn, ldr::SetupFamily, [](NativeEnv& env, const Args& a) {
                            return Run(env, "FamilyRegister", [&](s::GuestRam& g, ldr::Seams&, uint32_t&) {
                                s::FamilyRegister(g, a.a[0], a.a[1], a.a[2]);
                                return true;
                            });
                        }, false));
    rows.push_back(Make("rmd_handler", "s32 RmdHandler(u32 id, u8 *chunk)  [RASHCDI 0x8005CB9C]", s::kLdRmdFn, ldr::SetupRmd,
                        [](NativeEnv& env, const Args& a) {
                            return Run(env, "RmdHandler", [&](s::GuestRam& g, ldr::Seams& c, uint32_t& v0) {
                                int32_t r = 0;
                                const bool ok = s::RmdHandler(g, a.a[0], a.a[1], env.sp, c, r);
                                v0 = static_cast<uint32_t>(r);
                                return ok;
                            });
                        }, true, Callees({{s::kLdMalloc, 2}, {s::kLdMemset, 3}})));
    rows.push_back(Make("dod_handler", "s32 DodHandler(u32 id, u8 *chunk, s32 lod)  [RASHCDI 0x8005CC4C]", s::kLdDodFn,
                        ldr::SetupDod, [](NativeEnv& env, const Args& a) {
                            return Run(env, "DodHandler", [&](s::GuestRam& g, ldr::Seams& c, uint32_t& v0) {
                                int32_t r = 0;
                                const bool ok = s::DodHandler(g, a.a[0], a.a[1], static_cast<int32_t>(a.a[2]), env.sp, c, r);
                                v0 = static_cast<uint32_t>(r);
                                return ok;
                            });
                        }, true, Callees({{s::kLdMalloc, 2}})));
    rows.push_back(Make("dpd_handler", "s32 DpdHandler(u32 id, u8 *chunk, s32 lod, s32 part)  [RASHCDI 0x8005CD60]",
                        s::kLdDpdFn, ldr::SetupDpd, [](NativeEnv& env, const Args& a) {
                            return Run(env, "DpdHandler", [&](s::GuestRam& g, ldr::Seams&, uint32_t& v0) {
                                v0 = static_cast<uint32_t>(s::DpdHandler(g, a.a[0], a.a[1], static_cast<int32_t>(a.a[2]),
                                                                         static_cast<int32_t>(a.a[3])));
                                return true;
                            });
                        }, true));
    rows.push_back(Make("bbd_handler", "s32 BbdHandler(u32 id, u8 *chunk, s32 lod)  [RASHCDI 0x8005CE78]", s::kLdBbdFn,
                        ldr::SetupBbd, [](NativeEnv& env, const Args& a) {
                            return Run(env, "BbdHandler", [&](s::GuestRam& g, ldr::Seams&, uint32_t& v0) {
                                v0 = static_cast<uint32_t>(s::BbdHandler(g, a.a[0], a.a[1], static_cast<int32_t>(a.a[2])));
                                return true;
                            });
                        }, true));
    rows.push_back(Make("chunk_walk", "s32 ChunkWalk(u32 *out, u8 *buf, s32 size, s32 first)  [RASHCDI 0x8005C0C4]",
                        s::kLdChunkWalkFn, ldr::SetupWalk, [](NativeEnv& env, const Args& a) {
                            return Run(env, "ChunkWalk", [&](s::GuestRam& g, ldr::Seams& c, uint32_t& v0) {
                                int32_t r = 0;
                                const bool ok = s::ChunkWalk(g, a.a[0], a.a[1], static_cast<int32_t>(a.a[2]), a.a[3], env.sp, c, r);
                                v0 = static_cast<uint32_t>(r);
                                return ok;
                            });
                        }, true, Callees({{s::kLdMalloc, 2}, {s::kLdMemset, 3}})));
    rows.push_back(Make("geo_load", "s32 GeoLoad(char *geo, char *tex)  [RASHCDI 0x8005CA10]", s::kLdGeoLoadFn,
                        ldr::SetupGeo, [](NativeEnv& env, const Args& a) {
                            return Run(env, "GeoLoad", [&](s::GuestRam& g, ldr::Seams& c, uint32_t& v0) {
                                int32_t r = 0;
                                const bool ok = s::GeoLoad(g, a.a[0], a.a[1], env.sp, c, r);
                                v0 = static_cast<uint32_t>(r);
                                return ok;
                            });
                        }, true,
                        [] {
                            std::vector<OracleCallee> k = Callees({{s::kLdMalloc, 2}, {s::kLdMemset, 3}, {s::kLdFree, 1},
                                                                   {s::kLdLoadImage, 2}, {s::kLdDrawSync, 1}, {s::kLdReadTim, 2}});
                            k.push_back(OracleCallee{s::kLdLoadFile, 4, 1});
                            return k;
                        }()));
    rows.push_back(Make("car_models", "s32 CarModels(s32 race)  [RASHCDI 0x8005C630]", s::kLdCarModelsFn, ldr::SetupCars,
                        [](NativeEnv& env, const Args& a) {
                            return Run(env, "CarModels", [&](s::GuestRam& g, ldr::Seams& c, uint32_t& v0) {
                                int32_t r = 0;
                                const bool ok = s::CarModels(g, static_cast<int32_t>(a.a[0]), env.sp, c, r);
                                v0 = static_cast<uint32_t>(r);
                                return ok;
                            });
                        }, true,
                        [] {
                            std::vector<OracleCallee> k = Callees({{s::kLdMalloc, 2}, {s::kLdMemset, 3}, {s::kLdFree, 1},
                                                                   {s::kLdLoadImage, 2}, {s::kLdDrawSync, 1}, {s::kLdReadTim, 2}});
                            k.push_back(OracleCallee{s::kLdLoadFile, 4, 1});
                            k.push_back(OracleCallee{s::kLdSprintf, 4, 3});
                            return k;
                        }()));
    rows.back().allowBiosCalls = true; // sprintf's strlen / strcpy go through the BIOS vectors (the kernel runs them)
    rows.push_back(Make("population_reset", "void PopulationReset(void)  [RASHCDI 0x80068D54]", s::kLdPopResetFn,
                        ldr::SetupPop, [](NativeEnv& env, const Args&) {
                            return Run(env, "PopulationReset", [&](s::GuestRam& g, ldr::Seams& c, uint32_t&) {
                                return s::PopulationReset(g, env.sp, c);
                            });
                        }, false, Callees({{s::kLdMemset, 3}})));
    rows.push_back(Make("effect_sheet", "void EffectSheet(u32 a0, u32 *table)  [RASHCDI 0x80061FAC]", s::kLdEffectSheetFn,
                        ldr::SetupFx, [](NativeEnv& env, const Args& a) {
                            return Run(env, "EffectSheet", [&](s::GuestRam& g, ldr::Seams& c, uint32_t&) {
                                return s::EffectSheet(g, a.a[0], a.a[1], env.sp, c);
                            });
                        }, false, Callees({{s::kLdReadTim, 2}, {s::kLdLoadImage, 2}})));
    rows.push_back(Make("tex_file", "void TexFile(u8 *buf, s32 size)  [RASHCDI 0x8005C920]", s::kLdTexFile,
                        ldr::SetupTexFile, [](NativeEnv& env, const Args& a) {
                            return Run(env, "TexFile", [&](s::GuestRam& g, ldr::Seams& c, uint32_t&) {
                                return s::TexFile(g, a.a[0], static_cast<int32_t>(a.a[1]), env.sp, c);
                            });
                        }, false, Callees({{s::kLdLoadImage, 2}, {s::kLdDrawSync, 1}, {s::kLdReadTim, 2}})));
    rows.push_back(Make("ctkp_list", "void CtkpList(u8 *chunk)  [RASHCDI 0x8005BDAC]", s::kLdCtkpFn, ldr::SetupCtkp,
                        [](NativeEnv& env, const Args& a) {
                            return Run(env, "CtkpList", [&](s::GuestRam& g, ldr::Seams&, uint32_t&) {
                                s::CtkpList(g, a.a[0]);
                                return true;
                            });
                        }, false));
    rows.push_back(Make("rig_load", "s32 RigLoad(char *name, s32 *len)  [RASHCDI 0x8005C30C]", s::kLdRigLoadFn,
                        ldr::SetupRig, [](NativeEnv& env, const Args& a) {
                            return Run(env, "RigLoad", [&](s::GuestRam& g, ldr::Seams& c, uint32_t& v0) {
                                int32_t r = 0;
                                const bool ok = s::RigLoad(g, a.a[0], a.a[1], env.sp, c, r);
                                v0 = static_cast<uint32_t>(r);
                                return ok;
                            });
                        }, true,
                        Callees({{s::kLdReadInto, 4}, {s::kLdLoadImage, 2}, {s::kLdDrawSync, 1}, {s::kLdReadTim, 2}, {s::kLdMalloc, 2}, {s::kLdMemcpy, 3}, {s::kLdMemset, 3}})));
    rows.push_back(Make("load_bike_bank", "s32 LoadBikeBank(s32 bank)  [RASHCDI 0x8005C45C]", s::kLdBikeBankFn,
                        ldr::SetupBank, [](NativeEnv& env, const Args& a) {
                            return Run(env, "LoadBikeBank", [&](s::GuestRam& g, ldr::Seams& c, uint32_t& v0) {
                                int32_t r = 0;
                                const bool ok = s::LoadBikeBank(g, static_cast<int32_t>(a.a[0]), env.sp, c, r);
                                v0 = static_cast<uint32_t>(r);
                                return ok;
                            });
                        }, true,
                        [] {
                            std::vector<OracleCallee> k = Callees({{s::kLdMalloc, 2}, {s::kLdMemset, 3}, {s::kLdFree, 1},
                                                                   {s::kLdLoadImage, 2}, {s::kLdDrawSync, 1}, {s::kLdReadTim, 2},
                                                                   {s::kLdReadInto, 4}, {s::kLdMemcpy, 3}});
                            k.push_back(OracleCallee{s::kLdLoadFile, 4, 1});
                            k.push_back(OracleCallee{s::kLdSprintf, 4, 1});
                            return k;
                        }()));
    rows.back().allowBiosCalls = true;
    rows.push_back(Make("hazard_models", "s32 HazardModels(s32 set)  [RASHCDI 0x8005C7F0]", s::kLdHazardModelsFn,
                        [](CaseContext& c, Args& a) {
                            gridr::InjectOverlay(c);
                            ldr::GeoStubs(c);
                            Memory& m = *c.mem;
                            Rng& r = *c.rng;
                            const char* files[] = {"DATA\\HAZARD0.GEO", "DATA\\HAZARD3.GEO", "DATA\\HAZARD5.GEO"};
                            const uint32_t len = ldr::PlantGeo(c, files[c.sample ? 2u : r.U32() % 3u]);
                            ldr::StubLoadFile(m, ldr::Area(m), len, !c.sample && pop::Chance(r, 12));
                            a.a[0] = c.sample ? 5u : (pop::Chance(r, 6) ? r.U32() : r.U32() % 12u);
                            if (!c.sample) pop::Put8(m, ldr::Gs(m) + 4u, ldr::AnyType(r));
                        },
                        [](NativeEnv& env, const Args& a) {
                            return Run(env, "HazardModels", [&](s::GuestRam& g, ldr::Seams& c, uint32_t& v0) {
                                int32_t r = 0;
                                const bool ok = s::HazardModels(g, static_cast<int32_t>(a.a[0]), env.sp, c, r);
                                v0 = static_cast<uint32_t>(r);
                                return ok;
                            });
                        }, true,
                        [] {
                            std::vector<OracleCallee> k = Callees({{s::kLdMalloc, 2}, {s::kLdMemset, 3}, {s::kLdFree, 1},
                                                                   {s::kLdLoadImage, 2}, {s::kLdDrawSync, 1}, {s::kLdReadTim, 2}});
                            k.push_back(OracleCallee{s::kLdLoadFile, 4, 1});
                            k.push_back(OracleCallee{s::kLdSprintf, 4, 2});
                            return k;
                        }()));
    rows.back().allowBiosCalls = true;
    rows.push_back(Make("render_cam_init", "void RenderCamInit(s32 view)  [RASHCDI 0x8005CEE0]", s::kLdRenderCamInitFn,
                        [](CaseContext& c, Args& a) {
                            gridr::InjectOverlay(c);
                            Rng& r = *c.rng;
                            a.a[0] = c.sample ? 0u : r.U32() % 2u;                // NAMED BOUND: views 0 / 1
                            if (c.sample) return;
                            for (uint32_t k = 0; k < 256u; k += 4) c.mem->PokeWord(0x800D82B0u + k, r.U32());
                            c.mem->PokeWord(0x8005AEC0u, r.U32());
                            c.mem->PokeWord(0x8005AEC4u, r.U32());
                        },
                        [](NativeEnv& env, const Args& a) {
                            return Run(env, "RenderCamInit", [&](s::GuestRam& g, ldr::Seams& c, uint32_t&) {
                                return s::RenderCamInit(g, a.a[0], env.sp, c);
                            });
                        }, false, Callees({{s::kLdSetGeomScreen, 1}, {s::kLdSetGeomOffset, 2}})));
    rows.push_back(Make("bike_name", "char *BikeName(s32 table, s32 idx)  [RASHCDI 0x80064034]", s::kLdBikeName,
                        [](CaseContext& c, Args& a) {
                            gridr::InjectOverlay(c);
                            a.a[0] = c.sample ? 0u : (pop::Chance(*c.rng, 5) ? c.rng->U32() : c.rng->U32() % 3u);
                            a.a[1] = c.sample ? 7u : (pop::Chance(*c.rng, 5) ? c.rng->U32() : c.rng->U32() % 27u);
                        },
                        [](NativeEnv& env, const Args& a) {
                            return Run(env, "BikeName", [&](s::GuestRam&, ldr::Seams&, uint32_t& v0) {
                                v0 = s::BikeName(a.a[0], a.a[1]);
                                return true;
                            });
                        }, true));
    {
        const auto gpu = [] { return Callees({{s::kLdLoadImage, 2}, {s::kLdDrawSync, 1}, {s::kLdReadTim, 2}}); };
        rows.push_back(Make("lect_sheet", "void LectSheet(u8 *lect)  [RASHCDI 0x8005DDB8]", s::kLdLect, ldr::SetupLect,
                            [](NativeEnv& env, const Args& a) {
                                return Run(env, "LectSheet", [&](s::GuestRam& g, ldr::Seams& c, uint32_t&) {
                                    return s::LectSheet(g, a.a[0], env.sp, c);
                                });
                            }, false, gpu()));
        rows.push_back(Make("tslp_cluts", "s32 TslpCluts(u8 *tslp)  [RASHCDI 0x8005DBB8]", s::kLdTslp, ldr::SetupTslp,
                            [](NativeEnv& env, const Args& a) {
                                return Run(env, "TslpCluts", [&](s::GuestRam& g, ldr::Seams& c, uint32_t&) {
                                    return s::TslpCluts(g, a.a[0], env.sp, c);
                                });
                            }, false, gpu()));
        rows.push_back(Make("knbp_clut", "s32 KnbpClut(u8 *knbp)  [RASHCDI 0x8005DA38]", s::kLdKnbp, ldr::SetupKnbp,
                            [](NativeEnv& env, const Args& a) {
                                return Run(env, "KnbpClut", [&](s::GuestRam& g, ldr::Seams& c, uint32_t&) {
                                    return s::KnbpClut(g, a.a[0], env.sp, c);
                                });
                            }, false, gpu()));
        const auto tim = [] {
            std::vector<OracleCallee> k = Callees({{s::kLdLoadImage, 2}, {s::kLdDrawSync, 1}, {s::kLdFree, 1}, {s::kLdTimFile, 3}});
            k.push_back(OracleCallee{s::kLdLoadFile, 4, 1});
            return k;
        };
        rows.push_back(Make("rim_tim", "s32 RimTim(char *name, s32 players)  [RASHCDI 0x8005E6CC]", s::kLdRimTimFn,
                            ldr::SetupRim, [](NativeEnv& env, const Args& a) {
                                return Run(env, "RimTim", [&](s::GuestRam& g, ldr::Seams& c, uint32_t& v0) {
                                    int32_t r = 0;
                                    const bool ok = s::RimTim(g, a.a[0], a.a[1], env.sp, c, r);
                                    v0 = static_cast<uint32_t>(r);
                                    return ok;
                                });
                            }, true, tim()));
        rows.push_back(Make("tex_tables_init", "void TexTablesInit(s32 players)  [RASHCDI 0x8005E848]", s::kLdTexTablesFn,
                            ldr::SetupTexTables, [](NativeEnv& env, const Args& a) {
                                return Run(env, "TexTablesInit", [&](s::GuestRam& g, ldr::Seams& c, uint32_t&) {
                                    return s::TexTablesInit(g, a.a[0], env.sp, c);
                                });
                            }, false, tim()));
        rows.push_back(Make("gtp_load", "s32 GtpLoad(s32 players)  [RASHCDI 0x8005E4E8]", s::kLdGtpFn, ldr::SetupGtp,
                            [](NativeEnv& env, const Args& a) {
                                return Run(env, "GtpLoad", [&](s::GuestRam& g, ldr::Seams& c, uint32_t& v0) {
                                    int32_t r = 0;
                                    const bool ok = s::GtpLoad(g, a.a[0], env.sp, c, r);
                                    v0 = static_cast<uint32_t>(r);
                                    return ok;
                                });
                            }, true, tim()));
        rows.push_back(Make("tex_set_up", "void TexSetUp(void)  [RASHCDI 0x8005D9EC]", s::kLdTexSetUpFn, ldr::SetupTexSetUp,
                            [](NativeEnv& env, const Args&) {
                                return Run(env, "TexSetUp", [&](s::GuestRam& g, ldr::Seams& c, uint32_t&) {
                                    return s::TexSetUp(g, env.sp, c);
                                });
                            }, false, tim()));
    }
    rows.push_back(Make("sound_init", "s32 SoundInit(u32 *config)  [SLUS 0x8001E614]", s::kLdSoundInitFn,
                        ldr::SetupSoundInit, [](NativeEnv& env, const Args& a) {
                            return Run(env, "SoundInit", [&](s::GuestRam& g, ldr::Seams& c, uint32_t& v0) {
                                int32_t r = 0;
                                const bool ok = s::SoundInit(g, a.a[0], env.sp, c, r);
                                v0 = static_cast<uint32_t>(r);
                                return ok;
                            });
                        }, true, Callees({{s::kLdMalloc, 2}, {0x8001E738u, 0}})));
    {
        std::vector<OracleCallee> k = Callees({{0x8001F054u, 2}, {0x8001F080u, 0}, {0x8001F0D4u, 0}, {0x8001FBD4u, 0},
                                               {s::kLdMalloc, 2}, {s::kLdMemset, 3}, {0x80063448u, 1}, {0x8001458Cu, 2},
                                               {0x80014780u, 4}, {0x80043F00u, 1}, {0x80062D84u, 4}, {0x800148BCu, 0},
                                               {s::kLdFree, 1}, {0x8001EFE8u, 1}, {0x8001460Cu, 1}});
        Row r = Make("sound_load", "void SoundLoad(void)  [RASHCDI 0x800627F8]", s::kLdSoundLoadFn, ldr::SetupSoundLoad,
                     [](NativeEnv& env, const Args&) {
                         return Run(env, "SoundLoad", [&](s::GuestRam& g, ldr::Seams& c, uint32_t&) {
                             return s::SoundLoad(g, env.sp, c);
                         });
                     }, false, k);
        rows.push_back(std::move(r));
    }
    {
        Row r = Make("sound_spu_attr", "void SoundSpuAttr(void)  [SLUS 0x8001E8C4]", s::kLdSpuAttrFn, ldr::SetupSpuAttr,
                     [](NativeEnv& env, const Args&) {
                         return Run(env, "SoundSpuAttr", [&](s::GuestRam& g, ldr::Seams& c, uint32_t&) {
                             return s::SoundSpuAttr(g, env.sp, c);
                         });
                     }, false, Callees({{s::kLdSpuCommonAttr, 1}}));
        rows.push_back(std::move(r));
    }
}
