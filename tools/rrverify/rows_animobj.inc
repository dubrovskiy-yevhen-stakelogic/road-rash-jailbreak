// Bench rows of the rider animation objects (src\game\sim\anim_objects.h). Included LAST by verify_physics.cpp
// inside its anonymous namespace (rows_grid.inc's
// gridr::InjectOverlay, rows_bike_step.inc's bs::ExactOracle and rows_population.inc's pop:: are visible).
//
//   anim_desc_init     RASHCDI 0x8005D130 AnimDescInit(desc)
//   anim_objects_init  RASHCDI 0x8005D1A0 AnimObjectsInit(desc, n)   Malloc SLUS 0x8001447C through the oracle
//   race_start         SLUS 0x800119C0 RaceStart() WHOLE             ViewSlot / BankSwitch natively (PORTED);
//                      the stance event, the arm's EntityCell / BuildObb / Transition and the tail through the oracle
//
// The two RASHCDI rows lay the WHOLE loader overlay image over 0x8005B5E8 on both machines (rows_grid.inc's
// rule): it is not resident in the rr-race machine.
//
// INPUT FAMILIES
//   anim_desc_init: the descriptor at 0x800CE170 or a scratch copy, its eight words and the ten slot records
//     0x800CF5D8 (all 120 bytes) filled with noise, so every byte the leaf leaves alone is compared too.
//   anim_objects_init: the machine's own descriptor (or a scratch one), n = 0..26 (the loader's counts are
//     19..25), now and then 0 or negative; the descriptor's words noised first.
//   race_start: rr-race's pool 0 (18 bikes, their live / dormant +0x140 as captured) with the descriptor as the
//     loader leaves it (every object free, none used) or with some objects taken (flags 5 / 0x17, the used
//     count raised to match), the bank words 0 (bank 0) and 6 (the passenger bank: 0, rr-race's, a slot
//     record), 0..2 bikes made sidecars (+0xB4 in 6..8 / 15..17, +0x358 another pool-0 bike, its rider's +0x23C
//     bit 5 drawn), game_state+4 bit 0 drawn (the 0x800901A8 arm), *(0x8005B220) now and then 1 (0x80012764 /
//     0x80018C1C).
// NAMED BOUNDS:
//   * race_start: the descriptor has a free object for every rider the loop asks for (the loader sizes it as
//     pool 1 + 4 x the pedestrian switch + 1): with none, ViewSlot returns 0 and the original's BankSwitch
//     stores through address 0x28, which the port refuses.
//   * race_start: a sidecar-class bike has a passenger bike at +0x358 (SpawnPassenger 0x800670FC always
//     stores it); 0 there makes the original load through address 0x354.
//   * race_start: *(0x800CD694) != 2 with one player: the wait loop 0x80011BC4 spins on a word only the CD
//     stream's interrupt changes, which the bench does not raise.
//   * race_start: the kernel's two critical-section stubs (SLUS 0x80043DA4 / 0x80043DB4) have their `syscall`
//     replaced by a nop on both sides (rows_feel.inc's bound): the bench has no interrupts to hold off.
//   * anim_objects_init: n <= 26 so that SLUS 0x8001447C finds room in rr-race's heap for both blocks (the
//     loader's own n is at most 25); with no room it returns 0 and the original stores through 0x24.
namespace animobjrows {

using pop::Chance;

constexpr uint32_t kDesc = 0x800CE170u, kSlots = 0x800CF5D8u, kBankTable = 0x800CE190u;
constexpr uint32_t kPoolTab = 0x800CE4D0u, kGs = 0x8005B2F8u;
constexpr uint32_t kMalloc = 0x8001447Cu;
constexpr uint32_t kStance = 0x800C4550u, kEntityCell = 0x8008B99Cu, kBuildObb = 0x8008BA18u,
                   kTransition = 0x80093F94u;

void Noise(CaseContext& c, uint32_t at, uint32_t bytes) {
    for (uint32_t k = 0; k < bytes; k += 4) c.mem->PokeWord(at + k, c.rng->U32());
}

// ---------------------------------------------------------------- RASHCDI 0x8005D130
void SetupDescInit(CaseContext& c, Args& a) {
    gridr::InjectOverlay(c);
    const uint32_t d = Chance(*c.rng, 3) ? c.scratch + 0x400u : kDesc;
    Noise(c, d, 32);
    Noise(c, kSlots, 120);
    a.a[0] = d;
}
uint32_t NativeDescInit(NativeEnv& env, const Args& a) {
    rr::sim::GuestRam g = pop::View(env);
    rr::sim::AnimDescInit(g, a.a[0]);
    return pop::PopDone(env, g, true, "AnimDescInit");
}

// ---------------------------------------------------------------- RASHCDI 0x8005D1A0
struct ObjSeams final : rr::sim::AnimObjectsCallees {
    NativeEnv& env;
    explicit ObjSeams(NativeEnv& e) : env(e) {}
    bool Malloc(uint32_t bytes, uint32_t heap, uint32_t sp, uint32_t& v0) override {
        const uint32_t r[2] = {bytes, heap};
        v0 = bs::ExactOracle(env, kMalloc, r, 2, sp);
        return env.failure.empty();
    }
};
void SetupObjectsInit(CaseContext& c, Args& a) {
    gridr::InjectOverlay(c);
    Rng& r = *c.rng;
    uint32_t d = kDesc;
    if (Chance(r, 3)) {
        d = c.scratch + 0x400u;
        Noise(c, d, 32);
    } else if (Chance(r, 2)) {
        Noise(c, d, 16);
    }
    int32_t n = static_cast<int32_t>(19u + r.U32() % 7u);
    if (Chance(r, 3)) n = static_cast<int32_t>(r.U32() % 27u);
    if (Chance(r, 16)) n = -static_cast<int32_t>(1u + r.U32() % 4u);
    a.a[0] = d;
    a.a[1] = static_cast<uint32_t>(n);
}
uint32_t NativeObjectsInit(NativeEnv& env, const Args& a) {
    rr::sim::GuestRam g = pop::View(env);
    ObjSeams s(env);
    const bool ok = rr::sim::AnimObjectsInit(g, a.a[0], static_cast<int32_t>(a.a[1]), env.sp, s);
    return pop::PopDone(env, g, ok || !env.failure.empty(), "AnimObjectsInit");
}

// ---------------------------------------------------------------- SLUS 0x800119C0
struct StartSeams final : rr::sim::RaceStartCallees {
    NativeEnv& env;
    explicit StartSeams(NativeEnv& e) : env(e) {}
    bool Call(uint32_t f, std::initializer_list<uint32_t> args, uint32_t sp) {
        uint32_t r[4] = {0, 0, 0, 0};
        int n = 0;
        for (uint32_t x : args) r[n++] = x;
        bs::ExactOracle(env, f, r, n, sp);
        return env.failure.empty();
    }
    bool StanceEvent(uint32_t ev, uint32_t rd, uint32_t p, uint32_t sp) override { return Call(kStance, {ev, rd, p}, sp); }
    bool EntityCell(uint32_t e, uint32_t sp) override { return Call(kEntityCell, {e}, sp); }
    bool BuildObb(uint32_t e, uint32_t sp) override { return Call(kBuildObb, {e}, sp); }
    bool Transition(uint32_t e, uint32_t to, uint32_t sp) override { return Call(kTransition, {e, to}, sp); }
    bool Tail(uint32_t fn, int n, uint32_t a0, uint32_t sp) override {
        return n == 0 ? Call(fn, {}, sp) : Call(fn, {a0}, sp);
    }
};

void SetupRaceStart(CaseContext& c, Args&) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    const uint32_t base = m.PeekWord(kPoolTab), stride = m.PeekWord(kPoolTab + 4u);
    const int32_t high = static_cast<int32_t>(m.PeekWord(m.PeekWord(kPoolTab + 12u)));
    const uint32_t bikes = static_cast<uint32_t>(high + 1);
    // sidecars
    uint32_t sidecars = 0;
    if (!Chance(r, 3)) {
        const uint32_t want = 1u + r.U32() % 2u;
        for (uint32_t k = 0; k < want; ++k) {
            const uint32_t e = base + stride * (r.U32() % bikes);
            uint32_t p = base + stride * (r.U32() % bikes);
            if (p == e) p = base + stride * ((((p - base) / stride) + 1u) % bikes);
            static const uint32_t kCls[6] = {6, 7, 8, 15, 16, 17};
            if (m.PeekWord(e + 856u) == 0u || Chance(r, 2)) m.PokeWord(e + 856u, p);
            const uint32_t before = m.PeekWord(e + 180u);
            m.PokeWord(e + 180u, kCls[r.U32() % 6u]);
            if (!((before - 6u) < 3u || (before - 15u) < 3u)) ++sidecars;
            const uint32_t pr = m.PeekWord(m.PeekWord(e + 856u) + 852u);
            uint8_t f = m.PeekByte(pr + 572u);
            f = static_cast<uint8_t>(Chance(r, 2) ? (f | 0x20u) : (f & ~0x20u));
            m.WriteBlock(pr + 572u, &f, 1);
        }
    }
    // the descriptor: every object free (the loader's state), or some taken with room left for the loop
    const uint32_t objs = m.PeekWord(kDesc), count = m.PeekWord(kDesc + 12u);
    const uint32_t need = bikes + sidecars;
    for (uint32_t i = 0; i < count; ++i) m.PokeWord(objs + 2108u * i + 36u, 0);
    uint32_t used = 0;
    if (Chance(r, 2) && count > need) {
        const uint32_t take = r.U32() % (count - need + 1u);
        for (uint32_t k = 0; k < take; ++k) {
            const uint32_t i = r.U32() % count;
            if (m.PeekWord(objs + 2108u * i + 36u) != 0u) continue;
            m.PokeWord(objs + 2108u * i + 36u, Chance(r, 2) ? 5u : 0x17u);
            ++used;
        }
    }
    m.PokeWord(kDesc + 8u, used);
    // the bank words 0 and 6
    if (Chance(r, 5)) m.PokeWord(kBankTable, 0);
    {
        const uint32_t k = r.U32() % 4u;
        if (k == 0) m.PokeWord(kBankTable + 24u, 0);
        else if (k == 1) m.PokeWord(kBankTable + 24u, kSlots + 12u * (r.U32() % 6u));
    }
    // the race type's bit 0, the jail word, the stream wait
    const uint32_t gs = m.PeekWord(kGs);
    if (Chance(r, 3)) {
        uint8_t t = m.PeekByte(gs + 4u);
        t = static_cast<uint8_t>(t ^ 1u);
        m.WriteBlock(gs + 4u, &t, 1);
    }
    m.PokeWord(0x8005B220u, Chance(r, 8) ? 1u : 0u);
    if (m.PeekWord(0x800CD694u) == 2u) m.PokeWord(0x800CD694u, 0);
    m.PokeWord(0x80043DA8u, 0); // the kernel's two critical-section syscalls: nop (rows_feel.inc's bound)
    m.PokeWord(0x80043DB8u, 0);
}
uint32_t NativeRaceStart(NativeEnv& env, const Args&) {
    rr::sim::GuestRam g = pop::View(env);
    StartSeams s(env);
    const bool ok = rr::sim::RaceStart(g, env.sp, s);
    return pop::PopDone(env, g, ok || !env.failure.empty(), "RaceStart");
}

} // namespace animobjrows

void AddAnimObjRows(std::vector<Row>& rows) {
    {
        Row r{"anim_desc_init", "void AnimDescInit(Desc *d)  [RASHCDI 0x8005D130]", rr::sim::kAnimDescInitFn,
              animobjrows::SetupDescInit, nullptr, /*compareV0=*/false, animobjrows::NativeDescInit};
        rows.push_back(std::move(r));
    }
    {
        Row r{"anim_objects_init", "void AnimObjectsInit(Desc *d, s32 n)  [RASHCDI 0x8005D1A0]",
              rr::sim::kAnimObjectsInitFn, animobjrows::SetupObjectsInit, nullptr, /*compareV0=*/false,
              animobjrows::NativeObjectsInit};
        r.oracleCallees = {OracleCallee{animobjrows::kMalloc, 2}};
        r.maxSteps = 20'000'000;
        r.allowBiosCalls = true; // SLUS 0x8001447C reaches a kernel function through the A0 vector
        rows.push_back(std::move(r));
    }
    {
        Row r{"race_start", "void RaceStart(void)  [SLUS 0x800119C0, the race's first frame: objects, stances, primes]",
              rr::sim::kRaceStartFn, animobjrows::SetupRaceStart, nullptr, /*compareV0=*/false,
              animobjrows::NativeRaceStart};
        r.oracleCallees = {OracleCallee{animobjrows::kStance, 3},     OracleCallee{animobjrows::kEntityCell, 1},
                           OracleCallee{animobjrows::kBuildObb, 1},   OracleCallee{animobjrows::kTransition, 2},
                           OracleCallee{0x800A41ECu, 0},              OracleCallee{0x800A4774u, 1},
                           OracleCallee{0x8008D89Cu, 0},              OracleCallee{0x8009C308u, 0},
                           OracleCallee{0x800901A8u, 1},              OracleCallee{0x80012764u, 1},
                           OracleCallee{0x8001654Cu, 0},              OracleCallee{0x80030608u, 0},
                           OracleCallee{0x800247E8u, 0},              OracleCallee{0x800164B4u, 0},
                           OracleCallee{0x8001A424u, 0},              OracleCallee{0x80018C1Cu, 1}};
        r.maxSteps = 400'000'000;
        r.allowBiosCalls = true; // the tail's SpeechInit 0x8001A424 mallocs (SLUS 0x8001447C: the A0 vector)
        rows.push_back(std::move(r));
    }
}
