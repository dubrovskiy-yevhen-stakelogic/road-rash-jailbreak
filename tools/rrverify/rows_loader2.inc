// Bench rows of BuildRace and the rest of SetUpRace's children (src\game\sim\loader2.{h,cpp}),
// RASHCDI SHA-1 9a8b79d8739713c12b0a2dc4d75ef8f0a2cc0b06 and SLUS_010.53. Included
// LAST by verify_physics.cpp inside its anonymous namespace (rows_loader.inc's ldr:: is visible).
//
// The machine is a race capture (rr-race by default); every case lays the WHOLE RASHCDI image over 0x8005B5E8 first
// (gridr::InjectOverlay), as the console has it while the race loads.
//
// NAMED BOUNDS (the same on both machines):
//   * an orchestrator's children with frames of their own are stubbed (`lui v0 / jr ra / ori v0`): each is ported
//     and benched on its own row (here or in rows_loader.inc / rows_grid.inc / rows_hazards.inc), or loads the race
//     off the CD (the route parser 0x8006A0C8 and 0x8006ABC8, the stream's files, the album);
//   * the CD layer is the host's: LoadFile SLUS 0x80014654 is a stub that hands back a planted copy of the player's own
//     file (work\disc_us) with v0 = its length (0x80014B40's return, as the original) or -1; the open / size / read /
//     close stubs return a handle / a size / a count and read nothing; Free a `jr ra`;
//   * a malloc whose block the function fills returns a block of the planted area (or 0, the allocator's refusal);
//   * the BIOS string calls (strcpy / strcat / strrchr / printf) run the kernel's own code (allowBiosCalls).
namespace ldr2 {

namespace s = rr::sim;
using ldr::Area;
using ldr::DiscFile;
using ldr::Gs;
using ldr::JrRa;
using ldr::Plant;
using ldr::StubV0;
using pop::Chance;
using pop::Put8;

// LoadFile(name, mode, &buf, &len, 0): *a2 = buf, *a3 = len, v0 = len (or v0 = -1, nothing stored).
void StubLoadFileLen(Memory& m, uint32_t buf, uint32_t len, bool fail) {
    const uint32_t fn = s::kLdLoadFile;
    if (fail) {
        m.PokeWord(fn, 0x03E00008u);
        m.PokeWord(fn + 4u, 0x2402FFFFu);
        return;
    }
    const uint32_t w[8] = {0x3C080000u | (buf >> 16), 0x35080000u | (buf & 0xFFFFu), 0xACC80000u,
                           0x3C080000u | (len >> 16), 0x35080000u | (len & 0xFFFFu), 0xACE80000u,
                           0x03E00008u, 0x01001021u}; // move v0,t0 (the length)
    for (uint32_t k = 0; k < 8u; ++k) m.PokeWord(fn + 4u * k, w[k]);
}

// A stub that records its call: the arguments it really takes (`arity` of a0..a3) at dst, a marker 1 at dst+16, v0 0 -
// so that the bench's RAM diff sees the call of a function whose only effect is its calls.
void StubRecord(Memory& m, uint32_t fn, uint32_t dst, int arity) {
    std::vector<uint32_t> w = {0x3C080000u | (dst >> 16), 0x35080000u | (dst & 0xFFFFu), 0x24090001u, 0xAD090010u}; // li t1,1; sw t1,16(t0)
    for (int k = 0; k < arity && k < 4; ++k) w.push_back(0xAD000000u | (static_cast<uint32_t>(4 + k) << 16) | (4u * static_cast<uint32_t>(k)));
    w.push_back(0x03E00008u);
    w.push_back(0x00001021u);
    for (size_t k = 0; k < w.size(); ++k) m.PokeWord(fn + 4u * static_cast<uint32_t>(k), w[k]);
    for (uint32_t k = 0; k < 20u; k += 4) m.PokeWord(dst + k, 0xA5A5A5A5u);
}

void PutString(Memory& m, uint32_t at, const char* str) {
    size_t k = 0;
    for (; str[k] != 0; ++k) Put8(m, at + static_cast<uint32_t>(k), static_cast<uint8_t>(str[k]));
    Put8(m, at + static_cast<uint32_t>(k), 0);
}

constexpr uint32_t kTypes[] = {0u, 1u, 8u, 0x10u, 0x11u, 0x20u, 0x21u, 34u, 36u, 44u, 5u, 0x18u};
uint32_t AnyType(Rng& r) { return Chance(r, 8) ? r.U32() & 0xFFu : kTypes[r.U32() % (sizeof(kTypes) / sizeof(kTypes[0]))]; }

// ---------------------------------------------------------------------------- BuildRace 0x8006982C
const std::pair<uint32_t, int> kBuildKids[] = {{s::kL2WorldPoolsFn, 0}, {s::kL2PoolTableFn, 0}, {s::kL2BuildGrid, 2},
                                               {s::kL2ResetBikes, 0},  {s::kL2CameraSetUp, 0},  {s::kL2TrialLimitsFn, 0},
                                               {s::kL2PopBlockFn, 2},  {s::kL2SpeedClassFn, 2}, {s::kL2EscapeLoadFn, 0}};
void SetupBuildRace(CaseContext& c, Args&) {
    gridr::InjectOverlay(c);
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    for (const auto& k : kBuildKids) StubV0(m, k.first, 0);
    StubV0(m, s::kL2GetRCnt, c.sample ? 0x1234u : r.U32());
    JrRa(m, s::kLdFree);
    const bool b = !c.sample && Chance(r, 3);
    const std::vector<uint8_t>& f = DiscFile(b ? "DATA\\STARTDFB.BIN" : "DATA\\STARTDFA.BIN");
    const uint32_t len = Plant(m, f, Area(m), ldr::kAreaBytes);
    StubLoadFileLen(m, Area(m), len, !c.sample && Chance(r, 10));
    PutString(m, s::kL2GridName, "DATA\\STARTDFA.BIN");
    const uint32_t gs = Gs(m);
    if (c.sample) {
        m.PokeWord(gs + 64u, 4);
        return;
    }
    Put8(m, gs + 4u, AnyType(r));
    m.PokeWord(gs + 48u, 1u + r.U32() % 2u);
    const uint32_t blocks = len / 292u;
    m.PokeWord(gs + 64u, 1u + r.U32() % (blocks == 0u ? 1u : blocks));  // NAMED BOUND: a block of the planted file
    WriteU16Bench(m, 0x80053176u, static_cast<uint16_t>(r.U32()));
    WriteU16Bench(m, 0x8005317Au, static_cast<uint16_t>(r.U32()));
    for (uint32_t a : {0x8005B2A8u, 0x8005B2A4u, 0x8005B2E0u, 0x8005B290u, 0x8005B2ECu, 0x8005B2FCu, 0x8005B210u,
                       0x8005B318u, 0x8005B2A0u, 0x8005B30Cu, 0x8005B36Cu, 0x8005B368u, 0x8005B284u, 0x8005B358u,
                       0x8005AD34u, 0x8005B2E8u, 0x8005ACC0u})
        m.PokeWord(a, r.U32());
}

// ---------------------------------------------------------------------------- WorldPoolsInit 0x80069000
void SetupWorldPools(CaseContext& c, Args&) {
    gridr::InjectOverlay(c);
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    StubV0(m, s::kLdMalloc, (!c.sample && Chance(r, 6)) ? 0u : Area(m) + 8u * (r.U32() % 256u));
    if (c.sample) return;
    Put8(m, Gs(m) + 4u, AnyType(r));
    m.PokeWord(Gs(m) + 48u, r.U32() % 3u);                                 // NAMED BOUND: players 0..2 (two lists)
    m.PokeWord(s::kLdPedSwitch, Chance(r, 2) ? 0u : r.U32());
    for (uint32_t k = 0; k < 64u; k += 4) m.PokeWord(0x800D5CF8u + k, r.U32());
    for (uint32_t k = 0; k < 32u; k += 4) m.PokeWord(0x800CD6A8u + k, r.U32());
    for (uint32_t k = 0; k < 20u; k += 4) m.PokeWord(0x800D4B70u + k, r.U32());
}

// ---------------------------------------------------------------------------- PoolTableInit 0x80068AA4
void SetupPoolTable(CaseContext& c, Args&) {
    gridr::InjectOverlay(c);
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    StubV0(m, s::kLdPopResetFn, 0);
    Put8(m, 0x8005AD32u, (!c.sample && Chance(r, 5)) ? 1u + r.U32() % 255u : 0u);
    if (c.sample) return;
    const uint32_t gs = Gs(m);
    Put8(m, gs + 4u, AnyType(r));
    Put8(m, gs + 5u, r.U32());
    m.PokeWord(gs + 60u, Chance(r, 2) ? 0u : r.U32());
    Put8(m, 0x800D80DDu, r.U32());
    for (uint32_t k = 0; k < 20u * 72u; k += 4) m.PokeWord(0x800D5758u + k, r.U32());
    for (uint32_t k = 0; k < 112u; k += 4) m.PokeWord(s::kL2PoolTable + k, r.U32());
    for (uint32_t a : {0x800D4B80u, 0x800CD6D4u, 0x800CE5A4u, 0x800CD6C4u}) m.PokeWord(a, r.U32());
    for (uint32_t k = 0; k < 56u; k += 4) m.PokeWord(0x800D43C0u + k, r.U32());
}

// ---------------------------------------------------------------------------- TrialLimits 0x800696B8
void SetupTrial(CaseContext& c, Args&) {
    gridr::InjectOverlay(c);
    if (c.sample) return;
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    const uint32_t gs = Gs(m);
    m.PokeWord(0x8005B2B0u, Chance(r, 2) ? 0u : r.U32());
    m.PokeWord(gs + 60u, r.U32() % 3u);                                    // NAMED BOUND: the bank 0..2 (3-entry tables)
    m.PokeWord(gs + 48u, 1u + r.U32() % 2u);
    const uint32_t base = m.PeekWord(s::kL2PoolTable), hiPtr = m.PeekWord(s::kL2PoolTable + 12u);
    const uint32_t live = m.PeekWord(0x8005B1F8u);
    m.PokeWord(hiPtr, static_cast<uint32_t>(static_cast<int32_t>(r.U32() % (live + 1u)) - 1)); // NAMED BOUND: -1..live-1
    for (uint32_t i = 0; i < live && i < 18u; ++i) {
        const uint32_t e = base + 1096u * i;
        if (Chance(r, 3)) WriteU16Bench(m, e + 172u, static_cast<uint16_t>(r.U32() % 4u));
        const uint32_t rd = m.PeekWord(e + 1084u);
        if (rd >= 0x80000000u && rd < 0x80200000u && Chance(r, 3)) Put8(m, rd + 1u, r.U32());
    }
    for (uint32_t k = 0; k < 36u; k += 4) m.PokeWord(0x8005303Cu + k, r.U32() % 0x200000u);
}

// ---------------------------------------------------------------------------- PopBlockInit 0x800689D0
const std::pair<uint32_t, int> kPopKids[] = {{s::kL2PedTablesFn, 0}, {s::kL2PartSlotsFn, 0}, {s::kL2CensusFn, 0},
                                             {s::kLdBlockCopyFn, 2}};
void SetupPopBlock(CaseContext& c, Args& a) {
    gridr::InjectOverlay(c);
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    for (const auto& k : kPopKids) StubV0(m, k.first, 0);
    Put8(m, 0x8005AD31u, (!c.sample && Chance(r, 4)) ? 1u + r.U32() % 255u : 0u);
    a.a[0] = r.U32();
    a.a[1] = r.U32();
    if (!c.sample) m.PokeWord(s::kLdPedSwitch, Chance(r, 2) ? 0u : r.U32());
}

// ---------------------------------------------------------------------------- PedTablesInit 0x80068500
void SetupPedTables(CaseContext& c, Args&) {
    gridr::InjectOverlay(c);
    if (c.sample) return;
    for (uint32_t k = 0; k < 28u; k += 4) c.mem->PokeWord(0x800D5F40u + k, c.rng->U32());
    for (uint32_t k = 0; k < 40u; k += 4) c.mem->PokeWord(0x800D5730u + k, c.rng->U32());
}

// ---------------------------------------------------------------------------- PartSlotsInit 0x80069394
void SetupPartSlots(CaseContext& c, Args&) {
    gridr::InjectOverlay(c);
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    StubV0(m, s::kLdMalloc, (!c.sample && Chance(r, 6)) ? 0u : Area(m) + 8u * (r.U32() % 256u));
    if (c.sample) return;
    m.PokeWord(0x800D4B80u, Chance(r, 3) ? 0u : Area(m) + 0x8000u + 4u * (r.U32() % 256u)); // NAMED BOUND: in RAM
    m.PokeWord(0x800D4B7Cu, r.U32());
    m.PokeWord(0x800CF65Cu, r.U32());
    for (uint32_t k = 0; k < 16u; ++k)
        for (uint32_t o : {0u, 4u, 96u}) m.PokeWord(0x800CF660u + 512u * k + o, r.U32());
    for (uint32_t k = 0; k < 18u * 24u + 4u; k += 4) m.PokeWord(0x800D1660u + k, r.U32());
    m.PokeWord(0x800D1814u, r.U32());
}

// ---------------------------------------------------------------------------- EscapeLoad 0x80068740
void SetupEscape(CaseContext& c, Args&) {
    gridr::InjectOverlay(c);
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    JrRa(m, s::kLdFree);
    StubV0(m, s::kLdMalloc, Area(m) + 0x8000u + 8u * (r.U32() % 256u));
    PutString(m, s::kL2GridName, "DATA\\STARTDFA.BIN");
    std::vector<uint8_t> f = DiscFile("DATA\\STARTJBA.BIN");
    if (!c.sample && Chance(r, 2)) {                                       // NAMED BOUND: counts >= 0, records in the file
        std::vector<uint32_t> w;
        for (int blk = 0; blk < 2; ++blk) {
            uint32_t cnt[5], total = 0;
            for (uint32_t& k : cnt) {
                k = Chance(r, 3) ? 0u : r.U32() % 6u;
                total += k;
            }
            for (uint32_t k : cnt) w.push_back(k);
            for (uint32_t k = 0; k < 3u * total; ++k) w.push_back(r.U32());
        }
        f.assign(reinterpret_cast<const uint8_t*>(w.data()), reinterpret_cast<const uint8_t*>(w.data()) + 4u * w.size());
    }
    const uint32_t len = Plant(m, f, Area(m), 640u);
    StubLoadFileLen(m, Area(m), len, !c.sample && Chance(r, 10));
    Put8(m, Gs(m) + 4u, (c.sample || Chance(r, 4)) ? 44u : AnyType(r));
    for (uint32_t k = 0; k < 8u; k += 4) {
        m.PokeWord(s::kL2JailCounts + k, r.U32());
        m.PokeWord(s::kL2JailArrays + k, r.U32());
    }
}

// ---------------------------------------------------------------------------- AnimNoise 0x80063158
void SetupNoise(CaseContext& c, Args&) {
    gridr::InjectOverlay(c);
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    const std::vector<uint8_t>& f = DiscFile("DATA\\ANIMNOIZ.DAT");
    const uint32_t len = Plant(m, f, Area(m), ldr::kAreaBytes);
    const uint32_t k = c.sample ? 0u : r.U32() % 8u;
    StubLoadFileLen(m, Area(m), k == 1u ? 0u : len, k == 2u);             // a zero length, a failure
    m.PokeWord(s::kL2NoiseFile, k == 3u ? r.U32() : 0u);
    if (c.sample) return;
    for (uint32_t a : {s::kL2NoiseRecords, s::kL2NoiseEvents}) m.PokeWord(a, r.U32());
    if (Chance(r, 3) && len > 64u) {                                       // the offsets drawn: 0 or in the file
        for (int n = 0; n < 16; ++n) {
            const uint32_t at = Area(m) + 4u * (r.U32() % (len / 4u));
            if (at != Area(m) + 4u) m.PokeWord(at, Chance(r, 2) ? 0u : r.U32() % len);
        }
    }
}

// ---------------------------------------------------------------------------- FightLoad 0x800653E8
void SetupFight(CaseContext& c, Args& a) {
    gridr::InjectOverlay(c);
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    const std::vector<uint8_t>& f = DiscFile("DATA\\FIGHT.BIN");
    const uint32_t len = Plant(m, f, Area(m), ldr::kAreaBytes);
    StubLoadFileLen(m, Area(m), len, false);                               // NAMED BOUND: the file is there
    a.a[0] = 0x800524DCu;                                                  // "DATA\FIGHT"
    if (c.sample) return;
    for (uint32_t k = 0; k < 480u; k += 4) if (Chance(r, 4)) m.PokeWord(Area(m) + k, r.U32());
}

// ---------------------------------------------------------------------------- the road loader
void SetupRoadText(CaseContext& c, Args& a) {
    gridr::InjectOverlay(c);
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    StubV0(m, s::kL2Open, c.sample ? 3u : r.U32() & 0x7FFFFFFFu);          // NAMED BOUND: the file opens (else `break`)
    StubV0(m, s::kL2Read, r.U32());
    StubV0(m, s::kL2Close, 0);
    StubV0(m, s::kL2RouteParse, 0);
    a.a[0] = 0x80052400u;                                                  // "DATA\ROAD"
    if (c.sample) return;
    const uint32_t res = m.PeekWord(s::kLdResListPtr);
    m.PokeWord(res + 2648u, r.U32() % 8u);
    if (Chance(r, 2)) m.PokeWord(res + 2616u, r.U32());
    m.PokeWord(Gs(m) + 48u, r.U32() % 10u);
    for (uint32_t k = 0; k < 18u * 16u; k += 4) m.PokeWord(s::kLdFinishTable + k, r.U32());
}
void SetupRoadClear(CaseContext& c, Args&) {
    gridr::InjectOverlay(c);
    if (c.sample) return;
    for (uint32_t a : {0x8005B338u, 0x8005B33Cu, s::kLdPiecesUsed, s::kLdPiecesLast}) c.mem->PokeWord(a, c.rng->U32());
    for (uint32_t k = 0; k < 96u; k += 4) c.mem->PokeWord(s::kLdPieceList + k, c.rng->U32());
}
void SetupRoadRecords(CaseContext& c, Args& a) {
    gridr::InjectOverlay(c);
    StubV0(*c.mem, s::kL2RouteRecords, c.rng->U32());
    a.a[0] = c.sample ? 0x80052400u : c.rng->U32();                        // RoadLoad's name, passed through
}
void SetupRoadLoad(CaseContext& c, Args& a) {
    gridr::InjectOverlay(c);
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    StubV0(m, s::kL2RoadTextFn, r.U32());
    StubV0(m, s::kL2RoadClearFn, r.U32());
    StubV0(m, s::kL2RoadRecordsFn, Chance(r, 3) ? r.U32() & 0xFF00u : r.U32());
    a.a[0] = c.sample ? 0x80052400u : r.U32();
    m.PokeWord(0x8005AD54u, r.U32());
}

// ---------------------------------------------------------------------------- the stream's set-up (SLUS)
void SetupGrf(CaseContext& c, Args&) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    const std::vector<uint8_t>& f = DiscFile("DATA\\STREAM1.GRF");
    const uint32_t len = Plant(m, f, Area(m), ldr::kAreaBytes);
    StubV0(m, s::kL2OpenStream, r.U32() % 8u);
    StubV0(m, s::kL2Size, c.sample ? len : r.U32() % 0x10000u);
    StubV0(m, s::kLdMalloc, Area(m));
    StubV0(m, s::kL2Read, r.U32());
    StubV0(m, s::kL2Close, 0);
    if (c.sample) return;
    m.PokeWord(Gs(m) + 48u, Chance(r, 2) ? 2u : r.U32() % 4u);
    m.PokeWord(c.gp + 472u, Chance(r, 2) ? 0u : Area(m) + 4u * (r.U32() % 1024u)); // NAMED BOUND: a buffer in RAM
    m.PokeWord(c.gp + 468u, r.U32());
}
void SetupStreamSetUp(CaseContext& c, Args&) {
    StubRecord(*c.mem, 0x80024630u, c.scratch + 512u, 0);
    StubRecord(*c.mem, 0x80023498u, c.scratch + 544u, 0);
    StubRecord(*c.mem, 0x80022A1Cu, c.scratch + 576u, 1);
    if (!c.sample) c.mem->PokeWord(Gs(*c.mem) + 48u, c.rng->U32() % 3u);
}
void SetupStreamStart(CaseContext& c, Args&) {
    Memory& m = *c.mem;
    StubRecord(m, 0x800235C8u, c.scratch + 512u, 3);
    StubRecord(m, 0x80023714u, c.scratch + 544u, 0);
    if (c.sample) return;
    const uint32_t rec = c.scratch + 256u;
    for (uint32_t k = 0; k < 12u; k += 4) m.PokeWord(rec + k, c.rng->U32());
    m.PokeWord(0x800D6184u, rec);
}

Row Make(const char* name, const char* sig, uint32_t entry, std::function<void(CaseContext&, Args&)> setup,
         std::function<uint32_t(NativeEnv&, const Args&)> native, bool v0,
         std::initializer_list<std::pair<uint32_t, int>> callees = {}, bool bios = false) {
    std::vector<OracleCallee> k;
    for (const auto& [fn, n] : callees) k.push_back(OracleCallee{fn, n});
    for (OracleCallee& o : k)
        if (o.address == s::kLdLoadFile) o.stackArgs = 1;
    Row r = ldr::Make(name, sig, entry, std::move(setup), std::move(native), v0, std::move(k));
    r.allowBiosCalls = bios;
    return r;
}

} // namespace ldr2

void AddLoader2Rows(std::vector<Row>& rows) {
    namespace s = rr::sim;
    using ldr::Run;
    using ldr2::Make;
    using V = ldr::Seams;
    {
        std::vector<std::pair<uint32_t, int>> k(std::begin(ldr2::kBuildKids), std::end(ldr2::kBuildKids));
        Row r = Make("l2_build_race", "void BuildRace(void)  [RASHCDI 0x8006982C]", s::kL2BuildRaceFn, ldr2::SetupBuildRace,
                     [](NativeEnv& env, const Args&) {
                         return Run(env, "BuildRace", [&](s::GuestRam& g, V& c, uint32_t&) { return s::BuildRace(g, env.sp, c); });
                     }, false, {}, true);
        for (const auto& [fn, n] : k) r.oracleCallees.push_back(OracleCallee{fn, n});
        for (const auto& [fn, n] : std::initializer_list<std::pair<uint32_t, int>>{
                 {s::kL2StrRChr, 2}, {s::kL2StrCat, 2}, {s::kLdMemcpy, 3}, {s::kLdFree, 1}, {s::kL2GetRCnt, 1}, {s::kL2Printf, 1}})
            r.oracleCallees.push_back(OracleCallee{fn, n});
        r.oracleCallees.push_back(OracleCallee{s::kLdLoadFile, 4, 1});
        rows.push_back(std::move(r));
    }
    rows.push_back(Make("l2_world_pools", "void WorldPoolsInit(void)  [RASHCDI 0x80069000]", s::kL2WorldPoolsFn,
                        ldr2::SetupWorldPools, [](NativeEnv& env, const Args&) {
                            return Run(env, "WorldPoolsInit", [&](s::GuestRam& g, V& c, uint32_t&) { return s::WorldPoolsInit(g, env.sp, c); });
                        }, false, {{s::kLdMalloc, 2}, {s::kLdMemset, 3}}));
    rows.push_back(Make("l2_pool_table", "void PoolTableInit(void)  [RASHCDI 0x80068AA4]", s::kL2PoolTableFn,
                        ldr2::SetupPoolTable, [](NativeEnv& env, const Args&) {
                            return Run(env, "PoolTableInit", [&](s::GuestRam& g, V& c, uint32_t&) { return s::PoolTableInit(g, env.sp, c); });
                        }, false, {{s::kLdMemcpy, 3}, {s::kLdMemset, 3}, {s::kLdPopResetFn, 0}}));
    rows.push_back(Make("l2_race_flag", "void RaceFlag(void)  [RASHCDI 0x8006968C]", s::kL2RaceFlagFn,
                        [](CaseContext& c, Args&) {
                            gridr::InjectOverlay(c);
                            if (!c.sample) c.mem->PokeWord(0x8005AD34u, pop::Chance(*c.rng, 2) ? 0u : c.rng->U32());
                        },
                        [](NativeEnv& env, const Args&) {
                            return Run(env, "RaceFlag", [&](s::GuestRam& g, V&, uint32_t&) { s::RaceFlag(g); return true; });
                        }, false));
    rows.push_back(Make("l2_trial_limits", "void TrialLimits(void)  [RASHCDI 0x800696B8]", s::kL2TrialLimitsFn,
                        ldr2::SetupTrial, [](NativeEnv& env, const Args&) {
                            return Run(env, "TrialLimits", [&](s::GuestRam& g, V& c, uint32_t&) { return s::TrialLimits(g, env.sp, c); });
                        }, false, {{s::kL2Scale, 3}}));
    rows.push_back(Make("l2_pop_block", "void PopBlockInit(u8 *block, s32 size)  [RASHCDI 0x800689D0]", s::kL2PopBlockFn,
                        ldr2::SetupPopBlock, [](NativeEnv& env, const Args& a) {
                            return Run(env, "PopBlockInit", [&](s::GuestRam& g, V& c, uint32_t&) {
                                return s::PopBlockInit(g, a.a[0], a.a[1], env.sp, c);
                            });
                        }, false, {{s::kL2PedTablesFn, 0}, {s::kL2PartSlotsFn, 0}, {s::kL2CensusFn, 0}, {s::kLdBlockCopyFn, 2}}));
    rows.push_back(Make("l2_ped_tables", "void PedTablesInit(void)  [RASHCDI 0x80068500]", s::kL2PedTablesFn,
                        ldr2::SetupPedTables, [](NativeEnv& env, const Args&) {
                            return Run(env, "PedTablesInit", [&](s::GuestRam& g, V&, uint32_t&) { s::PedTablesInit(g); return true; });
                        }, false));
    rows.push_back(Make("l2_part_slots", "void PartSlotsInit(void)  [RASHCDI 0x80069394]", s::kL2PartSlotsFn,
                        ldr2::SetupPartSlots, [](NativeEnv& env, const Args&) {
                            return Run(env, "PartSlotsInit", [&](s::GuestRam& g, V& c, uint32_t&) { return s::PartSlotsInit(g, env.sp, c); });
                        }, false, {{s::kLdMalloc, 2}}));
    rows.push_back(Make("l2_census", "void CensusInit(void)  [RASHCDI 0x80068684]", s::kL2CensusFn,
                        [](CaseContext& c, Args&) {
                            gridr::InjectOverlay(c);
                            if (c.sample) return;
                            for (uint32_t a : {0x800D86F0u, 0x800D86F4u}) c.mem->PokeWord(a, c.rng->U32());
                        },
                        [](NativeEnv& env, const Args&) {
                            return Run(env, "CensusInit", [&](s::GuestRam& g, V& c, uint32_t&) { return s::CensusInit(g, env.sp, c); });
                        }, false));
    rows.push_back(Make("l2_census_count", "s32 CensusCount(s32 cls, s32 *live)  [RASHCDI 0x80068614]", s::kL2CensusCount,
                        [](CaseContext& c, Args& a) {
                            gridr::InjectOverlay(c);
                            Memory& m = *c.mem;
                            Rng& r = *c.rng;
                            a.a[0] = (c.sample || pop::Chance(r, 2)) ? 2u : r.U32() % 16u;
                            a.a[1] = c.scratch;
                            if (c.sample) return;
                            const uint32_t base = m.PeekWord(s::kL2PoolTable), live = m.PeekWord(0x8005B1F8u);
                            m.PokeWord(m.PeekWord(s::kL2PoolTable + 12u), static_cast<uint32_t>(static_cast<int32_t>(r.U32() % (live + 1u)) - 1));
                            for (uint32_t i = 0; i < live && i < 18u; ++i) {
                                const uint32_t e = base + 1096u * i;
                                if (pop::Chance(r, 3)) WriteU16Bench(m, e + 320u, pop::Chance(r, 2) ? 0u : static_cast<uint16_t>(r.U32()));
                                const uint32_t rd = m.PeekWord(e + 1084u);
                                if (rd >= 0x80000000u && rd < 0x80200000u && pop::Chance(r, 3)) pop::Put8(m, rd + 1u, r.U32());
                            }
                        },
                        [](NativeEnv& env, const Args& a) {
                            return Run(env, "CensusCount", [&](s::GuestRam& g, V&, uint32_t& v0) {
                                v0 = s::CensusCount(g, a.a[0], a.a[1]);
                                return true;
                            });
                        }, true));
    rows.push_back(Make("l2_speed_class", "void SpeedClassCopy(u8 *block, s32 off)  [RASHCDI 0x800645E4]", s::kL2SpeedClassFn,
                        [](CaseContext& c, Args& a) {
                            gridr::InjectOverlay(c);
                            Memory& m = *c.mem;
                            for (uint32_t k = 0; k < 292u + 16u; k += 4) m.PokeWord(c.scratch + k, c.rng->U32());
                            a.a[0] = c.scratch + 16u;
                            a.a[1] = (c.sample || pop::Chance(*c.rng, 2)) ? 280u : 4u * (c.rng->U32() % 71u) - 12u; // NAMED BOUND
                        },
                        [](NativeEnv& env, const Args& a) {
                            return Run(env, "SpeedClassCopy", [&](s::GuestRam& g, V& c, uint32_t&) {
                                return s::SpeedClassCopy(g, a.a[0], a.a[1], env.sp, c);
                            });
                        }, false, {{s::kLdMemcpy, 3}}));
    {
        Row r = Make("l2_escape_load", "void EscapeLoad(void)  [RASHCDI 0x80068740]", s::kL2EscapeLoadFn, ldr2::SetupEscape,
                     [](NativeEnv& env, const Args&) {
                         return Run(env, "EscapeLoad", [&](s::GuestRam& g, V& c, uint32_t&) { return s::EscapeLoad(g, env.sp, c); });
                     }, false,
                     {{s::kL2StrCpy, 2}, {s::kL2StrRChr, 2}, {s::kL2Printf, 2}, {s::kLdMemcpy, 3}, {s::kLdFree, 1}, {s::kLdMalloc, 2},
                      {s::kLdMemset, 3}, {s::kLdLoadFile, 4}},
                     true);
        rows.push_back(std::move(r));
    }
    rows.push_back(Make("l2_anim_noise", "void AnimNoise(void)  [RASHCDI 0x80063158]", s::kL2AnimNoiseFn, ldr2::SetupNoise,
                        [](NativeEnv& env, const Args&) {
                            return Run(env, "AnimNoise", [&](s::GuestRam& g, V& c, uint32_t&) { return s::AnimNoise(g, env.sp, c); });
                        }, false, {{s::kLdLoadFile, 4}}));
    rows.push_back(Make("l2_fight_load", "u8 *FightLoad(char *name)  [RASHCDI 0x800653E8]", s::kL2FightLoadFn, ldr2::SetupFight,
                        [](NativeEnv& env, const Args& a) {
                            return Run(env, "FightLoad", [&](s::GuestRam& g, V& c, uint32_t& v0) {
                                return s::FightLoad(g, a.a[0], env.sp, c, v0);
                            });
                        }, true, {{s::kLdSprintf, 3}, {s::kLdLoadFile, 4}}, true));
    rows.push_back(Make("l2_road_text", "s32 RoadText(char *name)  [RASHCDI 0x8006A7F8]", s::kL2RoadTextFn, ldr2::SetupRoadText,
                        [](NativeEnv& env, const Args& a) {
                            return Run(env, "RoadText", [&](s::GuestRam& g, V& c, uint32_t& v0) {
                                v0 = 1;
                                return s::RoadText(g, a.a[0], env.sp, c);
                            });
                        }, true,
                        {{s::kLdSprintf, 4}, {s::kL2Open, 2}, {s::kL2Read, 4}, {s::kL2Close, 1}, {s::kL2RouteParse, 2}}, true));
    rows.push_back(Make("l2_road_clear", "void RoadClear(void)  [RASHCDI 0x8006A98C]", s::kL2RoadClearFn, ldr2::SetupRoadClear,
                        [](NativeEnv& env, const Args&) {
                            return Run(env, "RoadClear", [&](s::GuestRam& g, V& c, uint32_t&) { return s::RoadClear(g, env.sp, c); });
                        }, false));
    rows.push_back(Make("l2_road_records", "s32 RoadRecords(void)  [RASHCDI 0x8006AC4C]", s::kL2RoadRecordsFn,
                        ldr2::SetupRoadRecords, [](NativeEnv& env, const Args& a) {
                            return Run(env, "RoadRecords", [&](s::GuestRam& g, V& c, uint32_t& v0) {
                                v0 = 1;
                                return s::RoadRecords(g, a.a[0], env.sp, c);
                            });
                        }, true, {{s::kL2RouteRecords, 1}}));
    rows.push_back(Make("l2_road_load", "s32 RoadLoad(char *name)  [RASHCDI 0x8006AC6C]", s::kL2RoadLoadFn, ldr2::SetupRoadLoad,
                        [](NativeEnv& env, const Args& a) {
                            return Run(env, "RoadLoad", [&](s::GuestRam& g, V& c, uint32_t& v0) {
                                return s::RoadLoad(g, a.a[0], env.sp, c, v0);
                            });
                        }, true, {{s::kL2RoadTextFn, 1}, {s::kL2RoadClearFn, 0}, {s::kL2RoadRecordsFn, 1}}));
    rows.push_back(Make("l2_grf_load", "void GrfLoad(void)  [SLUS 0x800244E0]", s::kL2GrfLoadFn, ldr2::SetupGrf,
                        [](NativeEnv& env, const Args&) {
                            return Run(env, "GrfLoad", [&](s::GuestRam& g, V& c, uint32_t&) { return s::GrfLoad(g, env.sp, c); });
                        }, false,
                        {{s::kLdSprintf, 2}, {s::kL2OpenStream, 1}, {s::kL2Size, 1}, {s::kLdMalloc, 2}, {s::kL2Read, 4}, {s::kL2Close, 1}}, true));
    rows.push_back(Make("l2_stream_set_up", "void StreamSetUp(void)  [SLUS 0x80022F78]", s::kL2StreamSetUpFn, ldr2::SetupStreamSetUp,
                        [](NativeEnv& env, const Args&) {
                            return Run(env, "StreamSetUp", [&](s::GuestRam& g, V& c, uint32_t&) { return s::StreamSetUp(g, env.sp, c); });
                        }, false, {{0x80024630u, 0}, {0x80023498u, 0}, {0x80022A1Cu, 1}}));
    rows.push_back(Make("l2_stream_start", "void StreamStart(void)  [SLUS 0x80023020]", s::kL2StreamStartFn, ldr2::SetupStreamStart,
                        [](NativeEnv& env, const Args&) {
                            return Run(env, "StreamStart", [&](s::GuestRam& g, V& c, uint32_t&) { return s::StreamStart(g, env.sp, c); });
                        }, false, {{0x800235C8u, 3}, {0x80023714u, 0}}));
    rows.push_back(Make("l2_siren_set", "void SirenSet(s32 set)  [SLUS 0x80018DC8]", s::kL2SirenSetFn,
                        [](CaseContext& c, Args& a) {
                            a.a[0] = (c.sample || pop::Chance(*c.rng, 4)) ? c.rng->U32() % 7u : c.rng->U32();
                            c.mem->PokeWord(c.gp + 1908u, c.rng->U32());
                        },
                        [](NativeEnv& env, const Args& a) {
                            return Run(env, "SirenSet", [&](s::GuestRam& g, V&, uint32_t&) {
                                s::SirenSet(g, static_cast<int32_t>(a.a[0]));
                                return true;
                            });
                        }, false));
    rows.push_back(Make("l2_fx_globals", "void FxGlobalsInit(void)  [SLUS 0x8002B83C]", s::kL2FxGlobalsFn,
                        [](CaseContext& c, Args&) {
                            if (c.sample) return;
                            for (uint32_t k = 0; k < 160u; k += 4) c.mem->PokeWord(0x800D7FC8u + k, c.rng->U32());
                        },
                        [](NativeEnv& env, const Args&) {
                            return Run(env, "FxGlobalsInit", [&](s::GuestRam& g, V&, uint32_t&) { s::FxGlobalsInit(g); return true; });
                        }, false));
    rows.push_back(Make("l2_srand", "void SRand(u32 seed)  [SLUS 0x8001FC84]", s::kL2SRandFn,
                        [](CaseContext& c, Args& a) { a.a[0] = c.rng->U32(); },
                        [](NativeEnv& env, const Args& a) {
                            return Run(env, "SRand", [&](s::GuestRam& g, V&, uint32_t&) { s::SRand(g, a.a[0]); return true; });
                        }, false));
    for (int k = 0; k < 2; ++k)
        rows.push_back(Make(k == 0 ? "l2_fx_reset" : "l2_fx_records", k == 0 ? "void FxReset(void)  [RASHCDI 0x8005D5E4]" : "void FxRecordsInit(void)  [RASHCDI 0x8005D988]",
                            k == 0 ? s::kL2FxResetFn : s::kL2FxRecordsFn,
                            [](CaseContext& c, Args&) {
                                gridr::InjectOverlay(c);
                                if (c.sample) return;
                                for (uint32_t a = 0x800D39B0u; a < 0x800D39B0u + 20u * 112u; a += 4)
                                    if (pop::Chance(*c.rng, 2)) c.mem->PokeWord(a, c.rng->U32());
                                for (uint32_t a : {0x800D8068u, 0x800D806Cu, 0x800D8070u, 0x800D8074u, 0x8005B360u, 0x8005B280u})
                                    c.mem->PokeWord(a, c.rng->U32());
                            },
                            [k](NativeEnv& env, const Args&) {
                                return Run(env, "FxReset", [&](s::GuestRam& g, V&, uint32_t&) {
                                    if (k == 0) s::FxReset(g);
                                    else s::FxRecordsInit(g);
                                    return true;
                                });
                            }, false));
    rows.push_back(Make("l2_grf_fix", "s32 GrfFix(u8 *grf)  [SLUS 0x80024610]", s::kL2GrfFixFn,
                        [](CaseContext& c, Args& a) {
                            for (uint32_t k = 0; k < 32u; k += 4) c.mem->PokeWord(c.scratch + k, c.rng->U32());
                            a.a[0] = c.scratch;
                        },
                        [](NativeEnv& env, const Args& a) {
                            return Run(env, "GrfFix", [&](s::GuestRam& g, V&, uint32_t& v0) {
                                v0 = s::GrfFix(g, a.a[0]);
                                return true;
                            });
                        }, true));
}
