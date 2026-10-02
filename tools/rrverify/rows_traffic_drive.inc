// Traffic sub-pass 'drive' (rows_traffic.inc includes this file): the per-frame traffic pass RASHCDG
// 0x8009A298 and the callees it reaches, src\game\sim\traffic_drive.{h,cpp}. One row per function.
//
// SEAMS (bs::ExactOracle at the port's exact stack pointer, every declaration frame 0, the call
// sequences compared): the population's (pop::TrafficSeamCallees: the release 0x8008C000 - the
// traffic_bind sub-pass's -, NodeLanes SLUS 0x8003E1E8 under LaneLateral for a car at a node, ...),
// SLUS 0x8003C520 (the cursor's road end, the traffic_leaves sub-pass's) and PlaySound3D SLUS
// 0x80017BA0 (ported in sound.h; here run by the oracle on the clone). BuildObb runs natively
// (bike.h, through pop::PopSeams), the road layer natively (RoadRuntimeNative).
//
// INPUT FAMILIES start from THIS machine (rr-race: one player, 18 bikes, the car in pool-3 slot 0,
// handle 0x60, lane 2 on road 9) and plant on top of it:
//   * cars: copies of slot 0 in free slots 1..15, re-seated on a drawn slice of their sub-object
//     (first, last - at its ends, for the dead-end and road-end arms - or any) at a drawn along
//     within it, the road coordinate recomputed by the ported RoadPosition, the box put on the
//     slice at the lateral; direction (with its heading and rows), lane (+-1..3, 0, 4), speed at
//     and one either side of 131 / 132 / 0x8F0D8 / 0x141DDD, acceleration, the stop timer, the flag
//     bytes +0x1FD / +0x1FE, class (0, 15, other), the bike index +0x1FF; the pool control's live
//     count and high index; every planted car entered into the proximity grid at its box;
//   * blockers: a car planted a few units ahead of another in its lane, or a bike (a finished one
//     now and then) moved onto a car's road ahead of it and entered into the grid;
//   * players: the player's bike put on a car's road key heading the other way just past it (the
//     pass warning), or 40..80 behind / ahead of it (the lane change); speeds around 40.0; the views'
//     cameras at octagonal distances around 160 from a car;
//   * the race: type bits 1 / 0x10, phase 0..3, bank 0..2, the clock against the warning clock
//     gp+1936 around 76, the finish along / direction, the lane-change accumulator around 2.0, the LCG
//     seed.
// NAMED BOUNDS (each keeps the family inside what the ORIGINAL survives; none bends a port):
//   1. The player count is 0, 1 or 2 (pop::DrawPlayers): 0x8009F6F8 and 0x800A04B0 keep one word per
//      player in their own 80- / 8-byte frames.
//   2. The road step's distance is below 4096.0 in magnitude, and the port refuses after 2^20 slice
//      stretches: a walk over zero-length slices of a cyclic graph never ends in the original.
//   3. The grid's chains end (128) within 2^20 links: a cyclic chain loops the original forever. The
//      families only prepend fresh link entries to real chains.
//   Bounds 2 and 3 are precautions: neither refusal was reached on the gate seed or on seed 2.
// UNREACHABLE (so `--coverage` cannot reach them from any family): the pass's 0x8009A8E0..8E8 (a
// speed both >= 132 and <= 0) and 0x8009AAA8 (a blocker returned without bit 4); the blocker's
// pool-6 arm 0x800BE8C4..8E8 (its grid mask 59 excludes pool 6) and the sign arms of its two
// divisions that a speed >= 132 / a gap >= 3276 exclude (0x800BE944..948, 968..96C, 9A8..9B4,
// 9D4..9DC); the gap's s1 <= 0 arms 0x800BF3D0 and 0x800BF3F4..400 (s1 >= 132 there).

namespace tdrive {
using pop::Chance;
using pop::Put8;
using pop::U;

constexpr uint32_t kPass = 0x8009A298, kRoadStep = 0x8003775C, kGridQuery = 0x8008AE94, kCarAhead = 0x8009BA5C,
                   kLanePlayer = 0x800A04B0, kPlayerClose = 0x800A05D4, kCarBrake = 0x8009B4C0,
                   kLaneChange = 0x8009B728, kGiveUp = 0x8009F6F8, kHorn = 0x8009F9EC, kWarnSound = 0x80017CF0,
                   kPassWarn = 0x8009FCDC, kBlockerGap = 0x800BF0F8, kBlocker = 0x800BE7D8;
constexpr uint32_t kRoadLen = 0x8003C520, kSound3D = 0x80017BA0;
// Scratch-block layout (8 KiB; pop:: uses 0x100..0x244 and 0x400..0x43F, rrt:: 0x600..0xDFC,
// tleaf:: 0xE00..0xF9F, rq 4096.. and 8000..).
constexpr uint32_t kList = 0x480;   // GridQuery's output list, 32 bytes
constexpr uint32_t kCount = 0x4A0;  // its count word
constexpr uint32_t kPoint = 0x4B0;  // a drawn query point, 12 bytes
constexpr uint32_t kLinkEntries = 128; // 0x800CCFA8 .. : entry 128 is the chain end

uint32_t Car(uint32_t slot) { return pop::kCar0 + 512u * slot; }
bool InRam(uint32_t a) { return a >= 0x80000000u && a < 0x80200000u; }

// ---------------------------------------------------------------- the seams
struct TdSeams final : rr::sim::TrafficDriveCallees {
    pop::PopSeams ps;
    explicit TdSeams(NativeEnv& e) : ps(e) {}
    rr::sim::PopulationCallees& Population() override { return ps; }
    bool RoadLength(uint32_t cursor, uint32_t sp, uint32_t& v0) override { return ps.Seam(kRoadLen, {cursor}, sp, &v0); }
    bool PlaySound3D(int32_t x, int32_t z, int32_t id, int32_t bank, uint32_t sp) override {
        return ps.Seam(kSound3D, {U(x), U(z), U(id), U(bank)}, sp);
    }
};

std::vector<OracleCallee> Seams() {
    std::vector<OracleCallee> v = pop::TrafficSeamCallees();
    v.push_back(OracleCallee{kRoadLen, 1});
    v.push_back(OracleCallee{kSound3D, 4});
    return v;
}

// ---------------------------------------------------------------- planting
// Car `e` re-seated on slice `idx` of its sub-object at `inSlice`: the road coordinate by the ported
// RoadPosition, the box on the slice at the car's lateral (as the spawner puts it).
void Seat(CaseContext& c, uint32_t e, int32_t idx, int32_t inSlice) {
    Memory& m = *c.mem;
    const uint32_t sl = m.PeekWord(e + 340), sub = m.PeekWord(e + 336);
    if (!InRam(sl) || !InRam(sub)) return;
    const int32_t n = GetS16(m, sub + 10), cur = GetS16(m, sl);
    if (n <= 0 || cur < 0 || cur >= n) return;
    if (idx < 0) idx = 0;
    if (idx >= n) idx = n - 1;
    const uint32_t ns = sl + 52u * U(idx - cur);
    m.PokeWord(e + 340, ns);
    PutS32(m, e + 348, inSlice);
    rr::sim::GuestRam g(m.ram().data(), c.gp);
    rr::sim::RoadPosition(g, e + 450, e + 328, e + 360, c.sp - 2048u);
    rr::sim::MulAddView(g, ns + 20, ns + 14, inSlice, e + 184);
    rr::sim::MulAddView(g, e + 184, ns + 2, GetS32(m, e + 344), e + 184);
}

// A drawn place on the car's own sub-object: its first or last slice at an end, or anywhere.
void DrawPlace(CaseContext& c, uint32_t e) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    const uint32_t sl = m.PeekWord(e + 340), sub = m.PeekWord(e + 336);
    if (!InRam(sl) || !InRam(sub)) return;
    const int32_t n = GetS16(m, sub + 10);
    if (n <= 0) return;
    int32_t idx;
    switch (r.U32() % 4u) {
        case 0: idx = 0; break;
        case 1: idx = n - 1; break;
        default: idx = static_cast<int32_t>(r.U32() % static_cast<uint32_t>(n)); break;
    }
    const uint32_t ns = sl + 52u * U(idx - GetS16(m, sl));
    const int32_t len = GetS32(m, ns + 32);
    int32_t at;
    switch (r.U32() % 5u) {
        case 0: at = 0; break;
        case 1: at = len; break;
        case 2: at = len - static_cast<int32_t>(r.U32() % 0x20000u); break;
        default: at = static_cast<int32_t>(r.U32() % static_cast<uint32_t>(len > 0 ? len : 1)); break;
    }
    Seat(c, e, idx, at);
}

// The proximity grid's free link entries (not reachable from any cell of either grid).
void GridInsert(CaseContext& c, uint32_t handleByte, int32_t x, int32_t z) {
    Memory& m = *c.mem;
    bool used[kLinkEntries] = {};
    for (uint32_t cell = 0; cell < 2u * 24u * 24u; ++cell) {
        uint32_t t = m.PeekByte(rr::sim::kTdGridCells + cell);
        for (uint32_t k = 0; t < kLinkEntries && k < kLinkEntries; ++k) {
            used[t] = true;
            t = m.PeekByte(rr::sim::kTdGridLinks + 2u * t);
        }
    }
    uint32_t f = kLinkEntries;
    for (uint32_t i = 0; i < kLinkEntries; ++i)
        if (!used[i]) { f = i; break; }
    if (f == kLinkEntries) return;
    const int32_t gx = (static_cast<int32_t>(U(x) - m.PeekWord(rr::sim::kTdGridOriginX)) >> 21) + 11;
    const int32_t gz = (static_cast<int32_t>(U(z) - m.PeekWord(rr::sim::kTdGridOriginZ)) >> 21) + 11;
    if (gx < 0 || gx >= 24 || gz < 0 || gz >= 24) return;
    const uint32_t cell = rr::sim::kTdGridCells + U(gx) + 24u * U(gz);
    Put8(m, rr::sim::kTdGridLinks + 2u * f, m.PeekByte(cell));
    Put8(m, rr::sim::kTdGridLinks + 2u * f + 1u, handleByte & 0xFFu);
    Put8(m, cell, f);
}

// Direction flipped with the heading and the rows (a car the other way).
void FlipDir(Memory& m, uint32_t e) {
    PutS32(m, e + 364, -GetS32(m, e + 364));
    for (uint32_t o : {432u, 434u, 436u, 444u, 446u, 448u, 450u, 452u, 454u}) PutS16(m, e + o, static_cast<int16_t>(-GetS16(m, e + o)));
}

// The road coordinate alone (the cursor kept) put within 20.0 of one end of the car's road, facing
// it (the brake's end-node arm), or onto one of the road's end nodes (a car at a node).
void KeyNearEnd(CaseContext& c, uint32_t e) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    const pop::RaceGraph gr = pop::ReadGraph(m, c.gp);
    const uint32_t road = m.PeekWord(e + 360) & 0xFFFFu;
    if (gr.nRoads <= 0 || road >= static_cast<uint32_t>(gr.nRoads)) return;
    const uint32_t rrec = gr.roads + 16u * road;
    if (Chance(r, 4)) {
        const uint32_t kind = Chance(r, 4) ? 2u + r.U32() % 2u : 1u;
        m.PokeWord(e + 360, (kind << 16) | (m.PeekWord(rrec + (Chance(r, 2) ? 8u : 12u)) & 0xFFFFu));
        return;
    }
    const int32_t len = static_cast<int32_t>((m.PeekWord(rrec + 4) >> 6) << 16);
    const int32_t d = static_cast<int32_t>(r.U32() % 0x160000u) - 0x20000;
    if (Chance(r, 2)) {
        PutS32(m, e + 368, d);
        if (GetS32(m, e + 364) > 0) FlipDir(m, e);
    } else {
        PutS32(m, e + 368, len - d);
        if (GetS32(m, e + 364) < 0) FlipDir(m, e);
    }
}

// A planted car's drawn state.
void DrawCarFields(CaseContext& c, uint32_t e) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    if (Chance(r, 3)) FlipDir(m, e);
    static const int8_t kLane[9] = {1, 2, 3, -1, -2, -3, 0, 4, 2};
    if (Chance(r, 2)) Put8(m, e + 508, Chance(r, 10) ? r.U32() : static_cast<uint32_t>(static_cast<uint8_t>(kLane[r.U32() % 9u])));
    if (Chance(r, 3)) PutS32(m, e + 344, static_cast<int32_t>(r.U32() % 0xC0000u) - 0x60000);
    DrawPlace(c, e);
    static const int32_t kSpd[12] = {0, 130, 131, 132, 0x8F0D7, 0x8F0D9, 0x141DDC, 0x141DDE, 0x26666, 0x100000,
                                     0xB60000, 0x2000000};   // the last two: v * v overflows FixMul's sign
    if (!Chance(r, 4)) PutS32(m, e + 480, Chance(r, 3) ? static_cast<int32_t>(r.U32() % 0x200000u) : kSpd[r.U32() % 12u]);
    static const int32_t kAcc[5] = {0x26666, -0x26666, 0, 0x10000, -0x1000};
    if (Chance(r, 2)) PutS32(m, e + 484, kAcc[r.U32() % 5u]);
    static const int32_t kTimer[5] = {-0x10000, 0, 0x1FFFF - 0x884, 0x1FFFF, 0x10000};
    if (Chance(r, 2)) PutS32(m, e + 504, kTimer[r.U32() % 5u]);
    if (Chance(r, 2)) Put8(m, e + 509, r.U32() & 0xFBu);
    if (Chance(r, 3)) Put8(m, e + 510, r.U32() & 3u);
    static const uint32_t kCls[5] = {0u, 15u, 1u, 3u, 7u};
    if (Chance(r, 3)) m.PokeWord(e + 180, kCls[r.U32() % 5u]);
    if (Chance(r, 3)) Put8(m, e + 511, Chance(r, 4) ? 255u : r.U32() % 20u);
    if (Chance(r, 8)) PutS32(m, e + 304, static_cast<int32_t>(r.U32() % 0x60000u) - 0x10000);   // half-width
    if (Chance(r, 8)) PutS32(m, e + 308, static_cast<int32_t>(r.U32() % 0x80000u) - 0x20000);   // half-length
    if (Chance(r, 5)) KeyNearEnd(c, e);
}

// A pool-3 slot entered as a live car copied from slot 0 (the pool control follows).
uint32_t PlantCar(CaseContext& c, uint32_t slot) {
    Memory& m = *c.mem;
    const uint32_t e = Car(slot);
    uint8_t buf[512];
    m.ReadBlock(Car(0), buf, 512);
    m.WriteBlock(e, buf, 512);
    WriteU16Bench(m, e + 172, static_cast<uint16_t>(96u + slot));
    WriteU16Bench(m, e + 320, 1);
    return e;
}

// This machine's traffic with planted cars, blockers, players and race state around them.
// Returns the cars (live ones first) for the rows that take one.
std::vector<uint32_t> DrawWorld(CaseContext& c) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    pop::DrawPlayers(c);
    const uint32_t gs = m.PeekWord(kGameStatePtr);
    // the race
    if (Chance(r, 3)) Put8(m, gs + 4, (m.PeekByte(gs + 4) & ~0x11u) | (r.U32() & 0x11u));
    if (Chance(r, 3)) Put8(m, gs + 57, r.U32() % 4u);
    if (Chance(r, 3)) m.PokeWord(gs + 60, r.U32() % 3u);
    if (Chance(r, 2)) {
        const int32_t clock = GetS32(m, gs + 12);
        static const int32_t kGap[6] = {75, 76, -75, -76, 0, 1000};
        PutS32(m, c.gp + rr::sim::kTdWarnClockGp, clock - kGap[r.U32() % 6u]);
    }
    if (Chance(r, 4)) m.PokeWord(c.gp + rr::sim::kTdWarnPosGp, 0);
    if (Chance(r, 3)) PutS32(m, rr::sim::kTdFinishDir, Chance(r, 2) ? 1 : -1);
    static const int32_t kLt[4] = {0x20000 - 0x884, 0x20000 - 0x883, 0x1F000, 0};
    PutS32(m, rr::sim::kTdLaneTimer, Chance(r, 3) ? static_cast<int32_t>(r.U32() % 0x30000u) : kLt[r.U32() % 4u]);
    m.PokeWord(c.gp + kRandSeedGpOffset, r.U32());
    // the cars
    std::vector<uint32_t> cars;
    const bool live0 = (m.PeekWord(Car(0) + 172) & 0xFFFFu) != 0;
    uint32_t high = 0;
    if (live0) cars.push_back(Car(0));
    const uint32_t extra = live0 ? r.U32() % 6u : 0u;
    bool usedSlot[16] = {true};
    for (uint32_t k = 0; k < extra; ++k) {
        const uint32_t slot = 1u + r.U32() % 15u;
        if (usedSlot[slot]) continue;
        usedSlot[slot] = true;
        const uint32_t e = PlantCar(c, slot);
        DrawCarFields(c, e);
        if (Chance(r, 3)) {                                          // a blocker: just ahead in its lane
            const int32_t dir = GetS32(m, e + 364);
            rr::sim::GuestRam g(m.ram().data(), c.gp);
            const int32_t d = static_cast<int32_t>(2u + r.U32() % 40u) << 16;
            rr::sim::MulAddView(g, Car(0) + 184, Car(0) + 444, dir > 0 ? d : -d, e + 184);
            m.PokeWord(e + 360, m.PeekWord(Car(0) + 360));
            PutS32(m, e + 368, GetS32(m, Car(0) + 368) + (GetS32(m, Car(0) + 364) > 0 ? d : -d));
        }
        if (Chance(r, 8)) WriteU16Bench(m, e + 320, 0);              // dormant: CarCheck releases it
        GridInsert(c, m.PeekWord(e + 172) & 0xFFu, GetS32(m, e + 184), GetS32(m, e + 192));
        cars.push_back(e);
        high = slot;
    }
    if (!cars.empty()) {
        for (uint32_t s = 0; s < 16; ++s) if (usedSlot[s] && s > high) high = s;
        m.PokeWord(rr::sim::kPopPool3Ctrl, static_cast<uint32_t>(cars.size()));
        const uint32_t hp = m.PeekWord(rr::sim::kTdPool3Table + 12);
        if (InRam(hp)) m.PokeWord(hp, high);
    }
    if (live0 && Chance(r, 3)) DrawCarFields(c, Car(0));
    if (cars.empty()) return cars;
    std::swap(cars[0], cars[r.U32() % cars.size()]);                 // the focus car
    const uint32_t fc = cars[0];
    if (Chance(r, 6)) {                                              // parked in phase 1 / 2
        Put8(m, gs + 57, 1u + r.U32() % 2u);
        m.PokeWord(fc + 180, Chance(r, 2) ? 0u : 15u);
        PutS32(m, fc + 480, static_cast<int32_t>(r.U32() % 132u));
        PutS32(m, fc + 484, Chance(r, 2) ? 0 : static_cast<int32_t>(r.U32() % 132u));
    }
    if (Chance(r, 8)) {                                              // the catch-up cap, its word <= 0 now and then
        Put8(m, gs + 4, m.PeekByte(gs + 4) | 1u);
        Put8(m, gs + 57, Chance(r, 2) ? 0u : 3u);
        m.PokeWord(fc + 180, 0);
        m.PokeWord(gs + 60, r.U32() % 3u);
        if (Chance(r, 2)) PutS32(m, rr::sim::kTdCatchupTab + 4u * m.PeekWord(gs + 60), -static_cast<int32_t>(r.U32() % 0x100000u));
    }
    if (Chance(r, 6)) {                                              // a road of one or two lanes
        const uint32_t xs = m.PeekWord(fc + 372);
        if (InRam(xs)) {
            PutS16(m, xs + 140, static_cast<int16_t>(1 + r.U32() % 2u));
            PutS16(m, xs + 12, static_cast<int16_t>(1 + r.U32() % 2u));
        }
    }
    if (Chance(r, 3)) {                                              // the bike +0x1FF names, near the car
        const uint32_t k = r.U32() % 18u;
        Put8(m, fc + 511, k);
        const uint32_t b = pop::Bike(m, k);
        const uint32_t R = m.PeekWord(b + 852);
        const uint32_t who = (InRam(R) && Chance(r, 4)) ? R : b;
        if (who == R) m.PokeWord(R + 604, 3u + r.U32() % 2u);
        if (Chance(r, 4)) m.PokeWord(fc + 360, 0x10000u | (m.PeekWord(fc + 360) & 0xFFFFu));
        m.PokeWord(who + 360, m.PeekWord(fc + 360));
        static const int32_t kD[6] = {0x2FFFF, 0x30000, 0xC0000, 0xC0001, 0x10000, 0x200000};
        const int32_t d = Chance(r, 3) ? static_cast<int32_t>(r.U32() % 0x200000u) : kD[r.U32() % 6u];
        PutS32(m, who + 368, GetS32(m, fc + 368) + (Chance(r, 2) ? d : -d));
        if (Chance(r, 3)) PutS32(m, fc + 368, static_cast<int32_t>(r.U32() % 0x40000u));
    }
    if (Chance(r, 4)) {                                              // player 2 on the car's road
        const uint32_t p2 = m.PeekWord(rr::sim::kPopPlayerBikes + 4);
        if (InRam(p2)) {
            m.PokeWord(p2 + 360, m.PeekWord(fc + 360));
            Put8(m, gs + 4, m.PeekByte(gs + 4) | 0x10u);
        }
    }
    // a bike in a car's path
    if (Chance(r, 3)) {
        const uint32_t e = Chance(r, 3) ? cars[r.U32() % cars.size()] : fc;
        const uint32_t b = pop::Bike(m, 1u + r.U32() % 17u);
        rr::sim::GuestRam g(m.ram().data(), c.gp);
        const int32_t d = static_cast<int32_t>(2u + r.U32() % 50u) << 16;
        const int32_t dir = GetS32(m, e + 364);
        rr::sim::MulAddView(g, e + 184, e + 444, Chance(r, 5) ? -d : d, b + 184);
        m.PokeWord(b + 360, m.PeekWord(e + 360));
        PutS32(m, b + 364, Chance(r, 2) ? dir : -dir);
        PutS32(m, b + 368, GetS32(m, e + 368) + (dir > 0 ? d : -d));
        PutS32(m, b + 344, GetS32(m, e + 344) + static_cast<int32_t>(r.U32() % 0x40000u) - 0x20000);
        m.PokeWord(b + 336, m.PeekWord(e + 336));
        if (Chance(r, 2)) PutS32(m, b + 480, static_cast<int32_t>(r.U32() % 0x300000u));
        if (Chance(r, 4)) m.PokeWord(m.PeekWord(b + 1084) + 40, r.U32() % 2u);
        uint16_t hb = 0;
        m.ReadBlock(b + 172, &hb, 2);
        GridInsert(c, hb, GetS32(m, b + 184), GetS32(m, b + 192));
    }
    // the player around a car: passing it the other way, or behind / ahead of it
    if (Chance(r, 2)) {
        const uint32_t e = Chance(r, 4) ? cars[r.U32() % cars.size()] : fc;
        const uint32_t p = m.PeekWord(rr::sim::kPopPlayerBikes);
        const int32_t dir = GetS32(m, e + 364);
        if (Chance(r, 5)) m.PokeWord(e + 360, 0x10000u | (m.PeekWord(e + 360) & 0xFFFFu));   // both at a node
        m.PokeWord(p + 360, m.PeekWord(e + 360));
        m.PokeWord(p + 336, Chance(r, 4) ? m.PeekWord(e + 336) + 64u : m.PeekWord(e + 336));
        if (Chance(r, 2)) {
            PutS32(m, p + 364, -dir);
            PutS32(m, p + 368, GetS32(m, e + 368) + (static_cast<int32_t>(r.U32() % 0x140000u) - 0x40000) * (dir > 0 ? 1 : -1));
            PutS32(m, p + 344, GetS32(m, e + 344) + static_cast<int32_t>(r.U32() % 0x60000u) - 0x30000);
        } else {
            PutS32(m, p + 364, Chance(r, 2) ? dir : -dir);
            const int32_t d = static_cast<int32_t>(0x270000u + r.U32() % 0x2A0000u);
            PutS32(m, p + 368, GetS32(m, e + 368) + (Chance(r, 2) ? d : -d));
        }
        static const int32_t kPs[5] = {0x280000, 0x280001, 0x400000, 0x100000, 0};
        PutS32(m, p + 480, kPs[r.U32() % 5u]);
        if (Chance(r, 5)) m.PokeWord(m.PeekWord(p + 852) + 604, r.U32() % 5u);
        if (Chance(r, 2)) pop::PlaceView(c, 0, GetS16(m, e + 186), GetS16(m, e + 194), 150 + static_cast<int32_t>(r.U32() % 20u));
    }
    return cars;
}

uint32_t SomeCar(CaseContext& c, const std::vector<uint32_t>& cars) {
    if (cars.empty()) return Car(0);
    if (!Chance(*c.rng, 3)) return cars[0];                          // the focus car
    return cars[c.rng->U32() % cars.size()];
}

// A handle address the blocker functions are handed: a bike's, a rider's, a car's, a pool-6 record.
uint32_t SomeHandle(CaseContext& c, const std::vector<uint32_t>& cars) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    switch (r.U32() % 6u) {
        case 0: case 1: return pop::Bike(m, r.U32() % 18u) + 172u;
        case 2: return pop::kRider0 + pop::kRiderBytes * (r.U32() % 18u) + 172u;
        case 3: return SomeCar(c, cars) + 172u;
        case 4: {
            const uint32_t p6 = m.PeekWord(rr::sim::kTdPool6Ptr);
            return InRam(p6) ? p6 + 280u * (r.U32() % 4u) : SomeCar(c, cars) + 172u;
        }
        default: return pop::Bike(m, 0) + 172u;
    }
}

int32_t DrawDt(Rng& r) {
    static const int32_t kDt[4] = {0x884, 0x1108, 0x444, 0};
    return Chance(r, 5) ? static_cast<int32_t>(r.U32() % 0x8000u) : kDt[r.U32() % 4u];
}

// ---------------------------------------------------------------- the rows' setups and natives
void SetupLaneChange(CaseContext& c, Args& a);
void SetupPass(CaseContext& c, Args& a) {
    if (Chance(*c.rng, 3)) {                                         // a car ready to change lanes
        SetupLaneChange(c, a);
        a.a[0] = a.a[1];
        a.a[1] = 0;
        return;
    }
    DrawWorld(c);
    a.a[0] = U(DrawDt(*c.rng));
}
uint32_t NativePass(NativeEnv& env, const Args& a) {
    rr::sim::GuestRam g = pop::View(env);
    TdSeams s(env);
    return pop::PopDone(env, g, rr::sim::TrafficPass(g, static_cast<int32_t>(a.a[0]), env.sp, s), "TrafficPass");
}

void SetupRoadStep(CaseContext& c, Args& a) {
    Rng& r = *c.rng;
    const std::vector<uint32_t> cars = DrawWorld(c);
    const uint32_t e = SomeCar(c, cars);
    a.a[0] = Chance(r, 5) ? 0u : e + 172u;
    a.a[1] = Chance(r, 20) ? 0u : e + 328u;
    int32_t d;
    switch (r.U32() % 5u) {
        case 0: d = static_cast<int32_t>(r.U32() % 0x100000u); break;
        case 1: d = -static_cast<int32_t>(r.U32() % 0x100000u); break;
        case 2: d = static_cast<int32_t>(r.U32() % 0x20000000u) - 0x10000000; break;   // NAMED BOUND 2
        case 3: d = static_cast<int32_t>(r.U32() % 0x3000000u) - 0x1800000; break;
        default: d = Chance(r, 2) ? 0 : 1; break;
    }
    a.a[2] = U(d);
}
uint32_t NativeRoadStep(NativeEnv& env, const Args& a) {
    rr::sim::GuestRam g = pop::View(env);
    uint32_t v = 0;
    const bool ok = rr::sim::RoadStep(g, a.a[0], a.a[1], static_cast<int32_t>(a.a[2]), env.sp, v);
    return pop::PopDone(env, g, ok, "RoadStep", v);
}

void SetupGridQuery(CaseContext& c, Args& a) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    const std::vector<uint32_t> cars = DrawWorld(c);
    uint32_t self = 0;
    uint32_t pos;
    if (Chance(r, 4)) {
        pos = c.scratch + kPoint;
        const uint32_t e = SomeCar(c, cars);
        for (uint32_t k = 0; k < 3; ++k) PutS32(m, pos + 4u * k, GetS32(m, e + 184 + 4u * k) + (r.S32() >> (8 + r.U32() % 8u)));
        if (Chance(r, 5)) for (uint32_t k = 0; k < 3; ++k) PutS32(m, pos + 4u * k, r.S32());
    } else {
        const uint32_t e = Chance(r, 2) ? SomeCar(c, cars) : pop::Bike(m, r.U32() % 18u);
        pos = e + 184;
        self = m.PeekWord(e + 172) & 0xFFFFu;
    }
    RrNoise(c, c.scratch + kList, 32);
    m.PokeWord(c.scratch + kCount, Chance(r, 3) ? 1u + r.U32() % 16u : 16u);
    a.a[0] = c.scratch + kList;
    a.a[1] = c.scratch + kCount;
    a.a[2] = pos;
    a.a[3] = Chance(r, 3) ? r.U32() % 0x2000000u : 0x3C5997u;
    a.a4 = Chance(r, 3) ? (r.U32() & 0xFFu) : 59u;
    a.more[0] = Chance(r, 4) ? (r.U32() & 0xFFFFu) : self;
}
uint32_t NativeGridQuery(NativeEnv& env, const Args& a) {
    rr::sim::GuestRam g = pop::View(env);
    const bool ok = rr::sim::GridQuery(g, a.a[0], a.a[1], a.a[2], static_cast<int32_t>(a.a[3]), env.sp);
    return pop::PopDone(env, g, ok, "GridQuery");
}

void SetupCar(CaseContext& c, Args& a) {
    const std::vector<uint32_t> cars = DrawWorld(c);
    a.a[0] = SomeCar(c, cars);
}
// A car the lane change can act on half the time: the accumulator past 2.0, not accelerating, fast,
// flags clear, mid-road, the judged player 40..80 away on its road with his camera behind the car
// along its forward row; two lanes or more.
void SetupLaneChange(CaseContext& c, Args& a) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    const std::vector<uint32_t> cars = DrawWorld(c);
    const uint32_t car = SomeCar(c, cars);
    a.a[0] = car;
    a.a[1] = U(DrawDt(r));
    if (!Chance(r, 2)) return;
    PutS32(m, rr::sim::kTdLaneTimer, 0x20000);
    if (Chance(r, 3)) FlipDir(m, car);
    PutS32(m, car + 484, Chance(r, 2) ? 0 : -0x26666);
    PutS32(m, car + 480, 132 + static_cast<int32_t>(r.U32() % 0x200000u));
    Put8(m, car + 509, m.PeekByte(car + 509) & ~0x13u);
    Put8(m, car + 510, m.PeekByte(car + 510) & ~0x01u);
    m.PokeWord(car + 360, m.PeekWord(car + 360) & 0xFFFFu);
    PutS32(m, car + 368, static_cast<int32_t>(0x1000000u + r.U32() % 0x3000000u));
    static const int8_t kLane[6] = {1, 2, 3, 2, 1, 3};
    Put8(m, car + 508, static_cast<uint8_t>(kLane[r.U32() % 6u]));
    if (Chance(r, 3)) PutS16(m, car + 370, static_cast<int16_t>(r.U32()));
    const uint32_t xs = m.PeekWord(car + 372);
    if (InRam(xs) && Chance(r, 3)) {
        PutS16(m, xs + 140, 2);
        PutS16(m, xs + 12, 2);
    }
    const uint32_t gs = m.PeekWord(kGameStatePtr);
    if (Chance(r, 2)) Put8(m, gs + 4, m.PeekByte(gs + 4) & ~0x10u);
    const uint32_t p = m.PeekWord(rr::sim::kPopPlayerBikes);
    m.PokeWord(p + 360, m.PeekWord(car + 360));
    const int32_t d = static_cast<int32_t>(0x280000u + r.U32() % 0x280001u);
    PutS32(m, p + 368, GetS32(m, car + 368) + (Chance(r, 2) ? d : -d));
    // view 0 behind the car along its forward row, facing the same way
    const uint32_t v = rr::sim::kPopViewArray;
    rr::sim::GuestRam g(m.ram().data(), c.gp);
    rr::sim::MulAddView(g, car + 184, car + 444, -static_cast<int32_t>(0x100000u + r.U32() % 0x400000u), v + 184);
    for (uint32_t k = 0; k < 3; ++k) PutS16(m, v + 444 + 2u * k, GetS16(m, car + 444 + 2u * k));
    if (Chance(r, 4)) {                                              // a car ahead within / past 25.0
        for (uint32_t e : cars) {
            if (e == car) continue;
            m.PokeWord(e + 360, m.PeekWord(car + 360));
            PutS32(m, e + 364, GetS32(m, car + 364));
            const int32_t ahead = static_cast<int32_t>(0x10000u + r.U32() % 0x300000u);
            PutS32(m, e + 368, GetS32(m, car + 368) + (GetS32(m, car + 364) > 0 ? ahead : -ahead));
            break;
        }
    }
}

void SetupCarDt(CaseContext& c, Args& a) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    SetupCar(c, a);
    a.a[1] = U(DrawDt(r));
    a.a[2] = Chance(r, 3) ? 0u : (Chance(r, 4) ? r.U32() : 1u);
    if (Chance(r, 4)) {                                              // the brake for the road's start node
        const uint32_t car = a.a[0];
        Put8(m, car + 509, (m.PeekByte(car + 509) & ~0x41u) | (Chance(r, 2) ? 2u : 0u));
        m.PokeWord(car + 360, m.PeekWord(car + 360) & 0xFFFFu);
        if (GetS32(m, car + 364) > 0) FlipDir(m, car);
        PutS32(m, car + 368, static_cast<int32_t>(r.U32() % 0x200000u) - 0x100000);
        static const int32_t kSpd[4] = {132, 0x100000, 0xB60000, 0x2000000};
        PutS32(m, car + 480, kSpd[r.U32() % 4u]);
        a.a[2] = 1;
    }
}
void SetupCarHandle(CaseContext& c, Args& a) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    const std::vector<uint32_t> cars = DrawWorld(c);
    const uint32_t car = SomeCar(c, cars);
    a.a[0] = car;
    a.a[1] = Chance(r, 12) ? 0u : SomeHandle(c, cars);
    if (Chance(r, 2)) {                                              // a stopped, blocked car (the horn)
        PutS32(m, car + 480, static_cast<int32_t>(r.U32() % 132u));
        Put8(m, car + 509, m.PeekByte(car + 509) | 0x10u);
        if (a.a[1] != 0 && (m.PeekWord(a.a[1]) & 0xFFE0u) == 0 && Chance(r, 2))
            m.PokeWord(m.PeekWord(pop::Bike(m, m.PeekWord(a.a[1]) & 0x1Fu) + 1084) + 40, 0);
    }
}
// The entity of handle `hv` put `d` along the car's forward row from its box (the along gap).
void PlaceAhead(CaseContext& c, uint32_t car, uint32_t e, int32_t d) {
    rr::sim::GuestRam g(c.mem->ram().data(), c.gp);
    rr::sim::MulAddView(g, car + 184, car + 444, d, e + 184);
}
void SetupBlockerGap(CaseContext& c, Args& a) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    const std::vector<uint32_t> cars = DrawWorld(c);
    const uint32_t car = SomeCar(c, cars);
    uint16_t hv = 0;
    m.ReadBlock(SomeHandle(c, cars), &hv, 2);
    if ((static_cast<uint32_t>(hv) >> 5) == 6u || (static_cast<uint32_t>(hv) >> 5) == 2u) hv = static_cast<uint16_t>(0x60u);    // pools 0, 1, 3, 4, 5 only
    if (Chance(r, 10)) hv = static_cast<uint16_t>(((4u + r.U32() % 2u) << 5) | (r.U32() % 4u));   // pools 4 / 5
    const uint32_t kind = static_cast<uint32_t>(hv) >> 5;
    const uint32_t e = m.PeekWord(0x800CE4D0u + 16u * kind) + m.PeekWord(0x800CE4D4u + 16u * kind) * (hv & 0x1Fu);
    if (InRam(e) && kind != 4u && kind != 5u && e != car) {
        if (Chance(r, 3)) {                                          // on the car's key, in front of it
            m.PokeWord(e + 360, m.PeekWord(car + 360));
            m.PokeWord(e + 336, Chance(r, 3) ? m.PeekWord(car + 336) : r.U32());
            PutS32(m, e + 344, GetS32(m, car + 344) + static_cast<int32_t>(r.U32() % 0x60000u) - 0x30000);
            PlaceAhead(c, car, e, static_cast<int32_t>(r.U32() % 0x300000u) - 0x40000);
            if (Chance(r, 4)) m.PokeWord(car + 360, 0x10000u | (m.PeekWord(car + 360) & 0xFFFFu));
            if (Chance(r, 4)) m.PokeWord(e + 360, m.PeekWord(car + 360));
        }
        if (Chance(r, 4)) {                                          // the gap exactly 0 along a pure-z heading
            PutS16(m, car + 444, 0);
            PutS16(m, car + 446, 0);
            PutS16(m, car + 448, 4096);
            const int32_t sub = (kind != 0) ? 2 * GetS32(m, car + 308) + GetS32(m, e + 308) : GetS32(m, car + 308);
            for (uint32_t k = 0; k < 3; ++k) PutS32(m, e + 184 + 4u * k, GetS32(m, car + 184 + 4u * k));
            PutS32(m, e + 192, GetS32(m, car + 192) + sub + (Chance(r, 2) ? 0 : static_cast<int32_t>(r.U32() % 3u) - 1));
            PutS32(m, car + 480, 132 + static_cast<int32_t>(r.U32() % 0x100000u));
            PutS32(m, e + 480, 0);
        }
        if (Chance(r, 5)) {                                          // perpendicular headings
            PutS16(m, e + 444, GetS16(m, car + 448));
            PutS16(m, e + 446, 0);
            PutS16(m, e + 448, static_cast<int16_t>(-GetS16(m, car + 444)));
        }
        if (Chance(r, 6)) PutS32(m, e + 304, -static_cast<int32_t>(r.U32() % 0x80000u));
        if (Chance(r, 6)) PutS32(m, e + 308, -static_cast<int32_t>(r.U32() % 0x80000u));
        if (kind == 0 && Chance(r, 3)) m.PokeWord(e + 856, pop::Bike(m, r.U32() % 18u));   // a passenger
        if (Chance(r, 3)) PutS32(m, e + 364, Chance(r, 2) ? GetS32(m, car + 364) : -GetS32(m, car + 364));
        if (Chance(r, 3)) PutS32(m, e + 480, static_cast<int32_t>(r.U32() % 0x200000u));
    }
    a.a[0] = hv;
    a.a[1] = car;
}
void SetupWarnSound(CaseContext& c, Args& a) {
    Rng& r = *c.rng;
    const std::vector<uint32_t> cars = DrawWorld(c);
    const uint32_t e = SomeCar(c, cars);
    a.a[0] = r.U32() % 2u;
    a.a[1] = r.U32() % 2u;
    a.a[2] = Chance(r, 3) ? r.U32() : c.mem->PeekWord(e + 184);
    a.a[3] = Chance(r, 3) ? r.U32() : c.mem->PeekWord(e + 192);
}

uint32_t NativeCarAhead(NativeEnv& env, const Args& a) {
    rr::sim::GuestRam g = pop::View(env);
    return pop::PopDone(env, g, true, "CarAhead", rr::sim::CarAhead(g, a.a[0]));
}
uint32_t NativeLanePlayer(NativeEnv& env, const Args& a) {
    rr::sim::GuestRam g = pop::View(env);
    return pop::PopDone(env, g, true, "LanePlayer", rr::sim::LanePlayer(g, a.a[0], env.sp));
}
uint32_t NativePlayerClose(NativeEnv& env, const Args& a) {
    rr::sim::GuestRam g = pop::View(env);
    TdSeams s(env);
    int32_t v = 0;
    const bool ok = rr::sim::PlayerClose(g, a.a[0], env.sp, s, v);
    return pop::PopDone(env, g, ok, "PlayerClose", U(v));
}
uint32_t NativeCarBrake(NativeEnv& env, const Args& a) {
    rr::sim::GuestRam g = pop::View(env);
    rr::sim::CarBrake(g, a.a[0], static_cast<int32_t>(a.a[1]), a.a[2], env.sp);
    return pop::PopDone(env, g, true, "CarBrake");
}
uint32_t NativeLaneChange(NativeEnv& env, const Args& a) {
    rr::sim::GuestRam g = pop::View(env);
    rr::sim::LaneChange(g, a.a[0], static_cast<int32_t>(a.a[1]), env.sp);
    return pop::PopDone(env, g, true, "LaneChange");
}
uint32_t NativeGiveUp(NativeEnv& env, const Args& a) {
    rr::sim::GuestRam g = pop::View(env);
    return pop::PopDone(env, g, true, "BlockGiveUp", U(rr::sim::BlockGiveUp(g, a.a[0], a.a[1], env.sp)));
}
uint32_t NativeHorn(NativeEnv& env, const Args& a) {
    rr::sim::GuestRam g = pop::View(env);
    TdSeams s(env);
    return pop::PopDone(env, g, rr::sim::Horn(g, a.a[0], a.a[1], env.sp, s), "Horn");
}
uint32_t NativeWarnSound(NativeEnv& env, const Args& a) {
    rr::sim::GuestRam g = pop::View(env);
    TdSeams s(env);
    const bool ok = rr::sim::WarnSound(g, a.a[0], a.a[1], static_cast<int32_t>(a.a[2]), static_cast<int32_t>(a.a[3]), env.sp, s);
    return pop::PopDone(env, g, ok, "WarnSound");
}
uint32_t NativePassWarn(NativeEnv& env, const Args& a) {
    rr::sim::GuestRam g = pop::View(env);
    TdSeams s(env);
    int32_t v = 0;
    const bool ok = rr::sim::PassWarn(g, a.a[0], env.sp, s, v);
    return pop::PopDone(env, g, ok, "PassWarn", U(v));
}
uint32_t NativeBlockerGap(NativeEnv& env, const Args& a) {
    rr::sim::GuestRam g = pop::View(env);
    return pop::PopDone(env, g, true, "BlockerGap", U(rr::sim::BlockerGap(g, a.a[0], a.a[1], env.sp)));
}
uint32_t NativeBlocker(NativeEnv& env, const Args& a) {
    rr::sim::GuestRam g = pop::View(env);
    uint32_t v = 0;
    const bool ok = rr::sim::Blocker(g, a.a[0], env.sp, v);
    return pop::PopDone(env, g, ok, "Blocker", v);
}

Row TdRow(const char* name, const char* sig, uint32_t entry, std::function<void(CaseContext&, Args&)> setup,
          std::function<uint32_t(NativeEnv&, const Args&)> native, bool v0, bool seams) {
    Row r{name, sig, entry, std::move(setup), nullptr, v0, std::move(native)};
    r.maxSteps = 40'000'000;
    if (seams) r.oracleCallees = Seams();
    return r;
}

} // namespace tdrive


// The extent [entry, end) of each row's function, for `--coverage` (the address after the delay slot of
// its last `jr ra`, from our own listing). verify_physics.cpp consults it as a
// `TrafficDriveCoverage(row.name)` fallback beside PopulationCoverage.
const CoverageRange* TrafficDriveCoverage(const char* rowName) {
    struct Named {
        const char* name;
        CoverageRange range;
    };
    static const Named kRanges[] = {
        {"td_road_step", {0x8003775C, 0x80037A30}},   {"td_grid_query", {0x8008AE94, 0x8008B428}},
        {"td_car_ahead", {0x8009BA5C, 0x8009BB48}},   {"td_lane_player", {0x800A04B0, 0x800A05D4}},
        {"td_player_close", {0x800A05D4, 0x800A0708}}, {"td_car_brake", {0x8009B4C0, 0x8009B728}},
        {"td_lane_change", {0x8009B728, 0x8009BA5C}}, {"td_give_up", {0x8009F6F8, 0x8009F9EC}},
        {"td_horn", {0x8009F9EC, 0x8009FAD8}},        {"td_warn_sound", {0x80017CF0, 0x80017DA0}},
        {"td_pass_warn", {0x8009FCDC, 0x8009FE90}},   {"td_blocker_gap", {0x800BF0F8, 0x800BF424}},
        {"td_blocker", {0x800BE7D8, 0x800BEA30}},     {"traffic_pass", {0x8009A298, 0x8009AB60}},
    };
    for (const Named& n : kRanges)
        if (std::strcmp(n.name, rowName) == 0) return &n.range;
    return nullptr;
}

void AddTrafficDriveRows(std::vector<Row>& rows) {
    using namespace tdrive;
    rows.push_back(TdRow("td_road_step", "Slice *RoadStep(Handle *p, Cursor *c, s32 dist)  [SLUS 0x8003775C]", kRoadStep,
                         SetupRoadStep, NativeRoadStep, true, false));
    rows.push_back(TdRow("td_grid_query", "void GridQuery(u16 *out, s32 *n, s32 pos[3], s32 r, [sp+16] mask, [sp+20] self)  [RASHCDG 0x8008AE94]",
                         kGridQuery, SetupGridQuery, NativeGridQuery, false, false));
    rows.push_back(TdRow("td_car_ahead", "Car *CarAhead(Car *c)  [RASHCDG 0x8009BA5C]", kCarAhead, SetupCar, NativeCarAhead, true, false));
    rows.push_back(TdRow("td_lane_player", "u32 LanePlayer(Car *c)  [RASHCDG 0x800A04B0]", kLanePlayer, SetupCar,
                         NativeLanePlayer, true, false));
    rows.push_back(TdRow("td_player_close", "s32 PlayerClose(Car *c)  [RASHCDG 0x800A05D4]", kPlayerClose, SetupCar,
                         NativePlayerClose, true, true));
    rows.push_back(TdRow("td_car_brake", "void CarBrake(Car *c, s32 dt, s32 changed)  [RASHCDG 0x8009B4C0]", kCarBrake,
                         SetupCarDt, NativeCarBrake, false, false));
    rows.push_back(TdRow("td_lane_change", "void LaneChange(Car *c, s32 dt)  [RASHCDG 0x8009B728]", kLaneChange, SetupLaneChange,
                         NativeLaneChange, false, false));
    rows.push_back(TdRow("td_give_up", "s32 BlockGiveUp(Car *c, Handle *h)  [RASHCDG 0x8009F6F8]", kGiveUp, SetupCarHandle,
                         NativeGiveUp, true, false));
    rows.push_back(TdRow("td_horn", "void Horn(Car *c, Handle *h)  [RASHCDG 0x8009F9EC]", kHorn, SetupCarHandle, NativeHorn,
                         false, true));
    rows.push_back(TdRow("td_warn_sound", "void WarnSound(u32 p, u32 bit, s32 x, s32 z)  [SLUS 0x80017CF0]", kWarnSound,
                         SetupWarnSound, NativeWarnSound, false, true));
    rows.push_back(TdRow("td_pass_warn", "s32 PassWarn(Car *c)  [RASHCDG 0x8009FCDC]", kPassWarn, SetupCar, NativePassWarn,
                         true, true));
    rows.push_back(TdRow("td_blocker_gap", "s32 BlockerGap(u16 h, Car *c)  [RASHCDG 0x800BF0F8]", kBlockerGap,
                         SetupBlockerGap, NativeBlockerGap, true, false));
    rows.push_back(TdRow("td_blocker", "Handle *Blocker(Car *c)  [RASHCDG 0x800BE7D8]", kBlocker, SetupCar, NativeBlocker,
                         true, false));
    rows.push_back(TdRow("traffic_pass", "void TrafficPass(s32 dt)  [RASHCDG 0x8009A298]", kPass, SetupPass, NativePass,
                         false, true));
}
