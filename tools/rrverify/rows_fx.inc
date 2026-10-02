// Bench rows of the effect records' per-frame pass (src\game\sim\effects.h). Included by
// verify_physics.cpp inside its anonymous namespace, after rows_spine.inc, whose
// helpers (spinerows::PickBike, Chance, PlantEffects, PlantClock) it reuses.
//
// The GTE: the native side starts from the clone's own COP2 control registers (the snapshot's, the same
// ones the guest run starts from), so RT / TR / OFX / OFY / H / ZSF4 are the console's.
//
// The two siren callees of the police light (SLUS 0x8001836C / 0x800182B0) are sound code the effects port
// does not cover: the rows that reach them declare them and the oracle runs them at the original's depth.

namespace fxrows {

namespace s = rr::sim;
using spinerows::Chance;
using spinerows::Pick;
using spinerows::PickBike;

constexpr uint32_t kRenderCam = 0x8002F17C, kToView = 0x80028E8C, kFromModel = 0x80028C78, kSpin = 0x80029048,
                   kAround = 0x80029174, kEmit = 0x8002926C, kDebris = 0x800295AC, kDraw = 0x8002A2C8,
                   kDrawLight = 0x8002AB14, kFree = 0x8002A738, kBlink = 0x8002A8E4, kLight = 0x8002A974,
                   kUpdate = 0x80029D88, kPass = 0x8002823C, kCapture = 0x8002990C, kCaptureChain = 0x80029CA4,
                   kFreeChain = 0x8002847C, kSurface = 0x80027258;
// Frames (the prologues' `addiu sp,sp,-N`): 0x8002A974 40, 0x80029D88 40, 0x8002823C 72.
constexpr uint32_t kLightFrame = 40, kUpdateFrame = 40, kPassFrame = 72;

struct Sirens final : s::FxCallees {
    NativeEnv& env;
    explicit Sirens(NativeEnv& e) : env(e) {}
    bool SirenStop(uint32_t h) override {
        env.CallOracle(s::kFxSirenStopFn, h, 0, 0, 0);
        return env.failure.empty();
    }
    bool SirenStart(uint32_t h) override {
        env.CallOracle(s::kFxSirenStartFn, h, 0, 0, 0);
        return env.failure.empty();
    }
};

std::vector<OracleCallee> SirenCallees(uint32_t depth) {
    return {OracleCallee{s::kFxSirenStopFn, 1, 0, {}, depth}, OracleCallee{s::kFxSirenStartFn, 1, 0, {}, depth}};
}

template <class F>
uint32_t Run(NativeEnv& env, F&& f) {
    s::GuestRam g = RqView(env);
    g.SetScratchpad(env.clone->mem.scratchpad().data());
    Sirens sirens(env);
    s::FxEnv fe;
    fe.gte = s::FxGte::FromControl(env.clone->cpu.gte().cr);
    fe.io.rootCounter = env.clone->cpu.rootCounterValue;
    fe.callees = &sirens;
    const uint32_t v = f(g, fe);
    if (fe.refused && env.failure.empty()) env.failure = "fx: " + fe.why;
    return RqDone(env, g, v);
}

Row FxRow(const char* name, const char* sig, uint32_t entry, std::function<void(CaseContext&, Args&)> setup,
          std::function<uint32_t(NativeEnv&, const Args&)> native, bool compareV0,
          std::vector<OracleCallee> callees = {}) {
    Row r{name, sig, entry, std::move(setup), nullptr, compareV0, std::move(native), std::move(callees)};
    return r;
}

// ---------------------------------------------------------------------------- setup helpers
uint32_t RenderCamOf(Memory& m, uint32_t view) { return m.PeekWord(s::kFxRenderCamPtrs + 4u * view); }

int32_t Around(CaseContext& c, int32_t spread) {
    return static_cast<int32_t>(c.rng->U32() % static_cast<uint32_t>(2 * spread + 1)) - spread;
}

void PutS16(Memory& m, uint32_t a, int32_t v) { WriteU16Bench(m, a, static_cast<uint16_t>(v)); }
void PutByte(Memory& m, uint32_t a, uint32_t v) {
    const uint8_t b = static_cast<uint8_t>(v);
    m.WriteBlock(a, &b, 1);
}

// A world point in the effects' unit (1/64 world unit) near the render camera's eye, now and then far.
void NearEye(CaseContext& c, uint32_t view, int32_t out[3]) {
    Memory& m = *c.mem;
    const uint32_t cam = RenderCamOf(m, view);
    const int32_t spread = Chance(c, 6) ? 30000 : 700;
    for (uint32_t k = 0; k < 3; ++k) out[k] = static_cast<int32_t>(m.PeekWord(cam + 0x1Cu + 4u * k)) + Around(c, spread);
}

// The game clock, the camera's view distance and the fields of `e` the pass reads.
void PlantEntity(CaseContext& c, uint32_t e) {
    Memory& m = *c.mem;
    const uint32_t gs = m.PeekWord(kGameStatePtr);
    m.PokeWord(gs + 0x10u, 1000u + c.rng->U32() % 400000u);
    // Players 0 or 1, never 2: the snapshot is a one-player race, and the siren callees index the
    // second player's voice table (gp+0x788), which a one-player race does not have - a two-player
    // count on it makes the ORIGINAL read a wild pointer (measured: fx_light, second seed, case 1134,
    // a guest load from 0x600002F4 at SLUS 0x8001831C).
    if (Chance(c, 3)) m.PokeWord(gs + 0x30u, c.rng->U32() % 2u);
    if (c.sample == nullptr || Chance(c, 2)) {
        m.PokeWord(e + 0x234u, c.rng->U32());
        for (uint32_t k = 0; k < 3; ++k) PutS16(m, e + 0x1C2u + 2u * k, c.rng->Unit());
        m.PokeWord(e + 0x1E0u, Chance(c, 2) ? c.rng->U32() % 0x100000u : c.rng->U32());
        PutByte(m, e + 0x216u, c.rng->U32() % 16u);
        uint32_t f238 = m.PeekWord(e + 0x238u);
        f238 = Chance(c, 4) ? (f238 | (0x200u << (c.rng->U32() % 2u))) : (f238 & ~0x600u);
        m.PokeWord(e + 0x238u, f238);
        for (uint32_t k = 0; k < 2; ++k)
            m.PokeWord(e + 0x2Cu + 4u * k, Chance(c, 2) ? c.rng->U32() % 0x100000u : c.rng->U32());
        PutByte(m, e + 0x08u, Chance(c, 2) ? 0u : c.rng->U32() % 4u);
        PutByte(m, e + 0x09u, c.rng->U32());
        m.PokeWord(s::kFxJitterClock, Chance(c, 8) ? 4096u : c.rng->U32() % 4097u);
    }
}

// One record's fields, state `st` (0 = pick one), link bits kept.
void PlantRecord(CaseContext& c, uint32_t rec, uint32_t e, uint32_t st, uint32_t view) {
    Memory& m = *c.mem;
    if (st == 0) st = Chance(c, 12) ? c.rng->U32() % 16u : 1u + c.rng->U32() % 7u;
    static const uint32_t kSubs[6] = {0, 1, 2, 4, 1, 2};
    const uint32_t sub = Chance(c, 8) ? c.rng->U32() % 16u : kSubs[c.rng->U32() % 6u];
    uint32_t kind = c.rng->U32() % 7u;
    if (st == 2) kind = Chance(c, 2) ? 0u : (Chance(c, 2) ? 2u : kind);
    if (st == 3) kind = c.rng->U32() % 5u;
    if (st == 5) kind = c.rng->U32() % 8u;
    const uint32_t w = m.PeekWord(rec);
    m.PokeWord(rec, (w & 0x3Fu) | (st << 6) | (sub << 10) | (kind << 14));
    int32_t p[3];
    NearEye(c, view, p);
    for (uint32_t k = 0; k < 3; ++k) {
        m.PokeWord(rec + 4u + 4u * k, static_cast<uint32_t>(p[k]));
        m.PokeWord(rec + 0x14u + 4u * k, static_cast<uint32_t>(p[k] + Around(c, 60)));
        m.PokeWord(rec + 0x24u + 4u * k, static_cast<uint32_t>(Chance(c, 6) ? c.rng->S32() : Around(c, 4000)));
    }
    const uint32_t clock = m.PeekWord(m.PeekWord(kGameStatePtr) + 0x10u);
    m.PokeWord(rec + 0x30u, Chance(c, 10) ? c.rng->U32() : clock - c.rng->U32() % 700u);
    m.PokeWord(rec + 0x34u, Chance(c, 10) ? c.rng->U32() % 3u : c.rng->U32() % 900u);
    m.PokeWord(rec + 0x38u, c.rng->U32());
    PutByte(m, rec + 0x3Cu, c.rng->U32());
    PutByte(m, rec + 0x3Du, Chance(c, 2) ? 0u : 1u);
    PutS16(m, rec + 0x3Eu, static_cast<int32_t>(c.rng->U32() % 80u));
    m.PokeWord(rec + 0x40u, static_cast<uint32_t>(Around(c, 20)));
    PutS16(m, rec + 0x44u, Around(c, 400));
    PutS16(m, rec + 0x46u, Around(c, 400));
    PutS16(m, rec + 0x48u, Chance(c, 6) ? Around(c, 400) : static_cast<int32_t>(c.rng->U32() % 3000u));
    PutS16(m, rec + 0x4Cu, static_cast<int32_t>(c.rng->U32() % 256u));
    PutS16(m, rec + 0x4Eu, static_cast<int32_t>(c.rng->U32() % 256u));
    for (uint32_t k = 0; k < 2; ++k) m.PokeWord(rec + 0x50u + 4u * k, static_cast<uint32_t>(Around(c, 3000)));
    m.PokeWord(rec + 0x58u, Chance(c, 3) ? static_cast<uint32_t>(Around(c, 3000)) : c.rng->U32() % 40u);
    m.PokeWord(rec + 0x5Cu, static_cast<uint32_t>(Chance(c, 5) ? Around(c, 50) : static_cast<int32_t>(c.rng->U32() % 6000u)));
    PutByte(m, rec + 0x60u, c.rng->U32() % 11u);
    PutByte(m, rec + 0x61u, c.rng->U32() % 3u);
    PutS16(m, rec + 0x62u, 0);
    m.PokeWord(rec + 0x64u, clock - c.rng->U32() % 40u);
    for (uint32_t k = 0; k < 4; ++k) PutByte(m, rec + 0x68u + k, c.rng->U32() % 6u);
    PutByte(m, rec + 0x6Cu, c.rng->U32() % 4u);
    PutByte(m, rec + 0x6Du, Chance(c, 3) ? 0u : 1u + c.rng->U32() % 4u);
    (void)e;
}

// A terminating chain of up to six records for `e`, every record planted; returns the head (-1 empty).
int32_t PlantChain(CaseContext& c, uint32_t e, uint32_t view, bool allowLight) {
    Memory& m = *c.mem;
    const uint32_t len = c.rng->U32() % 7u;
    bool used[20] = {};
    int32_t head = -1;
    uint32_t prev = 0xFFFFFFFFu;
    for (uint32_t k = 0; k < len; ++k) {
        const uint32_t i = c.rng->U32() % 20u;
        if (used[i]) break;
        used[i] = true;
        const uint32_t rec = s::kEffectPool + s::kEffectRecordBytes * i;
        m.PokeWord(rec, (m.PeekWord(rec) & ~0x3Fu) | 63u);
        uint32_t st = 0;
        do st = Chance(c, 12) ? c.rng->U32() % 16u : 1u + c.rng->U32() % 7u;
        while (!allowLight && st == 5u);
        PlantRecord(c, rec, e, st, view);
        if (prev == 0xFFFFFFFFu) head = static_cast<int32_t>(i);
        else {
            const uint32_t pr = s::kEffectPool + s::kEffectRecordBytes * prev;
            m.PokeWord(pr, (m.PeekWord(pr) & ~0x3Fu) | i);
        }
        prev = i;
    }
    PutByte(m, e + 0x49u, static_cast<uint32_t>(head));
    return head;
}

// The player's bike (pool slot 0, the five-part model the part lists of every capture name), or any.
Pick PlayerOrAny(CaseContext& c) {
    Pick p = PickBike(c);
    if (!Chance(c, 4)) {
        p.slot = 0;
        p.e = c.mem->PeekWord(kPoolBasePtr);
    }
    return p;
}

void ReadQuad(s::GuestRam& g, uint32_t a, int16_t q[16]) {
    for (uint32_t k = 0; k < 16; ++k) q[k] = g.S16(a + 2u * k);
}
void WriteQuad(s::GuestRam& g, uint32_t a, const int16_t q[16]) {
    for (uint32_t v = 0; v < 4; ++v)
        for (uint32_t k = 0; k < 3; ++k) g.W16(a + 8u * v + 2u * k, static_cast<uint16_t>(q[4 * v + k]));
}

void PlantQuad(CaseContext& c, uint32_t a) {
    Memory& m = *c.mem;
    const int32_t cx = Around(c, 500), cy = Around(c, 500);
    const int32_t cz = Chance(c, 6) ? Around(c, 2000) : static_cast<int32_t>(c.rng->U32() % 6000u);
    for (uint32_t v = 0; v < 4; ++v) {
        PutS16(m, a + 8u * v + 0u, Chance(c, 20) ? c.rng->S16() : cx + Around(c, 200));
        PutS16(m, a + 8u * v + 2u, Chance(c, 20) ? c.rng->S16() : cy + Around(c, 200));
        PutS16(m, a + 8u * v + 4u, Chance(c, 20) ? c.rng->S16() : cz + Around(c, 100));
        PutS16(m, a + 8u * v + 6u, c.rng->S16());
    }
}

} // namespace fxrows

void AddFxRows(std::vector<Row>& rows) {
    using namespace fxrows;

    rows.push_back(FxRow(
        "fx_render_camera", "void RenderCamera(s32 view)  [SLUS 0x8002F17C]", kRenderCam,
        [](CaseContext& c, Args& a) {
            Memory& m = *c.mem;
            const uint32_t view = Chance(c, 4) ? 1u : 0u;
            const uint32_t v = s::kFxViewRecords + s::kFxViewStride * view;
            if (c.sample == nullptr || Chance(c, 2)) {
                for (uint32_t k = 0; k < 9; ++k) PutS16(m, v + 0x1B0u + 2u * k, c.rng->S16());
                for (uint32_t k = 0; k < 3; ++k) m.PokeWord(v + 0xB8u + 4u * k, c.rng->U32());
                PutS16(m, v + 0x2E8u, c.rng->S16());
            }
            a.a[0] = view;
        },
        [](NativeEnv& env, const Args& a) {
            return Run(env, [&](s::GuestRam& g, s::FxEnv&) {
                s::RenderCamera(g, a.a[0]);
                return 0u;
            });
        },
        false));

    rows.push_back(FxRow(
        "fx_to_view", "void EffectToView(const s32 p[3], s16 out[3], s32 view)  [SLUS 0x80028E8C]", kToView,
        [](CaseContext& c, Args& a) {
            Memory& m = *c.mem;
            const uint32_t view = Chance(c, 4) ? 1u : 0u;
            int32_t p[3];
            NearEye(c, view, p);
            for (uint32_t k = 0; k < 3; ++k) m.PokeWord(c.scratch + 64u + 4u * k, static_cast<uint32_t>(Chance(c, 10) ? c.rng->S32() : p[k]));
            a.a[0] = c.scratch + 64u;
            a.a[1] = c.scratch + 128u;
            a.a[2] = view;
        },
        [](NativeEnv& env, const Args& a) {
            return Run(env, [&](s::GuestRam& g, s::FxEnv& fe) {
                int32_t p[3];
                int16_t o[3];
                for (uint32_t k = 0; k < 3; ++k) p[k] = g.S32(a.a[0] + 4u * k);
                s::EffectToView(g, fe, p, o, a.a[2]);
                for (uint32_t k = 0; k < 3; ++k) g.W16(a.a[1] + 2u * k, static_cast<uint16_t>(o[k]));
                return 0u;
            });
        },
        false));

    rows.push_back(FxRow(
        "fx_from_model", "void EffectFromModel(Bike *e, s32 out[3], const s32 in[3], s32 view)  [SLUS 0x80028C78]",
        kFromModel,
        [](CaseContext& c, Args& a) {
            Memory& m = *c.mem;
            const Pick p = PickBike(c);
            for (uint32_t k = 0; k < 3; ++k)
                m.PokeWord(c.scratch + 64u + 4u * k, static_cast<uint32_t>(Chance(c, 8) ? c.rng->S32() : Around(c, 60000)));
            a.a[0] = p.e;
            a.a[1] = c.scratch + 128u;
            a.a[2] = c.scratch + 64u;
            a.a[3] = Chance(c, 4) ? 1u : 0u;
        },
        [](NativeEnv& env, const Args& a) {
            return Run(env, [&](s::GuestRam& g, s::FxEnv& fe) {
                int32_t in[3], out[3];
                for (uint32_t k = 0; k < 3; ++k) in[k] = g.S32(a.a[2] + 4u * k);
                s::EffectFromModel(g, fe, a.a[0], out, in, a.a[3]);
                for (uint32_t k = 0; k < 3; ++k) g.W32(a.a[1] + 4u * k, static_cast<uint32_t>(out[k]));
                return 0u;
            });
        },
        false));

    rows.push_back(FxRow(
        "fx_quad_spin", "void EffectQuadSpin(SVECTOR q[4], const s16 c[3], s32 angle, s32 size)  [SLUS 0x80029048]",
        kSpin,
        [](CaseContext& c, Args& a) {
            Memory& m = *c.mem;
            for (uint32_t k = 0; k < 3; ++k) PutS16(m, c.scratch + 64u + 2u * k, c.rng->S16());
            a.a[0] = c.scratch + 128u;
            a.a[1] = c.scratch + 64u;
            a.a[2] = c.rng->U32();
            a.a[3] = Chance(c, 6) ? c.rng->U32() : c.rng->U32() % 200u;
        },
        [](NativeEnv& env, const Args& a) {
            return Run(env, [&](s::GuestRam& g, s::FxEnv&) {
                int16_t q[16] = {}, cc[3];
                for (uint32_t k = 0; k < 3; ++k) cc[k] = g.S16(a.a[1] + 2u * k);
                s::EffectQuadSpin(g, q, cc, static_cast<int32_t>(a.a[2]), a.a[3]);
                WriteQuad(g, a.a[0], q);
                return 0u;
            });
        },
        false));

    rows.push_back(FxRow(
        "fx_quad_around",
        "s32 EffectQuadAround(SVECTOR q[4], const s32 w[3], const s16 v[3], s32 view)  [SLUS 0x80029174]", kAround,
        [](CaseContext& c, Args& a) {
            Memory& m = *c.mem;
            const uint32_t view = Chance(c, 4) ? 1u : 0u;
            int32_t p[3];
            NearEye(c, view, p);
            for (uint32_t k = 0; k < 3; ++k) m.PokeWord(c.scratch + 64u + 4u * k, static_cast<uint32_t>(p[k]));
            for (uint32_t k = 0; k < 3; ++k) PutS16(m, c.scratch + 96u + 2u * k, Chance(c, 8) ? c.rng->S16() : Around(c, 800));
            a.a[0] = c.scratch + 128u;
            a.a[1] = c.scratch + 64u;
            a.a[2] = c.scratch + 96u;
            a.a[3] = view;
        },
        [](NativeEnv& env, const Args& a) {
            return Run(env, [&](s::GuestRam& g, s::FxEnv& fe) {
                int16_t q[16] = {}, v[3];
                int32_t w[3];
                for (uint32_t k = 0; k < 3; ++k) w[k] = g.S32(a.a[1] + 4u * k);
                for (uint32_t k = 0; k < 3; ++k) v[k] = g.S16(a.a[2] + 2u * k);
                const uint32_t r = s::EffectQuadAround(g, fe, q, w, v, a.a[3]);
                WriteQuad(g, a.a[0], q);
                return r;
            });
        },
        true));

    rows.push_back(FxRow(
        "fx_emit_quad",
        "void EffectEmitQuad(SVECTOR q[4], s32 flags, s32 sprite, s32 clut, u32 colour)  [SLUS 0x8002926C]", kEmit,
        [](CaseContext& c, Args& a) {
            PlantQuad(c, c.scratch + 64u);
            a.a[0] = c.scratch + 64u;
            a.a[1] = c.rng->U32() % 4u;
            a.a[2] = c.rng->U32() % 11u;
            a.a[3] = c.rng->U32() % 4u;
            a.a4 = c.rng->U32();
        },
        [](NativeEnv& env, const Args& a) {
            return Run(env, [&](s::GuestRam& g, s::FxEnv& fe) {
                int16_t q[16];
                ReadQuad(g, a.a[0], q);
                s::EffectEmitQuad(g, fe, q, a.a[1], a.a[2], a.a[3], a.a4);
                return 0u;
            });
        },
        false));

    rows.push_back(FxRow(
        "fx_emit_debris",
        "void EffectEmitDebris(Rec *r, SVECTOR q[4], s32 flags, s32 sprite, s32 frame)  [SLUS 0x800295AC]", kDebris,
        [](CaseContext& c, Args& a) {
            PlantQuad(c, c.scratch + 64u);
            a.a[0] = s::kEffectPool;
            a.a[1] = c.scratch + 64u;
            a.a[2] = c.rng->U32() % 4u;
            a.a[3] = c.rng->U32() % 11u;
            a.a4 = c.rng->U32() % 6u;
        },
        [](NativeEnv& env, const Args& a) {
            return Run(env, [&](s::GuestRam& g, s::FxEnv& fe) {
                int16_t q[16];
                ReadQuad(g, a.a[1], q);
                s::EffectEmitDebris(g, fe, q, a.a[2], a.a[3], a.a4);
                return 0u;
            });
        },
        false));

    rows.push_back(FxRow(
        "fx_draw", "void EffectDraw(Bike *e, Rec *r, u32 age, s32 view, u32 *flags)  [SLUS 0x8002A2C8]", kDraw,
        [](CaseContext& c, Args& a) {
            const Pick p = PlayerOrAny(c);
            PlantEntity(c, p.e);
            const uint32_t view = Chance(c, 6) ? 1u : 0u;
            const uint32_t rec = s::kEffectPool + s::kEffectRecordBytes * (c.rng->U32() % 20u);
            PlantRecord(c, rec, p.e, 0, view);
            c.mem->PokeWord(c.scratch + 256u, Chance(c, 2) ? 0u : c.rng->U32());
            a.a[0] = p.e;
            a.a[1] = rec;
            a.a[2] = Chance(c, 8) ? c.rng->U32() : c.rng->U32() % 400u;
            a.a[3] = view;
            a.a4 = c.scratch + 256u;
        },
        [](NativeEnv& env, const Args& a) {
            return Run(env, [&](s::GuestRam& g, s::FxEnv& fe) {
                uint32_t flags = g.U32(a.a4);
                const uint32_t before = flags;
                s::EffectDraw(g, fe, a.a[0], a.a[1], a.a[2], a.a[3], flags);
                if (flags != before) g.W32(a.a4, flags);
                return 0u;
            });
        },
        false));

    rows.push_back(FxRow(
        "fx_draw_light", "void EffectDrawLight(Bike *e, Rec *r)  [SLUS 0x8002AB14]", kDrawLight,
        [](CaseContext& c, Args& a) {
            const Pick p = PlayerOrAny(c);
            PlantEntity(c, p.e);
            const uint32_t rec = s::kEffectPool + s::kEffectRecordBytes * (c.rng->U32() % 20u);
            PlantRecord(c, rec, p.e, 5, 0);
            a.a[0] = p.e;
            a.a[1] = rec;
        },
        [](NativeEnv& env, const Args& a) {
            return Run(env, [&](s::GuestRam& g, s::FxEnv& fe) {
                s::EffectDrawLight(g, fe, a.a[0], a.a[1]);
                return 0u;
            });
        },
        false));

    rows.push_back(FxRow(
        "fx_free", "void EffectFree(Bike *e, Rec *prev, Rec *r, s32 next)  [SLUS 0x8002A738]", kFree,
        [](CaseContext& c, Args& a) {
            const Pick p = PickBike(c);
            spinerows::PlantEffects(c, p.e);
            const uint32_t i = c.rng->U32() % 20u;
            const uint32_t rec = s::kEffectPool + s::kEffectRecordBytes * i;
            PlantRecord(c, rec, p.e, 0, 0);
            c.mem->PokeWord(p.e + 0x24u, c.rng->U32());
            a.a[0] = p.e;
            a.a[1] = Chance(c, 2) ? 0u : s::kEffectPool + s::kEffectRecordBytes * ((i + 1u + c.rng->U32() % 19u) % 20u);
            a.a[2] = rec;
            a.a[3] = Chance(c, 3) ? 0xFFFFFFFFu : (Chance(c, 8) ? c.rng->U32() : c.rng->U32() % 20u);
        },
        [](NativeEnv& env, const Args& a) {
            return Run(env, [&](s::GuestRam& g, s::FxEnv&) {
                s::EffectFree(g, a.a[0], a.a[1], a.a[2], a.a[3]);
                return 0u;
            });
        },
        false));

    rows.push_back(FxRow(
        "fx_blink", "void EffectBlink(u32 age, Rec *r)  [SLUS 0x8002A8E4]", kBlink,
        [](CaseContext& c, Args& a) {
            const Pick p = PickBike(c);
            PlantEntity(c, p.e);
            const uint32_t rec = s::kEffectPool + s::kEffectRecordBytes * (c.rng->U32() % 20u);
            PlantRecord(c, rec, p.e, 5, 0);
            a.a[0] = Chance(c, 2) ? c.rng->U32() % 160u : c.rng->U32();
            a.a[1] = rec;
        },
        [](NativeEnv& env, const Args& a) {
            return Run(env, [&](s::GuestRam& g, s::FxEnv&) {
                s::EffectBlink(g, a.a[0], a.a[1]);
                return 0u;
            });
        },
        false));

    rows.push_back(FxRow(
        "fx_light", "u32 EffectLight(Bike *e, Rec *r, u32 age, s32 view)  [SLUS 0x8002A974, sirens on the oracle]",
        kLight,
        [](CaseContext& c, Args& a) {
            Memory& m = *c.mem;
            const Pick p = PickBike(c);
            PlantEntity(c, p.e);
            const uint32_t rec = s::kEffectPool + s::kEffectRecordBytes * (c.rng->U32() % 20u);
            PlantRecord(c, rec, p.e, 5, 0);
            const uint32_t r = m.PeekWord(p.e + 0x354u);
            if (r >= 0x80000000u && Chance(c, 2)) {
                m.PokeWord(r + 0x25Cu, c.rng->U32() % 5u);
                PutS16(m, r + 0x220u, Chance(c, 3) ? 0 : static_cast<int32_t>(c.rng->U32() % 64u));
            }
            if (Chance(c, 3)) PutS16(m, m.PeekWord(p.e) + 14u, static_cast<int32_t>(c.rng->U32()));
            a.a[0] = p.e;
            a.a[1] = rec;
            a.a[2] = Chance(c, 2) ? c.rng->U32() % 160u : c.rng->U32();
            a.a[3] = Chance(c, 4) ? 1u : 0u;
        },
        [](NativeEnv& env, const Args& a) {
            return Run(env, [&](s::GuestRam& g, s::FxEnv& fe) { return s::EffectLight(g, fe, a.a[0], a.a[1], a.a[2], a.a[3]); });
        },
        true, SirenCallees(kLightFrame)));

    rows.push_back(FxRow(
        "fx_update", "void EffectUpdate(Bike *e, Rec *r, s32 view, u32 age)  [SLUS 0x80029D88]", kUpdate,
        [](CaseContext& c, Args& a) {
            const Pick p = PlayerOrAny(c);
            PlantEntity(c, p.e);
            const uint32_t view = Chance(c, 6) ? 1u : 0u;
            const uint32_t rec = s::kEffectPool + s::kEffectRecordBytes * (c.rng->U32() % 20u);
            PlantRecord(c, rec, p.e, 0, view);
            if (Chance(c, 3)) { // a two-rider bike: the partner the kind-3 spark follows
                c.mem->PokeWord(p.e + 0x358u, c.mem->PeekWord(kPoolBasePtr) + 1096u * (c.rng->U32() % 2u));
                c.mem->PokeWord(p.e + 0x440u, c.rng->U32() % 2u);
            }
            a.a[0] = p.e;
            a.a[1] = rec;
            a.a[2] = view;
            a.a[3] = Chance(c, 8) ? c.rng->U32() : c.rng->U32() % 900u;
        },
        [](NativeEnv& env, const Args& a) {
            return Run(env, [&](s::GuestRam& g, s::FxEnv& fe) {
                s::EffectUpdate(g, fe, a.a[0], a.a[1], a.a[2], a.a[3]);
                return 0u;
            });
        },
        false, SirenCallees(kUpdateFrame + kLightFrame)));

    rows.push_back(FxRow(
        "fx_pass", "void EffectPass(Entity *e, s32 view)  [SLUS 0x8002823C, recursive over +0x38 / +0x40]", kPass,
        [](CaseContext& c, Args& a) {
            Memory& m = *c.mem;
            const Pick p = PlayerOrAny(c);
            PlantEntity(c, p.e);
            const uint32_t view = Chance(c, 6) ? 1u : 0u;
            // the police light's sirens are declared at the top entity's depth only: the children's
            // chains carry no state-5 record
            PlantChain(c, p.e, view, true);
            for (uint32_t k = 0; k < 2; ++k) {
                const uint32_t child = m.PeekWord(p.e + 0x38u + 8u * k);
                if (child >= 0x80000000u && Chance(c, 2)) PlantChain(c, child, view, false);
            }
            a.a[0] = p.e;
            a.a[1] = view;
        },
        [](NativeEnv& env, const Args& a) {
            return Run(env, [&](s::GuestRam& g, s::FxEnv& fe) {
                s::EffectPass(g, fe, a.a[0], a.a[1]);
                return 0u;
            });
        },
        false, SirenCallees(kPassFrame + kUpdateFrame + kLightFrame)));

    rows.push_back(FxRow(
        "fx_capture", "void EffectCapture(Entity *e, Rec *r)  [SLUS 0x8002990C]", kCapture,
        [](CaseContext& c, Args& a) {
            Memory& m = *c.mem;
            const uint32_t e = m.PeekWord(kPoolBasePtr); // the five-part player bike: states 3, 5, 6 read its parts
            PlantEntity(c, e);
            PutByte(m, e + 0x08u, c.rng->U32() % 4u);
            const uint32_t rec = s::kEffectPool + s::kEffectRecordBytes * (c.rng->U32() % 20u);
            static const uint32_t kStates[6] = {3, 4, 5, 6, 5, 4};
            PlantRecord(c, rec, e, Chance(c, 10) ? 1u + c.rng->U32() % 7u : kStates[c.rng->U32() % 6u], 0);
            a.a[0] = e;
            a.a[1] = rec;
        },
        [](NativeEnv& env, const Args& a) {
            return Run(env, [&](s::GuestRam& g, s::FxEnv&) {
                s::EffectCapture(g, a.a[0], a.a[1]);
                return 0u;
            });
        },
        false));

    rows.push_back(FxRow(
        "fx_capture_chain", "void EffectCaptureChain(Entity *e)  [SLUS 0x80029CA4]", kCaptureChain,
        [](CaseContext& c, Args& a) {
            Memory& m = *c.mem;
            const uint32_t e = m.PeekWord(kPoolBasePtr);
            PlantEntity(c, e);
            PutByte(m, e + 0x08u, c.rng->U32() % 4u);
            PlantChain(c, e, 0, true);
            a.a[0] = e;
        },
        [](NativeEnv& env, const Args& a) {
            return Run(env, [&](s::GuestRam& g, s::FxEnv&) {
                s::EffectCaptureChain(g, a.a[0]);
                return 0u;
            });
        },
        false));

    rows.push_back(FxRow(
        "fx_free_chain", "void EffectFreeChain(Entity *e)  [SLUS 0x8002847C]", kFreeChain,
        [](CaseContext& c, Args& a) {
            const Pick p = PickBike(c);
            PlantEntity(c, p.e);
            c.mem->PokeWord(p.e + 0x24u, c.rng->U32());
            PlantChain(c, p.e, 0, true);
            a.a[0] = p.e;
        },
        [](NativeEnv& env, const Args& a) {
            return Run(env, [&](s::GuestRam& g, s::FxEnv&) {
                s::EffectFreeChain(g, a.a[0]);
                return 0u;
            });
        },
        false));

    rows.push_back(FxRow(
        "fx_surface", "void SurfaceFx(Bike *e, s32 kind)  [SLUS 0x80027258, the spark spawner]", kSurface,
        [](CaseContext& c, Args& a) {
            Memory& m = *c.mem;
            const Pick p = PlayerOrAny(c);
            spinerows::PlantEffects(c, p.e);
            spinerows::PlantClock(c);
            const uint32_t gs = m.PeekWord(kGameStatePtr);
            m.PokeWord(gs + 0x30u, Chance(c, 4) ? c.rng->U32() % 3u : 1u + c.rng->U32() % 2u);
            if (c.sample == nullptr || Chance(c, 2)) {
                uint32_t w = c.rng->U32();
                if (Chance(c, 2)) w &= ~(3u << 25);
                m.PokeWord(p.e + 0x24u, w);
                PutS16(m, p.e + 0x1E2u, Chance(c, 2) ? static_cast<int32_t>(c.rng->U32() % 120u) : c.rng->S16());
            }
            a.a[0] = p.e;
            a.a[1] = Chance(c, 8) ? c.rng->U32() % 256u : c.rng->U32() % 7u;
        },
        [](NativeEnv& env, const Args& a) {
            return Run(env, [&](s::GuestRam& g, s::FxEnv& fe) {
                s::SurfaceFx(g, fe, a.a[0], a.a[1]);
                return 0u;
            });
        },
        false));
}
