// Bench rows for the population runtime. Included by
// verify_physics.cpp inside its anonymous namespace, so everything defined above that point is visible here.
//
// The port is src\game\sim\population.{h,cpp}, over road_query.h's guest-address view. Every native
// side is: wrap the clone's RAM in a view, call the port with the guest's own arguments at the bench's
// `sp`, fail the case loudly if the port met an address the console would not survive.
//
// INPUT FAMILIES. rr-race holds three resident road objects (67 = road 9 along 527..1624; 15 = the
// approach half of junction 7, road 9 along 258..527 and roads 10, 8; 14 = its core, road 9 along
// 0..258), three entries in the resident piece list 0x800D4B10 (last index 2), 18 bikes (the player
// live, bike 6 live, the rest dormant), their 18 riders, one traffic car and one player.
//   * dump-derived: this machine's entities with their REAL road coordinates, cursors, boxes and
//     views, the VALUE fields (lateral, along, direction, box) of one of the captured bikes planted;
//   * randomised: road coordinates re-drawn onto every resident piece, junction arm and node key -
//     at and one unit past both ends of every along range - and onto roads nothing holds; the
//     resident piece list and residency (BTT_ +0x0C, +0x1C) stirred; the handle's pool, slot and the
//     live word drawn; the views put at EXACT octagonal distances one unit either side of every
//     radius; the player count 0..2 with a second view and player bike; the cop's class, progress
//     and the players' progress around the 201-unit cop bracket; the collision-volume radius.
// NAMED BOUND: the player count is 0, 1 or 2. The window test keeps its per-player distances in a
// two-word array in its own 56-byte frame (sp+24, sp+28) and writes one word per player: with three
// or more players it overwrites its own saved registers (and with eight its return address), which
// the console's two-player game never does and no port of the function can reproduce.

namespace pop {
constexpr uint32_t kPopWindowPred = 0x80013110, kPopPieceContains = 0x80039CFC, kPopRoadGate = 0x80039DFC,
                   kPopSeatAlong = 0x80037524, kPopCursorSeat = 0x8003A700, kPopCursorReseat = 0x8003A468,
                   kPopRoadWindow = 0x80039F68;
// Scratch-block layout (8 KiB; RqBike's rider record sits at 8000).
constexpr uint32_t kKeyOff = 0x100;   // a 12-byte road coordinate
constexpr uint32_t kCurOff = 0x120;   // a 32-byte cursor
constexpr uint32_t kPosOff = 0x140;   // a 12-byte point
constexpr uint32_t kBttOff = 0x160;   // a synthetic BTT_ record, 32 bytes
constexpr uint32_t kObjOff = 0x180;   // a synthetic object header, 0x70 bytes
constexpr uint32_t kRider0 = 0x801BB2EC;  // pool 1 slot 0 in rr-race (628-byte riders)
constexpr uint32_t kCar0 = 0x800CF660;    // pool 3 slot 0 (512-byte cars)
constexpr uint32_t kRiderBytes = 628;

uint32_t U(int32_t v) { return static_cast<uint32_t>(v); }
bool Chance(Rng& r, uint32_t n) { return r.Next() % n == 0u; }
void Put8(Memory& m, uint32_t a, uint32_t v) {
    const uint8_t b = static_cast<uint8_t>(v);
    m.WriteBlock(a, &b, 1);
}

// The octagonal distance of 0x80013110 (the same integer arithmetic as the port's).
int32_t Oct(int32_t dx, int32_t dz) {
    dx = dx < 0 ? -dx : dx;
    dz = dz < 0 ? -dz : dz;
    const int32_t big = dx < dz ? dz : dx, small = dx < dz ? dx : dz;
    const int32_t s = small + (small >> 1);
    return big - (big >> 5) - (big >> 7) + (s >> 2) + (s >> 6);
}

// Integer offsets (dx, dz) whose octagonal distance is exactly `target` (or the nearest above it).
void OffsetsAt(Rng& r, int32_t target, int32_t& dx, int32_t& dz) {
    dz = (target > 4) ? static_cast<int32_t>(r.U32() % static_cast<uint32_t>(target / 3 + 1)) : 0;
    dx = 0;
    while (dx < 4 * target + 8 && Oct(dx, dz) < target) ++dx;
    if (r.Next() & 1u) std::swap(dx, dz);
    if (r.Next() & 1u) dx = -dx;
    if (r.Next() & 1u) dz = -dz;
}

// A distance to test the window at: one unit either side of a radius (or of the collision-volume
// radius), or anything.
int32_t DistanceNear(Rng& r, int32_t volumeRadius) {
    static const int32_t kR[] = {200, 230, 300, 350};
    const uint64_t k = r.Next() % 6u;
    if (k < 4) return kR[r.U32() % 4u] - 1 + static_cast<int32_t>(r.U32() % 3u);
    if (k == 4) return volumeRadius - 1 + static_cast<int32_t>(r.U32() % 3u);
    return static_cast<int32_t>(r.U32() % 1200u);
}

// View p's camera (+0xB8 / +0xC0 16.16) put at integer distance `d` from (x, z), low halves drawn.
void PlaceView(CaseContext& c, uint32_t p, int32_t x, int32_t z, int32_t d) {
    int32_t dx, dz;
    OffsetsAt(*c.rng, d, dx, dz);
    const uint32_t v = rr::sim::kPopViewArray + rr::sim::kPopViewStride * p;
    c.mem->PokeWord(v + 0xB8, (U(x + dx) << 16) | (c.rng->U32() & 0xFFFFu));
    c.mem->PokeWord(v + 0xC0, (U(z + dz) << 16) | (c.rng->U32() & 0xFFFFu));
}

// The BTT_ record of object `obj`, indexed as RoadBttRecord indexes it.
uint32_t BttOf(const Memory& m, const RqNet& n, uint32_t obj) {
    const int32_t id = GetS32(m, obj);
    const int32_t idx = (id < n.nJn) ? id + n.nRoad : id - n.nJn;
    return n.btt + 32u * U(idx);
}

// A road coordinate at `at`: {road | kind << 16, dir, along}. Eight times in ten on a road or node a
// resident object holds - the along at, one unit outside, or inside its range - otherwise on a road
// or a kind nothing holds.
void PlantKey(CaseContext& c, const RqNet& n, uint32_t at) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    uint32_t road = r.U32() % 40u, kind = 0;
    int32_t lo = -100, hi = 3000;
    const uint64_t pick = r.Next() % 10u;
    if (pick < 8 && !n.resident.empty()) {
        const uint32_t obj = n.resident[r.U32() % n.resident.size()];
        const int32_t npc = GetS16(m, obj + 0x12);
        const uint32_t rec = m.PeekWord(obj + 0x2C) + 32u * RqPick(r, npc);
        if (GetS16(m, rec + 2) != 0 || pick == 7) {             // the junction's node key
            kind = 1;
            road = m.PeekWord(obj + 8) & 0xFFFFu;
        } else {
            road = m.PeekWord(rec + 12) & 0xFFFFu;
            lo = GetS16(m, rec + 26);
            hi = GetS16(m, rec + 30);
        }
    } else if (pick == 8) {
        kind = 1u + r.U32() % 3u;
    }
    int32_t a;
    switch (r.U32() % 7u) {
        case 0: a = lo - 1; break;
        case 1: a = lo; break;
        case 2: a = hi; break;
        case 3: a = hi + 1; break;
        case 4: a = lo - 60 + static_cast<int32_t>(r.U32() % static_cast<uint32_t>(hi - lo + 121)); break;
        default: a = lo + static_cast<int32_t>(r.U32() % static_cast<uint32_t>(std::max(1, hi - lo + 1))); break;
    }
    m.PokeWord(at + 0, (kind << 16) | road);
    static const int32_t kDir[4] = {1, -1, 2, -2};
    PutS32(m, at + 4, Chance(r, 8) ? r.S32() : kDir[r.U32() % 4u]);
    m.PokeWord(at + 8, (U(a) << 16) | (r.U32() & 0xFFFFu));
    if (Chance(r, 16)) PutS32(m, at + 8, r.S32());
}

// The resident piece list 0x800D4B10 and its last index 0x8005B31C, stirred one case in three:
// entries kept, emptied (-1), or set to a resident object's id, any object id or anything.
void StirPieceList(CaseContext& c, const RqNet& n) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    if (!Chance(r, 3)) return;
    const int32_t last = static_cast<int32_t>(r.U32() % 7u) - 1;   // -1 .. 5
    PutS32(m, rr::sim::kPopPieceLast, last);
    for (int32_t i = 0; i <= last; ++i) {
        const uint32_t e = rr::sim::kPopPieceList + 16u * U(i);
        switch (r.U32() % 5u) {
            case 0: m.PokeWord(e, 0xFFFFFFFFu); break;
            case 1: if (!n.resident.empty()) m.PokeWord(e, m.PeekWord(n.resident[r.U32() % n.resident.size()])); break;
            case 2: m.PokeWord(e, r.U32() % static_cast<uint32_t>(std::max(1, n.nObj + 4))); break;
            case 3: m.PokeWord(e, r.U32()); break;
            default: break;
        }
    }
}

// BTT_ +0x1C (a junction's "own node key" answer) flipped on the resident objects now and then.
void StirBttFlags(CaseContext& c, const RqNet& n) {
    for (uint32_t obj : n.resident)
        if (Chance(*c.rng, 4)) c.mem->PokeWord(BttOf(*c.mem, n, obj) + 28u, c.rng->U32() % 3u);
}

// The player count, 0..2 (the named bound), with a second player's bike and view when it is 2.
uint32_t DrawPlayers(CaseContext& c) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    const uint32_t np = Chance(r, 10) ? 0u : (Chance(r, 3) ? 2u : 1u);
    m.PokeWord(m.PeekWord(kGameStatePtr) + 0x30u, np);
    if (np == 2) {
        const uint32_t b = m.PeekWord(kPoolBasePtr) + 1096u * (1u + r.U32() % 17u);
        m.PokeWord(rr::sim::kPopPlayerBikes + 4u, b);
        m.PokeWord(kPlayer2Ptr, b);                                  // *(0x8005B21C), player 2's bike
    }
    return np;
}

// One of this machine's entities: a pool-0 bike (mostly), a pool-1 rider, or the traffic car.
uint32_t SomeEntity(CaseContext& c) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    const uint64_t k = r.Next() % 8u;
    if (k < 5) return m.PeekWord(kPoolBasePtr) + 1096u * (r.U32() % 18u);
    if (k < 7) return kRider0 + kRiderBytes * (r.U32() % 18u);
    return kCar0;
}

// The captured VALUE fields of a bike on an entity of this machine (never a pointer).
void PlantSampleValues(CaseContext& c, uint32_t e) {
    if (c.sample == nullptr || c.sample->entity.size() < 0x448) return;
    const std::vector<uint8_t>& s = c.sample->entity;
    for (uint32_t off : {0xB8u, 0xBCu, 0xC0u, 0x158u, 0x15Cu, 0x16Cu, 0x170u}) {
        uint32_t v;
        std::memcpy(&v, s.data() + off, 4);
        c.mem->PokeWord(e + off, v);
    }
}

rr::sim::GuestRam View(NativeEnv& env) {
    rr::sim::GuestRam g(env.clone->mem.ram().data(), env.gp);
    g.SetScratchpad(env.clone->mem.scratchpad().data());
    return g;
}

// ---------------------------------------------------------------- SLUS 0x80013110
void SetupWindowPred(CaseContext& c, Args& a) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    uint32_t pos = c.scratch + kPosOff;
    const uint32_t p = Chance(r, 8) ? 2u + r.U32() % 2u : r.U32() % 2u;
    if (!Chance(r, 5)) m.PokeWord(c.gp + rr::sim::kPopVolumeRadiusGp, (r.U32() % 200u) << 16 | (r.U32() & 0xFFFFu));
    const int32_t vol = static_cast<int32_t>(m.PeekWord(c.gp + rr::sim::kPopVolumeRadiusGp)) >> 16;
    if (c.sample != nullptr) {
        // a real entity's box, the real camera - or the captured box of a bike, the view put near it
        const uint32_t e = SomeEntity(c);
        pos = e + 0xB8;
        PlantSampleValues(c, e);
        if (Chance(r, 2)) PlaceView(c, p, GetS16(m, pos + 2), GetS16(m, pos + 10), DistanceNear(r, vol));
    } else {
        for (uint32_t k = 0; k < 3; ++k) PutS32(m, pos + 4u * k, r.S32() >> (r.U32() % 8u));
        PlaceView(c, p, GetS16(m, pos + 2), GetS16(m, pos + 10), DistanceNear(r, vol));
    }
    static const uint32_t kExtra[3] = {0u, 0x140000u, 0u};
    a.a[0] = Chance(r, 6) ? r.U32() : r.U32() % 8u;
    a.a[1] = pos;
    a.a[2] = p;
    a.a[3] = Chance(r, 5) ? r.U32() : kExtra[r.U32() % 3u];
    a.a4 = Chance(r, 6) ? r.U32() : r.U32() % 2u;
}

uint32_t NativeWindowPred(NativeEnv& env, const Args& a) {
    rr::sim::GuestRam g = View(env);
    return RqDone(env, g, static_cast<uint32_t>(rr::sim::WindowPred(g, a.a[0], a.a[1], a.a[2], a.a[3], a.a4)));
}

// ---------------------------------------------------------------- SLUS 0x80039CFC
void SetupPieceContains(CaseContext& c, Args& a) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    const RqNet n = RqRead(m);
    const uint32_t key = c.scratch + kKeyOff;
    PlantKey(c, n, key);
    uint32_t btt = 0;
    const uint64_t k = r.Next() % 10u;
    if (k < 6 && !n.resident.empty()) {
        btt = BttOf(m, n, n.resident[r.U32() % n.resident.size()]);
    } else if (k < 8) {
        btt = n.btt + 32u * RqPick(r, n.nObj);                       // any record, resident or not
    } else if (k == 8) {
        // a synthetic record: a real object behind it, the "junction" flag, id and ranges drawn
        btt = c.scratch + kBttOff;
        for (uint32_t i = 0; i < 32; i += 4) m.PokeWord(btt + i, r.U32());
        m.PokeWord(btt + 12, (Chance(r, 5) || n.resident.empty()) ? 0u : n.resident[r.U32() % n.resident.size()]);
        PutS16(m, btt + 4, static_cast<int16_t>(r.U32() % 3u));
        PutS16(m, btt + 16, static_cast<int16_t>(m.PeekWord(key) & 0xFFFFu));
        const int16_t hi = GetS16(m, key + 10);
        PutS16(m, btt + 22, static_cast<int16_t>(hi - static_cast<int16_t>(r.U32() % 3u)));
        PutS16(m, btt + 26, static_cast<int16_t>(hi + static_cast<int16_t>(r.U32() % 3u) - 1));
        m.PokeWord(btt + 28, r.U32() % 2u);
    }
    StirBttFlags(c, n);
    if (Chance(r, 3)) RqStirResidency(c, n);
    a.a[0] = btt;
    a.a[1] = Chance(r, 20) ? 0u : key;
}

uint32_t NativePieceContains(NativeEnv& env, const Args& a) {
    rr::sim::GuestRam g = View(env);
    return RqDone(env, g, static_cast<uint32_t>(rr::sim::PieceContains(g, a.a[0], a.a[1])));
}

// ---------------------------------------------------------------- SLUS 0x80039DFC
// An entity's handle, live word and cursor object drawn for the gate: pool 0 or 1 (the player
// test), another pool, slot under or over the player count; live or dormant; the cursor on a
// resident object or on its own.
void DrawGateEntity(CaseContext& c, const RqNet& n, uint32_t e) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    if (Chance(r, 3)) WriteU16Bench(m, e + 0x140, Chance(r, 2) ? 0u : static_cast<uint16_t>(r.U32()));
    if (Chance(r, 3)) {
        const uint32_t pool = Chance(r, 3) ? r.U32() % 8u : r.U32() % 2u;
        // NAMED BOUND: a pool-0 or pool-1 handle names one of the 18 bikes / riders. The window test's
        // cop bracket reads pool0 + 1096 * handle and that slot's rider record +0x43C; past the 18 live
        // slots that word is not a pointer, and on the second seed the ORIGINAL trapped loading through
        // it (0x8003A290) in 3 of 2696 road_window cases before this bound. Other pools keep 0..31.
        const uint32_t slot = Chance(r, 2) ? r.U32() % 3u : r.U32() % (pool < 2u ? 18u : 32u);
        WriteU16Bench(m, e + 0xAC, static_cast<uint16_t>((pool << 5) | slot));
    }
    if (Chance(r, 4) && !n.resident.empty()) m.PokeWord(e + 0x148, n.resident[r.U32() % n.resident.size()]);
    if (Chance(r, 3)) PlantKey(c, n, e + 0x168);
}

void SetupRoadGate(CaseContext& c, Args& a) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    const RqNet n = RqRead(m);
    DrawPlayers(c);
    uint32_t h = 0, key = c.scratch + kKeyOff;
    PlantKey(c, n, key);
    if (!Chance(r, 4)) {
        const uint32_t e = SomeEntity(c);
        PlantSampleValues(c, e);
        if (c.sample == nullptr) DrawGateEntity(c, n, e);
        h = e + 0xAC;
        if (!Chance(r, 3)) key = e + 0x168;                          // the entity's own coordinate
    }
    if (c.sample == nullptr) {
        StirPieceList(c, n);
        StirBttFlags(c, n);
        if (Chance(r, 4)) RqStirResidency(c, n);
    }
    a.a[0] = h;
    a.a[1] = key;
}

uint32_t NativeRoadGate(NativeEnv& env, const Args& a) {
    rr::sim::GuestRam g = View(env);
    return RqDone(env, g, rr::sim::RoadGate(g, a.a[0], a.a[1]));
}

// ---------------------------------------------------------------- SLUS 0x80037524
// A cursor on a resident object's piece (a core piece now and then), the along put at and one unit
// past both ends of the piece's range, the object's DIST count and table or the sub-object's DIST
// run cut now and then.
void SetupSeatAlong(CaseContext& c, Args& a) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    const RqNet n = RqRead(m);
    const uint32_t cur = c.scratch + kCurOff;
    if (c.sample != nullptr) {
        const uint32_t e = m.PeekWord(kPoolBasePtr) + 1096u * (r.U32() % 18u);
        uint8_t copy[32];
        m.ReadBlock(e + 0x148, copy, 32);
        m.WriteBlock(cur, copy, 32);
    } else if (Chance(r, 5)) {
        RqPlantCoreCursor(c, n, cur, (r.Next() & 1u) != 0, false);
    } else {
        RqPlantCursor(c, n, cur);
    }
    const uint32_t obj = m.PeekWord(cur), piece = m.PeekWord(cur + 4), sub = m.PeekWord(cur + 8);
    const int32_t cum = GetS32(m, m.PeekWord(cur + 12) + 40);
    const int32_t lo = GetS32(m, piece + 24) >> 16, hi = GetS16(m, piece + 30);
    int32_t target;                                                  // the integer the search sees
    switch (r.U32() % 6u) {
        case 0: target = lo - 1; break;
        case 1: target = lo; break;
        case 2: target = hi; break;
        case 3: target = hi + 1; break;
        default: target = lo + static_cast<int32_t>(r.U32() % static_cast<uint32_t>(std::max(1, hi - lo + 1))); break;
    }
    const int32_t sum = static_cast<int32_t>((U(target) << 16) | (r.U32() & 0xFFFFu));
    int32_t along = static_cast<int32_t>(U(sum) - U(cum));           // cum + along = sum ...
    if (along <= 0 && Chance(r, 2)) along = static_cast<int32_t>(U(cum) - U(sum));  // ... or cum - along
    if (Chance(r, 12)) along = r.S32() >> (r.U32() % 12u);
    if (c.sample == nullptr && Chance(r, 8)) {
        // the piece's range moved past the along the search will see: its start and end beyond it
        const int32_t seen = static_cast<int32_t>(((along > 0) ? U(cum) + U(along) : U(cum) - U(along))) >> 16;
        const int32_t lo2 = seen + 1 + static_cast<int32_t>(r.U32() % 3u);
        PutS32(m, piece + 24, static_cast<int32_t>(U(lo2) << 16));
        PutS16(m, piece + 30, static_cast<int16_t>(lo2 + static_cast<int32_t>(r.U32() % 3u)));
    }
    if (c.sample == nullptr) {
        if (Chance(r, 12)) PutS16(m, obj + 0x1C, static_cast<int16_t>(-static_cast<int32_t>(r.U32() % 2u)));
        if (Chance(r, 12)) m.PokeWord(obj + 0x44, 0);
        if (Chance(r, 8)) PutS16(m, sub + 22, static_cast<int16_t>(r.U32() % 3u));
    }
    a.a[0] = Chance(r, 30) ? 0u : cur;
    a.a[1] = U(along);
}

uint32_t NativeSeatAlong(NativeEnv& env, const Args& a) {
    rr::sim::GuestRam g = View(env);
    return RqDone(env, g, rr::sim::SeatAlong(g, a.a[0], static_cast<int32_t>(a.a[1])));
}

// ---------------------------------------------------------------- SLUS 0x8003A700 / 0x8003A468
// The node arm needs a route record for the junction's node whose turn names one of the core
// object's sub-objects: a record of this machine's route is re-keyed to the node and aimed at the
// core cursor's turn (RrAimTurn), or left as it is.
void AimNodeRecord(CaseContext& c, const RqNet& n, uint32_t key) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    if ((m.PeekWord(key) >> 16) != 1u || Chance(r, 5)) return;
    const int32_t count = GetS16(m, rrt::kCount);
    const uint32_t recs = m.PeekWord(rrt::kRecords);
    if (count <= 0 || recs < 0x80000000u) return;
    const uint32_t R = recs + 120u * RqPick(r, count);
    m.PokeWord(R, m.PeekWord(key) & 0xFFFFu);
    const uint32_t tmp = c.scratch + kCurOff + 64u;                     // a core cursor to aim the turn by
    if (RqPlantCoreCursor(c, n, tmp, (r.Next() & 1u) != 0, false) != 0) RrAimTurn(c, n, tmp, R);
    if (Chance(r, 5)) PutS32(m, R + 16, 0);
}

// A junction core object of this machine (object kind 1, half 0), or 0.
uint32_t CoreObject(const Memory& m, const RqNet& n) {
    for (uint32_t obj : n.resident)
        if (GetS16(m, obj + 16) == 1 && m.PeekWord(obj + 12) == 0) return obj;
    return 0;
}

// Every route record keyed `node` re-keyed to nothing: RouteLegOfRoad(node) then answers 0.
void DropNodeRecords(Memory& m, uint32_t node) {
    const int32_t count = GetS16(m, rrt::kCount);
    const uint32_t recs = m.PeekWord(rrt::kRecords);
    if (count <= 0 || recs < 0x80000000u) return;
    for (int32_t i = 0; i < count && i < 64; ++i)
        if ((m.PeekWord(recs + 120u * U(i)) & 0xFFFFu) == (node & 0xFFFFu)) m.PokeWord(recs + 120u * U(i), 0x7F);
}

void SetupCursorSeat(CaseContext& c, Args& a) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    const RqNet n = RqRead(m);
    uint32_t key = c.scratch + kKeyOff, cur = c.scratch + kCurOff;
    RrNoise(c, cur, 32);
    uint32_t obj = n.resident.empty() ? 0u : n.resident[r.U32() % n.resident.size()];
    if (c.sample != nullptr) {
        // a dormant bike's own coordinate and cursor, as the window test hands them over
        const uint32_t e = m.PeekWord(kPoolBasePtr) + 1096u * (1u + r.U32() % 17u);
        PlantSampleValues(c, e);
        key = e + 0x168;
        cur = e + 0x148;
    } else {
        PlantKey(c, n, key);
        AimNodeRecord(c, n, key);
        if (Chance(r, 3)) RqStirRoute(c, n);
        if (Chance(r, 20)) obj = 0;
        if (Chance(r, 8) && obj != 0) {                              // a node key that names this object
            m.PokeWord(key, (1u << 16) | (m.PeekWord(obj + 8) & 0xFFFFu));
            AimNodeRecord(c, n, key);
        }
        if (Chance(r, 6)) {                                          // the core, its own node key, its turn
            const uint32_t core = CoreObject(m, n);                  // aimed - and its IPT_ record gone
            if (core != 0) {
                obj = core;
                m.PokeWord(key, (1u << 16) | (m.PeekWord(core + 8) & 0xFFFFu));
                AimNodeRecord(c, n, key);
                const uint32_t ipt = RrIptOf(m, n, core);
                if (ipt != 0 && Chance(r, 2)) PutS16(m, ipt, static_cast<int16_t>(0x7000 + (r.U32() % 16u)));
            }
        }
    }
    a.a[0] = obj;
    a.a[1] = key;
    a.a[2] = cur;
}

uint32_t NativeCursorSeat(NativeEnv& env, const Args& a) {
    rr::sim::GuestRam g = View(env);
    return RqDone(env, g, static_cast<uint32_t>(rr::sim::CursorSeat(g, a.a[0], a.a[1], a.a[2], env.sp)));
}

void SetupCursorReseat(CaseContext& c, Args& a) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    const RqNet n = RqRead(m);
    uint32_t key = c.scratch + kKeyOff, cur = c.scratch + kCurOff;
    RrNoise(c, cur, 32);
    if (c.sample != nullptr && Chance(r, 2)) {
        const uint32_t e = m.PeekWord(kPoolBasePtr) + 1096u * (r.U32() % 18u);
        PlantSampleValues(c, e);
        key = e + 0x168;
        cur = e + 0x148;
    } else {
        PlantKey(c, n, key);
    }
    if (c.sample == nullptr) {
        StirPieceList(c, n);
        if (Chance(r, 4)) RqStirResidency(c, n);
        // a junction object's GRPT count cut to 0 now and then (the loop not entered: GRPT "+2" is
        // then read from guest address 2, as on the console)
        if (Chance(r, 16)) {
            for (uint32_t obj : n.resident)
                if (GetS16(m, obj + 16) != 0 && Chance(r, 2)) PutS16(m, obj + 18, 0);
        }
    }
    a.a[0] = key;
    a.a[1] = cur;
}

uint32_t NativeCursorReseat(NativeEnv& env, const Args& a) {
    rr::sim::GuestRam g = View(env);
    return RqDone(env, g, static_cast<uint32_t>(rr::sim::CursorReseat(g, a.a[0], a.a[1])));
}

// ---------------------------------------------------------------- SLUS 0x80039F68
// One entity of this machine through the whole window test: handle, live word, road coordinate
// drawn; the views put at exact distances from its box (its CURRENT box: a dormant one is re-seated
// first, so its distance is the re-seated box's); the player branch's partner, list node and rider
// mount state; a cop's class and the progress bracket.
void SetupRoadWindow(CaseContext& c, Args& a) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    const RqNet n = RqRead(m);
    const uint32_t np = DrawPlayers(c);
    const uint32_t e = SomeEntity(c);
    PlantSampleValues(c, e);
    const bool bike = e >= m.PeekWord(kPoolBasePtr) && e < m.PeekWord(kPoolBasePtr) + 1096u * 18u;
    if (c.sample == nullptr) {
        DrawGateEntity(c, n, e);
        StirPieceList(c, n);
        if (Chance(r, 6)) {                                           // a node key; the node's route record
            PlantKey(c, n, e + 0x168);                                // re-keyed, aimed, or gone
            AimNodeRecord(c, n, e + 0x168);
            if (Chance(r, 2)) RqStirRoute(c, n);
        }
        if (Chance(r, 8)) {                                           // the core's own node key with no
            const uint32_t core = CoreObject(m, n);                   // route record for the node: the
            if (core != 0) {                                          // dormant re-seat fails
                const uint32_t node = m.PeekWord(core + 8) & 0xFFFFu;
                m.PokeWord(e + 0x168, (1u << 16) | node);
                DropNodeRecords(m, node);
            }
        }
        if (Chance(r, 8)) {
            const uint16_t h = static_cast<uint16_t>((6u << 5) | (r.U32() % 32u));   // a collision volume
            WriteU16Bench(m, e + 0xAC, h);
        }
        if (!Chance(r, 3)) m.PokeWord(c.gp + rr::sim::kPopVolumeRadiusGp, (r.U32() % 200u) << 16 | (r.U32() & 0xFFFFu));
    }
    const int32_t vol = static_cast<int32_t>(m.PeekWord(c.gp + rr::sim::kPopVolumeRadiusGp)) >> 16;
    for (uint32_t p = 0; p < 2; ++p)
        if (p < np || Chance(r, 4)) PlaceView(c, p, GetS16(m, e + 0xBA), GetS16(m, e + 0xC2), DistanceNear(r, vol));
    // the player branch: the slot's list node, partner and rider mount state
    const uint32_t pool = m.PeekWord(kPoolBasePtr);
    uint16_t h = 0;
    m.ReadBlock(e + 0xAC, &h, 2);
    if ((h >> 5) == 0) {
        const uint32_t b = pool + 1096u * (h & 0x1Fu);
        if (Chance(r, 4)) m.PokeWord(b + 1088, 0);
        if (Chance(r, 3)) m.PokeWord(b + 856, Chance(r, 3) ? 0u : pool + 1096u * (r.U32() % 18u));
        const uint32_t R = m.PeekWord(b + 852);
        if (R >= 0x80000000u && R < 0x80200000u - 0x260u && Chance(r, 2)) m.PokeWord(R + 604, r.U32() % 5u);
        // the cop bracket: this bike's class 2, its progress and the players' 201 units apart
        if (bike && Chance(r, 3)) {
            const uint32_t rd = m.PeekWord(b + 1084);
            Put8(m, rd + 1, (m.PeekByte(rd + 1) & 0xF0u) | (Chance(r, 4) ? r.U32() % 16u : 2u));
            const int32_t base = static_cast<int32_t>(r.U32() % 0x1000000u);
            PutS32(m, b + 324, base);
            for (uint32_t p = 0; p < 2; ++p) {
                const uint32_t pb = m.PeekWord(rr::sim::kPopPlayerBikes + 4u * p);
                if (pb < 0x80000000u || pb >= 0x80200000u || pb == b) continue;
                const int32_t gap = (201 - 1 + static_cast<int32_t>(r.U32() % 3u)) << 12;
                PutS32(m, pb + 324, base + ((r.Next() & 1u) ? gap : -gap) + static_cast<int32_t>(r.U32() % 4096u));
            }
            Put8(m, b + 928, m.PeekByte(b + 928) & ~0x20u);
        }
    }
    a.a[0] = e + 0xAC;
}

uint32_t NativeRoadWindow(NativeEnv& env, const Args& a) {
    rr::sim::GuestRam g = View(env);
    return RqDone(env, g, rr::sim::RoadWindow(g, a.a[0], env.sp));
}


// ================================================================ the RASHCDG passes' callees
// The unported ones run as the ORIGINAL code on the clone at exactly the stack pointer the port
// computes for the call (bs::ExactOracle; every declaration below has frame 0). The ported ones run
// natively on the clone's own RAM: the remount (crash.h) with ITS unported callees at its own frame,
// the stance event (stance.h, witnessed), the bank switch and seat release (anim.h), the box, the AI
// stack helpers and the target speed (ai.h), the road layer (road_runtime.h, RoadRuntimeNative).
constexpr uint32_t kPopResetBike = 0x8002090C, kPopCopLeave = 0x8002847C, kPopCopDrop = 0x8002820C,
                   kPopCopJoin = 0x80028034, kPopAxisRot = 0x8003FB34, kPopRowsHeading = 0x8007EC30,
                   kPopAttach = 0x80012838, kPopViewSlot = 0x80012884, kPopDismount = 0x800C3104,
                   kPopReleaseObj = 0x80095AEC, kPopReFace = 0x80096564, kPopViewReset = 0x80086AF8,
                   kPopVoice = 0x80018440, kPopBind = 0x800235B0;
constexpr uint32_t kRemountFrame = 48; // `addiu sp,sp,-48` at 0x800903F4
constexpr uint32_t kPushFrame = 40;    // `addiu sp,sp,-40` at 0x800BCA68, the stance event's caller

std::vector<OracleCallee> PopSeamCallees() {
    return {OracleCallee{kPopResetBike, 1},   OracleCallee{kPopCopLeave, 1},
            OracleCallee{kPopCopJoin, 1},     OracleCallee{kPopAxisRot, 3},   OracleCallee{kPopRowsHeading, 1},
            OracleCallee{kPopDismount, 2},
            OracleCallee{kPopReleaseObj, 1},  OracleCallee{kPopReFace, 1},    OracleCallee{kPopViewReset, 1},
            OracleCallee{kPopVoice, 2},       OracleCallee{kPopBind, 2},
            // the ported stance event, witnessed, and the stance layer's own seams under it
            OracleCallee{kAiStance, 3},       OracleCallee{rr::sim::kAnimTransitionCaptureFn, 3},
            OracleCallee{rr::sim::kAnimApplyFrameFn, 1}, OracleCallee{rr::sim::kCombatLeaveFn, 4},
            OracleCallee{rr::sim::kCombatEnterFn, 4}};
}

uint8_t* PopRaw(NativeEnv& env, uint32_t a, uint32_t n) { return bs::RawAt(env.clone->mem, a, n); }

struct PopSeams final : rr::sim::PopulationCallees {
    NativeEnv& env;
    rr::sim::RoadRuntimeNative road;
    explicit PopSeams(NativeEnv& e) : env(e) {}

    bool Seam(uint32_t f, std::initializer_list<uint32_t> args, uint32_t sp, uint32_t* v0 = nullptr) {
        uint32_t a[4] = {0, 0, 0, 0};
        int n = 0;
        for (uint32_t x : args) a[n++] = x;
        const uint32_t r = bs::ExactOracle(env, f, a, n, sp);
        if (v0 != nullptr) *v0 = r;
        return env.failure.empty();
    }
    // A native port that reaches the oracle through a seam of ITS own (the stance layer's pose and
    // combat seams) makes that call at the bench's `sp`: moved to the call site for the duration, so
    // the oracle's frame lies below the caller's live frame words, as in the original.
    template <class F>
    uint32_t AtSp(uint32_t sp, F&& f) {
        const uint32_t saved = env.sp;
        env.sp = sp;
        const uint32_t r = f();
        env.sp = saved;
        return r;
    }

    bool ResetBike(uint32_t e, uint32_t sp) override { return Seam(kPopResetBike, {e}, sp); }
    bool CopLeave(uint32_t e, uint32_t sp) override { return Seam(kPopCopLeave, {e}, sp); }
    bool CopJoin(uint32_t e, uint32_t sp) override { return Seam(kPopCopJoin, {e}, sp); }
    bool AxisRotation(uint32_t axis, int32_t ang, uint32_t out, uint32_t sp) override {
        return Seam(kPopAxisRot, {axis, U(ang), out}, sp);
    }
    bool RowsFromHeading(uint32_t e, uint32_t sp) override { return Seam(kPopRowsHeading, {e}, sp); }
    bool RiderDismount(uint32_t r, int32_t how, uint32_t sp) override { return Seam(kPopDismount, {r, U(how)}, sp); }

    bool StanceEvent(uint32_t ev, uint32_t r, uint32_t p, uint32_t sp) override {
        AtSp(sp, [&] { return AnimNativeStanceEvent(env, ev, r, p, sp); });
        return env.failure.empty();
    }
    bool BankSwitch(uint32_t a, uint32_t bank, uint32_t sp) override {
        AtSp(sp, [&] { return animrows::RunNative(env, [&](rr::sim::StanceLayer& l) { return l.anim().BankSwitch(a, bank); }); });
        return env.failure.empty();
    }
    bool SeatRelease(uint32_t b, uint32_t r, uint32_t idx, uint32_t sp) override {
        AtSp(sp, [&] { return animrows::RunNative(env, [&](rr::sim::StanceLayer& l) { return l.anim().SeatRelease(b, r, idx); }); });
        return env.failure.empty();
    }
    bool BuildObb(uint32_t e) override {
        uint8_t* ep = PopRaw(env, e, 1096);
        if (ep == nullptr) return false;
        rr::sim::EntityView v(ep);
        uint16_t h = 0;
        env.clone->mem.ReadBlock(e + 0xAC, &h, 2);
        int32_t rev = 0;
        if ((h >> 5) == 0) {                                          // only the pool-0 arm reads the owner
            const uint32_t owner = v.U32(rr::sim::ent::kOwner);
            rev = owner != 0 ? static_cast<int32_t>(env.clone->mem.PeekWord(owner + 604)) : 0;
        }
        rr::sim::BuildObb(v, rev);
        return true;
    }
    bool ClearCommands(uint32_t e, uint32_t) override {
        uint8_t* ep = PopRaw(env, e, 1096);
        if (ep == nullptr) return false;
        rr::sim::AiClearCommands(ep);
        return true;
    }
    // AiPushCommand (ai.h) on raw views; `cmd` is 8 bytes of guest memory (the caller's frame), read
    // before and written back after, as the original passes its address.
    bool PushBytes(uint8_t cmd[8], int32_t mode, uint32_t B, uint32_t sp) {
        Memory& m = env.clone->mem;
        uint8_t* e = PopRaw(env, B, 1096);
        const uint32_t R = m.PeekWord(B + 852u);
        uint8_t* rider = PopRaw(env, R, 640);
        const uint8_t* alt = PopRaw(env, kAltKindTable, 8u * 65536u);
        const uint8_t* fight = PopRaw(env, m.PeekWord(kFightRecPtr), 12u * 256u);
        if (e == nullptr || rider == nullptr || alt == nullptr || fight == nullptr) return false;
        struct Sink final : rr::sim::AiStanceSink {
            PopSeams& s;
            uint32_t sp;
            Sink(PopSeams& x, uint32_t y) : s(x), sp(y) {}
            void PlayIdleStance(uint16_t event, uint32_t riderArg) override { s.StanceEvent(event, riderArg, 2, sp); }
        } sink(*this, sp - kPushFrame);
        rr::sim::AiPushEnv pe;
        pe.raceClock = static_cast<int32_t>(m.PeekWord(m.PeekWord(kGameStatePtr) + 0x10u));
        pe.rider = rider;
        pe.riderAddress = R;
        pe.altKindTable = alt;
        pe.fightRecords = fight;
        pe.stance = &sink;
        rr::sim::AiPushCommand(cmd, mode, e, pe);
        return env.failure.empty();
    }
    bool PushCommand(uint32_t cmd, int32_t mode, uint32_t e, uint32_t sp) override {
        uint8_t bytes[8];
        env.clone->mem.ReadBlock(cmd, bytes, 8);
        const bool ok = PushBytes(bytes, mode, e, sp);
        env.clone->mem.WriteBlock(cmd, bytes, 8);
        return ok;
    }
    bool TargetSpeed(uint32_t ea, int32_t dt, uint32_t, int32_t& v0) override {
        Memory& m = env.clone->mem;
        static const uint8_t kZero[1096] = {};
        auto raw = [&](uint32_t a, uint32_t n) -> uint8_t* {
            if (a == 0) return const_cast<uint8_t*>(kZero);
            return PopRaw(env, a, n);
        };
        uint8_t* e = raw(ea, 1096);
        uint8_t* rd = raw(m.PeekWord(ea + rr::sim::ent::kRiderDef), 72);
        uint8_t* st = raw(m.PeekWord(ea + rr::sim::ent::kStats), 512);
        if (e == nullptr || rd == nullptr || st == nullptr) return false;
        rr::sim::AiSpeedEnv s;
        s.gameState = raw(m.PeekWord(kGameStatePtr), 64);
        auto ref = [&](uint32_t b) {
            rr::sim::AiBikeRef r;
            r.entity = raw(b, 1096);
            r.riderDef = raw(b != 0 ? m.PeekWord(b + rr::sim::ent::kRiderDef) : 0u, 72);
            return r;
        };
        s.player1 = ref(m.PeekWord(kPlayer1Ptr));
        s.player2 = ref(m.PeekWord(kPlayer2Ptr));
        s.playerSlot[0] = ref(m.PeekWord(kPlayerArr + 0));
        s.playerSlot[1] = ref(m.PeekWord(kPlayerArr + 4));
        const uint32_t jb = m.PeekWord(kJailbreakPtr);
        s.jailbreakBike = jb != 0 ? raw(jb, 1096) : nullptr;
        s.liveBikes = static_cast<int32_t>(m.PeekWord(kLiveBikes));
        s.bikeCap = static_cast<int32_t>(m.PeekWord(kBikeCap));
        s.spreadDiv = static_cast<int32_t>(m.PeekWord(kSpreadDiv));
        s.raceFlags = m.PeekWord(kAiRaceFlags);
        s.altFlag = m.PeekWord(kAltFlag);
        s.copScale = static_cast<int32_t>(m.PeekWord(kCopScale));
        s.clockStamp = reinterpret_cast<int32_t*>(PopRaw(env, kClockStamp, 4));
        s.spreadBand = reinterpret_cast<int32_t*>(PopRaw(env, kSpreadBand, 4));
        s.tabSpeedClass = reinterpret_cast<const int32_t*>(PopRaw(env, kTabSpeedClass, 256));
        s.tabThinkA = reinterpret_cast<const int32_t*>(PopRaw(env, kTabThinkA, 256));
        s.tabThinkB = reinterpret_cast<const int32_t*>(PopRaw(env, kTabThinkB, 256));
        s.tabCopFlat = reinterpret_cast<const int32_t*>(PopRaw(env, kTabCopFlat, 256));
        s.tabAltA = reinterpret_cast<const int32_t*>(PopRaw(env, kTabAltA, 256));
        s.tabAltB = reinterpret_cast<const int32_t*>(PopRaw(env, kTabAltB, 256));
        s.tabSpeedCap = reinterpret_cast<const int32_t*>(PopRaw(env, kTabSpeedCap, 256));
        s.tabCopBase = reinterpret_cast<const int16_t*>(PopRaw(env, kTabCopBase, 256));
        s.tabCopStep = reinterpret_cast<const int16_t*>(PopRaw(env, kTabCopStep, 256));
        s.tabCopMul = reinterpret_cast<const int32_t*>(PopRaw(env, kTabCopMul, 256));
        s.profile = reinterpret_cast<const int8_t*>(PopRaw(env, kProfile, 60));
        v0 = rr::sim::AiTargetSpeed(e, rd, st, dt, s);
        return true;
    }
    bool Remount(uint32_t b, int32_t fromRoad, uint32_t sp) override;
    bool CellWalker(uint32_t sp) override;
    bool PoliceSched(int32_t acc, uint32_t sp) override;
    bool Budget(int32_t kind, uint32_t sp, uint32_t& v0) override;
    bool ShareFlags(uint32_t flags, uint32_t sp) override;
    bool RoadWalk(uint32_t from, uint32_t out, int32_t dist, uint32_t sp) override;
    bool Spacing(uint32_t rec, uint32_t sp, uint32_t& v0) override;
    bool Release(uint32_t h, int32_t pool, uint32_t sp) override;
    bool ModelBind(uint32_t car, int32_t pool, uint32_t cls, uint32_t sp, uint32_t& v0) override;
    bool CarSetup(uint32_t car, uint32_t sp) override;
    bool NodeLanes(uint32_t obj, int32_t road, uint32_t dirOut, uint32_t sp, uint32_t& v0) override;
    // A row that keeps the road layer's six on ITS seam (the per-bike step's) hands its own object.
    rr::sim::RoadRuntimeCallees* roadOverride = nullptr;
    rr::sim::RoadRuntimeCallees& Road() override { return roadOverride != nullptr ? *roadOverride : road; }
};

// The ported remount (crash.h), its unported callees at ITS frame (entry sp - 48).
struct PopRemount final : rr::sim::RemountCallees {
    PopSeams& s;
    uint32_t sp;
    PopRemount(PopSeams& x, uint32_t y) : s(x), sp(y) {}
    bool RowsFromHeading(uint32_t B) override { return s.Seam(kPopRowsHeading, {B}, sp); }
    bool ResetBikeState(uint32_t B) override { return s.Seam(kPopResetBike, {B}, sp); }
    bool ReleaseRiderObject(uint32_t R) override { return s.Seam(kPopReleaseObj, {R}, sp); }
    bool Attach(uint32_t B, uint32_t R, int32_t kind, int32_t seat) override {   // PORTED (population.h)
        rr::sim::GuestRam g(s.env.clone->mem.ram().data(), s.env.gp);
        rr::sim::Attach(g, B, R, kind, seat);
        return !g.Faulted();
    }
    bool Dismount(uint32_t R, int32_t how) override { return s.Seam(kPopDismount, {R, U(how)}, sp); }
    bool ClearCommands(uint32_t B) override { return s.ClearCommands(B, sp); }
    bool PushCommand(uint16_t op, uint16_t target, int32_t mode, uint32_t B) override {
        // the remount's 8-byte record lives in its own frame with only the first two halfwords
        // written; a push overwrites the rest (crash.h)
        uint8_t cmd[8] = {static_cast<uint8_t>(op), static_cast<uint8_t>(op >> 8), static_cast<uint8_t>(target),
                          static_cast<uint8_t>(target >> 8), 0, 0, 0, 0};
        return s.PushBytes(cmd, mode, B, sp);
    }
    bool ReFace(uint32_t B) override { return s.Seam(kPopReFace, {B}, sp); }
    bool ViewReset(uint32_t view) override { return s.Seam(kPopViewReset, {view}, sp); }
    bool PlayerVoice(uint32_t h, int32_t a1) override { return s.Seam(kPopVoice, {h, U(a1)}, sp); }
    bool PlayerBind(uint32_t h, uint32_t B) override { return s.Seam(kPopBind, {h, B}, sp); }
};

bool PopSeams::Remount(uint32_t b, int32_t fromRoad, uint32_t sp) {
    PopRemount rc(*this, sp - kRemountFrame);
    rr::sim::GuestRam g(env.clone->mem.ram().data(), env.gp);
    const bool ok = rr::sim::Remount(g, b, fromRoad, rc);
    return ok && !g.Faulted() && env.failure.empty();
}

// A refusal of the port that is not already a failure of the case becomes one.
uint32_t PopDone(NativeEnv& env, rr::sim::GuestRam& g, bool ok, const char* what, uint32_t v = 0) {
    if ((!ok || g.Faulted()) && env.failure.empty()) {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "%s refused (view fault %d at 0x%08X)", what, g.Faulted() ? 1 : 0,
                      g.FaultAddress());
        env.failure = buf;
    }
    return v;
}

// ================================================================ the RASHCDG families
constexpr uint32_t kPopCopCount = 0x8009DAF8, kPopRetire = 0x80093FE4, kPopPlacement = 0x8009432C,
                   kPopTransition = 0x80093F94, kPopActivate = 0x80093ED4, kPopActivationPass = 0x80093E6C,
                   kPopRiderChase = 0x80099D48, kPopRiderTransition = 0x800952AC, kPopDowned = 0x800951B8,
                   kPopDownedPass = 0x800950E8, kPopRouteChoice = 0x8003A3CC, kPopRouteWalk = 0x80097518,
                   kPopDormantDrive = 0x80095724;

uint32_t Bike(const Memory& m, uint32_t slot) { return m.PeekWord(kPoolBasePtr) + 1096u * slot; }

// The fields placement and retirement read on bike `e`, drawn over this machine's own values: the
// live word, the handle as a player's, the rider's mount state and live word, the class nibble (a
// cop), the cop bits of +0x3A0, the command stack's depth and top opcode, the finish state and place,
// the lateral / heading / half-width, the view slot, +0x24 bit 27, +0x29C.
void DrawBikeForTransition(CaseContext& c, uint32_t e, bool live) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    const uint32_t rd = m.PeekWord(e + 1084), R = m.PeekWord(e + 852);
    if (live) WriteU16Bench(m, e + 320, Chance(r, 4) ? static_cast<uint16_t>(r.U32() | 1u) : static_cast<uint16_t>(1u + 2u * (r.U32() % 2u)));
    else WriteU16Bench(m, e + 320, Chance(r, 10) ? static_cast<uint16_t>(r.U32() & ~1u) : 0u);
    if (Chance(r, 6)) WriteU16Bench(m, e + 0xAC, static_cast<uint16_t>(r.U32() % 2u));
    if (Chance(r, 2)) m.PokeWord(R + 604, r.U32() % 5u);
    if (Chance(r, 3)) WriteU16Bench(m, R + 320, static_cast<uint16_t>(r.U32() % 2u));
    if (Chance(r, 3)) Put8(m, rd + 1, (m.PeekByte(rd + 1) & 0xF0u) | (Chance(r, 2) ? 2u : r.U32() % 4u));
    Put8(m, e + 928, (m.PeekByte(e + 928) & ~0x30u) | (r.U32() & 0x30u));
    // A cop's rider with no view slot yet (placement then asks SLUS 0x80012884 for one). NAMED BOUND:
    // the slot is FREED the way retirement frees a bike's (object +0x24 = 0, *(0x800CE178) - 1), not
    // just forgotten: with the objects all in use the allocator's answer is not an object, and the
    // ORIGINAL then trapped storing through it in the bank switch (0x80012880) - 20 of 1160 cases.
    // (Only for a cop that is not a player: a player's rider gets no slot there, and the bank
    // switch that follows then works on object 0 - the kernel's words at guest 0 and 4 - and the
    // ORIGINAL trapped storing through the second, in 20 of 1160 cases before this condition.)
    uint16_t hd = 0;
    m.ReadBlock(e + 0xAC, &hd, 2);
    const bool player = hd < m.PeekWord(m.PeekWord(kGameStatePtr) + 0x30u);
    if ((m.PeekByte(rd + 1) & 0xFu) == 2u && !player && Chance(r, 2)) {
        const uint32_t obj = m.PeekWord(R + 540);
        if (obj >= 0x80000000u && obj < 0x80200000u) {
            m.PokeWord(obj + 36, 0);
            m.PokeWord(0x800CE178u, m.PeekWord(0x800CE178u) - 1u);
        }
        m.PokeWord(R + 540, 0);
    }
    // the command stack: depth 1..4, the top opcode 1 or any
    const uint8_t depth = static_cast<uint8_t>(1u + r.U32() % 4u);
    if (Chance(r, 2)) {
        Put8(m, e + 946, depth);
        WriteU16Bench(m, e + 956 + 8u * (depth - 1u), static_cast<uint16_t>(Chance(r, 2) ? 1u : r.U32() % 21u));
    }
    if (Chance(r, 3)) m.PokeWord(rd + 40, Chance(r, 2) ? 0u : r.U32() | 1u);   // finished
    if (Chance(r, 3)) Put8(m, rd + 39, Chance(r, 6) ? 254u : r.U32() % 20u);    // the place
    if (Chance(r, 3)) PutS32(m, e + 344, r.S32() >> (8 + r.U32() % 8u));
    if (Chance(r, 3)) PutS32(m, e + 292, static_cast<int32_t>(r.U32() % 4096u));
    if (Chance(r, 3)) PutS32(m, e + 308, static_cast<int32_t>(r.U32() % 0x40000u));
    if (Chance(r, 3)) PutS32(m, e + 924, static_cast<int32_t>(Chance(r, 2) ? 0xA0000u + r.U32() % 3u - 1u : r.U32() % 0x200000u));
    if (Chance(r, 4)) PutS32(m, e + 668, r.S32() >> 12);
    if (Chance(r, 4)) WriteU16Bench(m, e + 956, static_cast<uint16_t>(r.U32() % 3u));
    if (Chance(r, 4)) m.PokeWord(e + 36, m.PeekWord(e + 36) ^ 0x08000000u);
    if (Chance(r, 5)) {                                              // a view slot to free
        const uint32_t slot = c.scratch + 0x400;
        for (uint32_t i = 0; i < 64; i += 4) m.PokeWord(slot + i, r.U32());
        m.PokeWord(e + 540, slot);
        m.PokeWord(0x800CE178u, r.U32() % 6u);
    }
}

// The race-wide state placement and retirement branch on: race type, phase, player 1's finish and
// class, the bike count, the finish record's along, the cops-out counter and cap.
void DrawRace(CaseContext& c) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    const uint32_t gs = m.PeekWord(kGameStatePtr);
    if (Chance(r, 3)) Put8(m, gs + 4, Chance(r, 3) ? 44u : r.U32() & 0xFFu);
    if (Chance(r, 2)) Put8(m, gs + 57, r.U32() % 5u);
    const uint32_t p1 = m.PeekWord(0x8005B38Cu);
    const uint32_t p1rd = m.PeekWord(p1 + 1084);
    if (Chance(r, 3)) m.PokeWord(p1rd + 40, Chance(r, 2) ? 0u : 1u);
    if (Chance(r, 4)) Put8(m, p1rd + 1, (m.PeekByte(p1rd + 1) & 0xF0u) | (r.U32() % 4u));
    if (Chance(r, 3)) Put8(m, p1rd + 39, r.U32() % 20u);
    if (Chance(r, 3)) PutS32(m, p1 + 364, -GetS32(m, p1 + 364));   // heading the other way
    if (Chance(r, 3)) m.PokeWord(0x8005B1F8u, r.U32() % 24u);
    const uint32_t fin = m.PeekWord(0x800D6188u);
    if (Chance(r, 3)) PutS32(m, fin + 4, static_cast<int32_t>(r.U32() % 0x6000000u));
    if (Chance(r, 5)) m.PokeWord(fin, Chance(r, 2) ? 0x20u : r.U32() % 40u);
    PutS32(m, rr::sim::kPopCopsOut, static_cast<int32_t>(r.U32() % 4u) - 1);
    PutS32(m, rr::sim::kPopCopsOut + 4, static_cast<int32_t>(r.U32() % 4u));
    if (Chance(r, 3)) m.PokeWord(0x8005B244u, r.U32() % 0x800u);
}

// Views at the edges of the window of bike e (or anywhere), for the passes' window tests.
void ViewsNear(CaseContext& c, uint32_t e, uint32_t np) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    for (uint32_t p = 0; p < 2; ++p)
        if (p < np || Chance(r, 4))
            PlaceView(c, p, GetS16(m, e + 0xBA), GetS16(m, e + 0xC2), DistanceNear(r, 80));
}

// ---------------------------------------------------------------- SLUS 0x80012838 / 0x80012884 / 0x8002820C
// Attach on a bike and a rider of this machine (or copies in the scratch block), the seat 0..3 and the
// kind drawn; ViewSlot on the machine's own descriptor with its used count, the objects' flags and the
// rider's kind bits (its +0 record's +0x0E, bits 3..6: 1 and 4 take the table word) drawn; CopDrop on a
// bike with +0x24 drawn.
void SetupAttach(CaseContext& c, Args& a) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    uint32_t b = Bike(m, r.U32() % 18u), rd = m.PeekWord(b + 852);
    if (Chance(r, 3)) {
        b = c.scratch + 0x800;
        rd = c.scratch + 0xC80;
        RrNoise(c, b, 0x60);
        RrNoise(c, rd, 0x60);
    }
    a.a[0] = b;
    a.a[1] = rd;
    a.a[2] = Chance(r, 3) ? r.U32() : r.U32() % 3u;
    a.a[3] = r.U32() % 4u;
}
uint32_t NativeAttach(NativeEnv& env, const Args& a) {
    rr::sim::GuestRam g = View(env);
    rr::sim::Attach(g, a.a[0], a.a[1], static_cast<int32_t>(a.a[2]), static_cast<int32_t>(a.a[3]));
    return PopDone(env, g, true, "Attach");
}

void SetupViewSlot(CaseContext& c, Args& a) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    const uint32_t desc = 0x800CE170u;
    const uint32_t cap = m.PeekWord(desc + 12), base = m.PeekWord(desc);
    for (uint32_t i = 0; i < cap && i < 64; ++i)                     // objects freed or taken
        if (Chance(r, 6)) m.PokeWord(base + 2108u * i + 36u, Chance(r, 2) ? 0u : 0x11u);
    const uint64_t k = r.Next() % 5u;
    if (k == 0) m.PokeWord(desc + 8, cap);                            // none free by the count
    else if (k == 1) m.PokeWord(desc + 8, r.U32() % (cap + 1u));
    const uint32_t rider = kRider0 + kRiderBytes * (r.U32() % 18u);
    const uint32_t rec = m.PeekWord(rider);
    if (rec >= 0x80000000u && rec < 0x80200000u && Chance(r, 2)) {
        uint16_t h = 0;
        m.ReadBlock(rec + 14, &h, 2);
        static const uint16_t kKind[4] = {1, 4, 2, 0};
        h = static_cast<uint16_t>((h & ~0x78u) | (kKind[r.U32() % 4u] << 3));
        WriteU16Bench(m, rec + 14, h);
    }
    a.a[0] = desc;
    a.a[1] = rider;
}
uint32_t NativeViewSlot(NativeEnv& env, const Args& a) {
    rr::sim::GuestRam g = View(env);
    const uint32_t v = rr::sim::ViewSlot(g, a.a[0], a.a[1]);
    return PopDone(env, g, true, "ViewSlot", v);
}

void SetupCopDrop(CaseContext& c, Args& a) {
    const uint32_t e = Bike(*c.mem, c.rng->U32() % 18u);
    c.mem->PokeWord(e + 36, c.rng->U32());
    a.a[0] = e;
}
uint32_t NativeCopDrop(NativeEnv& env, const Args& a) {
    rr::sim::GuestRam g = View(env);
    rr::sim::CopDrop(g, a.a[0]);
    return PopDone(env, g, true, "CopDrop");
}

// ---------------------------------------------------------------- 0x8009DAF8
void SetupCopCount(CaseContext& c, Args& a) {
    Rng& r = *c.rng;
    PutS32(*c.mem, rr::sim::kPopCopsOut, Chance(r, 5) ? r.S32() : static_cast<int32_t>(r.U32() % 7u) - 2);
    PutS32(*c.mem, rr::sim::kPopCopsOut + 4, Chance(r, 5) ? r.S32() : static_cast<int32_t>(r.U32() % 5u) - 1);
    a.a[0] = Chance(r, 4) ? r.U32() : (Chance(r, 2) ? 1u : 0xFFFFFFFFu);
}

uint32_t NativeCopCount(NativeEnv& env, const Args& a) {
    rr::sim::GuestRam g = View(env);
    return RqDone(env, g, U(rr::sim::CopCount(g, static_cast<int32_t>(a.a[0]))));
}

// ---------------------------------------------------------------- 0x80093FE4 / 0x8009432C / 0x80093F94
void SetupRetire(CaseContext& c, Args& a) {
    Memory& m = *c.mem;
    const uint32_t e = Bike(m, c.rng->U32() % 18u);
    DrawPlayers(c);
    DrawRace(c);
    DrawBikeForTransition(c, e, false);
    PlantSampleValues(c, e);
    a.a[0] = Chance(*c.rng, 40) ? 0u : e;
}

void SetupPlacement(CaseContext& c, Args& a) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    const RqNet n = RqRead(m);
    const uint32_t e = Bike(m, Chance(r, 8) ? 0u : 1u + r.U32() % 17u);
    DrawPlayers(c);
    DrawRace(c);
    DrawBikeForTransition(c, e, true);
    PlantSampleValues(c, e);
    if (c.sample == nullptr && Chance(r, 3)) PlantKey(c, n, e + 0x168);
    a.a[0] = Chance(r, 40) ? 0u : e;
}

void SetupTransition(CaseContext& c, Args& a) {
    Rng& r = *c.rng;
    if (Chance(r, 2)) SetupPlacement(c, a);
    else SetupRetire(c, a);
    a.a[1] = Chance(r, 4) ? r.U32() : r.U32() % 4u;                  // `old`: bit 0 either way
}

uint32_t NativeRetire(NativeEnv& env, const Args& a) {
    rr::sim::GuestRam g = View(env);
    PopSeams s(env);
    return PopDone(env, g, rr::sim::Retire(g, a.a[0], env.sp, s), "Retire");
}
uint32_t NativePlacement(NativeEnv& env, const Args& a) {
    rr::sim::GuestRam g = View(env);
    PopSeams s(env);
    return PopDone(env, g, rr::sim::Placement(g, a.a[0], env.sp, s), "Placement");
}
uint32_t NativeTransition(NativeEnv& env, const Args& a) {
    rr::sim::GuestRam g = View(env);
    PopSeams s(env);
    return PopDone(env, g, rr::sim::Transition(g, a.a[0], a.a[1], env.sp, s), "Transition");
}

// ---------------------------------------------------------------- 0x80093ED4 / 0x80093E6C
// One bike (Activate) or all eighteen (the pass): live words drawn, the views put at the window's
// edges from one of them, so wakes, retirements and parked cops all happen.
void SetupActivate(CaseContext& c, Args& a) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    const RqNet n = RqRead(m);
    const uint32_t np = DrawPlayers(c);
    DrawRace(c);
    const uint32_t e = Bike(m, Chance(r, 8) ? 0u : 1u + r.U32() % 17u);
    DrawBikeForTransition(c, e, Chance(r, 2));
    PlantSampleValues(c, e);
    if (c.sample == nullptr && Chance(r, 3)) PlantKey(c, n, e + 0x168);
    ViewsNear(c, e, np);
    a.a[0] = e;
    a.a[1] = Chance(r, 6) ? (Chance(r, 2) ? 1u : r.U32()) : 0u;
}

void SetupActivationPass(CaseContext& c, Args& a) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    const RqNet n = RqRead(m);
    const uint32_t np = DrawPlayers(c);
    DrawRace(c);
    for (uint32_t s = 1; s < 18; ++s) {
        if (!Chance(r, 3)) continue;
        const uint32_t e = Bike(m, s);
        DrawBikeForTransition(c, e, Chance(r, 3));
        if (c.sample == nullptr && Chance(r, 4)) PlantKey(c, n, e + 0x168);
    }
    ViewsNear(c, Bike(m, r.U32() % 18u), np);
    (void)a;
}

uint32_t NativeActivate(NativeEnv& env, const Args& a) {
    rr::sim::GuestRam g = View(env);
    PopSeams s(env);
    return PopDone(env, g, rr::sim::Activate(g, a.a[0], a.a[1], env.sp, s), "Activate");
}
uint32_t NativeActivationPass(NativeEnv& env, const Args&) {
    rr::sim::GuestRam g = View(env);
    PopSeams s(env);
    return PopDone(env, g, rr::sim::ActivationPass(g, env.sp, s), "ActivationPass");
}

// ---------------------------------------------------------------- the downed riders
// The race graph at *(gp+472): road and node ids and their records.
struct RaceGraph {
    uint32_t nodes = 0, roads = 0;
    int32_t nNodes = 0, nRoads = 0;
};
RaceGraph ReadGraph(const Memory& m, uint32_t gp) {
    RaceGraph gr;
    const uint32_t base = m.PeekWord(gp + rr::sim::kRaceGraphGp);
    gr.nNodes = GetS32(m, base + 0x0C);
    gr.nRoads = GetS32(m, base + 0x10);
    gr.nodes = m.PeekWord(base + 0x14);
    gr.roads = m.PeekWord(base + 0x18);
    return gr;
}

// A road coordinate on the race graph at `at`: a road (the along inside, past either end, or at 0)
// or a node; `near` makes it a road or node joined to the coordinate at `near`, when there is one.
void PlantGraphKey(CaseContext& c, const RaceGraph& gr, uint32_t at, uint32_t nearKey) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    uint32_t key;
    if (nearKey != 0xFFFFFFFFu && Chance(r, 2) && gr.nRoads > 0 && gr.nNodes > 0) {
        if ((nearKey >> 16) == 0) {                                   // a node at one of its ends
            const uint32_t rec = gr.roads + 16u * (nearKey & 0xFFFFu);
            key = (1u << 16) | (m.PeekWord(rec + ((r.Next() & 1u) ? 8u : 12u)) & 0xFFFFu);
            if (Chance(r, 2)) {                                       // ... or a road that node joins
                const uint32_t node = gr.nodes + 40u * (key & 0xFFFFu);
                const uint32_t deg = std::min<uint32_t>(m.PeekWord(node + 4), 4u);
                if (deg != 0) key = m.PeekWord(node + 8u + 8u * (r.U32() % deg)) & 0xFFFFu;
            }
        } else {                                                      // a road the node joins
            const uint32_t node = gr.nodes + 40u * (nearKey & 0xFFFFu);
            const uint32_t deg = std::min<uint32_t>(m.PeekWord(node + 4), 4u);
            key = deg != 0 ? (m.PeekWord(node + 8u + 8u * (r.U32() % deg)) & 0xFFFFu) : 0u;
        }
    } else if (Chance(r, 3) && gr.nNodes > 0) {
        key = (1u << 16) | (r.U32() % static_cast<uint32_t>(gr.nNodes));
    } else {
        key = gr.nRoads > 0 ? r.U32() % static_cast<uint32_t>(gr.nRoads) : 0u;
    }
    m.PokeWord(at, key);
    int32_t along = static_cast<int32_t>(r.U32() % 0x4000000u);
    if ((key >> 16) == 0 && gr.nRoads > 0) {
        const int32_t len = static_cast<int32_t>((m.PeekWord(gr.roads + 16u * (key & 0xFFFFu) + 4) >> 6) << 16);
        switch (r.U32() % 5u) {
            case 0: along = len + static_cast<int32_t>(r.U32() % 0x60000u) - 0x30000; break;
            case 1: along = static_cast<int32_t>(r.U32() % 0x60000u) - 0x30000; break;
            case 2: along = len > 0 ? static_cast<int32_t>(r.U32() % static_cast<uint32_t>(len)) : 0; break;
            case 3: along = (len >> 1) + static_cast<int32_t>(r.U32() % 3u) - 1; break;
            default: break;
        }
    }
    PutS32(m, at + 8, along);
}

uint32_t Rider(uint32_t slot) { return kRider0 + kRiderBytes * slot; }

void SetupRiderChase(CaseContext& c, Args& a) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    const RaceGraph gr = ReadGraph(m, c.gp);
    const uint32_t rr_ = Rider(r.U32() % 18u);
    const uint32_t B = m.PeekWord(rr_ + 596);
    if (c.sample == nullptr || Chance(r, 2)) {
        PlantGraphKey(c, gr, B + 0x168, 0xFFFFFFFFu);
        if (Chance(r, 4)) {                                           // the same road id
            m.PokeWord(rr_ + 0x168, m.PeekWord(B + 0x168) | (Chance(r, 4) ? 0x10000u : 0u));
            PutS32(m, rr_ + 0x170, GetS32(m, B + 0x170) + (r.S32() >> 12));
        } else {
            PlantGraphKey(c, gr, rr_ + 0x168, m.PeekWord(B + 0x168));
        }
    }
    a.a[0] = rr_;
}

uint32_t NativeRiderChase(NativeEnv& env, const Args& a) {
    rr::sim::GuestRam g = View(env);
    rr::sim::RiderChase(g, a.a[0]);
    return PopDone(env, g, true, "RiderChase");
}

// A downed rider of this machine: mount state 3 or 4, its live word, +0x228 bit 26, +0x23C bits 4
// and 5, its handle as a player's rider now and then, its road coordinate on a resident road (so it
// can be re-seated) or not, its box near or far from the views; its bike live or dormant.
uint32_t DrawDownedRider(CaseContext& c, const RqNet& n, uint32_t np) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    const uint32_t rr_ = Rider(r.U32() % 18u);
    const uint32_t B = m.PeekWord(rr_ + 596);
    m.PokeWord(rr_ + 604, 3u + r.U32() % 2u);
    WriteU16Bench(m, rr_ + 320, Chance(r, 2) ? 0u : static_cast<uint16_t>(Chance(r, 4) ? r.U32() : 1u));
    m.PokeWord(rr_ + 552, (m.PeekWord(rr_ + 552) & ~0x04000000u) | (Chance(r, 3) ? 0x04000000u : 0u));
    Put8(m, rr_ + 572, (m.PeekByte(rr_ + 572) & ~0x20u) | (Chance(r, 4) ? 0x20u : 0u));
    if (Chance(r, 6)) WriteU16Bench(m, rr_ + 0xAC, static_cast<uint16_t>((1u << 5) | (r.U32() % 2u)));
    if (Chance(r, 2)) PlantKey(c, n, rr_ + 0x168);
    WriteU16Bench(m, B + 320, Chance(r, 2) ? 0u : 1u);
    PlantSampleValues(c, rr_);
    ViewsNear(c, rr_, np);
    return rr_;
}

void SetupDowned(CaseContext& c, Args& a) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    const RqNet n = RqRead(m);
    const uint32_t np = DrawPlayers(c);
    DrawRace(c);
    a.a[0] = DrawDownedRider(c, n, np);
    a.a[1] = Chance(r, 6) ? (Chance(r, 2) ? 1u : r.U32()) : 0u;
}

void SetupRiderTransition(CaseContext& c, Args& a) {
    Rng& r = *c.rng;
    SetupDowned(c, a);
    a.a[1] = Chance(r, 4) ? r.U32() : r.U32() % 4u;
}

void SetupDownedPass(CaseContext& c, Args& a) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    const RqNet n = RqRead(m);
    const uint32_t np = DrawPlayers(c);
    DrawRace(c);
    const uint32_t k = r.U32() % 4u;
    for (uint32_t i = 0; i < k; ++i) DrawDownedRider(c, n, np);
    // a passenger: the owner's +0x23C bit 4 with +0x358 another bike
    if (Chance(r, 3)) {
        const uint32_t e = Bike(m, 1u + r.U32() % 17u);
        const uint32_t R = m.PeekWord(e + 852);
        Put8(m, R + 572, m.PeekByte(R + 572) | 0x10u);
        m.PokeWord(e + 856, Bike(m, 1u + r.U32() % 17u));
    }
    (void)a;
}

uint32_t NativeDowned(NativeEnv& env, const Args& a) {
    rr::sim::GuestRam g = View(env);
    PopSeams s(env);
    return PopDone(env, g, rr::sim::Downed(g, a.a[0], a.a[1], env.sp, s), "Downed");
}
uint32_t NativeRiderTransition(NativeEnv& env, const Args& a) {
    rr::sim::GuestRam g = View(env);
    PopSeams s(env);
    return PopDone(env, g, rr::sim::RiderTransition(g, a.a[0], a.a[1], env.sp, s), "RiderTransition");
}
uint32_t NativeDownedPass(NativeEnv& env, const Args&) {
    rr::sim::GuestRam g = View(env);
    PopSeams s(env);
    return PopDone(env, g, rr::sim::DownedRiderPass(g, env.sp, s), "DownedRiderPass");
}

// ---------------------------------------------------------------- the dormant racers
// A dormant bike of this machine on its route: bound to one of the route records (or not), its
// coordinate on a road of the route, at one of the route's nodes, or anywhere; the records' exits
// (count 0..3) drawn from the graph's roads; the LCG seed drawn.
uint32_t DrawDormantBike(CaseContext& c) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    const RaceGraph gr = ReadGraph(m, c.gp);
    const uint32_t e = Bike(m, 1u + r.U32() % 17u);
    WriteU16Bench(m, e + 320, Chance(r, 10) ? 1u : 0u);
    const int32_t count = GetS16(m, rrt::kCount);
    const uint32_t recs = m.PeekWord(rrt::kRecords);
    if (count > 0 && recs >= 0x80000000u) {
        const uint32_t R = recs + 120u * (r.U32() % static_cast<uint32_t>(count));
        if (!Chance(r, 8)) m.PokeWord(e + 428, R);
        if (Chance(r, 2)) {
            // the record's exits at its node: 0..3 of them, graph roads (one of them the record's key)
            const uint32_t k = r.U32() % 4u;
            PutS32(m, R + 16, static_cast<int32_t>(k));
            for (uint32_t i = 0; i < 4; ++i)
                PutS32(m, R + 84 + 4u * i, static_cast<int32_t>(gr.nRoads > 0 ? r.U32() % static_cast<uint32_t>(gr.nRoads) : 0u));
        }
        const uint64_t where = r.Next() % 4u;
        if (where == 0) {
            m.PokeWord(e + 0x168, (1u << 16) | (m.PeekWord(R) & 0xFFFFu));  // at the record's node
        } else if (where == 1) {
            const int32_t legs = GetS32(m, R + 12);                        // on one of its legs' roads
            if (legs > 0 && legs < 8) m.PokeWord(e + 0x168, m.PeekWord(R + 20u + 16u * (r.U32() % static_cast<uint32_t>(legs))) & 0xFFFFu);
        } else if (where == 2) {
            PlantGraphKey(c, gr, e + 0x168, 0xFFFFFFFFu);
        }
    }
    PutS32(m, e + 0x170, static_cast<int32_t>(r.U32() % 0x8000000u) - (Chance(r, 4) ? 0x4000000 : 0));
    // NAMED BOUND: no leg's end node (+12 / +14) is made -1. The walk's "end of the route" arms
    // (0x80097644, 0x80097674) run only there, and on them the ORIGINAL keeps the remaining distance
    // and walks the same stretch again for ever: with -1 planted it ran past 40 million instructions
    // in 18 of 1160 cases, and no route of this machine has a -1 end. Those four instructions are
    // reachable only on the way into that endless loop, and stay unexecuted here.
    m.PokeWord(c.gp + kRandSeedGpOffset, r.U32());
    if (Chance(r, 10)) PutS16(m, rrt::kCount, -1);
    return e;
}

void SetupRouteChoice(CaseContext& c, Args& a) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    const uint32_t e = DrawDormantBike(c);
    uint32_t node = m.PeekWord(e + 0x168) & 0xFFFFu;
    const uint32_t R = m.PeekWord(e + 428);
    if (R >= 0x80000000u && Chance(r, 2)) node = m.PeekWord(R) & 0xFFFFu;
    a.a[0] = e;
    a.a[1] = Chance(r, 8) ? r.U32() % 40u : node;
}

void SetupRouteWalk(CaseContext& c, Args& a) {
    Rng& r = *c.rng;
    const uint32_t e = DrawDormantBike(c);
    a.a[0] = e;
    const uint64_t k = r.Next() % 5u;
    a.a[1] = k == 0 ? r.U32() : (k == 1 ? 0u : static_cast<uint32_t>(r.U32() % (k == 2 ? 0x100000u : 0x8000000u)));
}

void SetupDormantDrive(CaseContext& c, Args& a) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    DrawPlayers(c);
    const uint32_t e = DrawDormantBike(c);
    const uint32_t rd = m.PeekWord(e + 1084);
    if (Chance(r, 4)) Put8(m, rd + 1, (m.PeekByte(rd + 1) & 0xF0u) | (Chance(r, 2) ? 2u : r.U32() % 4u));
    if (Chance(r, 4)) Put8(m, e + 928, m.PeekByte(e + 928) ^ 0x10u);
    if (Chance(r, 6)) Put8(m, rd, m.PeekByte(rd) ^ 0x10u);
    if (Chance(r, 6)) m.PokeWord(rd + 40, 1);
    if (Chance(r, 4)) m.PokeWord(m.PeekWord(e + 852) + 604, r.U32() % 5u);
    if (Chance(r, 4)) m.PokeWord(rr::sim::kPopLiveSeen, r.U32() % 2u);
    a.a[0] = Chance(r, 40) ? 0u : e;
    a.a[1] = Chance(r, 5) ? (r.U32() & 0xFFFFu) : 218u * (1u + r.U32() % 8u);
}

uint32_t NativeRouteChoice(NativeEnv& env, const Args& a) {
    rr::sim::GuestRam g = View(env);
    return RqDone(env, g, U(rr::sim::RouteChoice(g, a.a[0], a.a[1])));
}
uint32_t NativeRouteWalk(NativeEnv& env, const Args& a) {
    rr::sim::GuestRam g = View(env);
    PopSeams s(env);
    return PopDone(env, g, rr::sim::RouteWalk(g, a.a[0], static_cast<int32_t>(a.a[1]), env.sp, s), "RouteWalk");
}
uint32_t NativeDormantDrive(NativeEnv& env, const Args& a) {
    rr::sim::GuestRam g = View(env);
    PopSeams s(env);
    return PopDone(env, g, rr::sim::DormantDrive(g, a.a[0], static_cast<int32_t>(a.a[1]), env.sp, s), "DormantDrive");
}

Row PopRow(const char* name, const char* sig, uint32_t entry, std::function<void(CaseContext&, Args&)> setup,
           std::function<uint32_t(NativeEnv&, const Args&)> native, bool v0, bool seams) {
    Row r{name, sig, entry, std::move(setup), nullptr, v0, std::move(native)};
    r.maxSteps = 40'000'000;
    if (seams) r.oracleCallees = PopSeamCallees();
    return r;
}

// ================================================================ the spawner and the traffic
constexpr uint32_t kPopDensity = 0x8009F44C, kPopCapTest = 0x8008DBA8, kPopDirCoin = 0x8003A98C,
                   kPopLaneOffset = 0x8009CF1C, kPopLaneLateral = 0x8009E768, kPopCarCheck = 0x8009FE90,
                   kPopCarSpawn = 0x8009AD48, kPopTrafficSched = 0x8009CFF4, kPopSchedDispatch = 0x8009B474,
                   kPopSpawnerPass = 0x8008CD88;
// Their unported callees, run by the oracle at the port's exact sp.
constexpr uint32_t kPopCellWalker = 0x8009C308, kPopPoliceSched = 0x8009E89C, kPopBudget = 0x8008CDF4,
                   kPopShareFlags = 0x8009FF24, kPopRoadWalk = 0x80012C1C, kPopSpacing = 0x8009F578,
                   kPopRelease = 0x8008C000, kPopModelBind = 0x8002FAD4, kPopCarSetup = 0x80012FC8,
                   kPopNodeLanes = 0x8003E1E8;

std::vector<OracleCallee> TrafficSeamCallees() {
    std::vector<OracleCallee> v = PopSeamCallees();
    for (const OracleCallee& oc :
         {OracleCallee{kPopCellWalker, 0}, OracleCallee{kPopPoliceSched, 1}, OracleCallee{kPopBudget, 1},
          OracleCallee{kPopShareFlags, 1}, OracleCallee{kPopRoadWalk, 3}, OracleCallee{kPopSpacing, 1},
          OracleCallee{kPopRelease, 2}, OracleCallee{kPopModelBind, 4}, OracleCallee{kPopCarSetup, 2},
          OracleCallee{kPopNodeLanes, 3}})
        v.push_back(oc);
    return v;
}

// PopSeams' traffic half (declared in the struct above).
bool PopSeams::CellWalker(uint32_t sp) { return Seam(kPopCellWalker, {}, sp); }
bool PopSeams::PoliceSched(int32_t acc, uint32_t sp) { return Seam(kPopPoliceSched, {U(acc)}, sp); }
bool PopSeams::Budget(int32_t kind, uint32_t sp, uint32_t& v0) { return Seam(kPopBudget, {U(kind)}, sp, &v0); }
bool PopSeams::ShareFlags(uint32_t flags, uint32_t sp) { return Seam(kPopShareFlags, {flags}, sp); }
bool PopSeams::RoadWalk(uint32_t from, uint32_t out, int32_t dist, uint32_t sp) {
    return Seam(kPopRoadWalk, {from, out, U(dist)}, sp);
}
bool PopSeams::Spacing(uint32_t rec, uint32_t sp, uint32_t& v0) { return Seam(kPopSpacing, {rec}, sp, &v0); }
bool PopSeams::Release(uint32_t h, int32_t pool, uint32_t sp) { return Seam(kPopRelease, {h, U(pool)}, sp); }
bool PopSeams::ModelBind(uint32_t car, int32_t pool, uint32_t cls, uint32_t sp, uint32_t& v0) {
    return Seam(kPopModelBind, {car, U(pool), cls, 0u}, sp, &v0);
}
bool PopSeams::CarSetup(uint32_t car, uint32_t sp) { return Seam(kPopCarSetup, {car, 0u}, sp); }
bool PopSeams::NodeLanes(uint32_t obj, int32_t roadId, uint32_t dirOut, uint32_t sp, uint32_t& v0) {
    return Seam(kPopNodeLanes, {obj, U(roadId), dirOut}, sp, &v0);
}

// The traffic's inputs: the switch, the gate, the percentages and distances, the caps and intervals,
// the accumulators, the clock around both of its gates (300 and 600 x the race's table word), the race
// type's bits (1, 4, 8, 0x10 and 44), the bank, the players' speeds around 131, 5.0 and 0x141DDD, their
// road coordinates (a node now and then), the frozen / finished bits, the LCG seed and the last lane.
void DrawTraffic(CaseContext& c) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    const uint32_t gs = m.PeekWord(kGameStatePtr);
    if (Chance(r, 12)) m.PokeWord(rr::sim::kPopTrafficOn, 0);
    if (Chance(r, 12)) WriteU16Bench(m, rr::sim::kPopCarGate, 0);
    if (Chance(r, 3)) Put8(m, gs + 4, Chance(r, 4) ? 44u : r.U32() & 0x1Fu);
    if (Chance(r, 3)) m.PokeWord(gs + 60, r.U32() % 4u);
    {   // the clock around the two gates
        const uint8_t rt = m.PeekByte(gs + 4);
        const uint32_t bank = m.PeekWord(gs + 60);
        const uint32_t k = (rt & 4u) ? bank + 6u : bank + ((0u - (rt & 1u)) & 3u);
        const uint32_t t = m.PeekWord(rr::sim::kPopClockTab + 4u * k);
        const int32_t g300 = static_cast<int32_t>(t * 300u) >> 16, g600 = static_cast<int32_t>(t * 600u) >> 16;
        switch (r.U32() % 6u) {
            case 0: PutS32(m, gs + 16, g300 + static_cast<int32_t>(r.U32() % 3u)); break;
            case 1: PutS32(m, gs + 16, g600 + static_cast<int32_t>(r.U32() % 3u)); break;
            case 2: PutS32(m, gs + 16, g600 + 1 + static_cast<int32_t>(r.U32() % 20000u)); break;
            case 3: PutS32(m, gs + 16, g300 + 1 + static_cast<int32_t>(r.U32() % static_cast<uint32_t>(std::max(1, g600 - g300)))); break;
            default: break;
        }
    }
    for (uint32_t p = 0; p < 2; ++p) {
        if (Chance(r, 3)) m.PokeWord(rr::sim::kPopTrafficBlk + 4 + 4u * p, r.U32() % 17u);
        if (Chance(r, 3)) m.PokeWord(rr::sim::kPopTrafficBlk + 12 + 4u * p, r.U32() % 8u);
        const uint32_t b = m.PeekWord(rr::sim::kPopPlayerBikes + 4u * p);
        if (b < 0x80000000u || b >= 0x80200000u) continue;
        static const int32_t kSpd[6] = {130, 131, 0x50000, 0x50001, 0x141DDD, 0x141DDE};
        if (Chance(r, 2)) PutS32(m, b + 480, Chance(r, 2) ? kSpd[r.U32() % 6u] : static_cast<int32_t>(r.U32() % 0x300000u));
        const uint32_t rd = m.PeekWord(b + 1084);
        if (Chance(r, 8)) Put8(m, rd, m.PeekByte(rd) ^ 0x40u);
        if (Chance(r, 8)) m.PokeWord(rd + 40, r.U32() % 2u);
        if (Chance(r, 4)) {                                           // at a node: one end of his road
            const RaceGraph gr = ReadGraph(m, c.gp);
            const uint32_t road = m.PeekWord(b + 360) & 0xFFFFu;
            uint32_t node = road;
            if (gr.nRoads > 0 && road < static_cast<uint32_t>(gr.nRoads))
                node = m.PeekWord(gr.roads + 16u * road + ((r.Next() & 1u) ? 8u : 12u)) & 0xFFFFu;
            m.PokeWord(b + 360, 0x10000u | node);
        }
        if (Chance(r, 3)) {                                           // near an end of the road: the walk crosses
            const int32_t a = Chance(r, 2) ? static_cast<int32_t>(r.U32() % 260u) : 1380 + static_cast<int32_t>(r.U32() % 260u);
            PutS32(m, b + 368, (a << 16) | static_cast<int32_t>(r.U32() & 0xFFFFu));
        }
        if (Chance(r, 3)) PutS32(m, b + 364, -GetS32(m, b + 364));
        {   // the player's route record: one of the route's (legs on the roads the walk reaches)
            const int32_t count = GetS16(m, rrt::kCount);
            const uint32_t recs = m.PeekWord(rrt::kRecords);
            if (count > 0 && recs >= 0x80000000u && Chance(r, 2)) m.PokeWord(b + 428, recs + 120u * (r.U32() % static_cast<uint32_t>(count + 1)));
        }
        if (Chance(r, 8)) m.PokeWord(b + 372, 0);
    }
    {   // two players side by side (the sharing of 0x8009FF24: race type bit 4, within 260 units)
        const uint32_t p1 = m.PeekWord(0x8005B38Cu), p2 = m.PeekWord(kPlayer2Ptr);
        if (p2 >= 0x80000000u && p2 < 0x80200000u && p1 != p2 && Chance(r, 2)) {
            for (uint32_t k = 0; k < 3; k += 2)
                PutS32(m, p2 + 0xB8 + 4u * k, GetS32(m, p1 + 0xB8 + 4u * k) + (r.S32() >> 9));
            Put8(m, gs + 4, m.PeekByte(gs + 4) | 0x10u);
        }
    }
    if (Chance(r, 4)) {
        PutS16(m, rr::sim::kPopTrafficBlk + 36, static_cast<int16_t>(r.U32() % 130u));
        PutS16(m, rr::sim::kPopTrafficBlk + 38, static_cast<int16_t>(r.U32() % 130u));
        PutS16(m, rr::sim::kPopTrafficBlk + 40, static_cast<int16_t>(r.U32() % 130u));
    }
    PutS32(m, rr::sim::kPopTrafficAcc, static_cast<int32_t>(r.U32() % 0x100000u));
    m.PokeWord(c.gp + kRandSeedGpOffset, r.U32());
    Put8(m, rr::sim::kPopLastLane, r.U32() % 4u);
}

// Pool 3 with room: the one car kept or removed (as the probe's sched-empty run removes it), the
// control's next-free index and high water drawn, and the slots' handle / +4 words the next-free scan
// tests.
void DrawPool3(CaseContext& c) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    const uint32_t ctrl = rr::sim::kPopPool3Ctrl;
    if (Chance(r, 2)) {
        WriteU16Bench(m, ctrl + 16 + 0xAC, 0);
        WriteU16Bench(m, ctrl + 16 + 0x140, 0);
        m.PokeWord(ctrl, 0);
        m.PokeWord(ctrl + 4, 0);
        m.PokeWord(ctrl + 8, 0xFFFFFFFFu);
    }
    if (Chance(r, 4)) m.PokeWord(ctrl + 4, Chance(r, 6) ? 16u + r.U32() % 3u : r.U32() % 16u);
    if (Chance(r, 4)) m.PokeWord(ctrl + 8, r.U32() % 17u - 1u);
    // NAMED BOUND: a slot's word +4 is kept or cleared, never drawn. The next-free scan only tests it
    // for non-zero, but the model binder under the spawner (SLUS 0x8002FAD4 -> 0x8002FDEC) stores
    // through it: a drawn 1 or 2 made the ORIGINAL trap on a misaligned store to address 2
    // (0x8002FF24) in 34 of 1160 car_spawn cases before this bound.
    for (uint32_t s = 0; s < 16; ++s) {
        if (!Chance(r, 6)) continue;
        if (Chance(r, 2)) m.PokeWord(ctrl + 16 + 512u * s + 4, 0);
        if (Chance(r, 2)) WriteU16Bench(m, ctrl + 16 + 512u * s + 0xAC, static_cast<uint16_t>(Chance(r, 2) ? 0u : 96u + s));
    }
}

// ---------------------------------------------------------------- the leaves
void SetupDensity(CaseContext& c, Args& a) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    DrawPlayers(c);
    const uint32_t gs = m.PeekWord(kGameStatePtr);
    if (!Chance(r, 3)) Put8(m, gs + 4, r.U32() & 0xFFu);
    if (!Chance(r, 3)) PutS32(m, gs + 60, static_cast<int32_t>(r.U32() % 6u) - 1);
    const uint32_t p = Chance(r, 10) ? 2u : r.U32() % 2u;
    const uint32_t b = m.PeekWord(rr::sim::kPopPlayerBikes + 4u * p);
    if (b >= 0x80000000u && b < 0x80200000u) {
        if (Chance(r, 2)) m.PokeWord(b + 360, (Chance(r, 8) ? 0x10000u : 0u) | (Chance(r, 4) ? r.U32() & 0xFFFFu : r.U32() % 40u));
        if (Chance(r, 8)) m.PokeWord(b + 372, 0);
    }
    a.a[0] = p;
}
uint32_t NativeDensity(NativeEnv& env, const Args& a) {
    rr::sim::GuestRam g = View(env);
    rr::sim::Density(g, a.a[0]);
    return PopDone(env, g, true, "Density");
}

void SetupCapTest(CaseContext& c, Args& a) {
    Rng& r = *c.rng;
    PutS32(*c.mem, rr::sim::kPopPool3Ctrl, static_cast<int32_t>(r.U32() % 18u) - 1);
    for (uint32_t p = 0; p < 2; ++p) PutS32(*c.mem, rr::sim::kPopTrafficBlk + 4 + 4u * p, static_cast<int32_t>(r.U32() % 18u) - 1);
    a.a[0] = Chance(r, 8) ? r.U32() % 8u : r.U32() % 2u;
}
uint32_t NativeCapTest(NativeEnv& env, const Args& a) {
    rr::sim::GuestRam g = View(env);
    return RqDone(env, g, U(rr::sim::CapTest(g, a.a[0])));
}

void SetupDirCoin(CaseContext& c, Args& a) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    const uint32_t b = Bike(m, r.U32() % 18u);
    PutS32(m, b + 480, Chance(r, 3) ? 130 + static_cast<int32_t>(r.U32() % 3u) : static_cast<int32_t>(r.U32() % 0x200000u) - 0x1000);
    m.PokeWord(c.gp + kRandSeedGpOffset, r.U32());
    a.a[0] = Chance(r, 4) ? r.U32() : (Chance(r, 2) ? 1u : 0xFFFFFFFFu);
    a.a[1] = b;
}
uint32_t NativeDirCoin(NativeEnv& env, const Args& a) {
    rr::sim::GuestRam g = View(env);
    return RqDone(env, g, U(rr::sim::DirCoin(g, static_cast<int32_t>(a.a[0]), a.a[1])));
}

void SetupLaneOffset(CaseContext& c, Args& a) {
    Rng& r = *c.rng;
    c.mem->PokeWord(c.gp + kRandSeedGpOffset, r.U32());
    a.a[0] = Chance(r, 4) ? r.U32() : r.U32() % 0x60000u;
    a.a[1] = Chance(r, 5) ? r.U32() : static_cast<uint32_t>(static_cast<int32_t>(r.U32() % 9u) - 2);
    static const uint32_t kSel[8] = {2u, 0xFEu, 3u, 0xFDu, 1u, 0xFFu, 4u, 0u};
    a.a[2] = Chance(r, 4) ? r.U32() : kSel[r.U32() % 8u] | (Chance(r, 3) ? r.U32() & 0xFFFFFF00u : 0u);
}
uint32_t NativeLaneOffset(NativeEnv& env, const Args& a) {
    rr::sim::GuestRam g = View(env);
    return RqDone(env, g, U(rr::sim::LaneOffset(g, static_cast<int32_t>(a.a[0]), static_cast<int32_t>(a.a[1]), a.a[2])));
}

// A car (or a bike standing in for one: the same road fields) with its lane, direction and cross-
// section fields drawn, on a road or at a node (a junction core cursor with its turn).
uint32_t DrawLaneEntity(CaseContext& c) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    const RqNet n = RqRead(m);
    const uint32_t e = Chance(r, 3) ? kCar0 : Bike(m, r.U32() % 18u);
    Put8(m, e + 508, Chance(r, 3) ? r.U32() : static_cast<uint32_t>(static_cast<int32_t>(r.U32() % 7u) - 3));
    PutS32(m, e + 364, Chance(r, 2) ? 1 : -1);
    if (Chance(r, 3)) {
        const uint32_t rec = m.PeekWord(e + 372);
        if (rec >= 0x80000000u && rec < 0x80200000u && Chance(r, 2)) {
            PutS16(m, rec + 142, static_cast<int16_t>(static_cast<int32_t>(r.U32() % 3u) - 1));
            PutS16(m, rec + 14, static_cast<int16_t>(static_cast<int32_t>(r.U32() % 3u) - 1));
        }
        PutS16(m, e + 420, static_cast<int16_t>(r.U32() % 5u));
        PutS16(m, e + 408, static_cast<int16_t>(r.U32() % 5u));
        PutS32(m, e + 424, static_cast<int32_t>(r.U32() % 0x60000u));
    }
    if (Chance(r, 6)) m.PokeWord(e + 372, 0);
    if (Chance(r, 4) && RqPlantCoreCursor(c, n, e + 0x148, (r.Next() & 1u) != 0, false) != 0)
        m.PokeWord(e + 0x168, (1u << 16) | (m.PeekWord(m.PeekWord(e + 0x148) + 8) & 0xFFFFu));
    m.PokeWord(c.gp + kRandSeedGpOffset, r.U32());
    return e;
}

void SetupLaneLateral(CaseContext& c, Args& a) { a.a[0] = DrawLaneEntity(c); }
uint32_t NativeLaneLateral(NativeEnv& env, const Args& a) {
    rr::sim::GuestRam g = View(env);
    PopSeams s(env);
    int32_t v = 0;
    const bool ok = rr::sim::LaneLateral(g, a.a[0], env.sp, s, v);
    return PopDone(env, g, ok, "LaneLateral", U(v));
}

void SetupCarCheck(CaseContext& c, Args& a) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    const RqNet n = RqRead(m);
    const uint32_t car = rr::sim::kPopPool3Ctrl + 16u + 512u * (Chance(r, 3) ? r.U32() % 16u : 0u);
    if (Chance(r, 4)) WriteU16Bench(m, car + 0xAC, Chance(r, 2) ? 0u : 0x60u);
    if (Chance(r, 4)) WriteU16Bench(m, car + 0x140, static_cast<uint16_t>(r.U32() % 2u));
    if (Chance(r, 3) && !n.resident.empty()) m.PokeWord(car + 0x148, n.resident[r.U32() % n.resident.size()]);
    if (Chance(r, 3)) RqStirResidency(c, n);
    a.a[0] = Chance(r, 20) ? 0u : car;
}
uint32_t NativeCarCheck(NativeEnv& env, const Args& a) {
    rr::sim::GuestRam g = View(env);
    PopSeams s(env);
    int32_t v = 0;
    const bool ok = rr::sim::CarCheck(g, a.a[0], env.sp, s, v);
    return PopDone(env, g, ok, "CarCheck", U(v));
}

// ---------------------------------------------------------------- 0x8009AD48
// A placement record in the scratch block, as the scheduler builds it (road, along, direction,
// class 0xFFFF, lane 4) or drawn (a node, direction 0, the other lanes, negative lanes), for the
// player or another bike; pool 3 with room.
void SetupCarSpawn(CaseContext& c, Args& a) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    const RqNet n = RqRead(m);
    DrawPlayers(c);
    DrawTraffic(c);
    DrawPool3(c);
    const uint32_t rec = c.scratch + 0x200;
    RrNoise(c, rec, 68);
    if (Chance(r, 4))
        for (uint32_t obj : n.resident) RrStirXsih(c, obj);         // XSIH groups -1: no cross-section
    const uint32_t key = c.scratch + kKeyOff;
    PlantKey(c, n, key);
    const uint32_t pl = Chance(r, 5) ? Bike(m, r.U32() % 18u) : m.PeekWord(rr::sim::kPopPlayerBikes);
    // the along near or far from the player's (the 120.0 guard, on his road and off it)
    if (Chance(r, 2) && (m.PeekWord(pl + 360) >> 16) == 0) {
        m.PokeWord(key, m.PeekWord(pl + 360));
        const int32_t off = (static_cast<int32_t>(r.U32() % 400u) - 200) << 16;
        PutS32(m, key + 8, GetS32(m, pl + 368) + off + static_cast<int32_t>(r.U32() % 0x10000u));
    }
    m.PokeWord(rec + 8, m.PeekWord(key));
    m.PokeWord(rec + 36, m.PeekWord(key + 8));
    PutS16(m, rec + 60, static_cast<int16_t>(Chance(r, 5) ? 0 : (Chance(r, 2) ? 1 : -1)));
    WriteU16Bench(m, rec + 2, Chance(r, 3) ? static_cast<uint16_t>(Chance(r, 2) ? 0u : r.U32() % 8u) : 0xFFFFu);
    if (Chance(r, 4)) Put8(m, m.PeekWord(kGameStatePtr) + 4, m.PeekByte(m.PeekWord(kGameStatePtr) + 4) | 1u);
    static const int16_t kLane[8] = {4, -4, 4, 1, 2, -2, 0, 3};
    PutS16(m, rec + 64, Chance(r, 8) ? static_cast<int16_t>(r.U32()) : kLane[r.U32() % 8u]);
    a.a[0] = rec;
    a.a[1] = pl;
}
uint32_t NativeCarSpawn(NativeEnv& env, const Args& a) {
    rr::sim::GuestRam g = View(env);
    PopSeams s(env);
    uint32_t v = 0;
    const bool ok = rr::sim::CarSpawn(g, a.a[0], a.a[1], env.sp, s, v);
    return PopDone(env, g, ok, "CarSpawn", v);
}

// ---------------------------------------------------------------- 0x8009CFF4 / 0x8009B474 / 0x8008CD88
void SetupTrafficSched(CaseContext& c, Args& a) {
    Rng& r = *c.rng;
    DrawPlayers(c);
    DrawTraffic(c);
    DrawPool3(c);
    a.a[0] = Chance(r, 4) ? r.U32() : 0x10000u + r.U32() % 0x1000u;
}
void SetupSchedDispatch(CaseContext& c, Args& a) {
    Rng& r = *c.rng;
    SetupTrafficSched(c, a);
    a.a[1] = a.a[0];
    static const uint32_t kKind[6] = {3u, 3u, 0u, 1u, 4u, 0xFFFFFFFFu};
    a.a[0] = kKind[r.U32() % 6u];
}
void SetupSpawnerPass(CaseContext& c, Args& a) {
    Rng& r = *c.rng;
    SetupTrafficSched(c, a);
    static const int32_t kAcc[6] = {16384 - 0x884, 16384 - 1, 0xFFFF - 0x884, 0xFFFF, 0x10000, 0};
    PutS32(*c.mem, rr::sim::kPopSpawnAcc, Chance(r, 4) ? static_cast<int32_t>(r.U32() % 0x20000u) : kAcc[r.U32() % 6u]);
    a.a[0] = Chance(r, 5) ? (r.U32() & 0xFFFFu) : 0x884u;
}
uint32_t NativeTrafficSched(NativeEnv& env, const Args& a) {
    rr::sim::GuestRam g = View(env);
    PopSeams s(env);
    return PopDone(env, g, rr::sim::TrafficSched(g, static_cast<int32_t>(a.a[0]), env.sp, s), "TrafficSched");
}
uint32_t NativeSchedDispatch(NativeEnv& env, const Args& a) {
    rr::sim::GuestRam g = View(env);
    PopSeams s(env);
    return PopDone(env, g, rr::sim::SchedDispatch(g, static_cast<int32_t>(a.a[0]), static_cast<int32_t>(a.a[1]), env.sp, s),
                   "SchedDispatch");
}
uint32_t NativeSpawnerPass(NativeEnv& env, const Args& a) {
    rr::sim::GuestRam g = View(env);
    PopSeams s(env);
    return PopDone(env, g, rr::sim::SpawnerPass(g, static_cast<int32_t>(a.a[0]), env.sp, s), "SpawnerPass");
}

Row TrafficRow(const char* name, const char* sig, uint32_t entry, std::function<void(CaseContext&, Args&)> setup,
               std::function<uint32_t(NativeEnv&, const Args&)> native, bool v0) {
    Row r{name, sig, entry, std::move(setup), nullptr, v0, std::move(native)};
    r.maxSteps = 40'000'000;
    r.oracleCallees = TrafficSeamCallees();
    return r;
}

} // namespace pop

// ---------------------------------------------------------------- the per-bike step's region J
// rows_bike_step.inc's TailCallees runs the two passes region J calls (0x80078C58 / 0x80078C60) through
// these: natively (population.h), on the clone's own RAM, from the step's frame, with their unported
// callees on the oracle at the depths the original calls them from (PopSeams), and the road layer's
// six through the step's own object (`road`: StepRoadSeams, the oracle, declared by those rows). The
// declarations the region-J rows need are PopPassSeams; the stance event's is given the frame those
// rows already use.
std::vector<OracleCallee> PopPassSeams(uint32_t stanceFrame) {
    std::vector<OracleCallee> v = pop::PopSeamCallees();
    for (OracleCallee& oc : v)
        if (oc.address == kAiStance) oc.frame = stanceFrame;
    return v;
}
bool PopActivationPassNative(NativeEnv& env, uint32_t sp, rr::sim::RoadRuntimeCallees& road) {
    rr::sim::GuestRam g = pop::View(env);
    pop::PopSeams s(env);
    s.roadOverride = &road;
    const bool ok = rr::sim::ActivationPass(g, sp, s) && !g.Faulted();
    pop::PopDone(env, g, ok, "ActivationPass (region J)");
    return ok;
}
bool PopDownedPassNative(NativeEnv& env, uint32_t sp, rr::sim::RoadRuntimeCallees& road) {
    rr::sim::GuestRam g = pop::View(env);
    pop::PopSeams s(env);
    s.roadOverride = &road;
    const bool ok = rr::sim::DownedRiderPass(g, sp, s) && !g.Faulted();
    pop::PopDone(env, g, ok, "DownedRiderPass (region J)");
    return ok;
}

// The extent [entry, end) of each population row's function, for `--coverage` (the address after the
// delay slot of its last `jr ra`, read out of our own listing; spawn.py's claim `extent` checks the
// same words for the ones it models).
const CoverageRange* PopulationCoverage(const char* rowName) {
    struct Named {
        const char* name;
        CoverageRange range;
    };
    static const Named kRanges[] = {
        {"window_pred", {0x80013110, 0x80013204}},     {"piece_contains", {0x80039CFC, 0x80039DFC}},
        {"road_gate", {0x80039DFC, 0x80039F68}},       {"seat_along", {0x80037524, 0x8003775C}},
        {"cursor_seat", {0x8003A700, 0x8003A98C}},     {"cursor_reseat", {0x8003A468, 0x8003A5F4}},
        {"road_window", {0x80039F68, 0x8003A37C}},     {"cop_count", {0x8009DAF8, 0x8009DB58}},
        {"retire", {0x80093FE4, 0x80094184}},          {"placement", {0x8009432C, 0x800950E8}},
        {"transition", {0x80093F94, 0x80093FE4}},      {"activate", {0x80093ED4, 0x80093F94}},
        {"activation_pass", {0x80093E6C, 0x80093ED4}}, {"rider_chase", {0x80099D48, 0x8009A038}},
        {"rider_transition", {0x800952AC, 0x80095410}}, {"downed", {0x800951B8, 0x800952AC}},
        {"downed_pass", {0x800950E8, 0x800951B8}},     {"route_choice", {0x8003A3CC, 0x8003A468}},
        {"route_walk", {0x80097518, 0x800976C4}},      {"dormant_drive", {0x80095724, 0x80095848}},
        {"density", {0x8009F44C, 0x8009F578}},         {"cap_test", {0x8008DBA8, 0x8008DBCC}},
        {"dir_coin", {0x8003A98C, 0x8003A9D8}},        {"lane_offset", {0x8009CF1C, 0x8009CFF4}},
        {"lane_lateral", {0x8009E768, 0x8009E89C}},    {"car_check", {0x8009FE90, 0x8009FF24}},
        {"car_spawn", {0x8009AD48, 0x8009B474}},       {"traffic_sched", {0x8009CFF4, 0x8009D664}},
        {"sched_dispatch", {0x8009B474, 0x8009B4C0}},  {"spawner_pass", {0x8008CD88, 0x8008CDF4}},
        {"attach", {0x80012838, 0x80012858}},          {"view_slot", {0x80012884, 0x8001298C}},
        {"cop_drop", {0x8002820C, 0x8002823C}},
    };
    for (const Named& n : kRanges)
        if (std::strcmp(n.name, rowName) == 0) return &n.range;
    return nullptr;
}

void AddPopulationRows(std::vector<Row>& rows) {
    rows.push_back(Row{"window_pred", "s32 WindowPred(u32 kind, s32 pos[3], u32 p, s32 extra, [sp+16] sticky)  [SLUS 0x80013110]",
                       pop::kPopWindowPred, pop::SetupWindowPred, nullptr, true, pop::NativeWindowPred});
    rows.push_back(Row{"piece_contains", "s32 PieceContains(Btt *b, RoadKey *k)  [SLUS 0x80039CFC]",
                       pop::kPopPieceContains, pop::SetupPieceContains, nullptr, true, pop::NativePieceContains});
    rows.push_back(Row{"road_gate", "Object *RoadGate(Handle *h, RoadKey *k)  [SLUS 0x80039DFC]",
                       pop::kPopRoadGate, pop::SetupRoadGate, nullptr, true, pop::NativeRoadGate});
    rows.push_back(Row{"seat_along", "Slice *SeatAlong(Cursor *c, s32 along)  [SLUS 0x80037524]",
                       pop::kPopSeatAlong, pop::SetupSeatAlong, nullptr, true, pop::NativeSeatAlong});
    rows.push_back(Row{"cursor_seat", "s32 CursorSeat(Object *o, RoadKey *k, Cursor *c)  [SLUS 0x8003A700]",
                       pop::kPopCursorSeat, pop::SetupCursorSeat, nullptr, true, pop::NativeCursorSeat});
    rows.push_back(Row{"cursor_reseat", "s32 CursorReseat(RoadKey *k, Cursor *c)  [SLUS 0x8003A468]",
                       pop::kPopCursorReseat, pop::SetupCursorReseat, nullptr, true, pop::NativeCursorReseat});
    rows.push_back(Row{"road_window", "s32 RoadWindow(Handle *h)  [SLUS 0x80039F68, the road-window test]",
                       pop::kPopRoadWindow, pop::SetupRoadWindow, nullptr, true, pop::NativeRoadWindow});
    // ---- three SLUS leaves the placement and the retirement reach
    rows.push_back(pop::PopRow("attach", "void Attach(Bike *b, Rider *r, s32 kind, s32 seat)  [SLUS 0x80012838]",
                               pop::kPopAttach, pop::SetupAttach, pop::NativeAttach, false, false));
    rows.push_back(pop::PopRow("view_slot", "Anim *ViewSlot(Desc *d, Rider *r)  [SLUS 0x80012884]", pop::kPopViewSlot,
                               pop::SetupViewSlot, pop::NativeViewSlot, true, false));
    rows.push_back(pop::PopRow("cop_drop", "void CopDrop(Bike *e)  [SLUS 0x8002820C]", pop::kPopCopDrop,
                               pop::SetupCopDrop, pop::NativeCopDrop, false, false));
    // ---- the RASHCDG passes
    rows.push_back(pop::PopRow("cop_count", "s32 CopCount(s32 d)  [RASHCDG 0x8009DAF8]", pop::kPopCopCount,
                               pop::SetupCopCount, pop::NativeCopCount, true, false));
    rows.push_back(pop::PopRow("retire", "void Retire(Bike *e)  [RASHCDG 0x80093FE4]", pop::kPopRetire,
                               pop::SetupRetire, pop::NativeRetire, false, true));
    rows.push_back(pop::PopRow("placement", "void Placement(Bike *e)  [RASHCDG 0x8009432C]", pop::kPopPlacement,
                               pop::SetupPlacement, pop::NativePlacement, false, true));
    rows.push_back(pop::PopRow("transition", "void Transition(Bike *e, s32 old)  [RASHCDG 0x80093F94]",
                               pop::kPopTransition, pop::SetupTransition, pop::NativeTransition, false, true));
    rows.push_back(pop::PopRow("activate", "void Activate(Bike *e, s32 force)  [RASHCDG 0x80093ED4]",
                               pop::kPopActivate, pop::SetupActivate, pop::NativeActivate, false, true));
    rows.push_back(pop::PopRow("activation_pass", "void ActivationPass(void)  [RASHCDG 0x80093E6C]",
                               pop::kPopActivationPass, pop::SetupActivationPass, pop::NativeActivationPass, false, true));
    rows.push_back(pop::PopRow("rider_chase", "void RiderChase(Rider *r)  [RASHCDG 0x80099D48]", pop::kPopRiderChase,
                               pop::SetupRiderChase, pop::NativeRiderChase, false, false));
    rows.push_back(pop::PopRow("rider_transition", "void RiderTransition(Rider *r, s32 old)  [RASHCDG 0x800952AC]",
                               pop::kPopRiderTransition, pop::SetupRiderTransition, pop::NativeRiderTransition, false, true));
    rows.push_back(pop::PopRow("downed", "void Downed(Rider *r, s32 off)  [RASHCDG 0x800951B8]", pop::kPopDowned,
                               pop::SetupDowned, pop::NativeDowned, false, true));
    rows.push_back(pop::PopRow("downed_pass", "void DownedRiderPass(void)  [RASHCDG 0x800950E8]", pop::kPopDownedPass,
                               pop::SetupDownedPass, pop::NativeDownedPass, false, true));
    rows.push_back(pop::PopRow("route_choice", "s32 RouteChoice(Bike *e, u32 node)  [SLUS 0x8003A3CC]",
                               pop::kPopRouteChoice, pop::SetupRouteChoice, pop::NativeRouteChoice, true, false));
    rows.push_back(pop::PopRow("route_walk", "void RouteWalk(Bike *e, s32 dist)  [RASHCDG 0x80097518]",
                               pop::kPopRouteWalk, pop::SetupRouteWalk, pop::NativeRouteWalk, false, false));
    rows.push_back(pop::PopRow("dormant_drive", "void DormantDrive(Bike *e, s32 dt)  [RASHCDG 0x80095724]",
                               pop::kPopDormantDrive, pop::SetupDormantDrive, pop::NativeDormantDrive, false, false));
    // ---- the spawner and the traffic
    rows.push_back(pop::PopRow("density", "void Density(u32 p)  [RASHCDG 0x8009F44C]", pop::kPopDensity,
                               pop::SetupDensity, pop::NativeDensity, false, false));
    rows.push_back(pop::PopRow("cap_test", "s32 CapTest(u32 p)  [RASHCDG 0x8008DBA8]", pop::kPopCapTest,
                               pop::SetupCapTest, pop::NativeCapTest, true, false));
    rows.push_back(pop::PopRow("dir_coin", "s32 DirCoin(s32 dir, Bike *player)  [SLUS 0x8003A98C]", pop::kPopDirCoin,
                               pop::SetupDirCoin, pop::NativeDirCoin, true, false));
    rows.push_back(pop::PopRow("lane_offset", "s32 LaneOffset(s32 spacing, s32 lanes, s8 sel)  [RASHCDG 0x8009CF1C]",
                               pop::kPopLaneOffset, pop::SetupLaneOffset, pop::NativeLaneOffset, true, false));
    rows.push_back(pop::TrafficRow("lane_lateral", "s32 LaneLateral(Car *c)  [RASHCDG 0x8009E768]", pop::kPopLaneLateral,
                                   pop::SetupLaneLateral, pop::NativeLaneLateral, true));
    rows.push_back(pop::TrafficRow("car_check", "s32 CarCheck(Car *c)  [RASHCDG 0x8009FE90]", pop::kPopCarCheck,
                                   pop::SetupCarCheck, pop::NativeCarCheck, true));
    rows.push_back(pop::TrafficRow("car_spawn", "Car *CarSpawn(Rec *r, Bike *player)  [RASHCDG 0x8009AD48]",
                                   pop::kPopCarSpawn, pop::SetupCarSpawn, pop::NativeCarSpawn, true));
    rows.push_back(pop::TrafficRow("traffic_sched", "void TrafficSched(s32 acc)  [RASHCDG 0x8009CFF4]",
                                   pop::kPopTrafficSched, pop::SetupTrafficSched, pop::NativeTrafficSched, false));
    rows.push_back(pop::TrafficRow("sched_dispatch", "void SchedDispatch(s32 kind, s32 acc)  [RASHCDG 0x8009B474]",
                                   pop::kPopSchedDispatch, pop::SetupSchedDispatch, pop::NativeSchedDispatch, false));
    rows.push_back(pop::TrafficRow("spawner_pass", "void SpawnerPass(s32 dt)  [RASHCDG 0x8008CD88]",
                                   pop::kPopSpawnerPass, pop::SetupSpawnerPass, pop::NativeSpawnerPass, false));
}
