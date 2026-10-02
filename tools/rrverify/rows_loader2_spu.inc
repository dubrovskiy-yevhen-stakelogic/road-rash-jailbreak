// Bench rows of libspu's SPU-RAM allocator and the music stream player's ring buffers (src\game\sim\spu_heap.{h,cpp}),
// SLUS_010.53 SHA-1 67ed165a2c517d4e6106fb0dfa324d66dd9a76f1. Included LAST by verify_physics.cpp inside its anonymous
// namespace (rows_loader.inc's ldr:: and rows_population.inc's pop:: are visible).
//
// The machine is a race capture (rr-race by default), whose heap is the boot's table 0x800D6A38 after the race's
// eleven blocks. A dump-derived case runs on that heap as captured; a randomised case builds a table of
// its own in the scratch block (n 1..32, the terminal index 0..n, blocks used / free / dropped / empty, adjacent free
// runs, shuffled order, or raw random words) and points 0x8005A484 at it, with the reverb reservation on or off and
// the address unit's shift / mask varied. Words past the table's n + 1 entries are random and never 0x2FFFFFFF, so
// the gc's dropped-entry scan ends as it does on the console.
//
// NAMED BOUND: MusicSpuAlloc's failure arm calls the debug print 0x80044894, a `jr ra` on both machines (the product
// does not print); the call itself is compared (a declared oracle callee).
namespace spuh {

namespace s = rr::sim;
using pop::Chance;

constexpr uint32_t kTabOff = 0x100;      // the case's table in the scratch block
constexpr uint32_t kTabSlots = 32 + 1 + 40; // n + 1 entries and the random tail

uint32_t Table(const CaseContext& c) { return c.scratch + kTabOff; }

void Globals(CaseContext& c) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    m.PokeWord(s::kSpuReverbOn, Chance(r, 2) ? 0u : (Chance(r, 5) ? r.U32() : 1u));
    m.PokeWord(s::kSpuReverbStart, Chance(r, 2) ? 0xF6F8u : (Chance(r, 3) ? r.U32() : r.U32() & 0xFFFFu));
    uint32_t shift = 3u;
    if (Chance(r, 4)) shift = Chance(r, 3) ? r.U32() : r.U32() % 8u;
    m.PokeWord(s::kSpuAddrShift, shift);
    m.PokeWord(s::kSpuAddrMask, Chance(r, 5) ? r.U32() : (1u << (shift & 31u)) - 1u);
}

uint32_t NotDropped(Rng& r) {
    uint32_t w = r.U32();
    return w == 0x2FFFFFFFu ? 0u : w;
}

// A table of `n` blocks with the terminal entry at `last`, in the scratch block, 0x8005A47C..84 set to it.
void RandomHeap(CaseContext& c) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    const uint32_t tab = Table(c);
    for (uint32_t k = 0; k < kTabSlots * 2u; ++k) m.PokeWord(tab + 4u * k, NotDropped(r));
    const int32_t n = Chance(r, 3) ? 12 : static_cast<int32_t>(1u + r.U32() % 32u);
    int32_t last = static_cast<int32_t>(r.U32() % static_cast<uint32_t>(n + 1));
    if (Chance(r, 16)) last = -1;
    const uint32_t unit = Chance(r, 4) ? 1u : 8u;
    if (Chance(r, 8)) {                                   // raw random words for the live entries
        for (int32_t k = 0; k <= last; ++k) {
            uint32_t w = r.U32();
            if (Chance(r, 5)) w = 0x2FFFFFFFu;
            m.PokeWord(tab + 8u * static_cast<uint32_t>(k), w);
            m.PokeWord(tab + 8u * static_cast<uint32_t>(k) + 4u, Chance(r, 2) ? r.U32() % 0x8000u : r.U32());
        }
    } else {
        uint32_t at = Chance(r, 4) ? (r.U32() & 0xFFFF8u) : 0x1010u;
        for (int32_t k = 0; k < last; ++k) {
            uint32_t size = Chance(r, 8) ? 0u : (r.U32() % (Chance(r, 2) ? 0x400u : 0x10000u)) * unit;
            const uint32_t kind = r.U32() % 8u;
            uint32_t w = at;
            if (kind < 3u) w |= 0x80000000u;              // free
            else if (kind == 3u) w = 0x2FFFFFFFu;          // dropped
            if (Chance(r, 16)) w ^= r.U32() & 0x30000000u;
            m.PokeWord(tab + 8u * static_cast<uint32_t>(k), w);
            m.PokeWord(tab + 8u * static_cast<uint32_t>(k) + 4u, size);
            if (!Chance(r, 6)) at += size;                 // mostly adjacent, sometimes a gap / an overlap
            else at += r.U32() % 0x100u * unit;
        }
        if (last >= 0) {
            const uint32_t rest = at < 0x80000u && !Chance(r, 6) ? 0x80000u - at : r.U32() % 0x90000u;
            m.PokeWord(tab + 8u * static_cast<uint32_t>(last), (at & 0x0FFFFFFFu) | 0x40000000u);
            m.PokeWord(tab + 8u * static_cast<uint32_t>(last) + 4u, rest);
        }
        if (last > 1 && Chance(r, 3)) {                    // out of address order
            for (int32_t k = 0; k < last; ++k) {
                const uint32_t j = r.U32() % static_cast<uint32_t>(last);
                const uint32_t a = tab + 8u * static_cast<uint32_t>(k), b = tab + 8u * j;
                const uint32_t aw = m.PeekWord(a), as = m.PeekWord(a + 4u);
                m.PokeWord(a, m.PeekWord(b));
                m.PokeWord(a + 4u, m.PeekWord(b + 4u));
                m.PokeWord(b, aw);
                m.PokeWord(b + 4u, as);
            }
        }
    }
    m.PokeWord(s::kSpuMallocTab, tab);
    m.PokeWord(s::kSpuMallocMax, static_cast<uint32_t>(n));
    m.PokeWord(s::kSpuMallocLast, static_cast<uint32_t>(last));
}

// The capture's heap as it is (a dump-derived case), or with some of its blocks freed (a randomised one).
void CaptureHeap(CaseContext& c, bool stir) {
    Memory& m = *c.mem;
    if (!stir) return;
    const uint32_t tab = m.PeekWord(s::kSpuMallocTab);
    const int32_t last = static_cast<int32_t>(m.PeekWord(s::kSpuMallocLast));
    for (int32_t k = 0; k < last && k < 64; ++k)
        if (Chance(*c.rng, 3)) m.PokeWord(tab + 8u * static_cast<uint32_t>(k), m.PeekWord(tab + 8u * static_cast<uint32_t>(k)) | 0x80000000u);
}

void AnyHeap(CaseContext& c) {
    if (c.sample) return CaptureHeap(c, false);
    Globals(c);
    if (Chance(*c.rng, 4)) CaptureHeap(c, true);
    else RandomHeap(c);
}

uint32_t AnySize(Rng& r) {
    switch (r.U32() % 8u) {
    case 0: return 0u;
    case 1: return r.U32();
    case 2: return 0xFFFFFFFFu - r.U32() % 16u;
    case 3: return 0x3FC0u;
    case 4: return 0x10000u;
    default: return r.U32() % (Chance(r, 2) ? 0x800u : 0x40000u);
    }
}

// An address to free: a live entry's word (used, free or terminal), or anything.
uint32_t AnyAddress(Memory& m, Rng& r) {
    const uint32_t tab = m.PeekWord(s::kSpuMallocTab);
    const int32_t last = static_cast<int32_t>(m.PeekWord(s::kSpuMallocLast));
    if (last >= 0 && last < 64 && !Chance(r, 4)) return m.PeekWord(tab + 8u * (r.U32() % static_cast<uint32_t>(last + 1)));
    return Chance(r, 2) ? r.U32() : r.U32() & 0x7FFF8u;
}

// ---------------------------------------------------------------------------- the setups
void SetupInit(CaseContext& c, Args& a) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    if (!c.sample) Globals(c);
    for (uint32_t k = 0; k < 8u; ++k) m.PokeWord(Table(c) + 4u * k, r.U32());
    a.a[0] = c.sample ? static_cast<uint32_t>(s::kSpuBootBlocks)
                      : (Chance(r, 4) ? r.U32() : static_cast<uint32_t>(static_cast<int32_t>(r.U32() % 40u) - 4));
    a.a[1] = Table(c) + 8u * (r.U32() % 4u);
}
void SetupMalloc(CaseContext& c, Args& a) {
    AnyHeap(c);
    a.a[0] = AnySize(*c.rng);
}
void SetupGc(CaseContext& c, Args&) { AnyHeap(c); }
void SetupFree(CaseContext& c, Args& a) {
    AnyHeap(c);
    a.a[0] = AnyAddress(*c.mem, *c.rng);
}
void SetupMusicAlloc(CaseContext& c, Args&) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    ldr::JrRa(m, s::kMusicPrintFn);
    const bool fresh = !c.sample && Chance(r, 2);
    AnyHeap(c);
    if (fresh) { // a fresh heap, as the loader leaves it: the three banks, then the rings
        const uint32_t tab = Table(c);
        m.PokeWord(tab, 0x40001010u);
        m.PokeWord(tab + 4u, 0x7EFF0u);
        m.PokeWord(s::kSpuMallocTab, tab);
        m.PokeWord(s::kSpuMallocMax, 12u);
        m.PokeWord(s::kSpuMallocLast, 0u);
    }
    if (c.sample) {
        m.PokeWord(s::kMusicStreamCount, 2u);
        return;
    }
    m.PokeWord(s::kMusicStreamCount, r.U32() % 5u);
    m.PokeWord(s::kMusicChunkBytes, Chance(r, 2) ? 0x2000u : (Chance(r, 4) ? r.U32() : r.U32() % 0x4000u));
    m.PokeWord(s::kMusicChunks, Chance(r, 2) ? 8u : (Chance(r, 4) ? r.U32() : r.U32() % 16u));
    for (uint32_t k = 0; k < 5u; ++k) m.PokeWord(s::kMusicStreams + 20u * k + 32u, r.U32());
}
void SetupMusicFree(CaseContext& c, Args&) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    AnyHeap(c);
    if (c.sample) return;
    m.PokeWord(s::kMusicStreamCount, static_cast<uint32_t>(static_cast<int32_t>(r.U32() % 7u) - 2));
    for (uint32_t k = 0; k < 5u; ++k) m.PokeWord(s::kMusicStreams + 20u * k + 32u, AnyAddress(m, r));
}

Row Make(const char* name, const char* sig, uint32_t entry, std::function<void(CaseContext&, Args&)> setup,
         std::function<uint32_t(NativeEnv&, const Args&)> native, bool v0, std::vector<OracleCallee> callees = {}) {
    Row r{name, sig, entry, std::move(setup), nullptr, v0, std::move(native)};
    r.oracleCallees = std::move(callees);
    r.maxSteps = 20'000'000;
    return r;
}

} // namespace spuh

void AddLoader2SpuRows(std::vector<Row>& rows) {
    namespace s = rr::sim;
    using ldr::Run;
    using spuh::Make;
    rows.push_back(Make("spu_init_malloc", "s32 SpuInitMalloc(s32 n, u32 *table)  [SLUS 0x8004F368]", s::kSpuInitMallocFn,
                        spuh::SetupInit, [](NativeEnv& env, const Args& a) {
                            return Run(env, "SpuInitMalloc", [&](s::GuestRam& g, ldr::Seams&, uint32_t& v0) {
                                v0 = static_cast<uint32_t>(s::SpuInitMalloc(g, static_cast<int32_t>(a.a[0]), a.a[1]));
                                return true;
                            });
                        }, true));
    rows.push_back(Make("spu_malloc", "s32 SpuMalloc(s32 size)  [SLUS 0x8004F3C8]", s::kSpuMallocFn, spuh::SetupMalloc,
                        [](NativeEnv& env, const Args& a) {
                            return Run(env, "SpuMalloc", [&](s::GuestRam& g, ldr::Seams&, uint32_t& v0) {
                                v0 = s::SpuMalloc(g, a.a[0]);
                                return true;
                            });
                        }, true));
    rows.push_back(Make("spu_malloc_gc", "void SpuMallocGc(void)  [SLUS 0x8004F698]", s::kSpuMallocGcFn, spuh::SetupGc,
                        [](NativeEnv& env, const Args&) {
                            return Run(env, "SpuMallocGc", [&](s::GuestRam& g, ldr::Seams&, uint32_t&) {
                                s::SpuMallocGc(g);
                                return true;
                            });
                        }, false));
    rows.push_back(Make("spu_free", "void SpuFree(u32 addr)  [SLUS 0x8004F998]", s::kSpuFreeFn, spuh::SetupFree,
                        [](NativeEnv& env, const Args& a) {
                            return Run(env, "SpuFree", [&](s::GuestRam& g, ldr::Seams&, uint32_t&) {
                                s::SpuFree(g, a.a[0]);
                                return true;
                            });
                        }, false));
    rows.push_back(Make("music_spu_alloc", "s32 MusicSpuAlloc(void)  [SLUS 0x800210C4]", s::kMusicSpuAllocFn,
                        spuh::SetupMusicAlloc, [](NativeEnv& env, const Args&) {
                            return Run(env, "MusicSpuAlloc", [&](s::GuestRam& g, ldr::Seams& c, uint32_t& v0) {
                                return s::MusicSpuAlloc(g, env.sp, c, v0);
                            });
                        }, true, ldr::Callees({{s::kMusicPrintFn, 1}})));
    rows.push_back(Make("music_spu_free", "s32 MusicSpuFree(void)  [SLUS 0x80021174]", s::kMusicSpuFreeFn,
                        spuh::SetupMusicFree, [](NativeEnv& env, const Args&) {
                            return Run(env, "MusicSpuFree", [&](s::GuestRam& g, ldr::Seams&, uint32_t& v0) {
                                v0 = s::MusicSpuFree(g);
                                return true;
                            });
                        }, true));
}
