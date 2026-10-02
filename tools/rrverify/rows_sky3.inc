// Bench rows of the sky split (src\game\sim\sky_split.h): the gradient, the two-player
// sky and the frame's packet heap manager. Included LAST by verify_physics.cpp (rows_sky2.inc's sky2rows:: helpers are
// visible): the machine is the rr-race capture - one player, the heap ring 0x800DC0CC..0xF9550 with buffer 0 building
// from 0xDEDDC and the limit at buffer 1's mark 0xF0460, the gradient's colours and corners of race 4.
//
// NAMED BOUNDS:
//   * the heap's flush SLUS 0x80021BE8 draws the frame's table (DrawOTag / DrawSync / ClearOTagR - the GPU and its DMA)
//     and prints: inside every row but its own it is a seam stubbed on both sides; its own row stubs those four
//     (printf 0x80044894 goes to the BIOS) and compares its calls and its two stores;
//   * the heap manager waits on the VSync counter for a busy buffer that is not 60 VSyncs old (the interrupt moves
//     0x8005B46C; the interpreter's does not): the generator gives every fresh busy buffer but the one being built a
//     mark past the request, as the console's frame flip does - the port refuses past 2^20 turns instead of spinning;
//   * the ring's indices gp+0x174 / 0x178 stay below the buffer count +0xF4 (1..4), as the frame flip keeps them;
//   * the two-player sky's index table holds -1 or record numbers (0..31, now and then up to 999): a larger one sends
//     the record read below RAM, which the console's loader never writes.
namespace sky3rows {

namespace s = rr::sim;
using strows::Chance;
using strows::Get16;
using strows::Pick;
using strows::Put16;

constexpr uint32_t kRec = 0x8005B470;

// The capture with sky2rows::Machine's stubs (the MDEC, DrawSync) and the flush stubbed - but in the flush's own row.
void StartCase(CaseContext& c, bool flushStub = true) {
    strows::Machine(c);
    for (uint32_t fn : {s::kDecDctInSyncFn, s::kDecDctOutSyncFn, s::kDecDctInFn, s::kDecDctOutFn, s::kDecDctResetFn,
                        s::kSkyDrawSyncFn})
        sky2rows::StubV0(*c.mem, fn);
    if (flushStub) sky2rows::StubV0(*c.mem, s::kHeapFlushFn);
}

// The heap ring's state, inside the frame flip's ranges; the request (cursor, size) it is asked for.
void StirHeap(CaseContext& c, uint32_t& cursor, uint32_t& size, bool wild = true) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    const uint32_t gp = c.gp;
    const uint32_t rec = m.PeekWord(kRec);
    uint32_t count = m.PeekByte(rec + 0xF4u);
    if (Chance(r, 3)) count = 1u + r.U32() % 4u;
    const uint8_t cb = static_cast<uint8_t>(count);
    m.WriteBlock(rec + 0xF4u, &cb, 1);
    const bool kseg = Chance(r, 5);
    uint32_t start = m.PeekWord(rec + 0xF0u);
    if (Chance(r, 3)) start = 0x800D0000u + 16u * (r.U32() % 0x800u);
    m.PokeWord(rec + 0xF0u, start);
    const uint32_t sPhys = start & 0x00FFFFFFu;
    uint32_t end = Chance(r, 2) ? m.PeekWord(gp + s::kGpHeapEnd) : sPhys + 0x4000u + 4u * (r.U32() % 0x8000u);
    if (kseg) end |= 0x80000000u;
    if (end <= sPhys + 64u) end = sPhys + 0x1000u;
    m.PokeWord(gp + s::kGpHeapEnd, end);
    const uint32_t a2 = r.U32() % count;
    m.PokeWord(gp + s::kGpFrameOtBuf, a2);
    m.PokeWord(gp + s::kGpFrameOtNext, Chance(r, 3) ? a2 : r.U32() % count);
    uint32_t vs = m.PeekWord(s::kHeapVsync);
    if (Chance(r, 3)) vs = r.U32() % 100000u;
    m.PokeWord(s::kHeapVsync, vs);
    const uint32_t span = (end & 0x00FFFFFFu) > sPhys ? (end & 0x00FFFFFFu) - sPhys : 0x1000u;
    const auto inside = [&]() { return sPhys + 4u * (r.U32() % (span / 4u + 1u)); };
    for (uint32_t b = 0; b < 4u; ++b) {
        if (!Chance(r, 3)) continue;
        m.PokeWord(s::kHeapMarks + 4u * b, wild && Chance(r, 8) ? r.U32() : inside());
        m.PokeWord(s::kHeapStamps + 4u * b, Chance(r, 2) ? vs - r.U32() % 61u : vs - 61u - r.U32() % 500u);
        m.PokeWord(s::kHeapBusy + 64u * b, Chance(r, 2) ? 0u : Pick(r, {1u, 1u, r.U32()}));
    }
    // the limit: the heap's end (path A), a buffer's mark (path B), or somewhere in the ring
    const uint32_t which = r.U32() % 3u;
    uint32_t limit = which == 0 ? end : which == 1 ? m.PeekWord(s::kHeapMarks + 4u * (r.U32() % count)) : inside();
    if ((limit & 0x00FFFFFFu) < sPhys) limit = end; // an unused buffer's mark (0) is never the limit
    m.PokeWord(gp + s::kGpHeapLimit, limit);
    cursor = Chance(r, 2) ? m.PeekWord(gp + s::kGpHeapLimit) - r.U32() % 400u : inside();
    if (kseg && Chance(r, 2)) cursor |= 0x80000000u;
    if (cursor >= 0xFFFF0000u) cursor &= 0x00FFFFFFu; // no request wraps the address space
    cursor &= ~3u;                                    // the heap hands out words
    size = Pick(r, {36u, 40u, 48u, 320u, 40u * (1u + r.U32() % 90u), 48u * (1u + r.U32() % 27u), r.U32() % 4096u});
    // a fresh busy buffer the manager could stop at lies past the request (the frame flip's order): the console's
    // manager would otherwise wait for the VSync interrupt
    const uint32_t past = std::max(sPhys, cursor) + size + 4u;
    for (uint32_t b = 0; b < count; ++b) {
        if (b == a2 || m.PeekWord(s::kHeapBusy + 64u * b) == 0) continue;
        if (!(vs - m.PeekWord(s::kHeapStamps + 4u * b) < 61u)) continue;
        m.PokeWord(s::kHeapMarks + 4u * b, past + 4u * (r.U32() % 4096u));
    }
}

// The heap asked for through the frame record's cursor (the callers' idiom): the cursor near the limit sometimes.
// The drawing rows build their packets where the manager says: the marks stay inside the ring (a mark anywhere in
// the address space is the manager's own row's business).
void StirCursor(CaseContext& c) {
    uint32_t cur = 0, size = 0;
    StirHeap(c, cur, size, false);
    Memory& m = *c.mem;
    const uint32_t rec = m.PeekWord(kRec);
    // the callers compare the cursor with *(0x8005B4D0) = gp+0x844
    const uint32_t limit = m.PeekWord(c.gp + s::kGpHeapLimit);
    m.PokeWord(rec + 0x10Cu, Chance(*c.rng, 3) ? (limit - c.rng->U32() % 2000u) & ~3u : (m.PeekWord(rec + 0x10Cu)));
}

std::vector<OracleCallee> Seams() { return {OracleCallee{s::kHeapFlushFn, 1}}; }

Row SkyRow3(const char* name, const char* sig, uint32_t entry, std::function<void(CaseContext&, Args&)> setup,
            std::function<uint32_t(NativeEnv&, const Args&)> native, bool v0 = false,
            std::vector<OracleCallee> seams = Seams()) {
    Row r{name, sig, entry, std::move(setup), nullptr, v0, std::move(native)};
    r.maxSteps = 40'000'000;
    r.oracleCallees = std::move(seams);
    std::erase_if(r.oracleCallees, [entry](const OracleCallee& o) { return o.address == entry; });
    return r;
}

// The gradient's inputs: the view's yaw, the level's sun, its colours, the players and their rectangles.
void StirGradient(CaseContext& c, uint32_t& view) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    view = 0;
    const uint32_t gs = m.PeekWord(s::kSkyGameState);
    if (Chance(r, 3)) {
        m.PokeWord(gs + 48u, 2);
        view = r.U32() % 2u;
        // the two views' rectangles (VIEWS.VI's records): rr-race's pointer, or a table of our own
        if (Chance(r, 2)) {
            const uint32_t t = c.scratch + 1536u;
            for (uint32_t k = 0; k < 2u; ++k) {
                Put16(m, t + 8u * k, static_cast<uint16_t>(Pick(r, {0u, 192u, r.U32() % 384u})));
                Put16(m, t + 8u * k + 2u, static_cast<uint16_t>(Pick(r, {0u, 120u, r.U32() % 240u})));
                Put16(m, t + 8u * k + 4u, static_cast<uint16_t>(Pick(r, {384u, 192u, r.U32() % 400u})));
                Put16(m, t + 8u * k + 6u, static_cast<uint16_t>(Pick(r, {120u, 240u, r.U32() % 240u})));
            }
            m.PokeWord(s::kSplitViewsPtr, t);
        }
    } else if (Chance(r, 8)) {
        m.PokeWord(gs + 48u, Pick(r, {0u, 3u, 4u}));
    }
    const uint32_t cam = m.PeekWord(s::kSkyRenderCams + 4u * view);
    if (Chance(r, 4) == false) Put16(m, cam + 124u, static_cast<uint16_t>(Chance(r, 4) ? r.U32() : r.U32() % 4096u));
    if (Chance(r, 2)) m.PokeWord(s::kSunYaw, Chance(r, 5) ? r.U32() % 20000u : r.U32() % 4096u);
    if (Chance(r, 2)) m.PokeWord(s::kSunElevation, Chance(r, 5) ? r.U32() : r.U32() % 1024u);
    for (uint32_t k = 0; k < 16u; k += 4)
        if (Chance(r, 2)) m.PokeWord(s::kGradSunA + k, r.U32());
    if (Chance(r, 3)) m.PokeWord(s::kSplitSkyOn2, 1);
    if (Chance(r, 3)) Put16(m, s::kSplitSkyLift, static_cast<uint16_t>(Chance(r, 4) ? r.U32() : r.U32() % 64u));
    if (Chance(r, 4)) // the corners' old contents
        for (uint32_t k = 0; k < 16u; k += 4) m.PokeWord(s::kGradCorners + k, r.U32());
}

// The two-player sky's level tables (the type-10 section at 0x800D5138) and the view's rectangle / stream flag.
void StirSplit(CaseContext& c, uint32_t& view) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    const uint32_t gs = m.PeekWord(s::kSkyGameState);
    view = 0;
    if (Chance(r, 2)) {
        m.PokeWord(gs + 48u, 2);
        view = r.U32() % 2u;
    }
    m.PokeWord(0x8005B2D0u, Chance(r, 2) ? 1u : 0u);
    {
        const uint32_t t = c.scratch + 1536u;
        for (uint32_t k = 0; k < 2u; ++k) {
            Put16(m, t + 8u * k, static_cast<uint16_t>(Pick(r, {0u, 192u, r.U32() % 384u})));
            Put16(m, t + 8u * k + 2u, static_cast<uint16_t>(Pick(r, {0u, 120u, r.U32() % 240u})));
            Put16(m, t + 8u * k + 4u, static_cast<uint16_t>(Pick(r, {384u, 192u, r.U32() % 400u, 0u})));
            Put16(m, t + 8u * k + 6u, static_cast<uint16_t>(Pick(r, {120u, 240u, r.U32() % 240u})));
        }
        if (Chance(r, 2)) m.PokeWord(s::kSplitViewsPtr, t);
    }
    Put16(m, s::kSplitSkyLift, static_cast<uint16_t>(Chance(r, 3) ? r.U32() % 64u : r.U32()));
    m.PokeWord(s::kSplitSkyCmd, Chance(r, 2) ? 0x64808080u : r.U32());
    for (uint32_t k = 0; k < s::kSplitSkyRecordCount * s::kSplitSkyRecordBytes; k += 4)
        m.PokeWord(s::kSplitSkyRecords + k, r.U32());
    for (uint32_t k = 0; k < 27u * 2u; ++k) {
        const int32_t idx = Chance(r, 4) ? -1 : Chance(r, 20) ? static_cast<int32_t>(r.U32() % 1000u) : static_cast<int32_t>(r.U32() % 32u);
        Put16(m, s::kSplitSkyIndex + 2u * k, static_cast<uint16_t>(idx));
    }
    for (uint32_t k = 0; k < 27u; ++k)
        Put16(m, s::kSplitSkyColX + 2u * k, static_cast<uint16_t>(Chance(r, 3) ? r.U32() : 64u * k));
    const uint32_t cam = m.PeekWord(s::kSkyRenderCams + 4u * view);
    Put16(m, cam + 124u, static_cast<uint16_t>(Chance(r, 4) ? r.U32() : r.U32() % 4096u));
    for (uint32_t k = 4; k < 12u; k += 2)
        if (Chance(r, 4)) Put16(m, s::kSkyColumn + k, static_cast<uint16_t>(r.U32()));
}

} // namespace sky3rows

void AddSky3Rows(std::vector<Row>& rows) {
    using namespace sky3rows;
    using G = s::GuestRam;
    using C = rr::sim::RecoverCallees;
    using sky2rows::Run;

    rows.push_back(SkyRow3("sky3_heap_next", "u32 0x80021C98(cursor, size)  [SLUS, the frame's packet heap manager]", s::kHeapNextFn,
        [](CaseContext& c, Args& a) {
            StartCase(c);
            uint32_t cur = 0, size = 0;
            StirHeap(c, cur, size);
            a.a[0] = cur;
            a.a[1] = size;
        },
        [](NativeEnv& env, const Args& a) { return Run(env, [&](G& g, C& o, s::FxGte&, uint32_t& v) { return s::HeapNext(g, a.a[0], a.a[1], env.sp, o, v); }); }, true));
    rows.push_back(SkyRow3("sky3_heap_flush", "void 0x80021BE8(cursor)  [SLUS, the heap's flush: the table drawn, the ring restarted]", s::kHeapFlushFn,
        [](CaseContext& c, Args& a) {
            StartCase(c, false);
            Memory& m = *c.mem;
            for (uint32_t fn : {s::kPrintfFn, s::kDrawOTagFn, s::kClearOTagRFn}) sky2rows::StubV0(m, fn);
            uint32_t cur = 0, size = 0;
            StirHeap(c, cur, size);
            if (Chance(*c.rng, 2)) m.PokeWord(c.gp + s::kGpFrameOtLen, c.rng->U32() % 0x600u);
            if (Chance(*c.rng, 3)) m.PokeWord(m.PeekWord(kRec) + 0x108u, m.PeekWord(m.PeekWord(kRec) + 0xF8u + 4u * (c.rng->U32() % 2u)));
            a.a[0] = cur;
        },
        [](NativeEnv& env, const Args&) { return Run(env, [&](G& g, C& o, s::FxGte&, uint32_t&) { return s::HeapFlush(g, env.sp, o); }); }, false,
        {OracleCallee{s::kPrintfFn, 1}, OracleCallee{s::kDrawOTagFn, 1}, OracleCallee{s::kSkyDrawSyncFn, 1},
         OracleCallee{s::kClearOTagRFn, 2}}));
    rows.push_back(SkyRow3("sky3_grad_quad", "void 0x800642F8(ot, xy0, xy1, xy2, xy3, c0, c1, c2, c3)  [RASHCDG, one POLY_G4 of the gradient]", s::kGradQuadFn,
        [](CaseContext& c, Args& a) {
            StartCase(c);
            StirCursor(c);
            Memory& m = *c.mem;
            Rng& r = *c.rng;
            for (uint32_t k = 0; k < 256u; k += 4) m.PokeWord(c.scratch + k, r.U32());
            const auto at = [&]() { return c.scratch + 4u * (r.U32() % 60u) + (Chance(r, 4) ? 1u : 0u); };
            a.a[0] = Chance(r, 2) ? m.PeekWord(s::kSkyOtPtr) + 12u : c.scratch + 4u * (r.U32() % 60u);
            a.a[1] = at() & ~1u;
            a.a[2] = at() & ~1u;
            a.a[3] = at() & ~1u;
            a.a4 = at() & ~1u;
            for (uint32_t k = 0; k < 4u; ++k) a.more[k] = Chance(r, 3) ? Pick(r, {s::kGradTop, s::kGradHorizon, s::kGradSunA}) : at();
        },
        [](NativeEnv& env, const Args& a) {
            return Run(env, [&](G& g, C& o, s::FxGte&, uint32_t&) {
                const uint32_t q[9] = {a.a[0], a.a[1], a.a[2], a.a[3], a.a4, a.more[0], a.more[1], a.more[2], a.more[3]};
                return s::GradQuad(g, q, env.sp, o);
            });
        }));
    rows.push_back(SkyRow3("sky3_grad", "void 0x80063C5C(view)  [RASHCDG, the Gouraud sky: two or four POLY_G4]", s::kSkyGradFn,
        [](CaseContext& c, Args& a) {
            StartCase(c);
            StirCursor(c);
            uint32_t view = 0;
            StirGradient(c, view);
            a.a[0] = view;
        },
        [](NativeEnv& env, const Args& a) { return Run(env, [&](G& g, C& o, s::FxGte& gte, uint32_t&) { return s::GradDraw(g, a.a[0], gte, env.sp, o); }); }));
    rows.push_back(SkyRow3("sky3_split_sky", "void 0x80064CC8(view)  [RASHCDG, the two-player sky: SPRT 64 x 31 with their DR_TPAGE]", s::kSkySplitFn,
        [](CaseContext& c, Args& a) {
            StartCase(c);
            StirCursor(c);
            uint32_t view = 0;
            StirSplit(c, view);
            a.a[0] = view;
        },
        [](NativeEnv& env, const Args& a) { return Run(env, [&](G& g, C& o, s::FxGte& gte, uint32_t&) { return s::SplitSkyDraw(g, a.a[0], gte, env.sp, o); }); }));
    rows.push_back(SkyRow3("sky3_split_load", "void 0x800627A8(section, payload)  [RASHCDI, the bundle's type-10 section: the two-player sky's tables]", s::kSplitSkyLoadFn,
        [](CaseContext& c, Args& a) {
            StartCase(c);
            gridr::InjectOverlay(c); // RASHCDI over 0x8005B5E8, as while the race loads
            Memory& m = *c.mem;
            Rng& r = *c.rng;
            const uint32_t sec = c.scratch;
            m.PokeWord(sec, 10u | ((Chance(r, 3) ? Pick(r, {0x5F8u, 0x5FDu, r.U32() & 0xFFFFu}) : 0x5FCu) << 16));
            for (uint32_t k = 0; k < s::kSplitSkyBytes; k += 4) m.PokeWord(sec + 4u + k, r.U32());
            if (Chance(r, 3)) m.PokeWord(s::kSplitSkyOn2, r.U32());
            a.a[0] = sec;
            a.a[1] = sec + 4u;
        },
        [](NativeEnv& env, const Args& a) { return Run(env, [&](G& g, C&, s::FxGte&, uint32_t&) { s::SplitSkyLoad(g, a.a[0], a.a[1]); return true; }); }));
    const auto GradSetUp = [](CaseContext& c) {
        StartCase(c);
        gridr::InjectOverlay(c); // RASHCDI over 0x8005B5E8, as while the race loads
        Memory& m = *c.mem;
        for (uint32_t k = 0; k < 48u; k += 4)
            if (Chance(*c.rng, 2)) m.PokeWord(s::kGradCorners + k, c.rng->U32());
        if (Chance(*c.rng, 2)) m.PokeWord(s::kGradOn, c.rng->U32());
    };
    rows.push_back(SkyRow3("sky3_grad_load", "void 0x80062384(section, payload)  [RASHCDI, the bundle's type-1 section: the gradient's colours]", s::kGradLoadFn,
        [GradSetUp](CaseContext& c, Args& a) {
            GradSetUp(c);
            for (uint32_t k = 0; k < 20u; k += 4) c.mem->PokeWord(c.scratch + 4u + k, c.rng->U32());
            c.mem->PokeWord(c.scratch, 1u | (0x14u << 16));
            a.a[0] = c.scratch;
            a.a[1] = c.scratch + 4u;
        },
        [](NativeEnv& env, const Args& a) { return Run(env, [&](G& g, C&, s::FxGte&, uint32_t&) { s::GradLoad(g, a.a[0], a.a[1]); return true; }); }));
    rows.push_back(SkyRow3("sky3_grad_init", "void 0x800611C4()  [RASHCDI, the gradient on: its corners and colour copies]", s::kGradInitFn,
        [GradSetUp](CaseContext& c, Args&) { GradSetUp(c); },
        [](NativeEnv& env, const Args&) { return Run(env, [&](G& g, C&, s::FxGte&, uint32_t&) { s::GradInit(g); return true; }); }));
}
