// Bench rows for the AI's per-frame brain. Included by
// verify_physics.cpp inside its anonymous namespace, after rows_population.inc (whose helpers and
// seams it reuses).
//
// The port is src\game\sim\ai_brain.{h,cpp}, over road_query.h's guest-address view. Unported callees
// (0x800BB448, 0x800BBBB8) run as the ORIGINAL code on the clone at exactly the stack pointer the port
// computes (bs::ExactOracle); the ported ones run natively on the clone's own RAM: AiPushCommand /
// AiPopCommand (ai.h, their stance trigger 0x800C4550 native and witnessed, rows_anim.inc) and
// SetAimDelta (ai.h).
//
// INPUT FAMILIES. rr-race holds 18 bikes, their riders, a traffic car and one player.
//   * dump-derived: the machine's own bikes, entity values and spatial grid as captured, the subject
//     moved onto a bike the grid lists;
//   * randomised: the spatial grid REBUILT around the subject (origins drawn so the subject lies in
//     grid 0 or - with two players - grid 1; every bike, some riders, cars and pool-6 records linked
//     into the cells their positions give), 0..4 candidates placed ahead of the subject along its
//     own heading at drawn distances and lateral offsets (road key shared or not), speeds, headings,
//     half sizes, the command stacks (depth 1..6, opcodes 3..17 and the stamp's fresh bit), the
//     other bike's top command (10, 6, 7 or any, its target the subject), the class nibbles, the
//     +0x3A0 / +0x184 / +0x230 / +0x234 bits, the lean (+0x28C) at the +-1143/1144 edges, the lane
//     counts and width, the level, the +0x2C0 timer and +0x2A4 around 2130, and the stale halfword
//     the brain reads at its frame's sp+98.
// NAMED BOUND: every object a grid cell or a command names is a live object carrying its own handle
// (cars 0..3 and pool-6 records 0..3 are given theirs). A slot whose handle word names another pool
// sends the original through a "bike" at guest 0 whose passenger word is a BIOS address - a read the
// guest-address view does not map and the game never makes.
// Unreachable arms (by `--coverage`): top slot address 0 (0x800BD738, 0x800BDD5C) and the sign arms
// of the divides by the gap (0x800BE420.., 0x800BE518.., 0x800BE72C..), which need gap <= 0 or
// limit <= 3.0 against guards that exclude both.

namespace aib {
using pop::Chance;
using pop::Put8;

constexpr uint32_t kQueryFn = 0x8008AE94, kProbeFn = 0x800BEBA4, kBrainFn = 0x800BD4D4, kPassFn = 0x800BCD48;
constexpr uint32_t kGapTestFn = 0x800BB448, kCloseTestFn = 0x800BBBB8;
constexpr uint32_t kPopFrame = 32; // `addiu sp,sp,-32` at 0x800BC8DC, the stance event's caller

uint32_t U(int32_t v) { return static_cast<uint32_t>(v); }
int32_t S(uint32_t v) { return static_cast<int32_t>(v); }
bool InRam(uint32_t a) { return a >= 0x80000000u && a < 0x801FF000u; }

std::vector<OracleCallee> Callees() {
    return {OracleCallee{kGapTestFn, 3},
            OracleCallee{kCloseTestFn, 3},
            // the ported stance event, witnessed, and the stance layer's own seams under it
            OracleCallee{kAiStance, 3},
            OracleCallee{rr::sim::kAnimTransitionCaptureFn, 3},
            OracleCallee{rr::sim::kAnimApplyFrameFn, 1},
            OracleCallee{rr::sim::kCombatLeaveFn, 4},
            OracleCallee{rr::sim::kCombatEnterFn, 4}};
}

// ================================================================ the seams
struct Seams final : rr::sim::AiBrainCallees {
    NativeEnv& env;
    pop::PopSeams ps;
    explicit Seams(NativeEnv& e) : env(e), ps(e) {}

    bool PushCommand(uint32_t cmd, int32_t mode, uint32_t e, uint32_t sp) override {
        return ps.PushCommand(cmd, mode, e, sp);
    }
    // AiPopCommand (ai.h) on raw views of the clone; its stance trigger at the depth of the level of
    // the recursion that fires it (each level's frame is 32 bytes).
    bool PopCommand(uint32_t ea, uint32_t sp) override {
        Memory& m = env.clone->mem;
        uint8_t* e = pop::PopRaw(env, ea, 1096);
        uint8_t* rd = pop::PopRaw(env, m.PeekWord(ea + 1084u), 72);
        const uint32_t R = m.PeekWord(ea + 852u);
        const uint8_t* rider = pop::PopRaw(env, R, 640);
        const uint8_t* gs = pop::PopRaw(env, m.PeekWord(kGameStatePtr), 64);
        const uint8_t* alt = pop::PopRaw(env, kAltKindTable, 8u * 65536u);
        const uint8_t* fight = pop::PopRaw(env, m.PeekWord(kFightRecPtr), 12u * 256u);
        const uint8_t* mask = pop::PopRaw(env, kAttackerMask, 16);
        if (e == nullptr || rd == nullptr || rider == nullptr || gs == nullptr || alt == nullptr ||
            fight == nullptr || mask == nullptr)
            return false;
        struct Sink final : rr::sim::AiStanceSink {
            pop::PopSeams& s;
            uint32_t sp;
            uint32_t level = 0;
            Sink(pop::PopSeams& x, uint32_t y) : s(x), sp(y) {}
            void PlayIdleStance(uint16_t event, uint32_t riderArg) override {
                ++level;
                s.StanceEvent(event, riderArg, 2, sp - kPopFrame * level);
            }
        } sink(ps, sp);
        rr::sim::AiPopEnv pe;
        pe.gameState = gs;
        pe.riderDef = rd;
        pe.rider = rider;
        pe.riderAddress = R;
        pe.altKindTable = alt;
        pe.fightRecords = fight;
        pe.attackerMask = reinterpret_cast<const uint16_t*>(mask);
        pe.stance = &sink;
        rr::sim::AiPopCommand(e, pe);
        return env.failure.empty();
    }
    // SetAimDelta (ai.h) on a raw view of the entity; the pointed-to arguments read out of the clone.
    bool SetAimDelta(uint32_t ea, uint32_t target, uint32_t dir, uint32_t scalar, int32_t mode, uint32_t) override {
        Memory& m = env.clone->mem;
        uint8_t* e = pop::PopRaw(env, ea, 1096);
        if (e == nullptr) return false;
        int32_t t[3] = {0, 0, 0}, s = 0;
        int16_t d[3] = {0, 0, 0};
        if (target != 0) GetVec3S32(m, target, t);
        if (dir != 0) GetVec3S16(m, dir, d);
        if (scalar != 0) s = GetS32(m, scalar);
        std::vector<int16_t> w(kSqrtWindowHalf);
        m.ReadBlock(kSqrtTable - kSqrtWindowHalf / 2u, w.data(), w.size() * 2u);
        rr::sim::SetAimDelta(rr::sim::EntityView(e), target != 0 ? t : nullptr, dir != 0 ? d : nullptr,
                             scalar != 0 ? &s : nullptr, mode, w.data() + kSqrtWindowHalf / 4u);
        return true;
    }
    bool GapTest(uint32_t e, uint32_t other, int32_t gap, uint32_t sp, uint32_t& v0) override {
        return ps.Seam(kGapTestFn, {e, other, U(gap)}, sp, &v0);
    }
    bool CloseTest(uint32_t e, uint32_t other, int32_t gap, uint32_t sp, uint32_t& v0) override {
        return ps.Seam(kCloseTestFn, {e, other, U(gap)}, sp, &v0);
    }
};

// ================================================================ the scene
// The address of the handle of object (pool, idx) as the query resolves it (x at +12, z at +20), or 0
// when the pool has no records in RAM.
uint32_t HandleAddr(const Memory& m, uint32_t pool, uint32_t idx) {
    uint32_t a;
    if (pool == 0) a = m.PeekWord(rr::sim::kAiBrainPool0Ptr) + 1096u * idx + 0xACu;
    else if (pool == 1) a = m.PeekWord(rr::sim::kAiBrainPool1Ptr) + 628u * idx + 0xACu;
    else {
        const uint32_t rec = rr::sim::kAiBrainPoolTable + 16u * pool;
        const uint32_t base = m.PeekWord(rec);
        if (!InRam(base)) return 0;
        a = base + m.PeekWord(rec + 4u) * idx + m.PeekWord(rr::sim::kAiBrainHandleOff + 4u * (pool - 2u));
    }
    return InRam(a) && InRam(a + 280u) ? a : 0;
}

struct Scene {
    uint32_t sel = 0;          // the grid the subject lies in
    int32_t ox[2]{}, oz[2]{};  // the origins drawn
    bool rebuilt = false;
};

// Player count and grid origins so that (x, z) lies in grid `sel` at a drawn cell.
Scene DrawGrid(CaseContext& c, int32_t x, int32_t z) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    Scene s;
    const uint32_t gs = m.PeekWord(kGameStatePtr);
    const uint32_t np = Chance(r, 12) ? 0u : (Chance(r, 3) ? 2u : 1u);
    m.PokeWord(gs + 0x30u, np);
    s.sel = (np == 2 && Chance(r, 2)) ? 1u : 0u;
    for (uint32_t k = 0; k < 2; ++k) {
        const int32_t col = 1 + S(r.U32() % 22u), row = 1 + S(r.U32() % 22u);
        s.ox[k] = S(U(x) - (U(col - 11) << 21) - (r.U32() & 0x1FFFFFu));
        s.oz[k] = S(U(z) - (U(row - 11) << 21) - (r.U32() & 0x1FFFFFu));
    }
    if (s.sel == 1) {   // the subject outside grid 0
        s.ox[0] = S(U(x) - (U(30 + S(r.U32() % 8u)) << 21));
        if (Chance(r, 2)) s.oz[0] = S(U(z) + (U(20 + S(r.U32() % 8u)) << 21));
    }
    for (uint32_t k = 0; k < 2; ++k) {
        PutS32(m, rr::sim::kAiBrainGridOrgX + 4u * k, s.ox[k]);
        PutS32(m, rr::sim::kAiBrainGridOrgZ + 4u * k, s.oz[k]);
    }
    return s;
}

// Every object `objs` (pool << 5 | idx) linked into the cell its position gives in grid s.sel, the
// grid emptied first; now and then one in a neighbouring (wrong) cell.
void BuildGrid(CaseContext& c, const Scene& s, const std::vector<uint32_t>& objs) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    for (uint32_t i = 0; i < 48u * 24u; ++i) Put8(m, rr::sim::kAiBrainGridCells + i, 128);
    uint32_t next = 0;
    for (uint32_t code : objs) {
        if (next >= 128) break;
        const uint32_t ha = HandleAddr(m, code >> 5, code & 31u);
        if (ha == 0) continue;
        int32_t col = (S(m.PeekWord(ha + 12u) - U(s.ox[s.sel])) >> 21) + 11;
        int32_t row = (S(m.PeekWord(ha + 20u) - U(s.oz[s.sel])) >> 21) + 11;
        if (Chance(r, 12)) col += Chance(r, 2) ? 1 : -1;
        if (col < 0 || col > 23 || row < 0 || row > 23) continue;
        const uint32_t cell = rr::sim::kAiBrainGridCells + U((row + 24 * S(s.sel)) * 24 + col);
        Put8(m, rr::sim::kAiBrainGridLinks + 2u * next, m.PeekByte(cell));
        Put8(m, rr::sim::kAiBrainGridLinks + 2u * next + 1u, code);
        if ((code >> 5) != 0u) WriteU16Bench(m, ha, static_cast<uint16_t>(code));   // a listed object carries its handle
        Put8(m, cell, next);
        ++next;
    }
}

// Object `ha` (its handle address) put `ahead` along e's heading (+0x210) and `lat` across it (+0x204)
// from e's reference point +0x1F8, its road coordinate made e's (or not).
void PlaceAhead(CaseContext& c, uint32_t e, uint32_t ha, int32_t ahead, int32_t lat) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    for (uint32_t k = 0; k < 3; ++k) {
        const int64_t f = GetS16(m, e + 0x210u + 2u * k), sd = GetS16(m, e + 0x204u + 2u * k);
        const int64_t d = (f * ahead + sd * lat) >> 12;
        const uint32_t now = m.PeekWord(e + 0x1F8u + 4u * k) + static_cast<uint32_t>(d);
        const uint32_t moved = now - m.PeekWord(ha + 12u + 4u * k);
        PutS32(m, ha + 12u + 4u * k, S(now));
        // the box corners (+24.. and +48.. from the handle) travel with the object
        m.PokeWord(ha + 24u + 4u * k, m.PeekWord(ha + 24u + 4u * k) + moved);
        m.PokeWord(ha + 48u + 4u * k, m.PeekWord(ha + 48u + 4u * k) + moved);
    }
    if (!Chance(r, 3)) {
        if (Chance(r, 3)) m.PokeWord(e + 0x168u, m.PeekWord(e + 0x168u) ^ (1u + r.U32() % 3u) << 16);  // a node key
        m.PokeWord(ha + 188u, m.PeekWord(e + 0x168u));
        if (Chance(r, 2)) m.PokeWord(ha + 164u, m.PeekWord(e + 0x150u));
        PutS32(m, ha + 172u, S(m.PeekWord(e + 0x158u) + U(lat)));
    } else if (Chance(r, 2)) {
        m.PokeWord(ha + 188u, m.PeekWord(e + 0x168u) + 1u);   // another road
    } else if (Chance(r, 2)) {   // far off the road laterally (the score's penalty wraps)
        m.PokeWord(ha + 188u, m.PeekWord(e + 0x168u) & 0xFFFFu);
        m.PokeWord(e + 0x168u, m.PeekWord(e + 0x168u) & 0xFFFFu);
        PutS32(m, ha + 172u, S(m.PeekWord(e + 0x158u) + (r.U32() | 0x40000000u)));
        PutS32(m, ha + 152u, S(m.PeekWord(e + 0x144u) - U(ahead / 16) + r.U32() % 64u - 32u));
    }
}

// The subject's own fields the brain branches on, drawn over its captured values.
void DrawSubject(CaseContext& c, uint32_t e) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    // Cars 0..3 and pool-6 records 0..3 made live objects carrying their own handles: a command or
    // grid entry naming a slot whose handle word says otherwise sends the original through a
    // non-object (a bike at guest 0 whose passenger word is a BIOS address), which the game never does.
    for (uint32_t k = 0; k < 4; ++k) {
        if (const uint32_t ha = HandleAddr(m, 3, k)) WriteU16Bench(m, ha, static_cast<uint16_t>(0x60u | k));
        if (const uint32_t ha = HandleAddr(m, 6, k)) WriteU16Bench(m, ha, static_cast<uint16_t>(0xC0u | k));
    }
    // e's reference point next to its box centre, so the query and the score measure alike
    for (uint32_t k = 0; k < 3; ++k)
        PutS32(m, e + 0x1F8u + 4u * k, GetS32(m, e + 0xB8u + 4u * k) + (Chance(r, 3) ? S(r.U32() % 0x20000u) - 0x10000 : 0));
    const uint32_t sp = r.U32() % 10u;
    PutS32(m, e + 0x1E0u, sp == 0 ? S(r.U32() % 200u) : (sp == 1 ? S(r.U32()) >> 4 : 0x40000 + S(r.U32() % 0x400000u)));
    if (Chance(r, 2)) Put8(m, e + 0x3A0u, (m.PeekByte(e + 0x3A0u) & ~0x05u) | (r.U32() & 0x05u));
    if (Chance(r, 3)) m.PokeWord(e + 0x184u, m.PeekWord(e + 0x184u) ^ 1u);
    if (Chance(r, 3)) PutS32(m, e + 0x144u, 0x3C000 - 2 + S(r.U32() % 5u) + (Chance(r, 2) ? S(r.U32() % 0x200000u) : 0));
    static const int32_t kSteer[] = {-1144, -1143, 1143, 1144};
    PutS32(m, e + 652u, Chance(r, 2) ? kSteer[r.U32() % 4u] : S(r.U32() % 8192u) - 4096);
    PutS32(m, e + 0x16Cu, Chance(r, 2) ? 1 : -1);
    WriteU16Bench(m, e + 408u, static_cast<uint16_t>(r.U32() % 4u));
    WriteU16Bench(m, e + 420u, static_cast<uint16_t>(r.U32() % 4u));
    if (Chance(r, 2)) PutS32(m, e + 424u, Chance(r, 3) ? -S(r.U32() % 0x40000u) : 0x10000 + S(r.U32() % 0x60000u));
    WriteU16Bench(m, e + 878u, static_cast<uint16_t>(S(r.U32() % 200u) - 100));
    if (Chance(r, 3)) PutS32(m, e + 304u, 0x4000 + S(r.U32() % 0x30000u));
    if (Chance(r, 3)) PutS32(m, e + 308u, Chance(r, 16) ? -S(r.U32() % 0x8000u) : 0x2000 + S(r.U32() % 0x40000u));
    if (Chance(r, 3)) PutS32(m, e + 312u, S(r.U32() % 0x400000u));
    if (Chance(r, 3)) PutS32(m, e + 924u, S(r.U32() % 0x400000u));
    m.PokeWord(e + 560u, (m.PeekWord(e + 560u) & ~0x18000000u) | (Chance(r, 8) ? (r.U32() & 0x18000000u) : 0x08000000u));
    if (Chance(r, 2)) m.PokeWord(e + 564u, m.PeekWord(e + 564u) ^ (r.U32() & 0x00080200u));
    PutS32(m, e + 676u, Chance(r, 3) ? 2129 + S(r.U32() % 3u) : S(r.U32() % 8192u) - 4096);
    PutS32(m, e + 704u, Chance(r, 3) ? S(r.U32() % 64u) : S(r.U32() % 0x10000u) - 0x2000);
    // the command stack: depth 1..6, opcodes 3..17 (now and then 0..20), stamps, the spare word
    const uint32_t depth = 1u + r.U32() % 6u;
    Put8(m, e + 946u, depth);
    for (uint32_t k = 1; k <= depth; ++k) {
        const uint32_t slot = e + 0x3B4u + 8u * k;
        WriteU16Bench(m, slot, static_cast<uint16_t>(Chance(r, 8) ? r.U32() % 21u : 3u + r.U32() % 15u));
        static const uint16_t kTg[] = {0x60, 0x61, 0x62, 0x63, 0xC0, 0xC1};
        WriteU16Bench(m, slot + 2u, static_cast<uint16_t>(Chance(r, 2) ? 224u : (Chance(r, 2) ? r.U32() % 18u : kTg[r.U32() % 6u])));
        const uint32_t w4 = r.U32() % 4u;
        WriteU16Bench(m, slot + 4u, static_cast<uint16_t>(w4 == 0 ? 0u : (w4 == 1 ? 299u + r.U32() % 3u : r.U32() % 400u)));
        WriteU16Bench(m, slot + 6u, static_cast<uint16_t>(r.U32()));
    }
    if (Chance(r, 3)) WriteU16Bench(m, e + 0x3B4u + 8u * depth, 3);
    // the class nibble of the rider records (equal or not to the others')
    const uint32_t rd = m.PeekWord(e + 1084u);
    if (InRam(rd) && Chance(r, 4)) Put8(m, rd + 1u, (m.PeekByte(rd + 1u) & 0xF0u) | (r.U32() % 3u));
    const uint32_t gs = m.PeekWord(kGameStatePtr);
    if (!Chance(r, 4)) m.PokeWord(gs + 60u, r.U32() % 4u);
    if (Chance(r, 3)) m.PokeWord(gs + 16u, r.U32() % 0x1000000u);   // the race clock (the stamps)
    // the aim point (+0x370) around the reference point, the aimed slice's lateral axis (+0x368)
    if (Chance(r, 2))
        for (uint32_t k = 0; k < 3; ++k)
            PutS32(m, e + 0x370u + 4u * k, S(m.PeekWord(e + 0x1F8u + 4u * k) + r.U32() % 0xA0000u - 0x50000u));
    if (Chance(r, 2)) {
        WriteU16Bench(m, e + 872u, static_cast<uint16_t>(r.S16()));
        WriteU16Bench(m, e + 876u, static_cast<uint16_t>(r.S16()));
    }
}

// A 3-vector of s16 at `dst`: e's heading +0x210, reversed, or e's side axis +0x204 (across).
void Orient16(Memory& m, uint32_t e, uint32_t dst, uint32_t mode) {
    for (uint32_t k = 0; k < 3; ++k) {
        const int16_t f = GetS16(m, e + 0x210u + 2u * k), sd = GetS16(m, e + 0x204u + 2u * k);
        const int32_t v = mode == 0 ? f : (mode == 1 ? -f : sd);
        WriteU16Bench(m, dst + 2u * k, static_cast<uint16_t>(v));
    }
}
// The 16.16 heading pair (+0x128, +0x12C) relative to handle address `ha` (+124/+128): e's, reversed
// or swapped.
void OrientPair(Memory& m, uint32_t e, uint32_t ha, uint32_t mode) {
    const int32_t x = GetS32(m, e + 0x128u), z = GetS32(m, e + 0x12Cu);
    PutS32(m, ha + 124u, mode == 0 ? x : (mode == 1 ? S(0u - U(x)) : z));
    PutS32(m, ha + 128u, mode == 0 ? z : (mode == 1 ? S(0u - U(z)) : S(0u - U(x))));
}

// A candidate's fields: another bike's stack top, class, passenger, speed, heading; a car's alive
// word, flag, axis and speed; a pool-6 record's kind word and sizes; the handle itself.
void DrawCandidate(CaseContext& c, uint32_t e, uint32_t code, uint32_t ha) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    const uint32_t pool = code >> 5;
    uint16_t eh = 0;
    m.ReadBlock(e + 0xACu, &eh, 2);
    if (pool != 0) WriteU16Bench(m, ha, static_cast<uint16_t>(code));
    const uint32_t mode = Chance(r, 2) ? 0u : (Chance(r, 2) ? 1u : 2u);
    const uint32_t o = ha - 0xACu;
    if (pool == 0) {
        const uint32_t d = 1u + r.U32() % 4u;
        Put8(m, o + 946u, d);
        static const uint16_t kOp[] = {10, 6, 7, 3, 12};
        WriteU16Bench(m, o + 0x3B4u + 8u * d, Chance(r, 5) ? static_cast<uint16_t>(r.U32() % 19u) : kOp[r.U32() % 5u]);
        WriteU16Bench(m, o + 0x3B6u + 8u * d, Chance(r, 2) ? eh : static_cast<uint16_t>(r.U32() % 225u));
        const uint32_t rd = m.PeekWord(o + 1084u), erd = m.PeekWord(e + 1084u);
        if (InRam(rd) && InRam(erd) && Chance(r, 2))
            Put8(m, rd + 1u, (m.PeekByte(rd + 1u) & 0xF0u) | (m.PeekByte(erd + 1u) & 0x0Fu));
        m.PokeWord(o + 856u, Chance(r, 3) ? pop::Bike(m, r.U32() % 18u) : 0u);
        PutS32(m, o + 0x1E0u, Chance(r, 3) ? S(m.PeekWord(e + 0x1E0u) - r.U32() % 0x100000u) : S(r.U32() % 0x200000u));
        if (!Chance(r, 4)) {
            Orient16(m, e, o + 0x210u, mode);
            OrientPair(m, e, ha, mode);
        }
        const uint32_t R = m.PeekWord(o + 852u);
        if (InRam(R) && Chance(r, 6)) m.PokeWord(R + 552u, m.PeekWord(R + 552u) ^ 0x8000u);
    } else if (pool == 3) {
        if (Chance(r, 2)) WriteU16Bench(m, o + 320u, static_cast<uint16_t>(Chance(r, 4) ? 0u : 1u));
        if (Chance(r, 2)) Put8(m, o + 510u, m.PeekByte(o + 510u) ^ 2u);
        PutS32(m, o + 0x1E0u, Chance(r, 2) ? S(m.PeekWord(e + 0x1E0u) - r.U32() % 0x100000u) : S(r.U32() % 0x30000u));
        if (Chance(r, 2)) PutS32(m, ha + 140u, 0x19998 - 1 + S(r.U32() % 3u) + (Chance(r, 2) ? S(r.U32() % 0x40000u) - 0x20000 : 0));
        if (!Chance(r, 4)) {
            Orient16(m, e, o + 444u, mode);
            OrientPair(m, e, ha, mode);
        }
    } else if (pool == 6) {
        if (!Chance(r, 3)) PutS32(m, ha + 8u, Chance(r, 2) ? 1 : S(r.U32() % 4u));
        if (Chance(r, 2)) PutS32(m, ha + 84u, S(r.U32() % 0x40000u));
        if (Chance(r, 2)) PutS32(m, ha + 88u, S(m.PeekWord(e + 188u) - m.PeekWord(e + 312u) + r.U32() % 0x40000u - 0x20000u));
        if (!Chance(r, 4)) OrientPair(m, e, ha, mode);
    }
    if (Chance(r, 3)) PutS32(m, ha + 132u, S(r.U32() % 0x30000u));
    if (Chance(r, 3)) PutS32(m, ha + 136u, S(r.U32() % 0x30000u));
}

// The subject and its neighbourhood. Returns the objects placed (their codes).
std::vector<uint32_t> DrawNeighbours(CaseContext& c, uint32_t e, uint32_t eIdx) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    std::vector<uint32_t> placed;
    const uint32_t n = Chance(r, 6) ? 0u : 1u + r.U32() % 3u;
    const int32_t speed = GetS32(m, e + 0x1E0u);
    const int32_t reach = S((U(speed) + 0x141DDDu) * 2u);
    for (uint32_t k = 0; k < n; ++k) {
        const uint32_t pk = r.U32() % 8u;
        uint32_t code;
        if (pk < 4) {
            uint32_t idx = r.U32() % 18u;
            if (idx == eIdx) idx = (idx + 1u) % 18u;
            code = idx;
        } else if (pk < 6) {
            code = 0x60u | (r.U32() % 4u);
        } else {
            code = 0xC0u | (r.U32() % 4u);
        }
        const uint32_t ha = HandleAddr(m, code >> 5, code & 31u);
        if (ha == 0) continue;
        const uint32_t ak = r.U32() % 6u;
        const int32_t ahead = ak == 0 ? S(r.U32() % 0x100000u) - 0x80000
                            : ak == 1 ? -S(r.U32() % 0x10000u)
                            : ak == 2 ? 0x8000 + S(r.U32() % 0x60000u)
                                     : S(r.U32() % U(std::max(reach, 0x10000)));
        const int32_t lat = Chance(r, 4) ? S(r.U32() % 0x60000u) - 0x30000 : S(r.U32() % 0x30000u) - 0x18000;
        PlaceAhead(c, e, ha, ahead, lat);
        DrawCandidate(c, e, code, ha);
        placed.push_back(code);
    }
    // e's top command often on one of them (engaged / to re-aim / to hold)
    if (!placed.empty() && Chance(r, 2)) {
        const uint32_t top = e + 0x3B4u + 8u * m.PeekByte(e + 946u);
        WriteU16Bench(m, top + 2u, static_cast<uint16_t>(placed[r.U32() % placed.size()]));
        WriteU16Bench(m, top, static_cast<uint16_t>(Chance(r, 2) ? 3u : 3u + r.U32() % 15u));
    }
    return placed;
}

// The stale halfword at the brain's sp+98, which the original compares before writing (ai_brain.h).
void PlantStale(CaseContext& c, uint32_t brainSp, uint32_t e) {
    uint16_t eh = 0;
    c.mem->ReadBlock(e + 0xACu, &eh, 2);
    WriteU16Bench(*c.mem, brainSp - rr::sim::kAiBrainFrame + 98u,
                  Chance(*c.rng, 2) ? eh : static_cast<uint16_t>(c.rng->U32() % 225u));
}

// The whole scene around subject bike `eIdx`; the grid rebuilt (randomised cases, and one derived
// case in two) or kept as captured with the subject moved onto a bike it lists.
void DrawScene(CaseContext& c, uint32_t eIdx, bool rebuildAlways) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    const uint32_t e = pop::Bike(m, eIdx);
    if (c.sample != nullptr && !rebuildAlways && Chance(r, 2)) {
        // captured grid: onto the first pool-0 bike a cell lists
        for (uint32_t i = 0; i < 48u * 24u; ++i) {
            const uint32_t head = m.PeekByte(rr::sim::kAiBrainGridCells + i);
            if (head >= 128u) continue;
            const uint32_t code = m.PeekByte(rr::sim::kAiBrainGridLinks + 2u * head + 1u);
            if ((code >> 5) != 0u || code == eIdx) continue;
            const uint32_t o = pop::Bike(m, code);
            for (uint32_t k = 0; k < 3; ++k)
                PutS32(m, e + 0xB8u + 4u * k, GetS32(m, o + 0xB8u + 4u * k) - (k == 1 ? 0 : S(r.U32() % 0x60000u)));
            break;
        }
        DrawSubject(c, e);
        return;
    }
    DrawSubject(c, e);
    std::vector<uint32_t> placed = DrawNeighbours(c, e, eIdx);
    if (placed.empty() && Chance(r, 2)) {   // nothing ahead: a command 3 on a car, alive or not
        const uint32_t k = r.U32() % 4u, car = rr::sim::kAiBrainCarPool + 512u * k;
        const uint32_t top = e + 0x3B4u + 8u * m.PeekByte(e + 946u);
        WriteU16Bench(m, top, 3);
        WriteU16Bench(m, top + 2u, static_cast<uint16_t>(Chance(r, 6) ? r.U32() % 224u : 0x60u | k));
        Put8(m, e + 0x3A0u, m.PeekByte(e + 0x3A0u) | (Chance(r, 4) ? 0u : 1u));
        WriteU16Bench(m, car + 320u, static_cast<uint16_t>(Chance(r, 4) ? 0u : 1u));
        PutS32(m, car + 324u, S(m.PeekWord(e + 324u) + r.U32() % 0x40000u - 0x20000u));
    }
    const Scene s = DrawGrid(c, GetS32(m, e + 0xB8u), GetS32(m, e + 0xC0u));
    std::vector<uint32_t> objs;
    for (uint32_t i = 0; i < 18; ++i) objs.push_back(i);
    for (uint32_t code : placed) objs.push_back(code);
    for (uint32_t k = 0; k < 3; ++k) objs.push_back(0x20u | (r.U32() % 18u));   // riders, outside the mask
    for (uint32_t k = 0; k < 2; ++k) objs.push_back(0x60u | (r.U32() % 4u));
    for (size_t i = objs.size(); i > 1; --i) std::swap(objs[i - 1], objs[r.U32() % i]);
    BuildGrid(c, s, objs);
}

// ---------------------------------------------------------------- 0x8008AE94
void SetupQuery(CaseContext& c, Args& a) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    const uint32_t eIdx = r.U32() % 18u;
    const uint32_t e = pop::Bike(m, eIdx);
    DrawScene(c, eIdx, false);
    const uint32_t list = c.scratch + 0x100u, count = c.scratch + 0x140u, pos = c.scratch + 0x150u;
    for (uint32_t k = 0; k < 16; ++k) WriteU16Bench(m, list + 2u * k, static_cast<uint16_t>(r.U32()));
    const uint32_t cap = r.U32() % 6u;
    PutS32(m, count, cap == 0 ? S(r.U32() % 3u) : (cap == 1 ? 1 + S(r.U32() % 4u) : 16));
    for (uint32_t k = 0; k < 3; ++k)
        PutS32(m, pos + 4u * k, GetS32(m, e + 0xB8u + 4u * k) + (Chance(r, 3) ? S(r.U32() % 0x200000u) - 0x100000 : 0));
    if (Chance(r, 16)) PutS32(m, pos, S(r.U32()));
    const uint32_t rk = r.U32() % 6u;
    int32_t radius = S((m.PeekWord(e + 0x1E0u) + 0x141DDDu) * 2u);
    if (rk == 0) radius = S(r.U32() % 0x2000000u);
    else if (rk == 1) radius = S(r.U32());
    a.a[0] = list;
    a.a[1] = count;
    a.a[2] = pos;
    a.a[3] = U(radius);
    a.a4 = Chance(r, 2) ? 73u : r.U32() & 0xFFu;
    uint16_t eh = 0;
    m.ReadBlock(e + 0xACu, &eh, 2);
    a.more[0] = Chance(r, 4) ? r.U32() : eh;
}
uint32_t NativeQuery(NativeEnv& env, const Args& a) {
    rr::sim::GuestRam g = pop::View(env);
    rr::sim::AiSpatialQuery(g, a.a[0], a.a[1], a.a[2], S(a.a[3]), a.a4, a.more[0]);
    return pop::PopDone(env, g, true, "AiSpatialQuery");
}

// ---------------------------------------------------------------- 0x800BEBA4
void SetupProbe(CaseContext& c, Args& a) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    const uint32_t eIdx = r.U32() % 18u;
    const uint32_t e = pop::Bike(m, eIdx);
    DrawSubject(c, e);
    uint32_t code;
    const uint32_t pk = r.U32() % 10u;
    if (pk < 5) code = (eIdx + 1u + r.U32() % 17u) % 18u;
    else if (pk < 7) code = 0x60u | (r.U32() % 4u);
    else if (pk < 9) code = 0xC0u | (r.U32() % 4u);
    else code = 0x20u | (r.U32() % 18u);
    const uint32_t ha = HandleAddr(m, code >> 5, code & 31u);
    if (ha != 0) {
        const int32_t speed = GetS32(m, e + 0x1E0u);
        const int32_t reach = std::max(S((U(speed) + 0x141DDDu) * 2u), 0x10000);
        const int32_t ahead = Chance(r, 5) ? S(r.U32() % 0x1000000u) - 0x100000 : S(r.U32() % U(reach));
        const int32_t lat = Chance(r, 3) ? S(r.U32() % 0x60000u) - 0x30000 : S(r.U32() % 0x30000u) - 0x18000;
        PlaceAhead(c, e, ha, ahead, lat);
        DrawCandidate(c, e, code, ha);
    }
    // NAMED BOUND: the handle's pool is one the grid lists (0, 1, 3, 6); a drawn pool 2/4/5/7 makes the
    // original chase that pool's table into the BIOS, which the guest-address view does not map.
    a.a[0] = Chance(r, 16) ? (r.U32() & 0xFFFF0000u) | code : code;
    a.a[1] = e;
}
uint32_t NativeProbe(NativeEnv& env, const Args& a) {
    rr::sim::GuestRam g = pop::View(env);
    const int32_t v = rr::sim::AiBrainProbe(g, a.a[0], a.a[1]);
    return pop::PopDone(env, g, true, "AiBrainProbe", U(v));
}

// ---------------------------------------------------------------- 0x800BD4D4
void SetupBrain(CaseContext& c, Args& a) {
    Rng& r = *c.rng;
    const uint32_t eIdx = Chance(r, 10) ? 0u : 1u + r.U32() % 17u;
    DrawScene(c, eIdx, false);
    PlantStale(c, c.sp, pop::Bike(*c.mem, eIdx));
    a.a[0] = Chance(r, 64) ? 0u : pop::Bike(*c.mem, eIdx);
}
uint32_t NativeBrain(NativeEnv& env, const Args& a) {
    rr::sim::GuestRam g = pop::View(env);
    Seams s(env);
    int32_t v0 = 0;
    const bool ok = rr::sim::AiBrain(g, a.a[0], env.sp, s, v0);
    return pop::PopDone(env, g, ok, "AiBrain", U(v0));
}

// ---------------------------------------------------------------- 0x800BCD48
void SetupPass(CaseContext& c, Args& a) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    // two or three subjects with neighbourhoods; every bike's gate fields drawn
    const uint32_t focus = r.U32() % 18u;
    DrawScene(c, focus, true);
    for (uint32_t i = 0; i < 18; ++i) {
        const uint32_t e = pop::Bike(m, i);
        if (i != focus) {
            if (Chance(r, 2)) m.PokeWord(e + 560u, (m.PeekWord(e + 560u) & ~0x18000000u) | (Chance(r, 6) ? (r.U32() & 0x18000000u) : 0x08000000u));
            if (Chance(r, 2)) PutS32(m, e + 0x1E0u, 0x40000 + S(r.U32() % 0x400000u));
            if (Chance(r, 3)) {
                const uint32_t d = 1u + r.U32() % 4u;
                Put8(m, e + 946u, d);
                WriteU16Bench(m, e + 0x3B4u + 8u * d, static_cast<uint16_t>(Chance(r, 4) ? r.U32() % 20u : 3u + r.U32() % 15u));
            }
            if (Chance(r, 2)) m.PokeWord(e + 564u, m.PeekWord(e + 564u) ^ (r.U32() & 0x200u));
            if (Chance(r, 2)) PutS32(m, e + 704u, Chance(r, 3) ? S(r.U32() % 64u) : S(r.U32() % 0x10000u));
            if (Chance(r, 3)) PutS32(m, e + 676u, 2129 + S(r.U32() % 3u));
        }
        if (Chance(r, 6)) WriteU16Bench(m, e + 320u, 0);
        else if (Chance(r, 3)) WriteU16Bench(m, e + 320u, 1);
        const uint32_t rd = m.PeekWord(e + 1084u);
        if (InRam(rd) && Chance(r, 8)) Put8(m, rd, m.PeekByte(rd) ^ 1u);
    }
    if (Chance(r, 4)) m.PokeWord(rr::sim::kAiBrainLiveCount, r.U32() % 19u);
    PlantStale(c, c.sp - rr::sim::kAiBrainPassFrame, pop::Bike(m, focus));
    a.a[0] = Chance(r, 3) ? r.U32() % 0x10000u : 0x1000u + r.U32() % 0x800u;   // dt
    a.a[1] = Chance(r, 2) ? 0u : r.U32() & r.U32();                            // skip
    a.a[2] = Chance(r, 3) ? 0xFFFFFFFFu : r.U32();                             // maskC
}
uint32_t NativePass(NativeEnv& env, const Args& a) {
    rr::sim::GuestRam g = pop::View(env);
    Seams s(env);
    const bool ok = rr::sim::AiBrainPass(g, S(a.a[0]), a.a[1], a.a[2], env.sp, s);
    return pop::PopDone(env, g, ok, "AiBrainPass");
}

Row BrainRow(const char* name, const char* sig, uint32_t entry, std::function<void(CaseContext&, Args&)> setup,
             std::function<uint32_t(NativeEnv&, const Args&)> native, bool v0, bool seams) {
    Row r{name, sig, entry, std::move(setup), nullptr, v0, std::move(native)};
    r.maxSteps = 40'000'000;
    if (seams) r.oracleCallees = Callees();
    return r;
}
} // namespace aib

// The extent [entry, end) of each row's function, for `--coverage` (the address after the delay slot
// of its last `jr ra`, read out of our own listing).
const CoverageRange* AiBrainCoverage(const char* rowName) {
    struct Named {
        const char* name;
        CoverageRange range;
    };
    static const Named kRanges[] = {
        {"ai_spatial_query", {0x8008AE94, 0x8008B428}}, {"ai_brain_probe", {0x800BEBA4, 0x800BF0F8}},
        {"ai_brain", {0x800BD4D4, 0x800BE7D8}},         {"ai_brain_pass", {0x800BCD48, 0x800BCEEC}},
    };
    for (const Named& n : kRanges)
        if (std::strcmp(n.name, rowName) == 0) return &n.range;
    return nullptr;
}

void AddAiBrainRows(std::vector<Row>& rows) {
    rows.push_back(aib::BrainRow("ai_spatial_query",
                                 "void AiSpatialQuery(u16 *list, s32 *count, s32 pos[3], s32 radius, [sp+16] mask, [sp+20] handle)  [RASHCDG 0x8008AE94]",
                                 aib::kQueryFn, aib::SetupQuery, aib::NativeQuery, false, false));
    rows.push_back(aib::BrainRow("ai_brain_probe", "s32 AiBrainProbe(u32 handle, Bike *e)  [RASHCDG 0x800BEBA4]",
                                 aib::kProbeFn, aib::SetupProbe, aib::NativeProbe, true, false));
    rows.push_back(aib::BrainRow("ai_brain", "s32 AiBrain(Bike *e)  [RASHCDG 0x800BD4D4]", aib::kBrainFn,
                                 aib::SetupBrain, aib::NativeBrain, true, true));
    rows.push_back(aib::BrainRow("ai_brain_pass", "void AiBrainPass(s32 dt, u32 skip, u32 maskC)  [RASHCDG 0x800BCD48]",
                                 aib::kPassFn, aib::SetupPass, aib::NativePass, false, true));
}
