// Bench rows of the hazard objects (src\game\sim\hazard.h). Included LAST by
// verify_physics.cpp inside its anonymous namespace (rows_engine_note.inc's sf::InjectLeaf, rows_recover.inc's
// rcv:: and rows_world.inc's worldrows:: are visible).
//
// The machine is a race capture (rr-race by default): its hazard blocks exist (the loader made them:
// *(0x8005B24C) six objects, *(0x8005B304) three records, *(0x8005B250) their model records, *(0x8005B35C)
// the events) but that race has none, so every case plants them. The objects are ENV.EN's own templates
// out of the player's work\disc_us\DATA\ENV.EN (0..26 are the used ones), stirred (flags, mode, key count,
// times, speeds), a random-leg object (byte 0 = -1) or the class-9 arm's zero object; the records are
// planted live near or far from player 1's bike, bound to the prop model by ModelBind (SLUS 0x8002FAD4,
// run natively on both machines by the setup - its own row accepts it) and set at any key of their object;
// the events on player 1's road near or far, armed or not, with or without a window.
//
// NAMED BOUNDS (states the game builds): an object's key count is 2..7 (its arrays hold seven); a used key
// segment has a positive duration and distinct time keys except in a stirred minority (the divides then
// take the console's divide-by-zero path, which the ports reproduce); rows and headings are unit vectors
// (a non-unit one makes Normalize raise its overflow exception on the console); HazardSetup's object list
// stays below its frame's locals (at most six entries: the disc has one event of at most three objects per
// race and only the first pair of a selection names a set below 6).
namespace hzrows {

namespace s = rr::sim;

bool Chance(Rng& r, uint32_t n) { return r.U32() % n == 0u; }
void Put16(Memory& m, uint32_t a, uint16_t v) { m.WriteBlock(a, &v, 2); }
void Put8(Memory& m, uint32_t a, uint8_t v) { m.WriteBlock(a, &v, 1); }
uint32_t Pick(Rng& r, std::initializer_list<uint32_t> v) { return *(v.begin() + r.U32() % v.size()); }

const std::vector<uint8_t>& Env() {
    static const std::vector<uint8_t> image = [] {
        std::vector<uint8_t> v;
        std::string error;
        if (!rr::interp::ReadWholeFile("work\\disc_us\\DATA\\ENV.EN", v, error)) {
            std::fprintf(stderr, "rows_hazards: work\\disc_us\\DATA\\ENV.EN not readable (%s); the rows will fail\n",
                         error.c_str());
            v.clear();
        }
        return v;
    }();
    return image;
}
const std::vector<uint8_t>& StartDf() {
    static const std::vector<uint8_t> image = [] {
        std::vector<uint8_t> v;
        std::string error;
        if (!rr::interp::ReadWholeFile("work\\disc_us\\DATA\\STARTDFA.BIN", v, error)) v.clear();
        return v;
    }();
    return image;
}

s::GuestRam View(CaseContext& c) { return s::GuestRam(c.mem->ram().data(), c.gp); }
uint32_t Bike(Memory& m) { return m.PeekWord(s::kHzBikePtr); }

// A unit rotation of random angles, stored as s16[9] at `a`.
void PutRows(Rng& r, Memory& m, uint32_t a) {
    const int16_t ang[3] = {static_cast<int16_t>(r.U32() % 4096u), static_cast<int16_t>(r.U32() % 4096u),
                            static_cast<int16_t>(Chance(r, 2) ? 0u : r.U32() % 4096u)};
    int16_t rm[9];
    s::GuestRam g(m.ram().data(), 0);
    const s::BikeTables t = s::RecoverTables(m.ram().data(), g);
    s::RotMatrix(ang, rm, t.sincos);
    m.WriteBlock(a, rm, 18);
}
void PutUnit(Rng& r, Memory& m, uint32_t a) {
    int16_t rm[9];
    const int16_t ang[3] = {static_cast<int16_t>(r.U32() % 4096u), static_cast<int16_t>(r.U32() % 4096u), 0};
    s::GuestRam g(m.ram().data(), 0);
    const s::BikeTables t = s::RecoverTables(m.ram().data(), g);
    s::RotMatrix(ang, rm, t.sincos);
    m.WriteBlock(a, rm + 6, 6); // a matrix's row 2: a unit vector
}

// Object k of the hazard pool, planted.
uint32_t PlantObject(Rng& r, Memory& m, uint32_t k) {
    const uint32_t o = m.PeekWord(s::kHzObjectsPtr) + 312u * k;
    const std::vector<uint8_t>& env = Env();
    std::vector<uint8_t> b(312, 0);
    const uint32_t kind = r.U32() % 8u;
    if (kind <= 4 && env.size() >= 408u + 312u * 27u) {
        const uint32_t t = r.U32() % 27u;
        std::memcpy(b.data(), env.data() + 408 + 312 * t, 312);
    }
    m.WriteBlock(o, b.data(), 312);
    auto put = [&](uint32_t off, int32_t v) { m.PokeWord(o + off, static_cast<uint32_t>(v)); };
    if (kind == 5 || kind == 6) { // a random-leg object as the spawner leaves it
        Put8(m, o, 0xFF);
        Put8(m, o + 1u, static_cast<uint8_t>(r.U32() % 2u));
        Put8(m, o + 3u, 2);
        put(4, static_cast<int32_t>((r.U32() & 0xFFFFu) + 0x8000u));
        put(88, static_cast<int32_t>(r.U32() % 196608u + 0x30000u) * (Chance(r, 2) ? 1 : -1));
        put(92, Chance(r, 3) ? 0 : static_cast<int32_t>(r.U32() % 0x140000u) - 0xA0000);
        put(120, static_cast<int32_t>(r.U32() % 0x50000u) - 0x20000);
        put(116, static_cast<int32_t>(r.U32() % 0x50000u) - 0x20000);
        put(148, static_cast<int32_t>(r.U32() % 0x60000u) - 0x30000);
        put(172, Chance(r, 3) ? 0 : static_cast<int32_t>(r.U32() % 0x80000u) - 0x40000);
        put(176, static_cast<int32_t>(r.U32() % 0x80000u) - 0x40000);
        put(228, static_cast<int32_t>(r.U32() % 0x80000u) - 0x40000);
        put(284, static_cast<int32_t>(r.U32() % 900u) - 450);
        put(288, static_cast<int32_t>(r.U32() % 900u) - 450);
    } else if (kind == 7) { // the class-9 arm's object: zero but its flag byte
        Put8(m, o, 0xFF);
    } else if (!Chance(r, 3)) { // a template, stirred
        Put8(m, o, static_cast<uint8_t>(Pick(r, {0u, 1u, 1u, 2u, 3u})));
        if (Chance(r, 3)) Put8(m, o + 1u, static_cast<uint8_t>(r.U32() % 2u));
        if (Chance(r, 3)) Put8(m, o + 2u, static_cast<uint8_t>(r.U32() % 2u));
        uint32_t n = m.PeekByte(o + 3u);
        if (n < 2u || n > 7u) n = 2u + r.U32() % 4u;
        Put8(m, o + 3u, static_cast<uint8_t>(n));
        for (uint32_t j = 0; j + 1 < n; ++j) {
            if (m.PeekWord(o + 4u + 4u * j) == 0 || Chance(r, 4)) put(4 + 4 * j, static_cast<int32_t>(0x8000u + r.U32() % 0x60000u));
            if (Chance(r, 3)) put(60 + 4 * j, static_cast<int32_t>(0x10000u * j + r.U32() % 0x8000u));
            if (Chance(r, 4)) put(32 + 4 * j, static_cast<int32_t>(r.U32() % 0x20000u) - 0x10000);
        }
        for (uint32_t j = 0; j < 7; ++j)
            if (Chance(r, 4)) put(256 + 4 * j, static_cast<int32_t>(r.U32() % 0xC80000u));
        if (Chance(r, 3)) for (uint32_t j = 0; j < 7; ++j) put(284 + 4 * j, static_cast<int32_t>(r.U32() % 900u) - 450);
        if (Chance(r, 12)) put(4 + 4 * (r.U32() % n), Chance(r, 2) ? 0 : -static_cast<int32_t>(r.U32() % 0x8000u)); // the zero / negative minority
    }
    return o;
}
void PlantObjects(Rng& r, Memory& m) {
    for (uint32_t k = 0; k < 6; ++k) PlantObject(r, m, k);
    m.PokeWord(s::kHzObjectCount, r.U32() % 7u);
}

// Record i, live, on object `obj`, bound to the prop model (ModelBind pool 6, group cls).
uint32_t PlantRecord(Rng& r, CaseContext& c, uint32_t i, uint32_t obj) {
    Memory& m = *c.mem;
    const uint32_t rec = m.PeekWord(s::kHzRecordsPtr) + 280u * i;
    const uint32_t bike = Bike(m);
    std::vector<uint8_t> z(280, 0);
    m.WriteBlock(rec, z.data(), 280);
    m.PokeWord(rec + 4u, m.PeekWord(s::kHzModelsPtr) + 24u * i);
    {
        s::GuestRam g = View(c);
        uint32_t v0 = 0;
        s::ModelBind(g, rec, 6, Pick(r, {0u, 9u, 9u, 20u}), 0, v0);
    }
    Put16(m, rec + 172u, static_cast<uint16_t>(0xBDu + i));
    m.PokeWord(rec + 176u, Chance(r, 2) ? 0xFFFFFFFFu : m.PeekWord(bike + 176u));
    m.PokeWord(rec + 180u, r.U32() % 2u);
    const uint32_t reach = Pick(r, {40u, 150u, 320u, 700u});
    for (uint32_t j = 0; j < 3; ++j) {
        const int32_t d = static_cast<int32_t>(r.U32() % (2u * reach + 1u)) - static_cast<int32_t>(reach);
        m.PokeWord(rec + 184u + 4u * j, m.PeekWord(bike + 184u + 4u * j) + (static_cast<uint32_t>(j == 1 ? d / 4 : d) << 16) +
                                            (r.U32() & 0xFFFFu));
        m.PokeWord(rec + 224u + 4u * j, m.PeekWord(bike + 184u + 4u * j) + ((r.U32() % 200u) << 16) - (100u << 16));
    }
    PutRows(r, m, rec + 196u);
    PutUnit(r, m, rec + 214u);
    m.PokeWord(rec + 220u, r.U32() % 0xA00000u);
    m.PokeWord(rec + 236u, r.U32() % 4096u);
    m.PokeWord(rec + 240u, (r.U32() % 1024u) - 512u);
    PutRows(r, m, rec + 244u);
    Put8(m, rec + 262u, static_cast<uint8_t>(Pick(r, {0xFFu, 0u, 1u})));
    Put8(m, rec + 263u, static_cast<uint8_t>(Pick(r, {0u, 1u})));
    const uint32_t n = m.PeekByte(obj + 3u);
    const uint32_t k = n == 0 ? 0u : (Chance(r, 8) ? n : r.U32() % n);
    m.PokeWord(rec + 264u, k);
    const uint32_t start = r.U32() % 0x40000u;
    const uint32_t dur = k < 7 ? m.PeekWord(obj + 4u + 4u * k) : 0u;
    m.PokeWord(rec + 268u, start);
    m.PokeWord(rec + 272u, Chance(r, 6) ? 0u : start + (dur != 0 && dur < 0x1000000u ? r.U32() % (dur + 0x2000u) : r.U32() % 0x20000u));
    m.PokeWord(rec + 276u, obj);
    if (k + 2u < n && dur != 0 && dur < 0x1000000u && Chance(r, 10)) { // the next key's duration not positive, the
        m.PokeWord(obj + 8u + 4u * k, static_cast<uint32_t>(-static_cast<int32_t>(r.U32() % 0x8000u))); // clock past this key
        m.PokeWord(rec + 272u, start + dur + r.U32() % 0x800u);
    }
    Put8(m, rec + 9u, static_cast<uint8_t>(r.U32() & 3u));
    Put8(m, rec + 72u, static_cast<uint8_t>(Pick(r, {0u, 0u, 3u})));
    return rec;
}

// The events: `n` 16-byte records on player 1's road (or not), armed or not.
void PlantEvents(Rng& r, Memory& m, uint32_t n) {
    const uint32_t ev = m.PeekWord(s::kHzEventsPtr);
    const uint32_t bike = Bike(m);
    for (uint32_t k = 0; k < n; ++k) {
        const uint32_t e = ev + 16u * k;
        m.PokeWord(e, Chance(r, 5) ? m.PeekWord(bike + 360u) + 1u : m.PeekWord(bike + 360u));
        const int32_t window = static_cast<int32_t>(Pick(r, {0u, 0u, 5u, 10u, 100u, 140u, 250u}));
        const int32_t reach = static_cast<int32_t>(Pick(r, {3u, 50u, 150u, 260u}));
        const int32_t d = static_cast<int32_t>(r.U32() % static_cast<uint32_t>(2 * reach + 1)) - reach;
        m.PokeWord(e + 4u, m.PeekWord(bike + 368u) + (static_cast<uint32_t>(d) << 16));
        Put8(m, e + 8u, static_cast<uint8_t>(Pick(r, {0u, 9u})));
        for (uint32_t j = 0; j < 3; ++j) Put8(m, e + 9u + j, static_cast<uint8_t>(Chance(r, 3) ? 0xFFu : r.U32() % 6u));
        Put8(m, e + 12u, static_cast<uint8_t>(Chance(r, 4) ? 0u : 1u));
        Put16(m, e + 14u, static_cast<uint16_t>(window));
    }
    m.PokeWord(s::kHzEventCount, n);
}

void Seed(Rng& r, CaseContext& c) { c.mem->PokeWord(c.gp + 2076u, r.U32()); }

// ---------------------------------------------------------------- the natives
template <class F>
uint32_t Run(NativeEnv& env, F&& f) {
    s::GuestRam g = RqView(env);
    rcv::RecoverOracle o(env);
    const s::BikeTables t = rcv::Tables(env, g);
    uint32_t v0 = 0;
    const bool ok = f(g, t, o, v0);
    if (!ok && env.failure.empty() && !g.Faulted()) env.failure = "the port refused";
    return RqDone(env, g, v0);
}

// ---------------------------------------------------------------- Hermite / HermiteD, SLUS leaves
void SetupHermite(CaseContext& c, Args& a) {
    Rng& r = *c.rng;
    const uint32_t S = c.scratch;
    a.a[0] = Pick(r, {0u, 0x10000u, 0x8000u, r.U32() % 0x10001u, r.U32() % 0x10001u, r.U32() >> (r.U32() % 16u)});
    a.a[1] = S;
    a.a[2] = S + 4u;
    a.a[3] = S + 8u;
    a.a4 = S + 12u;
    if (Chance(r, 6)) a.a4 = a.a[2];                 // aliased outputs: the re-reads show
    if (Chance(r, 6)) a.a[3] = a.a[1];
    for (uint32_t k = 0; k < 4; ++k) c.mem->PokeWord(S + 4u * k, r.U32());
}

// ---------------------------------------------------------------- HazardRelease SLUS 0x80014000
void SetupRelease(CaseContext& c, Args& a) {
    Rng& r = *c.rng;
    Memory& m = *c.mem;
    PlantObjects(r, m);
    const uint32_t i = r.U32() % 3u;
    const uint32_t rec = PlantRecord(r, c, i, m.PeekWord(s::kHzObjectsPtr) + 312u * (r.U32() % 6u));
    if (Chance(r, 4)) Put16(m, rec + 172u, 0);
    m.PokeWord(s::kHzOut, r.U32() % 4u);
    a.a[0] = rec;
}

// ---------------------------------------------------------------- HazardPlace 0x800A1318
void SetupPlace(CaseContext& c, Args& a) {
    Rng& r = *c.rng;
    Memory& m = *c.mem;
    PlantObjects(r, m);
    const uint32_t rec = PlantRecord(r, c, r.U32() % 3u, m.PeekWord(s::kHzObjectsPtr));
    const uint32_t S = c.scratch;
    for (uint32_t j = 0; j < 3; ++j) m.PokeWord(S + 4u * j, static_cast<uint32_t>(r.S32() >> (8 + r.U32() % 12u)));
    PutRows(r, m, S + 16u);
    a.a[0] = rec;
    a.a[1] = S;
    a.a[2] = Chance(r, 3) ? rec + 196u : S + 16u;
}

// ---------------------------------------------------------------- HazardRandomize 0x800A0F9C
void SetupRandomize(CaseContext& c, Args& a) {
    Rng& r = *c.rng;
    Memory& m = *c.mem;
    PlantObjects(r, m);
    const uint32_t o = m.PeekWord(s::kHzObjectsPtr) + 312u * (r.U32() % 6u);
    Put8(m, o, 0xFF);
    Put8(m, o + 3u, 2);
    if (m.PeekWord(o + 4u) == 0 || Chance(r, 2)) m.PokeWord(o + 4u, (r.U32() & 0xFFFFu) + 0x8000u);
    if (Chance(r, 5)) m.PokeWord(o + 4u, static_cast<uint32_t>(-static_cast<int32_t>(r.U32() % 0x10000u)));
    if (Chance(r, 4)) m.PokeWord(o + 88u, static_cast<uint32_t>(-static_cast<int32_t>(r.U32() % 0x60000u)));
    if (Chance(r, 3)) m.PokeWord(o + 92u, 0);
    if (Chance(r, 3)) m.PokeWord(o + 148u, r.U32() % 0x200000u);
    Seed(r, c);
    a.a[0] = o;
}

// ---------------------------------------------------------------- HazardSpawn 0x800A0A20
void SetupSpawn(CaseContext& c, Args& a) {
    Rng& r = *c.rng;
    Memory& m = *c.mem;
    PlantObjects(r, m);
    const uint32_t taken = Pick(r, {0u, 0u, 1u, 2u, 3u});
    for (uint32_t i = 0; i < 3; ++i) {
        const uint32_t rec = m.PeekWord(s::kHzRecordsPtr) + 280u * i;
        Put16(m, rec + 172u, static_cast<uint16_t>(i < taken ? 0xBDu + i : 0u));
    }
    if (taken > 0 && taken < 3 && Chance(r, 3)) Put16(m, m.PeekWord(s::kHzRecordsPtr), 0); // a hole first
    m.PokeWord(s::kHzOut, taken);
    const uint32_t S = c.scratch;
    const uint32_t bike = Bike(m);
    for (uint32_t j = 0; j < 3; ++j) m.PokeWord(S + 4u * j, m.PeekWord(bike + 184u + 4u * j) + ((r.U32() % 400u) << 16) - (200u << 16));
    PutRows(r, m, S + 16u);
    Seed(r, c);
    a.a[0] = Pick(r, {0u, 9u, 9u, 20u});
    a.a[1] = r.U32() % 2u;
    a.a[2] = S;
    a.a[3] = Chance(r, 2) ? 0u : S + 16u;
    a.a4 = m.PeekWord(s::kHzObjectsPtr) + 312u * (r.U32() % 6u);
}

// ---------------------------------------------------------------- HazardEvent 0x800A0EC4
void SetupEvent(CaseContext& c, Args& a) {
    Rng& r = *c.rng;
    Memory& m = *c.mem;
    PlantObjects(r, m);
    for (uint32_t i = 0; i < 3; ++i) {
        const uint32_t rec = m.PeekWord(s::kHzRecordsPtr) + 280u * i;
        Put16(m, rec + 172u, static_cast<uint16_t>(Chance(r, 4) ? 0xBDu + i : 0u));
    }
    m.PokeWord(s::kHzOut, r.U32() % 2u);
    PlantEvents(r, m, 1u + r.U32() % 3u);
    const uint32_t S = c.scratch;
    const uint32_t bike = Bike(m);
    for (uint32_t j = 0; j < 3; ++j) m.PokeWord(S + 4u * j, m.PeekWord(bike + 184u + 4u * j));
    PutRows(r, m, S + 16u);
    Seed(r, c);
    a.a[0] = m.PeekWord(s::kHzEventsPtr) + 16u * (r.U32() % m.PeekWord(s::kHzEventCount));
    a.a[1] = S;
    a.a[2] = S + 16u;
}

// ---------------------------------------------------------------- HazardPass 0x800A13C4
void SetupPass(CaseContext& c, Args& a) {
    Rng& r = *c.rng;
    Memory& m = *c.mem;
    PlantObjects(r, m);
    uint32_t live = 0;
    const bool none = Chance(r, 3);
    for (uint32_t i = 0; i < 3; ++i) {
        const uint32_t rec = m.PeekWord(s::kHzRecordsPtr) + 280u * i;
        if (!none && !Chance(r, 4)) {
            PlantRecord(r, c, i, m.PeekWord(s::kHzObjectsPtr) + 312u * (r.U32() % 6u));
            ++live;
        } else {
            Put16(m, rec + 172u, 0);
        }
    }
    m.PokeWord(s::kHzOut, live);
    PlantEvents(r, m, r.U32() % 4u);
    const uint32_t bike = Bike(m);
    if (Chance(r, 6)) Put16(m, bike + 320u, 0);
    if (Chance(r, 6)) {
        const uint32_t rd = m.PeekWord(bike + 1084u);
        Put8(m, rd, static_cast<uint8_t>(m.PeekByte(rd) ^ 0x80u));
    }
    Seed(r, c);
    a.a[0] = Pick(r, {0x884u, 0x884u, 0x442u, 0x1100u, r.U32() % 0x4000u});
}

// ---------------------------------------------------------------- HazardDraw 0x800A2138
void SetupDraw(CaseContext& c, Args& a) {
    Rng& r = *c.rng;
    Memory& m = *c.mem;
    PlantObjects(r, m);
    uint32_t live = 0;
    for (uint32_t i = 0; i < 3; ++i) {
        const uint32_t rec = m.PeekWord(s::kHzRecordsPtr) + 280u * i;
        if (!Chance(r, 4)) {
            PlantRecord(r, c, i, m.PeekWord(s::kHzObjectsPtr) + 312u * (r.U32() % 6u));
            ++live;
        } else {
            Put16(m, rec + 172u, 0);
        }
    }
    m.PokeWord(s::kHzOut, live);
    a.a[0] = Pick(r, {0u, 0u, 0u, 1u, 2u});
}

// ---------------------------------------------------------------- HazardSetup / HazardPick, RASHCDI
constexpr uint32_t kEnvAt = 0x8006B8B8, kSelAt = 0x8006EB40, kCountAt = 0x8006B8B4, kEventsAt = 0x8006EB10;

void PlantEnv(Rng& r, Memory& m, bool stir) {
    std::vector<uint8_t> env = Env();
    if (env.size() < 12888u) return;
    if (stir) {
        if (Chance(r, 2)) env[204] = static_cast<uint8_t>(Chance(r, 5) ? 0u : 1u + r.U32() % 8u);
        for (uint32_t k = 0; k < 16; ++k)
            if (Chance(r, 3)) env[205 + k] = static_cast<uint8_t>(Pick(r, {1u, 2u, 3u, 4u, 5u, 6u, 9u, 0u}));
    }
    m.WriteBlock(kEnvAt, env.data(), 12888);
}
// The class table as BuildRace leaves it: a real STARTDFA block's, or stirred.
void PlantTable(Rng& r, Memory& m) {
    const std::vector<uint8_t>& df = StartDf();
    uint8_t t[8] = {0xFF, 0, 0, 0, 0xFF, 0, 0, 0};
    if (df.size() >= 292u * 64u && !Chance(r, 3)) {
        std::memcpy(t, df.data() + 292u * (r.U32() % 64u) + 220u, 8);
    } else if (!Chance(r, 3)) {
        t[0] = static_cast<uint8_t>(Pick(r, {1u, 2u, 3u, 4u, 5u, 9u, 0u}));
        t[1] = static_cast<uint8_t>(Pick(r, {0u, 0u, 1u, 0x9Cu, 0xFEu, 0xFBu, 0xF0u}));
        const uint16_t w = static_cast<uint16_t>(Pick(r, {0u, 10u, 140u}));
        std::memcpy(t + 2, &w, 2);
        if (Chance(r, 3)) {
            t[4] = static_cast<uint8_t>(Pick(r, {1u, 2u, 3u, 4u, 5u, 9u}));
            t[5] = static_cast<uint8_t>(Pick(r, {0u, 0x9Cu, 0xFDu}));
        }
    }
    m.WriteBlock(s::kHzTable, t, 8);
}

void SetupPick(CaseContext& c, Args&) {
    sf::InjectLeaf(c, s::kHazardPickFn, s::kHazardSetupFn);
    Rng& r = *c.rng;
    PlantEnv(r, *c.mem, !Chance(r, 3));
    PlantTable(r, *c.mem);
    // NAMED BOUND: no pairs (ENV.EN +204 = 0) comes with an empty class table - with a set named, the
    // original's search would walk the bytes past ENV.EN for 2^32 steps (the disc holds five pairs)
    if (c.mem->PeekByte(kEnvAt + 204u) == 0) Put8(*c.mem, s::kHzTable, 0xFF);
    c.mem->PokeWord(kSelAt, r.U32());
    Seed(r, c);
}
uint32_t NativePick(NativeEnv& env, const Args&) {
    s::GuestRam g = RqView(env);
    std::vector<uint8_t> env8(12888);
    g.ReadBlock(kEnvAt, env8.data(), 12888);
    int32_t sel = 0, v = 0;
    const bool ok = s::HazardPick(g, env8.data(), env8.size(), sel, v);
    g.W32(kSelAt, static_cast<uint32_t>(sel)); // the original's own store of sel
    if (!ok && env.failure.empty()) env.failure = "HazardPick refused";
    return RqDone(env, g, static_cast<uint32_t>(v));
}

void SetupSetup(CaseContext& c, Args&) {
    sf::InjectLeaf(c, s::kHazardSetupFn, 0x8006B3FCu);
    Rng& r = *c.rng;
    Memory& m = *c.mem;
    PlantEnv(r, m, false);
    PlantTable(r, m);
    const std::vector<uint8_t>& df = StartDf();
    // sel: one whose first pair value is a set (1..5); the second is 6 on the disc
    const int32_t sel = static_cast<int32_t>(r.U32() % 5u);
    m.PokeWord(kSelAt, static_cast<uint32_t>(sel));
    uint8_t ev[48] = {};
    uint32_t count = 0;
    if (df.size() >= 292u * 64u && !Chance(r, 4)) {
        const uint32_t b = 292u * (r.U32() % 64u);
        std::memcpy(&count, df.data() + b + 228u, 4);
        std::memcpy(ev, df.data() + b + 232u, 48);
        if (count > 2u) count = 2u;
    } else {
        count = r.U32() % 3u;
        for (uint32_t k = 0; k < count; ++k) {
            const uint32_t road = Chance(r, 2) ? m.PeekWord(Bike(m) + 360u) : r.U32() % 40u;
            const int32_t lo = static_cast<int32_t>(r.U32() % 4000u) << 16;
            const int32_t hi = lo + (static_cast<int32_t>(r.U32() % 60u) - (Chance(r, 6) ? 30 : 0)) * 65536;
            std::memcpy(ev + 16 * k, &road, 4);
            std::memcpy(ev + 16 * k + 4, &lo, 4);
            std::memcpy(ev + 16 * k + 8, &hi, 4);
            const int16_t var = static_cast<int16_t>(Pick(r, {0u, 1u, 2u, 7u, 0xFFFFu}));
            const int16_t win = static_cast<int16_t>(Pick(r, {0u, 1u, 100u, 140u, 0xFFFFu}));
            std::memcpy(ev + 16 * k + 12, &var, 2);
            std::memcpy(ev + 16 * k + 14, &win, 2);
        }
    }
    m.PokeWord(kCountAt, count);
    m.WriteBlock(kEventsAt, ev, 48);
    Seed(r, c);
    // The race heap as the loader has it: the capture's heap is full (its four hazard blocks are taken),
    // so the upper half of the scratch block goes on the free list's head as one free block {next, size}
    // (SLUS 0x800142B4's list at 0x800D6500) - the set-up's four mallocs then come out of it, first fit.
    const uint32_t blk = c.scratch + 4096u;
    m.PokeWord(blk, m.PeekWord(0x800D6500u));
    m.PokeWord(blk + 4u, 4096u);
    m.PokeWord(0x800D6500u, blk);
}
uint32_t NativeSetup(NativeEnv& env, const Args&) {
    s::GuestRam g = RqView(env);
    std::vector<uint8_t> env8(12888);
    g.ReadBlock(kEnvAt, env8.data(), 12888);
    s::HazardLoaderData d;
    d.env = env8.data();
    d.envSize = env8.size();
    d.sel = g.S32(kSelAt);
    d.eventCount = g.S32(kCountAt);
    g.ReadBlock(kEventsAt, d.events, 48);
    rcv::RecoverOracle o(env);
    const bool ok = s::HazardSetup(g, d, env.sp, o);
    if (!ok && env.failure.empty() && !g.Faulted()) env.failure = "HazardSetup refused";
    return RqDone(env, g, 0);
}

Row HRow(const char* name, const char* sig, uint32_t entry, std::function<void(CaseContext&, Args&)> setup,
         std::function<uint32_t(NativeEnv&, const Args&)> native, bool v0, std::vector<OracleCallee> seams = {}) {
    Row r{name, sig, entry, std::move(setup), nullptr, v0, std::move(native)};
    r.maxSteps = 40'000'000;
    r.oracleCallees = std::move(seams);
    return r;
}

} // namespace hzrows

void AddHazardRows(std::vector<Row>& rows) {
    using namespace hzrows;
    using G = s::GuestRam;
    using T = s::BikeTables;
    using C = rr::sim::RecoverCallees;
    rows.push_back(HRow("hazard_hermite", "void 0x8002FA28(s32 t, s32 *h00, s32 *h01, s32 *h10, [s32 *h11])  [SLUS]",
                        s::kHermiteFn, SetupHermite, [](NativeEnv& env, const Args& a) {
                            return Run(env, [&](G& g, const T&, C&, uint32_t&) {
                                s::Hermite(g, static_cast<int32_t>(a.a[0]), a.a[1], a.a[2], a.a[3], a.a4);
                                return true;
                            });
                        }, false));
    rows.push_back(HRow("hazard_hermite_d", "void 0x8002F994(s32 t, s32 *d00, s32 *d01, s32 *d10, [s32 *d11])  [SLUS]",
                        s::kHermiteDFn, SetupHermite, [](NativeEnv& env, const Args& a) {
                            return Run(env, [&](G& g, const T&, C&, uint32_t&) {
                                s::HermiteD(g, static_cast<int32_t>(a.a[0]), a.a[1], a.a[2], a.a[3], a.a4);
                                return true;
                            });
                        }, false));
    rows.push_back(HRow("hazard_release", "void 0x80014000(Rec *r)  [SLUS]", s::kHazardReleaseFn, SetupRelease,
                        [](NativeEnv& env, const Args& a) {
                            return Run(env, [&](G& g, const T&, C&, uint32_t&) {
                                s::HazardRelease(g, a.a[0]);
                                return true;
                            });
                        }, false));
    rows.push_back(HRow("hazard_place", "void 0x800A1318(Rec *r, s32 local[3], s16 rows[9])  [RASHCDG]", s::kHazardPlaceFn,
                        SetupPlace, [](NativeEnv& env, const Args& a) {
                            return Run(env, [&](G& g, const T&, C&, uint32_t&) {
                                s::HazardPlace(g, a.a[0], a.a[1], a.a[2], env.sp);
                                return true;
                            });
                        }, false));
    rows.push_back(HRow("hazard_randomize", "void 0x800A0F9C(Obj *o)  [RASHCDG, a random object's next leg]",
                        s::kHazardRandomizeFn, SetupRandomize, [](NativeEnv& env, const Args& a) {
                            return Run(env, [&](G& g, const T&, C&, uint32_t&) {
                                s::HazardRandomize(g, a.a[0], env.sp);
                                return true;
                            });
                        }, false));
    rows.push_back(HRow("hazard_spawn", "Rec *0x800A0A20(cls, mode, s32 pos[3], s16 rows[9], [Obj *o])  [RASHCDG]",
                        s::kHazardSpawnFn, SetupSpawn, [](NativeEnv& env, const Args& a) {
                            return Run(env, [&](G& g, const T& t, C&, uint32_t& v) {
                                return s::HazardSpawn(g, a.a[0], a.a[1], a.a[2], a.a[3], a.a4, env.sp, t, v);
                            });
                        }, true));
    rows.push_back(HRow("hazard_event", "void 0x800A0EC4(Event *e, s32 pos[3], s16 rows[9])  [RASHCDG]", s::kHazardEventFn,
                        SetupEvent, [](NativeEnv& env, const Args& a) {
                            return Run(env, [&](G& g, const T& t, C&, uint32_t&) {
                                return s::HazardEvent(g, a.a[0], a.a[1], a.a[2], env.sp, t);
                            });
                        }, false));
    rows.push_back(HRow("hazard_pass", "void 0x800A13C4(s32 dt)  [RASHCDG, the hazard objects' pass]", s::kHazardPassFn,
                        SetupPass, [](NativeEnv& env, const Args& a) {
                            return Run(env, [&](G& g, const T& t, C&, uint32_t&) {
                                return s::HazardPass(g, static_cast<int32_t>(a.a[0]), env.sp, t);
                            });
                        }, false));
    rows.push_back(HRow("hazard_draw", "void 0x800A2138(p)  [RASHCDG, the hazard records' cell, frame, ModelVisible]",
                        s::kHazardDrawFn, SetupDraw, [](NativeEnv& env, const Args& a) {
                            return Run(env, [&](G& g, const T&, C& o, uint32_t&) { return s::HazardDraw(g, a.a[0], env.sp, o); });
                        }, false, {OracleCallee{s::kHzModelVisibleFn, 2}}));
    rows.push_back(HRow("hazard_pick", "s32 0x8006AD4C()  [RASHCDI, the HAZARD set and its pair]", s::kHazardPickFn,
                        SetupPick, NativePick, true));
    rows.push_back(HRow("hazard_setup", "void 0x8006AEF4()  [RASHCDI, the events, objects and records]", s::kHazardSetupFn,
                        SetupSetup, NativeSetup, false, {OracleCallee{s::kHzMallocFn, 2}}));
}

// The extent [entry, end) of each function these rows accept, for `--coverage` (the next function's entry).
const CoverageRange* HazardCoverage(const char* rowName) {
    struct Named {
        const char* name;
        CoverageRange range;
    };
    static const Named kRanges[] = {
        {"hazard_hermite", {0x8002FA28, 0x8002FAD4}},  {"hazard_hermite_d", {0x8002F994, 0x8002FA28}},
        {"hazard_release", {0x80014000, 0x80014044}},  {"hazard_place", {0x800A1318, 0x800A13C4}},
        {"hazard_randomize", {0x800A0F9C, 0x800A1318}}, {"hazard_spawn", {0x800A0A20, 0x800A0EC4}},
        {"hazard_event", {0x800A0EC4, 0x800A0F9C}},    {"hazard_pass", {0x800A13C4, 0x800A2138}},
        {"hazard_draw", {0x800A2138, 0x800A2448}},     {"hazard_pick", {0x8006AD4C, 0x8006AEF4}},
        {"hazard_setup", {0x8006AEF4, 0x8006B3FC}},
    };
    for (const Named& n : kRanges)
        if (std::strcmp(n.name, rowName) == 0) return &n.range;
    return nullptr;
}
