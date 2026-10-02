// Bench rows for src/game/sim/contact.{h,cpp}. Included by rows_coll.inc.
//
// The family: THIS machine (rr-race) with two of its 18 bikes placed in or near contact - the second
// bike's whole box (centre, previous centre, road point, corners) moved beside the first by MoveNear -
// with the fields the pair reads drawn: speeds +0x1E0, the heading angle +0x124 (parallel, crossing,
// opposed), the dump sample's orientation rows over the first bike for a dump-derived case, the camera
// cache +0x140 (in band / out of band / not cached), flagsC, a passenger (bike 17 riding with the first
// bike, +0x358 both ways, unlisted - the shape SetupPass uses), the dt of the pass 0x800CCE38, the chain
// bits of the previous and current pass and the contact list; now and then the partner is the traffic
// car 0x800CF660 (pool 3), which Response also serves (BikeVsTraffic calls it).
// NAMED BOUNDS: the passenger is only ever bike 17 on the first bike (no capture has a passenger; it is
// SetupPass's shape); the headings stay the machine's own unit rows (Normalize and the face leaves
// consume them); the traffic car appears only as Response's partner B.

namespace cpair {

using co::Chance;
using co::MoveNear;
using co::Pick;
using co::Pool0;

constexpr uint32_t kCar = 0x800CF660u;

int32_t Off(Rng& rng, uint32_t range) { return static_cast<int32_t>(rng.U32() % (2u * range + 1u)) - static_cast<int32_t>(range); }

struct Pair {
    uint32_t me = 0, other = 0;
};

uint32_t Refuse0(NativeEnv& env, EnRun& run, const char* what) { return co::Refuse(env, run, false, what); }

// Two live bikes near each other, with the pair's fields drawn.
Pair PlacePair(CaseContext& c, bool tight = false) {
    Rng& rng = *c.rng;
    Memory& m = *c.mem;
    Pair p;
    const uint32_t s0 = Pick(rng, {0u, 0u, 6u, rng.U32() % 17u});
    uint32_t s1 = 1u + rng.U32() % 16u;
    if (s1 == s0) s1 = (s1 + 1u) % 17u;
    p.me = Pool0(m, s0);
    p.other = Pool0(m, s1);
    if (c.sample != nullptr && c.sample->entity.size() == 1096) {
        m.WriteBlock(p.me + 0x1B0, c.sample->entity.data() + 0x1B0, 0x216 - 0x1B0);
        PutS32(m, p.me + 0x1E0, c.sample->speed);
    }
    // beside, behind, ahead, above: lateral and along offsets of a few box widths, sometimes exact
    const uint32_t spread = tight && !Chance(rng, 5) ? Pick(rng, {0x2000u, 0x4000u, 0x6000u, 0x8000u, 0xC000u})
                                                     : Pick(rng, {0x4000u, 0x8000u, 0xC000u, 0x10000u, 0x18000u, 0x20000u, 0x40000u, 0x140000u});
    MoveNear(m, p.other, p.me, Off(rng, spread), !tight && Chance(rng, 4) ? Off(rng, 0x30000u) : 0, Off(rng, spread));
    for (uint32_t e : {p.me, p.other}) {
        if (Chance(rng, 2)) PutS32(m, e + 0x1E0, static_cast<int32_t>(rng.U32() % 0x600000u));
        if (Chance(rng, 3)) PutS32(m, e + 0x1E0, -static_cast<int32_t>(rng.U32() % 0x100000u));
        uint32_t fc = m.PeekWord(e + 568);
        if (Chance(rng, 5)) fc = (fc & ~0x600u) | Pick(rng, {0x200u, 0x400u});
        if (Chance(rng, 4)) fc |= 0x01000000u; // StaleHeading re-derives
        if (Chance(rng, 5)) fc |= 0x02000000u;
        m.PokeWord(e + 568, fc);
        if (!Chance(rng, 3)) WriteU16Bench(m, e + 320, static_cast<uint16_t>(Pick(rng, {0u, 8u, 0xCu, 0xCu, 0xCu, 0x1Cu, 0x4u})));
    }
    // the heading angles: parallel, crossing or opposed
    const int32_t h0 = GetS32(m, p.me + 292);
    if (Chance(rng, 2)) PutS32(m, p.other + 292, h0 + Pick(rng, {0u, 0u, 100u, 0xFFFFFF9Cu, 383u, 384u, 2048u, 1024u,
                                                              rng.U32() % 4096u}));
    // a passenger on the first bike: bike 17, unlisted, whose +0x358 names its owner
    if (Chance(rng, 6)) {
        const uint32_t b17 = Pool0(m, 17);
        m.PokeWord(p.me + 856, b17);
        m.PokeWord(b17 + 856, p.me);
        m.PokeWord(b17 + 1088, 0);
        MoveNear(m, b17, p.me, Off(rng, 0x8000u), 0, Off(rng, 0x10000u));
    }
    m.PokeWord(rr::sim::kCollDt, Pick(rng, {0x884u, 0x884u, 0x442u, 0x1110u, rng.U32() % 0x3000u}));
    return p;
}

// The contact list and the chain bits as a pass leaves them part-way.
void PlaceList(CaseContext& c, const Pair& p) {
    Rng& rng = *c.rng;
    Memory& m = *c.mem;
    const uint32_t cnt = static_cast<uint32_t>(rng.Next() % 9u);
    m.PokeWord(rr::sim::kContactCount, cnt);
    for (uint32_t i = 0; i < 8; ++i) {
        const uint32_t r = rr::sim::kContactList + 36u * i;
        uint32_t ha = rng.U32() % 18u, hb = rng.U32() % 18u;
        if (Chance(rng, 6)) {
            ha = static_cast<uint16_t>(GetS16(m, p.me + 172));
            hb = static_cast<uint16_t>(GetS16(m, p.other + 172));
            if (Chance(rng, 2)) std::swap(ha, hb);
        }
        WriteU16Bench(m, r, static_cast<uint16_t>(ha));
        WriteU16Bench(m, r + 2, static_cast<uint16_t>(hb));
    }
    m.PokeWord(rr::sim::kChainPrev, rng.U32() & (Chance(rng, 2) ? 0x55555555u : 0xFFFFFFFFu));
    m.PokeWord(rr::sim::kChainPrev + 4, rng.U32() & 0x0Fu);
    m.PokeWord(rr::sim::kChainCur, rng.U32() & 0x55555555u);
    m.PokeWord(rr::sim::kChainCur + 4, rng.U32() & 0x0Fu);
    m.PokeWord(rr::sim::kChainMask, Chance(rng, 2) ? 0u : rng.U32());
    if (Chance(rng, 6)) {
        co::SetNumPlayers(m, 2);
        m.PokeWord(0x8005B21Cu, Chance(rng, 2) ? p.other : Pool0(m, 6));
    }
    m.PokeWord(c.gp + kRandSeedGpOffset, rng.U32());
}

// ---------------------------------------------------------------- BikePairClassify 0x800ABE78
void SetupClassify(CaseContext& c, Args& a) {
    Rng& rng = *c.rng;
    Memory& m = *c.mem;
    const Pair p = PlacePair(c);
    for (uint32_t k = 0; k < 4; ++k) m.PokeWord(c.scratch + 4 * k, rng.U32());
    const bool swap = Chance(rng, 3);
    a.a[0] = swap ? p.other : p.me;
    a.a[1] = swap ? p.me : p.other;
    a.a[2] = c.scratch;
    a.a[3] = c.scratch + 4;
    a.a4 = c.scratch + 8;
    a.more[0] = c.scratch + 12;
}
uint32_t NativeClassify(NativeEnv& env, const Args& a) {
    EnRun run(env);
    const int32_t v = rr::sim::BikePairClassify(run.g, a.a[0], a.a[1], a.a[2], a.a[3], env.sp);
    return run.Done(env, static_cast<uint32_t>(v));
}

// ---------------------------------------------------------------- ViewExtent 0x800AA34C
void SetupExtent(CaseContext& c, Args& a) {
    Rng& rng = *c.rng;
    const Pair p = PlacePair(c);
    const uint32_t A = Chance(rng, 2) ? p.me : p.other, B = Chance(rng, 2) ? p.me : p.other;
    a.a[0] = B + 196;
    a.a[1] = c.mem->PeekWord(A + 340);
    a.a[2] = c.scratch;
}
uint32_t NativeExtent(Memory& m, uint32_t, uint32_t gp, const Args& a) {
    rr::sim::GuestRam g(m.ram().data(), gp);
    rr::sim::ViewExtent(g, a.a[0], a.a[1], a.a[2]);
    return g.Faulted() ? 0xDEADDEADu : 0u;
}

// ---------------------------------------------------------------- ResponseFar 0x800AA140
void SetupFar(CaseContext& c, Args& a) {
    Rng& rng = *c.rng;
    Memory& m = *c.mem;
    const Pair p = PlacePair(c);
    for (uint32_t k = 0; k < 6; ++k) m.PokeWord(c.scratch + 4 * k, rng.U32());
    const bool swap = Chance(rng, 2);
    a.a[0] = swap ? p.me : p.other;
    a.a[1] = swap ? p.other : p.me;
    a.a[2] = c.scratch;
    a.a[3] = c.scratch + 4;
    a.a4 = c.scratch + 8;
}
uint32_t NativeFar(NativeEnv& env, const Args& a) {
    EnRun run(env);
    const cr::Tables tab(env.clone->mem, env.gp);
    uint32_t v = 0;
    const bool ok = rr::sim::ResponseFar(run.g, a.a[0], a.a[1], a.a[2], a.a[3], env.sp, tab.t, v);
    return ok ? run.Done(env, v) : Refuse0(env, run, "ResponseFar");
}

// ---------------------------------------------------------------- PairPushDir 0x800B5B48
void SetupPush(CaseContext& c, Args& a) {
    Rng& rng = *c.rng;
    Memory& m = *c.mem;
    const Pair p = PlacePair(c);
    for (uint32_t e : {p.me, p.other})
        if (Chance(rng, 4)) PutS32(m, e + 316, static_cast<int32_t>(rng.U32() % 0x200000u) + 0x100);
    if (Chance(rng, 6)) PutS32(m, p.me + 480, 0x7FFFFFFF - static_cast<int32_t>(rng.U32() % 0x1000u));
    m.PokeWord(c.scratch, rng.U32());
    m.PokeWord(c.scratch + 4, rng.U32());
    const bool swap = Chance(rng, 2);
    a.a[0] = swap ? p.me : p.other;
    a.a[1] = swap ? p.other : p.me;
    a.a[2] = c.scratch;
}
uint32_t NativePush(Memory& m, uint32_t, uint32_t gp, const Args& a) {
    rr::sim::GuestRam g(m.ram().data(), gp);
    const cr::Tables tab(m, gp);
    rr::sim::PairPushDir(g, a.a[0], a.a[1], a.a[2], tab.t);
    return g.Faulted() ? 0xDEADDEADu : 0u;
}

// ---------------------------------------------------------------- HeavyResponse 0x800AA474
// The class-31 pair as BikeVsBike hands it over: flags the side bits (a passenger on the first or the
// second), angle the heading difference, along / across the classifier's offsets (drawn around the
// pair's real ones).
void SetupHeavy(CaseContext& c, Args& a) {
    Rng& rng = *c.rng;
    Memory& m = *c.mem;
    const Pair p = PlacePair(c);
    for (uint32_t k = 0; k < 8; ++k) m.PokeWord(c.scratch + 4 * k, rng.U32());
    uint32_t flags = Pick(rng, {0u, 0u, 0u, 1u, 2u});
    if (flags == 1u && m.PeekWord(p.me + 856) == 0) flags = 0;
    const bool swap = flags == 2u || Chance(rng, 3);
    if (flags == 2u && m.PeekWord(p.me + 856) == 0) flags = 0;
    a.a[0] = swap && flags != 2u ? p.other : p.me;
    a.a[1] = swap && flags != 2u ? p.me : p.other;
    if (flags == 2u) { a.a[0] = p.other; a.a[1] = p.me; }
    a.a[2] = flags;
    a.a[3] = Pick(rng, {0u, 10u, 383u, 2048u, 2049u, 4000u, rng.U32() % 4096u});
    a.a4 = static_cast<uint32_t>(Pick(rng, {1u, 0u, 0xFFFFFFFFu, 0x10000u, 0xFFFF0000u, rng.U32() >> 12, 0u - (rng.U32() >> 12)}));
    a.more[0] = static_cast<uint32_t>(Pick(rng, {1u, 0u, 0xFFFFFFFFu, rng.U32() >> 13, 0u - (rng.U32() >> 13)}));
    a.more[1] = c.scratch;
    a.more[2] = c.scratch + 4;
    a.more[3] = c.scratch + 8;
}
uint32_t NativeHeavy(NativeEnv& env, const Args& a) {
    EnRun run(env);
    const cr::Tables tab(env.clone->mem, env.gp);
    uint32_t v = 0;
    const bool ok = rr::sim::HeavyResponse(run.g, a.a[0], a.a[1], a.a[2], a.a[3], env.sp, tab.t, v);
    return ok ? run.Done(env, v) : Refuse0(env, run, "HeavyResponse");
}

// ---------------------------------------------------------------- Response 0x800AAD30
void SetupResponse(CaseContext& c, Args& a) {
    Rng& rng = *c.rng;
    Memory& m = *c.mem;
    Pair p = PlacePair(c, true);
    for (uint32_t k = 0; k < 8; ++k) m.PokeWord(c.scratch + 4 * k, rng.U32());
    if (Chance(rng, 5)) { // the traffic car as the partner (BikeVsTraffic's call)
        MoveNear(m, kCar, p.me, Off(rng, 0x80000u), 0, Off(rng, 0x100000u));
        p.other = kCar;
    }
    const bool swap = p.other != kCar && Chance(rng, 2);
    a.a[0] = swap ? p.other : p.me;
    a.a[1] = swap ? p.me : p.other;
    a.a[2] = c.scratch;
    a.a[3] = c.scratch + 4;
    a.a4 = c.scratch + 8;
    a.more[0] = Chance(rng, 3) ? 0u : c.scratch + 20;
}
uint32_t NativeResponse(NativeEnv& env, const Args& a) {
    EnRun run(env);
    co::CloneCallees cc(env, run);
    const cr::Tables tab(env.clone->mem, env.gp);
    uint32_t v = 0;
    const bool ok = rr::sim::Response(run.g, a.a[0], a.a[1], a.a[2], a.a[3], env.sp, tab.t, cc, v);
    return ok ? run.Done(env, v) : Refuse0(env, run, "Response");
}

// ---------------------------------------------------------------- BikeVsBike 0x800AB7A0
void SetupBvb(CaseContext& c, Args& a) {
    Rng& rng = *c.rng;
    const Pair p = PlacePair(c, true);
    PlaceList(c, p);
    const bool swap = Chance(rng, 3);
    a.a[0] = swap ? p.other : p.me;
    a.a[1] = swap ? p.me : p.other;
    a.a[2] = Chance(rng, 4) ? 1u : 0u;
}
uint32_t NativeBvb(NativeEnv& env, const Args& a) {
    EnRun run(env);
    co::CloneCallees cc(env, run);
    const cr::Tables tab(env.clone->mem, env.gp);
    uint32_t v = 0;
    const bool ok = rr::sim::BikeVsBike(run.g, a.a[0], a.a[1], static_cast<int32_t>(a.a[2]), env.sp, tab.t, cc, v);
    return ok ? run.Done(env, v) : Refuse0(env, run, "BikeVsBike");
}

} // namespace cpair

void AddCollPairRows(std::vector<Row>& rows) {
    using namespace rr::sim::pair;
    rows.push_back(Row{"pair_classify", "s32 0x800ABE78(a, b, *side, *angle, [*along, *across])  [RASHCDG]", 0x800ABE78,
                       cpair::SetupClassify, nullptr});
    rows.back().nativeEx = cpair::NativeClassify;
    rows.push_back(Row{"pair_view_extent", "void 0x800AA34C(corners, rec, out[4])  [RASHCDG]", 0x800AA34C,
                       cpair::SetupExtent, cpair::NativeExtent, /*compareV0=*/false});
    rows.push_back(Row{"pair_response_far", "E *0x800AA140(A, B, *outA, *outB, [vout])  [RASHCDG]", 0x800AA140,
                       cpair::SetupFar, nullptr});
    rows.back().nativeEx = cpair::NativeFar;
    rows.push_back(Row{"pair_push_dir", "void 0x800B5B48(X, Y, s16 out[3])  [RASHCDG]", 0x800B5B48, cpair::SetupPush,
                       cpair::NativePush, /*compareV0=*/false});
    rows.push_back(Row{"pair_heavy_response", "E *0x800AA474(A, B, flags, angle, [along, across, *oA, *oB, vout])  [RASHCDG]",
                       0x800AA474, cpair::SetupHeavy, nullptr});
    rows.back().nativeEx = cpair::NativeHeavy;
    {
        rows.push_back(Row{"pair_response", "E *0x800AAD30(A, B, *outA, *outB, [vout, *remaining])  [RASHCDG]", 0x800AAD30,
                           cpair::SetupResponse, nullptr});
        Row& r = rows.back();
        r.nativeEx = cpair::NativeResponse;
        r.oracleCallees = {OracleCallee{kFacePoint, 4, 4}, OracleCallee{kFaceDeepest, 4, 4},
                           OracleCallee{kFaceNormal, 4, 1}};
    }
    {
        rows.push_back(Row{"bike_vs_bike", "s32 0x800AB7A0(Bike *me, Bike *other, s32 mode)  [RASHCDG, the bike pair]",
                           0x800AB7A0, cpair::SetupBvb, nullptr});
        Row& r = rows.back();
        r.nativeEx = cpair::NativeBvb;
        r.maxSteps = 20000000;
        r.oracleCallees = {OracleCallee{kFacePoint, 4, 4}, OracleCallee{kFaceDeepest, 4, 4},
                           OracleCallee{kFaceNormal, 4, 1}, OracleCallee{kBikeBikeReact, 4}};
        // ChainReaction's BikeVsBike (mode 1) runs natively inside the port (contact.cpp), so the
        // recursion is part of what the row compares; the guest's nested calls are not observed.
    }
}
