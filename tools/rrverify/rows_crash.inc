// Bench rows for the crash chain. Included by verify_physics.cpp inside
// its anonymous namespace, so everything defined above that point is visible here.
//
// The native code is src\game\sim\crash.{h,cpp}, a GUEST-ADDRESS port (road_query.h's `GuestRam`): the
// native side runs straight on the clone's RAM. The impact end's two sound callees are the PORTED
// GetRCnt and PlaySound3D on a `SoundMachine` over the same RAM, and the hardware counter is answered
// by `EnIo` (rows_engine_note.inc) from the clone's own `Cpu::ConcessionLoad` - the same code that
// answers the guest's load of root counter 2. The row drives that counter per case through
// `CaseContext::rootCounter`, on both machines alike, exactly as the `get_rcnt` row does.

namespace cr {

// RASHCDG.BIN, SHA-1 cfe43a7786759f2cb9c57751cf99e84d1074782c.
constexpr uint32_t kImpactStatePass = 0x80078DB4; // 3436 bytes, extent ..0x80079B20

// The static tables the ports take as host pointers, read out of the machine's own image.
struct Tables {
    std::vector<int16_t> sincos;
    std::vector<uint16_t> asin;
    int32_t atan[20]{};
    std::vector<uint16_t> rsqrt;
    std::vector<int16_t> sqrt;
    rr::sim::BikeTables t;
    Tables(const Memory& m, uint32_t gp) {
        sincos.assign(8192, 0);
        m.ReadBlock(kSinCosTable, sincos.data(), sincos.size() * sizeof(int16_t));
        asin.assign(kAsinEntries, 0);
        m.ReadBlock(kAsinTable, asin.data(), asin.size() * sizeof(uint16_t));
        for (uint32_t i = 0; i < 20; ++i) atan[i] = GetS32(m, kAtanTable + 4u * i);
        rsqrt.assign(2048, 0);
        m.ReadBlock(m.PeekWord(gp + kRsqrtTableGpOffset), rsqrt.data(), rsqrt.size() * sizeof(uint16_t));
        // SqrtGte's table inside a window (ai.h): a negative argument reads up to 640 bytes below it.
        sqrt.assign(kSqrtWindowHalf, 0);
        m.ReadBlock(kSqrtTable - kSqrtWindowHalf / 2u, sqrt.data(), sqrt.size() * 2u);
        t.sincos = sincos.data();
        t.asin = asin.data();
        t.atan = atan;
        t.rsqrt = rsqrt.data();
        t.sqrt = sqrt.data() + kSqrtWindowHalf / 4u;
    }
};

uint32_t Refused(NativeEnv& env, EnRun& run, bool ok, const char* what) {
    if (!ok && env.failure.empty() && !run.s.Faulted()) env.failure = std::string(what) + " refused";
    return run.Done(env, 0);
}

// Links `count` entities into an intrusive list headed at `head`, in order: {prev, next} at +0x440,
// entity = node - 1088.
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

// ---------------------------------------------------------------- ImpactStatePass, 0x80078DB4
//
// Scratch layout (8192 bytes): the list head at +0; up to three listed bikes at +16 + 1100*i; a
// fourth bike that is only ever a PARTNER (+0x358), never on the list, at +3320; four rider records
// (pool 1, 628 bytes; the function reads +0x1E0, +0x228, +0x23C, +0x25C) at +4416 + 640*j, rider j
// belonging to bike j; one stat block (BikeSteerLean reads +0x04, +0xE8, +0x118, +0x11C) at +6976.
//
// What is driven, and why, per bike: the class bits 0..4 of flagsC in every combination the pass
// branches on (none, 16, a bump, 2, 4, 8, and mixtures), the airborne / tumbling bits 0x600 (s6),
// "heading taken" 0x2000, "roll ramp started" 0x4000; |+0x16C| on both sides of 3 (the bit-22
// toggle); +0x2A4 zero or not (class 16); the rider's mount state +0x25C on both sides of 2; the
// pitch +0x268 AT and around both floors (-0.698, -0.873) and 0; the impact timer +0x2D4 / +0x2D8
// in both orders and equal, in the 2180-per-frame steps the game counts in; the roll +0x28C exactly
// at +-pi/2 or not; +0x26C on both sides of 66 and of 0; +0x294 zero or not; the partner +0x358 null,
// a listed bike, the unlisted bike or the bike itself; the node's prev word +0x440 zero (the pass
// walks `next` only, and tests `prev` as a word); the rider's +0x23C bit 4 (the partner's rider is
// then chased with no null test); the LCG seed; root counter 2.
//
// Named bounds on the inputs (each a range the original's own code produces or needs):
//   * the throw's +0x328..+0x32C are kept within +-20000: they become the tumble axis, and a larger
//     vector makes Normalize's trapping `add` raise the console's overflow exception;
//   * +0x358 is null with +0x23C bit 4 set only when the word the guest then reads at 0x354 +
//     0x354 names RAM (measured on this snapshot in the setup, never assumed). On rr-race it does
//     NOT: the word at 0x354 is 0xBFC09914, a KSEG1 address in the BIOS ROM, so the original would
//     read the ROM word at +0x25C and the port's RAM-only view refuses there. That arm is therefore
//     not driven, and the product must not reach it;
//   * the stat block pointer names the planted block (BikeSteerLean's own row drives its values).
void SetupImpact(CaseContext& c, Args& a) {
    Rng& rng = *c.rng;
    Memory& m = *c.mem;
    const bool derived = c.sample != nullptr;
    auto chance = [&rng](uint32_t n) { return rng.Next() % n == 0; };
    auto pick = [&rng](std::initializer_list<uint32_t> v) { return *(v.begin() + rng.Next() % v.size()); };
    const uint32_t head = c.scratch;
    const uint32_t count = 1u + rng.U32() % 3u;
    uint32_t bikes[4];
    for (uint32_t i = 0; i < 3; ++i) bikes[i] = c.scratch + 16u + 1100u * i;
    bikes[3] = c.scratch + 3320u;
    uint32_t riders[4];
    for (uint32_t j = 0; j < 4; ++j) riders[j] = c.scratch + 4416u + 640u * j;
    const uint32_t statsAt = c.scratch + 6976u;

    // The stat block: a real one of this machine for derived cases, else random words (four of
    // them are read).
    {
        std::vector<uint8_t> st(320);
        const uint32_t realArray = m.PeekWord(bs::kStatArrayPtr);
        if (derived && realArray >= 0x80000000u && realArray < 0x80200000u - 4u * 448u)
            m.ReadBlock(realArray + 448u * (rng.U32() % 4u), st.data(), st.size());
        else
            for (uint8_t& b : st) b = static_cast<uint8_t>(rng.U32());
        m.WriteBlock(statsAt, st.data(), st.size());
    }

    for (uint32_t i = 0; i < 4; ++i) {
        // ---- the bike
        std::vector<uint8_t> e = (c.sample != nullptr && c.sample->entity.size() == 1096)
                                     ? c.sample->entity
                                     : ((c.any != nullptr && c.any->entity.size() == 1096) ? c.any->entity
                                                                                          : std::vector<uint8_t>(1096, 0));
        // the rider record, from this machine's pool 1 where the captured bike names one
        std::vector<uint8_t> r(640, 0);
        const uint32_t realRider = GetU32V(e, 0x354);
        if (derived && realRider >= 0x80000000u && realRider < 0x80200000u - 640u)
            m.ReadBlock(realRider, r.data(), r.size());
        else
            for (uint8_t& b : r) b = static_cast<uint8_t>(rng.U32());
        if (!derived) {
            auto unit = [&](uint32_t off) {
                for (uint32_t k = 0; k < 3; ++k) PutU16V(e, off + 2u * k, static_cast<uint16_t>(rng.Unit()));
            };
            for (uint32_t off : {0x1B0u, 0x1B6u, 0x1BCu, 0x204u, 0x20Au, 0x210u, 0x1C2u, 0x32Eu, 0x334u})
                unit(off);
            for (uint32_t k = 0; k < 3; ++k) {
                PutU32V(e, 0x1F8 + 4u * k, static_cast<uint32_t>(rng.World()));
                PutU32V(e, 0xB8 + 4u * k, static_cast<uint32_t>(rng.World()));
            }
            PutU32V(e, 0x134, static_cast<uint32_t>(rng.S32() >> (2 + rng.Next() % 16u)));
            PutU32V(e, 0x1E0, static_cast<uint32_t>(rng.S32() >> (4 + rng.Next() % 10u)));
            PutU32V(e, 0x1E8, rng.U32());
            PutU32V(e, 0x240, static_cast<uint32_t>(rng.S32() >> (rng.Next() % 12u)));
            PutU32V(e, 0x27C, static_cast<uint32_t>(rng.S32() >> (rng.Next() % 14u)));
            PutU32V(e, 0x29C, static_cast<uint32_t>(rng.S32() >> (rng.Next() % 14u)));
            PutU32V(e, 0x2DC, static_cast<uint32_t>(rng.S32() >> (rng.Next() % 12u)));
            PutU32V(e, 0x2E0, static_cast<uint32_t>(rng.S32() >> (rng.Next() % 12u)));
            e[0x08] = static_cast<uint8_t>(rng.U32() % 5u);
            PutU32V(e, 0x230, rng.U32());
            PutU32V(e, 0x26C, rng.U32());
            PutU32V(e, 0x270, rng.U32());
        }
        // The bump's rescale (0x80078F4C): the vertical +0x1C4 at and beyond 1.0 (the `1 - y^2 <= 0`
        // arm) and a near-vertical +0x1BC row (the "horizontal part below 6" floor).
        if (chance(4)) PutU16V(e, 0x1C4, static_cast<uint16_t>(pick({4096u, 0xF000u, 4097u, rng.U32()})));
        if (chance(6)) {
            PutU16V(e, 0x1BC, static_cast<uint16_t>(static_cast<int32_t>(rng.U32() % 81u) - 40));
            PutU16V(e, 0x1C0, static_cast<uint16_t>(static_cast<int32_t>(rng.U32() % 81u) - 40));
        }
        // the throw's source vector (named bound above)
        for (uint32_t k = 0; k < 3; ++k) {
            const int32_t v = derived && !chance(3) ? static_cast<int16_t>(GetU32V(e, 0x328 + 2u * k))
                                                    : static_cast<int32_t>(rng.U32() % 8193u) - 4096;
            const int32_t b = std::clamp(v * (chance(8) ? 4 : 1), -20000, 20000);
            PutU16V(e, 0x328 + 2u * k, static_cast<uint16_t>(b));
        }
        // |+0x16C| on both sides of 3
        if (!derived || chance(2))
            PutU32V(e, 0x16C, chance(3) ? rng.U32() : static_cast<uint32_t>(static_cast<int32_t>(rng.U32() % 9u) - 4));
        // flagsC: the class bits, then the state bits
        uint32_t fc = derived && chance(2) ? GetU32V(e, 0x238) : rng.U32();
        fc &= ~0x3Fu;
        fc |= pick({0u, 0u, 0x10u, 1u, 1u, 2u, 4u, 4u, 8u, 8u, 8u, 5u, 9u, 0xCu, 0x11u, 0x18u, 3u, 0xFu, 0x20u});
        fc = chance(2) ? (fc & ~0x600u) : ((fc & ~0x600u) | pick({0x200u, 0x400u, 0x600u}));
        fc = chance(3) ? (fc | 0x2000u) : (fc & ~0x2000u);
        fc = chance(4) ? (fc | 0x4000u) : (fc & ~0x4000u);
        PutU32V(e, 0x238, fc);
        PutU32V(e, 0x2A4, chance(2) ? 0u : rng.U32());
        // the pitch +0x268, at and around both floors
        PutU32V(e, 0x268, pick({0xFFFF4D48u, 0xFFFF4D47u, 0xFFFF4D49u, 0xFFFF209Au, 0xFFFF2099u, 0xFFFF209Bu,
                                0xFFFFFFFFu, 0u, 1u, 0xFFFE4DD0u, 0xFFFF0000u, 0x10000u,
                                static_cast<uint32_t>(rng.S32() >> (12 + rng.Next() % 6u))}));
        // the impact timer: elapsed +0x2D4, duration +0x2D8
        {
            const uint32_t el = 2180u * (rng.U32() % 8u);
            const uint32_t du = pick({el, el + 1u, el - 1u, 1905u, 6553u, 9148u, 0u, 2180u * (rng.U32() % 8u),
                                      rng.U32()});
            PutU32V(e, 0x2D4, chance(10) ? rng.U32() : el);
            PutU32V(e, 0x2D8, du);
        }
        // the roll, the roll ramp, the partner shape
        PutU32V(e, 0x28C, pick({0x1921Fu, 0xFFFE6DE1u, 0u, rng.U32() >> (1 + rng.Next() % 14u),
                                static_cast<uint32_t>(static_cast<int32_t>(rng.U32() % 0x32000u) - 0x19000),
                                static_cast<uint32_t>(rng.S32() >> (12 + rng.Next() % 4u))}));
        PutU32V(e, 0x26C, pick({65u, 66u, 0u, 0xFFFFFFFFu, rng.U32() >> (8 + rng.Next() % 12u),
                                static_cast<uint32_t>(rng.S32() >> (8 + rng.Next() % 12u))}));
        if (!derived || chance(2)) PutU32V(e, 0x270, static_cast<uint32_t>(rng.S32() >> (rng.Next() % 16u)));
        PutU32V(e, 0x294, chance(2) ? 0u : rng.U32());
        PutU32V(e, 0x33C, rng.U32());
        // pointers
        PutU32V(e, 0x354, riders[i]);
        PutU32V(e, 0x22C, statsAt);
        uint32_t partner = 0;
        if (!chance(2)) partner = bikes[pick({0u, 1u, 2u, 3u, 3u, i})];
        PutU32V(e, 0x358, partner);
        m.WriteBlock(bikes[i], e.data(), e.size());

        // ---- its rider
        PutU32V(r, 0x1E0, derived && chance(2) ? GetU32V(r, 0x1E0)
                                               : static_cast<uint32_t>(rng.S32() >> (4 + rng.Next() % 10u)));
        PutU32V(r, 0x25C, pick({0u, 1u, 1u, 2u, 3u, 4u, rng.U32()}));
        r[0x23C] = static_cast<uint8_t>(chance(3) ? (r[0x23C] | 0x10u) : (r[0x23C] & ~0x10u));
        if (partner == 0 && (r[0x23C] & 0x10u)) {
            // The guest chases 0x354 -> +0x354 -> +0x25C. Keep the bit only where that chain stays in
            // RAM on this machine (measured here, on the setup's own memory).
            const uint32_t w = m.PeekWord(0x80000354u);
            const uint32_t seg = w >> 29; // KUSEG 0, KSEG0 4, KSEG1 5 reach RAM; the rest trap
            const bool ok = (w & 3u) == 0 && (w & 0x1FFFFFFFu) < 0x00800000u - 0x300u &&
                            (seg == 0u || seg == 4u || seg == 5u);
            if (!ok || !chance(2)) r[0x23C] = static_cast<uint8_t>(r[0x23C] & ~0x10u);
        }
        m.WriteBlock(riders[i], r.data(), 640);
    }
    const uint32_t listed[3] = {bikes[0], bikes[1], bikes[2]};
    LinkList(m, head, listed, count);
    // The node's prev word, which the pass never walks but tests at 0x80078E74 / 0x80079638 /
    // 0x800797B8: zero it on some listed bikes.
    for (uint32_t i = 0; i < count; ++i)
        if (chance(6)) m.PokeWord(bikes[i] + 1088u, 0);
    // the LCG seed and root counter 2
    if (!derived || chance(2)) m.PokeWord(c.gp + kRandSeedGpOffset, rng.U32());
    if (c.rootCounter != nullptr) *c.rootCounter = rng.U32() & 0xFFFFu;
    a.a[0] = head;
}

// The two sound callees of the impact end, answered by the PORTED functions (sound_engine.h's GetRCnt,
// sound.h's PlaySound3D through GuestPlaySound3D) on the clone's own RAM and root counter - no oracle.
struct ImpactSoundOnClone final : rr::sim::ImpactSound {
    rr::sim::SoundMachine& s;
    explicit ImpactSoundOnClone(rr::sim::SoundMachine& m) : s(m) {}
    bool GetRCnt(uint32_t id, uint32_t& value) override {
        value = rr::sim::GetRCnt(s, id);
        return !s.Faulted();
    }
    bool PlaySound3D(int32_t x, int32_t z, int32_t id, int32_t bank) override {
        rr::sim::GuestPlaySound3D(s, x, z, id, bank);
        return !s.Faulted();
    }
};

uint32_t NativeImpact(NativeEnv& env, const Args& a) {
    EnRun run(env);
    const Tables tab(env.clone->mem, env.gp);
    ImpactSoundOnClone snd(run.s);
    const bool ok = rr::sim::ImpactStatePass(run.g, a.a[0], tab.t, snd);
    return Refused(env, run, ok, "ImpactStatePass");
}

// ---------------------------------------------------------------- MulAdd16, SLUS 0x8002EA20
//
// Scratch: base s16[3] at +0, dir s16[3] at +8, out s32[3] at +16 - or, one case in four, `out`
// laid over `base` or `dir` (the port does component i's loads after component i-1's store, as the
// original does).
void SetupMulAdd16(CaseContext& c, Args& a) {
    Rng& rng = *c.rng;
    Memory& m = *c.mem;
    for (uint32_t i = 0; i < 64; i += 4) m.PokeWord(c.scratch + i, rng.U32());
    if (c.sample != nullptr) {
        PutVec3S16(m, c.scratch + 0, &c.sample->mB[3 * (rng.Next() % 3u)]);
        PutVec3S16(m, c.scratch + 8, &c.sample->mA[3 * (rng.Next() % 3u)]);
    }
    a.a[0] = c.scratch + 0;
    a.a[1] = c.scratch + 8;
    a.a[2] = c.sample != nullptr ? static_cast<uint32_t>(c.sample->speed) : rng.U32() >> (rng.Next() % 16u);
    if (rng.Next() % 5u == 0) a.a[2] = rng.U32();
    const uint32_t alias = static_cast<uint32_t>(rng.Next() % 8u);
    a.a[3] = alias == 0 ? c.scratch + 0 : alias == 1 ? c.scratch + 8 : alias == 2 ? c.scratch + 4 : c.scratch + 16;
}

uint32_t NativeMulAdd16(Memory& m, uint32_t, uint32_t gp, const Args& a) {
    rr::sim::GuestRam g(m.ram().data(), gp);
    const uint32_t v = rr::sim::MulAdd16(g, a.a[0], a.a[1], static_cast<int32_t>(a.a[2]), a.a[3]);
    return g.Faulted() ? 0xDEADDEADu : v;
}

// ---------------------------------------------------------------- Spin, 0x800849D8
//
// A captured bike (dump-derived) or a randomised one at the scratch; flagsC bit 19 on one case in
// five; +0x1E8 of both signs and at the 1.5 clamp; +0x1E0 on both sides of 0x9FFFF (and negative);
// the tumble axis +0x31C / +0x320 and the heading +0x1C2 / +0x1C6 so that their cross term takes
// both signs and 0; the LCG seed.
void SetupSpin(CaseContext& c, Args& a) {
    Rng& rng = *c.rng;
    Memory& m = *c.mem;
    const bool derived = c.sample != nullptr;
    auto chance = [&rng](uint32_t n) { return rng.Next() % n == 0; };
    std::vector<uint8_t> e = derived ? c.sample->entity : c.any->entity;
    if (!derived)
        for (uint32_t off : {0x1C2u, 0x1C4u, 0x1C6u, 0x31Cu, 0x31Eu, 0x320u})
            PutU16V(e, off, static_cast<uint16_t>(chance(3) ? rng.U32() : static_cast<uint32_t>(rng.Unit())));
    uint32_t fc = derived && chance(2) ? GetU32V(e, 0x238) : rng.U32();
    fc = chance(5) ? (fc | 0x80000u) : (fc & ~0x80000u);
    PutU32V(e, 0x238, fc);
    const uint32_t yaw[] = {0x18000u, 0xFFFE8000u, 0x17FFFu, 0xFFFE8001u, 0u, 0xFFFFFFFBu << 16,
                            rng.U32(), static_cast<uint32_t>(rng.S32() >> (8 + rng.Next() % 12u))};
    if (!derived || chance(2)) PutU32V(e, 0x1E8, yaw[rng.Next() % 8u]);
    const uint32_t spd[] = {0x9FFFFu, 0xA0000u, 0u, 0xFFFFFFFDu, rng.U32(),
                            static_cast<uint32_t>(rng.S32() >> (8 + rng.Next() % 12u))};
    if (!derived || chance(2)) PutU32V(e, 0x1E0, spd[rng.Next() % 6u]);
    if (chance(8)) { // a zero cross term: the axis parallel to the heading
        PutU16V(e, 0x31C, static_cast<uint16_t>(GetU32V(e, 0x1C2)));
        PutU16V(e, 0x320, static_cast<uint16_t>(GetU32V(e, 0x1C6)));
    }
    m.WriteBlock(c.scratch, e.data(), e.size());
    if (!derived || chance(2)) m.PokeWord(c.gp + kRandSeedGpOffset, rng.U32());
    a.a[0] = c.scratch;
}

uint32_t NativeSpin(NativeEnv& env, const Args& a) {
    rr::sim::GuestRam g(env.clone->mem.ram().data(), env.gp);
    const bool ok = rr::sim::Spin(g, a.a[0]);
    if (!ok && env.failure.empty()) env.failure = "Spin refused";
    return RqDone(env, g, 0);
}

// ---------------------------------------------------------------- Bounce, 0x80084564
//
// Scratch: n s16[3] at +0, d s16[3] at +8, the speed at +16, vel s32[3] at +20 (a real caller
// passes the bike's +0x360 / +0x35C / +0x1C8, which are this same shape). k at sp+16, floor at
// sp+20, flat at sp+24, as the o32 caller leaves them.
//
// ONE NAMED BOUND: n and d are directions of length at most 1.0 (4096), as every caller's are (a
// face normal, a bike row, a heading). A longer `d` can make Normalize's trapping `add` overflow on
// the console, and a longer `n` turns the restitution into arithmetic no caller can produce.
// Inside that bound every arm is driven: moving away (-1), a wall (|n.y| < 0.5) with the heading
// down (the mirror; flat or not) or up, a floor-like normal, near head-on (the 1.1 minimum), k 0 /
// the game's 3 and 5 / anything, floor 0 with the speed below 2.0, between 2.0 and 10.0 and above.
void RandomDir(Rng& rng, int16_t v[3], int32_t maxLen) {
    int64_t x[3];
    for (int k = 0; k < 3; ++k) x[k] = static_cast<int64_t>(rng.U32() % 8193u) - 4096;
    const uint64_t sq = static_cast<uint64_t>(x[0] * x[0] + x[1] * x[1] + x[2] * x[2]);
    uint64_t l = 0; // floor(sqrt(sq)), integer, so the setup is the same on every build
    while ((l + 1) * (l + 1) <= sq) ++l;
    // Mostly exactly unit length, as a caller's directions are; the rest shorter.
    const int64_t want = (rng.Next() % 4u != 0) ? maxLen
                                                 : static_cast<int64_t>(rng.U32() % static_cast<uint32_t>(maxLen + 1));
    for (int k = 0; k < 3; ++k)
        v[k] = static_cast<int16_t>(l == 0 ? 0 : (x[k] * want) / static_cast<int64_t>(l));
}

void SetupBounce(CaseContext& c, Args& a) {
    Rng& rng = *c.rng;
    Memory& m = *c.mem;
    auto chance = [&rng](uint32_t n) { return rng.Next() % n == 0; };
    int16_t n[3], d[3];
    if (c.sample != nullptr && !chance(3)) {
        // a captured bike's rows: its up row as the normal, a row of another orientation as heading
        for (int k = 0; k < 3; ++k) {
            n[k] = c.sample->mB[3 + k];
            d[k] = c.sample->mA[6 + k];
        }
        if (chance(2)) for (int k = 0; k < 3; ++k) n[k] = static_cast<int16_t>(-n[k]);
        if (chance(2)) for (int k = 0; k < 3; ++k) n[k] = c.sample->mB[k];
    } else {
        RandomDir(rng, n, 4096);
        RandomDir(rng, d, 4096);
        if (chance(3)) RandomDir(rng, n, 4096), n[1] = static_cast<int16_t>(n[1] / 4);    // wall-like
        if (chance(3)) d[1] = static_cast<int16_t>(d[1] > 0 ? -d[1] : d[1]); // heading down
        if (chance(8)) for (int k = 0; k < 3; ++k) d[k] = static_cast<int16_t>(-n[k]);      // head-on
    }
    PutVec3S16(m, c.scratch + 0, n);
    PutVec3S16(m, c.scratch + 8, d);
    const uint32_t spd[] = {0x1FFFFu, 0x20000u, 0x9FFFFu, 0xA0000u, 0u, rng.U32() % 0x800000u,
                            c.sample != nullptr ? static_cast<uint32_t>(c.sample->speed) : rng.U32(), rng.U32()};
    m.PokeWord(c.scratch + 16, spd[rng.Next() % 8u]);
    for (uint32_t i = 0; i < 12; i += 4) m.PokeWord(c.scratch + 20 + i, rng.U32());
    a.a[0] = c.scratch + 0;
    a.a[1] = c.scratch + 8;
    a.a[2] = c.scratch + 16;
    a.a[3] = c.scratch + 20;
    const uint32_t ks[] = {0u, 3u, 5u, 5u, rng.U32() % 16u, rng.U32()};
    a.a4 = ks[rng.Next() % 6u];
    const uint32_t floors[] = {0u, 0u, 0x30000u, 0x60000u, 0x131333u, 0x8F0A3u, rng.U32() % 0x200000u, rng.U32()};
    a.more[0] = floors[rng.Next() % 8u];
    a.more[1] = chance(3) ? (chance(2) ? 1u : rng.U32()) : 0u;
}

uint32_t NativeBounce(Memory& m, uint32_t, uint32_t gp, const Args& a) {
    rr::sim::GuestRam g(m.ram().data(), gp);
    const Tables tab(m, gp);
    bool ok = false;
    const int32_t v = rr::sim::Bounce(g, a.a[0], a.a[1], a.a[2], a.a[3], static_cast<int32_t>(a.a4),
                                      static_cast<int32_t>(a.more[0]), static_cast<int32_t>(a.more[1]), tab.t, ok);
    return ok ? static_cast<uint32_t>(v) : 0xDEADDEADu;
}

// ---------------------------------------------------------------- Remount, 0x800903F4
//
// Every case is built on a LIVE pool-0 bike of the bench's own machine, as `ai_drive`'s are:
// the oracle-supplied callees walk that bike's road slice and leg, its
// rider's animation object and the view records, so a bike lifted out of another RAM image would
// point at state this machine does not hold. The dump-derived half is "one of this machine's live
// bikes" with the flag words the function branches on driven; the randomised half drives the
// scalars too. Driven: fromRoad; +0x140 (live / dormant); flagsC bit 22; the rider's +0x228; the
// two-rider arm (rider +0x23C bit 4 with +0x358 a different live bike, and game_state +0x39 on
// both sides of 1); riderDef +0x28 / +0x27 (op 2 or 4); the stack depth +0x3B2; game_state +0x04 /
// +0x39 (the Jailbreak 44/2 view arm); the race clock; +0x15C, +0x158, +0x130.
//
// Named bounds (each the range the original's own code keeps): the stack depth +0x3B2 is 0..15
// (a negative one makes AiClearCommands clear the whole address space); the rider's +0x22C, which
// selects the call of 0x80095AEC, keeps its captured value.
namespace rm {
constexpr uint32_t kRemount = 0x800903F4;
constexpr uint32_t kRowsFromHeading = 0x8007EC30, kResetBike = 0x8002090C, kReleaseObj = 0x80095AEC;
constexpr uint32_t kAttach = 0x80012838, kDismount = 0x800C3104, kReFace = 0x80096564;
constexpr uint32_t kViewReset = 0x80086AF8, kPlayerVoice = 0x80018440, kPlayerBind = 0x800235B0;
constexpr uint32_t kFrame = 48;     // `addiu sp,sp,-48` at 0x800903F4
constexpr uint32_t kPushFrame = 40; // `addiu sp,sp,-40` at 0x800BCA68, the stance event's caller
} // namespace rm

void SetupRemount(CaseContext& c, Args& a) {
    Rng& rng = *c.rng;
    Memory& m = *c.mem;
    const bool derived = c.sample != nullptr;
    auto chance = [&rng](uint32_t n) { return rng.Next() % n == 0; };
    const uint32_t base = m.PeekWord(kPoolTable);
    const uint32_t hp = m.PeekWord(kPoolTable + 12u);
    uint32_t slots = (hp >= 0x80000000u) ? m.PeekWord(hp) + 1u : 1u;
    if (slots > 32u) slots = 1u;
    const uint32_t slot = static_cast<uint32_t>(rng.Next() % slots);
    const uint32_t B = base + 1096u * slot;
    const uint32_t R = m.PeekWord(B + 852u);
    const uint32_t gs = m.PeekWord(kGameStatePtr);
    if (chance(4)) WriteU16Bench(m, B + 320u, m.PeekByte(B + 320u) ? 0 : 1);
    uint32_t fc = m.PeekWord(B + 568u);
    fc = chance(2) ? (fc | 0x00400000u) : (fc & ~0x00400000u);
    m.PokeWord(B + 568u, fc);
    m.PokeWord(R + 552u, derived ? (m.PeekWord(R + 552u) ^ (rng.U32() & rng.U32())) : rng.U32());
    // the two-rider arm
    uint8_t b23c = m.PeekByte(R + 572u);
    if (chance(4) && slots > 1u) {
        b23c = static_cast<uint8_t>(b23c | 0x10u);
        const uint32_t other = (slot + 1u + static_cast<uint32_t>(rng.Next() % (slots - 1u))) % slots;
        m.PokeWord(B + 856u, base + 1096u * other);
    } else {
        b23c = static_cast<uint8_t>(b23c & ~0x10u);
    }
    m.WriteBlock(R + 572u, &b23c, 1);
    {
        const uint8_t b39 = static_cast<uint8_t>(chance(3) ? 1u : (chance(2) ? 2u : m.PeekByte(gs + 57u)));
        m.WriteBlock(gs + 57u, &b39, 1);
        if (chance(4)) {
            const uint8_t b4 = 44;
            m.WriteBlock(gs + 4u, &b4, 1);
            const uint8_t two = 2;
            if (chance(2)) m.WriteBlock(gs + 57u, &two, 1);
        }
    }
    // riderDef +0x28 / +0x27: RACE (4) or the finished op 2
    const uint32_t rd = m.PeekWord(B + 1084u);
    m.PokeWord(rd + 40u, chance(4) ? rng.U32() | 1u : 0u);
    const uint8_t b27 = static_cast<uint8_t>(chance(4) ? 248u + rng.U32() % 8u : rng.U32() % 248u);
    m.WriteBlock(rd + 39u, &b27, 1);
    // A bike TURNED round (its facing +0x210 and lateral +0x204 rows and its heading negated): the
    // re-facing callee 0x80096564 then settles the other road direction, so the passenger's lateral
    // takes both arms of 0x800906CC.
    if (chance(3)) {
        for (uint32_t off : {0x1C2u, 0x204u, 0x210u})
            for (uint32_t k = 0; k < 3; ++k) {
                const uint32_t at = B + off + 2u * k;
                WriteU16Bench(m, at, static_cast<uint16_t>(0u - (m.PeekByte(at) | (m.PeekByte(at + 1) << 8))));
            }
    }
    // No route object (+0x1AC = 0, a rider the route does not bind): 0x80096564 then takes its
    // direction from flagsC bit 22 instead of the route leg, so the re-faced +0x16C takes both signs.
    if (chance(4)) m.PokeWord(B + 0x1ACu, 0);
    // the road direction +0x16C, which the passenger arm reads when it is not rewritten here
    m.PokeWord(B + 364u, chance(2) ? 2u : (chance(2) ? 0xFFFFFFFEu : rng.U32()));
    // The rider object +0x22C (0 in every capture): a planted non-null object - a scratch block the
    // releaser 0x80095AEC may clear +0x24 of - with a planted index +0x23B, so both of its arms run
    // (the pool bit in 0x8005AD50 set or not).
    if (chance(6)) {
        m.PokeWord(R + 556u, c.scratch + 64u);
        const uint8_t idx = static_cast<uint8_t>(rng.U32() % 8u);
        m.WriteBlock(R + 571u, &idx, 1);
    }
    const uint8_t depth = static_cast<uint8_t>(rng.U32() % 16u);
    if (!derived || chance(2)) m.WriteBlock(B + 0x3B2u, &depth, 1);
    if (!derived) {
        m.PokeWord(gs + 16u, rng.U32());
        m.PokeWord(B + 348u, static_cast<uint32_t>(rng.S32() >> (8 + rng.Next() % 10u)));
        m.PokeWord(B + 344u, static_cast<uint32_t>(rng.S32() >> (8 + rng.Next() % 10u)));
        m.PokeWord(B + 304u, static_cast<uint32_t>(rng.S32() >> (12 + rng.Next() % 6u)));
    }
    a.a[0] = B;
    a.a[1] = chance(2) ? 0u : (chance(4) ? rng.U32() : 1u);
}

struct RemountOracle final : rr::sim::RemountCallees {
    NativeEnv& env;
    explicit RemountOracle(NativeEnv& e) : env(e) {}
    bool Call(uint32_t f, uint32_t a0, uint32_t a1 = 0, uint32_t a2 = 0, uint32_t a3 = 0) {
        env.CallOracle(f, a0, a1, a2, a3);
        return env.failure.empty();
    }
    uint8_t* Raw(uint32_t a, uint32_t n) {
        const uint32_t phys = a & 0x1FFFFFFFu;
        if (phys + n > env.clone->mem.ram().size()) return nullptr;
        return env.clone->mem.ram().data() + phys;
    }
    bool RowsFromHeading(uint32_t B) override { return Call(rm::kRowsFromHeading, B); }
    bool ResetBikeState(uint32_t B) override { return Call(rm::kResetBike, B); }
    bool ReleaseRiderObject(uint32_t R) override { return Call(rm::kReleaseObj, R); }
    bool Attach(uint32_t B, uint32_t R, int32_t kind, int32_t seat) override {
        return Call(rm::kAttach, B, R, static_cast<uint32_t>(kind), static_cast<uint32_t>(seat));
    }
    bool Dismount(uint32_t R, int32_t how) override { return Call(rm::kDismount, R, static_cast<uint32_t>(how)); }
    bool ClearCommands(uint32_t B) override { // PORTED (ai.h), on the clone's own RAM
        uint8_t* e = Raw(B, 1096);
        if (e == nullptr) return false;
        rr::sim::AiClearCommands(e);
        return true;
    }
    bool PushCommand(uint16_t op, uint16_t target, int32_t mode, uint32_t B) override { // PORTED
        uint8_t* e = Raw(B, 1096);
        const uint32_t R = env.clone->mem.PeekWord(B + 852u);
        uint8_t* rider = Raw(R, 640);
        const uint8_t* alt = Raw(kAltKindTable, 8u * 65536u);
        const uint8_t* fight = Raw(env.clone->mem.PeekWord(kFightRecPtr), 12u * 256u);
        if (e == nullptr || rider == nullptr || alt == nullptr || fight == nullptr) return false;
        struct Sink final : rr::sim::AiStanceSink {
            NativeEnv& env;
            explicit Sink(NativeEnv& v) : env(v) {}
            void PlayIdleStance(uint16_t event, uint32_t riderArg) override {
                env.CallOracle(kAiStance, event, riderArg, 2, 0);
            }
        } sink(env);
        rr::sim::AiPushEnv pe;
        pe.raceClock = static_cast<int32_t>(env.clone->mem.PeekWord(env.clone->mem.PeekWord(kGameStatePtr) + 0x10u));
        pe.rider = rider;
        pe.riderAddress = R;
        pe.altKindTable = alt;
        pe.fightRecords = fight;
        pe.stance = &sink;
        uint8_t cmd[8] = {static_cast<uint8_t>(op), static_cast<uint8_t>(op >> 8), static_cast<uint8_t>(target),
                          static_cast<uint8_t>(target >> 8), 0, 0, 0, 0};
        rr::sim::AiPushCommand(cmd, mode, e, pe);
        return env.failure.empty();
    }
    bool ReFace(uint32_t B) override { return Call(rm::kReFace, B); }
    bool ViewReset(uint32_t view) override { return Call(rm::kViewReset, view); }
    bool PlayerVoice(uint32_t h, int32_t a1) override { return Call(rm::kPlayerVoice, h, static_cast<uint32_t>(a1)); }
    bool PlayerBind(uint32_t h, uint32_t B) override { return Call(rm::kPlayerBind, h, B); }
};

uint32_t NativeRemount(NativeEnv& env, const Args& a) {
    rr::sim::GuestRam g(env.clone->mem.ram().data(), env.gp);
    RemountOracle calls(env);
    const bool ok = rr::sim::Remount(g, a.a[0], static_cast<int32_t>(a.a[1]), calls);
    if (!ok && env.failure.empty() && !g.Faulted()) env.failure = "Remount refused";
    return RqDone(env, g, 0);
}

// ---------------------------------------------------------------- ScaleTo16 SLUS 0x8002EED8,
// MassExchange RASHCDG 0x80083864: two leaves of the bike-bike gate, each its own row.
void SetupScaleTo16(CaseContext& c, Args& a) {
    Rng& rng = *c.rng;
    Memory& m = *c.mem;
    for (uint32_t i = 0; i < 32; i += 4) m.PokeWord(c.scratch + i, rng.U32() >> (rng.Next() % 20u));
    if (c.sample != nullptr) PutVec3S32(m, c.scratch, c.sample->p);
    a.a[0] = (rng.Next() % 3u == 0) ? rng.U32() : (rng.U32() >> (4 + rng.Next() % 20u));
    a.a[1] = c.scratch;
    const uint32_t alias = static_cast<uint32_t>(rng.Next() % 6u);
    a.a[2] = alias == 0 ? c.scratch : alias == 1 ? c.scratch + 2 : c.scratch + 16; // out over v, or apart
}
uint32_t NativeScaleTo16(Memory& m, uint32_t, uint32_t gp, const Args& a) {
    rr::sim::GuestRam g(m.ram().data(), gp);
    rr::sim::ScaleTo16(g, static_cast<int32_t>(a.a[0]), a.a[1], a.a[2]);
    return g.Faulted() ? 0xDEADDEADu : 0u;
}

void SetupMassExchange(CaseContext& c, Args& a) {
    Rng& rng = *c.rng;
    auto pick = [&rng](std::initializer_list<uint32_t> v) { return *(v.begin() + rng.Next() % v.size()); };
    // masses as the gate hands them (FixMul(mass, 0x80000000 / (m1 + m2)): a fraction of 1.0), speeds
    for (int k = 0; k < 2; ++k) a.a[k] = pick({rng.U32() % 0x10000u, 0x8000u, rng.U32() >> (rng.Next() % 20u), rng.U32()});
    a.a[2] = c.sample != nullptr ? static_cast<uint32_t>(c.sample->speed) : pick({0u, rng.U32() % 0x800000u, rng.U32()});
    a.a[3] = pick({rng.U32() % 0x800000u, 0u, rng.U32()});
    a.a4 = c.scratch;
    a.more[0] = (rng.Next() % 5u == 0) ? c.scratch : c.scratch + 8u; // both outputs to one word, or apart
    c.mem->PokeWord(c.scratch, rng.U32());
    c.mem->PokeWord(c.scratch + 8u, rng.U32());
}
uint32_t NativeMassExchange(Memory& m, uint32_t, uint32_t gp, const Args& a) {
    rr::sim::GuestRam g(m.ram().data(), gp);
    rr::sim::MassExchange(g, static_cast<int32_t>(a.a[0]), static_cast<int32_t>(a.a[1]), static_cast<int32_t>(a.a[2]),
                          static_cast<int32_t>(a.a[3]), a.a4, a.more[0]);
    return g.Faulted() ? 0xDEADDEADu : 0u;
}

// ---------------------------------------------------------------- BikeBikeGate, 0x80081D7C
//
// Scratch: bike a at +0, bike b at +1100 (both 1096-byte pool-0 records: captured bikes, whose
// pointer fields name this machine's real riders, stat blocks and road), the contact normal n
// (s16[3]) at +2200. Arguments a, b, codeA, codeB in a0..a3 and &n at sp+16, as BikeBikeReact
// leaves them. THE STALE FRAME WORDS: the setup writes the two words the ANGLED arm reads out of
// the gate's own frame (entry sp - 280 + 108 / + 116) - the live pair (8, 0x800B4C58), a pair
// inside the limit, or random words - and the two closing speeds the REAR arm never writes but the
// JOIN still hands HitSpeed (+ 120 / + 124); the port is handed exactly what is there.
//
// Driven: which arm - both crashed, one crashed (the other airborne ahead / behind, or knocked on,
// or not: flags 0x7FF, a player partner, flagsA bit 29), airborne above / below (half height +0x138
// against the vertical offset), MAIN with every face code (0x200 codes 1..3 and the low-bit
// codes), REAR both ways round, HEAD with speed gaps beyond +-10.0, ANGLED with t1 = 0 and 1 (the
// 13.43 gap and the sign of the closing speed), SIDE with the same partner, with the momentum GTE
// normal (either speed above 1.0) or the +0x32E normals; flagsC bits 7 and 8 and 0..3, the latch
// bit 25; the masses +0x13C; the handles +0xAC (a player or not; game_state +0x30 of 1 or 2).
//
// Named bounds: every direction (n, the rows +0x1B6 / +0x204 / +0x20A / +0x210 / +0x32E, the
// headings +0x1C2 / +0x360) is at most unit length and the speeds within +-128.0, as in every
// capture - Bounce and Spin run inside the gate, and a longer direction makes Normalize's trapping
// `add` raise the console's overflow exception; the handles are 0..17 (RiderSpeech indexes the pool
// with them).
namespace gt {
constexpr uint32_t kGate = 0x80081D7C, kImpactTurn = 0x80083F30, kHitSpeed = 0x80080D1C;
constexpr uint32_t kTakeHeading = 0x80080B10, kRiderSpeech = 0x8001A760;
constexpr uint32_t kFrame = 280;          // `addiu sp,sp,-280` at 0x80081D7C
constexpr uint32_t kStaleAng1 = 108, kStaleLim1 = 116; // frame slots ang[1] / lim[1] (ANGLED)
constexpr uint32_t kStaleCl0 = 120, kStaleCl1 = 124;   // frame slots cl[0] / cl[1] (REAR)
constexpr uint32_t kNrmSlot = 144;        // nrm[k] at sp+144 + 6k
} // namespace gt

void SetupGate(CaseContext& c, Args& a) {
    Rng& rng = *c.rng;
    Memory& m = *c.mem;
    const bool derived = c.sample != nullptr;
    auto chance = [&rng](uint32_t n) { return rng.Next() % n == 0; };
    auto pick = [&rng](std::initializer_list<uint32_t> v) { return *(v.begin() + rng.Next() % v.size()); };
    const uint32_t A = c.scratch, Bb = c.scratch + 1100u, N = c.scratch + 2200u;
    std::vector<uint8_t> ea = derived ? c.sample->entity : c.any->entity;
    std::vector<uint8_t> eb = c.any->entity;
    if (derived && chance(2)) eb = c.sample->entity;
    auto unitRow = [&](std::vector<uint8_t>& e, uint32_t off) {
        int16_t v[3];
        RandomDir(rng, v, 4096);
        for (uint32_t k = 0; k < 3; ++k) PutU16V(e, off + 2u * k, static_cast<uint16_t>(v[k]));
    };
    auto speed = [&]() {
        return pick({0x10000u, 0x10001u, 0xFFFFu, 0u, 0xA0000u, 0xD6944u, rng.U32() % 0x800000u,
                     rng.U32() % 0x3C0000u, static_cast<uint32_t>(rng.S32() >> 9)});
    };
    const bool closeTogether = chance(2);
    for (int w = 0; w < 2; ++w) {
        std::vector<uint8_t>& e = w == 0 ? ea : eb;
        if (!derived || chance(2))
            for (uint32_t off : {0x1B0u, 0x1B6u, 0x1BCu, 0x204u, 0x20Au, 0x210u, 0x1C2u, 0x32Eu, 0x360u}) unitRow(e, off);
        if (!derived || chance(2)) {
            PutU32V(e, 0x1E0, speed());
            PutU32V(e, 0x35C, speed());
            PutU32V(e, 0x1E8, static_cast<uint32_t>(rng.S32() >> (8 + rng.Next() % 10u)));
        }
        // masses: the game's (475.0 / 275.0 / 1360.0 in the captures), and - randomised only - zero or
        // negative, so the four sign arms of SIDE's divide (0x80082EE0..0x80082F44) run
        PutU32V(e, 0x13C, pick({475u << 16, 275u << 16, 1360u << 16, 1u << 16, rng.U32() % 0x8000000u}));
        if (!derived && chance(6)) PutU32V(e, 0x13C, pick({0u, 0u - (rng.U32() % 0x8000000u)}));
        PutU32V(e, 0x138, pick({0x19800u, 0x10000u, rng.U32() % 0x80000u}));
        if (closeTogether && w == 1)
            for (uint32_t k = 0; k < 3; ++k)
                PutU32V(e, 0xB8 + 4u * k, GetU32V(ea, 0xB8 + 4u * k) + static_cast<uint32_t>(rng.S32() >> 13));
        uint32_t fc = derived && chance(2) ? GetU32V(e, 0x238) : rng.U32();
        fc &= ~0x0200077Fu;
        if (chance(4)) fc |= 0x200u;                      // crashed
        if (chance(4)) fc |= 0x400u;                      // airborne
        if (chance(4)) fc |= pick({1u, 2u, 4u, 8u, 0x20u, 0x40u, 0x100u});
        if (chance(5)) fc |= 0x80u;                       // the 10-degree step
        if (chance(3)) fc |= 0x02000000u;                 // already latched
        if (chance(6)) fc |= 0x80000u;                    // Spin's "no spin"
        PutU32V(e, 0x238, fc);
        uint32_t fa = GetU32V(e, 0x230);
        fa = chance(5) ? (fa | 0x20000000u) : (fa & ~0x20000000u);
        PutU32V(e, 0x230, fa);
        PutU16V(e, 0xAC, static_cast<uint16_t>(pick({0u, 1u + rng.U32() % 17u, 1u + rng.U32() % 17u})));
    }
    // the same partner as last time (SIDE's early JOIN)
    PutU32V(ea, 0x33C, chance(6) ? Bb + 172u : GetU32V(ea, 0x33C));
    PutU32V(eb, 0x33C, chance(6) ? A + 172u : GetU32V(eb, 0x33C));
    m.WriteBlock(A, ea.data(), ea.size());
    m.WriteBlock(Bb, eb.data(), eb.size());
    int16_t n[3];
    RandomDir(rng, n, 4096);
    PutVec3S16(m, N, n);
    // game_state +0x30: one player, or two (the "both players" gate s8)
    if (chance(6)) m.PokeWord(m.PeekWord(kGameStatePtr) + 48u, 2);
    // the stale frame words ang[1] / lim[1]
    const uint32_t frame = c.sp - gt::kFrame;
    const uint32_t which = static_cast<uint32_t>(rng.Next() % 4u);
    const uint32_t s = which == 0 ? 8u : which == 1 ? static_cast<uint32_t>(rng.S32() >> 14) : rng.U32();
    const uint32_t l = which == 0 ? 0x800B4C58u : which == 1 ? pick({0xF5BEu, 0x1226Cu, rng.U32() % 0x30000u}) : rng.U32();
    m.PokeWord(frame + gt::kStaleAng1, s);
    m.PokeWord(frame + gt::kStaleLim1, l);
    // ... and the REAR arm's closing speeds: zero, a closing speed of the game's size, or anything
    for (uint32_t off : {gt::kStaleCl0, gt::kStaleCl1})
        m.PokeWord(frame + off, pick({0u, static_cast<uint32_t>(rng.S32() >> 9), rng.U32()}));
    auto code = [&]() {
        return chance(2) ? (0x200u | pick({1u, 2u, 3u, 0u, rng.U32() % 256u})) : (rng.U32() % 16u | (chance(8) ? rng.U32() & ~0x2FFu : 0u));
    };
    a.a[0] = A;
    a.a[1] = Bb;
    a.a[2] = code();
    a.a[3] = code();
    a.a4 = N;
}

struct GateOracle final : rr::sim::GateCallees {
    NativeEnv& env;
    std::vector<std::array<int16_t, 3>> seen; // HitSpeed's frame argument as it was when the callee returned
    explicit GateOracle(NativeEnv& e) : env(e) {}
    bool ImpactTurn(uint32_t e, uint32_t partner, uint32_t n, int32_t mode) override {
        env.CallOracle(gt::kImpactTurn, e, partner, n, static_cast<uint32_t>(mode));
        return env.failure.empty();
    }
    bool HitSpeed(uint32_t e, uint32_t dir, uint32_t pSpeed, uint32_t partner, const int16_t nrm[3], uint32_t nrmSlot,
                  int32_t k, int32_t ang, int32_t lim, int32_t out, int32_t a9, int32_t& v0) override {
        // The buffer goes where the ORIGINAL keeps it - its own frame slot sp+144 + 6k, the same guest
        // address, inside the excluded stack window - so the pointer argument is compared as a plain
        // number, and its content is compared against a reference run of the original (NativeGate).
        const uint32_t at = env.sp + gt::kNrmSlot + 6u * nrmSlot;
        for (uint32_t i = 0; i < 3; ++i) WriteU16Bench(env.clone->mem, at + 2u * i, static_cast<uint16_t>(nrm[i]));
        // A development control, off unless RRJB_GATE_NRM_LSB is set: one LSB off in the frame argument
        // (a measured control). The own-frame check must then fail the row.
        if (std::getenv("RRJB_GATE_NRM_LSB") != nullptr)
            WriteU16Bench(env.clone->mem, at, static_cast<uint16_t>(nrm[0] ^ 1));
        const uint32_t args[11] = {e, dir, pSpeed, partner, at, static_cast<uint32_t>(k), static_cast<uint32_t>(ang),
                                   static_cast<uint32_t>(lim), static_cast<uint32_t>(out), static_cast<uint32_t>(a9), 0};
        v0 = static_cast<int32_t>(env.CallOracleArgs(gt::kHitSpeed, args, 11));
        seen.push_back({GetS16(env.clone->mem, at), GetS16(env.clone->mem, at + 2), GetS16(env.clone->mem, at + 4)});
        return env.failure.empty();
    }
    bool TakePartnerHeading(uint32_t e, uint32_t dir, uint32_t pSpeed, uint32_t partnerDir, int32_t out,
                            int32_t& v0) override {
        const uint32_t args[5] = {e, dir, pSpeed, partnerDir, static_cast<uint32_t>(out)};
        v0 = static_cast<int32_t>(env.CallOracleArgs(gt::kTakeHeading, args, 5));
        return env.failure.empty();
    }
    bool RiderSpeech(uint32_t h, int32_t crash) override {
        env.CallOracle(gt::kRiderSpeech, h, static_cast<uint32_t>(crash), 0, 0);
        return env.failure.empty();
    }
};

// Counts of the own-frame check, printed once at exit: a check that never compares proves nothing.
struct GateFrameStats {
    long cases = 0, compared = 0, differed = 0;
    ~GateFrameStats() {
        if (cases != 0)
            std::printf("bike_bike_gate own-frame check: %ld case(s), %ld HitSpeed frame argument(s) compared, "
                        "%ld differed (a difference fails the case)\n", cases, compared, differed);
    }
};
GateFrameStats& GateStats() {
    static GateFrameStats s;
    return s;
}

uint32_t NativeGate(NativeEnv& env, const Args& a) {
    Memory& m = env.clone->mem;
    ++GateStats().cases;
    // ---- the reference: the ORIGINAL gate on a private copy of this very machine, with HitSpeed's
    // frame argument (argument 4, 6 bytes) captured when each call returns - the bench's version of
    // the model harness's "own-frame check". Only the gate's OWN calls are
    // compared (sp == the gate's sp); ImpactTurn's calls of HitSpeed are the oracle's business.
    std::vector<std::array<int16_t, 3>> ref;
    std::vector<Cpu::CallObservation> refCalls;
    {
        Machine rm;
        bs::CopyMachine(*env.clone, rm);
        rm.cpu.ClearCallObservers();
        rm.cpu.ObserveCalls(gt::kHitSpeed);
        rm.cpu.ObserveCallFrameOut(gt::kHitSpeed, 4, 6);
        if (std::getenv("RRJB_GATEDEBUG") != nullptr)
            for (uint32_t f : {gt::kImpactTurn, gt::kTakeHeading, gt::kRiderSpeech}) rm.cpu.ObserveCalls(f);
        bs::StartCall(rm.cpu, gt::kGate, a, env.sp + gt::kFrame);
        const Trap t = rm.cpu.Run(kSentinel, env.maxSteps);
        if (t.kind != TrapKind::Halted) {
            env.failure = "the reference run of the original gate trapped: " + t.ToString();
            return 0;
        }
        refCalls = rm.cpu.callObservations;
        for (const Cpu::CallObservation& o : rm.cpu.callObservations) {
            if (o.sp != env.sp || o.address != gt::kHitSpeed) continue;
            const Cpu::FrameOutCapture* f = o.FrameOutFor(4);
            std::array<int16_t, 3> v{};
            if (f != nullptr && f->len == 6) std::memcpy(v.data(), f->bytes, 6);
            ref.push_back(v);
        }
    }
    int32_t stale[4] = {GetS32(m, env.sp + gt::kStaleAng1), GetS32(m, env.sp + gt::kStaleLim1),
                        GetS32(m, env.sp + gt::kStaleCl0), GetS32(m, env.sp + gt::kStaleCl1)};
    // A development control, off unless RRJB_GATE_NOSTALE is set: hand the port zeroes instead of the
    // stale frame words. The row must then FAIL - that is what shows the words are load-bearing.
    if (std::getenv("RRJB_GATE_NOSTALE") != nullptr)
        for (int32_t& w : stale) w = 0;
    rr::sim::GuestRam g(m.ram().data(), env.gp);
    const Tables tab(m, env.gp);
    GateOracle calls(env);
    bool ok = false;
    const int32_t v = rr::sim::BikeBikeGate(g, a.a[0], a.a[1], a.a[2], a.a[3], a.a4, stale, tab.t, calls, ok);
    if (!ok && env.failure.empty() && !g.Faulted()) env.failure = "BikeBikeGate refused";
    if (std::getenv("RRJB_GATEDEBUG") != nullptr) {
        auto dump = [](const char* who, const std::vector<Cpu::CallObservation>& v) {
            std::printf("  %s:", who);
            for (const Cpu::CallObservation& o : v) {
                std::printf(" [%08X sp %08X", o.address, o.sp);
                for (int k = 0; k < 11; ++k) std::printf(" %X", o.a[k]);
                std::printf("]");
            }
            std::printf("\n");
        };
        dump("ref ", refCalls);
        dump("port", env.calls);
    }
    if (env.failure.empty() && !env.skipOracle) {
        if (calls.seen.size() != ref.size()) {
            env.failure = "own-frame check: the gate made " + std::to_string(ref.size()) +
                          " HitSpeed call(s) of its own, the port " + std::to_string(calls.seen.size());
        } else {
            for (size_t i = 0; i < ref.size(); ++i) {
                ++GateStats().compared;
                if (ref[i] != calls.seen[i]) {
                    ++GateStats().differed;
                    env.failure = "own-frame check: HitSpeed's frame argument differs at call " + std::to_string(i);
                    break;
                }
            }
        }
    }
    return RqDone(env, g, static_cast<uint32_t>(v));
}

} // namespace cr

// Every row is declared as `Row{"name", "signature", 0x<entry>, ...}` with the entry as a literal, so the
// probes that read the bench's row table (crash.py, spawn.py) see it.
void AddCrashRows(std::vector<Row>& rows) {
    rows.push_back(Row{"scale_to16", "void ScaleTo16(s32 t, const s32 v[3], s16 out[3])  [SLUS]", 0x8002EED8,
                       cr::SetupScaleTo16, cr::NativeScaleTo16, /*compareV0=*/false});
    rows.push_back(Row{"mass_exchange", "void 0x80083864(m1, m2, v1, v2, *o1, [*o2])  [RASHCDG]", 0x80083864,
                       cr::SetupMassExchange, cr::NativeMassExchange, /*compareV0=*/false});
    {
        rows.push_back(Row{"bike_bike_gate", "s32 0x80081D7C(a, b, codeA, codeB, [s16 *n])  [RASHCDG, one GTE OP]",
                           0x80081D7C, cr::SetupGate, nullptr});
        Row& r = rows.back();
        r.nativeEx = cr::NativeGate;
        r.oracleFrame = cr::gt::kFrame;
        r.oracleCallees = {OracleCallee{cr::gt::kImpactTurn, 4}, OracleCallee{cr::gt::kHitSpeed, 4, 7},
                           OracleCallee{cr::gt::kTakeHeading, 4, 1}, OracleCallee{cr::gt::kRiderSpeech, 2}};
    }
    {
        rows.push_back(Row{"remount", "void 0x800903F4(Bike *B, s32 fromRoad)  [RASHCDG, the instant remount]",
                           0x800903F4, cr::SetupRemount, nullptr});
        Row& r = rows.back();
        r.compareV0 = false;
        r.nativeEx = cr::NativeRemount;
        r.oracleFrame = cr::rm::kFrame;
        r.oracleCallees = {OracleCallee{cr::rm::kRowsFromHeading, 1}, OracleCallee{cr::rm::kResetBike, 1},
                           OracleCallee{cr::rm::kReleaseObj, 1},       OracleCallee{cr::rm::kAttach, 4},
                           OracleCallee{cr::rm::kDismount, 2},         OracleCallee{kAiStance, 3, 0, {}, cr::rm::kPushFrame},
                           OracleCallee{cr::rm::kReFace, 1},           OracleCallee{cr::rm::kViewReset, 1},
                           OracleCallee{cr::rm::kPlayerVoice, 2},      OracleCallee{cr::rm::kPlayerBind, 2}};
    }
    rows.push_back(Row{"muladd16", "s32 MulAdd16(const s16 b[3], const s16 d[3], s32 t, s32 out[3])  [SLUS]",
                       0x8002EA20, cr::SetupMulAdd16, cr::NativeMulAdd16});
    {
        rows.push_back(Row{"spin", "void 0x800849D8(Bike *e)  [RASHCDG, three Rand]", 0x800849D8, cr::SetupSpin, nullptr});
        Row& r = rows.back();
        r.compareV0 = false;
        r.nativeEx = cr::NativeSpin;
    }
    rows.push_back(Row{"bounce", "s32 0x80084564(n, d, *speed, vel, [k, floor, flat])  [RASHCDG]", 0x80084564,
                       cr::SetupBounce, cr::NativeBounce});
    {
        rows.push_back(Row{"impact_state_pass", "void 0x80078DB4(Node *head)  [RASHCDG, the impact state machine]",
                           0x80078DB4, cr::SetupImpact, nullptr});
        Row& r = rows.back();
        r.compareV0 = false;
        r.nativeEx = cr::NativeImpact;
    }
}
