// Bench rows for src/game/sim/impact_solve.{h,cpp}. Included by rows_coll.inc.
//
// The ports run on the clone's RAM (GUEST-ADDRESS, collision.h's model). Inside them the ported callees
// run natively (TouchDown, ApplyImpulse, PadRumble, and each other: BoxReact -> ImpactGate ->
// ImpactSolve -> FaceCrossing / LandOnTop / PropHitFace, BikeWallHit -> ImpactSolve); every unported one
// from the ORIGINAL code at exactly the stack pointer the original calls it at (co::CloneCallees ->
// bs::ExactOracle), and the call sequences are compared.
//
// Cases start from THIS machine (rr-race): the bike is one of its 18 (their rider, stat and road
// pointers are live, and the unported hand-over HitOutcome / ImpactTurn / ImpactSeverity follows them),
// the partner one of its entities (a bike, the car, the off-bike rider 6, a pedestrian), a synthetic
// pool-6 volume (co::PlantVolumes) or one of the player's road wall segments (S = *(P+0x148), segs =
// *(S+0x60), 40 bytes each, as SetupDeferred builds its pool-8 records).
//
// NAMED BOUNDS (shared by every row here):
//  * a prop partner (pool 4, empty on this machine and in the product) only in the impact_solve row, as
//    a planted synthetic record (PlantProp, with its own bound: no toppling prop); the gate / pole / box /
//    wall rows never have one.
//  * the rider's stance is left as the machine has it (rows_coll_react.inc: a stance of another
//    animation bank traps the ORIGINAL's stance layer, which ImpactSeverity reaches).

namespace csolve {

using co::Chance;
using co::Dir;
using co::Pick;
using co::Pool0;

uint32_t PoolEntity(const Memory& m, uint32_t pool, uint32_t slot) {
    const uint32_t T = rr::sim::kPoolTableAddr + 16u * pool;
    return m.PeekWord(T) + m.PeekWord(T + 4) * slot;
}

uint32_t Speed(Rng& rng) {
    return Pick(rng, {0u, 0x8000u, 0x1017Du, 0x1017Eu, 0x4786Bu, 0x80000u, 0xA0000u, 0xA0001u, 0xB2D0Du, 0xB2D0Eu,
                      0x140000u, 0x3C0000u, rng.U32() % 0x600000u});
}

// The bike's state words the solver and its hand-over read, drawn around their thresholds.
void PerturbBike(CaseContext& c, uint32_t e, uint32_t shape) {
    Rng& rng = *c.rng;
    Memory& m = *c.mem;
    uint32_t fc = m.PeekWord(e + 568) & ~0x02000FFFu;
    fc |= Pick(rng, {0u, 0u, 0u, 0u, 0x400u, 0x400u, 0x200u, 0x600u, 1u, 2u, 4u, 8u, 0x10u, 0x20u, 0x100u, 0x401u});
    if (Chance(rng, 3)) fc |= 0x02000000u;
    m.PokeWord(e + 568, fc);
    uint32_t fb = m.PeekWord(e + 564) & ~0x0003C000u;
    if (Chance(rng, 3)) fb |= 0x8000u;
    if (Chance(rng, 5)) fb |= 0x4000u;
    if (Chance(rng, 3)) fb |= 0x20000u;
    if (Chance(rng, 3)) fb |= 0x10000u;
    m.PokeWord(e + 564, fb);
    uint32_t fa = m.PeekWord(e + 560) & ~0x28000300u;
    if (Chance(rng, 3)) fa |= Pick(rng, {0x100u, 0x200u});
    if (Chance(rng, 6)) fa |= 0x08000000u;
    if (Chance(rng, 5)) fa |= 0x20000000u;
    m.PokeWord(e + 560, fa);
    m.PokeWord(e + 576, Speed(rng));
    m.PokeWord(e + 480, Chance(rng, 3) ? m.PeekWord(e + 576) : Speed(rng));
    m.PokeWord(e + 616, Pick(rng, {0u, 0u, 0x8000u, 0x10000u, 0xFFFF8000u, rng.U32() % 0x30000u}));
    m.PokeWord(e + 828, Pick(rng, {m.PeekWord(e + 828), 0u, shape, shape}));
    m.PokeWord(e + 832, Pick(rng, {m.PeekWord(e + 832), 0u, shape, shape, e + 172}));
    m.PokeWord(e + 388, Chance(rng, 2) ? (m.PeekWord(e + 388) | 0x10u) : (m.PeekWord(e + 388) & ~0x10u));
    if (c.sample != nullptr && c.sample->entity.size() == 1096 && Chance(rng, 2)) {
        m.WriteBlock(e + 0x1B0, c.sample->entity.data() + 0x1B0, 0x1C8 - 0x1B0);   // rows and heading
        m.WriteBlock(e + 0x204, c.sample->entity.data() + 0x204, 0x216 - 0x204);
    } else if (Chance(rng, 3)) {
        int16_t h[3];
        Dir(rng, h);
        PutVec3S16(m, e + 450, h);
    }
    co::SetNumPlayers(m, Pick(rng, {1u, 1u, 2u}));
    m.PokeWord(co::kSkipResults, Chance(rng, 5) ? 1u : 0u);
    m.PokeWord(c.gp + kRandSeedGpOffset, rng.U32());
}

// A contact normal at `at`: the partner's (when given), its reverse, the bike's up or forward row, anything.
void Normal(CaseContext& c, uint32_t at, uint32_t e, uint32_t given) {
    Rng& rng = *c.rng;
    Memory& m = *c.mem;
    int16_t n[3];
    const uint32_t mode = static_cast<uint32_t>(rng.Next() % 6u);
    if (given != 0 && mode < 2) {
        GetVec3S16(m, given, n);
        if (mode == 1) for (int16_t& x : n) x = static_cast<int16_t>(-x);
    } else if (mode == 2) {
        GetVec3S16(m, e + 522, n);
        if (Chance(rng, 2)) for (int16_t& x : n) x = static_cast<int16_t>(-x);
    } else if (mode == 3) {
        GetVec3S16(m, e + 528, n);
        if (Chance(rng, 2)) for (int16_t& x : n) x = static_cast<int16_t>(-x);
    } else {
        Dir(rng, n);
    }
    PutVec3S16(m, at, n);
}

// A push vector of the game's size at `at`, or none.
uint32_t Push(CaseContext& c, uint32_t at) {
    Rng& rng = *c.rng;
    for (uint32_t k = 0; k < 3; ++k) c.mem->PokeWord(at + 4 * k, static_cast<uint32_t>(rng.S32() >> (11 + rng.Next() % 8u)));
    return at;
}

// One of the player's road wall segments (40 bytes), its flag word redrawn half the time.
uint32_t WallSegment(CaseContext& c) {
    Rng& rng = *c.rng;
    Memory& m = *c.mem;
    const uint32_t P = Pool0(m, 0);
    const uint32_t S = m.PeekWord(P + 0x148), segs = m.PeekWord(S + 0x60);
    const uint32_t seg = segs + 40u * (rng.U32() % 200u);
    if (Chance(rng, 2)) {
        uint16_t f = static_cast<uint16_t>(Pick(rng, {1u, 2u, 3u, 3u, 4u, 5u, 6u, 7u, 8u, 0u}));
        if (Chance(rng, 2)) f = static_cast<uint16_t>(f | 0x100u);
        if (Chance(rng, 4)) f = static_cast<uint16_t>(f | 0x200u);
        if (Chance(rng, 3)) f = static_cast<uint16_t>(f | 0x400u);
        WriteU16Bench(m, seg + 2, f);
    }
    if (Chance(rng, 3)) m.PokeWord(seg + 4, Pick(rng, {0xFFFF0600u, 0x8000u, 0x10000u, 0x30000u, rng.U32() % 0x80000u}));
    return seg;
}

// The bike's ground point +0x1F8 (TOP / BOT of the solver's landing test) put on the segment's line, 0..3.0
// either side along `n`.
void PlaceAtWall(CaseContext& c, uint32_t e, uint32_t seg, uint32_t n) {
    Rng& rng = *c.rng;
    Memory& m = *c.mem;
    int32_t p0[3], p1[3];
    int16_t nn[3];
    GetVec3S32(m, seg + 8, p0);
    GetVec3S32(m, seg + 20, p1);
    GetVec3S16(m, n, nn);
    const int64_t tq = static_cast<int64_t>(rng.U32() % 0x10000u);
    const int64_t side = static_cast<int64_t>(rng.U32() % 0x60000u) - 0x30000;
    for (uint32_t k = 0; k < 3; ++k)
        PutS32(m, e + 504 + 4 * k, static_cast<int32_t>(p0[k] + (((static_cast<int64_t>(p1[k]) - p0[k]) * tq) >> 16) + ((side * nn[k]) >> 12)));
}

// A synthetic prop in pool 4 (slot 0..5, 596 bytes; the pool is empty on this machine): a copy of a live
// bike's first 596 bytes, its class record (+0) a scratch block whose +14 carries the class 1..7 in bits
// 7..11, its handle +0xAC, flags +0x250 (down 4, stuck 0x800, knocked 0x200, 0x400), +0xB4 (30 or the
// surface index SurfaceSound reads through shape+8), its speed +0x1E0 (0 skips PropHitFace), heading and
// rows drawn, placed within 2.0 of the bike.
// NAMED BOUND: +0x250 bit 1 (a prop already toppling) is never set: PropKick then hands the solver's
// sp+64 to PropTopple 0x800B3344, which reads a depth CornerMin leaves unwritten - stack below the
// solver's frame that the ORIGINAL's own callees (PropHitFace's Scale / Dot32 frames) happen to leave
// there and the inline port does not (SetupWall's bound, same cause).
uint32_t PlantProp(CaseContext& c, uint32_t e) {
    Rng& rng = *c.rng;
    Memory& m = *c.mem;
    const uint32_t slot = rng.U32() % 6u;
    const uint32_t P = PoolEntity(m, 4, slot);
    std::vector<uint8_t> img(596);
    m.ReadBlock(Pool0(m, 1u + rng.U32() % 17u), img.data(), img.size());
    const uint32_t cls = c.scratch + 6000;
    for (uint32_t k = 0; k < 32; k += 4) m.PokeWord(cls + k, rng.U32() & 0x0F0F0F0Fu);
    WriteU16Bench(m, cls + 14, static_cast<uint16_t>(Pick(rng, {3u, 4u, 5u, 3u, 1u, 7u}) << 7));
    PutU32V(img, 0, cls);
    PutU16V(img, 172, static_cast<uint16_t>(0x80u | slot));
    uint32_t f = 0;
    if (Chance(rng, 5)) f |= 4u;
    if (Chance(rng, 5)) f |= 0x800u;
    if (Chance(rng, 4)) f |= 0x200u;
    if (Chance(rng, 5)) f |= 0x400u;
    PutU32V(img, 592, f);
    PutU32V(img, 180, Pick(rng, {30u, 30u, 0u, 1u, 2u, 3u}));
    PutU32V(img, 480, Pick(rng, {0u, 0u, 0x10000u, 0x40000u, rng.U32() % 0x200000u}));
    int16_t d[3];
    for (uint32_t o : {438u, 450u, 522u}) {
        Dir(rng, d);
        if (o == 522u && Chance(rng, 2)) { d[0] = 0; d[1] = -4096; d[2] = 0; }
        for (uint32_t k = 0; k < 3; ++k) PutU16V(img, o + 2 * k, static_cast<uint16_t>(d[k]));
    }
    m.WriteBlock(P, img.data(), img.size());
    co::MoveBox(m, P, GetS32(m, e + 184) + static_cast<int32_t>(rng.U32() % 0x40000u) - 0x20000 - GetS32(m, P + 184), 0,
                GetS32(m, e + 192) + static_cast<int32_t>(rng.U32() % 0x40000u) - 0x20000 - GetS32(m, P + 192));
    return P;
}

// A partner for the solver and the gate: {shape, partner entity or 0}. kind 0 bike, 1 car, 2 volume, 3 wall,
// 4 rider 6, 5 a pedestrian.
struct Partner {
    uint32_t shape = 0, entity = 0, kind = 0;
};
Partner PickPartner(CaseContext& c, uint32_t e, uint32_t eslot, bool walls) {
    Rng& rng = *c.rng;
    Memory& m = *c.mem;
    Partner p;
    p.kind = static_cast<uint32_t>(rng.Next() % (walls ? 7u : 5u));
    if (walls && p.kind == 5) p.kind = 3;
    else if (!walls && p.kind == 3) p.kind = 5;
    auto off = [&rng](uint32_t units) {
        return static_cast<int32_t>(rng.U32() % (2u * 65536u * units)) - static_cast<int32_t>(65536u * units);
    };
    switch (p.kind) {
    case 0: {
        uint32_t k = rng.U32() % 18u;
        if (k == eslot) k = (k + 1u) % 18u;
        p.entity = Pool0(m, k);
        p.shape = p.entity + 172;
        break;
    }
    case 1: p.entity = PoolEntity(m, 3, 0); p.shape = p.entity + 172; break;
    case 2: {
        const uint32_t n6 = 1u + rng.U32() % 3u;
        co::PlantVolumes(c, e, n6);
        p.entity = PoolEntity(m, 6, rng.U32() % n6);
        p.shape = p.entity;
        break;
    }
    case 3: p.shape = WallSegment(c); break;
    case 6: p.entity = PlantProp(c, e); p.shape = p.entity + 172; break;
    case 4: p.entity = PoolEntity(m, 1, 6); p.shape = p.entity + 172; break;
    default: p.entity = PoolEntity(m, 2, rng.U32() % 3u); p.shape = p.entity + 172; break;
    }
    // the bike next to the partner (level with it, or up to 1.5 above / below) now and then
    if (p.entity != 0 && p.kind != 2 && Chance(rng, 2))
        co::MoveNear(m, e, p.entity, off(3), Chance(rng, 2) ? 0 : off(2) * 3 / 4, off(3));
    return p;
}

std::vector<OracleCallee> SolveCallees() {
    using namespace rr::sim::solve;
    return {OracleCallee{kHitOutcome, 3}, OracleCallee{kImpactTurn, 4}, OracleCallee{kPropKnock, 4, 1},
            OracleCallee{kPropKick, 3},   OracleCallee{kSurfaceSound, 1}, OracleCallee{kLaunch, 2},
            OracleCallee{rr::sim::coll::kPadMotor, 4}};
}

uint32_t Finish(NativeEnv& env, EnRun& run, bool ok, const char* what, uint32_t v0) {
    if (!ok && env.failure.empty() && !run.s.Faulted()) env.failure = std::string(what) + " refused";
    return run.Done(env, v0);
}

// ---------------------------------------------------------------- FaceCrossing, 0x800B5EB4
//
// Scratch: A +0, B +12, dirV +24, n +32, P0..P3 +40/+52/+64/+76, ax1 +88, ax2 +96, *cross +104, *t +108.
// A rectangle from P0 along two unit axes (perpendicular, or anything) with extents 0.25..4.0, its corners
// P1..P3 (or scattered); A and B on either side of its plane, inside or outside the slabs, the segment
// sometimes parallel to an axis (|DotLcm| < 655). Arguments 9..14 are stored at sp+36..56 (the bench
// stores a4 and more[0..3] at sp+16..32).
void SetupFace(CaseContext& c, Args& a) {
    Rng& rng = *c.rng;
    Memory& m = *c.mem;
    const uint32_t S = c.scratch;
    int16_t ax1[3], ax2[3], nn[3], dv[3];
    Dir(rng, ax1);
    if (Chance(rng, 4)) Dir(rng, ax2);
    else { ax2[0] = static_cast<int16_t>(-ax1[2]); ax2[1] = 0; ax2[2] = ax1[0]; }
    Dir(rng, nn);
    if (Chance(rng, 2)) { nn[0] = static_cast<int16_t>(ax1[1] * ax2[2] / 4096 - ax1[2] * ax2[1] / 4096);
                          nn[1] = static_cast<int16_t>(ax1[2] * ax2[0] / 4096 - ax1[0] * ax2[2] / 4096);
                          nn[2] = static_cast<int16_t>(ax1[0] * ax2[1] / 4096 - ax1[1] * ax2[0] / 4096); }
    const int32_t l1 = static_cast<int32_t>(0x4000u + rng.U32() % 0x3C000u), l2 = static_cast<int32_t>(0x4000u + rng.U32() % 0x3C000u);
    int32_t P0[3];
    for (int k = 0; k < 3; ++k) P0[k] = rng.World();
    auto at = [&](int32_t u1, int32_t u2, int32_t h, int32_t out[3]) {
        for (int k = 0; k < 3; ++k)
            out[k] = static_cast<int32_t>(P0[k] + ((static_cast<int64_t>(ax1[k]) * u1 + static_cast<int64_t>(ax2[k]) * u2 + static_cast<int64_t>(nn[k]) * h) >> 12));
    };
    int32_t P[4][3];
    for (int k = 0; k < 3; ++k) P[0][k] = P0[k];
    at(l1, 0, 0, P[1]);
    at(l1, l2, 0, P[2]);
    at(0, l2, 0, P[3]);
    if (Chance(rng, 6)) for (int q = 1; q < 4; ++q) for (int k = 0; k < 3; ++k) P[q][k] = rng.World();
    auto u = [&rng](int32_t lim) { return static_cast<int32_t>(rng.U32() % static_cast<uint32_t>(3 * lim)) - lim; };
    int32_t A[3], B[3];
    const int32_t hA = static_cast<int32_t>(rng.U32() % 0x40000u) - 0x20000, hB = Chance(rng, 3) ? hA : -static_cast<int32_t>(rng.U32() % 0x40000u) + 0x8000;
    at(u(l1), u(l2), hA, A);
    at(u(l1), u(l2), hB, B);
    if (Chance(rng, 3)) for (int k = 0; k < 3; ++k) dv[k] = static_cast<int16_t>(Chance(rng, 2) ? ax1[k] : ax2[k]);
    else Dir(rng, dv);
    if (Chance(rng, 8)) { Dir(rng, ax1); Dir(rng, ax2); }
    for (int k = 0; k < 3; ++k) { PutS32(m, S + 4 * k, A[k]); PutS32(m, S + 12 + 4 * k, B[k]); }
    PutVec3S16(m, S + 24, dv);
    PutVec3S16(m, S + 32, nn);
    for (int q = 0; q < 4; ++q) for (int k = 0; k < 3; ++k) PutS32(m, S + 40 + 12 * q + 4 * k, P[q][k]);
    PutVec3S16(m, S + 88, ax1);
    PutVec3S16(m, S + 96, ax2);
    m.PokeWord(S + 104, rng.U32());
    m.PokeWord(S + 108, rng.U32());
    a.a = {S, S + 12, S + 24, S + 32};
    a.a4 = Pick(rng, {0u, 0x20000u, rng.U32() % 0x40000u});
    a.more = {S + 40, S + 52, S + 64, S + 76};
    const uint32_t tail[6] = {S + 88, S + 96, static_cast<uint32_t>(Chance(rng, 8) ? rng.S32() >> 8 : l1),
                              static_cast<uint32_t>(Chance(rng, 8) ? rng.S32() >> 8 : l2), S + 104, S + 108};
    for (uint32_t k = 0; k < 6; ++k) m.PokeWord(c.sp + 36u + 4u * k, tail[k]);
}
uint32_t NativeFace(NativeEnv& env, const Args& a) {
    EnRun run(env);
    uint32_t st[11];
    for (uint32_t k = 0; k < 11; ++k) st[k] = run.g.U32(env.sp + 16u + 4u * k);
    uint32_t v0 = 0;
    const bool ok = rr::sim::FaceCrossing(run.g, a.a[0], a.a[1], a.a[2], a.a[3], st, env.sp, v0);
    return Finish(env, run, ok, "FaceCrossing", v0);
}

// ---------------------------------------------------------------- LandOnTop, 0x800AD9BC
//
// One of this machine's bikes. Driven: h (0, the game's heights, both signs), cross and throw (0, 1,
// anything), k (at and around 0x7FFF / 1.0, both signs), +0x134 left as the bike's own, +0x268 both signs.
// NAMED BOUND: |h| below 64.0, so that h / (2 * +0x134) stays a 16.16 value (Asin's INT32_MIN refusal).
void SetupLand(CaseContext& c, Args& a) {
    Rng& rng = *c.rng;
    Memory& m = *c.mem;
    const uint32_t e = Pool0(m, Pick(rng, {0u, 0u, 6u, rng.U32() % 18u}));
    PerturbBike(c, e, 0);
    m.PokeWord(e + 616, Pick(rng, {0u, 0x8000u, 0xFFFF8000u, 0x20000u, rng.U32() % 0x40000u, static_cast<uint32_t>(rng.S32() >> 12)}));
    a.a[0] = e;
    a.a[1] = Pick(rng, {0u, 0x8000u, 0x10000u, 0x30000u, 0xFFFF0000u, m.PeekWord(e + 308) * 2u, m.PeekWord(e + 308) * 3u,
                        static_cast<uint32_t>(rng.S32() >> 9)});
    a.a[2] = Pick(rng, {0u, 1u, 1u, rng.U32()});
    a.a[3] = Pick(rng, {0u, 1u, 1u, 0x20000u, rng.U32()});
    a.a4 = Pick(rng, {0u, 0x7FFFu, 0x8000u, 0xFFFFu, 0x10000u, 0x10001u, 0x20000u, rng.U32() % 0x20000u, 0xFFFF0000u});
}
uint32_t NativeLand(NativeEnv& env, const Args& a) {
    EnRun run(env);
    co::CloneCallees cc(env, run);
    const cr::Tables tab(env.clone->mem, env.gp);
    uint32_t v0 = 0;
    const bool ok = rr::sim::LandOnTop(run.g, a.a[0], static_cast<int32_t>(a.a[1]), a.a[2], a.a[3],
                                       static_cast<int32_t>(a.a4), env.sp, tab.t, cc, v0);
    return Finish(env, run, ok, "LandOnTop", v0);
}

// ---------------------------------------------------------------- PropHitFace, 0x800B0510
//
// Scratch: the bike at +0 (a captured image), the prop at +2200 (another), out at +4400. The prop placed
// around the bike (+-3.0), both velocities drawn (the separating test both ways), the headings anything.
void SetupPropFace(CaseContext& c, Args& a) {
    Rng& rng = *c.rng;
    Memory& m = *c.mem;
    std::vector<uint8_t> e = co::BikeImage(c), p = co::BikeImage(c);
    const uint32_t E = c.scratch, PR = c.scratch + 2200, OUT = c.scratch + 4400;
    for (uint32_t k = 0; k < 3; ++k) {
        PutU32V(p, 184 + 4 * k, GetU32V(e, 184 + 4 * k) + static_cast<uint32_t>(static_cast<int32_t>(rng.U32() % 0x60000u) - 0x30000));
        PutU32V(e, 456 + 4 * k, static_cast<uint32_t>(rng.S32() >> (8 + rng.Next() % 10u)));
        PutU32V(e, 196 + 4 * k, GetU32V(e, 184 + 4 * k) + static_cast<uint32_t>(static_cast<int32_t>(rng.U32() % 0x40000u) - 0x20000));
    }
    int16_t h[3];
    Dir(rng, h);
    for (uint32_t k = 0; k < 3; ++k) PutU16V(p, 450 + 2 * k, static_cast<uint16_t>(h[k]));
    PutU32V(p, 480, Pick(rng, {0u, 0x10000u, 0x80000u, rng.U32() % 0x400000u}));
    if (c.sample == nullptr || Chance(rng, 3)) {
        int16_t r[3];
        for (uint32_t o : {432u, 438u, 444u}) {
            Dir(rng, r);
            for (uint32_t k = 0; k < 3; ++k) PutU16V(e, o + 2 * k, static_cast<uint16_t>(r[k]));
        }
    }
    m.WriteBlock(E, e.data(), e.size());
    m.WriteBlock(PR, p.data(), p.size());
    for (uint32_t k = 0; k < 3; ++k) WriteU16Bench(m, OUT + 2 * k, static_cast<uint16_t>(rng.U32()));
    a.a[0] = E;
    a.a[1] = PR;
    a.a[2] = OUT;
}
uint32_t NativePropFace(Memory& m, uint32_t, uint32_t gp, const Args& a) {
    rr::sim::GuestRam g(m.ram().data(), gp);
    uint32_t v0 = 0;
    const bool ok = rr::sim::PropHitFace(g, a.a[0], a.a[1], a.a[2], 0, v0);
    return ok ? v0 : 0xDEADDEADu;
}

// ---------------------------------------------------------------- ImpactSolve, 0x800AF3B0
//
// e one of this machine's bikes (PerturbBike), the partner PickPartner's (walls included; the bike put on
// the wall's line), nrm at scratch+0, imp at scratch+16 (0 now and then), face 0..6, flags the callers'
// 1 / 3 / t0 | 4 / BikeWallHit's (fast << 3) | 1 or 5, or any low nibble.
void SetupSolve(CaseContext& c, Args& a) {
    Rng& rng = *c.rng;
    Memory& m = *c.mem;
    const uint32_t slot = Pick(rng, {0u, 0u, 6u, 6u, rng.U32() % 18u});
    const uint32_t e = Pool0(m, slot);
    const Partner p = PickPartner(c, e, slot, true);
    PerturbBike(c, e, p.shape);
    Normal(c, c.scratch, e, p.kind == 3 ? p.shape + 32 : 0);
    if (p.kind == 3) PlaceAtWall(c, e, p.shape, c.scratch);
    a.a[0] = e;
    a.a[1] = p.shape;
    a.a[2] = c.scratch;
    a.a[3] = static_cast<uint32_t>(rng.Next() % 7u);
    a.a4 = Pick(rng, {1u, 3u, 1u | 4u, 5u, 13u, 9u, 0u, 2u, 6u, rng.U32() & 0xFu});
    a.more[0] = Chance(rng, 10) ? 0u : Push(c, c.scratch + 16);
}
uint32_t NativeSolve(NativeEnv& env, const Args& a) {
    EnRun run(env);
    co::CloneCallees cc(env, run);
    const cr::Tables tab(env.clone->mem, env.gp);
    uint32_t v0 = 0;
    const bool ok = rr::sim::ImpactSolve(run.g, a.a[0], a.a[1], a.a[2], a.a[3], a.a4, a.more[0], env.sp, tab.t, cc, v0);
    return Finish(env, run, ok, "ImpactSolve", v0);
}

// ---------------------------------------------------------------- ImpactGate, 0x800AF224
//
// The partner as for the solver but no wall (a gate caller never has one). code 0 (the riding-on-it arm,
// with +0x340 = shape and flagsB bit 15 drawn so it passes now and then), 0x100 | side bits, a face 0..5
// with or without flags 0x200. NAMED BOUND: the face index handed to FaceNormal 0x800B675C is 0..5 (a box
// has six faces; the unported leaf indexes a table by it).
void SetupGate(CaseContext& c, Args& a) {
    Rng& rng = *c.rng;
    Memory& m = *c.mem;
    const uint32_t slot = Pick(rng, {0u, 0u, 6u, 6u, rng.U32() % 18u});
    const uint32_t e = Pool0(m, slot);
    const Partner p = PickPartner(c, e, slot, false);
    PerturbBike(c, e, p.shape);
    const uint32_t face = rng.U32() % 6u;
    a.a[0] = e;
    a.a[1] = p.shape;
    a.a[2] = Pick(rng, {0u, 0u, 0x100u, 0x101u, 0x102u, 0x103u, face, face | 0x100u});
    a.a[3] = face | (Chance(rng, 2) ? 0x200u : 0u) | (Chance(rng, 4) ? (rng.U32() & 0xFC00u) : 0u);
    a.a4 = Chance(rng, 10) ? 0u : Push(c, c.scratch + 16);
}
uint32_t NativeGate(NativeEnv& env, const Args& a) {
    EnRun run(env);
    co::CloneCallees cc(env, run);
    const cr::Tables tab(env.clone->mem, env.gp);
    uint32_t v0 = 0;
    const bool ok = rr::sim::ImpactGate(run.g, a.a[0], a.a[1], a.a[2], a.a[3], a.a4, env.sp, tab.t, cc, v0);
    return Finish(env, run, ok, "ImpactGate", v0);
}

// ---------------------------------------------------------------- PoleReact, 0x800AF0A0
//
// A synthetic pool-6 volume (co::PlantVolumes; a pole when +8 == 1, as DeferredContacts picks it), the
// bike next to it half the time; dir at scratch+0, imp at scratch+16; flags the record's `& 0xFFEF` (a face
// 0..5 and the high bits), bit4 0 or 0x10.
void SetupPole(CaseContext& c, Args& a) {
    Rng& rng = *c.rng;
    Memory& m = *c.mem;
    const uint32_t slot = Pick(rng, {0u, 0u, 6u, rng.U32() % 18u});
    const uint32_t e = Pool0(m, slot);
    const uint32_t n6 = 1u + rng.U32() % 3u;
    co::PlantVolumes(c, e, n6);
    const uint32_t shp = PoolEntity(m, 6, rng.U32() % n6);
    if (Chance(rng, 2)) {
        const int32_t ox = static_cast<int32_t>(rng.U32() % 0x60000u) - 0x30000, oz = static_cast<int32_t>(rng.U32() % 0x60000u) - 0x30000;
        co::MoveBox(m, e, GetS32(m, shp + 12) + ox - GetS32(m, e + 184), 0, GetS32(m, shp + 20) + oz - GetS32(m, e + 192));
    }
    PerturbBike(c, e, shp);
    Normal(c, c.scratch, e, 0);
    a.a[0] = e;
    a.a[1] = shp;
    a.a[2] = c.scratch;
    a.a[3] = (rng.U32() % 6u) | (Chance(rng, 4) ? (rng.U32() & 0xFFE0u) : 0u);
    a.a4 = Chance(rng, 2) ? 0u : 0x10u;
    a.more[0] = Push(c, c.scratch + 16);
}
uint32_t NativePole(NativeEnv& env, const Args& a) {
    EnRun run(env);
    co::CloneCallees cc(env, run);
    const cr::Tables tab(env.clone->mem, env.gp);
    uint32_t v0 = 0;
    const bool ok = rr::sim::PoleReact(run.g, a.a[0], a.a[1], a.a[2], a.a[3], a.a4, a.more[0], env.sp, tab.t, cc, v0);
    return Finish(env, run, ok, "PoleReact", v0);
}

// ---------------------------------------------------------------- BoxReact, 0x800B11B4
//
// The partner and code / flags as for the gate; imp at scratch+16, dir at scratch+0.
void SetupBox(CaseContext& c, Args& a) {
    SetupGate(c, a);
    Normal(c, c.scratch + 32, a.a[0], 0);
    a.more[0] = c.scratch + 32;
}
uint32_t NativeBox(NativeEnv& env, const Args& a) {
    EnRun run(env);
    co::CloneCallees cc(env, run);
    const cr::Tables tab(env.clone->mem, env.gp);
    uint32_t v0 = 0;
    const bool ok = rr::sim::BoxReact(run.g, a.a[0], a.a[1], a.a[2], a.a[3], a.a4, a.more[0], env.sp, tab.t, cc, v0);
    return Finish(env, run, ok, "BoxReact", v0);
}

// ---------------------------------------------------------------- BikeWallHit, 0x800B12A0
//
// The arguments WallContact and DeferredContacts' pool-8 arm pass: one of the player's road wall
// segments (its flag word and +4 redrawn half the time), the normal (the segment's, reversed, the bike's
// rows, anything) at scratch+0, flags = the segment's flags & 0xFFF or drawn (type 0..8, 0x200, 0x400, the
// sound nibble 0x10..0x40), segVal the segment's +4 or drawn (negative -> 0x640000), imp at scratch+16.
// The bike: PerturbBike, its +0x340 its own shape now and then (the early return), its ground point on
// the segment's line (the plane test of ImpactSolve's pool-8 arm), +0x184 bit 4 and the speed against 10.0
// (the fast arm, and flags & 8 into the solver).
void SetupWallHit(CaseContext& c, Args& a) {
    Rng& rng = *c.rng;
    Memory& m = *c.mem;
    const uint32_t slot = Pick(rng, {0u, 0u, 6u, 6u, rng.U32() % 18u});
    const uint32_t e = Pool0(m, slot);
    const uint32_t seg = WallSegment(c);
    PerturbBike(c, e, seg);
    if (Chance(rng, 8)) m.PokeWord(e + 832, e + 172);
    if (Chance(rng, 2)) m.PokeWord(e + 480, Pick(rng, {0xA0001u, 0x140000u, 0x3C0000u, rng.U32() % 0x600000u}));
    Normal(c, c.scratch, e, seg + 32);
    PlaceAtWall(c, e, seg, c.scratch);
    uint32_t fl = static_cast<uint32_t>(GetS16(m, seg + 2)) & 0xFFFu;
    if (Chance(rng, 2)) {
        fl = Pick(rng, {1u, 2u, 3u, 3u, 5u, 6u, 7u, 8u, 0u, 4u, rng.U32() % 16u});
        if (Chance(rng, 3)) fl |= 0x200u;
        if (Chance(rng, 3)) fl |= 0x400u;
        if (Chance(rng, 3)) fl |= 0x100u;
    }
    if (Chance(rng, 3)) fl |= (rng.U32() % 6u) << 4;
    a.a[0] = e;
    a.a[1] = c.scratch;
    a.a[2] = fl;
    a.a[3] = Pick(rng, {m.PeekWord(seg + 4), m.PeekWord(seg + 4), 0xFFFF0600u, 0x10000u, 0x8000u, 0x30000u,
                        static_cast<uint32_t>(rng.S32() >> 12)});
    a.a4 = seg;
    a.more[0] = Push(c, c.scratch + 16);
}
uint32_t NativeWallHit(NativeEnv& env, const Args& a) {
    EnRun run(env);
    co::CloneCallees cc(env, run);
    const cr::Tables tab(env.clone->mem, env.gp);
    const bool ok = rr::sim::BikeWallHit(run.g, a.a[0], a.a[1], a.a[2], static_cast<int32_t>(a.a[3]), a.a4, a.more[0],
                                         env.sp, tab.t, cc);
    return Finish(env, run, ok, "BikeWallHit", 0);
}

} // namespace csolve

void AddCollSolveRows(std::vector<Row>& rows) {
    using namespace rr::sim::solve;
    {
        rows.push_back(Row{"face_crossing", "s32 0x800B5EB4(A, B, dirV, n, [len2, P0..P3, ax1, ax2, lim1, lim2, *cross, *t])  [RASHCDG]",
                           kFaceCrossing, csolve::SetupFace, nullptr});
        rows.back().nativeEx = csolve::NativeFace;
    }
    {
        rows.push_back(Row{"land_on_top", "s32 0x800AD9BC(Bike *e, s32 h, cross, throw, [k])  [RASHCDG]", kLandOnTop,
                           csolve::SetupLand, nullptr});
        Row& r = rows.back();
        r.nativeEx = csolve::NativeLand;
        r.oracleCallees = {OracleCallee{kLaunch, 2}};
    }
    rows.push_back(Row{"prop_hit_face", "s32 0x800B0510(Bike *e, Prop *prop, s16 out[3])  [RASHCDG]", kPropHitFace,
                       csolve::SetupPropFace, csolve::NativePropFace});
    {
        rows.push_back(Row{"impact_solve", "s32 0x800AF3B0(e, shape, nrm, face, [flags, imp])  [RASHCDG, the impact outcome]",
                           kImpactSolve, csolve::SetupSolve, nullptr});
        Row& r = rows.back();
        r.nativeEx = csolve::NativeSolve;
        r.maxSteps = 20000000;
        r.oracleCallees = csolve::SolveCallees();
    }
    {
        rows.push_back(Row{"impact_gate", "s32 0x800AF224(e, shape, code, flags, [imp])  [RASHCDG]", kImpactGate,
                           csolve::SetupGate, nullptr});
        Row& r = rows.back();
        r.nativeEx = csolve::NativeGate;
        r.maxSteps = 20000000;
        r.oracleCallees = csolve::SolveCallees();
        r.oracleCallees.push_back(OracleCallee{kFaceNormal, 4, 1});
    }
    {
        rows.push_back(Row{"pole_react", "s32 0x800AF0A0(e, shp, dir, flags, [bit4, imp])  [RASHCDG]", kPoleReact,
                           csolve::SetupPole, nullptr});
        Row& r = rows.back();
        r.nativeEx = csolve::NativePole;
        r.maxSteps = 20000000;
        r.oracleCallees = csolve::SolveCallees();
        r.oracleCallees.push_back(OracleCallee{kImpactSeverity, 4, 1});
    }
    {
        rows.push_back(Row{"box_react", "s32 0x800B11B4(e, shp, code, flags, [imp, dir])  [RASHCDG]", kBoxReact,
                           csolve::SetupBox, nullptr});
        Row& r = rows.back();
        r.nativeEx = csolve::NativeBox;
        r.maxSteps = 20000000;
        r.oracleCallees = csolve::SolveCallees();
        r.oracleCallees.push_back(OracleCallee{kFaceNormal, 4, 1});
        r.oracleCallees.push_back(OracleCallee{kImpactSeverity, 4, 1});
    }
    {
        rows.push_back(Row{"bike_wall_hit", "void 0x800B12A0(Bike *e, s16 n[3], flags, segVal, [seg, imp])  [RASHCDG, the walls push]",
                           kBikeWallHit, csolve::SetupWallHit, nullptr});
        Row& r = rows.back();
        r.compareV0 = false;
        r.nativeEx = csolve::NativeWallHit;
        r.maxSteps = 20000000;
        r.oracleCallees = csolve::SolveCallees();
        r.oracleCallees.push_back(OracleCallee{kImpactSeverity, 4, 1});
        r.oracleCallees.push_back(OracleCallee{kRaiseHeading, 4});
        r.oracleCallees.push_back(OracleCallee{kGetRCnt, 1});
        r.oracleCallees.push_back(OracleCallee{kWallStance, 2});
    }
}
