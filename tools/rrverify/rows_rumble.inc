// Bench rows of the rumble: the pad motors SLUS 0x8001DC94 / 0x8001DD08 /
// 0x8001DD74, the actuator service 0x8001DDC4 (libpad through the oracle seam), the pad reader's rumble
// regions (a SPLICE of 0x8001CB3C), PadRumble RASHCDG 0x800B658C with the motors run natively, and the
// countdown's voice 0x800164B4 / 0x80016528 (src\game\sim\pad_motor.h, countdown_voice.h). Included at the
// end of verify_physics.cpp inside its anonymous namespace, after rows_feel.inc (feel::SetupPad), rows_collision.inc
// (co::SetupRumble, co::CloneCallees, co::Refuse), rows_engine_note.inc (EnRun, EnPlantSystem, EnPlantSpu,
// EnHandle) and rows_bike_step.inc (bs::StartCall).
//
// NAMED BOUNDS: the port table's port words stay the capture's (0, 0x10, 2, 3 - libpad indexes its own tables
// with them); the service's pad is 0..3; the splice keeps game_state+0x34 at the capture's one pad (pad 1's
// bike *(P1 + 0x358) is 0 on every captured machine, and the reader's controls dereference it).
namespace rmb {

uint32_t Pick(Rng& r, std::initializer_list<uint32_t> v) { return *(v.begin() + r.Next() % v.size()); }
bool Chance(Rng& r, uint32_t n) { return r.Next() % n == 0; }

uint32_t Clock(CaseContext& c) { return c.mem->PeekWord(c.mem->PeekWord(kGameStatePtr) + 0x0Cu); }

// The four records: the switch +0x08 (on, off, anything), the state +0x04, the buffer, the two stop times
// (none, passed, to come, anything) against a drawn clock.
void PlantPorts(CaseContext& c) {
    Rng& r = *c.rng;
    Memory& m = *c.mem;
    const uint32_t gs = m.PeekWord(kGameStatePtr);
    m.PokeWord(gs + 0x0Cu, Pick(r, {r.U32() % 0x40000u, r.U32(), 0x7FFFFFF0u, 5u}));
    const uint32_t now = m.PeekWord(gs + 0x0Cu);
    for (uint32_t k = 0; k < 4; ++k) {
        const uint32_t rec = rr::sim::kPadPorts + 24u * k;
        m.PokeWord(rec + 4u, Pick(r, {0u, 1u, 2u, r.U32()}));
        m.PokeWord(rec + 8u, Pick(r, {1u, 1u, 0u, r.U32()}));
        const uint8_t buf[2] = {static_cast<uint8_t>(Pick(r, {0u, 1u, 0x40u, r.U32()})), static_cast<uint8_t>(r.U32())};
        m.WriteBlock(rec + 0x0Cu, buf, 2);
        for (uint32_t i = 0; i < 2; ++i)
            m.PokeWord(rec + 0x10u + 4u * i, Pick(r, {0u, 0u, now - 1u - r.U32() % 300u, now + r.U32() % 300u, now, r.U32()}));
    }
}

// ---------------------------------------------------------------- the motors
void SetupMotor(CaseContext& c, Args& a) {
    PlantPorts(c);
    Rng& r = *c.rng;
    a.a[0] = Pick(r, {0u, 1u, 2u, 3u, 0u, 1u, 4u + r.U32() % 4u});
    a.a[1] = Pick(r, {0xFFFFFFFFu, 0u, 150u, 100u, r.U32() % 300u, r.U32(), 0u - r.U32() % 5u});
    a.a[2] = Pick(r, {0xFFFFFFFFu, 150u, 100u, r.U32() % 300u, r.U32()});
    a.a[3] = Pick(r, {0u, 50u, 255u, r.U32() % 256u, r.U32()});
}
uint32_t NativeSmall(NativeEnv& env, const Args& a) {
    rr::sim::GuestRam g(env.clone->mem.ram().data(), env.gp);
    rr::sim::PadMotorSmall(g, a.a[0], static_cast<int32_t>(a.a[1]));
    return RqDone(env, g, 0);
}
uint32_t NativeLarge(NativeEnv& env, const Args& a) {
    rr::sim::GuestRam g(env.clone->mem.ram().data(), env.gp);
    rr::sim::PadMotorLarge(g, a.a[0], static_cast<int32_t>(a.a[1]), static_cast<int32_t>(a.a[2]));
    return RqDone(env, g, 0);
}
uint32_t NativeMotor(NativeEnv& env, const Args& a) {
    rr::sim::GuestRam g(env.clone->mem.ram().data(), env.gp);
    rr::sim::PadMotor(g, a.a[0], static_cast<int32_t>(a.a[1]), static_cast<int32_t>(a.a[2]),
                      static_cast<int32_t>(a.a[3]));
    return RqDone(env, g, 0);
}

// ---------------------------------------------------------------- the actuator service
class BenchLibPad final : public rr::sim::LibPad {
public:
    explicit BenchLibPad(NativeEnv& env) : env_(env) {}
    int32_t GetState(uint32_t port) override {
        return static_cast<int32_t>(env_.CallOracle(rr::sim::kLibPadGetState, port, 0, 0, 0));
    }
    void SetAct(uint32_t port, uint32_t buffer, uint32_t bytes) override {
        env_.CallOracle(rr::sim::kLibPadSetAct, port, buffer, bytes, 0);
    }
    int32_t SetActAlign(uint32_t port, uint32_t table) override {
        return static_cast<int32_t>(env_.CallOracle(rr::sim::kLibPadSetActAlign, port, table, 0, 0));
    }
    int32_t InfoMode(uint32_t port, int32_t term, int32_t offs) override {
        return static_cast<int32_t>(env_.CallOracle(rr::sim::kLibPadInfoMode, port, static_cast<uint32_t>(term),
                                                    static_cast<uint32_t>(offs), 0));
    }

private:
    NativeEnv& env_;
};
// libpad's own state byte +0x49 of its two port records (0x800D9D70 for port 0, 0x800D9E60 for port 0x10 - the
// resolver *(0x80054910) = SLUS 0x80043174), which PadGetState 0x80040550 answers from: the capture's (2, the
// emulator's first-type pad), 6 (a stable pad: the align arm), 1, 0, 3, 4.
void SetupService(CaseContext& c, Args& a) {
    PlantPorts(c);
    Rng& r = *c.rng;
    for (uint32_t rec : {0x800D9D70u, 0x800D9E60u}) {
        const uint8_t st = static_cast<uint8_t>(Pick(r, {c.mem->PeekByte(rec + 0x49u), 6u, 6u, 2u, 1u, 0u, 3u, 4u}));
        c.mem->WriteBlock(rec + 0x49u, &st, 1);
    }
    a.a[0] = Pick(r, {0u, 1u, 0u, 1u, 2u, 3u});
    a.a[1] = Pick(r, {0u, 1u, r.U32()});
}
uint32_t NativeService(NativeEnv& env, const Args& a) {
    rr::sim::GuestRam g(env.clone->mem.ram().data(), env.gp);
    BenchLibPad lp(env);
    const bool ok = rr::sim::PadActuatorService(g, a.a[0], a.a[1], lp);
    if (!ok && env.failure.empty()) env.failure = "the port's view faulted";
    return RqDone(env, g, 0);
}
std::vector<OracleCallee> LibPadSeams() {
    std::vector<OracleCallee> v = {OracleCallee{rr::sim::kLibPadGetState, 1}, OracleCallee{rr::sim::kLibPadSetAct, 3},
                                   OracleCallee{rr::sim::kLibPadSetActAlign, 2},
                                   OracleCallee{rr::sim::kLibPadInfoMode, 3}};
    for (OracleCallee& o : v) o.frame = 32; // `addiu sp,sp,-32` at 0x8001DDC4
    return v;
}

// ---------------------------------------------------------------- the pad reader's rumble regions (SPLICE)
// The setup of `pad_rider_controls` (the reader reaches its player loop with the player's bike under pad
// control), then the region's inputs: the port table and the clock, the bike's +0x1E0 around 0x23C35, +0x2A4
// (0 half the time, INT_MIN now and then), the stats' +0xCC (both signs, 0), +0x2BC around 0x11FFF, +0x184
// bit 0, +0xB4 around 9, +0x140 = 0 now and then, the idle counter around 600, the attract flag now and
// then, and flagsA bit 27 set one case in five (the motors-off region 0x8001DA08).
void SetupEngine(CaseContext& c, Args& a) {
    feel::SetupPad(c, a);
    PlantPorts(c);
    Rng& r = *c.rng;
    Memory& m = *c.mem;
    const uint32_t bike = m.PeekWord(0x8005B38Cu);
    if (Chance(r, 5)) m.PokeWord(bike + 0x230u, m.PeekWord(bike + 0x230u) | 0x08000000u);
    m.PokeWord(0x8005B220u, Chance(r, 16) ? 1u : 0u);
    m.PokeWord(bike + 0x1E0u, Pick(r, {0x23C35u, 0x23C36u, r.U32() % 0x23C35u, 0x23C36u + r.U32() % 0x400000u,
                                       0u - r.U32() % 0x40000u, static_cast<uint32_t>(r.S32() >> (r.U32() % 12u))}));
    m.PokeWord(bike + 0x2A4u, Chance(r, 2) ? 0u : Pick(r, {static_cast<uint32_t>(r.S32() >> (8u + r.U32() % 16u)),
                                                           0x80000000u, r.U32(), 1u, 0xFFFFFFFFu}));
    const uint32_t stats = m.PeekWord(bike + 0x22Cu);
    m.PokeWord(stats + 0xCCu, Pick(r, {0x10BE4u, 0u, 0u - r.U32() % 0x20000u, r.U32() % 0x40000u, r.U32(), 1u}));
    m.PokeWord(bike + 0x2BCu, Pick(r, {0x11FFFu, 0x12000u, r.U32() % 0x30000u, r.U32(), 0u}));
    m.PokeWord(bike + 0x184u, m.PeekWord(bike + 0x184u) ^ (r.U32() & 1u));
    if (Chance(r, 3)) m.PokeWord(bike + 0xB4u, Pick(r, {8u, 9u, 10u}));
    if (Chance(r, 8)) WriteU16Bench(m, bike + 0x140u, 0);
    m.PokeWord(rr::sim::kPadIdleCounters, Pick(r, {0u, 599u, 600u, 601u, r.U32() % 700u, r.U32()}));
}
uint32_t NativeEngine(NativeEnv& env, const Args& a) {
    Machine& cl = *env.clone;
    const uint32_t sp = env.sp;
    bs::SpliceStats::Entry& st = bs::Stats().For("pad_engine_rumble");
    ++st.cases;
    auto fail = [&env](const std::string& why) {
        if (env.failure.empty()) env.failure = why;
    };
    cl.cpu.ClearHooks();
    cl.cpu.AddHook(rr::sim::kPadRumbleEntry, "rumble");
    cl.cpu.AddHook(rr::sim::kPadMotorsOffEntry, "off");
    bs::StartCall(cl.cpu, rr::sim::kPadReaderFn, a, sp);
    int entries = 0;
    for (;;) {
        cl.cpu.callObservations.clear();
        const Trap t = cl.cpu.Run(kSentinel, env.maxSteps);
        env.calls.insert(env.calls.end(), cl.cpu.callObservations.begin(), cl.cpu.callObservations.end());
        cl.cpu.callObservations.clear();
        if (t.kind == TrapKind::Halted) break;
        if (t.kind != TrapKind::Hook || (t.pc != rr::sim::kPadRumbleEntry && t.pc != rr::sim::kPadMotorsOffEntry)) {
            fail("the spliced run trapped: " + t.ToString());
            break;
        }
        if (++entries > 4) {
            fail("the regions were entered more often than the reader has pads");
            break;
        }
        ++st.entered;
        const CpuRegs saved = CaptureRegs(cl.cpu);
        const uint32_t s0 = cl.cpu.regs[16], bike = cl.cpu.regs[19], p = cl.cpu.regs[20], cnt = cl.cpu.regs[22];
        rr::sim::PadRumbleRegs regs;
        regs.s0 = s0;
        if (!env.spliceControl) {
            rr::sim::GuestRam g(cl.mem.ram().data(), env.gp);
            regs = t.pc == rr::sim::kPadRumbleEntry ? rr::sim::PadEngineRumble(g, p, bike, cnt, s0)
                                                    : rr::sim::PadMotorsOff(g, p, cnt, s0);
            if (g.Faulted()) {
                fail("the port's view faulted");
                break;
            }
        }
        RestoreRegs(saved, cl.cpu);
        cl.cpu.regs[16] = regs.s0;
        cl.cpu.pc = regs.exit;
        cl.cpu.npc = regs.exit + 4u;
        cl.cpu.loadDelayReg = Cpu::kNoLoadDelay;
        cl.cpu.loadDelayValue = 0;
        ++st.exitCompared;
    }
    cl.cpu.ClearHooks();
    return cl.cpu.regs[2];
}

// ---------------------------------------------------------------- PadRumble with the motors native
struct MotorEnv final : rr::sim::RumbleEnv {
    co::CloneCallees& cc;
    rr::sim::GuestRam& g;
    MotorEnv(co::CloneCallees& c, rr::sim::GuestRam& gg) : cc(c), g(gg) {}
    bool ReadForeignByte(uint32_t address, uint8_t& value) override { return cc.ReadForeignByte(address, value); }
    bool PadMotor(uint32_t pad, int32_t a, int32_t b, int32_t c, uint32_t) override {
        rr::sim::PadMotor(g, pad, a, b, c); // SLUS 0x8001DD74, PORTED - no oracle
        return !g.Faulted();
    }
};
void SetupRumbleMotors(CaseContext& c, Args& a) {
    co::SetupRumble(c, a);
    PlantPorts(c);
}
uint32_t NativeRumbleMotors(NativeEnv& env, const Args& a) {
    EnRun run(env);
    co::CloneCallees cc(env, run);
    MotorEnv me(cc, run.g);
    const bool ok = rr::sim::PadRumble(run.g, a.a[0], a.a[1], static_cast<int32_t>(a.a[2]), static_cast<int32_t>(a.a[3]),
                                       static_cast<int32_t>(a.a4), env.sp, me);
    return co::Refuse(env, run, ok, "PadRumble");
}

// ---------------------------------------------------------------- the countdown's voice
void SetupCountdownStart(CaseContext& c, Args&) {
    if (!c.sample) EnPlantSystem(c);
    EnPlantSpu(c);
    Rng& r = *c.rng;
    const uint32_t gs = c.mem->PeekWord(kGameStatePtr);
    EnPut(c, gs + 0x30u, Pick(r, {2u, 2u, 2u, 1u, 0u, r.U32()}));
    EnPut(c, 0x800D6C0Cu, Pick(r, {65u, r.U32() % 129u, r.U32()}));
    EnPut(c, c.gp + 1964u, r.U32());
}
void SetupCountdownStop(CaseContext& c, Args&) {
    if (!c.sample) EnPlantSystem(c);
    EnPlantSpu(c);
    EnPut(c, c.gp + 1964u, EnHandle(c));
}

} // namespace rmb

void AddRumbleRows(std::vector<Row>& rows) {
    auto ex = [&rows](const char* name, const char* sig, uint32_t entry, std::function<void(CaseContext&, Args&)> setup,
                      std::function<uint32_t(NativeEnv&, const Args&)> native) {
        rows.push_back(Row{name, sig, entry, std::move(setup), nullptr});
        Row& r = rows.back();
        r.compareV0 = false;
        r.nativeEx = std::move(native);
    };
    ex("pad_motor_small", "void 0x8001DC94(pad, frames)  [SLUS, the small motor]", rr::sim::kPadMotorSmallFn,
       rmb::SetupMotor, rmb::NativeSmall);
    ex("pad_motor_large", "void 0x8001DD08(pad, frames, strength)  [SLUS, the large motor]", rr::sim::kPadMotorLargeFn,
       rmb::SetupMotor, rmb::NativeLarge);
    ex("pad_motor", "void 0x8001DD74(pad, smallFrames, largeFrames, strength)  [SLUS, the pad motors]",
       rr::sim::kPadMotorFn, rmb::SetupMotor, rmb::NativeMotor);
    ex("pad_actuator_service", "void 0x8001DDC4(pad, single)  [SLUS, the actuator service; libpad through the seam]",
       rr::sim::kPadActuatorFn, rmb::SetupService, rmb::NativeService);
    rows.back().oracleCallees = rmb::LibPadSeams();
    ex("pad_engine_rumble",
       "void 0x8001CB3C()  [SLUS, the pad reader; SPLICE of [0x8001D7DC, 0x8001DA08) and [0x8001DA08, 0x8001DA2C)]",
       rr::sim::kPadReaderFn, rmb::SetupEngine, rmb::NativeEngine);
    rows.back().maxSteps = 20'000'000;
    ex("pad_rumble_motors", "void 0x800B658C(e, other, speed, k, [div])  [RASHCDG, with SLUS 0x8001DD74 native]",
       0x800B658Cu, rmb::SetupRumbleMotors, rmb::NativeRumbleMotors);
    ex("countdown_voice_start", "void 0x800164B4()  [SLUS, the two-player countdown voice]",
       rr::sim::kCountdownVoiceStartFn, rmb::SetupCountdownStart, [](NativeEnv& env, const Args&) {
           EnRun run(env);
           rr::sim::CountdownVoiceStart(run.s);
           return run.Done(env, 0);
       });
    rows.back().compareSpuControl = true;
    ex("countdown_voice_stop", "void 0x80016528()  [SLUS, StopVoice(gp+1964)]", rr::sim::kCountdownVoiceStopFn,
       rmb::SetupCountdownStop, [](NativeEnv& env, const Args&) {
           EnRun run(env);
           rr::sim::CountdownVoiceStop(run.s);
           return run.Done(env, 0);
       });
    rows.back().compareSpuControl = true;
}
