// Bench rows for the bike step 0x80075EE0. Included by verify_physics.cpp inside its anonymous
// namespace, so everything defined above that point is visible here.
//
// The native code is src\game\sim\bike_step.{h,cpp}.

// rows_population.inc (included later): region J's two passes, 0x80093E6C and 0x800950E8, run
// NATIVELY (src\game\sim\population.h) with their own unported callees on the seam, and the
// declarations of those callees. `road` answers the road layer's six for
// them, as it does for region H.
bool PopActivationPassNative(NativeEnv& env, uint32_t sp, rr::sim::RoadRuntimeCallees& road);
bool PopDownedPassNative(NativeEnv& env, uint32_t sp, rr::sim::RoadRuntimeCallees& road);
std::vector<OracleCallee> PopPassSeams(uint32_t stanceFrame);

namespace bs {

// RASHCDG.BIN, SHA-1 cfe43a7786759f2cb9c57751cf99e84d1074782c.
constexpr uint32_t kHeadingPass = 0x8007AC04; // 783 instructions, extent ..0x8007B840
constexpr uint32_t kResetOrient = 0x8008CF74; // 26 instructions, extent ..0x8008CFDC
constexpr uint32_t kStatArrayPtr = 0x8005B248;
constexpr uint32_t kRideList = 0x8005B298;
constexpr uint32_t kPlayersView = 0x8005B220; // read at 0x8007B3D0: non-zero, a player bike's roll is smoothed too

// The static tables the ports take as host pointers, read out of the machine's own image. The
// rsqrt table lives on the game's heap behind *(gp+2260).
struct Tables {
    std::vector<int16_t> sincos;
    int32_t atan[20]{};
    std::vector<uint16_t> rsqrt;
    rr::sim::BikeTables t;
    Tables(const Memory& m, uint32_t gp) {
        sincos.assign(8192, 0);
        m.ReadBlock(kSinCosTable, sincos.data(), sincos.size() * sizeof(int16_t));
        for (uint32_t i = 0; i < 20; ++i) atan[i] = GetS32(m, kAtanTable + 4u * i);
        rsqrt.assign(2048, 0);
        m.ReadBlock(m.PeekWord(gp + kRsqrtTableGpOffset), rsqrt.data(), rsqrt.size() * sizeof(uint16_t));
        t.sincos = sincos.data();
        t.atan = atan;
        t.rsqrt = rsqrt.data();
    }
};

uint32_t Done(NativeEnv& env, const rr::sim::GuestRam& g, bool ok, const char* what) {
    if (!ok && env.failure.empty()) {
        char buf[160];
        std::snprintf(buf, sizeof(buf), "%s refused%s (fault 0x%08X)", what,
                      g.Faulted() ? ": the view faulted" : "", g.FaultAddress());
        env.failure = buf;
    }
    return 0;
}

// Links `count` entities (addresses in `e`) into an intrusive list headed at `head`, in order:
// {prev, next} at +0x440, entity = node - 1088.
void LinkList(Memory& m, uint32_t head, const uint32_t* e, uint32_t count) {
    uint32_t prev = head;
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t node = e[i] + 1088u;
        m.PokeWord(prev + 4u, node);
        m.PokeWord(node, prev);
        prev = node;
    }
    m.PokeWord(prev + 4u, head);
    m.PokeWord(head, prev);
}

// A captured bike (dump-derived) or a captured bike with its orientation and the fields the rows
// read made random (randomised). The caller plants pointers and flags on top.
std::vector<uint8_t> BaseEntity(CaseContext& c) {
    if (c.sample != nullptr && c.sample->entity.size() == 1096) return c.sample->entity;
    if (c.any != nullptr && c.any->entity.size() == 1096) return c.any->entity;
    return std::vector<uint8_t>(1096, 0);
}

// ---------------------------------------------------------------- the heading pass, 0x8007AC04
//
// Scratch layout: the list head at +0, up to three bikes at +16 + 1100*i, the stat block at +3400.
void SetupHeading(CaseContext& c, Args& a) {
    Rng& rng = *c.rng;
    Memory& m = *c.mem;
    const bool derived = c.sample != nullptr;
    auto chance = [&rng](uint32_t n) { return rng.Next() % n == 0; };
    auto unit = [&rng](std::vector<uint8_t>& v, uint32_t off) {
        for (uint32_t k = 0; k < 3; ++k) PutU16V(v, off + 2u * k, static_cast<uint16_t>(rng.Unit()));
    };
    const uint32_t head = c.scratch;
    const uint32_t statsAt = c.scratch + 3400u;
    const uint32_t count = 1u + rng.U32() % 3u;
    uint32_t ents[3];
    for (uint32_t i = 0; i < count; ++i) ents[i] = c.scratch + 16u + 1100u * i;

    // The stat block: dump-derived cases read one of this machine's REAL blocks (the race loader's
    // LEVEL1.PH / CRUISEA1.PH slices); randomised ones a planted block whose ramp
    // window [+0x128, +0x12C) is drawn in both orders.
    std::vector<uint8_t> stats(448);
    const uint32_t realArray = m.PeekWord(kStatArrayPtr);
    if (derived && realArray >= 0x80000000u && realArray < 0x80200000u - 4u * 448u) {
        m.ReadBlock(realArray + 448u * (rng.U32() % 4u), stats.data(), stats.size());
    } else {
        for (uint8_t& b : stats) b = static_cast<uint8_t>(rng.U32());
        const int32_t lo = static_cast<int32_t>(rng.U32() % 0x20000u);
        const int32_t hi = chance(4) ? lo - static_cast<int32_t>(rng.U32() % 0x8000u)
                                     : lo + static_cast<int32_t>(rng.U32() % 0x30000u);
        PutU32V(stats, 0x128, static_cast<uint32_t>(lo));
        PutU32V(stats, 0x12C, static_cast<uint32_t>(hi));
        PutU32V(stats, 0xEC, static_cast<uint32_t>(rng.S32() >> 14));
        if (chance(6)) PutU32V(stats, 0x12C, static_cast<uint32_t>(lo)); // an empty window
    }
    m.WriteBlock(statsAt, stats.data(), stats.size());

    for (uint32_t i = 0; i < count; ++i) {
        std::vector<uint8_t> e = BaseEntity(c);
        if (!derived) {
            for (uint32_t r = 0; r < 3; ++r) unit(e, 0x1B0 + 6u * r);
            for (uint32_t r = 0; r < 3; ++r) unit(e, 0x204 + 6u * r);
            unit(e, 0x1C2);
            for (uint32_t k = 0; k < 3; ++k) PutU32V(e, 0x1F8 + 4u * k, static_cast<uint32_t>(rng.World()));
            PutU32V(e, 0x134, rng.U32() % 0x40000u);
        }
        // The rotate arm's zero vector: +0x210 and +0x204 both zero make Blend16 give 0, which is
        // the only way Normalize returns 0 and the identity reset 0x8008CF74 runs.
        if (chance(10)) {
            for (uint32_t k = 0; k < 3; ++k) { PutU16V(e, 0x210 + 2u * k, 0); PutU16V(e, 0x204 + 2u * k, 0); }
        }
        auto scalar = [&](uint32_t off, int shift, uint32_t zeroOdds) {
            if (!derived || chance(3)) PutU32V(e, off, static_cast<uint32_t>(rng.S32() >> shift));
            if (chance(zeroOdds)) PutU32V(e, off, 0);
        };
        scalar(0x240, 8, 4);
        scalar(0x1E8, 13, 3);
        scalar(0x2A4, 13, 2);  // the lean: 0 takes the copy arm
        scalar(0x28C, 13, 5);
        scalar(0x27C, 13, 5);
        scalar(0x268, 12, 2);  // the pitch: 0 puts the box centre on the contact point
        scalar(0x288, 13, 4);
        scalar(0x294, 12, 2);
        scalar(0x304, 12, 3);
        if (chance(3)) PutU32V(e, 0x2A4, (rng.U32() % 2u) ? 0xFFFFFFFFu : 0x80000000u); // cos < 0 arms
        // The inlined RatAtan2's x == 0 arms (0x8007AF28, 0x8007AF78): the orientation's +0x20C is 0.
        if (chance(8)) PutU16V(e, 0x20C, 0);
        if (chance(16)) PutU16V(e, 0x206, 0);
        // The ramp's `n == 0` arms (0x8007B168, 0x8007B284) need the magnitude EXACTLY at the ramp's
        // start stats[+0x128]. With +0x206 == 0 and +0x20C > 0 the slope angle is 0, so +0x27C
        // itself is the magnitude the second ramp tests.
        if (chance(6)) {
            const uint32_t lo = GetU32V(stats, 0x128);
            const uint32_t v = chance(2) ? lo : 0u - lo;
            PutU32V(e, 0x28C, v);
            PutU32V(e, 0x27C, v);
            PutU16V(e, 0x206, 0);
            PutU16V(e, 0x20C, static_cast<uint16_t>(1u + rng.U32() % 4096u));
            PutU32V(e, 0x1E8, (v & 0x80000000u) ? 0xFFFF0000u : 0x10000u); // same sign: the ramp is taken
        }
        PutU16V(e, 0x3B0, chance(2) ? 0 : static_cast<uint16_t>(rng.U32()));
        PutU32V(e, 0x358, chance(2) ? 0u : (0x80000000u | (rng.U32() & 0x1FFFFCu)));
        PutU16V(e, 0xAC, static_cast<uint16_t>(chance(2) ? rng.U32() % 3u : rng.U32()));
        uint32_t fb = rng.U32(), fc = rng.U32();
        if (derived && chance(2)) { fb = GetU32V(e, 0x234); fc = GetU32V(e, 0x238); }
        auto bits = [&](uint32_t& w, uint32_t mask, uint32_t odds) {
            w = chance(odds) ? (w | (mask & rng.U32())) : (w & ~mask);
        };
        bits(fb, 0x3000, 3);
        bits(fb, 0x40000, 3);
        bits(fb, 0x400, 2);
        bits(fb, 0x200000, 4);
        bits(fc, 0x1F, 3);
        bits(fc, 0x600, 3);
        bits(fc, 0x4000, 3);
        bits(fc, 0x14C, 2);
        bits(fc, 0x100, 3);
        bits(fc, 0xEC, 3);
        bits(fc, 0x7FF, 4);
        PutU32V(e, 0x234, fb);
        PutU32V(e, 0x238, fc);
        PutU32V(e, 0x22C, statsAt);
        m.WriteBlock(ents[i], e.data(), e.size());
    }
    LinkList(m, head, ents, count);
    // 0x8005B220 is 0 in every capture; a non-zero word smooths a PLAYER bike's roll too (0x8007B3D8).
    if (chance(3)) m.PokeWord(kPlayersView, rng.U32() | 1u);
    a.a[0] = head;
    a.a[1] = rng.U32(); // dt: dead in this function
}

// A development aid, off unless RRJB_BSDEBUG is set: runs the ORIGINAL at `entry` on a private copy
// of the clone's memory and prints every RAM byte where it and the port's result differ, relative
// to `base`. It is not part of any comparison.
void DebugDiff(NativeEnv& env, const std::vector<uint8_t>& before, uint32_t entry, const Args& a,
               uint32_t base) {
    if (std::getenv("RRJB_BSDEBUG") == nullptr) return;
    Machine mm;
    mm.mem.ram() = before;
    mm.mem.scratchpad() = env.clone->mem.scratchpad();
    std::memcpy(mm.cpu.cop0, env.clone->cpu.cop0, sizeof(mm.cpu.cop0));
    mm.cpu.regs[28] = env.gp;
    const CallResult r = CallGuest(mm.cpu, entry, a.a, env.sp, 50'000'000);
    const std::vector<uint8_t>& A = mm.mem.ram();
    const std::vector<uint8_t>& B = env.clone->mem.ram();
    std::printf("DEBUG %s:", r.ok() ? "" : r.trap.ToString().c_str());
    int shown = 0;
    for (size_t i = 0; i < A.size() && shown < 24; ++i) {
        const uint32_t addr = 0x80000000u + static_cast<uint32_t>(i);
        if (addr >= env.sp - 4096u && addr <= env.sp + 19u) continue;
        if (A[i] != B[i]) {
            std::printf(" +%X(%02X/%02X)", addr - base, A[i], B[i]);
            ++shown;
        }
    }
    std::printf("\n");
}

uint32_t NativeHeading(NativeEnv& env, const Args& a) {
    Memory& m = env.clone->mem;
    const std::vector<uint8_t> before = std::getenv("RRJB_BSDEBUG") ? m.ram() : std::vector<uint8_t>();
    rr::sim::GuestRam g(m.ram().data(), env.gp);
    const Tables tab(m, env.gp);
    const bool ok = rr::sim::BikeHeadingPass(g, a.a[0], tab.t);
    DebugDiff(env, before, kHeadingPass, a, a.a[0] + 16u);
    return Done(env, g, ok, "BikeHeadingPass");
}

void SetupReset(CaseContext& c, Args& a) {
    std::vector<uint8_t> e = BaseEntity(c);
    for (uint8_t& b : e) if (c.sample == nullptr) b = static_cast<uint8_t>(c.rng->U32());
    c.mem->WriteBlock(c.scratch, e.data(), e.size());
    a.a[0] = c.scratch;
}

uint32_t NativeReset(Memory& m, uint32_t scratch, uint32_t gp, const Args& a) {
    (void)scratch;
    rr::sim::GuestRam g(m.ram().data(), gp);
    rr::sim::BikeResetOrientation(g, a.a[0]);
    return 0;
}

// ================================================================ THE SPLICE
//
// A region of the per-bike step is not a function, so it cannot be called. A splice row calls the
// WHOLE step `0x80075EE0(dt)` instead, on both sides:
//
//   * the guest run (CmdPhysImpl, unchanged) is the original step, start to finish;
//   * the native run is the original step TOO, on the clone, with a hook at the region's entry PC:
//     when it fires, the port runs on the clone's own RAM, the registers the NEXT region reads are
//     set to the constants the disassembly proves they hold, `pc` goes to the region's exit, and
//     the original carries on from there;
//   * in addition, a THIRD run - the original on a private copy of the clone's starting machine,
//     stopped by a hook at the region's EXIT - gives the state the original region leaves, and the
//     clone's state at the same PC is compared with it, byte for byte, outside the stack window.
//     That is what keeps a later region that overwrites a field from hiding a wrong answer there.
//
// Everything outside the region is the original's own code on both sides, so a green splice row
// proves the region and nothing else.
struct Splice {
    const char* row;
    uint32_t entry, exit;
    // Registers the code at `exit` reads before writing (the live-in of the next region), with the
    // constant each provably holds there.
    std::vector<std::pair<int, uint32_t>> liveOut;
    std::function<bool(NativeEnv&, rr::sim::GuestRam&, int32_t dt)> region;
};

// Everything CmdPhysImpl sets on the clone, so the private reference run is the same machine.
void CopyMachine(Machine& from, Machine& to) {
    to.mem.ram() = from.mem.ram();
    to.mem.scratchpad() = from.mem.scratchpad();
    to.mem.bios() = from.mem.bios();
    std::memcpy(to.cpu.cop0, from.cpu.cop0, sizeof(to.cpu.cop0));
    to.cpu.trapOnBiosVector = from.cpu.trapOnBiosVector;
    to.cpu.rootCountersAreConstant = from.cpu.rootCountersAreConstant;
    to.cpu.rootCounterValue = from.cpu.rootCounterValue;
    to.cpu.spuWritesAreDropped = from.cpu.spuWritesAreDropped;
    to.cpu.spuControlRegisterFile = from.cpu.spuControlRegisterFile;
    std::memcpy(to.cpu.spuControl, from.cpu.spuControl, sizeof(to.cpu.spuControl));
    RestoreRegs(CaptureRegs(from.cpu), to.cpu);
}

void StartCall(Cpu& cpu, uint32_t entry, const Args& a, uint32_t sp) {
    for (int k = 0; k < 4; ++k) cpu.regs[4 + k] = a.a[static_cast<size_t>(k)];
    cpu.regs[29] = sp;
    cpu.regs[31] = kSentinel;
    cpu.pc = entry;
    cpu.npc = entry + 4u;
    cpu.loadDelayReg = Cpu::kNoLoadDelay;
    cpu.loadDelayValue = 0;
}

// Per-row counts, printed once at exit: how many cases entered the region, and how many exit
// states were compared - a splice row whose cases never reach its region proves nothing.
struct SpliceStats {
    struct Entry {
        std::string row;
        long cases = 0, entered = 0, exitCompared = 0, exitDiffered = 0;
    };
    std::vector<Entry> rows;
    Entry& For(const char* row) {
        for (Entry& e : rows)
            if (e.row == row) return e;
        rows.push_back(Entry{row});
        return rows.back();
    }
    ~SpliceStats() {
        for (const Entry& e : rows)
            std::printf("splice %-22s cases %ld, region entered in %ld, exit state compared %ld, "
                        "differed %ld\n",
                        e.row.c_str(), e.cases, e.entered, e.exitCompared, e.exitDiffered);
    }
};
SpliceStats& Stats() {
    static SpliceStats s;
    return s;
}

uint32_t RunSplice(NativeEnv& env, const Args& a, const Splice& s) {
    Machine& cl = *env.clone;
    const uint32_t sp = env.sp;
    SpliceStats::Entry& st = Stats().For(s.row);
    ++st.cases;
    auto fail = [&env](const std::string& why) {
        if (env.failure.empty()) env.failure = why;
    };

    // ---- the reference: the ORIGINAL on a private copy, stopped at the region's exit
    std::vector<uint8_t> refRam, refSpad;
    bool refReached = false;
    {
        Machine ref;
        CopyMachine(cl, ref);
        ref.cpu.AddHook(s.exit, "exit");
        StartCall(ref.cpu, rr::sim::kBikeStepFn, a, sp);
        const Trap t = ref.cpu.Run(kSentinel, env.maxSteps);
        if (t.kind == TrapKind::Hook && t.pc == s.exit) {
            refReached = true;
            refRam = ref.mem.ram();
            refSpad = ref.mem.scratchpad();
        } else if (t.kind != TrapKind::Halted) {
            fail("the reference run of the original trapped: " + t.ToString());
            return 0;
        }
    }

    // ---- the clone: the original with the region spliced out
    cl.cpu.ClearHooks();
    cl.cpu.AddHook(s.entry, "entry");
    StartCall(cl.cpu, rr::sim::kBikeStepFn, a, sp);
    int entries = 0;
    for (;;) {
        cl.cpu.callObservations.clear();
        const Trap t = cl.cpu.Run(kSentinel, env.maxSteps);
        env.calls.insert(env.calls.end(), cl.cpu.callObservations.begin(), cl.cpu.callObservations.end());
        cl.cpu.callObservations.clear();
        if (t.kind == TrapKind::Halted) break;
        if (t.kind != TrapKind::Hook || t.pc != s.entry) {
            fail("the spliced run trapped: " + t.ToString());
            break;
        }
        if (++entries > 1) {
            fail("the region's entry was reached twice in one step");
            break;
        }
        ++st.entered;
        // The port runs on the clone's RAM. The CPU is saved and restored around it, because an
        // oracle-supplied callee the port asks for runs on this same CPU.
        const CpuRegs saved = CaptureRegs(cl.cpu);
        const int32_t dt = GetS32(cl.mem, cl.cpu.regs[29] + 136u); // the caller's slot, sp+136
        if (!env.spliceControl) {
            rr::sim::GuestRam g(cl.mem.ram().data(), env.gp);
            if (!s.region(env, g, dt)) {
                char buf[128];
                std::snprintf(buf, sizeof(buf), "the port refused the region (view fault 0x%08X)",
                              g.FaultAddress());
                fail(buf);
                break;
            }
        }
        RestoreRegs(saved, cl.cpu);
        for (const auto& r : s.liveOut) cl.cpu.regs[r.first] = r.second;
        cl.cpu.pc = s.exit;
        cl.cpu.npc = s.exit + 4u;
        cl.cpu.loadDelayReg = Cpu::kNoLoadDelay;
        cl.cpu.loadDelayValue = 0;
        // ---- the region's output, compared at its exit
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
                          "at the region's exit 0x%08X the spliced state differs from the original's "
                          "in %zu byte(s), first 0x%08X",
                          s.exit, differ, first);
            fail(buf);
            break;
        }
    }
    cl.cpu.ClearHooks();
    if (entries == 0 && refReached) fail("the reference passed the region but the clone never entered it");
    return cl.cpu.regs[2];
}

// ---------------------------------------------------------------- the regions' callees
// RASHCDG, SHA-1 cfe43a77...: the three unported callees of the regions, each supplied by the
// oracle at the depth the step calls it from (its own frame is 136 bytes), and the PORTED crash
// timer, run natively on the clone's RAM with its own oracle-supplied emitter (as `bike_crash_timer`
// does), called from the timer's 24-byte frame inside the step's.
constexpr uint32_t kPassengerLaunch = 0x80074D58; // 1 argument (its dt is dead)
constexpr uint32_t kStepFrame = 136;              // `addiu sp,sp,-136` at 0x80075EE0
constexpr uint32_t kCrashTimerFrame = 24;         // `addiu sp,sp,-24` at 0x80074E6C
// PORTED: the list migration, its leaf, the wipeout end and its row
// rotation, and the passenger launch. What stays on the seam under them is the rider layer.
constexpr uint32_t kListMigrateFn = 0x80071BCC;   // frame 40
constexpr uint32_t kRideArmReset = 0x8007F08C;    // leaf
constexpr uint32_t kRecoverEnd = 0x80072994;      // frame 32
constexpr uint32_t kRotateRowPair = 0x8002ED94;   // SLUS, frame 56
constexpr uint32_t kRiderKnockOff = 0x80090D84;   // 1 argument, NOT ported
constexpr uint32_t kRiderLaunch = 0x80091468;     // 1 argument, NOT ported
constexpr uint32_t kMigrateFrame = 40;            // `addiu sp,sp,-40` at 0x80071BCC
constexpr uint32_t kRecoverFrame = 32;            // `addiu sp,sp,-32` at 0x80072994
constexpr uint32_t kLaunchFrame = 32;             // `addiu sp,sp,-32` at 0x80074D58
// Region J's two unported passes, no arguments.
constexpr uint32_t kActivationPass = 0x80093E6C;
constexpr uint32_t kDownedRiderPass = 0x800950E8;

// ---------------------------------------------------------------- exact-depth oracle calls
// The ORIGINAL code of an unported callee, run on the clone at EXACTLY the stack pointer the original
// makes the call at - which the port computes from the frames of its own call chain, as
// rows_road_runtime.inc's RrSeams does. Stack arguments are stored at sp+16.. first; every callee
// reached this way is declared with frame 0. The guest's call and the port's are then observed at the
// same depth, and their frames land on the same stack bytes.
uint32_t ExactOracle(NativeEnv& env, uint32_t address, const uint32_t* a, int n, uint32_t sp) {
    uint32_t r[4] = {0, 0, 0, 0};
    for (int k = 0; k < n && k < 4; ++k) r[k] = a[k];
    for (int k = 4; k < n; ++k) env.clone->mem.PokeWord(sp + 16u + 4u * static_cast<uint32_t>(k - 4), a[k]);
    const uint32_t saved = env.sp;
    env.sp = sp;
    const uint32_t v = env.CallOracleArgs(address, r, 4);
    env.sp = saved;
    return v;
}


// A raw host view of guest RAM (KSEG0 only, as GuestMirror maps it), or null. The ported callees of
// the step run on RAW views of the clone's own memory rather than on copies: a record two chases
// reach (a passenger that is another listed bike, a contact partner) is one record, as on the console.
uint8_t* RawAt(Memory& m, uint32_t a, uint32_t bytes) {
    if (a < 0x80000000u || static_cast<uint64_t>(a) + bytes > 0x80200000ull) return nullptr;
    return m.ram().data() + (a - 0x80000000u);
}

// Everything a bike's pointers lead to, resolved out of the clone's RAM (IntegratorHarness's rule,
// integrator.h BikeLinks), as raw views.
rr::sim::BikeLinks RawLinks(Memory& m, uint32_t ea) {
    rr::sim::BikeLinks L;
    const uint32_t ownerAddr = m.PeekWord(ea + rr::sim::ent::kOwner);
    const uint32_t riderAddr = m.PeekWord(ea + rr::sim::ent::kRider);
    L.owner = RawAt(m, ownerAddr, kOwnerBytes);
    if (riderAddr != 0) {
        L.rider = RawAt(m, riderAddr, 1096);
        if (L.rider != nullptr) L.riderOwner = RawAt(m, m.PeekWord(riderAddr + rr::sim::ent::kOwner), kOwnerBytes);
    }
    L.slice = RawAt(m, m.PeekWord(ea + 0x154), 32);
    L.obstacle = RawAt(m, m.PeekWord(ea + 0x33C), 64);
    L.stats = RawAt(m, m.PeekWord(ea + rr::sim::ent::kStats), kStatsBytes);
    const uint32_t ct = m.PeekWord(ea + 0x340);
    L.contact.contact = RawAt(m, ct, kContactBytes);
    if (L.contact.contact != nullptr) {
        const uint32_t h = static_cast<uint32_t>(L.contact.contact[0]) |
                           (static_cast<uint32_t>(L.contact.contact[1]) << 8);
        const uint32_t pool = h >> 5, slot = h & 31u;
        if (pool == 0) {
            L.contact.bike = RawAt(m, m.PeekWord(kPool0BasePtr) + 1096u * slot, 1096);
        } else if (pool == 3) {
            L.contact.traffic = RawAt(m, kTrafficRecords + 512u * slot, 512);
        } else if (pool == 4) {
            const uint32_t prop = m.PeekWord(kPool4BasePtr) + 596u * slot;
            L.contact.prop = RawAt(m, prop, 596);
            const uint32_t rec = m.PeekWord(prop);
            L.contact.propKindWord = static_cast<uint16_t>(m.PeekByte(rec + 14u) | (m.PeekByte(rec + 15u) << 8));
        }
    }
    L.wipeoutRateByte = m.PeekByte(kWipeoutRateByte);
    return L;
}

// Every game table the step's ports read, out of the machine's own image.
struct StepTables {
    std::vector<int16_t> sincos, sqrtw;
    std::vector<uint16_t> asin, rsqrt;
    int32_t atan[20]{};
    rr::sim::BikeTables t;
    StepTables(const Memory& m, uint32_t gp) {
        sincos.assign(8192, 0);
        m.ReadBlock(kSinCosTable, sincos.data(), sincos.size() * sizeof(int16_t));
        asin.assign(kAsinEntries, 0);
        m.ReadBlock(kAsinTable, asin.data(), asin.size() * sizeof(uint16_t));
        for (uint32_t i = 0; i < 20; ++i) atan[i] = GetS32(m, kAtanTable + 4u * i);
        rsqrt.assign(2048, 0);
        m.ReadBlock(m.PeekWord(gp + kRsqrtTableGpOffset), rsqrt.data(), rsqrt.size() * sizeof(uint16_t));
        sqrtw.assign(kSqrtWindowHalf, 0);
        m.ReadBlock(kSqrtTable - kSqrtWindowHalf / 2u, sqrtw.data(), sqrtw.size() * 2u);
        t.sincos = sincos.data();
        t.asin = asin.data();
        t.atan = atan;
        t.rsqrt = rsqrt.data();
        t.sqrt = sqrtw.data() + kSqrtWindowHalf / 4u;
    }
};

rr::sim::GuestRam StepView(NativeEnv& env) {
    rr::sim::GuestRam g(env.clone->mem.ram().data(), env.gp);
    g.SetScratchpad(env.clone->mem.scratchpad().data());
    return g;
}

bool Refuse(NativeEnv& env, const char* what) {
    if (env.failure.empty()) env.failure = what;
    return false;
}

// The list migration's callees: the two PORTED crash functions on raw views of the clone, and the
// rider layer from the original code at the depth the original calls it.
struct MigrateCallees final : rr::sim::BikeListMigrateCallees {
    NativeEnv& env;
    const rr::sim::BikeTables& t;
    MigrateCallees(NativeEnv& e, const rr::sim::BikeTables& tt) : env(e), t(tt) {}
    bool CrashLaunch(uint32_t e) override {
        Memory& m = env.clone->mem;
        uint8_t* ep = RawAt(m, e, 1096);
        if (ep == nullptr) return Refuse(env, "BikeCrashLaunch: the entity is not in RAM");
        if (!rr::sim::BikeCrashLaunch(rr::sim::EntityView(ep), RawLinks(m, e), t))
            return Refuse(env, "BikeCrashLaunch refused");
        return true;
    }
    bool WipeoutStart(uint32_t e) override {
        Memory& m = env.clone->mem;
        uint8_t* ep = RawAt(m, e, 1096);
        if (ep == nullptr) return Refuse(env, "BikeWipeoutStart: the entity is not in RAM");
        if (!rr::sim::BikeWipeoutStart(rr::sim::EntityView(ep), RawLinks(m, e)))
            return Refuse(env, "BikeWipeoutStart refused");
        return true;
    }
    bool RiderKnockOff(uint32_t r, uint32_t sp) override {
        ExactOracle(env, kRiderKnockOff, &r, 1, sp);
        return true; // a trap inside the callee is env.failure, which fails the case loudly
    }
    bool RiderLaunch(uint32_t r, uint32_t sp) override {
        ExactOracle(env, kRiderLaunch, &r, 1, sp);
        return true;
    }
};

// The declarations of the rider layer's two entries, observed on both sides at any depth.
OracleCallee KnockOffCallee() { return OracleCallee{kRiderKnockOff, 1}; }
OracleCallee LaunchCallee() { return OracleCallee{kRiderLaunch, 1}; }

struct StepCallees final : rr::sim::BikeStepCallees {
    NativeEnv& env;
    explicit StepCallees(NativeEnv& e) : env(e) {}
    // Region A's list migration and region D's passenger launch are PORTED; both
    // run natively on the clone's RAM, from the step's own frame (env.sp - 136).
    bool ListMigrate(uint32_t e, int32_t dt) override {
        (void)dt; // dead in the original
        const StepTables tab(env.clone->mem, env.gp);
        MigrateCallees mc(env, tab.t);
        rr::sim::GuestRam g = StepView(env);
        if (!rr::sim::BikeListMigrate(g, e, env.sp - kStepFrame, tab.t, mc)) {
            char buf[96];
            std::snprintf(buf, sizeof(buf), "BikeListMigrate refused (view fault 0x%08X)", g.FaultAddress());
            return Refuse(env, buf);
        }
        return true;
    }
    bool PassengerLaunch(uint32_t e) override {
        const StepTables tab(env.clone->mem, env.gp);
        MigrateCallees mc(env, tab.t);
        rr::sim::GuestRam g = StepView(env);
        if (!rr::sim::BikePassengerLaunch(g, e, env.sp - kStepFrame, mc)) {
            char buf[96];
            std::snprintf(buf, sizeof(buf), "BikePassengerLaunch refused (view fault 0x%08X)", g.FaultAddress());
            return Refuse(env, buf);
        }
        return true;
    }
    bool CrashTimer(uint32_t e, int32_t dt) override {
        Memory& m = env.clone->mem;
        std::vector<uint8_t> table(4096);
        m.ReadBlock(kCrashTable, table.data(), table.size());
        struct Emitter final : rr::sim::CrashEmitter {
            NativeEnv& env;
            uint32_t e;
            Emitter(NativeEnv& v, uint32_t a) : env(v), e(a) {}
            void Emit(int32_t which, int32_t kind) override {
                env.CallOracle(kCrashEmit, e, static_cast<uint32_t>(which), static_cast<uint32_t>(kind), 0);
            }
        } emitter(env, e);
        // The PORTED timer, on the clone's own RAM: the entity is a view of guest memory, so what
        // the oracle-supplied emitter changes is what the timer sees next, with no copy between.
        rr::sim::EntityView view(m.ram().data() + (e & 0x1FFFFFu));
        rr::sim::BikeCrashTimer(view, dt, static_cast<int32_t>(m.PeekWord(kCrashWindow)), table.data(), emitter);
        return true;
    }
    bool StanceEvent(int32_t ev, uint32_t rider, int32_t p) override {
        // PORTED (src\game\sim\stance.h, rows_anim.inc), run on the clone; the call stays declared as
        // a witness at the step's frame. A refusal is env.failure.
        AnimNativeStanceEvent(env, static_cast<uint32_t>(ev), rider, static_cast<uint32_t>(p), env.sp - kStepFrame);
        return true;
    }
    // Region B's two PORTED callees, run on views of the clone's own RAM. The list is walked first
    // (neither callee rewrites a node, bike.h), each bike's pointer chases resolved into the same
    // RAM, so a record two bikes share is one record here too.
    uint8_t* At(uint32_t a) { return env.clone->mem.ram().data() + (a & 0x1FFFFFu); }
    std::vector<uint32_t> Walk(uint32_t list) {
        Memory& m = env.clone->mem;
        std::vector<uint32_t> out;
        uint32_t node = m.PeekWord(list + 4u);
        while (node != list && out.size() < 64) {
            out.push_back(node - 1088u);
            node = m.PeekWord(node + 4u);
        }
        return out;
    }
    struct TrigTables {
        std::vector<int16_t> sincos;
        int32_t atan[20]{};
    };
    TrigTables Trig() {
        Memory& m = env.clone->mem;
        TrigTables t;
        t.sincos.assign(8192, 0);
        m.ReadBlock(kSinCosTable, t.sincos.data(), t.sincos.size() * 2u);
        for (uint32_t i = 0; i < 20; ++i) t.atan[i] = GetS32(m, kAtanTable + 4u * i);
        return t;
    }
    bool SteerDriver(uint32_t list, int32_t dt) override {
        Memory& m = env.clone->mem;
        const std::vector<uint32_t> bikes = Walk(list);
        const TrigTables t = Trig();
        const uint32_t gs = m.PeekWord(kGameStatePtr);
        std::vector<rr::sim::EntityView> riders;
        riders.reserve(bikes.size());
        std::vector<rr::sim::BikeSteerNode> nodes;
        for (uint32_t ea : bikes) {
            const uint32_t ra = m.PeekWord(ea + rr::sim::ent::kRider);
            riders.emplace_back(ra != 0u ? At(ra) : nullptr);
            const uint32_t owner = m.PeekWord(ea + rr::sim::ent::kOwner);
            nodes.push_back(rr::sim::BikeSteerNode{rr::sim::EntityView(At(ea)), ra != 0u ? &riders.back() : nullptr,
                                                   At(m.PeekWord(ea + rr::sim::ent::kStats)), At(kHandleTable),
                                                   m.PeekByte(owner + 0x23Cu),
                                                   static_cast<int32_t>(m.PeekWord(owner + 0x25Cu))});
        }
        rr::sim::BikeSteerDriver(nodes.data(), nodes.size(), dt, static_cast<int32_t>(m.PeekWord(gs + 0x30)),
                                 static_cast<int32_t>(m.PeekWord(gs + 0x34)), At(kAiTable), t.atan,
                                 t.sincos.data());
        return true;
    }
    bool SteerPass(uint32_t list, int32_t dt) override {
        Memory& m = env.clone->mem;
        const std::vector<uint32_t> bikes = Walk(list);
        const TrigTables t = Trig();
        std::vector<rr::sim::BikeSteerPassNode> nodes;
        for (uint32_t ea : bikes) {
            rr::sim::BikeSteerPassNode n{rr::sim::EntityView(At(ea))};
            n.ownerFlagByte = m.PeekByte(m.PeekWord(ea + rr::sim::ent::kOwner) + 0x23Cu);
            const uint32_t ra = m.PeekWord(ea + rr::sim::ent::kRider);
            n.passenger = ra != 0u ? At(ra) : nullptr;
            n.stats = At(m.PeekWord(ea + rr::sim::ent::kStats));
            n.riderDefByte0 = m.PeekByte(m.PeekWord(ea + rr::sim::ent::kRiderDef));
            nodes.push_back(n);
        }
        rr::sim::BikeSteerPassEnv e;
        e.numPlayers = m.PeekWord(m.PeekWord(kGameStatePtr) + 0x30);
        e.handleTable = At(kHandleTable);
        e.handleTableBytes = 0x200000u - (kHandleTable & 0x1FFFFFu); // the RAM above it: unbounded
        e.atan = t.atan;
        e.sincos = t.sincos.data();
        return rr::sim::BikeSteerPass(nodes.data(), nodes.size(), dt, e);
    }
};

// ---------------------------------------------------------------- the tail's callees (G..J)
// The six unread functions under region H's road passes, executed by the oracle
// at the port's own stack pointer - rows_road_runtime.inc's RrSeams, restated here because that file
// is included after this one.
class StepRoadSeams final : public rr::sim::RoadRuntimeCallees {
public:
    explicit StepRoadSeams(NativeEnv& env) : env_(env) {}
    uint32_t RoadClassCore(rr::sim::GuestRam&, uint32_t e, uint32_t node, uint32_t out, uint32_t cursorOut,
                           int32_t zone, uint32_t sp) override {
        const uint32_t a[5] = {e, node, out, cursorOut, static_cast<uint32_t>(zone)};
        return ExactOracle(env_, 0x8003E754u, a, 5, sp);
    }
    uint32_t NodeWedge(rr::sim::GuestRam&, uint32_t point, uint32_t node, uint32_t armA, uint32_t armB,
                       int32_t hint, uint32_t sp) override {
        const uint32_t a[5] = {point, node, armA, armB, static_cast<uint32_t>(hint)};
        return ExactOracle(env_, 0x8003EB58u, a, 5, sp);
    }
    uint32_t RoadsideRunNode(rr::sim::GuestRam&, uint32_t e, int32_t zone, uint32_t sp) override {
        const uint32_t a[2] = {e, static_cast<uint32_t>(zone)};
        return ExactOracle(env_, 0x8003F204u, a, 2, sp);
    }
    uint32_t RouteBindFirst(rr::sim::GuestRam&, uint32_t p, uint32_t e, uint32_t pool, uint32_t x,
                            uint32_t sp) override {
        const uint32_t a[4] = {p, e, pool, x};
        return ExactOracle(env_, 0x8003B024u, a, 4, sp);
    }
    uint32_t RouteBindStep(rr::sim::GuestRam&, uint32_t p, uint32_t pool, uint32_t e, uint32_t x,
                           uint32_t sp) override {
        const uint32_t a[4] = {p, pool, e, x};
        return ExactOracle(env_, 0x8003B1C4u, a, 4, sp);
    }
    uint32_t RoadShortcut(rr::sim::GuestRam&, uint32_t e, uint32_t sp) override {
        return ExactOracle(env_, 0x8003BFE8u, &e, 1, sp);
    }

private:
    NativeEnv& env_;
};
std::vector<OracleCallee> RoadSeamCallees() {
    return {OracleCallee{0x8003E754u, 4, 1}, OracleCallee{0x8003EB58u, 4, 1}, OracleCallee{0x8003F204u, 2},
            OracleCallee{0x8003B024u, 4},    OracleCallee{0x8003B1C4u, 4},    OracleCallee{0x8003BFE8u, 1}};
}

// The four-byte slot the integrator appends through: the scratchpad (where the game keeps the list)
// or RAM, or null.
uint8_t* ListSlotAt(Memory& m, uint32_t cursor) {
    if (cursor >= 0x1F800000u && cursor <= 0x1F800400u - 4u)
        return m.scratchpad().data() + (cursor - 0x1F800000u);
    return RawAt(m, cursor, 4);
}

struct TailCallees final : rr::sim::BikeStepTailCallees {
    NativeEnv& env;
    const StepTables& tab;
    StepRoadSeams road;
    TailCallees(NativeEnv& e, const StepTables& t) : env(e), tab(t), road(e) {}

    // RASHCDG 0x8007F0BC, ported; the append goes through the cursor the caller resolves.
    bool Integrate(uint32_t e, int32_t dt) override {
        Memory& m = env.clone->mem;
        uint8_t* ep = RawAt(m, e, 1096);
        if (ep == nullptr) return Refuse(env, "BikeIntegrate: the entity is not in RAM");
        rr::sim::BikeActiveList list;
        list.cursor = m.PeekWord(kActiveCursor);
        list.slot = ListSlotAt(m, list.cursor);
        if (list.slot == nullptr) return Refuse(env, "BikeIntegrate: the list cursor is not in memory");
        if (!rr::sim::BikeIntegrate(rr::sim::EntityView(ep), dt, e, RawLinks(m, e), tab.t, list)) {
            char buf[160];
            std::snprintf(buf, sizeof(buf), "BikeIntegrate refused (e 0x%08X flagsB 0x%08X flagsC 0x%08X "
                          "+0x340 0x%08X +0x358 0x%08X)", e, m.PeekWord(e + 0x234), m.PeekWord(e + 0x238),
                          m.PeekWord(e + 0x340), m.PeekWord(e + 0x358));
            return Refuse(env, buf);
        }
        if (list.pushed) m.PokeWord(kActiveCursor, list.cursor);
        return true;
    }

    // RASHCDG 0x8007504C, ported; its ground query 0x800A7BF8 ported too (ground.h), at the depth
    // the original calls it from and into the ORIGINAL's own frame slots (sp - 120 + 24/40/72),
    // which the port's frame struct starts from as they are - uninitialised in the original too.
    bool GroundFrame(uint32_t ea, uint32_t refAddr, uint32_t sp) override {
        Memory& m = env.clone->mem;
        uint8_t* ep = RawAt(m, ea, 1096);
        if (ep == nullptr) return Refuse(env, "BikeGroundFrame: the entity is not in RAM");
        rr::sim::EntityView ev(ep);
        const uint32_t riderAddr = ev.U32(rr::sim::ent::kRider);
        uint8_t* rider = riderAddr != 0 ? RawAt(m, riderAddr, 1096) : nullptr;
        if (riderAddr != 0 && rider == nullptr) return Refuse(env, "BikeGroundFrame: the rider is not in RAM");
        const uint32_t gsp = sp - kGroundFrameSz;
        const uint32_t bufA = gsp + kGroundSlotA, bufB = gsp + kGroundSlotB, bufC = gsp + kGroundSlotC;
        rr::sim::BikeGroundFrameSlots slots;
        for (uint32_t k = 0; k < 3; ++k) {
            slots.pointA[k] = GetS32(m, bufA + 4u * k);
            slots.pointB[k] = GetS32(m, bufB + 4u * k);
            slots.normalC[k] = GetS16(m, bufC + 2u * k);
        }
        rr::sim::BikeGroundEnv ge;
        ge.frame = &slots;
        auto slice = [&m](uint32_t addr) {
            rr::sim::BikeGroundSlice s;
            s.address = addr;
            if (addr >= 0x80000000u && addr < 0x80200000u) {
                for (uint32_t k = 0; k < 3; ++k) {
                    s.row[k] = GetS16(m, addr + 8u + 2u * k);
                    s.origin[k] = GetS32(m, addr + 20u + 4u * k);
                }
            }
            return s;
        };
        auto refresh = [&]() {
            ge.tracked = slice(ev.U32(0x154));
            ge.ground = slice(ev.U32(0x100));
            ge.listNode = ev.U32(0x440);
            const uint32_t rdAddr = ev.U32(rr::sim::ent::kRiderDef);
            ge.riderDefPlace = (rdAddr >= 0x80000000u) ? m.PeekByte(rdAddr + 0x27u) : 0u;
            ge.rider = rider;
            ge.riderTracked = rider != nullptr ? slice(rr::sim::EntityView(rider).U32(0x154))
                                               : rr::sim::BikeGroundSlice{};
        };
        struct GroundQ final : rr::sim::BikeGroundQuery {
            NativeEnv& env;
            uint32_t ea, riderAddr, bufA, bufB, bufC, mainRef, gsp;
            const uint16_t* rsqrt = nullptr;
            std::function<void()> refresh;
            GroundQ(NativeEnv& v, uint32_t a, uint32_t r, uint32_t p, uint32_t q, uint32_t s, uint32_t mr, uint32_t f)
                : env(v), ea(a), riderAddr(r), bufA(p), bufB(q), bufC(s), mainRef(mr), gsp(f) {}
            int32_t Query_(rr::sim::BikeGroundSite site, const int32_t* refPoint, int32_t outPoint[3],
                           int16_t outNormal[3], int32_t prevId) {
                Memory& mem = env.clone->mem;
                const bool third = (site == rr::sim::BikeGroundSite::kRider);
                const uint32_t pointAddr = (site == rr::sim::BikeGroundSite::kAlt) ? bufB : bufA;
                const uint32_t normAddr = third ? bufC : (ea + 0x10Cu);
                for (uint32_t k = 0; k < 3; ++k) mem.PokeWord(pointAddr + 4u * k, static_cast<uint32_t>(outPoint[k]));
                for (uint32_t k = 0; k < 3; ++k) PutS16(mem, normAddr + 2u * k, outNormal[k]);
                uint32_t refAddr = 0;
                if (site == rr::sim::BikeGroundSite::kMain) refAddr = mainRef;
                else if (site == rr::sim::BikeGroundSite::kAlt) refAddr = ea + 0x0F4u;
                // the reference point is a pointer in the original and a value list in the port:
                // the bench checks the port computed the three words that live at that address
                if ((refAddr == 0) != (refPoint == nullptr)) {
                    if (env.failure.empty()) env.failure = "the port asked for the wrong reference point";
                } else if (refPoint != nullptr) {
                    for (uint32_t k = 0; k < 3; ++k)
                        if (GetS32(mem, refAddr + 4u * k) != refPoint[k] && env.failure.empty())
                            env.failure = "the port's reference point is not the one the original passes";
                }
                const uint32_t a[5] = {third ? riderAddr : ea, refAddr, pointAddr, normAddr,
                                       static_cast<uint32_t>(prevId)};
                // RASHCDG 0x800A7BF8 is PORTED (ground.h, row `ground_query`):
                // run natively on the clone's own memory, with exactly the five arguments the
                // original's call site passes - its output buffers are the original ground frame's
                // own frame slots, at the same addresses.
                rr::sim::GuestRam gv(mem.ram().data(), env.gp);
                const rr::sim::GroundResult gr = rr::sim::GroundQuery(gv, a[0], a[1], a[2], a[3], a[4], rsqrt);
                if ((gr.declined || gv.Faulted()) && env.failure.empty())
                    env.failure = "the ported ground query declined or faulted";
                const int32_t r = static_cast<int32_t>(gr.value);
                (void)gsp;
                for (uint32_t k = 0; k < 3; ++k) outPoint[k] = static_cast<int32_t>(mem.PeekWord(pointAddr + 4u * k));
                for (uint32_t k = 0; k < 3; ++k) outNormal[k] = GetS16(mem, normAddr + 2u * k);
                refresh();
                return r;
            }
            int32_t Query(rr::sim::BikeGroundSite site, const int32_t* refPoint, int32_t outPoint[3],
                          int16_t outNormal[3], int32_t prevId) override {
                return Query_(site, refPoint, outPoint, outNormal, prevId);
            }
        } q(env, ea, riderAddr, bufA, bufB, bufC, refAddr, gsp);
        q.refresh = refresh;
        q.rsqrt = tab.rsqrt.data();
        ge.query = &q;
        refresh();
        int32_t ref[3];
        for (uint32_t k = 0; k < 3; ++k) ref[k] = GetS32(m, refAddr + 4u * k);
        rr::sim::BikeGroundFrame(ev, ref, ge);
        return env.failure.empty();
    }

    // RASHCDG 0x8007FA4C, ported; its road re-bind SLUS 0x800374D4 is ported too (road_runtime.h),
    // called at the contact frame's own sp, with the six unread functions under it on the seam.
    bool ContactFrame(uint32_t ea, uint32_t sp) override {
        Memory& m = env.clone->mem;
        uint8_t* ep = RawAt(m, ea, 1096);
        if (ep == nullptr) return Refuse(env, "BikeContactFrame: the entity is not in RAM");
        struct RebindImpl final : rr::sim::BikeRoadRebind {
            NativeEnv& env;
            StepRoadSeams& road;
            uint32_t ea, sp;
            bool ok = true;
            RebindImpl(NativeEnv& v, StepRoadSeams& r, uint32_t a, uint32_t s) : env(v), road(r), ea(a), sp(s) {}
            void Rebind_(int32_t flag) {
                rr::sim::GuestRam g = StepView(env);
                rr::sim::RoadRebind(g, ea, flag, sp, road);
                if (g.Faulted()) ok = false;
            }
            void Rebind(int32_t flag) override { Rebind_(flag); }
        } rebind(env, road, ea, sp - kContactFrameSz);
        if (!rr::sim::BikeContactFrame(rr::sim::EntityView(ep), RawLinks(m, ea), tab.t, rebind))
            return Refuse(env, "BikeContactFrame refused");
        if (!rebind.ok) return Refuse(env, "RoadRebind: the view faulted");
        return env.failure.empty();
    }

    rr::sim::RoadRuntimeCallees& RoadSeams() override { return road; }

    // RASHCDG 0x800807F0, ported, on raw views (a passenger that is another pool bike, or the bike
    // itself, is the same memory as on the console).
    bool RiderPose(uint32_t ea, int32_t dt) override {
        Memory& m = env.clone->mem;
        uint8_t* ep = RawAt(m, ea, 1096);
        if (ep == nullptr) return Refuse(env, "BikeRiderPose: the entity is not in RAM");
        rr::sim::EntityView bike(ep);
        const uint32_t riderAddr = bike.U32(rr::sim::ent::kRider);
        uint8_t* rp = riderAddr != 0 ? RawAt(m, riderAddr, 1096) : nullptr;
        if (riderAddr != 0 && rp == nullptr) return Refuse(env, "BikeRiderPose: the rider is not in RAM");
        rr::sim::EntityView rider(rp);
        auto ownerRev = [&m](rr::sim::EntityView& v) {
            const uint32_t o = v.U32(rr::sim::ent::kOwner);
            return (o != 0) ? static_cast<int32_t>(m.PeekWord(o + 604)) : 0;
        };
        const int32_t bikeRev = ownerRev(bike);
        const int32_t riderRev = rp != nullptr ? ownerRev(rider) : 0;
        rr::sim::BikeRiderPose(bike, dt, bikeRev, rp != nullptr ? &rider : nullptr, riderRev, tab.t.atan,
                               tab.t.sincos);
        return true;
    }

    // PORTED (population.h, rows_population.inc): run natively on the clone.
    bool ActivationPass(uint32_t sp) override { return PopActivationPassNative(env, sp, road); }
    bool DownedRiderPass(uint32_t sp) override { return PopDownedPassNative(env, sp, road); }

    // RASHCDG 0x80092C7C, ported; its three callees from the original code, at its own frame's sp.
    bool EndRace(uint32_t b, int32_t reason, uint32_t sp) override {
        Memory& m = env.clone->mem;
        const uint32_t csp = sp - kEndRaceFrame;
        const uint32_t handle = static_cast<uint32_t>(m.PeekByte(b + 0xACu) | (m.PeekByte(b + 0xADu) << 8));
        const uint32_t view = kViewArray + kViewStride * handle;
        rr::sim::EndRaceEnv ee;
        ee.entity = RawAt(m, b, 1096);
        ee.riderDef = RawAt(m, m.PeekWord(b + rr::sim::ent::kRiderDef), 72);
        ee.owner = RawAt(m, m.PeekWord(b + rr::sim::ent::kOwner), kRiderStride);
        ee.view = RawAt(m, view, 0x310);
        ee.gameState = RawAt(m, m.PeekWord(kGameStatePtr), 64);
        ee.postDelay = reinterpret_cast<int32_t*>(RawAt(m, kCountdownWord, 4));
        struct Calls final : rr::sim::EndRaceCalls {
            NativeEnv& env;
            uint32_t b, view, csp;
            Calls(NativeEnv& e, uint32_t bb, uint32_t v, uint32_t s) : env(e), b(bb), view(v), csp(s) {}
            void StampResult() override { ExactOracle(env, kResultStamp, &b, 1, csp); }
            void WipeoutEffect() override { ExactOracle(env, kWipeoutEffect, &b, 1, csp); }
            void ViewEvent(int32_t reason) override {
                const uint32_t a[2] = {view, static_cast<uint32_t>(reason)};
                ExactOracle(env, kViewEvent, a, 2, csp);
            }
        } calls(env, b, view, csp);
        ee.calls = &calls;
        if (!rr::sim::EndRace(reason, ee)) return Refuse(env, "EndRace refused");
        return env.failure.empty();
    }
};

// ---------------------------------------------------------------- the step's input family
//
// Every splice case starts from THIS machine (rr-race) as it is - the real pool 0, its real
// riders, road cursors, lists and globals - because everything outside the region runs as the
// original's own code and would trap on pointers lifted out of another image. On top of that:
//
//   * the RIDING LIST is rebuilt: slot 0 (the player) plus 0..5 more pool-0 slots, drawn, in a
//     drawn order; every other slot stays on the list it was on. rr-race has only the player on it,
//     so without this the walks see one bike. A slot moved there keeps its own +0x140 (0 for the
//     dormant AI), which the census never saw - named; regions C..F read neither +0x140 nor the
//     membership rule, and G / I skip such a bike exactly as the original does;
//   * per riding bike, the scalars the focused region reads are drawn (randomised family) or copied
//     from one of the 324 captured bikes of the 18 RAM images (dump-derived family), pointers never;
//   * `dt` is 218 * (1..8) ticks, the frame's own unit, and sometimes any 16-bit value.
constexpr uint32_t kListHeads[5] = {0x8005B270, 0x8005B298, 0x8005B2D8, 0x8005B350, 0x8005B378};

struct StepCase {
    uint32_t pool = 0, slots = 0;
    std::vector<uint32_t> riding; // entity addresses, in list order
};

StepCase PlantRidingList(CaseContext& c, uint32_t thrownMax = 0) {
    Memory& m = *c.mem;
    Rng& rng = *c.rng;
    StepCase sc;
    sc.pool = m.PeekWord(kPoolTable);
    const uint32_t hp = m.PeekWord(kPoolTable + 12u);
    sc.slots = (hp >= 0x80000000u) ? m.PeekWord(hp) + 1u : 1u;
    if (sc.slots > 64u) sc.slots = 1u;
    // which list each slot is on now (walk every head)
    std::vector<int> on(sc.slots, -1);
    for (int h = 0; h < 5; ++h) {
        uint32_t n = m.PeekWord(kListHeads[h] + 4u);
        for (int guard = 0; n != kListHeads[h] && guard < 64; ++guard) {
            const uint32_t e = n - 1088u;
            if (e >= sc.pool && (e - sc.pool) % 1096u == 0u && (e - sc.pool) / 1096u < sc.slots)
                on[(e - sc.pool) / 1096u] = h;
            n = m.PeekWord(n + 4u);
        }
    }
    std::vector<uint32_t> pick{0u};
    const uint32_t extra = rng.U32() % 6u;
    for (uint32_t k = 0; k < extra; ++k) {
        const uint32_t s = 1u + rng.U32() % (sc.slots - 1u);
        if (std::find(pick.begin(), pick.end(), s) == pick.end()) pick.push_back(s);
    }
    for (size_t k = pick.size(); k > 1; --k) std::swap(pick[k - 1], pick[rng.U32() % k]);
    for (uint32_t s : pick) on[s] = 1;
    // optionally a few more slots on the 0x400 list 0x8005B2D8 (region B steers it too)
    if (thrownMax != 0) {
        const uint32_t n = rng.U32() % (thrownMax + 1u);
        for (uint32_t k = 0; k < n; ++k) {
            const uint32_t s = 1u + rng.U32() % (sc.slots - 1u);
            if (on[s] != 1) on[s] = 2;
        }
    }
    // relink all five lists from scratch: the riding list in the drawn order, the rest by slot
    for (int h = 0; h < 5; ++h) {
        std::vector<uint32_t> ents;
        if (h == 1) {
            for (uint32_t s : pick) ents.push_back(sc.pool + 1096u * s);
        } else {
            for (uint32_t s = 0; s < sc.slots; ++s)
                if (on[s] == h) ents.push_back(sc.pool + 1096u * s);
        }
        LinkList(m, kListHeads[h], ents.data(), static_cast<uint32_t>(ents.size()));
    }
    for (uint32_t s : pick) sc.riding.push_back(sc.pool + 1096u * s);
    return sc;
}

int32_t DrawDt(Rng& rng) {
    // dt is always positive in the game; 0 and negative values are drawn too, because the step's
    // sign-split divides by it (region D's chase, 0x80076974..0x800769BC) and so has arms for them.
    if (rng.Next() % 16u == 0) return -static_cast<int32_t>(rng.U32() % 4000u);
    if (rng.Next() % 6u == 0) return static_cast<int32_t>(rng.U32() & 0xFFFFu);
    return 218 * static_cast<int32_t>(1u + rng.U32() % 8u);
}

// The fields a region reads, per bike: drawn (randomised) or copied from a captured bike
// (dump-derived). `shift` sets the magnitude of a drawn 32-bit field.
struct Field {
    uint32_t off;
    int bytes;  // 4, 2 or 1
    int shift;  // for 4-byte fields
    uint32_t zeroOdds;
};

void DrawFields(CaseContext& c, uint32_t e, const Field* f, size_t n) {
    Memory& m = *c.mem;
    Rng& rng = *c.rng;
    const bool derived = c.sample != nullptr && c.sample->entity.size() == 1096;
    for (size_t i = 0; i < n; ++i) {
        uint32_t v = rng.U32();
        if (f[i].bytes == 4) v = static_cast<uint32_t>(static_cast<int32_t>(v) >> f[i].shift);
        if (derived && rng.Next() % 4u != 0u) {
            v = 0;
            for (int k = 0; k < f[i].bytes; ++k) v |= static_cast<uint32_t>(c.sample->entity[f[i].off + static_cast<uint32_t>(k)]) << (8 * k);
        }
        if (f[i].zeroOdds != 0 && rng.Next() % f[i].zeroOdds == 0u) v = 0;
        if (f[i].bytes == 4) m.PokeWord(e + f[i].off, v);
        else if (f[i].bytes == 2) WriteU16Bench(m, e + f[i].off, static_cast<uint16_t>(v));
        else { const uint8_t b = static_cast<uint8_t>(v); m.WriteBlock(e + f[i].off, &b, 1); }
    }
}

// A random stat block in the scratch block, for a bike that should not read the real ones.
uint32_t PlantStats(CaseContext& c, uint32_t at) {
    std::vector<uint8_t> s(448);
    for (uint8_t& b : s) b = static_cast<uint8_t>(c.rng->U32());
    // magnitudes of the fields the walks read: 16.16 values around the real blocks' range
    for (uint32_t off = 0; off < 448; off += 4) PutU32V(s, off, static_cast<uint32_t>(c.rng->S32() >> (8 + c.rng->U32() % 12u)));
    c.mem->WriteBlock(at, s.data(), s.size());
    return at;
}

// ---------------------------------------------------------------- region C
void SetupStepC(CaseContext& c, Args& a) {
    Memory& m = *c.mem;
    Rng& rng = *c.rng;
    auto chance = [&rng](uint32_t n) { return rng.Next() % n == 0; };
    const StepCase sc = PlantRidingList(c);
    const uint32_t statsAt = c.scratch + 64u;
    PlantStats(c, statsAt);
    static const Field kC[] = {
        {0x2A4, 4, 12, 3}, {0x2CC, 4, 0, 3}, {0x16C, 4, 0, 4}, {0x268, 4, 12, 2}, {0x26C, 4, 12, 2},
        {0x270, 4, 12, 2}, {0x254, 4, 12, 3}, {0x240, 4, 8, 4},  {0x244, 4, 8, 3},  {0x2F8, 4, 10, 3},
        {0x2E0, 4, 10, 3}, {0x212, 2, 0, 4},  {0x216, 1, 0, 4},  {0x144, 4, 4, 4},
    };
    for (uint32_t e : sc.riding) {
        DrawFields(c, e, kC, sizeof(kC) / sizeof(kC[0]));
        // the flag words: the bits region C tests, drawn over the captured / drawn background
        uint32_t fa = m.PeekWord(e + 0x230), fb = m.PeekWord(e + 0x234), fc = m.PeekWord(e + 0x238);
        auto bits = [&](uint32_t& w, uint32_t mask, uint32_t odds) {
            w = chance(odds) ? (w | (mask & rng.U32())) : (w & ~mask);
        };
        bits(fa, 0x08000000u, 2);  // AI
        bits(fa, 0x300u, 2);       // steering
        bits(fa, 0x20000000u, 4);
        bits(fa, 0x60u, 2);        // brake bits
        bits(fb, 0x4u, 3);
        bits(fb, 0x400u, 4);
        bits(fb, 0x80u, 3);
        bits(fc, 0x7FFu, 3);
        // flagsA bit 10 (region D's passenger launch 0x80074D58) and flagsC bits 9/10 (a crash in
        // progress, the integrator's crash arms in G) are drawn too: region C reads neither, but
        // they send the rest of the step - original code on both sides - down its other arms.
        // Measured: no guest trap over the whole family.
        if (chance(2)) fa |= 0x400u;
        if (chance(3)) fc |= rng.U32() & 0x600u;
        m.PokeWord(e + 0x230, fa);
        m.PokeWord(e + 0x234, fb);
        m.PokeWord(e + 0x238, fc);
        // the passenger: none, or another live pool-0 bike (never a pointer lifted from elsewhere)
        m.PokeWord(e + 0x358, chance(2) ? 0u : sc.pool + 1096u * (rng.U32() % sc.slots));
        // the stat block: this machine's real ones or a drawn one
        if (chance(3)) m.PokeWord(e + 0x22C, statsAt);
        // the rider's mount state +0x25C, which the rider-off trigger tests `>= 2` unsigned
        const uint32_t rider = m.PeekWord(e + 0x354);
        if (rider >= 0x80000000u && rider < 0x80200000u - 0x260u && chance(2))
            m.PokeWord(rider + 0x25C, chance(3) ? rng.U32() : rng.U32() % 5u);
        // the lean against the loop-out limit exactly, and the handle as a player's
        if (chance(8)) m.PokeWord(e + 0x2A4, m.PeekWord(m.PeekWord(e + 0x22C) + 0xCC));
        if (chance(6)) WriteU16Bench(m, e + 0xAC, 0);
    }
    // the trailing-player option byte, and player 1's bike as some other bike
    {
        const uint8_t opt = chance(2) ? 0 : static_cast<uint8_t>(rng.U32() | 1u);
        m.WriteBlock(rr::sim::kOptionByte, &opt, 1);
    }
    a.a[0] = static_cast<uint32_t>(DrawDt(rng));
}

bool RegionC(NativeEnv& env, rr::sim::GuestRam& g, int32_t dt) {
    (void)env;
    (void)dt;
    return rr::sim::BikeStepRegionC(g);
}

const Splice kSpliceC{"bike_step_c", 0x80076208, 0x80076804, {}, RegionC};

uint32_t NativeStepC(NativeEnv& env, const Args& a) { return RunSplice(env, a, kSpliceC); }

// ---------------------------------------------------------------- region F
void SetupStepF(CaseContext& c, Args& a) {
    Memory& m = *c.mem;
    Rng& rng = *c.rng;
    auto chance = [&rng](uint32_t n) { return rng.Next() % n == 0; };
    const StepCase sc = PlantRidingList(c);
    const uint32_t statsAt = c.scratch + 64u;
    PlantStats(c, statsAt);
    static const Field kF[] = {
        {0x240, 4, 8, 6},  {0x2A4, 4, 12, 3}, {0x27C, 4, 12, 4}, {0x28C, 4, 12, 4}, {0x2AC, 4, 12, 3},
        {0x2A8, 4, 12, 3}, {0x2CC, 4, 12, 3}, {0x1E8, 4, 12, 3}, {0x290, 4, 12, 3}, {0x2D0, 4, 14, 3},
        {0x294, 4, 12, 2}, {0x2D4, 4, 14, 3}, {0x298, 4, 12, 3}, {0x284, 4, 12, 3},
        // Region F's flagsB bits 2, 3 and 4 are what region E2 LEAVES (wheelspin on a throttle
        // friction ratio +0x2BC over 1.0, front lock on a brake ratio +0x2B8 over 1.0, bit 4 on the
        // edge), so the family drives E0/E2's inputs: the throttle and brake amounts and
        // capacities, and a small grip gain +0x2E0, which makes both ratios large.
        {0x24C, 4, 10, 4}, {0x254, 4, 10, 4}, {0x250, 4, 10, 4}, {0x258, 4, 10, 4}, {0x2E0, 4, 16, 6},
    };
    for (uint32_t e : sc.riding) {
        DrawFields(c, e, kF, sizeof(kF) / sizeof(kF[0]));
        uint32_t fa = m.PeekWord(e + 0x230), fb = m.PeekWord(e + 0x234);
        auto bits = [&](uint32_t& w, uint32_t mask, uint32_t odds) {
            w = chance(odds) ? (w | (mask & rng.U32())) : (w & ~mask);
        };
        bits(fa, 0x08000000u, 2);
        bits(fa, 0x340u, 2);
        bits(fa, 0x63u, 2);       // throttle and brake bits, for E2
        bits(fb, 0x400u, 5);
        bits(fb, 0x8u, 2);        // wheelspin
        bits(fb, 0x10u, 2);       // the skid edge
        bits(fb, 0x6u, 2);        // front lock / the brake-edge bit
        bits(fb, 0x01000000u, 3); // arm the steering ramp
        bits(fb, 0x80000u, 2);
        if (chance(2)) fa |= 0x400u; // as for region C
        // Two scenarios E2 carries over unchanged, so that F sees them WITHOUT the edge bit 4:
        // a front lock that continues (human, brake held, steering, bit 2) and a wheelspin that
        // continues (throttle only, bit 3) - each with a tiny grip gain so E0's ratio stays > 1.0.
        const uint32_t scenario = rng.U32() % 4u;
        if (scenario == 1u) {
            fa = (fa & ~(0x08000000u | 0x2u)) | 0x40u | ((rng.U32() & 1u) ? 0x100u : 0x200u);
            fb = (fb & ~0x1Au) | 0x4u;
            m.PokeWord(e + 0x2E0, rng.U32() % 64u);
            m.PokeWord(e + 0x254, 1u + rng.U32() % 0x1000u);
            m.PokeWord(e + 0x258, 1u + rng.U32() % 0x1000u);
            m.PokeWord(e + 0x240, 0x100u + rng.U32() % 0x80000u);
        } else if (scenario == 2u) {
            // ... and a steer past stats[+0xF4] on the lean's side; flagsA bit 19 clear so that
            // region B's steering does not rewrite +0x27C first
            fa = (fa & ~(0x60u | 0x80000u)) | 0x2u;
            fb = (fb & ~0x14u) | 0x8u;
            m.PokeWord(e + 0x2E0, rng.U32() % 64u);
            m.PokeWord(e + 0x24C, 0x10000u + rng.U32() % 0x100000u);
            m.PokeWord(e + 0x250, 0x10000u + rng.U32() % 0x100000u);
            const int32_t lim = static_cast<int32_t>(m.PeekWord(m.PeekWord(e + 0x22C) + 0xF4));
            const int32_t lean = static_cast<int32_t>(m.PeekWord(e + 0x2A4));
            const int32_t steer = lim + static_cast<int32_t>(rng.U32() % 0x20000u);
            m.PokeWord(e + 0x27C, static_cast<uint32_t>(lean < 0 ? -steer : steer));
        }
        m.PokeWord(e + 0x230, fa);
        m.PokeWord(e + 0x234, fb);
        {
            uint32_t w = m.PeekWord(e + 0x184);
            w = chance(2) ? (w | 1u) : (w & ~1u);
            m.PokeWord(e + 0x184, w);
        }
        m.PokeWord(e + 0x358, chance(2) ? 0u : sc.pool + 1096u * (rng.U32() % sc.slots));
        if (chance(3)) m.PokeWord(e + 0x22C, statsAt);
        const uint32_t st = m.PeekWord(e + 0x22C);
        // exact thresholds: the steer at +-stats[+0xF4], the lean at the gain ramp's end
        // stats[+0xDC], the speed on the schedule's two ends, the ramp duration at 64/65
        if (chance(8)) m.PokeWord(e + 0x27C, chance(2) ? m.PeekWord(st + 0xF4) : 0u - m.PeekWord(st + 0xF4));
        if (chance(8)) m.PokeWord(e + 0x2A4, chance(2) ? m.PeekWord(st + 0xDC) : 0u - m.PeekWord(st + 0xDC));
        if (chance(8)) m.PokeWord(e + 0x240, m.PeekWord(st + (chance(2) ? 0x164u : 0x168u)));
        if (chance(6)) m.PokeWord(e + 0x2D0, 64u + rng.U32() % 2u);
        if (chance(8)) m.PokeWord(e + 0x2A4, (rng.U32() % 1310u) - 655u); // around the snap
    }
    a.a[0] = static_cast<uint32_t>(DrawDt(rng));
}

bool RegionF(NativeEnv& env, rr::sim::GuestRam& g, int32_t dt) {
    (void)env;
    return rr::sim::BikeStepRegionF(g, dt);
}

// ---------------------------------------------------------------- region A
// Region A walks EVERY pool-0 slot, so its family draws the fields of all of them, not only the
// riding list's. Its callee, the list migration 0x80071BCC, is ported (kListMigrateFn above).

void SetupStepA(CaseContext& c, Args& a) {
    Memory& m = *c.mem;
    Rng& rng = *c.rng;
    auto chance = [&rng](uint32_t n) { return rng.Next() % n == 0; };
    const StepCase sc = PlantRidingList(c);
    static const Field kA[] = {
        {0x388, 4, 12, 4}, {0x38C, 4, 14, 3}, {0x370, 4, 12, 4}, {0x374, 4, 12, 4}, {0x378, 4, 12, 4},
    };
    for (uint32_t s = 0; s < sc.slots; ++s) {
        const uint32_t e = sc.pool + 1096u * s;
        if (!chance(3)) continue; // a third of the slots keep this machine's own values
        DrawFields(c, e, kA, sizeof(kA) / sizeof(kA[0]));
        // the aim delta: full-range words (SumSquares wraps, SqrtGte reads below its table - the
        // window covers it; measured: no guest trap over the whole family), game-sized ones, and
        // tiny ones so that the `len < 16` arm runs
        for (uint32_t k = 0; k < 3; ++k) {
            uint32_t v = static_cast<uint32_t>(rng.S32() >> (8 + rng.U32() % 12u));
            if (chance(5)) v = rng.U32() % 8u;
            else if (chance(4)) v = rng.U32();
            m.PokeWord(e + 0x37C + 4u * k, v);
        }
        // the timer: 0, negative, positive, and exactly the frame's step from above
        int16_t t = static_cast<int16_t>(rng.U32());
        switch (rng.U32() % 5u) {
            case 0: t = 0; break;
            case 1: t = static_cast<int16_t>(-static_cast<int32_t>(1u + rng.U32() % 64u)); break;
            case 2: t = static_cast<int16_t>(1u + rng.U32() % 8u); break;
            default: break;
        }
        WriteU16Bench(m, e + 0x3B0, static_cast<uint16_t>(t));
        // the AI command stack, whose top opcode the re-arm consults: depth 1..4, opcode 0..20
        const uint8_t depth = static_cast<uint8_t>(1u + rng.U32() % 4u);
        m.WriteBlock(e + 0x3B2, &depth, 1);
        WriteU16Bench(m, e + 0x3B4 + 8u * depth, static_cast<uint16_t>(rng.U32() % 21u));
        uint32_t fa = m.PeekWord(e + 0x230), fb = m.PeekWord(e + 0x234), fc = m.PeekWord(e + 0x238);
        fa = chance(2) ? (fa | 0x08000000u) : (fa & ~0x08000000u);
        fb = chance(2) ? (fb | 0x08000200u) : (fb & ~0x08000200u);
        // the migration's gate, flagsC & 0x08001800: bit 27 alone moves the bike by the rule;
        // bits 0..12 drawn as well drive the callee's crash launch, wipeout
        // start and wipeout end (all original code on both sides; measured: no guest trap).
        if (chance(3)) fc |= 0x08000000u;
        if (chance(3)) fc |= rng.U32() & 0x1FFFu;
        m.PokeWord(e + 0x230, fa);
        m.PokeWord(e + 0x234, fb);
        m.PokeWord(e + 0x238, fc);
    }
    a.a[0] = static_cast<uint32_t>(DrawDt(rng));
}


bool RegionA(NativeEnv& env, rr::sim::GuestRam& g, int32_t dt) {
    Machine& cl = *env.clone;
    std::vector<int16_t> sq(kSqrtWindowHalf);
    cl.mem.ReadBlock(kSqrtTable - kSqrtWindowHalf / 2u, sq.data(), sq.size() * 2u);
    rr::sim::BikeTables t;
    t.sqrt = sq.data() + kSqrtWindowHalf / 4u;
    StepCallees calls(env);
    return rr::sim::BikeStepRegionA(g, dt, t, calls);
}

const Splice kSpliceA{"bike_step_a", 0x80075F18, 0x800761C8, {}, RegionA};

uint32_t NativeStepA(NativeEnv& env, const Args& a) { return RunSplice(env, a, kSpliceA); }

// ---------------------------------------------------------------- region D
constexpr uint32_t kPlayer2Bike = 0x8005B21C;     // non-zero: two players (D3's tow)
constexpr uint32_t kPlayerBikeArr = 0x8005B268;   // player p's bike at +4p
constexpr uint32_t kAnalogAxes = 0x800CE540;      // s32 per handle, 8-byte stride (D1)
constexpr uint32_t kEngagedMask = 0x800CCAC0;     // u16 per handle (D3)

void SetupStepD(CaseContext& c, Args& a) {
    Memory& m = *c.mem;
    Rng& rng = *c.rng;
    auto chance = [&rng](uint32_t n) { return rng.Next() % n == 0; };
    const StepCase sc = PlantRidingList(c);
    const uint32_t statsAt = c.scratch + 64u;
    PlantStats(c, statsAt);
    static const Field kD[] = {
        {0x2D0, 4, 14, 2}, {0x394, 4, 8, 2},  {0x398, 4, 8, 3},  {0x244, 4, 8, 3},  {0x258, 4, 10, 3},
        {0x2F8, 4, 12, 3}, {0x39C, 4, 8, 4},  {0x240, 4, 8, 4},  {0x2FC, 4, 12, 3}, {0x2F0, 4, 12, 3},
        {0x2F4, 4, 12, 3}, {0x250, 4, 10, 4}, {0x24C, 4, 10, 3}, {0x254, 4, 10, 3}, {0x2E0, 4, 12, 5},
        {0x28C, 4, 12, 4}, {0x1E0, 4, 8, 5},  {0x216, 1, 0, 4},
    };
    for (uint32_t e : sc.riding) {
        DrawFields(c, e, kD, sizeof(kD) / sizeof(kD[0]));
        uint32_t fa = m.PeekWord(e + 0x230), fb = m.PeekWord(e + 0x234);
        auto bits = [&](uint32_t& w, uint32_t mask, uint32_t odds) {
            w = chance(odds) ? (w | (mask & rng.U32())) : (w & ~mask);
        };
        bits(fa, 0x08000000u, 2); // AI
        bits(fa, 0x00100000u, 3); // the analogue axis
        bits(fa, 0x73u, 2);       // throttle / brake bits and flagsA bit 4 (the grip cap)
        bits(fa, 0x400u, 3);      // the passenger launch's gate
        bits(fb, 0x80000u, 4);    // the AI's latched brake
        m.PokeWord(e + 0x230, fa);
        m.PokeWord(e + 0x234, fb);
        if (chance(3)) m.PokeWord(e + 0x22C, statsAt);
        // the passenger: none, or another live pool-0 bike
        m.PokeWord(e + 0x358, chance(2) ? 0u : sc.pool + 1096u * (rng.U32() % sc.slots));
        // the brake point's decision exactly at 2.0, and a hold that just runs out
        if (chance(10)) m.PokeWord(e + 0x394, 0x20000u);
        if (chance(10)) m.PokeWord(e + 0x2D0, 1u + rng.U32() % 2000u);
        // the handle as a player's, so a player's analogue axis and engaged mask are read
        if (chance(4)) WriteU16Bench(m, e + 0xAC, static_cast<uint16_t>(rng.U32() % 2u));
    }
    // The analogue arm: region B's steering driver sets flagsA bit 20 for a human player whose
    // 192-byte pad record at 0x800D7128 has a non-zero word at +16 (bike.cpp, 0x800738C8), and
    // clears it otherwise - so that word is what drives D1's analogue arm.
    for (uint32_t h = 0; h < 2; ++h) m.PokeWord(kAiTable + 192u * h + 16u, chance(2) ? (rng.U32() | 1u) : 0u);
    // The analogue axes and the engaged masks of the first handles, drawn.
    for (uint32_t h = 0; h < 20; ++h) {
        m.PokeWord(kAnalogAxes + 8u * h, chance(3) ? 0u : static_cast<uint32_t>(rng.S32() >> 14));
        WriteU16Bench(m, kEngagedMask + 2u * h, chance(3) ? static_cast<uint16_t>(rng.U32()) : 0u);
    }
    // TWO PLAYERS, one case in two: player 2 is another riding bike, placed so that D3's tow can
    // arm - within a few units ahead along player 1's heading, heading the same way, both fast
    // (near their stats[+0xE0]) and far from the end (+0x144) - or can drop, by the draws below.
    // No capture has a second player, so this is the only source of D3's tow.
    const uint32_t p1 = sc.pool;
    uint32_t p2 = 0;
    if (sc.riding.size() > 1 && chance(2)) {
        p2 = sc.riding[1 + rng.U32() % (sc.riding.size() - 1)];
        m.PokeWord(kPlayer2Bike, p2);
        m.PokeWord(kPlayerBikeArr, p1);
        m.PokeWord(kPlayerBikeArr + 4u, p2);
        WriteU16Bench(m, p1 + 0xAC, 0);
        const uint32_t pair[2] = {p1, p2};
        for (uint32_t k = 0; k < 2; ++k) {
            const uint32_t e = pair[k];
            uint32_t fa = m.PeekWord(e + 0x230) & ~0x08100040u;       // human, digital, no brake
            if (!chance(4)) fa |= 0x2u;                                // on throttle
            m.PokeWord(e + 0x230, fa);
            m.PokeWord(e + 0x184, m.PeekWord(e + 0x184) & ~1u);
            m.PokeWord(e + 0x144, chance(6) ? rng.U32() % 0x130000u : 0x130000u + rng.U32() % 0x1000000u);
            const uint32_t st = m.PeekWord(e + 0x22C);
            const int32_t top = static_cast<int32_t>(m.PeekWord(st + 0xE0));
            m.PokeWord(e + 0x1E0, static_cast<uint32_t>(top - static_cast<int32_t>(rng.U32() % 0x60000u) + 0x30000));
            m.PokeWord(e + 0x238, (m.PeekWord(e + 0x238) & ~0x10000000u) | (chance(3) ? 0x10000000u : 0u));
            if (chance(3)) m.PokeWord(e + 0x238, m.PeekWord(e + 0x238) & ~0x7ECu);
        }
        // player 2 a few units ahead of player 1 along player 1's heading (+0x1C2), same heading
        int16_t h[3];
        for (uint32_t k = 0; k < 3; ++k) h[k] = GetS16(m, p1 + 0x1C2 + 2u * k);
        for (uint32_t k = 0; k < 3; ++k) WriteU16Bench(m, p2 + 0x1C2 + 2u * k, static_cast<uint16_t>(h[k]));
        const int32_t ahead = static_cast<int32_t>(rng.U32() % (chance(3) ? 0x800000u : 0x300000u)) -
                              (chance(4) ? 0x100000 : 0);
        for (uint32_t k = 0; k < 3; ++k) {
            const int32_t off = static_cast<int32_t>((static_cast<int64_t>(ahead) * h[k]) >> 12);
            PutS32(m, p2 + 0xB8 + 4u * k, GetS32(m, p1 + 0xB8 + 4u * k) + off);
            PutS32(m, p2 + 0x1F8 + 4u * k, GetS32(m, p1 + 0x1F8 + 4u * k) + off);
        }
        WriteU16Bench(m, kEngagedMask, chance(4) ? 1u : 0u);
    } else {
        m.PokeWord(kPlayer2Bike, 0);
    }
    {
        const uint8_t opt = chance(2) ? 0 : static_cast<uint8_t>(chance(2) ? 1u : rng.U32());
        m.WriteBlock(rr::sim::kOptionByte, &opt, 1);
    }
    a.a[0] = static_cast<uint32_t>(DrawDt(rng));
}

bool RegionD(NativeEnv& env, rr::sim::GuestRam& g, int32_t dt) {
    Machine& cl = *env.clone;
    std::vector<int16_t> sq(kSqrtWindowHalf);
    cl.mem.ReadBlock(kSqrtTable - kSqrtWindowHalf / 2u, sq.data(), sq.size() * 2u);
    rr::sim::BikeTables t;
    t.sqrt = sq.data() + kSqrtWindowHalf / 4u;
    StepCallees calls(env);
    return rr::sim::BikeStepRegionD(g, dt, t, calls);
}

const Splice kSpliceD{"bike_step_d", 0x80076804, 0x800774C8, {}, RegionD};

uint32_t NativeStepD(NativeEnv& env, const Args& a) { return RunSplice(env, a, kSpliceD); }

// ---------------------------------------------------------------- region E
// Region E's inputs are mostly what D4 leaves (the amounts, the ratio seeds, the passive term), so
// the family draws D's inputs too, and plants scenarios for E's rare arms - no capture has a fall,
// a pitch move or a latched impact on the riding list.
void SetupStepE(CaseContext& c, Args& a) {
    Memory& m = *c.mem;
    Rng& rng = *c.rng;
    auto chance = [&rng](uint32_t n) { return rng.Next() % n == 0; };
    const StepCase sc = PlantRidingList(c);
    const uint32_t statsAt = c.scratch + 64u;
    PlantStats(c, statsAt);
    static const Field kE[] = {
        {0x240, 4, 8, 4},  {0x250, 4, 10, 4}, {0x24C, 4, 10, 3}, {0x258, 4, 10, 4}, {0x254, 4, 10, 3},
        {0x2E0, 4, 12, 5}, {0x28C, 4, 12, 4}, {0x27C, 4, 13, 4}, {0x268, 4, 12, 2}, {0x2F0, 4, 12, 3},
        {0x2F4, 4, 12, 3}, {0x2F8, 4, 12, 3}, {0x2EC, 4, 12, 4}, {0x2DC, 4, 10, 4}, {0x2E4, 4, 10, 4},
        {0x2A4, 4, 12, 3}, {0x2A8, 4, 12, 3}, {0x2C4, 4, 12, 3}, {0x1E8, 4, 12, 3}, {0x2B0, 4, 12, 3},
        {0x39C, 4, 8, 4},  {0x158, 4, 12, 3}, {0x216, 1, 0, 4},  {0x212, 2, 0, 4},  {0x1C4, 2, 0, 4},
    };
    for (uint32_t e : sc.riding) {
        DrawFields(c, e, kE, sizeof(kE) / sizeof(kE[0]));
        uint32_t fa = m.PeekWord(e + 0x230), fb = m.PeekWord(e + 0x234);
        auto bits = [&](uint32_t& w, uint32_t mask, uint32_t odds) {
            w = chance(odds) ? (w | (mask & rng.U32())) : (w & ~mask);
        };
        bits(fa, 0x08000000u, 2);
        bits(fa, 0x01001863u, 2); // throttle/brake, bits 11/12 (the wheelie), bit 24 (the tow)
        bits(fa, 0x300u, 3);
        bits(fb, 0x0020173Cu, 2); // 2..5, 8, 10 (latched), 12 (stop), 21
        m.PokeWord(e + 0x230, fa);
        m.PokeWord(e + 0x234, fb);
        {
            uint32_t w = m.PeekWord(e + 0x184);
            w = chance(3) ? (w | 1u) : (w & ~1u);
            m.PokeWord(e + 0x184, w);
        }
        if (chance(3)) m.PokeWord(e + 0x22C, statsAt);
        switch (rng.U32() % 8u) {
            case 1: // the grip-loss fall: the lateral demand past the grip, the steer small
                m.PokeWord(e + 0x28C, (rng.U32() & 1u) ? 0x7FFF0000u : 0x80010000u);
                m.PokeWord(e + 0x27C, static_cast<uint32_t>(static_cast<int32_t>(rng.U32() % 40000u) - 20000));
                m.PokeWord(e + 0x234, m.PeekWord(e + 0x234) & ~0x400u);
                break;
            case 2: // latched: the fall continues or settles
                m.PokeWord(e + 0x234, m.PeekWord(e + 0x234) | 0x400u);
                if (chance(2)) m.PokeWord(e + 0x2C4, static_cast<uint32_t>(rng.U32() % 0x40000u));
                break;
            case 3: // wheelspin: throttle only, a tiny grip gain
                m.PokeWord(e + 0x230, (m.PeekWord(e + 0x230) & ~0x40u) | 0x2u);
                m.PokeWord(e + 0x2E0, rng.U32() % 64u);
                m.PokeWord(e + 0x254, 0);
                break;
            case 4: // front lock: the brake, a tiny grip gain
                m.PokeWord(e + 0x230, (m.PeekWord(e + 0x230) & ~0x2u) | 0x40u);
                m.PokeWord(e + 0x2E0, rng.U32() % 64u);
                break;
            case 5: // a pitch move from rest: the load past +-1.0 (a small stats[+0x08] x +0x2E0)
                m.PokeWord(e + 0x268, 0);
                m.PokeWord(e + 0x2E0, 1u + rng.U32() % 256u);
                m.PokeWord(e + 0x28C, rng.U32() % 4000u);
                // ... sometimes with the facing's vertical just under 3548, where the target's
                // scale FixMul(0xDDB2 - 16 * y, 0xBB68) is tiny and the move is SET, not ramped
                // (0x80078454)
                if (chance(2)) WriteU16Bench(m, e + 0x212, static_cast<uint16_t>(3490u + rng.U32() % 57u));
                break;
            case 6: // a hard stop: flagsB bit 12, a small speed
                m.PokeWord(e + 0x234, m.PeekWord(e + 0x234) | 0x1000u);
                m.PokeWord(e + 0x240, rng.U32() % 0x2000u);
                break;
            default:
                break;
        }
    }
    a.a[0] = static_cast<uint32_t>(DrawDt(rng));
}

bool RegionE(NativeEnv& env, rr::sim::GuestRam& g, int32_t dt) {
    const Tables tab(env.clone->mem, env.gp);
    StepCallees calls(env);
    return rr::sim::BikeStepRegionE(g, dt, tab.t, calls);
}

const Splice kSpliceE{"bike_step_e", 0x800774C8, 0x80078478, {}, RegionE};

uint32_t NativeStepE(NativeEnv& env, const Args& a) { return RunSplice(env, a, kSpliceE); }

// ---------------------------------------------------------------- region B
void SetupStepB(CaseContext& c, Args& a) {
    Memory& m = *c.mem;
    Rng& rng = *c.rng;
    auto chance = [&rng](uint32_t n) { return rng.Next() % n == 0; };
    const StepCase sc = PlantRidingList(c, 3);
    static const Field kB[] = {
        {0x2D0, 4, 14, 3}, {0x2D4, 4, 14, 3}, {0x1E0, 4, 8, 3},  {0x240, 4, 8, 3},  {0x27C, 4, 13, 3},
        {0x248, 4, 13, 3}, {0x1E8, 4, 13, 3}, {0x29C, 4, 13, 3}, {0x2DC, 4, 12, 3}, {0x2E0, 4, 12, 3},
        {0x268, 4, 13, 3}, {0x2A4, 4, 13, 3}, {0x2A0, 4, 13, 3}, {0x28C, 4, 13, 3}, {0x33A, 2, 0, 3},
    };
    // both lists' bikes: the riding list and whatever landed on 0x8005B2D8
    std::vector<uint32_t> bikes = sc.riding;
    {
        uint32_t n = m.PeekWord(rr::sim::kThrownListHead + 4u);
        for (int guard = 0; n != rr::sim::kThrownListHead && guard < 64; ++guard) {
            bikes.push_back(n - 1088u);
            n = m.PeekWord(n + 4u);
        }
    }
    for (uint32_t e : bikes) {
        DrawFields(c, e, kB, sizeof(kB) / sizeof(kB[0]));
        uint32_t fa = m.PeekWord(e + 0x230), fb = m.PeekWord(e + 0x234), fc = m.PeekWord(e + 0x238);
        auto bits = [&](uint32_t& w, uint32_t mask, uint32_t odds) {
            w = chance(odds) ? (w | (mask & rng.U32())) : (w & ~mask);
        };
        bits(fa, 0x08186300u, 2); // AI, steering bits, 13/14 (recovery), 19/20
        bits(fb, 0x18000040u, 3);
        bits(fc, 0x7FFu, 4);
        m.PokeWord(e + 0x230, fa);
        m.PokeWord(e + 0x234, fb);
        m.PokeWord(e + 0x238, fc);
        // the passenger: none, or another live pool-0 bike. NAMED BOUND:
        // the owner's +0x23C bit 4 only with a passenger - without one the original's passenger
        // steer reads guest 0x2D0 and the port refuses it.
        const bool pas = !chance(2);
        m.PokeWord(e + 0x358, pas ? sc.pool + 1096u * (rng.U32() % sc.slots) : 0u);
        const uint32_t owner = m.PeekWord(e + 0x354);
        if (owner >= 0x80000000u && owner < 0x80200000u - 0x260u) {
            uint8_t ob = m.PeekByte(owner + 0x23Cu);
            ob = static_cast<uint8_t>(pas && chance(2) ? (ob | 0x10u) : (ob & ~0x10u));
            m.WriteBlock(owner + 0x23Cu, &ob, 1);
        }
        if (chance(3)) WriteU16Bench(m, e + 0xAC, static_cast<uint16_t>(rng.U32() % 3u));
    }
    for (uint32_t h = 0; h < 6; ++h) {
        m.PokeWord(kAiTable + 192u * h + 16u, chance(2) ? (rng.U32() | 1u) : 0u);
        m.PokeWord(kHandleTable + 8u * h + 4u, static_cast<uint32_t>(rng.S32() >> 14));
    }
    const uint32_t gs = m.PeekWord(kGameStatePtr);
    m.PokeWord(gs + 0x34, rng.U32() % 4u);
    a.a[0] = static_cast<uint32_t>(DrawDt(rng));
}

bool RegionB(NativeEnv& env, rr::sim::GuestRam& g, int32_t dt) {
    (void)g;
    StepCallees calls(env);
    return rr::sim::BikeStepRegionB(dt, calls);
}

// C's live-in: `s1 = 0x8005B298`, which B's own code never sets for C - the
// original loads it for its first call at 0x800761C8 and keeps it.
const Splice kSpliceB{"bike_step_b", 0x800761C8, 0x80076208, {{17, 0x8005B298u}}, RegionB};

uint32_t NativeStepB(NativeEnv& env, const Args& a) { return RunSplice(env, a, kSpliceB); }

// G's live-in: `a0 = 0x1F800000`, set by the `lui` at 0x8007848C / 0x80078AA8.
const Splice kSpliceF{"bike_step_f", 0x80078478, 0x80078AAC, {{4, 0x1F800000u}}, RegionF};

uint32_t NativeStepF(NativeEnv& env, const Args& a) { return RunSplice(env, a, kSpliceF); }

// ================================================================ the regions' former seams
// Ordinary rows, one per function, on THIS machine's live bikes and riders -
// the rider layer the seam still runs walks the rider's animation objects, which only a real rider
// of this snapshot has.

void SetupRotate(CaseContext& c, Args& a) {
    Rng& rng = *c.rng;
    const uint32_t pa = c.scratch + 0x10u, pb = c.scratch + 0x04u; // the game's pair: b = a - 12
    for (uint32_t k = 0; k < 3; ++k) {
        WriteU16Bench(*c.mem, pa + 2u * k, static_cast<uint16_t>((rng.Next() & 1u) ? rng.Unit() : rng.S16()));
        WriteU16Bench(*c.mem, pb + 2u * k, static_cast<uint16_t>((rng.Next() & 1u) ? rng.Unit() : rng.S16()));
    }
    a.a[0] = pa;
    a.a[1] = pb;
    a.a[2] = (rng.Next() % 4u == 0) ? rng.U32() : (rng.U32() % 4096u);
}

uint32_t NativeRotate(Memory& m, uint32_t scratch, uint32_t gp, const Args& a) {
    (void)scratch;
    std::vector<int16_t> sincos(8192);
    m.ReadBlock(kSinCosTable, sincos.data(), sincos.size() * 2u);
    rr::sim::GuestRam g(m.ram().data(), gp);
    rr::sim::RotateRowPair(g, a.a[0], a.a[1], static_cast<int32_t>(a.a[2]), sincos.data());
    return g.Faulted() ? 0xDEADDEADu : 0u;
}

void SetupRideArm(CaseContext& c, Args& a) {
    std::vector<uint8_t> e = BaseEntity(c);
    std::vector<uint8_t> p = BaseEntity(c);
    Rng& rng = *c.rng;
    PutU32V(e, 0x230, rng.U32());
    PutU32V(e, 0x1E8, static_cast<uint32_t>(rng.S32() >> (rng.U32() % 16u)));
    PutU32V(p, 0x1E8, rng.U32());
    const uint32_t ea = c.scratch, pa = c.scratch + 1100u;
    switch (rng.U32() % 3u) {
        case 0: PutU32V(e, 0x358, 0); break;
        case 1: PutU32V(e, 0x358, pa); break;
        default: PutU32V(e, 0x358, ea); break; // the bike its own passenger
    }
    c.mem->WriteBlock(ea, e.data(), e.size());
    c.mem->WriteBlock(pa, p.data(), p.size());
    a.a[0] = ea;
}

uint32_t NativeRideArm(Memory& m, uint32_t scratch, uint32_t gp, const Args& a) {
    (void)scratch;
    rr::sim::GuestRam g(m.ram().data(), gp);
    rr::sim::BikeRideArmReset(g, a.a[0]);
    return g.Faulted() ? 0xDEADDEADu : 0u;
}

// The three functions that reach the rider layer: a live pool-0 bike of this machine, its riders
// real, and the fields their arms test drawn - including the rider's knock-off request bit 15 and a
// stance in {89, 39, 88, 40, 38}, which is what sends the rider layer's two entries into play.
enum class RiderCase { kRecover, kMigrate, kLaunch };
constexpr uint32_t kStanceTable = 0x800541D4; // SLUS, 8 bytes per stance

void SetupRiderCase(CaseContext& c, Args& a, RiderCase mode) {
    Memory& m = *c.mem;
    Rng& rng = *c.rng;
    auto chance = [&rng](uint32_t n) { return rng.Next() % n == 0; };
    const uint32_t pool = m.PeekWord(kPoolTable);
    const uint32_t hp = m.PeekWord(kPoolTable + 12u);
    uint32_t slots = (hp >= 0x80000000u) ? m.PeekWord(hp) + 1u : 1u;
    if (slots > 64u) slots = 1u;
    const uint32_t e = pool + 1096u * (rng.U32() % slots);
    static const Field kR[] = {
        {0x2A4, 4, 12, 3}, {0x27C, 4, 12, 3}, {0x1E8, 4, 12, 3}, {0x228, 4, 12, 3}, {0x134, 4, 12, 4},
        {0x1E0, 4, 8, 4},  {0x268, 4, 12, 2}, {0x240, 4, 8, 4},
    };
    DrawFields(c, e, kR, sizeof(kR) / sizeof(kR[0]));
    uint32_t fa = m.PeekWord(e + 0x230), fb = m.PeekWord(e + 0x234), fc = m.PeekWord(e + 0x238);
    fb ^= rng.U32() & rng.U32();
    fc = (fc & ~0x1FFFu) | (rng.U32() & 0x1FFFu);
    if (chance(3)) fc &= ~0x600u;
    if (chance(3)) fc &= ~0x1FFu;
    if (chance(2)) fc |= 0x08000000u;
    fa = chance(2) ? (fa | 0x400u) : (fa & ~0x400u);
    m.PokeWord(e + 0x230, fa);
    m.PokeWord(e + 0x234, fb);
    m.PokeWord(e + 0x238, fc);
    if (mode == RiderCase::kMigrate) {
        const uint16_t live = chance(3) ? 0u : static_cast<uint16_t>(1u + rng.U32() % 3u);
        WriteU16Bench(m, e + 0x140, live);
    }
    // the passenger: none, or another live pool-0 bike (the launch always has one)
    const bool pas = (mode == RiderCase::kLaunch) ? true : !chance(2);
    uint32_t pe = 0;
    if (pas) {
        pe = pool + 1096u * (rng.U32() % slots);
        if (mode == RiderCase::kLaunch && slots > 1u && pe == e) pe = pool + 1096u * ((1u + (e - pool) / 1096u) % slots);
    }
    m.PokeWord(e + 0x358, pe);
    // NAMED BOUND (as region B's): the owner's +0x23C bit 4 only with a passenger - without one the
    // original's passenger arm reads the rider pointer out of guest address 0x354.
    const uint32_t owner = m.PeekWord(e + 0x354);
    if (owner >= 0x80000000u && owner < 0x80200000u - 0x260u) {
        uint8_t ob = m.PeekByte(owner + 0x23Cu);
        ob = static_cast<uint8_t>(pas && chance(2) ? (ob | 0x10u) : (ob & ~0x10u));
        m.WriteBlock(owner + 0x23Cu, &ob, 1);
    }
    // the riders the arms reach: the own rider and the passenger's
    auto rider = [&](uint32_t r) {
        if (r < 0x80000000u || r >= 0x80200000u - 0x260u) return;
        uint32_t f = m.PeekWord(r + 0x228);
        f = chance(3) ? (f | 0x8000u) : (f & ~0x8000u);
        m.PokeWord(r + 0x228, f);
        // A drawn stance must keep the rider's animation state consistent: the stance id names an
        // animation BANK (SLUS 0x800541D4 + 8 * id, bits 0..3) and the rider's animation object
        // holds that bank's clip table, so a stance of another bank makes the next clip start index
        // past the loaded table (measured: the guest traps at 0x8005BF94). Only stances of the
        // current stance's bank are planted - a consistency rule, not a bound on the arms.
        static const uint16_t kStance[6] = {89, 39, 88, 40, 38, 41};
        if (chance(2)) {
            const uint32_t cur = static_cast<uint32_t>(m.PeekByte(r + 0x220) | (m.PeekByte(r + 0x221) << 8));
            const uint32_t bankCur = m.PeekByte(kStanceTable + 8u * cur) & 0xFu;
            uint16_t pick[6];
            uint32_t n = 0;
            for (uint16_t s : kStance)
                if ((m.PeekByte(kStanceTable + 8u * s) & 0xFu) == bankCur) pick[n++] = s;
            if (n != 0) WriteU16Bench(m, r + 0x220, pick[rng.U32() % n]);
        }
        if (chance(2)) m.PokeWord(r + 0x25C, rng.U32() % 5u);
    };
    rider(owner);
    if (pe != 0) rider(m.PeekWord(pe + 0x354));
    a.a[0] = e;
    a.a[1] = static_cast<uint32_t>(DrawDt(rng)); // dead in all three
}

uint32_t NativeRiderCase(NativeEnv& env, const Args& a, RiderCase mode) {
    const StepTables tab(env.clone->mem, env.gp);
    MigrateCallees mc(env, tab.t);
    rr::sim::GuestRam g = StepView(env);
    bool ok = false;
    const char* what = "";
    switch (mode) {
        case RiderCase::kRecover: ok = rr::sim::BikeRecoverEnd(g, a.a[0], env.sp, tab.t, mc); what = "BikeRecoverEnd"; break;
        case RiderCase::kMigrate: ok = rr::sim::BikeListMigrate(g, a.a[0], env.sp, tab.t, mc); what = "BikeListMigrate"; break;
        case RiderCase::kLaunch: ok = rr::sim::BikePassengerLaunch(g, a.a[0], env.sp, mc); what = "BikePassengerLaunch"; break;
    }
    return Done(env, g, ok, what);
}

// ================================================================ regions G, H, I, J
//
// Their family is the splice family of A..F (the riding list rebuilt on rr-race, 0..5 more bikes on
// it) plus, per pool slot, what the tail reads: the live word +0x140 (a dormant AI bike made live,
// a live one parked), a passenger, the integrator's inputs (speed, the yaw terms, the crash and
// wipeout bits), and for J the players: a second player, the riders' mount state and bit 19, the
// views' +0x21C / +0x314 and the bike's +0x2C + 4p around their thresholds.
void DrawTailSlots(CaseContext& c, const StepCase& sc) {
    Memory& m = *c.mem;
    Rng& rng = *c.rng;
    auto chance = [&rng](uint32_t n) { return rng.Next() % n == 0; };
    static const Field kT[] = {
        {0x240, 4, 8, 3}, {0x1E0, 4, 8, 4}, {0x1E8, 4, 13, 2}, {0x2A4, 4, 13, 2}, {0x2E8, 4, 13, 2},
    };
    for (uint32_t s = 0; s < sc.slots; ++s) {
        const uint32_t e = sc.pool + 1096u * s;
        const bool riding = std::find(sc.riding.begin(), sc.riding.end(), e) != sc.riding.end();
        if (!riding && !chance(3)) continue;
        DrawFields(c, e, kT, sizeof(kT) / sizeof(kT[0]));
        if (chance(4)) WriteU16Bench(m, e + 0x140, chance(3) ? 0u : static_cast<uint16_t>(1u + rng.U32() % 3u));
        if (chance(4)) m.PokeWord(e + 0x358, chance(2) ? 0u : sc.pool + 1096u * (rng.U32() % sc.slots));
        uint32_t fb = m.PeekWord(e + 0x234), fc = m.PeekWord(e + 0x238);
        auto bits = [&](uint32_t& w, uint32_t mask, uint32_t odds) {
            w = chance(odds) ? (w | (mask & rng.U32())) : (w & ~mask);
        };
        bits(fb, 0x8000u, 4);
        // NAMED BOUND: flagsB bit 18 (riding along a contact, integrator.h) only on a bike whose
        // contact record +0x340 is in RAM - without one the original reads the contact axes out of
        // guest 0x104.., the kernel's area, and the ported integrator refuses to follow it.
        const uint32_t ct = m.PeekWord(e + 0x340);
        if (ct >= 0x80000000u && ct < 0x80200000u - kContactBytes) bits(fb, 0x40000u, 4);
        else fb &= ~0x40000u;
        bits(fb, 0x400u, 6);
        bits(fb, 0x1000u, 8);
        bits(fc, 0xFu, 6);
        bits(fc, 0x10u, 6);
        bits(fc, 0x600u, 6);
        m.PokeWord(e + 0x234, fb);
        m.PokeWord(e + 0x238, fc);
    }
}

void DrawPlayers(CaseContext& c, const StepCase& sc) {
    Memory& m = *c.mem;
    Rng& rng = *c.rng;
    auto chance = [&rng](uint32_t n) { return rng.Next() % n == 0; };
    const uint32_t gs = m.PeekWord(kGameStatePtr);
    uint32_t players = 1;
    if (sc.riding.size() > 1 && chance(2)) {
        players = 2;
        const uint32_t p2 = sc.riding[1 + rng.U32() % (sc.riding.size() - 1)];
        m.PokeWord(kPlayer2Bike, p2);
        m.PokeWord(kPlayerBikeArr + 4u, p2);
    }
    if (chance(12)) players = 0;
    m.PokeWord(gs + 0x30, players);
    for (uint32_t p = 0; p < 2; ++p) {
        const uint32_t b = m.PeekWord(kPlayerBikeArr + 4u * p);
        if (b < 0x80000000u || b >= 0x80200000u - 1096u) continue;
        const uint32_t r = m.PeekWord(b + 0x354);
        if (r >= 0x80000000u && r < 0x80200000u - 0x260u) {
            if (chance(2)) m.PokeWord(r + 0x25C, rng.U32() % 5u);
            uint32_t f = m.PeekWord(r + 0x228);
            f = chance(2) ? (f | 0x80000u) : (f & ~0x80000u);
            m.PokeWord(r + 0x228, f);
        }
        const uint32_t v = kViewArray + kViewStride * p;
        if (chance(4)) m.PokeWord(v + 0x21C, 6);
        else if (chance(2)) m.PokeWord(v + 0x21C, rng.U32() % 8u);
        switch (rng.U32() % 4u) {
            case 0: m.PokeWord(v + 0x314, 0x30000u + (rng.U32() % 2u)); break;
            case 1: m.PokeWord(v + 0x314, 0x30000u - (rng.U32() % 0x4000u)); break;
            case 2: m.PokeWord(v + 0x314, rng.U32()); break;
            default: break;
        }
        const uint32_t w = b + 0x2Cu + 4u * p;
        switch (rng.U32() % 3u) {
            case 0: m.PokeWord(w, 4800u + (rng.U32() % 2u)); break;
            case 1: m.PokeWord(w, rng.U32() % 9600u); break;
            default: break;
        }
        m.PokeWord(0x8005B380u + 4u * p, rng.U32());
    }
}

void SetupStepTail(CaseContext& c, Args& a) {
    const StepCase sc = PlantRidingList(c);
    DrawTailSlots(c, sc);
    DrawPlayers(c, sc);
    a.a[0] = static_cast<uint32_t>(DrawDt(*c.rng));
}

bool RegionTail(NativeEnv& env, char which, int32_t dt) {
    const StepTables tab(env.clone->mem, env.gp);
    TailCallees calls(env, tab);
    rr::sim::GuestRam g = StepView(env);
    const uint32_t ssp = env.sp - kStepFrame;
    bool ok = false;
    switch (which) {
        case 'G': ok = rr::sim::BikeStepRegionG(g, dt, calls); break;
        case 'H': ok = rr::sim::BikeStepRegionH(g, ssp, calls); break;
        case 'I': ok = rr::sim::BikeStepRegionI(g, dt, calls); break;
        default: ok = rr::sim::BikeStepRegionJ(g, dt, ssp, calls); break;
    }
    if (!ok && env.failure.empty()) {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "region %c refused (view fault 0x%08X)", which, g.FaultAddress());
        env.failure = buf;
    }
    return ok;
}

bool RegionG(NativeEnv& env, rr::sim::GuestRam&, int32_t dt) { return RegionTail(env, 'G', dt); }
bool RegionH(NativeEnv& env, rr::sim::GuestRam&, int32_t dt) { return RegionTail(env, 'H', dt); }
bool RegionI(NativeEnv& env, rr::sim::GuestRam&, int32_t dt) { return RegionTail(env, 'I', dt); }
bool RegionJ(NativeEnv& env, rr::sim::GuestRam&, int32_t dt) { return RegionTail(env, 'J', dt); }

// G needs nothing in a register at H; H leaves I's `v1 = 0x800D0000` (the `lui` in the delay slots
// at 0x80078BE8 / 0x80078C0C); I and J leave nothing.
const Splice kSpliceG{"bike_step_g", 0x80078AAC, 0x80078B54, {}, RegionG};
const Splice kSpliceH{"bike_step_h", 0x80078B54, 0x80078C10, {{3, 0x800D0000u}}, RegionH};
const Splice kSpliceI{"bike_step_i", 0x80078C10, 0x80078C58, {}, RegionI};
const Splice kSpliceJ{"bike_step_j", 0x80078C58, 0x80078D84, {}, RegionJ};
uint32_t NativeStepG(NativeEnv& env, const Args& a) { return RunSplice(env, a, kSpliceG); }
uint32_t NativeStepH(NativeEnv& env, const Args& a) { return RunSplice(env, a, kSpliceH); }
uint32_t NativeStepI(NativeEnv& env, const Args& a) { return RunSplice(env, a, kSpliceI); }
uint32_t NativeStepJ(NativeEnv& env, const Args& a) { return RunSplice(env, a, kSpliceJ); }

// The ground query RASHCDG 0x800A7BF8 is NOT a seam of these rows: it is ported (ground.h) and runs
// natively. Its two output buffers are the ORIGINAL ground frame's own frame slots (sp - 136 - 120 +
// 24 / 40 / 72), at the same addresses on both sides. As a seam their content could NOT be
// compared at the return: when the query writes nothing there, the
// bytes are stale stack - the original road passes' register spills on the guest's side, which the
// ported passes do not write (measured then: 53 of 1688 buffers in region H, 24 of 1084 in the whole
// step differed, every first-reported one untouched, RAM, scratchpad and calls equal).
std::vector<OracleCallee> TailJCallees() {
    // 0x80093E6C and 0x800950E8 are ported (rows_population.inc): what stays declared is THEIR unported
    // callees (and the stance event they reach, witnessed), the road layer's six they reach through
    // the step's own road seams (placement and the rider re-entry run RoadClass / RoadRebindBody /
    // RouteBind), then EndRace's three.
    std::vector<OracleCallee> v = PopPassSeams(kStepFrame);
    for (const OracleCallee& oc : RoadSeamCallees()) v.push_back(oc);
    for (const OracleCallee& oc : {OracleCallee{kResultStamp, 1}, OracleCallee{kWipeoutEffect, 1}, OracleCallee{kViewEvent, 2}})
        v.push_back(oc);
    return v;
}

// ================================================================ the whole step
// `0x80075EE0(dt)` against the ten native regions run in order. Not a
// splice: the native side is the port alone. Each case draws one region's family, so the whole
// function sees every family the regions were benched on.
void SetupStepWhole(CaseContext& c, Args& a) {
    switch (c.rng->U32() % 10u) {
        case 0: SetupStepA(c, a); break;
        case 1: SetupStepB(c, a); break;
        case 2: SetupStepC(c, a); break;
        case 3: SetupStepD(c, a); break;
        case 4: SetupStepE(c, a); break;
        case 5: SetupStepF(c, a); break;
        default: SetupStepTail(c, a); break;
    }
}

uint32_t NativeStepWhole(NativeEnv& env, const Args& a) {
    const StepTables tab(env.clone->mem, env.gp);
    StepCallees calls(env);
    TailCallees tail(env, tab);
    rr::sim::GuestRam g = StepView(env);
    const bool ok = rr::sim::BikeStep(g, static_cast<int32_t>(a.a[0]), env.sp, tab.t, calls, tail);
    return Done(env, g, ok, "BikeStep");
}

} // namespace bs

const CoverageRange* BikeStepCoverage(const char* rowName) {
    struct Named {
        const char* name;
        CoverageRange range;
    };
    static const Named kRanges[] = {
        {"bike_reset_orientation", {bs::kResetOrient, 0x8008CFDC}},
        {"bike_heading_pass", {bs::kHeadingPass, 0x8007B840}},
        {"bike_step_a", {0x80075F18, 0x800761C8}},
        {"bike_step_b", {0x800761C8, 0x80076208}},
        {"bike_step_c", {0x80076208, 0x80076804}},
        {"bike_step_d", {0x80076804, 0x800774C8}},
        {"bike_step_e", {0x800774C8, 0x80078478}},
        {"bike_step_f", {0x80078478, 0x80078AAC}},
        {"bike_step_g", {0x80078AAC, 0x80078B54}},
        {"bike_step_h", {0x80078B54, 0x80078C10}},
        {"bike_step_i", {0x80078C10, 0x80078C58}},
        {"bike_step_j", {0x80078C58, 0x80078D84}},
        {"bike_step", {rr::sim::kBikeStepFn, 0x80078DB4}},
        {"rotate_row_pair", {bs::kRotateRowPair, 0x8002EE50}},
        {"bike_ride_arm_reset", {bs::kRideArmReset, 0x8007F0BC}},
        {"bike_recover_end", {bs::kRecoverEnd, 0x80072C7C}},
        {"bike_list_migrate", {bs::kListMigrateFn, 0x80071D24}},
        {"bike_passenger_launch", {bs::kPassengerLaunch, 0x80074E6C}},
    };
    for (const Named& n : kRanges)
        if (std::strcmp(n.name, rowName) == 0) return &n.range;
    return nullptr;
}

void AddBikeStepRows(std::vector<Row>& rows) {
    {
        Row r{"bike_reset_orientation", "void 0x8008CF74(Bike*)", bs::kResetOrient, bs::SetupReset,
              bs::NativeReset};
        r.compareV0 = false;
        rows.push_back(r);
    }
    {
        Row r{"bike_heading_pass", "void 0x8007AC04(list, dt)", bs::kHeadingPass, bs::SetupHeading,
              nullptr};
        r.compareV0 = false;
        r.nativeEx = bs::NativeHeading;
        rows.push_back(r);
    }
    // The splice rows: the whole step on both sides, one region of it native.
    {
        Row r{"bike_step_a", "0x80075EE0(dt), region A spliced", rr::sim::kBikeStepFn, bs::SetupStepA,
              nullptr};
        r.compareV0 = false;
        r.nativeEx = bs::NativeStepA;
        r.maxSteps = 20'000'000;
        // The list migration 0x80071BCC is PORTED; what stays
        // on the seam under it is the rider layer, reached through 0x80072994 at the step's frame
        // + 40 + 32 (the port calls it at exactly that sp; frame 0 here).
        r.oracleCallees = {bs::KnockOffCallee(), bs::LaunchCallee()};
        rows.push_back(r);
    }
    {
        Row r{"bike_step_b", "0x80075EE0(dt), region B spliced", rr::sim::kBikeStepFn, bs::SetupStepB,
              nullptr};
        r.compareV0 = false;
        r.nativeEx = bs::NativeStepB;
        r.maxSteps = 20'000'000;
        rows.push_back(r);
    }
    {
        Row r{"bike_step_c", "0x80075EE0(dt), region C spliced", rr::sim::kBikeStepFn, bs::SetupStepC,
              nullptr};
        r.compareV0 = false;
        r.nativeEx = bs::NativeStepC;
        r.maxSteps = 20'000'000;
        rows.push_back(r);
    }
    {
        Row r{"bike_step_d", "0x80075EE0(dt), region D spliced", rr::sim::kBikeStepFn, bs::SetupStepD,
              nullptr};
        r.compareV0 = false;
        r.nativeEx = bs::NativeStepD;
        r.maxSteps = 20'000'000;
        // D3's unported callees, at the depths the original calls them from: the rider knock-off
        // from inside the PORTED passenger launch (step frame + 32, called at exactly that sp;
        // frame 0 here), the crash emitter from inside the PORTED crash timer.
        r.oracleCallees = {bs::KnockOffCallee(),
                           OracleCallee{kCrashEmit, 3, 0, {}, bs::kStepFrame + bs::kCrashTimerFrame}};
        rows.push_back(r);
    }
    {
        Row r{"bike_step_e", "0x80075EE0(dt), region E spliced", rr::sim::kBikeStepFn, bs::SetupStepE,
              nullptr};
        r.compareV0 = false;
        r.nativeEx = bs::NativeStepE;
        r.maxSteps = 20'000'000;
        // E3's stance event, from the step's own frame (0x80078058).
        r.oracleCallees = {OracleCallee{kAiStance, 3, 0, {}, bs::kStepFrame}};
        rows.push_back(r);
    }
    {
        Row r{"bike_step_f", "0x80075EE0(dt), region F spliced", rr::sim::kBikeStepFn, bs::SetupStepF,
              nullptr};
        r.compareV0 = false;
        r.nativeEx = bs::NativeStepF;
        r.maxSteps = 20'000'000;
        rows.push_back(r);
    }
    // ---- regions G..J, spliced the same way
    {
        Row r{"bike_step_g", "0x80075EE0(dt), region G spliced", rr::sim::kBikeStepFn, bs::SetupStepTail,
              nullptr};
        r.compareV0 = false;
        r.nativeEx = bs::NativeStepG;
        r.maxSteps = 40'000'000;
        rows.push_back(r); // no seam: the integrator is a closed tree
    }
    {
        Row r{"bike_step_h", "0x80075EE0(dt), region H spliced", rr::sim::kBikeStepFn, bs::SetupStepTail,
              nullptr};
        r.compareV0 = false;
        r.nativeEx = bs::NativeStepH;
        r.maxSteps = 40'000'000;
        // the six unread road functions under the four road passes and the contact frame's
        // re-bind, at the exact sp the port computes (the ground query is ported, ground.h)
        r.oracleCallees = bs::RoadSeamCallees();
        rows.push_back(r);
    }
    {
        Row r{"bike_step_i", "0x80075EE0(dt), region I spliced", rr::sim::kBikeStepFn, bs::SetupStepTail,
              nullptr};
        r.compareV0 = false;
        r.nativeEx = bs::NativeStepI;
        r.maxSteps = 40'000'000;
        rows.push_back(r); // no seam: BikeRiderPose is a closed tree
    }
    {
        Row r{"bike_step_j", "0x80075EE0(dt), region J spliced", rr::sim::kBikeStepFn, bs::SetupStepTail,
              nullptr};
        r.compareV0 = false;
        r.nativeEx = bs::NativeStepJ;
        r.maxSteps = 40'000'000;
        r.oracleCallees = bs::TailJCallees();
        rows.push_back(r);
    }
    // ---- the whole step: all ten regions native, in order
    {
        Row r{"bike_step", "void 0x80075EE0(s32 dt)  [RASHCDG, the per-bike step, all ten regions]",
              rr::sim::kBikeStepFn, bs::SetupStepWhole, nullptr};
        r.compareV0 = false;
        r.nativeEx = bs::NativeStepWhole;
        r.maxSteps = 40'000'000;
        r.oracleCallees = {bs::KnockOffCallee(), bs::LaunchCallee(),
                           OracleCallee{kCrashEmit, 3, 0, {}, bs::kStepFrame + bs::kCrashTimerFrame},
                           OracleCallee{kAiStance, 3, 0, {}, bs::kStepFrame}};
        for (const OracleCallee& oc : bs::RoadSeamCallees()) r.oracleCallees.push_back(oc);
        for (const OracleCallee& oc : bs::TailJCallees()) r.oracleCallees.push_back(oc);
        rows.push_back(r);
    }
    // ---- the regions' former seams, one row per function
    {
        Row r{"rotate_row_pair", "void 0x8002ED94(s16 a[3], s16 b[3], s32 ang)  [SLUS]", bs::kRotateRowPair,
              bs::SetupRotate, bs::NativeRotate};
        r.compareV0 = false;
        rows.push_back(r);
    }
    {
        Row r{"bike_ride_arm_reset", "void 0x8007F08C(Bike*)  [RASHCDG, a leaf]", bs::kRideArmReset,
              bs::SetupRideArm, bs::NativeRideArm};
        r.compareV0 = false;
        rows.push_back(r);
    }
    {
        Row r{"bike_recover_end", "void 0x80072994(Bike*)  [RASHCDG]", bs::kRecoverEnd,
              [](CaseContext& c, Args& a) { bs::SetupRiderCase(c, a, bs::RiderCase::kRecover); }, nullptr};
        r.compareV0 = false;
        r.nativeEx = [](NativeEnv& env, const Args& a) { return bs::NativeRiderCase(env, a, bs::RiderCase::kRecover); };
        r.maxSteps = 20'000'000;
        r.oracleCallees = {bs::KnockOffCallee(), bs::LaunchCallee()};
        rows.push_back(r);
    }
    {
        Row r{"bike_list_migrate", "void 0x80071BCC(Bike*, s32 dt)  [RASHCDG, the list migration]",
              bs::kListMigrateFn,
              [](CaseContext& c, Args& a) { bs::SetupRiderCase(c, a, bs::RiderCase::kMigrate); }, nullptr};
        r.compareV0 = false;
        r.nativeEx = [](NativeEnv& env, const Args& a) { return bs::NativeRiderCase(env, a, bs::RiderCase::kMigrate); };
        r.maxSteps = 20'000'000;
        r.oracleCallees = {bs::KnockOffCallee(), bs::LaunchCallee()};
        rows.push_back(r);
    }
    {
        Row r{"bike_passenger_launch", "void 0x80074D58(Bike*)  [RASHCDG, the passenger launch]",
              bs::kPassengerLaunch,
              [](CaseContext& c, Args& a) { bs::SetupRiderCase(c, a, bs::RiderCase::kLaunch); }, nullptr};
        r.compareV0 = false;
        r.nativeEx = [](NativeEnv& env, const Args& a) { return bs::NativeRiderCase(env, a, bs::RiderCase::kLaunch); };
        r.maxSteps = 20'000'000;
        r.oracleCallees = {bs::KnockOffCallee()};
        rows.push_back(r);
    }
}
