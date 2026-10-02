// Bench rows for the race HUD. Included by verify_physics.cpp inside its
// anonymous namespace, so everything defined above that point is visible here.
//
// The port is src\game\sim\hud.{h,cpp}: HudFrame RASHCDG 0x8005E848 and every function under it that
// a race outside the cop missions reaches, over road_query.h's guest-address view. One row per
// function, and one for the driver.
//
// THE SEAMS. What the port does not contain is executed by the oracle on the candidate's own clone and
// witnessed: the 3D sound emitter SLUS 0x80017BA0 (the countdown's cues) and SLUS
// 0x80016528, ComputePlace SLUS 0x800138E8 (ported in race.h over a host view; the HUD rows take the
// original), the packet heap's manager SLUS 0x80021C98, the radar strip RASHCDG 0x800C52A0 and the
// six cop-mission elements.
//
// INPUT FAMILIES. Every case runs on THIS machine (rr-race) with its HUD state:
//   * dump-derived: the HUD state of one of the 17 race RAM images (4 savestates, 13 frame dumps; the
//     item records, the three per-player records, the HUD globals, every live bike's HUD-read fields
//     and rider bytes) planted over it. The first image IS rr-race.
//   * randomised: the snapshot's HUD state with every field the functions branch on drawn - the frame
//     counter's parity, the race type (mostly the plain races, one in eight a cop or split type whose
//     elements the oracle runs), the state byte, the countdown, speeds, healths, weapons, contacts,
//     progress, nitro, TKO counts, the flash timers, the slides, the last-drawn values.
// Named bounds, each a range the game's own data stays in:
//   * one player (game_state+0x30 = 1): a second player's bike pointer is 0 in every capture;
//   * the contact count +0x3B2 is 0..6 and its slot ids 0..24 (live bikes are 18);
//   * a string id (rider def +0x26) is 0..180 (GAMESTRG.LOC holds 181);
//   * the weapon byte +0x2E is 0..10 (the art table has 11 weapon rows after kArtWeapon0);
//   * the heap's next-free word is left as the capture has it (the heap manager runs through the seam
//     when it is reached, and no plain frame fills 0xDE000 bytes).

namespace hudrows {

namespace s = rr::sim;

constexpr uint32_t kHudFrameFn = 0x8005E848, kFlashFn = 0x80013E64, kSetArtFullFn = 0x80013AF8,
                   kLinkFn = 0x8005FA68, kSlideFn = 0x8005F834, kSlideItemsFn = 0x8005F9B0,
                   kSignFn = 0x8005FB4C, kArrestFn = 0x8005FE58, kCountdownFn = 0x8005FF84,
                   kSpeedFn = 0x80060178, kOdometerFn = 0x800603E4, kPlaceFn = 0x800606F0,
                   kPlayerPanelFn = 0x80060C10, kTargetPanelFn = 0x8006148C, kNamesFn = 0x80061E50,
                   kRadarFn = 0x80061F6C, kNitroFn = 0x80062368, kSlidesFn = 0x80063408, kTkoFn = 0x8005F030,
                   kMessagesFn = 0x80062D9C, kDrawTextFn = 0x8002CD78, kGlyphFn = 0x8002D1A0,
                   kWidthFn = 0x8002D0D8, kRouteOffFn = 0x80095410, kRoadObjectFn = 0x8003B9A8,
                   kExitsFn = 0x8003BC48, kMaskFn = 0x8003C590, kRivalFn = 0x8008B84C, kDrawModeFn = 0x8004CE14;
// The seams.
constexpr uint32_t kEmitter = 0x80017BA0, kStopVoice = 0x80016528, kPlace = 0x800138E8, kHeapMgr = 0x80021C98,
                   kRadarStrip = 0x800C52A0, kCopMsgs = 0x80062F34, kCopClock = 0x80062C40, kSuspect = 0x8005FAC4,
                   kArrests = 0x80062610, kCopPanel = 0x80063530, kCopSign = 0x800636F0;

constexpr uint32_t kBike = 0x801B65D4; // the player's bike in every capture (pool 0 slot 0)
constexpr uint32_t kLive = 18;         // rr-race: pool-0 slots 0..17
constexpr uint32_t kRiderRecs = 0x800D5758;

uint32_t Rd32(const std::vector<uint8_t>& ram, uint32_t a) {
    const uint32_t o = a & 0x1FFFFFu;
    if (o + 4u > ram.size()) return 0;
    return static_cast<uint32_t>(ram[o]) | (static_cast<uint32_t>(ram[o + 1]) << 8) |
           (static_cast<uint32_t>(ram[o + 2]) << 16) | (static_cast<uint32_t>(ram[o + 3]) << 24);
}
uint32_t Pick(Rng& r, uint32_t n) { return n == 0 ? 0u : r.U32() % n; }
bool Chance(Rng& r, uint32_t n) { return r.Next() % n == 0; }
void Put8(Memory& m, uint32_t a, uint32_t v) {
    const uint8_t b = static_cast<uint8_t>(v);
    m.WriteBlock(a, &b, 1);
}
void Put16(Memory& m, uint32_t a, uint32_t v) { WriteU16Bench(m, a, static_cast<uint16_t>(v)); }
uint32_t Bike(const Memory& m, uint32_t slot) { return m.PeekWord(s::kHudPool0) + 1096u * slot; }

// ---------------------------------------------------------------------------- captured HUD states
struct Span {
    uint32_t address;
    uint32_t bytes;
};
// Everything the HUD reads or writes that a capture carries, as (address, length) spans: the whole
// per-player HUD state, the globals, and each live bike's HUD-read fields and rider bytes.
std::vector<Span> HudSpans() {
    std::vector<Span> v = {
        {0x8010449Cu, s::kHudItemCount * s::kHudItemBytes},
        {s::kHudState88, 88},  {s::kHudDash224, 224},    {s::kHudSlide72, 72},
        {s::kHudFrameCount, 4}, {s::kHudFinished, 4},     {s::kHudPanelDirty, 4},
        {s::kHudTarget, 4},     {s::kHudOdometer, 4},     {s::kHudFieldSize, 4},
        {s::kHudCountdown, 4},  {s::kHudDemo, 4},         {s::kHudArrestFsm, 4},
        {s::kHudPlayerRecs, 36u * 20u}, {0, 4},
    };
    for (uint32_t i = 0; i < kLive; ++i) {
        const uint32_t e = kBike + 1096u * i;
        v.push_back({e + 324u, 4});   // progress
        v.push_back({e + 360u, 12});  // road position
        v.push_back({e + 450u, 6});   // facing
        v.push_back({e + 480u, 4});   // speed
        v.push_back({e + 564u, 4});   // flags (nitro)
        v.push_back({e + 848u, 1});   // nitro held
        v.push_back({e + 946u, 1});   // contact count
        v.push_back({e + 950u, 48});  // the contact records
        const uint32_t rd = kRiderRecs + 72u * i;
        v.push_back({rd, 72});
    }
    return v;
}

struct Captured {
    std::vector<std::vector<uint8_t>> blocks;
    uint32_t riderState = 0; // the player's rider +0x25C
};

const std::vector<Captured>& Images() {
    static const std::vector<Captured> images = [] {
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
        const std::vector<Span> spans = HudSpans();
        std::vector<Captured> out;
        for (const std::string& f : files) {
            std::vector<uint8_t> ram;
            std::string error;
            if (!rr::interp::ReadWholeFile(f, ram, error) || ram.size() < 0x200000u) continue;
            if (Rd32(ram, s::kHudItemsPtr) != 0x8010449Cu) continue;   // not a race image
            if (Rd32(ram, s::kHudPlayerBikes) != kBike) continue;
            Captured cap;
            for (const Span& sp : spans) {
                const uint32_t o = sp.address & 0x1FFFFFu;
                cap.blocks.emplace_back(ram.begin() + o, ram.begin() + o + sp.bytes);
            }
            cap.riderState = Rd32(ram, Rd32(ram, kBike + 852u) + 604u);
            out.push_back(std::move(cap));
        }
        return out;
    }();
    return images;
}

void PlantCaptured(Memory& m, const Captured& cap) {
    const std::vector<Span> spans = HudSpans();
    for (size_t i = 0; i < spans.size() && i < cap.blocks.size(); ++i)
        m.WriteBlock(spans[i].address, cap.blocks[i].data(), spans[i].bytes);
    m.PokeWord(m.PeekWord(kBike + 852u) + 604u, cap.riderState);
}

// A road position one junction away: another bike's road key, pointed at its START node (dir < 0)
// with a small along, so the turn arrows' junction test (the end node within the arrow range, the
// route naming it, exactly two exits) is reached rather than skipped.
struct JunctionPair {
    int32_t node, road;
};
const std::vector<JunctionPair>& JunctionPairs(Memory& m, uint32_t gp);

void NearJunction(Memory& m, uint32_t e, Rng& r) {
    s::GuestRam g(m.ram().data(), 0x8005AC8Cu);
    const uint32_t other = Bike(m, Pick(r, kLive));
    uint32_t key = m.PeekWord(other + 360u);
    int32_t node = -1;
    // Half the time a road that has turn records at a junction of the map, driven toward THAT
    // junction, so the case reaches junctions no bike of the capture is near.
    const std::vector<JunctionPair>& pairs = JunctionPairs(m, 0x8005AC8Cu);
    if (!pairs.empty() && Chance(r, 2)) {
        const JunctionPair& jp = pairs[Pick(r, static_cast<uint32_t>(pairs.size()))];
        key = static_cast<uint32_t>(jp.road);
        node = jp.node;
    }
    m.PokeWord(e + 360u, key);
    const uint32_t near = Pick(r, 200u << 16);
    // Toward the start (dir < 0, along small) or toward the end (along near the road's length,
    // (graph road +4 >> 6) << 16 - road_runtime.h RoadEndNode).
    const uint32_t road = s::GraphRoad(g, key & 0xFFFFu);
    const uint32_t len = (road != 0) ? ((g.U32(road + 4u) >> 6) << 16) : 0u;
    bool toEnd = Chance(r, 2);
    if (node >= 0 && road != 0) {
        const uint32_t startNode = s::GraphNode(g, g.U32(road + 8u));
        toEnd = !(startNode != 0 && g.S32(startNode) == node);
    }
    g.ClearFault();
    m.PokeWord(e + 364u, toEnd ? 1u : 0xFFFFFFFFu);
    m.PokeWord(e + 368u, toEnd ? len - near : near);
    Put16(m, e + 362u, 0);
    if (node >= 0) {
        const uint32_t rd = m.PeekWord(e + 1084u);
        Put8(m, rd, m.PeekWord(rd) & ~0x80u); // no wrong-way test: the arrows are what is wanted
    }
}

// The junction nodes of the resident road map (kind-0 objects with +4 == 1) and, for each, the roads
// with at least one turn record there - found with the ported leaves, so a case names a real pair.
const std::vector<JunctionPair>& JunctionPairs(Memory& m, uint32_t gp) {
    static std::vector<JunctionPair> pairs;
    static bool built = false;
    if (built) return pairs;
    built = true;
    s::GuestRam g(m.ram().data(), gp);
    const uint32_t map = g.U32(0x8005B240u);
    const int32_t count = g.S16(map + 40u);
    for (int32_t i = g.S16(map + 42u); i < count && i < 4096; ++i) {
        const uint32_t p = g.U32(map + 28u) + 32u * static_cast<uint32_t>(i);
        if (g.S16(p + 4u) != 1 || g.S32(p + 28u) != 0) continue;
        const int32_t node = g.S32(p + 16u);
        const uint32_t x = g.U32(p + 12u);
        if (x == 0) continue;
        const uint32_t ipt = s::RoadJunctionIndex(g, g.S32(x));
        if (ipt == 0) continue;
        for (int32_t road = 0; road < 1024; ++road) {
            std::vector<uint8_t> save(16);
            // RoadTurnsFrom writes its list; aim it at a scratch word block we restore at once.
            const uint32_t out = 0x801FF000u;
            g.ReadBlock(out, save.data(), 16);
            if (s::RoadTurnsFrom(g, ipt, road, out, 3) > 0) pairs.push_back({node, road});
            g.WriteBlock(out, save.data(), 16);
        }
    }
    g.ClearFault();
    return pairs;
}

// A flash timer drawn: phases, times near the race clock, a start/delay/length window sometimes.
void RandomTimer(Memory& m, uint32_t t, Rng& r, int32_t clock) {
    m.PokeWord(t + 0, static_cast<uint32_t>(clock - static_cast<int32_t>(Pick(r, 200))));
    m.PokeWord(t + 4, Pick(r, 100));
    m.PokeWord(t + 8, Pick(r, 100));
    m.PokeWord(t + 12, static_cast<uint32_t>(static_cast<int32_t>(Pick(r, 80)) - 60));
    m.PokeWord(t + 16, Pick(r, 2));
    m.PokeWord(t + 20, Chance(r, 3) ? static_cast<uint32_t>(clock - static_cast<int32_t>(Pick(r, 4000))) : 0u);
    m.PokeWord(t + 24, Pick(r, 3000));
    m.PokeWord(t + 28, Pick(r, 3000));
}

// The randomised family: the snapshot's HUD state with every branch input drawn.
void PlantRandom(Memory& m, Rng& r) {
    const uint32_t gs = m.PeekWord(s::kHudGameStatePtr);
    const int32_t clock = static_cast<int32_t>(m.PeekWord(gs + 16u));
    m.PokeWord(s::kHudFrameCount, r.U32());
    // The race type: the plain races mostly, one in eight a type whose elements the oracle runs.
    static const uint8_t kPlain[] = {0x22, 0x22, 0x00, 0x02, 0x20, 0x08, 0x2A, 0x0A, 0x28};
    static const uint8_t kOther[] = {0x01, 0x21, 0x04, 0x24, 44, 33, 36, 0x05};
    Put8(m, gs + 4u, Chance(r, 8) ? kOther[Pick(r, sizeof(kOther))] : kPlain[Pick(r, sizeof(kPlain))]);
    static const uint8_t kStates[] = {1, 1, 1, 1, 1, 1, 6, 3, 4, 2};
    Put8(m, gs, kStates[Pick(r, sizeof(kStates))]);
    Put8(m, gs + 6u, Pick(r, kLive));
    Put8(m, gs + 10u, Chance(r, 2) ? 1u : 0u);
    Put8(m, gs + 57u, Pick(r, 3));
    m.PokeWord(s::kHudDemo, Chance(r, 8) ? (Chance(r, 2) ? 1u : 0xFFFFFFFFu) : 0u);
    m.PokeWord(s::kHudFinished, Chance(r, 6) ? 1u : 0u);
    m.PokeWord(s::kHudPanelDirty, Chance(r, 3) ? 1u : 0u);
    m.PokeWord(s::kHudCountdown, static_cast<uint32_t>(static_cast<int32_t>(r.U32() % (8u << 16)) - (3 << 16)));
    m.PokeWord(s::kHudOdometer, Chance(r, 8) ? r.U32() : r.U32() % 0x1000000u);
    m.PokeWord(s::kHudFieldSize, 1u + Pick(r, 20));
    m.PokeWord(s::kHudArrestFsm, Pick(r, 11));
    m.PokeWord(s::kHudTarget, Chance(r, 4) ? 0u : Bike(m, Pick(r, kLive)));
    for (uint32_t k = 0; k < 36u * 20u; k += 4) {
        if (Chance(r, 3)) m.PokeWord(s::kHudPlayerRecs + k, r.U32() & 0x0F0F0F0Fu);
    }
    Put8(m, s::kHudPlayerRecs + 0x18u + 1u, Chance(r, 3) ? 0u : Pick(r, 26));
    Put8(m, s::kHudNitroBonus, Pick(r, 3));
    // the bikes
    const uint32_t player = kBike;
    for (uint32_t i = 0; i < kLive; ++i) {
        const uint32_t e = Bike(m, i);
        if (Chance(r, 2)) m.PokeWord(e + 324u, m.PeekWord(e + 324u) + static_cast<uint32_t>(static_cast<int32_t>(Pick(r, 0x40000)) - 0x20000));
        Put8(m, e + 946u, Pick(r, 7));
        for (uint32_t k = 0; k < 6; ++k) Put16(m, e + 950u + 8u * k, Pick(r, 25));
        const uint32_t rd = m.PeekWord(e + 1084u);
        if (Chance(r, 2)) {
            Put8(m, rd + 13u, Pick(r, 256));
            Put8(m, rd + 15u, Chance(r, 6) ? 0u : Pick(r, 256));
            Put8(m, rd + 36u, Pick(r, 256));
            Put8(m, rd + 37u, Chance(r, 4) ? 0u : Pick(r, 256));
        }
        Put8(m, rd + 38u, Pick(r, 181));
        Put8(m, rd + 46u, Pick(r, 11));
        Put8(m, rd + 47u, Pick(r, 2));
        if (Chance(r, 8)) {
            static const uint8_t kStatus[] = {254, 255, 248, 252, 253, 1, 16};
            Put8(m, rd + 39u, kStatus[Pick(r, sizeof(kStatus))]);
        }
        if (Chance(r, 10)) m.PokeWord(rd + 40u, 1u + Pick(r, 100000));
    }
    const uint32_t rd = m.PeekWord(player + 1084u);
    Put8(m, rd, (m.PeekWord(rd) & ~0xE0u) | (Pick(r, 8) << 5));
    if (Chance(r, 12)) Put8(m, rd + 1u, (m.PeekWord(rd) >> 8 & 0xF0u) | 2u); // a cop rider
    m.PokeWord(m.PeekWord(player + 852u) + 604u, Pick(r, 6));
    m.PokeWord(player + 480u, static_cast<uint32_t>(static_cast<int32_t>(r.U32() % 0x6000000u) - 0x1000000));
    m.PokeWord(player + 564u, Chance(r, 3) ? 0x200u : 0u);
    Put8(m, player + 848u, Pick(r, 5));
    Put8(m, m.PeekWord(player + 556u) + 445u, Pick(r, 6));
    m.PokeWord(player + 180u, Pick(r, 30));
    if (Chance(r, 5)) m.PokeWord(player + 856u, Bike(m, 1u + Pick(r, kLive - 1u)));
    for (uint32_t k = 0; k < 3; ++k) Put16(m, player + 450u + 2u * k, static_cast<uint32_t>(static_cast<int32_t>(Pick(r, 8193)) - 4096));
    if (Chance(r, 3)) NearJunction(m, player, r);
    // the radar strip: the live flag +0x140 of the bikes, the player's heading pair +0x1BC/+0x1C0, the
    // other bikes' box centres near the player's (so that marks land inside the strip), the scale
    for (uint32_t i = 0; i < kLive; ++i) {
        const uint32_t e = Bike(m, i);
        if (Chance(r, 4)) Put16(m, e + 320u, Chance(r, 2) ? 0u : 1u + Pick(r, 30));
        if (i != 0 && Chance(r, 2))
            for (uint32_t k = 0; k < 3; ++k)
                m.PokeWord(e + 184u + 4u * k, m.PeekWord(player + 184u + 4u * k) +
                                                  static_cast<uint32_t>(static_cast<int32_t>(Pick(r, 400u << 16)) - (200 << 16)));
        if (i != 0 && Chance(r, 8)) m.PokeWord(m.PeekWord(e + 852u) + 604u, Pick(r, 6));
    }
    for (uint32_t k = 0; k < 2; ++k) Put16(m, player + 444u + 4u * k, static_cast<uint32_t>(static_cast<int32_t>(Pick(r, 8193)) - 4096));
    if (Chance(r, 3)) m.PokeWord(s::kHudRadarScale, Chance(r, 4) ? r.U32() : 0x3D50u + Pick(r, 0x4000));
    // the per-player records
    const uint32_t state = s::kHudState88, dash = s::kHudDash224, slides = s::kHudSlide72;
    for (uint32_t k = 0; k < 88; k += 4)
        if (Chance(r, 3)) m.PokeWord(state + k, Chance(r, 2) ? Pick(r, 20) : r.U32() % 200u);
    m.PokeWord(state + 60u, Pick(r, 6));
    m.PokeWord(state + 64u, Chance(r, 3) ? 0u : Bike(m, Pick(r, kLive)));
    m.PokeWord(state + 84u, Pick(r, 4));
    for (uint32_t t = 0; t < 7; ++t)
        if (Chance(r, 2)) RandomTimer(m, dash + 32u * t, r, clock);
    m.PokeWord(dash + 140u, static_cast<uint32_t>(static_cast<int32_t>(Pick(r, 6)) - 2));
    for (uint32_t k = 0; k < 2; ++k) {
        const uint32_t sl = slides + 36u * k;
        const uint32_t st = Pick(r, 4);
        m.PokeWord(sl, st);
        const uint32_t limit = (st == 0) ? m.PeekWord(sl + 8u) : m.PeekWord(sl + 20u);
        m.PokeWord(sl + 4u, limit == 0 ? 0u : Pick(r, limit));
    }
}

void PlantHud(CaseContext& c) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    const std::vector<Captured>& imgs = Images();
    if (c.sample != nullptr && !imgs.empty()) {
        PlantCaptured(m, imgs[Pick(r, static_cast<uint32_t>(imgs.size()))]);
        m.PokeWord(s::kHudFrameCount, r.U32()); // both parities, and every eighth frame
    } else {
        PlantRandom(m, r);
    }
}

// ---------------------------------------------------------------------------- the seams
struct HudOracle final : s::HudCallees {
    NativeEnv& env;
    explicit HudOracle(NativeEnv& e) : env(e) {}
    bool PlaySound(uint32_t, int32_t id) override {
        env.CallOracle(kEmitter, 0, 0, static_cast<uint32_t>(id), 0);
        return env.failure.empty();
    }
    bool StopCountdownVoice(uint32_t) override {
        env.CallOracle(kStopVoice, 0, 0, 0, 0);
        return env.failure.empty();
    }
    bool ComputePlace(uint32_t, uint32_t bike, int32_t mode, int32_t* place) override {
        *place = static_cast<int32_t>(env.CallOracle(kPlace, bike, static_cast<uint32_t>(mode), 0, 0));
        return env.failure.empty();
    }
    bool HeapOverflow(uint32_t, uint32_t next, uint32_t bytes, uint32_t* answer) override {
        *answer = env.CallOracle(kHeapMgr, next, bytes, 0, 0);
        return env.failure.empty();
    }
    bool Unported(uint32_t, uint32_t address, const uint32_t* args, int count) override {
        env.CallOracleArgs(address, args, count);
        return env.failure.empty();
    }
};

// Runs `f(g, callees)` on the clone's RAM; a fault or a refusal fails the case.
template <class F>
uint32_t Run(NativeEnv& env, F&& f) {
    s::GuestRam g(env.clone->mem.ram().data(), env.gp);
    g.SetScratchpad(env.clone->mem.scratchpad().data());
    HudOracle callees(env);
    const uint32_t v = f(g, callees);
    if (g.Faulted() && env.failure.empty()) {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "the port met an address the console faults on: 0x%08X", g.FaultAddress());
        env.failure = buf;
    }
    return v;
}

// The seams an element reaches, at the depth below the row's own frame.
std::vector<OracleCallee> Seams(uint32_t emitterDepth, uint32_t placeDepth, uint32_t heapDepth) {
    return {OracleCallee{kEmitter, 4, 0, {}, emitterDepth}, OracleCallee{kStopVoice, 0, 0, {}, emitterDepth},
            OracleCallee{kPlace, 2, 0, {}, placeDepth},     OracleCallee{kHeapMgr, 2, 0, {}, heapDepth}};
}

Row HudRow(const char* name, const char* sig, uint32_t entry, std::function<void(CaseContext&, Args&)> setup,
           std::function<uint32_t(NativeEnv&, const Args&)> native, bool v0 = false,
           std::vector<OracleCallee> callees = {}, uint32_t frame = 0) {
    Row r{name, sig, entry, std::move(setup), nullptr, v0, std::move(native), std::move(callees)};
    r.oracleFrame = frame;
    r.maxSteps = 20'000'000;
    return r;
}
// The text path calls the BIOS's strlen A(1Bh) through SLUS 0x800448F4; these rows run the kernel.
Row Bios(Row r) {
    r.allowBiosCalls = true;
    return r;
}

// The standard argument set of an element: (ot, items, state, bike) and the stack words.
void ElementArgs(CaseContext& c, Args& a) {
    Memory& m = *c.mem;
    a.a[0] = m.PeekWord(s::kHudOt);
    a.a[1] = m.PeekWord(s::kHudItemsPtr);
    a.a[2] = s::kHudState88;
    a.a[3] = m.PeekWord(s::kHudPlayerBikes);
}

} // namespace hudrows

void AddHudRows(std::vector<Row>& rows) {
    using namespace hudrows;

    // ------------------------------------------------------------------ RASHCDG 0x8005E848
    {
        std::vector<OracleCallee> seams = Seams(40, 56, 48 + 32 + 88);
        // kCopMsgs 0x80062F34: PORTED (modes.h HudArrestMessage, row mode_hud_arrest_msg), native inside HudFrame
        // kCopClock 0x80062C40, kSuspect 0x8005FAC4, kCopPanel 0x80063530, kCopSign 0x800636F0: PORTED
        // (src\game\sim\modes.h, rows mode_hud_* in rows_modes.inc), run natively inside HudFrame
        // kArrests 0x80062610: PORTED (modes.h HudArrests, row mode_hud_arrests), native inside HudFrame
        rows.push_back(Bios(HudRow(
            "hud_frame", "void HudFrame(void)  [RASHCDG 0x8005E848, 2024 bytes - the whole race HUD]", kHudFrameFn,
            [](CaseContext& c, Args&) { PlantHud(c); },
            [](NativeEnv& env, const Args&) {
                return Run(env, [&](s::GuestRam& g, s::HudCallees& k) {
                    if (!s::HudFrame(g, k, env.sp + 88u) && env.failure.empty()) env.failure = "HudFrame refused";
                    return 0u;
                });
            },
            false, seams, 88)));
    }

    // ------------------------------------------------------------------ SLUS 0x80013E64
    rows.push_back(HudRow(
        "hud_flash_timer", "void FlashTimer(Timer *t)  [SLUS 0x80013E64]", kFlashFn,
        [](CaseContext& c, Args& a) {
            PlantHud(c);
            Memory& m = *c.mem;
            Rng& r = *c.rng;
            const uint32_t gs = m.PeekWord(s::kHudGameStatePtr);
            const uint32_t t = s::kHudDash224 + 32u * Pick(r, 7);
            if (!c.sample) RandomTimer(m, t, r, static_cast<int32_t>(m.PeekWord(gs + 16u)));
            if (Chance(r, 6)) Put8(m, gs, 3u + Pick(r, 2));
            a.a[0] = t;
        },
        [](NativeEnv& env, const Args& a) {
            return Run(env, [&](s::GuestRam& g, s::HudCallees&) { s::HudFlashTimer(g, a.a[0]); return 0u; });
        }));

    // ------------------------------------------------------------------ SLUS 0x80013AF8
    rows.push_back(HudRow(
        "hud_set_art_full", "void SetArtFull(Item *it, Art *a)  [SLUS 0x80013AF8]", kSetArtFullFn,
        [](CaseContext& c, Args& a) {
            PlantHud(c);
            Rng& r = *c.rng;
            a.a[0] = c.mem->PeekWord(s::kHudItemsPtr) + s::kHudItemBytes * Pick(r, s::kHudItemCount);
            a.a[1] = s::kHudArtTable + 16u * Pick(r, 62);
        },
        [](NativeEnv& env, const Args& a) {
            return Run(env, [&](s::GuestRam& g, s::HudCallees&) { s::HudSetArtFull(g, a.a[0], a.a[1]); return 0u; });
        }));

    // ------------------------------------------------------------------ RASHCDG 0x8005FA68
    rows.push_back(HudRow(
        "hud_link_range", "void LinkRange(u32 *ot, Item *items, s32 first, s32 last)  [RASHCDG 0x8005FA68]", kLinkFn,
        [](CaseContext& c, Args& a) {
            PlantHud(c);
            Rng& r = *c.rng;
            ElementArgs(c, a);
            const uint32_t first = Pick(r, s::kHudItemCount);
            a.a[2] = first;
            a.a[3] = first + Pick(r, s::kHudItemCount - first);
        },
        [](NativeEnv& env, const Args& a) {
            return Run(env, [&](s::GuestRam& g, s::HudCallees&) {
                s::HudLinkRange(g, a.a[0], a.a[1], static_cast<int32_t>(a.a[2]), static_cast<int32_t>(a.a[3]));
                return 0u;
            });
        }));

    // ------------------------------------------------------------------ SLUS 0x8004CE14
    rows.push_back(HudRow(
        "hud_set_draw_mode", "void SetDrawMode(DR_MODE *p, s32 dfe, s32 dtd, s32 tpage)  [SLUS 0x8004CE14, libgpu]",
        kDrawModeFn,
        [](CaseContext& c, Args& a) {
            Rng& r = *c.rng;
            a.a[0] = Chance(r, 2) ? s::kHudDrawModeHead : c.scratch + 4u * Pick(r, 64);
            a.a[1] = Chance(r, 2) ? Pick(r, 2) : r.U32();
            a.a[2] = Chance(r, 2) ? Pick(r, 2) : r.U32();
            a.a[3] = r.U32();
        },
        [](NativeEnv& env, const Args& a) {
            return Run(env, [&](s::GuestRam& g, s::HudCallees&) { s::HudSetDrawMode(g, a.a[0], a.a[1], a.a[2], a.a[3]); return 0u; });
        }));

    // ------------------------------------------------------------------ RASHCDG 0x8005F9B0
    rows.push_back(HudRow(
        "hud_slide_items", "void SlideItems(Item *items, s16 dy, s32 first, s32 last)  [RASHCDG 0x8005F9B0]",
        kSlideItemsFn,
        [](CaseContext& c, Args& a) {
            PlantHud(c);
            Rng& r = *c.rng;
            a.a[0] = c.mem->PeekWord(s::kHudItemsPtr);
            a.a[1] = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(Chance(r, 4) ? r.U32() : Pick(r, 141) - 70u)));
            const uint32_t first = Pick(r, s::kHudItemCount);
            a.a[2] = first;
            a.a[3] = Chance(r, 8) ? first - 1u : first + Pick(r, 20);
            if (a.a[3] >= s::kHudItemCount) a.a[3] = s::kHudItemCount - 1u;
        },
        [](NativeEnv& env, const Args& a) {
            return Run(env, [&](s::GuestRam& g, s::HudCallees&) {
                s::HudSlideItems(g, a.a[0], static_cast<int16_t>(a.a[1]), static_cast<int32_t>(a.a[2]), static_cast<int32_t>(a.a[3]));
                return 0u;
            });
        }));

    // ------------------------------------------------------------------ RASHCDG 0x8005F834
    rows.push_back(HudRow(
        "hud_slide", "void Slide(Item *items, Slide *s, s32 trigger)  [RASHCDG 0x8005F834]", kSlideFn,
        [](CaseContext& c, Args& a) {
            PlantHud(c);
            Rng& r = *c.rng;
            a.a[0] = c.mem->PeekWord(s::kHudItemsPtr);
            a.a[1] = s::kHudSlide72 + (Chance(r, 2) ? 36u : 0u);
            a.a[2] = Chance(r, 2) ? 0u : (Chance(r, 2) ? 1u : r.U32());
        },
        [](NativeEnv& env, const Args& a) {
            return Run(env, [&](s::GuestRam& g, s::HudCallees&) {
                s::HudSlide(g, a.a[0], a.a[1], static_cast<int32_t>(a.a[2]));
                return 0u;
            });
        }));

    // ------------------------------------------------------------------ RASHCDG 0x8005FB4C
    rows.push_back(HudRow(
        "hud_sign", "void Sign(u32 *ot, Item *it, State *s, Bike *b, u32 mask, Dash *d, s32 p)  [RASHCDG 0x8005FB4C]",
        kSignFn,
        [](CaseContext& c, Args& a) {
            PlantHud(c);
            Rng& r = *c.rng;
            ElementArgs(c, a);
            a.a4 = Chance(r, 2) ? (r.U32() & 0xD3u) : (Chance(r, 2) ? 0x10u : 0x40u);
            a.more[0] = s::kHudDash224;
            a.more[1] = 0;
        },
        [](NativeEnv& env, const Args& a) {
            return Run(env, [&](s::GuestRam& g, s::HudCallees&) {
                s::HudSign(g, a.a[0], a.a[1], a.a[2], a.a[3], a.a4, a.more[0], static_cast<int32_t>(a.more[1]));
                return 0u;
            });
        }));

    // ------------------------------------------------------------------ RASHCDG 0x8005FE58
    rows.push_back(HudRow(
        "hud_arrest_icon", "void ArrestIcon(u32 *ot, Item *it, Dash *d, Bike *b)  [RASHCDG 0x8005FE58]", kArrestFn,
        [](CaseContext& c, Args& a) {
            PlantHud(c);
            ElementArgs(c, a);
            a.a[2] = s::kHudDash224;
        },
        [](NativeEnv& env, const Args& a) {
            return Run(env, [&](s::GuestRam& g, s::HudCallees&) { s::HudArrestIcon(g, a.a[0], a.a[1], a.a[2], a.a[3]); return 0u; });
        }));

    // ------------------------------------------------------------------ RASHCDG 0x8005FF84
    rows.push_back(HudRow(
        "hud_countdown", "void Countdown(u32 *ot, Item *it, State *s, Bike *b)  [RASHCDG 0x8005FF84]", kCountdownFn,
        [](CaseContext& c, Args& a) {
            PlantHud(c);
            ElementArgs(c, a);
        },
        [](NativeEnv& env, const Args& a) {
            return Run(env, [&](s::GuestRam& g, s::HudCallees& k) {
                if (!s::HudCountdown(g, k, env.sp + 40u, a.a[0], a.a[1], a.a[2], a.a[3]) && env.failure.empty())
                    env.failure = "Countdown refused";
                return 0u;
            });
        },
        false, Seams(0, 0, 0), 40));

    // ------------------------------------------------------------------ RASHCDG 0x80060178
    rows.push_back(HudRow(
        "hud_speed", "void Speed(u32 *ot, Item *it, State *s, Bike *b)  [RASHCDG 0x80060178]", kSpeedFn,
        [](CaseContext& c, Args& a) {
            PlantHud(c);
            ElementArgs(c, a);
        },
        [](NativeEnv& env, const Args& a) {
            return Run(env, [&](s::GuestRam& g, s::HudCallees&) { s::HudSpeed(g, a.a[0], a.a[1], a.a[2], a.a[3]); return 0u; });
        }));

    // ------------------------------------------------------------------ RASHCDG 0x800603E4
    rows.push_back(HudRow(
        "hud_odometer", "void Odometer(u32 *ot, Item *it, State *s, s32 p)  [RASHCDG 0x800603E4]", kOdometerFn,
        [](CaseContext& c, Args& a) {
            PlantHud(c);
            ElementArgs(c, a);
            a.a[3] = 0;
        },
        [](NativeEnv& env, const Args& a) {
            return Run(env, [&](s::GuestRam& g, s::HudCallees&) {
                s::HudOdometer(g, a.a[0], a.a[1], a.a[2], static_cast<int32_t>(a.a[3]));
                return 0u;
            });
        }));

    // ------------------------------------------------------------------ RASHCDG 0x800606F0
    rows.push_back(HudRow(
        "hud_place", "void Place(u32 *ot, Item *it, State *s, Bike *b, u32 mask, s32 p, Dash *d)  [RASHCDG 0x800606F0]",
        kPlaceFn,
        [](CaseContext& c, Args& a) {
            PlantHud(c);
            Rng& r = *c.rng;
            ElementArgs(c, a);
            a.a4 = Chance(r, 3) ? 2u : (r.U32() & 0xD3u);
            a.more[0] = 0;
            a.more[1] = s::kHudDash224;
        },
        [](NativeEnv& env, const Args& a) {
            return Run(env, [&](s::GuestRam& g, s::HudCallees& k) {
                if (!s::HudPlace(g, k, env.sp + 56u, a.a[0], a.a[1], a.a[2], a.a[3], a.a4,
                                 static_cast<int32_t>(a.more[0]), a.more[1]) && env.failure.empty())
                    env.failure = "Place refused";
                return 0u;
            });
        },
        false, Seams(0, 0, 0), 56));

    // ------------------------------------------------------------------ RASHCDG 0x80060C10
    rows.push_back(HudRow(
        "hud_player_panel", "void PlayerPanel(u32 *ot, Item *it, State *s, Bike *b, s32 p, Slides *sl, Dash *d)  [RASHCDG 0x80060C10]",
        kPlayerPanelFn,
        [](CaseContext& c, Args& a) {
            PlantHud(c);
            ElementArgs(c, a);
            a.a4 = 0;
            a.more[0] = s::kHudSlide72;
            a.more[1] = s::kHudDash224;
        },
        [](NativeEnv& env, const Args& a) {
            return Run(env, [&](s::GuestRam& g, s::HudCallees&) {
                s::HudPlayerPanel(g, a.a[0], a.a[1], a.a[2], a.a[3], static_cast<int32_t>(a.a4), a.more[0], a.more[1]);
                return 0u;
            });
        }));

    // ------------------------------------------------------------------ RASHCDG 0x8006148C
    rows.push_back(HudRow(
        "hud_target_panel", "s32 TargetPanel(u32 *ot, Item *it, State *s, Bike *b, s32 p, Slides *sl, Dash *d)  [RASHCDG 0x8006148C]",
        kTargetPanelFn,
        [](CaseContext& c, Args& a) {
            PlantHud(c);
            ElementArgs(c, a);
            a.a4 = 0;
            a.more[0] = s::kHudSlide72;
            a.more[1] = s::kHudDash224;
        },
        [](NativeEnv& env, const Args& a) {
            return Run(env, [&](s::GuestRam& g, s::HudCallees&) {
                return static_cast<uint32_t>(s::HudTargetPanel(g, a.a[0], a.a[1], a.a[2], a.a[3], static_cast<int32_t>(a.a4),
                                                               a.more[0], a.more[1]));
            });
        },
        true));

    // ------------------------------------------------------------------ RASHCDG 0x80061E50
    rows.push_back(Bios(HudRow(
        "hud_names", "void Names(u32 *ot, Item *it, State *s, Bike *b, s32 p, Dash *d, Slides *sl)  [RASHCDG 0x80061E50]",
        kNamesFn,
        [](CaseContext& c, Args& a) {
            PlantHud(c);
            ElementArgs(c, a);
            a.a4 = 0;
            a.more[0] = s::kHudDash224;
            a.more[1] = s::kHudSlide72;
        },
        [](NativeEnv& env, const Args& a) {
            return Run(env, [&](s::GuestRam& g, s::HudCallees& k) {
                if (!s::HudNames(g, k, env.sp + 48u, a.a[1], static_cast<int32_t>(a.a4), a.more[1]) && env.failure.empty())
                    env.failure = "Names refused";
                return 0u;
            });
        },
        false, Seams(0, 0, 32 + 88), 48)));

    // ------------------------------------------------------------------ RASHCDG 0x80061F6C
    rows.push_back(HudRow(
        "hud_radar_distance", "void RadarDistance(u32 *ot, Item *it, State *s, Bike *b, s32 p)  [RASHCDG 0x80061F6C]",
        kRadarFn,
        [](CaseContext& c, Args& a) {
            PlantHud(c);
            ElementArgs(c, a);
            a.a4 = 0;
        },
        [](NativeEnv& env, const Args& a) {
            return Run(env, [&](s::GuestRam& g, s::HudCallees&) {
                s::HudRadarDistance(g, a.a[0], a.a[1], a.a[2], a.a[3], static_cast<int32_t>(a.a4));
                return 0u;
            });
        }));

    // ------------------------------------------------------------------ RASHCDG 0x80062368
    rows.push_back(HudRow(
        "hud_nitro", "void Nitro(u32 *ot, Item *it, State *s, Bike *b, Dash *d)  [RASHCDG 0x80062368]", kNitroFn,
        [](CaseContext& c, Args& a) {
            PlantHud(c);
            ElementArgs(c, a);
            a.a4 = s::kHudDash224;
        },
        [](NativeEnv& env, const Args& a) {
            return Run(env, [&](s::GuestRam& g, s::HudCallees&) { s::HudNitro(g, a.a[0], a.a[1], a.a[2], a.a[3], a.a4); return 0u; });
        }));

    // ------------------------------------------------------------------ RASHCDG 0x80063408
    rows.push_back(HudRow(
        "hud_panel_slides", "void PanelSlides(Bike *b, Item *it, Slides *sl, s32 p, s32 changed)  [RASHCDG 0x80063408]",
        kSlidesFn,
        [](CaseContext& c, Args& a) {
            PlantHud(c);
            Rng& r = *c.rng;
            a.a[0] = c.mem->PeekWord(s::kHudPlayerBikes);
            a.a[1] = c.mem->PeekWord(s::kHudItemsPtr);
            a.a[2] = s::kHudSlide72;
            a.a[3] = 0;
            a.a4 = Chance(r, 2) ? 0u : (Chance(r, 2) ? 1u : r.U32());
        },
        [](NativeEnv& env, const Args& a) {
            return Run(env, [&](s::GuestRam& g, s::HudCallees&) {
                s::HudPanelSlides(g, a.a[0], a.a[1], a.a[2], static_cast<int32_t>(a.a[3]), static_cast<int32_t>(a.a4));
                return 0u;
            });
        }));

    // ------------------------------------------------------------------ RASHCDG 0x8005F030
    rows.push_back(HudRow(
        "hud_tko", "void Tko(u32 *ot, Item *it, Dash *d, Bike *b, s32 p, u32 mask)  [RASHCDG 0x8005F030]", kTkoFn,
        [](CaseContext& c, Args& a) {
            PlantHud(c);
            Rng& r = *c.rng;
            ElementArgs(c, a);
            a.a[2] = s::kHudDash224;
            a.a4 = 0;
            a.more[0] = Chance(r, 4) ? (r.U32() & 0x53u) : 0u;
            if (Chance(r, 2)) Put8(*c.mem, s::kHudPlayerRecs + 0x18u + 1u, 1u + Pick(r, 25));
            if (Chance(r, 3)) c.mem->PokeWord(s::kHudDash224 + 140u, 0);
        },
        [](NativeEnv& env, const Args& a) {
            return Run(env, [&](s::GuestRam& g, s::HudCallees& k) {
                if (!s::HudTko(g, k, env.sp + 72u, a.a[0], a.a[1], a.a[2], a.a[3], static_cast<int32_t>(a.a4), a.more[0]) &&
                    env.failure.empty())
                    env.failure = "Tko refused";
                return 0u;
            });
        },
        false, Seams(0, 0, 0), 72));

    // ------------------------------------------------------------------ RASHCDG 0x80062D9C
    rows.push_back(Bios(HudRow(
        "hud_messages", "void Messages(Item *it, Dash *d, Bike *b, s32 p)  [RASHCDG 0x80062D9C]", kMessagesFn,
        [](CaseContext& c, Args& a) {
            PlantHud(c);
            Rng& r = *c.rng;
            a.a[0] = c.mem->PeekWord(s::kHudItemsPtr);
            a.a[1] = s::kHudDash224;
            a.a[2] = c.mem->PeekWord(s::kHudPlayerBikes);
            a.a[3] = 0;
            if (Chance(r, 2)) {
                static const uint8_t kStatus[] = {254, 255, 1};
                Put8(*c.mem, c.mem->PeekWord(a.a[2] + 1084u) + 39u, kStatus[Pick(r, 3)]);
            }
        },
        [](NativeEnv& env, const Args& a) {
            return Run(env, [&](s::GuestRam& g, s::HudCallees& k) {
                if (!s::HudMessages(g, k, env.sp + 56u, a.a[0], a.a[1], a.a[2], static_cast<int32_t>(a.a[3])) &&
                    env.failure.empty())
                    env.failure = "Messages refused";
                return 0u;
            });
        },
        false, Seams(0, 0, 32 + 88), 56)));

    // ------------------------------------------------------------------ SLUS 0x8002CD78
    rows.push_back(Bios(HudRow(
        "hud_draw_text", "void DrawText(s32 font, s32 id, s16 x, s16 y, u32 *ot, u32 colour)  [SLUS 0x8002CD78]",
        kDrawTextFn,
        [](CaseContext& c, Args& a) {
            PlantHud(c);
            Rng& r = *c.rng;
            a.a[0] = Chance(r, 8) ? 0xFFFFFFFFu : 1u;
            a.a[1] = Pick(r, 181);
            a.a[2] = Chance(r, 4) ? r.U32() : Pick(r, 400);
            a.a[3] = Chance(r, 4) ? r.U32() : Pick(r, 260);
            a.a4 = c.mem->PeekWord(s::kHudOt);
            a.more[0] = Chance(r, 2) ? 0x808080u : r.U32() & 0xFFFFFFu;
        },
        [](NativeEnv& env, const Args& a) {
            return Run(env, [&](s::GuestRam& g, s::HudCallees& k) {
                if (!s::HudDrawText(g, k, env.sp, static_cast<int32_t>(a.a[0]), static_cast<int32_t>(a.a[1]),
                                    static_cast<int16_t>(a.a[2]), static_cast<int16_t>(a.a[3]), a.a4, a.more[0]) &&
                    env.failure.empty())
                    env.failure = "DrawText refused";
                return 0u;
            });
        },
        false, Seams(0, 0, 32 + 88), 0)));

    // ------------------------------------------------------------------ SLUS 0x8002D1A0
    rows.push_back(HudRow(
        "hud_glyph", "Glyph *Glyph(u8 c, FontHeader *h, Glyph *table)  [SLUS 0x8002D1A0]", kGlyphFn,
        [](CaseContext& c, Args& a) {
            Rng& r = *c.rng;
            const uint32_t rec = s::kHudFontRecords + 24u;
            a.a[0] = Chance(r, 4) ? r.U32() : 32u + Pick(r, 100);
            a.a[1] = c.mem->PeekWord(rec + 4u);
            a.a[2] = c.mem->PeekWord(rec + 8u);
        },
        [](NativeEnv& env, const Args& a) {
            return Run(env, [&](s::GuestRam& g, s::HudCallees&) { return s::HudGlyph(g, a.a[0], a.a[1], a.a[2]); });
        },
        true));

    // ------------------------------------------------------------------ SLUS 0x8002D0D8
    rows.push_back(Bios(HudRow(
        "hud_string_width", "s16 StringWidth(s32 font, const char *s)  [SLUS 0x8002D0D8]", kWidthFn,
        [](CaseContext& c, Args& a) {
            Rng& r = *c.rng;
            a.a[0] = Chance(r, 8) ? 0xFFFFFFFFu : 1u;
            a.a[1] = c.mem->PeekWord(c.mem->PeekWord(s::kHudStringTable) + 4u * Pick(r, 181));
        },
        [](NativeEnv& env, const Args& a) {
            return Run(env, [&](s::GuestRam& g, s::HudCallees&) {
                return static_cast<uint32_t>(s::HudStringWidth(g, static_cast<int32_t>(a.a[0]), a.a[1]));
            });
        },
        true)));

    // ------------------------------------------------------------------ RASHCDG 0x80095410
    rows.push_back(HudRow(
        "hud_route_off", "s32 RouteOff(Bike *b)  [RASHCDG 0x80095410]", kRouteOffFn,
        [](CaseContext& c, Args& a) {
            PlantHud(c);
            Rng& r = *c.rng;
            a.a[0] = Bike(*c.mem, Pick(r, kLive));
            if (Chance(r, 3)) Put16(*c.mem, a.a[0] + 362u, Pick(r, 3));
        },
        [](NativeEnv& env, const Args& a) {
            return Run(env, [&](s::GuestRam& g, s::HudCallees&) { return static_cast<uint32_t>(s::HudRouteOff(g, a.a[0])); });
        },
        true));

    // ------------------------------------------------------------------ SLUS 0x8003B9A8
    rows.push_back(HudRow(
        "hud_road_object", "RoadObj *RoadObject(s32 id, s32 kind)  [SLUS 0x8003B9A8]", kRoadObjectFn,
        [](CaseContext& c, Args& a) {
            Rng& r = *c.rng;
            a.a[0] = Chance(r, 8) ? r.U32() : Pick(r, 700);
            a.a[1] = Chance(r, 2) ? 0u : Pick(r, 4);
        },
        [](NativeEnv& env, const Args& a) {
            return Run(env, [&](s::GuestRam& g, s::HudCallees&) {
                return s::HudRoadObject(g, static_cast<int32_t>(a.a[0]), static_cast<int32_t>(a.a[1]));
            });
        },
        true));

    // ------------------------------------------------------------------ SLUS 0x8003BC48
    rows.push_back(HudRow(
        "hud_junction_exits", "s32 JunctionExits(s32 node, s32 road, s32 *out, s32 max)  [SLUS 0x8003BC48]", kExitsFn,
        [](CaseContext& c, Args& a) {
            Rng& r = *c.rng;
            const std::vector<JunctionPair>& pairs = JunctionPairs(*c.mem, c.gp);
            if (!pairs.empty() && !Chance(r, 4)) {
                const JunctionPair& jp = pairs[Pick(r, static_cast<uint32_t>(pairs.size()))];
                a.a[0] = static_cast<uint32_t>(jp.node);
                a.a[1] = static_cast<uint32_t>(jp.road);
            } else {
                a.a[0] = Chance(r, 8) ? r.U32() : Pick(r, 700);
                a.a[1] = Chance(r, 8) ? r.U32() : Pick(r, 700);
            }
            a.a[2] = c.scratch;
            a.a[3] = Pick(r, 5);
        },
        [](NativeEnv& env, const Args& a) {
            return Run(env, [&](s::GuestRam& g, s::HudCallees&) {
                return static_cast<uint32_t>(s::HudJunctionExits(g, static_cast<int32_t>(a.a[0]), static_cast<int32_t>(a.a[1]),
                                                                 a.a[2], static_cast<int32_t>(a.a[3]), env.sp));
            });
        },
        true));

    // ------------------------------------------------------------------ SLUS 0x8003C590
    rows.push_back(HudRow(
        "hud_wrong_way_mask", "u32 WrongWayMask(Bike *b)  [SLUS 0x8003C590, rules.md 6.6]", kMaskFn,
        [](CaseContext& c, Args& a) {
            PlantHud(c);
            Rng& r = *c.rng;
            a.a[0] = Chance(r, 2) ? kBike : Bike(*c.mem, Pick(r, kLive));
            if (Chance(r, 2)) Put8(*c.mem, c.mem->PeekWord(a.a[0] + 1084u), c.mem->PeekWord(c.mem->PeekWord(a.a[0] + 1084u)) | 0x80u);
            if (Chance(r, 3)) Put16(*c.mem, a.a[0] + 362u, Pick(r, 2));
            if (Chance(r, 2)) NearJunction(*c.mem, a.a[0], r);
        },
        [](NativeEnv& env, const Args& a) {
            return Run(env, [&](s::GuestRam& g, s::HudCallees&) { return s::HudWrongWayMask(g, a.a[0], env.sp); });
        },
        true));

    // ------------------------------------------------------------------ RASHCDG 0x8008B84C
    rows.push_back(HudRow(
        "hud_nearest_rival", "Bike *NearestRival(Bike *b)  [RASHCDG 0x8008B84C]", kRivalFn,
        [](CaseContext& c, Args& a) {
            PlantHud(c);
            Rng& r = *c.rng;
            a.a[0] = Chance(r, 2) ? kBike : Bike(*c.mem, Pick(r, kLive));
            if (Chance(r, 4)) c.mem->PokeWord(s::kHudLiveBikes, Pick(r, kLive + 1u));
        },
        [](NativeEnv& env, const Args& a) {
            return Run(env, [&](s::GuestRam& g, s::HudCallees&) { return s::HudNearestRival(g, a.a[0]); });
        },
        true));

    // ------------------------------------------------------------------ the radar strip, RASHCDG 0x800C52A0
    rows.push_back(HudRow(
        "hud_radar_strip", "void RadarStrip(Bike *b, s32 p)  [RASHCDG 0x800C52A0]", 0x800C52A0u,
        [](CaseContext& c, Args& a) {
            PlantHud(c);
            a.a[0] = c.mem->PeekWord(s::kHudPlayerBikes);
            a.a[1] = 0;
        },
        [](NativeEnv& env, const Args& a) {
            return Run(env, [&](s::GuestRam& g, s::HudCallees& k) {
                if (!s::HudRadarStrip(g, k, env.sp + 32u, a.a[0], static_cast<int32_t>(a.a[1])) && env.failure.empty())
                    env.failure = "RadarStrip refused";
                return 0u;
            });
        },
        false, Seams(0, 0, 80 + 72 + 40), 32));
    rows.push_back(HudRow(
        "hud_radar_marks", "void RadarMarks(Entity *e, u32 kind, s32 angle, s32 p)  [RASHCDG 0x800C569C]", 0x800C569Cu,
        [](CaseContext& c, Args& a) {
            PlantHud(c);
            Rng& r = *c.rng;
            a.a[0] = c.mem->PeekWord(s::kHudPlayerBikes);
            a.a[1] = Chance(r, 2) ? (c.mem->PeekWord(c.mem->PeekWord(a.a[0] + 1084u)) >> 8) & 0xFFu : Pick(r, 256);
            a.a[2] = Pick(r, 4096);
            a.a[3] = 0;
        },
        [](NativeEnv& env, const Args& a) {
            return Run(env, [&](s::GuestRam& g, s::HudCallees& k) {
                if (!s::HudRadarMarks(g, k, env.sp + 80u, a.a[0], a.a[1], static_cast<int32_t>(a.a[2]),
                                      static_cast<int32_t>(a.a[3])) && env.failure.empty())
                    env.failure = "RadarMarks refused";
                return 0u;
            });
        },
        false, Seams(0, 0, 72 + 40), 80));
    rows.push_back(HudRow(
        "hud_radar_mark", "void RadarMark(const s32 v[3], s32 angle, s32 p, s32 kind)  [RASHCDG 0x800C5380]", 0x800C5380u,
        [](CaseContext& c, Args& a) {
            PlantHud(c);
            Rng& r = *c.rng;
            for (uint32_t k = 0; k < 3; ++k)
                c.mem->PokeWord(c.scratch + 4u * k, Chance(r, 8) ? r.U32() : static_cast<uint32_t>(static_cast<int32_t>(Pick(r, 400u << 16)) - (200 << 16)));
            a.a[0] = c.scratch;
            a.a[1] = Chance(r, 8) ? r.U32() : Pick(r, 4096);
            a.a[2] = 0;
            a.a[3] = Pick(r, 6);
        },
        [](NativeEnv& env, const Args& a) {
            return Run(env, [&](s::GuestRam& g, s::HudCallees& k) {
                if (!s::HudRadarMark(g, k, env.sp + 72u, a.a[0], static_cast<int32_t>(a.a[1]), static_cast<int32_t>(a.a[2]),
                                     static_cast<int32_t>(a.a[3])) && env.failure.empty())
                    env.failure = "RadarMark refused";
                return 0u;
            });
        },
        false, Seams(0, 0, 40), 72));
    rows.push_back(HudRow(
        "hud_radar_quad", "void RadarQuad(const s16 v[8], const u8 rgb[3], u32 code, s32 p)  [RASHCDG 0x800C5168]",
        0x800C5168u,
        [](CaseContext& c, Args& a) {
            PlantHud(c);
            Rng& r = *c.rng;
            for (uint32_t k = 0; k < 16; k += 4) c.mem->PokeWord(c.scratch + k, r.U32());
            a.a[0] = c.scratch;
            a.a[1] = s::kHudMarkColours + 4u * Pick(r, 6);
            a.a[2] = Chance(r, 2) ? 0u : (r.U32() & 0xFFu);
            a.a[3] = 0;
        },
        [](NativeEnv& env, const Args& a) {
            return Run(env, [&](s::GuestRam& g, s::HudCallees& k) {
                if (!s::HudRadarQuad(g, k, env.sp + 40u, a.a[0], a.a[1], a.a[2], static_cast<int32_t>(a.a[3])) &&
                    env.failure.empty())
                    env.failure = "RadarQuad refused";
                return 0u;
            });
        },
        false, Seams(0, 0, 0), 40));
    rows.push_back(HudRow(
        "hud_radar_rect_area", "void RadarRectArea(s32 p)  [RASHCDG 0x800C5558]", 0x800C5558u,
        [](CaseContext& c, Args& a) {
            PlantHud(c);
            Rng& r = *c.rng;
            const uint32_t heap = c.mem->PeekWord(s::kHudHeapPtr);
            if (Chance(r, 2)) Put8(*c.mem, heap + 5u, Pick(r, 2));
            a.a[0] = 0;
        },
        [](NativeEnv& env, const Args& a) {
            return Run(env, [&](s::GuestRam& g, s::HudCallees& k) {
                if (!s::HudRadarRectArea(g, k, env.sp + 48u, static_cast<int32_t>(a.a[0])) && env.failure.empty())
                    env.failure = "RadarRectArea refused";
                return 0u;
            });
        },
        false, Seams(0, 0, 56), 48));
    rows.push_back(HudRow(
        "hud_radar_full_area", "void RadarFullArea(s32 p)  [RASHCDG 0x800C5618]", 0x800C5618u,
        [](CaseContext& c, Args& a) {
            PlantHud(c);
            Rng& r = *c.rng;
            if (Chance(r, 2)) {
                Put16(*c.mem, s::kGpuDrawEnv, Pick(r, 1024));
                Put16(*c.mem, s::kGpuDrawEnv + 2u, Pick(r, 512));
            }
            a.a[0] = 0;
        },
        [](NativeEnv& env, const Args& a) {
            return Run(env, [&](s::GuestRam& g, s::HudCallees& k) {
                if (!s::HudRadarFullArea(g, k, env.sp + 144u, static_cast<int32_t>(a.a[0])) && env.failure.empty())
                    env.failure = "RadarFullArea refused";
                return 0u;
            });
        },
        false, Seams(0, 0, 56), 144));
    rows.push_back(HudRow(
        "hud_draw_area", "void DrawArea(s16 x, s16 y, s16 w, s16 h, s32, s32, u32 *ot)  [SLUS 0x8001C304]", 0x8001C304u,
        [](CaseContext& c, Args& a) {
            PlantHud(c);
            Rng& r = *c.rng;
            for (int k = 0; k < 4; ++k)
                a.a[static_cast<size_t>(k)] = Chance(r, 6) ? r.U32() : static_cast<uint32_t>(static_cast<int32_t>(Pick(r, 1400)) - 200);
            a.a4 = r.U32();
            a.more[0] = r.U32();
            a.more[1] = c.mem->PeekWord(s::kHudOt);
        },
        [](NativeEnv& env, const Args& a) {
            return Run(env, [&](s::GuestRam& g, s::HudCallees& k) {
                if (!s::HudDrawArea(g, k, env.sp + 56u, static_cast<int16_t>(a.a[0]), static_cast<int16_t>(a.a[1]),
                                    static_cast<int16_t>(a.a[2]), static_cast<int16_t>(a.a[3]), a.more[1]) &&
                    env.failure.empty())
                    env.failure = "DrawArea refused";
                return 0u;
            });
        },
        false, Seams(0, 0, 0), 56));
    rows.push_back(HudRow(
        "hud_set_draw_area", "void SetDrawArea(DR_AREA *p, const RECT *r)  [SLUS 0x8004954C, libgpu]", 0x8004954Cu,
        [](CaseContext& c, Args& a) {
            Rng& r = *c.rng;
            for (uint32_t k = 0; k < 8; k += 2)
                Put16(*c.mem, c.scratch + 16u + k, Chance(r, 6) ? r.U32() : static_cast<uint32_t>(static_cast<int32_t>(Pick(r, 1400)) - 200));
            if (Chance(r, 4)) {
                Put16(*c.mem, s::kGpuVramSize, Pick(r, 2048));
                Put16(*c.mem, s::kGpuVramSize + 2u, Pick(r, 1024));
            }
            a.a[0] = c.scratch;
            a.a[1] = c.scratch + 16u;
        },
        [](NativeEnv& env, const Args& a) {
            return Run(env, [&](s::GuestRam& g, s::HudCallees&) { s::HudSetDrawArea(g, a.a[0], a.a[1]); return 0u; });
        }));
    rows.push_back(HudRow(
        "hud_area_top_left", "u32 AreaTopLeft(s16 x, s16 y)  [SLUS 0x80049A94, libgpu]", 0x80049A94u,
        [](CaseContext& c, Args& a) {
            Rng& r = *c.rng;
            a.a[0] = Chance(r, 3) ? r.U32() : static_cast<uint32_t>(static_cast<int32_t>(Pick(r, 1400)) - 200);
            a.a[1] = Chance(r, 3) ? r.U32() : static_cast<uint32_t>(static_cast<int32_t>(Pick(r, 800)) - 200);
        },
        [](NativeEnv& env, const Args& a) {
            return Run(env, [&](s::GuestRam& g, s::HudCallees&) {
                return s::HudAreaTopLeft(g, static_cast<int32_t>(a.a[0]), static_cast<int32_t>(a.a[1]));
            });
        },
        true));
    rows.push_back(HudRow(
        "hud_area_bottom_right", "u32 AreaBottomRight(s16 x, s16 y)  [SLUS 0x80049B2C, libgpu]", 0x80049B2Cu,
        [](CaseContext& c, Args& a) {
            Rng& r = *c.rng;
            a.a[0] = Chance(r, 3) ? r.U32() : static_cast<uint32_t>(static_cast<int32_t>(Pick(r, 1400)) - 200);
            a.a[1] = Chance(r, 3) ? r.U32() : static_cast<uint32_t>(static_cast<int32_t>(Pick(r, 800)) - 200);
        },
        [](NativeEnv& env, const Args& a) {
            return Run(env, [&](s::GuestRam& g, s::HudCallees&) {
                return s::HudAreaBottomRight(g, static_cast<int32_t>(a.a[0]), static_cast<int32_t>(a.a[1]));
            });
        },
        true));
    rows.push_back(HudRow(
        "hud_rotate2", "void Rotate2(s32 angle, s32 v[2])  [SLUS 0x8002DE40]", 0x8002DE40u,
        [](CaseContext& c, Args& a) {
            Rng& r = *c.rng;
            c.mem->PokeWord(c.scratch, r.U32() >> Pick(r, 12));
            c.mem->PokeWord(c.scratch + 4u, r.U32() >> Pick(r, 12));
            a.a[0] = Chance(r, 4) ? r.U32() : Pick(r, 4096);
            a.a[1] = c.scratch;
        },
        [](NativeEnv& env, const Args& a) {
            return Run(env, [&](s::GuestRam& g, s::HudCallees&) {
                s::HudRotate2(g, static_cast<int32_t>(a.a[0]), a.a[1], env.sp);
                return 0u;
            });
        }));
    rows.push_back(HudRow(
        "hud_scale2", "void Scale2(s32 v[2], s32 k)  [SLUS 0x8002DEC8]", 0x8002DEC8u,
        [](CaseContext& c, Args& a) {
            Rng& r = *c.rng;
            c.mem->PokeWord(c.scratch, r.U32() >> Pick(r, 12));
            c.mem->PokeWord(c.scratch + 4u, r.U32() >> Pick(r, 12));
            a.a[0] = c.scratch;
            a.a[1] = r.U32() >> Pick(r, 16);
        },
        [](NativeEnv& env, const Args& a) {
            return Run(env, [&](s::GuestRam& g, s::HudCallees&) {
                s::HudScale2(g, a.a[0], static_cast<int32_t>(a.a[1]));
                return 0u;
            });
        }));
    rows.push_back(HudRow(
        "hud_sincos", "void SinCos(s32 angle, s32 *s, s32 *c)  [SLUS 0x8001FE80]", 0x8001FE80u,
        [](CaseContext& c, Args& a) {
            Rng& r = *c.rng;
            a.a[0] = r.U32();
            a.a[1] = c.scratch;
            a.a[2] = c.scratch + 4u;
        },
        [](NativeEnv& env, const Args& a) {
            return Run(env, [&](s::GuestRam& g, s::HudCallees&) {
                s::HudSinCos(g, static_cast<int32_t>(a.a[0]), a.a[1], a.a[2]);
                return 0u;
            });
        }));
}
