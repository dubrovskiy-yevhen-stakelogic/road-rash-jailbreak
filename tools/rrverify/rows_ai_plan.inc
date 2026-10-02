// Bench rows for the AI planner. Included by verify_physics.cpp inside
// its anonymous namespace AFTER rows_population.inc, whose seam object (pop::PopSeams: the oracle at an
// exact sp, the ported stance event) these rows reuse.
//
// The port is src\game\sim\ai_plan.{h,cpp}, over road_query.h's guest-address view. Every native side
// wraps the clone's RAM in a view, calls the port with the guest's own arguments at the bench's `sp`,
// and fails the case loudly if the port refused or met an address the console would not survive.
// Unported callees (0x800BC1EC, 0x800BBD44, 0x8009DC90, 0x80097388, SLUS 0x8001A760) and SLUS
// 0x800138E8 ComputePlace run as the ORIGINAL code on the clone at exactly the sp the port computes
// (bs::ExactOracle); AiPushCommand (ai.h) and the stance event (stance.h) run natively.
//
// INPUT FAMILIES. rr-race holds 18 pool-0 bikes (the player and bike 6 live), one player.
//   * dump-derived: the machine's bikes as captured; the player count (0..2, a second player bike),
//     the pass parity word and the planner's globals drawn;
//   * randomised: in addition, every bike's planner fields - the rider record's mood, divisor and
//     accumulator, health, grudge bytes, place, finish word, weapon fields, handle lists, event byte;
//     the entity's live word, progress (around player 1's, both sides of every window), speed, AI
//     bits, proximity score and rank, rider state, engagement list, road flags, command stack (depth,
//     opcodes, targets, stamps), road keys - plus the partner chain (e+0x358 / e+0x440), the game mode
//     byte (cop mode, 36, 44), the players' state bytes, the rank gate, the race flags, the head-to-
//     head word and claim masks, the drama amounts and the three nibble tables.
// KNOWN HAZARD (not a bound; the full family runs clean): AiChooseCommand's record lives at AiPlan
// sp-128 and its spare halfword (+4, sp-124) is never written by the planner, while AiPushCommand's
// mode-2 rotation copies it into the entity's stack. The guest's ComputePlace (frame 48, at AiPlan
// sp-96) spills the planner's s1 register exactly there, which no port reproduces. A case where the
// places are recomputed AND the chosen record is rotated from below the top would show it as a RAM
// difference in that slot's spare halfword; the family below (stacks up to 5 deep on every parity)
// has produced none on either seed.
// NAMED BOUND (ai_plan): a player whose game_state state byte (+0x0A + p) is 2 has a partner bike at
// e+0x358. Loop 5 reads partner[+0x354][+604] without a null test; with no partner the console reads
// kernel RAM at 0x354 and then the BIOS ROM, which the guest-address view does not map (the port
// refuses there rather than guess). Such a player is drawn finished (riderDef+0x28 != 0) and not a
// cop: loop 2 moves its own loop variable onto the partner (e = e[+0x358]) and walks on from there, so
// an unfinished player (or a cop - it skips the finished test) with a partner in a higher slot runs
// the loop past the pool's end into pool 1 (the console then faults on a rider record read as a
// bike). AI partner chains are drawn toward a LOWER slot for the same reason.

namespace aip {
using pop::Chance;
using pop::Put8;
using pop::U;

constexpr uint32_t kParity = 0x8005B2A8, kRankGate = 0x800CD540, kRaceFlagsW = 0x8005AD48,
                   kDrama = 0x800CCAC4, kClaims = 0x800CCAC0, kDramaAmt = 0x800CD530,
                   kNibbleTabs = 0x8005ADCC, kPlayer1 = 0x8005B38C, kBikesMax = 18;

uint8_t Get8(const Memory& m, uint32_t a) {
    uint8_t b = 0;
    m.ReadBlock(a, &b, 1);
    return b;
}
uint16_t Get16(const Memory& m, uint32_t a) {
    uint16_t v = 0;
    m.ReadBlock(a, &v, 2);
    return v;
}
bool InRam(uint32_t a) { return a >= 0x80000000u && a < 0x801FF000u; }
uint32_t RdOf(const Memory& m, uint32_t e) { return m.PeekWord(e + 0x43C); }
uint32_t BikeAt(const Memory& m, uint32_t i) { return m.PeekWord(kPoolBasePtr) + 1096u * i; }
uint32_t AnyHandle(Rng& r) { return r.U32() % kBikesMax; }

// A packed handle list (AiHandleInList): up to three 5-bit fields of handle + 1.
uint32_t DrawList(Rng& r, uint32_t want, uint32_t bits) {
    uint32_t v = 0;
    const uint32_t n = r.U32() % 4u;
    for (uint32_t i = 0; i < n; ++i) v |= (((Chance(r, 2) ? want : AnyHandle(r)) + 1u) & 31u) << (5u * i);
    if (Chance(r, 10)) v = r.U32();
    return bits >= 32u ? v : v & ((1u << bits) - 1u);
}

// The command stack: depth 0..maxDepth (now and then -1 or 16 when deep stacks are allowed), each
// slot's opcode from the planner's set, its target one of the two focus handles or any.
void DrawStack(CaseContext& c, uint32_t e, uint32_t maxDepth, uint32_t hA, uint32_t hB) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    static const uint16_t kOps[] = {0, 3, 4, 6, 8, 9, 10, 13, 14, 15, 16, 16, 16, 11, 2, 18};
    uint32_t depth = r.U32() % (maxDepth + 1u);
    if (maxDepth > 1u && Chance(r, 40)) depth = Chance(r, 2) ? 0xFFu : 16u;
    Put8(m, e + 0x3B2, depth);
    const uint32_t n = depth <= 16u ? depth : 0u;
    for (uint32_t k = 1; k <= n; ++k) {
        const uint32_t s = e + 0x3B4u + 8u * k;
        WriteU16Bench(m, s, kOps[r.U32() % 16u]);
        const uint32_t t = Chance(r, 3) ? hA : (Chance(r, 2) ? hB : AnyHandle(r));
        WriteU16Bench(m, s + 2, static_cast<uint16_t>(t));
        WriteU16Bench(m, s + 4, static_cast<uint16_t>(r.U32()));
        WriteU16Bench(m, s + 6, static_cast<uint16_t>(r.U32()));
    }
}

// One bike's planner fields (the header comment's list). `focus` is the handle its lists and stack
// are drawn toward; `p1` player 1's progress.
void StirBike(CaseContext& c, uint32_t e, uint32_t maxDepth, uint32_t focus, int32_t p1, uint32_t key) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    const uint32_t rd = RdOf(m, e);
    if (!InRam(rd)) return;
    const uint32_t h = Get16(m, e + 0xAC);
    if (Chance(r, 16)) Put8(m, rd + 1, (Get8(m, rd + 1) & 0xF0u) | 2u);           // a cop bike
    else if (Chance(r, 8)) Put8(m, rd + 1, (Get8(m, rd + 1) & 0xF0u) | (r.U32() % 2u));
    Put8(m, rd + 2, r.U32());
    Put8(m, rd + 3, Chance(r, 12) ? 0u : 1u + r.U32() % 8u);
    m.PokeWord(rd + 4, Chance(r, 2) ? r.U32() % 0x80000u : r.U32());
    Put8(m, rd + 14, r.U32() % 40u);
    Put8(m, rd + 15, r.U32() % 40u);
    for (uint32_t k = 0; k < 20u; ++k)
        Put8(m, rd + 16u + k, Chance(r, 10) ? r.U32() : U(static_cast<int32_t>(r.U32() % 33u) - 16));
    Put8(m, rd + 0x27, Chance(r, 10) ? 245u + r.U32() % 11u : r.U32() % 20u);
    m.PokeWord(rd + 0x28, Chance(r, 8) ? 1u + r.U32() % 1000u : 0u);
    WriteU16Bench(m, rd + 0x2C, static_cast<uint16_t>(Chance(r, 4) ? 0u : r.U32() & 0x1FFu));
    m.PokeWord(rd + 0x30, r.U32());
    for (uint32_t k = 0; k < 8u; ++k) Put8(m, rd + 0x34u + k, r.U32());
    Put8(m, rd + 0x3C, r.U32());
    Put8(m, rd + 0x3D, r.U32());
    WriteU16Bench(m, rd + 0x40, static_cast<uint16_t>(DrawList(r, focus, 15)));
    WriteU16Bench(m, rd + 0x42, static_cast<uint16_t>(DrawList(r, focus, 15)));
    Put8(m, rd + 0x44, Chance(r, 3) ? r.U32() & 0x7Fu : 0u);
    Put8(m, rd + 0x46, Chance(r, 3) ? focus : (Chance(r, 2) ? 31u : AnyHandle(r)));
    Put8(m, rd + 0x47, Chance(r, 3) ? focus : (Chance(r, 2) ? 31u : AnyHandle(r)));
    static const uint16_t kLive[] = {0, 0x13, 0x10, 0x20, 0x9, 1, 0x30, 0x13};
    WriteU16Bench(m, e + 0x140, kLive[r.U32() % 8u]);
    PutS32(m, e + 0x144, static_cast<int32_t>(U(p1) + U(static_cast<int32_t>(r.U32() % 0x80000u) - 0x60000)));
    PutS32(m, e + 0x1E0, static_cast<int32_t>(r.U32() % 0x400000u));
    {
        uint32_t f = m.PeekWord(e + 0x230);
        if (Chance(r, 6)) f ^= 0x08000000u;
        if (Chance(r, 12)) f ^= 0x10000000u;
        m.PokeWord(e + 0x230, f);
    }
    Put8(m, e + 0x352, r.U32());
    Put8(m, e + 0x353, r.U32() % 24u);
    if (const uint32_t R = m.PeekWord(e + 0x354); InRam(R)) m.PokeWord(R + 604, r.U32() % 6u);
    m.PokeWord(e + 0x390, DrawList(r, focus, 32));
    Put8(m, e + 0x3A0, r.U32() & 0x19u);
    DrawStack(c, e, maxDepth, focus, h);
    if (Chance(r, 2)) m.PokeWord(e + 0x168, key);
    if (Chance(r, 2)) m.PokeWord(e + 0x150, 7u);
    PutS32(m, e + 0x158, static_cast<int32_t>(r.U32() % 0x600000u) - 0x300000);
    PutS32(m, e + 0x16C, Chance(r, 2) ? 1 : -1);
}

// The planner's globals (the header comment's list); returns the player count.
uint32_t StirGlobals(CaseContext& c, bool deep) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    const uint32_t np = pop::DrawPlayers(c);
    const uint32_t gs = m.PeekWord(kGameStatePtr);
    static const uint8_t kModes[] = {0x22, 0x23, 36, 44, 0x22, 0x21};
    Put8(m, gs + 4, kModes[r.U32() % 6u]);
    Put8(m, gs + 10, Chance(r, 3) ? 2u : r.U32() % 4u);
    Put8(m, gs + 11, Chance(r, 3) ? 2u : r.U32() % 4u);
    WriteU16Bench(m, kRankGate, static_cast<uint16_t>(Chance(r, 3) ? 1u : r.U32() % 24u));
    m.PokeWord(kRaceFlagsW, Chance(r, 2) ? 0u : r.U32() & 0x3Fu);
    static const uint32_t kDramaBits[] = {0, 1, 0x10, 0x11, 0x40, 0x41, 0x50, 0xF1, 0x20};
    m.PokeWord(kDrama, (m.PeekWord(kDrama) & ~0xFFu) | kDramaBits[r.U32() % 9u]);
    WriteU16Bench(m, kClaims, static_cast<uint16_t>(Chance(r, 2) ? 0u : r.U32() & 0xFu));
    WriteU16Bench(m, kClaims + 2, static_cast<uint16_t>(Chance(r, 2) ? 0u : r.U32() & 0xFu));
    for (uint32_t k = 0; k < kBikesMax; ++k) Put8(m, kDramaAmt + k, r.U32() % 12u);
    if (deep && Chance(r, 3))
        for (uint32_t k = 0; k < 5u; ++k) m.PokeWord(kNibbleTabs + 4u * k, r.U32());
    // the fields of the player's own lasting state the cop-mode tail reads
    PutS32(m, m.PeekWord(kPlayer1) + 0x144, static_cast<int32_t>(r.U32() % 0x4000000u) + 0x100000);
    return np;
}

// A player's partner (e+0x358) for loop 5, and now and then a partner chain on an AI bike; e+0x440 is
// cleared only on the chain's first bike, so the regeneration walk always ends.
void DrawPartners(CaseContext& c, uint32_t np, bool deep) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    for (uint32_t p = 0; p < np && p < 2u; ++p) {
        const uint32_t pb = m.PeekWord(kPlayerArr + 4u * p);
        const bool state2 = Get8(m, m.PeekWord(kGameStatePtr) + 10u + p) == 2u;
        if (!InRam(pb) || (!state2 && !Chance(r, 2))) continue;
        uint32_t q = BikeAt(m, 2u + r.U32() % 15u);
        if (q == pb) q = BikeAt(m, 1);
        m.PokeWord(pb + 0x358, q);
        if (InRam(RdOf(m, pb))) {   // the bound below: finished, and not a cop (a cop skips that test)
            m.PokeWord(RdOf(m, pb) + 0x28, 1u + r.U32() % 100u);
            if ((Get8(m, RdOf(m, pb) + 1) & 0xFu) == 2u) Put8(m, RdOf(m, pb) + 1, Get8(m, RdOf(m, pb) + 1) & 0xF0u);
        }
    }
    if (deep && Chance(r, 3)) {
        const uint32_t a = 10u + r.U32() % 8u, b = 2u + r.U32() % 8u;   // b below a: the walk stays in the pool
        m.PokeWord(BikeAt(m, a) + 0x358, BikeAt(m, b));
        if (Chance(r, 2)) m.PokeWord(BikeAt(m, a) + 0x440, 0);
    }
}

// ---------------------------------------------------------------------------- the seams
std::vector<OracleCallee> AiPlanSeamCallees() {
    std::vector<OracleCallee> v = pop::PopSeamCallees();
    for (const OracleCallee& oc :
         {OracleCallee{rr::sim::kAiCanEngageFn, 3}, OracleCallee{rr::sim::kAiChaseTestFn, 3},
          OracleCallee{rr::sim::kAiCopTailFn, 1}, OracleCallee{rr::sim::kAiCopArrestScanFn, 1},
          OracleCallee{rr::sim::kAiRiderVoiceFn, 2}, OracleCallee{rr::sim::kAiComputePlaceFn, 2}})
        v.push_back(oc);
    return v;
}

struct Seams final : rr::sim::AiPlanCallees {
    NativeEnv& env;
    pop::PopSeams ps;
    explicit Seams(NativeEnv& e) : env(e), ps(e) {}
    bool CanEngage(uint32_t e, uint32_t t, int32_t lat, uint32_t sp, uint32_t& v0) override {
        return ps.Seam(rr::sim::kAiCanEngageFn, {e, t, U(lat)}, sp, &v0);
    }
    bool ChaseTest(uint32_t e, uint32_t t, int32_t lat, uint32_t sp, uint32_t& v0) override {
        return ps.Seam(rr::sim::kAiChaseTestFn, {e, t, U(lat)}, sp, &v0);
    }
    bool CopTail(uint32_t e, uint32_t sp) override { return ps.Seam(rr::sim::kAiCopTailFn, {e}, sp); }
    bool CopArrestScan(uint32_t e, uint32_t sp) override { return ps.Seam(rr::sim::kAiCopArrestScanFn, {e}, sp); }
    bool RiderVoice(uint32_t h, int32_t a1, uint32_t sp) override {
        return ps.Seam(rr::sim::kAiRiderVoiceFn, {h, U(a1)}, sp);
    }
    bool ComputePlace(uint32_t e, int32_t mode, uint32_t sp, int32_t& place) override {
        uint32_t v = 0;
        const bool ok = ps.Seam(rr::sim::kAiComputePlaceFn, {e, U(mode)}, sp, &v);
        place = static_cast<int32_t>(v);
        return ok;
    }
    bool StanceEvent(uint32_t ev, uint32_t r, uint32_t p, uint32_t sp) override { return ps.StanceEvent(ev, r, p, sp); }
    // AiPushCommand (ai.h) on raw views of the clone, the record read out of and written back to the
    // caller's frame, its return value kept (PopSeams::PushBytes drops it).
    bool PushCommand(uint32_t cmd, int32_t mode, uint32_t B, uint32_t sp, int32_t& v0) override {
        Memory& m = env.clone->mem;
        uint8_t bytes[8];
        m.ReadBlock(cmd, bytes, 8);
        uint8_t* e = pop::PopRaw(env, B, 1096);
        const uint32_t R = m.PeekWord(B + 852u);
        uint8_t* rider = pop::PopRaw(env, R, 640);
        const uint8_t* alt = pop::PopRaw(env, kAltKindTable, 8u * 65536u);
        const uint8_t* fight = pop::PopRaw(env, m.PeekWord(kFightRecPtr), 12u * 256u);
        if (e == nullptr || rider == nullptr || alt == nullptr || fight == nullptr) return false;
        struct Sink final : rr::sim::AiStanceSink {
            pop::PopSeams& s;
            uint32_t sp;
            Sink(pop::PopSeams& x, uint32_t y) : s(x), sp(y) {}
            void PlayIdleStance(uint16_t event, uint32_t riderArg) override { s.StanceEvent(event, riderArg, 2, sp); }
        } sink(ps, sp - pop::kPushFrame);
        rr::sim::AiPushEnv pe;
        pe.raceClock = static_cast<int32_t>(m.PeekWord(m.PeekWord(kGameStatePtr) + 0x10u));
        pe.rider = rider;
        pe.riderAddress = R;
        pe.altKindTable = alt;
        pe.fightRecords = fight;
        pe.stance = &sink;
        v0 = rr::sim::AiPushCommand(bytes, mode, e, pe);
        m.WriteBlock(cmd, bytes, 8);
        return env.failure.empty();
    }
};

Row AiRow(const char* name, const char* sig, uint32_t entry, std::function<void(CaseContext&, Args&)> setup,
          std::function<uint32_t(NativeEnv&, const Args&)> native, bool v0, bool seams) {
    Row r{name, sig, entry, std::move(setup), nullptr, v0, std::move(native)};
    r.maxSteps = 40'000'000;
    if (seams) r.oracleCallees = AiPlanSeamCallees();
    return r;
}

// ---------------------------------------------------------------------------- SLUS leaves
void SetupReach(CaseContext& c, Args& a) {
    Rng& r = *c.rng;
    const uint32_t k = r.U32() % 4u;
    a.a[0] = k == 0 ? r.U32() : (k == 1 ? r.U32() % 0x100000u : r.U32() % 0x1000000u);
    if (Chance(r, 8)) a.a[0] = U(-static_cast<int32_t>(r.U32() % 0x1000000u));
    a.a[1] = Chance(r, 8) ? r.U32() % 64u : r.U32() % 4u;
}
uint32_t NativeReachAhead(NativeEnv& env, const Args& a) {
    rr::sim::GuestRam g = pop::View(env);
    return pop::PopDone(env, g, true, "AiReachAhead", U(rr::sim::AiReachAhead(g, static_cast<int32_t>(a.a[0]), a.a[1])));
}
uint32_t NativeReachBehind(NativeEnv& env, const Args& a) {
    rr::sim::GuestRam g = pop::View(env);
    return pop::PopDone(env, g, true, "AiReachBehind", U(rr::sim::AiReachBehind(g, static_cast<int32_t>(a.a[0]), a.a[1])));
}
uint32_t NativeReachSide(NativeEnv& env, const Args& a) {
    rr::sim::GuestRam g = pop::View(env);
    return pop::PopDone(env, g, true, "AiReachSide", U(rr::sim::AiReachSide(g, static_cast<int32_t>(a.a[0]), a.a[1])));
}

// ---------------------------------------------------------------------------- the two-bike rows
// Two distinct bikes (or, one case in eight, the same one), both stirred toward each other.
void DrawPair(CaseContext& c, uint32_t maxDepth, uint32_t& e, uint32_t& t) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    const uint32_t np = (c.sample == nullptr || Chance(r, 2)) ? StirGlobals(c, c.sample == nullptr) : 1u;
    (void)np;
    const uint32_t i = Chance(r, 4) ? 0u : r.U32() % kBikesMax;
    uint32_t j = Chance(r, 8) ? i : r.U32() % kBikesMax;
    if (Chance(r, 5)) j = 0;
    e = BikeAt(m, i);
    t = BikeAt(m, j);
    const int32_t p1 = static_cast<int32_t>(m.PeekWord(m.PeekWord(kPlayer1) + 0x144));
    const uint32_t key = Chance(r, 2) ? 9u : (9u | (1u << 16));
    if (c.sample == nullptr) {
        StirBike(c, e, maxDepth, Get16(m, t + 0xAC), p1, key);
        if (t != e) StirBike(c, t, maxDepth, Get16(m, e + 0xAC), p1, key);
    }
    m.PokeWord(c.gp + kRandSeedGpOffset, r.U32());
}

void SetupRelation(CaseContext& c, Args& a) {
    uint32_t e, t;
    DrawPair(c, 5, e, t);
    a.a[0] = e;
    a.a[1] = t;
}
uint32_t NativeRelation(NativeEnv& env, const Args& a) {
    rr::sim::GuestRam g = pop::View(env);
    const uint32_t v = rr::sim::AiRelation(g, a.a[0], a.a[1]);
    return pop::PopDone(env, g, true, "AiRelation", v);
}

void SetupReachable(CaseContext& c, Args& a) {
    uint32_t e, t;
    DrawPair(c, 3, e, t);
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    if (c.sample == nullptr && Chance(r, 2)) {   // the target placed around e's own origin
        for (uint32_t k = 0; k < 3u; ++k)
            PutS32(m, t + 0xB8 + 4u * k,
                   static_cast<int32_t>(m.PeekWord(e + 0x1F8 + 4u * k)) + (static_cast<int32_t>(r.U32() % 0x1000000u) - 0x800000));
    }
    a.a[0] = t + 0xAC;
    a.a[1] = e;
}
uint32_t NativeReachable(NativeEnv& env, const Args& a) {
    rr::sim::GuestRam g = pop::View(env);
    const int32_t v = rr::sim::AiReachable(g, a.a[0], a.a[1]);
    return pop::PopDone(env, g, true, "AiReachable", U(v));
}

void SetupOwnEvent(CaseContext& c, Args& a) {
    uint32_t e, t;
    DrawPair(c, 2, e, t);
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    m.PokeWord(kParity, r.U32() % 4u);
    if (Chance(r, 4)) m.PokeWord(kLiveBikes, r.U32() % 24u);
    a.a[0] = e;
    a.a[1] = Chance(r, 8) ? r.U32() : r.U32() % 22u;
}
uint32_t NativeOwnEvent(NativeEnv& env, const Args& a) {
    rr::sim::GuestRam g = pop::View(env);
    const uint32_t v = rr::sim::AiOwnEvent(g, a.a[0], a.a[1]);
    return pop::PopDone(env, g, true, "AiOwnEvent", v);
}

void SetupWeaponPick(CaseContext& c, Args& a) {
    uint32_t e, t;
    DrawPair(c, 1, e, t);
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    const uint32_t rd = RdOf(m, e);
    static const uint8_t kCmd[] = {0x80, 0x8F, 0x20, 0x26, 0x00, 0xA0, 0x10, 0xFF};
    if (Chance(r, 2)) Put8(m, rd + 0x3C, kCmd[r.U32() % 8u]);
    a.a[0] = rd;
}
uint32_t NativeWeaponPick(NativeEnv& env, const Args& a) {
    rr::sim::GuestRam g = pop::View(env);
    const bool ok = rr::sim::AiWeaponPick(g, a.a[0]);
    return pop::PopDone(env, g, ok, "AiWeaponPick");
}
uint32_t NativeTaunt(NativeEnv& env, const Args& a) {
    rr::sim::GuestRam g = pop::View(env);
    const bool ok = rr::sim::AiTaunt(g, a.a[0]);
    return pop::PopDone(env, g, ok, "AiTaunt");
}

void SetupChoose(CaseContext& c, Args& a) {
    uint32_t e, t;
    DrawPair(c, 5, e, t);
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    if (t == e) t = BikeAt(m, (Get16(m, e + 0xAC) + 1u) % kBikesMax);
    // the top slot aimed at the target half the time (the replace arm), with a fight on top now and then
    const int32_t d = static_cast<int8_t>(Get8(m, e + 0x3B2));
    if (d > 0 && d <= 16) {
        const uint32_t top = e + 0x3B4u + 8u * U(d);
        if (Chance(r, 2)) WriteU16Bench(m, top + 2, Get16(m, t + 0xAC));
        if (Chance(r, 3)) WriteU16Bench(m, top, static_cast<uint16_t>(Chance(r, 3) ? 6u : 16u));
    }
    if (const uint32_t R = m.PeekWord(e + 0x354); InRam(R) && Chance(r, 4))
        WriteU16Bench(m, R + 0x220, static_cast<uint16_t>(r.U32() % 48u));
    PutS32(m, t + 0x144, static_cast<int32_t>(m.PeekWord(e + 0x144)) + static_cast<int32_t>(r.U32() % 0x20000u) - 0x10000);
    if (!Chance(r, 3)) {   // an engageable target (0x800BC1EC's gates): live, riding, on e's road, close
        WriteU16Bench(m, t + 0x140, 0x13);
        if (const uint32_t R = m.PeekWord(t + 0x354); InRam(R)) m.PokeWord(R + 604, r.U32() % 2u);
        m.PokeWord(t + 0x168, m.PeekWord(e + 0x168));
        m.PokeWord(t + 0x150, m.PeekWord(e + 0x150));
        PutS32(m, t + 0x158, static_cast<int32_t>(m.PeekWord(e + 0x158)) + static_cast<int32_t>(r.U32() % 0x100000u) - 0x80000);
        if (Chance(r, 4)) Put8(m, m.PeekWord(kGameStatePtr) + 0x39, 3);
    }
    a.a[0] = e;
    a.a[1] = Chance(r, 6) ? r.U32() : U(static_cast<int32_t>(r.U32() % 33u) - 16);
    a.a[2] = t;
}
uint32_t NativeChoose(NativeEnv& env, const Args& a) {
    rr::sim::GuestRam g = pop::View(env);
    Seams s(env);
    uint32_t v0 = 0;
    const bool ok = rr::sim::AiChooseCommand(g, a.a[0], static_cast<int32_t>(a.a[1]), a.a[2], env.sp, s, v0);
    return pop::PopDone(env, g, ok, "AiChooseCommand", v0);
}

// ---------------------------------------------------------------------------- the planner
void SetupPlan(CaseContext& c, Args& a) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    const bool deep = c.sample == nullptr;
    const uint32_t np = StirGlobals(c, deep);
    const uint32_t parity = r.U32() % 4u;
    m.PokeWord(kParity, parity);
    const uint32_t maxDepth = 5u;
    const int32_t p1 = static_cast<int32_t>(m.PeekWord(m.PeekWord(kPlayer1) + 0x144));
    const uint32_t key = Chance(r, 2) ? 9u : (9u | (1u << 16));
    for (uint32_t i = 0; i < kBikesMax; ++i) {
        const uint32_t e = BikeAt(m, i);
        if (deep) {
            StirBike(c, e, maxDepth, Chance(r, 2) ? 0u : AnyHandle(r), p1, key);
        } else {
            if (Chance(r, 3)) WriteU16Bench(m, e + 0x140, 0x13);
            PutS32(m, e + 0x144, static_cast<int32_t>(U(p1) + U(static_cast<int32_t>(r.U32() % 0x80000u) - 0x60000)));
            if (Chance(r, 2)) Put8(m, e + 0x3A0, Get8(m, e + 0x3A0) | 1u);
        }
        if (static_cast<int8_t>(Get8(m, e + 0x3B2)) > static_cast<int32_t>(maxDepth)) Put8(m, e + 0x3B2, maxDepth);
    }
    DrawPartners(c, np, deep);
    m.PokeWord(c.gp + kRandSeedGpOffset, r.U32());
    a.a[0] = Chance(r, 4) ? 0u : r.U32() % 0x60000u;
}
uint32_t NativePlan(NativeEnv& env, const Args& a) {
    rr::sim::GuestRam g = pop::View(env);
    Seams s(env);
    const bool ok = rr::sim::AiPlan(g, static_cast<int32_t>(a.a[0]), env.sp, s);
    return pop::PopDone(env, g, ok, "AiPlan");
}

} // namespace aip

void AddAiPlanRows(std::vector<Row>& rows) {
    rows.push_back(aip::AiRow("ai_reach_ahead", "s32 AiReachAhead(s32 speed, u32 cls)  [SLUS 0x800205C0]",
                              rr::sim::kAiReachAheadFn, aip::SetupReach, aip::NativeReachAhead, true, false));
    rows.push_back(aip::AiRow("ai_reach_behind", "s32 AiReachBehind(s32 speed, u32 cls)  [SLUS 0x8002064C]",
                              rr::sim::kAiReachBehindFn, aip::SetupReach, aip::NativeReachBehind, true, false));
    rows.push_back(aip::AiRow("ai_reach_side", "s32 AiReachSide(s32 speed, u32 cls)  [SLUS 0x800206DC]",
                              rr::sim::kAiReachSideFn, aip::SetupReach, aip::NativeReachSide, true, false));
    rows.push_back(aip::AiRow("ai_own_event", "u32 AiOwnEvent(Bike *e, u32 oldPlace)  [RASHCDG 0x800BD2A0]",
                              rr::sim::kAiOwnEventFn, aip::SetupOwnEvent, aip::NativeOwnEvent, true, false));
    rows.push_back(aip::AiRow("ai_relation", "u32 AiRelation(Bike *me, Bike *other)  [RASHCDG 0x800BCF6C]",
                              rr::sim::kAiRelationFn, aip::SetupRelation, aip::NativeRelation, true, false));
    rows.push_back(aip::AiRow("ai_reachable", "s32 AiReachable(Handle *h, Bike *e)  [RASHCDG 0x800BEA30]",
                              rr::sim::kAiReachableFn, aip::SetupReachable, aip::NativeReachable, true, false));
    rows.push_back(aip::AiRow("ai_weapon_pick", "void AiWeaponPick(RiderDef *rd)  [RASHCDG 0x800B9340]",
                              rr::sim::kAiWeaponPickFn, aip::SetupWeaponPick, aip::NativeWeaponPick, false, false));
    rows.push_back(aip::AiRow("ai_taunt", "void AiTaunt(RiderDef *rd)  [RASHCDG 0x800B92C0]",
                              rr::sim::kAiTauntFn, aip::SetupWeaponPick, aip::NativeTaunt, false, false));
    rows.push_back(aip::AiRow("ai_choose_command", "s32 AiChooseCommand(Bike *e, s8 aggr, Bike *t)  [RASHCDG 0x800B8FB0]",
                              rr::sim::kAiChooseCommandFn, aip::SetupChoose, aip::NativeChoose, true, true));
    rows.push_back(aip::AiRow("ai_plan", "void AiPlan(s32 acc)  [RASHCDG 0x800B8018, the planner pass]",
                              rr::sim::kAiPlanFn, aip::SetupPlan, aip::NativePlan, false, true));
}
