// Bench rows for the camera's collision step, the pad's analogue axes, the walking
// rider's contacts and the bike's own parts (src\game\sim\camera_collide.*, ...). Included LAST by
// verify_physics.cpp inside its anonymous namespace, so the helpers of rows_collision.inc (co::) and
// rows_coll_resolve.inc (crs::) are visible.

namespace feel {

using co::Chance;
using co::Pick;

int32_t Units(Rng& rng, int32_t units) {
    return static_cast<int32_t>(rng.U32() % (2u * 65536u * static_cast<uint32_t>(units))) - 65536 * units;
}

// Volumes 0..n-1 of pool 6 listed as the collision pass's NoteBigVolume lists them (0x800CCF88, the
// count at 0x800CCF90); now and then the count 0 or one id listed twice.
void ListVolumes(CaseContext& c, uint32_t n) {
    Rng& rng = *c.rng;
    Memory& m = *c.mem;
    uint32_t cnt = n;
    for (uint32_t i = 0; i < 8; ++i) {
        const uint8_t id = static_cast<uint8_t>(0xC0u | (n ? (i < n ? i : rng.U32() % n) : 0u));
        m.WriteBlock(0x800CCF88u + i, &id, 1);
    }
    if (n > 0 && Chance(rng, 6)) cnt = 0;
    else if (n > 1 && Chance(rng, 3)) cnt = n + 1u;
    m.PokeWord(0x800CCF90u, cnt);
}

// Volume k's box (centre and eight corners) moved so that its centre is p + off.
void MoveVolume(Memory& m, uint32_t k, const int32_t p[3], const int32_t off[3]) {
    const uint32_t r = m.PeekWord(rr::sim::kPoolTableAddr + 16u * 6u) + 280u * k;
    int32_t d[3];
    for (uint32_t i = 0; i < 3; ++i) d[i] = p[i] + off[i] - GetS32(m, r + 12 + 4 * i);
    for (uint32_t f = 12; f < 120; f += 12)
        for (uint32_t i = 0; i < 3; ++i) PutS32(m, r + f + 4 * i, GetS32(m, r + f + 4 * i) + d[i]);
}

// ---------------------------------------------------------------- CameraCollide, 0x800A421C
// View record 0 of THIS machine (rr-race: the chase camera behind the player), in place, its eye:
//   0 as captured (2.9 .. 3.9 units behind the player's aim point);
//   1 against one of the wall segments the view scans (-3.0 .. 3.0 units either side of the segment's
//     line, -0.2 .. 1.2 along it: the walls of the view's road section);
//   2 inside or beside a static volume of our own making (co::PlantVolumes, pool 6, listed);
//   3 below or above the ground: the player's contact height +-2.0 (0..8 box corners behind the plane).
// +0x1D4 (last frame's eye) is put back along the heading +0x1C2 by up to 4.0. Drawn besides: +0x184
// bit 0 (the terrain query or the road slice), +0x218 (the query hint), +0x174 (0: surface 1), +0x1E0
// 0 or not (the settle speed), +0x224 (bit 18, bits 9..11), +0x21C 13/14 (ViewTrack's early return),
// the handle 0x9F / 0x9E (views 0 / 1, both pool 4 slot >= 30), the frame dt 0x800CCE38.
// NAMED BOUND: the entity is a view record (RaceStep, the only caller, hands it one).
void SetupCamCollide(CaseContext& c, Args& a) {
    Rng& rng = *c.rng;
    Memory& m = *c.mem;
    const uint32_t v = rr::sim::kCollViews;
    const uint32_t P = co::Pool0(m, 0);
    const uint32_t mode = static_cast<uint32_t>(rng.Next() % 4u);
    int32_t eye[3] = {GetS32(m, v + 184), GetS32(m, v + 188), GetS32(m, v + 192)};
    if (mode == 1) {
        const uint32_t S = m.PeekWord(v + 0x148), q = m.PeekWord(v + 0x1EC);
        const uint32_t segs = m.PeekWord(S + 0x60), grps = m.PeekWord(S + 0x64);
        const bool left = Chance(rng, 2);
        const uint32_t g0 = m.PeekByte(q + (left ? 0u : 2u)), gn = m.PeekByte(q + (left ? 1u : 3u));
        const uint32_t gi = g0 + (gn ? rng.U32() % gn : 0u);
        const uint32_t gr = grps + 4u * gi;
        const int32_t first = GetS16(m, gr);
        const uint32_t count = m.PeekByte(gr + 3);
        const uint32_t seg = segs + 40u * (static_cast<uint32_t>(first) + (count ? rng.U32() % count : 0u));
        int32_t p0[3], p1[3];
        GetVec3S32(m, seg + 8, p0);
        GetVec3S32(m, seg + 20, p1);
        int16_t n[3];
        GetVec3S16(m, seg + 32, n);
        const int64_t tq = static_cast<int64_t>(rng.U32() % 0x16666u) - 0x3333;
        const int64_t side = static_cast<int64_t>(rng.U32() % 0x60000u) - 0x30000;
        for (uint32_t k : {0u, 2u})
            eye[k] = static_cast<int32_t>(p0[k] + (((static_cast<int64_t>(p1[k]) - p0[k]) * tq) >> 16) +
                                          ((side * n[k]) >> 12));
        if (Chance(rng, 2)) eye[1] = static_cast<int32_t>(p0[1] + (((static_cast<int64_t>(p1[1]) - p0[1]) * tq) >> 16)) - 0x1B333;
    } else if (mode == 3) {
        eye[1] = GetS32(m, P + 0x1FC) + Units(rng, 2);
        eye[0] += Units(rng, 1);
        eye[2] += Units(rng, 1);
    }
    for (uint32_t k = 0; k < 3; ++k) PutS32(m, v + 184 + 4 * k, eye[k]);
    // the volumes: around the eye for mode 2, elsewhere now and then
    uint32_t n6 = 0;
    if (mode == 2 || Chance(rng, 4)) {
        n6 = 1u + rng.U32() % 3u;
        co::PlantVolumes(c, v, n6);
        if (mode == 2) {
            const int32_t off[3] = {Units(rng, 2), Chance(rng, 2) ? -0x8000 - Units(rng, 1) / 2 : Units(rng, 1), Units(rng, 2)};
            MoveVolume(m, rng.U32() % n6, eye, off);
        }
    }
    ListVolumes(c, n6);
    // last frame's eye, behind along a heading
    int16_t h[3];
    co::Dir(rng, h);
    if (Chance(rng, 2)) for (uint32_t k = 0; k < 3; ++k) h[k] = GetS16(m, v + 450 + 2 * k);
    for (uint32_t k = 0; k < 3; ++k) WriteU16Bench(m, v + 450 + 2 * k, static_cast<uint16_t>(h[k]));
    const int32_t back = Chance(rng, 5) ? 0 : static_cast<int32_t>(rng.U32() % 0x40000u);
    for (uint32_t k = 0; k < 3; ++k)
        PutS32(m, v + 468 + 4 * k, eye[k] - static_cast<int32_t>((static_cast<int64_t>(h[k]) * back) >> 12));
    // the view's own words
    if (Chance(rng, 3)) m.PokeWord(v + 388, m.PeekWord(v + 388) ^ 1u);
    if (Chance(rng, 3)) m.PokeWord(v + 536, Pick(rng, {0u, 0u, 0xFFFFFFFFu, rng.U32()}));
    if (Chance(rng, 6)) m.PokeWord(v + 372, 0);
    m.PokeWord(v + 480, Pick(rng, {0u, 0u, 0x1C9C4u, rng.U32() % 0x400000u}));
    uint32_t f = m.PeekWord(v + 548);
    if (Chance(rng, 4)) f ^= 0x40000u;
    if (Chance(rng, 3)) f |= rng.U32() & 0xE00u;
    m.PokeWord(v + 548, f);
    if (Chance(rng, 6)) m.PokeWord(v + 540, Pick(rng, {13u, 14u, 12u, 15u}));
    if (Chance(rng, 5)) WriteU16Bench(m, v + 172, 0x9Eu);
    if (Chance(rng, 3)) PutS32(m, v + 632, rng.S32() >> (8 + rng.Next() % 12u)); // +0x278: ViewTrack(v, 0)'s distance
    m.PokeWord(rr::sim::kCollDt, Pick(rng, {0x884u, 0x884u, 0x442u, 0u, rng.U32() % 0x4000u}));
    a.a[0] = v;
}
uint32_t NativeCamCollide(NativeEnv& env, const Args& a) {
    EnRun run(env);
    co::CloneCallees cc(env, run);
    const cr::Tables tab(env.clone->mem, env.gp);
    uint32_t v0 = 0;
    const bool ok = rr::sim::CameraCollide(run.g, a.a[0], env.sp, tab.t, cc, v0);
    if (!ok && env.failure.empty() && !run.s.Faulted()) env.failure = "CameraCollide refused";
    return run.Done(env, v0);
}

// ---------------------------------------------------------------- PropVsShape, 0x800B2E64
// e: the view record (as in camera_collide) or one of this machine's bikes (the player or bike 6),
// against a planted pool-6 volume moved onto e's box centre (+-1.5 units) or left where PlantVolumes
// put it. Drawn: the view's handle 0x9F / 0x9E, a passenger (+0x358) with or without a list node for a
// bike.
// NAMED BOUNDS: a bike's passenger is one of this machine's bikes (ApplyImpulse moves it). +0x250 bit 1
// is kept clear: it sends the call to PropTopple 0x800B3344 (unported, run from the original code),
// which - like under wall_contact - reads a depth word of stack the original's earlier callees left
// below this frame and the port does not reproduce (measured: 133 of 1160 cases differed, every one a
// PropTopple call, every difference in e's own box). The collision pass hands this function a prop
// (pool 4, empty on this machine and in the product) and RaceStep's CameraCollide a view record, which
// returns before that arm.
void SetupPropShape(CaseContext& c, Args& a) {
    Rng& rng = *c.rng;
    Memory& m = *c.mem;
    const bool view = Chance(rng, 2);
    const uint32_t e = view ? rr::sim::kCollViews : co::Pool0(m, Pick(rng, {0u, 0u, 6u}));
    const uint32_t n6 = 1u + rng.U32() % 3u;
    co::PlantVolumes(c, e, n6);
    const uint32_t k = rng.U32() % n6;
    if (!Chance(rng, 4)) {
        const int32_t p[3] = {GetS32(m, e + 184), GetS32(m, e + 188), GetS32(m, e + 192)};
        const int32_t off[3] = {Units(rng, 1) * 3 / 2, Units(rng, 1), Units(rng, 1) * 3 / 2};
        MoveVolume(m, k, p, off);
    }
    if (view && Chance(rng, 3)) WriteU16Bench(m, e + 172, 0x9Eu);
    if (!view) {
        m.PokeWord(e + 592, m.PeekWord(e + 592) & ~2u);
        if (Chance(rng, 5)) {
            const uint32_t b17 = co::Pool0(m, 17);
            m.PokeWord(e + 856, b17);
            m.PokeWord(b17 + 856, e);
            m.PokeWord(b17 + 1088, 0);
        }
    }
    m.PokeWord(e + 480, Pick(rng, {0u, 0x80000u, rng.U32() % 0x400000u}));
    a.a[0] = e;
    a.a[1] = m.PeekWord(rr::sim::kPoolTableAddr + 16u * 6u) + 280u * k;
}
uint32_t NativePropShape(NativeEnv& env, const Args& a) {
    EnRun run(env);
    co::CloneCallees cc(env, run);
    uint32_t v0 = 0;
    const bool ok = rr::sim::PropVsShape(run.g, a.a[0], a.a[1], env.sp, cc, v0);
    if (!ok && env.failure.empty() && !run.s.Faulted()) env.failure = "PropVsShape refused";
    return run.Done(env, v0);
}

// ---------------------------------------------------------------- the pad reader's rider controls
// SPLICE ROW of SLUS 0x8001CB3C (the pad reader, whole): the original runs on the clone up to the
// region [0x8001CFB0, exit), the port (pad_reader.h) replaces the region, the clone then continues
// the original from the exit the port names; a private reference run of the original stops at its
// first exit, and the state there is compared with the spliced one (the same exit, the whole RAM
// outside the stack window, the scratchpad). The framework then compares the two whole runs.
// Setup (THIS machine: game_state[0] = 1, one player, the player's bike under pad control): the LIVE
// record 0x800D6DE0 (the reader copies it to 0x800D7128 first): the analogue flag +0x10 (a 0x73 pad
// now and then) and its three stick bytes, the 19 slots' hold stamps (0, positive, negative) and
// codes; +0xB4 = 2 now and then (the d-pad steering override); the bike's +0x230 bits (bit 27 kept
// clear) and +0x350; the rider's mount state +0x25C 0..4, +0x140 0 now and then, a walking stance
// (category 8 of 0x800541D4) or another when off the bike, riderDef +0x28, +0x228; the axes table.
// NAMED BOUNDS: the kernel's two critical-section stubs (SLUS 0x80043DA4 / 0x80043DB4: `li a0,n;
// syscall`) have their `syscall` replaced by a nop in the case's RAM on both sides - the bench has no
// interrupts to hold off and the reader ignores their v0; control 1's slot (the pause) has code 0
// (a press switches game_state[0] to the pause, which leaves the loop before the region); a mounted
// rider keeps the machine's stance (another bank's stance traps the original's stance layer under
// the combat decode that follows the region).
uint32_t WalkStance(Memory& m, Rng& rng) {
    std::vector<uint32_t> walk;
    for (uint32_t k = 0; k < 400; ++k)
        if ((m.PeekWord(0x800541D4u + 8u * k) >> 16) == 8u) walk.push_back(k);
    return walk.empty() ? 0u : walk[rng.U32() % walk.size()];
}
void SetupPad(CaseContext& c, Args& a) {
    Rng& rng = *c.rng;
    Memory& m = *c.mem;
    m.PokeWord(0x80043DA8u, 0);                                             // the two syscalls: nop
    m.PokeWord(0x80043DB8u, 0);
    const uint32_t live = rr::sim::kPadRecordsLive;
    const uint32_t bike = m.PeekWord(0x8005B38Cu);
    const uint32_t rider = m.PeekWord(bike + 0x354u);
    const bool analog = Chance(rng, 3);
    m.PokeWord(live + 0x10u, analog ? 1u : 0u);
    for (uint32_t k = 8; k < 11; ++k) {
        const uint8_t b = static_cast<uint8_t>(Pick(rng, {0x7Fu, 0x80u, 0u, 0xFFu, 0x7Eu, rng.U32() & 0xFFu}));
        m.WriteBlock(live + k, &b, 1);
    }
    for (uint32_t k = 0; k < 19; ++k) {
        const uint32_t s = live + 0x14u + 8u * k;
        m.PokeWord(s, Pick(rng, {0u, 0u, 0u, 0x1DF1u, rng.U32() % 0x3000u, 0u - (rng.U32() % 0x100u)}));
        const uint8_t cd = static_cast<uint8_t>(Pick(rng, {0u, 0u, 1u, 2u, 3u, 0xFFu}));
        const uint8_t fr = static_cast<uint8_t>(rng.U32() % 32u);
        m.WriteBlock(s + 6u, &cd, 1);
        m.WriteBlock(s + 5u, &fr, 1);
    }
    const uint32_t remap = m.PeekWord(live + 0xB8u);
    const uint8_t zero = 0;
    m.WriteBlock(live + 0x1Au + 8u * m.PeekWord(remap + 4u), &zero, 1);    // control 1: no pause
    if (Chance(rng, 3)) m.PokeWord(live + 0xB4u, 2u);
    uint32_t f = m.PeekWord(bike + 0x230u);
    f ^= rng.U32() & 0x7EFu;
    f &= ~0x08000000u;
    m.PokeWord(bike + 0x230u, f);
    const uint8_t n350 = static_cast<uint8_t>(Pick(rng, {0u, 1u, 3u, 0xFFu}));
    m.WriteBlock(bike + 0x350u, &n350, 1);
    const uint32_t mount = Pick(rng, {0u, 1u, 1u, 2u, 3u, 4u, 4u, 4u});
    m.PokeWord(rider + 0x25Cu, mount);
    if (Chance(rng, 8)) WriteU16Bench(m, rider + 0x140u, 0);
    if (mount >= 2) {
        if (!Chance(rng, 4)) WriteU16Bench(m, rider + 0x220u, static_cast<uint16_t>(WalkStance(m, rng)));
        m.PokeWord(m.PeekWord(bike + 0x43Cu) + 0x28u, Chance(rng, 5) ? 1u : 0u);
    }
    m.PokeWord(rider + 0x228u, m.PeekWord(rider + 0x228u) ^ (rng.U32() & 0x01FFFF80u));
    for (uint32_t k = 0; k < 2; ++k)
        m.PokeWord(rr::sim::kPadAxesTable + 4u * k, Chance(rng, 2) ? 0u : static_cast<uint32_t>(rng.S32() >> 14));
    a.a[0] = 0;
}
uint32_t NativePad(NativeEnv& env, const Args& a) {
    Machine& cl = *env.clone;
    const uint32_t sp = env.sp;
    bs::SpliceStats::Entry& st = bs::Stats().For("pad_rider_controls");
    ++st.cases;
    auto fail = [&env](const std::string& why) {
        if (env.failure.empty()) env.failure = why;
    };
    const uint32_t exits[3] = {rr::sim::kPadExitMerged, rr::sim::kPadExitCamera, rr::sim::kPadExitIdle};
    std::vector<uint8_t> refRam, refSpad;
    uint32_t refExit = 0;
    {
        Machine ref;
        bs::CopyMachine(cl, ref);
        for (uint32_t x : exits) ref.cpu.AddHook(x, "exit");
        bs::StartCall(ref.cpu, rr::sim::kPadReaderFn, a, sp);
        const Trap t = ref.cpu.Run(kSentinel, env.maxSteps);
        if (t.kind == TrapKind::Hook) {
            refExit = t.pc;
            refRam = ref.mem.ram();
            refSpad = ref.mem.scratchpad();
        } else if (t.kind != TrapKind::Halted) {
            fail("the reference run of the original trapped: " + t.ToString());
            return 0;
        }
    }
    cl.cpu.ClearHooks();
    cl.cpu.AddHook(rr::sim::kPadRegionEntry, "entry");
    bs::StartCall(cl.cpu, rr::sim::kPadReaderFn, a, sp);
    int entries = 0;
    for (;;) {
        cl.cpu.callObservations.clear();
        const Trap t = cl.cpu.Run(kSentinel, env.maxSteps);
        env.calls.insert(env.calls.end(), cl.cpu.callObservations.begin(), cl.cpu.callObservations.end());
        cl.cpu.callObservations.clear();
        if (t.kind == TrapKind::Halted) break;
        if (t.kind != TrapKind::Hook || t.pc != rr::sim::kPadRegionEntry) {
            fail("the spliced run trapped: " + t.ToString());
            break;
        }
        if (++entries > 1) {
            fail("the region's entry was reached twice in one call (one player)");
            break;
        }
        ++st.entered;
        const CpuRegs saved = CaptureRegs(cl.cpu);
        const uint32_t rec = cl.cpu.regs[18], bike = cl.cpu.regs[19], p = cl.cpu.regs[20], s8 = cl.cpu.regs[30];
        uint32_t exit = 0;
        if (!env.spliceControl) {
            rr::sim::GuestRam g(cl.mem.ram().data(), env.gp);
            if (!rr::sim::PadRiderControls(g, p, rec, bike, rr::sim::kPadRecordsLive + s8, exit)) {
                fail("the port refused the region (view fault)");
                break;
            }
        } else {
            exit = rr::sim::kPadExitCamera;
        }
        RestoreRegs(saved, cl.cpu);
        if (exit == rr::sim::kPadExitMerged) {                              // live into 0x8001D350
            cl.cpu.regs[3] = GetU32V(cl.mem.ram(), (bike + 0x230u) & 0x1FFFFFu);
            cl.cpu.regs[2] = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(cl.mem.ram()[(bike + 0x3B2u) & 0x1FFFFFu])));
        }
        cl.cpu.pc = exit;
        cl.cpu.npc = exit + 4u;
        cl.cpu.loadDelayReg = Cpu::kNoLoadDelay;
        cl.cpu.loadDelayValue = 0;
        if (refExit == 0) {
            fail("the clone entered the region but the reference never reached an exit");
            break;
        }
        ++st.exitCompared;
        if (exit != refExit) {
            char buf[120];
            std::snprintf(buf, sizeof(buf), "the port left by 0x%08X, the original by 0x%08X", exit, refExit);
            fail(buf);
            break;
        }
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
            char buf[240];
            const uint32_t o = first & 0x1FFFFCu;
            std::snprintf(buf, sizeof(buf),
                          "at the exit 0x%08X the spliced state differs in %zu byte(s), first 0x%08X (word: original "
                          "0x%08X, port 0x%08X; analogue %u)",
                          exit, differ, first, GetU32V(refRam, o), GetU32V(B, o), GetU32V(B, (rec + 0x10u) & 0x1FFFFFu));
            fail(buf);
            break;
        }
    }
    cl.cpu.ClearHooks();
    if (entries == 0 && refExit != 0) fail("the reference reached the region but the clone never entered it");
    return cl.cpu.regs[2];
}

// ---------------------------------------------------------------- PedHit, 0x800A9868
// e: one of this machine's live riders (airrows::AirDrawRider: the players drawn, the mount state
// 2..4) or now and then a pedestrian (pool 2); its stance now and then one of the arms' (60..63,
// 67..70, 43, 45, 51, 53, 57, a kind-5 stance) through airrows::AirSetStance (the animation object
// moved to that stance's bank, as the gate keeps it), the rest resident. n and n2: unit directions
// (n2 = n half the time); the speed at and around 0x23C36 and 0x8F0D8; `other` 0, e's own +0xAC or
// another's; force 0 / 1; +0x228 bits 11, 29, 30, 3/4, 20 and 0x100018; +0x130 / +0x134 (the turn
// rate's half extent) both signs; the rider's old speed; the view record's +0x304 / +0x224 bit 15.
// NAMED BOUNDS: a stance is set only when resident (a clip of a bank not loaded traps the original's
// stance layer); the running op +0x0C is one of the program's five (AirSetStance's bound).
void SetupPedHit(CaseContext& c, Args& a) {
    Rng& rng = *c.rng;
    Memory& m = *c.mem;
    uint32_t e = airrows::AirDrawRider(c);
    if (Chance(rng, 5)) e = m.PeekWord(rr::sim::kPoolTableAddr + 32u) + 572u * (rng.U32() % 3u);
    if ((GetU32V(m.ram(), (e + 172u) & 0x1FFFFFu) >> 5 & 0x7FFu) == 1u && Chance(rng, 3)) {
        uint32_t st = Pick(rng, {60u, 61u, 62u, 63u, 67u, 68u, 69u, 70u, 43u, 45u, 51u, 53u, 57u, 0u});
        if (st == 0u) st = airrows::AirKindFive(c);
        if (st != 0u && animrows::Resident(m, st)) airrows::AirSetStance(c, e, st);
    }
    const uint32_t S = c.scratch;
    int16_t n[3];
    co::Dir(rng, n);
    if (Chance(rng, 2)) n[1] = static_cast<int16_t>(n[1] / 8);
    PutVec3S16(m, S, n);
    int16_t n2[3];
    if (Chance(rng, 2)) for (uint32_t k = 0; k < 3; ++k) n2[k] = n[k];
    else co::Dir(rng, n2);
    PutVec3S16(m, S + 8u, n2);
    uint32_t f = m.PeekWord(e + 552u);
    for (uint32_t bit : {0x800u, 0x20000000u, 0x40000000u, 0x8u, 0x10u, 0x100000u})
        if (Chance(rng, 3)) f ^= bit;
    m.PokeWord(e + 552u, f);
    if (Chance(rng, 3)) m.PokeWord(e + 304u, static_cast<uint32_t>(rng.S32() >> (10 + rng.Next() % 10u)));
    if (Chance(rng, 3)) m.PokeWord(e + 308u, static_cast<uint32_t>(rng.S32() >> (10 + rng.Next() % 10u)));
    m.PokeWord(e + 480u, Pick(rng, {0u, 0x10000u, 0x100000u, rng.U32() % 0x200000u}));
    for (uint32_t p = 0; p < 2; ++p) {
        const uint32_t v = rr::sim::kRcViewArray + rr::sim::kRcViewStride * p;
        if (Chance(rng, 4)) m.PokeWord(v + 772u, 1u);
        if (Chance(rng, 3)) m.PokeWord(v + 548u, m.PeekWord(v + 548u) ^ 0x8000u);
    }
    m.PokeWord(0x8005B220u, Chance(rng, 6) ? 1u : 0u);
    m.PokeWord(c.gp + kRandSeedGpOffset, rng.U32());
    a.a[0] = e;
    a.a[1] = S;
    a.a[2] = Pick(rng, {0u, 0x23C36u, 0x23C37u, 0x8F0D8u, 0x8F0D9u, 0x100000u, rng.U32() % 0x200000u, rng.U32() % 0x30000u});
    a.a[3] = S + 8u;
    const uint32_t other = co::Pool0(m, rng.U32() % 18u);
    a.a4 = Pick(rng, {0u, e + 172u, e + 172u, other + 172u});
    a.more[0] = Chance(rng, 4) ? 1u : 0u;
}
uint32_t NativePedHit(NativeEnv& env, const Args& a) {
    rr::sim::GuestRam g = rcv::View(env);
    const rr::sim::BikeTables t = rcv::Tables(env, g);
    rcv::RecoverOracle c(env);
    uint32_t v0 = 0;
    const bool ok = rr::sim::PedHit(g, a.a[0], a.a[1], static_cast<int32_t>(a.a[2]), a.a[3], a.a4, a.more[0], env.sp,
                                    t, c, v0);
    return rcv::Done(env, g, ok, "PedHit", v0);
}

// ---------------------------------------------------------------- the director (camera_director.h)
// View record 0 of this machine, in place. KnotReverse: the knot count +0x320 0..6 and the knots
// +0x33C.. (x) of both signs, zeros and all-negative runs (the mirror arm), the look knots. ShotSetup:
// a script in the scratch - the marker 7, the kind 0..3 (and now and then another), then (type,
// index) pairs over types 0..5 (and now and then any byte), indices 0..3 (the rr-race table's points
// are resident; bases at 0x800D83BC), ended by a byte that is not a position type; or one of the
// machine's own shot alternatives. NAMED BOUNDS: at most six knots follow (the arrays are 6 words; a
// longer script writes past +0x3B4 into the look arrays on the console too, which a port reproduces,
// but +0x320 > 6 then walks the arrays of 0x800852E8 beyond the record); indices 0..3.
void DrawKnots(CaseContext& c, uint32_t v) {
    Rng& rng = *c.rng;
    Memory& m = *c.mem;
    m.PokeWord(v + 800u, rng.U32() % 7u);
    const uint32_t kind = static_cast<uint32_t>(rng.Next() % 4u);
    for (uint32_t k = 0; k < 6; ++k) {
        uint32_t x;
        if (kind == 0) x = Pick(rng, {0u, 0u, 0xFFFF0000u, 0x10000u, rng.U32()});
        else if (kind == 1) x = 0u - (rng.U32() % 0x60000u);               // none positive
        else x = static_cast<uint32_t>(rng.S32() >> (8 + rng.Next() % 12u));
        m.PokeWord(v + 828u + 4u * k, x);
        for (uint32_t o : {852u, 876u, 996u, 1020u}) m.PokeWord(v + o + 4u * k, static_cast<uint32_t>(rng.S32() >> 10));
    }
}
void SetupKnotReverse(CaseContext& c, Args& a) {
    DrawKnots(c, rr::sim::kCollViews);
    a.a[0] = rr::sim::kCollViews;
}
uint32_t NativeKnotReverse(NativeEnv& env, const Args& a) {
    EnRun run(env);
    const bool ok = rr::sim::KnotReverse(run.g, a.a[0]);
    return crs::Fin(env, run, ok, 0, "KnotReverse");
}
void SetupShot(CaseContext& c, Args& a) {
    Rng& rng = *c.rng;
    Memory& m = *c.mem;
    const uint32_t v = rr::sim::kCollViews;
    DrawKnots(c, v);
    m.PokeWord(v + 548u, rng.U32());
    m.PokeWord(v + 544u, rng.U32() % 4u);
    const uint32_t S = c.scratch;
    const uint32_t shots = m.PeekWord(rr::sim::kShotTableAddr);
    if (shots > 0 && shots < 32 && Chance(rng, 4)) {                        // one of the machine's own
        const uint32_t sh = rr::sim::kShotTableAddr + 4u + 26u * (rng.U32() % shots);
        for (uint32_t k = 3; k < 25; ++k)
            if (m.PeekByte(sh + k) == 7u) { a.a[0] = v; a.a[1] = sh + k; return; }
    }
    std::vector<uint8_t> s;
    s.push_back(7);
    s.push_back(static_cast<uint8_t>(Chance(rng, 12) ? rng.U32() : rng.U32() % 4u));
    const uint32_t pairs = rng.U32() % 9u;
    uint32_t pos = 0;
    for (uint32_t i = 0; i < pairs; ++i) {
        uint32_t t = Pick(rng, {0u, 0u, 1u, 2u, 2u, 3u, 4u, 5u, 5u});
        if (Chance(rng, 16)) t = rng.U32() & 0xFFu;
        if (t == 0u || t == 2u) {
            if (pos >= 5) t = 1;
            else ++pos;
        }
        s.push_back(static_cast<uint8_t>(t));
        s.push_back(static_cast<uint8_t>((t == 4u || t == 5u) ? rng.U32() : rng.U32() % 4u));
    }
    s.push_back(static_cast<uint8_t>(Pick(rng, {1u, 3u, 4u, 5u, 6u, 7u, rng.U32() & 0xFFu})));
    s.push_back(static_cast<uint8_t>(rng.U32() % 4u));
    s.push_back(static_cast<uint8_t>(Pick(rng, {1u, 6u, 7u})));
    s.push_back(0);
    m.WriteBlock(S + 8u, s.data(), static_cast<uint32_t>(s.size()));
    a.a[0] = v;
    a.a[1] = S + 8u;
}
uint32_t NativeShot(NativeEnv& env, const Args& a) {
    EnRun run(env);
    const bool ok = rr::sim::ShotSetup(run.g, a.a[0], a.a[1]);
    return crs::Fin(env, run, ok, 0, "ShotSetup");
}
// SplineSlopes: x (the knot spans, 16.16, positive and now and then 0 or negative), y (values), out
// (out[n] the given end slope), n 0..7. NAMED BOUND: n <= 9 (the two ten-word frame arrays; the
// original itself overruns them past that).
void SetupSlopes(CaseContext& c, Args& a) {
    Rng& rng = *c.rng;
    Memory& m = *c.mem;
    const uint32_t S = c.scratch;
    for (uint32_t k = 0; k < 12; ++k) {
        uint32_t x = 0x800u + rng.U32() % 0x40000u;
        if (Chance(rng, 12)) x = Pick(rng, {0u, 1u, 0xFFFFFFFFu, 0u - (rng.U32() % 0x40000u), rng.U32()});
        m.PokeWord(S + 4u * k, x);
        m.PokeWord(S + 64u + 4u * k, static_cast<uint32_t>(rng.S32() >> (6 + rng.Next() % 14u)));
        m.PokeWord(S + 128u + 4u * k, static_cast<uint32_t>(rng.S32() >> (6 + rng.Next() % 14u)));
    }
    a.a[0] = S;
    a.a[1] = S + 64u;
    a.a[2] = S + 128u;
    a.a[3] = Pick(rng, {0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u, 9u});
}
uint32_t NativeSlopes(NativeEnv& env, const Args& a) {
    EnRun run(env);
    const bool ok = rr::sim::SplineSlopes(run.g, a.a[0], a.a[1], a.a[2], static_cast<int32_t>(a.a[3]), env.sp);
    return crs::Fin(env, run, ok, 0, "SplineSlopes");
}
void SetupCoef(CaseContext& c, Args& a) {
    Rng& rng = *c.rng;
    const uint32_t S = c.scratch;
    for (uint32_t k = 0; k < 3; ++k) a.a[k] = static_cast<uint32_t>(rng.S32() >> (6 + rng.Next() % 14u));
    auto span = [&rng]() {
        uint32_t x = 0x800u + rng.U32() % 0x40000u;
        if (Chance(rng, 3)) x = 0u - x;
        if (Chance(rng, 12)) x = Pick(rng, {0u, 1u, 2u, 0xFFFFFFFFu, rng.U32()});
        return x;
    };
    a.a[3] = span();
    a.a4 = span();
    a.more[0] = S;
    a.more[1] = S + 4u;
    a.more[2] = S + 8u;
    a.more[3] = S + 12u;
}
uint32_t NativeCoef(NativeEnv& env, const Args& a) {
    EnRun run(env);
    const uint32_t sp = env.sp;
    run.g.W32(sp + 16u, a.a4);
    run.g.W32(sp + 20u, a.more[0]);
    run.g.W32(sp + 24u, a.more[1]);
    run.g.W32(sp + 28u, a.more[2]);
    run.g.W32(sp + 32u, a.more[3]);
    const bool ok = rr::sim::SplineCoef(run.g, static_cast<int32_t>(a.a[0]), static_cast<int32_t>(a.a[1]),
                                        static_cast<int32_t>(a.a[2]), static_cast<int32_t>(a.a[3]), sp);
    return crs::Fin(env, run, ok, 0, "SplineCoef");
}

// ---------------------------------------------------------------- the bike's parts (bike_parts.h)
struct PartCallees final : rr::sim::BikePartCallees {
    NativeEnv& env;
    explicit PartCallees(NativeEnv& e) : env(e) {}
    bool Call(uint32_t fn, uint32_t a0, uint32_t a1, uint32_t sp) override {
        const uint32_t a[2] = {a0, a1};
        bs::ExactOracle(env, fn, a, 2, sp);
        return env.failure.empty();
    }
};
// RotMatrix: three angles, in and out of 0..4095 (only the low 12 bits count), the output in the scratch.
void SetupRot(CaseContext& c, Args& a) {
    Rng& rng = *c.rng;
    for (uint32_t k = 0; k < 3; ++k) a.a[k] = Chance(rng, 3) ? 0u : (Chance(rng, 4) ? rng.U32() : rng.U32() % 4096u);
    a.a[3] = c.scratch;
}
uint32_t NativeRot(NativeEnv& env, const Args& a) {
    EnRun run(env);
    rr::sim::RotMatrix(run.g, static_cast<int32_t>(a.a[0]), static_cast<int32_t>(a.a[1]), static_cast<int32_t>(a.a[2]),
                       a.a[3]);
    return crs::Fin(env, run, true, 0, "RotMatrix");
}
// BikeInstance: one of this machine's two live bikes (the player, bike 6), its part slots +4 as captured.
// Drawn: game_state[0] 3 (the pause) now and then, flagsC 0x600, +0x264 / +0x268 both signs, the pitch spring +0x34C / +0x34E,
// the lean +0x212, flagsB bits 5 / 6, +0x260 both signs, a passenger with its lean spring
// +0x290 / +0x294 (the other live bike as the passenger), +0x27C, +0x1E8. The draw 0x80067AC4 runs
// from the original code on both sides.
// NAMED BOUNDS: a live bike (the draw loop draws only those; a dormant one's draw state made the ORIGINAL
// trap in the draw); the entity's +0x04 part slots are its own; the LOD byte +0x08 is the machine's
// (the draw indexes the model's LOD list by it: another value made the ORIGINAL trap in the draw on a
// model this machine has not loaded), so the +0x08 >= 2 arm is reached only by the pause; +0x48 is the machine's
// (3 names another object kind to the draw), so the +0x68 arm of the object matrix is not reached here;
// the second argument (the draw's view index) is 0, the one view of this machine (1..3 made the ORIGINAL
// trap in the draw, measured).
void SetupInstance(CaseContext& c, Args& a) {
    Rng& rng = *c.rng;
    Memory& m = *c.mem;
    const uint32_t e = co::Pool0(m, Pick(rng, {0u, 0u, 6u}));
    if (Chance(rng, 8)) { const uint8_t v = 3; m.WriteBlock(m.PeekWord(co::kGameStatePtr), &v, 1); }
    uint32_t fc = m.PeekWord(e + 568u);
    if (Chance(rng, 5)) fc ^= rng.U32() & 0x700u;
    m.PokeWord(e + 568u, fc);
    m.PokeWord(e + 612u, Chance(rng, 2) ? 0u : rng.U32());
    m.PokeWord(e + 616u, Pick(rng, {0u, 1u, 0xFFFFFFFFu, rng.U32()}));
    WriteU16Bench(m, e + 846u, static_cast<uint16_t>(Pick(rng, {0u, 1u, 0xFFFFu, rng.U32() % 800u, rng.U32()})));
    WriteU16Bench(m, e + 844u, static_cast<uint16_t>(rng.U32() >> (rng.Next() % 16u)));
    WriteU16Bench(m, e + 530u, static_cast<uint16_t>(rng.Unit()));
    WriteU16Bench(m, e + 842u, static_cast<uint16_t>(rng.U32() % 512u));
    WriteU16Bench(m, e + 826u, static_cast<uint16_t>(rng.U32()));
    uint32_t fb = m.PeekWord(e + 564u);
    fb = (fb & ~0x60u) | Pick(rng, {0u, 0u, 0x20u, 0x40u, 0x60u});
    m.PokeWord(e + 564u, fb);
    m.PokeWord(e + 608u, Pick(rng, {0u, rng.U32() % 0x20000u, 0u - (rng.U32() % 0x20000u), rng.U32()}));
    if (Chance(rng, 3)) {                                  // the other live bike carried
        const uint32_t b = co::Pool0(m, e == co::Pool0(m, 0) ? 6u : 0u);
        m.PokeWord(e + 856u, b);
        m.PokeWord(b + 656u, static_cast<uint32_t>(static_cast<int32_t>(rng.U32() % 40000u) - 20000));
        m.PokeWord(b + 660u, static_cast<uint32_t>(rng.S32() >> (rng.Next() % 20u)));
    } else {
        m.PokeWord(e + 856u, 0);
    }
    m.PokeWord(e + 636u, Pick(rng, {0u, 1u, 0xFFFFFFFFu, rng.U32() % 0x10000u, 0u - (rng.U32() % 0x10000u)}));
    m.PokeWord(e + 488u, static_cast<uint32_t>(rng.S32() >> (rng.Next() % 20u)));
    a.a[0] = e;
    a.a[1] = 0;                                                             // the view index (one player)
}
uint32_t NativeInstance(NativeEnv& env, const Args& a) {
    EnRun run(env);
    PartCallees pc(env);
    const cr::Tables tab(env.clone->mem, env.gp);
    const bool ok = rr::sim::BikeInstance(run.g, a.a[0], a.a[1], env.sp, tab.t.asin, pc);
    return crs::Fin(env, run, ok, 0, "BikeInstance");
}
// BikeWheels: one of this machine's bikes; its model's part count as captured (5) or 6 now and then
// (the passenger wheel), the wheel angles +0x344 / +0x346, the attachment words +0x38 / +0x40, a
// passenger; the matrix-stack pointer *(0x1F8002BC) as captured. The part program 0x80067064 and the
// attachment 0x80066B98 run from the original code.
void SetupWheels(CaseContext& c, Args& a) {
    Rng& rng = *c.rng;
    Memory& m = *c.mem;
    const uint32_t e = co::Pool0(m, Pick(rng, {0u, 0u, 6u, rng.U32() % 18u}));
    WriteU16Bench(m, e + 836u, static_cast<uint16_t>(rng.U32()));
    WriteU16Bench(m, e + 838u, static_cast<uint16_t>(rng.U32()));
    if (Chance(rng, 4)) {
        const uint32_t b17 = co::Pool0(m, 17);
        m.PokeWord(e + 856u, b17);
        WriteU16Bench(m, b17 + 836u, static_cast<uint16_t>(rng.U32()));
    }
    a.a[0] = e;
}
uint32_t NativeWheels(NativeEnv& env, const Args& a) {
    EnRun run(env);
    run.g.SetScratchpad(env.clone->mem.scratchpad().data());
    PartCallees pc(env);
    const bool ok = rr::sim::BikeWheels(run.g, a.a[0], env.sp, pc);
    return crs::Fin(env, run, ok, 0, "BikeWheels");
}
// LodChoice: one of this machine's riders (the instance with the LOD table +0x64); the view
// distances +0x2C / +0x30 at and around the table's steps; the current LOD bytes +0x0A / +0x0B 0..3;
// the player count 1 / 2 (and 0: the loop runs once all the same); the model type; the passenger bit.
// NAMED BOUND: a rider or a pedestrian (the draw loop's instances): a bike's +0x64 is another table,
// without the -inf / +inf sentinels the walks stop on (the ORIGINAL read past it, 33 cases); a distance is
// never beyond a sentinel (0 .. 0x7FFFFFFF: an ApproxLen3, ViewDistance's +0x2C).
void SetupLod(CaseContext& c, Args& a) {
    Rng& rng = *c.rng;
    Memory& m = *c.mem;
    const uint32_t e = m.PeekWord(0x8005B3A4u) + 628u * (rng.U32() % 18u);
    const uint32_t tbl = m.PeekWord(e + 100u);
    for (uint32_t p = 0; p < 2; ++p) {
        uint32_t d = rng.U32() % 4000u;
        if (tbl >= 0x80000000u && tbl < 0x80200000u && Chance(rng, 2)) {
            const uint32_t k = rng.U32() % 3u;
            const uint32_t step = m.PeekWord(tbl + 8u * k + (rng.U32() & 4u));
            if (step < 0x100000u) d = step + (rng.U32() % 3u) - 1u;         // a step, not a sentinel
        }
        m.PokeWord(e + 44u + 4u * p, d);
        const uint8_t l = static_cast<uint8_t>(rng.U32() % 4u);
        m.WriteBlock(e + 10u + p, &l, 1);
    }
    if (Chance(rng, 4)) { const uint8_t v = static_cast<uint8_t>(m.PeekByte(e + 572u) ^ 0x20u); m.WriteBlock(e + 572u, &v, 1); }
    a.a[0] = e;
    a.a[1] = Pick(rng, {1u, 1u, 2u, 0u});
}
uint32_t NativeLod(NativeEnv& env, const Args& a) {
    EnRun run(env);
    const bool ok = rr::sim::LodChoice(run.g, a.a[0], static_cast<int32_t>(a.a[1]));
    return crs::Fin(env, run, ok, 0, "LodChoice");
}

void Ex(std::vector<Row>& rows, const char* name, const char* sig, uint32_t entry,
        std::function<void(CaseContext&, Args&)> setup, std::function<uint32_t(NativeEnv&, const Args&)> native,
        bool compareV0, std::vector<OracleCallee> callees = {}) {
    rows.push_back(Row{name, sig, entry, std::move(setup), nullptr});
    Row& r = rows.back();
    r.compareV0 = compareV0;
    r.nativeEx = std::move(native);
    r.oracleCallees = std::move(callees);
}

} // namespace feel

void AddFeelRows(std::vector<Row>& rows) {
    using namespace rr::sim::coll;
    feel::Ex(rows, "camera_collide", "u32 0x800A421C(View *v)  [RASHCDG, the camera's collision step]",
             rr::sim::kCameraCollideFn, feel::SetupCamCollide, feel::NativeCamCollide, true,
             {OracleCallee{kBikeWallHit, 4, 2}, OracleCallee{kPedHit, 4, 2}, OracleCallee{kRiderWallHit, 3},
              OracleCallee{kPropTopple, 4}});
    rows.back().maxSteps = 20'000'000;
    feel::Ex(rows, "prop_vs_shape", "u32 0x800B2E64(E *e, u8 *shape)  [RASHCDG, an entity against a static volume]",
             rr::sim::kPropVsShapeFn, feel::SetupPropShape, feel::NativePropShape, true,
             {OracleCallee{kPropTopple, 4}});
    feel::Ex(rows, "pad_rider_controls",
             "void 0x8001CB3C()  [SLUS, the pad reader; SPLICE of [0x8001CFB0, 0x8001D350 | 0x8001D700 | 0x8001D7DC)]",
             rr::sim::kPadReaderFn, feel::SetupPad, feel::NativePad, false);
    rows.back().maxSteps = 20'000'000;
    feel::Ex(rows, "ped_hit",
             "s32 0x800A9868(E *e, s16 n[3], s32 speed, s16 n2[3], [u8 *other, s32 force])  [RASHCDG, the rider / "
             "pedestrian hit reaction]",
             rr::sim::kPedHitFn, feel::SetupPedHit, feel::NativePedHit, true,
             {OracleCallee{0x80091468u, 1}, OracleCallee{0x800A966Cu, 3}, OracleCallee{0x800C4550u, 3},
              OracleCallee{0x8005BE58u, 1}, OracleCallee{0x8007E868u, 4}, OracleCallee{0x800B658Cu, 4, 1}});
    rows.back().maxSteps = 20'000'000;
    feel::Ex(rows, "knot_reverse", "void 0x800852E8(View *v)  [RASHCDG, the pan's knots mirrored]",
             rr::sim::kKnotReverseFn, feel::SetupKnotReverse, feel::NativeKnotReverse, false);
    feel::Ex(rows, "shot_setup", "void 0x800853E4(View *v, u8 *script)  [RASHCDG, the director's shot reader]",
             rr::sim::kShotSetupFn, feel::SetupShot, feel::NativeShot, false);
    feel::Ex(rows, "spline_slopes", "void 0x8002F634(s32 x[], s32 y[], s32 out[], s32 n)  [SLUS, the knot slopes]",
             rr::sim::kSplineSlopesFn, feel::SetupSlopes, feel::NativeSlopes, false);
    feel::Ex(rows, "spline_coef",
             "void 0x8002F4D8(s32 y0, s32 y1, s32 y2, s32 h0, [s32 h1, s32 *o0, *o1, *o2, *o3])  [SLUS]",
             rr::sim::kSplineCoefFn, feel::SetupCoef, feel::NativeCoef, false);
    feel::Ex(rows, "euler_matrix", "void 0x8001FD24(s32 a, s32 b, s32 c, s16 out[9])  [SLUS, the Euler matrix]",
             rr::sim::kRotMatrixFn, feel::SetupRot, feel::NativeRot, false);
    feel::Ex(rows, "bike_instance", "void 0x80084E10(Bike *e, s32 a1)  [RASHCDG, the fork, the pitch spring]",
             rr::sim::kBikeInstanceFn, feel::SetupInstance, feel::NativeInstance, false,
             {OracleCallee{rr::sim::kBikeDrawFn, 2}});
    rows.back().maxSteps = 20'000'000;
    feel::Ex(rows, "bike_wheels", "void 0x80066EC4(Bike *e)  [RASHCDG, the wheels]", rr::sim::kBikeWheelsFn,
             feel::SetupWheels, feel::NativeWheels, false,
             {OracleCallee{rr::sim::kPartProgramFn, 2}, OracleCallee{rr::sim::kAttachChildFn, 2}});
    rows.back().maxSteps = 20'000'000;
    feel::Ex(rows, "lod_choice", "void 0x800667C4(Instance *e, s32 players)  [RASHCDG, the level of detail]",
             rr::sim::kLodChoiceFn, feel::SetupLod, feel::NativeLod, false);
}
