// Bench rows for the police scheduler (docs\formats\rules.md 9.2).
// Included by rows_traffic.inc inside verify_physics.cpp's anonymous namespace, AFTER
// rows_population.inc, so the pop:: helpers (PopSeams, View, PopDone, DrawPlayers, Bike) are visible.
//
// The port is src\game\sim\police.{h,cpp}. One row per function:
//   find_free_cop  RASHCDG 0x80095848  (a leaf, no seam)
//   cop_setup      RASHCDG 0x80094184  (no seam; Rand drawn natively)
//   police_share   RASHCDG 0x800A01CC  (no seam; DotLcm native)
//   police_sched   RASHCDG 0x8009E89C  (seams: the road walk SLUS 0x80012C1C and SLUS 0x8003C520 of
//                                       the other traffic sub-passes, and PopSeamCallees for the
//                                       release's ResetBike / placement / AI-stack callees)
//
// INPUT FAMILIES start from THIS machine (rr-race): the police switch 1, two dormant cop bikes (pool 0
// slots 16, 17, class 2), cops out 0 / available 2, the saved cop record valid (+19 = 1), the tables
// 0x800530FC (30.0 x 6), 0x80053114 (120.0 by bank), 0x80053120 (18, 17, 16, 10, 12, 14),
// 0x80053138 (18.0 / 29.0 / 33.0). Drawn over it: the switch, the race type (44, bits 1 and 0x10),
// the phase gs[+0x39] (3 = the forced arm), the bank 0..3 (bank 3 reads the next table's words, as
// the original does), the clock, cops out -1..3 and available 0..3, the record's valid byte and rider
// bytes, the field size 0x8005B1F8, the accumulators A / B one unit either side of their thresholds
// (and of half the threshold, and B negative), the players' finish / frozen / wanted bits, place,
// live words, rider mount state 0..4, speed either side of 0x165A11 and of the bank's cop speed,
// their road coordinate (along near either end of the road, direction flipped, now and then at one of
// its end nodes), the route record (on / off route), the cops' class, live word and +0x3A0 bit 4, the
// pool's high index, the LCG seed.
//
// NAMED BOUND 1: the player count is 0, 1 or 2 (pop::DrawPlayers). The scheduler keeps its two
// eligibility words at sp+24/+28 of its 80-byte frame and writes one per player; a third player would
// overwrite sp+32 (RoadEndNode's node word) and sp+36 (the leader flag) and, from the fifth on, its
// own saved registers. PoliceShare's own two words at sp+16/+20 of its 56-byte frame likewise.
// NAMED BOUND 2: a downed player's rider (mount state >= 3) is given a road coordinate on the
// player's own road (the bike's, the along moved by up to 200 units, the direction drawn +-1): the
// release walks from the RIDER's coordinate, and this machine's seated riders hold none.

namespace police {

using pop::Chance;
using pop::Put8;
using pop::U;

constexpr uint32_t kFindFreeCop = 0x80095848, kCopSetup = 0x80094184, kPoliceShare = 0x800A01CC,
                   kPoliceSched = 0x8009E89C;
constexpr uint32_t kRoadWalk = 0x80012C1C, kRoadLength = 0x8003C520;
constexpr uint32_t kCopSlotA = 16, kCopSlotB = 17;   // rr-race's two class-2 bikes

std::vector<OracleCallee> PoliceSeamCallees() {
    std::vector<OracleCallee> v = pop::PopSeamCallees();
    v.push_back(OracleCallee{kRoadWalk, 3});
    v.push_back(OracleCallee{kRoadLength, 1});
    return v;
}

// The scheduler's two seams at the port's exact sp; everything else through pop::PopSeams.
struct PoliceSeams final : rr::sim::PoliceCallees {
    pop::PopSeams& s;
    explicit PoliceSeams(pop::PopSeams& x) : s(x) {}
    bool RoadWalk(uint32_t from, uint32_t out, int32_t dist, uint32_t sp) override {
        return s.Seam(kRoadWalk, {from, out, U(dist)}, sp);
    }
    bool RoadLength(uint32_t cursor, uint32_t sp, uint32_t& v0) override {
        return s.Seam(kRoadLength, {cursor}, sp, &v0);
    }
};

uint32_t RiderDef(const Memory& m, uint32_t e) { return m.PeekWord(e + 1084); }

// A pool-0 bike made a (non-)candidate cop: class nibble, live word, +0x3A0 bit 4, handle.
void DrawCopBike(CaseContext& c, uint32_t e) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    const uint32_t rd = RiderDef(m, e);
    if (Chance(r, 3)) Put8(m, rd + 1, (m.PeekByte(rd + 1) & 0xF0u) | (Chance(r, 2) ? 2u : r.U32() % 16u));
    if (Chance(r, 4)) WriteU16Bench(m, e + 320, static_cast<uint16_t>(Chance(r, 2) ? 1u : r.U32()));
    if (Chance(r, 4)) Put8(m, e + 928, m.PeekByte(e + 928) ^ 0x10u);
    if (Chance(r, 10)) WriteU16Bench(m, e + 0xAC, static_cast<uint16_t>(r.U32() % 3u));
}

// ---------------------------------------------------------------- 0x80095848
void SetupFindFreeCop(CaseContext& c, Args&) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    pop::DrawPlayers(c);
    for (uint32_t s = 0; s < 18; ++s)
        if (s == kCopSlotA || s == kCopSlotB || Chance(r, 6)) DrawCopBike(c, pop::Bike(m, s));
    const uint32_t hiPtr = m.PeekWord(rr::sim::kPopPoolTable + 12);
    const uint64_t k = r.Next() % 8u;
    if (k == 0) m.PokeWord(hiPtr, 0xFFFFFFFFu);                      // an empty pool
    else if (k == 1) m.PokeWord(hiPtr, r.U32() % 18u);               // fewer slots scanned
    if (Chance(r, 40)) {                                             // a null pool: one slot, skipped
        m.PokeWord(rr::sim::kPopPoolTable, 0);
        m.PokeWord(hiPtr, 0);
    }
}
uint32_t NativeFindFreeCop(NativeEnv& env, const Args&) {
    rr::sim::GuestRam g = pop::View(env);
    const uint32_t v = rr::sim::FindFreeCop(g);
    return pop::PopDone(env, g, true, "FindFreeCop", v);
}

// ---------------------------------------------------------------- 0x80094184
// The saved record's valid byte and rider bytes, the bank (clamped by the function to 0..2; 3, a
// negative and a large one drawn), the old +0x26 put inside the draw's range so that the first draw
// repeats it one time in eight, the seed.
void DrawRecord(CaseContext& c) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    const uint32_t rec = rr::sim::kPoliceRecord;
    if (Chance(r, 4)) Put8(m, rec + 19, Chance(r, 2) ? 0u : r.U32() & 0xFFu);
    if (Chance(r, 3))
        for (uint32_t o = 8; o < 24; o += 4)
            if (o != 16) m.PokeWord(rec + o, r.U32());
            else m.PokeWord(rec + o, (m.PeekWord(rec + o) & 0xFF000000u) | (r.U32() & 0x00FFFFFFu));
}
int32_t DrawBank(Rng& r) {
    const uint64_t k = r.Next() % 12u;
    if (k == 0) return -1 - static_cast<int32_t>(r.U32() % 4u);
    if (k == 1) return 4 + static_cast<int32_t>(r.U32() % 60u);
    return static_cast<int32_t>(r.U32() % 4u);
}
void SetupCopSetup(CaseContext& c, Args& a) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    const uint32_t gs = m.PeekWord(kGameStatePtr);
    const uint32_t e = pop::Bike(m, Chance(r, 3) ? r.U32() % 18u : (Chance(r, 2) ? kCopSlotA : kCopSlotB));
    const uint32_t rd = RiderDef(m, e);
    if (Chance(r, 4)) Put8(m, rd + 1, (m.PeekByte(rd + 1) & 0xF0u) | (Chance(r, 2) ? 2u : r.U32() % 16u));
    DrawRecord(c);
    const int32_t bank = DrawBank(r);
    PutS32(m, gs + 60, bank);
    const int32_t cl = bank < 0 ? 0 : (bank > 2 ? 2 : bank);
    Put8(m, rd + 38, Chance(r, 4) ? r.U32() & 0xFFu : 66u + 8u * U(cl) + r.U32() % 8u);
    m.PokeWord(c.gp + kRandSeedGpOffset, r.U32());
    a.a[0] = Chance(r, 30) ? 0u : e;
}
uint32_t NativeCopSetup(NativeEnv& env, const Args& a) {
    rr::sim::GuestRam g = pop::View(env);
    rr::sim::CopSetup(g, a.a[0]);
    return pop::PopDone(env, g, true, "CopSetup");
}

// ---------------------------------------------------------------- 0x800A01CC
// The two players' bikes put at an exact octagonal distance either side of the share's limit (with
// bank 3 the limit is (18 >> 1) + 100 = 109 units; with the real banks it is 0x3C0000 + 100 and the
// test always passes - the original compares whole units with a 16.16 half-distance), the views'
// cameras near them and their +0x1BC rows drawn (so "in front" and the facing test go both ways).
void DrawShare(CaseContext& c) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    const uint32_t gs = m.PeekWord(kGameStatePtr);
    if (Chance(r, 5)) Put8(m, gs + 4, m.PeekByte(gs + 4) & ~0x10u);
    else Put8(m, gs + 4, m.PeekByte(gs + 4) | 0x10u);
    PutS32(m, gs + 60, Chance(r, 2) ? 3 : static_cast<int32_t>(r.U32() % 3u));
    const uint32_t p1 = m.PeekWord(kPlayerArr), p2 = m.PeekWord(kPlayer2Ptr);
    if (p2 >= 0x80000000u && p2 < 0x80200000u && p1 != p2) {
        int32_t dx, dz;
        pop::OffsetsAt(r, Chance(r, 2) ? 108 + static_cast<int32_t>(r.U32() % 3u) : static_cast<int32_t>(r.U32() % 400u), dx, dz);
        PutS32(m, p2 + 0xB8, static_cast<int32_t>((U(GetS16(m, p1 + 0xBA) + dx) << 16) | (r.U32() & 0xFFFFu)));
        PutS32(m, p2 + 0xC0, static_cast<int32_t>((U(GetS16(m, p1 + 0xC2) + dz) << 16) | (r.U32() & 0xFFFFu)));
        if (Chance(r, 2)) PutS32(m, p2 + 0xBC, GetS32(m, p1 + 0xBC) + (r.S32() >> 12));
    }
    for (uint32_t v = 0; v < 2; ++v) {
        const uint32_t view = rr::sim::kPopViewArray + rr::sim::kPopViewStride * v;
        const uint32_t near = m.PeekWord(kPlayerArr + 4u * v);
        if (near >= 0x80000000u && near < 0x80200000u && Chance(r, 2)) {
            for (uint32_t k = 0; k < 3; ++k)
                PutS32(m, view + 0xB8 + 4u * k, GetS32(m, near + 0xB8 + 4u * k) + (r.S32() >> (8 + r.U32() % 10u)));
        }
        if (Chance(r, 2))
            for (uint32_t k = 0; k < 3; ++k) PutS16(m, view + 0x1BC + 2u * k, static_cast<int16_t>(static_cast<int32_t>(r.U32() % 8193u) - 4096));
    }
}
void SetupPoliceShare(CaseContext& c, Args& a) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    pop::DrawPlayers(c);
    DrawShare(c);
    const uint32_t flags = c.scratch + 0x200;
    for (uint32_t k = 0; k < 2; ++k)
        m.PokeWord(flags + 4u * k, Chance(r, 5) ? 0u : (Chance(r, 3) ? r.U32() : 1u));
    a.a[0] = flags;
}
uint32_t NativePoliceShare(NativeEnv& env, const Args& a) {
    rr::sim::GuestRam g = pop::View(env);
    rr::sim::PoliceShare(g, a.a[0], env.sp);
    return pop::PopDone(env, g, true, "PoliceShare");
}

// ---------------------------------------------------------------- 0x8009E89C
// Player p's inputs to the two passes.
void DrawPolicePlayer(CaseContext& c, uint32_t p, int32_t acc) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    const uint32_t b = m.PeekWord(rr::sim::kPopPlayerBikes + 4u * p);
    if (b < 0x80000000u || b >= 0x80200000u) return;
    const uint32_t rd = RiderDef(m, b), R = m.PeekWord(b + 852);
    const uint32_t gs = m.PeekWord(kGameStatePtr);
    // pass 1
    if (Chance(r, 8)) m.PokeWord(rd + 40, Chance(r, 2) ? 0u : 1u);           // finished
    else m.PokeWord(rd + 40, 0);
    if (Chance(r, 10)) Put8(m, rd, m.PeekByte(rd) | 0x40u);                   // frozen
    else Put8(m, rd, m.PeekByte(rd) & ~0x40u);
    if (Chance(r, 3)) Put8(m, rd, m.PeekByte(rd) ^ 0x80u);                    // wanted
    if (Chance(r, 5)) WriteU16Bench(m, b + 320, static_cast<uint16_t>(Chance(r, 2) ? 0u : 1u));
    if (Chance(r, 3)) m.PokeWord(R + 604, r.U32() % 5u);
    if (Chance(r, 4)) WriteU16Bench(m, R + 320, static_cast<uint16_t>(r.U32() % 2u));
    // pass 2
    if (Chance(r, 3)) Put8(m, rd + 39, Chance(r, 3) ? 1u : r.U32() % 20u);   // the place
    {
        static const int32_t kSpd[4] = {0x165A11, 0x165A12, 0x165A10, 0x165A13};
        const uint64_t k = r.Next() % 4u;
        const uint32_t bank = m.PeekWord(gs + 60);
        if (k == 0) PutS32(m, b + 480, kSpd[r.U32() % 4u]);
        else if (k == 1 && bank < 16u)
            PutS32(m, b + 480, GetS32(m, rr::sim::kPoliceSpeedTab + 4u * bank) - 1 + static_cast<int32_t>(r.U32() % 3u));
        else if (k == 2) PutS32(m, b + 480, static_cast<int32_t>(r.U32() % 0x300000u));
    }
    // the road coordinate: road kept; along near either end, direction flipped, now and then a node
    {
        const pop::RaceGraph gr = pop::ReadGraph(m, c.gp);
        const uint32_t road = m.PeekWord(b + 360) & 0xFFFFu;
        if (Chance(r, 3)) {
            const int32_t a = Chance(r, 2) ? static_cast<int32_t>(r.U32() % 260u) : 1380 + static_cast<int32_t>(r.U32() % 260u);
            PutS32(m, b + 368, static_cast<int32_t>((U(a) << 16) | (r.U32() & 0xFFFFu)));
        }
        if (Chance(r, 3)) PutS32(m, b + 364, -GetS32(m, b + 364));
        if (Chance(r, 8) && gr.nRoads > 0 && road < static_cast<uint32_t>(gr.nRoads))
            m.PokeWord(b + 360, 0x10000u | (m.PeekWord(gr.roads + 16u * road + ((r.Next() & 1u) ? 8u : 12u)) & 0xFFFFu));
    }
    {   // on / off route
        const int32_t count = GetS16(m, rrt::kCount);
        const uint32_t recs = m.PeekWord(rrt::kRecords);
        if (count > 0 && recs >= 0x80000000u && Chance(r, 3))
            m.PokeWord(b + 428, recs + 120u * (r.U32() % static_cast<uint32_t>(count + 1)));
        if (Chance(r, 10)) m.PokeWord(b + 428, 0);
    }
    // NAMED BOUND 2: a downed rider's coordinate on the player's own road
    if (m.PeekWord(R + 604) >= 3u) {
        for (uint32_t k = 0; k < 12; k += 4) m.PokeWord(R + 360 + k, m.PeekWord(b + 360 + k));
        PutS32(m, R + 364, Chance(r, 2) ? 1 : -1);
        if ((m.PeekWord(R + 360) >> 16) == 0)
            PutS32(m, R + 368, GetS32(m, R + 368) + static_cast<int32_t>((r.U32() % 401u) << 16) - (200 << 16));
    }
    // the accumulators: one unit either side of the threshold / its half, or anything
    const uint32_t out = m.PeekWord(rr::sim::kPopCopsOut);
    const uint32_t row = m.PeekWord(gs + 60) + (static_cast<int32_t>(out) > 0 ? 3u : 0u);
    const int32_t thr = (row < 64u) ? GetS32(m, rr::sim::kPoliceThrTab + 4u * row) : 0x1E0000;
    auto around = [&](int32_t t) { return t - acc - 1 + static_cast<int32_t>(r.U32() % 3u); };
    {
        const uint64_t k = r.Next() % 4u;
        if (k == 0) PutS32(m, rr::sim::kPoliceAccA + 4u * p, around(thr));
        else if (k == 1) PutS32(m, rr::sim::kPoliceAccA + 4u * p, static_cast<int32_t>(r.U32() % 0x300000u));
        else if (k == 2) PutS32(m, rr::sim::kPoliceAccA + 4u * p, thr + static_cast<int32_t>(r.U32() % 0x10000u));
    }
    {
        const uint64_t k = r.Next() % 6u;
        const uint32_t at = rr::sim::kPoliceAccB + 4u * p;
        if (k == 0) PutS32(m, at, around(thr));
        else if (k == 1) PutS32(m, at, around(thr >> 1));
        else if (k == 2) PutS32(m, at, static_cast<int32_t>(0xFFFF0000u));
        else if (k == 3) PutS32(m, at, static_cast<int32_t>(r.U32() % 0x300000u));
        else if (k == 4) PutS32(m, at, thr + static_cast<int32_t>(r.U32() % 0x10000u));
    }
}

void SetupPoliceSched(CaseContext& c, Args& a) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    const uint32_t gs = m.PeekWord(kGameStatePtr);
    const uint32_t np = pop::DrawPlayers(c);
    if (Chance(r, 12)) m.PokeWord(rr::sim::kPoliceOn, 0);
    {
        const uint64_t k = r.Next() % 6u;                            // the race type
        if (k == 0) Put8(m, gs + 4, 44);
        else if (k == 1) Put8(m, gs + 4, r.U32() & 0xFFu);
        else if (k == 2) Put8(m, gs + 4, m.PeekByte(gs + 4) | 0x10u);
    }
    if (Chance(r, 3)) Put8(m, gs + 57, Chance(r, 2) ? 3u : r.U32() % 5u);
    PutS32(m, gs + 60, Chance(r, 6) ? 3 : static_cast<int32_t>(r.U32() % 3u));
    if (Chance(r, 3)) PutS32(m, gs + 16, static_cast<int32_t>(r.U32() % 0x8000000u));
    PutS32(m, rr::sim::kPopCopsOut, Chance(r, 2) ? 0 : static_cast<int32_t>(r.U32() % 5u) - 1);
    PutS32(m, rr::sim::kPoliceAvail, Chance(r, 2) ? 2 : static_cast<int32_t>(r.U32() % 4u));
    if (Chance(r, 3)) PutS32(m, rr::sim::kPoliceField, static_cast<int32_t>(r.U32() % 25u));
    DrawRecord(c);
    const int32_t acc = Chance(r, 6) ? static_cast<int32_t>(r.U32() % 0x40000u) : 0x10000 + static_cast<int32_t>(r.U32() % 0x1000u);
    for (uint32_t p = 0; p < 2; ++p) DrawPolicePlayer(c, p, acc);
    if (np == 2 && Chance(r, 2)) DrawShare(c);
    DrawCopBike(c, pop::Bike(m, kCopSlotA));
    DrawCopBike(c, pop::Bike(m, kCopSlotB));
    m.PokeWord(c.gp + kRandSeedGpOffset, r.U32());
    a.a[0] = U(acc);
}
uint32_t NativePoliceSched(NativeEnv& env, const Args& a) {
    rr::sim::GuestRam g = pop::View(env);
    pop::PopSeams s(env);
    PoliceSeams ps(s);
    const bool ok = rr::sim::PoliceSched(g, static_cast<int32_t>(a.a[0]), env.sp, ps, s);
    return pop::PopDone(env, g, ok, "PoliceSched");
}

Row PoliceRow(const char* name, const char* sig, uint32_t entry, std::function<void(CaseContext&, Args&)> setup,
              std::function<uint32_t(NativeEnv&, const Args&)> native, bool v0, bool seams) {
    Row r{name, sig, entry, std::move(setup), nullptr, v0, std::move(native)};
    r.maxSteps = 40'000'000;
    if (seams) r.oracleCallees = PoliceSeamCallees();
    return r;
}

} // namespace police

// The extent [entry, end) of each police row's function, for `--coverage` (the address after the
// delay slot of its last `jr ra`, from our own listing). verify_physics.cpp consults it as a
// `PoliceCoverage(row.name)` fallback beside PopulationCoverage.
const CoverageRange* PoliceCoverage(const char* rowName) {
    struct Named {
        const char* name;
        CoverageRange range;
    };
    static const Named kRanges[] = {
        {"find_free_cop", {0x80095848, 0x800958F0}}, {"cop_setup", {0x80094184, 0x8009432C}},
        {"police_share", {0x800A01CC, 0x800A04B0}},  {"police_sched", {0x8009E89C, 0x8009EF8C}},
    };
    for (const Named& n : kRanges)
        if (std::strcmp(n.name, rowName) == 0) return &n.range;
    return nullptr;
}

void AddPoliceRows(std::vector<Row>& rows) {
    rows.push_back(police::PoliceRow("find_free_cop", "Bike *FindFreeCop(void)  [RASHCDG 0x80095848]", police::kFindFreeCop,
                                     police::SetupFindFreeCop, police::NativeFindFreeCop, true, false));
    rows.push_back(police::PoliceRow("cop_setup", "void CopSetup(Bike *e)  [RASHCDG 0x80094184]", police::kCopSetup,
                                     police::SetupCopSetup, police::NativeCopSetup, false, false));
    rows.push_back(police::PoliceRow("police_share", "void PoliceShare(s32 flags[2])  [RASHCDG 0x800A01CC]",
                                     police::kPoliceShare, police::SetupPoliceShare, police::NativePoliceShare, false, false));
    rows.push_back(police::PoliceRow("police_sched", "void PoliceSched(s32 acc)  [RASHCDG 0x8009E89C]", police::kPoliceSched,
                                     police::SetupPoliceSched, police::NativePoliceSched, false, true));
}
