// Bench rows of recover_fall.h (see rows_recover.inc): the rider leaving the bike and flying.
//
// Every case is built on a LIVE pool-0 bike of the bench's own machine (rr-race, 18 bikes) and its
// rider R = bike+852: the callees the oracle answers (the pose initialisers, RiderSync, RoadRebind,
// the settle, BuildObbAlt, the ground query, RouteBind) walk that rider's animation object, road
// cursor and route, and the bikes it points at, so a record lifted out of another image would point
// at state this machine does not hold. The dump-derived half keeps the captured scalars more often;
// the randomised half draws them.
//
// Named bounds (each the range the game's own code keeps):
//  - the rider's animation object R+0x21C is always a real one of this machine (the rider's own or
//    another rider's), and its slot chain (+4, +12, +40) keeps its captured value: a drawn slot
//    would make the clip lookup read a pointer out of arbitrary memory. The clip phase scalars
//    (+16, +24, +32) and the clip length (clip+16) are drawn freely - every R3000 `div` corner
//    (0, INT_MIN / -1) included.
//  - a passenger (+0x23C bit 5, and bit 4 for the pass) always has a real pool-0 bike at +0x358.
//  - the ground query's hint R+0x218 is a word the query itself returns (0, -1, or the rider's or
//    its bike's own +0x218), never a drawn one: its bits 14..23 index a region-4 record
//    and a drawn index reads outside it.
//  - the pool-0 high index *(0x800CE4DC) is -1..17 (the pool holds 18 bikes).

namespace fallrows {

constexpr uint32_t kPoolHighPtr = 0x800CE4DCu; // pool 0: +12 -> the high index

bool Chance(Rng& r, uint32_t n) { return r.Next() % n == 0u; }
uint16_t Peek16(const Memory& m, uint32_t a) {
    return static_cast<uint16_t>(m.PeekByte(a) | (static_cast<uint16_t>(m.PeekByte(a + 1)) << 8));
}
bool InRam(uint32_t a) { return a >= 0x80000000u && a < 0x801FFFF0u; }

uint32_t AnyBike(CaseContext& c) { return pop::Bike(*c.mem, static_cast<uint32_t>(c.rng->Next() % 18u)); }
uint32_t RiderOf(const Memory& m, uint32_t bike) { return m.PeekWord(bike + 852u); }

// A rider of this machine; with `playerBias` the player's (slot 0) every fourth case.
uint32_t PickRider(CaseContext& c, bool playerBias) {
    Rng& r = *c.rng;
    const uint32_t k = (playerBias && Chance(r, 4)) ? 0u : static_cast<uint32_t>(r.Next() % 18u);
    return RiderOf(*c.mem, pop::Bike(*c.mem, k));
}

// +0x23C bit 5 (the passenger arm), with a real bike at the rider's bike's +0x358.
bool DrawPassenger(CaseContext& c, uint32_t R, uint32_t n) {
    Memory& m = *c.mem;
    uint8_t b = m.PeekByte(R + 572u);
    const bool pass = Chance(*c.rng, n);
    if (pass) {
        b = static_cast<uint8_t>(b | 0x20u);
        m.PokeWord(m.PeekWord(R + 596u) + 856u, AnyBike(c));
    } else {
        b = static_cast<uint8_t>(b & ~0x20u);
    }
    pop::Put8(m, R + 572u, b);
    return pass;
}

// The bike the pose initialisers place R against.
uint32_t PoseBikeOf(const Memory& m, uint32_t R) {
    const uint32_t B = m.PeekWord(R + 596u);
    return (m.PeekByte(R + 572u) & 0x20u) ? m.PeekWord(B + 856u) : B;
}

// The animation object and its clip phase: the object is the rider's own or
// another rider's; +16 / +24 / +32 and the clip's length are drawn around the division corners.
void DrawClip(CaseContext& c, uint32_t R, bool derived) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    if (Chance(r, 2)) m.PokeWord(R + 540u, m.PeekWord(RiderOf(m, AnyBike(c)) + 540u));
    const uint32_t A = m.PeekWord(R + 540u);
    if (!derived || !Chance(r, 3)) {
        const uint32_t frames[] = {0u, 1u, 2u, 3u, 4u, r.U32() % 64u, m.PeekWord(A + 16u), r.U32()};
        m.PokeWord(A + 16u, frames[r.Next() % 8u]);
        const uint32_t divs[] = {0u, 1u, 0xFFFFFFFFu, 1u + r.U32() % 100u, m.PeekWord(A + 24u), r.U32()};
        m.PokeWord(A + 24u, divs[r.Next() % 6u]);
        const uint32_t nums[] = {0u, r.U32() % 64u, 0x8000u, m.PeekWord(A + 32u), r.U32()};
        m.PokeWord(A + 32u, nums[r.Next() % 5u]);
    }
    // the clip record the slot chain reaches: its u16 length +16 (n = len - 1, s16)
    const uint32_t tab = m.PeekWord(A + 4u) + 12u * m.PeekWord(A + 12u);
    const uint32_t clips = m.PeekWord(m.PeekWord(A + 40u) + 4u);
    if (!InRam(tab) || !InRam(clips)) return;
    const uint32_t clip = m.PeekWord(clips + 4u * m.PeekByte(tab));
    if (InRam(clip) && Chance(r, 3)) {
        const uint16_t lens[] = {0u, 1u, 2u, 0x8000u, 0x8001u, static_cast<uint16_t>(r.U32())};
        WriteU16Bench(m, clip + 16u, lens[r.Next() % 6u]);
    }
}

void PutUnit3(Memory& m, Rng& r, uint32_t a) {
    for (uint32_t k = 0; k < 3; ++k) PutS16(m, a + 2u * k, r.Unit());
}
void PutWorld3(Memory& m, Rng& r, uint32_t a) {
    for (uint32_t k = 0; k < 3; ++k) PutS32(m, a + 4u * k, r.World());
}

// The speed word +0x1E0 around the 5.0 threshold the initialisers test (slt 0x4FFFF / 0x50000).
uint32_t SpeedNear5(Rng& r, uint32_t captured) {
    const uint32_t v[] = {0x4FFFFu, 0x50000u, 0x50001u, 0u, captured, r.U32() >> (r.Next() % 16u),
                          0u - (r.U32() >> (8 + r.Next() % 16u)), r.U32()};
    return v[r.Next() % 8u];
}

// ---------------------------------------------------------------- the pose initialisers
// (R, out): out 0 (RiderLaunch) or a scratch word (the tick's sp+16). Driven: the clip phase, the
// passenger arm, R+0x228 (bits 16, 27), the bike's +0x1E0 around 5.0, C's +0x244.. offsets and the
// bike's +0x238 bits 9/10, E's +0x28C and velocity +0x1C8, R+0x138.
void SetupPose(CaseContext& c, Args& a) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    const bool derived = c.sample != nullptr;
    const uint32_t R = PickRider(c, true);
    DrawClip(c, R, derived);
    DrawPassenger(c, R, 3);
    const uint32_t B = PoseBikeOf(m, R);
    m.PokeWord(R + 552u, derived ? (m.PeekWord(R + 552u) ^ (r.U32() & r.U32())) : r.U32());
    m.PokeWord(B + 480u, SpeedNear5(r, m.PeekWord(B + 480u)));
    for (uint32_t k = 0; k < 3; ++k) PutS32(m, R + 580u + 4u * k, r.World());
    m.PokeWord(B + 568u, m.PeekWord(B + 568u) ^ (r.U32() & 0x600u));
    m.PokeWord(B + 652u, Chance(r, 2) ? r.U32() : r.U32() % 0x20000u);
    if (!derived || Chance(r, 2)) PutWorld3(m, r, B + 456u);
    if (!derived) {
        PutUnit3(m, r, B + 432u);
        PutUnit3(m, r, B + 438u);
        PutUnit3(m, r, B + 444u);
        for (uint32_t off : {516u, 522u, 528u}) PutUnit3(m, r, B + off);
        PutWorld3(m, r, B + 184u);
        PutWorld3(m, r, B + 504u);
        PutUnit3(m, r, R + 432u);
        PutUnit3(m, r, R + 438u);
    }
    m.PokeWord(R + 312u, Chance(r, 2) ? m.PeekWord(R + 312u) : r.U32() >> (r.Next() % 20u));
    m.PokeWord(c.scratch, r.U32());
    a.a[0] = R;
    a.a[1] = Chance(r, 2) ? 0u : c.scratch;
}

template <bool (*Fn)(rr::sim::GuestRam&, uint32_t, uint32_t, uint32_t, const rr::sim::BikeTables&)>
uint32_t NativePose(NativeEnv& env, const Args& a) {
    rr::sim::GuestRam g = rcv::View(env);
    auto t = rcv::Tables(env, g);
    const bool ok = Fn(g, a.a[0], a.a[1], env.sp, t);
    return rcv::Done(env, g, ok, "pose initialiser");
}

// ---------------------------------------------------------------- RiderSync 0x8008DF74
// The source bike differs from the rider's usual one (another live bike at +0x254, or the
// passenger's +0x358), so every copy is visible; R's rows +0x1B0 are drawn.
void SetupSync(CaseContext& c, Args& a) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    const uint32_t R = PickRider(c, true);
    if (Chance(r, 2)) m.PokeWord(R + 596u, AnyBike(c));
    DrawPassenger(c, R, 3);
    const uint32_t B = PoseBikeOf(m, R);
    PutUnit3(m, r, R + 432u);
    PutUnit3(m, r, R + 438u);
    PutUnit3(m, r, R + 444u);
    if (Chance(r, 2)) m.PokeWord(B + 324u, r.U32());
    if (Chance(r, 2)) WriteU16Bench(m, B + 320u, static_cast<uint16_t>(r.U32()));
    a.a[0] = R;
}

uint32_t NativeSync(NativeEnv& env, const Args& a) {
    rr::sim::GuestRam g = rcv::View(env);
    auto t = rcv::Tables(env, g);
    rcv::RecoverOracle c(env);
    const bool ok = rr::sim::RiderSync(g, a.a[0], env.sp, t, c);
    return rcv::Done(env, g, ok, "RiderSync");
}

// ---------------------------------------------------------------- RiderSettle 0x8009246C
// Driven: +0x184 bit 0 (the ground query or the road slice), the query's point (the rider at its
// bike, near it, or far off every resident cell: -1), R+0x228 bit 30, +0x174 / +0x18A, +0x1E0 = 0,
// the up vector +0x20A, +0x1FC, and the slice's normal Y around -614.
uint32_t HintOf(CaseContext& c, uint32_t R) {
    Memory& m = *c.mem;
    const uint32_t v[] = {0u, 0xFFFFFFFFu, m.PeekWord(R + 536u), m.PeekWord(m.PeekWord(R + 596u) + 536u)};
    return v[c.rng->Next() % 4u];
}

void PlaceNearBike(CaseContext& c, uint32_t R) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    const uint32_t B = m.PeekWord(R + 596u);
    const uint64_t k = r.Next() % 4u;
    if (k == 0) return; // captured
    for (uint32_t i = 0; i < 3; ++i) {
        const uint32_t base = m.PeekWord(B + 184u + 4u * i);
        uint32_t off = 0;
        if (k == 2) off = static_cast<uint32_t>(r.S32() >> (12 + r.Next() % 8u));
        if (k == 3) off = static_cast<uint32_t>(r.S32() >> (r.Next() % 4u));
        m.PokeWord(R + 184u + 4u * i, base + off);
    }
}

void SetupSettle(CaseContext& c, Args& a) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    const bool derived = c.sample != nullptr;
    const uint32_t R = PickRider(c, true);
    PlaceNearBike(c, R);
    uint32_t f184 = m.PeekWord(R + 388u);
    f184 = Chance(r, 2) ? (f184 | 1u) : (f184 & ~1u);
    m.PokeWord(R + 388u, f184);
    uint32_t f228 = derived ? m.PeekWord(R + 552u) : r.U32();
    f228 = Chance(r, 2) ? (f228 | 0x40000000u) : (f228 & ~0x40000000u);
    m.PokeWord(R + 552u, f228);
    m.PokeWord(R + 372u, Chance(r, 2) ? 0u : (m.PeekWord(R + 372u) | 1u));
    pop::Put8(m, R + 394u, r.U32());
    m.PokeWord(R + 536u, HintOf(c, R));
    m.PokeWord(R + 480u, Chance(r, 2) ? 0u : static_cast<uint32_t>(r.World()));
    if (!derived || Chance(r, 2)) PutUnit3(m, r, R + 522u);
    m.PokeWord(R + 508u, static_cast<uint32_t>(r.World()));
    if (Chance(r, 3)) {
        const uint32_t B = m.PeekWord(R + 596u);
        for (uint32_t k = 0; k < 3; ++k) WriteU16Bench(m, R + 450u + 2u * k, Peek16(m, B + 450u + 2u * k));
    }
    if (Chance(r, 6)) { // the road slice's normal Y (+8 row 1, +10) around the -614 test
        const uint32_t sl = m.PeekWord(R + 340u);
        const int16_t ys[] = {-614, -615, -613, 0, r.Unit()};
        if (InRam(sl)) PutS16(m, sl + 10u, ys[r.Next() % 5u]);
    }
    a.a[0] = R;
}

uint32_t NativeSettle(NativeEnv& env, const Args& a) {
    rr::sim::GuestRam g = rcv::View(env);
    auto t = rcv::Tables(env, g);
    rcv::RecoverOracle c(env);
    const bool ok = rr::sim::RiderSettle(g, a.a[0], env.sp, t, c);
    return rcv::Done(env, g, ok, "RiderSettle");
}

// ---------------------------------------------------------------- RiderOffTick 0x8008F138
// Driven: the gate +0x140, the mount state +0x25C (2 and the flight states), the stance +0x220 over
// the whole table 38..91 and out of range, R+0x228 bits 30 / 3 / 4, +0x1FC around 6553, the flight
// vectors, the bike's +0x3B2 surface-flag word bit 15, the passenger arm, the clip phase and the
// settle's inputs. The rider's road state is, 7 cases in 8, the one RiderLaunch leaves it with
// (RiderSync's copies of its bike's cursor, key and road block).
void PrepareFlyingRider(CaseContext& c, uint32_t R, bool derived) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    const uint32_t B = m.PeekWord(R + 596u);
    if (!Chance(r, 8)) {
        for (uint32_t off = 328u; off < 428u; off += 4u) m.PokeWord(R + off, m.PeekWord(B + off));
        for (uint32_t off : {428u, 492u, 496u}) m.PokeWord(R + off, m.PeekWord(B + off));
    }
    DrawClip(c, R, derived);
    DrawPassenger(c, R, 4);
    const int16_t live = GetS16(m, R + 320u);
    PutS16(m, R + 320u, Chance(r, 8) ? int16_t{0} : (live != 0 ? live : static_cast<int16_t>(1 + r.U32() % 20u)));
    const uint32_t mounts[] = {2u, 2u, 3u, 4u, 5u, static_cast<uint32_t>(r.Next() % 8u), r.U32()};
    m.PokeWord(R + 604u, mounts[r.Next() % 7u]);
    WriteU16Bench(m, R + 544u,
                  Chance(r, 5) ? static_cast<uint16_t>(r.U32()) : static_cast<uint16_t>(38u + r.U32() % 54u));
    uint32_t f = derived ? m.PeekWord(R + 552u) ^ (r.U32() & r.U32()) : r.U32();
    f = Chance(r, 2) ? (f | 0x40000000u) : (f & ~0x40000000u);
    if (Chance(r, 2)) f &= ~0x18u;
    m.PokeWord(R + 552u, f);
    const uint32_t tb[] = {0u, 0xFFFFFFFFu, 1u, 6552u, 6553u, 6554u, r.U32() % 20000u, r.U32()};
    m.PokeWord(R + 508u, tb[r.Next() % 8u]);
    m.PokeWord(R + 480u, derived && Chance(r, 2) ? m.PeekWord(R + 480u) : static_cast<uint32_t>(r.World()));
    PutWorld3(m, r, R + 456u);
    if (!derived || Chance(r, 2)) PutUnit3(m, r, R + 450u);
    if (!derived || Chance(r, 2)) PutUnit3(m, r, R + 522u);
    PlaceNearBike(c, R);
    uint32_t f184 = m.PeekWord(R + 388u);
    f184 = Chance(r, 2) ? (f184 | 1u) : (f184 & ~1u);
    m.PokeWord(R + 388u, f184);
    m.PokeWord(R + 536u, HintOf(c, R));
    // the bike's surface-flag word at +0x3BA + 8 * (s8)+0x3B2, bit 15
    const int32_t i = static_cast<int8_t>(m.PeekByte(B + 946u));
    const uint32_t w = B + static_cast<uint32_t>(i) * 8u + 954u;
    if (Chance(r, 2) && InRam(w)) WriteU16Bench(m, w, static_cast<uint16_t>(Peek16(m, w) ^ 0x8000u));
    m.PokeWord(B + 480u, SpeedNear5(r, m.PeekWord(B + 480u)));
}

int32_t DrawDt(Rng& r) {
    const uint32_t v[] = {0x1000u, 0x2000u, 0x10000u >> (r.Next() % 6u), r.U32() % 0x20000u, r.U32() >> 12};
    return static_cast<int32_t>(v[r.Next() % 5u]);
}

void SetupTick(CaseContext& c, Args& a) {
    const uint32_t R = PickRider(c, true);
    PrepareFlyingRider(c, R, c.sample != nullptr);
    a.a[0] = R;
    a.a[1] = static_cast<uint32_t>(DrawDt(*c.rng));
}

uint32_t NativeTick(NativeEnv& env, const Args& a) {
    rr::sim::GuestRam g = rcv::View(env);
    auto t = rcv::Tables(env, g);
    rcv::RecoverOracle c(env);
    const bool ok = rr::sim::RiderOffTick(g, a.a[0], static_cast<int32_t>(a.a[1]), env.sp, t, c);
    return rcv::Done(env, g, ok, "RiderOffTick");
}

// ---------------------------------------------------------------- RiderOffPass 0x8008F068
// Every slot's rider on either side of the mount-state test (>=u 2), +0x23C bit 4 with a real
// +0x358 bike whose rider is on either side too, and the pool's high index.
void SetupPass(CaseContext& c, Args& a) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    const bool derived = c.sample != nullptr;
    for (uint32_t k = 0; k < 18; ++k) {
        const uint32_t B = pop::Bike(m, k);
        const uint32_t R = RiderOf(m, B);
        if (Chance(r, 4)) {
            PrepareFlyingRider(c, R, derived);
        } else {
            const uint32_t low[] = {0u, 1u, m.PeekWord(R + 604u)}; // or the captured state (slot 6: 4)
            m.PokeWord(R + 604u, low[r.Next() % 3u]);
        }
        uint8_t b = m.PeekByte(R + 572u);
        if (Chance(r, 6)) {
            b = static_cast<uint8_t>(b | 0x10u);
            m.PokeWord(B + 856u, AnyBike(c));
        } else {
            b = static_cast<uint8_t>(b & ~0x10u);
        }
        pop::Put8(m, R + 572u, b);
    }
    if (Chance(r, 8)) {
        const uint32_t hp = m.PeekWord(kPoolHighPtr);
        if (InRam(hp)) m.PokeWord(hp, Chance(r, 3) ? 0xFFFFFFFFu : static_cast<uint32_t>(r.Next() % 18u));
    }
    a.a[0] = static_cast<uint32_t>(DrawDt(r));
}

uint32_t NativePass(NativeEnv& env, const Args& a) {
    rr::sim::GuestRam g = rcv::View(env);
    auto t = rcv::Tables(env, g);
    rcv::RecoverOracle c(env);
    const bool ok = rr::sim::RiderOffPass(g, static_cast<int32_t>(a.a[0]), env.sp, t, c);
    return rcv::Done(env, g, ok, "RiderOffPass");
}

// ---------------------------------------------------------------- LaunchLift 0x8007E868
// (dir, speed, c, k): RiderLaunch passes (R+0x1C2, R+0x1E0, a sin/cos word, 0x633B6) or
// (R+0x1C2, 0, -2868, 0). Driven: the four sign arms of the k / speed quotient, the s16 compare
// against c, the early out (dir[1] already the result), the horizontal part below 6 (the 655
// fallback) and both signs of 1 - y^2.
void SetupLift(CaseContext& c, Args& a) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    const uint32_t dir = Chance(r, 2) ? PickRider(c, true) + 450u : c.scratch + 64u;
    const uint64_t shape = r.Next() % 4u;
    for (uint32_t k = 0; k < 3; ++k) {
        int16_t v = r.Unit();
        if (shape == 1 && k != 1) v = static_cast<int16_t>(static_cast<int32_t>(r.U32() % 5u) - 2); // horizontal ~0
        if (shape == 2) v = r.S16();
        if (shape == 3 && dir != c.scratch + 64u) v = GetS16(m, dir + 2u * k);
        PutS16(m, dir + 2u * k, v);
    }
    const int32_t ks[] = {0x633B6, 0, r.World(), r.S32(), -r.World()};
    const int32_t k = ks[r.Next() % 5u];
    const int32_t speeds[] = {0, r.World(), -r.World(), static_cast<int32_t>(static_cast<uint32_t>(k) + 1u),
                              static_cast<int32_t>(static_cast<uint32_t>(k) - 1u), r.S32(), 0x50000};
    const int32_t speed = speeds[r.Next() % 7u];
    const int32_t cs[] = {-2868, r.S16(), static_cast<int32_t>(r.S16()) * 16, r.S32(), 0};
    const int32_t cc = cs[r.Next() % 5u];
    if (Chance(r, 6)) { // k >= speed: the result is c itself; dir[1] already holds it
        PutS16(m, dir + 2u, static_cast<int16_t>(cc));
        a.a[1] = static_cast<uint32_t>(k);
        a.a[3] = static_cast<uint32_t>(k);
    } else {
        a.a[1] = static_cast<uint32_t>(speed);
        a.a[3] = static_cast<uint32_t>(k);
    }
    a.a[0] = dir;
    a.a[2] = static_cast<uint32_t>(cc);
}

uint32_t NativeLift(NativeEnv& env, const Args& a) {
    rr::sim::GuestRam g = rcv::View(env);
    auto t = rcv::Tables(env, g);
    const bool ok = rr::sim::LaunchLift(g, a.a[0], static_cast<int32_t>(a.a[1]), static_cast<int32_t>(a.a[2]),
                                        static_cast<int32_t>(a.a[3]), env.sp, t);
    return rcv::Done(env, g, ok, "LaunchLift");
}

} // namespace fallrows

// ---------------------------------------------------------------- the rider pass's down walk
//
// A SPLICE, exactly as rows_collision.inc's `rider_pass_thrown`: the guest run is the
// rider / engine pass RASHCDG 0x8007B840(dt) start to finish; the clone runs the same pass with a hook at
// the region's entry 0x8007DDF4, where RiderPassDownWalk runs on the clone's RAM and `pc` goes to the
// exit 0x8007E804 (no register live there: 0x8007E804.. writes v0 / s1 / s0 / a0 / a1 before reading
// them, the rest are restored from the frame; and the region reads no register it has not written - dt
// is the pass's home slot, entry sp + 0 = its frame + 248); a third run, the original on a private copy
// stopped at the exit, gives the state the original region leaves, compared byte for byte outside the
// stack window. `--splice-control` skips the port.
//
// The family: this machine's five bike lists relinked from scratch - 1..3 pool-0 slots (the player
// among them half the time) on the CRASHED-DOWN list 0x8005B378, the player on the riding list when he
// is not down, every other slot where it was - and per down bike the fields the region reads: flagsC
// 0x200 with 0x400 / 0x800 / 0x1000 / 0x0800_0000 clear (the list rule, and no migration pending, so
// the pass's own migration loops leave it there) and bits 19 (on the ground) / 20 (the side) / 21
// drawn; flagsB bit 20 (no drag); the yaw rate / yaw +0x280 / +0x2C4 at and around the +-1.5708 /
// +-2.7925 bounds; the passenger word +0x358; the ground frame +0x31C/+0x322/+0x328 and the up row
// +0x20A (both dot tests on both sides, |dot| around 6554); the roll rate +0x1E8 and roll +0x268 around
// the +-2 pi wrap; the blend axes +0x26C / +0x274; the impact timer +0x2D4 / +0x2D8 and its rates
// +0x290 / +0x280; the velocity (small, large, zero, captured) and the speed +0x1E0 around 8.94; +0x2F4;
// the drag word 0x800D3964; dt.
// NAMED BOUNDS: the ground frame is a rotation (the rows of RotMatrix of drawn angles, or of the bike's
// own +0x1B0, permuted and negated) and the up row / blend axes are unit directions: RowsFromUp and the
// region's Normalize overflow their trapping adds on a longer cross product. A passenger +0x358 is
// planted only on a dormant slot (+0x140 = 0), whose rider's +0x23C bit 4 is cleared: the pass's first
// pool loop runs its passenger code for a live bike (0x8007C0F4) and the second one follows a rider's
// bit 4 into +0x358 (0x8007DD84), and neither is part of this region (rows_collision.inc's thrown row
// names the same bound). A slot moved onto the list keeps its own +0x140 (0 for the dormant AI), as the
// thrown row's do.
namespace fallrows {

void SetupDown(CaseContext& c, Args& a) {
    Rng& rng = *c.rng;
    Memory& m = *c.mem;
    const bool derived = c.sample != nullptr;
    const uint32_t pool = m.PeekWord(kPoolTable);
    const uint32_t slots = 18;
    std::vector<int> on(slots, 0);
    for (int h = 0; h < 5; ++h) {
        uint32_t n = m.PeekWord(bs::kListHeads[h] + 4u);
        for (int guard = 0; n != bs::kListHeads[h] && guard < 64; ++guard) {
            const uint32_t e = n - 1088u;
            if (e >= pool && (e - pool) % 1096u == 0u && (e - pool) / 1096u < slots) on[(e - pool) / 1096u] = h;
            n = m.PeekWord(n + 4u);
        }
    }
    std::vector<uint32_t> down;
    if (Chance(rng, 2)) down.push_back(0u);
    const uint32_t more = (down.empty() ? 1u : 0u) + rng.U32() % 3u;
    for (uint32_t k = 0; k < more; ++k) {
        const uint32_t s = 1u + rng.U32() % (slots - 1u);
        if (std::find(down.begin(), down.end(), s) == down.end()) down.push_back(s);
    }
    for (size_t k = down.size(); k > 1; --k) std::swap(down[k - 1], down[rng.U32() % k]);
    on[0] = 1;
    for (uint32_t s : down) on[s] = 4;
    for (int h = 0; h < 5; ++h) {
        std::vector<uint32_t> ents;
        if (h == 4) {
            for (uint32_t s : down) ents.push_back(pool + 1096u * s);
        } else {
            for (uint32_t s = 0; s < slots; ++s)
                if (on[s] == h) ents.push_back(pool + 1096u * s);
        }
        bs::LinkList(m, bs::kListHeads[h], ents.data(), static_cast<uint32_t>(ents.size()));
    }
    const int16_t* sincos = reinterpret_cast<const int16_t*>(m.ram().data() + (rr::sim::kRcSinCos - 0x80000000u));
    auto pick = [&rng](std::initializer_list<uint32_t> v) { return *(v.begin() + rng.Next() % v.size()); };
    for (uint32_t s : down) {
        const uint32_t e = pool + 1096u * s;
        uint32_t fc = (m.PeekWord(e + 568u) & ~(0x08001C00u | 0x00380000u)) | 0x200u;
        if (Chance(rng, 2)) fc |= 0x80000u;
        if (Chance(rng, 2)) fc |= 0x100000u;
        if (Chance(rng, 4)) fc |= 0x200000u;
        m.PokeWord(e + 568u, fc);
        uint32_t fb = m.PeekWord(e + 564u) & ~0x100000u;
        if (Chance(rng, 6)) fb |= 0x100000u;
        m.PokeWord(e + 564u, fb);
        m.PokeWord(e + 640u, Chance(rng, 3) ? 0u : static_cast<uint32_t>(rng.S32() >> (8 + rng.Next() % 12u)));
        m.PokeWord(e + 708u, pick({0x1921Fu, 0xFFFE6DE1u, 0x2CAE2u, 0xFFFD351Eu, 0x19000u, 0x2C000u,
                                   static_cast<uint32_t>(rng.S32() >> (12 + rng.Next() % 8u))}));
        // the passenger word (named bound above)
        const uint32_t rider = m.PeekWord(e + 852u);
        if (GetS16(m, e + 320u) == 0 && Chance(rng, 3) && InRam(rider)) {
            m.PokeWord(e + 856u, pool + 1096u * static_cast<uint32_t>(rng.Next() % slots));
            pop::Put8(m, rider + 572u, m.PeekByte(rider + 572u) & ~0x10u);
        } else {
            m.PokeWord(e + 856u, 0u);
        }
        // the ground frame: a rotation, rows permuted and negated
        int16_t mat[9];
        if (derived && Chance(rng, 2)) {
            for (uint32_t i = 0; i < 9; ++i) mat[i] = GetS16(m, e + 432u + 2u * i);
        } else {
            const int16_t ang[3] = {static_cast<int16_t>(rng.U32() % 4096u), static_cast<int16_t>(rng.U32() % 4096u),
                                    static_cast<int16_t>(rng.U32() % 4096u)};
            rr::sim::RotMatrix(ang, mat, sincos);
        }
        uint32_t perm[3] = {0, 1, 2};
        for (uint32_t k = 3; k > 1; --k) std::swap(perm[k - 1], perm[rng.U32() % k]);
        int16_t rowsOut[3][3];
        for (uint32_t r = 0; r < 3; ++r) {
            const bool neg = Chance(rng, 2);
            for (uint32_t k = 0; k < 3; ++k) {
                const int16_t v = mat[3u * perm[r] + k];
                rowsOut[r][k] = neg ? static_cast<int16_t>(-v) : v;
                PutS16(m, e + 796u + 6u * r + 2u * k, rowsOut[r][k]);
            }
        }
        // the up row: one of the frame's rows (so the others are perpendicular to it), a direction, or
        // the captured one
        const uint64_t upKind = rng.Next() % 4u;
        if (upKind < 2) {
            const uint32_t r = static_cast<uint32_t>(rng.Next() % 3u);
            const bool neg = Chance(rng, 2);
            for (uint32_t k = 0; k < 3; ++k)
                PutS16(m, e + 522u + 2u * k, neg ? static_cast<int16_t>(-rowsOut[r][k]) : rowsOut[r][k]);
        } else if (upKind == 2) {
            int16_t d[3];
            co::Dir(rng, d);
            for (uint32_t k = 0; k < 3; ++k) PutS16(m, e + 522u + 2u * k, d[k]);
        }
        m.PokeWord(e + 488u, Chance(rng, 2) ? 0u : static_cast<uint32_t>(rng.S32() >> (10 + rng.Next() % 12u)));
        m.PokeWord(e + 616u, pick({0x6487Du, 0x6487Eu, 0xFFF9B782u, 0xFFF9B781u, 0u,
                                   static_cast<uint32_t>(rng.S32() >> (10 + rng.Next() % 10u))}));
        for (uint32_t off : {620u, 628u}) {
            int16_t d[3];
            co::Dir(rng, d);
            for (uint32_t k = 0; k < 3; ++k) PutS16(m, e + off + 2u * k, d[k]);
        }
        m.PokeWord(e + 692u, static_cast<uint32_t>(rng.S32() >> (8 + rng.Next() % 12u)));
        const uint32_t el = 2180u * (rng.U32() % 8u);
        m.PokeWord(e + 724u, el);
        m.PokeWord(e + 728u, pick({0u, el, el + 2180u, el + 1000u, rng.U32() % 30000u}));
        m.PokeWord(e + 656u, Chance(rng, 3) ? 0u : static_cast<uint32_t>(rng.S32() >> (8 + rng.Next() % 12u)));
        const uint64_t vk = rng.Next() % 4u;
        for (uint32_t k = 0; k < 3; ++k) {
            int32_t v = static_cast<int32_t>(rng.U32() % 0x500000u) - 0x280000;
            if (vk == 1) v = static_cast<int32_t>(rng.U32() % 0x40000u) - 0x20000;
            if (vk == 2) v = 0;
            if (vk == 3 && derived && c.sample->entity.size() == 1096)
                v = static_cast<int32_t>(GetU32V(c.sample->entity, 456 + 4 * k));
            PutS32(m, e + 456u + 4u * k, v);
        }
        m.PokeWord(e + 480u, pick({0x8F0D8u, 0x8F0D7u, 0x100000u, 0u, rng.U32() % 0x200000u}));
        m.PokeWord(e + 756u, rng.U32());
    }
    m.PokeWord(rr::sim::kDownDragWord,
               pick({m.PeekWord(rr::sim::kDownDragWord), 0u, rng.U32() % 0x10000u, rng.U32() % 0x1000u}));
    a.a[0] = static_cast<uint32_t>(bs::DrawDt(rng));
}

uint32_t NativeDown(NativeEnv& env, const Args& a) {
    Machine& cl = *env.clone;
    const uint32_t sp = env.sp;
    bs::SpliceStats::Entry& st = bs::Stats().For("rider_pass_down");
    ++st.cases;
    auto fail = [&env](const std::string& why) {
        if (env.failure.empty()) env.failure = why;
    };
    std::vector<uint8_t> refRam, refSpad;
    bool refReached = false;
    {
        Machine ref;
        bs::CopyMachine(cl, ref);
        ref.cpu.AddHook(rr::sim::kDownWalkExit, "exit");
        bs::StartCall(ref.cpu, rr::sim::kRiderPassFn, a, sp);
        const Trap t = ref.cpu.Run(kSentinel, env.maxSteps);
        if (t.kind == TrapKind::Hook && t.pc == rr::sim::kDownWalkExit) {
            refReached = true;
            refRam = ref.mem.ram();
            refSpad = ref.mem.scratchpad();
        } else if (t.kind != TrapKind::Halted) {
            fail("the reference run of the original trapped: " + t.ToString());
            return 0;
        }
    }
    cl.cpu.ClearHooks();
    cl.cpu.AddHook(rr::sim::kDownWalkEntry, "entry");
    bs::StartCall(cl.cpu, rr::sim::kRiderPassFn, a, sp);
    int entries = 0;
    for (;;) {
        cl.cpu.callObservations.clear();
        const Trap t = cl.cpu.Run(kSentinel, env.maxSteps);
        env.calls.insert(env.calls.end(), cl.cpu.callObservations.begin(), cl.cpu.callObservations.end());
        cl.cpu.callObservations.clear();
        if (t.kind == TrapKind::Halted) break;
        if (t.kind != TrapKind::Hook || t.pc != rr::sim::kDownWalkEntry) {
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
            rr::sim::GuestRam g(cl.mem.ram().data(), env.gp);
            g.SetScratchpad(cl.mem.scratchpad().data());
            const rr::sim::BikeTables tab = rr::sim::RecoverTables(cl.mem.ram().data(), g);
            rcv::RecoverOracle oc(env);
            if (!rr::sim::RiderPassDownWalk(g, dt, frame, tab, oc)) {
                char buf[128];
                std::snprintf(buf, sizeof(buf), "the port refused the region (view fault 0x%08X)", g.FaultAddress());
                fail(buf);
                break;
            }
        }
        RestoreRegs(saved, cl.cpu);
        cl.cpu.pc = rr::sim::kDownWalkExit;
        cl.cpu.npc = rr::sim::kDownWalkExit + 4u;
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
                          rr::sim::kDownWalkExit, differ, first);
            fail(buf);
            break;
        }
    }
    cl.cpu.ClearHooks();
    if (entries == 0 && refReached) fail("the reference passed the region but the clone never entered it");
    return cl.cpu.regs[2];
}

} // namespace fallrows

void AddRecoverFallRows(std::vector<Row>& rows) {
    auto add = [&rows](const char* name, const char* sig, uint32_t entry, std::function<void(CaseContext&, Args&)> setup,
                       std::function<uint32_t(NativeEnv&, const Args&)> native, std::vector<OracleCallee> callees,
                       uint64_t steps) {
        rows.push_back(Row{name, sig, entry, std::move(setup), nullptr, /*compareV0=*/false});
        Row& r = rows.back();
        r.nativeEx = std::move(native);
        r.oracleCallees = std::move(callees);
        r.maxSteps = steps;
    };
    const std::vector<OracleCallee> none;
    add("pose_a", "void 0x8008EE60(Rider *R, s32 *out)  [RASHCDG 0x8008EE60, pose A, stance 38]", 0x8008EE60,
        fallrows::SetupPose, fallrows::NativePose<rr::sim::PoseA>, none, 5'000'000);
    add("pose_b", "void 0x8008E818(Rider *R, s32 *out)  [RASHCDG 0x8008E818, pose B, stances 39 / 89]", 0x8008E818,
        fallrows::SetupPose, fallrows::NativePose<rr::sim::PoseB>, none, 5'000'000);
    add("pose_c", "void 0x8008E50C(Rider *R, s32 a1)  [RASHCDG 0x8008E50C, pose C, stances 40 / 88]", 0x8008E50C,
        fallrows::SetupPose, fallrows::NativePose<rr::sim::PoseC>, none, 5'000'000);
    add("pose_d", "void 0x8008EB88(Rider *R, s32 *out)  [RASHCDG 0x8008EB88, pose D, stance 41 and the rest]",
        0x8008EB88, fallrows::SetupPose, fallrows::NativePose<rr::sim::PoseD>, none, 5'000'000);
    add("pose_e", "void 0x8008E044(Rider *R, s32 *out)  [RASHCDG 0x8008E044, pose E, stances 42 / 91]", 0x8008E044,
        fallrows::SetupPose, fallrows::NativePose<rr::sim::PoseE>, none, 5'000'000);
    add("rider_sync", "void 0x8008DF74(Rider *R)  [RASHCDG 0x8008DF74, the rider takes its bike's road state]",
        0x8008DF74, fallrows::SetupSync, fallrows::NativeSync, {OracleCallee{rr::sim::kFallRouteBindFn, 3}}, 5'000'000);
    add("rider_settle", "void 0x8009246C(Rider *R)  [RASHCDG 0x8009246C, the rider onto the ground plane]",
        0x8009246C, fallrows::SetupSettle, fallrows::NativeSettle, {OracleCallee{rr::sim::kFallGroundQueryFn, 4, 1}},
        5'000'000);
    add("rider_off_tick", "void 0x8008F138(Rider *R, s32 dt)  [RASHCDG 0x8008F138, the rider off the bike]",
        0x8008F138, fallrows::SetupTick, fallrows::NativeTick,
        {OracleCallee{rr::sim::kFallPoseAFn, 2}, OracleCallee{rr::sim::kFallPoseBFn, 2},
         OracleCallee{rr::sim::kFallPoseCFn, 2}, OracleCallee{rr::sim::kFallPoseDFn, 2},
         OracleCallee{rr::sim::kFallPoseEFn, 2}, OracleCallee{rr::sim::kFallRiderSyncFn, 1},
         OracleCallee{rr::sim::kFallRoadRebindFn, 2}, OracleCallee{rr::sim::kFallRiderSettleFn, 1},
         OracleCallee{rr::sim::kFallBuildObbAltFn, 1}},
        10'000'000);
    add("rider_off_pass", "void 0x8008F068(s32 dt)  [RASHCDG 0x8008F068, every rider off its bike]", 0x8008F068,
        fallrows::SetupPass, fallrows::NativePass, {OracleCallee{rr::sim::kFallRiderOffTickFn, 2}}, 40'000'000);
    add("launch_lift", "void 0x8007E868(s16 dir[3], s32 speed, s32 c, s32 k)  [RASHCDG 0x8007E868]", 0x8007E868,
        fallrows::SetupLift, fallrows::NativeLift, none, 5'000'000);
    add("rider_pass_down",
        "region [0x8007DDF4, 0x8007E804) of 0x8007B840(s32 dt)  [RASHCDG, a SPLICE: the crashed-down walk]",
        rr::sim::kRiderPassFn, fallrows::SetupDown, fallrows::NativeDown,
        {OracleCallee{rr::sim::kFallRowsFromUpFn, 1}}, 20'000'000);
}
