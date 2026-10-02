// Bench rows of the original's streamer (src\game\sim\stream.h).
// Included LAST by verify_physics.cpp inside its anonymous namespace (rows_world.inc's worldrows:: and
// rows_recover.inc's rcv:: are visible).
//
// The machine is a race capture (rr-race by default: one player on road 9, the resource table with 28 of
// its 32 records loaded - cells, textures, road pieces, panoramas - three road pieces and ten cells
// resident). Every case stirs what the streamer reads: player 0's stream record (road, along, direction,
// the change flags, the mode, the read cursor), the entity it follows, the table's records (some freed,
// some re-armed, fresh chunks of the player's own work\disc_us\DATA\STREAM1.STR planted as a completed read
// leaves them - every key type), the new-block queue, the piece list, the cell and texture slots, the VRAM
// pages, the panorama slots. The seams (stream.h) are answered by the original code at the port's
// call-site sp and their call sequence compared.
//
// NAMED BOUNDS (states the game builds):
//   * one player (the capture's); the two-player MODE bit is stirred where a function reads it;
//   * the kernel's two critical-section stubs (SLUS 0x80043DA4 / 0x80043DB4: `li a0,n; syscall`) have their
//     `syscall` replaced by a nop on both sides (rows_feel.inc's bound): the bench has no interrupts;
//   * the CD layer is busy (*(0x80053464) = 1) so that the queue SLUS 0x80022D20 only files a request - the
//     drive itself is the host's (the product reads the disc image at once);
//   * a record's header pointer +0x14 is 0 (free) or its own chunk; a planted chunk is a whole chunk of the
//     disc's STREAM1.STR, never a relocated one (the loaders relocate in place, once).
namespace strows {

namespace s = rr::sim;

bool Chance(Rng& r, uint32_t n) { return r.U32() % n == 0u; }
uint32_t Pick(Rng& r, std::initializer_list<uint32_t> v) { return *(v.begin() + r.U32() % v.size()); }
void Put16(Memory& m, uint32_t a, uint16_t v) { m.WriteBlock(a, &v, 2); }
void Put8(Memory& m, uint32_t a, uint8_t v) { m.WriteBlock(a, &v, 1); }
uint16_t Get16(Memory& m, uint32_t a) {
    uint16_t v = 0;
    m.ReadBlock(a, &v, 2);
    return v;
}

// ---------------------------------------------------------------- the disc's STREAM1.STR
struct StrIndex {
    std::vector<uint32_t> byType[16];
    bool ok = false;
};
const char* kStrPath = "work\\disc_us\\DATA\\STREAM1.STR";
const StrIndex& Str() {
    static const StrIndex idx = [] {
        StrIndex x;
        FILE* f = std::fopen(kStrPath, "rb");
        if (f == nullptr) {
            std::fprintf(stderr, "rows_stream: %s not readable; the rows will fail\n", kStrPath);
            return x;
        }
        uint8_t b[4];
        for (uint32_t i = 0;; ++i) {
            if (_fseeki64(f, static_cast<long long>(i) * 0x4000, SEEK_SET) != 0 || std::fread(b, 1, 4, f) != 4) break;
            x.byType[b[3] >> 4].push_back(i);
        }
        std::fclose(f);
        x.ok = true;
        return x;
    }();
    return idx;
}
std::vector<uint8_t> ReadChunk(uint32_t i) {
    std::vector<uint8_t> v(0x4000, 0);
    FILE* f = std::fopen(kStrPath, "rb");
    if (f == nullptr) return v;
    _fseeki64(f, static_cast<long long>(i) * 0x4000, SEEK_SET);
    std::fread(v.data(), 1, v.size(), f);
    std::fclose(f);
    return v;
}
// A chunk of one of `types`, or of any type.
std::vector<uint8_t> SomeChunk(Rng& r, std::initializer_list<uint32_t> types) {
    const StrIndex& x = Str();
    std::vector<uint32_t> pool;
    for (uint32_t t : types)
        for (uint32_t i : x.byType[t & 15u]) pool.push_back(i);
    if (pool.empty()) return std::vector<uint8_t>(0x4000, 0);
    return ReadChunk(pool[r.U32() % pool.size()]);
}

// ---------------------------------------------------------------- the machine
constexpr uint32_t kGp = 0x8005AC8C;
uint32_t Res(Memory& m) { return m.PeekWord(s::kStResList); }
uint32_t Rec(Memory& m, uint32_t i) { return Res(m) + 0x2Cu + 36u * i; }
uint32_t State(uint32_t p) { return s::kStStates + 0x80u * p; }

void Machine(CaseContext& c) {
    Memory& m = *c.mem;
    m.PokeWord(0x80043DA8u, 0); // the two syscalls: nop
    m.PokeWord(0x80043DB8u, 0);
    m.PokeWord(s::kStLoadImageFn, 0x03E00008u);      // LoadImage: `jr ra; nop` - the GPU is the host's
    m.PokeWord(s::kStLoadImageFn + 4u, 0);
    m.PokeWord(s::kStSpeechLoadFn, 0x03E00008u);     // SpeechBankLoad: `jr ra; li v0,2` - the SPU is the host's
    m.PokeWord(s::kStSpeechLoadFn + 4u, 0x24020002u);
    m.PokeWord(s::kStMusicDoneFn, 0x03E00008u);      // the music streamer's chunk and frame: `jr ra; nop` - the
    m.PokeWord(s::kStMusicDoneFn + 4u, 0);           // SPU / XA side is the host's
    m.PokeWord(s::kStMusicFrameFn, 0x03E00008u);
    m.PokeWord(s::kStMusicFrameFn + 4u, 0);
    m.PokeWord(0x80053464u, 1); // the CD layer busy: 0x80022D20 files only
    m.PokeWord(kGp + s::kGpStCur, State(0));
}

// A fresh chunk into record i as ReadDone leaves it (kind 0x10, 0x31: loaded, stream; +0x20 marked or
// not), with its header, payload and extent pointers (0x80031540) and the 0x80031B4C header move for a
// texture half.
uint32_t PlantChunk(CaseContext& c, uint32_t i, const std::vector<uint8_t>& chunk, bool marked, uint32_t p = 0) {
    Memory& m = *c.mem;
    const uint32_t rec = Rec(m, i);
    const uint32_t buf = m.PeekWord(rec + 12u);
    m.WriteBlock(buf, chunk.data(), 0x4000);
    const uint32_t key = m.PeekWord(buf);
    const uint32_t type = key >> 28;
    m.PokeWord(rec, 0x11u | (marked ? 0x20u : 0u));
    m.PokeWord(rec + 8u, key);
    m.PokeWord(rec + 20u, buf);
    m.PokeWord(rec + 24u, 0);
    if (type == 1 || type == 2) {
        m.PokeWord(rec + 16u, buf);
        if (marked) { // the header moved to the chunk's end (0x80031B4C)
            std::vector<uint8_t> a(32), b(32);
            m.ReadBlock(buf, a.data(), 32);
            m.ReadBlock(buf + 0x3FE0u, b.data(), 32);
            m.WriteBlock(buf, b.data(), 32);
            m.WriteBlock(buf + 0x3FE0u, a.data(), 32);
            m.PokeWord(rec + 20u, buf + 0x3FE0u);
        }
    } else if (type == 0 || type == 8) {
        m.PokeWord(rec + 16u, buf + 0x20u);
        const int32_t len = static_cast<int32_t>(m.PeekWord(buf + 0x20u) * 4u) - 48;
        if (len > 0) m.PokeWord(rec + 24u, buf + 0x20u + static_cast<uint32_t>(len));
    } else {
        m.PokeWord(rec + 16u, buf + 0x20u);
    }
    m.PokeWord(rec + 28u, Chance(*c.rng, 3) ? 0u : c.rng->U32() & 0x1FFFFu);
    m.PokeWord(rec + 32u, p << 16);
    return rec;
}

// The free records of player 0's range (flags 0), and the loaded ones (0x11 set).
std::vector<uint32_t> Records(Memory& m, bool loaded) {
    std::vector<uint32_t> v;
    const uint32_t r = Res(m);
    for (uint32_t i = m.PeekWord(r + 0xA40u); i <= m.PeekWord(r + 0xA44u); ++i) {
        const uint32_t f = m.PeekWord(Rec(m, i));
        if (loaded ? (f & 0x11u) == 0x11u : f == 0) v.push_back(i);
    }
    return v;
}

// The TOC roads (for a stream record's road) and a road's length (>> 6).
uint32_t SomeRoad(Rng& r, Memory& m) {
    const uint32_t toc = m.PeekWord(kGp + s::kGpStToc);
    const uint32_t n = m.PeekWord(toc + 12u);
    if (n == 0) return 9u;
    return m.PeekWord(m.PeekWord(toc + 20u) + 20u * (r.U32() % n));
}
uint32_t RoadLen(Memory& m, uint32_t road) {
    return m.PeekWord(m.PeekWord(m.PeekWord(s::kStRoadTables) + 0x18u) + 16u * road + 4u) >> 6;
}

// Player 0's stream record and the entity it follows, stirred around the capture's.
void StirState(CaseContext& c) {
    Rng& r = *c.rng;
    Memory& m = *c.mem;
    const uint32_t st = State(0);
    const uint32_t e = m.PeekWord(st + 4u);
    if (!Chance(r, 3)) { // the entity's road position
        const uint32_t road = Chance(r, 3) ? m.PeekWord(st + 0x48u) : SomeRoad(r, m);
        const uint32_t len = RoadLen(m, road);
        Put16(m, e + 0x168u, static_cast<uint16_t>(road));
        Put16(m, e + 0x16Au, static_cast<uint16_t>(Chance(r, 6) ? 1u : 0u));
        const uint32_t along = Pick(r, {0u, 40u, 41u, len, len - 40u, len - 41u, r.U32() % (len + 1u), r.U32() % (len + 1u)});
        m.PokeWord(e + 0x170u, (along << 16) | (r.U32() & 0xFFFFu));
        m.PokeWord(e + 0x16Cu, Pick(r, {1u, 0xFFFFFFFFu, 1u, 0xFFFFFFFFu, 0u, r.U32()}));
    }
    auto flag = [&](uint32_t off, uint32_t n) { if (Chance(r, n)) m.PokeWord(st + off, r.U32() % 2u); };
    flag(0x24u, 4); flag(0x28u, 4); flag(0x2Cu, 4); flag(0x30u, 4); flag(0x3Cu, 3);
    if (Chance(r, 3)) m.PokeWord(st + 0x38u, Pick(r, {0u, 1u, 0u, 1u, 2u}));
    if (Chance(r, 3)) m.PokeWord(st + 0x40u, Pick(r, {1u, 0xFFFFFFFFu, 0u}));
    if (Chance(r, 3)) {
        const uint32_t road = SomeRoad(r, m);
        m.PokeWord(st + 0x48u, road);
        m.PokeWord(st + 0x5Cu, m.PeekWord(m.PeekWord(s::kStRoadTables) + 0x18u) + 16u * road);
        m.PokeWord(st + 0x60u, RoadLen(m, road));
        m.PokeWord(st + 0x44u, r.U32() % (RoadLen(m, road) + 1u));
    }
    if (Chance(r, 3)) m.PokeWord(st + 0x50u, Chance(r, 2) ? m.PeekWord(st + 0x48u) : SomeRoad(r, m));
    if (Chance(r, 3)) m.PokeWord(st + 0x58u, Pick(r, {1u, 0xFFFFFFFFu, 0u}));
    if (Chance(r, 3)) m.PokeWord(st + 0x54u, r.U32() % 2000u);
    if (Chance(r, 3)) m.PokeWord(st + 0x68u, Pick(r, {0u, 0x10000u, r.U32() % 3000u}));
    // the read cursor: position within / at / past its range, the end flag
    if (Chance(r, 2)) {
        const uint32_t start = m.PeekWord(st + 0x14u), end = m.PeekWord(st + 0x18u);
        const uint32_t span = end > start ? (end - start) / 0x4000u : 1u;
        m.PokeWord(st + 0x0Cu, start + 0x4000u * Pick(r, {0u, span, span + 1u, r.U32() % (span + 1u), span - 1u}));
        m.PokeWord(st + 0x1Cu, Pick(r, {1u, 3u, 0u, 2u}));
    }
    if (Chance(r, 4)) m.PokeWord(st + 0x6Cu, r.U32() % 2u);
}

// The table: some loaded records freed or re-armed, some freed records reading, the counters stirred.
void StirTable(CaseContext& c, bool freeSome) {
    Rng& r = *c.rng;
    Memory& m = *c.mem;
    const uint32_t res = Res(m);
    for (uint32_t i : Records(m, true)) {
        if (freeSome && Chance(r, 10)) { // freed (only where no load follows: a freed buffer is still referenced)
            const uint32_t rec = Rec(m, i);
            m.PokeWord(rec, 0);
            m.PokeWord(rec + 20u, 0);
        }
    }
    for (uint32_t o : {0x00u, 0x04u, 0x08u, 0x0Cu, 0x10u, 0x14u, 0x18u, 0x1Cu})
        if (Chance(r, 4)) m.PokeWord(res + o, r.U32() % 8u);
    if (Chance(r, 3)) m.PokeWord(0x800541D0u, 1u); // inside the CD layer's own release
}

// ---------------------------------------------------------------- the seams, the rows
std::vector<OracleCallee> Seams() {
    return {OracleCallee{s::kStCritEnterFn, 0}, OracleCallee{s::kStCritLeaveFn, 0},
            OracleCallee{s::kStCdQueueFn, 1},   OracleCallee{s::kStMusicFrameFn, 0},
            OracleCallee{s::kStMusicDoneFn, 2}, OracleCallee{s::kStEntityCellFn, 1},
            OracleCallee{s::kStRegionZeroFn, 2}, OracleCallee{s::kStCellTexFn, 2},
            OracleCallee{s::kStCellTexPassFn, 3}, OracleCallee{s::kStTexPassAllFn, 2},
            OracleCallee{s::kStTexUnlinkFn, 2}, OracleCallee{s::kStLoadImageFn, 2},
            OracleCallee{s::kStSpeechLoadFn, 3}, OracleCallee{s::kStActivateFn, 0},
            OracleCallee{s::kStDownedFn, 0},    OracleCallee{s::kStNearPieceFn, 1},
            OracleCallee{s::kStPoolDropFn, 2},  OracleCallee{s::kStSleepFn, 2},
            OracleCallee{s::kStRiderSleepFn, 2}, OracleCallee{s::kStJailCellFn, 2},
            OracleCallee{s::kStOtherSlotFn, 2}, OracleCallee{s::kStFxStopFn, 1},
            OracleCallee{s::kStFxFreeFn, 1},    OracleCallee{s::kStPedFreeFn, 1},
            OracleCallee{s::kStStampFn, 4},      OracleCallee{s::kStPrintFn, 1}};
}

template <class F>
uint32_t Run(NativeEnv& env, F&& f) {
    s::GuestRam g = RqView(env);
    rcv::RecoverOracle o(env);
    uint32_t v0 = 0;
    const bool ok = f(g, o, v0);
    if (!ok && env.failure.empty() && !g.Faulted()) env.failure = "the port refused";
    return RqDone(env, g, v0);
}

Row SRow(const char* name, const char* sig, uint32_t entry, std::function<void(CaseContext&, Args&)> setup,
         std::function<uint32_t(NativeEnv&, const Args&)> native, bool v0 = true) {
    Row r{name, sig, entry, std::move(setup), nullptr, v0, std::move(native)};
    r.maxSteps = 40'000'000;
    r.oracleCallees = Seams();
    r.allowBiosCalls = true; // PieceBind's strncmp SLUS 0x80044874 is the kernel's A(0x19)
    return r;
}

// A loaded chunk of `types` planted in a free record (freed first when none), marked or not.
uint32_t PlantSome(CaseContext& c, std::initializer_list<uint32_t> types, bool marked) {
    Memory& m = *c.mem;
    std::vector<uint32_t> free = Records(m, false);
    if (free.empty()) return 0; // NAMED BOUND: a chunk goes only into a free record (a loaded one's buffer is referenced)
    return PlantChunk(c, free[c.rng->U32() % free.size()], SomeChunk(*c.rng, types), marked);
}

// A texture slot of player 0 planted from a texture chunk (as TexLoad leaves it), its page or none.
void PlantTexSlots(CaseContext& c) {
    Rng& r = *c.rng;
    Memory& m = *c.mem;
    for (uint32_t k = 0; k < 24; ++k) {
        const uint32_t sl = s::kStTexSlots + 0x30u * k;
        if (Chance(r, 5) && m.PeekWord(sl + 8u) != 0xFFFFFFFFu) { // freed
            m.PokeWord(sl + 8u, 0xFFFFFFFFu);
            m.PokeWord(sl, 0xFFFFFFFFu);
            m.PokeWord(sl + 12u, 0);
            m.PokeWord(sl + 16u, 0);
        }
    }
    for (uint32_t k = 0; k < 7; ++k) // VRAM pages freed now and then
        if (Chance(r, 5)) m.PokeWord((k < 3 ? s::kStVramB + 8u * k : s::kStVramA + 8u * (k - 3u)), 0xFFFFFFFFu);
}

void StirWorldStream(CaseContext& c, bool freeSome = false) {
    Machine(c);
    StirState(c);
    StirTable(c, freeSome);
    if (Chance(*c.rng, 3)) PlantTexSlots(c);
}

} // namespace strows

void AddStreamRows(std::vector<Row>& rows) {
    using namespace strows;
    using G = s::GuestRam;
    using C = rr::sim::RecoverCallees;
    auto St = [](CaseContext& c) { StirWorldStream(c); };

    // ---- the stream record
    rows.push_back(SRow("stream_track", "void 0x80023A14()  [SLUS, the stream record follows its entity]", s::kStTrackFn,
        [St](CaseContext& c, Args&) { St(c); },
        [](NativeEnv& env, const Args&) { return Run(env, [&](G& g, C&, uint32_t&) { s::StreamTrack(g); return true; }); }, false));
    rows.push_back(SRow("stream_cursor", "void 0x80023C24(cursor, file, start, len)  [SLUS]", s::kStCursorFn,
        [St](CaseContext& c, Args& a) { St(c); a.a[0] = c.scratch; a.a[1] = c.rng->U32() % 8u; a.a[2] = c.rng->U32() & ~0x3FFFu; a.a[3] = (c.rng->U32() % 64u) << 14; },
        [](NativeEnv& env, const Args& a) { return Run(env, [&](G& g, C&, uint32_t&) { s::StreamCursor(g, a.a[0], a.a[1], a.a[2], a.a[3]); return true; }); }, false));
    rows.push_back(SRow("stream_range", "void 0x80023DE4()  [SLUS, the cursor over the TOC range]", s::kStRangeFn,
        [St](CaseContext& c, Args&) { St(c); },
        [](NativeEnv& env, const Args&) { return Run(env, [&](G& g, C&, uint32_t&) { s::StreamRange(g); return true; }); }, false));
    rows.push_back(SRow("stream_limit", "void 0x800243BC(off)  [SLUS]", s::kStLimitFn,
        [St](CaseContext& c, Args& a) { St(c); a.a[0] = Pick(*c.rng, {0u, 0x4000u, 0x68000u, c.rng->U32() % 0x200000u, 0xFFFFC000u}); },
        [](NativeEnv& env, const Args& a) { return Run(env, [&](G& g, C&, uint32_t&) { s::StreamLimit(g, a.a[0]); return true; }); }, false));
    rows.push_back(SRow("stream_seek", "void 0x80023FBC()  [SLUS, the seek state machine]", s::kStSeekFn,
        [St](CaseContext& c, Args&) { St(c); },
        [](NativeEnv& env, const Args&) { return Run(env, [&](G& g, C&, uint32_t&) { s::StreamSeek(g); return true; }); }, false));
    rows.push_back(SRow("stream_at_end", "u32 0x80023300(cursor)  [SLUS]", s::kStAtEndFn,
        [St](CaseContext& c, Args& a) { St(c); a.a[0] = State(0) + 8u; if (Chance(*c.rng, 4)) c.mem->PokeWord(State(0) + 8u, c.rng->U32() % 6u); },
        [](NativeEnv& env, const Args& a) { return Run(env, [&](G& g, C&, uint32_t& v) { v = s::StreamAtEnd(g, a.a[0]); return true; }); }));
    rows.push_back(SRow("stream_reset", "void 0x80024354()  [SLUS]", s::kStResetFn,
        [St](CaseContext& c, Args&) { St(c); },
        [](NativeEnv& env, const Args&) { return Run(env, [&](G& g, C&, uint32_t&) { s::StreamReset(g); return true; }); }, false));
    rows.push_back(SRow("stream_place", "void 0x800235C8(road, along, dir)  [SLUS, the [START] position]", s::kStPlaceFn,
        [St](CaseContext& c, Args& a) { St(c); a.a[0] = SomeRoad(*c.rng, *c.mem); a.a[1] = c.rng->U32() % 3000u; a.a[2] = Pick(*c.rng, {1u, 0xFFFFFFFFu, 0u}); },
        [](NativeEnv& env, const Args& a) { return Run(env, [&](G& g, C&, uint32_t&) { s::StreamPlace(g, a.a[0], static_cast<int32_t>(a.a[1]), static_cast<int32_t>(a.a[2])); return true; }); }, false));
    rows.push_back(SRow("stream_entry", "void 0x80023960(windows, p)  [SLUS]", s::kStEntryFn,
        [St](CaseContext& c, Args& a) {
            St(c);
            const uint32_t rec = PlantSome(c, {0u, 3u, 4u, 8u}, true);
            const uint32_t h = c.mem->PeekWord(rec + 20u);
            if (Chance(*c.rng, 2)) Put16(*c.mem, h + 4u + 6u * (c.rng->U32() % 4u), static_cast<uint16_t>(c.mem->PeekWord(State(0) + 0x48u)));
            a.a[0] = h; a.a[1] = 0;
        },
        [](NativeEnv& env, const Args& a) { return Run(env, [&](G& g, C&, uint32_t&) { s::StreamEntry(g, a.a[0], a.a[1]); return true; }); }, false));

    // ---- the table
    rows.push_back(SRow("res_count", "void 0x800322D0(from, to)  [SLUS]", s::kStCountFn,
        [St](CaseContext& c, Args& a) { St(c); a.a[0] = c.rng->U32() % 5u; a.a[1] = c.rng->U32() % 5u; },
        [](NativeEnv& env, const Args& a) { return Run(env, [&](G& g, C& o, uint32_t&) { s::ResCount(g, static_cast<int32_t>(a.a[0]), static_cast<int32_t>(a.a[1]), env.sp, o); return true; }); }, false));
    rows.push_back(SRow("res_free", "void 0x800313EC(rec)  [SLUS, a record done with]", s::kStFreeFn,
        [St](CaseContext& c, Args& a) {
            St(c);
            const uint32_t rec = Rec(*c.mem, c.rng->U32() % 32u);
            uint32_t f = c.mem->PeekWord(rec);
            if (Chance(*c.rng, 2)) f = Pick(*c.rng, {0x33u, 0x31u, 0x15u, 0x14u, 0x0Cu, 0x19u, 0x1133u, 0x2014u, 0x3u, 0x28u});
            c.mem->PokeWord(rec, f);
            a.a[0] = rec;
        },
        [](NativeEnv& env, const Args& a) { return Run(env, [&](G& g, C& o, uint32_t&) { s::ResFree(g, a.a[0], env.sp, o); return true; }); }, false));
    rows.push_back(SRow("res_free_by_buffer", "void 0x80031008(buf)  [SLUS]", s::kStFreeByBufFn,
        [St](CaseContext& c, Args& a) { St(c); a.a[0] = Chance(*c.rng, 6) ? c.rng->U32() : c.mem->PeekWord(Rec(*c.mem, c.rng->U32() % 32u) + 16u); },
        [](NativeEnv& env, const Args& a) { return Run(env, [&](G& g, C& o, uint32_t&) { s::ResFreeByBuffer(g, a.a[0], env.sp, o); return true; }); }, false));
    rows.push_back(SRow("res_take_music", "s32 0x80031064(list, n)  [SLUS]", s::kStTakeMusicFn,
        [St](CaseContext& c, Args& a) { St(c); if (Chance(*c.rng, 2)) c.mem->PokeWord(Rec(*c.mem, 30u + c.rng->U32() % 2u), 0); a.a[0] = c.scratch; a.a[1] = c.rng->U32() % 4u; },
        [](NativeEnv& env, const Args& a) { return Run(env, [&](G& g, C& o, uint32_t& v) { v = static_cast<uint32_t>(s::ResTakeMusic(g, a.a[0], static_cast<int32_t>(a.a[1]), env.sp, o)); return true; }); }));
    rows.push_back(SRow("res_take_stream", "s32 0x80031170(list, n, all, p)  [SLUS]", s::kStTakeStreamFn,
        [St](CaseContext& c, Args& a) { St(c); a.a[0] = c.scratch; a.a[1] = c.rng->U32() % 8u; a.a[2] = c.rng->U32() % 2u; a.a[3] = 0; },
        [](NativeEnv& env, const Args& a) { return Run(env, [&](G& g, C& o, uint32_t& v) { v = static_cast<uint32_t>(s::ResTakeStream(g, a.a[0], static_cast<int32_t>(a.a[1]), static_cast<int32_t>(a.a[2]), static_cast<int32_t>(a.a[3]), env.sp, o)); return true; }); }));
    rows.push_back(SRow("res_arm", "void 0x80031368(rec, kind)  [SLUS]", s::kStArmFn,
        [St](CaseContext& c, Args& a) { St(c); a.a[0] = Rec(*c.mem, c.rng->U32() % 32u); a.a[1] = Pick(*c.rng, {8u, 0x10u, 0x10u, 0u}); },
        [](NativeEnv& env, const Args& a) { return Run(env, [&](G& g, C& o, uint32_t&) { s::ResArm(g, a.a[0], a.a[1], env.sp, o); return true; }); }, false));
    rows.push_back(SRow("res_take", "s32 0x80030CD8(list, n, kind, contiguous, arg)  [SLUS, the records a read takes]", s::kStTakeFn,
        [St](CaseContext& c, Args& a) {
            St(c);
            for (uint32_t k = 0; k < 6; ++k) if (Chance(*c.rng, 2)) c.mem->PokeWord(Rec(*c.mem, c.rng->U32() % 32u), 0);
            a.a[0] = c.scratch; a.a[1] = Pick(*c.rng, {0u, 1u, 2u, 5u, 8u, 40u}); a.a[2] = Pick(*c.rng, {8u, 0x10u, 0x10u});
            a.a[3] = c.rng->U32() % 2u; a.a4 = 0;
        },
        [](NativeEnv& env, const Args& a) { return Run(env, [&](G& g, C& o, uint32_t& v) { v = static_cast<uint32_t>(s::ResTake(g, a.a[0], a.a[1], a.a[2], a.a[3], a.a4, env.sp, o)); return true; }); }));
    rows.push_back(SRow("stream_unwind", "void 0x80023358(list, n)  [SLUS]", s::kStUnwindFn,
        [St](CaseContext& c, Args& a) {
            St(c);
            const uint32_t n = c.rng->U32() % 4u;
            for (uint32_t k = 0; k < n; ++k) c.mem->PokeWord(c.scratch + 4u * k, Rec(*c.mem, c.rng->U32() % 32u));
            a.a[0] = c.scratch; a.a[1] = n;
        },
        [](NativeEnv& env, const Args& a) { return Run(env, [&](G& g, C& o, uint32_t&) { s::StreamUnwind(g, a.a[0], static_cast<int32_t>(a.a[1]), env.sp, o); return true; }); }, false));
    rows.push_back(SRow("stream_request", "s32 0x80023148(cursor, n, contiguous, arg)  [SLUS, a read request]", s::kStRequestFn,
        [St](CaseContext& c, Args& a) {
            St(c);
            for (uint32_t k = 0; k < 6; ++k) if (Chance(*c.rng, 2)) c.mem->PokeWord(Rec(*c.mem, c.rng->U32() % 30u), 0);
            if (Chance(*c.rng, 8)) c.mem->PokeWord(0x80053470u, 0x31u); // the CD queue full: -100
            a.a[0] = State(0) + 8u; a.a[1] = c.rng->U32() % 7u; a.a[2] = 0; a.a[3] = 0;
        },
        [](NativeEnv& env, const Args& a) { return Run(env, [&](G& g, C& o, uint32_t& v) { v = static_cast<uint32_t>(s::StreamRequest(g, a.a[0], a.a[1], a.a[2], a.a[3], env.sp, o)); return true; }); }));
    rows.push_back(SRow("res_free_count", "s32 0x800312D0(p)  [SLUS]", 0x800312D0u,
        [St](CaseContext& c, Args& a) { St(c); for (uint32_t k = 0; k < 5; ++k) if (Chance(*c.rng, 2)) c.mem->PokeWord(Rec(*c.mem, c.rng->U32() % 32u), 0); a.a[0] = 0; },
        [](NativeEnv& env, const Args& a) { return Run(env, [&](G& g, C&, uint32_t& v) { v = static_cast<uint32_t>(s::ResFreeCount(g, static_cast<int32_t>(a.a[0]))); return true; }); }));
    rows.push_back(SRow("stream_free_count", "void 0x80023C4C()  [SLUS]", s::kStFreeCountFn,
        [St](CaseContext& c, Args&) { St(c); for (uint32_t k = 0; k < 5; ++k) if (Chance(*c.rng, 2)) c.mem->PokeWord(Rec(*c.mem, c.rng->U32() % 32u), 0); },
        [](NativeEnv& env, const Args&) { return Run(env, [&](G& g, C&, uint32_t&) { s::StreamFreeCount(g); return true; }); }, false));
    rows.push_back(SRow("stream_read", "void 0x80023CAC()  [SLUS, the frame's read request]", s::kStReadFn,
        [St](CaseContext& c, Args&) {
            St(c);
            for (uint32_t k = 0; k < 6; ++k) if (Chance(*c.rng, 2)) c.mem->PokeWord(Rec(*c.mem, c.rng->U32() % 30u), 0);
            if (Chance(*c.rng, 3)) c.mem->PokeWord(Res(*c.mem) + 0x18u, Pick(*c.rng, {0u, 1u, 5u, 7u, 0xFFFFFFFFu}));
        },
        [](NativeEnv& env, const Args&) { return Run(env, [&](G& g, C& o, uint32_t&) { s::StreamRead(g, env.sp, o); return true; }); }, false));
    rows.push_back(SRow("res_window", "s32 0x80023900(type, windows, p)  [SLUS, the residency test]", s::kStWindowFn,
        [St](CaseContext& c, Args& a) {
            St(c);
            const uint32_t rec = PlantSome(c, {0u, 1u, 2u, 3u, 4u, 8u, 9u, 10u}, true);
            const uint32_t h = c.mem->PeekWord(rec + 20u);
            if (Chance(*c.rng, 2)) Put16(*c.mem, h + 4u + 6u * (c.rng->U32() % 4u), static_cast<uint16_t>(c.mem->PeekWord(State(0) + 0x48u)));
            a.a[0] = c.mem->PeekWord(h) >> 28; a.a[1] = h + 4u; a.a[2] = 0;
        },
        [](NativeEnv& env, const Args& a) { return Run(env, [&](G& g, C&, uint32_t& v) { v = static_cast<uint32_t>(s::ResWindow(g, a.a[0], a.a[1], a.a[2])); return true; }); }));

    // ---- the frame
    auto PlantMany = [](CaseContext& c) {
        const uint32_t n = c.rng->U32() % 4u;
        for (uint32_t k = 0; k < n; ++k) PlantSome(c, {0u, 1u, 2u, 3u, 3u, 4u, 8u, 8u, 9u, 10u}, Chance(*c.rng, 2));
    };
    rows.push_back(SRow("res_release", "void 0x80030E58(p)  [SLUS, the release pass]", s::kStReleaseFn,
        [St, PlantMany](CaseContext& c, Args& a) { St(c); PlantMany(c); a.a[0] = 0; },
        [](NativeEnv& env, const Args& a) { return Run(env, [&](G& g, C& o, uint32_t&) { s::ResRelease(g, a.a[0], env.sp, o); return true; }); }, false));
    rows.push_back(SRow("stream_players", "void 0x800237B8()  [SLUS, every player's stream frame]", s::kStPlayersFn,
        [St, PlantMany](CaseContext& c, Args&) { St(c); PlantMany(c); },
        [](NativeEnv& env, const Args&) { return Run(env, [&](G& g, C& o, uint32_t&) { s::StreamPlayers(g, env.sp, o); return true; }); }, false));
    // A completed read of a planted chunk: the record as 0x80022B0C leaves it reading (0x114).
    rows.push_back(SRow("res_read_done", "void 0x80030894(index, kind, arg)  [SLUS, the read's completion]", s::kStDoneFn,
        [St](CaseContext& c, Args& a) {
            St(c);
            const uint32_t kind = Chance(*c.rng, 5) ? 8u : 0x10u;
            const uint32_t i = kind == 8 ? 30u + c.rng->U32() % 2u : c.rng->U32() % 30u;
            const uint32_t rec = PlantChunk(c, i, SomeChunk(*c.rng, {0u, 1u, 2u, 3u, 4u, 8u, 9u, 10u}), false);
            c.mem->PokeWord(rec, Pick(*c.rng, {0x114u, 0x114u, 0x104u, 0x10Cu}));
            if (Chance(*c.rng, 8)) c.mem->PokeWord(Res(*c.mem) + 0x934u, 0x40u);
            a.a[0] = i; a.a[1] = kind; a.a[2] = 0;
        },
        [](NativeEnv& env, const Args& a) { return Run(env, [&](G& g, C& o, uint32_t&) { s::ResReadDone(g, a.a[0], a.a[1], a.a[2], env.sp, o); return true; }); }, false));
    // The pump over queued chunks: fresh chunks filed as ReadDone files them (0x80 queued), twins included.
    auto Queue = [](CaseContext& c) {
        Memory& m = *c.mem;
        const uint32_t q = Res(m);
        const uint32_t n = c.rng->U32() % 5u;
        m.PokeWord(q + 0x930u, c.rng->U32() % 64u);
        m.PokeWord(q + 0x92Cu, m.PeekWord(q + 0x930u));
        m.PokeWord(q + 0x934u, 0);
        for (uint32_t k = 0; k < n; ++k) {
            std::vector<uint32_t> free = Records(m, false);
            if (free.empty()) break;
            const uint32_t i = free[c.rng->U32() % free.size()];
            std::vector<uint8_t> chunk;
            if (Chance(*c.rng, 4)) { // a twin of a loaded record (a duplicate, or a texture's other half)
                std::vector<uint32_t> used = Records(m, true);
                const uint32_t o = used[c.rng->U32() % used.size()];
                chunk.resize(0x4000);
                m.ReadBlock(m.PeekWord(Rec(m, o) + 12u), chunk.data(), 0x4000);
                // a header moved to the chunk's end (a texture half): its bytes are not a chunk any more
                if (m.PeekWord(Rec(m, o) + 20u) != m.PeekWord(Rec(m, o) + 12u)) chunk = SomeChunk(*c.rng, {1u, 2u});
            } else {
                chunk = SomeChunk(*c.rng, {0u, 1u, 1u, 2u, 3u, 4u, 8u, 9u, 10u});
            }
            const uint32_t rec = PlantChunk(c, i, chunk, false);
            m.PokeWord(rec, 0x91u);
            m.PokeWord(q + 0x938u + 4u * m.PeekWord(q + 0x92Cu), rec);
            m.PokeWord(q + 0x92Cu, (m.PeekWord(q + 0x92Cu) + 1u) & 63u);
            m.PokeWord(q + 0x934u, m.PeekWord(q + 0x934u) + 1u);
        }
    };
    rows.push_back(SRow("res_pump", "void 0x80030608()  [SLUS, the new blocks to their loaders]", s::kStPumpFn,
        [St, Queue](CaseContext& c, Args&) { St(c); Queue(c); },
        [](NativeEnv& env, const Args&) { return Run(env, [&](G& g, C& o, uint32_t&) { s::ResPump(g, env.sp, o); return true; }); }, false));
    rows.push_back(SRow("res_load", "void 0x80031604(rec, p)  [SLUS, a chunk to its loader]", s::kStLoadFn,
        [St](CaseContext& c, Args& a) {
            St(c);
            const uint32_t rec = PlantSome(c, {0u, 1u, 2u, 3u, 3u, 4u, 8u, 9u, 10u}, !Chance(*c.rng, 6));
            if (Chance(*c.rng, 10)) c.mem->PokeWord(rec, c.mem->PeekWord(rec) | 8u); // a music chunk
            a.a[0] = rec; a.a[1] = 0;
        },
        [](NativeEnv& env, const Args& a) { return Run(env, [&](G& g, C& o, uint32_t&) { s::ResLoad(g, a.a[0], a.a[1], env.sp, o); return true; }); }, false));
    rows.push_back(SRow("res_unload", "void 0x800318E8(rec, p)  [SLUS, a chunk to its unloader]", s::kStUnloadFn,
        [St](CaseContext& c, Args& a) {
            St(c);
            std::vector<uint32_t> used = Records(*c.mem, true);
            const uint32_t rec = used.empty() ? Rec(*c.mem, 2) : Rec(*c.mem, used[c.rng->U32() % used.size()]);
            if (Chance(*c.rng, 6)) c.mem->PokeWord(rec, c.mem->PeekWord(rec) & ~2u);
            a.a[0] = rec; a.a[1] = 0;
        },
        [](NativeEnv& env, const Args& a) { return Run(env, [&](G& g, C& o, uint32_t&) { s::ResUnload(g, a.a[0], a.a[1], env.sp, o); return true; }); }, false));
    rows.push_back(SRow("res_entry_tag", "void 0x80031E1C(road, rec, p)  [SLUS, the cells' entry coordinates]", s::kStEntryTagFn,
        [St, PlantMany](CaseContext& c, Args& a) {
            St(c); PlantMany(c);
            a.a[0] = Chance(*c.rng, 2) ? c.mem->PeekWord(State(0) + 0x48u) : SomeRoad(*c.rng, *c.mem);
            std::vector<uint32_t> used = Records(*c.mem, true);
            a.a[1] = (Chance(*c.rng, 2) || used.empty()) ? 0u : Rec(*c.mem, used[c.rng->U32() % used.size()]);
            a.a[2] = 0;
        },
        [](NativeEnv& env, const Args& a) { return Run(env, [&](G& g, C&, uint32_t&) { s::ResEntryTag(g, a.a[0], a.a[1], a.a[2]); return true; }); }, false));
    rows.push_back(SRow("stream_stall", "s32 0x80022C64()  [SLUS]", s::kStStallFn,
        [St](CaseContext& c, Args&) {
            St(c);
            c.mem->PokeWord(kGp + s::kGpStCdBusy, c.rng->U32() % 2u);
            c.mem->PokeWord(kGp + s::kGpStCdStamp, c.mem->PeekWord(c.mem->PeekWord(s::kStGameState) + 12u) + Pick(*c.rng, {0u, 240u, 241u, 3000u, 3001u, 0xFFFFFF00u, c.rng->U32() % 5000u}));
        },
        [](NativeEnv& env, const Args&) { return Run(env, [&](G& g, C&, uint32_t& v) { v = static_cast<uint32_t>(s::StreamStall(g)); return true; }); }));
    rows.push_back(SRow("stream_stall_cells", "u32 0x80023870()  [SLUS]", s::kStStallCellsFn,
        [St](CaseContext& c, Args&) { St(c); },
        [](NativeEnv& env, const Args&) { return Run(env, [&](G& g, C& o, uint32_t& v) { v = s::StreamStallCells(g, env.sp, o); return true; }); }));
    rows.push_back(SRow("stream_frame", "void 0x8002305C()  [SLUS, GameFrame's stream step]", s::kStFrameFn,
        [St, PlantMany, Queue](CaseContext& c, Args&) {
            St(c); PlantMany(c); if (Chance(*c.rng, 2)) Queue(c);
            if (Chance(*c.rng, 4)) Put8(*c.mem, c.mem->PeekWord(s::kStGameState), 4);
        },
        [](NativeEnv& env, const Args&) { return Run(env, [&](G& g, C& o, uint32_t&) { s::StreamFrame(g, env.sp, o); return true; }); }, false));

    // ---- the loaders
    rows.push_back(SRow("cell_load", "u32 0x80032A20(payload, id, type, ext, entry, p)  [SLUS, a cell into the slots]", s::kStCellLoadFn,
        [St](CaseContext& c, Args& a) {
            St(c);
            Memory& m = *c.mem;
            const uint32_t rec = PlantSome(c, {0u, 8u, 8u, 9u}, true);
            const uint32_t h = m.PeekWord(rec + 20u);
            uint32_t key = m.PeekWord(h);
            if ((key >> 28) == 9 && Chance(*c.rng, 2)) { // the type-9 half of a resident cell
                const uint32_t sl = s::kStCellSlots + 0x70u * (c.rng->U32() % 10u);
                if (m.PeekWord(sl) != 0xFFFFFFFFu) key = 0x90000000u | (m.PeekWord(sl) & 0x0FFFFFFFu);
                if (Chance(*c.rng, 3)) m.PokeWord(sl + 12u, m.PeekWord(sl + 12u) | 2u);
            }
            if (Chance(*c.rng, 6)) m.PokeWord(kGp + s::kGpStCellCount, 24);
            for (uint32_t k = 0; k < 12; ++k) if (Chance(*c.rng, 8)) m.PokeWord(s::kStCellSlots + 0x70u * k + 8u, m.PeekWord(s::kStCellSlots + 0x70u * k) == 0xFFFFFFFFu ? 0x01800123u : 0xFFFFFFFFu);
            a.a[0] = m.PeekWord(rec + 16u); a.a[1] = key & 0x0FFFFFFFu; a.a[2] = key >> 28; a.a[3] = m.PeekWord(rec + 24u);
            a.a4 = rec + 28u; a.more[0] = 0;
        },
        [](NativeEnv& env, const Args& a) { return Run(env, [&](G& g, C& o, uint32_t& v) { v = s::CellLoad(g, a.a[0], a.a[1], a.a[2], a.a[3], a.a4, a.more[0], env.sp, o); return true; }); }));
    rows.push_back(SRow("cell_fix", "u32 0x8003234C(slot, payload)  [SLUS, the cell relocated in place]", s::kStCellFixFn,
        [St](CaseContext& c, Args& a) {
            St(c);
            const uint32_t rec = PlantSome(c, {0u, 8u}, true);
            a.a[0] = s::kStCellSlots + 0x70u * (c.rng->U32() % 24u); a.a[1] = c.mem->PeekWord(rec + 16u);
        },
        [](NativeEnv& env, const Args& a) { return Run(env, [&](G& g, C&, uint32_t& v) { s::CellFix(g, a.a[0], a.a[1]); v = 1; return true; }); }));
    rows.push_back(SRow("cell_unload", "u32 0x80032810(id, type, p)  [SLUS, a slot emptied]", s::kStCellUnloadFn,
        [St](CaseContext& c, Args& a) {
            St(c);
            Memory& m = *c.mem;
            const uint32_t sl = s::kStCellSlots + 0x70u * (c.rng->U32() % 12u);
            if (Chance(*c.rng, 2)) worldrows::PlantProps(*c.rng, m);
            if (Chance(*c.rng, 2)) worldrows::PlantVolumes(*c.rng, m, m.PeekWord(sl + 8u));
            a.a[0] = Chance(*c.rng, 6) ? 0x01800999u : m.PeekWord(sl + 8u);
            a.a[1] = Pick(*c.rng, {0u, 8u, 9u}); a.a[2] = 0;
        },
        [](NativeEnv& env, const Args& a) { return Run(env, [&](G& g, C& o, uint32_t& v) { v = s::CellUnload(g, a.a[0], a.a[1], a.a[2], env.sp, o); return true; }); }));
    rows.push_back(SRow("cell_release", "void 0x8008C45C(id, p)  [RASHCDG, what lives in a freed cell]", s::kStCellFreeFn,
        [St](CaseContext& c, Args& a) {
            St(c);
            Memory& m = *c.mem;
            const uint32_t sl = s::kStCellSlots + 0x70u * (c.rng->U32() % 10u);
            const uint32_t id = m.PeekWord(sl + 8u);
            if (Chance(*c.rng, 2)) worldrows::PlantProps(*c.rng, m);
            if (Chance(*c.rng, 2)) worldrows::PlantVolumes(*c.rng, m, id);
            for (uint32_t k = 0; k < 4; ++k) // pool-4 props in this cell
                if (Chance(*c.rng, 2)) m.PokeWord(m.PeekWord(s::kWpPool4Ctrl + 12u) + 596u * k + 0xB0u, id);
            a.a[0] = id; a.a[1] = 0;
        },
        [](NativeEnv& env, const Args& a) { return Run(env, [&](G& g, C& o, uint32_t&) { s::CellRelease(g, a.a[0], a.a[1], env.sp, o); return true; }); }, false));
    rows.push_back(SRow("piece_load", "u32 0x8003CEC4(payload, id, p)  [SLUS, a road object]", s::kStPieceLoadFn,
        [St](CaseContext& c, Args& a) {
            St(c);
            Memory& m = *c.mem;
            const uint32_t rec = PlantSome(c, {3u}, true);
            const uint32_t key = m.PeekWord(m.PeekWord(rec + 20u));
            for (uint32_t k = 0; k < 6; ++k) if (Chance(*c.rng, 6)) m.PokeWord(s::kStPieces + 16u * k, Chance(*c.rng, 2) ? 0xFFFFFFFFu : c.rng->U32() % 300u);
            if (Chance(*c.rng, 5)) Put8(m, m.PeekWord(s::kStGameState) + 4u, static_cast<uint8_t>(m.PeekByte(m.PeekWord(s::kStGameState) + 4u) ^ 0x10u));
            if (Chance(*c.rng, 4)) m.PokeWord(m.PeekWord(m.PeekWord(s::kStPlayer1) + 0x43Cu) + 0x28u, c.rng->U32() % 2u);
            a.a[0] = m.PeekWord(rec + 16u); a.a[1] = Chance(*c.rng, 8) ? 0xFFFFu : key & 0xFFFFu; a.a[2] = 0;
        },
        [](NativeEnv& env, const Args& a) { return Run(env, [&](G& g, C& o, uint32_t& v) { v = s::PieceLoad(g, a.a[0], a.a[1], a.a[2], env.sp, o); return true; }); }));
    rows.push_back(SRow("piece_add", "s32 0x8003CA50(id, obj, half)  [SLUS]", s::kStPieceAddFn,
        [St](CaseContext& c, Args& a) {
            St(c);
            Memory& m = *c.mem;
            for (uint32_t k = 0; k < 6; ++k) if (Chance(*c.rng, 5)) m.PokeWord(s::kStPieces + 16u * k, Chance(*c.rng, 2) ? 0xFFFFFFFFu : c.rng->U32() % 300u);
            if (Chance(*c.rng, 4)) Put8(m, m.PeekWord(s::kStGameState) + 4u, static_cast<uint8_t>(m.PeekByte(m.PeekWord(s::kStGameState) + 4u) ^ 0x10u));
            a.a[0] = Chance(*c.rng, 3) ? m.PeekWord(s::kStPieces + 16u * (c.rng->U32() % 3u)) : c.rng->U32() % 300u;
            a.a[1] = c.rng->U32(); a.a[2] = c.rng->U32() % 2u;
        },
        [](NativeEnv& env, const Args& a) { return Run(env, [&](G& g, C&, uint32_t& v) { v = static_cast<uint32_t>(s::PieceAdd(g, a.a[0], a.a[1], a.a[2])); return true; }); }));
    rows.push_back(SRow("piece_bind", "u32 0x8003CFCC(obj, id)  [SLUS, a road object's blocks and BTT_]", s::kStPieceBindFn,
        [St](CaseContext& c, Args& a) {
            St(c);
            Memory& m = *c.mem;
            const uint32_t rec = PlantSome(c, {3u}, true);
            const uint32_t key = m.PeekWord(m.PeekWord(rec + 20u));
            if (Chance(*c.rng, 4)) Put8(m, m.PeekWord(s::kStGameState) + 4u, static_cast<uint8_t>(m.PeekByte(m.PeekWord(s::kStGameState) + 4u) ^ 0x10u));
            if (Chance(*c.rng, 5)) m.PokeWord(s::kStFirstPiece, 0);
            a.a[0] = m.PeekWord(rec + 16u); a.a[1] = Chance(*c.rng, 8) ? 0x7FFFu : key & 0xFFFFu;
        },
        [](NativeEnv& env, const Args& a) { return Run(env, [&](G& g, C&, uint32_t& v) { v = s::PieceBind(g, a.a[0], a.a[1]); return true; }); }));
    rows.push_back(SRow("piece_unload", "u32 0x8003D844(id, p)  [SLUS, a road object released]", s::kStPieceUnloadFn,
        [St](CaseContext& c, Args& a) {
            St(c);
            Memory& m = *c.mem;
            if (Chance(*c.rng, 3)) worldrows::PlantProps(*c.rng, m);
            if (Chance(*c.rng, 3)) worldrows::PlantVolumes(*c.rng, m, 0x01800080u);
            if (Chance(*c.rng, 4)) m.PokeWord(s::kStViews + 0x148u, 0);
            if (Chance(*c.rng, 4)) m.PokeWord(m.PeekWord(s::kStPlayer1) + 0x148u, 0);
            a.a[0] = Pick(*c.rng, {0x43u, 0xFu, 0xEu, c.rng->U32() % 300u, 0xFFFFu}); a.a[1] = 0;
        },
        [](NativeEnv& env, const Args& a) { return Run(env, [&](G& g, C& o, uint32_t& v) { v = s::PieceUnload(g, a.a[0], a.a[1], env.sp, o); return true; }); }));
    rows.push_back(SRow("piece_drop", "s32 0x8003CB6C(id, half)  [SLUS]", s::kStPieceDropFn,
        [St](CaseContext& c, Args& a) {
            St(c);
            Memory& m = *c.mem;
            for (uint32_t k = 0; k < 6; ++k) {
                if (Chance(*c.rng, 4)) m.PokeWord(s::kStPieces + 16u * k, Chance(*c.rng, 2) ? 0xFFFFFFFFu : c.rng->U32() % 300u);
                if (Chance(*c.rng, 4)) m.PokeWord(s::kStPieces + 16u * k + 12u, Chance(*c.rng, 2) ? 0u : c.rng->U32());
                if (Chance(*c.rng, 4)) m.PokeWord(s::kStPieces + 16u * k + 4u, c.rng->U32() % 2u);
            }
            m.PokeWord(s::kStPiecesLast, Pick(*c.rng, {0xFFFFFFFFu, 0u, 2u, 5u, c.rng->U32() % 6u}));
            a.a[0] = Chance(*c.rng, 3) ? c.rng->U32() % 300u : m.PeekWord(s::kStPieces + 16u * (c.rng->U32() % 6u)); a.a[1] = c.rng->U32() % 2u;
        },
        [](NativeEnv& env, const Args& a) { return Run(env, [&](G& g, C&, uint32_t& v) { v = static_cast<uint32_t>(s::PieceDrop(g, a.a[0], a.a[1])); return true; }); }));
    rows.push_back(SRow("piece_rebase", "void 0x8003D39C(newObj, oldObj)  [SLUS, cursors on a moved object]", s::kStPieceRebaseFn,
        [St](CaseContext& c, Args& a) {
            St(c);
            if (Chance(*c.rng, 2)) worldrows::PlantProps(*c.rng, *c.mem);
            a.a[0] = Pick(*c.rng, {0x8010D43Cu, 0x8014543Cu, 0x8015D43Cu, 0x8017D43Cu}); a.a[1] = Pick(*c.rng, {0x8010D43Cu, 0x8014543Cu, 0x8015D43Cu});
        },
        [](NativeEnv& env, const Args& a) { return Run(env, [&](G& g, C&, uint32_t&) { s::PieceRebase(g, a.a[0], a.a[1]); return true; }); }, false));

    // ---- textures
    auto TexArgs = [](CaseContext& c, Args& a) {
        Memory& m = *c.mem;
        const uint32_t rec = PlantSome(c, {1u, 2u}, true);
        const uint32_t h = m.PeekWord(rec + 20u);
        const uint32_t id = m.PeekWord(h) & 0x0FFFFFFFu;
        const uint32_t type = m.PeekWord(h) >> 28;
        const uint32_t w7 = m.PeekWord(h + 28u);
        uint32_t key = ((id >> 16) & 0x8000u) | ((id >> 13) & 0x7C00u) | (id & 0x3FFu);
        if (type == 2) key |= 0x8000u;
        if (Chance(*c.rng, 4)) { // a key already in a slot
            const uint32_t sl = s::kStTexSlots + 0x30u * (c.rng->U32() % 8u);
            if (m.PeekWord(sl + 8u) != 0xFFFFFFFFu) key = m.PeekWord(sl + 8u);
        }
        a.a[0] = key; a.a[1] = m.PeekWord(rec + 16u); a.a[2] = type == 2 ? 1u : 0u; a.a[3] = type == 2 ? 0u : (w7 & 1u);
        a.a4 = w7; a.more[0] = h + 4u; a.more[1] = 0;
    };
    rows.push_back(SRow("tex_load", "u32 0x80034DEC(key, buf, kind, half, v, windows, p)  [SLUS, a texture into the slots]", s::kStTexLoadFn,
        [St, TexArgs](CaseContext& c, Args& a) { St(c); PlantTexSlots(c); TexArgs(c, a); },
        [](NativeEnv& env, const Args& a) { return Run(env, [&](G& g, C& o, uint32_t& v) { v = s::TexLoad(g, a.a[0], a.a[1], a.a[2], a.a[3], a.a4, a.more[0], a.more[1], env.sp, o); return true; }); }));
    rows.push_back(SRow("tex_find", "s32 0x80022218(key, p)  [SLUS]", s::kStTexFindFn,
        [St](CaseContext& c, Args& a) { St(c); a.a[0] = Chance(*c.rng, 3) ? 0xFFFFFFFFu : c.mem->PeekWord(s::kStTexSlots + 0x30u * (c.rng->U32() % 10u) + 8u); a.a[1] = c.rng->U32() % 2u; },
        [](NativeEnv& env, const Args& a) { return Run(env, [&](G& g, C&, uint32_t& v) { v = static_cast<uint32_t>(s::TexFind(g, a.a[0], a.a[1])); return true; }); }));
    rows.push_back(SRow("tex_link", "void 0x800324CC(slot, kind, p)  [SLUS]", s::kStTexLinkFn,
        [St](CaseContext& c, Args& a) {
            St(c);
            Memory& m = *c.mem;
            const uint32_t sl = s::kStTexSlots + 0x30u * (c.rng->U32() % 8u);
            if (Chance(*c.rng, 2)) m.PokeWord(s::kStCellSlots + 0x70u * (c.rng->U32() % 10u) + 0x50u + 4u * (c.rng->U32() % 4u), m.PeekWord(sl + 8u));
            a.a[0] = sl; a.a[1] = c.rng->U32() % 2u; a.a[2] = 0;
        },
        [](NativeEnv& env, const Args& a) { return Run(env, [&](G& g, C&, uint32_t&) { s::TexLink(g, a.a[0], a.a[1], a.a[2]); return true; }); }, false));
    rows.push_back(SRow("vram_find", "s32 0x800335B4(kind, key, p)  [SLUS]", s::kStVramFindFn,
        [St](CaseContext& c, Args& a) {
            St(c); PlantTexSlots(c);
            a.a[0] = c.rng->U32() % 2u; a.a[1] = Pick(*c.rng, {0xFFFFFFFFu, 0xC80u, 0x8C80u, 0xC7Cu, 0x8C01u, c.rng->U32() % 0x10000u}); a.a[2] = 0;
        },
        [](NativeEnv& env, const Args& a) { return Run(env, [&](G& g, C&, uint32_t& v) { v = static_cast<uint32_t>(s::VramFind(g, a.a[0], a.a[1], a.a[2])); return true; }); }));
    rows.push_back(SRow("vram_bind", "u32 0x80033B50(page, slot, kind, p)  [SLUS, a texture to its VRAM page]", s::kStVramBindFn,
        [St](CaseContext& c, Args& a) {
            St(c);
            Memory& m = *c.mem;
            // a slot holding one or two chunk halves (the texture records' own payloads)
            const uint32_t k = c.rng->U32() % 24u;
            const uint32_t sl = s::kStTexSlots + 0x30u * k;
            const uint32_t r1 = PlantSome(c, {1u, 2u}, true), r2 = PlantSome(c, {1u}, true);
            m.PokeWord(sl + 12u, Chance(*c.rng, 6) ? 0u : m.PeekWord(r1 + 16u));
            m.PokeWord(sl + 16u, Chance(*c.rng, 2) ? 0u : m.PeekWord(r2 + 16u));
            m.PokeWord(sl + 8u, Pick(*c.rng, {0xC99u, 0x8C99u, 0x8C98u, 0xC98u}));
            a.a[0] = c.rng->U32() % 4u; a.a[1] = k; a.a[2] = c.rng->U32() % 2u; a.a[3] = 0;
        },
        [](NativeEnv& env, const Args& a) { return Run(env, [&](G& g, C& o, uint32_t& v) { v = s::VramBind(g, a.a[0], a.a[1], a.a[2], a.a[3], env.sp, o); return true; }); }));
    rows.push_back(SRow("vram_upload", "void 0x8002227C(page, key, a, b, p)  [SLUS, the VRAM upload's bookkeeping]", s::kStVramUploadFn,
        [St](CaseContext& c, Args& a) {
            St(c);
            const uint32_t r1 = PlantSome(c, {1u, 2u}, true), r2 = PlantSome(c, {1u}, true);
            a.a[0] = c.rng->U32() % 4u; a.a[1] = Pick(*c.rng, {0xC99u, 0x8C99u, 0x8C80u, 0xC80u}); a.a[2] = c.mem->PeekWord(r1 + 16u); a.a[3] = c.mem->PeekWord(r2 + 16u); a.a4 = 0;
        },
        [](NativeEnv& env, const Args& a) { return Run(env, [&](G& g, C& o, uint32_t&) { s::VramUpload(g, a.a[0], a.a[1], a.a[2], a.a[3], a.a4, env.sp, o); return true; }); }, false));
    rows.push_back(SRow("tex_free", "s32 0x80034D38(key, kind, p)  [SLUS, a texture slot freed]", s::kStTexFreeFn,
        [St](CaseContext& c, Args& a) {
            St(c);
            Memory& m = *c.mem;
            const uint32_t sl = s::kStTexSlots + 0x30u * (c.rng->U32() % 8u);
            if (Chance(*c.rng, 2)) m.PokeWord(s::kStCellSlots + 0x70u * (c.rng->U32() % 10u) + 0x60u + 4u * (c.rng->U32() % 4u), sl);
            a.a[0] = Chance(*c.rng, 6) ? 0x1234u : m.PeekWord(sl + 8u); a.a[1] = c.rng->U32() % 2u; a.a[2] = 0;
        },
        [](NativeEnv& env, const Args& a) { return Run(env, [&](G& g, C& o, uint32_t& v) { v = static_cast<uint32_t>(s::TexFree(g, a.a[0], a.a[1], a.a[2], env.sp, o)); return true; }); }));
    rows.push_back(SRow("vram_free", "void 0x80033634(page, kind, p)  [SLUS]", s::kStVramFreeFn,
        [St](CaseContext& c, Args& a) { St(c); a.a[0] = Pick(*c.rng, {0xFFFFFFFFu, 0u, 1u, 2u, 3u}); a.a[1] = c.rng->U32() % 2u; a.a[2] = 0; },
        [](NativeEnv& env, const Args& a) { return Run(env, [&](G& g, C&, uint32_t&) { s::VramFree(g, static_cast<int32_t>(a.a[0]), a.a[1], a.a[2]); return true; }); }, false));
    rows.push_back(SRow("tex_sweep", "void 0x80033198(p)  [SLUS, the texture slots past their windows]", s::kStTexSweepFn,
        [St](CaseContext& c, Args& a) { St(c); PlantTexSlots(c); a.a[0] = 0; },
        [](NativeEnv& env, const Args& a) { return Run(env, [&](G& g, C& o, uint32_t&) { s::TexSweep(g, a.a[0], env.sp, o); return true; }); }, false));

    // ---- panoramas
    rows.push_back(SRow("pano_load", "u32 0x800136BC(payload, windows, key)  [SLUS, a panorama into the sky]", s::kStPanoLoadFn,
        [St](CaseContext& c, Args& a) {
            St(c);
            Memory& m = *c.mem;
            const uint32_t sky = m.PeekWord(s::kStSkyPtr);
            for (uint32_t k = 0; k < 20; ++k) if (Chance(*c.rng, 5)) { m.PokeWord(sky + 0x2E4u + 12u * k, 0); }
            if (Chance(*c.rng, 5)) Put16(m, sky + 0x22u, 20);
            if (Chance(*c.rng, 8)) m.PokeWord(kGp + s::kGpStSkyBusy, 1);
            const uint32_t rec = PlantSome(c, {4u, 4u, 4u, 3u}, true);
            const uint32_t h = m.PeekWord(rec + 20u);
            a.a[0] = m.PeekWord(rec + 16u); a.a[1] = Chance(*c.rng, 4) ? m.PeekWord(sky + 0x2E4u + 4u) : h + 4u; a.a[2] = m.PeekWord(h);
            if (a.a[1] == 0) a.a[1] = h + 4u;
        },
        [](NativeEnv& env, const Args& a) { return Run(env, [&](G& g, C&, uint32_t& v) { v = s::PanoLoad(g, a.a[0], a.a[1], a.a[2]); return true; }); }));
    rows.push_back(SRow("pano_free", "u32 0x80013828(payload)  [SLUS]", s::kStPanoFreeFn,
        [St](CaseContext& c, Args& a) {
            St(c);
            Memory& m = *c.mem;
            const uint32_t sky = m.PeekWord(s::kStSkyPtr);
            Put16(m, sky + 0x10u, static_cast<uint16_t>(c.rng->U32() % 12u));
            Put16(m, sky + 0x12u, static_cast<uint16_t>(Chance(*c.rng, 3) ? 0xFFFFu : c.rng->U32() % 12u));
            if (Chance(*c.rng, 8)) m.PokeWord(kGp + s::kGpStSkyBusy, 1);
            a.a[0] = Chance(*c.rng, 6) ? m.PeekWord(Rec(m, 2) + 16u) : m.PeekWord(sky + 0x2E4u + 12u * (c.rng->U32() % 12u));
            if (a.a[0] == 0) a.a[0] = m.PeekWord(sky + 0x2E4u);
        },
        [](NativeEnv& env, const Args& a) { return Run(env, [&](G& g, C&, uint32_t& v) { v = s::PanoFree(g, a.a[0]); return true; }); }));
}
