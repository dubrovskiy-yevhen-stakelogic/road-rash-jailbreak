// Bench rows for src/game/sim/passes.{h,cpp}. Included by
// verify_physics.cpp inside its anonymous namespace, after every other rows file (co::, creact::, fallrows::,
// rcv::, bs:: helpers).
//
//   rider_pass_class   the region [0x8007C9DC, 0x8007DCD4) of 0x8007B840, a SPLICE (rider_pass_thrown's form):
//                      the guest runs the original pass; the clone runs it with the region replaced by the port;
//                      a third run stops the original at the region's exit and the clone's state there is
//                      compared byte for byte outside the stack window. ClipDone 0x8005BE58 (AnimMachine) and
//                      PlaySound3D 0x80017BA0 (the sound port) run natively, GetRCnt through the oracle.
//   rider_pass         0x8007B840(dt) WHOLE and native: its four regions are the ported region functions on
//                      the other splice rows' own callees, every function it calls (the engine, ImpactStatePass,
//                      0x8008F404, the heading writer, DormantDrive) the original through the oracle at the
//                      pass's own frame; the list migration natively (BikeListMigrate), as the pool loop's row.
//                      A case is one of the four regions' families (or the machine as it is).
//   world_bike_pass    0x8008AC80(dt): the seven children through the oracle, the pedestrian switch drawn.
//   rider_engine_pass  0x8008ACE8(dt): the four children through the oracle, the switch drawn.
//
// The class family: this machine's five bike lists relinked with 1..4 pool-0 slots on the CLASS list
// 0x8005B350 (the player half the time) and per such bike the fields the walk reads: flagsC's class bits
// 0x100 / 0x80 / 0x40 / 0x20 / 0x10 / 0..15 and the latches 0x2000, 0x8000, 0x10000, 0x20000, 0x200000
// (0x08001800, 0x800000 and 0x600 clear, so the pass's own pool loop leaves the bike on its list), the
// speed +0x240 around every threshold the walk tests (2.2352, 5.0, 17.0, 7.53), the timer +0x2D4 around 0,
// -3.0 and 1.0, the yaw +0x27C and the lean +0x28C around their limits, the lean-back pair +0x2A4 / +0x2B0
// (equal half the time), the rates +0x280..+0x298, +0x248, +0x1E8, the push +0x2F8 and the blend weights
// +0x2DC / +0x2E4, the velocity (-48 .. 48 per axis, or the captured bike's), the up row's y +0x20C
// around 3547, the heading +0x1C2 with its y below -0.5 now and then, flagsA bit 26, +0x184 bit 0, the
// rider's mount +0x25C 0..4 and +0x220 (0 or its own), a passenger word +0x358 (another pool-0 bike) a
// third of the time with the rider's +0x23C bit 4 then drawn. NAMED BOUNDS: the rows +0x1B0, +0x1C2,
// +0x204, +0x20A, +0x210 and the contact normal +0x334 are unit directions (the GTE OP of two of them is
// normalised, and a longer one overflows Normalize's trapping add on the console); the velocity stays
// under 64 per axis so that |v|^2 fits the 32-bit sum the original forms; the surface byte +0x216 is
// the bike's own (it indexes the table 0x800D38E0).

namespace passrows {

using co::Chance;
using co::Pick;

constexpr uint32_t kClipDoneFn = 0x8005BE58, kGetRCntFn = 0x80043F00;
constexpr uint32_t kEngineFn = 0x80079B20, kImpactStateFn = 0x80078DB4, kRiderGroundFn = 0x8008F404,
                   kHeadingFn = 0x8007AC04, kDormantDriveFn = 0x80095724;

// ClipDone has no seam of its own: the machine's pose seams are never reached from it.
struct NoPose final : rr::sim::AnimPoseSeam {
    uint32_t TransitionCapture(uint32_t, uint32_t, uint32_t) override { return 0; }
    uint32_t ApplyFrame(uint32_t) override { return 0; }
};

struct ClassCallees final : rr::sim::ClassWalkCallees {
    NativeEnv& env;
    EnRun& run;
    explicit ClassCallees(NativeEnv& e, EnRun& r) : env(e), run(r) {}
    bool ClipDone(uint32_t anim, uint32_t, uint32_t& v0) override {
        rr::sim::GuestRam g = bs::StepView(env);
        NoPose pose;
        rr::sim::AnimMachine m(g, pose);
        v0 = m.ClipDone(anim);
        return !m.Failed();
    }
    bool GetRCnt(uint32_t id, uint32_t sp, uint32_t& v0) override {
        v0 = bs::ExactOracle(env, kGetRCntFn, &id, 1, sp);
        return env.failure.empty();
    }
    bool PlaySound3D(int32_t x, int32_t z, int32_t id, int32_t bank, uint32_t) override {
        rr::sim::GuestPlaySound3D(run.s, x, z, id, bank);
        return !run.s.Faulted();
    }
};

// ---------------------------------------------------------------- the class family
void SetupClass(CaseContext& c, Args& a) {
    Rng& rng = *c.rng;
    Memory& m = *c.mem;
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
    std::vector<uint32_t> cls;
    if (Chance(rng, 2)) cls.push_back(0u);
    const uint32_t more = (cls.empty() ? 1u : 0u) + rng.U32() % 4u;
    for (uint32_t k = 0; k < more; ++k) {
        const uint32_t s = 1u + rng.U32() % (slots - 1u);
        if (std::find(cls.begin(), cls.end(), s) == cls.end()) cls.push_back(s);
    }
    for (size_t k = cls.size(); k > 1; --k) std::swap(cls[k - 1], cls[rng.U32() % k]);
    on[0] = 1;
    for (uint32_t s : cls) on[s] = 3;
    for (int h = 0; h < 5; ++h) {
        std::vector<uint32_t> ents;
        if (h == 3) {
            for (uint32_t s : cls) ents.push_back(pool + 1096u * s);
        } else {
            for (uint32_t s = 0; s < slots; ++s)
                if (on[s] == h) ents.push_back(pool + 1096u * s);
        }
        bs::LinkList(m, bs::kListHeads[h], ents.data(), static_cast<uint32_t>(ents.size()));
    }
    auto dir = [&](uint32_t at) {
        int16_t d[3];
        co::Dir(rng, d);
        for (uint32_t k = 0; k < 3; ++k) WriteU16Bench(m, at + 2u * k, static_cast<uint16_t>(d[k]));
    };
    auto small = [&](int lo, int hi) { return static_cast<uint32_t>(rng.S32() >> (lo + static_cast<int>(rng.Next() % static_cast<uint32_t>(hi - lo)))); };
    auto inRam = [](uint32_t x) { return x >= 0x80000000u && x < 0x801FF000u; };
    for (uint32_t s : cls) {
        const uint32_t e = pool + 1096u * s;
        const uint32_t r = m.PeekWord(e + 852u);
        uint32_t fc = m.PeekWord(e + 568u) & ~(0x08001800u | 0x00800000u | 0x00000600u | 0x000001FFu | 0x00002000u |
                                               0x00018000u | 0x00020000u | 0x00200000u);
        switch (rng.Next() % 7u) {
            case 0: fc |= 0x100u; break;
            case 1: fc |= 0x80u; break;
            case 2: fc |= 0x40u; break;
            case 3: fc |= 0x20u; break;
            case 4: fc |= 0x10u | (rng.U32() & 0xFu); break;
            case 5: fc |= rng.U32() & 0xFu; break;
            default: fc |= rng.U32() & 0x1FFu; break;
        }
        if (Chance(rng, 3)) fc |= 0x2000u;
        if (Chance(rng, 3)) fc |= Pick(rng, {0x8000u, 0x10000u, 0x18000u});
        if (Chance(rng, 4)) fc |= 0x20000u;
        if (Chance(rng, 5)) fc |= 0x200000u;
        if (Chance(rng, 12)) fc |= 0x200u;
        m.PokeWord(e + 568u, fc);
        m.PokeWord(e + 560u, Chance(rng, 5) ? (m.PeekWord(e + 560u) | 0x04000000u) : (m.PeekWord(e + 560u) & ~0x04000000u));
        m.PokeWord(e + 388u, Chance(rng, 2) ? (m.PeekWord(e + 388u) | 1u) : (m.PeekWord(e + 388u) & ~1u));
        m.PokeWord(e + 576u, Pick(rng, {0u, 0x10000u, 0x23C36u, 0x4FFFFu, 0x50000u, 0x787EFu, 0x787F0u, 0x90000u,
                                        0x10FFFFu, 0x110000u, 0x200000u, rng.U32() % 0x300000u}));
        m.PokeWord(e + 580u, rng.U32() % 0x60000u);
        for (uint32_t k = 0; k < 3; ++k) {
            int32_t v = static_cast<int32_t>(rng.U32() % 0x600000u) - 0x300000;
            if (c.sample != nullptr && c.sample->entity.size() == 1096 && Chance(rng, 2))
                v = static_cast<int32_t>(GetU32V(c.sample->entity, 456 + 4 * k));
            if (v > 0x3FFFFF || v < -0x3FFFFF) v >>= 4;
            PutS32(m, e + 456 + 4 * k, v);
        }
        m.PokeWord(e + 480u, Chance(rng, 3) ? m.PeekWord(e + 576u) : rng.U32() % 0x300000u);
        m.PokeWord(e + 724u, Pick(rng, {0u, 0xFFFCFFFFu, 0xFFFD0000u, 0xFFFE8000u, 0xFFFFFFFFu, 1u, 0x8000u, 0xFFFFu,
                                        0x10000u, 0x20000u, small(8, 16)}));
        m.PokeWord(e + 636u, Pick(rng, {0u, 0xCCCCu, 0xCCCDu, 0xFFFF3333u, 0x8609u, 0x860Au, 0xFFFF79F6u, 0xFFFF79F7u,
                                        0xFFFF79F5u, small(10, 18)}));
        m.PokeWord(e + 652u, Pick(rng, {0u, 0xB2B8u, 0xB2B9u, 0x1921Eu, 0x1921Fu, 0xFFFE6DE1u, 0x19000u, small(12, 18)}));
        for (uint32_t off : {656u, 660u, 640u, 584u, 488u, 760u})
            m.PokeWord(e + off, Chance(rng, 3) ? 0u : small(10, 20));
        m.PokeWord(e + 664u, small(12, 18));
        m.PokeWord(e + 668u, small(12, 18));
        m.PokeWord(e + 644u, small(12, 18));
        const uint32_t lean = Chance(rng, 4) ? 0u : small(12, 18);
        m.PokeWord(e + 676u, lean);
        m.PokeWord(e + 688u, Chance(rng, 2) ? lean : small(12, 18));
        m.PokeWord(e + 684u, Pick(rng, {0u, 0x28000u, 0xFFFD8000u, rng.U32()}));
        m.PokeWord(e + 740u, rng.U32() % 0x10001u);
        m.PokeWord(e + 732u, rng.U32() % 0x10001u);
        m.PokeWord(e + 772u, small(12, 18));
        m.PokeWord(e + 304u, rng.U32() % 0x40000u);
        dir(e + 432u);
        dir(e + 450u);
        if (Chance(rng, 4)) WriteU16Bench(m, e + 452u, static_cast<uint16_t>(-2049 - static_cast<int>(rng.U32() % 2000u)));
        dir(e + 516u);
        dir(e + 522u);
        if (Chance(rng, 3)) WriteU16Bench(m, e + 524u, static_cast<uint16_t>(Pick(rng, {3546u, 3547u, 3548u, 4096u, 2000u})));
        dir(e + 528u);
        dir(e + 820u);
        const uint8_t gear = static_cast<uint8_t>(rng.U32() % 5u);
        m.WriteBlock(e + 849u, &gear, 1);
        if (inRam(r)) {
            m.PokeWord(r + 604u, Pick(rng, {0u, 1u, 2u, 3u, 4u, 4u}));
            if (Chance(rng, 2)) WriteU16Bench(m, r + 544u, 0);
            uint8_t b = static_cast<uint8_t>(m.PeekByte(r + 572u) & ~0x10u);
            if (Chance(rng, 3)) {
                uint32_t other = pool + 1096u * (rng.U32() % slots);
                if (other == e) other = pool + 1096u * ((s + 1u) % slots);
                m.PokeWord(e + 856u, other);
                if (Chance(rng, 2) && inRam(m.PeekWord(other + 852u))) b |= 0x10u;
            } else {
                m.PokeWord(e + 856u, 0u);
            }
            m.WriteBlock(r + 572u, &b, 1);
        }
    }
    co::SetNumPlayers(m, Chance(rng, 4) ? 2u : 1u);
    m.PokeWord(c.gp + kRandSeedGpOffset, rng.U32());
    if (c.rootCounter != nullptr) *c.rootCounter = rng.U32() & 0xFFFFu;
    a.a[0] = static_cast<uint32_t>(bs::DrawDt(rng));
}

uint32_t NativeClass(NativeEnv& env, const Args& a) {
    Machine& cl = *env.clone;
    const uint32_t sp = env.sp;
    bs::SpliceStats::Entry& st = bs::Stats().For("rider_pass_class");
    ++st.cases;
    auto fail = [&env](const std::string& why) {
        if (env.failure.empty()) env.failure = why;
    };
    std::vector<uint8_t> refRam, refSpad;
    bool refReached = false;
    {
        Machine ref;
        bs::CopyMachine(cl, ref);
        ref.cpu.AddHook(rr::sim::kClassWalkExit, "exit");
        bs::StartCall(ref.cpu, rr::sim::kRiderPassFn, a, sp);
        const Trap t = ref.cpu.Run(kSentinel, env.maxSteps);
        if (t.kind == TrapKind::Hook && t.pc == rr::sim::kClassWalkExit) {
            refReached = true;
            refRam = ref.mem.ram();
            refSpad = ref.mem.scratchpad();
        } else if (t.kind != TrapKind::Halted) {
            fail("the reference run of the original trapped: " + t.ToString());
            return 0;
        }
    }
    cl.cpu.ClearHooks();
    cl.cpu.AddHook(rr::sim::kClassWalkEntry, "entry");
    bs::StartCall(cl.cpu, rr::sim::kRiderPassFn, a, sp);
    int entries = 0;
    for (;;) {
        cl.cpu.callObservations.clear();
        const Trap t = cl.cpu.Run(kSentinel, env.maxSteps);
        env.calls.insert(env.calls.end(), cl.cpu.callObservations.begin(), cl.cpu.callObservations.end());
        cl.cpu.callObservations.clear();
        if (t.kind == TrapKind::Halted) break;
        if (t.kind != TrapKind::Hook || t.pc != rr::sim::kClassWalkEntry) {
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
            ClassCallees cc(env, run);
            const bool ok = rr::sim::RiderPassClassWalk(g, dt, frame, tab.t, cc);
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
        cl.cpu.pc = rr::sim::kClassWalkExit;
        cl.cpu.npc = rr::sim::kClassWalkExit + 4u;
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
                          rr::sim::kClassWalkExit, differ, first);
            fail(buf);
            break;
        }
    }
    cl.cpu.ClearHooks();
    if (entries == 0 && refReached) fail("the reference passed the region but the clone never entered it");
    return cl.cpu.regs[2];
}

// ---------------------------------------------------------------- the rider pass, whole
void SetupWhole(CaseContext& c, Args& a) {
    switch (c.rng->Next() % 5u) {
        case 0: creact::SetupPoolLoop(c, a); break;
        case 1: co::SetupWalk(c, a); break;
        case 2: fallrows::SetupDown(c, a); break;
        case 3: SetupClass(c, a); break;
        default: a.a[0] = static_cast<uint32_t>(bs::DrawDt(*c.rng)); break;
    }
}

struct WholeCallees final : rr::sim::RiderPassCallees {
    NativeEnv& env;
    explicit WholeCallees(NativeEnv& e) : env(e) {}
    bool Oracle(uint32_t fn, std::initializer_list<uint32_t> args, uint32_t sp) {
        uint32_t r[4] = {0, 0, 0, 0};
        int n = 0;
        for (uint32_t x : args) r[n++] = x;
        bs::ExactOracle(env, fn, r, n, sp);
        return env.failure.empty();
    }
    bool PoolLoop(int32_t dt, uint32_t sp) override {
        EnRun run(env);
        rr::sim::GuestRam g = bs::StepView(env);
        const bs::StepTables tab(env.clone->mem, env.gp);
        creact::PoolCallees pc(env, run, tab.t);
        const bool ok = rr::sim::RiderPassPoolLoop(g, dt, sp, tab.t, pc);
        if (run.s.failed && env.failure.empty()) env.failure = "the sound port could not serve PlaySound3D";
        return ok && env.failure.empty();
    }
    bool ThrownWalk(int32_t dt, uint32_t sp) override {
        rr::sim::GuestRam g = bs::StepView(env);
        const bs::StepTables tab(env.clone->mem, env.gp);
        co::WalkCallees wc(env);
        return rr::sim::RiderPassThrownWalk(g, dt, sp, tab.t, wc) && env.failure.empty();
    }
    bool Engine(uint32_t list, int32_t dt, uint32_t sp) override { return Oracle(kEngineFn, {list, static_cast<uint32_t>(dt)}, sp); }
    bool ClassWalk(int32_t dt, uint32_t sp) override {
        EnRun run(env);
        rr::sim::GuestRam g = bs::StepView(env);
        const bs::StepTables tab(env.clone->mem, env.gp);
        ClassCallees cc(env, run);
        const bool ok = rr::sim::RiderPassClassWalk(g, dt, sp, tab.t, cc);
        if (run.s.failed && env.failure.empty()) env.failure = "the sound port could not serve PlaySound3D";
        return ok && env.failure.empty();
    }
    bool ImpactState(uint32_t list, uint32_t sp) override { return Oracle(kImpactStateFn, {list}, sp); }
    bool ListMigrate(uint32_t e, int32_t, uint32_t sp) override {
        const bs::StepTables tab(env.clone->mem, env.gp);
        bs::MigrateCallees mc(env, tab.t);
        rr::sim::GuestRam g = bs::StepView(env);
        return rr::sim::BikeListMigrate(g, e, sp, tab.t, mc) && env.failure.empty();
    }
    bool RiderGround(uint32_t r, int32_t dt, uint32_t sp) override {
        return Oracle(kRiderGroundFn, {r, static_cast<uint32_t>(dt)}, sp);
    }
    bool Heading(uint32_t list, int32_t dt, uint32_t sp) override {
        return Oracle(kHeadingFn, {list, static_cast<uint32_t>(dt)}, sp);
    }
    bool DownWalk(int32_t dt, uint32_t sp) override {
        rr::sim::GuestRam g = bs::StepView(env);
        const rr::sim::BikeTables tab = rr::sim::RecoverTables(env.clone->mem.ram().data(), g);
        // RowsFromUp 0x8007EA64, the walk's one callee, natively (collision.h) - the thrown walk runs it
        // natively too, so it is not an observed callee of this row; anything else through the oracle.
        struct Down final : rr::sim::RecoverCallees {
            NativeEnv& env;
            rr::sim::GuestRam& g;
            const rr::sim::BikeTables& t;
            Down(NativeEnv& e, rr::sim::GuestRam& gg, const rr::sim::BikeTables& tt) : env(e), g(gg), t(tt) {}
            bool Call(uint32_t fn, const uint32_t* a, int n, uint32_t sp, uint32_t& v0) override {
                v0 = 0;
                if (fn == rr::sim::kFallRowsFromUpFn) return rr::sim::RowsFromUp(g, a[0], t) && !g.Faulted();
                v0 = bs::ExactOracle(env, fn, a, n, sp);
                return env.failure.empty();
            }
        } oc(env, g, tab);
        return rr::sim::RiderPassDownWalk(g, dt, sp, tab, oc) && env.failure.empty();
    }
    bool DormantDrive(uint32_t e, int32_t dt, uint32_t sp) override {
        return Oracle(kDormantDriveFn, {e, static_cast<uint32_t>(dt)}, sp);
    }
};

uint32_t NativeWhole(NativeEnv& env, const Args& a) {
    rr::sim::GuestRam g = bs::StepView(env);
    WholeCallees wc(env);
    if (!rr::sim::RiderPass(g, static_cast<int32_t>(a.a[0]), env.sp, wc) && env.failure.empty()) {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "RiderPass refused (view fault 0x%08X)", g.FaultAddress());
        env.failure = buf;
    }
    return 0;
}

// ---------------------------------------------------------------- the two top-level passes
void SetupTop(CaseContext& c, Args& a) {
    Rng& rng = *c.rng;
    c.mem->PokeWord(rr::sim::kPedSwitchWord, Pick(rng, {0u, 1u, 1u, rng.U32() | 1u}));
    a.a[0] = static_cast<uint32_t>(218 * static_cast<int32_t>(1u + rng.U32() % 8u));
}

struct TopCallees final : rr::sim::WorldBikePassCallees, rr::sim::RiderEnginePassCallees {
    NativeEnv& env;
    explicit TopCallees(NativeEnv& e) : env(e) {}
    bool One(uint32_t fn, int n, int32_t dt, uint32_t sp) {
        uint32_t r[1] = {static_cast<uint32_t>(dt)};
        bs::ExactOracle(env, fn, r, n, sp);
        return env.failure.empty();
    }
    bool TrafficPass(int32_t dt, uint32_t sp) override { return One(rr::sim::passes::kTrafficPassFn, 1, dt, sp); }
    bool BikeStep(int32_t dt, uint32_t sp) override { return One(rr::sim::passes::kBikeStepPassFn, 1, dt, sp); }
    bool RiderOffPass(int32_t dt, uint32_t sp) override { return One(rr::sim::passes::kRiderOffPassFn, 1, dt, sp); }
    bool PropPass(int32_t dt, uint32_t sp) override { return One(rr::sim::passes::kPropPassFn, 1, dt, sp); }
    bool PedPass(int32_t dt, uint32_t sp) override { return One(rr::sim::passes::kPedPassFn, 1, dt, sp); }
    bool Pool5Pass(uint32_t sp) override { return One(rr::sim::passes::kPool5PassFn, 0, 0, sp); }
    bool VolumePass(uint32_t sp) override { return One(rr::sim::passes::kVolumePassFn, 0, 0, sp); }
    bool RiderPass(int32_t dt, uint32_t sp) override { return One(rr::sim::passes::kRiderPassWholeFn, 1, dt, sp); }
    bool PropAnimPass(int32_t dt, uint32_t sp) override { return One(rr::sim::passes::kPropAnimPassFn, 1, dt, sp); }
    bool PedRelease(int32_t dt, uint32_t sp) override { return One(rr::sim::passes::kPedReleaseFn, 1, dt, sp); }
    bool HazardPass(int32_t dt, uint32_t sp) override { return One(rr::sim::passes::kHazardPassFn, 1, dt, sp); }
};

uint32_t NativeWorld(NativeEnv& env, const Args& a) {
    rr::sim::GuestRam g = bs::StepView(env);
    TopCallees tc(env);
    rr::sim::WorldBikePass(g, static_cast<int32_t>(a.a[0]), env.sp, tc);
    return 0;
}
uint32_t NativeRiderEngine(NativeEnv& env, const Args& a) {
    rr::sim::GuestRam g = bs::StepView(env);
    TopCallees tc(env);
    rr::sim::RiderEnginePass(g, static_cast<int32_t>(a.a[0]), env.sp, tc);
    return 0;
}

// ---------------------------------------------------------------- the frame's clock
// frame_delta: SLUS 0x8001C428 on this machine with the clock +0x0C, the previous clock gp+1996, the frame
// counter gp+256 (58..61 and anything) and the state byte (1, 3, 4, 5, 6, anything) drawn.
void SetupFrameDelta(CaseContext& c, Args& a) {
    Rng& rng = *c.rng;
    Memory& m = *c.mem;
    const uint32_t gs = m.PeekWord(kGameStatePtr);
    m.PokeWord(gs + 0x0Cu, rng.U32());
    m.PokeWord(rr::sim::kFrameDeltaPrev, Chance(rng, 2) ? m.PeekWord(gs + 0x0Cu) - (rng.U32() % 64u) : rng.U32());
    m.PokeWord(rr::sim::kFrameDeltaCount, Pick(rng, {58u, 59u, 60u, 61u, 0u, rng.U32()}));
    m.PokeWord(gs + 0x64u, rng.U32());
    const uint8_t st = static_cast<uint8_t>(Pick(rng, {1u, 1u, 3u, 4u, 5u, 6u, 2u, rng.U32() & 0xFFu}));
    m.WriteBlock(gs, &st, 1);
    a.a[0] = 0;
}
uint32_t NativeFrameDelta(Memory& m, uint32_t, uint32_t gp, const Args&) {
    rr::sim::GuestRam g(m.ram().data(), gp);
    rr::sim::FrameDelta(g);
    return 0;
}

// pad_clock: a SPLICE of SLUS 0x8001CB3C (feel::SetupPad's family - the kernel's two critical-section
// syscalls replaced by nops on both sides, control 1's pause code 0) with the region [0x8001CD00,
// 0x8001CD7C) replaced by PadPollClock; live in and out s0. Drawn on top: the state byte (1 mostly, 3,
// 6, 0, 2 - which leaves before the region), game_state+0x02 and +0x03 (EnterRace's first-frame byte),
// the clock +0x0C and the last poll's +0x14, gp+2028 (the pause request of state 1).
void SetupPadClock(CaseContext& c, Args& a) {
    feel::SetupPad(c, a);
    Rng& rng = *c.rng;
    Memory& m = *c.mem;
    const uint32_t gs = m.PeekWord(kGameStatePtr);
    const uint8_t st = static_cast<uint8_t>(Pick(rng, {1u, 1u, 1u, 3u, 6u, 0u, 2u}));
    m.WriteBlock(gs, &st, 1);
    const uint8_t b2 = static_cast<uint8_t>(Chance(rng, 3) ? (rng.U32() & 0xFFu) : 0u);
    const uint8_t b3 = static_cast<uint8_t>(Chance(rng, 3) ? Pick(rng, {1u, 0x80u, rng.U32() & 0xFFu}) : 0u);
    m.WriteBlock(gs + 2u, &b2, 1);
    m.WriteBlock(gs + 3u, &b3, 1);
    const uint32_t clock = rng.U32();
    m.PokeWord(gs + 0x0Cu, clock);
    m.PokeWord(gs + 0x14u, Chance(rng, 2) ? clock - (rng.U32() % 64u) : rng.U32());
    m.PokeWord(c.gp + 2028u, Chance(rng, 6) ? 1u : 0u);
}
uint32_t NativePadClock(NativeEnv& env, const Args& a) {
    Machine& cl = *env.clone;
    const uint32_t sp = env.sp;
    bs::SpliceStats::Entry& st = bs::Stats().For("pad_clock");
    ++st.cases;
    auto fail = [&env](const std::string& why) {
        if (env.failure.empty()) env.failure = why;
    };
    std::vector<uint8_t> refRam, refSpad;
    bool refReached = false;
    {
        Machine ref;
        bs::CopyMachine(cl, ref);
        ref.cpu.AddHook(rr::sim::kPadClockExit, "exit");
        bs::StartCall(ref.cpu, rr::sim::kPadPollFn, a, sp);
        const Trap t = ref.cpu.Run(kSentinel, env.maxSteps);
        if (t.kind == TrapKind::Hook && t.pc == rr::sim::kPadClockExit) {
            refReached = true;
            refRam = ref.mem.ram();
            refSpad = ref.mem.scratchpad();
        } else if (t.kind != TrapKind::Halted) {
            fail("the reference run of the original trapped: " + t.ToString());
            return 0;
        }
    }
    cl.cpu.ClearHooks();
    cl.cpu.AddHook(rr::sim::kPadClockEntry, "entry");
    bs::StartCall(cl.cpu, rr::sim::kPadPollFn, a, sp);
    int entries = 0;
    for (;;) {
        cl.cpu.callObservations.clear();
        const Trap t = cl.cpu.Run(kSentinel, env.maxSteps);
        env.calls.insert(env.calls.end(), cl.cpu.callObservations.begin(), cl.cpu.callObservations.end());
        cl.cpu.callObservations.clear();
        if (t.kind == TrapKind::Halted) break;
        if (t.kind != TrapKind::Hook || t.pc != rr::sim::kPadClockEntry) {
            fail("the spliced run trapped: " + t.ToString());
            break;
        }
        if (++entries > 1) {
            fail("the region's entry was reached twice in one poll");
            break;
        }
        ++st.entered;
        const CpuRegs saved = CaptureRegs(cl.cpu);
        uint32_t s0 = cl.cpu.regs[16];
        if (!env.spliceControl) {
            rr::sim::GuestRam g(cl.mem.ram().data(), env.gp);
            s0 = rr::sim::PadPollClock(g, s0);
            if (g.Faulted()) {
                fail("the port faulted");
                break;
            }
        }
        RestoreRegs(saved, cl.cpu);
        cl.cpu.regs[16] = s0;
        cl.cpu.pc = rr::sim::kPadClockExit;
        cl.cpu.npc = rr::sim::kPadClockExit + 4u;
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
            std::snprintf(buf, sizeof(buf), "at the region's exit 0x%08X the spliced state differs in %zu byte(s), first 0x%08X",
                          rr::sim::kPadClockExit, differ, first);
            fail(buf);
            break;
        }
    }
    cl.cpu.ClearHooks();
    if (entries == 0 && refReached) fail("the reference passed the region but the clone never entered it");
    return cl.cpu.regs[2];
}

// results_prepare: SLUS 0x8003F708 on this machine's pool 0 with the finishing-order table 0x800D5D98 drawn:
// its first k (0..18) entries 1.. filled with distinct pool-0 handles (their places, a flag, rising stamps),
// the rest -1 (and now and then a hole with entries after it); per bike the place byte (a place, 0, or a
// result code 248..255), the finish stamp, the progress +0x144 (distinct, equal, 0, negative), the class
// nibble riderDef[1] (2 = police) and the stat block's +0xE0 (the speed the estimate divides by: below 256
// makes the divisor 0, R3000 `div` by zero); the player count 1 or 2; the race clock.
void SetupResults(CaseContext& c, Args& a) {
    Rng& rng = *c.rng;
    Memory& m = *c.mem;
    const uint32_t pool = m.PeekWord(kPoolTable);
    const uint32_t slots = 18;
    const uint32_t gs = m.PeekWord(kGameStatePtr);
    m.PokeWord(gs + 0x10u, rng.U32() % 400000u);
    co::SetNumPlayers(m, Chance(rng, 4) ? 2u : 1u);
    std::vector<uint32_t> order(slots);
    for (uint32_t k = 0; k < slots; ++k) order[k] = k;
    for (size_t k = slots; k > 1; --k) std::swap(order[k - 1], order[rng.U32() % k]);
    const uint32_t filled = Pick(rng, {0u, 1u, 2u, 5u, 17u, 18u, rng.U32() % 19u});
    for (uint32_t k = 0; k <= 18u; ++k) {
        const uint32_t rec = rr::sim::kFinishOrderTable + 16u * k;
        if (k >= 1 && k <= filled) {
            m.PokeWord(rec, order[k - 1]);
            m.PokeWord(rec + 4u, k);
            m.PokeWord(rec + 8u, Pick(rng, {0u, 1u}));
            m.PokeWord(rec + 12u, 1000u * k + rng.U32() % 900u);
        } else {
            m.PokeWord(rec, 0xFFFFFFFFu);
            m.PokeWord(rec + 4u, rng.U32());
            m.PokeWord(rec + 8u, rng.U32());
            m.PokeWord(rec + 12u, rng.U32());
        }
    }
    if (filled >= 3 && Chance(rng, 5)) m.PokeWord(rr::sim::kFinishOrderTable + 16u * 2u, 0xFFFFFFFFu); // a hole
    for (uint32_t s = 0; s < slots; ++s) {
        const uint32_t e = pool + 1096u * s;
        const uint32_t rd = m.PeekWord(e + 0x43Cu);
        const uint32_t st = m.PeekWord(e + 0x22Cu);
        const uint8_t place = static_cast<uint8_t>(Pick(rng, {0u, 1u + rng.U32() % 18u, 1u + rng.U32() % 18u, 248u + rng.U32() % 8u}));
        m.WriteBlock(rd + 0x27u, &place, 1);
        m.PokeWord(rd + 0x28u, Chance(rng, 3) ? rng.U32() % 100000u : 0u);
        const uint8_t cls = static_cast<uint8_t>((m.PeekByte(rd + 1u) & 0xF0u) | (Chance(rng, 5) ? 2u : (rng.U32() & 0xFu)));
        m.WriteBlock(rd + 1u, &cls, 1);
        m.PokeWord(e + 0x144u, Pick(rng, {0u, rng.U32() % 0x4000000u, rng.U32() % 0x4000000u, 0x1000000u,
                                         static_cast<uint32_t>(-static_cast<int32_t>(rng.U32() % 0x100000u))}));
        if (st >= 0x80000000u && st < 0x801FF000u)
            m.PokeWord(st + 0xE0u, Pick(rng, {m.PeekWord(st + 0xE0u), m.PeekWord(st + 0xE0u), 0u, 0xFFu, 0x100u,
                                              rng.U32() % 0x800000u}));
    }
    a.a[0] = 0;
}
struct ResultsBench final : rr::sim::ResultsCallees {
    Memory& m;
    explicit ResultsBench(Memory& mm) : m(mm) {}
    bool RecordFinish(uint32_t handle, int32_t flag) override { // SLUS 0x8003F680, PORTED (race.h), on host views
        const uint32_t e = m.PeekWord(0x8005B3A0u) + 1096u * handle;
        uint8_t* ep = bs::RawAt(m, e, 1096);
        uint8_t* rd = ep != nullptr ? bs::RawAt(m, m.PeekWord(e + 0x43Cu), 72) : nullptr;
        if (rd == nullptr) return false;
        rr::sim::FinishOrderEnv fo;
        fo.entity = ep;
        fo.riderDef = rd;
        fo.table = bs::RawAt(m, rr::sim::kFinishOrderTable, 19u * 16u);
        fo.tableBytes = 19 * 16;
        return rr::sim::RecordFinish(flag, fo);
    }
};
uint32_t NativeResults(Memory& m, uint32_t, uint32_t gp, const Args&) {
    rr::sim::GuestRam g(m.ram().data(), gp);
    ResultsBench rb(m);
    return rr::sim::ResultsPrepare(g, rb) ? 0u : 0xDEADDEADu;
}

} // namespace passrows

void AddPassesRows(std::vector<Row>& rows) {
    using namespace passrows;
    {
        rows.push_back(Row{"rider_pass_class",
                           "region [0x8007C9DC, 0x8007DCD4) of 0x8007B840(s32 dt)  [RASHCDG, a SPLICE: the class-list walk]",
                           rr::sim::kRiderPassFn, SetupClass, nullptr});
        Row& r = rows.back();
        r.compareV0 = false;
        r.nativeEx = NativeClass;
        r.maxSteps = 20000000;
        r.oracleCallees = {OracleCallee{kGetRCntFn, 1}};
    }
    {
        rows.push_back(Row{"rider_pass", "void 0x8007B840(s32 dt)  [RASHCDG, the rider / engine pass WHOLE]",
                           rr::sim::kRiderPassFn, SetupWhole, nullptr});
        Row& r = rows.back();
        r.compareV0 = false;
        r.nativeEx = NativeWhole;
        r.maxSteps = 40000000;
        const uint32_t d = rr::sim::kRiderPassFrame;
        using namespace rr::sim::react;
        using namespace animrows;
        r.oracleCallees = {OracleCallee{kJunctionMargin, 1},  OracleCallee{rr::sim::coll::kImpactTurn, 4},
                           OracleCallee{kLaunch, 2},          OracleCallee{kRoadRebind, 2},
                           OracleCallee{kGroundFrame, 2},     OracleCallee{kSteerLean, 1},
                           OracleCallee{kTurnFacing, 2},      bs::KnockOffCallee(),
                           bs::LaunchCallee(),                OracleCallee{rr::sim::kStanceEventFn, 3},
                           OracleCallee{kCrashEmit, 3},
                           Capture(d + kFrEvent + kFrGate + kFrTransition + kFrRestart + kFrPoseStart),
                           Leave(d + kFrEvent + kFrLeave),    Enter(d + kFrEvent + kFrEnter),
                           OracleCallee{kGetRCntFn, 1},       OracleCallee{kEngineFn, 2},
                           OracleCallee{kImpactStateFn, 1},   OracleCallee{kRiderGroundFn, 2},
                           OracleCallee{kHeadingFn, 2},       OracleCallee{kDormantDriveFn, 2}};
    }
    rows.push_back(Row{"frame_delta", "void 0x8001C428()  [SLUS, the main loop's frame bookkeeping: game_state+0x20]",
                       rr::sim::kFrameDeltaFn, SetupFrameDelta, NativeFrameDelta, /*compareV0=*/false});
    {
        rows.push_back(Row{"pad_clock",
                           "region [0x8001CD00, 0x8001CD7C) of 0x8001CB3C()  [SLUS, a SPLICE: the pad poll's clock and the racing state]",
                           rr::sim::kPadPollFn, SetupPadClock, nullptr});
        Row& r = rows.back();
        r.compareV0 = false;
        r.nativeEx = NativePadClock;
        r.maxSteps = 20000000;
    }
    rows.push_back(Row{"results_prepare", "void 0x8003F708()  [SLUS, the places and stamps of the riders still racing]",
                       rr::sim::kResultsPrepareFn, SetupResults, NativeResults, /*compareV0=*/false});
    {
        rows.push_back(Row{"world_bike_pass", "void 0x8008AC80(s32 dt)  [RASHCDG, RaceTick's world pass]",
                           rr::sim::kWorldBikePassFn, SetupTop, nullptr});
        Row& r = rows.back();
        r.compareV0 = false;
        r.nativeEx = NativeWorld;
        r.maxSteps = 200000000;
        r.oracleCallees = {OracleCallee{rr::sim::passes::kTrafficPassFn, 1}, OracleCallee{rr::sim::passes::kBikeStepPassFn, 1},
                           OracleCallee{rr::sim::passes::kRiderOffPassFn, 1}, OracleCallee{rr::sim::passes::kPropPassFn, 1},
                           OracleCallee{rr::sim::passes::kPedPassFn, 1},      OracleCallee{rr::sim::passes::kPool5PassFn, 0},
                           OracleCallee{rr::sim::passes::kVolumePassFn, 0}};
    }
    {
        rows.push_back(Row{"rider_engine_pass", "void 0x8008ACE8(s32 dt)  [RASHCDG, RaceTick's rider / engine pass]",
                           rr::sim::kRiderEnginePassFn, SetupTop, nullptr});
        Row& r = rows.back();
        r.compareV0 = false;
        r.nativeEx = NativeRiderEngine;
        r.maxSteps = 200000000;
        r.oracleCallees = {OracleCallee{rr::sim::passes::kRiderPassWholeFn, 1}, OracleCallee{rr::sim::passes::kPropAnimPassFn, 1},
                           OracleCallee{rr::sim::passes::kPedReleaseFn, 1},     OracleCallee{rr::sim::passes::kHazardPassFn, 1}};
    }
}

// The extent [entry, end) of each function these rows accept, for `--coverage`.
const CoverageRange* PassesCoverage(const char* rowName) {
    struct Named {
        const char* name;
        CoverageRange range;
    };
    static const Named kRanges[] = {
        {"rider_pass_class", {0x8007C9DC, 0x8007DCD4}}, {"rider_pass", {0x8007B840, 0x8007E868}},
        {"world_bike_pass", {0x8008AC80, 0x8008ACE8}},  {"rider_engine_pass", {0x8008ACE8, 0x8008AD38}},
        {"frame_delta", {0x8001C428, 0x8001C498}},      {"pad_clock", {0x8001CD00, 0x8001CD7C}},
        {"results_prepare", {0x8003F708, 0x8003F8D8}},
    };
    for (const Named& n : kRanges)
        if (std::strcmp(n.name, rowName) == 0) return &n.range;
    return nullptr;
}
