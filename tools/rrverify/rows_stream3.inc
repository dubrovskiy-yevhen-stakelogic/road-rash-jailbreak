// Bench rows of the stream's file loaders (src\game\sim\stream_files.{h,cpp}): the stream's files
// (SLUS 0x80023498 with 0x80023F08 / 0x8002428C), the .STP start (0x80023714 with 0x80024168), the CD layer's waits
// (0x80022A1C / 0x80022A78 / 0x800229F4) and the album (0x80024630 with 0x80024FF8), SLUS_010.53 SHA-1
// 67ed165a2c517d4e6106fb0dfa324d66dd9a76f1. Included LAST by verify_physics.cpp (rows_stream.inc's strows::,
// rows_stream2.inc's st2rows:: and rows_loader.inc's ldr:: are visible). The machine is the race capture (rr-race).
//
// INPUTS: the player's own DATA\STREAM1/2.TOC, .RLS and RACE1_<n>.STP (work\disc_us), whole or cut, their offsets
// stirred; the handles, sizes, read results and the drive's busy word varied; the stream record, the table and the
// CD ring stirred as rows_stream.inc / rows_stream2.inc do.
//
// NAMED BOUNDS (the same on both machines):
//   * the CD file layer is the host's: open (SLUS 0x80023100) and size (0x800148BC) answer the case's list in order,
//     read (0x80014780) copies from the case's source in order (or answers -1), close is `jr ra`; malloc 0x8001447C
//     is the host's block rule (user = block + 4, size (n + 11) & ~7) over the buffers of records 28..31; each stub
//     sits in the scratch area behind a `j` at the function's address, which the bench watches;
//   * VSync (SLUS 0x80047724) is the host's: a stub that counts the fields and clears the drive's busy word
//     *(0x80053464) at the case's count (the drive's interrupt, which the bench has not); the music stream player's
//     set-up 0x80020BEC is the sound runtime's: `v0` = the case's answer;
//   * CdWait's mode 1 runs only with the drive idle (the console spins on the interrupt);
//   * a release list's header byte +4 is >= 8 (its relocation count; the disc's files have 0x9C);
//   * one player's stream record (the capture's): 0x80023714 runs with game_state+0x30 = 0 or 1.
//   * game_state+0x40 is a race id (0..99): 0x80023714's name buffer is 16 bytes (a long %ld overruns its frame);
namespace st3rows {

namespace s = rr::sim;
using namespace strows;

constexpr uint32_t Hi(uint32_t a) { return (a + 0x8000u) >> 16; }
constexpr uint32_t Lo(uint32_t a) { return a & 0xFFFFu; }

// scratch layout
constexpr uint32_t kName = 0x000, kCellOpen = 0x040, kCellSize = 0x044, kCellHeap = 0x048, kCellSrc = 0x04C,
                   kCellFields = 0x050, kOpenList = 0x060, kSizeList = 0x080, kSrcList = 0x0A0, kCode = 0x100, kHdr = 0x400, kOut = 0x800;

void Code(Memory& m, uint32_t at, std::initializer_list<uint32_t> w) {
    uint32_t k = 0;
    for (uint32_t x : w) m.PokeWord(at + 4u * k++, x);
}
void Jump(Memory& m, uint32_t fn, uint32_t to) {
    m.PokeWord(fn, 0x08000000u | ((to >> 2) & 0x03FFFFFFu));
    m.PokeWord(fn + 4u, 0);
}
// v0 = the next word of the list *cell points at
void SeqStub(Memory& m, uint32_t fn, uint32_t at, uint32_t cell) {
    Code(m, at, {0x3C080000u | Hi(cell), 0x8D090000u | Lo(cell), 0, 0x8D220000u, 0x25290004u, 0x03E00008u,
                 0xAD090000u | Lo(cell)});
    Jump(m, fn, at);
}
// the host's block rule: v0 = *cell + 4, *cell += (a0 + 11) & ~7
void BumpStub(Memory& m, uint32_t fn, uint32_t at, uint32_t cell) {
    Code(m, at, {0x3C080000u | Hi(cell), 0x8D0B0000u | Lo(cell), 0x2489000Bu, 0x240AFFF8u, 0x012A4824u, 0x01694821u,
                 0xAD090000u | Lo(cell), 0x03E00008u, 0x25620004u});
    Jump(m, fn, at);
}
// read(h, buf, n): n bytes from the next source of the list *cell points at to buf, v0 = n
void CopyStub(Memory& m, uint32_t fn, uint32_t at, uint32_t cell) {
    Code(m, at, {0x3C080000u | Hi(cell), 0x8D0C0000u | Lo(cell), 0, 0x8D8B0000u, 0x258C0004u, 0xAD0C0000u | Lo(cell),
                 0x00C01021u, 0x18C00007u, 0, 0x91690000u, 0x256B0001u, 0xA0A90000u, 0x24C6FFFFu, 0x1CC0FFFBu, 0x24A50001u,
                 0x03E00008u, 0});
    Jump(m, fn, at);
}
// VSync(0): --*cell; at 0 the drive's busy word cleared; v0 = 0
void FieldStub(Memory& m, uint32_t fn, uint32_t at, uint32_t cell) {
    Code(m, at, {0x3C080000u | Hi(cell), 0x8D090000u | Lo(cell), 0, 0x2529FFFFu, 0xAD090000u | Lo(cell), 0x1D200003u, 0,
                 0x3C0A8005u, 0xAD403464u, 0x03E00008u, 0x00001021u});
    Jump(m, fn, at);
}

uint32_t Src(Memory& m) { return ldr::Area(m) - 0x10000u; } // records 24..27's buffers
void Put(Memory& m, uint32_t at, const std::vector<uint8_t>& f, size_t n) {
    if (n) m.WriteBlock(at, f.data(), static_cast<uint32_t>(n));
}

// The host's stubs over the file layer, the heap, the fields; the lists empty.
void Host(CaseContext& c, bool readsFail) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    const uint32_t sc = c.scratch;
    SeqStub(m, s::kSfOpen, sc + kCode, sc + kCellOpen);
    SeqStub(m, s::kSfSize, sc + kCode + 0x20u, sc + kCellSize);
    BumpStub(m, s::kSfMalloc, sc + kCode + 0x40u, sc + kCellHeap);
    if (readsFail) ldr::StubV0(m, s::kSfRead, 0xFFFFFFFFu);
    else CopyStub(m, s::kSfRead, sc + kCode + 0x80u, sc + kCellSrc);
    FieldStub(m, s::kSfVSync, sc + kCode + 0x100u, sc + kCellFields);
    ldr::StubV0(m, s::kSfClose, 0);
    m.PokeWord(sc + kCellOpen, sc + kOpenList);
    m.PokeWord(sc + kCellSize, sc + kSizeList);
    m.PokeWord(sc + kCellHeap, ldr::Area(m) + 8u * (r.U32() % 16u));
    m.PokeWord(sc + kCellSrc, sc + kSrcList);
    m.PokeWord(sc + kCellFields, Pick(r, {1u, 2u, 17u, 199u, 200u, 201u, 400u, 1000u}));
    for (uint32_t k = 0; k < 8u; ++k) {
        m.PokeWord(sc + kOpenList + 4u * k, 3u);
        m.PokeWord(sc + kSizeList + 4u * k, 0);
        m.PokeWord(sc + kSrcList + 4u * k, Src(m));
    }
    const char* nm = "STREAM1.TOC";
    for (uint32_t k = 0; k <= 11u; ++k) pop::Put8(m, sc + kName + k, k < 11u ? static_cast<uint8_t>(nm[k]) : 0u);
}
uint32_t Handle(Rng& r, bool sample, uint32_t natural) {
    if (sample || !Chance(r, 5)) return natural;
    return Pick(r, {0xFFFFFFFFu, 0xFFFFFF9Cu, 0u, 1u, 7u});
}

// A stream file planted at the source cursor; its size (whole, or cut) into the size list slot k; returns the size.
uint32_t PlantFile(CaseContext& c, const char* name, uint32_t& src, uint32_t k, bool rls) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    std::vector<uint8_t> f = ldr::DiscFile(name);
    if (f.size() < 0x20u) return 0;
    if (!c.sample && f.size() >= 0x20u) {
        if (!rls && Chance(r, 3)) { // the TOC's two offsets
            const uint32_t v = r.U32() % 0x1000u;
            std::memcpy(f.data() + (Chance(r, 2) ? 20u : 24u), &v, 4);
        }
        if (rls && Chance(r, 3)) f[4] = static_cast<uint8_t>(8u + 4u * (r.U32() % 40u)); // NAMED BOUND: >= 8
    }
    uint32_t n = static_cast<uint32_t>(f.size());
    if (!c.sample && Chance(r, 4)) n = std::max<uint32_t>(rls ? 8u + f[4] : 0x20u, n - r.U32() % (n / 2u + 1u));
    Put(m, src, f, f.size());
    m.PokeWord(c.scratch + kSrcList + 4u * k, src);
    src += static_cast<uint32_t>(f.size());
    m.PokeWord(c.scratch + kSizeList + 4u * k, n);
    return n;
}

void GameSet(CaseContext& c) {
    if (!c.sample && Chance(*c.rng, 3)) c.mem->PokeWord(ldr::Gs(*c.mem) + 48u, 2u);
}

// The stream record, the table and the ring as the streamer leaves them; the CD busy or idle.
void Stream(CaseContext& c) {
    st2rows::Machine2(c);
    StirState(c);
    StirTable(c, false);
    st2rows::StirRing(c, 6);
}

// A .STP header (the disc's, or its fields stirred) at the scratch source.
void PlantHeader(CaseContext& c) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    const uint32_t race = c.sample ? 4u : 1u + r.U32() % 30u;
    const std::string name = "DATA\\RACE1_" + std::to_string(race) + ".STP";
    const std::vector<uint8_t>& f = ldr::DiscFile(name.c_str());
    std::vector<uint8_t> h(0x28, 0);
    std::memcpy(h.data(), f.data(), std::min<size_t>(f.size(), 0x28));
    if (!c.sample && Chance(r, 3)) {
        const uint32_t start = 0x4000u * (r.U32() % 8u), len = 0x4000u * (r.U32() % 12u), resume = 0x4000u * (r.U32() % 16u);
        const uint32_t o = Pick(r, {0x14u, 0x18u, 0x24u});
        std::memcpy(h.data() + o, o == 0x14u ? &start : (o == 0x18u ? &len : &resume), 4);
    }
    m.WriteBlock(c.scratch + kHdr, h.data(), 0x28);
    m.PokeWord(c.scratch + kSrcList, c.scratch + kHdr);
}

Row S3Row(const char* name, const char* sig, uint32_t entry, std::function<void(CaseContext&, Args&)> setup,
          std::function<uint32_t(NativeEnv&, const Args&)> native, bool v0,
          std::initializer_list<OracleCallee> extra) {
    Row r = SRow(name, sig, entry, std::move(setup), std::move(native), v0);
    for (const OracleCallee& o : extra) r.oracleCallees.push_back(o);
    return r;
}

} // namespace st3rows

void AddStream3Rows(std::vector<Row>& rows) {
    using namespace st3rows;
    using G = s::GuestRam;
    using C = rr::sim::RecoverCallees;
    const OracleCallee kOpen{s::kSfOpen, 1}, kSize{s::kSfSize, 1}, kMalloc{s::kSfMalloc, 2}, kRead{s::kSfRead, 4},
        kClose{s::kSfClose, 1}, kVSync{s::kSfVSync, 1};

    // ---- the stream's files
    rows.push_back(S3Row("sf_toc_load", "u32 0x80023F08(out, name)  [SLUS, STREAM<n>.TOC into a heap block]", s::kSfTocLoadFn,
        [](CaseContext& c, Args& a) {
            strows::Machine(c);
            const bool fail = !c.sample && Chance(*c.rng, 6);
            Host(c, fail);
            uint32_t src = Src(*c.mem);
            PlantFile(c, Chance(*c.rng, 2) ? "DATA\\STREAM1.TOC" : "DATA\\STREAM2.TOC", src, 0, false);
            c.mem->PokeWord(c.scratch + kOpenList, Handle(*c.rng, c.sample != nullptr, 3u));
            a.a[0] = (c.sample || Chance(*c.rng, 2)) ? c.gp + s::kGpStToc : c.scratch + kOut;
            a.a[1] = c.scratch + kName;
        },
        [](NativeEnv& env, const Args& a) { return Run(env, [&](G& g, C& o, uint32_t& v) { return s::TocLoad(g, a.a[0], a.a[1], env.sp, o, v); }); },
        true, {kOpen, kSize, kMalloc, kRead, kClose}));
    rows.push_back(S3Row("sf_rls_load", "u32 0x8002428C(name)  [SLUS, STREAM<n>.RLS into a heap block, relocated]", s::kSfRlsLoadFn,
        [](CaseContext& c, Args& a) {
            strows::Machine(c);
            const bool fail = !c.sample && Chance(*c.rng, 6);
            Host(c, fail);
            uint32_t src = Src(*c.mem);
            PlantFile(c, Chance(*c.rng, 2) ? "DATA\\STREAM1.RLS" : "DATA\\STREAM2.RLS", src, 0, true);
            c.mem->PokeWord(c.scratch + kOpenList, Handle(*c.rng, c.sample != nullptr, 3u));
            if (!c.sample && Chance(*c.rng, 3)) { c.mem->PokeWord(c.gp + s::kGpRlsFile, c.rng->U32()); c.mem->PokeWord(c.gp + s::kGpRlsTable, c.rng->U32()); }
            a.a[0] = c.scratch + kName;
        },
        [](NativeEnv& env, const Args& a) { return Run(env, [&](G& g, C& o, uint32_t& v) { return s::RlsLoad(g, a.a[0], env.sp, o, v); }); },
        true, {kOpen, kSize, kMalloc, kRead, kClose}));
    rows.push_back(S3Row("sf_stream_files", "void 0x80023498()  [SLUS, the stream's files: TOC, RLS, STR]", s::kSfFilesFn,
        [](CaseContext& c, Args&) {
            strows::Machine(c);
            Rng& r = *c.rng;
            const bool fail = !c.sample && Chance(r, 8);
            Host(c, fail);
            GameSet(c);
            const char* set = Chance(r, 2) ? "1" : "2";
            uint32_t src = Src(*c.mem);
            PlantFile(c, (std::string("DATA\\STREAM") + set + ".TOC").c_str(), src, 0, false);
            PlantFile(c, (std::string("DATA\\STREAM") + set + ".RLS").c_str(), src, 1, true);
            Memory& m = *c.mem;
            m.PokeWord(c.scratch + kSizeList + 8u, c.sample ? 0x070C8000u : r.U32());          // the .STR's size
            m.PokeWord(c.scratch + kOpenList + 0u, 3u);
            m.PokeWord(c.scratch + kOpenList + 4u, Handle(r, c.sample != nullptr, 3u));        // the RLS may be missing
            m.PokeWord(c.scratch + kOpenList + 8u, Handle(r, c.sample != nullptr, 3u));
            for (uint32_t o : {s::kGpStToc, s::kGpRlsFile, s::kGpRlsTable, s::kGpRlsCursor, s::kGpRlsCursor + 4u, s::kGpStStr,
                               s::kGpStStrSize, s::kGpStLastPos, s::kGpStTurn})
                if (!c.sample && Chance(r, 2)) m.PokeWord(c.gp + o, r.U32());
        },
        [](NativeEnv& env, const Args&) { return Run(env, [&](G& g, C& o, uint32_t&) { return s::StreamFiles(g, env.sp, o); }); },
        false, {OracleCallee{s::kSfSprintf, 4, 1}, kOpen, kSize, kMalloc, kRead, kClose}));

    // ---- the .STP start
    rows.push_back(S3Row("sf_stp_start", "u32 0x80024168(name)  [SLUS, one player's .STP start]", s::kSfStpStartFn,
        [](CaseContext& c, Args& a) {
            Stream(c);
            Rng& r = *c.rng;
            const bool fail = !c.sample && Chance(r, 8);
            Host(c, fail);
            PlantHeader(c);
            c.mem->PokeWord(c.scratch + kOpenList, Handle(r, c.sample != nullptr, 4u));
            a.a[0] = c.scratch + kName;
        },
        [](NativeEnv& env, const Args& a) { return Run(env, [&](G& g, C& o, uint32_t& v) { return s::StpStart(g, a.a[0], env.sp, o, v); }); },
        true, {kOpen, kRead, kClose, kVSync}));
    rows.push_back(S3Row("sf_stp_players", "void 0x80023714()  [SLUS, each player's .STP start]", s::kSfStpFn,
        [](CaseContext& c, Args&) {
            Stream(c);
            Rng& r = *c.rng;
            const bool fail = !c.sample && Chance(r, 8);
            Host(c, fail);
            PlantHeader(c);
            Memory& m = *c.mem;
            m.PokeWord(c.scratch + kOpenList, Handle(r, c.sample != nullptr, 4u));
            if (!c.sample) {
                m.PokeWord(ldr::Gs(m) + 48u, Chance(r, 5) ? 0u : 1u);
                m.PokeWord(ldr::Gs(m) + 64u, Pick(r, {1u, 4u, 31u, 64u, 0u, r.U32() % 100u})); // NAMED BOUND: a race id (the name buffer is 16 bytes)
            }
        },
        [](NativeEnv& env, const Args&) { return Run(env, [&](G& g, C& o, uint32_t&) { return s::StpPlayers(g, env.sp, o); }); },
        false, {OracleCallee{s::kSfSprintf, 4}, kOpen, kRead, kClose, kVSync}));

    // ---- the CD layer's waits
    auto waitSetup = [](CaseContext& c, Args& a) {
        Stream(c);
        Host(c, false);
        Rng& r = *c.rng;
        Memory& m = *c.mem;
        m.PokeWord(0x80053464u, r.U32() % 2u);
        a.a[0] = c.sample ? 2u : Pick(r, {2u, 2u, 2u, 1u, 0u, 3u, r.U32()});
        if (a.a[0] == 1u) m.PokeWord(0x80053464u, 0); // NAMED BOUND: mode 1 only with the drive idle
    };
    rows.push_back(S3Row("sf_cd_wait", "s32 0x80022A78(mode)  [SLUS, the wait for the idle drive]", s::kSfCdWaitFn, waitSetup,
        [](NativeEnv& env, const Args& a) { return Run(env, [&](G& g, C& o, uint32_t& v) { return s::CdWait(g, a.a[0], env.sp, o, v); }); },
        true, {kVSync}));
    rows.push_back(S3Row("sf_cd_flush", "s32 0x80022A1C(mode)  [SLUS, the ring flushed in the critical section, the wait]", s::kSfCdFlushFn, waitSetup,
        [](NativeEnv& env, const Args& a) { return Run(env, [&](G& g, C& o, uint32_t& v) { return s::CdFlush(g, a.a[0], env.sp, o, v); }); },
        true, {kVSync}));
    rows.push_back(S3Row("sf_cd_abort", "s32 0x800229F4(mode)  [SLUS, the flush after a timed-out wait]", s::kSfCdAbortFn, waitSetup,
        [](NativeEnv& env, const Args& a) { return Run(env, [&](G& g, C& o, uint32_t& v) { return s::CdAbort(g, a.a[0], env.sp, o, v); }); },
        true, {kVSync}));

    // ---- the album
    rows.push_back(S3Row("sf_album_clear", "void 0x80024FF8()  [SLUS, the music request table emptied]", s::kSfAlbumClearFn,
        [](CaseContext& c, Args&) {
            strows::Machine(c);
            for (uint32_t k = 0; k < 64u; ++k)
                if (!c.sample && Chance(*c.rng, 2)) c.mem->PokeWord(s::kSfMusicReqs + 4u * k, c.rng->U32());
        },
        [](NativeEnv& env, const Args&) { return Run(env, [&](G& g, C&, uint32_t&) { s::AlbumClear(g); return true; }); },
        false, {}));
    rows.push_back(S3Row("sf_album", "void 0x80024630()  [SLUS, the album opened into its stream record]", s::kSfAlbumFn,
        [](CaseContext& c, Args&) {
            strows::Machine(c);
            Host(c, false);
            Rng& r = *c.rng;
            Memory& m = *c.mem;
            m.PokeWord(c.scratch + kOpenList, Handle(r, c.sample != nullptr, 0u));
            m.PokeWord(c.scratch + kOpenList + 4u, Handle(r, c.sample != nullptr, 2u));
            m.PokeWord(c.scratch + kSizeList, c.sample ? 63537152u : r.U32());
            ldr::StubV0(m, s::kSfMusicSetUp, (c.sample || Chance(r, 2)) ? 0u : Pick(r, {0xFFFFFFFFu, 1u, 0x80000000u, 0x434E0u}));
            for (uint32_t k = 0; k < 16u; ++k)
                if (!c.sample && Chance(r, 2)) m.PokeWord(s::kSfAlbum + 4u * k, r.U32());
            for (uint32_t o : {0x8005B4E8u, 0x80053658u, c.gp + 1380u})
                if (!c.sample && Chance(r, 2)) m.PokeWord(o, r.U32());
        },
        [](NativeEnv& env, const Args&) { return Run(env, [&](G& g, C& o, uint32_t&) { return s::AlbumOpen(g, env.sp, o); }); },
        false, {kOpen, kSize, OracleCallee{s::kSfMusicSetUp, 4, 8}}));
}
