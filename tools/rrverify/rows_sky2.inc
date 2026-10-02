// Bench rows of the sky (src\game\sim\sky_draw.h): the sky draw, the panorama's
// packets, its MDEC column decode and VRAM tile cache, the cloud ring, and the level-load tables they read.
// Included LAST by verify_physics.cpp (rows_stream.inc's strows:: helpers are visible): the machine is the rr-race
// capture - one player, a panorama shown (+0x12 = 1) with its 110 columns' tables, the ring 164..77 of 192 slots,
// columns 42..79 decoded, the cloud ring and slices built.
//
// The GTE: the native side starts from the clone's own COP2 control registers (OFX / OFY / H, and for the rows
// below the sky draw its RT / TR); sky_draw sets RT from the render camera and TR = 0 itself, so its row varies
// the matrix through RAM.
//
// The three RASHCDI rows lay the loader overlay over 0x8005B5E8 first (rows_grid.inc's gridr::InjectOverlay).
//
// NAMED BOUNDS:
//   * THE MDEC is the host's: DecDCTin / DecDCTout / their syncs / DecDCTReset (SLUS 0x8004D90C / 0x8004D988 /
//     0x8004D9A8 / 0x8004D9E4 / 0x8004D7B4) are stubbed on both sides (`jr ra; move v0,zero` - the interpreter
//     has no MDEC) and compared as seams, as are LoadImage 0x80048A6C (strows) and DrawSync 0x800487C0 (idle). A
//     decode started in a case therefore never completes: the draw's poll loop runs its 10000 polls and gives up
//     (0x800647F0), which is the original's own path for a dead MDEC;
//   * the sky block's fields keep the ranges the game gives them: columns 0..109, pairs -1..54, ring slots
//     0..191 of the capture's 192, the upload RECT on the cache's 16-texel grid (x from *(0x8005B390), 12 columns;
//     y from *(0x8005B394) under the page limits +0x40.., each at least one row), +0x1E 0..26 columns to decode and
//     at least 1 while a decode runs and whenever TileUpload / DecodeStart are entered (the game starts a decode only
//     with columns to do; with none and the pair already decoded the two call each other until the stack is gone).
namespace sky2rows {

namespace s = rr::sim;
using strows::Chance;
using strows::Get16;
using strows::Pick;
using strows::Put16;

void StubV0(Memory& m, uint32_t fn) {
    m.PokeWord(fn, 0x03E00008u);     // jr ra
    m.PokeWord(fn + 4u, 0x00001021u); // move v0,zero
}

void Machine(CaseContext& c) {
    strows::Machine(c);
    Memory& m = *c.mem;
    for (uint32_t fn : {s::kDecDctInSyncFn, s::kDecDctOutSyncFn, s::kDecDctInFn, s::kDecDctOutFn, s::kDecDctResetFn,
                        s::kSkyDrawSyncFn})
        StubV0(m, fn);
    StubV0(m, s::kHeapFlushFn); // the heap's flush draws the table (the GPU) - a seam, stubbed on both sides
}

uint32_t Sky(Memory& m) { return m.PeekWord(s::kSkyBlockPtr); }

// The sky block's decode and ring state, stirred inside the game's ranges.
void StirSky(CaseContext& c) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    const uint32_t sky = Sky(m);
    const uint32_t bx = m.PeekWord(s::kSkyVramX), by = m.PeekWord(s::kSkyVramY);
    if (Chance(r, 2)) m.PokeWord(sky + 0x18u, Pick(r, {0u, 0u, 2u, 2u, 1u}));
    if (Chance(r, 2)) Put16(m, sky + 0x1Cu, static_cast<uint16_t>(Chance(r, 12) ? 2u : r.U32() % 2u));
    // columns still to decode: while a decode runs (+0x18) at least one (the game starts one only with |turn| or 26)
    if (Chance(r, 2)) Put16(m, sky + 0x1Eu, static_cast<uint16_t>(Pick(r, {0u, 1u, 2u, 3u, r.U32() % 27u})));
    if (m.PeekWord(sky + 0x18u) != 0 && static_cast<int16_t>(Get16(m, sky + 0x1Eu)) < 1)
        Put16(m, sky + 0x1Eu, static_cast<uint16_t>(1u + r.U32() % 26u));
    if (Chance(r, 3)) Put16(m, sky + 0x20u, static_cast<uint16_t>(r.U32() % 2u));
    if (Chance(r, 3)) Put16(m, sky + 0x14u, static_cast<uint16_t>(static_cast<int32_t>(r.U32() % 56u) - 1));
    for (uint32_t o : {0x30u, 0x34u, 0x36u})
        if (Chance(r, 3)) Put16(m, sky + o, static_cast<uint16_t>(r.U32() % 110u));
    if (Chance(r, 3)) {
        const uint32_t n = 192u;
        const uint32_t a = r.U32() % n, len = r.U32() % n;
        Put16(m, sky + 0x24u, static_cast<uint16_t>(a));
        Put16(m, sky + 0x2Cu, static_cast<uint16_t>(Chance(r, 3) ? r.U32() % n : a));
        Put16(m, sky + 0x2Eu, static_cast<uint16_t>((a + len) % n));
        Put16(m, sky + 0x26u, static_cast<uint16_t>(Chance(r, 4) ? 1u + r.U32() % n : n));
    }
    // the page limits (at least one row each) and the two upload RECTs on the grid under them
    if (Chance(r, 4))
        for (uint32_t k = 0; k < 4; ++k) Put16(m, sky + 0x40u + 2u * k, static_cast<uint16_t>(by + 16u * (1u + r.U32() % 16u)));
    for (uint32_t o : {0x38u, 0x3Cu}) {
        if (!Chance(r, 2)) continue;
        const uint32_t x = bx + 16u * (r.U32() % 12u);
        int32_t d = static_cast<int32_t>(x - bx);
        const uint32_t limit = Get16(m, sky + 0x40u + 2u * static_cast<uint32_t>(d >> 6));
        const uint32_t rows = limit > by ? (limit - by) / 16u : 1u;
        Put16(m, sky + o, static_cast<uint16_t>(x));
        Put16(m, sky + o + 2u, static_cast<uint16_t>(by + 16u * (r.U32() % std::max(1u, rows))));
    }
    if (Chance(r, 6)) m.PokeWord(s::kSkySplitFlag, 1);
    if (Chance(r, 5)) m.PokeWord(s::kCloudOn, r.U32() % 2u);
    if (Chance(r, 5)) m.PokeWord(s::kGradOn, r.U32() % 2u);
    if (Chance(r, 8)) m.PokeWord(s::kSplitSkyOn, 1);
    if (Chance(r, 3)) Put16(m, s::kSkyColumn, static_cast<uint16_t>(r.U32() % 110u));
}

// The render camera's matrix (+0x5C, what the sky draw hands SetRotMatrix) and yaw (+0x7C).
void StirCamera(CaseContext& c) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    const uint32_t cam = m.PeekWord(s::kSkyRenderCams);
    if (Chance(r, 2)) Put16(m, cam + 124u, static_cast<uint16_t>(r.U32()));
    if (Chance(r, 3)) return;
    const double yaw = (r.U32() % 4096u) * 6.283185307179586 / 4096.0, pitch = (static_cast<int32_t>(r.U32() % 400u) - 200) / 1000.0;
    const double cy = std::cos(yaw), sy = std::sin(yaw), cp = std::cos(pitch), sp = std::sin(pitch);
    const double rm[9] = {cy, 0, -sy, sy * sp, cp, cy * sp, sy * cp, -sp, cy * cp};
    for (uint32_t k = 0; k < 9; ++k) {
        int32_t v = static_cast<int32_t>(std::lround(rm[k] * 4096.0));
        if (k >= 3 && k < 6) v = (v * 3413) >> 12;
        if (Chance(r, 20)) v = static_cast<int16_t>(r.U32());
        Put16(m, cam + 0x5Cu + 2u * k, static_cast<uint16_t>(v));
    }
}

std::vector<OracleCallee> Seams() {
    // The two-player sky 0x80064CC8, the gradient 0x80063C5C and the heap manager 0x80021C98 are PORTED
    // (sky_split.h, rows_sky3.inc) and run natively inside these rows; the heap's flush 0x80021BE8 is the seam
    return {OracleCallee{0x8005E840u, 1},            OracleCallee{s::kSkyLoadImageFn, 2},
            OracleCallee{s::kDecDctInSyncFn, 1},     OracleCallee{s::kDecDctOutSyncFn, 1},
            OracleCallee{s::kDecDctInFn, 2},         OracleCallee{s::kDecDctOutFn, 2},
            OracleCallee{s::kDecDctResetFn, 1},      OracleCallee{s::kSkyDrawSyncFn, 1},
            OracleCallee{s::kSkyEmptyFn, 0},         OracleCallee{s::kHeapFlushFn, 1}};
}

// The oracle's callees run on the clone's own COP2: the port's RT / TR go there before each call (the gradient
// 0x80063C5C RTPS's with what 0x80064B9C set), and whatever the callee leaves comes back.
struct GteSync final : rr::sim::RecoverCallees {
    NativeEnv& env;
    rcv::RecoverOracle o;
    s::FxGte& gte;
    GteSync(NativeEnv& e, s::FxGte& g) : env(e), o(e), gte(g) {}
    bool Call(uint32_t fn, const uint32_t* a, int n, uint32_t sp, uint32_t& v0) override {
        uint32_t* cr = env.clone->cpu.gte().cr;
        for (uint32_t k = 0; k < 4; ++k)
            cr[k] = static_cast<uint16_t>(gte.rt[2 * k]) | (static_cast<uint32_t>(static_cast<uint16_t>(gte.rt[2 * k + 1])) << 16);
        cr[4] = (cr[4] & 0xFFFF0000u) | static_cast<uint16_t>(gte.rt[8]);
        for (uint32_t k = 0; k < 3; ++k) cr[5 + k] = static_cast<uint32_t>(gte.tr[k]);
        const bool ok = o.Call(fn, a, n, sp, v0);
        const s::FxGte back = s::FxGte::FromControl(cr);
        for (int k = 0; k < 9; ++k) gte.rt[k] = back.rt[k];
        for (int k = 0; k < 3; ++k) gte.tr[k] = back.tr[k];
        return ok;
    }
};

template <class F>
uint32_t Run(NativeEnv& env, F&& f) {
    s::GuestRam g = RqView(env);
    g.SetScratchpad(env.clone->mem.scratchpad().data());
    s::FxGte gte = s::FxGte::FromControl(env.clone->cpu.gte().cr);
    GteSync o(env, gte);
    uint32_t v0 = 0;
    const bool ok = f(g, o, gte, v0);
    if (!ok && env.failure.empty() && !g.Faulted()) env.failure = "the port refused";
    return RqDone(env, g, v0);
}

Row SkyRow(const char* name, const char* sig, uint32_t entry, std::function<void(CaseContext&, Args&)> setup,
           std::function<uint32_t(NativeEnv&, const Args&)> native, bool v0 = false) {
    Row r{name, sig, entry, std::move(setup), nullptr, v0, std::move(native)};
    r.maxSteps = 40'000'000;
    r.oracleCallees = Seams();
    std::erase_if(r.oracleCallees, [entry](const OracleCallee& o) { return o.address == entry; });
    return r;
}

// A resident MDEC strip of the shown panorama (the pair's record + 8, its BS at +8), or 0.
uint32_t SomeStrip(CaseContext& c) {
    Memory& m = *c.mem;
    const uint32_t sky = Sky(m);
    std::vector<uint32_t> strips;
    for (uint32_t p = 0; p < 55u; ++p) {
        const uint32_t e = m.PeekWord(sky + 1000u + 4u * p);
        if (e != 0 && m.PeekWord(e + 12u) != 0) strips.push_back(e + 8u);
    }
    return strips.empty() ? 0u : strips[c.rng->U32() % strips.size()];
}

} // namespace sky2rows

void AddSky2Rows(std::vector<Row>& rows) {
    using namespace sky2rows;
    using G = s::GuestRam;
    using C = rr::sim::RecoverCallees;
    auto St = [](CaseContext& cc) { sky2rows::Machine(cc); sky2rows::StirSky(cc); };

    rows.push_back(SkyRow("sky2_draw", "void 0x80064B9C(view)  [RASHCDG, the sky of a view: pick, panorama, clouds]", s::kSkyDrawFn,
        [St](CaseContext& c, Args& a) { St(c); StirCamera(c); a.a[0] = 0; },
        [](NativeEnv& env, const Args& a) { return Run(env, [&](G& g, C& o, s::FxGte& gte, uint32_t&) { return s::SkyDraw(g, a.a[0], gte, env.sp, o); }); }));
    rows.push_back(SkyRow("sky2_pano_draw", "void 0x800644F4()  [RASHCDG, the panorama's tiles: the decodes waited for, 25 columns of POLY_FT4]", s::kSkyPanoFn,
        [St](CaseContext& c, Args&) { St(c); },
        [](NativeEnv& env, const Args&) { return Run(env, [&](G& g, C& o, s::FxGte& gte, uint32_t&) { return s::PanoDraw(g, gte, env.sp, o); }); }));
    const auto Busy = [](CaseContext& c) { // TileUpload / DecodeStart run with columns still to decode
        if (static_cast<int16_t>(Get16(*c.mem, Sky(*c.mem) + 0x1Eu)) < 1)
            Put16(*c.mem, Sky(*c.mem) + 0x1Eu, static_cast<uint16_t>(1u + c.rng->U32() % 26u));
    };
    rows.push_back(SkyRow("sky2_tile_upload", "s32 0x800657A8()  [RASHCDG, a decoded strip's tiles into the VRAM ring]", s::kSkyTileFn,
        [St, Busy](CaseContext& c, Args&) { St(c); Busy(c); },
        [](NativeEnv& env, const Args&) { return Run(env, [&](G& g, C& o, s::FxGte&, uint32_t& v) { return s::TileUpload(g, env.sp, o, v); }); }, true));
    rows.push_back(SkyRow("sky2_decode_start", "void 0x800662BC()  [RASHCDG, the next column pair's MDEC decode]", s::kSkyDecodeFn,
        [St, Busy](CaseContext& c, Args&) { St(c); Busy(c); },
        [](NativeEnv& env, const Args&) { return Run(env, [&](G& g, C& o, s::FxGte&, uint32_t&) { return s::DecodeStart(g, env.sp, o); }); }));
    rows.push_back(SkyRow("sky2_mdec_done", "void 0x80013AE4()  [SLUS, the MDEC-out callback]", s::kMdecDoneFn,
        [St](CaseContext& c, Args&) { St(c); },
        [](NativeEnv& env, const Args&) { return Run(env, [&](G& g, C&, s::FxGte&, uint32_t&) { s::MdecDone(g); return true; }); }));
    rows.push_back(SkyRow("sky2_horz_cut", "void 0x800102A4(tile, record)  [SLUS, a tile's skyline cut-out]", s::kHorzCutFn,
        [St](CaseContext& c, Args& a) {
            St(c);
            Memory& m = *c.mem;
            for (uint32_t k = 0; k < 512u; k += 4) m.PokeWord(c.scratch + k, c.rng->U32() | 0x80008000u);
            for (uint32_t k = 0; k < 8u; ++k) {
                const uint8_t b = static_cast<uint8_t>(Chance(*c.rng, 3) ? 0u : c.rng->U32());
                m.WriteBlock(c.scratch + 1024u + k, &b, 1);
            }
            a.a[0] = c.scratch;
            a.a[1] = c.scratch + 1024u;
            if (Chance(*c.rng, 3)) { // a real tile and HORZ record of the capture
                const uint32_t sky = Sky(m);
                a.a[0] = sky + 0x4C4u + 512u * (c.rng->U32() % 8u);
                a.a[1] = m.PeekWord(sky + 0x3E4u) + 0x48u + 8u * (c.rng->U32() % 100u);
            }
        },
        [](NativeEnv& env, const Args& a) { return Run(env, [&](G& g, C&, s::FxGte&, uint32_t&) { s::HorzCut(g, a.a[0], a.a[1]); return true; }); }));
    rows.push_back(SkyRow("sky2_dct_vlc", "s32 0x80020400(bs, out)  [SLUS, the BS VLC decoder into MDEC run-levels]", s::kDctVlcFn,
        [St](CaseContext& c, Args& a) {
            St(c);
            Memory& m = *c.mem;
            const uint32_t strip = SomeStrip(c);
            a.a[0] = strip + 8u;
            a.a[1] = Sky(m) + 0x26C0u - 4u * Get16(m, strip + 8u);
        },
        [](NativeEnv& env, const Args& a) { return Run(env, [&](G& g, C&, s::FxGte&, uint32_t& v) { v = s::DctVlc(g, a.a[0], a.a[1]); return true; }); }, true));
    rows.push_back(SkyRow("sky2_cloud_draw", "void 0x8006396C(view)  [RASHCDG, the cloud ring's eight segments]", s::kSkyCloudFn,
        [St](CaseContext& c, Args& a) {
            St(c);
            Memory& m = *c.mem;
            if (Chance(*c.rng, 3)) // the ring's band moved (0x80061100's inputs)
                for (uint32_t i = 0; i < 25u; ++i)
                    for (uint32_t k = 0; k < 2u; ++k)
                        Put16(m, s::kCloudRing + 16u * i + 8u * k + 2u, static_cast<uint16_t>(c.rng->U32() % 6000u - 4000u));
            a.a[0] = 0;
        },
        [](NativeEnv& env, const Args&) { return Run(env, [&](G& g, C& o, s::FxGte& gte, uint32_t&) { return s::CloudDraw(g, gte, env.sp, o); }); }));
    rows.push_back(SkyRow("sky2_vlc_build", "s32 0x800202B8(tab0, tab1, symbols)  [SLUS, the VLC decoder's two tables]", s::kVlcBuildFn,
        [St](CaseContext& c, Args& a) {
            St(c);
            Memory& m = *c.mem;
            const uint32_t t0 = m.PeekWord(0x8005AC8Cu + 0x128u), t1 = m.PeekWord(0x8005AC8Cu + 0x12Cu);
            for (uint32_t k = 0; k < 0x6000u; k += 4) if (Chance(*c.rng, 4)) m.PokeWord(t0 + k, c.rng->U32());
            for (uint32_t k = 0; k < 0x600u; k += 4) if (Chance(*c.rng, 4)) m.PokeWord(t1 + k, c.rng->U32());
            a.a[0] = Chance(*c.rng, 12) ? 0u : t0;
            a.a[1] = Chance(*c.rng, 12) ? 0u : t1;
            a.a[2] = 0;
            if (Chance(*c.rng, 3)) { // a symbol table of its own (a .WVE's VLC0)
                for (uint32_t k = 0; k < 224u; ++k) Put16(m, c.scratch + 2u * k, static_cast<uint16_t>(c.rng->U32()));
                a.a[2] = c.scratch;
            } else if (Chance(*c.rng, 2)) {
                a.a[2] = 0x80052C20u;
            }
        },
        [](NativeEnv& env, const Args& a) { return Run(env, [&](G& g, C&, s::FxGte&, uint32_t& v) { v = static_cast<uint32_t>(s::VlcBuild(g, a.a[0], a.a[1], a.a[2])); return true; }); }, true));
    rows.push_back(SkyRow("sky2_tile_records", "void 0x80060AA8()  [RASHCDI, the tile cache's slot records]", s::kTileRecordsFn,
        [St](CaseContext& c, Args&) {
            St(c);
            gridr::InjectOverlay(c); // RASHCDI over 0x8005B5E8, as while the race loads
            Memory& m = *c.mem;
            Rng& r = *c.rng;
            const uint32_t sky = Sky(m);
            for (uint32_t k = 0; k < 192u * 3u; ++k) if (Chance(r, 3)) m.PokeWord(sky + 0x2744u + 4u * k, r.U32());
            if (Chance(r, 2)) {
                m.PokeWord(s::kSkyVramX, 64u * (r.U32() % 13u));
                m.PokeWord(s::kSkyVramY, Chance(r, 2) ? 256u : 16u * (r.U32() % 16u));
            }
            const uint32_t by = m.PeekWord(s::kSkyVramY);
            for (uint32_t k = 0; k < 4; ++k)
                Put16(m, sky + 0x40u + 2u * k, static_cast<uint16_t>(by + 16u * (Chance(r, 8) ? 0u : 1u + r.U32() % 16u)));
        },
        [](NativeEnv& env, const Args&) { return Run(env, [&](G& g, C&, s::FxGte&, uint32_t&) { s::TileRecords(g); return true; }); }));
    rows.push_back(SkyRow("sky2_cloud_slice", "void 0x80060780(record, mode, rect, clutX, clutY)  [RASHCDI, one cloud slice record]", s::kCloudSliceFn,
        [St](CaseContext& c, Args& a) {
            St(c);
            gridr::InjectOverlay(c); // RASHCDI over 0x8005B5E8, as while the race loads
            Memory& m = *c.mem;
            Rng& r = *c.rng;
            a.a[0] = c.scratch;
            a.a[1] = Pick(r, {8u, 8u, 9u, 10u, r.U32() % 16u});
            a.a[2] = c.scratch + 64u;
            m.PokeWord(c.scratch + 64u, 64u * (r.U32() % 16u) + 16u * (r.U32() % 4u));
            m.PokeWord(c.scratch + 68u, r.U32() % 512u);
            m.PokeWord(c.scratch + 72u, Pick(r, {16u, 15u, r.U32() % 64u}));
            m.PokeWord(c.scratch + 76u, Pick(r, {0x3Eu, r.U32() % 256u}));
            a.a[3] = 64u * (r.U32() % 16u);
            a.a4 = r.U32() % 512u;
        },
        [](NativeEnv& env, const Args& a) { return Run(env, [&](G& g, C&, s::FxGte&, uint32_t&) { s::CloudSlice(g, a.a[0], a.a[1], a.a[2], a.a[3], a.a4); return true; }); }));
    rows.push_back(SkyRow("sky2_cloud_ring", "void 0x80061100()  [RASHCDI, the cloud ring's 25 points]", s::kCloudRingFn,
        [St](CaseContext& c, Args&) {
            St(c);
            gridr::InjectOverlay(c); // RASHCDI over 0x8005B5E8, as while the race loads
            Memory& m = *c.mem;
            if (Chance(*c.rng, 2)) m.PokeWord(s::kCloudBand, c.rng->U32() % 3000u);
            if (Chance(*c.rng, 2)) m.PokeWord(s::kCloudBand + 4u, static_cast<uint32_t>(-static_cast<int32_t>(c.rng->U32() % 3000u)));
            for (uint32_t k = 0; k < 100u; ++k) if (Chance(*c.rng, 2)) m.PokeWord(s::kCloudRing + 4u * k, c.rng->U32());
        },
        [](NativeEnv& env, const Args&) { return Run(env, [&](G& g, C&, s::FxGte&, uint32_t&) { s::CloudRing(g); return true; }); }));
}
