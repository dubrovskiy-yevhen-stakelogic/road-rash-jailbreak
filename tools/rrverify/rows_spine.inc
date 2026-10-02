// Bench rows that take seams off the race spine. Included by
// verify_physics.cpp inside its anonymous namespace, so everything defined above that point is visible here.
//
// The callees the spine rows had the oracle execute are
// ported in src\game\sim\spine.{h,cpp}, each gets its own row below, and the spine rows call them
// through `SpineNativeCall` - NATIVE code on the clone's RAM, with the call still WITNESSED: the row
// keeps the callee declared, so the guest's calls are observed at its entry, and the port's call
// (address and arguments) is appended to the port's sequence here, so a call the port omits, adds,
// reorders or mis-argues still fails the row. The same mechanism rows_anim.inc uses for 0x800C4550.

namespace spinerows {

namespace s = rr::sim;

constexpr uint32_t kFindFree = 0x80027178, kLink = 0x800271CC, kJitterSpray = 0x8002705C,
                   kJitterBurst = 0x800270F0, kLocalToWorld = 0x800289E8, kGetRCnt = 0x80043F00,
                   kBurst = 0x80027778, kSpray = 0x80027974;
constexpr uint32_t kStampFrame = 40; // `addiu sp,sp,-40` at 0x800BC7CC

uint8_t* Raw(NativeEnv& env, uint32_t a, uint32_t n) {
    const uint32_t phys = a & 0x1FFFFFFFu;
    if (a < 0x80000000u || phys + n > env.clone->mem.ram().size()) return nullptr;
    return env.clone->mem.ram().data() + phys;
}

// StampResult's three callees on the bench: the command stack is PORTED (ai.h) and runs on the clone's
// RAM in place; the stance event is PORTED (stance.h) and runs natively, witnessed
// (AnimNativeStanceEvent), its own unported pose/combat children on the oracle.
struct StampCallees final : s::StampResultCallees {
    NativeEnv& env;
    uint32_t sp;
    StampCallees(NativeEnv& e, uint32_t stampSp) : env(e), sp(stampSp) {}
    bool StanceEvent(uint32_t ev, uint32_t rider, uint32_t p) override {
        AnimNativeStanceEvent(env, ev, rider, p, sp);
        return env.failure.empty();
    }
    bool ClearCommands(uint32_t e) override {
        uint8_t* b = Raw(env, e, 1096);
        if (b == nullptr) return false;
        // `8 * (s8)depth` bytes from +0x3BC: a negative depth clears the whole address space on the
        // console. The port refuses rather than imitate that.
        const int32_t depth = static_cast<int8_t>(b[0x3B2]);
        if (depth < 0 || Raw(env, e + 0x3BCu, 8u * static_cast<uint32_t>(depth)) == nullptr) return false;
        s::AiClearCommands(b);
        return true;
    }
    bool PushCommand(uint16_t op, uint16_t target, int32_t mode, uint32_t e) override {
        uint8_t* b = Raw(env, e, 1096);
        const uint32_t R = env.clone->mem.PeekWord(e + 852u);
        const uint8_t* rider = Raw(env, R, 640);
        const uint8_t* alt = Raw(env, kAltKindTable, 8u * 65536u);
        const uint8_t* fight = Raw(env, env.clone->mem.PeekWord(kFightRecPtr), 12u * 256u);
        if (b == nullptr || rider == nullptr || alt == nullptr || fight == nullptr) return false;
        struct Sink final : s::AiStanceSink {
            NativeEnv& env;
            uint32_t sp;
            Sink(NativeEnv& v, uint32_t p) : env(v), sp(p) {}
            void PlayIdleStance(uint16_t event, uint32_t riderArg) override {
                AnimNativeStanceEvent(env, event, riderArg, 2, sp);
            }
        } sink(env, sp - 40u);
        s::AiPushEnv pe;
        pe.raceClock = static_cast<int32_t>(env.clone->mem.PeekWord(env.clone->mem.PeekWord(kGameStatePtr) + 0x10u));
        pe.rider = rider;
        pe.riderAddress = R;
        pe.altKindTable = alt;
        pe.fightRecords = fight;
        pe.stance = &sink;
        uint8_t cmd[8] = {static_cast<uint8_t>(op), static_cast<uint8_t>(op >> 8), static_cast<uint8_t>(target),
                          static_cast<uint8_t>(target >> 8), 0, 0, 0, 0};
        s::AiPushCommand(cmd, mode, b, pe);
        return env.failure.empty();
    }
};

// The finish_test row's own port harness (race.h FinishTest with everything it resolves), reused by
// the rows whose port reaches the finish test through the ported ProgressPass. Taken from the row
// table in AddSpineRows, which runs last.
std::function<uint32_t(NativeEnv&, const Args&)> g_finishTest;
constexpr uint32_t kFinishOwnFrame = 72; // `addiu sp,sp,-72` at 0x800B9958

} // namespace spinerows

// The stance event's own seams, for a row whose port runs StampResult natively: `depth` is how far
// below the row's `env.sp` StampResult's own `sp` lies (the stance event is called from there). The
// witness of 0x800C4550 itself and its three unported children, at the depths rows_anim.inc uses.
std::vector<OracleCallee> SpineStampSeams(uint32_t depth) {
    using namespace animrows;
    const uint32_t d = depth;
    return {OracleCallee{kAiStance, 3, 0, {}, d},
            Capture(d + kFrEvent + kFrGate + kFrTransition + kFrRestart + kFrPoseStart),
            Leave(d + kFrEvent + kFrLeave), Enter(d + kFrEvent + kFrEnter)};
}

// Everything a natively run FinishTest 0x800B9958 declares, for a row whose port calls it `depth` below
// the row's `env.sp`: its own witness, its two unported milestone callees on the oracle, its three
// ported ones witnessed, and the stance event under StampResult.
std::vector<OracleCallee> SpineFinishSeams(uint32_t depth) {
    const uint32_t d = depth + spinerows::kFinishOwnFrame;
    std::vector<OracleCallee> v{OracleCallee{kFinishTest, 1, 0, {}, depth},
                                OracleCallee{kMilestoneAdvance, 1, 0, {}, d}, OracleCallee{kMilestoneFirst, 0, 0, {}, d},
                                OracleCallee{kResultStamp, 1, 0, {}, d},     OracleCallee{kRecordFinish, 2, 0, {}, d},
                                OracleCallee{kRoadAdvance, 2, 0, {}, d}};
    for (const OracleCallee& oc : SpineStampSeams(d + spinerows::kStampFrame)) v.push_back(oc);
    return v;
}

// A spine seam run natively, witnessed. Declared in verify_physics.cpp.
uint32_t SpineNativeCall(NativeEnv& env, uint32_t address, const uint32_t* a, int n) {
    using namespace spinerows;
    Cpu::CallObservation o;
    o.address = address;
    for (int k = 0; k < n && k < 12; ++k) o.a[k] = a[k];
    uint32_t extra = 0;
    if (env.callees != nullptr)
        for (const OracleCallee& oc : *env.callees)
            if (oc.address == address) extra = oc.frame;
    o.sp = env.sp - extra;
    env.calls.push_back(o);

    s::GuestRam g = RqView(env);
    g.SetScratchpad(env.clone->mem.scratchpad().data());
    s::SpineIo io;
    io.rootCounter = env.clone->cpu.rootCounterValue;
    uint32_t v0 = 0;
    bool ok = true;
    switch (address) {
    case kGoEvent: s::RaceGo(g); break;
    case kCrashEmit: s::CrashEmit(g, a[0], a[1], a[2]); break;
    case kBurst: s::EffectBurst(g, a[0], a[1], a[2], a[3], io); break;
    case kSpray: s::EffectSpray(g, a[0], a[2], io); break;
    case kMallocEntry: {
        bool refused = false;
        v0 = s::SpineMalloc(g, a[0], a[1], refused);
        if (refused && env.failure.empty()) env.failure = "malloc: the out-of-memory arm (a BIOS print) is not ported";
        break;
    }
    case kViewEvent: s::ViewEvent(g, a[0], a[1]); break;
    case kWipeoutEffect: ok = s::ResetBikeState(g, a[0]); break;
    case kResultStamp: {
        StampCallees c(env, o.sp - kStampFrame);
        ok = s::StampResult(g, a[0], c);
        break;
    }
    case kRoadAdvance: v0 = static_cast<uint32_t>(s::RoadNextObjectMissing(g, a[0], static_cast<int32_t>(a[1]))); break;
    case kRecordFinish: {
        const uint32_t ea = env.clone->mem.PeekWord(kPoolBasePtr) + 1096u * a[0];
        s::FinishOrderEnv fe;
        fe.entity = Raw(env, ea, 1096);
        fe.riderDef = fe.entity ? Raw(env, env.clone->mem.PeekWord(ea + 0x43Cu), 72) : nullptr;
        fe.table = Raw(env, kFinishOrder - 16u, 16u * 20u);
        fe.tableBytes = static_cast<int32_t>(16u * 20u);
        ok = s::RecordFinish(static_cast<int32_t>(a[1]), fe);
        break;
    }
    case kFinishTest: {
        // the finish_test row's harness, on this env: its oracle calls take this row's declared
        // depths (SpineFinishSeams)
        if (!g_finishTest) { ok = false; break; }
        Args fa;
        fa.a[0] = a[0];
        v0 = g_finishTest(env, fa);
        if (v0 == 0xDEADDEADu) ok = false;
        break;
    }
    default: ok = false; break;
    }
    if (!ok && env.failure.empty()) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "the native seam 0x%08X refused", address);
        env.failure = buf;
    }
    return RqDone(env, g, v0);
}

namespace spinerows {

// ---------------------------------------------------------------------------- setup helpers
struct Pick {
    uint32_t e = 0;    // a live pool-0 slot of the bench's machine
    uint32_t slot = 0;
    int32_t live = 0;
};

Pick PickBike(CaseContext& c) {
    Pick p;
    const uint32_t base = c.mem->PeekWord(kPoolBasePtr);
    p.live = static_cast<int32_t>(c.mem->PeekWord(kLiveBikes));
    if (p.live < 1 || p.live > 24) p.live = 1;
    p.slot = c.rng->U32() % static_cast<uint32_t>(p.live);
    p.e = base + 1096u * p.slot;
    return p;
}

bool Chance(CaseContext& c, uint32_t n) { return c.rng->U32() % n == 0; }

// The 20 effect records: a random busy/free pattern (bits 6..9 of word 0) and links (bits 0..5) that
// form terminating chains, so both EffectFindFree's arms and every step of EffectLink run.
void PlantEffects(CaseContext& c, uint32_t e) {
    Memory& m = *c.mem;
    const uint32_t mode = c.rng->U32() % 4u; // 0: leave the snapshot's, 1: all busy, 2/3: mixed
    if (mode != 0) {
        for (uint32_t i = 0; i < 20; ++i) {
            const uint32_t rec = s::kEffectPool + s::kEffectRecordBytes * i;
            uint32_t w = m.PeekWord(rec);
            uint32_t state = (mode == 1 || c.rng->U32() % 3u != 0) ? 1u + c.rng->U32() % 15u : 0u;
            w = (w & ~0x3C0u) | (state << 6);
            m.PokeWord(rec, (w & ~0x3Fu) | 63u);
        }
    }
    // A chain for this entity: a random walk over distinct records, terminated by 63.
    if (c.sample == nullptr || Chance(c, 2)) {
        uint8_t head = 0xFF;
        const uint32_t len = c.rng->U32() % 6u;
        uint32_t prev = 0xFFFFFFFFu;
        bool used[20] = {};
        for (uint32_t k = 0; k < len; ++k) {
            uint32_t i = c.rng->U32() % 20u;
            if (used[i]) break;
            used[i] = true;
            const uint32_t rec = s::kEffectPool + s::kEffectRecordBytes * i;
            m.PokeWord(rec, (m.PeekWord(rec) & ~0x3Fu) | 63u);
            if (prev == 0xFFFFFFFFu) head = static_cast<uint8_t>(i);
            else {
                const uint32_t pr = s::kEffectPool + s::kEffectRecordBytes * prev;
                m.PokeWord(pr, (m.PeekWord(pr) & ~0x3Fu) | i);
            }
            prev = i;
        }
        m.WriteBlock(e + 0x49u, &head, 1);
    }
}

void PlantClock(CaseContext& c) {
    const uint32_t gs = c.mem->PeekWord(kGameStatePtr);
    const uint32_t clock = c.rng->U32() % 400000u;
    c.mem->PokeWord(gs + 0x10u, clock);
    c.mem->PokeWord(s::kEffectLastStamp, Chance(c, 4) ? clock : c.rng->U32() % 400000u);
    if (c.rootCounter != nullptr) *c.rootCounter = c.rng->U32();
}

void PlantPlayers(CaseContext& c) {
    const uint32_t gs = c.mem->PeekWord(kGameStatePtr);
    if (c.sample == nullptr || Chance(c, 2)) c.mem->PokeWord(gs + 0x30u, c.rng->U32() % 3u);
}

Row SpineRow(const char* name, const char* sig, uint32_t entry, std::function<void(CaseContext&, Args&)> setup,
             std::function<uint32_t(NativeEnv&, const Args&)> native, bool compareV0) {
    Row r{name, sig, entry, std::move(setup), nullptr, compareV0, std::move(native), {}};
    return r;
}

// Runs one port on the clone's RAM with the case's root-counter value; a fault fails the case.
template <class F>
uint32_t Run(NativeEnv& env, F&& f) {
    s::GuestRam g = RqView(env);
    g.SetScratchpad(env.clone->mem.scratchpad().data());
    s::SpineIo io;
    io.rootCounter = env.clone->cpu.rootCounterValue;
    const uint32_t v = f(g, io);
    return RqDone(env, g, v);
}

} // namespace spinerows

void AddSpineRows(std::vector<Row>& rows) {
    using namespace spinerows;
    for (const Row& r : rows)
        if (std::string(r.name) == "finish_test") g_finishTest = r.nativeEx;

    // SLUS 0x80043F00 GetRCnt has its own row already (`get_rcnt`, rows_engine_note.inc); spine.h's
    // address-view twin `SpineGetRCnt` is exercised through the two jitter rows below.

    // ------------------------------------------------------------------ the effect records
    rows.push_back(SpineRow(
        "effect_find_free", "s32 EffectFindFree(void)  [SLUS 0x80027178]", kFindFree,
        [](CaseContext& c, Args&) { PlantEffects(c, PickBike(c).e); },
        [](NativeEnv& env, const Args&) {
            return Run(env, [&](s::GuestRam& g, const s::SpineIo&) { return static_cast<uint32_t>(s::EffectFindFree(g)); });
        },
        true));

    rows.push_back(SpineRow(
        "effect_link", "void EffectLink(Bike *e, s32 idx)  [SLUS 0x800271CC]", kLink,
        [](CaseContext& c, Args& a) {
            const Pick p = PickBike(c);
            PlantEffects(c, p.e);
            a.a[0] = p.e;
            a.a[1] = Chance(c, 6) ? c.rng->U32() : c.rng->U32() % 20u;
        },
        [](NativeEnv& env, const Args& a) {
            return Run(env, [&](s::GuestRam& g, const s::SpineIo&) {
                s::EffectLink(g, a.a[0], a.a[1]);
                return 0u;
            });
        },
        false));

    rows.push_back(SpineRow(
        "effect_jitter_spray", "void EffectJitterSpray(Rec *r, s32 speed)  [SLUS 0x8002705C, GetRCnt x1..2]",
        kJitterSpray,
        [](CaseContext& c, Args& a) {
            PlantClock(c);
            a.a[0] = s::kEffectPool + s::kEffectRecordBytes * (c.rng->U32() % 20u);
            static const int32_t kSpeeds[6] = {0, 1, 17, 18, -1, 200};
            a.a[1] = static_cast<uint32_t>(Chance(c, 3) ? c.rng->S32() >> (c.rng->U32() % 24u) : kSpeeds[c.rng->U32() % 6u]);
        },
        [](NativeEnv& env, const Args& a) {
            return Run(env, [&](s::GuestRam& g, const s::SpineIo& io) {
                s::EffectJitterSpray(g, a.a[0], static_cast<int32_t>(a.a[1]), io);
                return 0u;
            });
        },
        false));

    rows.push_back(SpineRow(
        "effect_jitter_burst", "void EffectJitterBurst(Rec *r)  [SLUS 0x800270F0, GetRCnt x2]", kJitterBurst,
        [](CaseContext& c, Args& a) {
            PlantClock(c);
            a.a[0] = s::kEffectPool + s::kEffectRecordBytes * (c.rng->U32() % 20u);
        },
        [](NativeEnv& env, const Args& a) {
            return Run(env, [&](s::GuestRam& g, const s::SpineIo& io) {
                s::EffectJitterBurst(g, a.a[0], io);
                return 0u;
            });
        },
        false));

    rows.push_back(SpineRow(
        "effect_local_to_world", "void EffectLocalToWorld(Bike *e, const s32 v[3], s32 out[3])  [SLUS 0x800289E8]",
        kLocalToWorld,
        [](CaseContext& c, Args& a) {
            const Pick p = PickBike(c);
            Memory& m = *c.mem;
            const uint32_t v = c.scratch + 64u;
            if (c.sample == nullptr) {
                for (uint32_t k = 0; k < 3; ++k) {
                    m.PokeWord(p.e + 0x130u + 4u * k, static_cast<uint32_t>(c.rng->S32() >> (c.rng->U32() % 20u)));
                    m.PokeWord(p.e + 0xB8u + 4u * k, c.rng->U32());
                }
                for (uint32_t k = 0; k < 9; ++k) WriteU16Bench(m, p.e + 0x1B0u + 2u * k, static_cast<uint16_t>(c.rng->U32()));
            }
            for (uint32_t k = 0; k < 3; ++k) m.PokeWord(v + 4u * k, static_cast<uint32_t>(c.rng->S32() >> (c.rng->U32() % 24u)));
            if (Chance(c, 4)) { // one of the game's own offsets
                const uint32_t kind = c.rng->U32() % 8u;
                a.a[1] = s::kEffectOffsetTable + 12u * kind;
            } else {
                a.a[1] = v;
            }
            a.a[0] = p.e;
            a.a[2] = Chance(c, 8) ? v : c.scratch + 128u; // an output that aliases the input, sometimes
        },
        [](NativeEnv& env, const Args& a) {
            return Run(env, [&](s::GuestRam& g, const s::SpineIo&) {
                s::EffectLocalToWorld(g, a.a[0], a.a[1], a.a[2]);
                return 0u;
            });
        },
        false));

    // The three spawners. They pick up a bike of the machine (its model pointer +0x00 and part list
    // +0x04 are the ones the crash emitter reads the part pair through) and plant everything they gate on.
    auto spawnerSetup = [](CaseContext& c, Pick& p) {
        Memory& m = *c.mem;
        p = PickBike(c);
        // All three spawn for PLAYERS only (handle < game_state+0x30): three cases in four take pool
        // slot 0 or 1 (handles 0 and 1 in every capture) with one or two players.
        if (!Chance(c, 4)) {
            p.slot = c.rng->U32() % 2u;
            p.e = c.mem->PeekWord(kPoolBasePtr) + 1096u * p.slot;
        }
        PlantEffects(c, p.e);
        PlantClock(c);
        const uint32_t gs = m.PeekWord(kGameStatePtr);
        m.PokeWord(gs + 0x30u, Chance(c, 4) ? c.rng->U32() % 3u : 1u + c.rng->U32() % 2u);
        if (c.sample == nullptr || Chance(c, 2)) {
            // the budget word, each gate field open half the time
            uint32_t w = c.rng->U32();
            if (Chance(c, 2)) w &= 0x3FFFFFFFu;                                  // crash debris < 2
            if (Chance(c, 2)) w &= ~0x60u;                                       // sprays not blocked
            if (Chance(c, 2)) w = (w & ~(0xFu << 19)) | ((c.rng->U32() % 5u) << 19); // bursts / kind-0
            if (Chance(c, 2)) w &= ~(3u << 23);
            m.PokeWord(p.e + 0x24u, w);
            const uint8_t pend = static_cast<uint8_t>(Chance(c, 2) ? 0u : c.rng->U32());
            m.WriteBlock(p.e + 0x08u, &pend, 1);
            WriteU16Bench(m, p.e + 0x1E2u, static_cast<uint16_t>(Chance(c, 2) ? c.rng->U32() % 120u : c.rng->U32()));
            uint8_t f4 = m.PeekByte(gs + 4u);
            f4 = static_cast<uint8_t>((f4 & ~0x10u) | (Chance(c, 2) ? 0x10u : 0u));
            m.WriteBlock(gs + 4u, &f4, 1);
        }
    };

    rows.push_back(SpineRow(
        "crash_emit", "void CrashEmit(Bike *e, s32 which, s32 kind)  [SLUS 0x80027540, the crash debris]", kCrashEmit,
        [spawnerSetup](CaseContext& c, Args& a) {
            Pick p;
            spawnerSetup(c, p);
            if (c.sample == nullptr || Chance(c, 2)) c.mem->PokeWord(p.e + 0xB4u, c.rng->U32() % 12u);
            a.a[0] = p.e;
            a.a[1] = c.rng->U32() % 3u;
            a.a[2] = Chance(c, 3) ? c.rng->U32() : c.rng->U32() % 4u;
        },
        [](NativeEnv& env, const Args& a) {
            return Run(env, [&](s::GuestRam& g, const s::SpineIo&) {
                s::CrashEmit(g, a.a[0], a.a[1], a.a[2]);
                return 0u;
            });
        },
        false));

    rows.push_back(SpineRow(
        "effect_burst", "void EffectBurst(Bike *e, s32 kind, s32 life, s32 tag)  [SLUS 0x80027778]", kBurst,
        [spawnerSetup](CaseContext& c, Args& a) {
            Pick p;
            spawnerSetup(c, p);
            a.a[0] = p.e;
            a.a[1] = Chance(c, 2) ? 1u : c.rng->U32() % 8u;
            a.a[2] = Chance(c, 2) ? 600u : c.rng->U32();
            a.a[3] = Chance(c, 2) ? 0u : c.rng->U32();
        },
        [](NativeEnv& env, const Args& a) {
            return Run(env, [&](s::GuestRam& g, const s::SpineIo& io) {
                s::EffectBurst(g, a.a[0], a.a[1], a.a[2], a.a[3], io);
                return 0u;
            });
        },
        false));

    rows.push_back(SpineRow(
        "effect_spray", "void EffectSpray(Bike *e, (unused), s32 kind)  [SLUS 0x80027974]", kSpray,
        [spawnerSetup](CaseContext& c, Args& a) {
            Pick p;
            spawnerSetup(c, p);
            a.a[0] = p.e;
            a.a[1] = Chance(c, 2) ? p.e + 0xB8u : c.rng->U32(); // never read
            static const uint32_t kKinds[4] = {0, 0, 2, 1};
            a.a[2] = Chance(c, 5) ? c.rng->U32() % 256u : kKinds[c.rng->U32() % 4u];
        },
        [](NativeEnv& env, const Args& a) {
            return Run(env, [&](s::GuestRam& g, const s::SpineIo& io) {
                s::EffectSpray(g, a.a[0], a.a[2], io);
                return 0u;
            });
        },
        false));

    // ------------------------------------------------------------------ RASHCDG 0x80090270
    // Every pool-0 bike's sleep word, class nibble and place are driven, and a partner is planted
    // (another live slot) behind a third of the owners, so both passes and the partner copy run.
    rows.push_back(SpineRow(
        "race_go", "void RaceGo(void)  [RASHCDG 0x80090270, the countdown's GO]", kGoEvent,
        [](CaseContext& c, Args&) {
            Memory& m = *c.mem;
            const uint32_t base = m.PeekWord(kPoolBasePtr);
            int32_t live = static_cast<int32_t>(m.PeekWord(kLiveBikes));
            if (live < 1 || live > 24) live = 1;
            PlantPlayers(c);
            const bool drive = c.sample == nullptr || Chance(c, 2);
            for (int32_t i = 0; i < live && drive; ++i) {
                const uint32_t e = base + 1096u * static_cast<uint32_t>(i);
                WriteU16Bench(m, e + 0x140u, static_cast<uint16_t>(Chance(c, 3) ? 0u : 1u + c.rng->U32() % 3u));
                const uint32_t rd = m.PeekWord(e + 0x43Cu);
                const uint32_t ow = m.PeekWord(e + 0x354u);
                if (rd >= 0x80000000u) {
                    uint8_t cls = m.PeekByte(rd + 1u);
                    if (Chance(c, 4)) cls = static_cast<uint8_t>((cls & 0xF0u) | (Chance(c, 2) ? 2u : c.rng->U32() % 16u));
                    m.WriteBlock(rd + 1u, &cls, 1);
                    const uint8_t place = static_cast<uint8_t>(Chance(c, 4) ? c.rng->U32() : 1u + c.rng->U32() % 20u);
                    m.WriteBlock(rd + 0x27u, &place, 1);
                }
                if (ow >= 0x80000000u) {
                    uint8_t b = m.PeekByte(ow + 0x23Cu);
                    const bool two = Chance(c, 3);
                    b = static_cast<uint8_t>((b & ~0x10u) | (two ? 0x10u : 0u));
                    m.WriteBlock(ow + 0x23Cu, &b, 1);
                    if (two) {
                        const uint32_t pe = base + 1096u * (c.rng->U32() % static_cast<uint32_t>(live));
                        m.PokeWord(e + 0x358u, pe);
                        const uint8_t depth = static_cast<uint8_t>(c.rng->U32() % 16u);
                        m.WriteBlock(pe + 0x3B2u, &depth, 1);
                    }
                }
            }
        },
        [](NativeEnv& env, const Args&) {
            return Run(env, [&](s::GuestRam& g, const s::SpineIo&) {
                s::RaceGo(g);
                return 0u;
            });
        },
        false));

    // ------------------------------------------------------------------ SLUS 0x8001447C
    // Half the cases plant a free list of their own in the scratch block - blocks sized to hit the
    // exact, the split and the too-small arms - whose last link is the machine's real head, so the
    // walk always ends in the real heap and never reaches the out-of-memory print.
    rows.push_back(SpineRow(
        "malloc", "void *Malloc(u32 n, u32 heap)  [SLUS 0x8001447C + 0x800142B4, first fit]", kMallocEntry,
        [](CaseContext& c, Args& a) {
            Memory& m = *c.mem;
            // BOUNDED INPUT FAMILY, named: the snapshot's heap has ONE free block (10824 bytes) and the
            // bench's own 8 KiB scratch block comes out of it, so sizes stay under 2400; above that
            // the original reaches its out-of-memory print through the BIOS, which the bench's
            // machine does not have (a trap on the guest side, a refusal on the port's).
            static const uint32_t kSizes[8] = {0, 1, 4, 5, 12, 100, 2048, 2300};
            const uint32_t n = Chance(c, 4) ? c.rng->U32() % 2400u : kSizes[c.rng->U32() % 8u];
            a.a[0] = n;
            a.a[1] = Chance(c, 4) ? c.rng->U32() % 5u : (Chance(c, 2) ? 1u : 0u);
            if (c.sample == nullptr) {
                const uint32_t want = (n + 11u) & ~7u;
                uint32_t next = m.PeekWord(s::kHeapHeads);
                uint32_t at = c.scratch + 4096u;
                const uint32_t blocks = 1u + c.rng->U32() % 4u;
                uint32_t addrs[4], sizes[4];
                for (uint32_t k = 0; k < blocks; ++k) {
                    const uint32_t roll = c.rng->U32() % 3u;
                    uint32_t sz = roll == 0 ? want : roll == 1 ? want + 8u * (1u + c.rng->U32() % 8u)
                                                            : (want > 16u ? want - 8u * (1u + c.rng->U32() % (want / 16u)) : 8u);
                    if (sz > 900u) sz = 8u * (1u + c.rng->U32() % 100u);
                    addrs[k] = at;
                    sizes[k] = sz;
                    at += 904u;
                }
                for (uint32_t k = blocks; k-- > 0;) {
                    m.PokeWord(addrs[k], next);
                    m.PokeWord(addrs[k] + 4u, sizes[k]);
                    next = addrs[k];
                }
                m.PokeWord(s::kHeapHeads, next);
            }
        },
        [](NativeEnv& env, const Args& a) {
            return Run(env, [&](s::GuestRam& g, const s::SpineIo&) {
                bool refused = false;
                const uint32_t v = s::SpineMalloc(g, a.a[0], a.a[1], refused);
                if (refused && env.failure.empty()) env.failure = "malloc: out of memory";
                return v;
            });
        },
        true));

    // ------------------------------------------------------------------ RASHCDG 0x8008A998
    rows.push_back(SpineRow(
        "view_event", "void ViewEvent(View *v, u32 mode)  [RASHCDG 0x8008A998, a leaf]", kViewEvent,
        [](CaseContext& c, Args& a) {
            Memory& m = *c.mem;
            const Pick p = PickBike(c);
            const uint32_t view = kViewArray + kViewStride * (c.rng->U32() % 2u);
            if (c.sample == nullptr || Chance(c, 2)) {
                m.PokeWord(view + 0x21Cu, Chance(c, 6) ? c.rng->U32() : c.rng->U32() % 15u);
                m.PokeWord(view + 0x220u, Chance(c, 6) ? c.rng->U32() : c.rng->U32() % 8u);
                m.PokeWord(view + 0x224u, c.rng->U32());
                m.PokeWord(view + 0x228u, c.rng->U32());
                WriteU16Bench(m, view + 0xACu, static_cast<uint16_t>(Chance(c, 2) ? 0x9Eu : c.rng->U32()));
                // the followed entity: a bike (handle < 32) or its rider (a pool-1 handle)
                const uint32_t rider = m.PeekWord(p.e + 0x354u);
                m.PokeWord(view + 0x238u, (Chance(c, 2) && rider >= 0x80000000u) ? rider : p.e);
            }
            a.a[0] = view;
            a.a[1] = Chance(c, 6) ? c.rng->U32() : c.rng->U32() % 13u;
        },
        [](NativeEnv& env, const Args& a) {
            return Run(env, [&](s::GuestRam& g, const s::SpineIo&) {
                s::ViewEvent(g, a.a[0], a.a[1]);
                return 0u;
            });
        },
        false));

    // ------------------------------------------------------------------ SLUS 0x8002090C (+ 0x8002076C)
    // The contact record is planted in the scratch block with a handle of every pool the release
    // distinguishes (a bike, a car, a prop, and the ones it ignores), and the partner +0x358 / +0x440
    // pair is driven so the partner's +0x1E8 store runs.
    rows.push_back(SpineRow(
        "reset_bike_state", "void ResetBikeState(Bike *e)  [SLUS 0x8002090C, with ReleaseContact 0x8002076C]",
        kWipeoutEffect,
        [](CaseContext& c, Args& a) {
            Memory& m = *c.mem;
            const Pick p = PickBike(c);
            const uint32_t contact = c.scratch + 64u;
            const uint32_t roll = c.rng->U32() % 6u;
            if (roll == 0) {
                m.PokeWord(p.e + 0x340u, 0);
            } else {
                static const uint32_t kPools[5] = {0, 3, 4, 1, 5};
                const uint32_t pool = kPools[roll - 1u];
                const uint32_t slot = pool == 0 ? c.rng->U32() % static_cast<uint32_t>(p.live) : c.rng->U32() % 8u;
                WriteU16Bench(m, contact, static_cast<uint16_t>((pool << 5) | slot));
                m.PokeWord(p.e + 0x340u, contact);
                if (pool == 3u) {
                    const uint32_t car = s::kSpineTrafficPool + 512u * slot;
                    m.PokeWord(car + 0x1E0u, static_cast<uint32_t>(c.rng->S32() >> (8 + c.rng->U32() % 8u)));
                    for (uint32_t k = 0; k < 3; ++k) WriteU16Bench(m, car + 0x1C2u + 2u * k, static_cast<uint16_t>(c.rng->Unit()));
                }
            }
            uint32_t f234 = m.PeekWord(p.e + 0x234u);
            if (Chance(c, 2)) f234 = (f234 & ~0x60000u) | (Chance(c, 3) ? 0x20000u : 0x40000u);
            m.PokeWord(p.e + 0x234u, f234);
            if (c.sample == nullptr) {
                m.PokeWord(p.e + 0x230u, c.rng->U32());
                m.PokeWord(p.e + 0x238u, c.rng->U32());
                m.PokeWord(p.e + 0x240u, static_cast<uint32_t>(c.rng->S32() >> (8 + c.rng->U32() % 10u)));
                for (uint32_t off : {0x206u, 0x20Cu, 0x212u}) WriteU16Bench(m, p.e + off, static_cast<uint16_t>(c.rng->U32()));
                for (uint32_t k = 0; k < 3; ++k) WriteU16Bench(m, p.e + 0x1C2u + 2u * k, static_cast<uint16_t>(c.rng->Unit()));
            }
            if (Chance(c, 3)) {
                m.PokeWord(p.e + 0x358u, c.scratch + 1024u);
                m.PokeWord(p.e + 0x440u, Chance(c, 2) ? 0u : 1u);
            }
            a.a[0] = p.e;
        },
        [](NativeEnv& env, const Args& a) {
            return Run(env, [&](s::GuestRam& g, const s::SpineIo&) {
                if (!s::ResetBikeState(g, a.a[0]) && env.failure.empty()) env.failure = "ResetBikeState refused";
                return 0u;
            });
        },
        false));

    // ------------------------------------------------------------------ RASHCDG 0x800BC7CC
    // The command stack (depth 0..15, the top opcode 0 / 18 / other), the finished flag, and the
    // rider's stance category: half the cases force the category of the rider's current stance to 3,
    // so the stance event really runs - natively, witnessed, its pose/combat children on the oracle.
    {
        Row r = SpineRow(
            "stamp_result", "void StampResult(Entity *e)  [RASHCDG 0x800BC7CC, the finished rider's AI stack]",
            kResultStamp,
            [](CaseContext& c, Args& a) {
                Memory& m = *c.mem;
                const Pick p = PickBike(c);
                const uint8_t depth = static_cast<uint8_t>(1u + c.rng->U32() % 15u);
                if (c.sample == nullptr || Chance(c, 2)) m.WriteBlock(p.e + 0x3B2u, &depth, 1);
                const int32_t d = static_cast<int8_t>(m.PeekByte(p.e + 0x3B2u));
                static const uint16_t kTops[4] = {0, 18, 4, 16};
                WriteU16Bench(m, p.e + 0x3BCu + static_cast<uint32_t>((d - 1) * 8), kTops[c.rng->U32() % 4u]);
                uint32_t f = m.PeekWord(p.e + 0x230u);
                f = Chance(c, 3) ? (f & ~0x08000000u) : (f | 0x08000000u);
                m.PokeWord(p.e + 0x230u, f);
                const uint32_t R = m.PeekWord(p.e + 0x354u);
                // BOUNDED INPUT FAMILY, named: when the rider's current stance is forced into the fight
                // category (3), the fight record the call reads is pointed at a stance of the SAME
                // animation bank as the current one (0x800541D4 w0 bits 0..3). A stance of another
                // bank names a clip index the rider's loaded bank does not have, and the ORIGINAL's
                // clip start then reads through a wild pointer (measured: 25 guest-side traps in 1160
                // at 0x8005BF8C, HardStart) - an input the game does not produce.
                // SECOND NAMED BOUND, and never event 0: the fight record of a rider that is NOT in a
                // fight holds event 0 (all 18 riders of all four captures sit on record 41, event 0),
                // so the original never pairs a fight stance with event 0. And event 0 is the one
                // StanceEnter 0x800C4454 sends to clip 0 of the BIKE's animation object
                // `rider[+0x254][+0x21C]`, which is 0 on every bike of every capture: HardStart then
                // reads `*(0 + 0x28)` out of the kernel area and faults (measured: case 1049 of this
                // row on the gate seed, pool slot 0, current stance 11 forced to category 3, event 0,
                // unmapped load at 0x8005BF8C from 0x275A0C80).
                if (R >= 0x80000000u && Chance(c, 2)) {
                    const uint32_t st = m.PeekByte(R + 0x220u) | (m.PeekByte(R + 0x221u) << 8);
                    const uint32_t bank = (st < s::kStanceNone) ? (m.PeekWord(kAltKindTable + 8u * st) & 0xFu) : 0u;
                    std::vector<uint32_t> same;
                    for (uint32_t k = 1; k < s::kStanceNone && st < s::kStanceNone; ++k)
                        if ((m.PeekWord(kAltKindTable + 8u * k) & 0xFu) == bank) same.push_back(k);
                    if (!same.empty()) {
                        const uint32_t ev = same[c.rng->U32() % same.size()];
                        const uint32_t fr = m.PeekWord(kFightRecPtr) + 12u * m.PeekByte(R + 0x239u);
                        WriteU16Bench(m, fr, static_cast<uint16_t>(ev));
                        WriteU16Bench(m, kAltKindTable + 8u * st + 2u, 3);
                    }
                }
                const uint32_t gs = m.PeekWord(kGameStatePtr);
                m.PokeWord(gs + 0x10u, c.rng->U32() % 400000u);
                a.a[0] = p.e;
            },
            [](NativeEnv& env, const Args& a) {
                spinerows::StampCallees cs(env, env.sp);
                s::GuestRam g = RqView(env);
                g.SetScratchpad(env.clone->mem.scratchpad().data());
                if (!s::StampResult(g, a.a[0], cs) && env.failure.empty() && !g.Faulted())
                    env.failure = "StampResult refused";
                return RqDone(env, g, 0u);
            },
            false);
        r.oracleCallees = SpineStampSeams(0); // env.sp IS StampResult's own sp here (oracleFrame)
        r.oracleFrame = kStampFrame;
        r.maxSteps = 50'000'000;
        rows.push_back(r);
    }
}
