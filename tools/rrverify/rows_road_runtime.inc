// Bench rows for the road runtime. Included by verify_physics.cpp inside its anonymous
// namespace, so everything defined above that point is visible here.
//
// The port is src\game\sim\road_runtime.{h,cpp}, over road_query.h's guest-address view. Every
// native side below is: wrap the clone's RAM (and scratchpad) in a view, call the port with the
// guest's own arguments and the bench's `sp`, fail the case loudly if the port met an address the
// console would not survive.
//
// NO SEAMS. The six functions under this layer (the re-bind) are ported
// (AddRoadRebindRows below, one row each) and every row passes `rr::sim::RoadRuntimeNative`, so
// these rows are closed trees: `--skip-oracle` does not touch them.
//
// INPUT FAMILIES. rr-race holds three resident road objects (67 = road 9, 14 and 15 = junction 7),
// ROAD1.MAP, four route records and the finish record; its 18 live bikes all stand on road 9.
//   * dump-derived: this machine's live bikes and riders with their REAL cursors, cross-section
//     groups, run pointers and route bindings, and the VALUE fields (lateral, along, direction,
//     speed, zone word, progress) of one of the 324 bikes captured in the 18 RAM images;
//   * randomised: cursors re-seated (the road query rows' generator) including onto junction-core
//     pieces, +0x168 consistent with the piece or not, the cached XSIH record and the group drawn,
//     XSIH ranges and groups edited in place, along-distances on the clamp and range boundaries,
//     noise cross-section groups, roadside-zone kinds redrawn, points placed on segment end points
//     and arm planes, the route count, records, finish record and bindings drawn, the scratchpad
//     list planted with one to four entities and their passengers.
// Named bound, as in road_query's rows: residency is NOT stirred for anything that reaches the slice
// search - a stirred BTT_ can close the road into a ring and the original then never returns.

constexpr uint32_t rrb_kGateWord = 0xFF0; // scratch: RrShortcutGateOpen's node word (restored)
namespace rrt {
constexpr uint32_t kEdgeClass = 0x8003E67C, kStraddle = 0x8003DCB8, kRunOfSlice = 0x8003F1F0,
                   kFindLeg = 0x8003B4B0, kProject = 0x80036800, kMulAdd = 0x8002EAD8,
                   kCrossSection = 0x8003EF34, kClassNode = 0x8003EE68, kClass = 0x8003DE28,
                   kNodeZone = 0x8003DDB0, kRoadsideRun = 0x8003DF54, kPosition = 0x8003662C,
                   kClassify = 0x8003DFF4, kClassPass = 0x8003E150, kTurnSub = 0x8003BE1C,
                   kProgress = 0x8003B61C, kProgressPass = 0x8003B520, kRouteBind = 0x8003AF9C,
                   kZones = 0x8003A9D8, kRouteCheck = 0x8003AE24, kTrack = 0x8003701C,
                   kReseat = 0x800396A8, kProbe = 0x80037104, kTrackPass = 0x80037338,
                   kRebindBody = 0x80037450, kRebind = 0x800374D4;
// The six unread callees.
constexpr uint32_t kClassCore = 0x8003E754, kNodeWedge = 0x8003EB58, kRunNode = 0x8003F204,
                   kBindFirst = 0x8003B024, kBindStep = 0x8003B1C4, kShortcut = 0x8003BFE8;
constexpr uint32_t kFinishPtr = 0x800D6188;
constexpr uint32_t kRecords = 0x800D6194;  // == rq::kRoute + 36
constexpr uint32_t kCount = 0x800D6182;    // == rq::kRoute + 18
// Scratch-block layout (8 KiB; rq uses 0x390..0x4A0, 4096.. and 8000..).
constexpr uint32_t kOut = 0x600;           // an output block, 64 bytes
constexpr uint32_t kCur = 0x680;           // a cursor, 32 bytes
constexpr uint32_t kGroup = 0x700;         // a noise cross-section group, 264 bytes
constexpr uint32_t kXsih = 0x810;          // noise XSIH records, 4 x 16 bytes
constexpr uint32_t kSlice = 0x860;         // a noise slice, 52 bytes
constexpr uint32_t kSeg = 0x8A0;           // a noise BSDT record, 40 bytes
constexpr uint32_t kRec = 0x8D0;           // a noise route record, 120 bytes
constexpr uint32_t kContact = 0x960;       // a contact record, 0xC4 bytes
constexpr uint32_t kEnt = 0xA40;           // a noise entity, 0x200 bytes
} // namespace rrt

rr::sim::GuestRam RrView(NativeEnv& env) {
    rr::sim::GuestRam g(env.clone->mem.ram().data(), env.gp);
    g.SetScratchpad(env.clone->mem.scratchpad().data());
    return g;
}

// The six functions under the re-bind are ported, each with its own row further down
// (AddRoadRebindRows), and every row of this layer passes `rr::sim::RoadRuntimeNative`: no row here
// declares an oracle-supplied callee.

// ---------------------------------------------------------------- input helpers

uint32_t RrScratch(CaseContext& c, uint32_t off) { return c.scratch + off; }

void RrNoise(CaseContext& c, uint32_t at, uint32_t bytes) {
    for (uint32_t i = 0; i < bytes; i += 4) c.mem->PokeWord(at + i, c.rng->U32());
}

// A resident object, and one of its sub-objects' XSIH records (group >= 0 or -1 as it comes).
uint32_t RrSomeObject(CaseContext& c, const RqNet& n) {
    return n.resident.empty() ? 0u : n.resident[c.rng->U32() % n.resident.size()];
}
uint32_t RrSomeXsih(CaseContext& c, uint32_t obj) {
    Memory& m = *c.mem;
    const int32_t nsub = GetS16(m, obj + 0x14);
    const uint32_t sub = m.PeekWord(obj + 0x30) + 28u * RqPick(*c.rng, nsub);
    const uint32_t run = m.PeekWord(sub + 0x10);
    const int32_t first = static_cast<int16_t>(run >> 16);
    const int32_t cnt = static_cast<int16_t>(run & 0xFFFFu);
    return m.PeekWord(obj + 0x38) + 16u * static_cast<uint32_t>(first + static_cast<int32_t>(RqPick(*c.rng, std::max(cnt, 1))));
}
// A cross-section group of `obj`, by a real XSIH record with a group, or 0.
uint32_t RrSomeGroup(CaseContext& c, uint32_t obj) {
    for (int tries = 0; tries < 8; ++tries) {
        const uint32_t x = RrSomeXsih(c, obj);
        const int32_t g = GetS16(*c.mem, x + 4);
        if (g >= 0) return c.mem->PeekWord(obj + 0x3C) + 264u * static_cast<uint32_t>(g);
    }
    return 0;
}

// A noise group at `at`: every field the edge classifier reads drawn so that each arm comes up.
void RrNoiseGroup(CaseContext& c, uint32_t at) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    RrNoise(c, at, 264);
    for (uint32_t half = 8; half <= 136; half += 128) {
        const uint32_t h = at + half;
        static const int16_t kFlag[5] = {0, 1, -1, 2, 0};
        PutS16(m, h + 2, (r.Next() % 6u == 0) ? r.S16() : kFlag[r.U32() % 5u]);
        PutS16(m, h + 6, (r.Next() % 6u == 0) ? r.S16() : kFlag[r.U32() % 5u]);
        const auto edge = [&r]() {
            const uint64_t k = r.Next() % 8u;
            if (k == 0) return static_cast<int32_t>(0x80000000u);
            if (k == 1) return r.S32();
            const int32_t v = static_cast<int32_t>(r.U32() % 0x00200000u);
            return (k & 1u) ? -v : v;
        };
        PutS32(m, h + 8, edge());
        PutS32(m, h + 24, edge());
        PutS32(m, h + 72, edge());
    }
}

// A lateral near one of the edges of `group`'s halves: the classifier's comparisons are strict, so
// the boundary values matter.
int32_t RrLateralNear(CaseContext& c, uint32_t group) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    if (group == 0 || r.Next() % 8u == 0) {
        const uint64_t k = r.Next() % 4u;
        if (k == 0) return static_cast<int32_t>(0x80000000u);
        if (k == 1) return r.S32();
        return static_cast<int32_t>(r.U32() % 0x01000000u) - 0x00800000;
    }
    static const uint32_t kField[3] = {8, 24, 72};
    const uint32_t h = group + ((r.Next() & 1u) ? 136u : 8u);
    const int32_t edge = GetS32(m, h + kField[r.U32() % 3u]);
    int32_t mag = (edge < 0) ? static_cast<int32_t>(0u - static_cast<uint32_t>(edge)) : edge;
    mag += static_cast<int32_t>(r.U32() % 5u) - 2;
    return (r.Next() & 1u) ? mag : static_cast<int32_t>(0u - static_cast<uint32_t>(mag));
}

// The VALUE fields of a captured bike planted on a live one: where on the road it is and how it
// moves, never a pointer.
void RrPlantSampleValues(CaseContext& c, uint32_t e) {
    if (c.sample == nullptr || c.sample->entity.size() < 0x448) return;
    const std::vector<uint8_t>& s = c.sample->entity;
    const auto w = [&s](uint32_t off) {
        uint32_t v;
        std::memcpy(&v, s.data() + off, 4);
        return v;
    };
    static const uint32_t kFields[] = {0x158, 0x15C, 0x16C, 0x170, 0x1E0, 0x144};
    for (uint32_t off : kFields) c.mem->PokeWord(e + off, w(off));
    // the zone word's zone bits (1..9) only; the rest of +0x24 is the entity header
    c.mem->PokeWord(e + 0x24, (c.mem->PeekWord(e + 0x24) & ~0x3FEu) | (w(0x24) & 0x3FEu));
}

// +0x168 as RoadPosition would write it for the entity's cursor: (GRPT +2 << 16) | GRPT +0x0C.
uint32_t RrKeyOf(const Memory& m, uint32_t e) {
    const uint32_t piece = m.PeekWord(e + 0x14C);
    uint16_t hi = 0, lo = 0;
    m.ReadBlock(piece + 2, &hi, 2);
    m.ReadBlock(piece + 12, &lo, 2);
    return (static_cast<uint32_t>(hi) << 16) | lo;
}

// A live entity of this machine, stirred around everything the classifier and the tracking read.
// `rider` picks the bike's pool-1 rider (+0x354) instead of the bike one case in `riderOneIn`.
uint32_t RrEntity(CaseContext& c, const RqNet& n, bool stir, uint32_t riderOneIn = 0) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    uint32_t e = RqBike(c, n, stir, /*stirResidency=*/false);
    // NAMED BOUND: a pool-1 handle names one of the 18 live riders. RouteBindFirst (0x8003B024,
    // ported) follows riderPool[slot]+0x254 -> +0x1AC for pool 1 (0x8003B144),
    // and past the live riders that word is not a pointer - measured: 1 of 512 route_bind cases
    // trapped at 0x8003B14C before this bound.
    {
        uint16_t h = 0;
        m.ReadBlock(e + 0xAC, &h, 2);
        if ((h >> 5) == 1u && (h & 0x1Fu) >= 18u)
            WriteU16Bench(m, e + 0xAC, static_cast<uint16_t>((1u << 5) | ((h & 0x1Fu) % 18u)));
    }
    if (riderOneIn != 0 && r.Next() % riderOneIn == 0) {
        const uint32_t q = m.PeekWord(e + 0x354);
        if (q >= 0x80000000u && q < 0x80200000u) e = q;
    }
    RrPlantSampleValues(c, e);
    if (!stir) return e;
    // the cursor: RqBike re-seated it six times in ten; a junction core piece one case in four
    if (r.Next() % 4u == 0) RqPlantCoreCursor(c, n, e + 0x148, (r.Next() & 1u) != 0, false);
    const uint32_t obj = m.PeekWord(e + 0x148);
    const uint32_t slice = m.PeekWord(e + 0x154);
    // +0x168: consistent with the piece four times in five
    if (r.Next() % 5u != 0) m.PokeWord(e + 0x168, RrKeyOf(m, e));
    else m.PokeWord(e + 0x168, ((r.U32() % 3u) << 16) | (r.U32() % 16u));
    static const int32_t kDir[8] = {1, -1, 2, -2, 3, -3, 5, -5};
    PutS32(m, e + 0x16C, (r.Next() % 8u == 0) ? r.S32() : kDir[r.U32() % 8u]);
    // along: around the slice's running distance, past both ends now and then
    {
        const int32_t dist = GetS32(m, slice + 40);
        const int32_t chord = std::max(1, GetS32(m, slice + 32));
        const int64_t span = static_cast<int64_t>(chord) * 3;
        const int64_t off = static_cast<int64_t>(r.Next() % static_cast<uint64_t>(span + 1)) - chord;
        PutS32(m, e + 0x170, (r.Next() % 10u == 0) ? r.S32() : static_cast<int32_t>(dist + off));
        if (r.Next() % 6u == 0) {
            // exactly on an XSIH boundary of the object (the search compares HIGH halves)
            const uint32_t x = RrSomeXsih(c, obj);
            const int32_t b = GetS32(m, x + ((r.Next() & 1u) ? 8u : 12u));
            PutS32(m, e + 0x170, b + static_cast<int32_t>(r.U32() % 3u) - 1);
        }
    }
    PutS32(m, e + 0x158, (r.Next() & 1u) ? RrLateralNear(c, m.PeekWord(e + 0x174)) : r.World());
    // the cross-section cache: group and XSIH record
    {
        const uint64_t k = r.Next() % 6u;
        if (k == 0) m.PokeWord(e + 0x174, 0);
        else if (k <= 2) m.PokeWord(e + 0x174, RrSomeGroup(c, obj));
        else if (k == 3) {
            RrNoiseGroup(c, RrScratch(c, rrt::kGroup));
            m.PokeWord(e + 0x174, RrScratch(c, rrt::kGroup));
        }
        const uint64_t j = r.Next() % 6u;
        if (j == 0) m.PokeWord(e + 0x178, 0);
        else if (j <= 2) m.PokeWord(e + 0x178, RrSomeXsih(c, obj));
        else if (j == 3) m.PokeWord(e + 0x178, RrSomeXsih(c, RrSomeObject(c, n)));
        else if (j == 4) {
            const uint32_t x = RrScratch(c, rrt::kXsih);
            RrNoise(c, x, 64);
            for (uint32_t i = 0; i < 4; ++i) {
                PutS16(m, x + 16u * i + 4, static_cast<int16_t>(static_cast<int32_t>(r.U32() % 4u) - 1));
                const int32_t a = GetS32(m, e + 0x170) - static_cast<int32_t>(r.U32() % 0x40000u);
                PutS32(m, x + 16u * i + 8, a);
                PutS32(m, x + 16u * i + 12, a + static_cast<int32_t>(r.U32() % 0x80000u));
            }
            m.PokeWord(e + 0x178, x + 16u * (r.U32() % 4u));
        }
    }
    // +0x184: the low byte drawn (bit 0 off-road, 5 re-seated, 6 requery, 7 latch)
    if (r.Next() & 1u) m.PokeWord(e + 0x184, (m.PeekWord(e + 0x184) & ~0xFFu) | (r.U32() & 0xFFu));
    return e;
}

// Stale stack below sp, for the rows whose code reaches the slice search: the partial candidate
// leaves a candidate slot's +0x1C (a turn, a PDT_ record) as whatever the frame held and the picker
// reads it. NAMED BOUND: pointer-plausible words - the snapshot's own stale word
// kept, 0, or a real PDT_ record - not road_query's arbitrary 32-bit words. Measured: on a third
// seed one road_class_pass case trapped the ORIGINAL at 0x80039C40 (TurnTarget, `lh a0,2(a0)`) on
// the junk turn 0x86B9A1E0; a console stack holds what earlier frames left, which is not that.
void RrStackJunk(CaseContext& c, const RqNet& n, uint32_t bytes) {
    for (uint32_t i = 4; i <= bytes; i += 4) {
        const uint64_t k = c.rng->Next() % 3u;
        if (k == 0) continue;
        if (k == 1 || n.nPdt <= 0) c.mem->PokeWord(c.sp - i, 0);
        else c.mem->PokeWord(c.sp - i, n.pdt + 12u * RqPick(*c.rng, n.nPdt));
    }
}

// An along-distance distribution for the cross-section row: on the XSIH ranges' boundaries, on the
// slice's clamp boundaries, or anywhere.
void RrStirXsih(CaseContext& c, uint32_t obj) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    if (r.Next() % 4u != 0) return;
    // edit a few XSIH records of the object in place: group -1, a range moved, or kept
    for (int k = 0; k < 3; ++k) {
        const uint32_t x = RrSomeXsih(c, obj);
        const uint64_t roll = r.Next() % 3u;
        if (roll == 0) PutS16(m, x + 4, -1);
        else if (roll == 1) PutS32(m, x + 8, GetS32(m, x + 8) + static_cast<int32_t>(r.U32() % 0x60000u) - 0x30000);
        else PutS32(m, x + 12, GetS32(m, x + 12) + static_cast<int32_t>(r.U32() % 0x60000u) - 0x30000);
    }
    // a sub-object's run count negative or zero now and then
    if (r.Next() % 4u == 0) {
        const int32_t nsub = GetS16(m, obj + 0x14);
        const uint32_t sub = m.PeekWord(obj + 0x30) + 28u * RqPick(r, nsub);
        PutS16(m, sub + 0x10, static_cast<int16_t>(static_cast<int32_t>(r.U32() % 3u) - 1));
    }
}

// ---------------------------------------------------------------- helpers for the upper layers

// The junction record (IPT_) of object `obj`, or 0 - as RoadJunctionIndex finds it.
uint32_t RrIptOf(const Memory& m, const RqNet& n, uint32_t obj) {
    const int32_t id = GetS32(m, obj);
    for (int32_t q = 0; q < n.nIpt && q < 256; ++q)
        if (GetS16(m, n.ipt + 12u * static_cast<uint32_t>(q)) == id) return n.ipt + 12u * static_cast<uint32_t>(q);
    return 0;
}

// A route record aimed at the entity's own junction: a crossing length rounded like one of the
// piece's sub-objects and an exit road out of the junction's own turn table, so TurnSubObject can
// answer. Leaves `rec` alone when the cursor is not on a junction object.
void RrAimTurn(CaseContext& c, const RqNet& n, uint32_t cursor, uint32_t rec) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    const uint32_t obj = m.PeekWord(cursor + 0), piece = m.PeekWord(cursor + 4);
    const uint32_t ipt = RrIptOf(m, n, obj);
    if (ipt == 0) return;
    const int32_t cnt = GetS16(m, ipt + 8);
    if (cnt <= 0) return;
    const uint32_t t = n.pdt + 12u * static_cast<uint32_t>(GetS16(m, ipt + 6) + static_cast<int32_t>(RqPick(r, cnt)));
    const uint32_t g = n.gpdt + 12u * static_cast<uint32_t>(GetS16(m, t + 2));
    const uint32_t sub = m.PeekWord(obj + 0x30) + 28u * static_cast<uint32_t>(GetS16(m, piece + 20) + GetS16(m, g + 4));
    const int32_t len = GetS32(m, sub + 12);
    PutS32(m, rec + 8, ((len + 0x8000) >> 16) * 4096 + static_cast<int32_t>(r.U32() % 4096u));
    PutS32(m, rec + 0x54, GetS16(m, t + 10));
    PutS32(m, rec + 16, 1 + static_cast<int32_t>(r.U32() % 3u));
}

// The route layer drawn around entity `e`: the count, the binding, the key's arm (node, finish
// road, a leg, nothing), the finish record, the record's crossing, and the along-distance.
void RrStirProgress(CaseContext& c, const RqNet& n, uint32_t e) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    const int32_t count = GetS16(m, rrt::kCount);
    const uint32_t recs = m.PeekWord(rrt::kRecords);
    if (count <= 0) return;
    const uint32_t R = recs + 120u * RqPick(r, count);
    const uint64_t binding = r.Next() % 8u;
    if (binding == 0) m.PokeWord(e + 0x1AC, 0);
    else if (binding <= 5) m.PokeWord(e + 0x1AC, R);
    const uint32_t fin = m.PeekWord(rrt::kFinishPtr);
    const int32_t along = GetS32(m, e + 0x170);
    switch (r.U32() % 5u) {
        case 0: {                                                  // the node arm
            RqPlantCoreCursor(c, n, e + 0x148, (r.Next() & 1u) != 0, false);
            m.PokeWord(e + 0x168, (1u << 16) | (m.PeekWord(R) & 0xFFFFu));
            if (r.Next() % 4u != 0) RrAimTurn(c, n, e + 0x148, R);
            if (r.Next() % 3u == 0) {                              // the turn's sub-object is ours
                const uint32_t sub = m.PeekWord(e + 0x150);
                PutS32(m, R + 8, ((GetS32(m, sub + 12) + 0x8000) >> 16) * 4096);
            }
            break;
        }
        case 1: {                                                  // the finish road
            m.PokeWord(e + 0x168, m.PeekWord(fin) & 0xFFFFu);
            static const int32_t kDir[3] = {1, 0, -1};
            PutS32(m, fin + 8, kDir[r.U32() % 3u]);
            PutS32(m, fin + 4, along + static_cast<int32_t>(r.U32() % 0x200000u) - 0x100000);
            const uint64_t k = r.Next() % 3u;
            const uint32_t bound = m.PeekWord(e + 0x1AC);
            m.PokeWord(fin + 12, (k == 0) ? m.PeekWord(bound ? bound : R) : (k == 1) ? 0xFFFFFFFFu : r.U32() % 8u);
            break;
        }
        case 2: {                                                  // a leg of the record
            const int32_t legs = GetS32(m, R + 12);
            if (legs > 0 && legs < 8) {
                const uint32_t leg = R + 20u + 16u * (r.U32() % static_cast<uint32_t>(legs));
                m.PokeWord(e + 0x168, m.PeekWord(leg) & 0xFFFFu);
                if (r.Next() & 1u) PutS32(m, leg + 4, (r.Next() & 1u) ? 1 : -1);
                if (r.Next() % 3u == 0) PutS16(m, leg + 12 + 2u * (r.U32() % 2u), -1);
            }
            break;
        }
        default: break;                                            // as it is
    }
    if (r.Next() % 4u == 0) PutS32(m, e + 0x170, r.S32() >> (r.U32() % 8u));
}

// Roadside zones drawn around entity `e`: the run or a node's zone entry, the lateral side, the
// zone kinds of the segments the scan will reach (redrawn, 0 and 9..15 included), the box centre on
// the first segment, the zone word, the speed, the handle, the LCG seed and the second chain.
void RrStirZones(CaseContext& c, uint32_t e) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    const uint32_t obj = m.PeekWord(e + 0x148);
    const uint32_t slice = m.PeekWord(e + 0x154);
    const int32_t nslc = GetS16(m, obj + 0x16);
    const uint32_t other = m.PeekWord(obj + 0x34) + 52u * RqPick(r, nslc);
    const uint64_t k = r.Next() % 8u;
    m.PokeWord(e + 0x1F0, 0);
    if (k == 0) m.PokeWord(e + 0x1EC, 0);
    else if (k <= 4) m.PokeWord(e + 0x1EC, slice + 0x2C);
    else if (k <= 6) m.PokeWord(e + 0x1EC, other + 0x2C);
    else {                                                         // a node's one zone entry
        uint8_t run[2] = {0, 0};
        m.ReadBlock(other + 0x2C, run, 2);
        m.PokeWord(e + 0x1F0, m.PeekWord(obj + 0x64) + 4u * run[0]);
    }
    if (r.Next() & 1u) PutS32(m, e + 0x158, static_cast<int32_t>(0u - m.PeekWord(e + 0x158)));
    // the segments the scan reaches: redraw their kinds, put the box centre on the first
    const uint32_t runp = m.PeekWord(e + 0x1EC);
    uint32_t list = m.PeekWord(e + 0x1F0);
    uint32_t cnt = 1;
    if (list == 0 && runp != 0) {
        const uint32_t rp = runp + ((GetS32(m, e + 0x158) >= 0) ? 2u : 0u);
        uint8_t b[2] = {0, 0};
        m.ReadBlock(rp, b, 2);
        list = m.PeekWord(obj + 0x64) + 4u * b[0];
        cnt = b[1];
    }
    if (list != 0 && cnt != 0) {
        for (uint32_t i = 0; i < cnt && i < 8; ++i) {
            const uint32_t bz = list + 4u * i;
            uint8_t nseg = 0;
            m.ReadBlock(bz + 3, &nseg, 1);
            const uint32_t seg0 = m.PeekWord(obj + 0x60) + 40u * static_cast<uint32_t>(GetS16(m, bz));
            for (uint32_t j = 0; j < nseg; ++j) {
                if (r.Next() % 3u == 0) continue;
                uint16_t w = 0;
                m.ReadBlock(seg0 + 40u * j + 2u, &w, 2);
                const uint16_t kind = static_cast<uint16_t>((r.Next() % 6u == 0) ? (r.U32() % 16u) : (1u + r.U32() % 8u));
                w = static_cast<uint16_t>((w & ~0x20Fu) | kind | ((r.Next() & 1u) ? 0x200u : 0u));
                WriteU16Bench(m, seg0 + 40u * j + 2u, w);
            }
            if (i == 0 && nseg != 0 && (r.Next() % 3u != 0)) {
                const uint32_t seg = seg0 + 40u * (r.U32() % nseg);
                for (uint32_t a = 0; a < 3; a += 2) {
                    const int64_t p0 = GetS32(m, seg + 8 + 4u * a), p1 = GetS32(m, seg + 20 + 4u * a);
                    PutS32(m, e + 0xB8 + 4u * a, static_cast<int32_t>((p0 + p1) / 2) + (r.S32() >> 20));
                }
            }
        }
    }
    m.PokeWord(e + 0x24, (m.PeekWord(e + 0x24) & ~0x3FEu) | (r.U32() & 0x3FEu));
    switch (r.U32() % 6u) {
        case 0: PutS32(m, e + 0x1E0, 0); break;
        case 1: PutS32(m, e + 0x1E0, static_cast<int32_t>(r.U32() % 0x10000u)); break;
        case 2: PutS32(m, e + 0x1E0, -static_cast<int32_t>(r.U32() % 0x800000u)); break;
        case 3: PutS32(m, e + 0x1E0, r.S32()); break;
        default: PutS32(m, e + 0x1E0, static_cast<int32_t>(0x10000u + r.U32() % 0x700000u)); break;
    }
    if (r.Next() % 6u == 0) WriteU16Bench(m, e + 0xAC, static_cast<uint16_t>((4u << 5) | (28u + r.U32() % 4u)));
    m.PokeWord(c.gp + kRandSeedGpOffset, r.U32());
    if (r.Next() % 4u == 0) {                                      // the second chain: a rider
        // NAMED BOUND: never the entity itself - the propagation walk (0x8003ADB8) follows
        // o[+0x40] until 0, and a self-link never ends (measured: 2 step-limit traps in the
        // re-bind rows, where the entity is sometimes a rider, before this bound).
        const uint32_t pool = m.PeekWord(kPoolBasePtr);
        const uint32_t q = m.PeekWord(pool + 1096u * (r.U32() % 18u) + 0x354);
        if (q != e && m.PeekWord(q + 0x40) == 0) m.PokeWord(e + 0x40, q);
    }
}

// The fields the tracking reads: the heading, the box centre near the slice, the half extents, the
// crash bits, the contact point and the probe heading, the passenger, the list node, the suppress
// bit, the latched tag and the contact record.
//
// NAMED BOUND: only fields INSIDE the entity's own record are written. A pool-1 rider is a 628-byte
// record (`0x801BB560 - 0x801BB2EC`); the bike-only fields +0x33C/+0x340/+0x358/+0x440 of a rider
// address whatever follows it - for the last rider, whatever follows the rider pool. Measured
// before this bound: one road_rebind case on the default seed wrote there and the ORIGINAL then
// ran 200 million instructions inside the slice search (0x80036D7C) without returning. The
// random draws are made either way, so the family is otherwise unchanged.
void RrStirTracking(CaseContext& c, uint32_t e) {
    Rng& r = *c.rng;
    const uint32_t pool0 = c.mem->PeekWord(kPoolBasePtr);
    const bool bike = e >= pool0 && e < pool0 + 1096u * 32u && (e - pool0) % 1096u == 0;
    const uint32_t limit = bike ? 1096u : 628u;
    // A view of the memory that drops the writes past the record (the draws still happen).
    struct Bounded {
        Memory& mem;
        uint32_t e, limit;
        void PokeWord(uint32_t a, uint32_t v) { if (a + 4u <= e + limit) mem.PokeWord(a, v); }
        uint32_t PeekWord(uint32_t a) const { return mem.PeekWord(a); }
        void Put16(uint32_t a, int16_t v) { if (a + 2u <= e + limit) PutS16(mem, a, v); }
        void Put32(uint32_t a, int32_t v) { if (a + 4u <= e + limit) PutS32(mem, a, v); }
    } m{*c.mem, e, limit};
    for (uint32_t k = 0; k < 3; ++k) m.Put16(e + 0x1C2 + 2u * k, r.Unit());
    if (r.Next() & 1u) RqPlantPoint(c, m.PeekWord(e + 0x154), e + 0xB8);
    m.Put32(e + 0x134, static_cast<int32_t>(r.U32() % 0x100000u));
    m.Put32(e + 0x130, static_cast<int32_t>(r.U32() % 0x40000u));
    if (r.Next() % 3u == 0) m.PokeWord(e + 0x238, m.PeekWord(e + 0x238) ^ (0x200u << (r.U32() % 2u)));
    if (r.Next() % 4u == 0) m.PokeWord(e + 0x238, m.PeekWord(e + 0x238) ^ 0x800u);
    for (uint32_t k = 0; k < 3; ++k) {
        m.Put32(e + 0x1F8 + 4u * k, static_cast<int32_t>(m.PeekWord(e + 0xB8 + 4u * k)) + (r.S32() >> 14));
        m.Put16(e + 0x204 + 2u * k, r.Unit());
        m.Put16(e + 0x210 + 2u * k, r.Unit());
    }
    m.PokeWord(e + 0x358, (r.Next() % 3u == 0) ? m.PeekWord(e + 0x354) : 0u);
    if (r.Next() % 5u == 0) m.PokeWord(e + 0x440, 0);
    if (r.Next() % 3u == 0) m.PokeWord(e + 0x230, m.PeekWord(e + 0x230) ^ 0x08000000u);
    m.PokeWord(e + 0x33C, (r.Next() & 1u) ? 0u : m.PeekWord(e + 0x154));
    if (r.Next() % 4u == 0) {
        m.PokeWord(e + 0x234, m.PeekWord(e + 0x234) | 0x40000u);
        const uint32_t ct = RrScratch(c, rrt::kContact);
        RrNoise(c, ct, 0xC4);
        m.PokeWord(e + 0x340, ct);
    } else {
        m.PokeWord(e + 0x234, m.PeekWord(e + 0x234) & ~0x40000u);
    }
}

// A junction core cursor and a box centre just outside one of the node's arms, so RoadReseat's
// plane test (-131 raw units) can pass.
void RrNearArm(CaseContext& c, const RqNet& n, uint32_t e) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    if (RqPlantCoreCursor(c, n, e + 0x148, (r.Next() & 1u) != 0, false) == 0) return;
    m.PokeWord(e + 0x168, RrKeyOf(m, e));
    const uint32_t obj = m.PeekWord(e + 0x148);
    const int32_t id = GetS32(m, obj);
    if (id < 0 || id >= n.nJn) return;
    const uint32_t node = n.bit + 104u * static_cast<uint32_t>(id);
    const int32_t arms = GetS16(m, node + 2);
    if (arms <= 0 || arms > 4) return;
    const uint32_t arm = node + 8u + 24u * RqPick(r, arms);
    const int32_t s = static_cast<int32_t>(r.U32() % 0x60000u) - 0x10000;   // outside mostly
    for (uint32_t k = 0; k < 3; ++k) {
        const int32_t v = GetS16(m, arm + 6 + 2u * k);
        const int32_t pt = GetS32(m, arm + 12 + 4u * k);
        PutS32(m, e + 0xB8 + 4u * k, pt - static_cast<int32_t>((static_cast<int64_t>(v) * s) >> 12) + (r.S32() >> 18));
    }
}

// NAMED BOUND, two parts, for an entity whose cursor
// lies on a JUNCTION object (kind 1); applied last in every row that reaches RoadShortcut
// (0x8003BFE8) - road_reseat, road_probe_ahead, road_track_pass, road_rebind_body, road_rebind,
// road_shortcut:
//   (a) on a STUB, a box centre farther than kRbNearUnits from its slice's origin is moved onto that
//       slice (up to 2 units past either end, 45 across). RoadTrack keeps the cursor on the slice
//       nearest the point every frame, so a stub cursor with a far point is not a game state;
//   (b) when the drawn +0x168/+0x170 would open RoadShortcut's 50-unit gate, +0x168/+0x16C/+0x170
//       are rewritten by RoadPosition (0x8003662C) for the entity's own cursor. That is the game's
//       invariant: every writer of +0x168..+0x170 is RoadPosition on the entity's cursor (RoadTrack,
//       RoadClassify's node-arm adoption) or a copy of its output made with the cursor (RoadShortcut's
//       re-bind), and RoadReseat - RoadShortcut's only caller - runs before the frame's RoadTrack on
//       last frame's consistent pair.
// WHY, measured. Without them the ORIGINAL does not return (step-limit in the slice search, more
// than 4 billion instructions): seed 0x7777AAAA13 one case each of road_track_pass and road_rebind;
// seed 0xA5A5C3C312345678 (--cases 2048) one each of road_reseat and road_track_pass; with (b) alone
// seed 0x5EED0007 one road_track_pass and 0x7777AAAA13 one road_rebind; with (a) alone the 0xA5A5
// cases. The mechanism, traced with PyCpu on the dumped road_reseat case: the entity is on the
// road-8 stub of junction 7 with a random +0x170 that opens the gate (the consistent along there is
// about 2600 of road 8's 2839 graph units, 235 from node 7: the gate stays shut). RoadShortcut's
// first arm (road 9) searches backward through the core and out onto object 15, and that search
// commits object 15 into the cursor's +0x00 - a word RoadShortcut sets ONCE, before its arm loop,
// like +0x10/+0x18/+0x1C. The next arm (road 10) writes only
// +4/+8/+12/+20, so its search starts from object 15 with object 14's piece, sub-object and slice;
// the backward neighbour of that mixed cursor is slice 14 of the same sub-object, whose backward
// neighbour is slice 0 again, and the two candidate distances alternate with a difference far above
// the 131-unit guard - forever. With (a) and (b) together every case of the six rows finishes and
// matches on the default seed, 0xA5A5C3C312345678 --cases 2048, 0x5EED0007 --cases 1024 and
// 0x7777AAAA13 --cases 1024. That the mixed cursor CAN arise in the game (it is the original's own
// quirk) is not excluded; that it loops needs the gate open and a search that leaves the core object,
// and no game-consistent case in these runs (about 30 000 cases) did so. The draws are the same
// either way, so the rest of each family is unchanged.
constexpr int64_t kRbNearUnits = 100;  // see RrNearOwnStub
bool RrShortcutGateOpen(CaseContext& c, uint32_t e) {
    Memory& m = *c.mem;
    const uint32_t key = m.PeekWord(e + 0x168);
    if ((key >> 16) != 0 || m.PeekWord(e + 0x174) == 0 || (m.PeekWord(e + 0x184) & 1u) == 0) return false;
    rr::sim::GuestRam g(m.ram().data(), c.gp);
    const uint32_t out = RrScratch(c, rrb_kGateWord);
    const uint32_t keep = m.PeekWord(out);
    const int32_t d = rr::sim::RoadEndNode(g, e + 0x168, out, 0);   // a host-side reading, restored
    const uint32_t node = m.PeekWord(out);
    m.PokeWord(out, keep);
    return !g.Faulted() && node != 0xFFFFFFFFu && d <= 0x31FFFF;
}
void RrNearOwnStub(CaseContext& c, uint32_t e) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    const uint32_t obj = m.PeekWord(e + 0x148), piece = m.PeekWord(e + 0x14C), s = m.PeekWord(e + 0x154);
    if (GetS16(m, obj + 16) != 1) return;                           // not a junction object
    int64_t far = 0;                                                // the box centre's distance, units
    for (uint32_t k = 0; k < 3; ++k) {
        const int64_t dk = (static_cast<int64_t>(GetS32(m, e + 0xB8 + 4u * k)) - GetS32(m, s + 20u + 4u * k)) >> 16;
        far = std::max<int64_t>(far, dk < 0 ? -dk : dk);
    }
    if (GetS16(m, piece + 2) == 0 && far > kRbNearUnits) {          // a stub, the point far: onto it
        const int32_t chord = std::max(1, GetS32(m, s + 32));
        const int64_t d = static_cast<int64_t>(r.U32() % static_cast<uint32_t>(chord + 0x40000)) - 0x20000;
        const int64_t l = static_cast<int64_t>(r.U32() % (91u * 65536u)) - 45 * 65536;
        for (uint32_t k = 0; k < 3; ++k) {
            const int64_t pos = GetS32(m, s + 20u + 4u * k);
            const int64_t t2 = GetS16(m, s + 14u + 2u * k);
            const int64_t t0 = GetS16(m, s + 2u + 2u * k);
            PutS32(m, e + 0xB8 + 4u * k, static_cast<int32_t>(pos + ((t2 * d) >> 12) + ((t0 * l) >> 12)));
        }
    }
    if (!RrShortcutGateOpen(c, e)) return;
    rr::sim::GuestRam g(m.ram().data(), c.gp);
    rr::sim::RoadPosition(g, e + 0x1C2, e + 0x148, e + 0x168, c.sp - 1024);   // its frame: inside the window
}

// The scratchpad list: `count` entities (1..4 when 0), each one of this machine's live bikes,
// stirred by `stir(e)` in the randomised family; returns the entities.
std::vector<uint32_t> RrPlantList(CaseContext& c, const RqNet& n, uint32_t count,
                                  const std::function<void(uint32_t)>& stir) {
    Memory& m = *c.mem;
    if (count == 0) count = 1u + c.rng->U32() % 4u;
    std::vector<uint32_t> es;
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t e = RrEntity(c, n, c.sample == nullptr);
        // pool 0: the list holds bikes (RqBike may have moved the handle to another pool)
        WriteU16Bench(m, e + 0xAC, static_cast<uint16_t>((e - m.PeekWord(kPoolBasePtr)) / 1096u));
        if (c.sample == nullptr) stir(e);
        es.push_back(e);
    }
    for (uint32_t i = 0; i < count; ++i) m.PokeWord(rr::sim::kRoadListFirst + 4u * i, es[i]);
    m.PokeWord(rr::sim::kRoadListFirst + 4u * count, c.rng->U32());   // the load one past the end
    m.PokeWord(rr::sim::kRoadListHead, rr::sim::kRoadListFirst + 4u * count);
    return es;
}

void AddRoadRuntimeRows(std::vector<Row>& rows) {
    using rr::sim::GuestRam;

    // ---------------------------------------------------------------- 0x8003E67C
    rows.push_back(Row{
        "road_edge_class", "s32 RoadEdgeClass(Group *g, s32 lateral, s32 side, u16 out[2])  [SLUS 0x8003E67C]",
        rrt::kEdgeClass,
        [](CaseContext& c, Args& args) {
            const RqNet n = RqRead(*c.mem);
            uint32_t group = 0, out = RrScratch(c, rrt::kOut);
            RrNoise(c, out, 16);
            if (c.sample) {
                const uint32_t e = RrEntity(c, n, false);
                group = c.mem->PeekWord(e + 0x174);
                if (group == 0) group = RrSomeGroup(c, RrSomeObject(c, n));
                out = e + 0x188;                                    // where 0x8003EF34 points it
            } else if (c.rng->Next() & 1u) {
                group = RrSomeGroup(c, RrSomeObject(c, n));
            }
            if (group == 0) {
                group = RrScratch(c, rrt::kGroup);
                RrNoiseGroup(c, group);
            }
            static const int32_t kSide[6] = {1, 2, 2, 1, 0, 3};
            args.a[0] = group;
            // a captured bike's lateral (dump-derived), or one on an edge of the group
            args.a[1] = static_cast<uint32_t>(c.sample ? c.sample->lateral : RrLateralNear(c, group));
            args.a[2] = (c.rng->Next() % 8u == 0) ? c.rng->U32() : static_cast<uint32_t>(kSide[c.rng->U32() % 6u]);
            args.a[3] = out;
        },
        nullptr, /*compareV0=*/true,
        [](NativeEnv& env, const Args& a) -> uint32_t {
            GuestRam g = RrView(env);
            return RqDone(env, g, static_cast<uint32_t>(rr::sim::RoadEdgeClass(
                                      g, a.a[0], static_cast<int32_t>(a.a[1]), static_cast<int32_t>(a.a[2]), a.a[3])));
        }});

    // ---------------------------------------------------------------- 0x8003DCB8
    rows.push_back(Row{
        "segment_straddle", "s32 SegmentStraddle(Entity *e, Seg *r)  [SLUS 0x8003DCB8]", rrt::kStraddle,
        [](CaseContext& c, Args& args) {
            Memory& m = *c.mem;
            Rng& r = *c.rng;
            const RqNet n = RqRead(m);
            const uint32_t e = RrEntity(c, n, c.sample == nullptr);
            // a real roadside segment of the entity's own object, through its slice's run
            const uint32_t obj = m.PeekWord(e + 0x148);
            uint32_t seg = m.PeekWord(obj + 0x60) + 40u * (r.U32() % 32u);
            const uint32_t slice = m.PeekWord(e + 0x154);
            uint8_t run[4] = {0, 0, 0, 0};
            m.ReadBlock(slice + 0x2C, run, 4);
            const uint32_t side = (r.Next() & 1u) ? 2u : 0u;
            if (run[side + 1] != 0) {
                const uint32_t bz = m.PeekWord(obj + 0x64) + 4u * (run[side] + r.U32() % run[side + 1]);
                const int32_t first = GetS16(m, bz);
                uint8_t cnt = 0;
                m.ReadBlock(bz + 3, &cnt, 1);
                seg = m.PeekWord(obj + 0x60) + 40u * static_cast<uint32_t>(first + static_cast<int32_t>(RqPick(r, cnt)));
            }
            if (!c.sample) {
                if (r.Next() % 4u == 0) {                           // a segment of noise
                    seg = RrScratch(c, rrt::kSeg);
                    RrNoise(c, seg, 40);
                    for (uint32_t k = 0; k < 3; ++k) {
                        const int32_t b = GetS32(m, e + 0xB8 + 4u * k);
                        PutS32(m, seg + 8 + 4u * k, b + (r.S32() >> (8 + r.U32() % 16u)));
                        PutS32(m, seg + 20 + 4u * k, b + (r.S32() >> (8 + r.U32() % 16u)));
                    }
                }
                if (r.Next() % 4u == 0) {                           // a tangent of noise
                    const uint32_t s = RrScratch(c, rrt::kSlice);
                    RrNoise(c, s, 52);
                    m.PokeWord(e + 0x154, s);
                }
                // the box centre between the end points, on one of them, or anywhere near
                const uint64_t k = r.Next() % 4u;
                for (uint32_t i = 0; i < 3; i += 2) {
                    const int32_t p0 = GetS32(m, seg + 8 + 4u * i), p1 = GetS32(m, seg + 20 + 4u * i);
                    int32_t v;
                    if (k == 0) v = static_cast<int32_t>((static_cast<int64_t>(p0) + p1) / 2);
                    else if (k == 1) v = (r.Next() & 1u) ? p0 : p1;
                    else if (k == 2) v = p0 + (r.S32() >> (10 + r.U32() % 12u));
                    else v = r.S32();
                    PutS32(m, e + 0xB8 + 4u * i, v);
                }
            }
            args.a[0] = e;
            args.a[1] = seg;
        },
        nullptr, /*compareV0=*/true,
        [](NativeEnv& env, const Args& a) -> uint32_t {
            GuestRam g = RrView(env);
            return RqDone(env, g, static_cast<uint32_t>(rr::sim::SegmentStraddle(g, a.a[0], a.a[1], env.sp)));
        }});

    // ---------------------------------------------------------------- 0x8003F1F0
    rows.push_back(Row{
        "roadside_run_of_slice", "u8 *RoadsideRunOfSlice(Entity *e)  [SLUS 0x8003F1F0]", rrt::kRunOfSlice,
        [](CaseContext& c, Args& args) {
            const RqNet n = RqRead(*c.mem);
            uint32_t e = RrEntity(c, n, false);
            if (!c.sample) {
                if (c.rng->Next() & 1u) {
                    e = RrScratch(c, rrt::kEnt);
                    RrNoise(c, e, 0x200);
                }
                if (c.rng->Next() & 1u) c.mem->PokeWord(e + 0x154, c.rng->U32());
            }
            args.a[0] = e;
        },
        nullptr, /*compareV0=*/true,
        [](NativeEnv& env, const Args& a) -> uint32_t {
            GuestRam g = RrView(env);
            return RqDone(env, g, rr::sim::RoadsideRunOfSlice(g, a.a[0]));
        }});

    // ---------------------------------------------------------------- 0x8003B4B0, over the view
    // Already ported over a host view (`RouteFindLeg`, race.h, row `route_find_leg`); the road layer
    // needs it over guest addresses, which is a second implementation and gets its own row.
    rows.push_back(Row{
        "route_find_leg_view", "Leg *RouteFindLeg(Record *o, u32 key)  [SLUS 0x8003B4B0, over guest addresses]",
        rrt::kFindLeg,
        [](CaseContext& c, Args& args) {
            Memory& m = *c.mem;
            Rng& r = *c.rng;
            const int32_t count = GetS16(m, rrt::kCount);
            uint32_t o = m.PeekWord(rrt::kRecords) + 120u * RqPick(r, count);
            if (!c.sample) {
                const uint64_t k = r.Next() % 6u;
                if (k == 0) o = 0;
                else if (k == 1) {
                    o = RrScratch(c, rrt::kRec);
                    for (uint32_t i = 0; i < 120; i += 4) m.PokeWord(o + i, r.U32() % 12u);
                    PutS32(m, o + 12, static_cast<int32_t>(r.U32() % 7u) - 2);
                }
                if (r.Next() % 8u == 0) PutS16(m, rrt::kCount, -1);
            }
            uint32_t key = r.U32() % 12u;
            const int32_t legs = (o != 0) ? GetS32(m, o + 12) : 0;
            if (o != 0 && legs > 0 && legs < 8 && (r.Next() % 3u != 0))
                key = m.PeekWord(o + 20u + 16u * (r.U32() % static_cast<uint32_t>(legs)));
            if (r.Next() % 10u == 0) key = r.U32();
            args.a[0] = o;
            args.a[1] = key;
        },
        nullptr, /*compareV0=*/true,
        [](NativeEnv& env, const Args& a) -> uint32_t {
            GuestRam g = RrView(env);
            return RqDone(env, g, rr::sim::RouteFindLegView(g, a.a[0], a.a[1]));
        }});

    // ---------------------------------------------------------------- 0x80036800, over the view
    rows.push_back(Row{
        "road_project_view",
        "void RoadProject(const s32 p[3], Slice *s, s32 *lat, s32 *along)  [SLUS 0x80036800, over guest addresses]",
        rrt::kProject,
        [](CaseContext& c, Args& args) {
            Memory& m = *c.mem;
            Rng& r = *c.rng;
            const RqNet n = RqRead(m);
            const uint32_t e = RrEntity(c, n, false);
            uint32_t slice = m.PeekWord(e + 0x154);
            uint32_t p = e + 0xB8;
            if (!c.sample) {
                if (r.Next() & 1u) {
                    slice = RrScratch(c, rrt::kSlice);
                    RrNoise(c, slice, 52);
                }
                p = RrScratch(c, rrt::kOut);
                for (uint32_t k = 0; k < 3; ++k) PutS32(m, p + 4u * k, (r.Next() & 1u) ? r.S32() : r.World());
            } else {
                PutVec3S32(m, p, c.sample->q);
            }
            const uint32_t out = RrScratch(c, rrt::kOut + 16);
            RrNoise(c, out, 8);
            args.a[0] = p;
            args.a[1] = slice;
            args.a[2] = (r.Next() % 5u == 0) ? 0u : out;
            args.a[3] = (r.Next() % 5u == 0) ? 0u : out + 4u;
        },
        nullptr, /*compareV0=*/false,
        [](NativeEnv& env, const Args& a) -> uint32_t {
            GuestRam g = RrView(env);
            rr::sim::RoadProjectView(g, a.a[0], a.a[1], a.a[2], a.a[3], env.sp);
            return RqDone(env, g, 0u);
        }});

    // ---------------------------------------------------------------- 0x8002EAD8, over the view
    rows.push_back(Row{
        "mul_add_view", "void MulAdd(const s32 b[3], const s16 d[3], s32 t, s32 out[3])  [SLUS 0x8002EAD8, over guest addresses]",
        rrt::kMulAdd,
        [](CaseContext& c, Args& args) {
            Memory& m = *c.mem;
            Rng& r = *c.rng;
            const uint32_t base = RrScratch(c, rrt::kOut);
            RrNoise(c, base, 48);
            for (uint32_t k = 0; k < 3; ++k) PutS16(m, base + 32 + 2u * k, r.Unit());
            if (c.sample) {
                PutVec3S32(m, base, c.sample->p);
                PutVec3S16(m, base + 32, c.sample->mA);
            }
            // out: its own block, the base itself, or overlapping it by one word either way
            static const int32_t kOut[4] = {16, 0, 4, -4};
            args.a[0] = base + 4u;
            args.a[1] = base + 32;
            args.a[2] = c.sample ? static_cast<uint32_t>(c.sample->speed) : r.U32();
            args.a[3] = base + 4u + static_cast<uint32_t>(kOut[r.U32() % 4u]);
        },
        nullptr, /*compareV0=*/false,
        [](NativeEnv& env, const Args& a) -> uint32_t {
            GuestRam g = RrView(env);
            rr::sim::MulAddView(g, a.a[0], a.a[1], static_cast<int32_t>(a.a[2]), a.a[3]);
            return RqDone(env, g, 0u);
        }});

    // ---------------------------------------------------------------- 0x8003EF34
    rows.push_back(Row{
        "road_cross_section", "s32 RoadCrossSection(Cursor *c, const s32 pos[3], Out *out)  [SLUS 0x8003EF34]",
        rrt::kCrossSection,
        [](CaseContext& c, Args& args) {
            const RqNet n = RqRead(*c.mem);
            const uint32_t e = RrEntity(c, n, c.sample == nullptr);
            if (!c.sample) RrStirXsih(c, c.mem->PeekWord(e + 0x148));
            args.a[0] = e + 0x148;
            args.a[1] = e + 0x168;
            args.a[2] = e + 0x174;
        },
        nullptr, /*compareV0=*/true,
        [](NativeEnv& env, const Args& a) -> uint32_t {
            GuestRam g = RrView(env);
            return RqDone(env, g, static_cast<uint32_t>(rr::sim::RoadCrossSection(g, a.a[0], a.a[1], a.a[2])));
        }});

    // ---------------------------------------------------------------- 0x8003EE68
    rows.push_back(Row{
        "road_class_node", "u8 RoadClassNode(Entity *e, Out *out, Cursor *x, s32 zone)  [SLUS 0x8003EE68]",
        rrt::kClassNode,
        [](CaseContext& c, Args& args) {
            const RqNet n = RqRead(*c.mem);
            const uint32_t e = RrEntity(c, n, c.sample == nullptr);
            // on a junction core piece three cases in four, so the unread 0x8003E754 runs
            if (!c.sample && c.rng->Next() % 4u != 0) {
                RqPlantCoreCursor(c, n, e + 0x148, (c.rng->Next() & 1u) != 0, false);
                c.mem->PokeWord(e + 0x168, RrKeyOf(*c.mem, e));
            }
            const uint32_t x = RrScratch(c, rrt::kCur);
            RrNoise(c, x, 32);
            args.a[0] = e;
            args.a[1] = e + 0x174;
            args.a[2] = (c.rng->Next() % 5u == 0) ? 0u : x;
            args.a[3] = (c.rng->Next() & 1u) ? 0xFFFFFFFFu : c.rng->U32() % 5u;
        },
        nullptr, /*compareV0=*/true,
        [](NativeEnv& env, const Args& a) -> uint32_t {
            GuestRam g = RrView(env);
            rr::sim::RoadRuntimeNative seams;
            return RqDone(env, g, rr::sim::RoadClassNode(g, a.a[0], a.a[1], a.a[2], static_cast<int32_t>(a.a[3]),
                                                         env.sp, seams));
        },
        {}});

    // ---------------------------------------------------------------- 0x8003DE28
    rows.push_back(Row{
        "road_class", "u8 RoadClass(Entity *e, s32 mode, Cursor *x, s32 zone)  [SLUS 0x8003DE28]", rrt::kClass,
        [](CaseContext& c, Args& args) {
            Memory& m = *c.mem;
            const RqNet n = RqRead(m);
            const uint32_t e = RrEntity(c, n, c.sample == nullptr);
            if (!c.sample) {
                RrStirXsih(c, m.PeekWord(e + 0x148));
                if (c.rng->Next() % 3u == 0) {
                    RqPlantCoreCursor(c, n, e + 0x148, (c.rng->Next() & 1u) != 0, false);
                    m.PokeWord(e + 0x168, RrKeyOf(m, e));
                }
            }
            const uint32_t x = RrScratch(c, rrt::kCur);
            RrNoise(c, x, 32);
            static const int32_t kMode[4] = {0, 0, 1, 2};
            args.a[0] = e;
            args.a[1] = static_cast<uint32_t>(c.sample ? 0 : kMode[c.rng->U32() % 4u]);
            args.a[2] = (c.rng->Next() % 5u == 0) ? 0u : x;
            args.a[3] = (c.rng->Next() & 1u) ? 0xFFFFFFFFu : c.rng->U32() % 5u;
        },
        nullptr, /*compareV0=*/true,
        [](NativeEnv& env, const Args& a) -> uint32_t {
            GuestRam g = RrView(env);
            rr::sim::RoadRuntimeNative seams;
            return RqDone(env, g, rr::sim::RoadClass(g, a.a[0], static_cast<int32_t>(a.a[1]), a.a[2],
                                                     static_cast<int32_t>(a.a[3]), env.sp, seams));
        },
        {}});

    // ---------------------------------------------------------------- 0x8003DDB0
    rows.push_back(Row{
        "node_zone", "s32 NodeZone(Entity *e)  [SLUS 0x8003DDB0]", rrt::kNodeZone,
        [](CaseContext& c, Args& args) {
            const RqNet n = RqRead(*c.mem);
            uint32_t e = RrEntity(c, n, c.sample == nullptr, 4);
            if (!c.sample) {
                const uint64_t k = c.rng->Next() % 8u;
                if (k < 4) {
                    RqPlantCoreCursor(c, n, e + 0x148, (c.rng->Next() & 1u) != 0, false);
                    c.mem->PokeWord(e + 0x168, RrKeyOf(*c.mem, e));
                } else if (k == 4) {
                    e = 0;
                }
            }
            args.a[0] = e;
        },
        nullptr, /*compareV0=*/true,
        [](NativeEnv& env, const Args& a) -> uint32_t {
            GuestRam g = RrView(env);
            rr::sim::RoadRuntimeNative seams;
            return RqDone(env, g, static_cast<uint32_t>(rr::sim::NodeZone(g, a.a[0], env.sp, seams)));
        },
        {}});

    // ---------------------------------------------------------------- 0x8003DF54
    rows.push_back(Row{
        "roadside_run", "void RoadsideRun(Entity *e, s32 mode, s32 zone)  [SLUS 0x8003DF54]", rrt::kRoadsideRun,
        [](CaseContext& c, Args& args) {
            Memory& m = *c.mem;
            Rng& r = *c.rng;
            const RqNet n = RqRead(m);
            const uint32_t e = RrEntity(c, n, c.sample == nullptr, 4);
            if (!c.sample) {
                if (r.Next() % 3u == 0) {
                    RqPlantCoreCursor(c, n, e + 0x148, (r.Next() & 1u) != 0, false);
                    m.PokeWord(e + 0x168, RrKeyOf(m, e));
                }
                static const uint32_t kRun[4] = {0u, 0xFFFFFFFFu, 1u, 2u};
                m.PokeWord(e + 0x1EC, kRun[r.U32() % 4u]);
                m.PokeWord(e + 0x1F0, kRun[r.U32() % 4u]);
                m.PokeWord(e + 0x1F4, r.U32());
            }
            static const int32_t kMode[4] = {0, 1, 0, 2};
            args.a[0] = e;
            args.a[1] = static_cast<uint32_t>(c.sample ? 0 : kMode[r.U32() % 4u]);
            args.a[2] = (r.Next() & 1u) ? 0xFFFFFFFFu : r.U32() % 4u;
        },
        nullptr, /*compareV0=*/true,
        [](NativeEnv& env, const Args& a) -> uint32_t {
            GuestRam g = RrView(env);
            rr::sim::RoadRuntimeNative seams;
            return RqDone(env, g, rr::sim::RoadsideRun(g, a.a[0], static_cast<int32_t>(a.a[1]),
                                                       static_cast<int32_t>(a.a[2]), env.sp, seams));
        },
        {}});

    // ---------------------------------------------------------------- 0x8003662C
    rows.push_back(Row{
        "road_position", "void RoadPosition(const s16 h[3], Cursor *c, s32 out[3])  [SLUS 0x8003662C]",
        rrt::kPosition,
        [](CaseContext& c, Args& args) {
            Memory& m = *c.mem;
            Rng& r = *c.rng;
            const RqNet n = RqRead(m);
            const uint32_t e = RrEntity(c, n, c.sample == nullptr, 4);
            if (!c.sample) {
                RqStirResidency(c, n);                             // no search here: bounded
                for (uint32_t k = 0; k < 3; ++k) PutS16(m, e + 0x1C2 + 2u * k, r.Unit());
                const int32_t chord = GetS32(m, m.PeekWord(e + 0x154) + 32);
                PutS32(m, e + 0x15C, (r.Next() % 4u == 0) ? r.S32()
                                                          : static_cast<int32_t>(r.U32() % static_cast<uint32_t>(std::max(1, chord) * 2)) - chord / 2);
                if (r.Next() % 3u == 0) PutS32(m, e + 0x170, r.S32());
            }
            args.a[0] = e + 0x1C2;
            args.a[1] = e + 0x148;
            args.a[2] = e + 0x168;
        },
        nullptr, /*compareV0=*/false,
        [](NativeEnv& env, const Args& a) -> uint32_t {
            GuestRam g = RrView(env);
            rr::sim::RoadPosition(g, a.a[0], a.a[1], a.a[2], env.sp);
            return RqDone(env, g, 0u);
        }});

    // ---------------------------------------------------------------- 0x8003DFF4
    rows.push_back(Row{
        "road_classify", "void RoadClassify(Entity *e)  [SLUS 0x8003DFF4]", rrt::kClassify,
        [](CaseContext& c, Args& args) {
            Memory& m = *c.mem;
            const RqNet n = RqRead(m);
            const uint32_t e = RrEntity(c, n, c.sample == nullptr, 4);
            if (!c.sample) {
                RrStirXsih(c, m.PeekWord(e + 0x148));
                if (c.rng->Next() % 3u == 0) {
                    RqPlantCoreCursor(c, n, e + 0x148, (c.rng->Next() & 1u) != 0, false);
                    m.PokeWord(e + 0x168, RrKeyOf(m, e));
                }
                static const uint32_t kRun[3] = {0u, 0xFFFFFFFFu, 1u};
                m.PokeWord(e + 0x1EC, kRun[c.rng->U32() % 3u]);
                m.PokeWord(e + 0x1F0, kRun[c.rng->U32() % 3u]);
                RrStackJunk(c, n, 512);
            }
            args.a[0] = e;
        },
        nullptr, /*compareV0=*/true,
        [](NativeEnv& env, const Args& a) -> uint32_t {
            GuestRam g = RrView(env);
            rr::sim::RoadRuntimeNative seams;
            return RqDone(env, g, rr::sim::RoadClassify(g, a.a[0], env.sp, seams));
        },
        {}});

    // ---------------------------------------------------------------- 0x8003E150
    rows.push_back(Row{
        "road_class_pass", "void RoadClassPass(void)  [SLUS 0x8003E150, pass H call 2]", rrt::kClassPass,
        [](CaseContext& c, Args& args) {
            (void)args;
            const RqNet n = RqRead(*c.mem);
            RrPlantList(c, n, 0, [&c, &n](uint32_t e) {
                Memory& m = *c.mem;
                RrStirXsih(c, m.PeekWord(e + 0x148));
                if (c.rng->Next() % 3u == 0) {
                    RqPlantCoreCursor(c, n, e + 0x148, (c.rng->Next() & 1u) != 0, false);
                    m.PokeWord(e + 0x168, RrKeyOf(m, e));
                }
                m.PokeWord(e + 0x358, (c.rng->Next() % 3u == 0) ? m.PeekWord(e + 0x354) : 0u);
                if (c.rng->Next() % 4u == 0) m.PokeWord(e + 0x238, m.PeekWord(e + 0x238) ^ 0x800u);
            });
            if (!c.sample) RrStackJunk(c, n, 512);
        },
        nullptr, /*compareV0=*/true,
        [](NativeEnv& env, const Args&) -> uint32_t {
            GuestRam g = RrView(env);
            rr::sim::RoadRuntimeNative seams;
            return RqDone(env, g, rr::sim::RoadClassPass(g, env.sp, seams));
        },
        {}});

    // ---------------------------------------------------------------- 0x8003BE1C
    rows.push_back(Row{
        "turn_sub_object", "Sub *TurnSubObject(Cursor *c, s32 len, s32 road, s32 *dir)  [SLUS 0x8003BE1C]",
        rrt::kTurnSub,
        [](CaseContext& c, Args& args) {
            Memory& m = *c.mem;
            Rng& r = *c.rng;
            const RqNet n = RqRead(m);
            const uint32_t e = RrEntity(c, n, c.sample == nullptr);
            uint32_t cur = e + 0x148;
            if (!c.sample && r.Next() % 4u != 0)
                RqPlantCoreCursor(c, n, cur, (r.Next() & 1u) != 0, false);
            // a record-shaped block for the length and the road, aimed at the cursor's junction
            const uint32_t rec = RrScratch(c, rrt::kRec);
            RrNoise(c, rec, 120);
            PutS32(m, rec + 8, r.S32() >> (r.U32() % 16u));
            PutS32(m, rec + 0x54, static_cast<int32_t>(r.U32() % 16u));
            if (r.Next() % 4u != 0) RrAimTurn(c, n, cur, rec);
            if (!c.sample) {
                if (r.Next() % 10u == 0) cur = 0;
                if (r.Next() % 10u == 0) PutS32(m, rec + 0x54, -1);
                RqStirTurns(c, n);
            }
            const uint32_t dir = RrScratch(c, rrt::kOut);
            m.PokeWord(dir, r.U32());
            args.a[0] = cur;
            args.a[1] = m.PeekWord(rec + 8);
            args.a[2] = m.PeekWord(rec + 0x54);
            args.a[3] = dir;
        },
        nullptr, /*compareV0=*/true,
        [](NativeEnv& env, const Args& a) -> uint32_t {
            GuestRam g = RrView(env);
            return RqDone(env, g, rr::sim::TurnSubObject(g, a.a[0], static_cast<int32_t>(a.a[1]),
                                                         static_cast<int32_t>(a.a[2]), a.a[3]));
        }});

    // ---------------------------------------------------------------- 0x8003B61C
    rows.push_back(Row{
        "progress_scalar", "s32 ProgressScalar(u8 *p)  [SLUS 0x8003B61C, p = e + 0xAC]", rrt::kProgress,
        [](CaseContext& c, Args& args) {
            const RqNet n = RqRead(*c.mem);
            const uint32_t e = RrEntity(c, n, c.sample == nullptr, 4);
            if (!c.sample) {
                RrStirProgress(c, n, e);
                RqStirTurns(c, n);
                RrStackJunk(c, n, 640);
            }
            args.a[0] = e + 0xAC;
        },
        nullptr, /*compareV0=*/true,
        [](NativeEnv& env, const Args& a) -> uint32_t {
            GuestRam g = RrView(env);
            return RqDone(env, g, static_cast<uint32_t>(rr::sim::ProgressScalar(g, a.a[0], env.sp)));
        }});

    // ---------------------------------------------------------------- 0x8003B520
    rows.push_back(Row{
        "road_progress_pass", "void ProgressPass(void)  [SLUS 0x8003B520, pass H call 4]", rrt::kProgressPass,
        [](CaseContext& c, Args& args) {
            (void)args;
            const RqNet n = RqRead(*c.mem);
            RrPlantList(c, n, 0, [&c, &n](uint32_t e) {
                Memory& m = *c.mem;
                Rng& r = *c.rng;
                RrStirProgress(c, n, e);
                m.PokeWord(e + 0x358, (r.Next() % 3u == 0) ? m.PeekWord(e + 0x354) : 0u);
                // the last mile: an old progress just above one of the five markers, and a new one
                // (no leg for the key: +0x170 >> 4) just below it
                if (r.Next() & 1u) {
                    const int32_t t = 0x649581 - static_cast<int32_t>(r.U32() % 5u) * 0x192560;
                    PutS32(m, e + 0x144, t + static_cast<int32_t>(r.U32() % 0x20000u) - 0x4000);
                    if (r.Next() & 1u) {
                        m.PokeWord(e + 0x168, 0x3FFFu);
                        PutS32(m, e + 0x170, (t - static_cast<int32_t>(r.U32() % 0x20000u)) * 16);
                    }
                }
            });
            if (!c.sample) RrStackJunk(c, n, 640);
        },
        nullptr, /*compareV0=*/true,
        [](NativeEnv& env, const Args&) -> uint32_t {
            GuestRam g = RrView(env);
            return RqDone(env, g, rr::sim::ProgressPass(g, env.sp));
        }});

    // ---------------------------------------------------------------- 0x8003AF9C
    rows.push_back(Row{
        "route_bind", "void RouteBind(u8 *p, s32 step, s32 x)  [SLUS 0x8003AF9C, p = e + 0xAC]", rrt::kRouteBind,
        [](CaseContext& c, Args& args) {
            const RqNet n = RqRead(*c.mem);
            const uint32_t e = RrEntity(c, n, c.sample == nullptr, 4);
            static const int32_t kStep[4] = {1, 1, 0, 2};
            args.a[0] = (!c.sample && c.rng->Next() % 10u == 0) ? 0u : e + 0xAC;
            args.a[1] = static_cast<uint32_t>(c.sample ? 1 : kStep[c.rng->U32() % 4u]);
            // `x` is an ENTITY: RouteBindFirst reads x[+0x1AC] (0x8003B0BC, 0x8003B168). Every
            // caller in the layer passes 0; the drawn case is another live bike.
            const uint32_t pool = c.mem->PeekWord(kPoolBasePtr);
            args.a[2] = (c.sample || (c.rng->Next() & 1u)) ? 0u : pool + 1096u * (c.rng->U32() % 18u);
        },
        // v0 is the callee's, or - for p == 0 - whatever the CALLER left in v0, which no port has
        nullptr, /*compareV0=*/false,
        [](NativeEnv& env, const Args& a) -> uint32_t {
            GuestRam g = RrView(env);
            rr::sim::RoadRuntimeNative seams;
            return RqDone(env, g, rr::sim::RouteBind(g, a.a[0], static_cast<int32_t>(a.a[1]), a.a[2], env.sp, seams));
        },
        {}});

    // ---------------------------------------------------------------- 0x8003A9D8
    rows.push_back(Row{
        "roadside_zones", "void RoadsideZones(Entity *e)  [SLUS 0x8003A9D8]", rrt::kZones,
        [](CaseContext& c, Args& args) {
            const RqNet n = RqRead(*c.mem);
            const uint32_t e = RrEntity(c, n, c.sample == nullptr);
            if (!c.sample) RrStirZones(c, e);
            args.a[0] = e;
        },
        nullptr, /*compareV0=*/true,
        [](NativeEnv& env, const Args& a) -> uint32_t {
            GuestRam g = RrView(env);
            return RqDone(env, g, rr::sim::RoadsideZones(g, a.a[0], env.sp));
        }});

    // ---------------------------------------------------------------- 0x8003AE24
    rows.push_back(Row{
        "route_check_pass", "void RouteCheckPass(void)  [SLUS 0x8003AE24, pass H call 3]", rrt::kRouteCheck,
        [](CaseContext& c, Args& args) {
            (void)args;
            const RqNet n = RqRead(*c.mem);
            RrPlantList(c, n, 0, [&c, &n](uint32_t e) {
                Memory& m = *c.mem;
                Rng& r = *c.rng;
                // NAMED BOUND: the zones are drawn LAST, from the entity's final cursor. The run
                // bytes index the cursor object's own BZDT/BSDT, and a cursor re-seated after the
                // run was drawn sends the original through another object's tables with this
                // one's indices - measured: 3 of 1024 cases on the second seed read unmapped
                // memory at 0x8003DCEC before this order.
                RrStirProgress(c, n, e);
                RrStirZones(c, e);
                WriteU16Bench(m, e + 0xAC, static_cast<uint16_t>((e - m.PeekWord(kPoolBasePtr)) / 1096u));
                m.PokeWord(e + 0x358, (r.Next() % 3u == 0) ? m.PeekWord(e + 0x354) : 0u);
                // the rider mounted or not
                const uint32_t rider = m.PeekWord(e + 0x354);
                m.PokeWord(rider + 0x25C, r.U32() % 5u);
            });
        },
        nullptr, /*compareV0=*/true,
        [](NativeEnv& env, const Args&) -> uint32_t {
            GuestRam g = RrView(env);
            rr::sim::RoadRuntimeNative seams;
            return RqDone(env, g, rr::sim::RouteCheckPass(g, env.sp, seams));
        },
        {}});

    // ---------------------------------------------------------------- 0x8003701C
    rows.push_back(Row{
        "road_track", "void RoadTrack(Entity *e)  [SLUS 0x8003701C]", rrt::kTrack,
        [](CaseContext& c, Args& args) {
            const RqNet n = RqRead(*c.mem);
            const uint32_t e = RrEntity(c, n, c.sample == nullptr, 4);
            if (!c.sample) {
                RrStirTracking(c, e);
                RqPlantTags(c, c.mem->PeekWord(e + 0x148));
                RrStackJunk(c, n, 640);
            }
            args.a[0] = e;
        },
        nullptr, /*compareV0=*/true,
        [](NativeEnv& env, const Args& a) -> uint32_t {
            GuestRam g = RrView(env);
            return RqDone(env, g, rr::sim::RoadTrack(g, a.a[0], env.sp));
        }});

    // ---------------------------------------------------------------- 0x800396A8
    rows.push_back(Row{
        "road_reseat", "void RoadReseat(Entity *e)  [SLUS 0x800396A8]", rrt::kReseat,
        [](CaseContext& c, Args& args) {
            const RqNet n = RqRead(*c.mem);
            const uint32_t e = RrEntity(c, n, c.sample == nullptr, 4);
            if (!c.sample) {
                if (c.rng->Next() % 3u != 0) RrNearArm(c, n, e);
                RrStackJunk(c, n, 640);
            }
            RrNearOwnStub(c, e);
            args.a[0] = e;
        },
        nullptr, /*compareV0=*/true,
        [](NativeEnv& env, const Args& a) -> uint32_t {
            GuestRam g = RrView(env);
            rr::sim::RoadRuntimeNative seams;
            return RqDone(env, g, rr::sim::RoadReseat(g, a.a[0], env.sp, seams));
        },
        {}});

    // ---------------------------------------------------------------- 0x80037104
    rows.push_back(Row{
        "road_probe_ahead", "void RoadProbeAhead(Entity *e)  [SLUS 0x80037104]", rrt::kProbe,
        [](CaseContext& c, Args& args) {
            const RqNet n = RqRead(*c.mem);
            const uint32_t e = RrEntity(c, n, c.sample == nullptr);
            if (!c.sample) {
                RrStirTracking(c, e);
                if (c.rng->Next() % 4u == 0) RrNearArm(c, n, c.mem->PeekWord(e + 0x358) ? c.mem->PeekWord(e + 0x358) : e);
                RrStackJunk(c, n, 640);
            }
            RrNearOwnStub(c, e);
            args.a[0] = e;
        },
        nullptr, /*compareV0=*/true,
        [](NativeEnv& env, const Args& a) -> uint32_t {
            GuestRam g = RrView(env);
            rr::sim::RoadRuntimeNative seams;
            return RqDone(env, g, rr::sim::RoadProbeAhead(g, a.a[0], env.sp, seams));
        },
        {}});

    // ---------------------------------------------------------------- 0x80037338
    rows.push_back(Row{
        "road_track_pass", "void RoadTrackPass(void)  [SLUS 0x80037338, pass H call 1]", rrt::kTrackPass,
        [](CaseContext& c, Args& args) {
            (void)args;
            const RqNet n = RqRead(*c.mem);
            RrPlantList(c, n, 0, [&c, &n](uint32_t e) {
                RrStirTracking(c, e);
                if (c.rng->Next() % 4u == 0) RrNearArm(c, n, e);
                RrNearOwnStub(c, e);
            });
            if (!c.sample) RrStackJunk(c, n, 640);
        },
        nullptr, /*compareV0=*/true,
        [](NativeEnv& env, const Args&) -> uint32_t {
            GuestRam g = RrView(env);
            rr::sim::RoadRuntimeNative seams;
            return RqDone(env, g, rr::sim::RoadTrackPass(g, env.sp, seams));
        },
        {}});

    // ---------------------------------------------------------------- 0x80037450 / 0x800374D4
    const auto rebindSetup = [](CaseContext& c) {
        const RqNet n = RqRead(*c.mem);
        const uint32_t e = RrEntity(c, n, c.sample == nullptr, 4);
        if (!c.sample) {
            Memory& m = *c.mem;
            RrStirTracking(c, e);
            RrStirXsih(c, m.PeekWord(e + 0x148));
            const uint64_t k = c.rng->Next() % 4u;
            if (k == 0) RrNearArm(c, n, e);
            else if (k == 1) RrStirProgress(c, n, e);
            if (c.rng->Next() & 1u) RrStirZones(c, e);
            RrStackJunk(c, n, 640);
        }
        RrNearOwnStub(c, e);
        return e;
    };
    rows.push_back(Row{
        "road_rebind_body", "u32 RoadRebindBody(Entity *e)  [SLUS 0x80037450]", rrt::kRebindBody,
        [rebindSetup](CaseContext& c, Args& args) { args.a[0] = rebindSetup(c); },
        nullptr, /*compareV0=*/true,
        [](NativeEnv& env, const Args& a) -> uint32_t {
            GuestRam g = RrView(env);
            rr::sim::RoadRuntimeNative seams;
            return RqDone(env, g, rr::sim::RoadRebindBody(g, a.a[0], env.sp, seams));
        },
        {}});
    rows.push_back(Row{
        "road_rebind", "u32 RoadRebind(Entity *e, s32 flag)  [SLUS 0x800374D4]", rrt::kRebind,
        [rebindSetup](CaseContext& c, Args& args) {
            args.a[0] = rebindSetup(c);
            static const uint32_t kFlag[4] = {1u, 1u, 0u, 0x100u};
            args.a[1] = c.sample ? 1u : kFlag[c.rng->U32() % 4u];
        },
        nullptr, /*compareV0=*/true,
        [](NativeEnv& env, const Args& a) -> uint32_t {
            GuestRam g = RrView(env);
            rr::sim::RoadRuntimeNative seams;
            return RqDone(env, g, rr::sim::RoadRebind(g, a.a[0], static_cast<int32_t>(a.a[1]), env.sp, seams));
        },
        {}});

    // Every row that reaches the slice search - directly, or through 0x8003E754 / 0x8003BFE8
    // (ported) - gets road_look_ahead's budget: a search from a re-seated cursor toward a point
    // thousands of units away walks slice by slice, and on the second seed one road_rebind case ran
    // the ORIGINAL past the default 5 million instructions (a step-limit trap inside 0x8003697C). A
    // budget changes nothing a case compares; a case that exhausts it still FAILS.
    static const uint32_t kSearching[] = {rrt::kClassNode, rrt::kClass, rrt::kClassify, rrt::kClassPass,
                                          rrt::kProgress, rrt::kProgressPass, rrt::kTrack, rrt::kReseat,
                                          rrt::kProbe, rrt::kTrackPass, rrt::kRebindBody, rrt::kRebind};
    for (Row& row : rows)
        for (uint32_t entry : kSearching)
            if (row.entry == entry && row.maxSteps < 200'000'000) row.maxSteps = 200'000'000;
}

// ================================================================ under the re-bind
//
// The six functions under the road runtime's re-bind and every function under them, one row each.
// 0x8003F3B4 (RouteRecordByKey)
// and 0x8003F580 (RouteHasExit) were already ported and benched by the road query (rows
// `route_leg_of_road`, `route_leg_has_road`) and are reused, not re-ported.
//
// INPUT FAMILIES, mined from roadrb.py's captures and synthetic families:
// rr-race holds junction 7 (object 14 = the core object: core piece 0 with three sub-objects, the
// stubs 1..3 on roads 9, 10, 8; object 15 its half), road 9 (object 67), the race graph STREAM1.GRF
// at *(gp+472), four route records and the START record at index count.
//   * dump-derived: a live bike with a captured bike's value fields, moved - as roadrb.py's
//     TELEPORT captures move it - onto the core or onto a stub with the captured lateral;
//   * randomised: the core entity with and without its junction +0x160 (the null sub-object path),
//     laterals on both sides of the turn and past the outer edge, hints -1 / in range / past the
//     arms, a point in no wedge (synthetic arm vectors), node records edited (two arms: the |lat|
//     path; more arms), XSIH / XSAI links cut (the three "no cross-section" exits), BGDT zone
//     records redrawn (type, wedge index, the road-pair arms, the cap of 3), entities off the road
//     on each stub with the box centre on ANOTHER stub's centre line (the re-bind), route bindings
//     for pools 0 (player, AI, police), 1 (a live rider) and 2..4 with the start record, a
//     successor, a non-successor and a record without exits, roads whose end node has degree 2.
// NAMED BOUNDS are stated where they are set.

namespace rrb {
constexpr uint32_t kGraphRoadFn = 0x800245DC, kGraphNodeFn = 0x800245F4, kEndNode = 0x8003A5F4,
                   kRecordOfRoad = 0x8003F4D8, kTurnByRoads = 0x8003BD2C, kLerp = 0x8003E61C,
                   kEdge = 0x8003E45C, kCross = 0x8003ED14;
// Scratch-block layout, above rows_road_runtime's 0x600..0xC40.
constexpr uint32_t kGroupB = 0xC40;  // a second noise cross-section group, 264 bytes
constexpr uint32_t kNode = 0xD50;    // a synthetic BIT_ record, up to 6 arms (152 bytes)
constexpr uint32_t kPoint = 0xDF0;   // a point, 12 bytes
constexpr uint32_t kArms = 0xE00;    // two arm-pointer words
constexpr uint32_t kOutB = 0xE10;    // an output block, 64 bytes
constexpr uint32_t kPos = 0xE50;     // a {key, dir, along} triple
constexpr uint32_t kCurB = 0xE60;    // a cursor, 32 bytes
} // namespace rrb

rr::sim::GuestRam RbView(CaseContext& c) { return rr::sim::GuestRam(c.mem->ram().data(), c.gp); }

// The junction core object of this machine (kind 1, half 0), its BIT_ record and its IPT_ record.
struct RbCore {
    uint32_t obj = 0, node = 0, ipt = 0;
};
RbCore RbFindCore(CaseContext& c, const RqNet& n) {
    RbCore k;
    rr::sim::GuestRam g = RbView(c);
    for (uint32_t obj : n.resident) {
        if (GetS16(*c.mem, obj + 16) != 1 || c.mem->PeekWord(obj + 12) != 0) continue;
        const int32_t id = GetS32(*c.mem, obj);
        const uint32_t node = rr::sim::RoadNodeRecord(g, id);
        if (node == 0) continue;
        k.obj = obj;
        k.node = node;
        k.ipt = rr::sim::RoadJunctionIndex(g, id);
        return k;
    }
    return k;
}

// The pieces of `obj` that are core pieces (+2 == 1) or stubs (+2 == 0).
std::vector<uint32_t> RbPieces(const Memory& m, uint32_t obj, bool core) {
    std::vector<uint32_t> out;
    const int32_t npc = GetS16(m, obj + 0x12);
    for (int32_t i = 0; i < npc && i < 64; ++i) {
        const uint32_t pc = m.PeekWord(obj + 0x2C) + 32u * static_cast<uint32_t>(i);
        if ((GetS16(m, pc + 2) == 1) == core) out.push_back(pc);
    }
    return out;
}

// roadrb.py's `place`: `e` on `piece` of `obj` - a sub-object of the piece (a core piece has one per
// turn), a slice in it, an along-distance around the slice and `lat` across - with the cursor,
// +0x158..+0x170, the box centre +0xB8 and the contact point +0x1F8 consistent; on a core piece the
// junction (IPT_, nine times in ten) and a turn of that sub-object. With `posSlice` the box centre
// is put on THAT slice's centre line instead (the re-bind family of RoadShortcut).
void RbPlace(CaseContext& c, uint32_t e, uint32_t obj, uint32_t piece, uint32_t ipt, int32_t lat,
             uint32_t posSlice = 0, bool firstSlice = false) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    const bool core = GetS16(m, piece + 2) == 1;
    const int32_t nsub = std::max<int32_t>(1, GetS16(m, piece + 22));
    const int32_t so = core ? static_cast<int32_t>(RqPick(r, nsub)) : 0;
    const uint32_t sub = m.PeekWord(obj + 0x30) + 28u * static_cast<uint32_t>(GetS16(m, piece + 20) + so);
    const int32_t first = GetS16(m, sub + 8), cnt = GetS16(m, sub + 10);
    const int32_t pick = static_cast<int32_t>(RqPick(r, cnt));
    const uint32_t slc = m.PeekWord(obj + 0x34) + 52u * static_cast<uint32_t>(first + (firstSlice ? 0 : pick));
    const int32_t chord = std::max(1, GetS32(m, slc + 32));
    const int32_t along = static_cast<int32_t>(r.U32() % static_cast<uint32_t>(chord + 0x40000)) - 0x20000;
    const uint32_t ps = (posSlice != 0) ? posSlice : slc;
    const int32_t palong = (posSlice != 0) ? std::max(1, GetS32(m, ps + 32)) / 2 : along;
    const int32_t plat = (posSlice != 0) ? (r.S32() >> 20) : lat;
    for (uint32_t k = 0; k < 3; ++k) {
        const int64_t pos = GetS32(m, ps + 20u + 4u * k);
        const int64_t t2 = GetS16(m, ps + 14u + 2u * k);
        const int64_t t0 = GetS16(m, ps + 2u + 2u * k);
        const int32_t p = static_cast<int32_t>(pos + ((t2 * palong) >> 12) + ((t0 * plat) >> 12));
        PutS32(m, e + 0xB8 + 4u * k, p);
        PutS32(m, e + 0x1F8 + 4u * k, p + (r.S32() >> 16));
    }
    uint32_t turn = 0;
    if (core && ipt != 0) {
        const RqNet n = RqRead(m);
        std::vector<uint32_t> all, match;
        for (int32_t q = 0; q < GetS16(m, ipt + 8) && q < 32; ++q) {
            const uint32_t t = n.pdt + 12u * static_cast<uint32_t>(GetS16(m, ipt + 6) + q);
            all.push_back(t);
            if (GetS16(m, n.gpdt + 12u * static_cast<uint32_t>(GetS16(m, t + 2)) + 4u) == so) match.push_back(t);
        }
        const std::vector<uint32_t>& from = (!match.empty() && r.Next() % 8u != 0) ? match : all;
        if (!from.empty()) turn = from[r.U32() % from.size()];
    }
    uint16_t hi = 0, lo = 0;
    m.ReadBlock(piece + 2, &hi, 2);
    m.ReadBlock(piece + 12, &lo, 2);
    m.PokeWord(e + 0x148, obj);
    m.PokeWord(e + 0x14C, piece);
    m.PokeWord(e + 0x150, sub);
    m.PokeWord(e + 0x154, slc);
    PutS32(m, e + 0x158, lat);
    PutS32(m, e + 0x15C, along);
    m.PokeWord(e + 0x160, (core && r.Next() % 10u != 0) ? ipt : 0u);
    m.PokeWord(e + 0x164, turn);
    m.PokeWord(e + 0x168, (static_cast<uint32_t>(hi) << 16) | lo);
    PutS32(m, e + 0x170, GetS32(m, slc + 40) + along);
}

// A lateral for the core: -45..45 units (roadrb's), or on an edge of a group of the object.
int32_t RbCoreLateral(CaseContext& c, uint32_t obj) {
    Rng& r = *c.rng;
    if (r.Next() % 3u == 0) return RrLateralNear(c, RrSomeGroup(c, obj));
    return (static_cast<int32_t>(r.U32() % 91u) - 45) * 65536 + static_cast<int32_t>(r.U32() % 65536u);
}

// The race graph at *(gp+472).
struct RbGraph {
    uint32_t nodes = 0, roads = 0;
    int32_t nNodes = 0, nRoads = 0;
};
RbGraph RbReadGraph(const Memory& m, uint32_t gp) {
    RbGraph g;
    const uint32_t base = m.PeekWord(gp + rr::sim::kRaceGraphGp);
    g.nNodes = GetS32(m, base + 12);
    g.nRoads = GetS32(m, base + 16);
    g.nodes = m.PeekWord(base + 20);
    g.roads = m.PeekWord(base + 24);
    return g;
}

// An along-distance for a road-end query on graph road `road`: near its start, near its end (both
// sides of the 50-unit gate), past either end, or anywhere.
int32_t RbAlongOnRoad(CaseContext& c, const RbGraph& g, int32_t road) {
    Rng& r = *c.rng;
    int32_t len = 0;
    if (road >= 0 && road < g.nRoads)
        len = static_cast<int32_t>((c.mem->PeekWord(g.roads + 16u * static_cast<uint32_t>(road) + 4u) >> 6) << 16);
    const int32_t near = static_cast<int32_t>(r.U32() % (60u * 65536u));
    switch (r.U32() % 6u) {
        case 0: return near;
        case 1: return len - near;
        case 2: return 0x320000 + static_cast<int32_t>(r.U32() % 5u) - 2;         // on the gate
        case 3: return len - 0x320000 + static_cast<int32_t>(r.U32() % 5u) - 2;
        case 4: return -static_cast<int32_t>(r.U32() % (100u * 65536u));
        default: return static_cast<int32_t>(r.U32() % static_cast<uint32_t>(std::max(1, len) + 0x640000));
    }
}

// A road id of the race graph: a road of a route record's legs, one whose end node has degree < 3
// (roads 11, 12 -> node 8), or any road of the graph. NAMED BOUND: a VALID graph road. GraphRoad
// has no bounds check; a road past the table reads a node id out of whatever follows it and
// GraphNode then multiplies that by 40 - an address the console faults on.
int32_t RbSomeRoad(CaseContext& c, const RbGraph& g) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    if (g.nRoads <= 0) return 0;
    const uint64_t k = r.Next() % 4u;
    if (k == 0) {
        std::vector<int32_t> low;
        for (int32_t i = 0; i < g.nRoads && i < 256; ++i) {
            const uint32_t rd = g.roads + 16u * static_cast<uint32_t>(i);
            for (uint32_t off : {8u, 12u}) {
                const uint32_t nd = m.PeekWord(rd + off);
                if (static_cast<int32_t>(nd) >= 0 && static_cast<int32_t>(nd) < g.nNodes &&
                    m.PeekWord(g.nodes + 40u * nd + 4u) < 3u)
                    low.push_back(i);
            }
        }
        if (!low.empty()) return low[r.U32() % low.size()];
    }
    if (k == 1) {
        const int32_t count = GetS16(m, rrt::kCount);
        const uint32_t recs = m.PeekWord(rrt::kRecords);
        if (count >= 0) {
            const uint32_t rec = recs + 120u * RqPick(r, count + 1);
            const int32_t legs = GetS32(m, rec + 12);
            if (legs > 0 && legs < 8) {
                const int32_t road = GetS32(m, rec + 20u + 16u * (r.U32() % static_cast<uint32_t>(legs)));
                if (road >= 0 && road < g.nRoads) return road;
            }
        }
    }
    return static_cast<int32_t>(RqPick(r, g.nRoads));
}

// The route binding around handle block `p` (p = e + 0xAC): the key +0xBC (a node of a record - its
// own, a successor, any - or a road), the along +0xC4 and direction +0xC0, the bound record +0x100
// (a real one, the START record at index count, or 0), and now and then the record count cut to
// 0 or 1 or a record's exits / successors redrawn.
void RbStirBinding(CaseContext& c, const RbGraph& g, uint32_t p) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    const int32_t count = GetS16(m, rrt::kCount);
    const uint32_t recs = m.PeekWord(rrt::kRecords);
    if (count < 0) return;
    const uint32_t R = recs + 120u * RqPick(r, count + 1);          // count: the start record
    int32_t road = -1;
    switch (r.U32() % 6u) {
        case 0: m.PokeWord(p + 0xBC, (1u << 16) | (m.PeekWord(R) & 0xFFFFu)); break;
        case 1: {
            const int32_t ns = GetS32(m, R + 16);
            const uint32_t succ = (ns > 0 && ns < 5) ? m.PeekWord(R + 100u + 4u * (r.U32() % static_cast<uint32_t>(ns))) : 7u;
            m.PokeWord(p + 0xBC, (1u << 16) | (succ & 0xFFFFu));
            break;
        }
        case 2: m.PokeWord(p + 0xBC, (1u << 16) | (r.U32() % 26u)); break;
        default: road = RbSomeRoad(c, g); m.PokeWord(p + 0xBC, static_cast<uint32_t>(road)); break;
    }
    PutS32(m, p + 0xC4, (road >= 0) ? RbAlongOnRoad(c, g, road) : r.World());
    PutS32(m, p + 0xC0, (r.Next() & 1u) ? 1 : -1);
    const uint64_t b = r.Next() % 8u;
    m.PokeWord(p + 0x100, (b == 0) ? 0u : recs + 120u * RqPick(r, count + 1));
    if (r.Next() % 6u == 0) {                                       // a record redrawn
        const uint32_t Q = recs + 120u * RqPick(r, count + 1);
        PutS32(m, Q + 16, static_cast<int32_t>(r.U32() % 4u) - 1);
        for (uint32_t k = 0; k < 4; ++k) {
            PutS32(m, Q + 84u + 4u * k, (r.Next() & 1u) ? RbSomeRoad(c, g) : -1);
            PutS32(m, Q + 100u + 4u * k, (r.Next() % 3u == 0) ? -1 : static_cast<int32_t>(r.U32() % 12u));
        }
    }
    if (r.Next() % 10u == 0) PutS16(m, rrt::kCount, static_cast<int16_t>(r.U32() % 2u));
}

// The node zone records of `obj` (BGDT 0..3) redrawn as tools\scout\roadrb.py's zone family does:
// type 0 / 1, the wedge index, road byte +8 = 255 or an arm road, +9 an arm road; and the
// count +0x4E and the block pointer +0x5C now and then.
void RbStirZoneRecords(CaseContext& c, uint32_t obj, uint32_t node) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    const uint32_t bg = m.PeekWord(obj + 0x5C);
    if (bg != 0 && (r.Next() & 1u)) {
        std::vector<uint8_t> roads;
        for (int32_t a = 0; a < GetS16(m, node + 2) && a < 4; ++a)
            roads.push_back(static_cast<uint8_t>(GetS16(m, node + 8u + 24u * static_cast<uint32_t>(a) + 4u)));
        roads.push_back(static_cast<uint8_t>(r.U32() % 40u));
        for (uint32_t q = 0; q < 4; ++q) {
            const uint32_t rec = bg + 20u * q;
            const uint8_t b[4] = {static_cast<uint8_t>((r.Next() % 3u == 0) ? 0u : 1u), static_cast<uint8_t>(r.U32() % 4u),
                                  static_cast<uint8_t>((r.Next() & 1u) ? 255u : roads[r.U32() % roads.size()]),
                                  roads[r.U32() % roads.size()]};
            m.WriteBlock(rec + 2, &b[0], 1);
            m.WriteBlock(rec + 3, &b[1], 1);
            m.WriteBlock(rec + 8, &b[2], 1);
            m.WriteBlock(rec + 9, &b[3], 1);
        }
    }
    if (r.Next() % 6u == 0) PutS16(m, obj + 0x4E, static_cast<int16_t>(static_cast<int32_t>(r.U32() % 7u) - 1));
    if (r.Next() % 12u == 0) m.PokeWord(obj + 0x5C, 0);
}

// A synthetic BIT_ record at `at` with `arms` arms (NAMED BOUND: at most 6 - the distances of
// NodeWedge live at its sp+16 + 4k, and from the seventh on they overwrite its own callee-saved
// spills, which the original then restores into its CALLER's registers; the tables have at most 4).
// Arm vectors are units, the arm points around `centre`.
uint32_t RbSyntheticNode(CaseContext& c, uint32_t at, int32_t arms, const int32_t centre[3]) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    RrNoise(c, at, 160);
    PutS16(m, at + 2, static_cast<int16_t>(arms));
    for (uint32_t a = 0; a < 6; ++a) {
        const uint32_t arm = at + 8u + 24u * a;
        PutS16(m, arm + 2, (r.Next() & 1u) ? 1 : -1);
        PutS16(m, arm + 4, static_cast<int16_t>(r.U32() % 13u));
        for (uint32_t k = 0; k < 3; ++k) {
            PutS16(m, arm + 6 + 2u * k, r.Unit());
            PutS32(m, arm + 12 + 4u * k, centre[k] + (r.S32() >> 10));
        }
    }
    return at;
}

void AddRoadRebindRows(std::vector<Row>& rows) {
    using rr::sim::GuestRam;
    const auto rowIndex = rows.size();

    // ---------------------------------------------------------------- 0x800245DC / 0x800245F4
    rows.push_back(Row{
        "graph_road", "Road *GraphRoad(u32 id)  [SLUS 0x800245DC]", rrb::kGraphRoadFn,
        [](CaseContext& c, Args& args) {
            const RbGraph g = RbReadGraph(*c.mem, c.gp);
            args.a[0] = (c.sample != nullptr) ? (c.mem->PeekWord(RrEntity(c, RqRead(*c.mem), false) + 0x168) & 0xFFFFu)
                        : (c.rng->Next() % 4u == 0) ? c.rng->U32()
                                                     : RqPick(*c.rng, g.nRoads + 3);
        },
        nullptr, /*compareV0=*/true,
        [](NativeEnv& env, const Args& a) -> uint32_t {
            GuestRam g = RrView(env);
            return RqDone(env, g, rr::sim::GraphRoad(g, a.a[0]));
        }});
    rows.push_back(Row{
        "graph_node", "Node *GraphNode(u32 id)  [SLUS 0x800245F4]", rrb::kGraphNodeFn,
        [](CaseContext& c, Args& args) {
            const RbGraph g = RbReadGraph(*c.mem, c.gp);
            args.a[0] = (c.sample != nullptr) ? RqPick(*c.rng, g.nNodes)
                        : (c.rng->Next() % 4u == 0) ? c.rng->U32()
                                                     : RqPick(*c.rng, g.nNodes + 3);
        },
        nullptr, /*compareV0=*/true,
        [](NativeEnv& env, const Args& a) -> uint32_t {
            GuestRam g = RrView(env);
            return RqDone(env, g, rr::sim::GraphNode(g, a.a[0]));
        }});

    // ---------------------------------------------------------------- 0x8003A5F4
    rows.push_back(Row{
        "road_end_node", "s32 RoadEndNode(u32 pos[3], s32 *node, s32 mode)  [SLUS 0x8003A5F4]", rrb::kEndNode,
        [](CaseContext& c, Args& args) {
            Memory& m = *c.mem;
            Rng& r = *c.rng;
            const RqNet n = RqRead(m);
            const RbGraph g = RbReadGraph(m, c.gp);
            const uint32_t e = RrEntity(c, n, false);
            uint32_t pos = e + 0x168;
            if (c.sample == nullptr) {
                if (r.Next() & 1u) pos = RrScratch(c, rrb::kPos);
                const uint64_t k = r.Next() % 5u;
                int32_t road = -1;
                if (k == 0) m.PokeWord(pos, (1u << 16) | (r.U32() % static_cast<uint32_t>(std::max(1, g.nNodes + 2))));
                else if (k == 1) m.PokeWord(pos, (1u << 16) | (r.U32() & 0xFFFFu));
                else {
                    road = RbSomeRoad(c, g);
                    m.PokeWord(pos, ((r.Next() % 12u == 0) ? ((2u + r.U32() % 3u) << 16) : 0u) | static_cast<uint32_t>(road));
                }
                PutS32(m, pos + 4, (r.Next() % 6u == 0) ? r.S32() : ((r.Next() & 1u) ? 1 : -1));
                PutS32(m, pos + 8, (road >= 0) ? RbAlongOnRoad(c, g, road) : r.World());
                if (r.Next() % 10u == 0) {                          // a node of degree 2, directly
                    for (int32_t i = 0; i < g.nNodes; ++i)
                        if (c.mem->PeekWord(g.nodes + 40u * static_cast<uint32_t>(i) + 4u) < 3u)
                            m.PokeWord(pos, (1u << 16) | static_cast<uint32_t>(i));
                }
            } else {
                // the captured bike's key is a road of ITS race; keep this graph's roads valid
                const uint32_t key = m.PeekWord(pos);
                if ((key >> 16) != 1 && static_cast<int32_t>(key & 0xFFFFu) >= g.nRoads)
                    m.PokeWord(pos, static_cast<uint32_t>(RbSomeRoad(c, g)));
            }
            const uint32_t out = RrScratch(c, rrb::kArms);
            m.PokeWord(out, r.U32());
            static const int32_t kMode[6] = {0, 0, 0, 1, 2, -1};
            args.a[0] = pos;
            args.a[1] = out;
            args.a[2] = static_cast<uint32_t>((c.sample != nullptr) ? 0 : kMode[r.U32() % 6u]);
        },
        nullptr, /*compareV0=*/true,
        [](NativeEnv& env, const Args& a) -> uint32_t {
            GuestRam g = RrView(env);
            return RqDone(env, g, static_cast<uint32_t>(rr::sim::RoadEndNode(g, a.a[0], a.a[1],
                                                                              static_cast<int32_t>(a.a[2]))));
        }});

    // ---------------------------------------------------------------- 0x8003F4D8
    rows.push_back(Row{
        "route_record_of_road", "Record *RouteRecordOfRoad(u32 road)  [SLUS 0x8003F4D8]", rrb::kRecordOfRoad,
        [](CaseContext& c, Args& args) {
            Memory& m = *c.mem;
            Rng& r = *c.rng;
            const RbGraph g = RbReadGraph(m, c.gp);
            uint32_t road = static_cast<uint32_t>(RbSomeRoad(c, g));
            if (c.sample != nullptr) {
                road = m.PeekWord(RrEntity(c, RqRead(m), false) + 0x168) & 0xFFFFu;
            } else {
                if (r.Next() % 6u == 0) road = r.U32() % 40u;
                if (r.Next() % 8u == 0) PutS16(m, rrt::kCount, static_cast<int16_t>(static_cast<int32_t>(r.U32() % 7u) - 1));
                if (r.Next() % 4u == 0) {                           // a leg of some record on the road
                    const int32_t count = std::max<int32_t>(1, GetS16(m, rrt::kCount) + 1);
                    const uint32_t rec = m.PeekWord(rrt::kRecords) + 120u * RqPick(r, count);
                    PutS32(m, rec + 12, 1 + static_cast<int32_t>(r.U32() % 5u));
                    m.PokeWord(rec + 20u + 16u * (r.U32() % 5u), road);
                }
            }
            args.a[0] = road;
        },
        nullptr, /*compareV0=*/true,
        [](NativeEnv& env, const Args& a) -> uint32_t {
            GuestRam g = RrView(env);
            return RqDone(env, g, rr::sim::RouteRecordOfRoad(g, a.a[0]));
        }});

    // ---------------------------------------------------------------- 0x8003B024 / 0x8003B1C4
    // The handle block of one of four kinds: a live bike (pool 0: player, AI or police by the
    // players count and the rider class RqBike draws), a live rider (pool 1), a traffic record
    // (pool 3) or one relabelled pool 2 / 4. NAMED BOUND: a pool-1 handle names one of the 18 live
    // riders (RouteBindFirst follows riderPool[slot]+0x254).
    struct RbBinding {
        uint32_t p = 0, pool = 0, e = 0, x = 0;
    };
    const auto bindingSetup = [](CaseContext& c) {
        Memory& m = *c.mem;
        Rng& r = *c.rng;
        const RqNet n = RqRead(m);
        const RbGraph g = RbReadGraph(m, c.gp);
        RbBinding b;
        const uint32_t bike = RrEntity(c, n, c.sample == nullptr);
        const uint32_t pool0 = m.PeekWord(kPoolBasePtr);
        const uint64_t kind = (c.sample != nullptr) ? 0u : r.Next() % 8u;
        if (kind < 5) {                                             // a bike
            const uint32_t slot = (bike - pool0) / 1096u;
            WriteU16Bench(m, bike + 0xAC, static_cast<uint16_t>(slot));
            b.p = bike + 0xAC;
            b.pool = 0;
            b.e = bike;
        } else if (kind == 5) {                                     // a rider
            uint32_t k = r.U32() % 18u;
            if (r.Next() % 12u == 0) {                              // no rider pool at all: slot 0 of a
                k = 0;                                              // pool at 0 (0x8003B13C's null test)
                m.PokeWord(rr::sim::kRoadPool1Ptr, 0);
            }
            const uint32_t rider = m.PeekWord(rr::sim::kRoadPool1Ptr) + 628u * k;
            WriteU16Bench(m, rider + 0xAC, static_cast<uint16_t>((1u << 5) | k));
            b.p = rider + 0xAC;
            b.pool = 1;
        } else {                                                    // traffic, or relabelled
            const uint32_t k = r.U32() % 4u;
            const uint32_t pool = (kind == 6) ? 3u : ((r.Next() & 1u) ? 2u : 4u);
            b.p = rr::sim::kRoadTraffic + 512u * k + 0xAC;
            WriteU16Bench(m, b.p, static_cast<uint16_t>((pool << 5) | k));
            b.pool = pool;
        }
        if (c.sample == nullptr) RbStirBinding(c, g, b.p);
        else {                                                      // the captured bike's values, a graph road
            const uint32_t key = m.PeekWord(b.p + 0xBC);
            if ((key >> 16) != 1 && static_cast<int32_t>(key & 0xFFFFu) >= g.nRoads)
                m.PokeWord(b.p + 0xBC, static_cast<uint32_t>(RbSomeRoad(c, g)));
        }
        b.x = (c.sample == nullptr && (r.Next() % 3u == 0)) ? pool0 + 1096u * (r.U32() % 18u) : 0u;
        return b;
    };
    rows.push_back(Row{
        "route_bind_first", "void RouteBindFirst(u8 *p, Entity *e, s32 pool, Entity *x)  [SLUS 0x8003B024]",
        rrt::kBindFirst,
        [bindingSetup](CaseContext& c, Args& args) {
            RbBinding b = bindingSetup(c);
            if (c.sample == nullptr && c.rng->Next() % 16u == 0) b.p = 0;
            args.a[0] = b.p;
            args.a[1] = b.e;
            args.a[2] = b.pool;
            args.a[3] = b.x;
        },
        nullptr, /*compareV0=*/true,
        [](NativeEnv& env, const Args& a) -> uint32_t {
            GuestRam g = RrView(env);
            // p == 0 leaves the caller's v0: here the register file the bench started the call with
            return RqDone(env, g, rr::sim::RouteBindFirst(g, a.a[0], a.a[1], a.a[2], a.a[3], env.clone->cpu.regs[2]));
        }});
    rows.push_back(Row{
        "route_bind_step", "void RouteBindStep(u8 *p, s32 pool, Entity *e, Entity *x)  [SLUS 0x8003B1C4]",
        rrt::kBindStep,
        [bindingSetup](CaseContext& c, Args& args) {
            RbBinding b = bindingSetup(c);
            if (c.sample == nullptr) {
                RrStackJunk(c, RqRead(*c.mem), 128);
                if (c.rng->Next() % 16u == 0) b.e = 0;              // pool 0 without an entity
            }
            args.a[0] = b.p;
            args.a[1] = b.pool;
            args.a[2] = b.e;
            args.a[3] = b.x;
        },
        nullptr, /*compareV0=*/true,
        [](NativeEnv& env, const Args& a) -> uint32_t {
            GuestRam g = RrView(env);
            return RqDone(env, g, rr::sim::RouteBindStep(g, a.a[0], a.a[1], a.a[2], a.a[3], env.sp));
        }});

    // ---------------------------------------------------------------- 0x8003BD2C
    rows.push_back(Row{
        "turn_by_roads", "Sub *TurnByRoads(Cursor *c, s32 from, s32 to, s32 *dir, Pdt **turn)  [SLUS 0x8003BD2C]",
        rrb::kTurnByRoads,
        [](CaseContext& c, Args& args) {
            Memory& m = *c.mem;
            Rng& r = *c.rng;
            const RqNet n = RqRead(m);
            const RbCore k = RbFindCore(c, n);
            const uint32_t e = RrEntity(c, n, c.sample == nullptr);
            uint32_t cur = e + 0x148;
            if (k.obj != 0) {
                const std::vector<uint32_t> cores = RbPieces(m, k.obj, true), stubs = RbPieces(m, k.obj, false);
                const bool core = !cores.empty() && (stubs.empty() || r.Next() % 4u != 0);
                const std::vector<uint32_t>& from = core ? cores : stubs;
                if (!from.empty())
                    RbPlace(c, e, k.obj, from[r.U32() % from.size()], k.ipt,
                            c.sample ? c.sample->lateral : RbCoreLateral(c, k.obj));
            }
            // the road pair: a PDT_ record's own (from, to) - the found arm - or the wedge's arms, or any
            int32_t from = 0, to = 0;
            const uint32_t j = m.PeekWord(e + 0x160);
            if (j != 0 && GetS16(m, j + 8) > 0 && r.Next() % 4u != 0) {
                const uint32_t t = n.pdt + 12u * static_cast<uint32_t>(GetS16(m, j + 6) + static_cast<int32_t>(RqPick(r, GetS16(m, j + 8))));
                from = GetS16(m, t + 8);
                to = GetS16(m, t + 10);
            } else if (k.node != 0) {
                const int32_t arms = std::max<int32_t>(1, GetS16(m, k.node + 2));
                from = GetS16(m, k.node + 8u + 24u * RqPick(r, arms) + 4u);
                to = GetS16(m, k.node + 8u + 24u * RqPick(r, arms) + 4u);
            }
            if (c.sample == nullptr) {
                if (r.Next() % 8u == 0) from = static_cast<int32_t>(r.U32() % 16u);
                if (r.Next() % 8u == 0) to = r.S32();
                if (r.Next() % 12u == 0) cur = 0;
                if (r.Next() % 8u == 0) m.PokeWord(e + 0x160, 0);
                RqStirTurns(c, n);
            }
            const uint32_t out = RrScratch(c, rrb::kArms);
            m.PokeWord(out, r.U32());
            m.PokeWord(out + 4, r.U32());
            args.a[0] = cur;
            args.a[1] = static_cast<uint32_t>(from);
            args.a[2] = static_cast<uint32_t>(to);
            args.a[3] = out;
            args.a4 = out + 4;                                      // the turn out, at sp+16
        },
        nullptr, /*compareV0=*/true,
        [](NativeEnv& env, const Args& a) -> uint32_t {
            GuestRam g = RrView(env);
            return RqDone(env, g, rr::sim::TurnByRoads(g, a.a[0], static_cast<int32_t>(a.a[1]),
                                                       static_cast<int32_t>(a.a[2]), a.a[3], a.a4));
        }});

    // ---------------------------------------------------------------- 0x8003E61C
    rows.push_back(Row{
        "lerp16", "u32 Lerp16(u32 t, u32 a, u32 b)  [SLUS 0x8003E61C]", rrb::kLerp,
        [](CaseContext& c, Args& args) {
            Rng& r = *c.rng;
            static const uint32_t kT[4] = {0u, 0x10000u, 0x8000u, 0xFFFFFFFFu};
            args.a[0] = (r.Next() % 4u == 0) ? kT[r.U32() % 4u] : ((r.Next() & 1u) ? r.U32() % 0x10001u : r.U32());
            args.a[1] = c.sample ? static_cast<uint32_t>(c.sample->lateral) : static_cast<uint32_t>(r.World());
            args.a[2] = (r.Next() & 1u) ? static_cast<uint32_t>(r.World()) : r.U32();
        },
        nullptr, /*compareV0=*/true,
        [](NativeEnv& env, const Args& a) -> uint32_t {
            GuestRam g = RrView(env);
            return RqDone(env, g, rr::sim::Lerp16(g, a.a[0], a.a[1], a.a[2], env.sp));
        }});

    // Two cross-section groups: real ones of the core object (its XSDH), or noise.
    const auto twoGroups = [](CaseContext& c, const RqNet& n, uint32_t& gA, uint32_t& gB) {
        const RbCore k = RbFindCore(c, n);
        const uint32_t obj = (k.obj != 0) ? k.obj : RrSomeObject(c, n);
        gA = (c.rng->Next() % 4u != 0) ? RrSomeGroup(c, obj) : 0u;
        gB = (c.rng->Next() % 4u != 0) ? RrSomeGroup(c, obj) : 0u;
        if (gA == 0) {
            gA = RrScratch(c, rrt::kGroup);
            RrNoiseGroup(c, gA);
        }
        if (gB == 0) {
            gB = RrScratch(c, rrb::kGroupB);
            RrNoiseGroup(c, gB);
        }
    };
    const auto someFraction = [](Rng& r) {
        static const uint32_t kF[3] = {0u, 0x10000u, 0x8000u};
        const uint64_t k = r.Next() % 6u;
        return (k == 0) ? kF[r.U32() % 3u] : (k == 1) ? r.U32() : r.U32() % 0x10001u;
    };
    const auto someSide = [](Rng& r) {
        static const int32_t kS[6] = {1, 2, 2, 1, 0, 3};
        return static_cast<uint32_t>((r.Next() % 8u == 0) ? r.S32() : kS[r.U32() % 6u]);
    };

    // ---------------------------------------------------------------- 0x8003E45C
    rows.push_back(Row{
        "core_edge_class",
        "s32 CoreEdgeClass(Group *a, Group *b, u32 f, s32 lat, s32 sideA, s32 sideB, s32 dir, Out *o)  [SLUS 0x8003E45C]",
        rrb::kEdge,
        [twoGroups, someFraction, someSide](CaseContext& c, Args& args) {
            Rng& r = *c.rng;
            const RqNet n = RqRead(*c.mem);
            uint32_t gA = 0, gB = 0;
            twoGroups(c, n, gA, gB);
            const uint32_t out = RrScratch(c, rrb::kOutB);
            RrNoise(c, out, 16);
            args.a[0] = gA;
            args.a[1] = gB;
            args.a[2] = someFraction(r);
            args.a[3] = static_cast<uint32_t>(c.sample ? c.sample->lateral : RrLateralNear(c, (r.Next() & 1u) ? gA : gB));
            args.a4 = someSide(r);
            args.more[0] = someSide(r);
            args.more[1] = static_cast<uint32_t>((r.Next() % 6u == 0) ? r.S32() : ((r.Next() % 4u != 0) ? 1 : -1));
            args.more[2] = out;
        },
        nullptr, /*compareV0=*/true,
        [](NativeEnv& env, const Args& a) -> uint32_t {
            GuestRam g = RrView(env);
            return RqDone(env, g, static_cast<uint32_t>(rr::sim::CoreEdgeClass(
                                      g, a.a[0], a.a[1], a.a[2], static_cast<int32_t>(a.a[3]), static_cast<int32_t>(a.a4),
                                      static_cast<int32_t>(a.more[0]), static_cast<int32_t>(a.more[1]), a.more[2], env.sp)));
        }});

    // ---------------------------------------------------------------- 0x8003ED14
    rows.push_back(Row{
        "core_cross_section",
        "s32 CoreCrossSection(Group *a, Xsih *x, s32 dA, s32 dB, Group *b, s32 lat, u32 f, Out *o, s32 dir)  [SLUS 0x8003ED14]",
        rrb::kCross,
        [twoGroups, someFraction](CaseContext& c, Args& args) {
            Memory& m = *c.mem;
            Rng& r = *c.rng;
            const RqNet n = RqRead(m);
            uint32_t gA = 0, gB = 0;
            twoGroups(c, n, gA, gB);
            const RbCore k = RbFindCore(c, n);
            uint32_t out = RrScratch(c, rrb::kOutB);
            RrNoise(c, out, 64);
            if (c.sample != nullptr || (r.Next() & 1u)) out = RrEntity(c, n, false) + 0x174;
            const auto dir = [&r]() {
                return static_cast<uint32_t>((r.Next() % 8u == 0) ? r.S32() : ((r.Next() & 1u) ? 1 : -1));
            };
            args.a[0] = gA;
            args.a[1] = (k.obj != 0 && (r.Next() % 4u != 0)) ? RrSomeXsih(c, k.obj) : r.U32();
            args.a[2] = dir();
            args.a[3] = dir();
            args.a4 = gB;
            args.more[0] = static_cast<uint32_t>(c.sample ? c.sample->lateral : RrLateralNear(c, (r.Next() & 1u) ? gA : gB));
            args.more[1] = someFraction(r);
            args.more[2] = out;
            args.more[3] = static_cast<uint32_t>((r.Next() % 6u == 0) ? r.S32() : ((r.Next() % 4u != 0) ? 1 : -1));
            (void)m;
        },
        nullptr, /*compareV0=*/true,
        [](NativeEnv& env, const Args& a) -> uint32_t {
            GuestRam g = RrView(env);
            return RqDone(env, g, rr::sim::CoreCrossSection(g, a.a[0], a.a[1], static_cast<int32_t>(a.a[2]),
                                                            static_cast<int32_t>(a.a[3]), a.a4,
                                                            static_cast<int32_t>(a.more[0]), a.more[1], a.more[2],
                                                            static_cast<int32_t>(a.more[3]), env.sp));
        }});

    // ---------------------------------------------------------------- 0x8003EB58
    rows.push_back(Row{
        "node_wedge", "s32 NodeWedge(s32 p[3], Node *n, Arm **a, Arm **b, s32 hint)  [SLUS 0x8003EB58]",
        rrt::kNodeWedge,
        [](CaseContext& c, Args& args) {
            Memory& m = *c.mem;
            Rng& r = *c.rng;
            const RqNet n = RqRead(m);
            const RbCore k = RbFindCore(c, n);
            uint32_t node = k.node;
            int32_t centre[3] = {0, 0, 0};
            if (k.node != 0) {
                const int32_t arms = std::max<int32_t>(1, GetS16(m, k.node + 2));
                for (uint32_t q = 0; q < 3; ++q) {
                    int64_t s = 0;
                    for (int32_t a = 0; a < arms; ++a) s += GetS32(m, k.node + 8u + 24u * static_cast<uint32_t>(a) + 12u + 4u * q);
                    centre[q] = static_cast<int32_t>(s / arms);
                }
            }
            uint32_t point = RrScratch(c, rrb::kPoint);
            if (c.sample != nullptr) {
                // a captured bike's lateral and along, around the junction's centre
                for (uint32_t q = 0; q < 3; ++q) PutS32(m, point + 4u * q, centre[q] + ((q == 1) ? 0 : ((q == 0) ? c.sample->lateral : c.sample->along) >> 4));
            } else {
                const uint64_t f = r.Next() % 8u;
                if (f == 0 && n.nJn > 0) node = n.bit + 104u * RqPick(r, n.nJn);           // any record
                else if (f == 1) node = RbSyntheticNode(c, RrScratch(c, rrb::kNode),
                                                        static_cast<int32_t>(r.U32() % 8u) - 1, centre);
                else if (f == 2 && k.node != 0)                     // an arm count edited in place
                    PutS16(m, k.node + 2, static_cast<int16_t>(static_cast<int32_t>(r.U32() % 6u)));
                for (uint32_t q = 0; q < 3; ++q)
                    PutS32(m, point + 4u * q, centre[q] + static_cast<int32_t>(r.U32() % (121u * 65536u)) - 60 * 65536);
                if (r.Next() % 4u == 0) {                           // an entity's own box centre
                    const uint32_t e = RrEntity(c, n, false);
                    if (k.obj != 0) {
                        const std::vector<uint32_t> cores = RbPieces(m, k.obj, true);
                        if (!cores.empty()) RbPlace(c, e, k.obj, cores[0], k.ipt, RbCoreLateral(c, k.obj));
                    }
                    point = e + 0xB8;
                }
            }
            const uint32_t arms = RrScratch(c, rrb::kArms);
            m.PokeWord(arms, r.U32());
            m.PokeWord(arms + 4, r.U32());
            static const int32_t kHint[8] = {-1, -1, -1, 0, 1, 2, 3, 4};
            args.a[0] = point;
            args.a[1] = node;
            args.a[2] = (r.Next() % 4u == 0) ? 0u : arms;
            args.a[3] = (r.Next() % 4u == 0) ? 0u : arms + 4;
            args.a4 = static_cast<uint32_t>((c.sample != nullptr) ? -1 : (r.Next() % 10u == 0) ? r.S32() : kHint[r.U32() % 8u]);
        },
        nullptr, /*compareV0=*/true,
        [](NativeEnv& env, const Args& a) -> uint32_t {
            GuestRam g = RrView(env);
            return RqDone(env, g, static_cast<uint32_t>(rr::sim::NodeWedge(g, a.a[0], a.a[1], a.a[2], a.a[3],
                                                                            static_cast<int32_t>(a.a4), env.sp)));
        }});

    // An entity on the junction core: roadrb.py's core families, and the edits that reach the
    // classifier's rare exits. Returns the entity (0 when this machine has no core object).
    const auto coreEntity = [](CaseContext& c, const RqNet& n, const RbCore& k) -> uint32_t {
        Memory& m = *c.mem;
        Rng& r = *c.rng;
        const uint32_t e = RrEntity(c, n, c.sample == nullptr, 4);
        if (k.obj == 0) return e;
        const std::vector<uint32_t> cores = RbPieces(m, k.obj, true), stubs = RbPieces(m, k.obj, false);
        const bool onCore = c.sample != nullptr || stubs.empty() || r.Next() % 6u != 0;
        const std::vector<uint32_t>& from = (onCore && !cores.empty()) ? cores : stubs;
        if (!from.empty())
            RbPlace(c, e, k.obj, from[r.U32() % from.size()], k.ipt,
                    c.sample ? c.sample->lateral : RbCoreLateral(c, k.obj));
        if (c.sample != nullptr) return e;
        m.PokeWord(e + 0x184, (m.PeekWord(e + 0x184) & ~0x61u) | ((r.Next() % 3u != 0) ? 1u : 0u) |
                                  ((r.Next() % 3u == 0) ? 0x20u : 0u) | ((r.Next() % 5u == 0) ? 0x40u : 0u));
        return e;
    };
    // The classifier's rare exits, by data edits in this case's copy only: the XSAI block gone, a
    // sub-object without XSIH, an XSAI record without two groups, a two-arm node.
    const auto stirCore = [](CaseContext& c, const RbCore& k) {
        Memory& m = *c.mem;
        Rng& r = *c.rng;
        if (k.obj == 0) return;
        if (r.Next() % 24u == 0) m.PokeWord(k.obj + 0x40, 0);
        if (r.Next() % 12u == 0) {
            const int32_t nsub = GetS16(m, k.obj + 0x14);
            PutS16(m, m.PeekWord(k.obj + 0x30) + 28u * RqPick(r, nsub) + 18u, -1);
        }
        if (r.Next() % 12u == 0) {                                  // the XSAI record a turn names
            const int32_t nsub = GetS16(m, k.obj + 0x14);
            const int32_t x = GetS16(m, m.PeekWord(k.obj + 0x30) + 28u * RqPick(r, nsub) + 18u);
            if (x >= 0) PutS16(m, m.PeekWord(k.obj + 0x40) + 20u * static_cast<uint32_t>(x) + ((r.Next() & 1u) ? 4u : 6u), -1);
        }
        if (r.Next() % 12u == 0) PutS16(m, k.node + 2, 2);
        if (r.Next() % 8u == 0) {                                   // a turn length <= 0: SDiv's arms
            // Every length on disc is positive. INT_MIN + a little makes `len - along` wrap, so the
            // clamped along is positive while len is not - the one arm (0x8003EA2C) no other length
            // reaches.
            const int32_t nsub = GetS16(m, k.obj + 0x14);
            const uint32_t sub = m.PeekWord(k.obj + 0x30) + 28u * RqPick(r, nsub);
            PutS32(m, sub + 12, (r.Next() & 1u) ? static_cast<int32_t>(0x80000000u + r.U32() % 0x10000u)
                                                : -static_cast<int32_t>(r.U32() % 0x400000u));
        }
    };

    // ---------------------------------------------------------------- 0x8003E754
    rows.push_back(Row{
        "road_class_core", "u8 RoadClassCore(Entity *e, Node *n, Out *o, Cursor *x, s32 zone)  [SLUS 0x8003E754]",
        rrt::kClassCore,
        [coreEntity, stirCore](CaseContext& c, Args& args) {
            Memory& m = *c.mem;
            Rng& r = *c.rng;
            const RqNet n = RqRead(m);
            const RbCore k = RbFindCore(c, n);
            const uint32_t e = coreEntity(c, n, k);
            uint32_t node = k.node;
            if (c.sample == nullptr) {
                stirCore(c, k);
                RrStirXsih(c, m.PeekWord(e + 0x148));
                const uint64_t f = r.Next() % 16u;
                if (f == 0) node = 0;
                else if (f == 1 && n.nJn > 0) node = n.bit + 104u * RqPick(r, n.nJn);
                RrStackJunk(c, n, 640);
            }
            const uint32_t x = RrScratch(c, rrt::kCur);
            RrNoise(c, x, 32);
            args.a[0] = e;
            args.a[1] = node;
            args.a[2] = e + 0x174;
            args.a[3] = (r.Next() % 5u == 0) ? 0u : x;
            static const int32_t kZone[7] = {-1, -1, -1, 0, 1, 2, 3};
            args.a4 = static_cast<uint32_t>((c.sample != nullptr) ? -1 : kZone[r.U32() % 7u]);
        },
        nullptr, /*compareV0=*/true,
        [](NativeEnv& env, const Args& a) -> uint32_t {
            GuestRam g = RrView(env);
            rr::sim::RoadRuntimeNative six;
            return RqDone(env, g, rr::sim::RoadClassCore(g, a.a[0], a.a[1], a.a[2], a.a[3],
                                                         static_cast<int32_t>(a.a4), env.sp, six));
        }});

    // ---------------------------------------------------------------- 0x8003F204
    rows.push_back(Row{
        "roadside_run_node", "void RoadsideRunNode(Entity *e, s32 zone)  [SLUS 0x8003F204]", rrt::kRunNode,
        [coreEntity](CaseContext& c, Args& args) {
            Memory& m = *c.mem;
            Rng& r = *c.rng;
            const RqNet n = RqRead(m);
            const RbCore k = RbFindCore(c, n);
            uint32_t e = coreEntity(c, n, k);
            if (c.sample == nullptr) {
                if (k.obj != 0) RbStirZoneRecords(c, k.obj, k.node);
                if (r.Next() % 8u == 0) e = RrEntity(c, n, false);  // on road 9: not a junction object
                if (r.Next() % 16u == 0) e = 0;
                if (k.node != 0 && r.Next() % 12u == 0) PutS16(m, k.node + 2, static_cast<int16_t>(r.U32() % 2u));
            }
            if (e != 0) {
                for (uint32_t i = 0; i < 12; i += 4) m.PokeWord(e + 0x1EC + i, r.U32());
            }
            args.a[0] = e;
            static const int32_t kZone[7] = {-1, -1, 0, 1, 2, 3, 4};
            args.a[1] = static_cast<uint32_t>((c.sample != nullptr) ? -1 : kZone[r.U32() % 7u]);
        },
        nullptr, /*compareV0=*/true,
        [](NativeEnv& env, const Args& a) -> uint32_t {
            GuestRam g = RrView(env);
            rr::sim::RoadRuntimeNative six;
            // e == 0 leaves the caller's v0: here the register file the bench started the call with
            return RqDone(env, g, rr::sim::RoadsideRunNode(g, a.a[0], static_cast<int32_t>(a.a[1]), env.sp, six,
                                                           env.clone->cpu.regs[2]));
        }});

    // ---------------------------------------------------------------- 0x8003BFE8
    // roadrb.py's stub captures: the entity off the road on one of the core object's stubs, 4..40
    // units across, or with its box centre on ANOTHER stub's centre line (tp_x*: the re-bind); the
    // along-distance is the stub's own, so the junction is within the 50-unit gate.
    rows.push_back(Row{
        "road_shortcut", "void RoadShortcut(Entity *e)  [SLUS 0x8003BFE8]", rrt::kShortcut,
        [](CaseContext& c, Args& args) {
            Memory& m = *c.mem;
            Rng& r = *c.rng;
            const RqNet n = RqRead(m);
            const RbGraph g = RbReadGraph(m, c.gp);
            const RbCore k = RbFindCore(c, n);
            const uint32_t e = RrEntity(c, n, c.sample == nullptr, 4);
            const bool stub = k.obj != 0 && (c.sample != nullptr ? (r.Next() & 1u) != 0 : r.Next() % 8u != 0);
            if (stub) {
                const std::vector<uint32_t> stubs = RbPieces(m, k.obj, false);
                if (!stubs.empty()) {
                    const uint32_t piece = stubs[r.U32() % stubs.size()];
                    uint32_t other = 0;
                    if (c.sample == nullptr && r.Next() % 5u < 2u) {  // on another stub's centre line
                        const uint32_t op = stubs[r.U32() % stubs.size()];
                        const uint32_t osub = m.PeekWord(k.obj + 0x30) + 28u * static_cast<uint32_t>(GetS16(m, op + 20));
                        other = m.PeekWord(k.obj + 0x34) +
                                52u * static_cast<uint32_t>(GetS16(m, osub + 8) + static_cast<int32_t>(RqPick(r, GetS16(m, osub + 10))));
                    }
                    const int32_t lat = c.sample ? c.sample->lateral
                                                 : ((r.Next() & 1u) ? 1 : -1) * static_cast<int32_t>((4u + r.U32() % 37u) * 65536u + r.U32() % 65536u);
                    // the slice next to the core half the time: with the road-position invariant the 50-unit gate
                    // opens only there (roads that START at the junction), and the re-bind needs it
                    RbPlace(c, e, k.obj, piece, k.ipt, lat, other, (r.Next() & 1u) != 0);
                    m.PokeWord(e + 0x184, m.PeekWord(e + 0x184) | ((c.sample != nullptr || r.Next() % 10u != 0) ? 1u : 0u));
                    if (m.PeekWord(e + 0x174) == 0 || r.Next() % 4u == 0) {
                        const uint32_t grp = RrSomeGroup(c, k.obj);
                        if (grp != 0) m.PokeWord(e + 0x174, grp);
                    }
                    if (c.sample == nullptr && r.Next() % 6u == 0) {   // the along past the 50-unit gate
                        const int32_t road = static_cast<int32_t>(m.PeekWord(e + 0x168) & 0xFFFFu);
                        if (road < g.nRoads) PutS32(m, e + 0x170, RbAlongOnRoad(c, g, road));
                    }
                }
            }
            if (c.sample == nullptr) {
                for (uint32_t q = 0; q < 3; ++q) PutS16(m, e + 0x1C2 + 2u * q, r.Unit());
                if (r.Next() % 12u == 0) m.PokeWord(e + 0x168, m.PeekWord(e + 0x168) | 0x10000u);
                if (r.Next() % 16u == 0) m.PokeWord(e + 0x174, 0);
                if (r.Next() % 3u == 0) RbStirBinding(c, g, e + 0xAC);
                if (r.Next() % 3u == 0) m.PokeWord(kLiveBikes, r.U32() % 10u);   // the pool-1 slot adjustment
                if (r.Next() % 5u == 0) {                           // the junction's node has no record
                    const uint32_t recs = m.PeekWord(rrt::kRecords);
                    const int32_t count = GetS16(m, rrt::kCount);
                    for (int32_t q = 0; q < count && q < 8; ++q) PutS32(m, recs + 120u * static_cast<uint32_t>(q), -2 - q);
                }
                RrStackJunk(c, n, 640);
            }
            RrNearOwnStub(c, e);                                    // the road-position invariant, LAST
            args.a[0] = e;
        },
        nullptr, /*compareV0=*/true,
        [](NativeEnv& env, const Args& a) -> uint32_t {
            GuestRam g = RrView(env);
            rr::sim::RoadRuntimeNative six;
            return RqDone(env, g, rr::sim::RoadShortcut(g, a.a[0], env.sp, six));
        }});

    // Budgets: the rows that reach the slice search - road_look_ahead's 200 million, as in
    // AddRoadRuntimeRows. A case that exhausts it still FAILS.
    for (size_t i = rowIndex; i < rows.size(); ++i)
        if (rows[i].entry == rrt::kClassCore || rows[i].entry == rrt::kShortcut) rows[i].maxSteps = 200'000'000;
}
