// Bench rows for the race camera. Included by
// verify_physics.cpp inside its anonymous namespace, so everything defined above that point is
// visible here.
//
// The port is src\game\sim\camera.{h,cpp}: ViewUpdate RASHCDG 0x800881B4 and the fourteen functions
// under it, over road_query.h's guest-address view. One row per function.
//
// INPUT FAMILIES. Every row runs on THIS machine's view record 0 (0x800CD898), in place - its road
// cursor, slice and roadside pointers are this machine's, because ViewUpdate re-binds the view record
// to the road like an entity (SLUS 0x800374D4) and a cursor lifted from another image would point at
// road rr-race does not hold.
//   * dump-derived: the CAMERA BLOCK +0x21C..+0x46B of view record 0 of one of the 18 RAM images
//     (4 savestates, 14 frame dumps), minus its three pointers (+0x238, +0x2F0, +0x2F4), with the eye
//     +0xB8, the look point +0x22C and the aim point +0x23C re-based onto this machine's followed bike
//     at the captured offsets, the captured render/aim frames and the captured frame dt. The first
//     image IS rr-race, so one family member is the untouched console state;
//   * randomised: the same block drawn - every flag bit of +0x224 / +0x228 that the code tests, the
//     mode +0x21C over 0..14, the director switch +0x304 a quarter of the time with its shot index,
//     springs, angles, rates, the six six-knot tracks and their durations; the followed entity is
//     the player's bike, another live bike, or (Aim, SetMode) a non-bike entity; the followed bike's
//     and rider's flag bits the camera reads, its speed and off-road bit; the root counter value
//     (GetRCnt) per case.
// Named bounds - each a range the original's own data stays in, and each stated because the row
// would otherwise run into a hang or an unrelated trap rather than test the port:
//   * the director's last pick +0x310 is never a value the random pick can produce (0x7FFF0000): with
//     the bench's constant root counter a pick equal to it loops forever;
//   * the knot count +0x320 is 0..6 (six knots per track in the record) and the knot times are
//     increasing, the segment durations positive;
//   * the shot index +0x308 is 0..8 (the table holds 9 shots): the shot script decides the knot count
//     +0x320, and a script read from beyond the table makes SLUS 0x8002F634 overrun its ten-entry
//     stack arrays - measured: the original traps (a misaligned load at 0x8002F740);
//   * +0x228 bit 1 (advance to the next shot) only on shots 0..7, for the same reason;
//   * a director pan (mode 12 with +0x304) has a shot point +0x2F0 (see PlantRandom);
//   * race type 44 (the director's shots 4/5) comes with a passenger on the player's bike (PlantRandom);
//   * +0x224 bit 28 comes with a non-zero lead factor +0x460 (see PlantRandom);
//   * the mode is 0..14 (the CAMERA.CA record index is the mode, and a mode >= 15 reads the record
//     out of the view array itself; modes 7..12 without the director already do that, and are kept).

namespace camrows {

namespace s = rr::sim;

constexpr uint32_t kCamViewUpdate = 0x800881B4, kCamAim = 0x80087420, kCamOrient = 0x80086E1C,
                   kCamTransition = 0x8008676C, kCamLead = 0x800871A8, kCamToFrame = 0x80086584,
                   kCamToPolar = 0x800863EC, kCamAngleChase = 0x80086C00, kCamSetMode = 0x8008A998,
                   kCamChaseSpring = 0x80086B1C, kCamAngleSpring = 0x80086D54, kCamHermite = 0x8002FA28,
                   kCamZones = 0x80088140, kCamResetFlags = 0x8008AAB0, kCamSpringReset = 0x80086AF8;
// The unported callees, executed by the oracle (camera.h).
constexpr uint32_t kCamShotSetup = 0x800853E4, kCamSplineSlopes = 0x8002F634, kCamRaceOver = 0x80018C1C,
                   kCamGetRCnt = 0x80043F00;
constexpr uint32_t kCamView = s::kCameraViews;
constexpr uint32_t kCamBikeStride = 1096;
constexpr uint32_t kCamLiveBikes = 18; // rr-race: pool 0 slots 0..17

// ---------------------------------------------------------------------------- captured views
struct CapturedView {
    std::vector<uint8_t> view;  // 1132 bytes of view record 0
    std::vector<uint8_t> bike;  // 1096 bytes of the bike it follows
    int32_t dt = 0;
};

uint32_t Rd32(const std::vector<uint8_t>& ram, uint32_t a) {
    const uint32_t o = a & 0x1FFFFFu;
    if (o + 4u > ram.size()) return 0;
    return static_cast<uint32_t>(ram[o]) | (static_cast<uint32_t>(ram[o + 1]) << 8) |
           (static_cast<uint32_t>(ram[o + 2]) << 16) | (static_cast<uint32_t>(ram[o + 3]) << 24);
}

// The same RAM images `HarvestSamples` reads (its default roots), read once; rr-race sorts first.
const std::vector<CapturedView>& Views() {
    static const std::vector<CapturedView> views = [] {
        namespace fs = std::filesystem;
        std::vector<std::string> files;
        std::error_code ec;
        if (fs::is_directory("work\\oracle\\state", ec))
            for (const auto& d : fs::directory_iterator("work\\oracle\\state", ec)) {
                const fs::path p = d.path() / "ram.bin";
                if (d.is_directory() && fs::exists(p, ec)) files.push_back(p.string());
            }
        if (fs::is_directory("work\\oracle\\vr_capture\\ramdumps", ec))
            for (const auto& f : fs::directory_iterator("work\\oracle\\vr_capture\\ramdumps", ec))
                if (f.is_regular_file() && f.path().extension() == ".bin") files.push_back(f.path().string());
        std::sort(files.begin(), files.end(), [](const std::string& a, const std::string& b) {
            const bool ra = a.find("rr-race") != std::string::npos, rb = b.find("rr-race") != std::string::npos;
            return (ra != rb) ? ra : a < b;
        });
        std::vector<CapturedView> out;
        for (const std::string& f : files) {
            std::vector<uint8_t> ram;
            std::string error;
            if (!rr::interp::ReadWholeFile(f, ram, error) || ram.size() < 0x200000u) continue;
            const uint32_t vo = kCamView & 0x1FFFFFu;
            const uint32_t bike = Rd32(ram, kCamView + 0x238);
            if (bike < 0x80000000u || bike >= 0x80200000u) continue;     // not a race image
            if (Rd32(ram, kCamView + 0x304) != 0) continue;                 // a menu / attract state
            CapturedView cv;
            cv.view.assign(ram.begin() + vo, ram.begin() + vo + s::kCameraViewBytes);
            const uint32_t bo = bike & 0x1FFFFFu;
            cv.bike.assign(ram.begin() + bo, ram.begin() + bo + kCamBikeStride);
            cv.dt = static_cast<int32_t>(Rd32(ram, Rd32(ram, kGameStatePtr) + 28u));
            out.push_back(std::move(cv));
        }
        return out;
    }();
    return views;
}

uint32_t W(const std::vector<uint8_t>& b, uint32_t o) {
    return static_cast<uint32_t>(b[o]) | (static_cast<uint32_t>(b[o + 1]) << 8) |
           (static_cast<uint32_t>(b[o + 2]) << 16) | (static_cast<uint32_t>(b[o + 3]) << 24);
}
uint32_t Pick(Rng& r, uint32_t n) { return n == 0 ? 0u : r.U32() % n; }
bool Chance(Rng& r, uint32_t n) { return r.Next() % n == 0; }
// A 16.16 value within +-units.
int32_t Fx(Rng& r, int32_t units) {
    return static_cast<int32_t>(r.U32() % (2u * static_cast<uint32_t>(units) << 16)) - (units << 16);
}
int32_t AngleArg(Rng& r) { return static_cast<int32_t>(r.U32() % 12288u) - 4096; }
uint32_t Bits(Rng& r, uint32_t mask, uint32_t oneIn) {
    uint32_t v = 0;
    for (uint32_t b = 0; b < 32; ++b)
        if ((mask >> b) & 1u)
            if (Chance(r, oneIn)) v |= 1u << b;
    return v;
}
void Put16(Memory& m, uint32_t a, uint32_t v) { WriteU16Bench(m, a, static_cast<uint16_t>(v)); }
uint32_t Bike(const Memory& m, uint32_t slot) { return m.PeekWord(kPool0BasePtr) + kCamBikeStride * slot; }
int32_t CaseDt(CaseContext& c) {
    Rng& r = *c.rng;
    const std::vector<CapturedView>& vs = Views();
    if (!vs.empty() && !Chance(r, 4)) return vs[Pick(r, static_cast<uint32_t>(vs.size()))].dt;
    return static_cast<int32_t>(r.U32() % 0x3000u);
}

// A random unit-ish row triple (4096 = 1.0) - the aim frame is a rotation in the game, noise here.
void RandomMatrix(Memory& m, uint32_t a, Rng& r) {
    for (uint32_t i = 0; i < 9; ++i) PutS16(m, a + 2u * i, r.Unit());
}

// The camera block of a captured view, planted into this machine's view record `v` following `bike`.
void PlantCaptured(Memory& m, uint32_t v, uint32_t bike, const CapturedView& cv) {
    for (uint32_t o = 0x21C; o < s::kCameraViewBytes; o += 4)
        if (o != 0x238 && o != 0x2F0 && o != 0x2F4) m.PokeWord(v + o, W(cv.view, o));
    for (uint32_t f : {0xB8u, 0x22Cu, 0x23Cu})
        for (uint32_t k = 0; k < 3; ++k) {
            const uint32_t rel = W(cv.view, f + 4 * k) - W(cv.bike, 0xB8 + 4 * k);
            m.PokeWord(v + f + 4 * k, m.PeekWord(bike + 0xB8 + 4 * k) + rel);
        }
    m.WriteBlock(v + 0x1B0, cv.view.data() + 0x1B0, 18);             // the render frame
    for (uint32_t o : {0x13Cu, 0x1C8u, 0x1E4u}) m.PokeWord(v + o, W(cv.view, o));
}

// The camera block drawn at random. `bike` is the followed bike (already stored at +0x238).
void PlantRandom(Memory& m, uint32_t v, uint32_t bike, Rng& r, bool director) {
    const uint32_t f224 = Bits(r, 0x3F7FF17Fu, 4) | (Chance(r, 2) ? 0x800120u : 0u);
    m.PokeWord(v + 0x224, f224);
    m.PokeWord(v + 0x228, Bits(r, 0x7FFu, 5) | (Chance(r, 2) ? 0x20u : 0u));
    uint32_t mode;
    if (director) {
        static const uint32_t kDir[] = {0, 1, 2, 3, 7, 8, 9, 10, 11, 12, 12, 13, 13, 14, 14};
        mode = kDir[Pick(r, 15)];
    } else {
        static const uint32_t kChase[] = {0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 5, 6, 13, 14, 7, 12};
        mode = kChase[Pick(r, 16)];
    }
    m.PokeWord(v + 0x21C, mode);
    m.PokeWord(v + 0x220, Pick(r, 4));
    m.PokeWord(v + 0x304, director ? 1u : 0u);
    m.PokeWord(v + 0x308, Pick(r, 9));                                 // named bound: a real shot (9 in the table)
    m.PokeWord(v + 0x30C, Pick(r, 8));
    m.PokeWord(v + 0x310, 0x7FFF0000u);                              // named bound, see the header
    // placements: current, target, transition start, and their second halves
    for (uint32_t o = 0x270; o < 0x2B8; o += 4) m.PokeWord(v + o, static_cast<uint32_t>(Fx(r, 12)));
    if (Chance(r, 3)) m.PokeWord(v + 0x270, static_cast<uint32_t>(Fx(r, 4)));   // radians
    m.PokeWord(v + 0x2B8, static_cast<uint32_t>(r.U32() % 0x30000u));
    m.PokeWord(v + 0x45C, Chance(r, 16) ? 0u : 0x4000u + r.U32() % 0x30000u);
    m.PokeWord(v + 0x2BC, Chance(r, 16) ? 0x27100000u + r.U32() % 0x100000u : r.U32() % 0x100000u);
    // springs and followers
    m.PokeWord(v + 0x2C4, static_cast<uint32_t>(Fx(r, 4)));
    m.PokeWord(v + 0x2C8, static_cast<uint32_t>(Fx(r, 4)));
    m.PokeWord(v + 0x2D0, static_cast<uint32_t>(Fx(r, 4)));
    m.PokeWord(v + 0x2D4, m.PeekWord(bike + 0x1E0) + static_cast<uint32_t>(Fx(r, 8)));
    m.PokeWord(v + 0x2DC, static_cast<uint32_t>(Fx(r, 3)));
    m.PokeWord(v + 0x2E0, static_cast<uint32_t>(Fx(r, 2)));
    m.PokeWord(v + 0x2E4, static_cast<uint32_t>(Fx(r, 3)));
    // angles and rates
    m.PokeWord(v + 0x248, r.U32() % 0x14000u);
    m.PokeWord(v + 0x260, static_cast<uint32_t>(AngleArg(r)));
    m.PokeWord(v + 0x264, static_cast<uint32_t>(static_cast<int32_t>(r.U32() % 1024u) - 512));
    m.PokeWord(v + 0x268, static_cast<uint32_t>(Fx(r, 64)));
    m.PokeWord(v + 0x26C, static_cast<uint32_t>(Fx(r, 64)));
    m.PokeWord(v + 0x2E8, static_cast<uint32_t>(AngleArg(r)));
    m.PokeWord(v + 0x2EC, static_cast<uint32_t>(static_cast<int32_t>(r.U32() % 1024u) - 512));
    m.PokeWord(v + 0x2F8, static_cast<uint32_t>(Fx(r, 4)));
    m.PokeWord(v + 0x13C, r.U32() % 0x14000u);
    m.PokeWord(v + 0x1E4, static_cast<uint32_t>(static_cast<int32_t>(r.U32() % 2048u) - 1024));
    m.PokeWord(v + 0x1C8, static_cast<uint32_t>(Fx(r, 16)));
    m.PokeWord(v + 0x460, Chance(r, 3) ? 0u : r.U32() % 0x10000u);
    m.PokeWord(v + 0x464, Chance(r, 3) ? 0u : static_cast<uint32_t>(Fx(r, 6)));
    m.PokeWord(v + 0x468, Chance(r, 3) ? 0u : 0x40000u + r.U32() % 0x40000u);
    if (director) {                                                  // the race types the director tests
        static const uint8_t kTypes[] = {34, 35, 44, 45};
        const uint32_t gs = m.PeekWord(kGameStatePtr);
        const uint8_t type = kTypes[Pick(r, 4)], stage = static_cast<uint8_t>(Pick(r, 6));
        m.WriteBlock(gs + 4u, &type, 1);
        m.WriteBlock(gs + 57u, &stage, 1);
        if (Chance(r, 2)) Put16(m, bike + 0x172, Pick(r, 5));        // the shot thresholds are 0 and 3
        // named bound: race type 44's shot 4 re-targets to the rider of the player bike's +0x358
        // (0x80088478); rr-race's player has none, so one is lent (bike slot 1) - measured: without it
        // the original chases a rider pointer read from low RAM and traps (0x80087528).
        if (type == 44) {
            const uint32_t pb = m.PeekWord(s::kCameraPlayer1Bike);
            if (m.PeekWord(pb + 0x358) == 0) m.PokeWord(pb + 0x358, Bike(m, 1));
        }
    }
    // pointers: a point to look at (the bike's box centre, the fixed point the director uses, none)
    const uint32_t pick = Pick(r, 3);
    m.PokeWord(v + 0x2F0, pick == 0 ? 0u : (pick == 1 ? bike + 0xB8u : 0x800CE5A8u));
    m.PokeWord(v + 0x2F4, Chance(r, 2) ? 0u : (Chance(r, 2) ? bike + 0x140u : bike + 0x142u));
    // the scripted tracks: 0..6 knots, increasing times, positive segment durations
    const uint32_t n = Pick(r, 7);
    m.PokeWord(v + 0x320, n);
    m.PokeWord(v + 0x31C, n ? Pick(r, n) : 0u);
    m.PokeWord(v + 0x314, r.U32() % 0x40000u);
    m.PokeWord(v + 0x318, Chance(r, 2) ? 0u : r.U32() % 0x200000u);
    for (uint32_t k = 0; k < 6; ++k) m.PokeWord(v + 0x324 + 4 * k, 0x4000u + r.U32() % 0x40000u);
    for (uint32_t t = 0; t < 6; ++t) {
        int32_t x = (t == 0) ? -0x30000 + static_cast<int32_t>(r.U32() % 0x10000u) : 0;
        for (uint32_t k = 0; k < 6; ++k) {
            if (t == 0) x += 0x4000 + static_cast<int32_t>(r.U32() % 0x18000u);
            else x = Fx(r, 12);
            m.PokeWord(v + 0x33C + 24 * t + 4 * k, static_cast<uint32_t>(x));
            m.PokeWord(v + 0x3CC + 24 * t + 4 * k, static_cast<uint32_t>(Fx(r, 4)));
        }
    }
    // the frames and the points
    RandomMatrix(m, v + 0x24C, r);
    if (Chance(r, 2)) RandomMatrix(m, v + 0x1B0, r);
    for (uint32_t f : {0xB8u, 0x22Cu, 0x23Cu})
        for (uint32_t k = 0; k < 3; ++k)
            m.PokeWord(v + f + 4 * k, m.PeekWord(bike + 0xB8 + 4 * k) + static_cast<uint32_t>(Fx(r, 12)));
    if (Chance(r, 8)) m.PokeWord(v + 0xB8, m.PeekWord(v + 0xB8) + (100u << 16));   // far from the rider
    // named bound: a director pan (mode 12) always has its shot point +0x2F0 - the director sets it
    // whenever it selects one (0x80088488, 0x800884B4, 0x8008879C); without it the pan reads the length
    // of a vector its frame never wrote (0x800891B4).
    if (director && mode == 12u && m.PeekWord(v + 0x2F0) == 0) m.PokeWord(v + 0x2F0, bike + 0xB8u);
    // named bound: +0x228 bit 1 ("advance to the next shot") only on a shot that has a successor - the
    // cut adds 1 to +0x308 unconditionally, and shot 9 would be read from beyond the table.
    if ((m.PeekWord(v + 0x228) & 2u) && m.PeekWord(v + 0x308) >= 8u) m.PokeWord(v + 0x308, Pick(r, 8));
    // named bound: +0x224 bit 28 ("the lead is on") is only ever set by CameraAim together with a
    // non-zero lead factor +0x460; without one the aim reads a stale stack slot.
    if ((f224 & 0x10000000u) && m.PeekWord(v + 0x460) == 0) m.PokeWord(v + 0x460, 0xCCCCu);
}

// The followed bike's and rider's fields the camera reads, drawn (randomised family only).
void PerturbBike(Memory& m, uint32_t bike, Rng& r) {
    const uint32_t rider = m.PeekWord(bike + 0x354);
    if (Chance(r, 2)) m.PokeWord(bike + 0x184, (m.PeekWord(bike + 0x184) & ~1u) | Pick(r, 2));
    if (Chance(r, 2)) m.PokeWord(bike + 0x1E0, static_cast<uint32_t>(Fx(r, 70)));
    if (Chance(r, 3)) m.PokeWord(bike + 0x1E4, static_cast<uint32_t>(Fx(r, 8)));
    m.PokeWord(bike + 0x234, (m.PeekWord(bike + 0x234) & ~0xB800u) | Bits(r, 0xB800u, 6));
    m.PokeWord(bike + 0x238, (m.PeekWord(bike + 0x238) & ~0x71Fu) | Bits(r, 0x71Fu, 8));
    if (Chance(r, 3)) m.PokeWord(bike + 0x2A4, static_cast<uint32_t>(Fx(r, 2)));
    if (Chance(r, 4)) m.PokeWord(bike + 0xB4, Pick(r, 20));
    if (rider >= 0x80000000u && rider < 0x80200000u) {
        m.PokeWord(rider + 0x228, (m.PeekWord(rider + 0x228) & ~0x41081818u) | Bits(r, 0x41081818u, 6));
        if (Chance(r, 3)) m.PokeWord(rider + 0x25C, Pick(r, 6));
        if (Chance(r, 3)) {
            static const uint32_t kKinds[] = {0, 1, 40, 69, 70, 72, 73, 100, 200};
            Put16(m, rider + 0x220, kKinds[Pick(r, 9)]);
        }
    }
}

// ---------------------------------------------------------------------------- the port's side
struct CamTables {
    std::vector<int16_t> sincos;
    std::vector<uint16_t> asin;
    int32_t atan[20]{};
    std::vector<uint16_t> rsqrt;
    std::vector<int16_t> sqrt;
    s::BikeTables t;
    CamTables(const Memory& m, uint32_t gp) {
        sincos.assign(8192, 0);
        m.ReadBlock(kSinCosTable, sincos.data(), sincos.size() * sizeof(int16_t));
        asin.assign(kAsinEntries, 0);
        m.ReadBlock(kAsinTable, asin.data(), asin.size() * sizeof(uint16_t));
        for (uint32_t i = 0; i < 20; ++i) atan[i] = GetS32(m, kAtanTable + 4u * i);
        rsqrt.assign(2048, 0);
        m.ReadBlock(m.PeekWord(gp + kRsqrtTableGpOffset), rsqrt.data(), rsqrt.size() * sizeof(uint16_t));
        sqrt.assign(kSqrtWindowHalf, 0);
        m.ReadBlock(kSqrtTable - kSqrtWindowHalf / 2u, sqrt.data(), sqrt.size() * 2u);
        t.sincos = sincos.data();
        t.asin = asin.data();
        t.atan = atan;
        t.rsqrt = rsqrt.data();
        t.sqrt = sqrt.data() + kSqrtWindowHalf / 4u;
    }
};

// The four unported callees, executed by the oracle on the clone.
struct CamOracleSeams final : s::CameraSeams {
    NativeEnv& env;
    explicit CamOracleSeams(NativeEnv& e) : env(e) {}
    bool GetRCnt(uint32_t id, uint32_t& value) override {
        value = env.CallOracle(kCamGetRCnt, id, 0, 0, 0);
        return env.failure.empty();
    }
    bool ShotSetup(s::GuestRam&, uint32_t v, uint32_t script) override {
        env.CallOracle(kCamShotSetup, v, script, 0, 0);
        return env.failure.empty();
    }
    bool SplineSlopes(s::GuestRam&, uint32_t x, uint32_t y, uint32_t out, int32_t n) override {
        env.CallOracle(kCamSplineSlopes, x, y, out, static_cast<uint32_t>(n));
        return env.failure.empty();
    }
    bool RaceOverSignal(uint32_t a0) override {
        env.CallOracle(kCamRaceOver, a0, 0, 0, 0);
        return env.failure.empty();
    }
};

// Runs `f(port)` on the clone's own RAM and scratchpad; a refusal of the port fails the case.
template <class F>
uint32_t Run(NativeEnv& env, F&& f) {
    Memory& m = env.clone->mem;
    s::GuestRam g(m.ram().data(), env.gp);
    g.SetScratchpad(m.scratchpad().data());
    const CamTables tabs(m, env.gp);
    CamOracleSeams seams(env);
    s::RoadRuntimeNative road;
    s::CameraPort port(g, tabs.t, seams, road);
    f(port);
    if (port.Failed() && env.failure.empty()) {
        char buf[160];
        std::snprintf(buf, sizeof(buf), "the port refused: %s at 0x%08X (view fault %d at 0x%08X)",
                      port.FailReason(), port.FailAddress(), g.Faulted() ? 1 : 0, g.FaultAddress());
        env.failure = buf;
    }
    return 0;
}

Row CamRow(const char* name, const char* sig, uint32_t entry, std::function<void(CaseContext&, Args&)> setup,
           std::function<uint32_t(NativeEnv&, const Args&)> native, std::vector<OracleCallee> callees = {},
           uint32_t frame = 0) {
    Row r{name, sig, entry, std::move(setup), nullptr, false, std::move(native), std::move(callees)};
    r.oracleFrame = frame;
    return r;
}

// The shared setup: view record 0 follows the player's bike (or, in the randomised family, a
// quarter of the time another live bike), with a captured or drawn camera block.
uint32_t PlantView(CaseContext& c, bool allowDirector) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    const uint32_t v = kCamView;
    uint32_t bike = m.PeekWord(v + 0x238);
    if (!c.sample && Chance(r, 4)) bike = Bike(m, Pick(r, kCamLiveBikes));
    m.PokeWord(v + 0x238, bike);
    const std::vector<CapturedView>& vs = Views();
    if (c.sample && !vs.empty()) {
        PlantCaptured(m, v, bike, vs[Pick(r, static_cast<uint32_t>(vs.size()))]);
    } else {
        PlantRandom(m, v, bike, r, allowDirector && Chance(r, 4));
        PerturbBike(m, bike, r);
    }
    if (c.rootCounter != nullptr) *c.rootCounter = r.U32() & 0xFFFFu;
    return v;
}

} // namespace camrows

void AddCameraRows(std::vector<Row>& rows) {
    using namespace camrows;

    // ------------------------------------------------------------------ RASHCDG 0x80086AF8
    rows.push_back(CamRow(
        "camera_spring_reset", "void CameraSpringReset(View *v)  [RASHCDG, +0x2C4/+0x2D0/+0x2DC/+0x2E0 = 0, +0x2D4 = speed]",
        kCamSpringReset,
        [](CaseContext& c, Args& args) { args.a[0] = PlantView(c, false); },
        [](NativeEnv& env, const Args& a) { return Run(env, [&](s::CameraPort& p) { p.SpringReset(a.a[0]); }); }));

    // ------------------------------------------------------------------ RASHCDG 0x80086B1C
    rows.push_back(CamRow(
        "camera_chase_spring", "void CameraChaseSpring(View *v, s32 dt, s32 x)  [RASHCDG, the +0x2DC spring]",
        kCamChaseSpring,
        [](CaseContext& c, Args& args) {
            args.a[0] = PlantView(c, false);
            Rng& r = *c.rng;
            if (!c.sample) c.mem->PokeWord(kCamView + 0x21C, Pick(r, 4));
            args.a[1] = static_cast<uint32_t>(CaseDt(c));
            const uint32_t pick = Pick(r, 3);
            args.a[2] = pick == 0 ? c.mem->PeekWord(kCamView + 0x2DC)
                                  : (pick == 1 ? c.mem->PeekWord(kCamView + 0x2E4) : static_cast<uint32_t>(Fx(r, 6)));
        },
        [](NativeEnv& env, const Args& a) {
            return Run(env, [&](s::CameraPort& p) {
                p.ChaseSpring(a.a[0], static_cast<int32_t>(a.a[1]), static_cast<int32_t>(a.a[2]));
            });
        }));

    // ------------------------------------------------------------------ RASHCDG 0x8008AAB0
    rows.push_back(CamRow(
        "camera_reset_flags", "void CameraResetFlags(View *v)  [RASHCDG, +0x224]", kCamResetFlags,
        [](CaseContext& c, Args& args) {
            args.a[0] = PlantView(c, false);
            if (!c.sample) c.mem->PokeWord(kCamView + 0x224, c.rng->U32());
        },
        [](NativeEnv& env, const Args& a) { return Run(env, [&](s::CameraPort& p) { p.ResetFlags(a.a[0]); }); }));

    // ------------------------------------------------------------------ RASHCDG 0x8008A998
    rows.push_back(CamRow(
        "camera_set_mode", "void CameraSetMode(View *v, u32 mode)  [RASHCDG, 9 callers]", kCamSetMode,
        [](CaseContext& c, Args& args) {
            Memory& m = *c.mem;
            Rng& r = *c.rng;
            args.a[0] = PlantView(c, true);
            if (Chance(r, 3)) m.PokeWord(kCamView + 0x238, m.PeekWord(Bike(m, Pick(r, kCamLiveBikes)) + 0x354)); // a rider
            if (Chance(r, 4)) Put16(m, kCamView + 0xAC, Chance(r, 2) ? 0x9Eu : 0x9Fu);
            if (!c.sample) {
                m.PokeWord(kCamView + 0x21C, Pick(r, 16));
                m.PokeWord(kCamView + 0x220, Pick(r, 5));
            }
            const uint32_t pick = Pick(r, 4);
            args.a[1] = pick == 0 ? m.PeekWord(kCamView + 0x220) : (pick == 1 ? Pick(r, 4) : (pick == 2 ? Pick(r, 16) : r.U32()));
        },
        [](NativeEnv& env, const Args& a) { return Run(env, [&](s::CameraPort& p) { p.SetMode(a.a[0], a.a[1]); }); }));

    // ------------------------------------------------------------------ RASHCDG 0x80086584
    rows.push_back(CamRow(
        "camera_to_frame", "void CameraToFrame(View *v, const s32 o[3], const s32 p[3], s32 out[3])  [RASHCDG]",
        kCamToFrame,
        [](CaseContext& c, Args& args) {
            Memory& m = *c.mem;
            Rng& r = *c.rng;
            args.a[0] = PlantView(c, false);
            const uint32_t pick = Pick(r, 3);
            args.a[1] = kCamView + 0x23C;
            args.a[2] = pick == 0 ? kCamView + 0xB8 : (pick == 1 ? kCamView + 0x22C : c.scratch);
            for (uint32_t k = 0; k < 3; ++k)
                m.PokeWord(c.scratch + 4 * k, Chance(r, 8) ? r.U32() : m.PeekWord(kCamView + 0x23C + 4 * k) + static_cast<uint32_t>(Fx(r, 20)));
            args.a[3] = Chance(r, 2) ? kCamView + 0x270 : kCamView + 0x294;
        },
        [](NativeEnv& env, const Args& a) {
            return Run(env, [&](s::CameraPort& p) { p.ToFrame(a.a[0], a.a[1], a.a[2], a.a[3]); });
        }));

    // ------------------------------------------------------------------ RASHCDG 0x800863EC
    rows.push_back(CamRow(
        "camera_to_polar", "void CameraToPolar(s32 p[3])  [RASHCDG, (x,y,z) -> (radians, y, r)]", kCamToPolar,
        [](CaseContext& c, Args& args) {
            Memory& m = *c.mem;
            Rng& r = *c.rng;
            PlantView(c, false);
            const uint32_t p = Chance(r, 2) ? kCamView + 0x270 : c.scratch;
            for (uint32_t k = 0; k < 3; ++k) {
                const uint32_t pick = Pick(r, 5);
                m.PokeWord(p + 4 * k, pick == 0 ? r.U32() : (pick == 1 ? 0u : static_cast<uint32_t>(Fx(r, 1 + static_cast<int32_t>(Pick(r, 30))))));
            }
            args.a[0] = p;
        },
        [](NativeEnv& env, const Args& a) { return Run(env, [&](s::CameraPort& p) { p.ToPolar(a.a[0], env.sp); }); }));

    // ------------------------------------------------------------------ RASHCDG 0x80086C00
    rows.push_back(CamRow(
        "camera_angle_chase", "void CameraAngleChase(s32 *angle, s32 target, s32 dt, s32 rate)  [RASHCDG]",
        kCamAngleChase,
        [](CaseContext& c, Args& args) {
            Memory& m = *c.mem;
            Rng& r = *c.rng;
            PlantView(c, false);
            m.PokeWord(kCamView + 0x260, static_cast<uint32_t>(Chance(r, 8) ? r.S32() : AngleArg(r)));
            args.a[0] = kCamView + 0x260;
            args.a[1] = static_cast<uint32_t>(Chance(r, 5) ? m.PeekWord(kCamView + 0x260) + Pick(r, 5) - 2u
                                                           : static_cast<uint32_t>(AngleArg(r)));
            args.a[2] = static_cast<uint32_t>(CaseDt(c));
            const uint32_t pick = Pick(r, 4);
            args.a[3] = pick == 0 ? 0u : (pick == 1 ? m.PeekWord(s::kCameraFile + 56u * Pick(r, 4)) : static_cast<uint32_t>(Fx(r, 40)));
        },
        [](NativeEnv& env, const Args& a) {
            return Run(env, [&](s::CameraPort& p) {
                p.AngleChase(a.a[0], static_cast<int32_t>(a.a[1]), static_cast<int32_t>(a.a[2]), static_cast<int32_t>(a.a[3]));
            });
        }));

    // ------------------------------------------------------------------ RASHCDG 0x80086D54
    rows.push_back(CamRow(
        "camera_angle_spring", "void CameraAngleSpring(s32 *a, s32 target, s32 dt, s32 *rate, s32 k, s32 c)  [RASHCDG]",
        kCamAngleSpring,
        [](CaseContext& c, Args& args) {
            Memory& m = *c.mem;
            Rng& r = *c.rng;
            PlantView(c, false);
            const bool yaw = Chance(r, 2);
            args.a[0] = kCamView + (yaw ? 0x260u : 0x264u);
            args.a[3] = kCamView + (yaw ? 0x26Cu : 0x268u);
            if (!c.sample) {
                m.PokeWord(args.a[0], static_cast<uint32_t>(Chance(r, 8) ? r.S32() : AngleArg(r)));
                m.PokeWord(args.a[3], static_cast<uint32_t>(Fx(r, 200)));
            }
            args.a[1] = static_cast<uint32_t>(AngleArg(r));
            args.a[2] = static_cast<uint32_t>(CaseDt(c));
            args.a4 = Chance(r, 2) ? 0x186A7u : static_cast<uint32_t>(Fx(r, 4));
            args.more[0] = Chance(r, 2) ? 30015u : static_cast<uint32_t>(Fx(r, 2));
        },
        [](NativeEnv& env, const Args& a) {
            return Run(env, [&](s::CameraPort& p) {
                p.AngleSpring(a.a[0], static_cast<int32_t>(a.a[1]), static_cast<int32_t>(a.a[2]), a.a[3],
                              static_cast<int32_t>(a.a4), static_cast<int32_t>(a.more[0]));
            });
        }));

    // ------------------------------------------------------------------ SLUS 0x8002FA28
    rows.push_back(CamRow(
        "camera_hermite", "void Hermite(s32 t, s32 *h00, s32 *h01, s32 *h10, s32 *h11)  [SLUS]", kCamHermite,
        [](CaseContext& c, Args& args) {
            Rng& r = *c.rng;
            const uint32_t pick = Pick(r, 4);
            args.a[0] = pick == 0 ? r.U32() : (pick == 1 ? (Chance(r, 2) ? 0u : 0x10000u) : r.U32() % 0x10001u);
            args.a[1] = c.scratch + 0;
            args.a[2] = c.scratch + 4;
            args.a[3] = c.scratch + 8;
            args.a4 = Chance(r, 8) ? c.scratch + 8 : c.scratch + 12;    // h11 aliasing h10 once in eight
            for (uint32_t k = 0; k < 16; k += 4) c.mem->PokeWord(c.scratch + k, r.U32());
        },
        [](NativeEnv& env, const Args& a) {
            return Run(env, [&](s::CameraPort& p) { p.Hermite(static_cast<int32_t>(a.a[0]), a.a[1], a.a[2], a.a[3], a.a4); });
        }));

    // ------------------------------------------------------------------ RASHCDG 0x800871A8
    rows.push_back(CamRow(
        "camera_lead", "void CameraLead(const s32 a[3], const s32 b[3], s32 k, View *v, s32 out[3])  [RASHCDG]",
        kCamLead,
        [](CaseContext& c, Args& args) {
            Memory& m = *c.mem;
            Rng& r = *c.rng;
            PlantView(c, false);
            const uint32_t bike = m.PeekWord(kCamView + 0x238);
            const uint32_t rider = m.PeekWord(bike + 0x354);
            const bool onRider = Chance(r, 2);
            args.a[0] = (onRider ? rider : bike) + 0xB8u;
            args.a[1] = (onRider ? rider : bike) + 0x1F8u;
            if (Chance(r, 4)) {                                     // far apart, or on top of each other
                args.a[0] = c.scratch;
                for (uint32_t k = 0; k < 3; ++k)
                    m.PokeWord(c.scratch + 4 * k, m.PeekWord(args.a[1] + 4 * k) +
                                                      (Chance(r, 2) ? r.U32() : static_cast<uint32_t>(Fx(r, 1))));
            }
            args.a[2] = Chance(r, 4) ? r.U32() : r.U32() % 0x10000u;
            args.a[3] = kCamView;
            if (Chance(r, 3)) m.PokeWord(kCamView + 0x228, m.PeekWord(kCamView + 0x228) ^ 0x400u);
            if (Chance(r, 3)) m.PokeWord(kCamView + 0x24, m.PeekWord(kCamView + 0x24) ^ 0x100u);
            args.a4 = c.scratch + 64;
        },
        [](NativeEnv& env, const Args& a) {
            return Run(env, [&](s::CameraPort& p) {
                p.Lead(a.a[0], a.a[1], static_cast<int32_t>(a.a[2]), a.a[3], a.a4, env.sp);
            });
        }));

    // ------------------------------------------------------------------ RASHCDG 0x80088140
    rows.push_back(CamRow(
        "camera_zones", "void CameraZones(View *v)  [RASHCDG, SLUS 0x8003A9D8 with +0x158 mirrored]", kCamZones,
        [](CaseContext& c, Args& args) {
            Memory& m = *c.mem;
            Rng& r = *c.rng;
            args.a[0] = PlantView(c, false);
            if (Chance(r, 2)) m.PokeWord(kCamView + 0x24, m.PeekWord(kCamView + 0x24) ^ (Bits(r, 0x3FEu, 3)));
            if (Chance(r, 2)) m.PokeWord(kCamView + 0x158, static_cast<uint32_t>(Fx(r, 12)));
        },
        [](NativeEnv& env, const Args& a) { return Run(env, [&](s::CameraPort& p) { p.Zones(a.a[0], env.sp); }); }));

    // ------------------------------------------------------------------ RASHCDG 0x8008676C
    rows.push_back(CamRow(
        "camera_transition", "void CameraTransition(View *v, s32 dt)  [RASHCDG, the eased cut]", kCamTransition,
        [](CaseContext& c, Args& args) {
            Memory& m = *c.mem;
            Rng& r = *c.rng;
            args.a[0] = PlantView(c, false);
            const uint32_t f = m.PeekWord(kCamView + 0x224);
            m.PokeWord(kCamView + 0x224, (f & ~0x58u) | Bits(r, 0x58u, 2));
            if (c.sample) {                                          // a transition under way
                m.PokeWord(kCamView + 0x45C, 0x8000u + r.U32() % 0x20000u);
                m.PokeWord(kCamView + 0x2B8, r.U32() % 0x28000u);
            }
            args.a[1] = static_cast<uint32_t>(CaseDt(c));
        },
        [](NativeEnv& env, const Args& a) {
            return Run(env, [&](s::CameraPort& p) { p.Transition(a.a[0], static_cast<int32_t>(a.a[1]), env.sp); });
        }));

    // ------------------------------------------------------------------ RASHCDG 0x80086E1C
    rows.push_back(CamRow(
        "camera_orient", "void CameraOrient(View *v)  [RASHCDG, the render frame +0x1B0]", kCamOrient,
        [](CaseContext& c, Args& args) {
            Memory& m = *c.mem;
            Rng& r = *c.rng;
            args.a[0] = PlantView(c, false);
            const uint32_t f = m.PeekWord(kCamView + 0x224);
            if (!c.sample || Chance(r, 2)) m.PokeWord(kCamView + 0x224, (f & ~0x700Cu) | Bits(r, 0x700Cu, 2));
            if (Chance(r, 4)) m.PokeWord(kCamView + 0x304, 1);
        },
        [](NativeEnv& env, const Args& a) { return Run(env, [&](s::CameraPort& p) { p.Orient(a.a[0], env.sp); }); }));

    // ------------------------------------------------------------------ RASHCDG 0x80087420
    rows.push_back(CamRow(
        "camera_aim", "void CameraAim(View *v, s32 dt)  [RASHCDG, look point, aim angles, frame +0x24C]", kCamAim,
        [](CaseContext& c, Args& args) {
            Memory& m = *c.mem;
            Rng& r = *c.rng;
            args.a[0] = PlantView(c, false);
            if (!c.sample && Chance(r, 8))                            // a non-bike target: the rider
                m.PokeWord(kCamView + 0x238, m.PeekWord(m.PeekWord(kCamView + 0x238) + 0x354));
            args.a[1] = static_cast<uint32_t>(CaseDt(c));
        },
        [](NativeEnv& env, const Args& a) {
            return Run(env, [&](s::CameraPort& p) { p.Aim(a.a[0], static_cast<int32_t>(a.a[1]), env.sp); });
        }));

    // ------------------------------------------------------------------ RASHCDG 0x800881B4
    // The seams: all four are called by ViewUpdate itself, so they run at its own frame (208).
    rows.push_back(CamRow(
        "view_update", "void ViewUpdate(View *v, s32 dt)  [RASHCDG 0x800881B4, the per-player camera]",
        kCamViewUpdate,
        [](CaseContext& c, Args& args) {
            args.a[0] = PlantView(c, true);
            args.a[1] = static_cast<uint32_t>(CaseDt(c));
        },
        [](NativeEnv& env, const Args& a) {
            return Run(env, [&](s::CameraPort& p) { p.ViewUpdate(a.a[0], static_cast<int32_t>(a.a[1]), env.sp + 208u); });
        },
        {OracleCallee{kCamGetRCnt, 1, 0, {}, 0}, OracleCallee{kCamShotSetup, 2, 0, {}, 0},
         OracleCallee{kCamSplineSlopes, 4, 0, {}, 0}, OracleCallee{kCamRaceOver, 1, 0, {}, 0}},
        208));
    rows.back().maxSteps = 20'000'000;
}

// The extent each camera row's `--coverage` is measured over (the function's own instructions).
const CoverageRange* CameraCoverage(const char* rowName) {
    struct Named {
        const char* name;
        CoverageRange range;
    };
    static const Named kRanges[] = {
        {"view_update", {camrows::kCamViewUpdate, 0x8008A998}},   {"camera_aim", {camrows::kCamAim, 0x80088140}},
        {"camera_orient", {camrows::kCamOrient, 0x800871A8}},     {"camera_transition", {camrows::kCamTransition, 0x80086AF8}},
        {"camera_lead", {camrows::kCamLead, 0x80087420}},         {"camera_to_frame", {camrows::kCamToFrame, 0x8008676C}},
        {"camera_to_polar", {camrows::kCamToPolar, 0x80086584}},  {"camera_angle_chase", {camrows::kCamAngleChase, 0x80086D54}},
        {"camera_set_mode", {camrows::kCamSetMode, 0x8008AAB0}},  {"camera_chase_spring", {camrows::kCamChaseSpring, 0x80086C00}},
        {"camera_angle_spring", {camrows::kCamAngleSpring, 0x80086E1C}}, {"camera_hermite", {camrows::kCamHermite, 0x8002FAD4}},
        {"camera_zones", {camrows::kCamZones, 0x800881B4}},       {"camera_reset_flags", {camrows::kCamResetFlags, 0x8008AB00}},
        {"camera_spring_reset", {camrows::kCamSpringReset, 0x80086B1C}},
    };
    for (const Named& n : kRanges)
        if (std::strcmp(n.name, rowName) == 0) return &n.range;
    return nullptr;
}
