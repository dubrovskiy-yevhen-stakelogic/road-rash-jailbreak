// Bench rows of the stream's CD side (src\game\sim\stream_cd.h): the CD access layer,
// the sky's panorama pick, the cells' texture binding and readiness, the speech bank's buffer release.
// Included LAST by verify_physics.cpp (rows_stream.inc's strows:: helpers are visible): the machine is the same
// race capture (rr-race: one player, the table full, panoramas resident).
//
// NAMED BOUNDS:
//   * the drive (SLUS 0x80014634 seek, 0x80014894 read) and the sky's MDEC column decode (RASHCDG 0x800662BC)
//     are the host's / the renderer's: stubbed identically on both sides (`jr ra` with v0 = 0 / -1 for the
//     read) and compared as seams;
//   * a ring request's record index is 0..31 (every request the streamer files names a table record);
//   * a sky slot is emptied the way PanoFree 0x80013828 empties it (all three words): a slot with a key has
//     its panorama.
namespace st2rows {

namespace s = rr::sim;
using namespace strows;

void Stub(Memory& m, uint32_t fn, uint32_t v0Insn) {
    m.PokeWord(fn, 0x03E00008u); // jr ra
    m.PokeWord(fn + 4u, v0Insn);
}

void Machine2(CaseContext& c) {
    strows::Machine(c);
    Memory& m = *c.mem;
    Stub(m, s::kCdSeekFn, 0);
    Stub(m, s::kCdReadFn, Chance(*c.rng, 10) ? 0x2402FFFFu : 0x00001021u); // li v0,-1 / move v0,zero
    Stub(m, s::kSkyUploadFn, 0);
    m.PokeWord(0x80053464u, c.rng->U32() % 2u);
}

// A plausible request: kind 0x10 (or 8 / 0x200), the stream file, a position, a record index, its buffer.
void PlantRequest(CaseContext& c, uint32_t at) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    const uint32_t idx = r.U32() % 32u;
    m.PokeWord(at + 0u, Pick(r, {0x10u, 0x10u, 0x10u, 8u, 0x200u}));
    m.PokeWord(at + 4u, 3u);
    m.PokeWord(at + 8u, (r.U32() % 0x2000u) << 14);
    m.PokeWord(at + 12u, idx);
    m.PokeWord(at + 16u, m.PeekWord(Rec(m, idx) + 12u));
    m.PokeWord(at + 20u, 0);
    m.PokeWord(at + 24u, Chance(r, 8) ? 0u : s::kStDoneFn);
    m.PokeWord(at + 28u, 0x4000u);
}

void StirRing(CaseContext& c, uint32_t maxCount) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    for (uint32_t k = 0; k < s::kCdRingSize; ++k) PlantRequest(c, s::kCdRing + 32u * k);
    const uint32_t n = r.U32() % (maxCount + 1u);
    const uint32_t head = r.U32() % s::kCdRingSize;
    m.PokeWord(s::kCdHead, head);
    m.PokeWord(s::kCdTail, (head + n) % s::kCdRingSize);
    m.PokeWord(s::kCdCount, n);
    m.PokeWord(s::kCdMusicCount, r.U32() % 3u);
    // the records the queued requests name: reading (a request of a read record is dropped)
    for (uint32_t k = 0; k < 32; ++k)
        if (Chance(r, 3)) m.PokeWord(Rec(m, k), Pick(r, {0x114u, 0x104u, 0x115u, 0x11u, 0u}));
    m.PokeWord(kGp + s::kGpCdCur, s::kCdRing + 32u * (r.U32() % s::kCdRingSize));
    for (uint32_t o : {s::kGpCdDone, s::kGpCdStarts, s::kGpStCdStamp, s::kGpStCdBusy, s::kGpCdInit})
        if (Chance(r, 3)) m.PokeWord(kGp + o, r.U32() % 4u);
}

// The sky block: slots emptied, the shown pair, the release list's panorama word, the view's yaw, the stream
// record's position.
void StirSky(CaseContext& c) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    const uint32_t sky = m.PeekWord(s::kStSkyPtr);
    std::vector<uint32_t> held;
    for (uint32_t k = 0; k < 20; ++k) {
        const uint32_t e = sky + 0x2E4u + 12u * k;
        if (m.PeekWord(e) == 0) continue;
        if (Chance(r, 6)) {
            m.PokeWord(e, 0);
            m.PokeWord(e + 4u, 0);
            m.PokeWord(e + 8u, 0);
            Put16(m, sky + 0x22u, static_cast<uint16_t>(Get16(m, sky + 0x22u) - 1u));
        } else {
            held.push_back(k);
        }
    }
    if (Chance(r, 10)) Put16(m, sky + 0x22u, 0);
    auto slot = [&]() -> uint16_t {
        if (held.empty() || Chance(r, 5)) return static_cast<uint16_t>(Chance(r, 2) ? 0xFFFFu : r.U32() % 20u);
        return static_cast<uint16_t>(held[r.U32() % held.size()]);
    };
    if (Chance(r, 2)) Put16(m, sky + 0x12u, slot());
    if (Chance(r, 3)) Put16(m, sky + 0x10u, slot());
    if (Chance(r, 4)) m.PokeWord(sky + 0x18u, r.U32() % 3u);
    for (uint32_t o : {0x2Au, 0x30u, 0x32u, 0x34u, 0x36u})
        if (Chance(r, 3)) Put16(m, sky + o, static_cast<uint16_t>(r.U32() % 110u));
    // the release list's record: its first word a held panorama's key, another key, or as captured
    const uint32_t word = c.scratch + 0x200u;
    if (!Chance(r, 4)) {
        uint32_t key = 0x40000000u | (r.U32() & 0x0FFFFFFFu);
        if (!held.empty() && Chance(r, 2)) key = m.PeekWord(sky + 0x2E4u + 12u * held[r.U32() % held.size()] + 8u);
        m.PokeWord(word, key);
        m.PokeWord(s::kRlsCursorWord, word);
    }
    const uint32_t cam = m.PeekWord(s::kStRenderCams);
    if (Chance(r, 2)) Put16(m, cam + 124u, static_cast<uint16_t>(r.U32()));
    const uint32_t st = State(0);
    if (Chance(r, 2)) m.PokeWord(st + 0x48u, Chance(r, 2) ? m.PeekWord(st + 0x48u) : SomeRoad(r, m));
    if (Chance(r, 2)) m.PokeWord(st + 0x44u, r.U32() % 3000u);
    if (Chance(r, 5)) m.PokeWord(s::kSkySplit, 1);
}

// A cell slot of player 0 with its page keys stirred and the texture slots it names bound or not.
uint32_t StirCellTex(CaseContext& c) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    PlantTexSlots(c);
    const uint32_t slot = s::kStCellSlots + 0x70u * (r.U32() % 12u);
    std::vector<uint32_t> keys;
    for (uint32_t k = 0; k < 24; ++k)
        if (m.PeekWord(s::kStTexSlots + 0x30u * k + 8u) != 0xFFFFFFFFu) keys.push_back(m.PeekWord(s::kStTexSlots + 0x30u * k + 8u));
    for (uint32_t o : {0x50u, 0x54u, 0x58u, 0x5Cu}) {
        if (!Chance(r, 2)) continue;
        uint32_t key = Pick(r, {0xFFFFFFFFu, 0x8000u | (r.U32() & 0x7FFFu), r.U32() & 0xFFFFu});
        if (!keys.empty() && Chance(r, 2)) key = keys[r.U32() % keys.size()];
        m.PokeWord(slot + o, key);
    }
    for (uint32_t o : {0x60u, 0x64u, 0x68u, 0x6Cu})
        if (Chance(r, 3)) m.PokeWord(slot + o, Chance(r, 2) ? 0u : s::kStTexSlots + 0x30u * (r.U32() % 24u));
    return slot;
}

Row S2Row(const char* name, const char* sig, uint32_t entry, std::function<void(CaseContext&, Args&)> setup,
          std::function<uint32_t(NativeEnv&, const Args&)> native, bool v0 = true) {
    Row r = SRow(name, sig, entry, std::move(setup), std::move(native), v0);
    r.oracleCallees.push_back(OracleCallee{s::kCdSeekFn, 3});
    r.oracleCallees.push_back(OracleCallee{s::kCdReadFn, 4});
    r.oracleCallees.push_back(OracleCallee{s::kSkyNopFn, 1});
    r.oracleCallees.push_back(OracleCallee{s::kSkyUploadFn, 0});
    r.oracleCallees.push_back(OracleCallee{s::kStDoneFn, 3});
    // the function under test is not its own seam (0x80022D20 and 0x800325BC are rows_stream.inc's)
    std::erase_if(r.oracleCallees, [entry](const OracleCallee& o) { return o.address == entry; });
    return r;
}

} // namespace st2rows

void AddStream2Rows(std::vector<Row>& rows) {
    using namespace st2rows;
    using G = s::GuestRam;
    using C = rr::sim::RecoverCallees;
    auto St = [](CaseContext& c) { Machine2(c); StirState(c); StirTable(c, false); };

    // ---- the CD access layer
    rows.push_back(S2Row("cd_queue", "s32 0x80022D20(req)  [SLUS, a read request filed]", s::kCdQueueFn,
        [St](CaseContext& c, Args& a) {
            St(c); StirRing(c, 49);
            if (Chance(*c.rng, 3)) c.mem->PokeWord(s::kCdCount, 0);
            PlantRequest(c, c.scratch); a.a[0] = c.scratch;
        },
        [](NativeEnv& env, const Args& a) { return Run(env, [&](G& g, C& o, uint32_t& v) { v = static_cast<uint32_t>(s::CdQueue(g, a.a[0], env.sp, o)); return true; }); }));
    rows.push_back(S2Row("cd_next", "void 0x80022B0C(a0)  [SLUS, the drive's next request]", s::kCdNextFn,
        [St](CaseContext& c, Args& a) { St(c); StirRing(c, 6); a.a[0] = Chance(*c.rng, 6) ? 1u : 0u; },
        [](NativeEnv& env, const Args& a) { return Run(env, [&](G& g, C& o, uint32_t&) { s::CdNext(g, a.a[0], env.sp, o); return true; }); }, false));
    rows.push_back(S2Row("cd_take", "s32 0x80022E38(out)  [SLUS, the oldest request]", s::kCdTakeFn,
        [St](CaseContext& c, Args& a) { St(c); StirRing(c, 49); a.a[0] = c.scratch; },
        [](NativeEnv& env, const Args& a) { return Run(env, [&](G& g, C&, uint32_t& v) { v = static_cast<uint32_t>(s::CdTake(g, a.a[0])); return true; }); }));
    rows.push_back(S2Row("cd_start", "void 0x80022BEC(req)  [SLUS, the seek and the read started]", s::kCdStartFn,
        [St](CaseContext& c, Args& a) { St(c); StirRing(c, 4); a.a[0] = s::kCdRing + 32u * (c.rng->U32() % s::kCdRingSize); },
        [](NativeEnv& env, const Args& a) { return Run(env, [&](G& g, C& o, uint32_t&) { s::CdStart(g, a.a[0], env.sp, o); return true; }); }, false));
    rows.push_back(S2Row("cd_done", "void 0x80022EEC()  [SLUS, the read's completion from the drive]", s::kCdDoneLabel,
        [St](CaseContext& c, Args&) {
            St(c); StirRing(c, 6);
            // the finished request's record reading, as the request left it
            Memory& m = *c.mem;
            const uint32_t q = m.PeekWord(kGp + s::kGpCdCur);
            const uint32_t kind = m.PeekWord(q);
            const uint32_t idx = kind == 8u ? 30u + c.rng->U32() % 2u : c.rng->U32() % 30u;
            m.PokeWord(q + 12u, idx);
            m.PokeWord(q + 16u, m.PeekWord(Rec(m, idx) + 12u));
            PlantChunk(c, idx, SomeChunk(*c.rng, {0u, 1u, 2u, 3u, 4u, 8u, 9u, 10u}), false);
            m.PokeWord(Rec(m, idx), kind == 8u ? 0x10Cu : 0x114u);
            m.PokeWord(kGp + s::kGpStCdBusy, 1);
        },
        [](NativeEnv& env, const Args&) { return Run(env, [&](G& g, C& o, uint32_t&) { s::CdDone(g, env.sp, o); return true; }); }, false));
    rows.push_back(S2Row("cd_reset", "void 0x80022CC4()  [SLUS, the ring emptied]", s::kCdResetFn,
        [St](CaseContext& c, Args&) { St(c); StirRing(c, 49); },
        [](NativeEnv& env, const Args&) { return Run(env, [&](G& g, C&, uint32_t&) { s::CdReset(g); return true; }); }, false));
    rows.push_back(S2Row("cd_init", "void 0x800229B0()  [SLUS, the layer's reset and the stall stamps]", s::kCdInitFn,
        [St](CaseContext& c, Args&) { St(c); StirRing(c, 49); c.mem->PokeWord(kGp + s::kGpCdInit, c.rng->U32() % 2u); },
        [](NativeEnv& env, const Args&) { return Run(env, [&](G& g, C& o, uint32_t&) { s::CdInit(g, env.sp, o); return true; }); }, false));
    rows.push_back(S2Row("res_free_irq", "void 0x80030FA0(index)  [SLUS, a record freed from the SPU / CD side]", s::kResFreeIrqFn,
        [St](CaseContext& c, Args& a) {
            St(c);
            const uint32_t i = c.rng->U32() % 32u;
            if (Chance(*c.rng, 2)) c.mem->PokeWord(Rec(*c.mem, i), Pick(*c.rng, {0x33u, 0x31u, 0x13u, 0x14u, 0x1033u, 0x19u}));
            a.a[0] = i;
        },
        [](NativeEnv& env, const Args& a) { return Run(env, [&](G& g, C& o, uint32_t&) { s::ResFreeIrq(g, a.a[0], env.sp, o); return true; }); }, false));

    // ---- the sky
    rows.push_back(S2Row("sky_view", "void 0x800650D0(view)  [RASHCDG, the view's sky columns and the pick]", s::kSkyViewFn,
        [St](CaseContext& c, Args& a) { St(c); StirSky(c); a.a[0] = 0; if (Chance(*c.rng, 5)) c.mem->PokeWord(c.mem->PeekWord(s::kStGameState) + 48u, 2); },
        [](NativeEnv& env, const Args& a) { return Run(env, [&](G& g, C& o, uint32_t&) { s::SkyView(g, a.a[0], env.sp, o); return true; }); }, false));
    rows.push_back(S2Row("sky_frame", "void 0x80065174()  [RASHCDG, the panorama shown and its columns]", s::kSkyFrameFn,
        [St](CaseContext& c, Args&) { St(c); StirSky(c); },
        [](NativeEnv& env, const Args&) { return Run(env, [&](G& g, C& o, uint32_t&) { s::SkyFrame(g, env.sp, o); return true; }); }, false));
    rows.push_back(S2Row("sky_pick", "s32 0x800654B4(road, along)  [RASHCDG, the panorama slot to show]", s::kSkyPickFn,
        [St](CaseContext& c, Args& a) {
            St(c); StirSky(c);
            a.a[0] = Chance(*c.rng, 2) ? c.mem->PeekWord(State(0) + 0x48u) : SomeRoad(*c.rng, *c.mem);
            a.a[1] = Pick(*c.rng, {0u, c.rng->U32() % 3000u, (c.rng->U32() % 3000u) << 6, c.rng->U32() % 0x10000u});
        },
        [](NativeEnv& env, const Args& a) { return Run(env, [&](G& g, C&, uint32_t& v) { v = static_cast<uint32_t>(s::SkyPick(g, a.a[0], a.a[1])); return true; }); }));
    rows.push_back(S2Row("sky_turn", "s32 0x800656A8(a, b, c, d)  [RASHCDG, the columns a turn uncovers]", s::kSkyTurnFn,
        [St](CaseContext& c, Args& a) {
            St(c);
            for (int k = 0; k < 4; ++k) a.a[k] = static_cast<uint32_t>(static_cast<int32_t>(c.rng->U32() % 240u) - 10) | (Chance(*c.rng, 4) ? 0xABCD0000u : 0u);
        },
        [](NativeEnv& env, const Args& a) { return Run(env, [&](G&, C&, uint32_t& v) { v = static_cast<uint32_t>(s::SkyTurn(a.a[0], a.a[1], a.a[2], a.a[3])); return true; }); }));

    // ---- the cells' textures
    rows.push_back(S2Row("cell_tex_bind", "u32 0x800325BC(slot, p)  [SLUS, a cell's pages bound to the texture slots]", s::kCellTexBindFn,
        [St](CaseContext& c, Args& a) { St(c); a.a[0] = StirCellTex(c); a.a[1] = 0; },
        [](NativeEnv& env, const Args& a) { return Run(env, [&](G& g, C&, uint32_t& v) { v = s::CellTexBind(g, a.a[0], a.a[1]); return true; }); }));
}
