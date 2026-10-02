// Bench rows for the collision pass (the landing and the pass).
// Included by verify_physics.cpp inside its anonymous namespace, so everything defined above that
// point is visible here.
//
// The native code is src\game\sim\collision.{h,cpp}, a GUEST-ADDRESS port (road_query.h's
// `GuestRam`): the native side runs straight on the clone's RAM. Callees that are PORTED elsewhere
// run natively on the same RAM (Bounce, Spin, MulAdd16, ReleaseContact, PlaySound3D through the sound
// runtime's SoundMachine); the ones that are not run from the ORIGINAL code on the clone at exactly
// the stack pointer the original calls them at (bs::ExactOracle; every declaration has frame 0), and
// the two call sequences are compared. The deferred handler and ChainReaction make their unported
// calls REGISTER-FAITHFUL (FaithfulOracle below: s0..s8 and the return address the original has at the
// call site, with the ports' prologue spills written where the original writes them), because
// BikeTrafficReact reads its own uninitialised stack and finds there what the previous callee at that
// depth saved; without it 131 of 1160 deferred cases differed.
//
// Two opt-in development aids, off by default: RRJB_WALLKIND=<0..19> forces wall_contact's entity kind,
// RRJB_COLLDBG prints wall_contact's case parameters.

namespace co {

// RASHCDG.BIN, SHA-1 cfe43a7786759f2cb9c57751cf99e84d1074782c.
constexpr uint32_t kGameStatePtr = 0x8005B2F8;
constexpr uint32_t kSkipResults = 0x8005B220;

bool Chance(Rng& r, uint32_t n) { return r.Next() % n == 0; }
uint32_t Pick(Rng& r, std::initializer_list<uint32_t> v) { return *(v.begin() + r.Next() % v.size()); }

// A 1096-byte bike image: the captured one for a dump-derived case, else any captured bike.
std::vector<uint8_t> BikeImage(const CaseContext& c) {
    if (c.sample != nullptr && c.sample->entity.size() == 1096) return c.sample->entity;
    if (c.any != nullptr && c.any->entity.size() == 1096) return c.any->entity;
    return std::vector<uint8_t>(1096, 0);
}
uint32_t NumPlayersOf(const Memory& m) { return m.PeekWord(m.PeekWord(kGameStatePtr) + 48u); }
void SetNumPlayers(Memory& m, uint32_t n) { m.PokeWord(m.PeekWord(kGameStatePtr) + 48u, n); }
uint32_t Pool0(const Memory& m, uint32_t slot) { return m.PeekWord(kPool0BasePtr) + 1096u * slot; }

// A unit-ish direction, as a row of an orientation.
void Dir(Rng& rng, int16_t v[3]) { cr::RandomDir(rng, v, 4096); }

// An exact-depth oracle call made with the callee-saved registers and the RETURN ADDRESS the original
// has at that call site. bs::ExactOracle calls with ra = the bench's
// sentinel and with whatever s0..s8 the clone holds; a callee saves both into its frame, and an
// unported callee called later at that depth reads them back as its own uninitialised stack
// (BikeTrafficReact's face normal, 19.3). Here the callee returns to its real return address: a hook
// there stops the run when `sp` is back at the call's (a nested pass through the same address - the
// original ChainReaction inside BikeVsBike - is stepped over).
uint32_t FaithfulOracle(NativeEnv& env, uint32_t fn, const uint32_t* a, int n, uint32_t sp,
                        const rr::sim::GuestRegs& r) {
    if (env.skipOracle) return 0;
    Cpu& cpu = env.clone->cpu;
    for (int k = 4; k < n; ++k) env.clone->mem.PokeWord(sp + 16u + 4u * static_cast<uint32_t>(k - 4), a[k]);
    Cpu::CallObservation o;
    o.address = fn;
    for (int k = 0; k < n && k < 12; ++k) o.a[k] = a[k];
    o.sp = sp;
    const size_t outer = env.calls.size();
    (void)outer;
    cpu.callObservations.clear();
    for (int k = 0; k < 4; ++k) cpu.regs[4 + k] = k < n ? a[k] : 0u;
    for (int k = 0; k < 8; ++k) cpu.regs[16 + k] = r.s[k];
    cpu.regs[30] = r.s[8];
    cpu.regs[29] = sp;
    cpu.regs[31] = r.ra;
    cpu.pc = fn;
    cpu.npc = fn + 4u;
    cpu.loadDelayReg = Cpu::kNoLoadDelay;
    cpu.loadDelayValue = 0;
    cpu.AddHook(r.ra, "return");
    Trap t;
    for (;;) {
        t = cpu.Run(kSentinel, env.maxSteps);
        if (t.kind != TrapKind::Hook) break;
        if (cpu.regs[29] == sp) break;                                  // returned to the call site
        cpu.ClearHooks();                                               // a nested pass: step over it
        const Trap s = cpu.Run(kSentinel, 1);
        (void)s;
        cpu.AddHook(r.ra, "return");
    }
    cpu.ClearHooks();
    if (cpu.callObservations.empty()) env.calls.push_back(o);
    else env.calls.insert(env.calls.end(), cpu.callObservations.begin(), cpu.callObservations.end());
    cpu.callObservations.clear();
    if (t.kind != TrapKind::Hook && env.failure.empty()) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "0x%08X", fn);
        env.failure = std::string("oracle callee ") + buf + " did not return to its call site: " + t.ToString();
    }
    return cpu.regs[2];
}

// The callees of the collision ports on the clone: the PORTED ones natively on the same RAM, the
// unported ones from the original code at the exact stack pointer.
struct CloneCallees final : rr::sim::CollisionCallees, rr::sim::RumbleEnv {
    NativeEnv& env;
    EnRun& run;
    explicit CloneCallees(NativeEnv& e, EnRun& r) : env(e), run(r) {}
    bool PlaySound3D(int32_t x, int32_t z, int32_t id, int32_t bank) override {
        rr::sim::GuestPlaySound3D(run.s, x, z, id, bank);
        return !run.s.Faulted();
    }
    bool ReleaseContact(uint32_t e) override {
        Memory& m = env.clone->mem;
        uint8_t* ep = bs::RawAt(m, e, 1096);
        if (ep == nullptr) return false;
        const rr::sim::BikeLinks L = bs::RawLinks(m, e);
        return rr::sim::ReleaseContact(rr::sim::EntityView(ep), L.contact);
    }
    bool Rumble(uint32_t e, uint32_t other, int32_t speed, int32_t k, int32_t div, uint32_t sp) override {
        return rr::sim::PadRumble(run.g, e, other, speed, k, div, sp, *this) && env.failure.empty();
    }
    bool Unported(uint32_t fn, const uint32_t* a, int n, uint32_t sp, uint32_t& v0) override {
        v0 = bs::ExactOracle(env, fn, a, n, sp);
        return env.failure.empty();
    }
    bool UnportedAt(uint32_t fn, const uint32_t* a, int n, uint32_t sp, const rr::sim::GuestRegs& r,
                    uint32_t& v0) override {
        v0 = FaithfulOracle(env, fn, a, n, sp, r);
        return env.failure.empty();
    }
    bool ReadForeignByte(uint32_t address, uint8_t& value) override {
        return env.clone->mem.ReadBlock(address, &value, 1);
    }
    bool PadMotor(uint32_t pad, int32_t a, int32_t b, int32_t c, uint32_t sp) override {
        const uint32_t args[4] = {pad, static_cast<uint32_t>(a), static_cast<uint32_t>(b), static_cast<uint32_t>(c)};
        bs::ExactOracle(env, rr::sim::coll::kPadMotor, args, 4, sp);
        return env.failure.empty();
    }
};

// The registers the guest run's function starts with (CmdPhysImpl: the snapshot's s0..s8, ra = the
// sentinel), which is what a port called directly by a row has as its caller's.
rr::sim::GuestRegs BenchCaller(NativeEnv& env) {
    rr::sim::GuestRegs r;
    for (int k = 0; k < 8; ++k) r.s[k] = env.clone->cpu.regs[16 + k];
    r.s[8] = env.clone->cpu.regs[30];
    r.ra = kSentinel;
    return r;
}

uint32_t Refuse(NativeEnv& env, EnRun& run, bool ok, const char* what) {
    if (!ok && env.failure.empty() && !run.s.Faulted()) env.failure = std::string(what) + " refused";
    return run.Done(env, 0);
}

// ---------------------------------------------------------------- ApplyImpulse, 0x800A8DF0
//
// Scratch: the entity at +0 (1096 bytes), a passenger entity at +2200, the vector at +4400. Driven: the
// pool of the handle (0..7), the passenger pointer (none, the scratch one, the entity ITSELF - the
// original re-reads it for every word, so an aliased passenger moves twice), the list node, the flag.
void SetupImpulse(CaseContext& c, Args& a) {
    Rng& rng = *c.rng;
    Memory& m = *c.mem;
    std::vector<uint8_t> e = BikeImage(c), p = BikeImage(c);
    if (c.sample == nullptr)
        for (uint32_t o = 184; o < 292; o += 4) PutU32V(e, o, static_cast<uint32_t>(rng.World()));
    const uint32_t E = c.scratch, P = c.scratch + 2200, V = c.scratch + 4400;
    PutU16V(e, 172, static_cast<uint16_t>((Pick(rng, {0u, 0u, 0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u}) << 5) | (rng.U32() % 32u)));
    PutU32V(e, 856, Pick(rng, {0u, P, P, E}));
    PutU32V(e, 1088, Chance(rng, 3) ? 0u : rng.U32() | 1u);
    m.WriteBlock(E, e.data(), e.size());
    m.WriteBlock(P, p.data(), p.size());
    for (uint32_t k = 0; k < 3; ++k) m.PokeWord(V + 4 * k, static_cast<uint32_t>(rng.World()));
    a.a[0] = E;
    a.a[1] = V;
    a.a[2] = Pick(rng, {0u, 1u, 1u, rng.U32()});
}
uint32_t NativeImpulse(Memory& m, uint32_t, uint32_t gp, const Args& a) {
    rr::sim::GuestRam g(m.ram().data(), gp);
    rr::sim::ApplyImpulse(g, a.a[0], a.a[1], static_cast<int32_t>(a.a[2]));
    return g.Faulted() ? 0xDEADDEADu : 0u;
}

// ---------------------------------------------------------------- StaleHeading, 0x800A8FE8
//
// Scratch: the bike at +0, a passenger bike at +2200. Driven: flagsC bits 24 and 25; the displacement
// of the box since +0x1D4 (none, below the 132 threshold, the game's size, anything); dt at 0x800CCE38
// of both signs and 0; the passenger (none, the scratch bike with or without a list node). NAMED BOUND:
// a passenger equal to the bike itself with a list node recurses without end on the console (the bit
// it clears is set again before the recursion), so it is not driven.
void SetupStale(CaseContext& c, Args& a) {
    Rng& rng = *c.rng;
    Memory& m = *c.mem;
    const uint32_t E = c.scratch, P = c.scratch + 2200;
    for (uint32_t which = 0; which < 2; ++which) {
        std::vector<uint8_t> e = BikeImage(c);
        uint32_t fc = GetU32V(e, 568);
        fc = Chance(rng, 5) ? (fc & ~0x01000000u) : (fc | 0x01000000u);
        fc = Chance(rng, 2) ? (fc & ~0x02000000u) : (fc | 0x02000000u);
        PutU32V(e, 568, fc);
        const uint32_t mode = static_cast<uint32_t>(rng.Next() % 4u);
        for (uint32_t k = 0; k < 3; ++k) {
            const int32_t base = static_cast<int32_t>(GetU32V(e, 184 + 4 * k));
            int32_t d = 0;
            if (mode == 1) d = static_cast<int32_t>(rng.U32() % 7u) - 3;
            else if (mode == 2) d = static_cast<int32_t>(rng.U32() % 0x60000u) - 0x30000;
            else if (mode == 3) d = rng.S32() >> (rng.Next() % 16u);
            PutU32V(e, 468 + 4 * k, static_cast<uint32_t>(base - d));
        }
        if (which == 0) {
            PutU32V(e, 856, Chance(rng, 2) ? 0u : P);
            PutU32V(e, 1088, Chance(rng, 3) ? 0u : rng.U32() | 1u);
        } else {
            PutU32V(e, 856, 0);
        }
        m.WriteBlock(which == 0 ? E : P, e.data(), e.size());
    }
    m.PokeWord(rr::sim::kCollDt, Pick(rng, {0x884u, 0x884u, 0x442u, 0u, 0xFFFFF77Cu, rng.U32() >> (rng.Next() % 20u)}));
    a.a[0] = E;
}
uint32_t NativeStale(Memory& m, uint32_t, uint32_t gp, const Args& a) {
    rr::sim::GuestRam g(m.ram().data(), gp);
    const cr::Tables tab(m, gp);
    int32_t v = 0;
    const bool ok = rr::sim::StaleHeading(g, a.a[0], tab.t, v);
    return ok ? static_cast<uint32_t>(v) : 0xDEADDEADu;
}

// ---------------------------------------------------------------- InCameraBox, 0x800A8C78
//
// The entity at the scratch; this machine's own view objects (0x800CD898 + 1132 p). Driven: the cache
// bits 2/3 of +0x140; the box centre placed along the first view's axes at forward -1 .. 40 and lateral
// -30 .. 30 (so both tests go both ways, exactly at the 0.5 and `depth` bounds too); the player count
// 1, 2 and 0 (the loop runs once all the same); the half width and depth of the game (20.0, 28.0) or
// anything.
void SetupCamera(CaseContext& c, Args& a) {
    Rng& rng = *c.rng;
    Memory& m = *c.mem;
    std::vector<uint8_t> e = BikeImage(c);
    PutU16V(e, 320, static_cast<uint16_t>((GetU32V(e, 320) & ~0xCu) | (Chance(rng, 4) ? (rng.U32() & 0xCu) : 0u)));
    const uint32_t view = rr::sim::kCollViews;
    int32_t o[3];
    for (uint32_t k = 0; k < 3; ++k) o[k] = GetS32(m, view + 184 + 4 * k);
    int16_t fwd[3], lat[3];
    GetVec3S16(m, view + 444, fwd);
    GetVec3S16(m, view + 432, lat);
    const int32_t f = Pick(rng, {0x8000u, 0x8001u, 0x1C0000u, 0x1BFFFu, 0u}) != 0 && Chance(rng, 3)
                          ? static_cast<int32_t>(Pick(rng, {0x8000u, 0x8001u, 0x1C0000u, 0x1BFFFu}))
                          : static_cast<int32_t>(rng.U32() % 0x2A0000u) - 0x10000;
    const int32_t l = static_cast<int32_t>(rng.U32() % 0x3C0000u) - 0x1E0000;
    for (uint32_t k = 0; k < 3; ++k)
        PutU32V(e, 184 + 4 * k, static_cast<uint32_t>(o[k] + ((static_cast<int64_t>(fwd[k]) * f + static_cast<int64_t>(lat[k]) * l) >> 12)));
    m.WriteBlock(c.scratch, e.data(), e.size());
    SetNumPlayers(m, Pick(rng, {1u, 1u, 2u, 0u}));
    a.a[0] = c.scratch;
    a.a[1] = Chance(rng, 4) ? rng.U32() >> (rng.Next() % 16u) : 0x140000u;
    a.a[2] = Chance(rng, 4) ? rng.U32() >> (rng.Next() % 16u) : 0x1C0000u;
}
uint32_t NativeCamera(Memory& m, uint32_t, uint32_t gp, const Args& a) {
    rr::sim::GuestRam g(m.ram().data(), gp);
    const int32_t v = rr::sim::InCameraBox(g, a.a[0], static_cast<int32_t>(a.a[1]), static_cast<int32_t>(a.a[2]));
    return g.Faulted() ? 0xDEADDEADu : static_cast<uint32_t>(v);
}

// ---------------------------------------------------------------- CornerMin, 0x800B6F40
//
// Scratch: eight corners at +0 (a captured box, or random), the normal at +100, the point at +112,
// depth at +128 and count at +132 (each pointer 0 now and then). The point sits among the corners so
// that 0..8 of them are behind the plane; equal depths test the first strict minimum.
void SetupCornerMin(CaseContext& c, Args& a) {
    Rng& rng = *c.rng;
    Memory& m = *c.mem;
    std::vector<uint8_t> e = BikeImage(c);
    const uint32_t C = c.scratch, N = c.scratch + 100, Q = c.scratch + 112, D = c.scratch + 128, K = c.scratch + 132;
    for (uint32_t i = 0; i < 24; ++i) {
        uint32_t w = GetU32V(e, 196 + 4 * i);
        if (c.sample == nullptr) w = static_cast<uint32_t>(rng.World());
        m.PokeWord(C + 4 * i, w);
    }
    if (Chance(rng, 6)) for (uint32_t k = 0; k < 3; ++k) m.PokeWord(C + 36 + 4 * k, m.PeekWord(C + 12 + 4 * k)); // a tie
    int16_t n[3];
    if (c.sample != nullptr && !Chance(rng, 3)) {
        for (int k = 0; k < 3; ++k) n[k] = c.sample->mB[3 + k];
        if (Chance(rng, 2)) for (int16_t& x : n) x = static_cast<int16_t>(-x);
    } else {
        Dir(rng, n);
    }
    PutVec3S16(m, N, n);
    const uint32_t pick = static_cast<uint32_t>(rng.Next() % 8u);
    for (uint32_t k = 0; k < 3; ++k)
        m.PokeWord(Q + 4 * k, m.PeekWord(C + 12 * pick + 4 * k) + static_cast<uint32_t>(rng.S32() >> (12 + rng.Next() % 8u)));
    m.PokeWord(D, rng.U32());
    m.PokeWord(K, rng.U32());
    a.a[0] = C;
    a.a[1] = N;
    a.a[2] = Q;
    a.a[3] = Chance(rng, 5) ? 0u : D;
    a.a4 = Chance(rng, 3) ? 0u : K;
}
uint32_t NativeCornerMin(Memory& m, uint32_t, uint32_t gp, const Args& a) {
    rr::sim::GuestRam g(m.ram().data(), gp);
    const uint32_t v = rr::sim::CornerMin(g, a.a[0], a.a[1], a.a[2], a.a[3], a.a4);
    return g.Faulted() ? 0xDEADDEADu : v;
}

// ---------------------------------------------------------------- RayPlane, 0x800B6BD0
//
// Scratch: p at +0, dir at +16, n at +24, q at +32. Driven: dir against n with |DotLcm| at and around
// 64 (the parallel refusal), both signs of the numerator and of the dot.
void SetupRayPlane(CaseContext& c, Args& a) {
    Rng& rng = *c.rng;
    Memory& m = *c.mem;
    int16_t d[3], n[3];
    if (c.sample != nullptr) {
        for (int k = 0; k < 3; ++k) { d[k] = c.sample->mA[6 + k]; n[k] = c.sample->mB[3 * (rng.Next() % 3u) + k]; }
    } else {
        Dir(rng, d);
        Dir(rng, n);
    }
    if (Chance(rng, 5)) { // nearly parallel: a small dot of either sign
        n[0] = static_cast<int16_t>(d[2]); n[1] = 0; n[2] = static_cast<int16_t>(-d[0]);
        d[1] = static_cast<int16_t>(d[1] + static_cast<int16_t>(rng.U32() % 9u) - 4);
    }
    for (uint32_t k = 0; k < 3; ++k) {
        const int32_t p = c.sample != nullptr ? c.sample->p[k] : rng.World();
        m.PokeWord(c.scratch + 4 * k, static_cast<uint32_t>(p));
        m.PokeWord(c.scratch + 32 + 4 * k, static_cast<uint32_t>(p + (rng.S32() >> (8 + rng.Next() % 14u))));
    }
    PutVec3S16(m, c.scratch + 16, d);
    PutVec3S16(m, c.scratch + 24, n);
    a.a[0] = c.scratch;
    a.a[1] = c.scratch + 16;
    a.a[2] = c.scratch + 24;
    a.a[3] = c.scratch + 32;
}
uint32_t NativeRayPlane(Memory& m, uint32_t, uint32_t gp, const Args& a) {
    rr::sim::GuestRam g(m.ram().data(), gp);
    const int32_t v = rr::sim::RayPlane(g, a.a[0], a.a[1], a.a[2], a.a[3]);
    return g.Faulted() ? 0xDEADDEADu : static_cast<uint32_t>(v);
}

// ---------------------------------------------------------------- ContactMerge, 0x800B59F0
//
// The contact list 0x800CCE48 filled with 0..8 records whose handle pairs and segment ids collide
// with the new one now and then; the new record is the one after the last (where WallContact writes
// it) or in the scratch. Normals near-parallel, oblique and opposed; depths both ways.
void SetupMerge(CaseContext& c, Args& a) {
    Rng& rng = *c.rng;
    Memory& m = *c.mem;
    const uint32_t n = static_cast<uint32_t>(rng.Next() % 8u);
    m.PokeWord(rr::sim::kContactCount, Chance(rng, 10) ? rng.U32() % 12u : n);
    int16_t nrm[3];
    Dir(rng, nrm);
    const uint16_t hA = static_cast<uint16_t>(rng.U32() % 3u), id = static_cast<uint16_t>(rng.U32() % 3u);
    auto fill = [&](uint32_t r, bool same) {
        for (uint32_t k = 0; k < 36; k += 4) m.PokeWord(r + k, rng.U32() >> (rng.Next() % 12u));
        WriteU16Bench(m, r, same ? hA : static_cast<uint16_t>(rng.U32() % 3u));
        WriteU16Bench(m, r + 2, same ? id : static_cast<uint16_t>(rng.U32() % 3u));
        int16_t v[3];
        const uint32_t mode = static_cast<uint32_t>(rng.Next() % 4u);
        for (int k = 0; k < 3; ++k)
            v[k] = mode == 0 ? nrm[k] : mode == 1 ? static_cast<int16_t>(-nrm[k]) : static_cast<int16_t>(rng.Unit());
        if (mode == 3) Dir(rng, v);
        PutVec3S16(m, r + 4, v);
    };
    for (uint32_t i = 0; i < 9; ++i) fill(rr::sim::kContactList + 36 * i, Chance(rng, 2));
    const uint32_t rec = Chance(rng, 4) ? c.scratch : rr::sim::kContactList + 36 * n;
    fill(rec, true);
    PutVec3S16(m, rec + 4, nrm);
    a.a[0] = rec;
}
uint32_t NativeMerge(Memory& m, uint32_t, uint32_t gp, const Args& a) {
    rr::sim::GuestRam g(m.ram().data(), gp);
    const int32_t v = rr::sim::ContactMerge(g, a.a[0]);
    return g.Faulted() ? 0xDEADDEADu : static_cast<uint32_t>(v);
}

// ---------------------------------------------------------------- ViewTrack, 0x800A451C
//
// A copy of this machine's view object for player 1 at the scratch, its followed bike (+0x238) this
// machine's player, a scratch bike or none. Driven: the flag; the displacement since +0x1D4 (below
// 132, the game's size, past 0x3FFEFFFF squared); dt both signs and 0; +0x224 bit 18, +0x21C at 13/14.
void SetupViewTrack(CaseContext& c, Args& a) {
    Rng& rng = *c.rng;
    Memory& m = *c.mem;
    std::vector<uint8_t> v(1132);
    m.ReadBlock(rr::sim::kCollViews, v.data(), v.size());
    const uint32_t mode = static_cast<uint32_t>(rng.Next() % 4u);
    for (uint32_t k = 0; k < 3; ++k) {
        const int32_t base = static_cast<int32_t>(GetU32V(v, 184 + 4 * k));
        int32_t d = 0;
        if (mode == 1) d = static_cast<int32_t>(rng.U32() % 21u) - 10;
        else if (mode == 2) d = static_cast<int32_t>(rng.U32() % 0x40000u) - 0x20000;
        else if (mode == 3) d = rng.S32() >> (rng.Next() % 8u);
        PutU32V(v, 468 + 4 * k, static_cast<uint32_t>(base - d));
    }
    PutU32V(v, 548, Chance(rng, 4) ? (GetU32V(v, 548) | 0x40000u) : (GetU32V(v, 548) & ~0x40000u));
    PutU32V(v, 540, Pick(rng, {13u, 14u, 12u, 15u, GetU32V(v, 540), rng.U32()}));
    PutU32V(v, 632, rng.S32() >> (rng.Next() % 16u));
    for (uint32_t k = 0; k < 3; ++k) PutU16V(v, 600 + 2 * k, static_cast<uint16_t>(rng.Unit()));
    std::vector<uint8_t> b = BikeImage(c);
    PutU16V(b, 172, static_cast<uint16_t>(Chance(rng, 3) ? (rng.U32() % 256u) : rng.U32() % 18u));
    PutU32V(b, 852, c.scratch + 3200);                 // its rider, mount state drawn around 3
    m.WriteBlock(c.scratch + 2000, b.data(), b.size());
    {
        std::vector<uint8_t> r(640);
        m.ReadBlock(m.PeekWord(Pool0(m, 0) + 852u), r.data(), r.size());
        PutU32V(r, 604, Pick(rng, {0u, 1u, 2u, 3u, 4u, rng.U32()}));
        m.WriteBlock(c.scratch + 3200, r.data(), r.size());
    }
    PutU32V(v, 568, Pick(rng, {0u, Pool0(m, 0), c.scratch + 2000}));
    m.WriteBlock(c.scratch, v.data(), v.size());
    m.PokeWord(rr::sim::kCollDt, Pick(rng, {0x884u, 0x884u, 0u, 0xFFFFF77Cu, rng.U32() >> (rng.Next() % 20u)}));
    a.a[0] = c.scratch;
    a.a[1] = Chance(rng, 2) ? 0u : Pick(rng, {1u, rng.U32()});
}
uint32_t NativeViewTrack(Memory& m, uint32_t, uint32_t gp, const Args& a) {
    rr::sim::GuestRam g(m.ram().data(), gp);
    const cr::Tables tab(m, gp);
    const bool ok = rr::sim::ViewTrack(g, a.a[0], static_cast<int32_t>(a.a[1]), tab.t);
    return ok ? 0u : 0xDEADDEADu;
}

// ---------------------------------------------------------------- PadRumble, 0x800B658C
//
// e at the scratch (a copy of a captured bike), `other` none / a scratch rider-like entity / a scratch
// bike, e's rider at +4400 and its passenger (+0x358) none or a scratch bike whose rider is at +5100.
// Driven: the speed (0, the game's, anything), k (0x165A1C, 0 - the R3000 divide by zero - or
// anything), div (1, 2, 0, anything), the flag bits 4/5/6 of the riders' +0x23C, their mount states,
// the handles (players or not) and the player count. With no passenger the original reads the word at
// 0x354 and follows it: into the BIOS ROM on this machine, which the port asks the caller for.
void SetupRumble(CaseContext& c, Args& a) {
    Rng& rng = *c.rng;
    Memory& m = *c.mem;
    const uint32_t E = c.scratch, O = c.scratch + 1100, PB = c.scratch + 2200, R = c.scratch + 4400,
                   PR = c.scratch + 5100, OB = c.scratch + 5800;
    std::vector<uint8_t> e = BikeImage(c), o = BikeImage(c), pb = BikeImage(c), ob(1096, 0);
    auto rider = [&](uint32_t at) {
        std::vector<uint8_t> r(640);
        for (uint8_t& x : r) x = static_cast<uint8_t>(rng.U32());
        r[572] = static_cast<uint8_t>(rng.U32() & 0x70u);
        PutU32V(r, 604, Pick(rng, {0u, 1u, 2u, 3u, rng.U32()}));
        m.WriteBlock(at, r.data(), r.size());
    };
    rider(R);
    rider(PR);
    PutU16V(e, 172, static_cast<uint16_t>(Chance(rng, 2) ? rng.U32() % 2u : rng.U32() % 224u));
    PutU32V(e, 852, R);
    PutU32V(e, 856, Chance(rng, 2) ? 0u : PB);
    PutU32V(pb, 852, PR);
    PutU16V(pb, 172, static_cast<uint16_t>(rng.U32() % 18u));
    PutU16V(o, 172, static_cast<uint16_t>(Chance(rng, 2) ? (32u + rng.U32() % 3u) : rng.U32() % 224u));
    o[572] = static_cast<uint8_t>(rng.U32() & 0x20u);
    PutU32V(o, 596, OB);
    PutU16V(ob, 172, static_cast<uint16_t>(rng.U32() % 18u));
    PutU32V(ob, 856, PB);
    m.WriteBlock(E, e.data(), e.size());
    m.WriteBlock(O, o.data(), o.size());
    m.WriteBlock(PB, pb.data(), pb.size());
    m.WriteBlock(OB, ob.data(), ob.size());
    SetNumPlayers(m, Pick(rng, {1u, 1u, 2u}));
    a.a[0] = E;
    a.a[1] = Chance(rng, 2) ? 0u : O;
    a.a[2] = Pick(rng, {0u, static_cast<uint32_t>(c.sample != nullptr ? c.sample->speed : rng.S32() >> 8),
                        rng.U32() >> (rng.Next() % 16u), rng.U32()});
    a.a[3] = Pick(rng, {0x165A1Cu, 0x165A1Cu, 0u, rng.U32() >> (rng.Next() % 24u)});
    a.a4 = Pick(rng, {1u, 1u, 2u, 0u, rng.U32() % 300u, rng.U32()});
}
uint32_t NativeRumble(NativeEnv& env, const Args& a) {
    EnRun run(env);
    CloneCallees cc(env, run);
    const bool ok = rr::sim::PadRumble(run.g, a.a[0], a.a[1], static_cast<int32_t>(a.a[2]),
                                       static_cast<int32_t>(a.a[3]), static_cast<int32_t>(a.a4), env.sp, cc);
    return Refuse(env, run, ok, "PadRumble");
}

// ---------------------------------------------------------------- the airborne bike
//
// The bike is planted at the scratch as a copy of a captured one (or of this machine's player for a
// randomised case), its pointers re-aimed at scratch records: the rider (+0x354, 640 bytes at +4400,
// its +0x228 / +0x25C / +0x23C drawn), a passenger bike (+0x358, at +2200, none half the time) and a
// contact record (+0x340 at +5100, none half the time; its first halfword a handle of pool 0, 3, 4 or
// anything, which ReleaseContact resolves on this machine's pools). The handle is a player's (0) or
// not, the player count 1 or 2, so the view object 0x800CD898 + 1132 h is this machine's.
//
// The GROUND: +0x1F8 is the ground point and -(+0x20A) the plane's normal. The box (centre and eight
// corners) is placed so that its deepest corner is from 1.0 above the plane to 6.0 below it, and
// +0x2C4 (the airborne bike's clearance) is drawn around that depth - so the airborne test goes both
// ways. Driven besides: flagsC 0x200 / 0x400 / both / neither, bits 19, 21 and 25; flagsB bits 15, 20
// and 23; the surface byte +0x216 = 4; the tumble counter +0x2F4 at and around 32767 and 0x1FFFF;
// the pre-contact speed +0x35C around 0x8F0D7; the drop +0x300 around 3.0; the yaw +0x1E8 on both sides
// of +-5.0; +0x2B4; the global 0x8005B220; the LCG seed (Spin draws three numbers).
//
// NAMED BOUNDS (the ranges the original's callees need, as for Bounce in rows_crash.inc):
// the plane normal +0x20A and the pre-contact heading +0x360 are directions of length <= 1.0.
struct AirLayout {
    uint32_t E, P, R, PR, CT;
};
AirLayout PlantAirBike(CaseContext& c) {
    Rng& rng = *c.rng;
    Memory& m = *c.mem;
    AirLayout L{c.scratch, c.scratch + 2200, c.scratch + 4400, c.scratch + 5100, c.scratch + 5800};
    std::vector<uint8_t> e;
    if (c.sample != nullptr && c.sample->entity.size() == 1096) e = c.sample->entity;
    else {
        e.assign(1096, 0);
        m.ReadBlock(Pool0(m, 0), e.data(), e.size());
    }
    // the plane: its point +0x1F8, its "up" row +0x20A (the bike's second orientation row)
    int16_t up[3];
    for (uint32_t k = 0; k < 3; ++k) up[k] = static_cast<int16_t>(GetU32V(e, 522 + 2 * k));
    if (c.sample == nullptr || Chance(rng, 4)) {
        Dir(rng, up);
        if (Chance(rng, 2)) { up[0] = static_cast<int16_t>(up[0] / 8); up[2] = static_cast<int16_t>(up[2] / 8); up[1] = static_cast<int16_t>(up[1] < 0 ? -4000 : 4000); }
    }
    for (uint32_t k = 0; k < 3; ++k) PutU16V(e, 522 + 2 * k, static_cast<uint16_t>(up[k]));
    // the box: centre and corners moved by the same vector so the deepest corner lands at depth d
    int32_t lo = 0x7FFFFFFF;
    const int32_t q[3] = {static_cast<int32_t>(GetU32V(e, 504)), static_cast<int32_t>(GetU32V(e, 508)),
                          static_cast<int32_t>(GetU32V(e, 512))};
    for (uint32_t cix = 0; cix < 8; ++cix) {
        int64_t s = 0;
        for (uint32_t k = 0; k < 3; ++k)
            s += static_cast<int64_t>(static_cast<int32_t>(GetU32V(e, 196 + 12 * cix + 4 * k)) - q[k]) * up[k];
        lo = std::min<int32_t>(lo, static_cast<int32_t>(s >> 12));
    }
    // depth measured along -up: a corner at distance s along +up is "behind" by s. Move the box along
    // +up by (want - max) where max is the largest s.
    int32_t hi = -0x7FFFFFFF;
    for (uint32_t cix = 0; cix < 8; ++cix) {
        int64_t s = 0;
        for (uint32_t k = 0; k < 3; ++k)
            s += static_cast<int64_t>(static_cast<int32_t>(GetU32V(e, 196 + 12 * cix + 4 * k)) - q[k]) * up[k];
        hi = std::max<int32_t>(hi, static_cast<int32_t>(s >> 12));
    }
    (void)lo;
    const int32_t want = static_cast<int32_t>(rng.U32() % 0x70000u) - 0x10000;   // -1.0 .. 6.0 behind
    const int32_t shift = want - hi;
    for (uint32_t off = 184; off < 292; off += 4) {
        const uint32_t k = ((off - 184) / 4) % 3;
        PutU32V(e, off, GetU32V(e, off) + static_cast<uint32_t>((static_cast<int64_t>(shift) * up[k]) >> 12));
    }
    PutU32V(e, 708, static_cast<uint32_t>(Chance(rng, 3) ? rng.S32() >> (8 + rng.Next() % 16u)
                                                          : want + static_cast<int32_t>(rng.U32() % 0x8000u) - 0x4000));
    // flags
    uint32_t fc = GetU32V(e, 568) & ~0x02380600u;
    fc |= Pick(rng, {0x400u, 0x400u, 0x200u, 0x200u, 0x600u, 0u});
    if (Chance(rng, 3)) fc |= 0x80000u;
    if (Chance(rng, 3)) fc |= 0x200000u;
    if (Chance(rng, 3)) fc |= 0x02000000u;
    if (Chance(rng, 8)) fc |= 0x100000u;
    PutU32V(e, 568, fc);
    uint32_t fb = GetU32V(e, 564) & ~0x00908000u;
    if (Chance(rng, 6)) fb |= 0x8000u;
    if (Chance(rng, 6)) fb |= 0x100000u;
    if (Chance(rng, 8)) fb |= 0x800000u;
    PutU32V(e, 564, fb);
    if (Chance(rng, 8)) e[534] = 4;
    PutU32V(e, 756, Pick(rng, {32767u, 32768u, 0x1FFFFu, 0x20000u, 0u, rng.U32() % 0x40000u, rng.U32()}));
    PutU32V(e, 860, Pick(rng, {0x8F0D7u, 0x8F0D8u, 0u, rng.U32() % 0x400000u, static_cast<uint32_t>(rng.S32() >> 10)}));
    PutU32V(e, 768, Pick(rng, {0x30000u, 0x30001u, 0u, rng.U32() % 0x80000u, rng.U32()}));
    PutU32V(e, 488, Pick(rng, {0x50000u, 0x4FFFFu, 0xFFFB0000u, 0xFFFB0001u, 0u, 0x80000000u,
                               static_cast<uint32_t>(rng.S32() >> (8 + rng.Next() % 12u))}));
    PutU32V(e, 692, rng.U32());
    for (uint32_t k = 0; k < 3; ++k) PutU32V(e, 456 + 4 * k, static_cast<uint32_t>(rng.S32() >> (8 + rng.Next() % 10u)));
    // the pre-contact heading +0x360 (now and then straight along +up, the -0.9 clamp of the tumble
    // re-aim), and the heading +0x1C2 against +0x1BC (TouchDown's 0.9 / 0.1): directions
    if (c.sample == nullptr || Chance(rng, 3)) {
        int16_t d[3];
        Dir(rng, d);
        if (Chance(rng, 4)) for (uint32_t k = 0; k < 3; ++k) d[k] = up[k];
        for (uint32_t k = 0; k < 3; ++k) PutU16V(e, 864 + 2 * k, static_cast<uint16_t>(d[k]));
    }
    if (Chance(rng, 3)) {
        int16_t d[3], f[3];
        Dir(rng, d);
        Dir(rng, f);
        for (uint32_t k = 0; k < 3; ++k) {
            PutU16V(e, 450 + 2 * k, static_cast<uint16_t>(d[k]));
            PutU16V(e, 444 + 2 * k, static_cast<uint16_t>(f[k]));
        }
    }
    // handle and pointers
    const uint32_t np = Pick(rng, {1u, 1u, 2u});
    SetNumPlayers(m, np);
    PutU16V(e, 172, static_cast<uint16_t>(Pick(rng, {0u, 0u, 1u, 6u, 17u})));
    PutU32V(e, 852, L.R);
    PutU32V(e, 856, Chance(rng, 2) ? 0u : L.P);
    PutU32V(e, 832, Chance(rng, 2) ? 0u : L.CT);
    PutU32V(e, 828, rng.U32());
    PutU32V(e, 1088, rng.U32() | 1u);
    m.WriteBlock(L.E, e.data(), e.size());
    // the passenger: another captured bike, its box shifted like e's
    {
        std::vector<uint8_t> p = BikeImage(c);
        for (uint32_t off = 184; off < 292; off += 4)
            PutU32V(p, off, GetU32V(e, off) + static_cast<uint32_t>(static_cast<int32_t>(rng.U32() % 0x20000u) - 0x10000));
        PutU32V(p, 852, L.PR);
        PutU32V(p, 856, 0);
        PutU32V(p, 1088, 0);
        m.WriteBlock(L.P, p.data(), p.size());
    }
    // the riders
    for (uint32_t at : {L.R, L.PR}) {
        std::vector<uint8_t> r(640);
        const uint32_t real = m.PeekWord(Pool0(m, 0) + 852u);
        m.ReadBlock(real, r.data(), r.size());
        PutU32V(r, 552, rng.U32() & 0x000FA000u);
        PutU32V(r, 604, Pick(rng, {0u, 1u, 2u, 3u, 4u}));
        r[572] = static_cast<uint8_t>(rng.U32() & 0x70u);
        m.WriteBlock(at, r.data(), r.size());
    }
    // the contact record ReleaseContact reads (0x118 bytes)
    {
        std::vector<uint8_t> ct(0x120);
        for (uint8_t& x : ct) x = static_cast<uint8_t>(rng.U32());
        const uint32_t h = Pick(rng, {0u << 5, (0u << 5) | 6u, 3u << 5, 4u << 5, 1u << 5, 2u << 5, 5u << 5});
        PutU16V(ct, 0, static_cast<uint16_t>(h));
        m.WriteBlock(L.CT, ct.data(), ct.size());
    }
    // the players' view records: +0x314 negative now and then (a set rider +0x228 bit 19 clears it)
    for (uint32_t p = 0; p < 2; ++p)
        if (Chance(rng, 3)) m.PokeWord(rr::sim::kCollViews + 1132u * p + 788u, Pick(rng, {0xFFFF0000u, 0x80000000u, 0x10000u}));
    m.PokeWord(kSkipResults, Chance(rng, 4) ? 1u : 0u);
    m.PokeWord(c.gp + kRandSeedGpOffset, rng.U32());
    return L;
}

void SetupAir(CaseContext& c, Args& a) {
    const AirLayout L = PlantAirBike(c);
    a.a[0] = L.E;
}
uint32_t NativeAir(NativeEnv& env, const Args& a) {
    EnRun run(env);
    CloneCallees cc(env, run);
    const cr::Tables tab(env.clone->mem, env.gp);
    const bool ok = rr::sim::AirContact(run.g, a.a[0], env.sp, tab.t, cc);
    return Refuse(env, run, ok, "AirContact");
}

// TouchDown(e, pSpeed): the same planted bike; pSpeed is its +0x35C or a scratch word.
void SetupTouch(CaseContext& c, Args& a) {
    Rng& rng = *c.rng;
    const AirLayout L = PlantAirBike(c);
    c.mem->PokeWord(c.scratch + 8000, Pick(rng, {0u, 0x8F0D8u, rng.U32() % 0x400000u, rng.U32()}));
    PutS32(*c.mem, L.E + 460, Chance(rng, 4) ? rng.S32() : rng.S32() >> (4 + rng.Next() % 16u));
    a.a[0] = L.E;
    a.a[1] = Chance(rng, 2) ? L.E + 860 : c.scratch + 8000;
}
uint32_t NativeTouch(NativeEnv& env, const Args& a) {
    EnRun run(env);
    CloneCallees cc(env, run);
    const bool ok = rr::sim::TouchDown(run.g, a.a[0], a.a[1], env.sp, cc);
    return Refuse(env, run, ok, "TouchDown");
}


// ---------------------------------------------------------------- WallContact, 0x800B3AD0
//
// The entity is a scratch COPY of one of this machine's own entities whose road pointers are live -
// the player's bike, bike 6, or the player-1 view object (handle pool 4 slot 31) - because the walls
// it walks are reached through its +0x148 road section, +0x154 road frame, +0x174 road piece and
// +0x1EC group ranges, all of which must name this machine's resident road. For
// a dump-derived case the captured bike's orientation rows, heading, speed and steering are laid over
// the copy (its positions are another machine's, so they are not). Its handle is then drawn: a bike
// (pool 0; a player or not), a rider (pool 1), a pedestrian (pool 2), a prop (pool 4) or a view object
// (pool 4, slot 30/31) - the unported reactions (BikeWallHit, RiderWallHit, PedHit, PropTopple) then
// run from the original code on this machine's entity of that slot, as they do on the console.
//
// PLACEMENT: one wall segment of the groups the entity scans is chosen and the box (centre, the eight
// corners, +0x1D4, +0x1F8, +0x310) is moved so that its centre is on the segment's line at a
// parameter -0.2 .. 1.2, 0..3.0 units on either side along the segment's normal; +0x1D4 is moved back
// along the heading by up to 4.0 (so a box can cross the wall between the two). +0x158 is set on that
// side, beyond the edge gate or not; `force` 0 or 1.
//
// DATA EDITS OF THE ROAD: on one case in
// two the flag words of the chosen group's segments are rewritten - type 0..15 (0 and >= 9 are
// rewritten to 0x402 by the function itself), bits 0x100 / 0x200 / 0x400 / 0x800 / 0x8000 -, and their
// +4 value, +0x26 height mode and the normal's y. Also drawn: +0x184 (the last type nibble, bit 0),
// +0x1F0 (an explicit group), +0x140 bits 2/3 (the camera cache), the list node +0x440, a passenger
// (+0x358, a second scratch copy), +0x2A4 (mode 2 at 2130), +0x27C, +0x228, +0x25C, +0x250, the
// contact count and same-key records (ContactMerge), the player count, dt, the view's +0x300 / +0x224.
void SetupWall(CaseContext& c, Args& a) {
    Rng& rng = *c.rng;
    Memory& m = *c.mem;
    const uint32_t E = c.scratch, PS = c.scratch + 2200;
    uint32_t kind = static_cast<uint32_t>(rng.Next() % 20u);         // 0..11 bike, 12..13 rider, 14..15 ped,
    if (const char* k = std::getenv("RRJB_WALLKIND")) kind = static_cast<uint32_t>(std::atoi(k));
    // NAMED BOUND: no prop (pool 4 below slot 30). Pool 4 is empty on this machine (and in the
    // product), and the prop reaction PropTopple 0x800B3344 - run from the original code - reads a
    // depth that CornerMin leaves UNWRITTEN when no corner is behind the plane, i.e. a word of stack
    // below WallContact's frame that the ORIGINAL's own callees happen to leave there. The port does
    // not reproduce the original's callee frames, so on such a call the two sides differ by
    // construction (measured: 136 of 848 prop cases, every difference in the prop's own box). A port of
    // PropTopple would have to reproduce that stale word; until then the prop arm is the original's.
    if (!std::getenv("RRJB_WALLKIND") && (kind == 16 || kind == 17)) kind = 20 + kind;
    const bool isView = kind == 18 || kind == 19, isProp = kind == 16 || kind == 17; // 16..17 prop, 18..19 view
    std::vector<uint8_t> e(1132, 0);
    const uint32_t src = isView ? rr::sim::kCollViews : Pool0(m, Chance(rng, 3) ? 6u : 0u);
    m.ReadBlock(src, e.data(), isView ? 1132u : 1096u);
    if (c.sample != nullptr && !isView && c.sample->entity.size() == 1096) {
        const std::vector<uint8_t>& s = c.sample->entity;
        for (uint32_t o = 0x1B0; o < 0x216; ++o) e[o] = s[o];                 // rows, heading, facing
        for (uint32_t o : {0x1E0u, 0x240u, 0x27Cu, 0x2A4u, 0x228u}) PutU32V(e, o, GetU32V(s, o));
    }
    // the side-group record: the entity's own, or bike 6's (two groups a side) now and then
    if (Chance(rng, 4)) PutU32V(e, 0x1EC, m.PeekWord(Pool0(m, 6) + 0x1EC));
    uint32_t handle = GetU32V(e, 172) & 0xFFFFu;
    if (kind < 12 || kind >= 20) handle = Chance(rng, 3) ? 0u : rng.U32() % 18u;
    else if (kind < 14) handle = 32u + (Chance(rng, 2) ? 6u : rng.U32() % 18u);
    else if (kind < 16) handle = 64u + rng.U32() % 3u;
    else if (isProp) handle = 128u + rng.U32() % 6u;
    else handle = 128u + 30u + rng.U32() % 2u;
    PutU16V(e, 172, static_cast<uint16_t>(handle));
    // the walls this entity scans
    const uint32_t S = GetU32V(e, 0x148), q = GetU32V(e, 0x1EC);
    const uint32_t segs = m.PeekWord(S + 0x60), grps = m.PeekWord(S + 0x64);
    const bool left = Chance(rng, 2);
    const uint32_t g0 = m.PeekByte(q + (left ? 0u : 2u)), gn = m.PeekByte(q + (left ? 1u : 3u));
    const uint32_t gi = g0 + (gn ? rng.U32() % gn : 0u);
    const uint32_t gr = grps + 4u * gi;
    const int32_t first = GetS16(m, gr);
    const uint32_t count = m.PeekByte(gr + 3);
    const uint32_t si = static_cast<uint32_t>(first) + (count ? rng.U32() % count : 0u);
    const uint32_t seg = segs + 40u * si;
    int32_t p0[3], p1[3];
    GetVec3S32(m, seg + 8, p0);
    GetVec3S32(m, seg + 20, p1);
    int16_t n[3];
    GetVec3S16(m, seg + 32, n);
    const int64_t tq = static_cast<int64_t>(rng.U32() % 0x16666u) - 0x3333;   // -0.2 .. 1.2
    const int64_t side = Chance(rng, 5) ? -static_cast<int64_t>(0x30000u + rng.U32() % 0x30000u)   // wholly behind
                                        : static_cast<int64_t>(rng.U32() % 0x60000u) - 0x30000;   // -3.0 .. 3.0
    int32_t target[3];
    for (uint32_t k = 0; k < 3; ++k)
        target[k] = static_cast<int32_t>(p0[k] + (((static_cast<int64_t>(p1[k]) - p0[k]) * tq) >> 16) + ((side * n[k]) >> 12));
    const int32_t dx = target[0] - static_cast<int32_t>(GetU32V(e, 184)), dz = target[2] - static_cast<int32_t>(GetU32V(e, 192));
    const int32_t dy = Chance(rng, 2) ? 0 : static_cast<int32_t>(rng.U32() % 0x60000u) - 0x30000;
    for (uint32_t f : {0x0B8u, 0x1D4u, 0x1F8u, 0x310u, 0x0C4u, 0x0D0u, 0x0DCu, 0x0E8u, 0x0F4u, 0x100u, 0x10Cu, 0x118u}) {
        PutU32V(e, f, GetU32V(e, f) + static_cast<uint32_t>(dx));
        PutU32V(e, f + 4, GetU32V(e, f + 4) + static_cast<uint32_t>(dy));
        PutU32V(e, f + 8, GetU32V(e, f + 8) + static_cast<uint32_t>(dz));
    }
    // heading: toward or away from the wall, along it, or anything; +0x1D4 back along it
    int16_t h[3];
    if (Chance(rng, 2)) {
        h[0] = static_cast<int16_t>(-n[0]); h[1] = 0; h[2] = static_cast<int16_t>(-n[2]);
        if (Chance(rng, 3)) { h[0] = static_cast<int16_t>(-h[0]); h[2] = static_cast<int16_t>(-h[2]); }
        if (Chance(rng, 2)) { h[0] = static_cast<int16_t>((h[0] + n[2]) / 2); h[2] = static_cast<int16_t>((h[2] - n[0]) / 2); }
    } else {
        Dir(rng, h);
    }
    for (uint32_t k = 0; k < 3; ++k) PutU16V(e, 450 + 2 * k, static_cast<uint16_t>(h[k]));
    const int32_t spd = static_cast<int32_t>(Pick(rng, {0u, 0x10000u, 0x80000u, 0x1E0000u, 0x3C0000u, rng.U32() % 0x500000u}));
    PutU32V(e, 480, static_cast<uint32_t>(spd));
    const int32_t back = static_cast<int32_t>(rng.U32() % 0x40000u);
    for (uint32_t k = 0; k < 3; ++k)
        PutU32V(e, 468 + 4 * k, GetU32V(e, 184 + 4 * k) - static_cast<uint32_t>((static_cast<int64_t>(h[k]) * back) >> 12));
    // the lateral offset on that side, past the edge gate or not
    const int32_t lat = static_cast<int32_t>(rng.U32() % 0x200000u);
    PutU32V(e, 0x158, static_cast<uint32_t>(left ? -lat : lat));
    // the entity's own state words
    PutU32V(e, 388, Chance(rng, 3) ? rng.U32() : GetU32V(e, 388));
    PutU32V(e, 496, Chance(rng, 8) ? gr : 0u);
    PutU16V(e, 320, static_cast<uint16_t>((GetU32V(e, 320) & ~0xCu) | (rng.U32() & 0xCu)));
    PutU32V(e, 676, Pick(rng, {0u, 2129u, 2130u, 0xFFFFF7AEu, rng.U32()}));
    PutU32V(e, 636, rng.U32());
    PutU32V(e, 552, rng.U32());
    PutU32V(e, 604, Pick(rng, {0u, 2u, 4u, 4u, rng.U32()}));
    PutU32V(e, 592, rng.U32());
    PutU32V(e, 364, Pick(rng, {0u, 1u, 2u, 0xFFFFFFFEu, rng.U32()}));   // +0x16C: the step wall's side
    PutU32V(e, 596, Pool0(m, 0));
    if (isView) {
        PutU32V(e, 768, Chance(rng, 2) ? 0u : seg);
        PutU32V(e, 548, rng.U32());
    }
    // The other rider's bike. A LISTED bike (+0x440 != 0) carries a passenger (+0x358) or none; an
    // UNLISTED one is itself a passenger, and its +0x358 names its listed owner (WallContact takes the
    // owner as the bike that is charged, 0x800B54B8). NAMED BOUND: an unlisted pool-0 entity without an
    // owner is not a state of the game - the original then charges "bike 0x0" (a store at 0x228) and
    // hands address 0 to BikeWallHit, which chases *(0x354) into the BIOS ROM and stores there (the
    // interpreter stops on that store; the console would drop it).
    if (!isView) {
        const bool unlisted = (kind < 12 || kind >= 20) && Chance(rng, 4);
        PutU32V(e, 1088, unlisted ? 0u : (GetU32V(e, 1088) | 1u));
        if (unlisted || Chance(rng, 4)) {
            std::vector<uint8_t> ps = e;
            for (uint32_t f = 184; f < 292; f += 4)
                PutU32V(ps, f, GetU32V(ps, f) + static_cast<uint32_t>(static_cast<int32_t>(rng.U32() % 0x20000u) - 0x10000));
            PutU32V(ps, 856, unlisted ? E : 0u);
            PutU32V(ps, 1088, unlisted ? (GetU32V(ps, 1088) | 1u) : 0u);
            PutU16V(ps, 172, static_cast<uint16_t>(rng.U32() % 18u));
            m.WriteBlock(PS, ps.data(), 1096);
            PutU32V(e, 856, PS);
        } else {
            PutU32V(e, 856, 0);
        }
    }
    m.WriteBlock(E, e.data(), isView ? 1132u : 1096u);
    // the road data edits
    if (Chance(rng, 2)) {
        for (uint32_t k = 0; k < count; ++k) {
            const uint32_t sa = segs + 40u * (static_cast<uint32_t>(first) + k);
            uint16_t f = static_cast<uint16_t>(Pick(rng, {1u, 2u, 3u, 3u, 5u, 5u, 8u, 0u, 9u, rng.U32() % 16u}));
            if (Chance(rng, 2)) f = static_cast<uint16_t>(f | 0x100u);
            if (Chance(rng, 4)) f = static_cast<uint16_t>(f | 0x200u);
            if (Chance(rng, 3)) f = static_cast<uint16_t>(f | 0x400u);
            if (Chance(rng, 5)) f = static_cast<uint16_t>(f | 0x800u);
            // NAMED BOUND: bit 15 on a type-5 segment hangs the original. `(u32)(s16)flags != (u32)prevF`
            // (0x800B4264) is then true for ever, so the step arm fires on every pass over the segment,
            // adds one to the segment count and does not advance - the walk never ends (measured: the
            // guest runs out of its 5 000 000-instruction budget). No shipped segment has bit 15.
            if (Chance(rng, 12) && (f & 0xFu) != 5u) f = static_cast<uint16_t>(f | 0x8000u);
            WriteU16Bench(m, sa + 2, f);
            if (Chance(rng, 3)) m.PokeWord(sa + 4, static_cast<uint32_t>(static_cast<int32_t>(rng.U32() % 0x60000u) - 0x30000));
            WriteU16Bench(m, sa + 38, static_cast<uint16_t>(Pick(rng, {0u, 0u, 1u, 2u})));
            if (Chance(rng, 4)) WriteU16Bench(m, sa + 34, static_cast<uint16_t>(static_cast<int32_t>(rng.U32() % 400u) - 200));
        }
    }
    // the contact list: a count 0..8 and records keyed like the ones this call may write
    const uint32_t cnt = Pick(rng, {0u, 0u, 1u, 3u, 7u, 8u});
    m.PokeWord(rr::sim::kContactCount, cnt);
    for (uint32_t i = 0; i < cnt; ++i) {
        const uint32_t r = rr::sim::kContactList + 36u * i;
        for (uint32_t k = 0; k < 36; k += 4) m.PokeWord(r + k, rng.U32() >> (rng.Next() % 16u));
        WriteU16Bench(m, r, static_cast<uint16_t>(Chance(rng, 2) ? handle : rng.U32() % 3u));
        WriteU16Bench(m, r + 2, static_cast<uint16_t>(Chance(rng, 2) ? (m.PeekByte(seg) | (m.PeekByte(seg + 1) << 8)) : rng.U32()));
        int16_t v[3];
        GetVec3S16(m, seg + 32, v);
        if (Chance(rng, 3)) Dir(rng, v);
        PutVec3S16(m, r + 4, v);
    }
    SetNumPlayers(m, Pick(rng, {1u, 1u, 1u, 2u}));
    m.PokeWord(rr::sim::kCollDt, Pick(rng, {0x884u, 0x884u, 0x442u, 0u, rng.U32() % 0x4000u}));
    a.a[0] = E;
    a.a[1] = Chance(rng, 2) ? 0u : Pick(rng, {1u, 1u, rng.U32()});
    if (std::getenv("RRJB_COLLDBG") != nullptr) {
        static long calls = 0;
        if ((calls++ & 1) == 0)
            std::printf("  wall case %ld: kind %u handle 0x%X seg %u (group %u, %u segs) left %d force 0x%X cnt %u\n",
                        calls / 2, kind, handle, si, gi, count, left ? 1 : 0, a.a[1], cnt);
    }
}
uint32_t NativeWall(NativeEnv& env, const Args& a) {
    EnRun run(env);
    CloneCallees cc(env, run);
    const cr::Tables tab(env.clone->mem, env.gp);
    uint32_t v0 = 0;
    const bool ok = rr::sim::WallContact(run.g, a.a[0], static_cast<int32_t>(a.a[1]), env.sp, tab.t, cc, v0);
    if (!ok && env.failure.empty() && !run.s.Faulted()) env.failure = "WallContact refused";
    return run.Done(env, v0);
}


// ---------------------------------------------------------------- CollisionPass, 0x800A4774
//
// The pass works on the whole machine - the seven pools, the grid, the contact globals - so a case is
// THIS machine (rr-race: the player and bike 6 live, rider 6 off its bike, three pedestrians, one
// car) with its entities moved and flagged, never an entity lifted out of another image (its road,
// rider and stat pointers would name another machine's data, and the unported resolvers run from the
// original code would follow them). What a case draws:
//   * dt: the frame's, half of it, 0 (the prime pass's call) or anything small;
//   * 0..5 dormant AI bikes made live (+0x140) and, with the live ones, placed around the player's box
//     (every position field moved together) at -10 .. 10 units, level or not; for a dump-derived case
//     one of them takes the captured bike's orientation rows, heading and speed;
//   * per bike: the wipeout kinds (flagsC 0x1E0) with bit 21 or a large +0x28C (the POINT resolver
//     choice), 0x600 (airborne / crashed, the AirContact tail and WallContact's force), flagsB bits
//     22 / 23 (the impact hand-over, the AirContact gate), +0x268 / +0x2A4 (the stale bit), the
//     rider's stance +0x220 (0 sends the pair to the point resolver) and mount state +0x25C;
//   * the off-bike rider 6, a pedestrian and the car moved next to the player or left where they are;
//     rider 6's mount state (a rider enters the grid only at 2 and above);
//   * the player airborne above or sunk into the ground (AirContact) half the time;
//   * two players (player 2 = a live bike), the contact globals left by "a previous pass", junk in
//     the grid (the pass clears it).
// NAMED BOUNDS: the chain sentinel 0x800CD0A8 is the machine's own {0x80, 0xFF};
// no prop and no static volume is made live (pool 4 and 6 are empty on this machine and in the
// product; the prop reaction reads stale stack, see SetupWall); every bike made live is one of this
// machine's 18 with its own pointers.
constexpr uint32_t kBoxFields[] = {0x1D4, 0x0B8, 0x1F8, 0x310, 0x0C4, 0x0D0, 0x0DC, 0x0E8, 0x0F4, 0x100, 0x10C, 0x118};
void MoveBox(Memory& m, uint32_t e, int32_t dx, int32_t dy, int32_t dz) {
    for (uint32_t f : kBoxFields) {
        PutS32(m, e + f, GetS32(m, e + f) + dx);
        PutS32(m, e + f + 4, GetS32(m, e + f + 4) + dy);
        PutS32(m, e + f + 8, GetS32(m, e + f + 8) + dz);
    }
}
void MoveNear(Memory& m, uint32_t e, uint32_t to, int32_t ox, int32_t oy, int32_t oz) {
    MoveBox(m, e, GetS32(m, to + 184) + ox - GetS32(m, e + 184), GetS32(m, to + 188) + oy - GetS32(m, e + 188),
            GetS32(m, to + 192) + oz - GetS32(m, e + 192));
}

void SetupPass(CaseContext& c, Args& a) {
    Rng& rng = *c.rng;
    Memory& m = *c.mem;
    auto off = [&rng](int32_t units) { return static_cast<int32_t>(rng.U32() % (2u * 65536u * static_cast<uint32_t>(units))) - 65536 * units; };
    const uint32_t P = Pool0(m, 0);
    // bikes made live and placed
    const uint32_t extra = static_cast<uint32_t>(rng.Next() % 6u);
    std::vector<uint32_t> bikes = {6u};
    for (uint32_t i = 0; i < extra; ++i) bikes.push_back(1u + rng.U32() % 17u);
    bool laid = false;
    for (uint32_t slot : bikes) {
        const uint32_t b = Pool0(m, slot);
        if (GetS16(m, b + 320) == 0) WriteU16Bench(m, b + 320, static_cast<uint16_t>(Pick(rng, {1u, 3u, 0x13u})));
        if (!Chance(rng, 4)) MoveNear(m, b, P, off(10), Chance(rng, 3) ? off(1) : 0, off(10));
        if (c.sample != nullptr && !laid && c.sample->entity.size() == 1096) {
            laid = true;
            m.WriteBlock(b + 0x1B0, c.sample->entity.data() + 0x1B0, 0x216 - 0x1B0);
            PutS32(m, b + 0x1E0, c.sample->speed);
        }
    }
    bikes.push_back(0u);
    for (uint32_t slot : bikes) {
        const uint32_t b = Pool0(m, slot);
        uint32_t fc = m.PeekWord(b + 568);
        if (Chance(rng, 5)) fc = (fc & ~0x1E0u) | (Pick(rng, {0x20u, 0x40u, 0x80u, 0x100u}));
        if (Chance(rng, 5)) fc |= 0x200000u;
        if (Chance(rng, 6)) fc = (fc & ~0x600u) | Pick(rng, {0x200u, 0x400u});
        if (Chance(rng, 5)) fc = (fc & ~0xFu) | (rng.U32() & 0xFu);
        m.PokeWord(b + 568, fc);
        uint32_t fb = m.PeekWord(b + 564);
        if (Chance(rng, 5)) fb |= 0x400000u;
        if (Chance(rng, 8)) fb |= 0x800000u;
        m.PokeWord(b + 564, fb);
        if (Chance(rng, 4)) m.PokeWord(b + 652, Pick(rng, {0xB2B9u, 0xB2B8u, rng.U32()}));
        if (Chance(rng, 4)) m.PokeWord(b + 616, rng.U32() >> 12);
        if (Chance(rng, 4)) m.PokeWord(b + 676, rng.U32() >> 16);
        const uint32_t r = m.PeekWord(b + 852);
        if (r >= 0x80000000u && r < 0x80200000u) {
            // stance 0 or its own (NAMED BOUND: a stance of another animation bank makes the ORIGINAL's
            // stance layer trap on a clip index past the loaded table)
            if (Chance(rng, 4)) WriteU16Bench(m, r + 544, 0);
        }
    }
    // the player: airborne or sunk now and then (AirContact on the tail)
    if (Chance(rng, 2)) {
        int16_t up[3];
        GetVec3S16(m, P + 522, up);
        const int32_t lift = Chance(rng, 2) ? -static_cast<int32_t>(rng.U32() % 0x40000u) : static_cast<int32_t>(rng.U32() % 0x20000u);
        MoveBox(m, P, 0, lift, 0);
        m.PokeWord(P + 568, (m.PeekWord(P + 568) & ~0x600u) | Pick(rng, {0x400u, 0x400u, 0x200u, 0xC00u}));
        m.PokeWord(P + 708, rng.U32() % 0x10000u);
        m.PokeWord(P + 860, rng.U32() % 0x400000u);
        (void)up;
    }
    // the rider on the road, a pedestrian, the car
    const uint32_t riders = m.PeekWord(0x8005B3A4u);
    const uint32_t r6 = riders + 628u * 6u;
    if (Chance(rng, 2)) MoveNear(m, r6, P, off(5), 0, off(5));
    if (Chance(rng, 5)) m.PokeWord(r6 + 604, Pick(rng, {1u, 2u, 3u, 4u}));
    const uint32_t peds = m.PeekWord(rr::sim::kPoolTableAddr + 32u);
    if (Chance(rng, 2)) MoveNear(m, peds + 572u * (rng.U32() % 3u), P, off(5), 0, off(5));
    if (Chance(rng, 2)) MoveNear(m, 0x800CF660u, P, off(7), 0, off(7));
    // static volumes (pool 6), built in the spawner's layout
    // (docs\formats\population.md 4.1): handle 0xC0 | slot, class 1 (a pole: one point) or 0 (a
    // box: four), the centre and eight corners around it, the half extents, +0x94 live, an identity
    // basis. DATA of our own making on this machine's (empty) pool-6 slots - said plainly.
    if (Chance(rng, 3)) {
        const uint32_t rec6 = rr::sim::kPoolTableAddr + 16u * 6u;
        const uint32_t base6 = m.PeekWord(rec6);
        const uint32_t n6 = 1u + rng.U32() % 3u;
        for (uint32_t k = 0; k < n6; ++k) {
            const uint32_t r = base6 + 280u * k;
            for (uint32_t i = 0; i < 280; i += 4) m.PokeWord(r + i, 0);
            WriteU16Bench(m, r, static_cast<uint16_t>(0xC0u | k));
            const uint32_t cls = Chance(rng, 2) ? 1u : 0u;
            m.PokeWord(r + 8, cls);
            const int32_t hx = static_cast<int32_t>(rng.U32() % 0x30000u) + 0x3000, hz = static_cast<int32_t>(rng.U32() % 0x30000u) + 0x3000;
            const int32_t hy = static_cast<int32_t>(rng.U32() % 0x60000u) + 0x8000;
            int32_t cc[3] = {GetS32(m, P + 184) + off(6), GetS32(m, P + 188) + (Chance(rng, 2) ? 0 : off(1)), GetS32(m, P + 192) + off(6)};
            for (uint32_t i = 0; i < 3; ++i) PutS32(m, r + 12 + 4 * i, cc[i]);
            const int32_t sx[4] = {-1, 1, 1, -1}, sz[4] = {-1, -1, 1, 1};
            for (uint32_t q = 0; q < 4; ++q) {
                PutS32(m, r + 24 + 12 * q, cc[0] + sx[q] * hx);
                PutS32(m, r + 28 + 12 * q, cc[1]);
                PutS32(m, r + 32 + 12 * q, cc[2] + sz[q] * hz);
                PutS32(m, r + 24 + 12 * (q + 4), cc[0] + sx[q] * hx);
                PutS32(m, r + 28 + 12 * (q + 4), cc[1] - 0x8000 - hy);
                PutS32(m, r + 32 + 12 * (q + 4), cc[2] + sz[q] * hz);
            }
            PutS32(m, r + 0x84, hx);
            PutS32(m, r + 0x88, hz);
            PutS32(m, r + 0x8C, hy);
            WriteU16Bench(m, r + 0x94, 1);
            for (uint32_t i = 0; i < 3; ++i) WriteU16Bench(m, r + 0x104 + 8 * i, 4096);   // rows (1,0,0) (0,1,0) (0,0,1)
        }
        m.PokeWord(m.PeekWord(rec6 + 8), n6);
        m.PokeWord(m.PeekWord(rec6 + 12), n6 - 1u);
    }
    // a passenger on the player: bike 17, unlisted, whose +0x358 names its owner
    if (Chance(rng, 8)) {
        const uint32_t b17 = Pool0(m, 17);
        m.PokeWord(P + 856, b17);
        m.PokeWord(b17 + 856, P);
        m.PokeWord(b17 + 1088, 0);
        MoveNear(m, b17, P, off(1), 0, off(1));
    }
    // a rider getting up (+0x228 bits 30/31)
    if (Chance(rng, 3)) m.PokeWord(r6 + 552, (m.PeekWord(r6 + 552) & 0x3FFFFFFFu) | (rng.U32() & 0xC0000000u));
    // two players
    if (Chance(rng, 6)) {
        SetNumPlayers(m, 2);
        m.PokeWord(0x8005B21Cu, Pool0(m, 6));
        // far from player 1 half the time, with the car beside it: the second grid band
        if (Chance(rng, 2)) {
            MoveBox(m, Pool0(m, 6), 0x2000000 + off(50), 0, off(50));
            MoveNear(m, 0x800CF660u, Pool0(m, 6), off(6), 0, off(6));
        }
    }
    // what "the previous pass" left
    m.PokeWord(rr::sim::kChainCur, rng.U32() & 0x55555555u);
    m.PokeWord(rr::sim::kChainCur + 4, rng.U32() & 0x55555555u);
    m.PokeWord(rr::sim::kContactCount, rng.U32() % 9u);
    for (uint32_t i = 0; i < 16; ++i) m.PokeWord(rr::sim::kCollGrid + 72u * i, rng.U32());
    m.PokeWord(c.gp + kRandSeedGpOffset, rng.U32());
    a.a[0] = Pick(rng, {0x884u, 0x884u, 0x884u, 0x442u, 0u, rng.U32() % 0x2000u});
}
uint32_t NativePass(NativeEnv& env, const Args& a) {
    EnRun run(env);
    CloneCallees cc(env, run);
    const cr::Tables tab(env.clone->mem, env.gp);
    const bool ok = rr::sim::CollisionPass(run.g, static_cast<int32_t>(a.a[0]), env.sp, tab.t, cc);
    return Refuse(env, run, ok, "CollisionPass");
}


// ---------------------------------------------------------------- the rider pass's thrown walk
//
// A SPLICE on the rider / engine pass RASHCDG 0x8007B840(dt): the guest run is the
// original pass start to finish; the clone runs the original pass with a hook at the region's entry
// 0x8007C23C, where the port runs on the clone's RAM and `pc` goes to the exit 0x8007C9A8 (no register
// is live there - a scan of every path from the exit to the pass's `jr ra` reads none before writing
// it; dt is the pass's home slot, entry sp + 0 = its frame + 248); a third run - the original on a
// private copy, stopped at the exit - gives the state the original region leaves, compared byte for
// byte outside the stack window at that instant. `--splice-control` skips the port.
//
// The family: this machine with its five bike lists relinked from scratch - 1..4 pool-0 slots (the
// player among them half the time) on the THROWN list 0x8005B2D8, the player on the riding list when
// he is not thrown, every other slot where it was - and per thrown bike the fields the walk reads:
// flagsC 0x400 with the class bits 0..3 and bit 14 drawn (0x0800_1800 clear, so the pass's own list
// migration does not move it before the walk), the velocity +0x1C8 (-40 .. 40 per axis, or the
// captured bike's for a dump-derived case), g +0x1E4 (the crash launch's 0x9D087 or anything), the
// three tumble rates and angles (+0x268..+0x270, +0x27C/+0x280/+0x2A0, +0x28C, +0x2B4, +0x2D4 / +0x2D8
// / +0x2E8), the drop +0x300 at and around 3.0, flagsA bit 27 (AI or human), flagsB bit 20 (no drag),
// the rider's +0x228 bit 13; the air drag 0x800D3960; dt.
// NAMED BOUNDS: no passenger (+0x358 = 0: no capture has one, and a planted one sends the pass's own
// passenger code into states it never builds); the rows +0x1BC and +0x20A are the bike's own or unit
// directions (RowsFromUp normalises their cross products - a longer vector overflows Normalize).
struct WalkCallees final : rr::sim::ThrownWalkCallees {
    NativeEnv& env;
    explicit WalkCallees(NativeEnv& e) : env(e) {}
    bool StanceEvent(int32_t ev, uint32_t rider, int32_t p, uint32_t sp) override {
        AnimNativeStanceEvent(env, static_cast<uint32_t>(ev), rider, static_cast<uint32_t>(p), sp);
        return env.failure.empty();
    }
    bool PassengerLaunch(uint32_t e, uint32_t sp) override {
        const bs::StepTables tab(env.clone->mem, env.gp);
        bs::MigrateCallees mc(env, tab.t);
        rr::sim::GuestRam g = bs::StepView(env);
        return rr::sim::BikePassengerLaunch(g, e, sp, mc) && env.failure.empty();
    }
    bool CrashTimer(uint32_t e, int32_t dt, uint32_t sp) override {
        Memory& m = env.clone->mem;
        std::vector<uint8_t> table(4096);
        m.ReadBlock(kCrashTable, table.data(), table.size());
        struct Emitter final : rr::sim::CrashEmitter {
            NativeEnv& env;
            uint32_t e, sp;
            Emitter(NativeEnv& v, uint32_t a, uint32_t s) : env(v), e(a), sp(s) {}
            void Emit(int32_t which, int32_t kind) override {
                const uint32_t args[3] = {e, static_cast<uint32_t>(which), static_cast<uint32_t>(kind)};
                bs::ExactOracle(env, kCrashEmit, args, 3, sp - 24u);   // from the timer's 24-byte frame
            }
        } emitter(env, e, sp);
        rr::sim::EntityView view(m.ram().data() + (e & 0x1FFFFFu));
        rr::sim::BikeCrashTimer(view, dt, static_cast<int32_t>(m.PeekWord(kCrashWindow)), table.data(), emitter);
        return env.failure.empty();
    }
};

void SetupWalk(CaseContext& c, Args& a) {
    Rng& rng = *c.rng;
    Memory& m = *c.mem;
    const uint32_t pool = m.PeekWord(kPoolTable);
    const uint32_t slots = 18;
    // which list each slot is on now
    std::vector<int> on(slots, 0);
    for (int h = 0; h < 5; ++h) {
        uint32_t n = m.PeekWord(bs::kListHeads[h] + 4u);
        for (int guard = 0; n != bs::kListHeads[h] && guard < 64; ++guard) {
            const uint32_t e = n - 1088u;
            if (e >= pool && (e - pool) % 1096u == 0u && (e - pool) / 1096u < slots) on[(e - pool) / 1096u] = h;
            n = m.PeekWord(n + 4u);
        }
    }
    std::vector<uint32_t> thrown;
    if (Chance(rng, 2)) thrown.push_back(0u);
    const uint32_t more = (thrown.empty() ? 1u : 0u) + rng.U32() % 3u;
    for (uint32_t k = 0; k < more; ++k) {
        const uint32_t s = 1u + rng.U32() % (slots - 1u);
        if (std::find(thrown.begin(), thrown.end(), s) == thrown.end()) thrown.push_back(s);
    }
    for (size_t k = thrown.size(); k > 1; --k) std::swap(thrown[k - 1], thrown[rng.U32() % k]);
    on[0] = 1;
    for (uint32_t s : thrown) on[s] = 2;
    for (int h = 0; h < 5; ++h) {
        std::vector<uint32_t> ents;
        if (h == 2) {
            for (uint32_t s : thrown) ents.push_back(pool + 1096u * s);
        } else {
            for (uint32_t s = 0; s < slots; ++s)
                if (on[s] == h) ents.push_back(pool + 1096u * s);
        }
        bs::LinkList(m, bs::kListHeads[h], ents.data(), static_cast<uint32_t>(ents.size()));
    }
    for (uint32_t s : thrown) {
        const uint32_t e = pool + 1096u * s;
        uint32_t fc = (m.PeekWord(e + 568) & ~0x08001E1Fu) | 0x400u;
        if (Chance(rng, 3)) fc |= rng.U32() & 0xFu;
        if (Chance(rng, 4)) fc |= 0x4000u;
        if (Chance(rng, 4)) fc |= 0x200u;
        m.PokeWord(e + 568, fc);
        uint32_t fb = m.PeekWord(e + 564) & ~0x100000u;
        if (Chance(rng, 6)) fb |= 0x100000u;
        m.PokeWord(e + 564, fb);
        uint32_t fa = m.PeekWord(e + 560);
        fa = Chance(rng, 2) ? (fa | 0x08000000u) : (fa & ~0x08000000u);
        m.PokeWord(e + 560, fa);
        for (uint32_t k = 0; k < 3; ++k) {
            int32_t v = static_cast<int32_t>(rng.U32() % 0x500000u) - 0x280000;
            if (c.sample != nullptr && c.sample->entity.size() == 1096 && Chance(rng, 2))
                v = static_cast<int32_t>(GetU32V(c.sample->entity, 456 + 4 * k));
            PutS32(m, e + 456 + 4 * k, v);
        }
        if (Chance(rng, 8)) for (uint32_t k = 0; k < 3; ++k) PutS32(m, e + 456 + 4 * k, 0);
        m.PokeWord(e + 484, Pick(rng, {0x9D087u, 0x9D087u, 0x9D087u, rng.U32() % 0x200000u, rng.U32()}));
        m.PokeWord(e + 616, static_cast<uint32_t>(rng.S32() >> (12 + rng.Next() % 8u)));
        m.PokeWord(e + 620, static_cast<uint32_t>(rng.S32() >> (10 + rng.Next() % 10u)));
        m.PokeWord(e + 624, Chance(rng, 3) ? 0u : static_cast<uint32_t>(rng.S32() >> (8 + rng.Next() % 12u)));
        m.PokeWord(e + 636, static_cast<uint32_t>(rng.S32() >> (12 + rng.Next() % 8u)));
        m.PokeWord(e + 640, Chance(rng, 3) ? 0u : static_cast<uint32_t>(rng.S32() >> (8 + rng.Next() % 12u)));
        m.PokeWord(e + 652, static_cast<uint32_t>(rng.S32() >> (12 + rng.Next() % 6u)));
        m.PokeWord(e + 692, rng.U32());
        const uint32_t el = 2180u * (rng.U32() % 8u);
        m.PokeWord(e + 724, el);
        m.PokeWord(e + 728, Pick(rng, {el, el + 2180u, el + 1000u, 9148u, rng.U32() % 30000u}));
        m.PokeWord(e + 744, static_cast<uint32_t>(rng.S32() >> (8 + rng.Next() % 10u)));
        m.PokeWord(e + 768, Pick(rng, {0x30000u, 0x30001u, 0u, 0x50000u, rng.U32() % 0x80000u}));
        m.PokeWord(e + 856, 0);
        const uint32_t r = m.PeekWord(e + 852);
        if (r >= 0x80000000u && r < 0x80200000u && Chance(rng, 2)) m.PokeWord(r + 552, m.PeekWord(r + 552) & ~0x2000u);
    }
    m.PokeWord(rr::sim::kAirDrag, Pick(rng, {m.PeekWord(rr::sim::kAirDrag), m.PeekWord(rr::sim::kAirDrag), 0u, rng.U32() % 0x10000u}));
    a.a[0] = static_cast<uint32_t>(bs::DrawDt(rng));
}

uint32_t NativeWalk(NativeEnv& env, const Args& a) {
    Machine& cl = *env.clone;
    const uint32_t sp = env.sp;
    bs::SpliceStats::Entry& st = bs::Stats().For("rider_pass_thrown");
    ++st.cases;
    auto fail = [&env](const std::string& why) {
        if (env.failure.empty()) env.failure = why;
    };
    std::vector<uint8_t> refRam, refSpad;
    bool refReached = false;
    {
        Machine ref;
        bs::CopyMachine(cl, ref);
        ref.cpu.AddHook(rr::sim::kThrownWalkExit, "exit");
        bs::StartCall(ref.cpu, rr::sim::kRiderPassFn, a, sp);
        const Trap t = ref.cpu.Run(kSentinel, env.maxSteps);
        if (t.kind == TrapKind::Hook && t.pc == rr::sim::kThrownWalkExit) {
            refReached = true;
            refRam = ref.mem.ram();
            refSpad = ref.mem.scratchpad();
        } else if (t.kind != TrapKind::Halted) {
            fail("the reference run of the original trapped: " + t.ToString());
            return 0;
        }
    }
    cl.cpu.ClearHooks();
    cl.cpu.AddHook(rr::sim::kThrownWalkEntry, "entry");
    bs::StartCall(cl.cpu, rr::sim::kRiderPassFn, a, sp);
    int entries = 0;
    for (;;) {
        cl.cpu.callObservations.clear();
        const Trap t = cl.cpu.Run(kSentinel, env.maxSteps);
        env.calls.insert(env.calls.end(), cl.cpu.callObservations.begin(), cl.cpu.callObservations.end());
        cl.cpu.callObservations.clear();
        if (t.kind == TrapKind::Halted) break;
        if (t.kind != TrapKind::Hook || t.pc != rr::sim::kThrownWalkEntry) {
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
            const bs::StepTables tab(cl.mem, env.gp);
            WalkCallees wc(env);
            if (!rr::sim::RiderPassThrownWalk(g, dt, frame, tab.t, wc)) {
                char buf[128];
                std::snprintf(buf, sizeof(buf), "the port refused the region (view fault 0x%08X)", g.FaultAddress());
                fail(buf);
                break;
            }
        }
        RestoreRegs(saved, cl.cpu);
        cl.cpu.pc = rr::sim::kThrownWalkExit;
        cl.cpu.npc = rr::sim::kThrownWalkExit + 4u;
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
                          rr::sim::kThrownWalkExit, differ, first);
            fail(buf);
            break;
        }
    }
    cl.cpu.ClearHooks();
    if (entries == 0 && refReached) fail("the reference passed the region but the clone never entered it");
    return cl.cpu.regs[2];
}


// ---------------------------------------------------------------- RowsFromUp, 0x8007EA64
//
// A bike at the scratch with its rows +0x1B0 (three of them), +0x1BC, +0x20A drawn: captured rows, unit
// directions, and the degenerate shapes that reach the other arms - +0x1BC parallel to the up row
// (the first cross product is zero), and +0x1B0 parallel to it as well (the identity reset
// 0x8008CF74). NAMED BOUND: every row a direction of length <= 1.0 (the original's Normalize of a
// longer cross product can overflow its trapping add).
void SetupRowsUp(CaseContext& c, Args& a) {
    Rng& rng = *c.rng;
    Memory& m = *c.mem;
    std::vector<uint8_t> e = BikeImage(c);
    int16_t up[3], lean[3], r0[3];
    if (c.sample == nullptr || Chance(rng, 3)) {
        Dir(rng, up);
        Dir(rng, lean);
        Dir(rng, r0);
        for (uint32_t k = 0; k < 3; ++k) {
            PutU16V(e, 522 + 2 * k, static_cast<uint16_t>(up[k]));
            PutU16V(e, 444 + 2 * k, static_cast<uint16_t>(lean[k]));
            PutU16V(e, 432 + 2 * k, static_cast<uint16_t>(r0[k]));
        }
    }
    const uint32_t mode = static_cast<uint32_t>(rng.Next() % 5u);
    for (uint32_t k = 0; k < 3; ++k) {
        const uint16_t u = static_cast<uint16_t>(GetU32V(e, 522 + 2 * k));
        if (mode >= 2) PutU16V(e, 444 + 2 * k, mode == 4 ? static_cast<uint16_t>(0u - u) : u);
        if (mode >= 3) PutU16V(e, 432 + 2 * k, u);
    }
    m.WriteBlock(c.scratch, e.data(), e.size());
    a.a[0] = c.scratch;
}
uint32_t NativeRowsUp(Memory& m, uint32_t, uint32_t gp, const Args& a) {
    rr::sim::GuestRam g(m.ram().data(), gp);
    const cr::Tables tab(m, gp);
    return rr::sim::RowsFromUp(g, a.a[0], tab.t) ? 0u : 0xDEADDEADu;
}


// ---------------------------------------------------------------- the deferred reactions
//
// Both rows run on THIS machine's bikes (their reactions run from the original code and follow the
// bikes' own pointers). ChainReaction: e one of the 18 bikes (the two live ones mostly), `vec` a push
// in the scratch, `skip` 31 or a handle; the previous pass's chain words 0x800CCF70 drawn (2 bits per
// handle), the bike count 0x8005B1F8 18, 0 or anything up to 20. DeferredContacts: the contact list
// filled with 0..8 records of the shapes the writers of 17.5 / 20.6 leave - a bike (pool 0 slot 0..17)
// against another bike, the car (pool 3 slot 0), a wall (pool 8, the segment id and pointer of one of
// the player's road's walls, its flags and value), or a pool-6 volume (the synthetic records of
// SetupPass); unit normals, pushes and depths of the game's size, the packed codes.
// NAMED BOUND: no record of a partner pool other than 0, 3, 6 and 8: the handler then reads a shape at
// *(0x800CE5A4) + 452 x slot + 0xAC, which 19.4 could not tie to any pool (the pool table gives pool 5 a
// stride of -452), and on this machine that is stale memory the unported BoxReact would walk.
// The synthetic pool-6 volumes of SetupPass, n6 of them around the player's box.
void PlantVolumes(CaseContext& c, uint32_t P, uint32_t n6) {
    Rng& rng = *c.rng;
    Memory& m = *c.mem;
    auto off = [&rng](int32_t units) { return static_cast<int32_t>(rng.U32() % (2u * 65536u * static_cast<uint32_t>(units))) - 65536 * units; };
    const uint32_t rec6 = rr::sim::kPoolTableAddr + 16u * 6u;
    const uint32_t base6 = m.PeekWord(rec6);
    for (uint32_t k = 0; k < n6; ++k) {
        const uint32_t r = base6 + 280u * k;
        for (uint32_t i = 0; i < 280; i += 4) m.PokeWord(r + i, 0);
        WriteU16Bench(m, r, static_cast<uint16_t>(0xC0u | k));
        m.PokeWord(r + 8, Chance(rng, 2) ? 1u : 0u);
        const int32_t hx = static_cast<int32_t>(rng.U32() % 0x30000u) + 0x3000, hz = static_cast<int32_t>(rng.U32() % 0x30000u) + 0x3000;
        const int32_t hy = static_cast<int32_t>(rng.U32() % 0x60000u) + 0x8000;
        int32_t cc[3] = {GetS32(m, P + 184) + off(6), GetS32(m, P + 188) + (Chance(rng, 2) ? 0 : off(1)), GetS32(m, P + 192) + off(6)};
        for (uint32_t i = 0; i < 3; ++i) PutS32(m, r + 12 + 4 * i, cc[i]);
        const int32_t sx[4] = {-1, 1, 1, -1}, sz[4] = {-1, -1, 1, 1};
        for (uint32_t q = 0; q < 4; ++q) {
            PutS32(m, r + 24 + 12 * q, cc[0] + sx[q] * hx);
            PutS32(m, r + 28 + 12 * q, cc[1]);
            PutS32(m, r + 32 + 12 * q, cc[2] + sz[q] * hz);
            PutS32(m, r + 24 + 12 * (q + 4), cc[0] + sx[q] * hx);
            PutS32(m, r + 28 + 12 * (q + 4), cc[1] - 0x8000 - hy);
            PutS32(m, r + 32 + 12 * (q + 4), cc[2] + sz[q] * hz);
        }
        PutS32(m, r + 0x84, hx);
        PutS32(m, r + 0x88, hz);
        PutS32(m, r + 0x8C, hy);
        WriteU16Bench(m, r + 0x94, 1);
        for (uint32_t i = 0; i < 3; ++i) WriteU16Bench(m, r + 0x104 + 8 * i, 4096);
    }
    m.PokeWord(m.PeekWord(rec6 + 8), n6);
    m.PokeWord(m.PeekWord(rec6 + 12), n6 - 1u);
}
void SetupChain(CaseContext& c, Args& a) {
    Rng& rng = *c.rng;
    Memory& m = *c.mem;
    const uint32_t slot = Pick(rng, {0u, 0u, 6u, 6u, rng.U32() % 18u});
    const uint32_t e = Pool0(m, slot);
    for (uint32_t k = 0; k < 3; ++k) m.PokeWord(c.scratch + 4 * k, static_cast<uint32_t>(rng.S32() >> (12 + rng.Next() % 6u)));
    m.PokeWord(rr::sim::kChainPrev, rng.U32() & (Chance(rng, 2) ? 0x55555555u : 0xFFFFFFFFu));
    m.PokeWord(rr::sim::kChainPrev + 4, rng.U32() & 0x0Fu);
    m.PokeWord(0x8005B1F8u, Pick(rng, {18u, 18u, 18u, 0u, 1u, 7u, rng.U32() % 21u}));
    // the partners near e, so that BikeVsBike finds contacts now and then
    for (uint32_t k = 0; k < 3; ++k) {
        const uint32_t o = Pool0(m, rng.U32() % 18u);
        if (o != e && Chance(rng, 2)) MoveNear(m, o, e, static_cast<int32_t>(rng.U32() % 0x40000u) - 0x20000, 0,
                                               static_cast<int32_t>(rng.U32() % 0x40000u) - 0x20000);
    }
    a.a[0] = e;
    a.a[1] = c.scratch;
    a.a[2] = Pick(rng, {31u, 31u, 0u, 6u, rng.U32()});
}
uint32_t NativeChain(NativeEnv& env, const Args& a) {
    EnRun run(env);
    CloneCallees cc(env, run);
    const rr::sim::GuestRegs caller = BenchCaller(env);
    const bool ok = rr::sim::ChainReaction(run.g, a.a[0], a.a[1], a.a[2], env.sp, cc, &caller);
    return Refuse(env, run, ok, "ChainReaction");
}

void SetupDeferred(CaseContext& c, Args& a) {
    (void)a;
    Rng& rng = *c.rng;
    Memory& m = *c.mem;
    const uint32_t P = Pool0(m, 0);
    const uint32_t n6 = 1u + rng.U32() % 3u;
    PlantVolumes(c, P, n6);
    const uint32_t S = m.PeekWord(P + 0x148), segs = m.PeekWord(S + 0x60);
    const uint32_t cnt = static_cast<uint32_t>(rng.Next() % 9u);
    m.PokeWord(rr::sim::kContactCount, cnt);
    for (uint32_t i = 0; i < 8; ++i) {
        const uint32_t r = rr::sim::kContactList + 36u * i;
        const uint32_t kind = static_cast<uint32_t>(rng.Next() % 4u);
        const uint32_t hA = Pick(rng, {0u, 0u, 6u, rng.U32() % 18u});
        uint32_t hB = 0;
        const uint32_t seg = segs + 40u * (rng.U32() % 200u);
        if (kind == 0) hB = rng.U32() % 18u;
        else if (kind == 1) hB = 0x60u;
        else if (kind == 2) hB = 0x100u | (m.PeekByte(seg) | (m.PeekByte(seg + 1) << 8));
        else hB = 0xC0u | (rng.U32() % n6);
        if (kind == 2 && (hB >> 5) != 8) hB = 0x100u;
        WriteU16Bench(m, r, static_cast<uint16_t>(hA));
        WriteU16Bench(m, r + 2, static_cast<uint16_t>(hB));
        int16_t n[3];
        Dir(rng, n);
        PutVec3S16(m, r + 4, n);
        WriteU16Bench(m, r + 10, static_cast<uint16_t>(kind == 2 ? (m.PeekByte(seg + 2) | (m.PeekByte(seg + 3) << 8) | (rng.U32() & 0x10u))
                                                                : (rng.U32() & 0x0FFFu)));
        m.PokeWord(r + 12, kind == 2 ? seg : rng.U32());
        for (uint32_t k = 0; k < 3; ++k) m.PokeWord(r + 16 + 4 * k, static_cast<uint32_t>(rng.S32() >> (13 + rng.Next() % 6u)));
        m.PokeWord(r + 28, kind == 2 ? m.PeekWord(seg + 4) : (Pick(rng, {1u, 2u, 4u, 8u, 13u, 31u}) | (Pick(rng, {1u, 2u, 4u, 8u, 13u, 31u}) << 16)));
        m.PokeWord(r + 32, rng.U32() % 0x30000u);
    }
    m.PokeWord(rr::sim::kChainPrev, rng.U32() & (Chance(rng, 2) ? 0x55555555u : 0xFFFFFFFFu));
    m.PokeWord(rr::sim::kChainPrev + 4, rng.U32() & 0x0Fu);
    // the bikes near the player, as they are when the pass logs contacts
    for (uint32_t k = 0; k < 3; ++k) {
        const uint32_t o = Pool0(m, 1u + rng.U32() % 17u);
        if (Chance(rng, 2)) MoveNear(m, o, P, static_cast<int32_t>(rng.U32() % 0x60000u) - 0x30000, 0,
                                     static_cast<int32_t>(rng.U32() % 0x60000u) - 0x30000);
    }
    if (Chance(rng, 2)) MoveNear(m, 0x800CF660u, P, static_cast<int32_t>(rng.U32() % 0x60000u) - 0x30000, 0,
                                 static_cast<int32_t>(rng.U32() % 0x60000u) - 0x30000);
    m.PokeWord(c.gp + kRandSeedGpOffset, rng.U32());
}
uint32_t NativeDeferred(NativeEnv& env, const Args&) {
    EnRun run(env);
    CloneCallees cc(env, run);
    const rr::sim::GuestRegs caller = BenchCaller(env);
    const bool ok = rr::sim::DeferredContacts(run.g, env.sp, cc, &caller);
    return Refuse(env, run, ok, "DeferredContacts");
}

} // namespace co

void AddCollisionRows(std::vector<Row>& rows) {
    rows.push_back(Row{"apply_impulse", "void 0x800A8DF0(E *e, const s32 v[3], s32 flag)  [RASHCDG]", 0x800A8DF0,
                       co::SetupImpulse, co::NativeImpulse, /*compareV0=*/false});
    rows.push_back(Row{"stale_heading", "s32 0x800A8FE8(E *e)  [RASHCDG]", 0x800A8FE8, co::SetupStale, co::NativeStale});
    rows.push_back(Row{"in_camera_box", "s32 0x800A8C78(E *e, s32 halfWidth, s32 depth)  [RASHCDG]", 0x800A8C78,
                       co::SetupCamera, co::NativeCamera});
    rows.push_back(Row{"corner_min", "u32 0x800B6F40(c[8][3], n, q, *depth, [*count])  [RASHCDG]", 0x800B6F40,
                       co::SetupCornerMin, co::NativeCornerMin});
    rows.push_back(Row{"ray_plane", "s32 0x800B6BD0(p, dir, n, q)  [RASHCDG]", 0x800B6BD0, co::SetupRayPlane,
                       co::NativeRayPlane});
    rows.push_back(Row{"contact_merge", "s32 0x800B59F0(ContactRec *rec)  [RASHCDG]", 0x800B59F0, co::SetupMerge,
                       co::NativeMerge});
    rows.push_back(Row{"view_track", "void 0x800A451C(View *v, s32 flag)  [RASHCDG]", 0x800A451C,
                       co::SetupViewTrack, co::NativeViewTrack, /*compareV0=*/false});
    {
        rows.push_back(Row{"pad_rumble", "void 0x800B658C(e, other, speed, k, [div])  [RASHCDG]", 0x800B658C,
                           co::SetupRumble, nullptr});
        Row& r = rows.back();
        r.compareV0 = false;
        r.nativeEx = co::NativeRumble;
        r.oracleCallees = {OracleCallee{rr::sim::coll::kPadMotor, 4}};
    }
    {
        rows.push_back(Row{"touch_down", "void 0x800B16F4(Bike *e, s32 *pSpeed)  [RASHCDG, the landing]",
                           0x800B16F4, co::SetupTouch, nullptr});
        Row& r = rows.back();
        r.compareV0 = false;
        r.nativeEx = co::NativeTouch;
        r.oracleCallees = {OracleCallee{rr::sim::coll::kPadMotor, 4}};
    }
    {
        rows.push_back(Row{"chain_reaction", "void 0x800A7AB4(Bike *e, s32 vec[3], u32 skip)  [RASHCDG]", 0x800A7AB4,
                           co::SetupChain, nullptr});
        Row& r = rows.back();
        r.compareV0 = false;
        r.nativeEx = co::NativeChain;
        r.oracleCallees = {OracleCallee{rr::sim::coll::kBikeVsBike, 3}};
    }
    {
        using namespace rr::sim::coll;
        rows.push_back(Row{"deferred_contacts", "void 0x800A77B0()  [RASHCDG, the deferred reactions]", 0x800A77B0,
                           co::SetupDeferred, nullptr});
        Row& r = rows.back();
        r.compareV0 = false;
        r.nativeEx = co::NativeDeferred;
        r.maxSteps = 20000000;
        r.oracleCallees = {OracleCallee{kBikeWallHit, 4, 2}, OracleCallee{kBikeTrafficReact, 4, 1},
                           OracleCallee{kPoleReact, 4, 2},   OracleCallee{kBoxReact, 4, 2},
                           OracleCallee{kBikeBikeReact, 4},  OracleCallee{kBikeVsBike, 3},
                           OracleCallee{kPadMotor, 4}};
    }
    rows.push_back(Row{"rows_from_up", "void 0x8007EA64(Bike *e)  [RASHCDG, the rows from the up row]", 0x8007EA64,
                       co::SetupRowsUp, co::NativeRowsUp, /*compareV0=*/false});
    {
        rows.push_back(Row{"rider_pass_thrown", "region [0x8007C23C, 0x8007C9A8) of 0x8007B840(s32 dt)  [RASHCDG, a SPLICE: the airborne walk]",
                           0x8007B840, co::SetupWalk, nullptr});
        Row& r = rows.back();
        r.compareV0 = false;
        r.nativeEx = co::NativeWalk;
        r.maxSteps = 20000000;
        // the stance event runs NATIVELY (stance.h) with its own seams at the depths the walk's frame
        // (248) puts them: rows_spine.inc's rule for a stance event reached from a caller's frame
        const uint32_t d = rr::sim::kRiderPassFrame;
        using namespace animrows;
        r.oracleCallees = {OracleCallee{rr::sim::kStanceEventFn, 3}, OracleCallee{kCrashEmit, 3},
                           OracleCallee{bs::kRiderKnockOff, 1}, OracleCallee{bs::kRiderLaunch, 1},
                           Capture(d + kFrEvent + kFrGate + kFrTransition + kFrRestart + kFrPoseStart),
                           Leave(d + kFrEvent + kFrLeave), Enter(d + kFrEvent + kFrEnter)};
    }
    {
        using namespace rr::sim::coll;
        rows.push_back(Row{"collision_pass", "void 0x800A4774(s32 dt)  [RASHCDG, the collision pass]", 0x800A4774,
                           co::SetupPass, nullptr});
        Row& r = rows.back();
        r.compareV0 = false;
        r.nativeEx = co::NativePass;
        r.maxSteps = 40'000'000;
        r.oracleCallees = {OracleCallee{kBikeVsBike, 3},      OracleCallee{kPointResolve, 2},
                           OracleCallee{kBoxResolve, 2},      OracleCallee{kPoleResolve, 2},
                           OracleCallee{kBikeVsRider, 2},     OracleCallee{kBikeVsTraffic, 2},
                           OracleCallee{kRiderNone, 2},       OracleCallee{kRiderVsTraffic, 2},
                           OracleCallee{kRiderVsShape, 2},    OracleCallee{kTrafficVsTraffic, 2},
                           OracleCallee{kTrafficVsProp, 2},   OracleCallee{kPropVsShape, 2},
                           OracleCallee{kBikeTrafficReact, 4, 1}, OracleCallee{kPoleReact, 4, 2},
                           OracleCallee{kBoxReact, 4, 2},     OracleCallee{kBikeBikeReact, 4},
                           OracleCallee{kImpactTurn, 4},
                           OracleCallee{kRiderGetUp, 1},      OracleCallee{kPropTopple, 4},
                           OracleCallee{kBikeWallHit, 4, 2},  OracleCallee{kRiderWallHit, 3},
                           OracleCallee{kPedHit, 4, 2},       OracleCallee{kPadMotor, 4}};
    }
    {
        rows.push_back(Row{"wall_contact", "u32 0x800B3AD0(E *e, s32 force)  [RASHCDG, the road walls]", 0x800B3AD0,
                           co::SetupWall, nullptr});
        Row& r = rows.back();
        r.nativeEx = co::NativeWall;
        r.oracleCallees = {OracleCallee{rr::sim::coll::kBikeWallHit, 4, 2}, OracleCallee{rr::sim::coll::kPedHit, 4, 2},
                           OracleCallee{rr::sim::coll::kRiderWallHit, 3}, OracleCallee{rr::sim::coll::kPropTopple, 4}};
    }
    {
        rows.push_back(Row{"air_contact", "void 0x800B1978(Bike *e)  [RASHCDG, the ground contact of an airborne bike]",
                           0x800B1978, co::SetupAir, nullptr});
        Row& r = rows.back();
        r.compareV0 = false;
        r.nativeEx = co::NativeAir;
        r.oracleCallees = {OracleCallee{rr::sim::coll::kPadMotor, 4}};
    }
}

// The extent [entry, end) of each function these rows accept, for `--coverage` (the address after the
// delay slot of its last `jr ra`, from our own disassembly of RASHCDG).
const CoverageRange* CollisionCoverage(const char* rowName) {
    struct Named {
        const char* name;
        CoverageRange range;
    };
    static const Named kRanges[] = {
        {"apply_impulse", {0x800A8DF0, 0x800A8FE8}}, {"stale_heading", {0x800A8FE8, 0x800A91AC}},
        {"in_camera_box", {0x800A8C78, 0x800A8DF0}}, {"corner_min", {0x800B6F40, 0x800B7030}},
        {"ray_plane", {0x800B6BD0, 0x800B6D70}},     {"contact_merge", {0x800B59F0, 0x800B5B48}},
        {"view_track", {0x800A451C, 0x800A4774}},    {"pad_rumble", {0x800B658C, 0x800B675C}},
        {"touch_down", {0x800B16F4, 0x800B1978}},    {"air_contact", {0x800B1978, 0x800B208C}},
        {"wall_contact", {0x800B3AD0, 0x800B59F0}},  {"collision_pass", {0x800A4774, 0x800A77B0}},
        {"rider_pass_thrown", {0x8007C23C, 0x8007C9A8}}, {"rows_from_up", {0x8007EA64, 0x8007EC30}},
        {"chain_reaction", {0x800A7AB4, 0x800A7BF8}}, {"deferred_contacts", {0x800A77B0, 0x800A7AB4}},
    };
    for (const Named& n : kRanges)
        if (std::strcmp(n.name, rowName) == 0) return &n.range;
    return nullptr;
}
