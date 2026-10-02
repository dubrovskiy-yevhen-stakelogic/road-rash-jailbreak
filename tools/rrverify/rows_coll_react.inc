// Bench rows for src/game/sim/bike_react.{h,cpp}. Included by rows_coll.inc.
//
// The ports run on the clone's RAM (GUEST-ADDRESS, collision.h's model). BikeBikeGate 0x80081D7C runs
// NATIVELY inside BikeBikeReact (crash.h), its four callees and every other unported callee from the
// ORIGINAL code at exactly the stack pointer the original calls them at (co::CloneCallees ->
// bs::ExactOracle), and the call sequences are compared.

namespace creact {

using co::Chance;
using co::Pick;
using co::Pool0;
// How many cases reached each arm, measured by the callees the port's run made (printed once at exit:
// the reach of the random family, since `--coverage` has no range for these rows).
struct ReachStats {
    const char* row;
    std::vector<std::pair<uint32_t, const char*>> watch;
    std::vector<long> hit;
    long cases = 0;
    ReachStats(const char* r, std::vector<std::pair<uint32_t, const char*>> w) : row(r), watch(std::move(w)), hit(watch.size(), 0) {}
    void Note(const NativeEnv& env) {
        ++cases;
        for (size_t i = 0; i < watch.size(); ++i)
            for (const Cpu::CallObservation& o : env.calls)
                if (o.address == watch[i].first) {
                    ++hit[i];
                    break;
                }
    }
    ~ReachStats() {
        if (cases == 0) return;
        std::printf("%s reach: %ld case(s);", row, cases);
        for (size_t i = 0; i < watch.size(); ++i) std::printf(" %s %ld", watch[i].second, hit[i]);
        std::printf("\n");
    }
};

// ---------------------------------------------------------------- SideFromCos, 0x800AC56C
// Driven: the cosine at and around both +-0xDDB2 bounds, the game's range, anything; fl with and without
// bit 1; other 0..7 (the `sltu 4` arm) or anything.
void SetupSide(CaseContext& c, Args& a) {
    Rng& rng = *c.rng;
    a.a[0] = Pick(rng, {0xDDB2u, 0xDDB3u, 0xDDB1u, 0xFFFF224Eu, 0xFFFF224Du, 0xFFFF224Fu, rng.U32() % 0x20000u - 0x10000u,
                        rng.U32()});
    a.a[1] = Pick(rng, {0u, 2u, 8u, rng.U32()});
    a.a[2] = Pick(rng, {0u, 1u, 2u, 3u, 4u, 6u, rng.U32() % 8u, rng.U32()});
}
uint32_t NativeSide(Memory&, uint32_t, uint32_t, const Args& a) {
    return rr::sim::SideFromCos(static_cast<int32_t>(a.a[0]), a.a[1], a.a[2]);
}

// ---------------------------------------------------------------- RememberHandle, 0x800A8BE0
// The set word at the scratch or at a live bike's +0x390. Driven: 0..7 five-bit entries (the full
// case at 6), a set that already holds h + 1, h 0..17 (a real handle), 31 (h + 1 = 32 never matches a
// 5-bit entry and inserts 0), or anything in the upper half-word (compared unmasked, inserted masked).
void SetupRemember(CaseContext& c, Args& a) {
    Rng& rng = *c.rng;
    Memory& m = *c.mem;
    const uint32_t set = Chance(rng, 2) ? c.scratch : Pool0(m, rng.U32() % 18u) + 912u;
    const uint32_t h = Pick(rng, {rng.U32() % 18u, rng.U32() % 18u, 31u, 30u, rng.U32()});
    uint32_t w = 0;
    const uint32_t n = static_cast<uint32_t>(rng.Next() % 8u);
    for (uint32_t k = 0; k < n && k < 7; ++k) {
        uint32_t e = Pick(rng, {1u + rng.U32() % 31u, 1u + rng.U32() % 18u});
        if (Chance(rng, 4)) e = ((h & 0xFFFFu) + 1u) & 0x1Fu;
        w |= (e << (5u * k));
    }
    if (Chance(rng, 6)) w = rng.U32();
    m.PokeWord(set, w);
    a.a[0] = h;
    a.a[1] = set;
}
uint32_t NativeRemember(Memory& m, uint32_t, uint32_t gp, const Args& a) {
    rr::sim::GuestRam g(m.ram().data(), gp);
    uint32_t v = 0;
    return rr::sim::RememberHandle(g, a.a[0], a.a[1], v) ? v : 0xDEADDEADu;
}

// ---------------------------------------------------------------- the shared bike perturbation
// What both reactions read of a live bike and its rider. The rider's stance +0x220 stays its own, 0
// or a state in 26..37 (NAMED BOUND: a stance of another animation bank makes the ORIGINAL's stance
// layer trap on a clip index past the loaded table); the second stance
// +0x262 likewise.
void PerturbRider(CaseContext& c, uint32_t e) {
    Rng& rng = *c.rng;
    Memory& m = *c.mem;
    const uint32_t r = m.PeekWord(e + 852);
    if (r < 0x80000000u || r >= 0x80200000u) return;
    if (Chance(rng, 3)) m.PokeWord(r + 604, Pick(rng, {1u, 1u, 0u, 2u, 3u}));
    const uint32_t own = static_cast<uint16_t>(GetS16(m, r + 544));
    if (Chance(rng, 4)) WriteU16Bench(m, r + 544, static_cast<uint16_t>(Pick(rng, {0u, 26u + rng.U32() % 12u, own})));
    if (Chance(rng, 3)) {
        WriteU16Bench(m, r + 608, static_cast<uint16_t>(Pick(rng, {0u, 1u, rng.U32() & 0xFFFFu})));
        WriteU16Bench(m, r + 610, static_cast<uint16_t>(Pick(rng, {own, 0u, 26u + rng.U32() % 12u, 25u, 38u})));
    }
}
void PerturbDamage(CaseContext& c, uint32_t e) {
    Rng& rng = *c.rng;
    Memory& m = *c.mem;
    const uint32_t r = m.PeekWord(e + 1084);
    if (r < 0x80000000u || r >= 0x80200000u) return;
    const uint8_t hp = static_cast<uint8_t>(Pick(rng, {rng.U32() & 0xFFu, 128u, 129u, 64u, 65u, 1u, 0u, 200u}));
    m.WriteBlock(r + 37, &hp, 1);
    const uint8_t fl = static_cast<uint8_t>(rng.U32() & 0xFFu);
    m.WriteBlock(r + 68, &fl, 1);
}

// ---------------------------------------------------------------- ImpactSeverity, 0x800A9408
// A live bike (the player, bike 6 or any of the 18: its rider, model record +0x22C and damage record
// +0x43C are this machine's). Driven: flagsC & 0x22C (the ratio and the damage arm); mag of the game's
// size (closing speeds 0..40.0) or anything; num / den the masses (475.0 / 275.0 / 1360.0), 1.0 (the
// den == 1.0 skip), their negatives and zero (the four sign arms of the ratio) or anything; mode 0..3,
// 4 (the 21 default) or anything; the model record's divisor +0xE0 its own, 0 (the R3000 `div` by
// zero) or anything; the rider (PerturbRider) and the damage byte / flag byte (PerturbDamage).
void SetupSeverity(CaseContext& c, Args& a) {
    Rng& rng = *c.rng;
    Memory& m = *c.mem;
    const uint32_t e = Pool0(m, Pick(rng, {0u, 6u, rng.U32() % 18u}));
    uint32_t fc = m.PeekWord(e + 568) & ~0x22Cu;
    if (!Chance(rng, 3)) fc |= Pick(rng, {0x4u, 0x8u, 0x20u, 0x200u, rng.U32() & 0x22Cu});
    m.PokeWord(e + 568, fc);
    PerturbRider(c, e);
    PerturbDamage(c, e);
    const uint32_t model = m.PeekWord(e + 556);
    if (model >= 0x80000000u && model < 0x80200000u && Chance(rng, 5))
        m.PokeWord(model + 224, Pick(rng, {0u, 0x10000u, 0xFFFFFFFFu, rng.U32()}));
    auto mass = [&]() {
        return Pick(rng, {475u << 16, 275u << 16, 1360u << 16, 0x10000u, 0u, 0u - (475u << 16), 0u - (rng.U32() % 0x8000000u),
                          rng.U32() % 0x8000000u, rng.U32()});
    };
    a.a[0] = e;
    a.a[1] = Pick(rng, {rng.U32() % 0x280000u, rng.U32() % 0x40000u, 0u, rng.U32()});
    a.a[2] = mass();
    a.a[3] = Chance(rng, 4) ? 0x10000u : mass();
    a.a4 = Pick(rng, {0u, 1u, 2u, 3u, 4u, rng.U32()});
    if (c.rootCounter != nullptr) *c.rootCounter = rng.U32() & 0xFFFFu;
}
uint32_t NativeSeverity(NativeEnv& env, const Args& a) {
    EnRun run(env);
    co::CloneCallees cc(env, run);
    uint32_t v = 0;
    const bool ok = rr::sim::ImpactSeverity(run.g, a.a[0], static_cast<int32_t>(a.a[1]), static_cast<int32_t>(a.a[2]),
                                            static_cast<int32_t>(a.a[3]), static_cast<int32_t>(a.a4), env.sp, cc, v);
    static ReachStats reach("impact_severity", {{rr::sim::react::kStanceEvent, "stance"}});
    reach.Note(env);
    if (!ok && env.failure.empty() && !run.s.Faulted()) env.failure = "ImpactSeverity refused";
    return run.Done(env, v);
}

// ---------------------------------------------------------------- BikeBikeReact, 0x800AC130
// Two DIFFERENT live bikes of this machine (the deferred handler hands it a logged pair), b moved
// next to a half the time. Driven: a captured bike's rows / heading / speed laid over a (dump-derived);
// flagsC bit 0, bits 1..3 (the rider test), 0x200 / 0x400 (the gate's crashed / airborne arms), 0x80,
// 0x02000000 (the gate's latch) and 0x20D (the rumble speed); +0x33C naming the partner (the early
// return); the speeds +0x1E0 / +0x35C across the 0x1017E "fast" bound; the masses +0x13C; the codes
// (0x200 | 0..3 and the low codes, as the gate row draws them); the stale words of the gate's frame
// (entry sp - 72 - 280 + 108 .. + 124: the live pair (8, 0x800B4C58), words inside the limit, or
// anything - rows_crash.inc SetupGate's rule); the player count 1 or 2; the results-screen word
// 0x8005B220; the two +0x390 handle sets; the riders (PerturbRider) and damage (PerturbDamage); the
// Rand seed and root counter 2.
// NAMED BOUNDS (the gate's, crash.h / rows_crash.inc): every direction at most unit length (live rows
// or unit random ones) and speeds within +-128.0; handles are the bikes' own (0..17).
void SetupBikeReact(CaseContext& c, Args& a) {
    Rng& rng = *c.rng;
    Memory& m = *c.mem;
    const uint32_t sa = Pick(rng, {0u, 6u, rng.U32() % 18u});
    uint32_t sb = Pick(rng, {0u, 6u, rng.U32() % 18u});
    if (sb == sa) sb = (sa + 1u + rng.U32() % 17u) % 18u;
    const uint32_t A = Pool0(m, sa), B = Pool0(m, sb);
    if (!Chance(rng, 3))
        co::MoveNear(m, B, A, static_cast<int32_t>(rng.U32() % 0x40000u) - 0x20000, 0,
                     static_cast<int32_t>(rng.U32() % 0x40000u) - 0x20000);
    if (c.sample != nullptr && c.sample->entity.size() == 1096) {
        m.WriteBlock(A + 0x1B0, c.sample->entity.data() + 0x1B0, 0x216 - 0x1B0);
        PutS32(m, A + 0x1E0, c.sample->speed);
    }
    auto unitRow = [&](uint32_t e, uint32_t off) {
        int16_t v[3];
        co::Dir(rng, v);
        PutVec3S16(m, e + off, v);
    };
    auto speed = [&]() {
        return Pick(rng, {0x1017Eu, 0x1017Fu, 0x10000u, 0u, 0xA0000u, 0xD6944u, rng.U32() % 0x800000u, rng.U32() % 0x3C0000u,
                          static_cast<uint32_t>(rng.S32() >> 9)});
    };
    for (uint32_t e : {A, B}) {
        if (c.sample == nullptr && Chance(rng, 2))
            for (uint32_t off : {0x1B0u, 0x1B6u, 0x1BCu, 0x204u, 0x20Au, 0x210u, 0x1C2u, 0x32Eu, 0x360u}) unitRow(e, off);
        if (Chance(rng, 2)) PutS32(m, e + 0x1E0, static_cast<int32_t>(speed()));
        if (Chance(rng, 2)) PutS32(m, e + 0x35C, static_cast<int32_t>(speed()));
        if (Chance(rng, 3)) m.PokeWord(e + 0x1E8, static_cast<uint32_t>(rng.S32() >> (8 + rng.Next() % 10u)));
        if (Chance(rng, 3)) m.PokeWord(e + 0x13C, Pick(rng, {475u << 16, 275u << 16, 1360u << 16, 1u << 16, rng.U32() % 0x8000000u}));
        uint32_t fc = m.PeekWord(e + 568);
        if (Chance(rng, 2)) {
            fc &= ~0x0200077Fu;
            if (Chance(rng, 4)) fc |= 0x200u;
            if (Chance(rng, 4)) fc |= 0x400u;
            if (Chance(rng, 4)) fc |= Pick(rng, {1u, 2u, 4u, 8u, 0x20u, 0x40u, 0x100u});
            if (Chance(rng, 5)) fc |= 0x80u;
            if (Chance(rng, 3)) fc |= 0x02000000u;
            if (Chance(rng, 6)) fc |= 0x80000u;
        }
        m.PokeWord(e + 568, fc);
        if (Chance(rng, 5)) m.PokeWord(e + 560, m.PeekWord(e + 560) ^ 0x20000000u);
        if (Chance(rng, 3)) m.PokeWord(e + 912, Pick(rng, {0u, rng.U32(), rng.U32() & 0x3FFu}));
        PerturbRider(c, e);
        PerturbDamage(c, e);
    }
    if (Chance(rng, 5)) m.PokeWord(A + 828, B + 172u);
    if (Chance(rng, 5)) m.PokeWord(B + 828, A + 172u);
    co::SetNumPlayers(m, Chance(rng, 4) ? 2u : 1u);
    m.PokeWord(co::kSkipResults, Chance(rng, 8) ? 1u : 0u);
    // the gate's stale frame words (crash.h), at the depth BikeBikeReact calls it
    const uint32_t gf = c.sp - rr::sim::react::kBikeBikeReactFrame - rr::sim::react::kGateFrame;
    const uint32_t which = static_cast<uint32_t>(rng.Next() % 4u);
    const uint32_t s = which == 0 ? 8u : which == 1 ? static_cast<uint32_t>(rng.S32() >> 14) : rng.U32();
    const uint32_t l = which == 0 ? 0x800B4C58u : which == 1 ? Pick(rng, {0xF5BEu, 0x1226Cu, rng.U32() % 0x30000u}) : rng.U32();
    m.PokeWord(gf + 108, s);
    m.PokeWord(gf + 116, l);
    for (uint32_t off : {120u, 124u}) m.PokeWord(gf + off, Pick(rng, {0u, static_cast<uint32_t>(rng.S32() >> 9), rng.U32()}));
    auto code = [&]() {
        return Chance(rng, 2) ? (0x200u | Pick(rng, {1u, 2u, 3u, 0u, 4u, 5u, rng.U32() % 6u}))
                              : (rng.U32() % 6u | (Chance(rng, 8) ? rng.U32() & ~0x2FFu : 0u));
    };
    m.PokeWord(c.gp + kRandSeedGpOffset, rng.U32());
    if (c.rootCounter != nullptr) *c.rootCounter = rng.U32() & 0xFFFFu;
    a.a[0] = A;
    a.a[1] = B;
    a.a[2] = code();
    a.a[3] = code();
}
ReachStats& ReactReach() {
    using namespace rr::sim::react;
    static ReachStats s("bike_bike_react", {{kFaceNormal, "face-normal"}, {rr::sim::coll::kImpactTurn, "impact-turn"},
                                           {kHitSpeed, "hit-speed"}, {kTakePartnerHeading, "take-heading"},
                                           {kRiderSpeech, "speech"}, {kStanceEvent, "stance"}, {kGetRCnt, "rcnt"},
                                           {rr::sim::coll::kPadMotor, "pad"}});
    return s;
}
uint32_t NativeBikeReact(NativeEnv& env, const Args& a) {
    EnRun run(env);
    co::CloneCallees cc(env, run);
    const cr::Tables tab(env.clone->mem, env.gp);
    const bool ok = rr::sim::BikeBikeReact(run.g, a.a[0], a.a[1], a.a[2], a.a[3], env.sp, tab.t, cc);
    ReactReach().Note(env);
    return co::Refuse(env, run, ok, "BikeBikeReact");
}

// ---------------------------------------------------------------- the traffic pair
// A live bike of this machine and a LIVE traffic record of it (pool 3, 0x800CF660 + 512 k, k < 8, with a
// non-zero half width +0x130: on rr-race only slot 0 is populated). NAMED BOUND: an empty record (all
// zero) is never a contact partner - the pass inserts only live cars - and the unported ImpactGate
// path on one reads what its own solver leaves in the stack.
uint32_t TrafficCar(const Memory& m, Rng& rng) {
    uint32_t live[8];
    uint32_t n = 0;
    for (uint32_t k = 0; k < 8; ++k)
        if (GetS32(m, 0x800CF660u + 512u * k + 304u) != 0) live[n++] = 0x800CF660u + 512u * k;
    return n == 0 ? 0x800CF660u : live[rng.U32() % n];
}
uint32_t SideFlags(Rng& rng) {
    return Chance(rng, 3) ? (0x200u | Pick(rng, {1u, 3u, 1u, 3u, 0u, 2u, 4u, 5u}))
                          : Pick(rng, {1u, 3u, rng.U32() % 8u, rng.U32() & 0xFFFFu});
}

// TrafficSideShove, 0x800A91AC. Driven: flags (0x200 | 1 / 3, other faces, no 0x200); the corner
// code 0..7 (NAMED BOUND: the original indexes the corners with code & 0xFF unchecked; 0..7 are the
// box's, the callers' codes are corner indices); imp placing the corner's point at a lateral offset of
// +-(0.6 .. 1.25) x the car's half width +0x130 along its lateral row, or anything; the bike heading
// (the side test's sign); the start-of-frame centre +0x1D4 near or far along the car's length.
void SetupShove(CaseContext& c, Args& a) {
    Rng& rng = *c.rng;
    Memory& m = *c.mem;
    const uint32_t e = Pool0(m, Pick(rng, {0u, 6u, rng.U32() % 18u}));
    const uint32_t car = TrafficCar(m, rng);
    const uint32_t code = Chance(rng, 8) ? (rng.U32() & 0xFFFFFF07u) : rng.U32() % 8u;
    const uint32_t I = c.scratch;
    const int32_t w = GetS32(m, car + 304);
    const int32_t frac = static_cast<int32_t>(rng.U32() % 0xA000u) + 0x9800;                  // 0.59 .. 1.22
    int32_t p = static_cast<int32_t>((static_cast<int64_t>(w) * frac) >> 16);
    if (Chance(rng, 2)) p = -p;
    int16_t n[3];
    GetVec3S16(m, car + 432, n);
    for (uint32_t k = 0; k < 3; ++k) {
        const int32_t q = GetS32(m, car + 184 + 4 * k) + static_cast<int32_t>((static_cast<int64_t>(n[k]) * p) >> 12) +
                          static_cast<int32_t>(rng.U32() % 0x8000u) - 0x4000;
        int32_t v = q - GetS32(m, e + 196 + 12 * (code & 0xFFu) + 4 * k);
        if (Chance(rng, 6)) v = rng.S32() >> (8 + rng.Next() % 12u);
        PutS32(m, I + 4 * k, v);
    }
    if (Chance(rng, 2)) {
        int16_t h[3];
        co::Dir(rng, h);
        PutVec3S16(m, e + 450, h);
    }
    if (Chance(rng, 2)) co::MoveNear(m, e, car, 0, 0, static_cast<int32_t>(rng.U32() % 0x100000u) - 0x80000);
    a.a[0] = code;
    a.a[1] = Chance(rng, 3) ? SideFlags(rng) : (0x200u | Pick(rng, {1u, 3u}) | (Chance(rng, 4) ? (rng.U32() & 0xFC00u) : 0u));
    a.a[2] = e;
    a.a[3] = car;
    a.a4 = I;
}
uint32_t NativeShove(NativeEnv& env, const Args& a) {
    EnRun run(env);
    uint32_t v = 0;
    const bool ok = rr::sim::TrafficSideShove(run.g, a.a[0], a.a[1], a.a[2], a.a[3], a.a4, env.sp, v);
    if (!ok && env.failure.empty()) env.failure = "TrafficSideShove refused";
    return run.Done(env, v);
}

// What both traffic rows draw on the bike and the car: the riding-on-the-car flags (flagsB bits 15,
// 17, 18), +0x268, +0x304, +0x1E8, +0x2B4, the speeds +0x1E0 (both) / +0x240 across the 0x1017E
// bound and the 2.2352 floor, flagsC & 0x20E and bit 1, +0x24 bits 25..26 (the surface effect), the
// car's +0x1FD, the riders and damage (PerturbRider / PerturbDamage), the player count, 0x8005B220.
// NAMED BOUND: the rows +0x20A / +0x210 stay unit (the OP's result is Normalized; a longer one
// overflows Normalize's trapping add).
void PerturbTrafficPair(CaseContext& c, uint32_t e, uint32_t car) {
    Rng& rng = *c.rng;
    Memory& m = *c.mem;
    uint32_t fb = m.PeekWord(e + 564);
    if (Chance(rng, 2)) fb = (fb & ~0x68000u) | Pick(rng, {0x8000u, 0x48000u, 0x8000u, 0x28000u, 0x40000u, 0u});
    m.PokeWord(e + 564, fb);
    if (Chance(rng, 2)) m.PokeWord(e + 616, Chance(rng, 2) ? 0u : static_cast<uint32_t>(rng.S32() >> 14));
    if (Chance(rng, 2)) m.PokeWord(e + 772, Pick(rng, {0u, 1u, rng.U32() % 0x40000u}));
    if (Chance(rng, 2)) m.PokeWord(e + 488, Chance(rng, 2) ? 0u : static_cast<uint32_t>(rng.S32() >> 12));
    if (Chance(rng, 2)) m.PokeWord(e + 692, rng.U32() & 0xFFFFu);
    auto speed = [&]() {
        return Pick(rng, {0x1017Eu, 0x1017Fu, 0u, 0x23C36u, 0x23C35u, rng.U32() % 0x800000u, rng.U32() % 0x40000u});
    };
    if (Chance(rng, 2)) m.PokeWord(e + 480, speed());
    if (Chance(rng, 2)) m.PokeWord(car + 480, speed());
    if (Chance(rng, 2)) m.PokeWord(e + 576, speed());
    uint32_t fc = m.PeekWord(e + 568);
    if (Chance(rng, 2)) fc = (fc & ~0x20Fu) | Pick(rng, {0u, 2u, 4u, 8u, 0x200u, 1u, 0x20Eu & rng.U32()});
    m.PokeWord(e + 568, fc);
    if (Chance(rng, 3)) m.PokeWord(e + 36, (m.PeekWord(e + 36) & ~0x06000000u) | ((rng.U32() % 4u) << 25));
    if (Chance(rng, 3)) {
        uint8_t b = static_cast<uint8_t>(rng.U32());
        m.WriteBlock(car + 509, &b, 1);
    }
    if (Chance(rng, 3)) {
        int16_t v[3];
        co::Dir(rng, v);
        PutVec3S16(m, e + 450, v);
    }
    PerturbRider(c, e);
    PerturbDamage(c, e);
    co::SetNumPlayers(m, Chance(rng, 4) ? 2u : 1u);
    m.PokeWord(co::kSkipResults, Chance(rng, 8) ? 1u : 0u);
    m.PokeWord(c.gp + kRandSeedGpOffset, rng.U32());
    if (c.rootCounter != nullptr) *c.rootCounter = rng.U32() & 0xFFFFu;
}

// BikeTrafficReact, 0x800AC958. A live bike and car (TrafficCar), moved together half the time; code
// and flags as the deferred handler hands them (SideFlags; code a corner or anything); imp the scratch
// vector of the game's size; PerturbTrafficPair. THE UNINITIALISED SLOTS: the words
// at entry sp - 80 + 24 / + 28 are planted (a unit-ish normal, zero or anything) - the !(flags & 0x200)
// path reads them, and the port reads them where the original does, from the guest frame.
void SetupTrafficReact(CaseContext& c, Args& a) {
    Rng& rng = *c.rng;
    Memory& m = *c.mem;
    const uint32_t e = Pool0(m, Pick(rng, {0u, 6u, rng.U32() % 18u}));
    const uint32_t car = TrafficCar(m, rng);
    if (Chance(rng, 2))
        co::MoveNear(m, car, e, static_cast<int32_t>(rng.U32() % 0x40000u) - 0x20000, 0,
                     static_cast<int32_t>(rng.U32() % 0x40000u) - 0x20000);
    PerturbTrafficPair(c, e, car);
    for (uint32_t k = 0; k < 3; ++k) PutS32(m, c.scratch + 4 * k, rng.S32() >> (12 + rng.Next() % 8u));
    const uint32_t slot = c.sp - 80u + 24u;
    int16_t n[3];
    co::Dir(rng, n);
    if (Chance(rng, 4)) PutVec3S16(m, slot, n);
    else m.PokeWord(slot, Pick(rng, {0u, rng.U32()})), m.PokeWord(slot + 4, Pick(rng, {0u, rng.U32()}));
    a.a[0] = e;
    a.a[1] = car;
    // NAMED BOUND: the face indices (code & 0xFF, flags & 0xFF) are 0..5. FaceNormal 0x800B675C indexes its
    // 6-byte table with the face unchecked, and past 7 it reads its CALLER's frame - here the prologue spills
    // of BikeTrafficReact (entry sp - 80 + 56 ..), which a port does not write; the responses only ever
    // produce the six box faces. The high bits (0x100 in code: ImpactGate's own-face arm; 0x200 in flags) are drawn.
    a.a[2] = (rng.U32() % 6u) | (Chance(rng, 3) ? 0x100u : 0u) | (Chance(rng, 8) ? (rng.U32() & 0xFE00u) : 0u);
    a.a[3] = (SideFlags(rng) & ~0xFFu) | Pick(rng, {1u, 3u, rng.U32() % 6u});
    a.a4 = c.scratch;
}
uint32_t NativeTrafficReact(NativeEnv& env, const Args& a) {
    EnRun run(env);
    co::CloneCallees cc(env, run);
    const cr::Tables tab(env.clone->mem, env.gp);
    // A development control, off unless RRJB_REACT_PLAIN is set: the unported calls made plain (ra = the
    // sentinel, the clone's s0..s8) instead of register-faithful.
    const rr::sim::GuestRegs caller = co::BenchCaller(env);
    const bool ok = rr::sim::BikeTrafficReact(run.g, a.a[0], a.a[1], a.a[2], a.a[3], a.a4, env.sp, tab.t, cc,
                                            std::getenv("RRJB_REACT_PLAIN") != nullptr ? nullptr : &caller);
    using namespace rr::sim::react;
    static ReachStats reach("bike_traffic_react", {{kFaceNormal, "face-normal"}, {kXzDot, "xz-dot"}, {kStanceEvent, "stance"},
                                                   {kGetRCnt, "rcnt"}, {rr::sim::coll::kPadMotor, "pad"}, {kSurfaceFx, "surface-fx"}});
    reach.Note(env);
    return co::Refuse(env, run, ok, "BikeTrafficReact");
}

// BikeVsTraffic, 0x800AC5BC. A live bike with one of the cars moved next to it (or left where it is);
// +0x340 / +0x33C naming the car's shape (and flagsB bit 31: the "on the car" test), flagsC bit 0,
// +0x140 bit 2 (the camera band: defer or react now), the contact count 0..8 (a full list reacts now);
// a passenger now and then (bike 17, unlisted, whose +0x358 names the bike); the heading and
// StaleHeading's flagsC bit 24; PerturbTrafficPair.
void SetupVsTraffic(CaseContext& c, Args& a) {
    Rng& rng = *c.rng;
    Memory& m = *c.mem;
    const uint32_t e = Pool0(m, Pick(rng, {0u, 0u, 6u, rng.U32() % 17u}));
    const uint32_t car = TrafficCar(m, rng);
    if (!Chance(rng, 4))
        co::MoveNear(m, car, e, static_cast<int32_t>(rng.U32() % 0x60000u) - 0x30000, 0,
                     static_cast<int32_t>(rng.U32() % 0x60000u) - 0x30000);
    PerturbTrafficPair(c, e, car);
    if (Chance(rng, 5)) m.PokeWord(e + 832, car + 172);
    if (Chance(rng, 5)) m.PokeWord(e + 828, car + 172);
    if (Chance(rng, 3)) m.PokeWord(e + 564, m.PeekWord(e + 564) ^ 0x80000000u);
    if (Chance(rng, 3)) m.PokeWord(e + 568, m.PeekWord(e + 568) ^ 1u);
    if (Chance(rng, 3)) m.PokeWord(e + 568, m.PeekWord(e + 568) ^ 0x01000000u);
    if (Chance(rng, 3)) WriteU16Bench(m, e + 320, static_cast<uint16_t>(GetS16(m, e + 320) ^ 4));
    m.PokeWord(rr::sim::kContactCount, Pick(rng, {0u, 0u, rng.U32() % 9u, 8u}));
    if (Chance(rng, 8)) {
        const uint32_t b17 = Pool0(m, 17);
        m.PokeWord(e + 856, b17);
        m.PokeWord(b17 + 856, e);
        m.PokeWord(b17 + 1088, 0);
        co::MoveNear(m, b17, e, static_cast<int32_t>(rng.U32() % 0x20000u) - 0x10000, 0, 0);
    }
    a.a[0] = e;
    a.a[1] = car;
}
uint32_t NativeVsTraffic(NativeEnv& env, const Args& a) {
    EnRun run(env);
    co::CloneCallees cc(env, run);
    const cr::Tables tab(env.clone->mem, env.gp);
    const bool ok = rr::sim::BikeVsTraffic(run.g, a.a[0], a.a[1], env.sp, tab.t, cc);
    using namespace rr::sim::react;
    static ReachStats reach("bike_vs_traffic", {{kResponse, "response"}, {kResponseFar, "response-far"}, {kImpactGate, "impact-gate"},
                                                {kFaceNormal, "face-normal"}, {kSurfaceFx, "surface-fx"}});
    reach.Note(env);
    return co::Refuse(env, run, ok, "BikeVsTraffic");
}

// ---------------------------------------------------------------- the rider pass's first pool loop
//
// A SPLICE on the rider / engine pass RASHCDG 0x8007B840(dt), exactly as
// rows_collision.inc's rider_pass_thrown: the guest run is the original pass start to finish; the
// clone runs the original pass with a hook at the region's entry 0x8007B894, where the port runs on the
// clone's RAM and `pc` goes to the exit 0x8007C23C (the thrown walk's entry: no register is live there,
// its delay slot writes t9 and the walk writes the rest before reading them; the loop's own branch back
// to 0x8007B894 is inside the port); a third run - the original on a private copy, stopped at the exit
// - gives the state the original region leaves, compared byte for byte outside the stack window.
// `--splice-control` skips the port. The region's callees: the list migration 0x80071BCC and
// BikeRecoverEnd 0x80072994 PORTED (bike_step.h, natively on the clone, the rider layer under them from
// the oracle), PlaySound3D and ReleaseContact PORTED (as co::CloneCallees runs them), the rest from the
// ORIGINAL code at the pass's frame: 0x8003E338, ImpactTurn 0x80083F30, 0x80084BE8, SLUS 0x800374D4,
// 0x8007504C, 0x80073D74, 0x8007ED64.
//
// The family: this machine's 18 pool-0 slots, each perturbed now and then: flagsC bit 25 (the latch,
// with +0x35C / +0x360 drawn), bit 27 (the migration; bits 11 / 12 more rarely - the crash launch, the
// wipeout start, the recover end, run natively), 0x200 / 0x400 (crashed / airborne: the ImpactTurn
// arm), the class bits 0..3, bits 8, 23, 26; +0x140 (live or not); the rider's +0x228 bit 19 (the
// road-edge skip); +0x3A0 bit 6, +0x184 bit 0, +0x174, +0x24 bit 9 (the edge gates); the lateral +0x158
// at and around the slice margins SLCT +0x30 / +0x31 (x 0.5) or anything; the heading +0x1C2 (the dot
// against the edge normal); flagsA bits 13 / 27, +0x1E0, +0x240, +0x2A4, +0x2B8 / +0x2BC around 1.0
// (the slow-down); flagsB & 0x38000 (the contact release, the launch on bit 17);
// +0x340 a contact (the car, or a bike: ReleaseContact and, on flagsB bit 17, the launch 0x80084BE8) or
// 0; +0x268. The player's edge record 0x800D43C0 + 28 h (a live flag, the key +0 = the bike's +0x168, a
// point beside the bike and a unit normal) for player 0 (and 1 of two); on one case in five the margin
// bytes of the player's slice are rewritten (0 -> the 0x8003E338 junction lookup; small margins) -
// DATA EDITS of the road, said plainly. dt drawn as the step rows draw it; the Rand seed.
// NAMED BOUNDS: no passenger (+0x358 = 0: as rider_pass_thrown, a planted one sends the pass's own
// passenger code into states it never builds - so the tumble-centre passenger MulAdds are uncovered);
// the rows and headings stay unit (Normalize / Asin consumers); rider stances are left as captured.
struct PoolCallees final : rr::sim::PoolLoopCallees {
    NativeEnv& env;
    EnRun& run;
    const rr::sim::BikeTables& t;
    PoolCallees(NativeEnv& e, EnRun& r, const rr::sim::BikeTables& tt) : env(e), run(r), t(tt) {}
    uint32_t Oracle(uint32_t fn, std::initializer_list<uint32_t> args, uint32_t sp) {
        uint32_t a[8] = {};
        int n = 0;
        for (uint32_t x : args) a[n++] = x;
        return bs::ExactOracle(env, fn, a, n, sp);
    }
    bool ListMigrate(uint32_t e, int32_t, uint32_t sp) override {
        bs::MigrateCallees mc(env, t);
        rr::sim::GuestRam g = bs::StepView(env);
        return rr::sim::BikeListMigrate(g, e, sp, t, mc) && env.failure.empty();
    }
    bool JunctionMargin(uint32_t e, uint32_t sp, uint32_t& v0) override {
        v0 = Oracle(rr::sim::react::kJunctionMargin, {e}, sp);
        return env.failure.empty();
    }
    bool ImpactTurn(uint32_t e, uint32_t partner, uint32_t n, int32_t mode, uint32_t sp, uint32_t& v0) override {
        v0 = Oracle(rr::sim::coll::kImpactTurn, {e, partner, n, static_cast<uint32_t>(mode)}, sp);
        return env.failure.empty();
    }
    bool PlaySound3D(int32_t x, int32_t z, int32_t id, int32_t bank) override {
        rr::sim::GuestPlaySound3D(run.s, x, z, id, bank);
        return !run.s.Faulted();
    }
    bool RecoverEnd(uint32_t e, int32_t, uint32_t sp) override {
        bs::MigrateCallees mc(env, t);
        rr::sim::GuestRam g = bs::StepView(env);
        return rr::sim::BikeRecoverEnd(g, e, sp, t, mc) && env.failure.empty();
    }
    bool Launch(uint32_t e, int32_t k, uint32_t sp) override {
        Oracle(rr::sim::react::kLaunch, {e, static_cast<uint32_t>(k)}, sp);
        return env.failure.empty();
    }
    bool ReleaseContact(uint32_t e) override {
        Memory& m = env.clone->mem;
        uint8_t* ep = bs::RawAt(m, e, 1096);
        if (ep == nullptr) return false;
        const rr::sim::BikeLinks L = bs::RawLinks(m, e);
        return rr::sim::ReleaseContact(rr::sim::EntityView(ep), L.contact);
    }
    bool RoadRebind(uint32_t e, int32_t flag, uint32_t sp) override {
        Oracle(rr::sim::react::kRoadRebind, {e, static_cast<uint32_t>(flag)}, sp);
        return env.failure.empty();
    }
    bool GroundFrame(uint32_t e, uint32_t ref, uint32_t sp) override {
        Oracle(rr::sim::react::kGroundFrame, {e, ref}, sp);
        return env.failure.empty();
    }
    bool SteerLean(uint32_t e, uint32_t sp) override {
        Oracle(rr::sim::react::kSteerLean, {e}, sp);
        return env.failure.empty();
    }
    bool TurnFacing(uint32_t e, int32_t ang, uint32_t sp) override {
        Oracle(rr::sim::react::kTurnFacing, {e, static_cast<uint32_t>(ang)}, sp);
        return env.failure.empty();
    }
};

void SetupPoolLoop(CaseContext& c, Args& a) {
    Rng& rng = *c.rng;
    Memory& m = *c.mem;
    const uint32_t pool = Pool0(m, 0);
    auto unit = [&](uint32_t at) {
        int16_t v[3];
        co::Dir(rng, v);
        PutVec3S16(m, at, v);
    };
    auto around1 = [&]() { return Pick(rng, {0x10000u, 0xF333u, 0x10001u, 0x8000u, rng.U32() % 0x20000u, rng.U32()}); };
    for (uint32_t s = 0; s < 18; ++s) {
        const uint32_t e = pool + 1096u * s;
        if (!Chance(rng, 2)) continue;
        uint32_t fc = m.PeekWord(e + 568);
        if (Chance(rng, 3)) {
            fc |= 0x02000000u;
            PutS32(m, e + 0x35C, static_cast<int32_t>(rng.U32() % 0x600000u));
            unit(e + 0x360);
        }
        if (Chance(rng, 5)) fc |= 0x08000000u;
        if (Chance(rng, 14)) fc |= Pick(rng, {0x800u, 0x1000u});
        if (Chance(rng, 3)) fc = (fc & ~0x600u) | Pick(rng, {0x200u, 0x400u, 0x600u});
        if (Chance(rng, 4)) fc = (fc & ~0xFu) | (rng.U32() & 0xFu);
        if (Chance(rng, 5)) fc ^= 0x100u;
        if (Chance(rng, 3)) fc |= 0x00800000u;
        if (Chance(rng, 3)) fc |= 0x04000000u;
        m.PokeWord(e + 568, fc);
        if (Chance(rng, 10)) WriteU16Bench(m, e + 320, 0);
        const uint32_t r = m.PeekWord(e + 852);
        if (r >= 0x80000000u && r < 0x80200000u && Chance(rng, 4)) m.PokeWord(r + 552, m.PeekWord(r + 552) ^ 0x80000u);
        if (Chance(rng, 4)) {
            uint8_t b = m.PeekByte(e + 928);
            b ^= 0x40u;
            m.WriteBlock(e + 928, &b, 1);
        }
        if (Chance(rng, 2)) m.PokeWord(e + 388, m.PeekWord(e + 388) | 1u);
        if (Chance(rng, 6)) m.PokeWord(e + 388, m.PeekWord(e + 388) & ~1u);
        if (Chance(rng, 6)) m.PokeWord(e + 372, 0);   // +0x174 is the road piece POINTER: none, or its own
        if (Chance(rng, 5)) m.PokeWord(e + 36, m.PeekWord(e + 36) ^ 0x200u);
        const uint32_t slice = m.PeekWord(e + 340);
        if (slice >= 0x80000000u && slice < 0x80200000u && Chance(rng, 3) ? true : false) {
            const int32_t lim = static_cast<int32_t>(Chance(rng, 2) ? m.PeekByte(slice + 48) : m.PeekByte(slice + 49)) << 15;
            const int32_t sgn = Chance(rng, 2) ? 1 : -1;
            PutS32(m, e + 344, sgn * lim + static_cast<int32_t>(rng.U32() % 0x20000u) - 0x10000);
        } else if (Chance(rng, 6)) {
            PutS32(m, e + 344, rng.S32() >> (8 + rng.Next() % 8u));
        }
        if (Chance(rng, 3)) unit(e + 450);
        uint32_t fa = m.PeekWord(e + 560);
        if (Chance(rng, 4)) fa ^= 0x2000u;
        if (Chance(rng, 4)) fa ^= 0x08000000u;
        m.PokeWord(e + 560, fa);
        if (Chance(rng, 6)) PutS32(m, e + 480, 0);
        if (Chance(rng, 3)) m.PokeWord(e + 576, rng.U32() % 0x600000u);
        if (Chance(rng, 3)) m.PokeWord(e + 676, static_cast<uint32_t>(rng.S32() >> (12 + rng.Next() % 8u)));
        if (Chance(rng, 3)) m.PokeWord(e + 696, around1());
        if (Chance(rng, 3)) m.PokeWord(e + 700, around1());
        uint32_t fb = m.PeekWord(e + 564);
        if (Chance(rng, 3)) fb = (fb & ~0x38000u) | Pick(rng, {0x8000u, 0x8000u, 0x28000u, 0x10000u, 0x18000u, 0x20000u});
        m.PokeWord(e + 564, fb);
        if (Chance(rng, 6)) m.PokeWord(e + 832, 0);
        else if (Chance(rng, 3)) m.PokeWord(e + 832, Pick(rng, {0x800CF660u + 172u, pool + 1096u * (rng.U32() % 18u) + 172u}));   // a contact: the car, or a bike
        if (Chance(rng, 3)) m.PokeWord(e + 616, Chance(rng, 3) ? 0u : static_cast<uint32_t>(rng.S32() >> (12 + rng.Next() % 6u)));
        m.PokeWord(e + 856, 0);
    }
    // the players' edge records
    const uint32_t players = Chance(rng, 4) ? 2u : 1u;
    co::SetNumPlayers(m, players);
    for (uint32_t h = 0; h < players; ++h) {
        if (!Chance(rng, 3)) continue;
        const uint32_t e = pool + 1096u * h;
        const uint32_t rec = 0x800D43C0u + 28u * h;
        WriteU16Bench(m, rec + 22, static_cast<uint16_t>(Pick(rng, {1u, 0u, rng.U32() & 0xFFFFu})));
        m.PokeWord(rec, Chance(rng, 5) ? rng.U32() : m.PeekWord(e + 360));
        for (uint32_t k = 0; k < 3; ++k)
            PutS32(m, rec + 4 + 4 * k, GetS32(m, e + 184 + 4 * k) + static_cast<int32_t>(rng.U32() % 0x80000u) - 0x40000);
        unit(rec + 16);
    }
    // the player's slice margins (a DATA EDIT of the road)
    if (Chance(rng, 3)) {
        m.PokeWord(pool + 388, m.PeekWord(pool + 388) | 1u);
        m.PokeWord(pool + 36, m.PeekWord(pool + 36) & ~0x200u);
        const uint32_t slice = m.PeekWord(pool + 340);
        if (slice >= 0x80000000u && slice < 0x80200000u) {
            for (uint32_t k = 48; k < 50; ++k) {
                const uint8_t b = static_cast<uint8_t>(Pick(rng, {0u, 0u, 1u, 4u, rng.U32() & 0xFFu}));
                m.WriteBlock(slice + k, &b, 1);
            }
        }
    }
    m.PokeWord(c.gp + kRandSeedGpOffset, rng.U32());
    if (c.rootCounter != nullptr) *c.rootCounter = rng.U32() & 0xFFFFu;
    a.a[0] = static_cast<uint32_t>(bs::DrawDt(rng));
}

uint32_t NativePoolLoop(NativeEnv& env, const Args& a) {
    Machine& cl = *env.clone;
    const uint32_t sp = env.sp;
    bs::SpliceStats::Entry& st = bs::Stats().For("rider_pass_pool");
    ++st.cases;
    auto fail = [&env](const std::string& why) {
        if (env.failure.empty()) env.failure = why;
    };
    std::vector<uint8_t> refRam, refSpad;
    bool refReached = false;
    {
        Machine ref;
        bs::CopyMachine(cl, ref);
        ref.cpu.AddHook(rr::sim::kPoolLoopExit, "exit");
        bs::StartCall(ref.cpu, rr::sim::kRiderPassFn, a, sp);
        const Trap t = ref.cpu.Run(kSentinel, env.maxSteps);
        if (t.kind == TrapKind::Hook && t.pc == rr::sim::kPoolLoopExit) {
            refReached = true;
            refRam = ref.mem.ram();
            refSpad = ref.mem.scratchpad();
        } else if (t.kind != TrapKind::Halted) {
            fail("the reference run of the original trapped: " + t.ToString());
            return 0;
        }
    }
    cl.cpu.ClearHooks();
    cl.cpu.AddHook(rr::sim::kPoolLoopEntry, "entry");
    bs::StartCall(cl.cpu, rr::sim::kRiderPassFn, a, sp);
    int entries = 0;
    for (;;) {
        cl.cpu.callObservations.clear();
        const Trap t = cl.cpu.Run(kSentinel, env.maxSteps);
        env.calls.insert(env.calls.end(), cl.cpu.callObservations.begin(), cl.cpu.callObservations.end());
        cl.cpu.callObservations.clear();
        if (t.kind == TrapKind::Halted) break;
        if (t.kind != TrapKind::Hook || t.pc != rr::sim::kPoolLoopEntry) {
            fail("the spliced run trapped: " + t.ToString());
            break;
        }
        if (++entries > 1) {
            fail("the region's entry was reached twice in one pass");
            break;
        }
        ++st.entered;
        const CpuRegs saved = CaptureRegs(cl.cpu);
        const uint32_t frame = cl.cpu.regs[29];
        const int32_t dt = GetS32(cl.mem, frame + rr::sim::kRiderPassFrame);
        if (!env.spliceControl) {
            EnRun run(env);
            rr::sim::GuestRam g(cl.mem.ram().data(), env.gp);
            g.SetScratchpad(cl.mem.scratchpad().data());
            const bs::StepTables tab(cl.mem, env.gp);
            PoolCallees pc(env, run, tab.t);
            const bool ok = rr::sim::RiderPassPoolLoop(g, dt, frame, tab.t, pc);
            if (run.s.failed) {
                fail("the sound port could not serve PlaySound3D");
                break;
            }
            if (!ok) {
                char buf[128];
                std::snprintf(buf, sizeof(buf), "the port refused the region (view fault 0x%08X)", g.FaultAddress());
                fail(buf);
                break;
            }
            if (!env.failure.empty()) break;
        }
        RestoreRegs(saved, cl.cpu);
        cl.cpu.pc = rr::sim::kPoolLoopExit;
        cl.cpu.npc = rr::sim::kPoolLoopExit + 4u;
        cl.cpu.loadDelayReg = Cpu::kNoLoadDelay;
        cl.cpu.loadDelayValue = 0;
        if (!refReached) {
            fail("the clone entered the region but the reference never reached its exit");
            break;
        }
        ++st.exitCompared;
        const std::vector<uint8_t>& B = cl.mem.ram();
        size_t differ = 0;
        uint32_t first = 0;
        for (size_t i = 0; i < B.size(); ++i) {
            const uint32_t addr = 0x80000000u + static_cast<uint32_t>(i);
            if (addr >= sp - 4096u && addr <= sp + 19u) continue;
            if (B[i] != refRam[i]) {
                if (differ == 0) first = addr;
                ++differ;
            }
        }
        if (cl.mem.scratchpad() != refSpad) ++differ;
        if (differ != 0) {
            ++st.exitDiffered;
            char buf[160];
            std::snprintf(buf, sizeof(buf),
                          "at the region's exit 0x%08X the spliced state differs from the original's in %zu byte(s), first 0x%08X",
                          rr::sim::kPoolLoopExit, differ, first);
            fail(buf);
            break;
        }
    }
    cl.cpu.ClearHooks();
    if (entries == 0 && refReached) fail("the reference passed the region but the clone never entered it");
    // the observed callees over the WHOLE pass (the region's are the only callers of these in it but the
    // rider layer's two, which the engine part reaches too)
    using namespace rr::sim::react;
    static ReachStats reach("rider_pass_pool (whole pass)",
                            {{kJunctionMargin, "junction"}, {rr::sim::coll::kImpactTurn, "impact-turn"}, {kLaunch, "launch"},
                             {kRoadRebind, "rebind"}, {kGroundFrame, "ground-frame"}, {kSteerLean, "steer-lean"},
                             {kTurnFacing, "turn-facing"}, {bs::kRiderKnockOff, "knock-off"}, {bs::kRiderLaunch, "rider-launch"}});
    reach.Note(env);
    return cl.cpu.regs[2];
}

} // namespace creact

void AddCollReactRows(std::vector<Row>& rows) {
    using namespace rr::sim::react;
    rows.push_back(Row{"side_from_cos", "u32 0x800AC56C(s32 c, u32 fl, u32 other)  [RASHCDG, leaf]", 0x800AC56C,
                       creact::SetupSide, creact::NativeSide});
    rows.push_back(Row{"remember_handle", "s32 0x800A8BE0(u16 h, u32 *set)  [RASHCDG, leaf]", 0x800A8BE0,
                       creact::SetupRemember, creact::NativeRemember});
    {
        rows.push_back(Row{"impact_severity", "s32 0x800A9408(e, mag, num, den, [mode])  [RASHCDG]", 0x800A9408,
                           creact::SetupSeverity, nullptr});
        Row& r = rows.back();
        r.nativeEx = creact::NativeSeverity;
        r.oracleCallees = {OracleCallee{kStanceEvent, 3}};
    }
    {
        rows.push_back(Row{"bike_bike_react", "void 0x800AC130(a, b, codeA, codeB)  [RASHCDG, the gate native]",
                           0x800AC130, creact::SetupBikeReact, nullptr});
        Row& r = rows.back();
        r.compareV0 = false;
        r.nativeEx = creact::NativeBikeReact;
        r.maxSteps = 20000000;
        r.oracleCallees = {OracleCallee{kFaceNormal, 4, 1},         OracleCallee{rr::sim::coll::kImpactTurn, 4},
                           OracleCallee{kHitSpeed, 4, 7},           OracleCallee{kTakePartnerHeading, 4, 1},
                           OracleCallee{kRiderSpeech, 2},           OracleCallee{kStanceEvent, 3},
                           OracleCallee{kGetRCnt, 1},               OracleCallee{rr::sim::coll::kPadMotor, 4}};
    }
    {
        rows.push_back(Row{"traffic_side_shove", "u32 0x800A91AC(code, flags, bike, car, [s32 *imp])  [RASHCDG]", 0x800A91AC,
                           creact::SetupShove, nullptr});
        rows.back().nativeEx = creact::NativeShove;
    }
    const std::vector<OracleCallee> reactCallees = {
        OracleCallee{kImpactGate, 4, 1}, OracleCallee{kFaceNormal, 4, 1}, OracleCallee{kXzDot, 3},
        OracleCallee{kStanceEvent, 3},   OracleCallee{kGetRCnt, 1},       OracleCallee{rr::sim::coll::kPadMotor, 4},
        OracleCallee{kSurfaceFx, 2}};
    {
        rows.push_back(Row{"bike_traffic_react", "void 0x800AC958(bike, car, code, flags, [s32 *imp])  [RASHCDG, one GTE OP]",
                           0x800AC958, creact::SetupTrafficReact, nullptr});
        Row& r = rows.back();
        r.compareV0 = false;
        r.nativeEx = creact::NativeTrafficReact;
        r.maxSteps = 20000000;
        r.oracleCallees = reactCallees;
    }
    {
        rows.push_back(Row{"bike_vs_traffic", "void 0x800AC5BC(bike, car)  [RASHCDG]", 0x800AC5BC, creact::SetupVsTraffic, nullptr});
        Row& r = rows.back();
        r.compareV0 = false;
        r.nativeEx = creact::NativeVsTraffic;
        r.maxSteps = 20000000;
        r.oracleCallees = reactCallees;
        r.oracleCallees.push_back(OracleCallee{kResponse, 4, 2});
        r.oracleCallees.push_back(OracleCallee{kResponseFar, 4, 1});
    }
    {
        rows.push_back(Row{"rider_pass_pool", "region [0x8007B894, 0x8007C23C) of 0x8007B840(s32 dt)  [RASHCDG, a SPLICE: the first pool loop]",
                           0x8007B840, creact::SetupPoolLoop, nullptr});
        Row& r = rows.back();
        r.compareV0 = false;
        r.nativeEx = creact::NativePoolLoop;
        r.maxSteps = 20000000;
        r.oracleCallees = {OracleCallee{kJunctionMargin, 1},  OracleCallee{rr::sim::coll::kImpactTurn, 4},
                           OracleCallee{kLaunch, 2},          OracleCallee{kRoadRebind, 2},
                           OracleCallee{kGroundFrame, 2},     OracleCallee{kSteerLean, 1},
                           OracleCallee{kTurnFacing, 2},      bs::KnockOffCallee(),
                           bs::LaunchCallee()};
    }
}
