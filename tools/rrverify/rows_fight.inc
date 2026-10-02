// Bench rows for combat. Included by
// verify_physics.cpp inside its anonymous namespace, so everything defined above that point is visible.
//
// The native code is src\game\sim\fight.{h,cpp}, a GUEST-ADDRESS port: the native side runs straight on
// the clone's RAM. The ported callees that live elsewhere (the stance event, the animation clip test,
// the sound emitter and queue, EndRace, SetAimDelta) and the unported ones (Arrest, the pad motors,
// the rider's line, the weapon objects, the overlay clip) are executed by the ORACLE and witnessed; the
// AI command stack's push and pop run natively (ai.h) on the clone, their stance request on the oracle.
//
// THE WORLD. Every case is built on LIVE pool-0 bikes of the bench's own machine (rr-race: 18 bikes,
// their pool-1 riders, their runtime rider records, FIGHT.BIN at *(0x8005AD4C), the riders' animation
// objects), because the fight code chases bike -> rider -> animation object -> clip and the FIGHT.BIN
// node blocks; a bike lifted out of another image would point at state this machine does not hold.
// `Stir` then drives every field the functions branch on (below), and each row picks its arguments.
//
// Named bounds (each a range the original's own code keeps, or a pointer it never sees null):
//   * a bike whose list node +0x440 is 0, or whose rider has +0x23C bit 4 (passenger), gets a live
//     partner +0x358: the code reads the partner with no null test there, and on rr-race the null
//     partner's word 0x354 is a BIOS-ROM address;
//   * the FIGHT.BIN record +0x239 stays 0..39 and the queue's node ids are the record's own or 63
//     (41 = "none" indexes past the 40 records into the node blocks, which the original never does
//     with a live queue);
//   * the rider stance +0x220 is a stance id < 223, the animation frame +0x10 in 0..40;
//   * game_state +0x3C (the bank) is 0..5 - it indexes the 6-entry SPEED and DIV tables.
namespace fg {

namespace F = rr::sim::fight;

constexpr uint32_t kClipDone = 0x8005BE58, kSound3D = 0x80017BA0, kQueueSound = 0x80017B6C;
constexpr uint32_t kEndRace = 0x80092C7C, kSetAim = 0x80093CAC, kArrest = 0x80096F30;
constexpr uint32_t kPadMotor = 0x8001DD74, kSpeech = 0x8001A760, kWeaponObj = 0x800958F0;
constexpr uint32_t kObjRelease = 0x80068D20, kObjStop = 0x8002847C, kObjSound = 0x800273EC;
constexpr uint32_t kOverlayClip = 0x800C2F84;
constexpr uint32_t kPadRecord = 0x800D7128;

struct World {
    uint32_t gs = 0, pool0 = 0, n = 0;
    uint32_t me = 0, t = 0, x = 0;       // three distinct live bikes
    uint32_t meSlot = 0, tSlot = 0;
    std::vector<uint16_t> cat3, other; // stance ids of category 3 and of another category
};

uint8_t B8(Memory& m, uint32_t a) { return m.PeekByte(a); }
void W8(Memory& m, uint32_t a, uint32_t v) { const uint8_t b = static_cast<uint8_t>(v); m.WriteBlock(a, &b, 1); }
void W16(Memory& m, uint32_t a, uint32_t v) { WriteU16Bench(m, a, static_cast<uint16_t>(v)); }
uint16_t H16(Memory& m, uint32_t a) { return static_cast<uint16_t>(GetS16(m, a)); }

bool Valid(Memory& m, uint32_t e) {
    const uint32_t R = m.PeekWord(e + 852u), rd = m.PeekWord(e + 1084u);
    if (R < 0x80000000u || R >= 0x801FF000u || rd < 0x80000000u || rd >= 0x801FF000u) return false;
    const uint32_t a = m.PeekWord(R + 0x21Cu);
    if (a < 0x80000000u || a >= 0x801FF000u || (a & 3u)) return false;
    return m.PeekWord(R + 0x254u) == e;
}

// Picks three distinct bikes whose rider, runtime record and animation object are real.
World Pick3(CaseContext& c, bool playerMe = false) {
    World w;
    Memory& m = *c.mem;
    Rng& rng = *c.rng;
    w.gs = m.PeekWord(kGameStatePtr);
    w.pool0 = m.PeekWord(kPoolTable);
    const uint32_t hp = m.PeekWord(kPoolTable + 12u);
    w.n = (hp >= 0x80000000u) ? m.PeekWord(hp) + 1u : 1u;
    if (w.n > 32u) w.n = 1u;
    std::vector<uint32_t> ok;
    for (uint32_t i = 0; i < w.n; ++i)
        if (Valid(m, w.pool0 + 1096u * i)) ok.push_back(i);
    if (ok.size() < 3) return w;
    for (size_t i = ok.size() - 1; i > 0; --i) std::swap(ok[i], ok[rng.Next() % (i + 1)]);
    if (playerMe) // the pad path's bike: a player (handle < players), as PollPads only calls it for one
        for (size_t i = 0; i < ok.size(); ++i)
            if (H16(m, w.pool0 + 1096u * ok[i] + 0xACu) == 0) { std::swap(ok[0], ok[i]); break; }
    w.meSlot = ok[0];
    w.tSlot = ok[1];
    w.me = w.pool0 + 1096u * ok[0];
    w.t = w.pool0 + 1096u * ok[1];
    w.x = w.pool0 + 1096u * ok[2];
    // Stances whose animation bank (w0 & 15) is one of the six the descriptor's bank table 0x800CE190 holds.
    for (uint32_t s = 0; s < 223; ++s)
        if ((m.PeekWord(kAltKindTable + 8u * s) & 15u) < 6u)
            (H16(m, kAltKindTable + 8u * s + 2u) == 3 ? w.cat3 : w.other).push_back(static_cast<uint16_t>(s));
    return w;
}

uint32_t Fight(Memory& m) { return m.PeekWord(kFightRecPtr); }

// SAFE ANIMATION (a named bound, for every row that reaches the stance event): the rider's fight record
// is one of the bare-fisted / kicking sets every rider's animation bank holds (records 0, 1, 10, 11, 16,
// 17), the weapon is fists (+0x2E = 9, no weapon owned, the move list bare-fisted) and the passenger
// bit +0x23C 0x20 is clear. A weapon or passenger record whose clips this rider's bank does not hold
// makes the ORIGINAL read a garbage clip pointer (0x8005BF8C) - a state the game never builds, since
// it picks the record from the weapon it has (FightPickRecord's own row drives every record). The
// rider's +0x23C bit 6 is clear too (see StirBike).
bool gSafeAnim = false;
uint32_t CountB(Memory& m, uint32_t rec) { return B8(m, Fight(m) + 12u * rec + 3u); }

// Drives one bike, its rider and its runtime record. `near` is the bike the geometry is made
// relative to (0: none).
void StirBike(CaseContext& c, const World& w, uint32_t e, uint32_t near, uint32_t partner) {
    Memory& m = *c.mem;
    Rng& rng = *c.rng;
    auto chance = [&rng](uint32_t n) { return rng.Next() % n == 0; };
    const uint32_t R = m.PeekWord(e + 852u), rd = m.PeekWord(e + 1084u), an = m.PeekWord(R + 0x21Cu);
    // --- the bike
    if (near != 0 && !chance(5)) {
        // beside `near`: the same road piece, a small lateral and along offset
        for (uint32_t off : {0x168u, 0x150u, 0x16Cu}) m.PokeWord(e + off, m.PeekWord(near + off));
        const int32_t dl = static_cast<int32_t>(rng.U32() % 0x60000u) - 0x30000;
        m.PokeWord(e + 0x158u, m.PeekWord(near + 0x158u) + static_cast<uint32_t>(dl));
        for (uint32_t k = 0; k < 3; ++k) {
            const int32_t d = static_cast<int32_t>(rng.U32() % 0x30000u) - 0x18000;
            m.PokeWord(e + 0x1F8u + 4u * k, m.PeekWord(near + 0x1F8u + 4u * k) + static_cast<uint32_t>(d));
            m.PokeWord(e + 0xB8u + 4u * k, m.PeekWord(near + 0xB8u + 4u * k) + static_cast<uint32_t>(d / 2));
        }
        if (chance(3)) m.PokeWord(e + 0x144u, m.PeekWord(near + 0x144u) + (rng.U32() % 0x20000u));
        if (chance(4)) m.PokeWord(e + 0x168u, rng.U32());
    } else if (c.sample != nullptr) {
        PutVec3S32(m, e + 0x1F8u, c.sample->p);
        PutVec3S32(m, e + 0xB8u, c.sample->p);
        m.PokeWord(e + 0x158u, static_cast<uint32_t>(c.sample->lateral));
    }
    if (chance(3)) m.PokeWord(e + 0x16Cu, chance(2) ? 2u : 0xFFFFFFFEu);
    if (chance(4)) W16(m, e + 0x140u, chance(3) ? 0 : 1);
    uint32_t fa = m.PeekWord(e + 0x230u);
    fa = chance(2) ? (fa | 0x08000000u) : (fa & ~0x08000000u);
    fa = chance(6) ? (fa | 0x20000000u) : (fa & ~0x20000000u);
    m.PokeWord(e + 0x230u, fa);
    if (chance(3)) m.PokeWord(e + 0x238u, m.PeekWord(e + 0x238u) ^ (rng.U32() & (chance(2) ? 0x7FFu : 0x400600u)));
    if (chance(4)) m.PokeWord(e + 0x234u, rng.U32());
    if (chance(3)) m.PokeWord(e + 0x1E0u, rng.U32() % 0x400000u);
    if (chance(3)) m.PokeWord(e + 0x240u, rng.U32() % 0x20000u);
    if (chance(4)) m.PokeWord(e + 0x27Cu, rng.U32());
    if (chance(4)) m.PokeWord(e + 0x130u, rng.U32() % 0x20000u);
    if (chance(4)) m.PokeWord(e + 0x138u, (rng.U32() % 0x30000u) - (chance(8) ? 0x18000u : 0u));
    if (chance(4)) m.PokeWord(e + 0xBCu, m.PeekWord(e + 0xBCu) + (rng.U32() % 0x20000u) - 0x10000u);
    if (chance(4)) m.PokeWord(e + 0x174u, chance(2) ? 0 : rng.U32());
    if (chance(4)) W16(m, e + 0x16Au, rng.U32() % 3u);
    if (chance(4)) { W16(m, e + 0x1A4u, rng.U32() % 64u); W16(m, e + 0x198u, rng.U32() % 64u); m.PokeWord(e + 0x1A8u, rng.U32() % 0x4000u); }
    // partner / list node: the partner is a live bike or 0 (the captured word is not trusted), and a
    // bike with no list node always has one (the named bound above)
    m.PokeWord(e + 0x358u, (partner != 0 && chance(4)) ? partner : 0u);
    if (chance(6) && partner != 0) { m.PokeWord(e + 0x358u, partner); m.PokeWord(e + 0x440u, 0); }
    else if (m.PeekWord(e + 0x440u) == 0) m.PokeWord(e + 0x440u, e + 1088u);
    // the command stack: depth 0..6, the top op 16 at a target, or 6 / 14..17
    {
        const uint8_t d = static_cast<uint8_t>(rng.U32() % 7u);
        if (chance(2)) W8(m, e + 0x3B2u, d);
        const uint32_t top = e + 0x3B4u + 8u * B8(m, e + 0x3B2u);
        if (chance(2)) {
            const uint16_t ops[] = {16, 16, 6, 14, 15, 17, 4, 2};
            W16(m, top, ops[rng.Next() % 8u]);
            W16(m, top + 2u, chance(4) ? 224u : rng.U32() % w.n);
        }
        if (chance(4) && B8(m, e + 0x3B2u) > 1) { // a deeper op 16 / 6 so AiPushCommand's rotation runs
            const uint32_t deep = e + 0x3B4u + 8u * (1u + rng.U32() % (B8(m, e + 0x3B2u) - 1u));
            W16(m, deep, chance(2) ? 16u : 6u);
            W16(m, deep + 2u, rng.U32() % w.n);
        }
    }
    // --- the rider
    const uint32_t safeRecs[] = {0, 1, 10, 11, 16, 17};
    const uint32_t rec = gSafeAnim ? safeRecs[rng.Next() % 6u] : rng.U32() % 40u;
    W8(m, R + 0x239u, rec);
    const uint32_t nb = CountB(m, rec) == 0 ? 1u : CountB(m, rec);
    for (uint32_t i = 0; i < 6; ++i) W8(m, R + 0x23Eu + i, chance(3) ? 63u : rng.U32() % nb);
    W8(m, R + 0x23Au, rng.U32() % 6u);
    {
        // The stance is one of the bank the animation object holds (a named bound: the stance gate
        // switches banks only on a CHANGE of bank, and the object's current program names clips of its
        // own bank, so a stance of another bank - or the object moved to another bank - plays a garbage
        // clip in the original, 0x8005BF8C / 0x8005C3F4).
        uint32_t held = 16;
        for (uint32_t b = 0; b < 6; ++b)
            if (m.PeekWord(0x800CE190u + 4u * b) == m.PeekWord(an + 40u)) held = b;
        auto inBank = [&](uint16_t s) { return (m.PeekWord(kAltKindTable + 8u * s) & 15u) == held; };
        std::vector<uint16_t> c3, ot;
        for (uint16_t s : w.cat3) if (inBank(s)) c3.push_back(s);
        for (uint16_t s : w.other) if (inBank(s)) ot.push_back(s);
        const uint32_t k = rng.U32() % 8u;
        if (k < 4 && !c3.empty()) W16(m, R + 0x220u, c3[rng.Next() % c3.size()]);
        else if (k == 4 && inBank(static_cast<uint16_t>(30u))) W16(m, R + 0x220u, 30u + rng.U32() % 8u);
        else if (!ot.empty()) W16(m, R + 0x220u, ot[rng.Next() % ot.size()]);
    }
    W8(m, R + 0x222u, chance(2) ? 0 : 1);
    uint8_t b23c = B8(m, R + 0x23Cu);
    b23c = static_cast<uint8_t>((b23c & 0x0Cu) | (rng.U32() & (chance(3) ? 0xE3u : 0xC3u)));
    if (chance(5) && m.PeekWord(e + 0x358u) != 0) b23c |= 0x10u;
    if ((b23c & 0x30u) && m.PeekWord(e + 0x358u) == 0) b23c &= ~0x30u; // passenger bits need a partner
    // the passenger set (+0x23C 0x20) and the no-pad rider (0x40, whose fight exit plays stance 77) use
    // animation bank 6, which a one-rider race (rr-race) does not load: the descriptor's slot is 0
    if (gSafeAnim) b23c &= ~0x60u;
    W8(m, R + 0x23Cu, b23c);
    W8(m, R + 0x23Bu, chance(2) ? 0xFFu : rng.U32() % 8u);
    W8(m, R + 0x23Du, chance(2) ? 0u : rng.U32());
    m.PokeWord(R + 0x228u, m.PeekWord(R + 0x228u) ^ (rng.U32() & 0x08800000u));
    m.PokeWord(R + 0x25Cu, chance(4) ? rng.U32() % 5u : (chance(2) ? 1u : 0u));
    m.PokeWord(R + 0x230u, rng.U32() % 200u);
    m.PokeWord(R + 0x234u, rng.U32() % 200u);
    W8(m, R + 0x238u, rng.U32() % 7u);
    m.PokeWord(an + 0x10u, rng.U32() % 41u);
    // --- the runtime record
    W8(m, rd + 1u, (B8(m, rd + 1u) & 0xF0u) | (rng.U32() % 3u));
    m.PokeWord(rd + 8u, rng.U32() % 0x20000u);
    W8(m, rd + 12u, rng.U32());
    W8(m, rd + 14u, rng.U32() % 128u);
    W8(m, rd + 15u, chance(4) ? rng.U32() % 16u : rng.U32() % 128u);
    W8(m, rd + 36u, rng.U32());
    W8(m, rd + 37u, rng.U32());
    W8(m, rd + 39u, chance(8) ? 248u + rng.U32() % 8u : rng.U32() % 20u);
    m.PokeWord(rd + 40u, chance(8) ? rng.U32() | 1u : 0u);
    W16(m, rd + 44u, chance(3) ? 0u : rng.U32() & 0x1FFu);
    W8(m, rd + 46u, chance(2) ? 9u : rng.U32() % 10u);
    if (B8(m, rd + 46u) < 9u) W16(m, rd + 44u, H16(m, rd + 44u) | (1u << B8(m, rd + 46u))); // the weapon in hand is owned
    W8(m, rd + 47u, rng.U32() % 6u);
    m.PokeWord(rd + 48u, rng.U32());
    for (uint32_t i = 0; i < 8; ++i) {
        const uint8_t cmds[] = {32, 36, 38, 71, 75, 77, 142, 143, 146, 148, 33, 72};
        W8(m, rd + 52u + i, cmds[rng.Next() % 12u]);
    }
    {
        const uint8_t cmds[] = {32, 36, 38, 71, 72, 75, 76, 77, 142, 143, 144, 146, 147, 148, 64, 0, 200};
        W8(m, rd + 60u, cmds[rng.Next() % 17u]);
    }
    // an armed command (bit 7) comes with a weapon in hand: PickWeapon 0x800B9340 turns it into 32
    // (bare fists) when there is none, so a swing with +0x2E = 9 is a state the game never builds
    if ((B8(m, rd + 60u) & 0x80u) && B8(m, rd + 46u) >= 9u) {
        W8(m, rd + 46u, rng.U32() % 9u);
        W16(m, rd + 44u, H16(m, rd + 44u) | (1u << B8(m, rd + 46u)));
    }
    W8(m, rd + 61u, rng.U32());
    W16(m, rd + 64u, chance(2) ? 0u : rng.U32() & 0x3FFFFFFu);
    W16(m, rd + 66u, chance(2) ? 0u : rng.U32());
    W8(m, rd + 68u, rng.U32());
    if (gSafeAnim) {
        const uint8_t fists[] = {32, 33, 36, 38, 71, 72, 75, 77};
        W16(m, rd + 44u, 0);
        W8(m, rd + 46u, 9);
        for (uint32_t i = 0; i < 8; ++i) W8(m, rd + 52u + i, fists[rng.Next() % 8u]);
        W8(m, rd + 60u, fists[rng.Next() % 8u]);
    }
}

// The globals every function may read.
void StirGlobals(CaseContext& c, const World& w) {
    Memory& m = *c.mem;
    Rng& rng = *c.rng;
    auto chance = [&rng](uint32_t n) { return rng.Next() % n == 0; };
    m.PokeWord(w.gs + 0x10u, rng.U32() % 0x100000u);
    m.PokeWord(w.gs + 0x20u, rng.U32() % 12u);
    // one player, as rr-race: with two, code under these functions (Arrest 0x80096F30) chases player 2's
    // bike pointer 0x8005B26C, which a one-player capture holds as 0 (Pick's own row drives two players)
    W8(m, w.gs + 57u, chance(6) ? 3u : B8(m, w.gs + 57u));
    m.PokeWord(w.gs + 60u, rng.U32() % 6u);
    W8(m, w.gs + 4u, chance(4) ? (B8(m, w.gs + 4u) ^ 1u) : B8(m, w.gs + 4u));
    m.PokeWord(0x8005B220u, chance(6) ? 1u : 0u);
    for (uint32_t h = 0; h < 2; ++h) {
        W16(m, F::kLastSlot + 2u * h, chance(3) ? 224u : rng.U32() % w.n);
        m.PokeWord(F::kLastTime + 4u * h, m.PeekWord(w.gs + 0x10u) - rng.U32() % 120u);
        W16(m, F::kAttackers + 2u * h, chance(2) ? 0u : rng.U32() & 0x3FFFFu);
    }
    // the rider-object pool bitmap 0x8005AD50 keeps the machine's value here (a named bound: a random
    // bitmap names weapon objects that do not exist, whose clips the original then reads as garbage);
    // ReleaseRiderObject's own row drives it
    m.PokeWord(c.gp + kRandSeedGpOffset, rng.U32());
}

// The pad record's stamps (the frame clock +0x00 and the 19 slots at +0x14 stride 8).
void StirPad(CaseContext& c, const World& w) {
    Memory& m = *c.mem;
    Rng& rng = *c.rng;
    const uint32_t clk = rng.U32() % 0x10000u + 100u;
    m.PokeWord(kPadRecord, clk);
    for (uint32_t s = 0; s < 19; ++s) {
        const uint32_t k = rng.U32() % 6u;
        int32_t v = 0;
        if (k == 1) v = static_cast<int32_t>(clk) - static_cast<int32_t>(rng.U32() % 16u);
        else if (k == 2) v = static_cast<int32_t>(clk) - static_cast<int32_t>(rng.U32() % 200u);
        else if (k == 3) v = -static_cast<int32_t>(rng.U32() % 120u);
        else if (k == 4) v = static_cast<int32_t>(rng.U32());
        m.PokeWord(kPadRecord + 20u + 8u * s, static_cast<uint32_t>(v));
    }
    (void)w;
}

World Stir(CaseContext& c, bool playerMe = false, bool safeAnim = false) {
    gSafeAnim = safeAnim;
    World w = Pick3(c, playerMe);
    if (w.me == 0) return w;
    StirGlobals(c, w);
    StirBike(c, w, w.me, 0, w.x);
    StirBike(c, w, w.t, w.me, w.x);
    return w;
}

// ---------------------------------------------------------------- the callees

struct Bench final : F::Callees {
    NativeEnv& env;
    explicit Bench(NativeEnv& e) : env(e) {}
    Memory& M() { return env.clone->mem; }
    uint8_t* Raw(uint32_t a, uint32_t n) {
        const uint32_t phys = a & 0x1FFFFFFFu;
        if (phys + n > M().ram().size()) return nullptr;
        return M().ram().data() + phys;
    }
    bool Ok() const { return env.failure.empty(); }
    bool StanceEvent(uint32_t ev, uint32_t r, uint32_t p, uint32_t& v0) override {
        v0 = env.CallOracle(kAiStance, ev, r, p, 0);
        return Ok();
    }
    bool ClipDone(uint32_t a, uint32_t& v0) override { v0 = env.CallOracle(kClipDone, a, 0, 0, 0); return Ok(); }
    bool PlaySound3D(int32_t x, int32_t z, int32_t id, int32_t bank) override {
        env.CallOracle(kSound3D, static_cast<uint32_t>(x), static_cast<uint32_t>(z), static_cast<uint32_t>(id),
                       static_cast<uint32_t>(bank));
        return Ok();
    }
    bool QueueListenerSound(uint32_t e, int32_t id, int32_t t, int32_t p) override {
        env.CallOracle(kQueueSound, e, static_cast<uint32_t>(id), static_cast<uint32_t>(t), static_cast<uint32_t>(p));
        return Ok();
    }
    struct Sink final : rr::sim::AiStanceSink {
        NativeEnv& env;
        explicit Sink(NativeEnv& v) : env(v) {}
        void PlayIdleStance(uint16_t event, uint32_t riderArg) override { env.CallOracle(kAiStance, event, riderArg, 2, 0); }
    };
    bool PushCommand(uint8_t cmd[8], uint32_t, int32_t mode, uint32_t e) override { // PORTED (ai.h)
        uint8_t* ep = Raw(e, 1096);
        const uint32_t R = M().PeekWord(e + 852u);
        uint8_t* rider = Raw(R, 640);
        const uint8_t* alt = Raw(kAltKindTable, 8u * 256u);
        const uint8_t* fight = Raw(M().PeekWord(kFightRecPtr), 12u * 256u);
        if (ep == nullptr || rider == nullptr || alt == nullptr || fight == nullptr) {
            env.failure = "PushCommand: an unmapped record";
            return false;
        }
        Sink sink(env);
        rr::sim::AiPushEnv pe;
        pe.raceClock = static_cast<int32_t>(M().PeekWord(M().PeekWord(kGameStatePtr) + 0x10u));
        pe.rider = rider;
        pe.riderAddress = R;
        pe.altKindTable = alt;
        pe.fightRecords = fight;
        pe.stance = &sink;
        rr::sim::AiPushCommand(cmd, mode, ep, pe);
        return Ok();
    }
    bool PopCommand(uint32_t e) override { // PORTED (ai.h)
        uint8_t* ep = Raw(e, 1096);
        const uint32_t R = M().PeekWord(e + 852u);
        const uint32_t gs = M().PeekWord(kGameStatePtr);
        uint8_t* rd = Raw(M().PeekWord(e + 1084u), 72);
        const uint8_t* rider = Raw(R, 640);
        const uint8_t* fight = Raw(M().PeekWord(kFightRecPtr), 12u * 256u);
        if (ep == nullptr || rd == nullptr || rider == nullptr || fight == nullptr) {
            env.failure = "PopCommand: an unmapped record";
            return false;
        }
        Sink sink(env);
        rr::sim::AiPopEnv pe;
        pe.gameState = Raw(gs, 0x60);
        pe.riderDef = rd;
        pe.rider = rider;
        pe.riderAddress = R;
        pe.altKindTable = Raw(kAltKindTable, 8u * 256u);
        pe.fightRecords = fight;
        pe.attackerMask = reinterpret_cast<const uint16_t*>(Raw(F::kAttackers, 16));
        pe.stance = &sink;
        rr::sim::AiPopCommand(ep, pe);
        return Ok();
    }
    bool EndRace(uint32_t e, int32_t reason) override {
        env.CallOracle(kEndRace, e, static_cast<uint32_t>(reason), 0, 0);
        return Ok();
    }
    bool SetAimDelta(uint32_t e, uint32_t scalarAt, int32_t) override {
        const uint32_t a[5] = {e, 0, e + 0x368u, scalarAt, 1};
        env.CallOracleArgs(kSetAim, a, 5);
        return Ok();
    }
    bool Arrest(uint32_t a, uint32_t v, int32_t k, uint32_t a3) override {
        env.CallOracle(kArrest, a, v, static_cast<uint32_t>(k), a3);
        return Ok();
    }
    bool PadMotor(int32_t port, int32_t a, int32_t b, int32_t c) override {
        env.CallOracle(kPadMotor, static_cast<uint32_t>(port), static_cast<uint32_t>(a), static_cast<uint32_t>(b),
                       static_cast<uint32_t>(c));
        return Ok();
    }
    bool RiderSpeech(uint32_t h, int32_t a) override { env.CallOracle(kSpeech, h, static_cast<uint32_t>(a), 0, 0); return Ok(); }
    bool WeaponObject(uint32_t r, uint32_t side) override { env.CallOracle(kWeaponObj, r, side, 0, 0); return Ok(); }
    bool ObjectRelease(uint32_t r, uint32_t slot, uint32_t z) override { env.CallOracle(kObjRelease, r, slot, z, 0); return Ok(); }
    bool ObjectStop(uint32_t slot) override { env.CallOracle(kObjStop, slot, 0, 0, 0); return Ok(); }
    bool ObjectSound(uint32_t slot, int32_t a1, int32_t a2, int32_t a3, int32_t a4) override {
        const uint32_t a[5] = {slot, static_cast<uint32_t>(a1), static_cast<uint32_t>(a2), static_cast<uint32_t>(a3),
                               static_cast<uint32_t>(a4)};
        env.CallOracleArgs(kObjSound, a, 5);
        return Ok();
    }
    bool OverlayClip(uint32_t ev, uint32_t r, uint32_t a2, uint32_t a3, uint32_t& v0) override {
        env.clone->cpu.regs[2] = v0; // the v0 the caller leaves: 0x800C2F84 returns it untouched on one arm
        v0 = env.CallOracle(kOverlayClip, ev, r, a2, a3);
        return Ok();
    }
};

uint32_t Done(NativeEnv& env, rr::sim::GuestRam& g, bool ok, const char* what, uint32_t v0) {
    if (!ok && env.failure.empty() && !g.Faulted()) env.failure = std::string(what) + " refused";
    return RqDone(env, g, v0);
}

// Every row's native side: wrap the clone, run `body`, fail loudly on a refusal.
template <class Fn>
uint32_t Run(NativeEnv& env, const char* what, Fn body) {
    rr::sim::GuestRam g(env.clone->mem.ram().data(), env.gp);
    Bench b(env);
    uint32_t v0 = 0;
    const bool ok = body(g, b, v0);
    return Done(env, g, ok, what, v0);
}

// A seam callee reached through ported functions below the row's own: its extra depth (Row::oracleFrame's
// note), so that the stale stack words it reads lie where the original's call finds them.
void SeamDepth(std::vector<Row>& rows, uint32_t callee, uint32_t extra) {
    for (OracleCallee& oc : rows.back().oracleCallees)
        if (oc.address == callee) oc.frame = extra;
}

std::vector<OracleCallee> AllSeams(uint32_t frame) {
    return {OracleCallee{kAiStance, 3, 0, {}, frame}, OracleCallee{kClipDone, 1, 0, {}, frame},
            OracleCallee{kSound3D, 4, 0, {}, frame},  OracleCallee{kQueueSound, 4, 0, {}, frame},
            OracleCallee{kEndRace, 2, 0, {}, frame},  OracleCallee{kSetAim, 4, 1, {}, frame},
            OracleCallee{kArrest, 4, 0, {}, frame},   OracleCallee{kPadMotor, 4, 0, {}, frame},
            OracleCallee{kSpeech, 2, 0, {}, frame},   OracleCallee{kWeaponObj, 2, 0, {}, frame},
            OracleCallee{kObjRelease, 3, 0, {}, frame}, OracleCallee{kObjStop, 1, 0, {}, frame},
            OracleCallee{kObjSound, 4, 1, {}, frame}, OracleCallee{kOverlayClip, 4, 0, {}, frame}};
}

// ---------------------------------------------------------------- the setups
// Each builds the world, then its arguments. A world the machine cannot give (fewer than three
// real bikes) leaves every argument 0 and the case runs the function on bike 0 - both sides alike.

void SetupEdge(CaseContext& c, Args& a) {
    const World w = Stir(c);
    StirPad(c, w);
    a.a[0] = kPadRecord;
    a.a[1] = c.rng->U32() % 9u;
}

void SetupDecode(CaseContext& c, Args& a) {
    const World w = Stir(c, true, true);
    StirPad(c, w);
    a.a[0] = kPadRecord;
    a.a[1] = w.me;
}

void SetupPush(CaseContext& c, Args& a) {
    const World w = Stir(c, true, true);
    a.a[0] = w.me;
}

void SetupPick(CaseContext& c, Args& a) {
    const World w = Stir(c);
    Rng& rng = *c.rng;
    Memory& m = *c.mem;
    const uint32_t e = (rng.Next() % 3u == 0) ? w.x : w.me;
    if (rng.Next() % 3u == 0) { // far away: the growing squares run off the grid
        m.PokeWord(e + 0xB8u, rng.U32());
        m.PokeWord(e + 0xC0u, rng.U32());
    }
    if (rng.Next() % 3u == 0) {
        m.PokeWord(w.gs + 0x30u, 2);
        m.PokeWord(0x800CCF9Cu, m.PeekWord(0x800CCF98u) + (rng.U32() % 0x4000000u) - 0x2000000u);
        m.PokeWord(0x800CCFA4u, m.PeekWord(0x800CCFA0u) + (rng.U32() % 0x4000000u) - 0x2000000u);
    }
    a.a[0] = e + 0xACu;
    a.a[1] = (rng.Next() % 4u == 0) ? (rng.U32() & 3u) : 1u;
}

void SetupCombo(CaseContext& c, Args& a) {
    const World w = Stir(c, true);
    StirPad(c, w);
    a.a[0] = kPadRecord;
    a.a[1] = w.me;
}

void SetupMe(CaseContext& c, Args& a) { // FightStep, FightPickRecord, FightRestart's me
    const World w = Stir(c, false, true);
    // FightRestart's "animation-object pool full" test (0x800C12A0): the pool's use count at its capacity.
    // Only here - elsewhere a count that disagrees with the pool makes the weapon object's allocation
    // (0x800958F0 -> 0x8008CC94) evict an object that does not exist, in the original.
    if (c.rng->Next() % 4u == 0) c.mem->PokeWord(F::kAnimPool + 8u, c.mem->PeekWord(F::kAnimPool + 12u));
    a.a[0] = w.me;
    a.a[1] = w.t;
    a.a[2] = (c.rng->Next() & 1u) ? 0x100u : 0u;
}

void SetupStart(CaseContext& c, Args& a) {
    const World w = Stir(c, false, true);
    a.a[0] = w.me;
    const uint32_t k = c.rng->U32() % 4u;
    a.a[1] = k == 0 ? 0u : (k == 1 ? c.mem->PeekWord(w.me + 0x358u) : w.t);
}

void SetupBegin(CaseContext& c, Args& a) {
    const World w = Stir(c, false, true);
    a.a[0] = w.me;
    a.a[1] = c.rng->U32() & 0x10300u;
}

void SetupRider(CaseContext& c, Args& a) { // FightEnd(r), CloseQueue(r), ReleaseRiderObject(r)
    const World w = Stir(c, false, true);
    a.a[0] = c.mem->PeekWord(w.me + 852u);
    if (c.rng->Next() % 3u == 0) c.mem->PokeWord(F::kObjPoolBits, c.rng->U32());
}

void SetupPair(CaseContext& c, Args& a) { // (me, t, along)
    const World w = Stir(c, false, true);
    a.a[0] = w.me;
    a.a[1] = w.t;
    const int32_t alongs[] = {0, 0x8000, -0x8000, 0x80000, -0x80000, 0x200000, 0x2000000};
    a.a[2] = static_cast<uint32_t>((c.rng->Next() & 1u) ? alongs[c.rng->Next() % 7u]
                                                          : static_cast<int32_t>(c.rng->U32() % 0x1000000u) - 0x800000);
}

void SetupContinue(CaseContext& c, Args& a) {
    SetupPair(c, a);
    a.a[3] = c.rng->U32() & 3u;
    // bit 1 (the victim's partner is the one in reach) comes only with a partner (FightUpdate's s5)
    if (c.mem->PeekWord(a.a[1] + 0x358u) == 0) a.a[3] &= ~2u;
}

void SetupOtherSide(CaseContext& c, Args& a) {
    const World w = Stir(c);
    a.a[0] = w.t;
    a.a[1] = (c.rng->Next() & 1u) ? H16(*c.mem, w.me + 0xACu) : c.rng->U32() % 20u;
    a.a[2] = c.rng->U32();
}

void SetupSideAim(CaseContext& c, Args& a) {
    const World w = Stir(c);
    a.a[0] = w.me;
    a.a[1] = w.t;
    a.a[2] = static_cast<uint32_t>(static_cast<int32_t>(c.rng->U32() % 0x100000u) - 0x80000);
}

void SetupRd(CaseContext& c, Args& a) { // NextMove(rd), PickWeapon(rd)
    const World w = Stir(c);
    a.a[0] = c.mem->PeekWord(w.me + 1084u);
}

void SetupReach(CaseContext& c, Args& a) {
    const World w = Stir(c);
    Rng& rng = *c.rng;
    a.a[0] = w.me;
    a.a[1] = w.t;
    a.a[2] = static_cast<uint32_t>(static_cast<int32_t>(rng.U32() % 0x60000u) - 0x30000);
    a.a[3] = static_cast<uint32_t>(static_cast<int32_t>(rng.U32() % 0x30000u) - 0x18000);
    const uint32_t flags = rng.U32() & (rng.Next() % 2u ? 0x1C3u : 0xFFFFFFFFu);
    c.mem->PokeWord(c.scratch, flags);
    a.a4 = c.scratch;
}

void SetupHitStance(CaseContext& c, Args& a) {
    const World w = Stir(c);
    a.a[0] = c.mem->PeekWord(w.me + 852u);
    a.a[1] = c.mem->PeekWord(w.t + 852u);
    a.a[2] = c.rng->U32() & 1u;
    c.mem->PokeWord(c.scratch, c.rng->U32());
    a.a[3] = c.scratch;
}

void SetupShove(CaseContext& c, Args& a) {
    const World w = Stir(c);
    a.a[0] = w.t;
    a.a[1] = c.rng->U32() % 8u;
    a.a[2] = c.rng->U32() % 40u;
    a.a[3] = c.rng->U32() & 1u;
}

void SetupRumble(CaseContext& c, Args& a) {
    const World w = Stir(c);
    a.a[0] = (c.rng->Next() & 1u) ? w.me : w.t;
    a.a[1] = static_cast<uint32_t>(static_cast<int32_t>(c.rng->U32() % 400u) - 40);
}

void SetupKnock(CaseContext& c, Args& a) {
    const World w = Stir(c, false, true);
    a.a[0] = w.t;
    const uint8_t cmds[] = {32, 36, 37, 38, 71, 75, 76, 77, 142, 146, 147, 148};
    a.a[1] = cmds[c.rng->Next() % 12u];
    a.a[2] = c.rng->U32() & 1u;
    a.a[3] = w.me;
}

void SetupStat(CaseContext& c, Args& a) {
    const World w = Stir(c);
    a.a[0] = (c.rng->Next() & 1u) ? w.me : w.t;
    a.a[1] = c.rng->U32() % 4u;
    a.a[2] = c.rng->U32() & 1u;
}

void SetupRemember(CaseContext& c, Args& a) {
    Rng& rng = *c.rng;
    a.a[0] = rng.U32() % 40u;
    uint32_t set = 0;
    const uint32_t n = rng.U32() % 8u;
    for (uint32_t i = 0; i < n; ++i) set |= ((rng.U32() % 31u) + 1u) << (5u * i);
    if (rng.Next() % 4u == 0) set = rng.U32();
    c.mem->PokeWord(c.scratch, set);
    a.a[1] = c.scratch;
}

void SetupStealWindow(CaseContext& c, Args& a) {
    const World w = Stir(c);
    Memory& m = *c.mem;
    const uint32_t R = m.PeekWord(w.me + 852u);
    if (c.rng->Next() & 1u) { W8(m, R + 0x239u, 0); W8(m, R + 0x23Eu + B8(m, R + 0x23Au), 1); }
    a.a[0] = R;
    a.a[1] = c.rng->U32() % 10u;
}

void SetupSteal(CaseContext& c, Args& a) {
    const World w = Stir(c);
    Memory& m = *c.mem;
    Rng& rng = *c.rng;
    const uint32_t R = m.PeekWord(w.me + 852u), TR = m.PeekWord(w.t + 852u);
    if (rng.Next() % 3u) { W8(m, R + 0x239u, 0); W8(m, R + 0x23Eu + B8(m, R + 0x23Au), 1); }
    if (rng.Next() % 3u && !w.cat3.empty()) W16(m, TR + 0x220u, w.cat3[rng.Next() % w.cat3.size()]);
    if (rng.Next() % 3u) W8(m, m.PeekWord(w.t + 1084u) + 60u, 0x80u | (rng.U32() & 0x7Fu));
    if (rng.Next() % 2u) W16(m, w.me + 0xACu, 0);
    a.a[0] = w.me;
    a.a[1] = w.t;
    a.a[2] = rng.U32() & 0x10300u;
    m.PokeWord(c.scratch, rng.U32() & 0x1C3u);
    a.a[3] = c.scratch;
}

// The attacker's current node is a STRIKING one (damage != 0): ApplyHit is reached only through
// ReachTest's true answer, which needs one (FightUpdate clears the swing bit before that call). A node
// with no damage has animHit / animMiss 224 ("none"), which the stance event does not take.
void StrikingNode(CaseContext& c, uint32_t e) {
    Memory& m = *c.mem;
    const uint32_t R = m.PeekWord(e + 852u);
    const uint32_t rec = B8(m, R + 0x239u);
    const uint32_t blk = m.PeekWord(Fight(m) + 12u * rec + 8u);
    std::vector<uint32_t> hits;
    for (uint32_t n = 0; n < CountB(m, rec); ++n)
        if (H16(m, blk + 12u * n + 8u) != 0) hits.push_back(n);
    if (!hits.empty()) W8(m, R + 0x23Eu + B8(m, R + 0x23Au), hits[c.rng->Next() % hits.size()]);
}

void SetupApply(CaseContext& c, Args& a) {
    const World w = Stir(c, false, true);
    StrikingNode(c, w.me);
    a.a[0] = w.me;
    a.a[1] = w.t;
    a.a[2] = c.rng->U32() & 0x1030Fu;
    a.a[3] = c.rng->U32() & 0x1C3u;
}

void SetupUpdate(CaseContext& c, Args& a) {
    const World w = Stir(c, false, true);
    // Jailbreak phase 3 (game_state +0x39 = 3) is the two-up escape: its target has a partner (a named
    // bound - the bust test reads the partner's rider with no null test)
    if (B8(*c.mem, w.gs + 57u) == 3 && c.mem->PeekWord(w.t + 0x358u) == 0) c.mem->PokeWord(w.t + 0x358u, w.x);
    a.a[0] = w.me;
    // 224 (no target) only on the human's half, which returns before the geometry: an AI half with no
    // target reads through a null bike (a named bound; the AI always fights someone)
    const bool gated = (c.mem->PeekWord(w.me + 0x230u) & 0x08000000u) ||
                       (B8(*c.mem, c.mem->PeekWord(w.me + 852u) + 0x23Cu) & 0x40u);
    a.a[1] = (!gated && c.rng->Next() % 5u == 0) ? 224u : w.tSlot;
    a.a[2] = c.rng->U32() % 2000u;
}

void SetupLeave(CaseContext& c, Args& a) {
    const World w = Stir(c, false, true);
    Rng& rng = *c.rng;
    a.a[0] = c.mem->PeekWord(w.me + 852u);
    a.a[1] = H16(*c.mem, a.a[0] + 0x220u);
    const bool cat3 = (rng.Next() & 1u) && !w.cat3.empty();
    a.a[2] = cat3 ? w.cat3[rng.Next() % w.cat3.size()] : w.other[rng.Next() % w.other.size()];
    a.a[3] = rng.U32() & 0x10310u;
}

// CombatEnter reaches no stance event: armed riders and every record (no safe-animation bound).
void SetupEnter(CaseContext& c, Args& a) {
    const World w = Stir(c);
    Rng& rng = *c.rng;
    Memory& m = *c.mem;
    a.a[0] = m.PeekWord(w.me + 852u);
    a.a[1] = (rng.Next() & 1u) ? H16(m, a.a[0] + 0x220u) : rng.U32() % 223u;
    // The new stance is one of the rider's own FIGHT.BIN record's (a named bound): an armed stance
    // (145..185) is a clip of the weapon object's bank, and a stance of another weapon's record indexes
    // past that bank in the original (0x800C2F84 -> 0x8005BF6C reads a garbage clip). The record is the
    // one FightPickRecord gives the weapon in hand (rules.md 8.3 table), or any stance below 145.
    static const uint8_t kRec[10][3] = {{6, 7, 14}, {4, 5, 13}, {2, 3, 12}, {2, 3, 12}, {8, 9, 15},
                                        {4, 5, 13}, {18, 18, 18}, {18, 18, 18}, {19, 19, 19}, {0, 1, 16}};
    const uint32_t wpn = std::min<uint32_t>(B8(m, m.PeekWord(w.me + 1084u) + 46u), 9u);
    const uint32_t rec = kRec[wpn][rng.Next() % 3u];
    const uint32_t blk = m.PeekWord(Fight(m) + 12u * rec + 8u), nb = CountB(m, rec);
    if (nb != 0 && (rng.Next() & 3u) != 0) {
        const uint16_t ev = H16(m, blk + 12u * (rng.U32() % nb) + 2u * (rng.U32() % 4u));
        a.a[2] = ev < 223u ? ev : rng.U32() % 145u;
    } else {
        a.a[2] = rng.U32() % 145u;
    }
    a.a[3] = rng.U32() & 0x10310u;
}


// ---------------------------------------------------------------- the native sides

uint32_t NEdge(Memory& m, uint32_t, uint32_t gp, const Args& a) {
    rr::sim::GuestRam g(m.ram().data(), gp);
    return static_cast<uint32_t>(F::Edge(g, a.a[0], static_cast<int32_t>(a.a[1])));
}
uint32_t NEdgeRel(Memory& m, uint32_t, uint32_t gp, const Args& a) {
    rr::sim::GuestRam g(m.ram().data(), gp);
    return static_cast<uint32_t>(F::EdgeReleased(g, a.a[0], static_cast<int32_t>(a.a[1])));
}
uint32_t NHeld(Memory& m, uint32_t, uint32_t gp, const Args& a) {
    rr::sim::GuestRam g(m.ram().data(), gp);
    return static_cast<uint32_t>(F::Held(g, a.a[0], static_cast<int32_t>(a.a[1])));
}
uint32_t NReleased(Memory& m, uint32_t, uint32_t gp, const Args& a) {
    rr::sim::GuestRam g(m.ram().data(), gp);
    return F::Released(g, a.a[0], static_cast<int32_t>(a.a[1]));
}

} // namespace fg

void AddFightRows(std::vector<Row>& rows) {
    using namespace fg;
    auto add = [&rows](const char* name, const char* sig, uint32_t entry, std::function<void(CaseContext&, Args&)> setup,
                       std::function<uint32_t(NativeEnv&, const Args&)> nat, bool v0, uint32_t frame) {
        rows.push_back(Row{name, sig, entry, std::move(setup), nullptr});
        Row& r = rows.back();
        r.compareV0 = v0;
        r.nativeEx = std::move(nat);
        r.oracleFrame = frame;
        r.oracleCallees = AllSeams(0);
    };
    rows.push_back(Row{"fight_edge", "s32 Edge(pad, n)  [RASHCDG 0x800C2178]", 0x800C2178, SetupEdge, NEdge});
    rows.push_back(Row{"fight_edge_released", "s32 EdgeReleased(pad, n)  [RASHCDG 0x800C2070]", 0x800C2070, SetupEdge, NEdgeRel});
    rows.push_back(Row{"fight_held", "s32 Held(pad, n)  [RASHCDG 0x800C2100]", 0x800C2100, SetupEdge, NHeld});
    rows.push_back(Row{"fight_released", "u32 Released(pad, n)  [RASHCDG 0x800C213C]", 0x800C213C, SetupEdge, NReleased});
    add("combat_decode", "s32 CombatDecode(pad, bike)  [RASHCDG 0x800C2348]", 0x800C2348, SetupDecode,
        [](NativeEnv& env, const Args& a) {
            return Run(env, "CombatDecode", [&](rr::sim::GuestRam& g, Bench& b, uint32_t& v0) {
                int32_t r = 0;
                const bool ok = F::CombatDecode(g, b, a.a[0], a.a[1], env.sp + 40u, r);
                v0 = static_cast<uint32_t>(r);
                return ok;
            });
        }, true, 40);
    add("fight_push", "void FightPush(bike)  [RASHCDG 0x800C1DD4]", 0x800C1DD4, SetupPush,
        [](NativeEnv& env, const Args& a) {
            return Run(env, "FightPush", [&](rr::sim::GuestRam& g, Bench& b, uint32_t&) {
                return F::FightPush(g, b, a.a[0], env.sp + 32u);
            });
        }, false, 32);
    rows.push_back(Row{"fight_pick", "u16 Pick(u16 *hf, u32 mask)  [RASHCDG 0x8008B428]", 0x8008B428, SetupPick,
                       [](Memory& m, uint32_t, uint32_t gp, const Args& a) {
                           rr::sim::GuestRam g(m.ram().data(), gp);
                           const uint32_t v = F::Pick(g, a.a[0], a.a[1]);
                           return v;
                       }});
    add("fight_combo_input", "s32 ComboInput(pad, bike)  [RASHCDG 0x800C258C]", 0x800C258C, SetupCombo,
        [](NativeEnv& env, const Args& a) {
            return Run(env, "ComboInput", [&](rr::sim::GuestRam& g, Bench&, uint32_t& v0) {
                v0 = 0;
                return F::ComboInput(g, a.a[0], a.a[1]);
            });
        }, true, 64);
    add("fight_step", "s32 FightStep(me)  [RASHCDG 0x800C09B0]", 0x800C09B0, SetupMe,
        [](NativeEnv& env, const Args& a) {
            return Run(env, "FightStep", [&](rr::sim::GuestRam& g, Bench& b, uint32_t& v0) {
                return F::FightStep(g, b, a.a[0], v0);
            });
        }, true, 40);
    rows.push_back(Row{"fight_pick_record", "u8 FightPickRecord(me)  [RASHCDG 0x800BF978]", 0x800BF978, SetupMe,
                       [](Memory& m, uint32_t, uint32_t gp, const Args& a) {
                           rr::sim::GuestRam g(m.ram().data(), gp);
                           return F::FightPickRecord(g, a.a[0]);
                       }});
    add("fight_begin", "void FightBegin(me, side)  [RASHCDG 0x800C12D8]", 0x800C12D8, SetupBegin,
        [](NativeEnv& env, const Args& a) {
            return Run(env, "FightBegin", [&](rr::sim::GuestRam& g, Bench& b, uint32_t&) {
                return F::FightBegin(g, b, a.a[0], a.a[1]);
            });
        }, false, 32);
    add("fight_start", "void FightStart(me, t)  [RASHCDG 0x800C110C, one Rand]", 0x800C110C, SetupStart,
        [](NativeEnv& env, const Args& a) {
            return Run(env, "FightStart", [&](rr::sim::GuestRam& g, Bench& b, uint32_t&) {
                return F::FightStart(g, b, a.a[0], a.a[1]);
            });
        }, false, 64);
    add("fight_end", "u8 FightEnd(rider)  [RASHCDG 0x800C1014]", 0x800C1014, SetupRider,
        [](NativeEnv& env, const Args& a) {
            return Run(env, "FightEnd", [&](rr::sim::GuestRam& g, Bench& b, uint32_t& v0) {
                return F::FightEnd(g, b, a.a[0], v0);
            });
        }, true, 24);
    add("fight_restart", "void FightRestart(me, t, side)  [RASHCDG 0x800C122C]", 0x800C122C, SetupMe,
        [](NativeEnv& env, const Args& a) {
            return Run(env, "FightRestart", [&](rr::sim::GuestRam& g, Bench& b, uint32_t&) {
                return F::FightRestart(g, b, a.a[0], a.a[1], a.a[2]);
            });
        }, false, 64);
    rows.push_back(Row{"fight_can_engage", "s32 CanEngage(me, t, along)  [RASHCDG 0x800BC1EC]", 0x800BC1EC, SetupPair,
                       [](Memory& m, uint32_t, uint32_t gp, const Args& a) {
                           rr::sim::GuestRam g(m.ram().data(), gp);
                           return static_cast<uint32_t>(F::CanEngage(g, a.a[0], a.a[1], static_cast<int32_t>(a.a[2])));
                       }});
    rows.push_back(Row{"fight_back_off", "s32 BackOff(me, t, x)  [RASHCDG 0x800BB8FC]", 0x800BB8FC, SetupPair,
                       [](Memory& m, uint32_t, uint32_t gp, const Args& a) {
                           rr::sim::GuestRam g(m.ram().data(), gp);
                           return static_cast<uint32_t>(F::BackOff(g, a.a[0], a.a[1], static_cast<int32_t>(a.a[2])));
                       }});
    add("fight_continue", "s32 FightContinue(me, t, along, s)  [RASHCDG 0x800C0BE8]", 0x800C0BE8, SetupContinue,
        [](NativeEnv& env, const Args& a) {
            return Run(env, "FightContinue", [&](rr::sim::GuestRam& g, Bench& b, uint32_t& v0) {
                int32_t r = 0;
                const bool ok = F::FightContinue(g, b, a.a[0], a.a[1], static_cast<int32_t>(a.a[2]), a.a[3], env.sp + 40u, r);
                v0 = static_cast<uint32_t>(r);
                return ok;
            });
        }, true, 40);
    rows.push_back(Row{"fight_other_on_side", "s32 OtherOnSide(t, h, bias)  [RASHCDG 0x800BC618, GTE LZCS]", 0x800BC618,
                       SetupOtherSide, [](Memory& m, uint32_t, uint32_t gp, const Args& a) {
                           rr::sim::GuestRam g(m.ram().data(), gp);
                           return static_cast<uint32_t>(F::OtherOnSide(g, a.a[0], a.a[1], static_cast<int32_t>(a.a[2])));
                       }});
    add("fight_side_aim", "void SideAim(me, t, b)  [RASHCDG 0x800BC4FC]", 0x800BC4FC, SetupSideAim,
        [](NativeEnv& env, const Args& a) {
            return Run(env, "SideAim", [&](rr::sim::GuestRam& g, Bench& b, uint32_t&) {
                return F::SideAim(g, b, a.a[0], a.a[1], static_cast<int32_t>(a.a[2]), env.sp + 32u);
            });
        }, false, 32);
    rows.push_back(Row{"fight_pace", "void FightPace(me, t, along, dv)  [RASHCDG 0x800C0E98]", 0x800C0E98, SetupPair,
                       [](Memory& m, uint32_t, uint32_t gp, const Args& a) {
                           rr::sim::GuestRam g(m.ram().data(), gp);
                           F::FightPace(g, a.a[0], a.a[1], static_cast<int32_t>(a.a[2]));
                           return 0u;
                       }, false});
    add("fight_steer", "void FightSteer(me, t, along)  [RASHCDG 0x800C0CE8]", 0x800C0CE8, SetupPair,
        [](NativeEnv& env, const Args& a) {
            return Run(env, "FightSteer", [&](rr::sim::GuestRam& g, Bench& b, uint32_t&) {
                return F::FightSteer(g, b, a.a[0], a.a[1], static_cast<int32_t>(a.a[2]), env.sp + 48u);
            });
        }, false, 48);
    rows.push_back(Row{"fight_next_move", "void NextMove(rd)  [RASHCDG 0x800B92C0, one Rand]", 0x800B92C0, SetupRd,
                       [](Memory& m, uint32_t, uint32_t gp, const Args& a) {
                           rr::sim::GuestRam g(m.ram().data(), gp);
                           F::NextMove(g, a.a[0]);
                           return 0u;
                       }, false});
    rows.push_back(Row{"fight_pick_weapon", "void PickWeapon(rd)  [RASHCDG 0x800B9340]", 0x800B9340, SetupRd,
                       [](Memory& m, uint32_t, uint32_t gp, const Args& a) {
                           rr::sim::GuestRam g(m.ram().data(), gp);
                           F::PickWeapon(g, a.a[0]);
                           return 0u;
                       }, false});
    rows.push_back(Row{"fight_reach_test", "s32 ReachTest(me, t, lat, along, [u32 *flags])  [RASHCDG 0x800C159C]", 0x800C159C,
                       SetupReach, [](Memory& m, uint32_t, uint32_t gp, const Args& a) {
                           rr::sim::GuestRam g(m.ram().data(), gp);
                           uint32_t flags = g.U32(a.a4);
                           const int32_t v = F::ReachTest(g, a.a[0], a.a[1], static_cast<int32_t>(a.a[2]),
                                                          static_cast<int32_t>(a.a[3]), flags);
                           g.W32(a.a4, flags);
                           return static_cast<uint32_t>(v);
                       }});
    rows.push_back(Row{"fight_hit_stance", "u16 HitStance(r, v, hit, u32 *damage)  [RASHCDG 0x800BF860]", 0x800BF860,
                       SetupHitStance, [](Memory& m, uint32_t, uint32_t gp, const Args& a) {
                           rr::sim::GuestRam g(m.ram().data(), gp);
                           uint32_t d = g.U32(a.a[3]);
                           const uint32_t v = F::HitStance(g, a.a[0], a.a[1], static_cast<int32_t>(a.a[2]), d);
                           g.W32(a.a[3], d);
                           return v;
                       }});
    rows.push_back(Row{"fight_note_hit", "void NoteHit(a, v)  [RASHCDG 0x800BF424]", 0x800BF424, SetupPair,
                       [](Memory& m, uint32_t, uint32_t gp, const Args& a) {
                           rr::sim::GuestRam g(m.ram().data(), gp);
                           F::NoteHit(g, a.a[0], a.a[1]);
                           return 0u;
                       }, false});
    rows.push_back(Row{"fight_hit_shove", "void HitShove(v, node, rec, side)  [RASHCDG 0x800BF604]", 0x800BF604, SetupShove,
                       [](Memory& m, uint32_t, uint32_t gp, const Args& a) {
                           rr::sim::GuestRam g(m.ram().data(), gp);
                           F::HitShove(g, a.a[0], a.a[1], static_cast<int32_t>(a.a[2]), static_cast<int32_t>(a.a[3]));
                           return 0u;
                       }, false});
    add("fight_hit_rumble", "void HitRumble(e, fin)  [RASHCDG 0x800BFD74]", 0x800BFD74, SetupRumble,
        [](NativeEnv& env, const Args& a) {
            return Run(env, "HitRumble", [&](rr::sim::GuestRam& g, Bench& b, uint32_t&) {
                return F::HitRumble(g, b, a.a[0], static_cast<int32_t>(a.a[1]));
            });
        }, false, 24);
    add("fight_knock_off", "void KnockOff(v, u8 cmd, side, a)  [RASHCDG 0x800BF674]", 0x800BF674, SetupKnock,
        [](NativeEnv& env, const Args& a) {
            return Run(env, "KnockOff", [&](rr::sim::GuestRam& g, Bench& b, uint32_t&) {
                return F::KnockOff(g, b, a.a[0], static_cast<uint8_t>(a.a[1]), static_cast<int32_t>(a.a[2]), a.a[3]);
            });
        }, false, 32);
    rows.push_back(Row{"fight_stat", "void FightStat(e, k, which)  [RASHCDG 0x800BFE58]", 0x800BFE58, SetupStat,
                       [](Memory& m, uint32_t, uint32_t gp, const Args& a) {
                           rr::sim::GuestRam g(m.ram().data(), gp);
                           F::FightStat(g, a.a[0], static_cast<int32_t>(a.a[1]), static_cast<int32_t>(a.a[2]));
                           return 0u;
                       }, false});
    rows.push_back(Row{"fight_remember_handle", "s32 RememberHandle(h, u32 *set)  [RASHCDG 0x800A8BE0]", 0x800A8BE0,
                       SetupRemember, [](Memory& m, uint32_t, uint32_t gp, const Args& a) {
                           rr::sim::GuestRam g(m.ram().data(), gp);
                           uint32_t set = g.U32(a.a[1]);
                           const int32_t v = F::RememberHandle(a.a[0], set);
                           g.W32(a.a[1], set);
                           return static_cast<uint32_t>(v);
                       }});
    rows.push_back(Row{"fight_steal_window", "s32 StealWindow(r, u8 weapon)  [RASHCDG 0x800C2E9C]", 0x800C2E9C,
                       SetupStealWindow, [](Memory& m, uint32_t, uint32_t gp, const Args& a) {
                           rr::sim::GuestRam g(m.ram().data(), gp);
                           return static_cast<uint32_t>(F::StealWindow(g, a.a[0], static_cast<uint8_t>(a.a[1])));
                       }});
    rows.push_back(Row{"fight_close_queue", "void CloseQueue(r)  [RASHCDG 0x800C2030]", 0x800C2030, SetupRider,
                       [](Memory& m, uint32_t, uint32_t gp, const Args& a) {
                           rr::sim::GuestRam g(m.ram().data(), gp);
                           F::CloseQueue(g, a.a[0]);
                           return 0u;
                       }, false});
    add("fight_weapon_steal", "void WeaponSteal(me, t, side, u32 *flags)  [RASHCDG 0x800BFF04]", 0x800BFF04, SetupSteal,
        [](NativeEnv& env, const Args& a) {
            return Run(env, "WeaponSteal", [&](rr::sim::GuestRam& g, Bench& b, uint32_t&) {
                uint32_t flags = g.U32(a.a[3]);
                const bool ok = F::WeaponSteal(g, b, a.a[0], a.a[1], a.a[2], flags);
                g.W32(a.a[3], flags);
                return ok;
            });
        }, false, 56);
    add("fight_apply_hit", "s32 ApplyHit(me, t, side, flags)  [RASHCDG 0x800C17B0]", 0x800C17B0, SetupApply,
        [](NativeEnv& env, const Args& a) {
            return Run(env, "ApplyHit", [&](rr::sim::GuestRam& g, Bench& b, uint32_t& v0) {
                return F::ApplyHit(g, b, a.a[0], a.a[1], a.a[2], a.a[3], v0);
            });
        }, true, 72);
    SeamDepth(rows, kArrest, 32); // KnockOff 0x800BF674's frame
    add("fight_update", "void FightUpdate(me, slot, dt)  [RASHCDG 0x800C035C, the fight machine]", 0x800C035C, SetupUpdate,
        [](NativeEnv& env, const Args& a) {
            return Run(env, "FightUpdate", [&](rr::sim::GuestRam& g, Bench& b, uint32_t&) {
                return F::FightUpdate(g, b, a.a[0], static_cast<uint16_t>(a.a[1]), env.sp + 72u);
            });
        }, false, 72);
    SeamDepth(rows, kArrest, 72 + 32); // ApplyHit's and KnockOff's frames
    add("fight_release_object", "void ReleaseRiderObject(r)  [RASHCDG 0x80095AEC]", 0x80095AEC, SetupRider,
        [](NativeEnv& env, const Args& a) {
            return Run(env, "ReleaseRiderObject", [&](rr::sim::GuestRam& g, Bench& b, uint32_t&) {
                return F::ReleaseRiderObject(g, b, a.a[0]);
            });
        }, false, 32);
    add("fight_combat_leave", "s32 CombatLeave(r, cur, ev, p)  [RASHCDG 0x800BFD24]", 0x800BFD24, SetupLeave,
        [](NativeEnv& env, const Args& a) {
            return Run(env, "CombatLeave", [&](rr::sim::GuestRam& g, Bench& b, uint32_t& v0) {
                return F::CombatLeave(g, b, a.a[0], a.a[1], a.a[2], a.a[3], v0);
            });
        }, true, 24);
    add("fight_combat_enter", "s32 CombatEnter(r, cur, ev, p)  [RASHCDG 0x800BFC5C]", 0x800BFC5C, SetupEnter,
        [](NativeEnv& env, const Args& a) {
            return Run(env, "CombatEnter", [&](rr::sim::GuestRam& g, Bench& b, uint32_t& v0) {
                return F::CombatEnter(g, b, a.a[0], a.a[1], a.a[2], a.a[3], v0);
            });
        }, true, 40);
}
