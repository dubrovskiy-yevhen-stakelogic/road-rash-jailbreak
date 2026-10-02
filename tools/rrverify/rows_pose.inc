// Bench rows for the POSE side of the rider animation machine (16):
// src\game\sim\pose.{h,cpp}. Included by verify_physics.cpp inside its anonymous namespace, after
// rows_anim.inc, whose helpers (animrows::PlantObject, RandomProgram, RandomClock, Images, ...) these
// rows reuse. RASHCDG.BIN cfe43a7786759f2cb9c57751cf99e84d1074782c, SLUS_010.53
// 67ed165a2c517d4e6106fb0dfa324d66dd9a76f1.
//
// NO SEAMS: every callee (FixMul, FixDiv, Asin, QuatMul, Slerp, QuatToMatrix, SetRoot, ClipSelect,
// DecodeHeader) is native, and `pose_pass` runs the whole AnimationPass 0x8005E1D8 with the native
// pose under the native clock - the machine with nothing on the oracle.
//
// Named bounds (each because the ORIGINAL is never asked it, not because the port could not follow):
//   * QuatToMatrix gets s16 << 2 components (what all three of its callers pass);
//   * a sampled frame lies in [-1, keys + 1] of its clip (ApplyFrame runs before the tick; past
//     that the decode would run on through unrelated memory for as many keys as the frame is off);
//   * a blend buffer's mask names only slots the owner's model has (DOD3 +0x18 sub-meshes): the
//     capture ANDs it with the object's LOD mask, which never names more;
//   * stance 42 (the tilt, which chases owner +0x254 -> bike +0x28C) only on a pool-1 rider.

namespace poserows {
using namespace animrows;

template <class F>
uint32_t RunPose(NativeEnv& env, F&& f) {
    Memory& m = env.clone->mem;
    rr::sim::GuestRam g(m.ram().data(), env.gp);
    g.SetScratchpad(m.scratchpad().data());
    rr::sim::AnimPose pose(g);
    rr::sim::AnimMachine machine(g, pose);
    const uint32_t v = f(pose, machine);
    if ((pose.Failed() || machine.Failed()) && env.failure.empty()) {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "the port refused (view fault %d at 0x%08X)", g.Faulted() ? 1 : 0,
                      g.FaultAddress());
        env.failure = buf;
    }
    return v;
}

Row PoseRow(const char* name, const char* sig, uint32_t entry, std::function<void(CaseContext&, Args&)> setup,
            std::function<uint32_t(NativeEnv&, const Args&)> native, bool compareV0) {
    Row r{name, sig, entry, std::move(setup), nullptr, compareV0, std::move(native), {}};
    return r;
}

// A plausible 1.14 quaternion: a captured channel quadruple now and then, else random.
void SomeQuat(CaseContext& c, int16_t q[4]) {
    Rng& r = *c.rng;
    const uint32_t k = r.U32() % 4u;
    if (k == 0) {
        for (int i = 0; i < 4; ++i) q[i] = r.S16();
    } else if (k == 1) {
        const uint32_t a = ObjAt(*c.mem, Pick(r, 18));
        const uint32_t part = Pick(r, 17);
        for (uint32_t i = 0; i < 4; ++i) q[i] = GetS16(*c.mem, a + 128u + 96u * part + 24u * i);
    } else {
        // near unit: one dominant component
        for (int i = 0; i < 4; ++i) q[i] = static_cast<int16_t>(Range(r, -6000, 6000));
        q[Pick(r, 4)] = static_cast<int16_t>((r.Next() & 1u) ? Range(r, 12000, 16384) : -Range(r, 12000, 16384));
    }
}
void PutQuat(Memory& m, uint32_t at, const int16_t q[4]) {
    for (uint32_t i = 0; i < 4; ++i) PutS16(m, at + 2u * i, q[i]);
}

// The DOD3 sub-mesh count of the object's owner's model.
uint32_t OwnerParts(const Memory& m, uint32_t a) {
    const uint32_t owner = m.PeekWord(a);
    const uint32_t model = m.PeekWord(owner);
    if (model < 0x80000000u) return 0;
    return Get16(m, model + 24u);
}

// A scratch model record for the owner now and then: another type (bits 3..6 of +0x0E), the
// blend's scale bit (bit 0) and a +0x14 scale; the sub-mesh count +0x18 kept.
void MaybeSwapModel(CaseContext& c, uint32_t a) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    if (r.Next() % 4u != 0u) return;
    const uint32_t owner = m.PeekWord(a);
    const uint32_t model = m.PeekWord(owner);
    const uint32_t fake = c.scratch + 7000u;
    for (uint32_t k = 0; k < 40u; k += 4u) m.PokeWord(fake + k, m.PeekWord(model + k));
    const uint32_t types[] = {1u, 4u, 5u, 2u, 0u, 1u};
    uint32_t f = (Get16(m, model + 14u) & ~0x79u) | (types[Pick(r, 6)] << 3) | (r.Next() & 1u);
    Put16(m, fake + 14u, f);
    PutS32(m, fake + 20u, (r.Next() & 1u) ? 0x1000 : Range(r, 0x400, 0x3000));
    m.PokeWord(owner, fake);
    if (r.Next() & 1u) m.PokeWord(owner + 52u, 0);
}

// Random pose-side clock fields: interpolation, sub-frame, rate (0 now and then: `div` by zero is
// defined on the R3000), the op's play flags.
void PerturbPoseClock(CaseContext& c, uint32_t a) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    uint32_t f = m.PeekWord(a + 36u);
    if (r.Next() & 1u) f ^= 4u;
    m.PokeWord(a + 36u, f);
    const uint32_t q = r.U32() % 10u;
    PutS32(m, a + 24u, q == 0 ? 0 : q == 1 ? -Range(r, 1, 30) : Range(r, 1, 60));
    PutS32(m, a + 32u, (r.Next() % 5u == 0) ? static_cast<int32_t>(r.U32() >> 12) : Range(r, 0, 60));
    const uint32_t op = m.PeekWord(a + 4u) + 12u * m.PeekWord(a + 12u);
    if (r.Next() & 1u) Put8(m, op + 2u, r.U32());
    if (r.Next() % 3u == 0) m.PokeWord(a + 1760u, (r.Next() & 1u) ? r.U32() : 0x1FFFFu);
}

// The owner's parent / hanging flags that pick the root offsets.
void PerturbOwner(CaseContext& c, uint32_t a) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    const uint32_t owner = m.PeekWord(a);
    if (r.Next() % 3u == 0) m.PokeWord(owner + 36u, m.PeekWord(owner + 36u) ^ 0x40000u);
    if (r.Next() % 3u == 0) PutS32(m, owner + 76u, Range(r, 0x8000, 0x18000));
    if (r.Next() % 5u == 0) m.PokeWord(owner + 52u, 0);
}

// The sampled frame inside [-1, keys + 1] of the current clip (the named bound).
void BoundFrame(CaseContext& c, uint32_t a) {
    Memory& m = *c.mem;
    const int32_t keys = static_cast<int32_t>(KeysOf(m, m.PeekWord(a + 44u)));
    const int32_t f = GetS32(m, a + 16u);
    if (f < -1 || f > keys + 1) PutS32(m, a + 16u, Range(*c.rng, -1, keys + 1));
}

// A consistent channel table for a drawn clip: the native DecodeHeader (itself a bench row) run on
// the case's memory, so both machines start from the same decoded header.
void Redecode(CaseContext& c, uint32_t a) {
    rr::sim::GuestRam g(c.mem->ram().data(), c.gp);
    rr::sim::AnimPose pose(g);
    rr::sim::AnimMachine machine(g, pose);
    machine.DecodeHeader(a, a + 48u);
}

// The blend buffer of the object: kept as captured, or drawn inside the owner's slots.
void DrawBlendBuffer(CaseContext& c, uint32_t a) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    const uint32_t n = OwnerParts(m, a);
    const uint32_t valid = n >= 32u ? 0xFFFFFFFFu : ((1u << n) - 1u);
    const uint32_t B = a + 0x6E4u;
    if (c.sample == nullptr || r.Next() % 3u == 0) {
        Put8(m, B, (r.Next() % 4u == 0) ? Pick(r, 21) : 20u);
        m.PokeWord(B + 8u, r.U32() & valid);
        for (uint32_t k = 0; k < 3; ++k) {
            PutS16(m, B + 12u + 2u * k, static_cast<int16_t>(Range(r, -300, 300)));
            PutS16(m, B + 18u + 2u * k, static_cast<int16_t>(Range(r, -300, 300)));
        }
        for (uint32_t k = 0; k < 20; ++k) {
            int16_t q[4];
            SomeQuat(c, q);
            PutQuat(m, B + 24u + 16u * k, q);
            if (r.Next() % 4u == 0) { for (int i = 0; i < 4; ++i) q[i] = static_cast<int16_t>(q[i] + Range(r, -40, 40)); }
            else if (r.Next() % 5u == 0) { for (int i = 0; i < 4; ++i) q[i] = static_cast<int16_t>(-q[i]); }
            else SomeQuat(c, q);
            PutQuat(m, B + 32u + 16u * k, q);
        }
    } else {
        m.PokeWord(B + 8u, m.PeekWord(B + 8u) & valid);
        if (m.PeekByte(B) > 20u) Put8(m, B, 20u);
    }
}

} // namespace poserows

// ---------------------------------------------------------------------------- the presentation pass
// (src\game\sim\present.h). Seams: the stance event, RiderLaunch, RiderKnockOff (ported, but on the
// oracle here: their own rows prove them) and SLUS 0x800273EC (not ported). The rider is planted by
// rows_anim.inc's PlantCrash, which keeps RiderLaunch / the knock-off inside the bounds the ORIGINAL
// survives (no passenger: +0x23C bits 4..5 clear); on top of it the
// fields the dispatcher branches on are drawn.
namespace poserows {

struct PresentOracleSeams final : rr::sim::PresentSeams {
    NativeEnv& env;
    rr::sim::GuestRam& g;
    PresentOracleSeams(NativeEnv& e, rr::sim::GuestRam& gr) : env(e), g(gr) {}
    uint32_t TransitionCapture(uint32_t a, uint32_t op, uint32_t b) override {
        rr::sim::AnimPose p(g);
        return p.TransitionCapture(a, op, b);
    }
    uint32_t ApplyFrame(uint32_t a) override {
        rr::sim::AnimPose p(g);
        return p.ApplyFrame(a);
    }
    uint32_t CombatLeave(uint32_t r, uint32_t cur, uint32_t ev, uint32_t p) override {
        return env.CallOracle(rr::sim::kCombatLeaveFn, r, cur, ev, p);
    }
    uint32_t CombatEnter(uint32_t r, uint32_t cur, uint32_t ev, uint32_t p) override {
        return env.CallOracle(rr::sim::kCombatEnterFn, r, cur, ev, p);
    }
    uint32_t StanceEvent(uint32_t ev, uint32_t r, uint32_t p) override {
        return env.CallOracle(rr::sim::kStanceEventFn, ev, r, p, 0);
    }
    uint32_t RiderLaunch(uint32_t r) override { return env.CallOracle(rr::sim::kRiderLaunchFn, r, 0, 0, 0); }
    uint32_t RiderKnockOff(uint32_t r) override { return env.CallOracle(rr::sim::kRiderKnockOffFn, r, 0, 0, 0); }
    uint32_t ObjectSound(uint32_t r, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4) override {
        const uint32_t a[5] = {r, a1, a2, a3, a4};
        return env.CallOracleArgs(rr::sim::kObjectSoundFn, a, 5);
    }
};

template <class F>
uint32_t RunPresent(NativeEnv& env, F&& f) {
    Memory& m = env.clone->mem;
    rr::sim::GuestRam g(m.ram().data(), env.gp);
    g.SetScratchpad(m.scratchpad().data());
    PresentOracleSeams seams(env, g);
    rr::sim::PresentLayer layer(g, seams);
    const uint32_t v = f(layer);
    if (layer.Failed() && env.failure.empty()) {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "the port refused (view fault %d at 0x%08X)", g.Faulted() ? 1 : 0,
                      g.FaultAddress());
        env.failure = buf;
    }
    return v;
}

std::vector<OracleCallee> PresentCallees(uint32_t depth) {
    return {OracleCallee{rr::sim::kStanceEventFn, 3, 0, {}, depth},
            OracleCallee{rr::sim::kRiderLaunchFn, 1, 0, {}, depth},
            OracleCallee{rr::sim::kRiderKnockOffFn, 1, 0, {}, depth},
            OracleCallee{rr::sim::kObjectSoundFn, 4, 1, {}, depth}};
}

// A rider for the dispatcher: PlantCrash's, then the fields the presentation code branches on.
uint32_t PlantPresentRider(CaseContext& c, bool leaving, bool leanBank = false) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    const CrashCase cc = PlantCrash(c);
    const uint32_t R = cc.rider, B = cc.bike;
    const uint32_t a = m.PeekWord(R + 540u);
    // The running op 0's clip byte inside every resident stance bank (>= 14 clips): a queued
    // transition across a bank change reads it in the NEW bank.
    Put8(m, m.PeekWord(a + 4u), Pick(r, 14));
    {   // stance 0 (a follow-up can name it) starts clip 0 on bike +0x21C: the named bound of
        // rows_anim.inc - a spare object planted there (0 in every capture: the ORIGINAL then reads
        // its program pointer from RAM word 4, an unmapped address)
        const uint32_t spare = ObjAt(m, 21);
        m.PokeWord(spare + 40u, BankPtr(m, 0));
        m.PokeWord(spare + 8u, 5);
        m.PokeWord(B + 540u, spare);
    }
    const uint32_t mounts[] = {0, 1, 1, 1, 1, 2, 3};
    m.PokeWord(R + 604u, leaving ? 2u : mounts[Pick(r, 7)]);
    uint32_t want = 0xFFFFu;
    if (leaving) {
        const uint32_t st[] = {38, 39, 40, 88, 89, 41, 42, 90, 91};
        want = st[Pick(r, 9)];
    } else if (r.Next() % 3u == 0) {
        const uint32_t st[] = {11, 12, 13, 14, 18, 19, 20, 8, 4, 6, 9, 10, 23, 24, 25, 15, 16, 17, 22};
        want = st[Pick(r, 19)];
    }
    if (leanBank) {
        // RidingLean's LoopStart takes the lean stances' clips (18..20) in the object's CURRENT bank:
        // the named bound keeps the rider in a stance of that bank, as every riding stance is.
        const uint32_t lb = m.PeekWord(rr::sim::kStanceTable + 8u * 18u) & 0xFu;
        const uint32_t st[] = {11, 12, 13, 14, 18, 19, 20};
        want = 0xFFFFu;
        for (int t = 0; t < 8 && want == 0xFFFFu; ++t) {
            const uint32_t s0 = st[Pick(r, 7)];
            if ((m.PeekWord(rr::sim::kStanceTable + 8u * s0) & 0xFu) == lb) want = s0;
        }
        if (want == 0xFFFFu) want = 18;
    }
    if (want != 0xFFFFu && Resident(m, want)) {
        // the object's bank follows the stance (the invariant the gate's bank switch keeps)
        Put16(m, R + 544u, want);
        m.PokeWord(a + 40u, BankPtr(m, m.PeekWord(rr::sim::kStanceTable + 8u * want) & 0xFu));
        RandomProgram(c, a, true);
        RandomClock(c, a);
        PutS32(m, a + 24u, Range(r, 1, 60));
    }
    Put8(m, m.PeekWord(a + 4u), Pick(r, 14));
    uint32_t f = m.PeekWord(R + 552u) & ~0x1E404000u;
    f |= r.U32() & 0x1C400000u;
    if (r.Next() % 6u == 0) f |= 0x4000u;
    if (r.Next() % 4u == 0) f |= 0x02000000u;
    m.PokeWord(R + 552u, f);
    Put8(m, R + 572u, (m.PeekByte(R + 572u) & ~0x3Cu) | ((r.Next() % 4u == 0) ? (r.U32() & 0xCu) : 0u));
    Put16(m, R + 608u, (r.Next() % 4u == 0) ? Pick(r, 3) : 0u);
    for (uint32_t k = 0; k < 3; ++k) Put16(m, R + 610u + 2u * k, ResidentStance(c));
    const int32_t clock = GetS32(m, m.PeekWord(kGameStatePtr) + 16u);
    PutS32(m, R + 548u, clock - Range(r, 0, 900));
    PutS32(m, 0x800CCB9Cu, (r.Next() & 1u) ? 1500 : Range(r, 0, 3000));
    // the bike
    PutS32(m, B + 636u, (r.Next() % 4u == 0) ? 0 : Range(r, -0x14000, 0x14000));
    PutS32(m, B + 672u, (r.Next() % 4u == 0) ? 0 : Range(r, -0x20000, 0x20000));
    PutS32(m, B + 616u, Range(r, -0x14000, 0x14000));
    PutS32(m, B + 620u, Range(r, -0x10000, 0x10000));
    PutS32(m, B + 652u, Range(r, -0x20000, 0x20000));
    PutS32(m, B + 480u, (r.Next() % 3u == 0) ? Range(r, 0, 0x9000) : Range(r, 0, 0x300000));
    m.PokeWord(B + 560u, m.PeekWord(B + 560u) ^ ((r.Next() & 1u) ? 1u : 0u));
    if (r.Next() % 3u == 0) m.PokeWord(B + 564u, m.PeekWord(B + 564u) ^ 0x08000000u);
    m.PokeWord(B + 180u, Pick(r, 16));
    m.PokeWord(B + 1088u, Pick(r, 2));
    PutS32(m, B + 600u, Range(r, 0, 0x60000));
    {
        const int32_t depth = GetS16(m, B + 946u) & 0xFF;
        Put16(m, B + 948u + 8u * static_cast<uint32_t>(static_cast<int8_t>(depth)), Pick(r, 18));
    }
    return R;
}

} // namespace poserows

// `--coverage`: each row's own function, [entry, the address after its last `jr ra`'s delay slot).
const CoverageRange* PoseCoverage(const char* rowName) {
    static const struct {
        const char* name;
        CoverageRange range;
    } kRanges[] = {
        {"pose_bits", {0x8005E558, 0x8005E5A4}},       {"pose_sample", {0x8005E5A4, 0x8005E840}},
        {"pose_pose", {0x8005D63C, 0x8005E1D8}},       {"pose_capture", {0x8005CB70, 0x8005D2A8}},
        {"pose_blend", {0x8005D36C, 0x8005D63C}},      {"pose_apply_frame", {0x8005D2A8, 0x8005D36C}},
        {"pose_set_root", {0x80066A60, 0x80066A84}},   {"pose_quat_mul", {0x800714FC, 0x800716C0}},
        {"pose_slerp", {0x800710C0, 0x800714A0}},      {"pose_quat_to_matrix", {0x8001005C, 0x800102A4}},
        {"present_scrub_lean", {0x800C31CC, 0x800C341C}}, {"present_scrub_lift", {0x800C341C, 0x800C3630}},
        {"present_scrub_pitch", {0x800C3630, 0x800C37B0}}, {"present_riding_lean", {0x800C3950, 0x800C3E9C}},
        {"present_follow_stance", {0x800C45D8, 0x800C47CC}}, {"present_follow_queue", {0x800C47CC, 0x800C4860}},
        {"present_follow_seat", {0x800C4860, 0x800C4B30}}, {"present_follow_gear", {0x800C4B30, 0x800C4BA0}},
        {"present_follow_ride", {0x800C4BA0, 0x800C4E18}}, {"present_follow_near", {0x800C4E18, 0x800C5078}},
        {"present_follow_up", {0x800C5078, 0x800C5168}}, {"present_pass", {0x80090814, 0x80090D84}},
    };
    for (const auto& r : kRanges)
        if (std::strcmp(r.name, rowName) == 0) return &r.range;
    return nullptr;
}

void AddPoseRows(std::vector<Row>& rows) {
    using namespace poserows;
    namespace s = rr::sim;

    // ======================================================================== the leaves
    rows.push_back(PoseRow(
        "pose_bits", "u8 Bits(u16 **p, u8 *pos, u32 w)  [RASHCDG 0x8005E558, MSB first]", s::kPoseBitsFn,
        [](CaseContext& c, Args& args) {
            Memory& m = *c.mem;
            Rng& r = *c.rng;
            if (c.sample != nullptr) {
                // a captured object's coded channel, where Sample would read it next
                const uint32_t a = PlantObject(c);
                const uint32_t n = static_cast<uint32_t>(std::max<int32_t>(GetS16(m, a + 50u), 1));
                for (uint32_t t = 0; t < 8; ++t) {
                    const uint32_t ch = a + 52u + 24u * Pick(r, std::min<uint32_t>(n, 71u));
                    if (m.PeekByte(ch + 20u) & 2u) {
                        args.a[0] = ch;
                        args.a[1] = ch + 23u;
                        args.a[2] = m.PeekByte(ch + 22u);
                        return;
                    }
                }
            }
            const uint32_t s0 = c.scratch;
            for (uint32_t k = 0; k < 40; ++k) Put16(m, s0 + 64u + 2u * k, r.U32());
            m.PokeWord(s0, s0 + 64u + 2u * Pick(r, 16));
            Put8(m, s0 + 8u, (r.Next() % 6u == 0) ? r.U32() : Pick(r, 16));
            args.a[0] = s0;
            args.a[1] = s0 + 8u;
            args.a[2] = (r.Next() % 8u == 0) ? Pick(r, 33) : Pick(r, 9);
        },
        [](NativeEnv& env, const Args& a) {
            return RunPose(env, [&](s::AnimPose& p, s::AnimMachine&) { return p.Bits(a.a[0], a.a[1], a.a[2]); });
        },
        true));

    rows.push_back(PoseRow(
        "pose_set_root", "void SetRoot(Entity *e, s16 v[3])  [RASHCDG 0x80066A60]", s::kPoseSetRootFn,
        [](CaseContext& c, Args& args) {
            Memory& m = *c.mem;
            Rng& r = *c.rng;
            const uint32_t v = c.scratch + 16u + 2u * Pick(r, 4);
            for (uint32_t k = 0; k < 3; ++k) Put16(m, v + 2u * k, r.U32());
            args.a[0] = (r.Next() & 1u) ? m.PeekWord(ObjAt(m, Pick(r, 18))) : c.scratch + 200u + Pick(r, 2) * 2u;
            args.a[1] = v;
        },
        [](NativeEnv& env, const Args& a) {
            return RunPose(env, [&](s::AnimPose& p, s::AnimMachine&) { return p.SetRoot(a.a[0], a.a[1]); });
        },
        false));

    rows.push_back(PoseRow(
        "pose_quat_mul", "void QuatMul(s16 a[4], s16 b[4], s16 out[4])  [RASHCDG 0x800714FC, 1.14]",
        s::kPoseQuatMulFn,
        [](CaseContext& c, Args& args) {
            Memory& m = *c.mem;
            Rng& r = *c.rng;
            int16_t qa[4], qb[4];
            SomeQuat(c, qa);
            SomeQuat(c, qb);
            const uint32_t s0 = c.scratch;
            PutQuat(m, s0, qa);
            PutQuat(m, s0 + 8u, qb);
            args.a[0] = (r.Next() % 3u == 0) ? s::kPoseSeatTable + 10u + 8u * Pick(r, 17) : s0;
            args.a[1] = s0 + 8u;
            const uint32_t o = r.U32() % 4u;
            args.a[2] = o == 0 ? s0 + 8u : o == 1 ? s0 : s0 + 16u; // out aliasing b, a, or neither
        },
        [](NativeEnv& env, const Args& a) {
            return RunPose(env, [&](s::AnimPose& p, s::AnimMachine&) { return p.QuatMul(a.a[0], a.a[1], a.a[2]); });
        },
        false));

    rows.push_back(PoseRow(
        "pose_quat_to_matrix", "void QuatToMatrix(MATRIX *m, s32 q[4])  [SLUS 0x8001005C, s = 2 / |q|^2]",
        s::kPoseQuatToMatrixFn,
        [](CaseContext& c, Args& args) {
            Memory& m = *c.mem;
            Rng& r = *c.rng;
            int16_t q[4];
            SomeQuat(c, q);
            if (r.Next() % 10u == 0) for (auto& v : q) v = static_cast<int16_t>(Range(r, -300, 300)); // |q| tiny
            if (r.Next() % 25u == 0) for (auto& v : q) v = 0;
            const uint32_t qv = c.scratch + 32u;
            for (uint32_t i = 0; i < 4; ++i) PutS32(m, qv + 4u * i, static_cast<int32_t>(q[i]) * 4);
            // a rider's own part slot, or the scratch
            args.a[0] = (r.Next() & 1u) ? m.PeekWord(m.PeekWord(ObjAt(m, Pick(r, 18))) + 4u) + 24u * Pick(r, 17) + 4u
                                        : c.scratch + 64u;
            args.a[1] = qv;
        },
        [](NativeEnv& env, const Args& a) {
            return RunPose(env, [&](s::AnimPose& p, s::AnimMachine&) { return p.QuatToMatrix(a.a[0], a.a[1]); });
        },
        false));

    rows.push_back(PoseRow(
        "pose_slerp", "void Slerp(s32 t, s16 q0[4], s16 q1[4], s16 out[4])  [RASHCDG 0x800710C0]", s::kPoseSlerpFn,
        [](CaseContext& c, Args& args) {
            Memory& m = *c.mem;
            Rng& r = *c.rng;
            int16_t q0[4], q1[4];
            SomeQuat(c, q0);
            const uint32_t k = r.U32() % 5u;
            if (k == 0) { for (int i = 0; i < 4; ++i) q1[i] = q0[i]; }                               // linear arm
            else if (k == 1) { for (int i = 0; i < 4; ++i) q1[i] = static_cast<int16_t>(-q0[i] + Range(r, -30, 30)); }
            else if (k == 2) { for (int i = 0; i < 4; ++i) q1[i] = static_cast<int16_t>(q0[i] + Range(r, -2000, 2000)); }
            else SomeQuat(c, q1);
            const uint32_t s0 = c.scratch;
            PutQuat(m, s0, q0);
            PutQuat(m, s0 + 8u, q1);
            const uint32_t tq = r.U32() % 6u;
            const int32_t t = tq == 0 ? 0 : tq == 1 ? 0x10000 : tq == 2 ? Range(r, -0x4000, 0x14000) : Range(r, 0, 0x10000);
            args.a[0] = static_cast<uint32_t>(t);
            args.a[1] = s0;
            args.a[2] = s0 + 8u;
            const uint32_t o = r.U32() % 4u;
            args.a[3] = o == 0 ? s0 : o == 1 ? s0 + 8u : s0 + 16u;
        },
        [](NativeEnv& env, const Args& a) {
            return RunPose(env, [&](s::AnimPose& p, s::AnimMachine&) {
                return p.Slerp(static_cast<int32_t>(a.a[0]), a.a[1], a.a[2], a.a[3]);
            });
        },
        false));

    // ======================================================================== the channels
    rows.push_back(PoseRow(
        "pose_sample", "s32 Sample(Anim *a, Track *T)  [RASHCDG 0x8005E5A4, every channel to frame / frame+1]",
        s::kPoseSampleFn,
        [](CaseContext& c, Args& args) {
            Memory& m = *c.mem;
            Rng& r = *c.rng;
            const uint32_t a = PlantObject(c);
            if (c.sample == nullptr) {
                Redecode(c, a);
                const int32_t keys = static_cast<int32_t>(KeysOf(m, m.PeekWord(a + 44u)));
                PutS32(m, a + 16u, Range(r, -1, keys - 1));
                if (r.Next() % 8u == 0) PutS32(m, a + 16u, Range(r, keys - 1, keys + 1));
            } else if (r.Next() & 1u) {
                PutS32(m, a + 16u, GetS32(m, a + 16u) + Range(r, -1, 2));
            }
            BoundFrame(c, a);
            args.a[0] = a;
            args.a[1] = a + 48u;
        },
        [](NativeEnv& env, const Args& a) {
            return RunPose(env, [&](s::AnimPose& p, s::AnimMachine&) { return p.Sample(a.a[0], a.a[1]); });
        },
        true));

    // ======================================================================== the pose
    rows.push_back(PoseRow(
        "pose_pose", "void Pose(Anim *a)  [RASHCDG 0x8005D63C, root + QuatToMatrix per part, stance 42 tilt]",
        s::kPoseFn,
        [](CaseContext& c, Args& args) {
            Memory& m = *c.mem;
            Rng& r = *c.rng;
            const uint32_t slot = Pick(r, 21);
            const uint32_t a = ObjAt(m, slot);
            const Captured* cap = c.sample ? PickCaptured(r, slot) : nullptr;
            if (cap != nullptr) {
                PlantCaptured(m, a, *cap);
            } else {
                RandomProgram(c, a, true);
                RandomClock(c, a);
                if (r.Next() & 1u)
                    for (uint32_t ch = 0; ch < 71u; ++ch) {
                        PutS16(m, a + 56u + 24u * ch, (ch < 3u) ? static_cast<int16_t>(Range(r, -400, 400)) : r.S16());
                        PutS16(m, a + 58u + 24u * ch, (ch < 3u) ? static_cast<int16_t>(Range(r, -400, 400)) : r.S16());
                    }
            }
            PerturbPoseClock(c, a);
            PerturbOwner(c, a);
            MaybeSwapModel(c, a);
            const uint32_t owner = m.PeekWord(a);
            if (slot < 18u && r.Next() % 4u == 0) {
                Put16(m, owner + 544u, 42u);
                PutS32(m, m.PeekWord(owner + 596u) + 652u, (r.Next() & 1u) ? Range(r, -0x20000, 0x20000) : r.S32());
                if (r.Next() & 1u) PutS32(m, a + 16u, Range(r, -1, static_cast<int32_t>(KeysOf(m, m.PeekWord(a + 44u)))));
            }
            args.a[0] = a;
        },
        [](NativeEnv& env, const Args& a) {
            return RunPose(env, [&](s::AnimPose& p, s::AnimMachine&) { return p.Pose(a.a[0]); });
        },
        false));

    // ======================================================================== the blend
    rows.push_back(PoseRow(
        "pose_capture", "u32 TransitionCapture(Anim *a, Op *op3, Blend *B)  [RASHCDG 0x8005CB70, FROM / TO poses]",
        s::kAnimTransitionCaptureFn,
        [](CaseContext& c, Args& args) {
            Memory& m = *c.mem;
            Rng& r = *c.rng;
            const uint32_t a = PlantObject(c, true);
            PerturbOwner(c, a);
            MaybeSwapModel(c, a);
            const uint32_t k = Pick(r, 3);
            const uint32_t op = m.PeekWord(a + 4u) + 12u * k;
            Put8(m, op + 28u, Pick(r, BankClips(m, m.PeekWord(a + 40u)))); // the new clip (op k+2's `a`)
            if (r.Next() & 1u) Put8(m, op + 14u, r.U32());
            if (r.Next() & 1u) Put8(m, op + 26u, r.U32());
            if (r.Next() % 3u == 0) m.PokeWord(a + 1760u, (r.Next() & 1u) ? r.U32() : 0x1FFFFu);
            args.a[0] = a;
            args.a[1] = op;
            args.a[2] = a + 0x6E4u;
        },
        [](NativeEnv& env, const Args& a) {
            return RunPose(env, [&](s::AnimPose& p, s::AnimMachine&) {
                return p.TransitionCapture(a.a[0], a.a[1], a.a[2]);
            });
        },
        true));

    rows.push_back(PoseRow(
        "pose_blend", "void TransitionBlend(Anim *a)  [RASHCDG 0x8005D36C, slerp FROM -> TO]", s::kPoseBlendFn,
        [](CaseContext& c, Args& args) {
            Memory& m = *c.mem;
            Rng& r = *c.rng;
            const uint32_t a = PlantObject(c, true);
            PerturbPoseClock(c, a);
            PerturbOwner(c, a);
            MaybeSwapModel(c, a);
            DrawBlendBuffer(c, a);
            const uint32_t op = m.PeekWord(a + 4u) + 12u * m.PeekWord(a + 12u);
            const uint32_t q = r.U32() % 8u;
            PutS16(m, op + 4u, static_cast<int16_t>(q == 0 ? 0 : q == 1 ? Range(r, -3, 12) : 5));
            PutS32(m, a + 16u, Range(r, -1, 6));
            if (r.Next() % 4u == 0) Put16(m, m.PeekWord(a) + 544u, Pick(r, 224));
            args.a[0] = a;
        },
        [](NativeEnv& env, const Args& a) {
            return RunPose(env, [&](s::AnimPose& p, s::AnimMachine&) { return p.Blend(a.a[0]); });
        },
        false));

    rows.push_back(PoseRow(
        "pose_apply_frame", "u32 ApplyFrame(Anim *a)  [RASHCDG 0x8005D2A8, blend or Sample + Pose]",
        s::kAnimApplyFrameFn,
        [](CaseContext& c, Args& args) {
            Memory& m = *c.mem;
            Rng& r = *c.rng;
            const uint32_t slot = Pick(r, 21);
            const uint32_t a = ObjAt(m, slot);
            const Captured* cap = c.sample ? PickCaptured(r, slot) : nullptr;
            if (cap != nullptr) {
                PlantCaptured(m, a, *cap);
            } else {
                RandomProgram(c, a, true);
                RandomClock(c, a);
                Redecode(c, a);
                const int32_t keys = static_cast<int32_t>(KeysOf(m, m.PeekWord(a + 44u)));
                PutS32(m, a + 16u, Range(r, -1, keys - 1));
            }
            if (r.Next() & 1u) PerturbPoseClock(c, a);
            PerturbOwner(c, a);
            BoundFrame(c, a);
            DrawBlendBuffer(c, a);
            if (slot < 18u && r.Next() % 6u == 0) {
                const uint32_t owner = m.PeekWord(a);
                Put16(m, owner + 544u, 42u);
                PutS32(m, m.PeekWord(owner + 596u) + 652u, Range(r, -0x20000, 0x20000));
            }
            args.a[0] = a;
        },
        [](NativeEnv& env, const Args& a) {
            return RunPose(env, [&](s::AnimPose& p, s::AnimMachine&) { return p.ApplyFrame(a.a[0]); });
        },
        true));

    // ======================================================================== the whole machine
    {
        std::function<void(CaseContext&, Args&)> passSetup;
        for (const Row& row : rows)
            if (std::string(row.name) == "anim_pass") passSetup = row.setup;
        if (passSetup) {
            Row r = PoseRow(
                "pose_pass", "void AnimationPass(Desc *d)  [RASHCDG 0x8005E1D8, the native pose under the native clock]",
                s::kAnimPassFn,
                [passSetup](CaseContext& c, Args& args) {
                    passSetup(c, args);
                    for (uint32_t slot = 0; slot < 21; ++slot) {
                        const uint32_t a = ObjAt(*c.mem, slot);
                        if (c.mem->PeekWord(a) == 0u) continue;
                        BoundFrame(c, a);
                        DrawBlendBuffer(c, a);
                    }
                },
                [](NativeEnv& env, const Args& a) {
                    return RunPose(env, [&](s::AnimPose&, s::AnimMachine& mc) { return mc.Pass(a.a[0]); });
                },
                true);
            r.maxSteps = 80'000'000;
            rows.push_back(r);
        }
    }

    // ======================================================================== the presentation pass
    const auto presentRow = [&](const char* name, const char* sig, uint32_t entry, uint32_t frame, bool leaving,
                                std::function<uint32_t(s::PresentLayer&, const Args&)> call) {
        Row r = PoseRow(
            name, sig, entry,
            [leaving](CaseContext& c, Args& args) { args.a[0] = PlantPresentRider(c, leaving); },
            [call](NativeEnv& env, const Args& a) {
                return RunPresent(env, [&](s::PresentLayer& l) { return call(l, a); });
            },
            false);
        r.oracleCallees = PresentCallees(frame);
        r.maxSteps = 50'000'000;
        rows.push_back(r);
    };
    presentRow("present_scrub_lean", "void FallScrubLean(Rider *r)  [RASHCDG 0x800C31CC, stances 39 / 89]",
               s::kFallScrubLeanFn, 40, true, [](s::PresentLayer& l, const Args& a) { return l.FallScrubLean(a.a[0]); });
    presentRow("present_scrub_lift", "void FallScrubLift(Rider *r)  [RASHCDG 0x800C341C, stances 40 / 88]",
               s::kFallScrubLiftFn, 40, true, [](s::PresentLayer& l, const Args& a) { return l.FallScrubLift(a.a[0]); });
    presentRow("present_scrub_pitch", "void FallScrubPitch(Rider *r)  [RASHCDG 0x800C3630, stance 38]",
               s::kFallScrubPitchFn, 32, true, [](s::PresentLayer& l, const Args& a) { return l.FallScrubPitch(a.a[0]); });
    {
        Row r = PoseRow(
            "present_riding_lean", "u8 RidingLean(Rider *r)  [RASHCDG 0x800C3950, the lean clip scrubbed]",
            s::kRidingLeanFn, [](CaseContext& c, Args& args) { args.a[0] = PlantPresentRider(c, false, true); },
            [](NativeEnv& env, const Args& a) {
                return RunPresent(env, [&](s::PresentLayer& l) { return l.RidingLean(a.a[0]); });
            },
            false);
        r.oracleCallees = PresentCallees(64);
        r.maxSteps = 50'000'000;
        rows.push_back(r);
    }
    {
        // A rival alongside: the bike's top command 6 (a fight) now and then, every other bike's
        // along-road word near ours on the same road, so Pick's nearest one lands in the windows.
        Row r = PoseRow(
            "present_follow_near", "s32 FollowNear(Rider *r)  [RASHCDG 0x800C4E18, a rival alongside]",
            s::kFollowNearFn,
            [](CaseContext& c, Args& args) {
                Memory& m = *c.mem;
                Rng& r = *c.rng;
                const uint32_t R = PlantPresentRider(c, false);
                const uint32_t B = m.PeekWord(R + 596u);
                m.PokeWord(R + 604u, 1);
                const int32_t depth = static_cast<int8_t>(m.PeekByte(B + 946u));
                Put16(m, B + 948u + 8u * static_cast<uint32_t>(depth), (r.Next() & 1u) ? 6u : 4u + Pick(r, 9));
                const uint32_t base = m.PeekWord(kPoolBasePtr);
                for (uint32_t k = 0; k < 18; ++k) {
                    const uint32_t o = base + 1096u * k;
                    if (o == B) continue;
                    PutS32(m, o + 324u, GetS32(m, B + 324u) + Range(r, -0x2000, 0x2000));
                    if (r.Next() % 3u != 0) {
                        m.PokeWord(o + 360u, m.PeekWord(B + 360u));
                        m.PokeWord(o + 336u, m.PeekWord(B + 336u));
                        PutS32(m, o + 344u, GetS32(m, B + 344u) + Range(r, -0x14000, 0x14000));
                    }
                    if (r.Next() % 4u == 0) {
                        const int32_t od = static_cast<int8_t>(m.PeekByte(o + 946u));
                        Put16(m, o + 948u + 8u * static_cast<uint32_t>(od), (r.Next() & 1u) ? 16u : 6u);
                    }
                }
                args.a[0] = R;
            },
            [](NativeEnv& env, const Args& a) {
                return RunPresent(env, [&](s::PresentLayer& l) { return l.FollowNear(a.a[0]); });
            },
            false);
        r.oracleCallees = PresentCallees(40);
        r.maxSteps = 50'000'000;
        rows.push_back(r);
    }
    presentRow("present_follow_queue", "s32 FollowQueue(Rider *r)  [RASHCDG 0x800C47CC, the queued stance]",
               s::kFollowQueueFn, 24, false, [](s::PresentLayer& l, const Args& a) { return l.FollowQueue(a.a[0]); });
    presentRow("present_follow_seat", "s32 FollowSeat(Rider *r)  [RASHCDG 0x800C4860, kick-start / push-off]",
               s::kFollowSeatFn, 40, false, [](s::PresentLayer& l, const Args& a) { return l.FollowSeat(a.a[0]); });
    presentRow("present_follow_gear", "s32 FollowGear(Rider *r)  [RASHCDG 0x800C4B30, +0x23C bits 2..3]",
               s::kFollowGearFn, 24, false, [](s::PresentLayer& l, const Args& a) { return l.FollowGear(a.a[0]); });
    presentRow("present_follow_ride", "s32 FollowRide(Rider *r)  [RASHCDG 0x800C4BA0, lean / brake / tuck / look]",
               s::kFollowRideFn, 40, false, [](s::PresentLayer& l, const Args& a) { return l.FollowRide(a.a[0]); });
    presentRow("present_follow_up", "void FollowUp(Rider *r)  [RASHCDG 0x800C5078, the riding stance machine]",
               s::kFollowUpFn, 40, false, [](s::PresentLayer& l, const Args& a) { return l.FollowUp(a.a[0]); });
    {
        Row r = PoseRow(
            "present_follow_stance", "u16 FollowStance(Rider *r, s32 *p)  [RASHCDG 0x800C45D8, a leaf]",
            s::kFollowStanceFn,
            [](CaseContext& c, Args& args) {
                args.a[0] = PlantPresentRider(c, false);
                if (c.rng->Next() % 3u == 0) Put16(*c.mem, args.a[0] + 544u, Pick(*c.rng, 90));
                if (c.rng->Next() % 4u == 0) Put16(*c.mem, args.a[0] + 172u, c.rng->U32());
                args.a[1] = c.scratch + 8u;
            },
            [](NativeEnv& env, const Args& a) {
                return RunPresent(env, [&](s::PresentLayer& l) { return l.FollowStance(a.a[0], a.a[1]); });
            },
            true);
        rows.push_back(r);
    }
    {
        Row r = PoseRow(
            "present_pass", "void PresentationPass(s32 dt)  [RASHCDG 0x80090814, pool 0: flash, knock-off, fall, follow-up]",
            s::kPresentPassFn,
            [](CaseContext& c, Args& args) {
                Memory& m = *c.mem;
                Rng& r = *c.rng;
                const uint32_t R = PlantPresentRider(c, (r.Next() % 3u) == 0);
                (void)R;
                // the flash timer's globals and the race mode byte it reads
                PutS32(m, 0x8005B2FCu, Range(r, 0, 4000));
                PutS32(m, 0x800CCA7Cu, (r.Next() & 1u) ? -1 : Range(r, 0, 6));
                args.a[0] = static_cast<uint32_t>((r.Next() & 1u) ? 5 : Range(r, 0, 30));
            },
            [](NativeEnv& env, const Args& a) {
                return RunPresent(env, [&](s::PresentLayer& l) { return l.Pass(static_cast<int32_t>(a.a[0])); });
            },
            false);
        r.oracleCallees = PresentCallees(56 + 40);
        r.maxSteps = 200'000'000;
        rows.push_back(r);
    }
}
