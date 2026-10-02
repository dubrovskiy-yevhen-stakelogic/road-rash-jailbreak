// Bench rows of recover_air.h (see rows_recover.inc): the rider pass's per-rider step of a rider off
// the bike (0x8008F404), the flying rider (0x800976C4) and its wall hit (0x800A966C), and the two SLUS
// road helpers the flight reaches (0x8003E338, 0x80038550).
//
// Every case is built on a LIVE rider of this machine (rr-race: the player's rider, pool 1 slot 0, and
// the downed rider of slot 6): the oracle-supplied callees (the ground step, the re-bind, the rows,
// the stance event, the road walkers) walk that rider's cursor, animation object and bike, so a rider
// lifted out of another image would point at state this machine does not hold.
//
// NAMED BOUNDS:
//   * only live riders (+0x140 != 0) with mount +0x25C 2..4 - the rider pass's own gate at 0x8007DD38 /
//     0x8007DD4C: a dormant rider (stance 224) never reaches these functions, and the callees would
//     then run its animation layer on a stance the game never plays;
//   * the heading +0x1C2, the up vector +0x20A, the tumble axes +0x1B0 / +0x1BC and every wall normal
//     stay unit (or shorter) directions: the shake's GTE OP feeds Normalize, whose trapping `add`
//     raises the console's overflow exception for two long int16 vectors (0x8002E4A0);
//   * the stance is a resident one (animrows::Resident) when it is rewritten, as the stance rows keep it.
// DATA EDITS, said plainly: the margin bytes +0x30 / +0x31 of the rider's slice are rewritten one case in
// four (0 sends the flight to the junction lookup 0x8003E338), and the junction core is where a rider is
// re-placed one case in four (its contact point +0x1F8 kept as captured).

namespace airrows {

constexpr uint32_t kOutOff = 0x1800; // a direction / an output triple in the scratch block

bool AirOneIn(Rng& r, uint32_t n) { return r.Next() % n == 0u; }
uint32_t AirPick(Rng& r, std::initializer_list<uint32_t> v) { return *(v.begin() + r.Next() % v.size()); }
bool AirInRam(uint32_t a) { return a >= 0x80000000u && a < 0x80200000u; }
void AirUnit(CaseContext& c, uint32_t at) {
    int16_t v[3];
    cr::RandomDir(*c.rng, v, 4096);
    PutVec3S16(*c.mem, at, v);
}
uint16_t AirHalf(const Memory& m, uint32_t a) {
    uint16_t v = 0;
    m.ReadBlock(a, &v, 2);
    return v;
}

// The live riders of this machine (the riders of the 18 pool-0 bikes whose +0x140 is set).
std::vector<uint32_t> AirLiveRiders(const Memory& m) {
    std::vector<uint32_t> out;
    for (uint32_t k = 0; k < 18; ++k) {
        const uint32_t R = m.PeekWord(pop::Bike(m, k) + 852u);
        if (AirInRam(R) && GetS16(m, R + 320u) != 0) out.push_back(R);
    }
    return out;
}
uint32_t AirRider(CaseContext& c) {
    const std::vector<uint32_t> v = AirLiveRiders(*c.mem);
    if (v.empty()) return c.mem->PeekWord(pop::Bike(*c.mem, 0) + 852u);
    return v[c.rng->U32() % v.size()];
}

// A resident stance of kind 5 (the stance table's +2), or 0 when none is.
uint32_t AirKindFive(CaseContext& c) {
    std::vector<uint32_t> v;
    for (uint32_t s = 0; s < rr::sim::kStanceNone; ++s)
        if (AirHalf(*c.mem, rr::sim::kRcStanceTable + 8u * s + 2u) == 5u && animrows::Resident(*c.mem, s)) v.push_back(s);
    return v.empty() ? 0u : v[c.rng->U32() % v.size()];
}

// A rewritten stance, with the rider's animation object put in that stance's bank (the invariant the
// stance gate's bank switch keeps, as animrows::PlantStanceCase does: a clip index is read in the bank
// of the stance that named it) and its program and clock redrawn in that bank when the bank changed.
void AirSetStance(CaseContext& c, uint32_t R, uint32_t st) {
    Memory& m = *c.mem;
    WriteU16Bench(m, R + 544u, static_cast<uint16_t>(st));
    const uint32_t anim = m.PeekWord(R + 540u);
    if (!AirInRam(anim)) return;
    const uint32_t want = animrows::BankPtr(m, m.PeekWord(rr::sim::kRcStanceTable + 8u * st) & 0xFu);
    if (m.PeekWord(anim + 40u) == want) return;
    m.PokeWord(anim + 40u, want);
    animrows::RandomProgram(c, anim, true);
    animrows::RandomClock(c, anim);
    // NAMED BOUND: the program counter +0x0C names one of the program's five ops. RandomClock draws
    // 5..7 one time in ten (the animation rows' Seq guard); a live rider's running op is one of the
    // five, and RiderLand's event-49 arm reads the running op's clip byte at +4 -> [12 * pc]
    // (0x800B2520..0x800B2540) - past the program that byte is not a clip of the bank, and the
    // ORIGINAL trapped at 0x800B255C (lhu through the clip table's word): measured 1 case of
    // --seed 0xA5A5C3C312345678 --cases 2048 before this bound.
    m.PokeWord(anim + 12u, m.PeekWord(anim + 12u) % 5u);
}

uint32_t AirDrive(Rng& r, uint32_t w, uint32_t bit, uint32_t oneIn) {
    if (!AirOneIn(r, oneIn)) return w;
    return (r.Next() & 1u) ? (w | bit) : (w & ~bit);
}

// The rider re-placed on the junction core of this machine (RbPlace, rows_road_runtime.inc), its
// +0x1F8..+0x203 kept as captured (RbPlace writes a bike's contact point there).
void AirPlaceOnCore(CaseContext& c, uint32_t e, bool keepContact) {
    Memory& m = *c.mem;
    const RqNet n = RqRead(m);
    const RbCore k = RbFindCore(c, n);
    if (k.obj == 0) return;
    const std::vector<uint32_t> cores = RbPieces(m, k.obj, true);
    if (cores.empty()) return;
    uint8_t keep[12];
    m.ReadBlock(e + 0x1F8u, keep, 12);
    RbPlace(c, e, k.obj, cores[c.rng->U32() % cores.size()], k.ipt, RbCoreLateral(c, k.obj));
    if (keepContact) m.WriteBlock(e + 0x1F8u, keep, 12);
}

// Everything the flight (and the step, through it) branches on, over rider R.
void AirDrawFlight(CaseContext& c, uint32_t R) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    if (AirOneIn(r, 4)) AirPlaceOnCore(c, R, true);
    uint32_t f = m.PeekWord(R + 552u);
    f = AirDrive(r, f, 0x80000u, 3);                  // bit 19: the wall tests skipped
    f ^= r.U32() & 0x80060000u;                    // bits 17, 18, 31: cleared at the end
    m.PokeWord(R + 552u, f);
    // the rider's road state: +0x184 bit 0, the piece +0x174, +0x24 bit 9, the lateral, the margins
    m.PokeWord(R + 388u, (r.Next() & 1u) ? (m.PeekWord(R + 388u) | 1u) : (m.PeekWord(R + 388u) & ~1u));
    if (AirOneIn(r, 6)) m.PokeWord(R + 372u, 0);
    else if (m.PeekWord(R + 372u) == 0 || AirOneIn(r, 3)) {
        const uint32_t grp = RrSomeGroup(c, m.PeekWord(R + 328u));
        if (grp != 0) m.PokeWord(R + 372u, grp);
    }
    if (AirOneIn(r, 5)) m.PokeWord(R + 36u, m.PeekWord(R + 36u) ^ 0x200u);
    const uint32_t slice = m.PeekWord(R + 340u);
    if (AirInRam(slice) && AirOneIn(r, 4))
        for (uint32_t k = 48; k < 50; ++k) pop::Put8(m, slice + k, AirPick(r, {0u, 0u, 1u, 4u, r.U32() & 0xFFu}));
    if (AirInRam(slice) && AirOneIn(r, 2)) {
        const int32_t lim = static_cast<int32_t>((r.Next() & 1u) ? m.PeekByte(slice + 48u) : m.PeekByte(slice + 49u)) << 15;
        const int32_t sgn = (r.Next() & 1u) ? 1 : -1;
        PutS32(m, R + 344u, sgn * lim + static_cast<int32_t>(r.U32() % 0x20000u) - 0x10000);
    } else if (AirOneIn(r, 4)) {
        PutS32(m, R + 344u, r.S32() >> (8 + r.Next() % 10u));
    }
    // along +0x15C (the edge probe's fraction): around the slice's chord, or anything
    if (AirInRam(slice) && AirOneIn(r, 2)) {
        const int32_t chord = std::max(1, GetS32(m, slice + 32u));
        PutS32(m, R + 348u, static_cast<int32_t>(r.U32() % static_cast<uint32_t>(std::min(chord, 0x2AAAAAAA) * 3 + 1)) - chord);
    } else if (AirOneIn(r, 5)) {
        PutS32(m, R + 348u, r.S32() >> (r.Next() % 16u));
    }
    // the player's edge record 0x800D43C0 + 28 h, h = the rider's bike's handle (players' records only)
    const uint32_t B = m.PeekWord(R + 596u);
    const uint16_t hb = AirInRam(B) ? AirHalf(m, B + 172u) : 0xFFFFu;
    if (hb < 2u && !AirOneIn(r, 4)) {
        const uint32_t rec = rr::sim::kAirEdgeRecords + rr::sim::kAirEdgeStride * hb;
        WriteU16Bench(m, rec + 22u, static_cast<uint16_t>(AirPick(r, {1u, 1u, 0u, r.U32() & 0xFFFFu})));
        m.PokeWord(rec, AirOneIn(r, 5) ? r.U32() : m.PeekWord(R + 360u));
        // a point within about 70 units: the octagonal distance straddles the 50-unit gate
        for (uint32_t k = 0; k < 3; ++k)
            PutS32(m, rec + 4u + 4u * k,
                   GetS32(m, R + 184u + 4u * k) + static_cast<int32_t>(r.U32() % (140u << 16)) - (70 << 16));
        if (AirOneIn(r, 3)) PutS32(m, rec + 4u, GetS32(m, R + 184u) + static_cast<int32_t>(r.U32() % 0x40000u) - 0x20000);
        AirUnit(c, rec + 16u);
    }
    // the heading, the up vector, the velocity and its drag
    if (!AirOneIn(r, 3)) AirUnit(c, R + 450u);
    if (AirOneIn(r, 3)) AirUnit(c, R + 522u);
    for (uint32_t k = 0; k < 3; ++k)
        if (!AirOneIn(r, 4)) PutS32(m, R + 456u + 4u * k, r.S32() >> (8 + r.Next() % 12u));
    if (AirOneIn(r, 8)) for (uint32_t k = 0; k < 3; ++k) PutS32(m, R + 456u + 4u * k, 0);
    m.PokeWord(R + 580u, AirPick(r, {0u, 0x10000u, r.U32() % 0x20000u, r.U32() % 0x4000u, r.U32() >> 12}));
}

// The common part: a live rider off the bike, the player count, the seed.
uint32_t AirDrawRider(CaseContext& c) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    pop::DrawPlayers(c);
    const uint32_t R = AirRider(c);
    m.PokeWord(R + 604u, 2u + r.U32() % 3u);
    m.PokeWord(c.gp + kRandSeedGpOffset, r.U32());
    return R;
}

// ---------------------------------------------------------------- RASHCDG 0x8008F404
// Driven: the stance's kind (5: everything but the view part skipped) and stance 43 (the view bits
// cleared), +0x228 bits 1 (the re-bind), 3 / 4, 26 and 30 (the flight or the ground step), the player
// count (a player's rider or not), the view record's +0x224 bits 17 (the gate), 18, 19, 31 (the side
// draw's mask) and its accumulator +0x318 at and around 0x8000 - dt, the speed +0x1E0 around 0xC0000
// and 3 x 0xC0000 (the floor, and the third of it), and everything the flight reads.
void SetupGroundStep(CaseContext& c, Args& a) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    const uint32_t R = AirDrawRider(c);
    const uint64_t q = r.Next() % 8u;
    if (q == 0) {
        const uint32_t st = AirKindFive(c);
        if (st != 0) AirSetStance(c, R, st);
    } else if (q == 1 && animrows::Resident(m, 43)) {
        AirSetStance(c, R, 43);
    }
    AirDrawFlight(c, R);
    uint32_t f = m.PeekWord(R + 552u);
    f = AirDrive(r, f, 0x2u, 2);
    f = AirDrive(r, f, (r.Next() & 1u) ? 0x8u : 0x10u, 6);
    f = AirDrive(r, f, 0x04000000u, 2);
    f = AirDrive(r, f, 0x40000000u, 2);
    m.PokeWord(R + 552u, f);
    const int32_t dt = bs::DrawDt(r);
    const uint32_t B = m.PeekWord(R + 596u);
    const uint16_t hb = AirInRam(B) ? AirHalf(m, B + 172u) : 0xFFFFu;
    if (hb < 2u) {
        const uint32_t view = rr::sim::kRcViewArray + rr::sim::kRcViewStride * hb;
        uint32_t w = m.PeekWord(view + 548u);
        w = AirOneIn(r, 4) ? (w & ~0x20000u) : (w | 0x20000u);
        w ^= r.U32() & 0x800C0000u;
        m.PokeWord(view + 548u, w);
        uint32_t acc = 0;
        switch (r.Next() % 4u) {
            case 0: acc = 0x8000u - static_cast<uint32_t>(dt) + (r.U32() % 3u) - 1u; break;
            case 1: acc = r.U32() % 0x9000u; break;
            case 2: acc = 0x8001u + r.U32() % 0x1000u; break;
            default: acc = r.U32(); break;
        }
        m.PokeWord(view + 792u, acc);
    }
    m.PokeWord(R + 480u, AirPick(r, {0xBFFFFu, 0xC0000u, 0xC0001u, 0x23FFFFu, 0x240000u, 0x240003u,
                                    r.U32() % 0x800000u, r.U32()}));
    a.a[0] = R;
    a.a[1] = static_cast<uint32_t>(dt);
}

uint32_t NativeGroundStep(NativeEnv& env, const Args& a) {
    rr::sim::GuestRam g = rcv::View(env);
    const rr::sim::BikeTables t = rcv::Tables(env, g);
    rcv::RecoverOracle c(env);
    const bool ok = rr::sim::RiderGroundStep(g, a.a[0], static_cast<int32_t>(a.a[1]), env.sp, t, c);
    return rcv::Done(env, g, ok, "RiderGroundStep");
}

// ---------------------------------------------------------------- RASHCDG 0x800976C4
// Driven: +0x228 bit 19 (the wall tests) and bits 17 / 18 / 31 (cleared at the end), the player count,
// the player's edge record (live word, key, a point in and out of the 50-unit gate, the normal against
// the heading), +0x184 bit 0, +0x174, +0x24 bit 9, the lateral at and around the slice's margins, the
// margin bytes (0: the junction lookup), the rider on the junction core, the velocity and its drag
// +0x244, dt, the up vector the edge probe may flip.
void SetupAirStep(CaseContext& c, Args& a) {
    const uint32_t R = AirDrawRider(c);
    AirDrawFlight(c, R);
    a.a[0] = R;
    a.a[1] = static_cast<uint32_t>(bs::DrawDt(*c.rng));
}

uint32_t NativeAirStep(NativeEnv& env, const Args& a) {
    rr::sim::GuestRam g = rcv::View(env);
    const rr::sim::BikeTables t = rcv::Tables(env, g);
    rcv::RecoverOracle c(env);
    const bool ok = rr::sim::RiderAirStep(g, a.a[0], static_cast<int32_t>(a.a[1]), env.sp, t, c);
    return rcv::Done(env, g, ok, "RiderAirStep");
}

// ---------------------------------------------------------------- RASHCDG 0x800A966C
// Driven: the wall normal (a unit direction in the scratch block, the slice's own row, or the edge
// record's), the tumble axis +0x1B0 against it (the 0xB332 gate), +0x1BC against the heading (the
// flip), the speed +0x1E0 and the velocity (Bounce's arms), k. The rider's handle +0xAC is moved to
// another pool one case in five: the function reads it only for the "a rider's own" flag Bounce gets,
// and a rider always passes that test, so this is the one way the flag's other arm runs.
void SetupAirHit(CaseContext& c, Args& a) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    const uint32_t R = AirDrawRider(c);
    AirUnit(c, R + 432u);
    if (!AirOneIn(r, 4)) AirUnit(c, R + 444u);
    if (!AirOneIn(r, 3)) AirUnit(c, R + 450u);
    for (uint32_t k = 0; k < 3; ++k) PutS32(m, R + 456u + 4u * k, r.S32() >> (8 + r.Next() % 12u));
    m.PokeWord(R + 480u, AirPick(r, {0u, r.U32() % 0x100000u, r.U32() % 0x800000u, r.U32() >> 4}));
    uint32_t dir = c.scratch + kOutOff;
    const uint64_t d = r.Next() % 4u;
    if (d == 0 && AirInRam(m.PeekWord(R + 340u))) {
        dir = m.PeekWord(R + 340u) + 2u;
    } else {
        AirUnit(c, dir);
        if (d == 1) { // the axis itself, or its reverse: |dot| past the gate
            for (uint32_t k = 0; k < 3; ++k) {
                const int16_t v = GetS16(m, R + 432u + 2u * k);
                PutS16(m, dir + 2u * k, (r.Next() & 1u) ? v : static_cast<int16_t>(-v));
            }
        }
    }
    if (AirOneIn(r, 5)) {
        const uint16_t h = AirHalf(m, R + 172u);
        WriteU16Bench(m, R + 172u, static_cast<uint16_t>((AirPick(r, {0u, 2u, 3u}) << 5) | (h & 0x1Fu)));
    }
    a.a[0] = R;
    a.a[1] = dir;
    a.a[2] = AirOneIn(r, 4) ? r.U32() % 0x100000u : 0x80000u;
}

uint32_t NativeAirHit(NativeEnv& env, const Args& a) {
    rr::sim::GuestRam g = rcv::View(env);
    const rr::sim::BikeTables t = rcv::Tables(env, g);
    rcv::RecoverOracle c(env);
    uint32_t v0 = 0;
    const bool ok = rr::sim::RiderAirHit(g, a.a[0], a.a[1], static_cast<int32_t>(a.a[2]), env.sp, t, c, v0);
    return rcv::Done(env, g, ok, "RiderAirHit", v0);
}

// ---------------------------------------------------------------- SLUS 0x8003E338
// Driven: e = 0, a live rider or one of this machine's 18 pool-0 bikes; its +0x140 (0: no lookup); its
// cursor re-placed on the junction core (whose object carries the +0x68 arm-pair list) with the box
// centre on it, re-seated on any resident object (+0x68 = 0, or no BIT_ record), or its object cleared;
// the stack under the call (NodeWedge's outputs are read only when it answers >= 0).
void SetupJunctionMargin(CaseContext& c, Args& a) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    const bool rider = AirOneIn(r, 3);
    uint32_t e = rider ? AirRider(c) : pop::Bike(m, r.U32() % 18u);
    switch (r.Next() % 6u) {
        case 0: case 1: case 2: AirPlaceOnCore(c, e, rider); break;
        case 3: RqPlantCursor(c, RqRead(m), e + 0x148u); break;
        case 4: if (AirOneIn(r, 3)) m.PokeWord(e + 328u, 0); break;
        default: break;
    }
    if (AirOneIn(r, 10)) WriteU16Bench(m, e + 320u, 0);
    else if (GetS16(m, e + 320u) == 0 && !AirOneIn(r, 4)) WriteU16Bench(m, e + 320u, 1);
    if (c.sample == nullptr) RqStackJunk(c, 256);
    if (AirOneIn(r, 16)) e = 0;
    a.a[0] = e;
}

uint32_t NativeJunctionMargin(NativeEnv& env, const Args& a) {
    rr::sim::GuestRam g = rcv::View(env);
    const rr::sim::BikeTables t = rcv::Tables(env, g);
    rcv::RecoverOracle c(env);
    uint32_t v0 = 0;
    const bool ok = rr::sim::JunctionMargin(g, a.a[0], env.sp, t, c, v0);
    return rcv::Done(env, g, ok, "JunctionMargin", v0);
}

// ---------------------------------------------------------------- SLUS 0x80038550
// Driven: e a live rider (out = its +0x20A, or a scratch triple) or a pool-0 bike (out in the scratch
// block); its cursor +0x148 re-seated on a resident object's first, last or any slice (the first / last
// take the missing-road check 0x800394F0), or on the junction core; along around the slice's chord
// (the fraction's clamp at 0 and 1.0) or anything; lateral (the tangent's correction) drawn; the stack
// under the call (the neighbour walker's partial candidates read it).
void SetupRoadEdgeProbe(CaseContext& c, Args& a) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    const bool rider = !AirOneIn(r, 3);
    const uint32_t e = rider ? AirRider(c) : pop::Bike(m, r.U32() % 18u);
    switch (r.Next() % 6u) {
        case 0: case 1: case 2: RqPlantCursor(c, RqRead(m), e + 0x148u); break;
        case 3: AirPlaceOnCore(c, e, rider); break;
        default: break;
    }
    const uint32_t slice = m.PeekWord(e + 0x154u);
    int32_t along = GetS32(m, e + 0x15Cu);
    if (AirInRam(slice) && !AirOneIn(r, 4)) {
        const int32_t chord = std::max(1, GetS32(m, slice + 32u));
        along = static_cast<int32_t>(r.U32() % static_cast<uint32_t>(std::min(chord, 0x2AAAAAAA) * 3 + 1)) - chord;
    } else if (AirOneIn(r, 2)) {
        along = r.S32() >> (r.Next() % 16u);
    }
    int32_t lateral = GetS32(m, e + 0x158u);
    if (!AirOneIn(r, 3)) lateral = r.S32() >> (8 + r.Next() % 12u);
    uint32_t out = c.scratch + kOutOff;
    if (rider && !AirOneIn(r, 4)) out = e + 522u;
    else for (uint32_t k = 0; k < 8; k += 4) m.PokeWord(out + k, r.U32());
    if (c.sample == nullptr) RqStackJunk(c, 512);
    a.a[0] = e;
    a.a[1] = static_cast<uint32_t>(along);
    a.a[2] = static_cast<uint32_t>(lateral);
    a.a[3] = e + 0x148u;
    a.a4 = out;
}

uint32_t NativeRoadEdgeProbe(NativeEnv& env, const Args& a) {
    rr::sim::GuestRam g = rcv::View(env);
    const rr::sim::BikeTables t = rcv::Tables(env, g);
    rcv::RecoverOracle c(env);
    uint32_t v0 = 0;
    const bool ok = rr::sim::RoadEdgeProbe(g, a.a[0], static_cast<int32_t>(a.a[1]), static_cast<int32_t>(a.a[2]),
                                           a.a[3], a.a4, env.sp, t, c, v0);
    return rcv::Done(env, g, ok, "RoadEdgeProbe", v0);
}

// ---------------------------------------------------------------- RASHCDG 0x800B208C
// The flying rider at the ground contact, as the collision pass's kind 1/2 tail calls it: +0x228 bit 30
// set, bit 31 clear, the up vector +0x20A and the heading +0x1C2 with a positive dot. The eight
// corners +0xC4 are built around the box centre along the rider's own up / heading / side (random
// half-extents), the contact point +0x1F8 near them (so CornerMin 0x800B6F40 answers a corner, or 8
// when none is behind the plane); the depth reaches past 5.0 both ways (the stuck arm's 0x50000).
// Driven: +0x216 (4: stuck), +0x228 bits 19 and 26, +0x217 (the landing count) and the speed +0x1E0
// around 0x6B4A1 (the fast bounce), the up vector's y around 2632 (the steep bounce), the stance
// (43 / 46 / 47 / 48 / 49 and the lying stances; resident ones only), +0x23C bits 5 / 6 and the
// rumble switch *(0x8005B220), the player count, the forward row +0x210 against the heading, the
// tumble axis +0x1B6, and everything the flight draws (the edge probe's inputs). One case in eight
// the handle is moved to pool 2: the tail's kind 2 is not a pool-1 rider, and the function's "a
// rider" register s2 is then 0 (+0x1E8 written instead of +0x258, no rumble, no view clock).
void SetupRiderLand(CaseContext& c, Args& a) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    const uint32_t R = AirDrawRider(c);
    AirDrawFlight(c, R);
    // the stance
    {
        const uint64_t q = r.Next() % 10u;
        uint32_t st = 0;
        if (q < 5) st = AirPick(r, {43u, 46u, 46u, 47u, 48u, 49u});
        else if (q < 8) st = 44u + r.U32() % 16u;
        // NAMED BOUND: the object in the stance's bank (AirSetStance). Measured before it: the GUEST
        // trapped in 2 of 1158 default-seed cases and 7 on --seed 0xA5A5C3C312345678 --cases 2048, at
        // 0x8005E2C8 (the DMD3 header decode under ClipSelect 0x8005C39C) - the landing's RangedStart
        // (event 49: the running op's clip byte and stance 49's clip) read in a bank the rewritten
        // stance never had, so a clip pointer came out of the bank's own header text.
        if (st != 0 && animrows::Resident(m, st)) AirSetStance(c, R, st);
    }
    // the flags: flying (bit 30), not bit 31; bit 26 (stuck), bit 19 (the view clock)
    uint32_t f = (m.PeekWord(R + 552u) | 0x40000000u) & 0x7FFFFFFFu;
    f = AirDrive(r, f, 0x04000000u, 3);
    f = AirDrive(r, f, 0x80000u, 2);
    m.PokeWord(R + 552u, f);
    pop::Put8(m, R + 534u, AirOneIn(r, 5) ? 4u : AirPick(r, {0u, 1u, 2u, 3u, 5u, r.U32() & 0xFFu}));
    pop::Put8(m, R + 535u, AirPick(r, {0u, 0u, 1u, 2u, 0xFFu, r.U32() & 0xFFu}));
    m.PokeWord(R + 480u, AirPick(r, {0x6B4A0u, 0x6B4A1u, 0x6B4A2u, r.U32() % 0x100000u, r.U32() % 0x400000u,
                                     r.U32() >> (4 + r.Next() % 12u)}));
    pop::Put8(m, R + 572u, (m.PeekByte(R + 572u) & ~0x60u) | AirPick(r, {0u, 0x20u, 0x20u, 0x40u, 0x60u}));
    m.PokeWord(0x8005B220u, AirOneIn(r, 4) ? 1u : 0u);
    // the geometry: up (y around 2632 one case in three), heading with a positive dot, forward, axis
    int16_t up[3];
    cr::RandomDir(r, up, 4096);
    if (AirOneIn(r, 3)) {
        up[1] = static_cast<int16_t>(2400 + static_cast<int32_t>(r.U32() % 500u));
        const int32_t rest = 4096 * 4096 - up[1] * up[1];
        int32_t s = 0;
        while ((s + 1) * (s + 1) <= rest) ++s;
        const int32_t k = static_cast<int32_t>(r.U32() % 1001u);
        up[0] = static_cast<int16_t>(s * k / 1000);
        up[2] = static_cast<int16_t>(-s * (1000 - k) / 1000);
    } else if (!AirOneIn(r, 3)) {
        if (up[1] < 0) up[1] = static_cast<int16_t>(-up[1]);
    }
    PutVec3S16(m, R + 522u, up);
    int16_t hd[3];
    for (int t = 0; t < 8; ++t) {
        cr::RandomDir(r, hd, 4096);
        const int64_t dot = static_cast<int64_t>(up[0]) * hd[0] + static_cast<int64_t>(up[1]) * hd[1] +
                            static_cast<int64_t>(up[2]) * hd[2];
        if ((dot >> 8) > 0) break;
        if ((dot >> 8) < 0) {
            for (int k = 0; k < 3; ++k) hd[k] = static_cast<int16_t>(-hd[k]);
            break;
        }
    }
    PutVec3S16(m, R + 450u, hd);
    AirUnit(c, R + 528u);
    if (AirOneIn(r, 4)) for (uint32_t k = 0; k < 6; k += 2) WriteU16Bench(m, R + 528u + k, AirHalf(m, R + 450u + k));
    AirUnit(c, R + 438u);
    // the corners around the box centre, the contact point near them
    int32_t box[3];
    for (uint32_t k = 0; k < 3; ++k) box[k] = GetS32(m, R + 184u + 4u * k);
    const int32_t hx = static_cast<int32_t>(r.U32() % 0x30000u), hy = static_cast<int32_t>(r.U32() % 0x40000u),
                  hz = static_cast<int32_t>(r.U32() % 0x30000u);
    for (uint32_t i = 0; i < 8; ++i) {
        const int32_t sx = (i & 1u) ? hx : -hx, sy = (i & 4u) ? -hy : hy, sz = (i & 2u) ? hz : -hz;
        for (uint32_t k = 0; k < 3; ++k) {
            // along up (y offset), heading (z offset) and a rough side (x offset)
            const int64_t v = static_cast<int64_t>(box[k]) + ((static_cast<int64_t>(up[k]) * sy) >> 12) +
                              ((static_cast<int64_t>(hd[k]) * sz) >> 12) + (k == 0 ? sx : (k == 2 ? -sx / 2 : 0));
            PutS32(m, R + 196u + 12u * i + 4u * k, static_cast<int32_t>(v));
        }
    }
    {
        const int32_t along = static_cast<int32_t>(r.U32() % 0x100000u) - 0x80000; // along the up vector
        for (uint32_t k = 0; k < 3; ++k) {
            const int64_t v = static_cast<int64_t>(box[k]) + ((static_cast<int64_t>(up[k]) * along) >> 12) +
                              static_cast<int32_t>(r.U32() % 0x20000u) - 0x10000;
            PutS32(m, R + 504u + 4u * k, static_cast<int32_t>(v));
        }
    }
    if (AirOneIn(r, 8)) {
        const uint16_t h = AirHalf(m, R + 172u);
        WriteU16Bench(m, R + 172u, static_cast<uint16_t>((2u << 5) | (h & 0x1Fu)));
    }
    a.a[0] = R;
}

uint32_t NativeRiderLand(NativeEnv& env, const Args& a) {
    rr::sim::GuestRam g = rcv::View(env);
    const rr::sim::BikeTables t = rcv::Tables(env, g);
    rcv::RecoverOracle c(env);
    const bool ok = rr::sim::RiderLand(g, a.a[0], env.sp, t, c);
    return rcv::Done(env, g, ok, "RiderLand");
}

} // namespace airrows

void AddRecoverAirRows(std::vector<Row>& rows) {
    using namespace rr::sim;
    rows.push_back(Row{"rider_ground_step", "void 0x8008F404(Rider *R, s32 dt)  [RASHCDG, the rider pass's step]",
                       0x8008F404, airrows::SetupGroundStep, nullptr, /*compareV0=*/false, airrows::NativeGroundStep,
                       {OracleCallee{kAirRoadRebindFn, 2}, OracleCallee{kAirSettleFn, 1},
                        OracleCallee{kRiderAirStepFn, 2}, OracleCallee{kAirGroundFn, 2}}});
    rows.push_back(Row{"rider_air_step", "void 0x800976C4(Rider *R, s32 dt)  [RASHCDG, the flying rider]", 0x800976C4,
                       airrows::SetupAirStep, nullptr, /*compareV0=*/false, airrows::NativeAirStep,
                       {OracleCallee{kJunctionMarginFn, 1}, OracleCallee{kRiderAirHitFn, 3},
                        OracleCallee{kAirStanceEventFn, 3}, OracleCallee{kAirRowsFromUpFn, 1},
                        OracleCallee{kRoadEdgeProbeFn, 4, 1}}});
    rows.push_back(Row{"rider_air_hit", "s32 0x800A966C(Rider *R, const s16 dir[3], s32 k)  [RASHCDG]", 0x800A966C,
                       airrows::SetupAirHit, nullptr, /*compareV0=*/true, airrows::NativeAirHit,
                       {OracleCallee{kAirBounceFn, 4, 3}}});
    rows.push_back(Row{"junction_margin", "Record *0x8003E338(Entity *e)  [SLUS, the junction arm pair]", 0x8003E338,
                       airrows::SetupJunctionMargin, nullptr, /*compareV0=*/true, airrows::NativeJunctionMargin,
                       {OracleCallee{kAirNodeWedgeFn, 4, 1}}});
    rows.push_back(Row{"road_edge_probe",
                       "s32 0x80038550(Entity *e, s32 along, s32 lat, Cursor *cur, [s16 out[3]])  [SLUS]", 0x80038550,
                       airrows::SetupRoadEdgeProbe, nullptr, /*compareV0=*/true, airrows::NativeRoadEdgeProbe,
                       {OracleCallee{kAirRoadMissingFn, 2}, OracleCallee{kAirNeighboursFn, 4, 1}}});
    rows.push_back(Row{"rider_land", "void 0x800B208C(Rider *R)  [RASHCDG, the flying rider meets the ground]",
                       0x800B208C, airrows::SetupRiderLand, nullptr, /*compareV0=*/false, airrows::NativeRiderLand,
                       {OracleCallee{kAirCornerMinFn, 4, 1}, OracleCallee{kAirEffectBurstFn, 4},
                        OracleCallee{kAirImpulseFn, 3}, OracleCallee{kRiderAirHitFn, 3},
                        OracleCallee{kAirStanceEventFn, 3}, OracleCallee{kAirSoundFn, 4},
                        OracleCallee{kRoadEdgeProbeFn, 4, 1}, OracleCallee{kAirRowsFromUpFn, 1},
                        OracleCallee{kAirRangedStartFn, 4, 3}, OracleCallee{kAirSetRiderStateFn, 3},
                        OracleCallee{kAirPadRumbleFn, 4, 1}}});
}
