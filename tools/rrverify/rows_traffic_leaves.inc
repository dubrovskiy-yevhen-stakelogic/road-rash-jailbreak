// Traffic sub-pass 'leaves' (rows_traffic.inc includes this file): the traffic's small leaves,
// src\game\sim\traffic_leaves.{h,cpp}. Every one of them is a
// complete port (all callees ported and called natively), so no row declares a seam.
//
// INPUT FAMILIES start from THIS machine (rr-race: one player, 18 bikes, one car on road 9) and plant
// on top of it, as rows_population.inc does:
//   * dump-derived: the real entities' road coordinates, cursors, model tables, handles and the real
//     pool 3; the argument drawn among them;
//   * randomised: road coordinates re-drawn on the race graph (roads, nodes, the along at and past both
//     ends), node degrees cut to 0..their own, the route switch 0x800D6182, the LCG seed, the budget globals at and one
//     either side of every bound, pool-3 cars planted around the 10.0 spacing and the lane arms, the
//     two players at exact octagonal distances 259 / 260 with their views' axes and cameras stirred,
//     junction arm directions and lane-record indices, model-table entries and shift nibbles.
// NAMED BOUNDS (each keeps the family inside what the ORIGINAL survives; none bends a port):
//   1. The player count is 0, 1 or 2 (pop::DrawPlayers): ShareFlags writes one word per player into
//      its own 32-byte frame and reads view (p ^ 1).
//   2. RoadWalk runs on the resident race graph with road / node ids inside it; its distance is
//      drawn below 2048.0 in magnitude (plus 0 and INT_MIN). The walk loops until the distance is
//      spent, so an id past the graph (an unchecked GraphRoad / GraphNode index) walks arbitrary
//      memory, and no caller passes more than 180.0.
//   3. Entities handed to ModelExtent / CarSetup have a model chain *(*(e+0x60)+8) that is a RAM
//      pointer, as every live entity of this machine has; the table index is 0..3.
//   4. NodeLanes' synthetic object header is a copy of a resident object's (see SetupNodeLanes).
//   5. RoadWalk's node degrees are only ever cut, never raised (see StirEnds).

namespace tleaf {
constexpr uint32_t kRoadWalk = 0x80012C1C, kBudget = 0x8008CDF4, kSpacing = 0x8009F578, kShareFlags = 0x8009FF24,
                   kNodeLanes = 0x8003E1E8, kModelExtent = 0x80012AEC, kCarSetup = 0x80012FC8,
                   kCursorRoadEnd = 0x8003C520;
// Scratch-block layout (8 KiB; pop:: uses 0x100..0x244, rrt:: 0x600..0xDFC, rq 4096.. and 8000..).
constexpr uint32_t kOutKey = 0xE00;   // RoadWalk's output coordinate, 12 bytes
constexpr uint32_t kFromKey = 0xE20;  // a planted source coordinate, 12 bytes
constexpr uint32_t kXs = 0xE40;       // a synthetic cross-section record (+4 the lane width), 16 bytes
constexpr uint32_t kFlags = 0xE60;    // two traffic flags
constexpr uint32_t kBox = 0xE80;      // ModelExtent's output, 12 bytes
constexpr uint32_t kObj = 0xEA0;      // a synthetic object header, 0x70 bytes
constexpr uint32_t kRec = 0xF20;      // a 68-byte placement record
constexpr uint32_t kCur = 0xF80;      // a 32-byte cursor

using pop::Chance;
using pop::Put8;
using pop::U;

bool InRam(uint32_t a) { return a >= 0x80000000u && a < 0x80200000u && (a & 3u) == 0; }

// ---------------------------------------------------------------- SLUS 0x80012C1C RoadWalk
// The node records at both ends of the coordinate's road: degree cut to 0..its own now and then (0
// and 1 stop the walk). NAMED BOUND 5: never RAISED - an exit slot past the real ones (or past the
// record's four) is not a road id, the walk then indexes the graph with it, and the ORIGINAL trapped
// at 0x80012D44 (GraphNode of a non-id) in 1 of 2695 cases of seed 2 when degrees up to 5 were drawn.
void StirEnds(CaseContext& c, const pop::RaceGraph& gr, uint32_t key) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    if (gr.nRoads <= 0 || gr.nNodes <= 0) return;
    uint32_t nodes[2] = {0xFFFFFFFFu, 0xFFFFFFFFu};
    if ((key >> 16) == 0 && (key & 0xFFFFu) < static_cast<uint32_t>(gr.nRoads)) {
        const uint32_t rd = gr.roads + 16u * (key & 0xFFFFu);
        nodes[0] = m.PeekWord(rd + 8) & 0xFFFFu;
        nodes[1] = m.PeekWord(rd + 12) & 0xFFFFu;
    } else {
        nodes[0] = key & 0xFFFFu;
    }
    for (uint32_t nd : nodes) {
        if (nd >= static_cast<uint32_t>(gr.nNodes) || !Chance(r, 3)) continue;
        const uint32_t deg = m.PeekWord(gr.nodes + 40u * nd + 4);
        if (deg <= 4u) m.PokeWord(gr.nodes + 40u * nd + 4, r.U32() % (deg + 1u));
    }
}

void SetupRoadWalk(CaseContext& c, Args& a) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    const pop::RaceGraph gr = pop::ReadGraph(m, c.gp);
    uint32_t from = c.scratch + kFromKey, out = c.scratch + kOutKey;
    RrNoise(c, out, 12);
    if (c.sample != nullptr && !Chance(r, 4)) {
        // a real coordinate: a bike's (the player's mostly), a rider's or the car's
        const uint64_t k = r.Next() % 4u;
        from = k < 2 ? m.PeekWord(rr::sim::kPopPlayerBikes) + 0x168
             : k == 2 ? pop::Bike(m, r.U32() % 18u) + 0x168 : pop::kCar0 + 0x168;
        if (Chance(r, 3)) out = pop::Bike(m, 16u + r.U32() % 2u) + 0x168;   // the police release's
    } else {
        pop::PlantGraphKey(c, gr, from, Chance(r, 2) ? 0xFFFFFFFFu : m.PeekWord(m.PeekWord(rr::sim::kPopPlayerBikes) + 0x168));
        static const int32_t kDir[5] = {1, -1, 0, 2, -3};
        PutS32(m, from + 4, Chance(r, 8) ? r.S32() : kDir[r.U32() % 5u]);
        // kind 2 / 3: walked as a node (s7 = the low half), so a NODE id goes there (bound 2: a road id
        // read as a node indexed a junk record in 1 of 2695 cases of seed 2 and the ORIGINAL trapped
        // at 0x80012D44)
        if (Chance(r, 10) && gr.nNodes > 0)
            m.PokeWord(from, (r.U32() % static_cast<uint32_t>(gr.nNodes)) | ((2u + r.U32() % 2u) << 16));
        StirEnds(c, gr, m.PeekWord(from));
        if (Chance(r, 8)) out = from;                                // in place
    }
    if (Chance(r, 5)) PutS16(m, rrt::kCount, -1);                    // no route loaded
    m.PokeWord(c.gp + kRandSeedGpOffset, r.U32());
    static const int32_t kDist[8] = {180 << 16, -(145 << 16), -(60 << 16), 60 << 16, 120 << 16, 0, 1, -1};
    int32_t d;
    const uint64_t k = r.Next() % 8u;
    if (k < 4) d = kDist[r.U32() % 8u];
    else if (k < 7) d = static_cast<int32_t>(r.U32() % 0x10000000u) - 0x8000000;   // NAMED BOUND 2
    else d = Chance(r, 2) ? static_cast<int32_t>(0x80000000u) : static_cast<int32_t>(r.U32() % 0x20000u) - 0x10000;
    a.a[0] = from;
    a.a[1] = out;
    a.a[2] = U(d);
}
uint32_t NativeRoadWalk(NativeEnv& env, const Args& a) {
    rr::sim::GuestRam g = pop::View(env);
    rr::sim::RoadWalk(g, a.a[0], a.a[1], static_cast<int32_t>(a.a[2]), env.sp);
    return pop::PopDone(env, g, true, "RoadWalk");
}

// ---------------------------------------------------------------- RASHCDG 0x8008CDF4 Budget
// Each word the seven arms read, at, one below or one above the value it is compared with.
void SetupBudget(CaseContext& c, Args& a) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    auto near = [&](uint32_t addr, int32_t v) {
        if (Chance(r, 3)) return;
        PutS32(m, addr, Chance(r, 8) ? r.S32() : v - 1 + static_cast<int32_t>(r.U32() % 3u));
    };
    if (c.sample == nullptr) {
        if (Chance(r, 2)) {
            const uint8_t b0 = static_cast<uint8_t>(r.U32() % 6u), b1 = static_cast<uint8_t>(b0 - 1u + r.U32() % 3u);
            Put8(m, 0x8005B2B8, Chance(r, 8) ? r.U32() : b0);
            Put8(m, 0x8005B2B9, Chance(r, 8) ? r.U32() : b1);
        }
        if (Chance(r, 3)) m.PokeWord(0x800D4B80, Chance(r, 2) ? 0u : c.scratch);
        near(0x800D4B74, 4);
        const int32_t cap = static_cast<int32_t>(r.U32() % 12u);
        near(0x800D8744, cap);
        near(0x800D4B70, cap);
        near(0x800D4C7C, 0);
        near(0x800D4C6C, 0);
        near(0x800D4C9C, 0);
        near(0x800CF654, 16);
        for (uint32_t b : {0x800CD6C8u, 0x800CE598u}) {
            const int32_t cap2 = static_cast<int32_t>(r.U32() % 12u);
            near(b + 8, cap2);
            near(b + 4, cap2);
        }
        near(0x800D1814, Chance(r, 2) ? 596 : 452);
        if (Chance(r, 3)) m.PokeWord(0x800CD6C4, Chance(r, 2) ? 0u : c.scratch);
        const int32_t t = static_cast<int32_t>(r.U32() % 100u);
        near(0x8005B214, t);
        near(0x800CD6AC, t);
    }
    a.a[0] = Chance(r, 8) ? r.U32() : r.U32() % 8u;
}
uint32_t NativeBudget(NativeEnv& env, const Args& a) {
    rr::sim::GuestRam g = pop::View(env);
    return RqDone(env, g, rr::sim::Budget(g, a.a[0]));
}

// ---------------------------------------------------------------- RASHCDG 0x8009F578 Spacing
// A placement record for the car's road (or another), its along against planted cars: the cars'
// handles, road keys, alongs at and one unit either side of 10.0 from the record's, the "follow"
// bits +0x1FD & 0x11, directions, cross-section records, lane counts 1..4 and lanes.
void SetupSpacing(CaseContext& c, Args& a) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    pop::DrawPool3(c);
    const uint32_t rec = c.scratch + kRec;
    RrNoise(c, rec, 68);
    const uint32_t xs = c.scratch + kXs;
    RrNoise(c, xs, 16);
    PutS32(m, xs + 4, static_cast<int32_t>(r.U32() % 0x60000u));
    const uint32_t key = m.PeekWord(pop::kCar0 + 360);
    m.PokeWord(rec + 8, Chance(r, 6) ? r.U32() % 40u : key);
    PutS32(m, rec + 36, GetS32(m, pop::kCar0 + 368) + (static_cast<int32_t>(r.U32() % 0x400000u) - 0x200000));
    PutS16(m, rec + 60, static_cast<int16_t>(Chance(r, 2) ? 1 : -1));
    if (c.sample == nullptr) {
        if (Chance(r, 3)) PutS32(m, 0x800CF658, static_cast<int32_t>(r.U32() % 17u) - 1);   // the high index
        for (uint32_t s = 0; s < 16; ++s) {
            const uint32_t car = pop::kCar0 + 512u * s;
            if (!Chance(r, 2)) continue;
            WriteU16Bench(m, car + 0xAC, static_cast<uint16_t>(Chance(r, 5) ? 0u : 96u + s));
            m.PokeWord(car + 360, Chance(r, 5) ? r.U32() % 40u : m.PeekWord(rec + 8));
            static const int32_t kOff[7] = {0xA0000, 0xA0001, -0xA0000, -0xA0001, 0x9FFFF, 0, 0x3E70000};
            const int32_t off = Chance(r, 2) ? kOff[r.U32() % 7u] : static_cast<int32_t>(r.U32() % 0x8000000u) - 0x4000000;
            PutS32(m, car + 368, GetS32(m, rec + 36) + off);
            Put8(m, car + 509, Chance(r, 3) ? r.U32() : (Chance(r, 2) ? 0x01u : (Chance(r, 2) ? 0x10u : 0u)));
            PutS32(m, car + 364, Chance(r, 6) ? r.S32() : (Chance(r, 2) ? 1 : -1));
            m.PokeWord(car + 372, Chance(r, 5) ? 0u : (Chance(r, 2) ? xs : m.PeekWord(pop::kCar0 + 372)));
            PutS16(m, car + 420, static_cast<int16_t>(Chance(r, 8) ? r.U32() : r.U32() % 5u));
            PutS16(m, car + 408, static_cast<int16_t>(Chance(r, 8) ? r.U32() : r.U32() % 5u));
            Put8(m, car + 508, Chance(r, 6) ? r.U32() : static_cast<uint32_t>(static_cast<int32_t>(r.U32() % 7u) - 2));
            const int32_t w = GetS32(m, xs + 4);
            PutS32(m, car + 344, Chance(r, 2) ? (Chance(r, 2) ? w : -w) + static_cast<int32_t>(r.U32() % 3u) - 1 : r.S32() >> 8);
        }
    }
    a.a[0] = rec;
}
uint32_t NativeSpacing(NativeEnv& env, const Args& a) {
    rr::sim::GuestRam g = pop::View(env);
    return RqDone(env, g, rr::sim::Spacing(g, a.a[0]));
}

// ---------------------------------------------------------------- RASHCDG 0x8009FF24 ShareFlags
// Two flags; race type bit 0x10; player 2's bike at an exact octagonal distance from player 1's
// (259 / 260 and around); each bike ahead of or behind the OTHER view (its camera and forward axis
// stirred).
void SetupShareFlags(CaseContext& c, Args& a) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    const uint32_t np = pop::DrawPlayers(c);
    const uint32_t gs = m.PeekWord(kGameStatePtr);
    Put8(m, gs + 4, Chance(r, 5) ? (m.PeekByte(gs + 4) & ~0x10u) : (m.PeekByte(gs + 4) | 0x10u));
    const uint32_t flags = c.scratch + kFlags;
    for (uint32_t k = 0; k < 2; ++k) PutS32(m, flags + 4u * k, Chance(r, 4) ? r.S32() : static_cast<int32_t>(r.U32() % 2u));
    if (np != 2 && Chance(r, 2)) m.PokeWord(kPlayer2Ptr, pop::Bike(m, 1u + r.U32() % 17u));
    const uint32_t p1 = m.PeekWord(kPlayer1Ptr), p2 = m.PeekWord(kPlayer2Ptr);
    if (InRam(p1) && InRam(p2) && p1 != p2) {
        static const int32_t kD[6] = {258, 259, 260, 261, 0, 40};
        int32_t dx, dz;
        pop::OffsetsAt(r, Chance(r, 4) ? static_cast<int32_t>(r.U32() % 800u) : kD[r.U32() % 6u], dx, dz);
        PutS16(m, p2 + 186, static_cast<int16_t>(GetS16(m, p1 + 186) + dx));
        PutS16(m, p2 + 194, static_cast<int16_t>(GetS16(m, p1 + 194) + dz));
    }
    if (c.sample == nullptr) {
        for (uint32_t p = 0; p < 2; ++p) {
            const uint32_t v = rr::sim::kPopViewArray + rr::sim::kPopViewStride * p;
            const uint32_t b = m.PeekWord(rr::sim::kPopPlayerBikes + 4u * p);
            if (Chance(r, 2) && InRam(b))                          // the camera near the other bike
                for (uint32_t k = 0; k < 3; ++k)
                    PutS32(m, v + 184 + 4u * k, GetS32(m, b + 184 + 4u * k) + (r.S32() >> (6 + r.U32() % 12u)));
            if (Chance(r, 2))
                for (uint32_t k = 0; k < 3; ++k)
                    PutS16(m, v + 444 + 2u * k, static_cast<int16_t>(Chance(r, 6) ? r.U32() : (r.U32() % 8193u) - 4096u));
        }
    }
    a.a[0] = flags;
}
uint32_t NativeShareFlags(NativeEnv& env, const Args& a) {
    rr::sim::GuestRam g = pop::View(env);
    rr::sim::ShareFlags(g, a.a[0], env.sp);
    return pop::PopDone(env, g, true, "ShareFlags");
}

// ---------------------------------------------------------------- SLUS 0x8003E1E8 NodeLanes
// A junction core object with a road one of its node's arms carries (the arm's direction and the
// lane-record index of the slice it lands on stirred), another resident object, a synthetic header
// or 0; a road nothing carries now and then.
void SetupNodeLanes(CaseContext& c, Args& a) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    const RqNet n = RqRead(m);
    rr::sim::GuestRam g(m.ram().data(), c.gp);                        // address discovery only
    uint32_t obj = pop::CoreObject(m, n);
    const uint64_t k = r.Next() % 10u;
    if (k == 7 && !n.resident.empty()) obj = n.resident[r.U32() % n.resident.size()];
    if (k == 8 && !n.resident.empty()) {
        // NAMED BOUND 4: a synthetic header is a copy of a resident object's, its table pointers
        // (GRPT +0x2C, SUBT +0x30, ...) kept: with noise there the ORIGINAL trapped on a misaligned
        // load in FindRoadPiece (0x80039CC4), 21 of 1139 cases of the gate seed.
        uint8_t copy[0x70];
        m.ReadBlock((obj != 0 && Chance(r, 2)) ? obj : n.resident[r.U32() % n.resident.size()], copy, 0x70);
        obj = c.scratch + kObj;
        m.WriteBlock(obj, copy, 0x70);
        if (Chance(r, 3)) PutS16(m, obj + 16, static_cast<int16_t>(Chance(r, 2) ? 1 : r.U32() % 3u));
        if (Chance(r, 3)) m.PokeWord(obj + 12, Chance(r, 2) ? 0u : r.U32());
        if (Chance(r, 3)) m.PokeWord(obj + 0, r.U32() % static_cast<uint32_t>(std::max(1, n.nJn + 2)));
    }
    if (k == 9) obj = 0;
    int32_t road = static_cast<int32_t>(r.U32() % 40u) - (Chance(r, 10) ? 41 : 0);
    const uint32_t node = obj != 0 && InRam(obj) ? rr::sim::RoadNodeRecord(g, g.S32(obj)) : 0u;
    g.ClearFault();
    if (node != 0) {
        const int32_t arms = g.S16(node + 2);
        if (arms > 0 && arms < 5 && !Chance(r, 6)) {
            const uint32_t arm = node + 8u + 24u * (r.U32() % static_cast<uint32_t>(arms));
            road = g.S16(arm + 4);
            if (c.sample == nullptr && Chance(r, 3))
                PutS16(m, arm + 2, static_cast<int16_t>(Chance(r, 4) ? 0 : (Chance(r, 2) ? 1 : -1)));
            if (c.sample == nullptr && Chance(r, 3)) {
                // the lane-record index of the slice this arm lands on: negative, or another record
                const uint32_t piece = rr::sim::RoadFindPiece(g, obj, road);
                if (piece != 0) {
                    const uint32_t sub = g.U32(obj + 48) + U(g.S16(piece + 20)) * 28u;
                    const uint32_t off = g.S16(arm + 2) > 0 ? U(g.S16(sub + 18)) << 4
                                                            : (U(g.S16(sub + 18) + g.S16(sub + 16)) << 4) - 16u;
                    const uint32_t sl = g.U32(obj + 56) + off;
                    if (!g.Faulted() && InRam(sl & ~3u))
                        PutS16(m, sl + 4, static_cast<int16_t>(Chance(r, 2) ? -1 - static_cast<int32_t>(r.U32() % 2u)
                                                                            : static_cast<int32_t>(r.U32() % 4u)));
                }
                g.ClearFault();
            }
        }
    }
    if (obj == c.scratch + kObj && Chance(r, 3)) {
        // the SUBT base put so that the arm's sub-object address is 0 (0x8003E294)
        const uint32_t piece = rr::sim::RoadFindPiece(g, obj, road);
        if (piece != 0 && !g.Faulted()) m.PokeWord(obj + 48, 0u - U(g.S16(piece + 20)) * 28u);
        g.ClearFault();
    }
    const uint32_t dir = c.scratch + kOutKey;
    RrNoise(c, dir, 4);
    a.a[0] = obj;
    a.a[1] = U(road);
    a.a[2] = dir;
}
uint32_t NativeNodeLanes(NativeEnv& env, const Args& a) {
    rr::sim::GuestRam g = pop::View(env);
    return RqDone(env, g, rr::sim::NodeLanes(g, a.a[0], static_cast<int32_t>(a.a[1]), a.a[2]));
}

// ---------------------------------------------------------------- SLUS 0x80012AEC / 0x80012FC8
// An entity whose model chain is sound (NAMED BOUND 3): the car, a bike or a rider of this machine.
uint32_t ModelEntity(CaseContext& c, uint32_t& idx) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    idx = Chance(r, 4) ? r.U32() % 4u : 0u;
    for (uint32_t tries = 0; tries < 8; ++tries) {
        const uint64_t k = r.Next() % 4u;
        const uint32_t e = k == 0 ? pop::kCar0 : k < 3 ? pop::Bike(m, r.U32() % 18u) : pop::Rider(r.U32() % 18u);
        const uint32_t p = m.PeekWord(e + 96);
        if (!InRam(p) || !InRam(m.PeekWord(p + 8))) continue;
        const uint32_t ent = m.PeekWord(p + 8) + 12u * idx;
        const uint32_t head = m.PeekWord(ent), box = m.PeekWord(ent + 8);
        if (!InRam(head & ~3u) || (box != 0 && !InRam(box & ~3u))) continue;
        return e;
    }
    idx = 0;
    return pop::kCar0;
}

// The entry's box, its shift nibble and its presence stirred (randomised cases).
void StirModel(CaseContext& c, uint32_t e, uint32_t idx) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    const uint32_t ent = m.PeekWord(m.PeekWord(e + 96) + 8) + 12u * idx;
    const uint32_t head = m.PeekWord(ent), box = m.PeekWord(ent + 8);
    if (Chance(r, 3) && InRam(head & ~3u))
        WriteU16Bench(m, head + 14, static_cast<uint16_t>(((m.PeekByte(head + 14) | (m.PeekByte(head + 15) << 8)) & 0x0FFFu) | ((r.U32() % 16u) << 12)));
    if (box != 0 && Chance(r, 3))
        for (uint32_t k = 0; k < 3; ++k) PutS16(m, box + 24 + 2u * k, static_cast<int16_t>(r.U32()));
    if (Chance(r, 6)) m.PokeWord(ent + 8, 0);
}

void SetupModelExtent(CaseContext& c, Args& a) {
    uint32_t idx = 0;
    const uint32_t e = ModelEntity(c, idx);
    if (c.sample == nullptr) StirModel(c, e, idx);
    const uint32_t out = c.scratch + kBox;
    RrNoise(c, out, 12);
    a.a[0] = e;
    a.a[1] = idx;
    a.a[2] = out;
}
uint32_t NativeModelExtent(NativeEnv& env, const Args& a) {
    rr::sim::GuestRam g = pop::View(env);
    return RqDone(env, g, U(rr::sim::ModelExtent(g, a.a[0], static_cast<int32_t>(a.a[1]), a.a[2])));
}

// CarSetup: the handle's pool (0..3) and slot (under and over the player count), the class around 9
// and 18, the bank 2 or not.
void SetupCarSetup(CaseContext& c, Args& a) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    uint32_t idx = 0;
    const uint32_t e = ModelEntity(c, idx);
    pop::DrawPlayers(c);
    if (c.sample == nullptr) {
        StirModel(c, e, idx);
        if (Chance(r, 2)) WriteU16Bench(m, e + 0xAC, static_cast<uint16_t>(((r.U32() % 4u) << 5) | (Chance(r, 2) ? r.U32() % 3u : r.U32() % 32u)));
        static const uint32_t kCls[8] = {0, 8, 9, 17, 18, 19, 0xFFFFFFFFu, 5};
        if (Chance(r, 2)) m.PokeWord(e + 180, Chance(r, 6) ? r.U32() : kCls[r.U32() % 8u]);
        const uint32_t gs = m.PeekWord(kGameStatePtr);
        if (Chance(r, 3)) m.PokeWord(gs + 60, Chance(r, 2) ? 2u : r.U32() % 4u);
    }
    a.a[0] = e;
    a.a[1] = idx;
}
uint32_t NativeCarSetup(NativeEnv& env, const Args& a) {
    rr::sim::GuestRam g = pop::View(env);
    rr::sim::CarSetup(g, a.a[0], static_cast<int32_t>(a.a[1]), env.sp);
    return pop::PopDone(env, g, true, "CarSetup");
}

// ---------------------------------------------------------------- SLUS 0x8003C520 CursorRoadEnd
// A real entity's cursor, or one planted on a resident piece or a junction core; the piece's kind
// word +2 and road +0x0C stirred.
void SetupCursorRoadEnd(CaseContext& c, Args& a) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    const RqNet n = RqRead(m);
    const pop::RaceGraph gr = pop::ReadGraph(m, c.gp);
    uint32_t cur = c.scratch + kCur;
    if (c.sample != nullptr && !Chance(r, 4)) {
        cur = (Chance(r, 4) ? pop::kCar0 : pop::Bike(m, r.U32() % 18u)) + 0x148;
    } else if (Chance(r, 3)) {
        if (RqPlantCoreCursor(c, n, cur, (r.Next() & 1u) != 0, false) == 0) RqPlantCursor(c, n, cur);
    } else {
        RqPlantCursor(c, n, cur);
    }
    const uint32_t piece = m.PeekWord(cur + 4);
    if (c.sample == nullptr && InRam(piece)) {
        if (Chance(r, 4)) PutS16(m, piece + 2, static_cast<int16_t>(r.U32() % 3u));
        if (Chance(r, 4) && gr.nRoads > 0) m.PokeWord(piece + 12, r.U32() % static_cast<uint32_t>(gr.nRoads));
        // the one road word whose graph record address wraps to 0 (0x8003C564), when the table allows
        const uint32_t base = m.PeekWord(m.PeekWord(c.gp + rr::sim::kRaceGraphGp) + 24);
        if (Chance(r, 16) && (base & 15u) == 0) m.PokeWord(piece + 12, (0u - base) >> 4);
        const uint32_t sub = m.PeekWord(cur + 8);
        if (Chance(r, 4) && InRam(sub)) m.PokeWord(sub + 12, r.U32());
    }
    a.a[0] = cur;
}
uint32_t NativeCursorRoadEnd(NativeEnv& env, const Args& a) {
    rr::sim::GuestRam g = pop::View(env);
    return RqDone(env, g, U(rr::sim::CursorRoadEnd(g, a.a[0])));
}

} // namespace tleaf

void AddTrafficLeafRows(std::vector<Row>& rows) {
    rows.push_back(pop::PopRow("road_walk", "void RoadWalk(RoadKey *from, RoadKey *out, s32 dist)  [SLUS 0x80012C1C]",
                               tleaf::kRoadWalk, tleaf::SetupRoadWalk, tleaf::NativeRoadWalk, false, false));
    rows.push_back(pop::PopRow("budget", "s32 Budget(u32 kind)  [RASHCDG 0x8008CDF4]", tleaf::kBudget,
                               tleaf::SetupBudget, tleaf::NativeBudget, true, false));
    rows.push_back(pop::PopRow("spacing", "s32 Spacing(Rec *r)  [RASHCDG 0x8009F578]", tleaf::kSpacing,
                               tleaf::SetupSpacing, tleaf::NativeSpacing, true, false));
    rows.push_back(pop::PopRow("share_flags", "void ShareFlags(s32 flags[2])  [RASHCDG 0x8009FF24]", tleaf::kShareFlags,
                               tleaf::SetupShareFlags, tleaf::NativeShareFlags, false, false));
    rows.push_back(pop::PopRow("node_lanes", "Lanes *NodeLanes(Object *o, s32 road, s32 *dir)  [SLUS 0x8003E1E8]",
                               tleaf::kNodeLanes, tleaf::SetupNodeLanes, tleaf::NativeNodeLanes, true, false));
    rows.push_back(pop::PopRow("model_extent", "s32 ModelExtent(Entity *e, s32 idx, s32 out[3])  [SLUS 0x80012AEC]",
                               tleaf::kModelExtent, tleaf::SetupModelExtent, tleaf::NativeModelExtent, true, false));
    rows.push_back(pop::PopRow("car_setup", "void CarSetup(Entity *e, s32 idx)  [SLUS 0x80012FC8]", tleaf::kCarSetup,
                               tleaf::SetupCarSetup, tleaf::NativeCarSetup, false, false));
    rows.push_back(pop::PopRow("cursor_road_end", "s32 CursorRoadEnd(Cursor *c)  [SLUS 0x8003C520]",
                               tleaf::kCursorRoadEnd, tleaf::SetupCursorRoadEnd, tleaf::NativeCursorRoadEnd, true, false));
}
