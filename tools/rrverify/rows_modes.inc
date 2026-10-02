// Bench rows for the game modes' own rules (src\game\sim\modes.{h,cpp}; docs\formats\rules.md 9).
// Included LAST by verify_physics.cpp inside its anonymous namespace,
// after rows_cops.inc, whose helpers (copr::DrawGlobals, copr::DrawBike, copr::Place, copr::Speed ...)
// these rows reuse, and rows_population.inc's (pop::View, pop::PopDone, pop::PopSeams).
//
// One row per function:
//   mode_stun 0x80097308, mode_jail_test 0x8009DA4C (leaves, no seam);
//   mode_jail_release 0x800A0708 (seam: Remount 0x800903F4; MulAdd native);
//   mode_arrest 0x80096F30 (seams: SpeechCue SLUS 0x8001B244, the arrest scene 0x8009D664, ViewEvent
//     0x8008A998; the stance event, AiClearCommands, AiPushCommand native);
//   mode_arrest_fsm 0x80096818 (seams: SpeechCue, ComputePlace SLUS 0x800138E8, CopJoin SLUS 0x80028034,
//     the voices' hold SLUS 0x80020E30; FindRouteLeg and DotLcm native);
//   mode_milestone 0x800C8D4C (seams: the escape scene 0x800C9420, ViewEvent, Remount, the rider's model
//     SLUS 0x800302C4; AiClearCommands / AiPushCommand / MulAdd native);
//   mode_milestone_first 0x800C92F8 (DotLcm native).
//
// INPUT FAMILIES start from THIS machine (rr-race: player 1 at slot 0, 18 bikes, two dormant cops). Drawn
// over it, per case (copr::DrawGlobals / DrawBike / Place, and here): the player count, the race type
// (0, 1, 0x21 = 33, 0x22, 0x11, 44, anything), the designated suspect gs+0x06 (the target's handle half
// the time), the bank, the clock, the phase gs+0x39 0..6, the arrest word 0x8005AD48 (0, 1, 2, 4, 8, 16,
// 3, anything), the quota 0x8005AD44 around 0..2, the beat clock 0x8005B2EC around the 2.5 s beat and
// 20000, the roadside flag 0x8005B2B0, the post-race skip 0x8005B220, the time limit 0x8005ACC8 either
// side of the clock, the Jailbreak direction 0x8005B2E8; for the player and the target: the class nibble
// (2 = police), the place byte (246..255), the finish word, the rider's mount state and +0x228 bits, the
// speeds either side of 8.94 / 7.00, the flags +0x230 / +0x234 / +0x24, the command stack, the view
// record's +0x228 / +0x224 / +0x304 / +0x21C / +0x220, the heading against the slice (dot either side of
// 0xF332 / 0xF5C2 / -46333), the along against the milestone, the road piece +0x174 and the direction.
//
// NAMED BOUNDS:
//   * mode_milestone phase 2 reads the partner +0x358 of player 1's bike with no null test (as every
//     two-rider path does): the phase-2 cases plant a partner (another pool-0 bike);
//   * stances drawn are the captured one or 72 / 73 (copr::DrawBike's bound: another may not be resident
//     and the ORIGINAL traps in HardStart 0x8005BF8C inside the stance event);
//   * the busted-music bound of rows_cops.inc does not arise: none of these functions reaches it.

namespace moder {

using pop::Chance;
using pop::Put8;
using pop::U;

constexpr uint32_t kBikes = 18;
constexpr uint32_t kPlaceFn = 0x800138E8, kCopJoinFn = 0x80028034;

uint32_t Bk(const Memory& m, uint32_t i) { return aip::BikeAt(m, i); }
uint32_t Rd(const Memory& m, uint32_t e) { return m.PeekWord(e + 1084); }
uint32_t Rider(const Memory& m, uint32_t e) { return m.PeekWord(e + 852); }
uint32_t ViewOf(const Memory& m, uint32_t e) { return 0x800CD898u + 1132u * aip::Get16(m, e + 0xAC); }

// The mode globals of the header's list, over copr::DrawGlobals (which draws the player count, the race
// type, gs+0x06 / +0x39 / +0x3C / +0x10 and the arrest word).
void DrawModeGlobals(CaseContext& c) {
    copr::DrawGlobals(c, false);
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    const uint32_t gs = m.PeekWord(kGameStatePtr);
    static const uint8_t kTypes[] = {1, 1, 33, 33, 0x22, 0x11, 44, 0, 0x21, 0x23};
    if (!Chance(r, 5)) Put8(m, gs + 4, kTypes[r.U32() % 10u]);
    static const uint32_t kWords[] = {0, 0, 1, 2, 4, 8, 16, 3};
    m.PokeWord(0x8005AD48, Chance(r, 10) ? r.U32() & 0x3Fu : kWords[r.U32() % 8u]);
    PutS32(m, 0x8005AD44, static_cast<int32_t>(r.U32() % 4u) - 1);
    {
        static const int32_t kBeat[] = {0x28000, 20000, 0x50000, 0};
        PutS32(m, 0x8005B2EC, copr::Around(r, kBeat[r.U32() % 4u]));
    }
    m.PokeWord(0x8005B2B0, Chance(r, 2) ? 0u : 1u);
    m.PokeWord(0x8005B220, Chance(r, 4) ? 1u + r.U32() % 2u : 0u);
    const int32_t clock = GetS32(m, gs + 16);
    PutS32(m, 0x8005ACC8, clock + static_cast<int32_t>(r.U32() % 2001u) - 1000);
    PutS32(m, 0x8005B2E8, Chance(r, 2) ? 1 : -1);
    Put8(m, gs + 57, r.U32() % 7u);
}

// A bike's mode fields (the header's list) over copr::DrawBike.
void DrawModeBike(CaseContext& c, uint32_t e, uint32_t other, bool cop) {
    copr::DrawBike(c, e, other, cop);
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    const uint32_t rd = Rd(m, e), R = Rider(m, e);
    if (!aip::InRam(rd) || !aip::InRam(R)) return;
    if (cop && Chance(r, 5)) Put8(m, rd + 1, (m.PeekByte(rd + 1) & 0xF0u) | 2u);
    if (Chance(r, 2)) m.PokeWord(rd + 40, 0);
    if (Chance(r, 4)) m.PokeWord(R + 552, m.PeekWord(R + 552) ^ 0x8000u);
    m.PokeWord(R + 604, Chance(r, 2) ? r.U32() % 2u : r.U32() % 5u);
    {
        static const int32_t kAt[] = {0x8F0D8, 0x6B4A1, 131, 0};
        if (Chance(r, 2)) PutS32(m, e + 480, copr::Around(r, kAt[r.U32() % 4u]));
    }
    if (Chance(r, 2)) m.PokeWord(e + 560, m.PeekWord(e + 560) ^ 0x08000000u);
    if (Chance(r, 3)) m.PokeWord(e + 560, m.PeekWord(e + 560) ^ 0x20000000u);
    if (Chance(r, 3)) m.PokeWord(e + 564, m.PeekWord(e + 564) ^ 0x200u);
    if (Chance(r, 3)) m.PokeWord(e + 36, m.PeekWord(e + 36) ^ 0x08000000u);
    if (Chance(r, 3)) WriteU16Bench(m, e + 362, static_cast<uint16_t>(Chance(r, 2) ? 0u : r.U32()));
    const uint32_t view = ViewOf(m, e);
    if (view < 0x800CD898u + 2u * 1132u) {
        m.PokeWord(view + 552, Chance(r, 2) ? m.PeekWord(view + 552) : r.U32() & 0x7Fu);
        m.PokeWord(view + 548, Chance(r, 3) ? r.U32() & 0x2008u : 0u);
        m.PokeWord(view + 772, Chance(r, 4) ? 1u : 0u);
        if (Chance(r, 3)) m.PokeWord(view + 540, r.U32() % 12u);
        if (Chance(r, 3)) m.PokeWord(view + 544, r.U32() % 12u);
    }
    // the heading +0x1BC and +0x210 against the slice's tangent (either side of the tests' thresholds)
    const uint32_t sl = m.PeekWord(e + 340);
    if (aip::InRam(sl) && Chance(r, 2)) {
        int16_t t[3];
        for (uint32_t k = 0; k < 3; ++k) t[k] = GetS16(m, sl + 14 + 2 * k);
        const int s = Chance(r, 2) ? 1 : -1;
        for (uint32_t k = 0; k < 3; ++k) {
            const int16_t v = static_cast<int16_t>(s * t[k] + static_cast<int32_t>(r.U32() % 1025u) - 512);
            WriteU16Bench(m, e + 444 + 2 * k, static_cast<uint16_t>(v));
            WriteU16Bench(m, e + 528 + 2 * k, static_cast<uint16_t>(Chance(r, 3) ? r.U32() : static_cast<uint32_t>(v)));
        }
    }
}

std::vector<OracleCallee> ModeSeamCallees() {
    std::vector<OracleCallee> v = pop::PopSeamCallees();
    for (const OracleCallee& oc :
         {OracleCallee{rr::sim::kModeSpeechCueFn, 1}, OracleCallee{kPlaceFn, 2}, OracleCallee{kCopJoinFn, 1},
          OracleCallee{rr::sim::kModeSoundHoldFn, 1}, OracleCallee{rr::sim::kModeArrestSceneFn, 2},
          OracleCallee{rr::sim::kModeViewEventFn, 2}, OracleCallee{rr::sim::kModeRemountFn, 2},
          OracleCallee{rr::sim::kModeEscapeSceneFn, 1}, OracleCallee{rr::sim::kModeRiderModelFn, 2}})
        v.push_back(oc);
    return v;
}

struct Seams final : rr::sim::ModeCallees {
    NativeEnv& env;
    pop::PopSeams ps;
    explicit Seams(NativeEnv& e) : env(e), ps(e) {}
    bool SpeechCue(int32_t kind, uint32_t sp) override { return ps.Seam(rr::sim::kModeSpeechCueFn, {U(kind)}, sp); }
    bool ComputePlace(uint32_t e, int32_t mode, uint32_t sp, int32_t& v0) override {
        uint32_t v = 0;
        const bool ok = ps.Seam(kPlaceFn, {e, U(mode)}, sp, &v);
        v0 = static_cast<int32_t>(v);
        return ok;
    }
    bool CopJoin(uint32_t e, uint32_t sp) override { return ps.Seam(kCopJoinFn, {e}, sp); }
    bool ViewEvent(uint32_t view, uint32_t mode, uint32_t sp) override {
        return ps.Seam(rr::sim::kModeViewEventFn, {view, mode}, sp);
    }
    bool StanceEvent(uint32_t ev, uint32_t rider, uint32_t p, uint32_t sp) override {
        return ps.StanceEvent(ev, rider, p, sp);
    }
    bool ClearCommands(uint32_t e, uint32_t sp) override { return ps.ClearCommands(e, sp); }
    bool PushCommand(uint32_t cmd, int32_t mode, uint32_t e, uint32_t sp) override {
        return ps.PushCommand(cmd, mode, e, sp);
    }
    bool Remount(uint32_t e, int32_t fromRoad, uint32_t sp) override {
        return ps.Seam(rr::sim::kModeRemountFn, {e, U(fromRoad)}, sp);
    }
    bool SoundHold(int32_t v, uint32_t sp) override { return ps.Seam(rr::sim::kModeSoundHoldFn, {U(v)}, sp); }
    bool ArrestScene(uint32_t cop, uint32_t suspect, uint32_t sp, uint32_t& v0) override {
        return ps.Seam(rr::sim::kModeArrestSceneFn, {cop, suspect}, sp, &v0);
    }
    bool EscapeScene(int32_t block, uint32_t sp) override {
        return ps.Seam(rr::sim::kModeEscapeSceneFn, {U(block)}, sp);
    }
    bool RiderModel(uint32_t rider, uint32_t id, uint32_t sp) override {
        return ps.Seam(rr::sim::kModeRiderModelFn, {rider, id}, sp);
    }
};

Row ModeRow(const char* name, const char* sig, uint32_t entry, std::function<void(CaseContext&, Args&)> setup,
            std::function<uint32_t(NativeEnv&, const Args&)> native, bool v0, bool seams) {
    Row r{name, sig, entry, std::move(setup), nullptr, v0, std::move(native)};
    r.maxSteps = 40'000'000;
    if (seams) r.oracleCallees = ModeSeamCallees();
    return r;
}

// The player cop (mostly the player's bike) and a target (mostly another racer, or the designated suspect).
void DrawCopPair(CaseContext& c, uint32_t& e, uint32_t& t) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    DrawModeGlobals(c);
    e = Chance(r, 5) ? Bk(m, r.U32() % kBikes) : m.PeekWord(kPlayerArr);
    t = Bk(m, 1u + r.U32() % (kBikes - 1u));
    if (Chance(r, 8)) t = e;
    const uint32_t eh = aip::Get16(m, e + 0xAC), th = aip::Get16(m, t + 0xAC);
    DrawModeBike(c, e, th, true);
    if (t != e) {
        DrawModeBike(c, t, eh, false);
        copr::Place(c, e, t);
    }
    if (Chance(r, 2)) Put8(m, m.PeekWord(kGameStatePtr) + 6, th);   // the designated suspect
}

// ---------------------------------------------------------------------------- the leaves
void SetupStun(CaseContext& c, Args& a) {
    uint32_t e, t;
    DrawCopPair(c, e, t);
    a.a[0] = t;
}
uint32_t NativeStun(NativeEnv& env, const Args& a) {
    rr::sim::GuestRam g = pop::View(env);
    rr::sim::ModeStun(g, a.a[0]);
    return pop::PopDone(env, g, true, "ModeStun");
}
// Pool 3 (the traffic) around the bike: some objects' handles, kinds, progress and speeds drawn.
void DrawPool3(CaseContext& c, uint32_t e) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    const uint32_t base = m.PeekWord(0x800CE500u), stride = m.PeekWord(0x800CE504u);
    const uint32_t hiPtr = m.PeekWord(0x800CE50Cu);
    if (!aip::InRam(base) || !aip::InRam(hiPtr) || stride == 0u || stride > 4096u) return;
    const int32_t n = GetS32(m, hiPtr);
    for (int32_t i = 0; i <= n && i < 32; ++i) {
        const uint32_t o = base + stride * U(i);
        if (!Chance(r, 2)) continue;
        WriteU16Bench(m, o + 0xAC, static_cast<uint16_t>(Chance(r, 5) ? 0u : 0x60u | U(i)));
        m.PokeWord(o + 180, Chance(r, 4) ? 1u + r.U32() % 3u : 0u);
        PutS32(m, o + 324, GetS32(m, e + 324) + (Chance(r, 3) ? r.S32() >> 6 : static_cast<int32_t>(r.U32() % 0x800000u) - 0x400000));
        PutS32(m, o + 480, Chance(r, 2) ? static_cast<int32_t>(r.U32() % 260u) : copr::Speed(r));
    }
    if (Chance(r, 8)) PutS32(m, hiPtr, Chance(r, 2) ? -1 : static_cast<int32_t>(r.U32() % 8u));
}
void SetupJailTest(CaseContext& c, Args& a) {
    uint32_t e, t;
    DrawCopPair(c, e, t);
    DrawPool3(c, e);
    a.a[0] = e;
    a.a[1] = c.scratch;
    c.mem->PokeWord(c.scratch, 0xA5A5A5A5u);
}
uint32_t NativeJailTest(NativeEnv& env, const Args& a) {
    rr::sim::GuestRam g = pop::View(env);
    return pop::PopDone(env, g, true, "ModeJailTest", rr::sim::ModeJailTest(g, a.a[0], a.a[1]));
}

// ---------------------------------------------------------------------------- the functions with callees
void SetupJailRelease(CaseContext& c, Args& a) {
    uint32_t e, t;
    DrawCopPair(c, e, t);
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    if (Chance(r, 6)) m.PokeWord(e + 372, 0);
    a.a[0] = e;
}
uint32_t NativeJailRelease(NativeEnv& env, const Args& a) {
    rr::sim::GuestRam g = pop::View(env);
    Seams s(env);
    const bool ok = rr::sim::ModeJailRelease(g, a.a[0], env.sp, s);
    return pop::PopDone(env, g, ok, "ModeJailRelease");
}
void SetupArrest(CaseContext& c, Args& a) {
    uint32_t e, t;
    DrawCopPair(c, e, t);
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    // the gates open three times in four (an idle arrest word, a racing suspect, a seated cop), so the
    // body is reached; each gate is still drawn shut a quarter of the time
    if (!Chance(r, 4)) m.PokeWord(0x8005AD48, 0);
    if (!Chance(r, 4) && aip::InRam(Rd(m, t))) m.PokeWord(Rd(m, t) + 40, 0);
    if (!Chance(r, 4) && aip::InRam(Rider(m, e))) m.PokeWord(Rider(m, e) + 604, r.U32() % 2u);
    if (!Chance(r, 4) && aip::InRam(Rd(m, e))) Put8(m, Rd(m, e) + 1, (m.PeekByte(Rd(m, e) + 1) & 0xF0u) | 2u);
    if (!Chance(r, 4) && m.PeekWord(m.PeekWord(kGameStatePtr) + 0x30) == 0u) m.PokeWord(m.PeekWord(kGameStatePtr) + 0x30, 1);
    a.a[0] = e;
    a.a[1] = t;
    a.a[2] = Chance(r, 4) ? r.U32() % 16u : 9u;
}
uint32_t NativeArrest(NativeEnv& env, const Args& a) {
    rr::sim::GuestRam g = pop::View(env);
    Seams s(env);
    const bool ok = rr::sim::ModeArrest(g, a.a[0], a.a[1], a.a[2], env.sp, s);
    return pop::PopDone(env, g, ok, "ModeArrest");
}
void SetupFsm(CaseContext& c, Args& a) {
    uint32_t e, t;
    DrawCopPair(c, e, t);
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    for (uint32_t i = 0; i < kBikes; ++i) {   // the field's mount states (the "down" count)
        if (!Chance(r, 4)) continue;
        const uint32_t R = Rider(m, Bk(m, i));
        if (aip::InRam(R)) m.PokeWord(R + 604, r.U32() % 5u);
    }
    if (Chance(r, 6)) m.PokeWord(0x8005B1F8, r.U32() % 24u);
    a.a[0] = e;
    static const int32_t kDt[] = {1094, 218 * 5, 218 * 10, 0x28000};
    a.a[1] = U(Chance(r, 4) ? static_cast<int32_t>(r.U32() % 0x30000u) : kDt[r.U32() % 4u]);
}
uint32_t NativeFsm(NativeEnv& env, const Args& a) {
    rr::sim::GuestRam g = pop::View(env);
    Seams s(env);
    uint32_t v0 = 0;
    const bool ok = rr::sim::ModeArrestFsm(g, a.a[0], static_cast<int32_t>(a.a[1]), env.sp, s, v0);
    return pop::PopDone(env, g, ok, "ModeArrestFsm", v0);
}
void SetupMilestone(CaseContext& c, Args& a) {
    uint32_t e, t;
    DrawCopPair(c, e, t);
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    const uint32_t p1 = m.PeekWord(0x8005B38C);
    if (aip::InRam(p1)) {
        if (e != p1) DrawModeBike(c, p1, aip::Get16(m, t + 0xAC), false);
        // NAMED BOUND (header): phase 2 needs a partner at +0x358
        m.PokeWord(p1 + 856, Bk(m, 1u + r.U32() % (kBikes - 1u)));
    }
    for (uint32_t i = 0; i < kBikes; ++i) {   // the field's classes (the escape column, the silencing)
        if (!Chance(r, 3)) continue;
        const uint32_t rd = Rd(m, Bk(m, i));
        if (aip::InRam(rd)) Put8(m, rd + 1, (m.PeekByte(rd + 1) & 0xF0u) | (r.U32() % 3u));
    }
    Put8(m, 0x8005ADF0, r.U32());
    Put8(m, 0x8005ADF1, r.U32());
    a.a[0] = 0;
}
uint32_t NativeMilestone(NativeEnv& env, const Args&) {
    rr::sim::GuestRam g = pop::View(env);
    Seams s(env);
    uint32_t v0 = 0;
    const bool ok = rr::sim::ModeMilestoneAdvance(g, env.sp, s, v0);
    return pop::PopDone(env, g, ok, "ModeMilestoneAdvance", v0);
}
void SetupMilestoneFirst(CaseContext& c, Args& a) {
    uint32_t e, t;
    DrawCopPair(c, e, t);
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    const uint32_t p1 = m.PeekWord(0x8005B38C);
    if (aip::InRam(p1)) {
        if (e != p1) DrawModeBike(c, p1, aip::Get16(m, t + 0xAC), false);
        PutS32(m, p1 + 364, Chance(r, 2) ? 2 : -2);
        const int32_t lim = (static_cast<int32_t>(aip::Get16(m, 0x8005317Au)) - 25) * 65536;
        PutS32(m, p1 + 368, Chance(r, 2) ? lim + static_cast<int32_t>(r.U32() % 0x20000u) - 0x10000 : r.S32());
    }
    a.a[0] = 0;
}
uint32_t NativeMilestoneFirst(NativeEnv& env, const Args&) {
    rr::sim::GuestRam g = pop::View(env);
    const bool ok = rr::sim::ModeMilestoneFirst(g, env.sp);
    return pop::PopDone(env, g, ok, "ModeMilestoneFirst");
}

} // namespace moder

void AddModeHudRows(std::vector<Row>& rows);
void AddModeHudArrestRow(std::vector<Row>& rows);
void AddModeHudMessageRow(std::vector<Row>& rows);

void AddModeRows(std::vector<Row>& rows) {
    using namespace moder;
    rows.push_back(ModeRow("mode_stun", "void Stun(Bike *t)  [RASHCDG 0x80097308]", rr::sim::kModeStunFn, SetupStun,
                           NativeStun, false, false));
    rows.push_back(ModeRow("mode_jail_test", "u32 JailTest(Bike *e, u32 *out)  [RASHCDG 0x8009DA4C]",
                           rr::sim::kModeJailTestFn, SetupJailTest, NativeJailTest, true, false));
    rows.push_back(ModeRow("mode_jail_release", "void JailRelease(Bike *e)  [RASHCDG 0x800A0708]",
                           rr::sim::kModeJailReleaseFn, SetupJailRelease, NativeJailRelease, false, true));
    rows.push_back(ModeRow("mode_arrest", "void Arrest(Bike *cop, Bike *t, s32 how)  [RASHCDG 0x80096F30]",
                           rr::sim::kModeArrestFn, SetupArrest, NativeArrest, false, true));
    rows.push_back(ModeRow("mode_arrest_fsm", "s32 ArrestFsm(Bike *e, s32 dt)  [RASHCDG 0x80096818]",
                           rr::sim::kModeArrestFsmFn, SetupFsm, NativeFsm, true, true));
    rows.push_back(ModeRow("mode_milestone", "s32 MilestoneAdvance(void)  [RASHCDG 0x800C8D4C]",
                           rr::sim::kModeMilestoneFn, SetupMilestone, NativeMilestone, true, true));
    rows.push_back(ModeRow("mode_milestone_first", "void MilestoneFirst(void)  [RASHCDG 0x800C92F8]",
                           rr::sim::kModeMilestoneFirstFn, SetupMilestoneFirst, NativeMilestoneFirst, false, false));
    AddModeHudRows(rows); // the modes' HUD elements (below)
    AddModeHudArrestRow(rows);
    AddModeHudMessageRow(rows);
}

// ---------------------------------------------------------------------------- the modes' HUD elements
// Rows over rows_hud.inc's machinery (hudrows::PlantHud: a captured HUD state or a random one; Run, HudRow,
// ElementArgs). Drawn on top, per case: the game-state byte (racing or not), the rider's frozen bit,
// the finish word, the race clock against the limit 0x8005ACC8 and the mission base 0x8005ACD0 (either
// side of 0 and of 31 s left), the state's last drawn clock +0x44, the dash's hold +0x8C and flash phase
// +0x50, the mask, the split index 0x8005ACD4 0..3, the split times 0x800D9C40, the split distances
// 0x800D5F60 against the bike's progress, the race id (56..64 and the others), the designated suspect.
namespace modehud {
using pop::Chance;
using pop::Put8;
void DrawClock(CaseContext& c) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    const uint32_t gs = m.PeekWord(kGameStatePtr);
    Put8(m, gs, Chance(r, 5) ? r.U32() % 7u : 1u);
    const uint32_t bike = m.PeekWord(rr::sim::kHudPlayerBikes);
    const uint32_t rd = aip::InRam(bike) ? m.PeekWord(bike + 1084) : 0u;
    if (aip::InRam(rd)) {
        Put8(m, rd, Chance(r, 4) ? (m.PeekByte(rd) | 0x40u) : (m.PeekByte(rd) & ~0x40u));
        m.PokeWord(rd + 40, Chance(r, 5) ? 1u + r.U32() % 1000u : 0u);
    }
    const int32_t clock = static_cast<int32_t>(r.U32() % 200000u);
    PutS32(m, gs + 16, clock);
    const int32_t base = Chance(r, 2) ? 0 : static_cast<int32_t>(r.U32() % 20000u);
    PutS32(m, 0x8005ACD0, base);
    static const int32_t kLeft[] = {0, 31 * 300, 30 * 300, 5 * 300, -300, 100000};
    PutS32(m, 0x8005ACC8, clock - base + kLeft[r.U32() % 6u] + static_cast<int32_t>(r.U32() % 7u) - 3);
    if (Chance(r, 6)) PutS32(m, 0x8005ACC8, r.S32() >> (r.U32() % 16u));
    PutS32(m, rr::sim::kHudState88 + 68, Chance(r, 3) ? r.S32() : 0);
    const uint32_t d = rr::sim::kHudDash224;
    PutS32(m, d + 140, Chance(r, 4) ? 1 + static_cast<int32_t>(r.U32() % 3u) : (Chance(r, 5) ? -1 : 0));
    m.PokeWord(d + 80, Chance(r, 2) ? 0u : 1u);
    m.PokeWord(d + 176, Chance(r, 2) ? 0u : 1u);
    m.PokeWord(d + 180, Chance(r, 2) ? 0u : r.U32() % 100000u);
}
void DrawSplits(CaseContext& c) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    const uint32_t gs = m.PeekWord(kGameStatePtr);
    PutS32(m, 0x8005ACD4, Chance(r, 8) ? static_cast<int32_t>(r.U32() % 8u) - 2 : static_cast<int32_t>(r.U32() % 4u));
    for (uint32_t k = 0; k < 3; ++k) {
        m.PokeWord(0x800D9C40 + 4 * k, Chance(r, 2) ? 0u : r.U32() % 100000u);
        PutS32(m, 0x800D5F60 + 4 * k, static_cast<int32_t>(r.U32() % 4000u));
    }
    const uint32_t bike = m.PeekWord(rr::sim::kHudPlayerBikes);
    if (aip::InRam(bike)) {
        const int32_t at = GetS32(m, 0x800D5F60 + 4 * (m.PeekWord(0x8005ACD4) & 3u));
        PutS32(m, bike + 324, Chance(r, 2) ? (at << 12) + static_cast<int32_t>(r.U32() % 0x4000u) - 0x2000
                                           : static_cast<int32_t>(r.U32() % 0x1000000u));
    }
    // NAMED BOUND: the race id stays 56..64 (the nine Time Trial races the record table 0x80053A88 holds;
    // the only races the Time Trial HUD runs on) - another reads 176-byte rows outside the table
    PutS32(m, gs + 64, 56 + static_cast<int32_t>(r.U32() % 9u));
}
} // namespace modehud

void AddModeHudRows(std::vector<Row>& rows) {
    using namespace hudrows;
    rows.push_back(HudRow(
        "mode_hud_clock_digits", "u32 ClockDigits(s32 t, Item *it, Art *a)  [SLUS 0x80013B90]", 0x80013B90u,
        [](CaseContext& c, Args& a) {
            PlantHud(c);
            Rng& r = *c.rng;
            static const int32_t kT[] = {0, 12, 0x176F0000, 0x176F0001, 60 * 256, 600 * 256 - 1, 9 * 256 + 250};
            a.a[0] = static_cast<uint32_t>(pop::Chance(r, 2) ? kT[r.U32() % 7u] : static_cast<int32_t>(r.U32() % 0x2000000u));
            if (pop::Chance(r, 8)) a.a[0] = r.U32();
            a.a[1] = c.mem->PeekWord(rr::sim::kHudItemsPtr) + (pop::Chance(r, 2) ? 1908u : 2412u);
            a.a[2] = pop::Chance(r, 4) ? 0x800D45D0u + 16u * (r.U32() % 40u) : 0x800D4620u;
        },
        [](NativeEnv& env, const Args& a) {
            return Run(env, [&](rr::sim::GuestRam& g, rr::sim::HudCallees&) {
                return rr::sim::HudClockDigits(g, static_cast<int32_t>(a.a[0]), a.a[1], a.a[2]);
            });
        },
        true));
    rows.push_back(HudRow(
        "mode_hud_race_clock",
        "void RaceClock(u32 *ot, Item *it, State *s, Dash *d, Bike *b, s32 down, u32 mask)  [RASHCDG 0x80063530]",
        0x80063530u,
        [](CaseContext& c, Args& a) {
            PlantHud(c);
            modehud::DrawClock(c);
            Rng& r = *c.rng;
            ElementArgs(c, a);
            a.a[3] = rr::sim::kHudDash224;
            a.a4 = c.mem->PeekWord(rr::sim::kHudPlayerBikes);
            a.more[0] = pop::Chance(r, 2) ? 1u : 0u;
            a.more[1] = pop::Chance(r, 4) ? 1u + r.U32() % 3u : 0u;
        },
        [](NativeEnv& env, const Args& a) {
            return Run(env, [&](rr::sim::GuestRam& g, rr::sim::HudCallees&) {
                rr::sim::HudRaceClock(g, a.a[0], a.a[1], a.a[2], a.a[3], a.a4, a.more[0], a.more[1]);
                return 0u;
            });
        }));
    rows.push_back(HudRow(
        "mode_hud_splits", "void Splits(u32 *ot, Item *it, Dash *d, Bike *b, u32 mask)  [RASHCDG 0x800636F0]",
        0x800636F0u,
        [](CaseContext& c, Args& a) {
            PlantHud(c);
            modehud::DrawClock(c);
            modehud::DrawSplits(c);
            Rng& r = *c.rng;
            a.a[0] = c.mem->PeekWord(rr::sim::kHudOt);
            a.a[1] = c.mem->PeekWord(rr::sim::kHudItemsPtr);
            a.a[2] = rr::sim::kHudDash224;
            a.a[3] = c.mem->PeekWord(rr::sim::kHudPlayerBikes);
            a.a4 = pop::Chance(r, 4) ? 1u + r.U32() % 3u : 0u;
        },
        [](NativeEnv& env, const Args& a) {
            return Run(env, [&](rr::sim::GuestRam& g, rr::sim::HudCallees&) {
                rr::sim::HudSplits(g, a.a[0], a.a[1], a.a[2], a.a[3], a.a4);
                return 0u;
            });
        }));
    rows.push_back(HudRow(
        "mode_hud_cop_clock", "void CopClock(u32 *ot, Item *it, Dash *d, State *s, Bike *b)  [RASHCDG 0x80062C40]",
        0x80062C40u,
        [](CaseContext& c, Args& a) {
            PlantHud(c);
            modehud::DrawClock(c);
            a.a[0] = c.mem->PeekWord(rr::sim::kHudOt);
            a.a[1] = c.mem->PeekWord(rr::sim::kHudItemsPtr);
            a.a[2] = rr::sim::kHudDash224;
            a.a[3] = rr::sim::kHudState88;
            a.a4 = c.mem->PeekWord(rr::sim::kHudPlayerBikes);
        },
        [](NativeEnv& env, const Args& a) {
            return Run(env, [&](rr::sim::GuestRam& g, rr::sim::HudCallees&) {
                rr::sim::HudCopClock(g, a.a[0], a.a[1], a.a[2], a.a[3], a.a4);
                return 0u;
            });
        }));
    rows.push_back(Bios(HudRow(
        "mode_hud_suspect", "void SuspectName(u32 *ot, Item *it, s32 p)  [RASHCDG 0x8005FAC4]", 0x8005FAC4u,
        [](CaseContext& c, Args& a) {
            PlantHud(c);
            Rng& r = *c.rng;
            Memory& m = *c.mem;
            // the designated suspect: a pool-0 slot 0..17 (the commit's table gives 1..10)
            pop::Put8(m, m.PeekWord(kGameStatePtr) + 6, r.U32() % 18u);
            a.a[0] = m.PeekWord(rr::sim::kHudOt);
            a.a[1] = m.PeekWord(rr::sim::kHudItemsPtr) + 3384u - 36u * (r.U32() % 3u);
            a.a[2] = 0;
        },
        [](NativeEnv& env, const Args& a) {
            return Run(env, [&](rr::sim::GuestRam& g, rr::sim::HudCallees& k) {
                if (!rr::sim::HudSuspectName(g, k, env.sp, a.a[0], a.a[1], static_cast<int32_t>(a.a[2])) &&
                    env.failure.empty())
                    env.failure = "SuspectName refused";
                return 0u;
            });
        },
        false, Seams(0, 0, 32), 0)));
}

void AddModeHudArrestRow(std::vector<Row>& rows) {
    using namespace hudrows;
    rows.push_back(HudRow(
        "mode_hud_arrests", "void Arrests(u32 *ot, Item *it, s32 p, u32 mask)  [RASHCDG 0x80062610]", 0x80062610u,
        [](CaseContext& c, Args& a) {
            PlantHud(c);
            Rng& r = *c.rng;
            Memory& m = *c.mem;
            const uint32_t gs = m.PeekWord(kGameStatePtr);
            // the quota gs+0x07 (the commit's per-venue byte, 4..6) and the arrests left 0x8005AD44
            const uint32_t quota = pop::Chance(r, 6) ? r.U32() % 16u : 3u + r.U32() % 5u;
            pop::Put8(m, gs + 7, quota);
            PutS32(m, 0x8005AD44, static_cast<int32_t>(r.U32() % (quota + 2u)) - (pop::Chance(r, 6) ? 1 : 0));
            pop::Put8(m, gs + 4, pop::Chance(r, 3) ? (m.PeekByte(gs + 4) | 0x10u) : (m.PeekByte(gs + 4) & ~0x10u));
            PutS32(m, 0x8005AD14, pop::Chance(r, 3) ? 2 : -1);
            a.a[0] = m.PeekWord(rr::sim::kHudOt);
            a.a[1] = m.PeekWord(rr::sim::kHudItemsPtr);
            a.a[2] = 0;
            a.a[3] = pop::Chance(r, 4) ? 1u + r.U32() % 3u : 0u;
        },
        [](NativeEnv& env, const Args& a) {
            return Run(env, [&](rr::sim::GuestRam& g, rr::sim::HudCallees& k) {
                if (!rr::sim::HudArrests(g, k, env.sp, a.a[0], a.a[1], static_cast<int32_t>(a.a[2]), a.a[3]) &&
                    env.failure.empty())
                    env.failure = "Arrests refused";
                return 0u;
            });
        },
        false, Seams(0, 0, 88), 0));
}

void AddModeHudMessageRow(std::vector<Row>& rows) {
    using namespace hudrows;
    rows.push_back(Bios(HudRow(
        "mode_hud_arrest_msg", "void ArrestMessage(Item *it, Bike *b, Dash *d, s32 p)  [RASHCDG 0x80062F34]", 0x80062F34u,
        [](CaseContext& c, Args& a) {
            PlantHud(c);
            Rng& r = *c.rng;
            Memory& m = *c.mem;
            const uint32_t gs = m.PeekWord(kGameStatePtr);
            const uint32_t bike = m.PeekWord(rr::sim::kHudPlayerBikes);
            const uint32_t rd = aip::InRam(bike) ? m.PeekWord(bike + 1084) : 0u;
            if (aip::InRam(rd)) {
                pop::Put8(m, rd + 1, (m.PeekByte(rd + 1) & 0xF0u) | (pop::Chance(r, 5) ? r.U32() % 3u : 2u));
                static const uint8_t kPlaces[] = {252, 248, 253, 254, 3, 1};
                pop::Put8(m, rd + 39, kPlaces[r.U32() % 6u]);
            }
            pop::Put8(m, gs + 4, pop::Chance(r, 3) ? 33u : 1u);
            static const uint32_t kWord[] = {0, 1, 2, 4, 8, 16, 0x1E, 6};
            m.PokeWord(0x8005AD48, kWord[r.U32() % 8u]);
            const uint32_t d = rr::sim::kHudDash224;
            m.PokeWord(d + 112, pop::Chance(r, 4) ? 0u : 1u);
            a.a[0] = m.PeekWord(rr::sim::kHudItemsPtr);
            a.a[1] = bike;
            a.a[2] = d;
            a.a[3] = 0;
        },
        [](NativeEnv& env, const Args& a) {
            return Run(env, [&](rr::sim::GuestRam& g, rr::sim::HudCallees& k) {
                if (!rr::sim::HudArrestMessage(g, k, env.sp, a.a[0], a.a[1], a.a[2], static_cast<int32_t>(a.a[3])) &&
                    env.failure.empty())
                    env.failure = "ArrestMessage refused";
                return 0u;
            });
        },
        false, Seams(0, 0, 72 + 32), 0)));
}
