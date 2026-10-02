// Bench rows of the AI command pass on guest memory (src\game\sim\ai_cmd.h).
// Included by verify_physics.cpp inside its anonymous namespace, after rows_population.inc (pop::PopSeams
// supplies the stance event and the exact-sp oracle seam).
//
// Callees: the ported host-pointer functions (AiCmdRace 0x800BA7F4, AiPopCommand 0x800BC8DC, SetAimDelta
// 0x80093CAC) run natively on raw views of the clone's RAM; the stance event 0x800C4550 runs natively and
// WITNESSED (its unported pose / combat children on the oracle); the eight unported callees run on the
// oracle at the exact sp the original makes the call at.

namespace airows {

namespace s = rr::sim;

constexpr uint32_t kGapOk = 0x800BB448, kBackOffOk = 0x800BB8FC, kCloseOk = 0x800BBBB8, kPassOk = 0x800BBD44,
                   kStrikeReach = 0x800BC1EC, kOtherSide = 0x800BC618, kAimBeside = 0x800BC4FC,
                   kRideOn = 0x800BADB8, kTakeGap = 0x800BB280, kBackOff = 0x800BB5B8, kClose = 0x800BBA18,
                   kPass = 0x800BBC20, kPassStrike = 0x800BBE00, kCmdPass = 0x800BA4CC;
// the unported callees (ai_cmd.h AiCmdCallees)
constexpr uint32_t kCopIdle = 0x800BAA2C, kCopRelease = 0x80092AD4, kLeaveRace = 0x80092E04, kFight = 0x800C035C,
                   kIntercept = 0x800BBEBC, kStrike = 0x800C1370, kJbFinish = 0x800C9E74, kCopGap = 0x8009E444;
constexpr uint32_t kPopFrame = 32; // `addiu sp,sp,-32` at 0x800BC8DC

bool Chance(Rng& r, uint32_t n) { return r.Next() % n == 0; }
int32_t Range(Rng& r, int32_t lo, int32_t hi) {
    return lo + static_cast<int32_t>(r.U32() % static_cast<uint32_t>(hi - lo + 1));
}
void P8(Memory& m, uint32_t a, uint32_t v) {
    const uint8_t b = static_cast<uint8_t>(v);
    m.WriteBlock(a, &b, 1);
}
void P16(Memory& m, uint32_t a, uint32_t v) { WriteU16Bench(m, a, static_cast<uint16_t>(v)); }
uint32_t BikeAt(const Memory& m, uint32_t slot) { return m.PeekWord(kPoolBasePtr) + 1096u * slot; }
uint32_t Tune(const Memory& m, uint32_t off) { return m.PeekWord(s::kAiTune + off); }

std::vector<OracleCallee> CmdCallees() {
    return {OracleCallee{kCopIdle, 3},   OracleCallee{kCopRelease, 1}, OracleCallee{kLeaveRace, 2},
            OracleCallee{kFight, 3},     OracleCallee{kIntercept, 2},  OracleCallee{kStrike, 2},
            OracleCallee{kJbFinish, 0},  OracleCallee{kCopGap, 2},
            // the ported stance event, witnessed, and the stance layer's own seams under it
            OracleCallee{kAiStance, 3},  OracleCallee{s::kAnimTransitionCaptureFn, 3},
            OracleCallee{s::kAnimApplyFrameFn, 1}, OracleCallee{s::kCombatLeaveFn, 4},
            OracleCallee{s::kCombatEnterFn, 4}};
}

struct CmdSeams final : s::AiCmdCallees {
    NativeEnv& env;
    pop::PopSeams ps;
    explicit CmdSeams(NativeEnv& e) : env(e), ps(e) {}
    uint8_t* Raw(uint32_t a, uint32_t n) { return bs::RawAt(env.clone->mem, a, n); }
    const int16_t* Sqrt() { return reinterpret_cast<const int16_t*>(Raw(kSqrtTable, 2)); }

    bool CmdRace(uint32_t e, uint32_t) override {
        Memory& m = env.clone->mem;
        uint8_t* ep = Raw(e, 1096);
        uint8_t* rd = Raw(m.PeekWord(e + 1084u), 72);
        const uint8_t* gs = Raw(m.PeekWord(kGameStatePtr), 64);
        const uint8_t* sl = Raw(m.PeekWord(e + 340u), 52);
        if (ep == nullptr || rd == nullptr || gs == nullptr || sl == nullptr) return false;
        s::AiCmdRaceEnv ce;
        ce.gameState = gs;
        ce.riderDef = rd;
        ce.slice = reinterpret_cast<const int16_t*>(sl);
        ce.flags = m.PeekWord(kAiRaceFlags);
        ce.sqrtTable = Sqrt();
        s::AiCmdRace(s::EntityView(ep), ce);
        return true;
    }
    bool PopCommand(uint32_t e, uint32_t sp) override {
        Memory& m = env.clone->mem;
        uint8_t* ep = Raw(e, 1096);
        const uint32_t R = m.PeekWord(e + 852u);
        struct Sink final : s::AiStanceSink {
            CmdSeams& c;
            uint32_t sp;
            Sink(CmdSeams& x, uint32_t y) : c(x), sp(y) {}
            void PlayIdleStance(uint16_t event, uint32_t riderArg) override { c.ps.StanceEvent(event, riderArg, 2, sp); }
        } sink(*this, sp - kPopFrame);
        s::AiPopEnv pe;
        pe.gameState = Raw(m.PeekWord(kGameStatePtr), 64);
        pe.riderDef = Raw(m.PeekWord(e + 1084u), 72);
        pe.rider = Raw(R, 640);
        pe.riderAddress = R;
        pe.altKindTable = Raw(kAltKindTable, 8u * 65536u);
        pe.fightRecords = Raw(m.PeekWord(kFightRecPtr), 12u * 256u);
        pe.attackerMask = reinterpret_cast<const uint16_t*>(Raw(kAttackerMask, 16));
        pe.stance = &sink;
        if (ep == nullptr || pe.gameState == nullptr || pe.riderDef == nullptr || pe.altKindTable == nullptr)
            return false;
        s::AiPopCommand(ep, pe);
        return env.failure.empty();
    }
    bool SetAimDelta(uint32_t e, int32_t scalar, uint32_t) override {
        uint8_t* ep = Raw(e, 1096);
        if (ep == nullptr) return false;
        s::EntityView v(ep);
        const int16_t* dir = reinterpret_cast<const int16_t*>(ep + 0x368);
        s::SetAimDelta(v, nullptr, dir, &scalar, 1, Sqrt());
        return true;
    }
    bool StanceEvent(uint32_t ev, uint32_t r, uint32_t p, uint32_t sp) override { return ps.StanceEvent(ev, r, p, sp); }
    bool CopIdle(uint32_t e, uint32_t cmd, int32_t dt, uint32_t sp) override {
        return ps.Seam(kCopIdle, {e, cmd, pop::U(dt)}, sp);
    }
    bool CopRelease(uint32_t e, uint32_t sp) override { return ps.Seam(kCopRelease, {e}, sp); }
    bool LeaveRace(uint32_t e, int32_t dt, uint32_t sp) override { return ps.Seam(kLeaveRace, {e, pop::U(dt)}, sp); }
    bool Fight(uint32_t e, uint32_t t, int32_t dt, uint32_t sp) override {
        return ps.Seam(kFight, {e, t, pop::U(dt)}, sp);
    }
    bool Intercept(uint32_t e, uint32_t t, uint32_t sp) override { return ps.Seam(kIntercept, {e, t}, sp); }
    bool Strike(uint32_t e, uint32_t t, uint32_t sp) override { return ps.Seam(kStrike, {e, t}, sp); }
    bool JailbreakFinish(uint32_t sp) override { return ps.Seam(kJbFinish, {}, sp); }
    bool CopGap(uint32_t e, uint32_t t, uint32_t sp, int32_t& v0) override {
        uint32_t r = 0;
        const bool ok = ps.Seam(kCopGap, {e, t}, sp, &r);
        v0 = static_cast<int32_t>(r);
        return ok;
    }
};

// ---------------------------------------------------------------- the inputs
// The fields the tests and arms read on bike `b`, drawn over this machine's own values.
void DrawBike(CaseContext& c, uint32_t b) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    const uint32_t R = m.PeekWord(b + 852u), rd = m.PeekWord(b + 1084u);
    P16(m, b + 320u, Chance(r, 8) ? 0u : (Chance(r, 3) ? 3u : 1u));                // awake (+ bit 1)
    if (R != 0u) m.PokeWord(R + 604u, Chance(r, 5) ? r.U32() % 4u : 1u);             // mounted
    PutS32(m, b + 344u, Range(r, -0x80000, 0x80000));                                 // lateral
    PutS32(m, b + 364u, Chance(r, 3) ? -1 : 1);                                       // direction
    PutS32(m, b + 480u, Range(r, 0, 0x400000));                                       // speed
    PutS32(m, b + 576u, Range(r, 0, 0x400000));
    PutS32(m, b + 308u, Range(r, 0, 0x40000));                                        // +0x134
    PutS32(m, b + 304u, Range(r, 0, 0x20000));                                        // +0x130
    m.PokeWord(b + 372u, Chance(r, 3) ? 0u : 1u);                                     // lane data
    P16(m, b + 362u, Chance(r, 3) ? r.U32() % 3u : 0u);
    P16(m, b + 420u, r.U32() % 4u);
    P16(m, b + 408u, r.U32() % 4u);
    PutS32(m, b + 424u, Range(r, 0x10000, 0x40000));
    m.PokeWord(b + 388u, r.U32() & 0x11u);
    m.PokeWord(b + 568u, Chance(r, 5) ? (r.U32() & 0x600u) : 0u);                    // flagsC 0x600
    m.PokeWord(b + 564u, r.U32() & 0x00080200u);                                      // flagsB 19, 9
    m.PokeWord(b + 560u, (m.PeekWord(b + 560u) & ~0x08000000u) | (Chance(r, 5) ? 0u : 0x08000000u));
    PutS32(m, b + 924u, Range(r, 0, 0x600000));
    PutS32(m, b + 916u, r.S32() >> 8);
    PutS32(m, b + 920u, r.S32() >> 8);
    P16(m, b + 944u, Chance(r, 2) ? static_cast<uint32_t>(Range(r, -4, 400)) : 0u);  // the slide
    for (uint32_t k = 0; k < 3; ++k) PutS32(m, b + 892u + 4u * k, Chance(r, 4) ? Range(r, -8, 8) : r.S32() >> 12);
    P16(m, b + 878u, static_cast<uint32_t>(Range(r, -300, 300)));                     // +0x36E
    m.PokeWord(b + 856u, Chance(r, 5) ? BikeAt(m, r.U32() % 18u) : 0u);               // partner
    if (rd != 0u) {
        if (Chance(r, 3)) P8(m, rd + 1u, (m.PeekByte(rd + 1u) & 0xF0u) | (Chance(r, 2) ? 2u : r.U32() % 4u));
        if (Chance(r, 3)) m.PokeWord(rd + 40u, Chance(r, 2) ? 0u : r.U32() | 1u);   // finished
        if (Chance(r, 3)) P8(m, rd + 39u, r.U32() % 8u);                               // place
    }
    if (Chance(r, 4)) m.PokeWord(b + 360u, m.PeekWord(BikeAt(m, r.U32() % 18u) + 360u));
}

// A command stack of depth 1..5 with opcodes from `ops` and targets among this machine's bikes.
void DrawStack(CaseContext& c, uint32_t b, const std::vector<uint32_t>& ops, uint32_t topOp, uint32_t topTarget) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    const uint32_t depth = 1u + r.U32() % 5u;
    P8(m, b + 946u, depth);
    for (uint32_t k = 1; k <= depth; ++k) {
        const uint32_t slot = b + 948u + 8u * k;
        P16(m, slot, ops[r.U32() % ops.size()]);
        P16(m, slot + 2u, Chance(r, 8) ? 224u : r.U32() % 18u);
        P16(m, slot + 4u, Chance(r, 4) ? r.U32() : 0u);
        P16(m, slot + 6u, r.U32());
    }
    const uint32_t top = b + 948u + 8u * depth;
    P16(m, top, topOp);
    P16(m, top + 2u, topTarget);
}

// The race-wide words: players, the race phase +0x39, the bank, the attacker masks.
void DrawRaceWords(CaseContext& c) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    const uint32_t gs = m.PeekWord(kGameStatePtr);
    m.PokeWord(gs + 0x30u, Chance(r, 3) ? 2u : 1u);
    P8(m, gs + 0x39u, Chance(r, 3) ? r.U32() % 5u : 0u);
    m.PokeWord(gs + 0x3Cu, r.U32() % 3u);
    if (Chance(r, 4)) P8(m, gs + 4u, Chance(r, 2) ? 44u : r.U32());
    for (uint32_t p = 0; p < 2; ++p) P16(m, kAttackerMask + 2u * p, Chance(r, 2) ? r.U32() & 0x3FFFFu : 0u);
}

// e and t, two of this machine's bikes, placed `gap` apart along the route (gap = (e - t) << 4).
void DrawPair(CaseContext& c, uint32_t& e, uint32_t& t, int32_t& gap, int32_t span) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    const uint32_t a = r.U32() % 18u;
    uint32_t b = r.U32() % 17u;
    if (b >= a) ++b;
    e = BikeAt(m, a);
    t = BikeAt(m, b);
    DrawRaceWords(c);
    DrawBike(c, e);
    DrawBike(c, t);
    if (c.sample != nullptr && c.sample->entity.size() >= 0x448) {
        uint32_t v;
        for (uint32_t off : {0x158u, 0x16Cu, 0x1E0u}) {
            std::memcpy(&v, c.sample->entity.data() + off, 4);
            m.PokeWord(e + off, v);
        }
    }
    gap = Range(r, -span, span) & ~0xF;
    PutS32(m, t + 324u, GetS32(m, e + 324u) - (gap >> 4));
}

uint32_t Done(NativeEnv& env, s::GuestRam& g, bool ok, const char* what, uint32_t v = 0) {
    return pop::PopDone(env, g, ok, what, v);
}

Row CmdRow(const char* name, const char* sig, uint32_t entry, std::function<void(CaseContext&, Args&)> setup,
           std::function<uint32_t(NativeEnv&, const Args&)> native, bool v0) {
    Row r{name, sig, entry, std::move(setup), nullptr, v0, std::move(native)};
    r.maxSteps = 40'000'000;
    r.oracleCallees = CmdCallees();
    return r;
}

// A test's case: the pair, and a gap around the test's own thresholds.
void SetupTest(CaseContext& c, Args& a) {
    uint32_t e, t;
    int32_t gap;
    const int32_t span = static_cast<int32_t>(Tune(*c.mem, 0x0C)) + 0x100000;
    DrawPair(c, e, t, gap, span);
    uint16_t th = 0;
    c.mem->ReadBlock(t + 172u, &th, 2);
    DrawStack(c, e, {3, 4, 5, 6, 7, 8, 9, 14, 16}, 4u + c.rng->U32() % 6u, th);
    DrawStack(c, t, {3, 4, 5, 6, 7, 14, 15, 16, 17}, c.rng->U32() % 19u, c.rng->U32() % 18u);
    a.a[0] = e;
    a.a[1] = t;
    a.a[2] = static_cast<uint32_t>(c.rng->Next() % 4u == 0 ? c.rng->S32() >> 8 : gap);
}

// An arm's case: the pair, the top command (op, t's handle - or 224 now and then).
void SetupArm(CaseContext& c, Args& a, uint32_t op) {
    uint32_t e, t;
    int32_t gap;
    DrawPair(c, e, t, gap, 0x600000);
    uint16_t th = 0;
    c.mem->ReadBlock(t + 172u, &th, 2);
    const uint32_t tg = Chance(*c.rng, 12) ? 224u : th;
    DrawStack(c, e, {3, 4, 5, 6, 7, 8, 9, 14, 15}, op, tg);
    DrawStack(c, t, {3, 4, 5, 6, 7, 14, 15, 16, 17}, c.rng->U32() % 19u, c.rng->U32() % 18u);
    a.a[0] = e;
    a.a[1] = tg;
}

using TestFn = int32_t (*)(s::GuestRam&, uint32_t, uint32_t, int32_t);
std::function<uint32_t(NativeEnv&, const Args&)> TestNative(TestFn f, const char* what) {
    return [f, what](NativeEnv& env, const Args& a) {
        s::GuestRam g = RqView(env);
        const int32_t v = f(g, a.a[0], a.a[1], static_cast<int32_t>(a.a[2]));
        return Done(env, g, true, what, static_cast<uint32_t>(v));
    };
}

} // namespace airows

void AddAiRows(std::vector<Row>& rows) {
    using namespace airows;
    rows.push_back(CmdRow("ai_gap_ok", "s32 AiGapOk(Bike *e, Bike *t, s32 gap)  [RASHCDG 0x800BB448]", kGapOk,
                          SetupTest, TestNative(s::AiGapOk, "AiGapOk"), true));
    rows.push_back(CmdRow("ai_backoff_ok", "s32 AiBackOffOk(Bike *e, Bike *t, s32 gap)  [RASHCDG 0x800BB8FC]",
                          kBackOffOk, SetupTest, TestNative(s::AiBackOffOk, "AiBackOffOk"), true));
    rows.push_back(CmdRow("ai_close_ok", "s32 AiCloseOk(Bike *e, Bike *t, s32 gap)  [RASHCDG 0x800BBBB8]", kCloseOk,
                          SetupTest, TestNative(s::AiCloseOk, "AiCloseOk"), true));
    rows.push_back(CmdRow("ai_pass_ok", "s32 AiPassOk(Bike *e, Bike *t, s32 gap)  [RASHCDG 0x800BBD44]", kPassOk,
                          SetupTest, TestNative(s::AiPassOk, "AiPassOk"), true));
    rows.push_back(CmdRow("ai_strike_reach", "s32 AiStrikeReach(Bike *e, Bike *t, s32 gap)  [RASHCDG 0x800BC1EC]",
                          kStrikeReach, SetupTest, TestNative(s::AiStrikeReach, "AiStrikeReach"), true));
    rows.push_back(CmdRow(
        "ai_other_side", "s32 AiOtherSide(Bike *t, u16 handle, s32 side)  [RASHCDG 0x800BC618]", kOtherSide,
        [](CaseContext& c, Args& a) {
            SetupTest(c, a);
            uint16_t h = 0;
            c.mem->ReadBlock(a.a[0] + 172u, &h, 2);
            a.a[0] = a.a[1];                                  // t
            a.a[1] = Chance(*c.rng, 6) ? c.rng->U32() : h;    // the asker's handle (the callers pass a u16)
            a.a[1] &= 0xFFFFu;
            a.a[2] = static_cast<uint32_t>(c.rng->S32() >> (c.rng->U32() % 24u));
        },
        [](NativeEnv& env, const Args& a) {
            s::GuestRam g = RqView(env);
            return Done(env, g, true, "AiOtherSide",
                        static_cast<uint32_t>(s::AiOtherSide(g, a.a[0], a.a[1], static_cast<int32_t>(a.a[2]))));
        },
        true));
    rows.push_back(CmdRow(
        "ai_aim_beside", "void AiAimBeside(Bike *e, Bike *t, s32 offset)  [RASHCDG 0x800BC4FC]", kAimBeside,
        [](CaseContext& c, Args& a) {
            SetupTest(c, a);
            a.a[2] = static_cast<uint32_t>(Range(*c.rng, -0x60000, 0x60000));
            // the aim axis +0x368 as a unit-ish s16 vector
            for (uint32_t k = 0; k < 3; ++k) P16(*c.mem, a.a[0] + 872u + 2u * k, static_cast<uint32_t>(c.rng->Unit()));
        },
        [](NativeEnv& env, const Args& a) {
            s::GuestRam g = RqView(env);
            CmdSeams cs(env);
            const bool ok = s::AiAimBeside(g, a.a[0], a.a[1], static_cast<int32_t>(a.a[2]), env.sp, cs);
            return Done(env, g, ok, "AiAimBeside");
        },
        false));

    auto arm = [&](const char* name, const char* sig, uint32_t entry, uint32_t op,
                   bool (*fn)(s::GuestRam&, uint32_t, uint32_t, uint32_t, s::AiCmdCallees&)) {
        rows.push_back(CmdRow(
            name, sig, entry, [op](CaseContext& c, Args& a) { SetupArm(c, a, op); },
            [fn, name](NativeEnv& env, const Args& a) {
                s::GuestRam g = RqView(env);
                CmdSeams cs(env);
                const bool ok = fn(g, a.a[0], a.a[1], env.sp, cs);
                return Done(env, g, ok && env.failure.empty(), name);
            },
            false));
    };
    arm("ai_cmd_take_gap", "void AiCmdTakeGap(Bike *e, u16 target)  [RASHCDG 0x800BB280, op 5]", kTakeGap, 5,
        s::AiCmdTakeGap);
    rows.push_back(CmdRow(
        "ai_cmd_back_off", "void AiCmdBackOff(Bike *e, u16 target)  [RASHCDG 0x800BB5B8, op 6]", kBackOff,
        [](CaseContext& c, Args& a) { SetupArm(c, a, 6); },
        [](NativeEnv& env, const Args& a) {
            s::GuestRam g = RqView(env);
            CmdSeams cs(env);
            const bool ok = s::AiCmdBackOff(g, a.a[0], a.a[1], env.sp, cs, cs.Sqrt());
            return Done(env, g, ok && env.failure.empty(), "AiCmdBackOff");
        },
        false));
    arm("ai_cmd_close", "void AiCmdClose(Bike *e, u16 target)  [RASHCDG 0x800BBA18, op 7]", kClose, 7, s::AiCmdClose);
    arm("ai_cmd_pass", "void AiCmdPass(Bike *e, u16 target)  [RASHCDG 0x800BBC20, op 8]", kPass, 8, s::AiCmdPass);
    arm("ai_cmd_pass_strike", "void AiCmdPassStrike(Bike *e, u16 target)  [RASHCDG 0x800BBE00, op 9]", kPassStrike, 9,
        s::AiCmdPassStrike);
    rows.push_back(CmdRow(
        "ai_cmd_ride_on", "void AiCmdRideOn(Bike *e, Cmd *c)  [RASHCDG 0x800BADB8, op 2]", kRideOn,
        [](CaseContext& c, Args& a) {
            SetupArm(c, a, 2);
            Memory& m = *c.mem;
            Rng& r = *c.rng;
            const uint32_t e = a.a[0];
            // a cop's target sometimes, the jailbreak phases, player 1's class
            const uint32_t slot = e + 948u + 8u * m.PeekByte(e + 946u);
            if (Chance(r, 3)) P16(m, slot + 2u, Chance(r, 2) ? 0u : r.U32() % 18u);
            if (Chance(r, 3)) P16(m, slot + 6u, (m.PeekWord(slot + 4u) >> 16) | 0x8000u);
            if (Chance(r, 4)) PutS32(m, e + 480u, Range(r, 0, 0x10000));
            a.a[1] = slot;
        },
        [](NativeEnv& env, const Args& a) {
            s::GuestRam g = RqView(env);
            CmdSeams cs(env);
            const bool ok = s::AiCmdRideOn(g, a.a[0], a.a[1], env.sp, cs);
            return Done(env, g, ok && env.failure.empty(), "AiCmdRideOn");
        },
        false));
    rows.push_back(CmdRow(
        "ai_command_pass", "void AiCommandPass(s32 dt, u32 skip, u32 maskB)  [RASHCDG 0x800BA4CC]", kCmdPass,
        [](CaseContext& c, Args& a) {
            Memory& m = *c.mem;
            Rng& r = *c.rng;
            DrawRaceWords(c);
            // every bike: its own fields, a stack whose top is one of the arms the pass reaches
            for (uint32_t i = 0; i < 18; ++i) {
                const uint32_t b = BikeAt(m, i);
                if (Chance(r, 3)) continue;                       // keep the machine's own bike as it is
                DrawBike(c, b);
                static const uint32_t kTop[] = {0, 2, 3, 4, 4, 5, 6, 7, 8, 9, 10, 13, 14, 15, 19};
                uint32_t top = kTop[r.U32() % (sizeof(kTop) / sizeof(kTop[0]))];
                uint32_t tg = r.U32() % 18u;
                if (tg == i) tg = (tg + 1u) % 18u;
                DrawStack(c, b, {3, 4, 5, 6, 7, 8, 14}, top, Chance(r, 10) ? 224u : tg);
            }
            if (Chance(r, 4)) m.PokeWord(kLiveBikes, r.U32() % 19u);
            a.a[0] = c.sample ? 0x884u : static_cast<uint32_t>(Range(r, 0, 0x2000));
            a.a[1] = Chance(r, 2) ? 0u : r.U32() & 0x3FFFFu;    // skip
            a.a[2] = r.U32() & 0x3FFFFu;                        // maskB
        },
        [](NativeEnv& env, const Args& a) {
            s::GuestRam g = RqView(env);
            CmdSeams cs(env);
            const bool ok = s::AiCommandPass(g, static_cast<int32_t>(a.a[0]), a.a[1], a.a[2], env.sp, cs, cs.Sqrt());
            return Done(env, g, ok && env.failure.empty(), "AiCommandPass");
        },
        false));
}
