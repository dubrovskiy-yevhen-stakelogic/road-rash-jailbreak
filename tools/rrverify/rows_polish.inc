// Bench rows of the pad reader's per-player L2 arm. Included by verify_physics.cpp inside its
// anonymous namespace, after rows_feel.inc (feel::SetupPad, feel::Ex), rows_bike_step.inc (bs::) and
// rows_hud.inc (hudrows::Bike), whose helpers it reuses.
//
// polish_taunt_arm - SPLICE ROW of SLUS 0x8001CB3C (the pad reader, whole): the L2 arm of its player loop,
// [0x8001D51C, 0x8001D5AC] (s2 = the loop's frame-stable pad record, s3 = its bike), is replaced by the
// port (pad_reader.h PadTauntPress / PadTauntHeld) in two halves around its one call: at 0x8001D51C the
// port decides the call - RiderSpeech 0x8001A760(bike +0xAC, 0) then runs as the ORIGINAL's jal on the
// clone (an oracle callee) - or goes straight on; at 0x8001D550 (the call's return, or directly) the port
// does the held-slot flag and the clone continues at 0x8001D700 with v0 / v1 / a0 as the original's path
// leaves them. The framework compares the whole spliced run with the original's.
// Setup: feel::SetupPad (game_state[0] = 1, player 1's live record's slots, stamps and codes, the bike
// under pad control, the rider's mount 0..4), then, in two cases of three, PLAYER 2 as the two-player race
// has it: game_state +0x30 / +0x34 = 2, *(0x8005B21C) = a live bike of the snapshot with a live rider (the
// reader's loop takes it for p = 1; pool slot 1's is dormant in rr-race), under pad control, its rider's
// mount 0..1 (on the bike: the arm is on the on-bike path) and +0x228 stirred, and the live
// record 1 (0x800D6DE0 + 192) = record 0's with its own slot stamps and codes. Control 8's slot (the
// remap +0x20) gets a press code > 0 in half the cases, else 0 / -1.
// NAMED BOUNDS: feel::SetupPad's (the two kernel syscalls nopped, control 1 with no press code, a mounted
// rider keeps the machine's stance).
namespace polish {

using co::Chance;
using co::Pick;

constexpr uint32_t kPlayer2Bike = 0x8005B21C;

struct TauntStats {
    long cases = 0, arm[2] = {0, 0}, calls[2] = {0, 0};
    ~TauntStats() {
        if (cases != 0)
            std::printf("splice polish_taunt_arm cases %ld, the arm entered for player 1 %ld / player 2 %ld, "
                        "RiderSpeech asked %ld / %ld\n",
                        cases, arm[0], arm[1], calls[0], calls[1]);
    }
};
TauntStats& Taunts() {
    static TauntStats s;
    return s;
}

void StirSlots(Memory& m, Rng& rng, uint32_t live) {
    for (uint32_t k = 0; k < 19; ++k) {
        const uint32_t s = live + 0x14u + 8u * k;
        m.PokeWord(s, Pick(rng, {0u, 0u, 0u, 0x1DF1u, rng.U32() % 0x3000u, 0u - (rng.U32() % 0x100u)}));
        const uint8_t cd = static_cast<uint8_t>(Pick(rng, {0u, 0u, 1u, 2u, 3u, 0xFFu}));
        m.WriteBlock(s + 6u, &cd, 1);
    }
    const uint32_t remap = m.PeekWord(live + 0xB8u);
    const uint8_t zero = 0;
    m.WriteBlock(live + 0x1Au + 8u * m.PeekWord(remap + 4u), &zero, 1); // control 1: no pause
    const uint32_t l2 = live + 0x14u + 8u * m.PeekWord(remap + 0x20u);   // control 8: the taunt
    const uint8_t code = static_cast<uint8_t>(Chance(rng, 2) ? Pick(rng, {1u, 2u, 3u}) : Pick(rng, {0u, 0xFFu}));
    m.WriteBlock(l2 + 6u, &code, 1);
    m.PokeWord(l2, Pick(rng, {0u, 0x1DF1u, rng.U32() % 0x3000u, 0u - (rng.U32() % 0x100u)}));
}

void SetupTaunt(CaseContext& c, Args& a) {
    feel::SetupPad(c, a);
    Memory& m = *c.mem;
    Rng& rng = *c.rng;
    StirSlots(m, rng, rr::sim::kPadRecordsLive);
    if (Chance(rng, 3)) return; // one player: 0x8005B21C as the capture has it
    // player 2's bike: a live bike of the snapshot with a live rider (pool slot 1's is dormant in rr-race)
    std::vector<uint32_t> live;
    const uint32_t b1 = m.PeekWord(0x8005B38Cu);
    for (uint32_t k = 1; k < 16u; ++k) {
        const uint32_t b = hudrows::Bike(m, k);
        const uint32_t r = m.PeekWord(b + 0x354u);
        if (b == b1 || (m.PeekWord(b + 0x140u) & 0xFFFFu) == 0 || r < 0x80000000u || r >= 0x80200000u) continue;
        if ((m.PeekWord(r + 0x140u) & 0xFFFFu) != 0) live.push_back(b);
    }
    if (live.empty()) return;
    const uint32_t b2 = live[rng.U32() % live.size()];
    m.PokeWord(kPlayer2Bike, b2);
    const uint32_t gs = m.PeekWord(0x8005B2F8u); // the pads the loop walks (gs+0x34) and the players (+0x30)
    m.PokeWord(gs + 0x34u, 2u);
    m.PokeWord(gs + 0x30u, 2u);
    m.PokeWord(b2 + 0x230u, (m.PeekWord(b2 + 0x230u) ^ (rng.U32() & 0x7EFu)) & ~0x08000000u);
    const uint32_t r2 = m.PeekWord(b2 + 0x354u);
    if (r2 >= 0x80000000u && r2 < 0x80200000u) {
        m.PokeWord(r2 + 0x25Cu, Pick(rng, {0u, 1u}));
        m.PokeWord(r2 + 0x228u, m.PeekWord(r2 + 0x228u) ^ (rng.U32() & 0x01FFFF80u));
    }
    const uint32_t live1 = rr::sim::kPadRecordsLive + 192u;
    for (uint32_t o = 0; o < 192u; o += 4u) m.PokeWord(live1 + o, m.PeekWord(rr::sim::kPadRecordsLive + o));
    m.PokeWord(live1 + 0x10u, 0); // digital
    StirSlots(m, rng, live1);
}

uint32_t NativeTaunt(NativeEnv& env, const Args& a) {
    Machine& cl = *env.clone;
    TauntStats& st = Taunts();
    ++st.cases;
    auto fail = [&env](const std::string& why) {
        if (env.failure.empty()) env.failure = why;
    };
    cl.cpu.ClearHooks();
    cl.cpu.AddHook(rr::sim::kPadTauntEntry, "taunt");
    cl.cpu.AddHook(rr::sim::kPadTauntHeld, "held");
    bs::StartCall(cl.cpu, rr::sim::kPadReaderFn, a, env.sp);
    auto jump = [&cl](uint32_t pc) {
        cl.cpu.pc = pc;
        cl.cpu.npc = pc + 4u;
        cl.cpu.loadDelayReg = Cpu::kNoLoadDelay;
        cl.cpu.loadDelayValue = 0;
    };
    for (int guard = 0; guard < 16; ++guard) {
        cl.cpu.callObservations.clear();
        const Trap t = cl.cpu.Run(kSentinel, env.maxSteps);
        env.calls.insert(env.calls.end(), cl.cpu.callObservations.begin(), cl.cpu.callObservations.end());
        cl.cpu.callObservations.clear();
        if (t.kind == TrapKind::Halted) break;
        if (t.kind != TrapKind::Hook) {
            fail("the spliced run trapped: " + t.ToString());
            break;
        }
        const uint32_t rec = cl.cpu.regs[18], bike = cl.cpu.regs[19];
        const uint32_t p = rec == rr::sim::kPadRecordsFrame + 192u ? 1u : 0u;
        rr::sim::GuestRam g(cl.mem.ram().data(), env.gp);
        auto held = [&]() {
            if (env.spliceControl) { // the control: the arm's second half not done
                jump(rr::sim::kPadExitCamera);
                return;
            }
            const rr::sim::PadTauntRegs r = rr::sim::PadTauntHeld(g, rec, bike);
            cl.cpu.regs[2] = r.v0;
            cl.cpu.regs[3] = r.v1;
            cl.cpu.regs[4] = r.a0;
            jump(rr::sim::kPadExitCamera);
        };
        if (t.pc == rr::sim::kPadTauntEntry) {
            ++st.arm[p];
            uint32_t handle = 0, v0 = 0;
            const bool call = rr::sim::PadTauntPress(g, rec, bike, handle, v0);
            cl.cpu.regs[2] = v0;
            if (call && !env.spliceControl) {
                ++st.calls[p];
                cl.cpu.regs[4] = handle;
                jump(rr::sim::kPadTauntCall); // the original's jal RiderSpeech (its delay slot: a1 = 0)
            } else {
                held();
            }
        } else if (t.pc == rr::sim::kPadTauntHeld) {
            held();
        } else {
            fail("an unexpected hook");
            break;
        }
        if (g.Faulted()) {
            fail("the port's view faulted");
            break;
        }
    }
    cl.cpu.ClearHooks();
    return cl.cpu.regs[2];
}

} // namespace polish

void AddPolishRows(std::vector<Row>& rows) {
    feel::Ex(rows, "polish_taunt_arm",
             "void 0x8001CB3C()  [SLUS, the pad reader; SPLICE of the L2 arm [0x8001D51C, 0x8001D5AC] for both players]",
             rr::sim::kPadReaderFn, polish::SetupTaunt, polish::NativeTaunt, false);
    rows.back().maxSteps = 20'000'000;
}
