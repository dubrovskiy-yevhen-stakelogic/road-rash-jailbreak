// Bench rows for src/game/sim/partners.{h,cpp}. Included
// by verify_physics.cpp inside its anonymous namespace, after rows_solid.inc (co::, crs::, creact::,
// solidrows:: helpers).
//
// The riders are this machine's own (pool 1, *(0x8005B3A4) + 628 k) or its pedestrians (pool 2, stride
// 572), the cars its live traffic records (creact::TrafficCar), moved into, beside or away from each
// other; the shapes pool-6 volumes (co::PlantVolumes) or a prop of our own making (solidrows::PlantProp).
// PedHit 0x800A9868 runs on the oracle (its own row is rows_feel's); every other callee is ported and
// runs natively. NAMED BOUNDS: headings are unit vectors (the game's); a rider's state +0x220 is left as
// the machine has it (PedHit's stance layer traps on another animation bank, as in bike_vs_rider).

namespace partnerrows {

using co::Chance;
using co::Pick;

int32_t Units(Rng& rng, int32_t units) {
    return static_cast<int32_t>(rng.U32() % (2u * 65536u * static_cast<uint32_t>(units))) - 65536 * units;
}

// A rider (pool 1) or a pedestrian (pool 2) of this machine.
uint32_t Walker(Memory& m, Rng& rng) {
    if (Chance(rng, 3)) return m.PeekWord(rr::sim::kPoolTableAddr + 32u) + 572u * (rng.U32() % 3u);
    return m.PeekWord(0x8005B3A4u) + 628u * Pick(rng, {6u, 0u, rng.U32() % 18u});
}
// The walker's state the resolvers read: the camera-band cache +0x140 bits 2/3, PedHit's +0x228 bit 30,
// the speed +0x1E0 over the classes and both signs, the heading words.
void StirWalker(Memory& m, Rng& rng, uint32_t e) {
    const uint16_t st = static_cast<uint16_t>(m.PeekWord(e + 320) & 0xFFFFu);
    WriteU16Bench(m, e + 320, static_cast<uint16_t>((st & ~0xCu) | Pick(rng, {0xCu, 0x8u, 0u, 0u})));
    if (!Chance(rng, 3)) m.PokeWord(e + 552, m.PeekWord(e + 552) | 0x40000000u);
    if (Chance(rng, 2)) {
        const int32_t s = static_cast<int32_t>(Pick(rng, {0u, 0x1017Eu, 0x1017Fu, 0x1FFFFu, 0x20000u, 0x60000u, 0x90000u, rng.U32() % 0x200000u}));
        PutS32(m, e + 480, Chance(rng, 3) ? -s : s);
    }
    if (Chance(rng, 3)) {
        int16_t d[3];
        co::Dir(rng, d);
        PutVec3S16(m, e + 450, d);
        PutS32(m, e + 296, static_cast<int32_t>(rng.U32() % 0x20000u) - 0x10000);
        PutS32(m, e + 300, static_cast<int32_t>(rng.U32() % 0x20000u) - 0x10000);
    }
}

// ---------------------------------------------------------------- RiderVsTraffic, 0x800B2844
void SetupRt(CaseContext& c, Args& a) {
    Rng& rng = *c.rng;
    Memory& m = *c.mem;
    const uint32_t e = Walker(m, rng);
    const uint32_t car = creact::TrafficCar(m, rng);
    if (!Chance(rng, 5)) co::MoveNear(m, e, car, Units(rng, 2), Chance(rng, 3) ? Units(rng, 1) / 2 : 0, Units(rng, 2));
    StirWalker(m, rng, e);
    if (Chance(rng, 2)) m.PokeWord(car + 480, Pick(rng, {0u, 0x1017Eu, 0x1017Fu, rng.U32() % 0x300000u}));
    if (Chance(rng, 3)) {
        const uint8_t f = static_cast<uint8_t>(rng.U32() & 0x90u);
        m.WriteBlock(car + 509, &f, 1);
    }
    m.PokeWord(c.gp + kRandSeedGpOffset, rng.U32());
    a.a[0] = e;
    a.a[1] = car;
}
uint32_t NativeRt(NativeEnv& env, const Args& a) {
    EnRun run(env);
    co::CloneCallees cc(env, run);
    const cr::Tables tab(env.clone->mem, env.gp);
    const bool ok = rr::sim::RiderVsTraffic(run.g, a.a[0], a.a[1], env.sp, tab.t, cc);
    return co::Refuse(env, run, ok, "RiderVsTraffic");
}

// ---------------------------------------------------------------- RiderVsShape, 0x800B2B00
void SetupRs(CaseContext& c, Args& a) {
    Rng& rng = *c.rng;
    Memory& m = *c.mem;
    uint32_t shape;
    const uint32_t e = Walker(m, rng);
    if (Chance(rng, 3)) {
        const uint32_t p = solidrows::PlantProp(c);
        co::MoveNear(m, p, e, Units(rng, 2), Chance(rng, 3) ? Units(rng, 1) / 2 : 0, Units(rng, 2));
        shape = p + 172;
    } else {
        const uint32_t n6 = 1u + rng.U32() % 3u;
        co::PlantVolumes(c, e, n6);
        const uint32_t r = m.PeekWord(rr::sim::kPoolTableAddr + 16u * 6u) + 280u * (rng.U32() % n6);
        if (Chance(rng, 2)) {                                                // the volume moved onto the walker
            const int32_t dx = GetS32(m, e + 184) + Units(rng, 1) - GetS32(m, r + 12);
            const int32_t dz = GetS32(m, e + 192) + Units(rng, 1) - GetS32(m, r + 20);
            for (uint32_t f = 12; f < 120; f += 12) {
                PutS32(m, r + f, GetS32(m, r + f) + dx);
                PutS32(m, r + f + 8, GetS32(m, r + f + 8) + dz);
            }
        }
        if (Chance(rng, 4)) m.PokeWord(r + 8, Pick(rng, {0u, 1u, 2u}));
        shape = r;
    }
    StirWalker(m, rng, e);
    m.PokeWord(0x800CCF90u, Pick(rng, {0u, 1u, 3u, 7u, 8u, rng.U32() % 8u}));
    for (uint32_t i = 0; i < 8; ++i) {
        const uint8_t id = static_cast<uint8_t>(0xC0u | (rng.U32() % 8u));
        m.WriteBlock(0x800CCF88u + i, &id, 1);
    }
    m.PokeWord(c.gp + kRandSeedGpOffset, rng.U32());
    a.a[0] = e;
    a.a[1] = shape;
}
uint32_t NativeRs(NativeEnv& env, const Args& a) {
    EnRun run(env);
    co::CloneCallees cc(env, run);
    const cr::Tables tab(env.clone->mem, env.gp);
    const bool ok = rr::sim::RiderVsShape(run.g, a.a[0], a.a[1], env.sp, tab.t, cc);
    return co::Refuse(env, run, ok, "RiderVsShape");
}

// ---------------------------------------------------------------- RiderNone, 0x800B2AF8 (`jr ra`)
void SetupNone(CaseContext& c, Args& a) {
    Rng& rng = *c.rng;
    a.a[0] = Walker(*c.mem, rng);
    a.a[1] = Walker(*c.mem, rng);
}
uint32_t NativeNone(NativeEnv& env, const Args& a) {
    EnRun run(env);
    co::CloneCallees cc(env, run);
    const cr::Tables tab(env.clone->mem, env.gp);
    const uint32_t args[2] = {a.a[0], a.a[1]};
    uint32_t v0 = 0;
    bool ok = true;
    rr::sim::ServePartners(run.g, rr::sim::CollCall{rr::sim::partners::kRiderNone, args, 2, env.sp}, tab.t, cc, v0, ok);
    return co::Refuse(env, run, ok, "RiderNone");
}

// ---------------------------------------------------------------- TrafficVsTraffic, 0x800B2D44
// Two cars: live records of this machine, or its car 0 copied into slot 1..7 (handle 0x60 | slot) and moved
// into, beside or away from the first.
void SetupTt(CaseContext& c, Args& a) {
    Rng& rng = *c.rng;
    Memory& m = *c.mem;
    const uint32_t car = creact::TrafficCar(m, rng);
    uint32_t other = creact::TrafficCar(m, rng);
    if (other == car || Chance(rng, 2)) {
        const uint32_t slot = 1u + rng.U32() % 7u;
        other = 0x800CF660u + 512u * slot;
        if (other == car) other = 0x800CF660u + 512u * (slot % 7u + 1u);
        std::vector<uint8_t> b(512);
        m.ReadBlock(car, b.data(), 512);
        m.WriteBlock(other, b.data(), 512);
        WriteU16Bench(m, other + 172, static_cast<uint16_t>(0x60u | ((other - 0x800CF660u) / 512u)));
    }
    if (!Chance(rng, 5)) co::MoveNear(m, other, car, Units(rng, 3), Chance(rng, 3) ? Units(rng, 1) / 2 : 0, Units(rng, 3));
    if (Chance(rng, 3)) {
        int16_t d[3];
        co::Dir(rng, d);
        d[1] = static_cast<int16_t>(d[1] / 8);
        PutVec3S16(m, other + 450, d);
        PutS32(m, other + 292, static_cast<int32_t>(rng.U32() % 4096u));
    }
    a.a[0] = car;
    a.a[1] = other;
}
uint32_t NativeTt(NativeEnv& env, const Args& a) {
    EnRun run(env);
    const cr::Tables tab(env.clone->mem, env.gp);
    const bool ok = rr::sim::TrafficVsTraffic(run.g, a.a[0], a.a[1], env.sp, tab.t);
    return co::Refuse(env, run, ok, "TrafficVsTraffic");
}

// ---------------------------------------------------------------- Launch, 0x80084BE8
// A bike of this machine; its contact +0x340 a car (pool 3), another bike (pool 0), a pool-6 volume, or a
// pool-5 handle of our own (slot 0..3, whose record *(0x800CE5A4) + 452 slot points at a model record in
// the scratch with the DOD3 kind 0..9 at +0x0E bits 7..11); k 0 or 1; flagsC bit 25 (the latch) drawn with
// a latched unit direction +0x360; the base +0x1F8, the lift distance +0x300, the speed +0x240.
void SetupLaunch(CaseContext& c, Args& a) {
    Rng& rng = *c.rng;
    Memory& m = *c.mem;
    const uint32_t S = c.scratch;
    const uint32_t e = co::Pool0(m, Pick(rng, {0u, 6u, rng.U32() % 18u}));
    uint32_t partner = 0;
    switch (rng.U32() % 4u) {
    case 0: partner = creact::TrafficCar(m, rng) + 172u; break;
    case 1: partner = co::Pool0(m, 1u + rng.U32() % 17u) + 172u; break;
    case 2: {
        co::PlantVolumes(c, e, 1);
        partner = m.PeekWord(rr::sim::kPoolTableAddr + 16u * 6u);
        break;
    }
    default: {
        const uint32_t slot = rng.U32() % 4u;
        WriteU16Bench(m, S, static_cast<uint16_t>(0xA0u | slot));
        const uint32_t kind = Pick(rng, {6u, 8u, 8u, 6u, rng.U32() % 10u});
        WriteU16Bench(m, S + 64 + 14, static_cast<uint16_t>((kind << 7) | (Chance(rng, 3) ? (rng.U32() & 0xF07Fu) : 0u)));
        m.PokeWord(m.PeekWord(0x800CE5A4u) + 452u * slot, S + 64);
        partner = S;
        break;
    }
    }
    m.PokeWord(e + 832, partner);
    int16_t d[3];
    co::Dir(rng, d);
    if (Chance(rng, 2)) d[1] = static_cast<int16_t>(d[1] / 4);
    PutVec3S16(m, e + 450, d);
    co::Dir(rng, d);
    PutVec3S16(m, e + 864, d);
    if (Chance(rng, 3)) {
        co::Dir(rng, d);
        PutVec3S16(m, e + 522, d);
    }
    uint32_t fc = m.PeekWord(e + 568);
    fc = Chance(rng, 2) ? (fc | 0x2000000u) : (fc & ~0x2000000u);
    m.PokeWord(e + 568, fc);
    PutS32(m, e + 768, static_cast<int32_t>(Pick(rng, {0u, 0x8000u, 0x10000u, static_cast<uint32_t>(Units(rng, 2))})));
    m.PokeWord(e + 576, Pick(rng, {0u, 0x8DC28u, 0xB1333u, 0x6A51Eu, rng.U32() % 0x400000u, 0u - rng.U32() % 0x100000u}));
    if (Chance(rng, 3)) m.PokeWord(e + 480, rng.U32() % 0x300000u);
    a.a[0] = e;
    a.a[1] = Chance(rng, 2) ? 0u : 1u;
}
uint32_t NativeLaunch(NativeEnv& env, const Args& a) {
    EnRun run(env);
    const cr::Tables tab(env.clone->mem, env.gp);
    const bool ok = rr::sim::Launch(run.g, a.a[0], static_cast<int32_t>(a.a[1]), env.sp, tab.t);
    return co::Refuse(env, run, ok, "Launch");
}

// ---------------------------------------------------------------- TurnFacing, 0x8007ED64
// A bike of this machine; the angle as the pool loop computes it ((p x 652) >> 16, small, both signs) or any
// 32-bit value; the length +0x134 the game's or anything; the rows unit vectors or stray.
void SetupTurn(CaseContext& c, Args& a) {
    Rng& rng = *c.rng;
    Memory& m = *c.mem;
    const uint32_t e = co::Pool0(m, Pick(rng, {0u, 6u, rng.U32() % 18u}));
    if (Chance(rng, 3)) PutS32(m, e + 308, static_cast<int32_t>(Pick(rng, {0u, 0x20000u, rng.U32() % 0x60000u, rng.U32()})));
    if (Chance(rng, 3)) {
        int16_t d[3];
        co::Dir(rng, d);
        PutVec3S16(m, e + 438, d);
        co::Dir(rng, d);
        PutVec3S16(m, e + 528, d);
    }
    if (Chance(rng, 4)) for (uint32_t k = 0; k < 3; ++k) PutS32(m, e + 504 + 4 * k, rng.S32());
    const int32_t ang = static_cast<int32_t>(Pick(rng, {0u, 1u, 0xFFFFFFFFu, rng.U32() % 800u, 0u - rng.U32() % 800u, rng.U32()}));
    a.a[0] = e;
    a.a[1] = static_cast<uint32_t>(ang);
}
uint32_t NativeTurn(NativeEnv& env, const Args& a) {
    EnRun run(env);
    rr::sim::TurnFacing(run.g, a.a[0], static_cast<int32_t>(a.a[1]));
    return co::Refuse(env, run, true, "TurnFacing");
}

} // namespace partnerrows

// ================================================================ partners2: the prop partner, the rider grab
namespace partnerrows {

// ---------------------------------------------------------------- PropVsShape 0x800B2E64 in the collision pass
// The collision pass hands it a prop (kind 4: pool 4) against a pool-5 record + 0xAC or a pool-6 volume. The
// prop is solidrows::PlantProp's (handle 0x80, +0x250 drawn with bit 1 - the toppling arm); now and then the
// view's handle 0x9E (slot 30: the view arm, ApplyImpulse flag 0). The shape: a pool-6 volume moved onto the
// prop (or left where PlantVolumes put it), or a bike record + 0xAC (the pool-5 layout: box +0xC4, rows +0x1B0)
// moved into or beside it. PropTopple 0x800B3344 runs NATIVELY (solid.h) - the product's configuration - so the
// stale depth it reads at its sp + 48 (FirstPointInsideBox's saved s2, PropVsShapePass) is compared too.
struct ToppleNative final : rr::sim::CollisionCallees {
    co::CloneCallees& in;
    rr::sim::GuestRam& g;
    const rr::sim::BikeTables& t;
    ToppleNative(co::CloneCallees& i, rr::sim::GuestRam& gg, const rr::sim::BikeTables& tt) : in(i), g(gg), t(tt) {}
    bool PlaySound3D(int32_t x, int32_t z, int32_t id, int32_t bank) override { return in.PlaySound3D(x, z, id, bank); }
    bool ReleaseContact(uint32_t e) override { return in.ReleaseContact(e); }
    bool Rumble(uint32_t e, uint32_t o, int32_t s, int32_t k, int32_t d, uint32_t sp) override {
        return in.Rumble(e, o, s, k, d, sp);
    }
    bool Unported(uint32_t fn, const uint32_t* a, int n, uint32_t sp, uint32_t& v0) override {
        if (fn == rr::sim::solid::kPropTopple) {
            bool ok = true;
            rr::sim::ServeSolid(g, rr::sim::CollCall{fn, a, n, sp}, t, *this, v0, ok);
            return ok;
        }
        return in.Unported(fn, a, n, sp, v0);
    }
    bool UnportedAt(uint32_t fn, const uint32_t* a, int n, uint32_t sp, const rr::sim::GuestRegs& r,
                    uint32_t& v0) override {
        if (fn == rr::sim::solid::kPropTopple) return Unported(fn, a, n, sp, v0);
        return in.UnportedAt(fn, a, n, sp, r, v0);
    }
};
void SetupPropShape(CaseContext& c, Args& a) {
    Rng& rng = *c.rng;
    Memory& m = *c.mem;
    const uint32_t p = solidrows::PlantProp(c);
    if (Chance(rng, 6)) WriteU16Bench(m, p + 172, 0x9Eu);
    if (Chance(rng, 2)) m.PokeWord(p + 592, m.PeekWord(p + 592) | 2u);
    uint32_t shape;
    if (Chance(rng, 3)) {
        const uint32_t b = co::Pool0(m, Pick(rng, {6u, 1u + rng.U32() % 17u}));
        co::MoveNear(m, b, p, Units(rng, 1), Chance(rng, 3) ? Units(rng, 1) / 2 : 0, Units(rng, 1));
        shape = b + 172;
    } else {
        const uint32_t n6 = 1u + rng.U32() % 3u;
        co::PlantVolumes(c, p, n6);
        const uint32_t r = m.PeekWord(rr::sim::kPoolTableAddr + 16u * 6u) + 280u * (rng.U32() % n6);
        if (!Chance(rng, 4)) {                                                // the volume moved onto the prop
            const int32_t dx = GetS32(m, p + 184) + Units(rng, 1) - GetS32(m, r + 12);
            const int32_t dy = Chance(rng, 2) ? GetS32(m, p + 188) + 0x8000 - GetS32(m, r + 16) : 0;
            const int32_t dz = GetS32(m, p + 192) + Units(rng, 1) - GetS32(m, r + 20);
            for (uint32_t f = 12; f < 120; f += 12) {
                PutS32(m, r + f, GetS32(m, r + f) + dx);
                PutS32(m, r + f + 4, GetS32(m, r + f + 4) + dy);
                PutS32(m, r + f + 8, GetS32(m, r + f + 8) + dz);
            }
        }
        shape = r;
    }
    m.PokeWord(c.gp + kRandSeedGpOffset, rng.U32());
    a.a[0] = p;
    a.a[1] = shape;
}
uint32_t NativePropShape(NativeEnv& env, const Args& a) {
    EnRun run(env);
    co::CloneCallees cc(env, run);
    const cr::Tables tab(env.clone->mem, env.gp);
    ToppleNative tn(cc, run.g, tab.t);
    uint32_t v0 = 0;
    const bool ok = rr::sim::PropVsShapePass(run.g, a.a[0], a.a[1], env.sp, tn, v0);
    if (!ok && env.failure.empty() && !run.s.Faulted()) env.failure = "PropVsShape refused";
    return run.Done(env, v0);
}

// ---------------------------------------------------------------- RiderGrab 0x800C29F0
// BikeVsRider's call: a rider of pool 1 (*(0x8005B3A4) + 628 k) and one of this machine's bikes. Drawn: the
// rider's heading +0x1C2 (unit, often along or against the bike's +0x1BC so that |d| > 0xDDB2, both signs),
// its speed +0x1E0 and its +0x254 (the machine's, a bike, the player's bike); the bike's rows +0x1B0 / +0x1BC,
// its passenger +0x358 (none or another bike), the owners' +0x25C over 0..3, the bike's +0x268 and latched
// heading +0x360, the +0x43C record's bytes +0x24 / +0x25.
void SetupGrab(CaseContext& c, Args& a) {
    Rng& rng = *c.rng;
    Memory& m = *c.mem;
    const uint32_t r = m.PeekWord(0x8005B3A4u) + 628u * Pick(rng, {6u, 0u, rng.U32() % 18u});
    const uint32_t bike = co::Pool0(m, Pick(rng, {0u, 6u, rng.U32() % 18u}));
    int16_t d[3];
    if (Chance(rng, 3)) {
        co::Dir(rng, d);
        PutVec3S16(m, bike + 432, d);
        co::Dir(rng, d);
        PutVec3S16(m, bike + 444, d);
    }
    GetVec3S16(m, bike + 444, d);
    switch (rng.U32() % 4u) {
    case 0: break;                                                             // along the bike's +0x1BC
    case 1:                                                                    // against it
        for (int16_t& x : d) x = static_cast<int16_t>(-x);
        break;
    case 2:                                                                    // across (+0x1B0, either side)
        GetVec3S16(m, bike + 432, d);
        if (Chance(rng, 2))
            for (int16_t& x : d) x = static_cast<int16_t>(-x);
        break;
    default: co::Dir(rng, d); break;
    }
    PutVec3S16(m, r + 450, d);
    PutS32(m, r + 480, static_cast<int32_t>(Pick(rng, {0u, 0x10000u, rng.U32() % 0x300000u, 0u - rng.U32() % 0x100000u})));
    if (Chance(rng, 2)) m.PokeWord(r + 596, Chance(rng, 2) ? co::Pool0(m, 0) : co::Pool0(m, 1u + rng.U32() % 17u));
    if (m.PeekWord(r + 596) == 0) m.PokeWord(r + 596, co::Pool0(m, 6));
    uint32_t pass = 0;
    if (Chance(rng, 2)) {
        pass = co::Pool0(m, 1u + rng.U32() % 17u);
        if (pass == bike) pass = co::Pool0(m, 17);
        if (pass == bike) pass = co::Pool0(m, 16);
    }
    m.PokeWord(bike + 856, pass);
    for (uint32_t who : {bike, pass}) {
        if (who == 0) continue;
        const uint32_t o = m.PeekWord(who + 852);
        if (o != 0) m.PokeWord(o + 604, Pick(rng, {0u, 1u, 2u, 3u, 0u, 1u}));
    }
    m.PokeWord(bike + 616, Chance(rng, 2) ? 0u : rng.U32());
    co::Dir(rng, d);
    PutVec3S16(m, bike + 864, d);
    const uint32_t rec = m.PeekWord(bike + 1084);
    if (rec != 0 && Chance(rng, 2)) {
        const uint8_t b[2] = {static_cast<uint8_t>(rng.U32()), static_cast<uint8_t>(rng.U32())};
        m.WriteBlock(rec + 36, b, 2);
    }
    a.a[0] = r;
    a.a[1] = bike;
}
uint32_t NativeGrab(NativeEnv& env, const Args& a) {
    EnRun run(env);
    rr::sim::RiderGrab(run.g, a.a[0], a.a[1], env.sp);
    return co::Refuse(env, run, true, "RiderGrab");
}

// ---------------------------------------------------------------- PedVoice SLUS 0x8001B44C
// BikeVsRider's call (x, z of the hit, the pedestrian, 1); flag also 0 and stray. e: a pedestrian of this
// machine (pool 2, stride 572) or a rider (pool 1); its model pointer +0x60 now and then aimed at a scratch word
// of 430 (the 104 voice), 429 / 431 or anything. The default bank gp+1912 the machine's or 1.
void SetupVoice(CaseContext& c, Args& a) {
    Rng& rng = *c.rng;
    Memory& m = *c.mem;
    const uint32_t e = Walker(m, rng);
    if (Chance(rng, 2)) {
        m.PokeWord(c.scratch + 128, Pick(rng, {430u, 430u, 429u, 431u, rng.U32()}));
        m.PokeWord(e + 96, c.scratch + 128);
    }
    if (Chance(rng, 4)) m.PokeWord(c.gp + 1912, 1u);
    a.a[0] = static_cast<uint32_t>(GetS32(m, e + 184) + Units(rng, 4));
    a.a[1] = static_cast<uint32_t>(GetS32(m, e + 192) + Units(rng, 4));
    a.a[2] = e;
    a.a[3] = Pick(rng, {1u, 1u, 0u, 2u, rng.U32()});
}
uint32_t NativeVoice(NativeEnv& env, const Args& a) {
    EnRun run(env);
    co::CloneCallees cc(env, run);
    const bool ok = rr::sim::PedVoice(run.g, static_cast<int32_t>(a.a[0]), static_cast<int32_t>(a.a[1]), a.a[2], a.a[3], cc);
    return co::Refuse(env, run, ok, "PedVoice");
}

} // namespace partnerrows

void AddPartnersRows(std::vector<Row>& rows) {
    using namespace partnerrows;
    using namespace rr::sim::partners;
    const OracleCallee ped{rr::sim::coll::kPedHit, 4, 2};
    crs::Ex(rows, "partners_rider_traffic", "void 0x800B2844(Rider *r, Car *car)  [RASHCDG]", kRiderVsTraffic, SetupRt,
            NativeRt, false, {ped});
    rows.back().maxSteps = 20'000'000;
    crs::Ex(rows, "partners_rider_shape", "void 0x800B2B00(Rider *r, u8 *shape)  [RASHCDG]", kRiderVsShape, SetupRs,
            NativeRs, false, {ped});
    rows.back().maxSteps = 20'000'000;
    crs::Ex(rows, "partners_rider_none", "void 0x800B2AF8(Rider *r, Rider *p)  [RASHCDG, jr ra]", kRiderNone, SetupNone,
            NativeNone, false);
    crs::Ex(rows, "partners_traffic_traffic", "void 0x800B2D44(Car *car, Car *p)  [RASHCDG]", kTrafficVsTraffic, SetupTt,
            NativeTt, false);
    crs::Ex(rows, "partners_launch", "void 0x80084BE8(Bike *e, s32 k)  [RASHCDG, LaunchLift native]", kLaunch,
            SetupLaunch, NativeLaunch, false);
    crs::Ex(rows, "partners_turn_facing", "void 0x8007ED64(Bike *e, s32 ang)  [RASHCDG]", kTurnFacing, SetupTurn,
            NativeTurn, false);
    // partners2
    crs::Ex(rows, "partners_prop_shape", "u32 0x800B2E64(Prop *p, u8 *shape)  [RASHCDG, PropTopple native]", kPropVsShape,
            SetupPropShape, NativePropShape, true);
    rows.back().maxSteps = 20'000'000;
    crs::Ex(rows, "partners_rider_grab", "void 0x800C29F0(Rider *r, Bike *b)  [RASHCDG, FightStat native]", kRiderGrab,
            SetupGrab, NativeGrab, false);
    crs::Ex(rows, "partners_ped_voice", "void 0x8001B44C(s32 x, s32 z, E *e, u32 flag)  [SLUS, PlaySound3D native]",
            kPedVoice, SetupVoice, NativeVoice, false);
}

// The extent [entry, end) of each function these rows accept, for `--coverage` (our own disassembly of
// RASHCDG: the address after the delay slot of its last `jr ra`).
const CoverageRange* PartnersCoverage(const char* rowName) {
    struct Named {
        const char* name;
        CoverageRange range;
    };
    static const Named kRanges[] = {
        {"partners_rider_traffic", {0x800B2844, 0x800B2AF8}}, {"partners_rider_none", {0x800B2AF8, 0x800B2B00}},
        {"partners_rider_shape", {0x800B2B00, 0x800B2C98}},   {"partners_traffic_traffic", {0x800B2D44, 0x800B2D88}},
        {"partners_launch", {0x80084BE8, 0x80084D94}},        {"partners_turn_facing", {0x8007ED64, 0x8007EF60}},
        {"partners_prop_shape", {0x800B2E64, 0x800B2F94}},    {"partners_rider_grab", {0x800C29F0, 0x800C2E9C}},
        {"partners_ped_voice", {0x8001B44C, 0x8001B494}},
    };
    for (const Named& n : kRanges)
        if (std::strcmp(n.name, rowName) == 0) return &n.range;
    return nullptr;
}
