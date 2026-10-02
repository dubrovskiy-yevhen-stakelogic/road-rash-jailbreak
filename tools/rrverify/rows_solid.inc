// Bench rows for src/game/sim/solid.{h,cpp}. Included by
// verify_physics.cpp inside its anonymous namespace, after rows_coll.inc (co::, crs::, cr:: helpers).
//
// The pole rows run on this machine's live bikes against pool-6 pole records (crs::PlantResolve kind 2),
// the lean +0x2A4 always inside the band LeanPoleTest is called for (+-1.5864, not 0). The prop rows run
// on a prop of our own making in pool-4 slot 0: a live bike's first 596 bytes (its box, rows, heading and
// speed), handle 0x80, its model record pointer +0 aimed at a scratch record whose +0x0E carries the DOD3
// kind 0..6 (every arm of PropKick's lift), the surface index +0xB4, the flags +0x250 bits 1 / 3 / 4 / 6 /
// 9, the tip rates +0x1E4 / +0x1E8, the fall side +0x21C, the quaternions +0x230 / +0x23C, the rows.
// NAMED BOUNDS: headings and plane normals are unit vectors (the game's; a non-unit one makes Normalize
// raise its overflow exception on the console); PropTopple without a point reads its own stale sp+48
// (the same stack on both sides here - in the game it is whatever the caller's frame left).

namespace solidrows {

using co::Chance;
using co::Pick;

constexpr uint32_t kPush = 0, kLen = 16, kPoint = 32, kN = 48, kModel = 64, kDirF = 96, kDirR = 104, kDirU = 112;

void Dir(Rng& rng, Memory& m, uint32_t a) {
    int16_t d[3];
    co::Dir(rng, d);
    PutVec3S16(m, a, d);
}
int32_t Units(Rng& rng, int32_t units) {
    return static_cast<int32_t>(rng.U32() % (2u * 65536u * static_cast<uint32_t>(units))) - 65536 * units;
}

// ---------------------------------------------------------------- LeanPoleTest, 0x800ADC74
// e and the pole from PlantResolve(kind 2); the lean (both signs, small to 1.58), the yaw +0x27C's
// sign, the frame's +0x20C around its 4033 threshold, corner 0 against corner 3 (the side), dt and
// speed; the radius as PoleResolve computes it (0 .. 3.0 less 16) or anything; the pole's centre moved
// into the footprint now and then.
void SetupLean(CaseContext& c, Args& a) {
    Rng& rng = *c.rng;
    Memory& m = *c.mem;
    const crs::ResolveCase rc = crs::PlantResolve(c, 2);
    const uint32_t e = rc.e, s = rc.shape;
    int32_t lean = static_cast<int32_t>(rng.U32() % 0x19000u) + 1;
    if (Chance(rng, 2)) lean = -lean;
    PutS32(m, e + 676, lean);
    if (Chance(rng, 2)) PutS32(m, e + 636, rng.S32() >> (rng.Next() % 16u));
    if (Chance(rng, 3)) WriteU16Bench(m, e + 524, static_cast<uint16_t>(Pick(rng, {4032u, 4033u, 0xF03Fu, 0xF040u, 4096u, rng.U32() & 0xFFFFu})));
    if (Chance(rng, 3)) PutS32(m, e + 236, GetS32(m, e + 200) + Units(rng, 1));
    if (Chance(rng, 2)) {
        PutS32(m, s + 12, GetS32(m, e + 184) + Units(rng, 2));
        PutS32(m, s + 20, GetS32(m, e + 192) + Units(rng, 2));
    }
    const uint32_t S = c.scratch;
    for (uint32_t k = 0; k < 4; ++k) m.PokeWord(S + kPush + 4 * k, rng.U32());
    a.a[0] = e;
    a.a[1] = s;
    a.a[2] = Chance(rng, 6) ? rng.U32() >> (rng.Next() % 12u) : rng.U32() % 0x30000u - 16u;
    a.a[3] = S + kPush;
    a.a4 = S + kLen;
}
uint32_t NativeLean(NativeEnv& env, const Args& a) {
    EnRun run(env);
    const cr::Tables tab(env.clone->mem, env.gp);
    uint32_t v = 0;
    const bool ok = rr::sim::LeanPoleTest(run.g, a.a[0], a.a[1], static_cast<int32_t>(a.a[2]), a.a[3], a.a4, env.sp,
                                          tab.t, v);
    return crs::Fin(env, run, ok, v, "LeanPoleTest");
}

// ---------------------------------------------------------------- PoleResolve 0x800AE794 with LeanPoleTest native
// The same case as rows_coll_resolve's pole_resolve, the lean now in the band half the time, so the
// leaning test runs natively under PoleResolve (PoleReact on the oracle).
struct LeanNative final : rr::sim::CollisionCallees {
    co::CloneCallees& in;
    rr::sim::GuestRam& g;
    const rr::sim::BikeTables& t;
    LeanNative(co::CloneCallees& i, rr::sim::GuestRam& gg, const rr::sim::BikeTables& tt) : in(i), g(gg), t(tt) {}
    bool PlaySound3D(int32_t x, int32_t z, int32_t id, int32_t bank) override { return in.PlaySound3D(x, z, id, bank); }
    bool ReleaseContact(uint32_t e) override { return in.ReleaseContact(e); }
    bool Rumble(uint32_t e, uint32_t o, int32_t s, int32_t k, int32_t d, uint32_t sp) override {
        return in.Rumble(e, o, s, k, d, sp);
    }
    bool Unported(uint32_t fn, const uint32_t* a, int n, uint32_t sp, uint32_t& v0) override {
        if (fn == rr::sim::solid::kLeanPoleTest) {
            bool ok = true;
            rr::sim::ServeSolid(g, rr::sim::CollCall{fn, a, n, sp}, t, *this, v0, ok);
            return ok;
        }
        return in.Unported(fn, a, n, sp, v0);
    }
    bool UnportedAt(uint32_t fn, const uint32_t* a, int n, uint32_t sp, const rr::sim::GuestRegs& r,
                    uint32_t& v0) override {
        if (fn == rr::sim::solid::kLeanPoleTest) return Unported(fn, a, n, sp, v0);
        return in.UnportedAt(fn, a, n, sp, r, v0);
    }
};
void SetupPoleNative(CaseContext& c, Args& a) {
    Rng& rng = *c.rng;
    Memory& m = *c.mem;
    const crs::ResolveCase rc = crs::PlantResolve(c, 2);
    if (Chance(rng, 2)) {
        int32_t lean = static_cast<int32_t>(rng.U32() % 0x19000u) + 1;
        PutS32(m, rc.e + 676, Chance(rng, 2) ? -lean : lean);
        const uint16_t st = static_cast<uint16_t>(m.PeekWord(rc.e + 320) & 0xFFFFu);
        if (Chance(rng, 2)) WriteU16Bench(m, rc.e + 320, static_cast<uint16_t>(st & ~0xCu));
    }
    a.a[0] = rc.e;
    a.a[1] = rc.shape;
}
uint32_t NativePoleNative(NativeEnv& env, const Args& a) {
    EnRun run(env);
    co::CloneCallees cc(env, run);
    const cr::Tables tab(env.clone->mem, env.gp);
    LeanNative ln(cc, run.g, tab.t);
    const bool ok = rr::sim::PoleResolve(run.g, a.a[0], a.a[1], env.sp, tab.t, ln);
    return co::Refuse(env, run, ok, "PoleResolve");
}

// ---------------------------------------------------------------- the prop of our own making
uint32_t PlantProp(CaseContext& c) {
    Rng& rng = *c.rng;
    Memory& m = *c.mem;
    const uint32_t S = c.scratch;
    const uint32_t base = m.PeekWord(rr::sim::kWpPool4Ctrl + 12u);
    const uint32_t p = base;
    const uint32_t bike = co::Pool0(m, Pick(rng, {0u, 6u, rng.U32() % 18u}));
    std::vector<uint8_t> b(596);
    m.ReadBlock(bike, b.data(), 596);
    m.WriteBlock(p, b.data(), 596);
    WriteU16Bench(m, p + 172, 0x80);
    // the model record: +0x0E = the DOD3 kind << 7 (and stray bits)
    const uint32_t kind = rng.U32() % 7u;
    WriteU16Bench(m, S + kModel + 14, static_cast<uint16_t>((kind << 7) | (Chance(rng, 3) ? (rng.U32() & 0xF07Fu) : 0u)));
    m.PokeWord(p + 0, S + kModel);
    PutS32(m, p + 180, Pick(rng, {0u, 3u, 17u, 51u, 52u, 0xFFFFFFFFu, rng.U32() % 64u}));
    uint32_t f = Pick(rng, {0u, 0u, 2u, 8u, 0x10u, 0x40u, 0x200u, rng.U32() & 0x25Bu});
    m.PokeWord(p + 592, f);
    if (Chance(rng, 2)) Dir(rng, m, p + 450);
    if (Chance(rng, 3)) Dir(rng, m, p + 522);
    if (Chance(rng, 3)) Dir(rng, m, p + 432);
    if (Chance(rng, 3)) Dir(rng, m, p + 444);
    m.PokeWord(p + 480, Pick(rng, {0u, 0x8000u, 0x4FFFFu, 0x50001u, 0x9FFFFu, 0xA0000u, rng.U32() % 0x200000u}));
    PutS32(m, p + 484, static_cast<int32_t>(Pick(rng, {0u, 0x7FFFu, 0x8000u, 0x30000u, rng.U32() % 0x20000u})));
    PutS32(m, p + 488, static_cast<int32_t>(Pick(rng, {0u, 0x10000u, 0x60000u, 0xC0000u, 0xFFFF0000u, 0xFFF00000u, rng.U32() % 0x100000u - 0x80000u})));
    PutS32(m, p + 540, static_cast<int32_t>(Pick(rng, {0u, 1u, 0xFFFFFFFFu, rng.U32()})));
    PutS32(m, p + 556, static_cast<int32_t>(rng.U32() % 0x10000u));
    for (uint32_t k = 0; k < 4; ++k) {
        WriteU16Bench(m, p + 560 + 2 * k, static_cast<uint16_t>(rng.S16() >> 3));
        WriteU16Bench(m, p + 572 + 2 * k, static_cast<uint16_t>(rng.S16() >> 3));
    }
    const uint16_t st = static_cast<uint16_t>(m.PeekWord(p + 320) & 0xFFFFu);
    WriteU16Bench(m, p + 320, static_cast<uint16_t>((st & ~0xCu) | Pick(rng, {0xCu, 0x8u, 0u})));
    m.PokeWord(rr::sim::kWpPool4Ctrl, 1);
    m.PokeWord(rr::sim::kWpPool4Ctrl + 4u, 1);
    m.PokeWord(rr::sim::kWpPool4Ctrl + 8u, 0);
    m.PokeWord(c.gp + kRandSeedGpOffset, rng.U32());
    return p;
}

// ---------------------------------------------------------------- PropUpRows, 0x800A3CBC
void SetupUpRows(CaseContext& c, Args& a) {
    Rng& rng = *c.rng;
    Memory& m = *c.mem;
    const uint32_t p = PlantProp(c);
    const uint32_t mode = rng.U32() % 4u;
    if (mode == 1) PutVec3S16(m, p + 522, std::array<int16_t, 3>{0, 0, 0}.data());         // degenerate first OP
    if (mode == 2) {                                                                          // up along row 0
        int16_t r[3];
        GetVec3S16(m, p + 432, r);
        PutVec3S16(m, p + 522, r);
    }
    if (mode == 3) {                                                                          // up along both
        int16_t r[3];
        GetVec3S16(m, p + 432, r);
        PutVec3S16(m, p + 522, r);
        PutVec3S16(m, p + 444, r);
    }
    a.a[0] = p;
}
uint32_t NativeUpRows(NativeEnv& env, const Args& a) {
    EnRun run(env);
    const cr::Tables tab(env.clone->mem, env.gp);
    const bool ok = rr::sim::PropUpRows(run.g, a.a[0], tab.t);
    return crs::Fin(env, run, ok, 0, "PropUpRows");
}

// ---------------------------------------------------------------- PropTopple, 0x800B3344
// The point: 0, a prop corner pushed through the plane, or one beside it; n: a unit direction, often
// the prop's own heading (the tip arms), mode 0 / 1 / -1 / anything.
void SetupTopple(CaseContext& c, Args& a) {
    Rng& rng = *c.rng;
    Memory& m = *c.mem;
    const uint32_t p = PlantProp(c);
    const uint32_t S = c.scratch;
    Dir(rng, m, S + kN);
    if (Chance(rng, 3)) {
        int16_t h[3];
        GetVec3S16(m, p + 450, h);
        if (Chance(rng, 2)) for (int16_t& x : h) x = static_cast<int16_t>(-x);
        PutVec3S16(m, S + kN, h);
    }
    const uint32_t k = rng.U32() % 8u;
    for (uint32_t j = 0; j < 3; ++j) PutS32(m, S + kPoint + 4 * j, GetS32(m, p + 196 + 12 * k + 4 * j) + Units(rng, 1) / 2);
    const uint32_t mode = Pick(rng, {0u, 1u, 1u, 1u, 0xFFFFFFFFu, rng.U32() % 4u});
    a.a[0] = p;
    a.a[1] = Chance(rng, 4) ? 0u : S + kPoint;
    a.a[2] = S + kN;
    a.a[3] = mode;
}
uint32_t NativeTopple(NativeEnv& env, const Args& a) {
    EnRun run(env);
    co::CloneCallees cc(env, run);
    const cr::Tables tab(env.clone->mem, env.gp);
    uint32_t v = 0;
    const bool ok = rr::sim::PropTopple(run.g, a.a[0], a.a[1], a.a[2], a.a[3], env.sp, tab.t, cc, v);
    return crs::Fin(env, run, ok, v, "PropTopple");
}

// ---------------------------------------------------------------- PropKick, 0x800B2F94
void SetupKick(CaseContext& c, Args& a) {
    Rng& rng = *c.rng;
    Memory& m = *c.mem;
    const uint32_t p = PlantProp(c);
    const uint32_t bike = co::Pool0(m, Pick(rng, {0u, 6u, rng.U32() % 18u}));
    m.PokeWord(bike + 480, Pick(rng, {0u, 0x50000u, 0x50001u, 0x9FFFFu, 0x1E0000u, rng.U32() % 0x400000u}));
    Dir(rng, m, c.scratch + kN);
    a.a[0] = p;
    a.a[1] = bike;
    a.a[2] = c.scratch + kN;
}
uint32_t NativeKick(NativeEnv& env, const Args& a) {
    EnRun run(env);
    co::CloneCallees cc(env, run);
    const cr::Tables tab(env.clone->mem, env.gp);
    uint32_t v = 0;
    const bool ok = rr::sim::PropKick(run.g, a.a[0], a.a[1], a.a[2], env.sp, tab.t, cc, v);
    return crs::Fin(env, run, ok, v, "PropKick");
}

// ---------------------------------------------------------------- PropKnock, 0x800B3838
// The three rows of a live bike (heading +0x1C2, side +0x32E, up +0x20A: what ImpactSolve hands it) or
// random unit vectors; the speed at sp+16 around 1.0059 and up to 32.0.
void SetupKnock(CaseContext& c, Args& a) {
    Rng& rng = *c.rng;
    Memory& m = *c.mem;
    const uint32_t p = PlantProp(c);
    const uint32_t S = c.scratch;
    const uint32_t bike = co::Pool0(m, Pick(rng, {0u, 6u, rng.U32() % 18u}));
    if (Chance(rng, 2)) {
        std::vector<uint8_t> b(6);
        m.ReadBlock(bike + 450, b.data(), 6);
        m.WriteBlock(S + kDirF, b.data(), 6);
        m.ReadBlock(bike + 814, b.data(), 6);
        m.WriteBlock(S + kDirR, b.data(), 6);
        m.ReadBlock(bike + 522, b.data(), 6);
        m.WriteBlock(S + kDirU, b.data(), 6);
    } else {
        Dir(rng, m, S + kDirF);
        Dir(rng, m, S + kDirR);
        Dir(rng, m, S + kDirU);
    }
    a.a[0] = p;
    a.a[1] = S + kDirF;
    a.a[2] = S + kDirR;
    a.a[3] = S + kDirU;
    a.a4 = Pick(rng, {0u, 0x1017Eu, 0x10000u, 0x80000u, rng.U32() % 0x200000u});
}
uint32_t NativeKnock(NativeEnv& env, const Args& a) {
    EnRun run(env);
    const cr::Tables tab(env.clone->mem, env.gp);
    const bool ok = rr::sim::PropKnock(run.g, a.a[0], a.a[1], a.a[2], a.a[3], static_cast<int32_t>(a.a4), env.sp, tab.t);
    return crs::Fin(env, run, ok, 0, "PropKnock");
}

// ---------------------------------------------------------------- TrafficVsProp, 0x800B2D88
// A live car (creact::TrafficCar) and the prop moved into, beside or away from it; the prop's camera
// band cache stirred (in / out / unset).
void SetupTvp(CaseContext& c, Args& a) {
    Rng& rng = *c.rng;
    Memory& m = *c.mem;
    const uint32_t p = PlantProp(c);
    const uint32_t car = creact::TrafficCar(m, rng);
    if (!Chance(rng, 5)) co::MoveNear(m, p, car, Units(rng, 2), Chance(rng, 3) ? Units(rng, 1) / 2 : 0, Units(rng, 2));
    if (Chance(rng, 3)) m.PokeWord(car + 480, rng.U32() % 0x300000u);
    a.a[0] = car;
    a.a[1] = p;
}
uint32_t NativeTvp(NativeEnv& env, const Args& a) {
    EnRun run(env);
    co::CloneCallees cc(env, run);
    const cr::Tables tab(env.clone->mem, env.gp);
    const bool ok = rr::sim::TrafficVsProp(run.g, a.a[0], a.a[1], env.sp, tab.t, cc);
    return co::Refuse(env, run, ok, "TrafficVsProp");
}


// ---------------------------------------------------------------- the prop animation pass and its leaves
// QuatNormalize / QuatMatrixT on a random quaternion (components +-4096 or +-16384: the game's 1.14 unit
// quaternions and what a slerp leaves), zero now and then.
void PlantQuat(Rng& rng, Memory& m, uint32_t a) {
    const uint32_t sh = Pick(rng, {1u, 3u, 3u, 5u});
    for (uint32_t k = 0; k < 4; ++k) WriteU16Bench(m, a + 2 * k, static_cast<uint16_t>(Chance(rng, 12) ? 0 : rng.S16() >> sh));
}
void SetupQuatNorm(CaseContext& c, Args& a) {
    PlantQuat(*c.rng, *c.mem, c.scratch + kModel);
    a.a[0] = c.scratch + kModel;
}
uint32_t NativeQuatNorm(NativeEnv& env, const Args& a) {
    EnRun run(env);
    const cr::Tables tab(env.clone->mem, env.gp);
    rr::sim::QuatNormalize(run.g, a.a[0], env.sp, tab.t);
    return crs::Fin(env, run, true, 0, "QuatNormalize");
}
void SetupQuatMat(CaseContext& c, Args& a) {
    PlantQuat(*c.rng, *c.mem, c.scratch + kModel);
    a.a[0] = c.scratch + kModel;
    a.a[1] = c.scratch + kDirF;
}
uint32_t NativeQuatMat(NativeEnv& env, const Args& a) {
    EnRun run(env);
    const bool ok = rr::sim::QuatMatrixT(run.g, a.a[0], a.a[1], env.sp);
    return crs::Fin(env, run, ok, 0, "QuatMatrixT");
}
// The prop's frames: +0x204 / +0x20A / +0x210 and the rows made unit vectors (a live bike's, or random),
// flags bit 2, class +0xB4 = 2 now and then, the old row +0x1BE of both signs.
void StirFrames(Rng& rng, Memory& m, uint32_t p) {
    const uint32_t bike = co::Pool0(m, Pick(rng, {0u, 6u, rng.U32() % 18u}));
    if (Chance(rng, 2)) {
        std::vector<uint8_t> b(18);
        m.ReadBlock(bike + 516, b.data(), 18);
        m.WriteBlock(p + 516, b.data(), 18);
        m.ReadBlock(bike + 432, b.data(), 18);
        m.WriteBlock(p + 432, b.data(), 18);
    } else {
        for (uint32_t k = 0; k < 3; ++k) Dir(rng, m, p + 516 + 6 * k);
        for (uint32_t k = 0; k < 3; ++k) Dir(rng, m, p + 432 + 6 * k);
    }
    if (Chance(rng, 3)) PutS32(m, p + 180, 2);
}
void SetupFrameRows(CaseContext& c, Args& a) {
    Rng& rng = *c.rng;
    Memory& m = *c.mem;
    const uint32_t p = PlantProp(c);
    StirFrames(rng, m, p);
    if (!Chance(rng, 3)) m.PokeWord(p + 592, m.PeekWord(p + 592) | 4u);
    a.a[0] = p;
}
uint32_t NativeFrameRows(NativeEnv& env, const Args& a) {
    EnRun run(env);
    const cr::Tables tab(env.clone->mem, env.gp);
    rr::sim::PropFrameRows(run.g, a.a[0], env.sp, tab.t);
    return crs::Fin(env, run, true, 0, "PropFrameRows");
}
// PropAnimPass: worldrows' props on the bike's road (1..4, pool-4 slots from 0), each in one of the
// states the reactions leave: pushed (bit 8), knocked (bit 0), tumbling (bit 1, with bits 3 / 5 / 6 and
// the spin rates), toppling (bit 9 with the target quaternion, bit 10), sliding (speed >= 0.2, bit 7),
// at rest; dt the frame's.
void SetupAnim(CaseContext& c, Args& a) {
    Rng& rng = *c.rng;
    Memory& m = *c.mem;
    worldrows::PlantProps(rng, m);
    const uint32_t base = m.PeekWord(rr::sim::kWpPool4Ctrl + 12u);
    const uint32_t k = m.PeekWord(rr::sim::kWpPool4Ctrl);
    for (uint32_t i = 0; i < k; ++i) {
        const uint32_t p = base + 596u * i;
        StirFrames(rng, m, p);
        Dir(rng, m, p + 450);
        for (uint32_t j = 0; j < 3; ++j) Dir(rng, m, p + 560 + 6 * j);
        if (Chance(rng, 3)) {                                              // a slerp pair of quaternions
            PlantQuat(rng, m, p + 560);
            PlantQuat(rng, m, p + 572);
        }
        const uint32_t state = rng.U32() % 7u;
        uint32_t f = rng.U32() & 0x84u;
        if (state == 0) f |= 0x100u;
        if (state == 1) f |= 1u;
        if (state == 2) f |= 2u | (rng.U32() & 0x68u);
        if (state == 3) f |= 0x200u | (rng.U32() & 0x440u);
        if (state == 4) f |= rng.U32() & 0x7FFu;
        m.PokeWord(p + 592, f);
        m.PokeWord(p + 480, Pick(rng, {0u, 13107u, 13108u, 0x40000u, rng.U32() % 0x200000u}));
        for (uint32_t j = 0; j < 3; ++j) PutS32(m, p + 456 + 4 * j, static_cast<int32_t>(rng.U32() % 0x100000u) - 0x80000);
        PutS32(m, p + 484, static_cast<int32_t>(Pick(rng, {0u, 0x1000u, 0x10000u, 0x30000u, rng.U32() % 0x40000u})));
        PutS32(m, p + 488, static_cast<int32_t>(Pick(rng, {0u, 0x8000u, 0x10000u, 0x60000u, 0xFFFA0000u, rng.U32() % 0x100000u - 0x80000u})));
        for (uint32_t j = 540; j <= 556; j += 4)
            PutS32(m, p + j, Chance(rng, 3) ? 0 : static_cast<int32_t>(rng.U32() % 0xC0000u) - 0x60000);
        if (Chance(rng, 3)) PutS32(m, p + 556, static_cast<int32_t>(Pick(rng, {0xFFF0u, 0x10000u, 22875u, 22876u, 0x6487Du, 0xFFF9B782u})));
        WriteU16Bench(m, p + 578, static_cast<uint16_t>(Pick(rng, {0u, 0u, 5u, 0x8000u, rng.U32() & 0xFFFFu})));
        if (Chance(rng, 3)) WriteU16Bench(m, p + 524, static_cast<uint16_t>(Pick(rng, {3547u, 3548u, 4096u})));
    }
    a.a[0] = Pick(rng, {0x884u, 0x884u, 0x442u, 0x1100u, rng.U32() % 0x2000u});
}
uint32_t NativeAnim(NativeEnv& env, const Args& a) {
    return worldrows::RunWorld(env, [&](rr::sim::GuestRam& g, const rr::sim::BikeTables& t, rr::sim::RecoverCallees& o,
                                        uint32_t&) { return rr::sim::PropAnimPass(g, static_cast<int32_t>(a.a[0]), env.sp, t, o); });
}

} // namespace solidrows

void AddSolidRows(std::vector<Row>& rows) {
    using namespace solidrows;
    using namespace rr::sim::solid;
    crs::Ex(rows, "solid_lean_pole_test", "u32 0x800ADC74(E *e, u8 *pole, s32 radius, s32 push[3], [s32 *len])  [RASHCDG]",
            kLeanPoleTest, SetupLean, NativeLean, true);
    rows.back().maxSteps = 20'000'000;
    crs::Ex(rows, "solid_pole_resolve", "void 0x800AE794(E *e, u8 *shape)  [RASHCDG, LeanPoleTest native]",
            rr::sim::coll::kPoleResolve, SetupPoleNative, NativePoleNative, false,
            {OracleCallee{rr::sim::coll::kPoleReact, 4, 2}});
    rows.back().maxSteps = 20'000'000;
    crs::Ex(rows, "solid_prop_up_rows", "void 0x800A3CBC(Prop *p)  [RASHCDG]", kPropUpRows, SetupUpRows, NativeUpRows,
            false);
    crs::Ex(rows, "solid_prop_topple", "u32 0x800B3344(Prop *p, s32 point[3], s16 n[3], s32 mode)  [RASHCDG]",
            kPropTopple, SetupTopple, NativeTopple, true);
    rows.back().maxSteps = 20'000'000;
    crs::Ex(rows, "solid_prop_kick", "u32 0x800B2F94(Prop *p, E *bike, s16 dir[3])  [RASHCDG]", kPropKick, SetupKick,
            NativeKick, true);
    rows.back().maxSteps = 20'000'000;
    crs::Ex(rows, "solid_prop_knock", "void 0x800B3838(Prop *p, s16 f[3], s16 r[3], s16 u[3], [s32 speed])  [RASHCDG]",
            kPropKnock, SetupKnock, NativeKnock, false);
    crs::Ex(rows, "solid_traffic_vs_prop", "void 0x800B2D88(Car *car, Prop *p)  [RASHCDG]", kTrafficVsProp, SetupTvp,
            NativeTvp, false);
    rows.back().maxSteps = 20'000'000;
    crs::Ex(rows, "solid_quat_normalize", "void 0x80071A28(s16 q[4])  [RASHCDG]", kQuatNormalize, SetupQuatNorm,
            NativeQuatNorm, false);
    crs::Ex(rows, "solid_quat_matrix_t", "void 0x8007198C(s16 q[4], s16 out[9])  [RASHCDG, with 0x800714A0]",
            kQuatMatrixT, SetupQuatMat, NativeQuatMat, false);
    crs::Ex(rows, "solid_prop_frame_rows", "void 0x800A40D4(Prop *p)  [RASHCDG]", kPropFrameRows, SetupFrameRows,
            NativeFrameRows, false);
    rows.push_back(worldrows::WRow("solid_prop_anim_pass", "void 0x800A2A64(s32 dt)  [RASHCDG, the prop animation pass]",
                                   kPropAnimPass, SetupAnim, NativeAnim, false, true));
}

// The extent [entry, end) of each function these rows accept, for `--coverage` (our own disassembly of
// RASHCDG: the next function's entry).
const CoverageRange* SolidCoverage(const char* rowName) {
    struct Named {
        const char* name;
        CoverageRange range;
    };
    static const Named kRanges[] = {
        {"solid_lean_pole_test", {0x800ADC74, 0x800AE794}}, {"solid_pole_resolve", {0x800AE794, 0x800AF0A0}},
        {"solid_prop_up_rows", {0x800A3CBC, 0x800A3ECC}},   {"solid_prop_topple", {0x800B3344, 0x800B3838}},
        {"solid_prop_kick", {0x800B2F94, 0x800B3344}},      {"solid_prop_knock", {0x800B3838, 0x800B3AD0}},
        {"solid_traffic_vs_prop", {0x800B2D88, 0x800B2E64}}, {"solid_quat_normalize", {0x80071A28, 0x80071BCC}},
        {"solid_quat_matrix_t", {0x8007198C, 0x80071A28}},  {"solid_prop_frame_rows", {0x800A40D4, 0x800A41EC}},
        {"solid_prop_anim_pass", {0x800A2A64, 0x800A3A84}},
    };
    for (const Named& n : kRanges)
        if (std::strcmp(n.name, rowName) == 0) return &n.range;
    return nullptr;
}
