// Bench rows for src/game/sim/hit_speed.{h,cpp}. Included by
// rows_coll.inc.
//
// The four ports call nothing unported (every callee is ported and runs inline, and HitOutcome / ImpactTurn
// call the native HitSpeed / TakePartnerHeading, so their rows prove the composition), hence no oracle
// callees: each row compares the whole RAM outside the stack window after the original and after the port.
//
// Every case starts from THIS machine's live bikes (rr-race: pool 0 slots 0..17, their riders, stat blocks
// and passengers), the traffic car 0x800CF660 (pool 3) and, for the pool-5 / 6 / 8 partners, a synthetic
// shape record in the scratch (HitSpeed reads only its handle, the pool-6 half extents +132..+140 and, for
// pool 3, +480); the fields each function reads are then drawn so that every arm runs: flagsC (+0x238) bits
// 0..5 (the class), 8 (air), 9/10 (crashed / airborne), 4, 25 (the latch); +0x234 bit 14 (the exchange);
// +0x230 bits 8/9, 13, 27, 29; the speeds +0x1E0 / +0x240 / +0x35C at and around 1.0, 2.0, 4.47, 5.0, 10.0;
// +0x268 around 17158, +0x2A4 around +-34314, +0x16C of both signs; the heading rows; the partner's flags;
// the speed hand-over arguments (the angle around +-57190 and +-pi/2, the limit 55 / 65 degrees). DATA EDIT:
// the word the class-2 car arm reads at car + 0x28C (the next traffic slot's +0x8C, 0x800CF8EC) is set to 0
// or left, so both of its arms run.
//
// NAMED BOUNDS: every direction (the rows +0x1B0 .. +0x210, +0x1C2, +0x32E, +0x360, the normal, the source
// heading) is at most unit length and the speeds within +-64.0, as in every capture - a longer direction
// makes Normalize's trapping `add` raise the console's overflow exception (Bounce and the re-projection
// normalise); the rider flag +0x23C bit 4 (a passenger on board) is set only when +0x358 names a live bike,
// because the original follows +0x358 -> +0x354 with no null test and on this machine a null +0x358 leads
// into the kernel's low RAM and on to an address outside RAM.
//
// RRJB_SPEEDDBG=1 prints, at exit, how often each arm ran (read from the state before and after a case).

namespace csp {

using co::Chance;
using co::Pick;
using co::Pool0;

constexpr uint32_t kHitSpeed = 0x80080D1C, kTakeHeading = 0x80080B10, kHitOutcome = 0x80083928;
constexpr uint32_t kImpactTurn = 0x80083F30;
constexpr uint32_t kCar = 0x800CF660;       // traffic slot 0 (pool 3), 512 bytes
constexpr uint32_t kShape = 3000;           // scratch offset of the synthetic partner shape
constexpr uint32_t kDirs = 3600;            // scratch offset of the free-standing direction vectors

// The arms each native wrapper saw, printed at exit under RRJB_SPEEDDBG.
struct ArmCounts {
    const char* name;
    std::vector<std::pair<std::string, long>> n;
    void Add(const std::string& k) {
        for (auto& p : n)
            if (p.first == k) { ++p.second; return; }
        n.emplace_back(k, 1);
    }
    ~ArmCounts() {
        if (std::getenv("RRJB_SPEEDDBG") == nullptr || n.empty()) return;
        std::printf("%s arms:", name);
        for (auto& p : n) std::printf(" %s=%ld", p.first.c_str(), p.second);
        std::printf("\n");
    }
};
ArmCounts gHitArms{"hit_speed", {}}, gTakeArms{"take_partner_heading", {}}, gOutArms{"hit_outcome", {}},
    gTurnArms{"impact_turn", {}};

bool LiveRider(const Memory& m, uint32_t b) {
    const uint32_t r = m.PeekWord(b + 852);
    return r >= 0x80000000u && r < 0x80200000u;
}
uint32_t PickBike(CaseContext& c, uint32_t avoid = 0) {
    Rng& rng = *c.rng;
    Memory& m = *c.mem;
    for (int k = 0; k < 16; ++k) {
        const uint32_t b = Pool0(m, Pick(rng, {0u, 0u, 6u, 6u, rng.U32() % 18u, rng.U32() % 18u}));
        if (b != avoid && LiveRider(m, b)) return b;
    }
    for (uint32_t s = 0; s < 18; ++s)
        if (Pool0(m, s) != avoid && LiveRider(m, Pool0(m, s))) return Pool0(m, s);
    return Pool0(m, 0);
}
void UnitRow(Rng& rng, Memory& m, uint32_t a) {
    int16_t v[3];
    co::Dir(rng, v);
    PutVec3S16(m, a, v);
}
uint32_t Speed(Rng& rng) {
    return Pick(rng, {0u, 0x10000u, 0xFFFFu, 0x1FFFFu, 0x20000u, 0x4786Bu, 0x4786Cu, 0x50000u, 0xA0000u, 0xA0001u,
                      rng.U32() % 0x400000u, rng.U32() % 0x400000u, rng.U32() % 0x1400000u >> 2,
                      static_cast<uint32_t>(rng.S32() >> 10)});
}

// A live bike with its heading, speeds, rates and flags drawn (see the head of this file).
void PerturbBike(CaseContext& c, uint32_t b) {
    Rng& rng = *c.rng;
    Memory& m = *c.mem;
    const bool derived = c.sample != nullptr && c.sample->entity.size() == 1096;
    if (derived && Chance(rng, 2)) {
        m.WriteBlock(b + 0x1B0, c.sample->entity.data() + 0x1B0, 0x216 - 0x1B0);
        PutS32(m, b + 0x1E0, c.sample->speed);
    }
    for (uint32_t off : {0x1B0u, 0x1B6u, 0x1BCu, 0x204u, 0x20Au, 0x210u, 0x1C2u, 0x32Eu, 0x360u})
        if (!derived || Chance(rng, 2)) UnitRow(rng, m, b + off);
    if (Chance(rng, 3)) for (uint32_t off : {0x1C2u, 0x32Eu}) PutVec3S16(m, b + off, std::array<int16_t, 3>{0, 0, 0}.data());
    for (uint32_t off : {0x1E0u, 0x240u, 0x35Cu})
        if (!derived || Chance(rng, 2)) m.PokeWord(b + off, Speed(rng));
    for (uint32_t k = 0; k < 3; ++k) PutS32(m, b + 456 + 4 * k, rng.S32() >> (9 + rng.Next() % 10u));
    PutS32(m, b + 488, rng.S32() >> (8 + rng.Next() % 10u));                   // +0x1E8 yaw rate
    PutS32(m, b + 636, rng.S32() >> (12 + rng.Next() % 6u));                   // +0x27C steering
    m.PokeWord(b + 616, Pick(rng, {0u, 0u, 17157u, 17158u, static_cast<uint32_t>(rng.S32() >> 13)}));
    m.PokeWord(b + 676, Pick(rng, {34314u, 34315u, static_cast<uint32_t>(-34314), static_cast<uint32_t>(-34315),
                                   static_cast<uint32_t>(rng.S32() >> 13)}));
    m.PokeWord(b + 364, Pick(rng, {2u, static_cast<uint32_t>(-2), rng.U32()}));
    uint32_t fc = derived && Chance(rng, 2) ? GetU32V(c.sample->entity, 0x238) : m.PeekWord(b + 568);
    fc &= ~0x0218077Fu;
    if (Chance(rng, 5)) fc |= 0x100u;                                           // air (HitSpeed's reflect)
    if (Chance(rng, 5)) fc |= 0x200u;                                           // crashed
    if (Chance(rng, 5)) fc |= 0x400u;                                           // airborne
    if (Chance(rng, 2)) fc |= Pick(rng, {1u, 2u, 4u, 8u, 5u, 9u, 3u, 0xCu, 0x20u, 0x10u});
    if (Chance(rng, 4)) fc |= 0x10u;
    if (Chance(rng, 3)) fc |= 0x02000000u;                                      // latched this pass
    if (Chance(rng, 6)) fc |= 0x80000u;                                         // Spin's "no spin"
    m.PokeWord(b + 568, fc);
    uint32_t fb = m.PeekWord(b + 564);
    fb = Chance(rng, 2) ? (fb | 0x4000u) : (fb & ~0x4000u);
    m.PokeWord(b + 564, fb);
    uint32_t fa = m.PeekWord(b + 560);
    fa = (fa & ~0x28002300u) | (rng.U32() & 0x28002300u);
    m.PokeWord(b + 560, fa);
    // the rider's state, and the passenger (bound: bit 4 of the rider's +0x23C only with a live +0x358)
    const uint32_t r = m.PeekWord(b + 852);
    m.PokeWord(r + 604, Pick(rng, {0u, 1u, 1u, 2u, 3u}));
    uint32_t p = 0;
    if (Chance(rng, 5)) {
        p = Pool0(m, 17);
        if (p == b || !LiveRider(m, p)) p = 0;
    }
    m.PokeWord(b + 856, p);
    uint8_t rb = m.PeekByte(r + 572);
    rb = static_cast<uint8_t>((p != 0 && Chance(rng, 2)) ? (rb | 0x10u) : (rb & ~0x10u));
    m.WriteBlock(r + 572, &rb, 1);
    if (p != 0) m.PokeWord(m.PeekWord(p + 852) + 604, Pick(rng, {0u, 1u, 2u}));
}

// A partner shape: another live bike's +0xAC, the car's, the bike's own (the collision pass's ImpactTurn
// call), or the synthetic record in the scratch with a pool 5 / 6 / 8 (or 3) handle.
uint32_t PickShape(CaseContext& c, uint32_t e) {
    Rng& rng = *c.rng;
    Memory& m = *c.mem;
    const uint32_t kind = static_cast<uint32_t>(rng.Next() % 7u);
    if (kind <= 1) {
        const uint32_t o = PickBike(c, e);
        uint32_t fc = m.PeekWord(o + 568);
        fc = Chance(rng, 2) ? (fc | 1u) : (fc & ~1u);
        m.PokeWord(o + 568, fc);
        uint32_t fa = m.PeekWord(o + 560);
        fa = Chance(rng, 2) ? (fa | 0x2000u) : (fa & ~0x2000u);
        m.PokeWord(o + 560, fa);
        PutS32(m, o + 636, rng.S32() >> (12 + rng.Next() % 6u));
        return o + 172;
    }
    if (kind == 2 || kind == 3) {
        if (Chance(rng, 2)) m.PokeWord(kCar + 652, Chance(rng, 2) ? 0u : rng.U32());   // car + 0x28C
        return kCar + 172;
    }
    if (kind == 4) return e + 172;
    const uint32_t S = c.scratch + kShape;
    const uint32_t pool = Pick(rng, {5u, 6u, 6u, 8u, 3u, rng.U32() % 64u});
    WriteU16Bench(m, S, static_cast<uint16_t>((pool << 5) | (rng.U32() % 32u)));
    for (uint32_t off : {132u, 136u, 140u})
        m.PokeWord(S + off, Pick(rng, {0x10000u, 0x10001u, 0x20000u, rng.U32() % 0x40000u}));
    m.PokeWord(S + 480, Chance(rng, 2) ? 0u : rng.U32());
    return S;
}

// A free-standing unit direction in the scratch (slot k), or the given field.
uint32_t DirIn(CaseContext& c, uint32_t k) {
    const uint32_t a = c.scratch + kDirs + 8u * k;
    UnitRow(*c.rng, *c.mem, a);
    return a;
}

// ---------------------------------------------------------------- HitSpeed, 0x80080D1C
//
// e a live bike; dir its +0x360 or a scratch vector; pSpeed its +0x35C or a scratch word; shape as PickShape;
// nrm its +0x334 or a scratch unit vector; a5 the closing speed x 0.0559 (around -1.0 and 0), a6 the hit
// angle (+-57190, +-pi/2, anything between), a7 55 / 65 degrees, pi/2 (the GLANCE divisor 0) or anything
// up to pi/2, a8 a speed, a9 one degree, a10 0 or 1. The stack arguments a9 / a10 (sp+36 / +40) are written
// by the setup on both machines, as the bench writes a4 .. a8.
void SetupHitSpeed(CaseContext& c, Args& a) {
    Rng& rng = *c.rng;
    Memory& m = *c.mem;
    const uint32_t e = PickBike(c);
    PerturbBike(c, e);
    const uint32_t shape = PickShape(c, e);
    if (Chance(rng, 6)) m.PokeWord(e + 828, shape);
    const uint32_t dir = Chance(rng, 4) ? DirIn(c, 0) : e + 864;
    uint32_t pSpeed = e + 860;
    if (Chance(rng, 4)) {
        pSpeed = c.scratch + kDirs + 64;
        m.PokeWord(pSpeed, Speed(rng));
    }
    const uint32_t nrm = Chance(rng, 3) ? e + 820 : DirIn(c, 1);
    if (nrm == e + 820) UnitRow(rng, m, e + 820);
    a.a[0] = e;
    a.a[1] = dir;
    a.a[2] = pSpeed;
    a.a[3] = shape;
    a.a4 = nrm;
    a.more[0] = Pick(rng, {0xFFFF0000u, 0xFFFEFFFFu, 0u, 1u, static_cast<uint32_t>(rng.S32() >> (13 + rng.Next() % 6u))});
    a.more[1] = Pick(rng, {57189u, 57190u, static_cast<uint32_t>(-57190), static_cast<uint32_t>(-57191), 0x1921Fu,
                           static_cast<uint32_t>(-0x1921F), rng.U32() % 0x3243Eu - 0x1921Fu, rng.U32() % 0x3243Eu - 0x1921Fu});
    a.more[2] = Pick(rng, {0xF5BEu, 0x1226Cu, 0xF5BEu, 0x1226Cu, 0x1921Fu, rng.U32() % 0x1921Fu});
    a.more[3] = Speed(rng);
    m.PokeWord(c.sp + 36, Chance(rng, 5) ? rng.U32() % 0x4000u : 1143u);
    m.PokeWord(c.sp + 40, Pick(rng, {0u, 0u, 1u}));
}
uint32_t NativeHitSpeed(NativeEnv& env, const Args& a) {
    EnRun run(env);
    const cr::Tables tab(env.clone->mem, env.gp);
    rr::sim::GuestRam& g = run.g;
    const uint32_t e = a.a[0];
    const uint32_t f0 = g.U32(e + 568), b0 = g.U32(e + 564);
    const int32_t a9 = g.S32(env.sp + 36), a10 = g.S32(env.sp + 40);
    uint32_t v0 = 0;
    const bool ok = rr::sim::HitSpeed(g, e, a.a[1], a.a[2], a.a[3], a.a4, static_cast<int32_t>(a.more[0]),
                                      static_cast<int32_t>(a.more[1]), static_cast<int32_t>(a.more[2]),
                                      static_cast<int32_t>(a.more[3]), a9, a10, env.sp, tab.t, v0);
    if (ok) {
        const uint32_t f1 = g.U32(e + 568);
        std::string k;
        if (f0 & 0x100u) k = "air";
        else if (v0 == 0) k = "slow";
        else if ((f1 & 0x200u) && !(f0 & 0x600u)) k = "crash";
        else if ((b0 & 0x4000u) && g.U32(e + 828) != a.a[3]) k = "exch";
        else if (f0 & 1u) k = "bump";
        else k = (f1 & 8u) ? "glance8" : "glance";
        if (f0 & 0x600u) k += "_s8";
        gHitArms.Add(k);
    }
    co::Refuse(env, run, ok, "HitSpeed");
    return v0;
}

// ---------------------------------------------------------------- TakePartnerHeading, 0x80080B10
//
// e a live bike; dir its +0x360 or a scratch vector; pSpeed its +0x35C or a scratch word; src another bike's
// heading +0x1C2 or a scratch unit vector; speed around 2.0 or any speed.
void SetupTakeHeading(CaseContext& c, Args& a) {
    Rng& rng = *c.rng;
    Memory& m = *c.mem;
    const uint32_t e = PickBike(c);
    PerturbBike(c, e);
    const uint32_t o = PickBike(c, e);
    UnitRow(rng, m, o + 450);
    a.a[0] = e;
    a.a[1] = Chance(rng, 4) ? DirIn(c, 0) : e + 864;
    a.a[2] = e + 860;
    if (Chance(rng, 4)) {
        a.a[2] = c.scratch + kDirs + 64;
        m.PokeWord(a.a[2], Speed(rng));
    }
    a.a[3] = Chance(rng, 3) ? DirIn(c, 1) : o + 450;
    a.a4 = Pick(rng, {0x1FFFFu, 0x20000u, Speed(rng), Speed(rng), Speed(rng)});
}
uint32_t NativeTakeHeading(NativeEnv& env, const Args& a) {
    EnRun run(env);
    const cr::Tables tab(env.clone->mem, env.gp);
    const uint32_t f0 = run.g.U32(a.a[0] + 568);
    uint32_t v0 = 0;
    const bool ok = rr::sim::TakePartnerHeading(run.g, a.a[0], a.a[1], a.a[2], a.a[3], static_cast<int32_t>(a.a4),
                                                env.sp, tab.t, v0);
    if (ok) gTakeArms.Add(v0 == 0 ? "slow" : (f0 & 0x600u) ? "s8" : (f0 & 0x100u) ? "air" : "take");
    co::Refuse(env, run, ok, "TakePartnerHeading");
    return v0;
}

// ---------------------------------------------------------------- HitOutcome, 0x80083928
//
// e a live bike, o the car (the solver's only partner kind here) or another live bike with its speed, heading
// and mass drawn; the masses +0x13C the game's or anything (zero and negative sums run the `divu`'s corners);
// n a scratch unit vector, half the time against e's pre-contact heading (a closing hit) so that the closing
// speed reaches the class thresholds 6.71 / 11.18 / 17.88 / 35.76.
void SetupHitOutcome(CaseContext& c, Args& a) {
    Rng& rng = *c.rng;
    Memory& m = *c.mem;
    const uint32_t e = PickBike(c);
    PerturbBike(c, e);
    const uint32_t o = Chance(rng, 3) ? PickBike(c, e) : kCar;
    if (o != kCar) PerturbBike(c, o);
    else if (Chance(rng, 2)) m.PokeWord(kCar + 652, Chance(rng, 2) ? 0u : rng.U32());   // car + 0x28C
    UnitRow(rng, m, o + 450);
    m.PokeWord(o + 480, Speed(rng));
    for (uint32_t b : {e, o}) {
        if (Chance(rng, 3))
            m.PokeWord(b + 316, Pick(rng, {475u << 16, 275u << 16, 1360u << 16, 1u << 16, rng.U32() % 0x8000000u,
                                           0u, 0u - (rng.U32() % 0x8000000u)}));
    }
    if (Chance(rng, 6)) m.PokeWord(e + 828, o + 172);
    const uint32_t n = c.scratch + kDirs + 16;
    int16_t v[3];
    if (Chance(rng, 2)) {
        const uint32_t h = (m.PeekWord(e + 568) & 0x02000000u) ? e + 864 : e + 450;
        GetVec3S16(m, h, v);
        for (int16_t& x : v) x = static_cast<int16_t>(-x + (static_cast<int32_t>(rng.U32() % 1025u) - 512));
        for (int16_t& x : v) if (x > 2364 || x < -2364) x = static_cast<int16_t>(x * 3 / 4);   // stay within unit length
    } else {
        co::Dir(rng, v);
    }
    PutVec3S16(m, n, v);
    a.a[0] = e;
    a.a[1] = o;
    a.a[2] = n;
}
uint32_t NativeHitOutcome(NativeEnv& env, const Args& a) {
    EnRun run(env);
    const cr::Tables tab(env.clone->mem, env.gp);
    rr::sim::GuestRam& g = run.g;
    const uint32_t e = a.a[0];
    const uint32_t f0 = g.U32(e + 568);
    const int32_t ny = g.S16(a.a[2] + 2);
    const bool bounce = (f0 & 0x200u) || ((f0 & 0x400u) && !(ny < 2048 && ny > -2048));
    uint32_t v0 = 0;
    const bool ok = rr::sim::HitOutcome(g, e, a.a[1], a.a[2], env.sp, tab.t, v0);
    if (ok) {
        char buf[32];
        if (bounce)
            std::snprintf(buf, sizeof(buf), "bounce%d", static_cast<int32_t>(v0));
        else
            std::snprintf(buf, sizeof(buf), "c%02X_x%d_v%d", g.U32(e + 568) & 0x3Fu, (g.U32(e + 564) >> 14) & 1u,
                          static_cast<int32_t>(v0));
        gOutArms.Add(buf);
    }
    co::Refuse(env, run, ok, "HitOutcome");
    return v0;
}

// ---------------------------------------------------------------- ImpactTurn, 0x80083F30
//
// e a live bike; shape as PickShape (the bike's own +0xAC is the pass's call); n its +0x334 (the pass) or a
// scratch unit vector, half the time against the velocity; mode 0..5 (the solver's kinds and the pass's 4)
// or anything; the velocity +0x1C8 drawn up to 64.0 so that the closing speed reaches 17.88 / 35.76.
void SetupImpactTurn(CaseContext& c, Args& a) {
    Rng& rng = *c.rng;
    Memory& m = *c.mem;
    const uint32_t e = PickBike(c);
    PerturbBike(c, e);
    const uint32_t shape = PickShape(c, e);
    if (Chance(rng, 5)) m.PokeWord(e + 828, shape);
    const uint32_t n = Chance(rng, 3) ? e + 820 : c.scratch + kDirs + 16;
    int16_t v[3];
    co::Dir(rng, v);
    PutVec3S16(m, n, v);
    if (Chance(rng, 2)) {
        const int32_t s = static_cast<int32_t>(rng.U32() % 0x400000u);
        for (uint32_t k = 0; k < 3; ++k)
            PutS32(m, e + 456 + 4 * k, static_cast<int32_t>((-static_cast<int64_t>(v[k]) * 16 * s) >> 16) +
                                           (rng.S32() >> 14));
    }
    a.a[0] = e;
    a.a[1] = shape;
    a.a[2] = n;
    a.a[3] = Pick(rng, {0u, 1u, 2u, 3u, 4u, 5u, 5u, 5u, 3u, 4u, rng.U32() % 8u, rng.U32()});
}
uint32_t NativeImpactTurn(NativeEnv& env, const Args& a) {
    EnRun run(env);
    const cr::Tables tab(env.clone->mem, env.gp);
    rr::sim::GuestRam& g = run.g;
    const uint32_t e = a.a[0];
    const uint32_t f0 = g.U32(e + 568);
    const int32_t ny = g.S16(a.a[2] + 2);
    const bool bounce = (f0 & 0x200u) || ((f0 & 0x400u) && !(ny < 2048 && ny > -2048));
    uint32_t v0 = 0;
    const bool ok = rr::sim::ImpactTurn(g, e, a.a[1], a.a[2], static_cast<int32_t>(a.a[3]), env.sp, tab.t, v0);
    if (ok) {
        char buf[40];
        const int32_t mode = a.a[3] < 8u ? static_cast<int32_t>(a.a[3]) : 9;
        if (bounce)
            std::snprintf(buf, sizeof(buf), "m%d_bounce%d", mode, static_cast<int32_t>(v0));
        else
            std::snprintf(buf, sizeof(buf), "m%d_c%02X_x%d_v%d", mode, g.U32(e + 568) & 0x3Fu,
                          (g.U32(e + 564) >> 14) & 1u, static_cast<int32_t>(v0));
        gTurnArms.Add(buf);
    }
    co::Refuse(env, run, ok, "ImpactTurn");
    return v0;
}

} // namespace csp

void AddCollSpeedRows(std::vector<Row>& rows) {
    {
        rows.push_back(Row{"hit_speed",
                           "s32 0x80080D1C(e, dir, pSpeed, shape, [nrm, a5, a6, a7, a8, a9, a10])  [RASHCDG, four GTE OP]",
                           csp::kHitSpeed, csp::SetupHitSpeed, nullptr});
        rows.back().nativeEx = csp::NativeHitSpeed;
    }
    {
        rows.push_back(Row{"take_partner_heading", "s32 0x80080B10(e, dir, pSpeed, src, [speed])  [RASHCDG, one GTE OP]",
                           csp::kTakeHeading, csp::SetupTakeHeading, nullptr});
        rows.back().nativeEx = csp::NativeTakeHeading;
    }
    {
        rows.push_back(Row{"hit_outcome", "s32 0x80083928(Bike *e, Entity *o, const s16 n[3])  [RASHCDG]",
                           csp::kHitOutcome, csp::SetupHitOutcome, nullptr});
        rows.back().nativeEx = csp::NativeHitOutcome;
    }
    {
        rows.push_back(Row{"impact_turn", "s32 0x80083F30(Bike *e, Shape *shape, s16 n[3], s32 mode)  [RASHCDG]",
                           csp::kImpactTurn, csp::SetupImpactTurn, nullptr});
        rows.back().nativeEx = csp::NativeImpactTurn;
    }
}
