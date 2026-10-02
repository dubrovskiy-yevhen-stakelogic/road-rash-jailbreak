// Bench rows for the police chase and the arrest. Included by verify_physics.cpp
// inside its anonymous namespace AFTER rows_population.inc and rows_ai_plan.inc, whose helpers (pop::
// PopSeams, pop::View, pop::PopDone, pop::DrawPlayers, aip::BikeAt ...) these rows reuse.
//
// The port is src\game\sim\cops.{h,cpp}. One row per function:
//   cop_dist 0x8009E444, cop_target_ok 0x8009DB58, cop_stop_test 0x8009DBA0, cop_ahead 0x8009E3F0,
//   cop_wanted_bits 0x8009FC80, cop_pick_target 0x8009E178, cop_in_range 0x800BC120 (leaves, no seam);
//   cop_arrest 0x8009E528 (seams: the busted music SLUS 0x8001B3C8, EndRace 0x80092C7C),
//   cop_tail 0x8009DC90 (the same, AiPushCommand / AiClearCommands native; AiBackOffOk, AiStrikeReach,
//   AiTaunt, FixMul, FixDiv native), cop_intercept 0x800BBEBC (seam: AiPopCommand 0x800BC8DC; MulAdd,
//   Length3 native), cop_idle 0x800BAA2C (seams: ComputePlace SLUS 0x800138E8, 0x8009DA4C, 0x800A0708),
//   cop_arrest_test 0x80097470, cop_arrest_scan 0x80097388 (seam: 0x80096F30).
//
// INPUT FAMILIES start from THIS machine (rr-race: the player at slot 0, bike 6 live, the two dormant
// cops at slots 16 / 17 holding op 1). Drawn over it, per case: the player count (0..2), the race type
// (0, cop mode bit 0, bit 4, 44, 36, anything), the race's target, the phase gs[+0x39], the bank 0..3,
// the clock, the race flags 0x8005AD48, the wanted mask, the LCG seed; for the cop and its target (a
// player, another bike or the cop itself): the live word, the rider's mount state 0..4, its flag 0x40 and
// stance (72 / 73), both speeds either side of 8.94 (0x8F0D8) or anything, the brake +0x258, the finish
// word, the place byte (246..255 and below), the class nibble, the road key (the same or not), the
// direction, the along and the lateral, the positions at a drawn octagonal distance (inside the arrest
// reach 5.0 / 8.0, around it, around 120.0, anything), the off-bike rider's own coordinate, the command
// stack (depth 0..3; ops 1, 2, 4, 6, 16, 17, 18 and anything; targets 224, the other, anything; stamps),
// the hold word +0x238, the aim-latch bit +0x234 0x80000, the hit list riderDef[+0x40].
//
// NAMED BOUND (cop_tail): race type bit 4 with fewer than two players is drawn without bit 4. There the
// target picker 0x8009E178 reads its frame word sp+12, which no player wrote; inside the tail that word
// is whatever the tail's earlier callee left on the stack (AiPushCommand's saved registers when the stack
// was empty), and the port's native push leaves no such word. The leaf row cop_pick_target draws that
// case (the word is the same stale memory on both sides there).

namespace copr {

using pop::Chance;
using pop::Put8;
using pop::U;

constexpr uint32_t kCopA = 16, kCopB = 17, kBikes = 18;
constexpr uint32_t kPopFn = 0x800BC8DC, kPlaceFn = 0x800138E8;

uint32_t Bk(const Memory& m, uint32_t i) { return aip::BikeAt(m, i); }
uint32_t Rider(const Memory& m, uint32_t e) { return m.PeekWord(e + 852); }
uint32_t Rd(const Memory& m, uint32_t e) { return m.PeekWord(e + 1084); }

// A 16.16 value around `v`: one either side, a unit either side, anything small, anything.
int32_t Around(Rng& r, int32_t v) {
    switch (r.U32() % 5u) {
    case 0: return v - 1 + static_cast<int32_t>(r.U32() % 3u);
    case 1: return v + static_cast<int32_t>(r.U32() % 0x20000u) - 0x10000;
    case 2: return static_cast<int32_t>(r.U32() % 0x1000000u);
    case 3: return -v - 1 + static_cast<int32_t>(r.U32() % 3u);
    default: return r.S32();
    }
}
int32_t Speed(Rng& r) {
    switch (r.U32() % 4u) {
    case 0: return Around(r, 0x8F0D8);
    case 1: return static_cast<int32_t>(r.U32() % 0x100000u);
    case 2: return static_cast<int32_t>(r.U32() % 0x400000u);
    default: return Chance(r, 3) ? 0 : r.S32() >> (r.U32() % 12u);
    }
}

// The globals of the header's list; returns the player count.
uint32_t DrawGlobals(CaseContext& c, bool bit4Needs2) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    const uint32_t np = pop::DrawPlayers(c);
    const uint32_t gs = m.PeekWord(kGameStatePtr);
    static const uint8_t kTypes[] = {0, 0, 0, 0x22, 0x21, 0x10, 0x11, 44, 36, 0x23, 0x30};
    uint32_t type = Chance(r, 12) ? r.U32() & 0xFFu : kTypes[r.U32() % 11u];
    if (bit4Needs2 && np < 2u) type &= ~0x10u;
    Put8(m, gs + 4, type);
    Put8(m, gs + 6, Chance(r, 4) ? 224u : r.U32() % kBikes);
    Put8(m, gs + 57, Chance(r, 3) ? r.U32() % 6u : 0u);
    PutS32(m, gs + 60, Chance(r, 8) ? 3 : static_cast<int32_t>(r.U32() % 3u));
    PutS32(m, gs + 16, static_cast<int32_t>(r.U32() % 0x8000000u));
    m.PokeWord(0x8005AD48, Chance(r, 2) ? 0u : r.U32() & 0x3Fu);
    m.PokeWord(rr::sim::kCopWantedMask, Chance(r, 2) ? 0u : r.U32());
    if (Chance(r, 6)) m.PokeWord(0x8005B1F8, r.U32() % 24u);
    m.PokeWord(rr::sim::kCopJailAcc, Chance(r, 2) ? 0u : r.U32() % 0x800000u);
    m.PokeWord(c.gp + kRandSeedGpOffset, r.U32());
    return np;
}

// A command stack of depth 0..3 (now and then deeper), ops from the police set.
void DrawStack(CaseContext& c, uint32_t e, uint32_t other) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    static const uint16_t kOps[] = {1, 2, 2, 4, 4, 4, 17, 16, 6, 3, 18, 0};
    uint32_t depth = r.U32() % 4u;
    if (Chance(r, 20)) depth = 4u + r.U32() % 8u;
    Put8(m, e + 0x3B2, depth);
    for (uint32_t k = 1; k <= depth; ++k) {
        const uint32_t s = e + 0x3B4u + 8u * k;
        WriteU16Bench(m, s, Chance(r, 12) ? static_cast<uint16_t>(r.U32() % 19u) : kOps[r.U32() % 12u]);
        // a target handle names a pool-0 bike (0..17), none (224) or is out of the pickers' range (>= 32);
        // 18..31 would name pool-1 records as bikes, which no command of the game holds (NAMED BOUND)
        const uint32_t t = Chance(r, 3) ? 224u : (Chance(r, 2) ? other : (Chance(r, 8) ? 32u + r.U32() % 224u : r.U32() % kBikes));
        WriteU16Bench(m, s + 2, static_cast<uint16_t>(t));
        WriteU16Bench(m, s + 4, static_cast<uint16_t>(Chance(r, 2) ? r.U32() % 64u : r.U32()));
        WriteU16Bench(m, s + 6, static_cast<uint16_t>(r.U32()));
    }
}

// One bike's police fields (the header's list).
void DrawBike(CaseContext& c, uint32_t e, uint32_t other, bool cop) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    const uint32_t rd = Rd(m, e), R = Rider(m, e);
    if (!aip::InRam(rd) || !aip::InRam(R)) return;
    static const uint16_t kLive[] = {1, 1, 0x13, 0x10, 0, 9};
    WriteU16Bench(m, e + 320, kLive[r.U32() % 6u]);
    m.PokeWord(R + 604, Chance(r, 2) ? 1u : r.U32() % 5u);
    if (Chance(r, 5)) m.PokeWord(R + 552, m.PeekWord(R + 552) ^ 0x40u);
    // the stance +0x220: the captured one, or 72 / 73 (the stop test's off-bike arm). NAMED BOUND: any
    // other drawn stance may not be resident, and EndRace's stance event then traps in the ORIGINAL's
    // HardStart 0x8005BF8C (the population rows name the same bound)
    if (Chance(r, 3)) WriteU16Bench(m, R + 544, static_cast<uint16_t>(72u + r.U32() % 2u));
    PutS32(m, e + 480, Speed(r));
    PutS32(m, R + 480, Speed(r));
    if (Chance(r, 2)) PutS32(m, e + 600, Chance(r, 6) ? 0 : static_cast<int32_t>(r.U32() % 0x100000u) - (Chance(r, 5) ? 0x80000 : 0));
    m.PokeWord(rd + 40, Chance(r, 6) ? 1u + r.U32() % 1000u : 0u);
    Put8(m, rd + 39, Chance(r, 3) ? 244u + r.U32() % 12u : r.U32() % 20u);
    if (cop) Put8(m, rd + 1, (m.PeekByte(rd + 1) & 0xF0u) | (Chance(r, 6) ? r.U32() % 16u : 2u));
    WriteU16Bench(m, rd + 64, static_cast<uint16_t>(Chance(r, 2) ? 0u : aip::DrawList(r, other, 15)));
    m.PokeWord(e + 568, Chance(r, 3) ? (r.U32() & 0x7FFu) : 0u);
    if (Chance(r, 3)) m.PokeWord(e + 564, m.PeekWord(e + 564) ^ 0x80000u);
    PutS32(m, e + 364, Chance(r, 2) ? 1 : -1);
    PutS32(m, e + 344, static_cast<int32_t>(r.U32() % 0x800000u) - 0x400000);
    DrawStack(c, e, other);
}

// `t` put at a drawn octagonal distance from `e`, on e's road or not, its along near e's, and the rider's
// own coordinate near it (for an off-bike target).
void Place(CaseContext& c, uint32_t e, uint32_t t) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    int32_t d;
    switch (r.U32() % 5u) {
    case 0: d = static_cast<int32_t>(r.U32() % 6u); break;
    case 1: d = 4 + static_cast<int32_t>(r.U32() % 6u); break;
    case 2: d = 115 + static_cast<int32_t>(r.U32() % 12u); break;
    case 3: d = static_cast<int32_t>(r.U32() % 40u); break;
    default: d = static_cast<int32_t>(r.U32() % 600u); break;
    }
    int32_t dx, dz;
    pop::OffsetsAt(r, d, dx, dz);
    for (uint32_t who : {t, Rider(m, t)}) {
        if (!aip::InRam(who)) continue;
        PutS32(m, who + 184, static_cast<int32_t>((U(GetS16(m, e + 186) + dx) << 16) | (r.U32() & 0xFFFFu)));
        PutS32(m, who + 188, GetS32(m, e + 188) + (r.S32() >> 14));
        PutS32(m, who + 192, static_cast<int32_t>((U(GetS16(m, e + 194) + dz) << 16) | (r.U32() & 0xFFFFu)));
        if (Chance(r, 2)) m.PokeWord(who + 360, m.PeekWord(e + 360));
        else if (Chance(r, 2)) m.PokeWord(who + 360, r.U32() % 40u | (Chance(r, 3) ? 0x10000u : 0u));
        PutS32(m, who + 368, GetS32(m, e + 368) + static_cast<int32_t>(r.U32() % 0x2000000u) - 0x1000000);
        PutS32(m, who + 364, Chance(r, 2) ? 1 : -1);
        dx += static_cast<int32_t>(r.U32() % 5u) - 2;
    }
    for (uint32_t k = 0; k < 3u; ++k)                                // the contact point near the box centre
        if (Chance(r, 2)) PutS32(m, e + 504 + 4u * k, GetS32(m, e + 184 + 4u * k) + (r.S32() >> 12));
}

// A cop (mostly slot 16 / 17) and its target (mostly the player), both drawn; returns {cop, target}.
void DrawPair(CaseContext& c, uint32_t& e, uint32_t& t, bool bit4Needs2 = false) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    DrawGlobals(c, bit4Needs2);
    e = Bk(m, Chance(r, 5) ? r.U32() % kBikes : (Chance(r, 2) ? kCopA : kCopB));
    t = Bk(m, Chance(r, 3) ? r.U32() % kBikes : 0u);
    if (Chance(r, 3)) {   // player 2 as the target
        const uint32_t p2 = m.PeekWord(kPlayerArr + 4);
        if (aip::InRam(p2) && m.PeekWord(m.PeekWord(kGameStatePtr) + 0x30) == 2u) t = p2;
    }
    const uint32_t eh = aip::Get16(m, e + 0xAC), th = aip::Get16(m, t + 0xAC);
    DrawBike(c, e, th, true);
    if (t != e) DrawBike(c, t, eh, false);
    if (t != e) Place(c, e, t);
    for (uint32_t p = 0; p < 2u; ++p) {   // the players' own place / finish (the tail's busted loop, the picker)
        const uint32_t pb = m.PeekWord(kPlayerArr + 4u * p);
        if (!aip::InRam(pb) || pb == e || pb == t) continue;
        const uint32_t rd = Rd(m, pb);
        if (Chance(r, 3)) Put8(m, rd + 39, Chance(r, 2) ? 254u : r.U32() % 20u);
        if (Chance(r, 4)) m.PokeWord(rd + 40, Chance(r, 2) ? 0u : 1u + r.U32() % 100u);
    }
}

// ---------------------------------------------------------------------------- the seams
std::vector<OracleCallee> CopSeamCallees() {
    std::vector<OracleCallee> v = pop::PopSeamCallees();
    for (const OracleCallee& oc :
         {OracleCallee{rr::sim::kCopBustedMusicFn, 2}, OracleCallee{rr::sim::kCopEndRaceFn, 2},
          OracleCallee{rr::sim::kCopJailTestFn, 2}, OracleCallee{rr::sim::kCopJailReleaseFn, 1},
          OracleCallee{rr::sim::kCopKnockArrestFn, 3}, OracleCallee{kPopFn, 1}, OracleCallee{kPlaceFn, 2}})
        v.push_back(oc);
    return v;
}

struct Seams final : rr::sim::CopCallees {
    NativeEnv& env;
    pop::PopSeams ps;
    explicit Seams(NativeEnv& e) : env(e), ps(e) {}
    bool PushCommand(uint32_t cmd, int32_t mode, uint32_t e, uint32_t sp) override {
        return ps.PushCommand(cmd, mode, e, sp);
    }
    bool ClearCommands(uint32_t e, uint32_t sp) override { return ps.ClearCommands(e, sp); }
    bool PopCommand(uint32_t e, uint32_t sp) override { return ps.Seam(kPopFn, {e}, sp); }
    bool EndRace(uint32_t e, int32_t how, uint32_t sp) override { return ps.Seam(rr::sim::kCopEndRaceFn, {e, U(how)}, sp); }
    bool ComputePlace(uint32_t e, int32_t mode, uint32_t sp, int32_t& v0) override {
        uint32_t v = 0;
        const bool ok = ps.Seam(kPlaceFn, {e, U(mode)}, sp, &v);
        v0 = static_cast<int32_t>(v);
        return ok;
    }
    bool BustedMusic(uint32_t h, uint32_t ch, uint32_t sp) override {
        return ps.Seam(rr::sim::kCopBustedMusicFn, {h, ch}, sp);
    }
    bool JailTest(uint32_t e, uint32_t out, uint32_t sp, int32_t& v0) override {
        uint32_t v = 0;
        const bool ok = ps.Seam(rr::sim::kCopJailTestFn, {e, out}, sp, &v);
        v0 = static_cast<int32_t>(v);
        return ok;
    }
    bool JailRelease(uint32_t e, uint32_t sp) override { return ps.Seam(rr::sim::kCopJailReleaseFn, {e}, sp); }
    bool KnockArrest(uint32_t e, uint32_t o, int32_t how, uint32_t sp) override {
        return ps.Seam(rr::sim::kCopKnockArrestFn, {e, o, U(how)}, sp);
    }
};

Row CopRow(const char* name, const char* sig, uint32_t entry, std::function<void(CaseContext&, Args&)> setup,
           std::function<uint32_t(NativeEnv&, const Args&)> native, bool v0, bool seams) {
    Row r{name, sig, entry, std::move(setup), nullptr, v0, std::move(native)};
    r.maxSteps = 40'000'000;
    if (seams) r.oracleCallees = CopSeamCallees();
    return r;
}

// ---------------------------------------------------------------------------- the leaves
void SetupPair(CaseContext& c, Args& a) {
    uint32_t e, t;
    DrawPair(c, e, t);
    a.a[0] = e;
    a.a[1] = t;
}
void SetupDist(CaseContext& c, Args& a) {
    SetupPair(c, a);
    Rng& r = *c.rng;
    if (Chance(r, 16)) a.a[0] = 0;
    if (Chance(r, 16)) a.a[1] = 0;
}
uint32_t NativeDist(NativeEnv& env, const Args& a) {
    rr::sim::GuestRam g = pop::View(env);
    return pop::PopDone(env, g, true, "CopDist", U(rr::sim::CopDist(g, a.a[0], a.a[1], env.sp)));
}
uint32_t NativeTargetOk(NativeEnv& env, const Args& a) {
    rr::sim::GuestRam g = pop::View(env);
    return pop::PopDone(env, g, true, "CopTargetOk", U(rr::sim::CopTargetOk(g, a.a[0], a.a[1])));
}
void SetupStopTest(CaseContext& c, Args& a) {
    SetupPair(c, a);
    Rng& r = *c.rng;
    const int32_t dist = Chance(r, 3) ? static_cast<int32_t>(r.U32() % 0x2000000u) : r.S32() >> (r.U32() % 16u);
    static const int32_t kReach[] = {0x50000, 0x80000, 0x10000};
    const int32_t k = kReach[r.U32() % 3u];
    int32_t stop = dist + (Chance(r, 2) ? 1 : -1) * Around(r, k);
    if (Chance(r, 6)) stop = r.S32();
    a.a[2] = U(dist);
    a.a[3] = U(stop);
}
uint32_t NativeStopTest(NativeEnv& env, const Args& a) {
    rr::sim::GuestRam g = pop::View(env);
    return pop::PopDone(env, g, true, "CopStopTest",
                        U(rr::sim::CopStopTest(g, a.a[0], a.a[1], static_cast<int32_t>(a.a[2]), static_cast<int32_t>(a.a[3]))));
}
uint32_t NativeAhead(NativeEnv& env, const Args& a) {
    rr::sim::GuestRam g = pop::View(env);
    return pop::PopDone(env, g, true, "CopAhead", U(rr::sim::CopAhead(g, a.a[0], a.a[1])));
}
void SetupWanted(CaseContext& c, Args& a) {
    DrawGlobals(c, false);
    Rng& r = *c.rng;
    a.a[0] = Chance(r, 3) ? r.U32() : aip::DrawList(r, r.U32() % kBikes, 32);
}
uint32_t NativeWanted(NativeEnv& env, const Args& a) {
    rr::sim::GuestRam g = pop::View(env);
    rr::sim::CopWantedBits(g, a.a[0]);
    return pop::PopDone(env, g, true, "CopWantedBits");
}
uint32_t NativePick(NativeEnv& env, const Args& a) {
    rr::sim::GuestRam g = pop::View(env);
    return pop::PopDone(env, g, true, "CopPickTarget", rr::sim::CopPickTarget(g, a.a[0], env.sp));
}
void SetupInRange(CaseContext& c, Args& a) {
    SetupPair(c, a);
    Rng& r = *c.rng;
    static const int32_t kAt[] = {0x780000, 0x50000, 0x80000, 0};
    a.a[2] = U(Around(r, kAt[r.U32() % 4u]));
}
uint32_t NativeInRange(NativeEnv& env, const Args& a) {
    rr::sim::GuestRam g = pop::View(env);
    return pop::PopDone(env, g, true, "CopInRange",
                        U(rr::sim::CopInRange(g, a.a[0], a.a[1], static_cast<int32_t>(a.a[2]))));
}

// ---------------------------------------------------------------------------- the functions with callees
// NAMED BOUND (cop_arrest, cop_tail): the busted music SLUS 0x8001B3C8 plays only with one player outside
// cop mode, and there it calls 0x80023148, which enters a critical section with SYSCALL 1 - the bench
// services no kernel call, so the GUEST run would trap. A one-player case that can reach the arrest (the
// arrest row itself; the tail when the cop's top command is 2) is drawn in cop mode (race type bit 0),
// where the music's own gate skips it. The port still makes the call (the seam runs the original).
void MusicBound(CaseContext& c) {
    Memory& m = *c.mem;
    const uint32_t gs = m.PeekWord(kGameStatePtr);
    if (m.PeekWord(gs + 0x30) < 2u) Put8(m, gs + 4, m.PeekByte(gs + 4) | 1u);
}
void SetupArrest(CaseContext& c, Args& a) {
    SetupPair(c, a);
    MusicBound(c);
}
uint32_t NativeArrest(NativeEnv& env, const Args& a) {
    rr::sim::GuestRam g = pop::View(env);
    Seams s(env);
    const bool ok = rr::sim::CopArrest(g, a.a[0], a.a[1], env.sp, s);
    return pop::PopDone(env, g, ok, "CopArrest");
}
void SetupTail(CaseContext& c, Args& a) {
    uint32_t e, t;
    DrawPair(c, e, t, true);
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    // the cop's top command aimed at the target half the time (the picker returns it)
    const int32_t d = static_cast<int8_t>(m.PeekByte(e + 0x3B2));
    if (d > 0 && d <= 16 && Chance(r, 2)) WriteU16Bench(m, e + 0x3B4u + 8u * U(d) + 2u, aip::Get16(m, t + 0xAC));
    if (d > 0 && d <= 16 && aip::Get16(m, e + 0x3B4u + 8u * U(d)) == 2u) MusicBound(c);
    a.a[0] = e;
}
uint32_t NativeTail(NativeEnv& env, const Args& a) {
    rr::sim::GuestRam g = pop::View(env);
    Seams s(env);
    uint32_t v0 = 0;
    const bool ok = rr::sim::CopTail(g, a.a[0], env.sp, s, v0);
    return pop::PopDone(env, g, ok, "CopTail", v0);
}
void SetupIntercept(CaseContext& c, Args& a) {
    uint32_t e, t;
    DrawPair(c, e, t);
    Rng& r = *c.rng;
    a.a[0] = e;
    a.a[1] = Chance(r, 8) ? 224u : (aip::Get16(*c.mem, t + 0xAC) | (Chance(r, 8) ? (r.U32() << 16) : 0u));
}
uint32_t NativeIntercept(NativeEnv& env, const Args& a) {
    rr::sim::GuestRam g = pop::View(env);
    Seams s(env);
    const int16_t* sq = reinterpret_cast<const int16_t*>(pop::PopRaw(env, 0x800560CCu, 2));
    const bool ok = sq != nullptr && rr::sim::CopIntercept(g, a.a[0], a.a[1], env.sp, s, sq);
    return pop::PopDone(env, g, ok, "CopIntercept");
}
void SetupIdle(CaseContext& c, Args& a) {
    uint32_t e, t;
    DrawPair(c, e, t);
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    const int32_t d = static_cast<int8_t>(m.PeekByte(e + 0x3B2));
    uint32_t cmd = e + 0x3B4u + 8u * U(d > 0 && d <= 16 ? d : 1);
    if (Chance(r, 3)) {   // the release time either side of now
        const uint32_t clock = m.PeekWord(m.PeekWord(kGameStatePtr) + 16);
        WriteU16Bench(m, cmd + 4, static_cast<uint16_t>(((clock * 0x6D00u) >> 16) + (r.U32() % 3u) - 1u));
    }
    if (Chance(r, 3)) {   // the Jailbreak race's player-1 along around its gate
        const uint32_t p1 = m.PeekWord(0x8005B38C);
        if (aip::InRam(p1)) PutS32(m, p1 + 368, static_cast<int32_t>(r.U32() % 0x4000000u));
        m.PokeWord(e + 180, r.U32() % 36u);
    }
    a.a[0] = e;
    a.a[1] = cmd;
    a.a[2] = Chance(r, 4) ? r.U32() : r.U32() % 0x4000u;
}
uint32_t NativeIdle(NativeEnv& env, const Args& a) {
    rr::sim::GuestRam g = pop::View(env);
    Seams s(env);
    const bool ok = rr::sim::CopIdle(g, a.a[0], a.a[1], static_cast<int32_t>(a.a[2]), env.sp, s);
    return pop::PopDone(env, g, ok, "CopIdle");
}
uint32_t NativeArrestTest(NativeEnv& env, const Args& a) {
    rr::sim::GuestRam g = pop::View(env);
    Seams s(env);
    uint32_t v0 = 0;
    const bool ok = rr::sim::CopArrestTest(g, a.a[0], a.a[1], env.sp, s, v0);
    return pop::PopDone(env, g, ok, "CopArrestTest", v0);
}
// The scan's family. Its only caller, the planner 0x800B8530, runs it in cop mode (race type bit 0) for a
// PLAYER's bike (handle < gs+0x30), and its only write is Arrest 0x80096F30(e, b, 9), whose own gate wants
// that same player-cop (handle < players, class nibble 2, mount < 2, no +0x228 bit 0x8000, the arrest
// word 0x8005AD48 clear) and an unfinished suspect. So the scanner is player 1 five times in six (else
// any bike, the gates then shut), cop mode with a player five times in six, and each of Arrest's gates is
// opened three times in four; the other bikes are drawn and put near it, half of them inside the arrest
// reach 5.0 / 8.0 (the stop test's `dist`), with the designated suspect gs+0x06 one of them half the time
// and the quota 0x8005AD44 around 0..2 (Arrest's two branches).
// Two arms of Arrest stay unexecuted (--coverage): 0x800971F8..0x80097218 (the cop faster than 8.94) is
// UNREACHABLE from the scan, whose own gate wants e+0x1E0 < 0x8F0D9 and nothing between writes it; and
// 0x8009713C..0x80097160 (the player-cop's stance of kind 3 -> StanceEvent) is a NAMED BOUND: drawing a
// kind-3 stance there makes the ORIGINAL trap in HardStart 0x8005BF8C (this rr-race machine has no cop
// arrest clip resident for the player's rider), copr::DrawBike's stance bound.
void SetupScan(CaseContext& c, Args& a) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    DrawGlobals(c, false);
    const uint32_t gs = m.PeekWord(kGameStatePtr);
    if (!Chance(r, 6)) {
        static const uint8_t kCopTypes[] = {1, 0x21, 0x21, 0x23, 0x11, 0x31};
        uint32_t type = kCopTypes[r.U32() % 6u];
        if (m.PeekWord(gs + 0x30) < 2u) type &= ~0x10u;
        Put8(m, gs + 4, type);
        if (m.PeekWord(gs + 0x30) == 0u) m.PokeWord(gs + 0x30, 1);
    }
    const uint32_t e = Chance(r, 6) ? Bk(m, r.U32() % kBikes) : m.PeekWord(kPlayerArr);
    const uint32_t eh = aip::Get16(m, e + 0xAC);
    DrawBike(c, e, Bk(m, 1u + r.U32() % (kBikes - 1u)), true);
    const uint32_t erd = Rd(m, e), eR = Rider(m, e);
    if (aip::InRam(erd) && aip::InRam(eR)) {
        if (!Chance(r, 4)) m.PokeWord(eR + 604, r.U32() % 2u);
        if (!Chance(r, 4)) m.PokeWord(eR + 552, m.PeekWord(eR + 552) & ~0x8040u);
        if (!Chance(r, 4)) PutS32(m, e + 480, Chance(r, 2) ? Around(r, 0x8F0D8) : static_cast<int32_t>(r.U32() % 0x8F0D9u));
        if (!Chance(r, 4)) Put8(m, erd + 1, (m.PeekByte(erd + 1) & 0xF0u) | 2u);
        if (!Chance(r, 4)) m.PokeWord(erd + 40, 0);
        if (Chance(r, 4)) Put8(m, erd + 39, 255u);
    }
    if (!Chance(r, 4)) m.PokeWord(0x8005AD48, 0);
    PutS32(m, 0x8005AD44, static_cast<int32_t>(r.U32() % 4u) - 1);
    uint32_t suspect = 224;
    for (uint32_t i = 0; i < kBikes; ++i) {   // the other bikes: live or not, stopped near the scanner
        if (Chance(r, 3)) continue;
        const uint32_t b = Bk(m, i);
        if (b == e) continue;
        DrawBike(c, b, eh, false);
        Place(c, e, b);
        if (Chance(r, 2)) {   // inside the arrest reach: the bike and its rider a few units from e
            for (uint32_t who : {b, Rider(m, b)}) {
                if (!aip::InRam(who)) continue;
                const int32_t dx = static_cast<int32_t>(r.U32() % 7u) - 3, dz = static_cast<int32_t>(r.U32() % 7u) - 3;
                PutS32(m, who + 184, static_cast<int32_t>((U(GetS16(m, e + 186) + dx) << 16) | (r.U32() & 0xFFFFu)));
                PutS32(m, who + 192, static_cast<int32_t>((U(GetS16(m, e + 194) + dz) << 16) | (r.U32() & 0xFFFFu)));
            }
            if (!Chance(r, 4) && aip::InRam(Rd(m, b))) m.PokeWord(Rd(m, b) + 40, 0);
        }
        if (suspect == 224u || Chance(r, 3)) suspect = aip::Get16(m, b + 0xAC);
    }
    if (suspect != 224u && Chance(r, 2)) Put8(m, gs + 6, suspect);
    if (Chance(r, 8)) {   // a shorter pool
        const uint32_t hiPtr = m.PeekWord(0x800CE4D0 + 12);
        m.PokeWord(hiPtr, Chance(r, 2) ? 0xFFFFFFFFu : r.U32() % 18u);
    }
    a.a[0] = e;
}
uint32_t NativeScan(NativeEnv& env, const Args& a) {
    rr::sim::GuestRam g = pop::View(env);
    Seams s(env);
    const bool ok = rr::sim::CopArrestScan(g, a.a[0], env.sp, s);
    return pop::PopDone(env, g, ok, "CopArrestScan");
}

} // namespace copr

// --coverage: cop_arrest_scan counts the contiguous extent Arrest 0x80096F30, Stun 0x80097308 and the scan
// 0x80097388..0x80097470, because the scan's only writes are Arrest's.
const CoverageRange* CopsCoverage(const char* rowName) {
    static const CoverageRange kScan{0x80096F30, 0x80097470};
    return std::strcmp(rowName, "cop_arrest_scan") == 0 ? &kScan : nullptr;
}

void AddCopRows(std::vector<Row>& rows) {
    using namespace copr;
    rows.push_back(CopRow("cop_dist", "s32 CopDist(Bike *a, Bike *b)  [RASHCDG 0x8009E444]", rr::sim::kCopDistFn,
                          SetupDist, NativeDist, true, false));
    rows.push_back(CopRow("cop_target_ok", "s32 CopTargetOk(Bike *cop, Bike *t)  [RASHCDG 0x8009DB58]",
                          rr::sim::kCopTargetOkFn, SetupPair, NativeTargetOk, true, false));
    rows.push_back(CopRow("cop_stop_test", "s32 CopStopTest(Bike *cop, Bike *t, s32 dist, s32 stop)  [RASHCDG 0x8009DBA0]",
                          rr::sim::kCopStopTestFn, SetupStopTest, NativeStopTest, true, false));
    rows.push_back(CopRow("cop_ahead", "s32 CopAhead(Bike *cop, Bike *t)  [RASHCDG 0x8009E3F0]", rr::sim::kCopAheadFn,
                          SetupPair, NativeAhead, true, false));
    rows.push_back(CopRow("cop_wanted_bits", "void CopWantedBits(u16 list)  [RASHCDG 0x8009FC80]",
                          rr::sim::kCopWantedBitsFn, SetupWanted, NativeWanted, false, false));
    rows.push_back(CopRow("cop_pick_target", "u16 CopPickTarget(Bike *cop)  [RASHCDG 0x8009E178]",
                          rr::sim::kCopPickTargetFn, SetupPair, NativePick, true, false));
    rows.push_back(CopRow("cop_in_range", "s32 CopInRange(Bike *cop, Bike *t, s32 dist)  [RASHCDG 0x800BC120]",
                          rr::sim::kCopInRangeFn, SetupInRange, NativeInRange, true, false));
    rows.push_back(CopRow("cop_arrest", "void CopArrest(Bike *cop, Bike *t)  [RASHCDG 0x8009E528]", rr::sim::kCopArrestFn,
                          SetupArrest, NativeArrest, false, true));
    rows.push_back(CopRow("cop_tail", "s32 CopTail(Bike *cop)  [RASHCDG 0x8009DC90, the planner's cop tail]",
                          rr::sim::kCopTailFn, SetupTail, NativeTail, true, true));
    rows.push_back(CopRow("cop_intercept", "void CopIntercept(Bike *cop, u16 target)  [RASHCDG 0x800BBEBC, op 17]",
                          rr::sim::kCopInterceptFn, SetupIntercept, NativeIntercept, false, true));
    rows.push_back(CopRow("cop_idle", "void CopIdle(Bike *e, Cmd *cmd, s32 dt)  [RASHCDG 0x800BAA2C, op 1]",
                          rr::sim::kCopIdleFn, SetupIdle, NativeIdle, false, true));
    rows.push_back(CopRow("cop_arrest_test", "s32 CopArrestTest(Bike *e, Bike *t)  [RASHCDG 0x80097470]",
                          rr::sim::kCopArrestTestFn, SetupPair, NativeArrestTest, true, true));
    rows.push_back(CopRow("cop_arrest_scan", "void CopArrestScan(Bike *e)  [RASHCDG 0x80097388]",
                          rr::sim::kCopArrestScanFn, SetupScan, NativeScan, false, true));
}
