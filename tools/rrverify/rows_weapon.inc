// Bench rows for weapons in the hand. Included by verify_physics.cpp
// inside its anonymous namespace, after rows_fight.inc / rows_anim.inc / rows_spine.inc, whose world
// builders it reuses (fg::Stir, animrows::RandomProgram, spinerows::PlantEffects).
//
// The native code is src\game\sim\weapon.{h,cpp}, a GUEST-ADDRESS port run straight on the clone's RAM.
// Its ported callees (ViewSlot, Attach, LodSelect, BankSwitch, the effect helpers, Restart, HardStart)
// run natively; FreeFarRider 0x8008CC94 (ported with the recovery layer, its own row there) and the
// animation machine's pose side (TransitionCapture 0x8005CB70) are executed by the ORACLE and witnessed.
//
// Named bounds (each a state the game builds or keeps):
//   * the eight weapon model objects 0x800CF018 are bound to model 800 (the race loader's
//     0x80066604 loop, ModelBind(obj, 5, 0, 1) - every capture): LodSelect reads their registry
//     record with no null test;
//   * the overlay clip's animation object is on the weapons bank *(0x800CE19C) (the only bank
//     WeaponObject puts it on) and an armed stance names a clip of that bank (145 + k, k < clips) -
//     a stance past the bank's clips makes the ORIGINAL read a garbage clip pointer (rows_fight.inc's
//     CombatEnter bound, from the other side);
//   * the weapon's animation object carries no blend op: its programs are written by HardStart
//     (0x8005BF6C, op 1 + op 5) and Transition 0x8005C140 - the only writer of a blend - has one caller,
//     0x800C42A8 in the stance gate, on the rider's own object +0x21C. (A drawn blend whose new-clip
//     halfword is out of the bank makes the ORIGINAL's TransitionCapture read a garbage clip, 0x8005E2C8.)
//   * the effect records are rows_spine.inc's: terminated chains, busy / free patterns.
namespace wpn {

namespace W = rr::sim::weapon;

constexpr uint32_t kFreeFarRider = 0x8008CC94;

void W8(Memory& m, uint32_t a, uint32_t v) { const uint8_t b = static_cast<uint8_t>(v); m.WriteBlock(a, &b, 1); }

// ---------------------------------------------------------------- 0x800958F0
void SetupObject(CaseContext& c, Args& a) {
    const fg::World w = fg::Stir(c);
    Memory& m = *c.mem;
    Rng& rng = *c.rng;
    auto chance = [&rng](uint32_t n) { return rng.Next() % n == 0; };
    const uint32_t e = w.me != 0 ? w.me : m.PeekWord(kPoolBasePtr);
    const uint32_t r = m.PeekWord(e + 852u), rd = m.PeekWord(e + 1084u);
    // the weapon in hand: 0 and 4 (the animation-object arm) often, any 0..9 else
    {
        const uint32_t k = rng.U32() % 6u;
        const uint32_t wpn = k == 0 ? 0u : k == 1 ? 4u : rng.U32() % 10u;
        W8(m, rd + 46u, wpn);
        W8(m, rd + 47u, chance(3) ? 0u : 1u + rng.U32() % 5u);
    }
    // the model-object bitmap: the machine's, all free, all taken (the early return), one free, noise
    {
        const uint32_t k = rng.U32() % 7u;
        uint32_t bits = m.PeekWord(W::kObjBits);
        if (k == 1) bits = 0;
        else if (k == 2) bits = 0xFFu;
        else if (k == 3) bits = 0xFFu & ~(1u << (rng.U32() % 8u));
        else if (k == 4) bits = rng.U32() & 0xFFu;
        else if (k == 5) bits = rng.U32() & 0x1FFu; // bit 8: never a slot, but not 0xFF either
        m.PokeWord(W::kObjBits, bits);
    }
    // each model object's current LOD (LodSelect runs when it differs from the weapon)
    for (uint32_t i = 0; i < 8; ++i)
        if (chance(2)) W8(m, W::kObjSlots + W::kObjBytes * i + 8u, chance(8) ? 0xFFu : rng.U32() % 10u);
    // the FreeFarRider gate: the flag, the rider's handle against the player count, a full pool
    m.PokeWord(W::kFreeFarFlag, chance(3) ? 0u : 1u);
    if (chance(3)) m.PokeWord(w.gs + 0x30u, 1u + rng.U32() % 2u);
    if (chance(3)) WriteU16Bench(m, e + 0xACu, static_cast<uint16_t>(rng.U32() % 4u));
    {
        const uint32_t cap = m.PeekWord(W::kAnimDesc + 12u);
        const uint32_t k = rng.U32() % 4u;
        if (k == 0) m.PokeWord(W::kAnimDesc + 8u, cap);
        else if (k == 1 && cap > 0) m.PokeWord(W::kAnimDesc + 8u, cap - 1u);
        if (chance(4)) // every animation object busy: ViewSlot finds none although the count has room
            for (uint32_t i = 0; i < cap && i < 32u; ++i) {
                const uint32_t o = animrows::ObjAt(m, i);
                if (m.PeekWord(o + 36u) == 0) m.PokeWord(o + 36u, 1u);
            }
    }
    W8(m, r + 0x23Cu, rng.U32());
    // the effect records and one model object's chain
    spinerows::PlantEffects(c, W::kObjSlots + W::kObjBytes * (rng.U32() % 8u));
    spinerows::PlantClock(c);
    a.a[0] = r;
    a.a[1] = chance(2) ? 0x100u : (chance(3) ? rng.U32() : 0u);
}

// ---------------------------------------------------------------- 0x800C2F84
void SetupClip(CaseContext& c, Args& a) {
    const fg::World w = fg::Stir(c);
    Memory& m = *c.mem;
    Rng& rng = *c.rng;
    auto chance = [&rng](uint32_t n) { return rng.Next() % n == 0; };
    const uint32_t e = w.me != 0 ? w.me : m.PeekWord(kPoolBasePtr);
    const uint32_t r = m.PeekWord(e + 852u);
    const uint32_t bank = m.PeekWord(W::kWeaponBank);
    const uint32_t clips = animrows::BankClips(m, bank);
    // the weapon's animation object: one of the descriptor's objects past the riders', moved to the
    // weapons bank with a program of that bank (or none: r+0x22C = 0)
    const uint32_t cap = m.PeekWord(W::kAnimDesc + 12u);
    const uint32_t slot = cap > 21u ? 21u + rng.U32() % (cap - 21u) : 0u;
    const uint32_t o = animrows::ObjAt(m, slot);
    // as ViewSlot / BankSwitch leave it: owned by a weapon model object, on the weapons bank, and a
    // current clip of that bank (a free object's +0x2C is 0, which DecodeHeader would read through)
    m.PokeWord(o, W::kObjSlots + W::kObjBytes * (rng.U32() % 8u));
    m.PokeWord(o + 40u, bank);
    m.PokeWord(o + 44u, animrows::ClipAt(m, bank, animrows::Pick(rng, clips)));
    animrows::RandomProgram(c, o, false); // the residue past op 1 (never reached, see below)
    {
        // The head of the program, as the only two writers of a weapon object's program leave it
        // (named bound in the file head): ViewSlot SLUS 0x80012884's `op0.code := 5` (a fresh object),
        // or HardStart RASHCDG 0x8005BF6C's [op1 clip 0..keys-1, op5] - OverlayClip is its only caller on
        // this object and passes ex = 0. Either head ends Seq at op 0, so the residue is never read.
        // A drawn head with a jump (op 2) may leave the 5-op program - past the last object's
        // program that is unmapped garbage, and the ORIGINAL's DecodeHeader faults (0x8005E2C8).
        const uint32_t P = m.PeekWord(o + 4u);
        if (chance(4)) {
            animrows::Put8(m, P + 1u, 5u);
        } else {
            const uint32_t clip = animrows::Pick(rng, clips);
            const uint32_t keys = animrows::KeysOf(m, animrows::ClipAt(m, bank, clip));
            const uint32_t fl = rng.U32() & 0xFFu;
            const uint32_t b3 = (fl & 8u) ? (rng.U32() & 0x1Fu) : 0u;
            for (uint32_t k = 0; k < 2; ++k) {
                const uint32_t op = P + 12u * k;
                animrows::Put8(m, op + 0u, clip);
                animrows::Put8(m, op + 1u, k == 0 ? 1u : 5u);
                animrows::Put8(m, op + 2u, fl);
                animrows::Put8(m, op + 3u, b3);
                animrows::Put16(m, op + 4u, 0u);
                animrows::Put16(m, op + 6u, k == 0 ? keys - 1u : 0u);
                m.PokeWord(op + 8u, 0u);
            }
        }
    }
    animrows::RandomClock(c, o);
    m.PokeWord(r + 0x22Cu, chance(5) ? 0u : o);
    // the stance: the rider's current one (Restart), an armed one of the bank, anything else
    const uint32_t k = rng.U32() % 5u;
    uint32_t ev;
    if (k == 0) ev = animrows::Get16(m, r + 0x220u);
    else if (k <= 2 && clips != 0) ev = 145u + rng.U32() % std::min<uint32_t>(clips, 41u);
    else ev = rng.U32() % 145u;
    a.a[0] = chance(6) ? (ev | (rng.U32() << 16)) : ev;
    a.a[1] = r;
    a.a[2] = chance(3) ? rng.U32() : rng.U32() & 0xFFu;
    a.a[3] = rng.U32() & 0x1Fu;
}

// ---------------------------------------------------------------- SLUS 0x800273EC
void SetupEffect(CaseContext& c, Args& a) {
    const fg::World w = fg::Stir(c);
    Memory& m = *c.mem;
    Rng& rng = *c.rng;
    auto chance = [&rng](uint32_t n) { return rng.Next() % n == 0; };
    // the entity: a live bike (with a handle to gate) or a weapon model object
    uint32_t e = w.me != 0 ? w.me : m.PeekWord(kPoolBasePtr);
    if (chance(2)) e = W::kObjSlots + W::kObjBytes * (rng.U32() % 8u);
    if (chance(2)) WriteU16Bench(m, e + 0xACu, static_cast<uint16_t>(chance(2) ? rng.U32() % 4u : rng.U32() % 0x80u));
    if (chance(2)) m.PokeWord(w.gs + 0x30u, 1u + rng.U32() % 2u);
    spinerows::PlantEffects(c, e);
    spinerows::PlantClock(c);
    a.a[0] = e;
    a.a[1] = chance(2) ? 0u : rng.U32();
    a.a[2] = chance(2) ? 1500u : rng.U32();
    a.a[3] = chance(2) ? 6u : rng.U32();
    a.a4 = chance(2) ? 0u : rng.U32() & 0xFFu;
}

// ---------------------------------------------------------------- the native sides
struct Callees final : W::WeaponCallees {
    NativeEnv& env;
    explicit Callees(NativeEnv& e) : env(e) {}
    bool FreeFarRider(uint32_t& v0) override {
        v0 = env.CallOracle(kFreeFarRider, 0, 0, 0, 0);
        return env.failure.empty();
    }
};

rr::sim::SpineIo Io(NativeEnv& env) {
    rr::sim::SpineIo io;
    io.rootCounter = env.clone->cpu.rootCounterValue;
    return io;
}

uint32_t NObject(NativeEnv& env, const Args& a) {
    rr::sim::GuestRam g(env.clone->mem.ram().data(), env.gp);
    Callees c(env);
    uint32_t v0 = 0;
    const bool ok = W::WeaponObject(g, c, a.a[0], a.a[1], Io(env), v0);
    if (!ok && env.failure.empty() && !g.Faulted()) env.failure = "WeaponObject refused";
    return RqDone(env, g, v0);
}

uint32_t NClip(NativeEnv& env, const Args& a) {
    rr::sim::GuestRam g(env.clone->mem.ram().data(), env.gp);
    g.SetScratchpad(env.clone->mem.scratchpad().data());
    animrows::AnimOracleSeams seams(env);
    rr::sim::AnimMachine anim(g, seams);
    uint32_t v0 = env.clone->cpu.regs[2]; // the v0 the caller leaves: returned untouched with no object
    const bool ok = W::OverlayClip(anim, a.a[0], a.a[1], a.a[2], a.a[3], v0);
    if (!ok && env.failure.empty() && !g.Faulted()) env.failure = "OverlayClip refused";
    return RqDone(env, g, v0);
}

uint32_t NEffect(NativeEnv& env, const Args& a) {
    rr::sim::GuestRam g(env.clone->mem.ram().data(), env.gp);
    W::ObjectEffect(g, a.a[0], a.a[1], a.a[2], a.a[3], a.a4, Io(env));
    return RqDone(env, g, 0);
}

} // namespace wpn

void AddWeaponRows(std::vector<Row>& rows) {
    using namespace wpn;
    {
        Row r{"weapon_object", "s32 WeaponObject(Rider *r, u32 side)  [RASHCDG 0x800958F0]", W::kWeaponObjectFn,
              SetupObject, nullptr, true, NObject, {OracleCallee{kFreeFarRider, 0, 0, {}, 0}}};
        r.oracleFrame = 48;
        rows.push_back(std::move(r));
    }
    {   // the pose side is reached through Restart (32) and PoseStart (32), below this function's 32
        Row r{"weapon_overlay_clip", "s32 OverlayClip(ev, Rider *r, flags, rate)  [RASHCDG 0x800C2F84]",
              W::kOverlayClipFn, SetupClip, nullptr, true, NClip,
              {OracleCallee{rr::sim::kAnimTransitionCaptureFn, 3, 0, {}, 32 + 32},
               OracleCallee{rr::sim::kAnimApplyFrameFn, 1, 0, {}, 32 + 32}}};
        r.oracleFrame = 32;
        rows.push_back(std::move(r));
    }
    rows.push_back(Row{"weapon_object_effect",
                       "void ObjectEffect(Entity *e, a1, a2, a3, [u8 flags])  [SLUS 0x800273EC]", W::kObjectEffectFn,
                       SetupEffect, nullptr, false, NEffect, {}});
}
