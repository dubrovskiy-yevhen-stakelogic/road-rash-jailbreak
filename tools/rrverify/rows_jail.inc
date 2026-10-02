// Bench rows for the Jailbreak mode and the two-seat bike (src\game\sim\jail.{h,cpp}; docs\formats\rules.md
// 9.6, 9.7, 16). Included LAST by verify_physics.cpp inside its anonymous namespace, after rows_modes.inc,
// whose helpers (and rows_population.inc's pop::View / PopDone / PopSeams / PopSeamCallees) these rows reuse.
//
// One row per function:
//   jail_escape_scene RASHCDG 0x800C9420 (the ported stance event native and witnessed, BuildObb and
//     SeatRelease native; seams: AxisRotation SLUS 0x8003FB34, Placement 0x8009432C, RoadGate SLUS
//     0x80039DFC, CursorSeat SLUS 0x8003A700, RoadUpdate SLUS 0x80037450, WeaponObject 0x800958F0,
//     GetRCnt SLUS 0x80043F00, CarSpawn 0x8009AD48, the roadblock 0x800A2630).
//
// INPUT FAMILIES start from THIS machine (rr-race: player 1 at slot 0, 18 bikes, the traffic pool). Drawn
// over it, per case: the block 0 / 1; a STARTJBA-shaped record array in the scratch block (1..14 records;
// the group nibble 0, 1, 3, 4 and the skipped 2 / 5..15; the road half either side of 17; angles of the
// whole 12-bit range; lateral / along around the file's); the anchor 0x80053178 on player 1's own road
// and along half the time (so the cursor and the placement arms run), else anywhere; player 1's direction
// +0x16C either sign (the in-place negation); the field's class nibbles 0 / 1 / 2; a partner +0x358 for
// player 1 and the bike group 1 lands on (the boarding bits), or none; the riders' seat byte +0x48 (1 =
// seated: SeatRelease), +0x23C bit 6, the first-bike word 0x8005B330 and the escape car 0x800CCC18.
//
// NAMED BOUNDS:
//   * the pool-1 count 0x8005B218 stays within pool 0 (<= 18): group 1 indexes POOL 0 by it (as the
//     original does, 0x800C9790) and a larger count reads past the 18 bikes the capture holds;
//   * the anchor's road is 0..63 (a road the map knows): the road gate walks the resident list only;
//   * group 0 walks pool 0 upward to the next bike of the other class than player 1's: a field with fewer
//     of them than group-0 records walks past the pool, and the ORIGINAL then writes through a rider record
//     read as a bike and traps (store into the BIOS image) - the classes alternate (one in eight flipped or
//     police) and a case holds at most 5 group-0 records (STARTJBA.BIN holds 6 over a 13-bike grid).
//   * player 1's class is 0 or 1 (Jailbreak has no player cop): with class 2 group 1's walk stops at once
//     (its start value 2 is the wanted class) and takes the bike at the pool-1 count, past the pool.

namespace jailr {

using pop::Chance;
using pop::Put8;
using pop::U;

constexpr uint32_t kBikes = 18;
constexpr uint32_t kPlacementFn = 0x8009432C, kRoadGateFn = 0x80039DFC, kCursorSeatFn = 0x8003A700,
                   kRoadUpdateFn = 0x80037450, kWeaponObjectFn = 0x800958F0, kGetRCntFn = 0x80043F00,
                   kCarSpawnFn = 0x8009AD48, kRoadblockFn = 0x800A2630, kPopCommandFn = 0x800BC8DC;

std::vector<OracleCallee> JailSeamCallees() {
    std::vector<OracleCallee> v = pop::PopSeamCallees();
    for (const OracleCallee& oc :
         {OracleCallee{kPlacementFn, 1}, OracleCallee{kRoadGateFn, 2}, OracleCallee{kCursorSeatFn, 3},
          OracleCallee{kRoadUpdateFn, 1}, OracleCallee{kWeaponObjectFn, 2}, OracleCallee{kGetRCntFn, 1},
          OracleCallee{kCarSpawnFn, 2}, OracleCallee{kRoadblockFn, 2}})
        v.push_back(oc);
    return v;
}

struct Seams final : rr::sim::EscapeCallees {
    NativeEnv& env;
    pop::PopSeams ps;
    explicit Seams(NativeEnv& e) : env(e), ps(e) {}
    bool SeatRelease(uint32_t b, uint32_t r, uint32_t idx, uint32_t sp) override { return ps.SeatRelease(b, r, idx, sp); }
    bool Placement(uint32_t e, uint32_t sp) override { return ps.Seam(kPlacementFn, {e}, sp); }
    bool RoadGate(uint32_t h, uint32_t key, uint32_t sp, uint32_t& v0) override {
        return ps.Seam(kRoadGateFn, {h, key}, sp, &v0);
    }
    bool CursorSeat(uint32_t obj, uint32_t key, uint32_t cursor, uint32_t sp, uint32_t& v0) override {
        return ps.Seam(kCursorSeatFn, {obj, key, cursor}, sp, &v0);
    }
    bool AxisRotation(uint32_t axis, int32_t ang, uint32_t out, uint32_t sp) override {
        return ps.AxisRotation(axis, ang, out, sp);
    }
    bool BuildObb(uint32_t e, uint32_t) override { return ps.BuildObb(e); }
    bool RoadUpdate(uint32_t e, uint32_t sp) override { return ps.Seam(kRoadUpdateFn, {e}, sp); }
    bool WeaponObject(uint32_t r, uint32_t side, uint32_t sp) override { return ps.Seam(kWeaponObjectFn, {r, side}, sp); }
    bool GetRCnt(uint32_t id, uint32_t sp, uint32_t& v0) override { return ps.Seam(kGetRCntFn, {id}, sp, &v0); }
    bool StanceEvent(uint32_t ev, uint32_t r, uint32_t p, uint32_t sp) override { return ps.StanceEvent(ev, r, p, sp); }
    bool CarSpawn(uint32_t rec, uint32_t a1, uint32_t sp, uint32_t& v0) override {
        return ps.Seam(kCarSpawnFn, {rec, a1}, sp, &v0);
    }
    bool Roadblock(uint32_t rec, uint32_t player, uint32_t sp) override { return ps.Seam(kRoadblockFn, {rec, player}, sp); }
};

uint32_t Bk(const Memory& m, uint32_t i) { return aip::BikeAt(m, i); }
uint32_t Rd(const Memory& m, uint32_t e) { return m.PeekWord(e + 1084); }
uint32_t Rider(const Memory& m, uint32_t e) { return m.PeekWord(e + 852); }

void SetupEscape(CaseContext& c, Args& a) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    const uint32_t gs = m.PeekWord(kGameStatePtr);
    Put8(m, gs + 4, 44);
    const uint32_t block = Chance(r, 2) ? 1u : 0u;
    const uint32_t p1 = m.PeekWord(0x8005B38C);
    // the anchor: player 1's own road and along (the resident arms), else anything
    const uint32_t road = aip::InRam(p1) ? (m.PeekWord(p1 + 360) & 0xFFFFu) : 0u;
    const uint32_t along = aip::InRam(p1) ? (m.PeekWord(p1 + 368) >> 16) : 0u;
    if (Chance(r, 2)) {
        WriteU16Bench(m, 0x80053178u, static_cast<uint16_t>(road));
        WriteU16Bench(m, 0x8005317Au, static_cast<uint16_t>(along + r.U32() % 41u - 20u));
    } else if (Chance(r, 2)) {
        WriteU16Bench(m, 0x80053178u, static_cast<uint16_t>(r.U32() % 64u));
        WriteU16Bench(m, 0x8005317Au, static_cast<uint16_t>(r.U32() % 3000u));
    }
    if (aip::InRam(p1)) {
        PutS32(m, p1 + 364, Chance(r, 2) ? 1 : -1);
        if (Chance(r, 3)) PutS32(m, p1 + 364, Chance(r, 2) ? 2 : -2);
    }
    // the field's classes: alternating, one in eight flipped or police (NAMED BOUND, header)
    for (uint32_t i = 0; i < kBikes; ++i) {
        const uint32_t rd = Rd(m, Bk(m, i));
        uint32_t cls = Chance(r, 8) ? (Chance(r, 2) ? 2u : ((i + 1u) & 1u)) : (i & 1u);
        if (Bk(m, i) == p1) cls = r.U32() % 2u;                         // NAMED BOUND: no player cop here
        if (aip::InRam(rd)) Put8(m, rd + 1, (m.PeekByte(rd + 1) & 0xF0u) | cls);
    }
    // the partner: player 1's +0x358 and the far end of pool 0 (where group 1's walk starts)
    for (uint32_t i = 0; i < kBikes; ++i) m.PokeWord(Bk(m, i) + 856, 0);
    if (aip::InRam(p1) && !Chance(r, 3)) {
        const uint32_t q = Bk(m, kBikes - 1u - r.U32() % 3u);
        m.PokeWord(p1 + 856, q);
        m.PokeWord(q + 856, p1);
    }
    // the riders' seat byte and bit 6 of +0x23C
    for (uint32_t i = 0; i < kBikes; ++i) {
        const uint32_t R = Rider(m, Bk(m, i));
        if (!aip::InRam(R)) continue;
        if (Chance(r, 2)) Put8(m, R + 72, Chance(r, 2) ? 1u : 0u);
        if (Chance(r, 3)) Put8(m, R + 572, m.PeekByte(R + 572) ^ 0x40u);
    }
    // NAMED BOUND: the pool-1 count indexes pool 0
    WriteU16Bench(m, 0x8005B218u, static_cast<uint16_t>(Chance(r, 4) ? r.U32() % 19u : kBikes));
    m.PokeWord(0x8005B330u, Chance(r, 2) ? 0u : Bk(m, 3));
    m.PokeWord(0x800CCC18u, r.U32());
    // the record array, STARTJBA-shaped, in the scratch block
    const uint32_t n = 1u + r.U32() % 14u;
    const uint32_t arr = c.scratch + 0x400u;
    static const uint32_t kGroups[] = {0, 0, 1, 1, 1, 3, 3, 4, 2, 5};
    uint32_t g0 = 0;
    for (uint32_t k = 0; k < n; ++k) {
        uint32_t grp = Chance(r, 12) ? r.U32() % 16u : kGroups[r.U32() % 10u];
        if (grp == 0u && ++g0 > 5u) grp = 1u;                           // NAMED BOUND (header)
        const uint32_t half = Chance(r, 2) ? 17u + r.U32() % 4u : r.U32() % 17u;
        const uint32_t ang = Chance(r, 3) ? (Chance(r, 2) ? 0xFE3u : 0x01Du) : r.U32() & 0xFFFu;
        m.PokeWord(arr + 12u * k + 8u, (half << 16) | (grp << 12) | ang);
        PutS32(m, arr + 12u * k + 0u, static_cast<int32_t>(r.U32() % 0xA0000u) - 0x50000);
        PutS32(m, arr + 12u * k + 4u, static_cast<int32_t>(r.U32() % 0x1400000u) - 0xA00000);
    }
    PutS32(m, rr::sim::kJailBlockCount + 4u * block, static_cast<int32_t>(n));
    m.PokeWord(rr::sim::kJailBlockArray + 4u * block, arr);
    if (Chance(r, 10)) PutS32(m, rr::sim::kJailBlockCount + 4u * block, 0);
    a.a[0] = block;
}

uint32_t NativeEscape(NativeEnv& env, const Args& a) {
    rr::sim::GuestRam g = pop::View(env);
    Seams s(env);
    const bool ok = rr::sim::EscapeScene(g, static_cast<int32_t>(a.a[0]), env.sp, s);
    return pop::PopDone(env, g, ok, "EscapeScene");
}

} // namespace jailr

void AddJailBoardRows(std::vector<Row>& rows); // below

void AddJailRows(std::vector<Row>& rows) {
    using namespace jailr;
    Row r{"jail_escape_scene", "void EscapeScene(s32 block)  [RASHCDG 0x800C9420]", rr::sim::kJailEscapeSceneFn,
          SetupEscape, nullptr, false, NativeEscape};
    r.maxSteps = 60'000'000;
    r.oracleCallees = JailSeamCallees();
    rows.push_back(std::move(r));
    AddJailBoardRows(rows);
}

// ---------------------------------------------------------------------------- the jail stop and the boarding
//   jail_finish RASHCDG 0x800C9E74 (JailBoard run natively under it), jail_board RASHCDG 0x800CA05C (seam:
//   AiPopCommand 0x800BC8DC; AiPushCommand 0x800BCA68 native, the stance event under it native and
//   witnessed). INPUT FAMILIES over the same machine: the phase gs+0x39 1 / 2 (and 0, 3); the
//   stop offsets *(s8 0x8005ADF2 / 0x8005ADF3); player 1's along +0x170 against the milestone's so the
//   distance falls in each band (beyond 40.0, 10.0..40.0, at the stop, either side of each edge); rd+0x45 and
//   stats+0xE0; +0x234 bit 19; the speed +0x1E0 either side of 0x7FFF; a partner +0x358 whose top command is
//   0 or not; guards - bikes of the other class, live or not, their riders in stance 64 half the time - at
//   distances either side of 4.0 from player 1's rider and facing it or not; the pace words 0x8005ADE8 /
//   0x8005ADEC. NAMED BOUND: player 1 has a partner (+0x358): JailbreakFinish's phase-2 arm and JailBoard(0)
//   read it with no null test, as every two-seat path does.
namespace jailr {

struct BoardSeams final : rr::sim::JailCallees {
    pop::PopSeams ps;
    explicit BoardSeams(NativeEnv& e) : ps(e) {}
    bool PopCommand(uint32_t e, uint32_t sp) override { return ps.Seam(kPopCommandFn, {e}, sp); }
    bool PushCommand(uint32_t cmd, int32_t mode, uint32_t e, uint32_t sp) override { return ps.PushCommand(cmd, mode, e, sp); }
};

void DrawJailStop(CaseContext& c, bool finish) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    const uint32_t gs = m.PeekWord(kGameStatePtr);
    Put8(m, gs + 4, 44);
    static const uint8_t kPhase[] = {1, 2, 2, 1, 0, 3};
    Put8(m, gs + 57, kPhase[r.U32() % 6u]);
    Put8(m, 0x8005ADF2u, Chance(r, 2) ? 0u : r.U32() % 32u);
    Put8(m, 0x8005ADF3u, Chance(r, 2) ? 0u : r.U32());
    const uint32_t p1 = m.PeekWord(0x8005B38C);
    if (!aip::InRam(p1)) return;
    const uint32_t ph = m.PeekByte(gs + 57);
    const uint32_t ms = 0x80053174u + 4u * (ph - 1u);
    const int32_t stop = (static_cast<int32_t>(static_cast<int8_t>(m.PeekByte(0x8005ADF2u))) << 16) +
                         (static_cast<int32_t>(aip::Get16(m, ms + 2)) << 16);
    static const int32_t kGap[] = {0xA0000, 0x280000, 0, 0x100000, 0x400000, -0x20000};
    const int32_t gap = copr::Around(r, kGap[r.U32() % 6u]);
    PutS32(m, p1 + 368, Chance(r, 8) ? r.S32() : stop - gap);
    const uint32_t rd = Rd(m, p1);
    if (aip::InRam(rd) && Chance(r, 2)) Put8(m, rd + 69, r.U32());
    const uint32_t st = m.PeekWord(p1 + 556);
    if (aip::InRam(st) && Chance(r, 3)) PutS32(m, st + 224, static_cast<int32_t>(r.U32() % 0x400000u) - (Chance(r, 8) ? 0x200000 : 0));
    if (Chance(r, 2)) m.PokeWord(p1 + 564, m.PeekWord(p1 + 564) ^ 0x80000u);
    PutS32(m, p1 + 480, Chance(r, 2) ? static_cast<int32_t>(r.U32() % 0x10000u) : copr::Speed(r));
    // the partner (NAMED BOUND) and its top command
    const uint32_t q = Bk(m, kBikes - 1u - r.U32() % 3u);
    m.PokeWord(p1 + 856, q);
    const int32_t depth = 1 + static_cast<int32_t>(r.U32() % 3u);
    Put8(m, q + 946, static_cast<uint32_t>(depth));
    WriteU16Bench(m, q + 956u + 8u * static_cast<uint32_t>(depth - 1), static_cast<uint16_t>(Chance(r, 2) ? 0u : r.U32() % 20u));
    // the guards
    const uint32_t pcls = aip::InRam(rd) ? (m.PeekByte(rd + 1) & 0xFu) : 0u;
    for (uint32_t i = 1; i < kBikes; ++i) {
        const uint32_t b = Bk(m, i);
        const uint32_t brd = Rd(m, b), R = Rider(m, b);
        if (!aip::InRam(brd) || !aip::InRam(R)) continue;
        if (Chance(r, 2)) Put8(m, brd + 1, (m.PeekByte(brd + 1) & 0xF0u) | (Chance(r, 6) ? 2u : (pcls ^ 1u) & 1u));
        if (Chance(r, 2)) WriteU16Bench(m, R + 544, 64);
        if (Chance(r, 6)) WriteU16Bench(m, b + 320, 0);
        if (!finish && Chance(r, 2)) {                                    // near player 1's rider or not
            const uint32_t pr = Rider(m, p1);
            if (aip::InRam(pr))
                for (uint32_t k = 0; k < 3; ++k)
                    PutS32(m, R + 184 + 4 * k, GetS32(m, pr + 184 + 4 * k) + static_cast<int32_t>(r.U32() % 0xA0000u) - 0x50000);
        }
    }
    PutS32(m, 0x8005ADE8u, copr::Around(r, 0x30000));
    PutS32(m, 0x8005ADECu, copr::Around(r, 0x60000));
}

void SetupFinish(CaseContext& c, Args& a) {
    DrawJailStop(c, true);
    a.a[0] = 0;
}
uint32_t NativeFinish(NativeEnv& env, const Args&) {
    rr::sim::GuestRam g = pop::View(env);
    BoardSeams s(env);
    const rr::sim::BikeTables t = rcv::Tables(env, g);
    const bool ok = rr::sim::JailbreakFinish(g, env.sp, t, s);
    return pop::PopDone(env, g, ok, "JailbreakFinish");
}
void SetupBoard(CaseContext& c, Args& a) {
    DrawJailStop(c, false);
    a.a[0] = Chance(*c.rng, 2) ? 1u : 0u;
}
uint32_t NativeBoard(NativeEnv& env, const Args& a) {
    rr::sim::GuestRam g = pop::View(env);
    BoardSeams s(env);
    const rr::sim::BikeTables t = rcv::Tables(env, g);
    const bool ok = rr::sim::JailBoard(g, a.a[0], env.sp, t, s);
    return pop::PopDone(env, g, ok, "JailBoard");
}

} // namespace jailr

void AddJailBoardRows(std::vector<Row>& rows) {
    using namespace jailr;
    Row f{"jail_finish", "void JailbreakFinish(void)  [RASHCDG 0x800C9E74]", rr::sim::kJailFinishFn, SetupFinish, nullptr,
          false, NativeFinish};
    f.maxSteps = 20'000'000;
    f.oracleCallees = pop::PopSeamCallees();
    f.oracleCallees.push_back(OracleCallee{kPopCommandFn, 1});
    rows.push_back(std::move(f));
    Row b{"jail_board", "void JailBoard(s32 late)  [RASHCDG 0x800CA05C]", rr::sim::kJailBoardFn, SetupBoard, nullptr,
          false, NativeBoard};
    b.maxSteps = 20'000'000;
    b.oracleCallees = pop::PopSeamCallees();
    b.oracleCallees.push_back(OracleCallee{kPopCommandFn, 1});
    rows.push_back(std::move(b));
}
