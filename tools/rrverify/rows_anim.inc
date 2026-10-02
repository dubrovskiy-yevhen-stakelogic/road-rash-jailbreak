// Bench rows for the rider animation machine. Included by
// verify_physics.cpp inside its anonymous namespace, so everything defined above that point is visible
// here.
//
// What is benched: the animation clock (src\game\sim\anim.{h,cpp}) plus the
// channel-header decode 0x8005E2BC, the stance event 0x800C4550 on top of them, and RiderKnockOff
// 0x80090D84 / RiderLaunch 0x80091468 on top of that (src\game\sim\stance.{h,cpp}).
// RASHCDG.BIN cfe43a7786759f2cb9c57751cf99e84d1074782c, SLUS_010.53
// 67ed165a2c517d4e6106fb0dfa324d66dd9a76f1.
//
// THE SEAMS, each declared per row next to its depth:
//   * RASHCDG 0x8005CB70 TransitionCapture(a, op, B) and 0x8005D2A8 ApplyFrame(a) - the POSE,
//     not ported here (rows_pose.inc). What the clock reads back of their effects is named in
//     anim.h (AnimPoseSeam).
//   * RASHCDG 0x800BFD24 / 0x800BFC5C - the stance layer's two combat children (FightEnd and the
//     rest), not ported;
//   * the knock-off's and the launch's unported callees.
// Everything else on these rows is native code.
//
// THE FAMILIES. Dump-derived cases plant an animation object CAPTURED in one of the 18 RAM images
// (the four race savestates and the 14 frame dumps: the banks are resident at the same addresses in
// all of them, so a captured object's clip, channel and event pointers are valid in this machine)
// into the same slot of this machine, keeping this machine's owner and program pointers; the stance
// rows also copy that rider's captured stance fields. Randomised cases draw the clock, the program
// and the arguments. Named bounds, each because the ORIGINAL would not return (or would walk off
// memory it never walks in the game), not because the port could not follow:
//   * a clip index is always < its bank's clip count, and an op 3 (blend) sits only at pc 0..2 with
//     op k+2's `a` a valid clip (the blend's new clip, read by TransitionCapture);
//   * a positive rate is drawn so that acc + dt stays under ~10^5 keys (the tick runs one guest loop
//     turn per key); a non-positive rate is drawn with acc + dt < rate (no turn), or in
//     [-(2^30 + 2^28), -2^30], where the loop wraps and ends at its second turn;
//   * synthetic DMD3 clips (decode row) keep their record lengths >= 3 and widths 0..7 (a record of
//     length -1 never advances; a width past 7 reads a pointer past the 8-entry table);
//   * banks are this machine's resident banks 0..5; stances whose bank is not resident (bank 6, the
//     short set's) are never requested, and stance 0 is requested only with a planted animation
//     object on the bike (StanceEnter starts clip 0 on `bike+0x21C`, which is 0 in every capture:
//     the original then takes its program pointer from RAM word 4, an unmapped address);
//   * the rest are named where they are drawn.

namespace animrows {

constexpr uint32_t kDesc = rr::sim::kAnimDescriptor;
constexpr uint32_t kObj = rr::sim::kAnimObjectBytes;
constexpr uint32_t kRiderBytes = 628;
constexpr uint32_t kResidentBanks = 6; // descriptor bank table entries 0..5 are loaded in rr-race

// ---------------------------------------------------------------------------- captured objects
struct Captured {
    int slot = 0;
    std::vector<uint8_t> obj;   // the whole 0x83C-byte object
    std::vector<uint8_t> prog;  // its 60-byte program
    std::vector<uint8_t> owner; // 628 bytes at its owner (a pool-1 rider for slots 0..17)
};
struct CapturedImage {
    std::string origin;
    std::vector<Captured> objs; // indexed by slot; an entry with an empty `obj` had no owner
    int32_t dt = 0;             // that image's game_state+0x1C
};

uint32_t Rd32(const std::vector<uint8_t>& ram, uint32_t a) {
    const uint32_t o = a & 0x1FFFFFu;
    if (o + 4u > ram.size()) return 0;
    return static_cast<uint32_t>(ram[o]) | (static_cast<uint32_t>(ram[o + 1]) << 8) |
           (static_cast<uint32_t>(ram[o + 2]) << 16) | (static_cast<uint32_t>(ram[o + 3]) << 24);
}

// The same RAM images `HarvestSamples` reads (its default roots), read once.
const std::vector<CapturedImage>& Images() {
    static const std::vector<CapturedImage> images = [] {
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
        std::sort(files.begin(), files.end());
        std::vector<CapturedImage> out;
        for (const std::string& f : files) {
            std::vector<uint8_t> ram;
            std::string error;
            if (!rr::interp::ReadWholeFile(f, ram, error) || ram.size() < 0x200000u) continue;
            const uint32_t objs = Rd32(ram, kDesc);
            const int32_t count = static_cast<int32_t>(Rd32(ram, kDesc + 12u));
            if (objs < 0x80000000u || count <= 0 || count > 32) continue;
            CapturedImage img;
            img.origin = f;
            img.dt = static_cast<int32_t>(Rd32(ram, Rd32(ram, kGameStatePtr) + 28u));
            img.objs.resize(static_cast<size_t>(count));
            size_t live = 0;
            for (int32_t i = 0; i < count; ++i) {
                const uint32_t a = objs + kObj * static_cast<uint32_t>(i);
                const uint32_t owner = Rd32(ram, a);
                const uint32_t prog = Rd32(ram, a + 4u);
                if (owner < 0x80000000u || prog < 0x80000000u) continue;
                Captured c;
                c.slot = i;
                const uint32_t ao = a & 0x1FFFFFu, po = prog & 0x1FFFFFu, oo = owner & 0x1FFFFFu;
                if (ao + kObj > ram.size() || po + 60u > ram.size() || oo + kRiderBytes > ram.size()) continue;
                c.obj.assign(ram.begin() + ao, ram.begin() + ao + kObj);
                c.prog.assign(ram.begin() + po, ram.begin() + po + 60);
                c.owner.assign(ram.begin() + oo, ram.begin() + oo + kRiderBytes);
                img.objs[static_cast<size_t>(i)] = std::move(c);
                ++live;
            }
            if (live != 0) out.push_back(std::move(img));
        }
        return out;
    }();
    return images;
}

int32_t CapturedDt(Rng& r) {
    const std::vector<CapturedImage>& imgs = Images();
    if (imgs.empty()) return 5;
    return imgs[r.U32() % static_cast<uint32_t>(imgs.size())].dt;
}

// ---------------------------------------------------------------------------- memory helpers
void Put8(Memory& m, uint32_t a, uint32_t v) {
    const uint8_t b = static_cast<uint8_t>(v);
    m.WriteBlock(a, &b, 1);
}
void Put16(Memory& m, uint32_t a, uint32_t v) { WriteU16Bench(m, a, static_cast<uint16_t>(v)); }
uint32_t Get16(const Memory& m, uint32_t a) {
    return static_cast<uint32_t>(m.PeekByte(a)) | (static_cast<uint32_t>(m.PeekByte(a + 1)) << 8);
}
uint32_t ObjAt(const Memory& m, uint32_t slot) { return m.PeekWord(kDesc) + kObj * slot; }
uint32_t BankPtr(const Memory& m, uint32_t k) { return m.PeekWord(rr::sim::kAnimBankTable + 4u * k); }
uint32_t BankClips(const Memory& m, uint32_t bank) { return Get16(m, bank + 2u); }
uint32_t ClipAt(const Memory& m, uint32_t bank, uint32_t idx) {
    return m.PeekWord(m.PeekWord(bank + 4u) + 4u * idx);
}
uint32_t KeysOf(const Memory& m, uint32_t clip) { return Get16(m, clip + 16u); }
uint32_t Pick(Rng& r, uint32_t n) { return n == 0 ? 0u : r.U32() % n; }
int32_t Range(Rng& r, int32_t lo, int32_t hi) { // inclusive; lo when the range is empty
    if (hi <= lo) return lo;
    return static_cast<int32_t>(static_cast<uint32_t>(lo) +
                                r.U32() % (static_cast<uint32_t>(hi) - static_cast<uint32_t>(lo) + 1u));
}
// The dt of a random captured image (5 without images).
int32_t CapturedDt(Rng& r);
// An `ex` word: a real per-stance event list pointer most of the time, else noise.
uint32_t SomeEx(CaseContext& c) {
    const uint32_t list = c.mem->PeekWord(rr::sim::kStanceEventList);
    if (list != 0u && c.rng->Next() % 4u != 0u) return c.mem->PeekWord(list + 4u * Pick(*c.rng, 224));
    return (c.rng->Next() & 1u) ? 0u : c.rng->U32();
}

// Plant a captured object into `a` (this machine's slot), keeping this machine's owner and program
// pointers; the captured program goes into this machine's program.
void PlantCaptured(Memory& m, uint32_t a, const Captured& cap) {
    m.WriteBlock(a + 8u, cap.obj.data() + 8, kObj - 8u);
    m.WriteBlock(m.PeekWord(a + 4u), cap.prog.data(), 60);
}
// A captured object for `slot` from a random image (any slot when that image has none there).
const Captured* PickCaptured(Rng& r, uint32_t slot) {
    const std::vector<CapturedImage>& imgs = Images();
    if (imgs.empty()) return nullptr;
    for (int tries = 0; tries < 8; ++tries) {
        const CapturedImage& img = imgs[Pick(r, static_cast<uint32_t>(imgs.size()))];
        if (slot < img.objs.size() && !img.objs[slot].obj.empty()) return &img.objs[slot];
    }
    return nullptr;
}

// A random 5-op program over the object's bank (the named bounds above).
void RandomProgram(CaseContext& c, uint32_t a, bool allowBlend) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    const uint32_t P = m.PeekWord(a + 4u);
    const uint32_t bank = m.PeekWord(a + 40u);
    const uint32_t n = BankClips(m, bank);
    uint8_t codes[5];
    for (uint32_t k = 0; k < 5; ++k) {
        const uint32_t d = r.U32() % 100u;
        uint8_t code = d < 20 ? 0 : d < 45 ? 1 : d < 55 ? 2 : d < 70 ? 3 : d < 80 ? 4 : d < 95 ? 5
                                                                                    : static_cast<uint8_t>(6 + r.U32() % 250u);
        if (code == 3 && (k > 2 || !allowBlend)) code = 1;
        codes[k] = code;
        const uint32_t clip = Pick(r, n);
        const int32_t keys = static_cast<int32_t>(KeysOf(m, ClipAt(m, bank, clip)));
        const uint32_t op = P + 12u * k;
        Put8(m, op + 0, clip);
        Put8(m, op + 1, code);
        Put8(m, op + 2, r.U32());
        Put8(m, op + 3, (r.Next() % 3u == 0) ? 0u : r.U32());
        int32_t av = 0, bv = 0;
        switch (code) {
        case 0: {
            const uint32_t q = r.U32() % 6u;
            av = q == 0 ? -1 : q == 1 ? 0 : q == 2 ? 1 : q == 3 ? 2 : q == 4 ? -2 : static_cast<int16_t>(r.U32());
            bv = static_cast<int16_t>(r.U32());
            break;
        }
        case 1: av = Range(r, -2, keys + 2); bv = Range(r, -2, keys + 2); break;
        case 2: av = (r.Next() % 5u == 0) ? Range(r, -2, 7) : Range(r, 0, 4); bv = static_cast<int16_t>(r.U32()); break;
        case 3: av = Range(r, -1, 8); bv = static_cast<int16_t>(r.U32()); break;
        default: av = static_cast<int16_t>(r.U32()); bv = static_cast<int16_t>(r.U32()); break;
        }
        Put16(m, op + 4, static_cast<uint32_t>(av));
        Put16(m, op + 6, static_cast<uint32_t>(bv));
        m.PokeWord(op + 8, SomeEx(c));
    }
    for (uint32_t k = 0; k <= 2; ++k)
        if (codes[k] == 3) Put8(m, P + 12u * (k + 2u) + 4u, Pick(r, n)); // the blend's new clip
}

// Random clock fields for the object at `a`; its current clip becomes a valid clip of its bank.
void RandomClock(CaseContext& c, uint32_t a) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    const uint32_t bank = m.PeekWord(a + 40u);
    const uint32_t clip = ClipAt(m, bank, Pick(r, BankClips(m, bank)));
    if (r.Next() % 4u != 0u) m.PokeWord(a + 44u, clip);
    const int32_t keys = static_cast<int32_t>(KeysOf(m, m.PeekWord(a + 44u)));
    const int32_t rate = (r.Next() % 5u != 0u) ? Range(r, 1, 60) : Range(r, 61, 300);
    PutS32(m, a + 24u, rate);
    PutS32(m, a + 16u, (r.Next() % 10u != 0u) ? Range(r, -2, keys + 2) : static_cast<int32_t>(r.U32() >> 20));
    PutS32(m, a + 28u, Range(r, 0, 3 * rate - 1));
    PutS32(m, a + 32u, Range(r, 0, rate));
    {
        const uint32_t q = r.U32() % 6u;
        m.PokeWord(a + 20u, q == 0 ? 0xFFFFFFFFu : q == 1 ? 0u : q == 2 ? 1u : q == 3 ? 0xFFFFu : q == 4 ? 2u : r.U32());
    }
    uint32_t f = r.U32();
    if (r.Next() % 5u != 0u) f |= 2u;
    m.PokeWord(a + 36u, f);
    m.PokeWord(a + 12u, (r.Next() % 10u != 0u) ? Pick(r, 5) : 5u + Pick(r, 3));
    PutS16(m, a + 48u, static_cast<int16_t>((r.Next() % 3u == 0) ? -1 : Range(r, -1, keys)));
}

// The object of a case: slot 0..20 (the owned ones), captured (derived) or drawn (random).
uint32_t PlantObject(CaseContext& c, bool allowBlend = true, uint32_t slotLimit = 21) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    const uint32_t slot = Pick(r, slotLimit);
    const uint32_t a = ObjAt(m, slot);
    const Captured* cap = c.sample ? PickCaptured(r, slot) : nullptr;
    if (cap != nullptr) {
        PlantCaptured(m, a, *cap);
        if (!allowBlend) // a captured blend program is replaced by a drawn one when the row has no seam
            for (uint32_t k = 0; k < 5; ++k)
                if (m.PeekByte(m.PeekWord(a + 4u) + 12u * k + 1u) == 3) { RandomProgram(c, a, false); break; }
    } else {
        RandomProgram(c, a, allowBlend);
        RandomClock(c, a);
    }
    return a;
}

// ---------------------------------------------------------------------------- the seams
struct AnimOracleSeams final : rr::sim::StanceSeams {
    NativeEnv& env;
    explicit AnimOracleSeams(NativeEnv& e) : env(e) {}
    uint32_t TransitionCapture(uint32_t a, uint32_t op, uint32_t b) override {
        return env.CallOracle(rr::sim::kAnimTransitionCaptureFn, a, op, b, 0);
    }
    uint32_t ApplyFrame(uint32_t a) override { return env.CallOracle(rr::sim::kAnimApplyFrameFn, a, 0, 0, 0); }
    uint32_t CombatLeave(uint32_t r, uint32_t cur, uint32_t ev, uint32_t p) override {
        return env.CallOracle(rr::sim::kCombatLeaveFn, r, cur, ev, p);
    }
    uint32_t CombatEnter(uint32_t r, uint32_t cur, uint32_t ev, uint32_t p) override {
        return env.CallOracle(rr::sim::kCombatEnterFn, r, cur, ev, p);
    }
};

// Runs `f(layer)` on the clone's own RAM and scratchpad; a refusal of the port fails the case.
template <class F>
uint32_t RunNative(NativeEnv& env, F&& f) {
    Memory& m = env.clone->mem;
    rr::sim::GuestRam g(m.ram().data(), env.gp);
    g.SetScratchpad(m.scratchpad().data());
    AnimOracleSeams seams(env);
    rr::sim::StanceLayer layer(g, seams);
    const uint32_t v = f(layer);
    if (layer.Failed() && env.failure.empty()) {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "the port refused (view fault %d at 0x%08X)", g.Faulted() ? 1 : 0,
                      g.FaultAddress());
        env.failure = buf;
    }
    return v;
}

// The depths (own frames, `addiu sp,sp,-N` at each entry) at which the seams are reached.
constexpr uint32_t kFrPoseStart = 32, kFrAdvanceFrame = 24, kFrRestart = 32, kFrTransition = 56,
                   kFrPass = 40, kFrGate = 88, kFrEvent = 40, kFrLeave = 24, kFrEnter = 40;

OracleCallee Capture(uint32_t depth) { return OracleCallee{rr::sim::kAnimTransitionCaptureFn, 3, 0, {}, depth}; }

Row AnimRow(const char* name, const char* sig, uint32_t entry, std::function<void(CaseContext&, Args&)> setup,
            std::function<uint32_t(NativeEnv&, const Args&)> native, bool compareV0,
            std::vector<OracleCallee> callees = {}) {
    Row r{name, sig, entry, std::move(setup), nullptr, compareV0, std::move(native), std::move(callees)};
    return r;
}


// ---------------------------------------------------------------------------- the stance families
// A stance a request may name: a record < 224 whose bank is resident in this machine and whose clip
// lies inside it (the named bound: the short set's bank 6 is never loaded in the long set).
bool Resident(const Memory& m, uint32_t st) {
    if (st >= rr::sim::kStanceNone) return false;
    const uint32_t w0 = m.PeekWord(rr::sim::kStanceTable + 8u * st);
    const uint32_t bank = w0 & 0xFu;
    if (bank >= kResidentBanks) return false;
    const uint32_t b = BankPtr(m, bank);
    return b != 0u && ((w0 >> 4) & 0xFFFu) < BankClips(m, b);
}
uint32_t ResidentStance(CaseContext& c) {
    for (int t = 0; t < 64; ++t) {
        const uint32_t st = Pick(*c.rng, rr::sim::kStanceNone);
        if (Resident(*c.mem, st)) return st;
    }
    return 2;
}

struct StanceCase {
    uint32_t rider = 0, anim = 0, ev = 0, p = 0, slot = 0;
};

// One rider of this machine (pool 1, slots 0..17, each owning animation object `slot`) with a
// captured (derived) or drawn (random) stance state, animation object and request.
StanceCase PlantStanceCase(CaseContext& c, bool rawEvHighBits) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    StanceCase sc;
    sc.slot = Pick(r, 18);
    sc.anim = ObjAt(m, sc.slot);
    sc.rider = m.PeekWord(sc.anim);
    const Captured* cap = c.sample ? PickCaptured(r, sc.slot) : nullptr;
    if (cap != nullptr) {
        PlantCaptured(m, sc.anim, *cap);
        // the captured rider's stance fields: +0x140 live, +0x220 stance, +0x222, +0x228 flags,
        // +0x23C, +0x25C mount, +0x260 queue
        auto copy = [&](uint32_t off, uint32_t n) { m.WriteBlock(sc.rider + off, cap->owner.data() + off, n); };
        copy(0x140, 2);
        copy(0x220, 3);
        copy(0x228, 4);
        copy(0x23C, 1);
        copy(0x25C, 4);
        copy(0x260, 16);
        if (Get16(m, sc.rider + 0x260u) > 6u) Put16(m, sc.rider + 0x260u, Pick(r, 4));
    } else {
        Put16(m, sc.rider + 0x140u, (r.Next() % 10u == 0) ? 0u : (1u + Pick(r, 0xFFFE)));
        const uint32_t q = r.U32() % 20u;
        Put16(m, sc.rider + 0x220u, q < 3 ? rr::sim::kStanceNone : q == 3 ? 0u : ResidentStance(c));
        Put8(m, sc.rider + 0x222u, r.U32());
        m.PokeWord(sc.rider + 0x228u, r.U32());
        Put8(m, sc.rider + 0x23Cu, r.U32());
        m.PokeWord(sc.rider + 0x25Cu, Pick(r, 5));
        Put16(m, sc.rider + 0x260u, Pick(r, 5));
        for (uint32_t k = 0; k < 7; ++k) Put16(m, sc.rider + 0x262u + 2u * k, ResidentStance(c));
    }
    // The request.
    const uint32_t cur = Get16(m, sc.rider + 0x220u);
    const uint32_t q = r.U32() % 40u;
    uint32_t ev;
    if (q < 2) {
        ev = 0;
    } else if (q < 6) {
        const uint32_t nr[5] = {11, 77, 13, 78, 4};
        ev = nr[Pick(r, 5)];
        if (!Resident(m, ev)) ev = 4;
    } else if (q < 9 && Resident(m, cur)) {
        ev = cur;
    } else if (q < 13) {
        // A pair of StancePath's table: a current stance with a path and a key under it.
        const uint32_t pairs[][2] = {{1, 4}, {5, 11}, {5, 26}, {5, 16}, {6, 7}, {6, 3}, {6, 11},
                                     {12, 14}, {12, 8}, {12, 9}, {13, 14}, {13, 16}, {20, 14}, {20, 9}};
        const uint32_t k = Pick(r, 14);
        Put16(m, sc.rider + 0x220u, pairs[k][0]);
        Put16(m, sc.rider + 0x140u, 1);
        ev = pairs[k][1];
    } else if (q < 16) {
        // leaving (category 5) -> tumbling (category 6): the seat release
        Put16(m, sc.rider + 0x220u, 38u + Pick(r, 5));
        ev = 43u + Pick(r, 17);
        const uint32_t bike = m.PeekWord(sc.rider + 596u);
        if (r.Next() & 1u) m.PokeWord(bike + 56u + 8u * Pick(r, 2), sc.rider);
        if (r.Next() % 3u == 0) m.PokeWord(bike + 856u, m.PeekWord(kPoolBasePtr) + 1096u * Pick(r, 18));
    } else {
        ev = ResidentStance(c);
    }
    if (Get16(m, sc.rider + 0x220u) == 0u && (r.Next() & 1u)) ev = 4; // from stance 0 only 4 is taken
    // The request word p.
    uint32_t p = r.U32() & 0xFFFF0000u;
    p |= Pick(r, 4) << 1; // play mode
    {
        const uint32_t a = r.U32() % 20u; // acceptance
        p |= a < 8 ? 0u : a < 14 ? 8u : a < 19 ? 16u : 24u;
    }
    if (r.Next() % 3u == 0) p |= 1u;
    if (r.Next() % 5u == 0) p |= 0x20u;
    if (r.Next() % 3u == 0) p |= 0x40u;
    if (r.Next() & 1u) p |= 0x80u;
    p |= (r.U32() & 0x7F00u) | ((r.Next() % 5u == 0) ? 0x8000u : 0u);
    if (q >= 9 && q < 13) p &= ~0x38u; // the path: acceptance mode 0, no redirect bit
    sc.p = p;
    // The object's bank is the current stance's (the invariant the gate's bank switch keeps: a clip
    // index is always read in the bank of the stance that named it); 224 "none" keeps whatever bank
    // the object has, as the captures show - its record's bank 8 always forces a switch.
    {
        const uint32_t cr = Get16(m, sc.rider + 0x220u);
        const bool fresh = cap == nullptr;
        bool redraw = fresh;
        if (Resident(m, cr)) {
            const uint32_t want = BankPtr(m, m.PeekWord(rr::sim::kStanceTable + 8u * cr) & 0xFu);
            if (m.PeekWord(sc.anim + 40u) != want) {
                m.PokeWord(sc.anim + 40u, want);
                redraw = true;
            }
        }
        if (redraw) {
            RandomProgram(c, sc.anim, true);
            RandomClock(c, sc.anim);
        }
    }
    // A queued transition across a bank change reads the RUNNING op 0's clip byte in the NEW bank
    // (the gate switches the bank first, 0x800C4158, and Transition reads the key count at
    // 0x8005C1E4..0x8005C200): the named bound keeps that byte inside both banks.
    if (((p >> 1) & 3u) >= 2u && Resident(m, ev)) {
        const uint32_t nb = BankPtr(m, m.PeekWord(rr::sim::kStanceTable + 8u * ev) & 0xFu);
        const uint32_t ob = m.PeekWord(sc.anim + 40u);
        const uint32_t n = std::min(BankClips(m, nb), BankClips(m, ob));
        Put8(m, m.PeekWord(sc.anim + 4u), Pick(r, n));
    }
    // Now and then not a pool-1 rider (+0xAC >> 5 != 1): the gate then works with rider 0, and its
    // seat arm (category 5 -> 6) would chase the kernel's low-RAM words into the BIOS - the named
    // bound: that pair is not drawn for a non-rider (never observed in the original).
    if (r.Next() % 10u == 0) {
        const uint32_t cc = Get16(m, rr::sim::kStanceTable + 8u * Get16(m, sc.rider + 0x220u) + 2u);
        const uint32_t ce = Get16(m, rr::sim::kStanceTable + 8u * ev + 2u);
        if (!(cc == 5u && ce == 6u)) Put16(m, sc.rider + 0xACu, (r.Next() & 1u) ? r.U32() : 0x40u);
    }
    if (ev == 0) {
        // StanceEnter's clip-0 start on the bike's object (the named bound): a spare object, slot 21.
        const uint32_t spare = ObjAt(m, 21);
        m.PokeWord(spare + 40u, BankPtr(m, 0));
        m.PokeWord(spare + 8u, 5);
        m.PokeWord(m.PeekWord(sc.rider + 596u) + 540u, spare);
    }
    sc.ev = ev | ((rawEvHighBits && r.Next() % 5u == 0) ? (r.U32() & 0xFFFF0000u) : 0u);
    // Globals the layer reads: the race clock, and the event-list pointer (0 = none, now and then).
    m.PokeWord(m.PeekWord(kGameStatePtr) + 16u, r.U32());
    if (r.Next() % 6u == 0) m.PokeWord(rr::sim::kStanceEventList, 0);
    return sc;
}

OracleCallee Leave(uint32_t depth) { return OracleCallee{rr::sim::kCombatLeaveFn, 4, 0, {}, depth}; }
OracleCallee Enter(uint32_t depth) { return OracleCallee{rr::sim::kCombatEnterFn, 4, 0, {}, depth}; }

// ---------------------------------------------------------------------------- the knock-off and the launch
struct RiderOracleSeams final : rr::sim::RiderSeams {
    NativeEnv& env;
    explicit RiderOracleSeams(NativeEnv& e) : env(e) {}
    uint32_t TransitionCapture(uint32_t a, uint32_t op, uint32_t b) override {
        return env.CallOracle(rr::sim::kAnimTransitionCaptureFn, a, op, b, 0);
    }
    uint32_t ApplyFrame(uint32_t a) override { return env.CallOracle(rr::sim::kAnimApplyFrameFn, a, 0, 0, 0); }
    uint32_t CombatLeave(uint32_t r, uint32_t cur, uint32_t ev, uint32_t p) override {
        return env.CallOracle(rr::sim::kCombatLeaveFn, r, cur, ev, p);
    }
    uint32_t CombatEnter(uint32_t r, uint32_t cur, uint32_t ev, uint32_t p) override {
        return env.CallOracle(rr::sim::kCombatEnterFn, r, cur, ev, p);
    }
    uint32_t Takedown(uint32_t bike) override { return env.CallOracle(rr::sim::kTakedownFn, bike, 0, 0, 0); }
    uint32_t RiderOffSound(uint32_t h, uint32_t mode) override {
        return env.CallOracle(rr::sim::kRiderOffSoundFn, h, mode, 0, 0);
    }
    uint32_t Arrest(uint32_t cop, uint32_t bike, uint32_t why) override {
        return env.CallOracle(rr::sim::kArrestFn, cop, bike, why, 0);
    }
    uint32_t PoseInit(uint32_t fn, uint32_t r, uint32_t a1) override { return env.CallOracle(fn, r, a1, 0, 0); }
    uint32_t RiderSync(uint32_t r) override { return env.CallOracle(rr::sim::kRiderSyncFn, r, 0, 0, 0); }
    uint32_t CrashEvent(uint32_t h, uint32_t k) override { return env.CallOracle(rr::sim::kCrashEventFn, h, k, 0, 0); }
    uint32_t LaunchLift(uint32_t dir, uint32_t speed, uint32_t c, uint32_t k) override {
        return env.CallOracle(rr::sim::kLaunchLiftFn, dir, speed, c, k);
    }
    uint32_t Settle(uint32_t r) override { return env.CallOracle(rr::sim::kSettleFn, r, 0, 0, 0); }
};

template <class F>
uint32_t RunRider(NativeEnv& env, F&& f) {
    Memory& m = env.clone->mem;
    rr::sim::GuestRam g(m.ram().data(), env.gp);
    g.SetScratchpad(m.scratchpad().data());
    RiderOracleSeams seams(env);
    rr::sim::RiderLayer layer(g, m.ram().data(), seams);
    const uint32_t v = f(layer);
    if (layer.Failed() && env.failure.empty()) {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "the port refused (view fault %d at 0x%08X)", g.Faulted() ? 1 : 0,
                      g.FaultAddress());
        env.failure = buf;
    }
    return v;
}

// The launch's callees at `depth` (the launch's own entry), its stance event's capture below it.
std::vector<OracleCallee> LaunchCallees(uint32_t depth) {
    constexpr uint32_t kFrLaunch = 80;
    const uint32_t d = depth + kFrLaunch;
    return {OracleCallee{rr::sim::kPoseInitAFn, 2, 0, {}, d}, OracleCallee{rr::sim::kPoseInitBFn, 2, 0, {}, d},
            OracleCallee{rr::sim::kPoseInitCFn, 1, 0, {}, d}, OracleCallee{rr::sim::kPoseInitDFn, 2, 0, {}, d},
            OracleCallee{rr::sim::kPoseInitEFn, 2, 0, {}, d}, OracleCallee{rr::sim::kRiderSyncFn, 1, 0, {}, d},
            OracleCallee{rr::sim::kCrashEventFn, 2, 0, {}, d}, OracleCallee{rr::sim::kLaunchLiftFn, 4, 0, {}, d},
            OracleCallee{rr::sim::kSettleFn, 1, 0, {}, d},
            Capture(d + kFrEvent + kFrGate + kFrTransition + kFrRestart + kFrPoseStart)};
}

// A crash of rider R's bike, on this machine: R and its animation object as the stance rows plant
// them, the bike's scalars captured (derived) or drawn, and the flags the knock-off and the launch
// branch on.
struct CrashCase {
    uint32_t rider = 0, bike = 0;
};
CrashCase PlantCrash(CaseContext& c) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    const StanceCase sc = PlantStanceCase(c, false);
    CrashCase cc;
    cc.rider = sc.rider;
    cc.bike = m.PeekWord(sc.rider + 596u);
    const uint32_t b = cc.bike;
    Put16(m, cc.rider + 0xACu, 0x20u | sc.slot);         // a pool-1 rider (the gate's own test)
    if (c.sample != nullptr && c.sample->entity.size() >= 1096u) {
        // the captured bike's scalars: the box, the frames, direction, velocity and speed
        m.WriteBlock(b + 0xB8u, c.sample->entity.data() + 0xB8, 0x60);
        m.WriteBlock(b + 0x1B0u, c.sample->entity.data() + 0x1B0, 0x34);
        m.WriteBlock(b + 0x204u, c.sample->entity.data() + 0x204, 0x12);
    } else {
        for (uint32_t k = 0; k < 9; ++k) PutS16(m, b + 432u + 2u * k, r.Unit());
        for (uint32_t k = 0; k < 3; ++k) PutS16(m, b + 450u + 2u * k, r.Unit());
        PutS32(m, b + 480u, static_cast<int32_t>(r.U32() % 0x400000u));
    }
    // flagsC (+0x238): the crash kinds the knock-off dispatches on
    {
        const uint32_t q = r.U32() % 8u;
        const uint32_t pick[] = {0u, 0x8000u, 0x400u, 0x20u, 0x40u | 0x20u, 0x100u, 0x80u, 0x40000u | 0x200u};
        uint32_t fc = (r.Next() % 4u == 0) ? r.U32() : pick[q] | (r.U32() & 0x20004u);
        if (r.Next() % 3u == 0) fc |= 0x400u;
        m.PokeWord(b + 568u, fc);
    }
    PutS32(m, b + 676u, r.S32() >> 8);
    PutS32(m, b + 652u, r.S32() >> 8);
    PutS32(m, cc.rider + 480u, static_cast<int32_t>(r.U32() % 0x300000u));
    {
        uint32_t f = m.PeekWord(cc.rider + 552u);
        f = (f & ~0x00268000u) | (r.U32() & 0x00260000u) | 0x8000u;  // bits 17/18 (and 21), the request
        if (r.Next() & 1u) f |= 0x10000u; else f &= ~0x10000u;       // the launch from the knock-off
        m.PokeWord(cc.rider + 552u, f);
    }
    m.PokeWord(cc.rider + 604u, (r.Next() % 8u == 0) ? 2u + Pick(r, 3) : Pick(r, 2)); // still on the bike
    // No passenger (the named bound: no capture has one, and planted ones trap the original's own
    // callees): +0x23C bit 5 clear, and bit 4 - set only with a passenger
    // - clear too, or RiderLaunch case C chases the null +0x358 into the BIOS.
    Put8(m, cc.rider + 572u, m.PeekByte(cc.rider + 572u) & ~0x30u);
    // the bike's AI command stack: a depth the game keeps (0..15), and the riderDef it reads
    Put8(m, b + 0x3B2u, Pick(r, 16));
    const uint32_t rd = m.PeekWord(b + 1084u);
    m.PokeWord(rd + 40u, (r.Next() % 4u == 0) ? r.U32() : 0u);
    Put8(m, rd + 39u, (r.Next() % 5u == 0) ? 248u + Pick(r, 8) : Pick(r, 20));
    Put8(m, rd + 1u, (m.PeekByte(rd + 1u) & 0xF0u) | ((r.Next() % 6u == 0) ? 2u : Pick(r, 2)));
    m.PokeWord(b + 564u, r.U32());
    // the crash camera's inputs: the pool-8 partner, the view record's busy word
    if (r.Next() % 4u == 0) {
        const uint32_t obj = c.scratch + 1024u;
        Put16(m, obj, (8u << 5) | Pick(r, 32));
        Put16(m, obj + 2u, r.U32());
        m.PokeWord(b + 828u, obj);
    } else {
        m.PokeWord(b + 828u, 0);
    }
    const uint32_t gs = m.PeekWord(kGameStatePtr);
    m.PokeWord(gs + 48u, 1u + Pick(r, 2));                          // players
    // a second player's bike when there are two (a null one is read as the kernel word at 0x43C)
    m.PokeWord(0x8005B26Cu, m.PeekWord(kPoolBasePtr) + 1096u * (1u + Pick(r, 17)));
    m.PokeWord(0x800CD898u + 772u, (r.Next() % 4u == 0) ? 1u : 0u); // view 0 busy
    // the police test: a cop player near the bike, now and then
    {
        const uint32_t mode = (r.Next() % 3u == 0) ? ((r.Next() % 5u == 0) ? 33u : (r.U32() | 1u)) : (r.U32() & ~1u);
        Put8(m, gs + 4u, mode);
        m.PokeWord(gs + 60u, Pick(r, 3));
        const uint32_t cop = m.PeekWord(0x8005B268u);
        const uint32_t crd = m.PeekWord(cop + 1084u);
        Put8(m, crd + 1u, (m.PeekByte(crd + 1u) & 0xF0u) | ((r.Next() & 1u) ? 2u : 0u));
        PutS32(m, cop + 480u, static_cast<int32_t>(r.U32() % 0x200000u));
        PutS16(m, cop + 186u, static_cast<int16_t>(GetS16(m, b + 186u) + Range(r, -20, 20)));
        PutS16(m, cop + 194u, static_cast<int16_t>(GetS16(m, b + 194u) + Range(r, -20, 20)));
        m.PokeWord(0x8005B1F8u, Pick(r, 24));
        // The arrest's own body is NOT exercised (the named bound): under Arrest 0x80096F30,
        // 0x8009D664 hands 0x8009AD48 a record in its own frame whose halfword +46 it never writes,
        // and 0x8009AD48 stores it into the spawned police car (`lhu v0,62(s6)` at 0x8009AEAC, `sh
        // v0,322(s2)` at 0x8009AEB8) - a stale stack halfword (on the console the high half of the
        // last return address saved there) that no port running Arrest on the seam can reproduce.
        // The arrest flag 0x8005AD48 set makes Arrest take its own early-out at 0x80096FC0; the call
        // itself and its three arguments are still compared.
        m.PokeWord(0x8005AD48u, 1u + Pick(r, 3));
    }
    // A pc inside the program (the pose initialisers read the op at the pc: 0x8008E868..0x8008E89C).
    m.PokeWord(sc.anim + 12u, Pick(r, 5));
    // The bike's direction along its own up row now and then: the launch's OP(up, dir) is then 0
    // and Normalize answers 0 (the identity reset 0x8008CF74 of case C, 0x80091D90).
    if (r.Next() % 16u == 0)
        for (uint32_t k = 0; k < 3; ++k) Put16(m, b + 450u + 2u * k, Get16(m, b + 522u + 2u * k));
    // The LCG seed (gp+2076): RiderLaunch's case C and the crash camera draw from it.
    m.PokeWord(c.gp + 2076u, r.U32());
    // A player's handle on the rider now and then (the crash camera and the view bits), and the
    // bike's crash direction +0x334 along the slice's axis (|DotLcm| past 0xDDB1: the camera's Rand).
    if (r.Next() & 1u) Put16(m, cc.rider + 0xACu, 0x20u);
    if (r.Next() % 3u == 0) {
        const uint32_t axis = m.PeekWord(b + 340u) + 14u;
        const bool flip = (r.Next() & 1u) != 0;
        for (uint32_t k = 0; k < 3; ++k) {
            const int16_t v = GetS16(m, axis + 2u * k);
            PutS16(m, b + 820u + 2u * k, flip ? static_cast<int16_t>(-v) : v);
        }
    }
    return cc;
}

} // namespace animrows

// The stance event run NATIVELY for a row whose port calls it (the AI command stack, the bike step's
// region E). The call is still WITNESSED: the row keeps 0x800C4550 declared, so the guest's calls are
// observed at its entry, and this appends the port's call - address and arguments - to the port's
// sequence, so a call the port omits, adds, reorders or mis-argues still fails the row; what the call
// then does is this native code on the clone's RAM, compared by the whole-RAM diff. `sp` is the one
// the original makes the call at (recorded, never compared). Declared in verify_physics.cpp.
uint32_t AnimNativeStanceEvent(NativeEnv& env, uint32_t ev, uint32_t rider, uint32_t p, uint32_t sp) {
    Cpu::CallObservation o;
    o.address = rr::sim::kStanceEventFn;
    o.a[0] = ev;
    o.a[1] = rider;
    o.a[2] = p;
    o.sp = sp;
    env.calls.push_back(o);
    return animrows::RunNative(env, [&](rr::sim::StanceLayer& l) { return l.Event(ev, rider, p); });
}

namespace animrows {
} // namespace animrows

void AddAnimRows(std::vector<Row>& rows) {
    using namespace animrows;
    namespace s = rr::sim;

    // ======================================================================== A1: the leaves
    rows.push_back(AnimRow(
        "anim_tick", "s32 Tick(Anim *a, s32 dt)  [RASHCDG, acc += dt; a key per `rate` ticks]", s::kAnimTickFn,
        [](CaseContext& c, Args& args) {
            Rng& r = *c.rng;
            const uint32_t a = PlantObject(c);
            int32_t dt = c.sample ? CapturedDt(r) : Range(r, -40, 400);
            if (c.sample == nullptr && r.Next() % 8u == 0) dt = Range(r, 0, 30);
            // The named bound on the tick's turns: acc + dt < 1000 x rate for a positive rate; a
            // non-positive one drawn below acc, or so large that two turns wrap.
            int32_t rate = GetS32(*c.mem, a + 24u);
            const uint32_t q = r.U32() % 10u;
            if (q == 0) rate = Range(r, -5, 0);
            else if (q == 1) rate = static_cast<int32_t>(0xC0000000u - (r.U32() & 0x0FFFFFFFu)); // wraps at turn 2
            else if (rate <= 0) rate = 10; // a captured unused object
            PutS32(*c.mem, a + 24u, rate);
            int32_t acc;
            if (rate > 0) acc = Range(r, 0, rate * 4) - ((r.Next() & 1u) ? 0 : dt);
            else if (q == 0) acc = rate - 1 - (dt > 0 ? dt : 0) - Range(r, 0, 100);
            else acc = Range(r, 0, 1000);
            if (rate > 0 && static_cast<int64_t>(acc) + dt > 1000ll * rate) acc = rate;
            PutS32(*c.mem, a + 28u, acc);
            if (r.Next() % 6u == 0) c.mem->PokeWord(a + 16u, r.U32()); // the frame wraps
            args.a[0] = a;
            args.a[1] = static_cast<uint32_t>(dt);
        },
        [](NativeEnv& env, const Args& a) {
            return RunNative(env, [&](s::StanceLayer& l) { return l.anim().Tick(a.a[0], static_cast<int32_t>(a.a[1])); });
        },
        true));

    rows.push_back(AnimRow(
        "anim_seq", "s32 Seq(Anim *a)  [RASHCDG, jump/skip/stop from the pc; the guard counts from the pc]",
        s::kAnimSeqFn,
        [](CaseContext& c, Args& args) {
            Rng& r = *c.rng;
            const uint32_t a = PlantObject(c);
            if (c.sample == nullptr || r.Next() % 2u == 0) RandomProgram(c, a, true);
            const uint32_t q = r.U32() % 10u;
            PutS32(*c.mem, a + 8u, q < 7 ? 5 : q == 7 ? Range(r, -2, 0) : Range(r, 1, 9));
            PutS32(*c.mem, a + 12u, (r.Next() % 8u != 0u) ? Range(r, 0, 5) : Range(r, -1, 8));
            args.a[0] = a;
        },
        [](NativeEnv& env, const Args& a) {
            return RunNative(env, [&](s::StanceLayer& l) { return l.anim().Seq(a.a[0]); });
        },
        true));

    rows.push_back(AnimRow(
        "anim_clip_done", "s32 ClipDone(Anim *a)  [RASHCDG, 'the clip is finished': frame >= last]",
        s::kAnimClipDoneFn,
        [](CaseContext& c, Args& args) {
            Rng& r = *c.rng;
            const uint32_t a = PlantObject(c);
            if (r.Next() % 3u == 0) {
                // exactly at, one before and one past the last key of the op at the pc
                const uint32_t op = c.mem->PeekWord(a + 4u) + 12u * (c.mem->PeekWord(a + 12u) % 5u);
                c.mem->PokeWord(a + 12u, (op - c.mem->PeekWord(a + 4u)) / 12u);
                Put8(*c.mem, op + 1u, 1);
                PutS32(*c.mem, a + 16u, GetS16(*c.mem, op + 6u) + Range(r, -1, 1));
            }
            args.a[0] = a;
        },
        [](NativeEnv& env, const Args& a) {
            return RunNative(env, [&](s::StanceLayer& l) { return l.anim().ClipDone(a.a[0]); });
        },
        true));

    rows.push_back(AnimRow(
        "anim_channel_free", "s32 ChannelFree(Anim *a)  [RASHCDG, 'a queued transition may start']",
        s::kAnimChannelFreeFn,
        [](CaseContext& c, Args& args) {
            Rng& r = *c.rng;
            const uint32_t a = PlantObject(c);
            if (r.Next() % 2u == 0) PutS32(*c.mem, a + 12u, Range(r, 0, 2));
            args.a[0] = a;
        },
        [](NativeEnv& env, const Args& a) {
            return RunNative(env, [&](s::StanceLayer& l) { return l.anim().ChannelFree(a.a[0]); });
        },
        true));

    rows.push_back(AnimRow(
        "anim_stop", "u32 Stop(Anim *a)  [RASHCDG, flags &= ~2]", s::kAnimStopFn,
        [](CaseContext& c, Args& args) { args.a[0] = PlantObject(c); },
        [](NativeEnv& env, const Args& a) {
            return RunNative(env, [&](s::StanceLayer& l) { return l.anim().Stop(a.a[0]); });
        },
        true));
    rows.push_back(AnimRow(
        "anim_stop_if_playing", "u32 StopIfPlaying(Anim *a)  [RASHCDG]", s::kAnimStopIfPlayingFn,
        [](CaseContext& c, Args& args) { args.a[0] = PlantObject(c); },
        [](NativeEnv& env, const Args& a) {
            return RunNative(env, [&](s::StanceLayer& l) { return l.anim().StopIfPlaying(a.a[0]); });
        },
        true));
    rows.push_back(AnimRow(
        "anim_resume", "u32 Resume(Anim *a)  [RASHCDG, acc := 0, flags |= 2]", s::kAnimResumeFn,
        [](CaseContext& c, Args& args) { args.a[0] = PlantObject(c); },
        [](NativeEnv& env, const Args& a) {
            return RunNative(env, [&](s::StanceLayer& l) { return l.anim().Resume(a.a[0]); });
        },
        true));

    rows.push_back(AnimRow(
        "anim_quat_get", "u16 QuatGet(Anim *a, s16 out[4])  [RASHCDG, the root part's channels 3..6]",
        s::kAnimQuatGetFn,
        [](CaseContext& c, Args& args) {
            const uint32_t a = PlantObject(c);
            if (c.sample == nullptr)
                for (uint32_t k = 0; k < 4; ++k) Put16(*c.mem, a + 128u + 24u * k, c.rng->U32());
            args.a[0] = a;
            args.a[1] = c.scratch + 64u + 2u * Pick(*c.rng, 8);
        },
        [](NativeEnv& env, const Args& a) {
            return RunNative(env, [&](s::StanceLayer& l) { return l.anim().QuatGet(a.a[0], a.a[1]); });
        },
        true));
    rows.push_back(AnimRow(
        "anim_quat_set", "u16 QuatSet(Anim *a, const s16 in[4])  [RASHCDG]", s::kAnimQuatSetFn,
        [](CaseContext& c, Args& args) {
            const uint32_t a = PlantObject(c);
            const uint32_t in = c.scratch + 64u + 2u * Pick(*c.rng, 8);
            for (uint32_t k = 0; k < 4; ++k) Put16(*c.mem, in + 2u * k, c.rng->U32());
            args.a[0] = a;
            args.a[1] = in;
        },
        [](NativeEnv& env, const Args& a) {
            return RunNative(env, [&](s::StanceLayer& l) { return l.anim().QuatSet(a.a[0], a.a[1]); });
        },
        true));

    rows.push_back(AnimRow(
        "anim_bank_switch", "s32 BankSwitch(Anim *a, Bank *b)  [SLUS 0x80012858, T.sampled := 0, op 0 := stop]",
        s::kAnimBankSwitchFn,
        [](CaseContext& c, Args& args) {
            Rng& r = *c.rng;
            const uint32_t a = PlantObject(c);
            Put8(*c.mem, c.mem->PeekWord(a) + 9u, r.U32());
            args.a[0] = a;
            args.a[1] = (r.Next() % 4u != 0u) ? BankPtr(*c.mem, Pick(r, 8)) : r.U32();
        },
        [](NativeEnv& env, const Args& a) {
            return RunNative(env, [&](s::StanceLayer& l) { return l.anim().BankSwitch(a.a[0], a.a[1]); });
        },
        true));

    rows.push_back(AnimRow(
        "anim_seat_release", "Rider *SeatRelease(Bike *b, Rider *r, s32 idx)  [RASHCDG 0x80068D20]",
        s::kSeatReleaseFn,
        [](CaseContext& c, Args& args) {
            Memory& m = *c.mem;
            Rng& r = *c.rng;
            const uint32_t bike = m.PeekWord(kPoolBasePtr) + 1096u * Pick(r, 18);
            const uint32_t rider = m.PeekWord(ObjAt(m, Pick(r, 18))); // a live rider
            const uint32_t idx = (r.Next() % 8u != 0u) ? Pick(r, 2) : Pick(r, 4);
            for (uint32_t k = 0; k < 4; ++k) {
                const uint32_t q = r.U32() % 3u;
                m.PokeWord(bike + 56u + 8u * k, q == 0 ? rider : q == 1 ? 0u : m.PeekWord(ObjAt(m, Pick(r, 18))));
                m.PokeWord(bike + 60u + 8u * k, r.U32());
            }
            if (r.Next() & 1u) m.PokeWord(bike + 56u + 8u * idx, rider);
            args.a[0] = bike;
            args.a[1] = rider;
            args.a[2] = idx;
        },
        [](NativeEnv& env, const Args& a) {
            return RunNative(env, [&](s::StanceLayer& l) { return l.anim().SeatRelease(a.a[0], a.a[1], a.a[2]); });
        },
        true));

    // ======================================================================== A2: the clock
    rows.push_back(AnimRow(
        "anim_decode_header", "s32 DecodeHeader(Anim *a, Track *T)  [RASHCDG, the DMD3 channel headers]",
        s::kAnimDecodeHeaderFn,
        [](CaseContext& c, Args& args) {
            Memory& m = *c.mem;
            Rng& r = *c.rng;
            const uint32_t a = PlantObject(c);
            const uint32_t q = r.U32() % 10u;
            if (c.sample == nullptr && q < 3) {
                // A synthetic DMD3 block in the scratch (the named bounds: lengths >= 0, widths 0..7).
                const uint32_t blk = c.scratch + 256u;
                const uint32_t parts = Pick(r, 21), keys = 1u + Pick(r, 20);
                const uint32_t flags = r.U32() & 0xFFu;
                std::vector<uint8_t> z(7000, 0);
                m.WriteBlock(blk, z.data(), static_cast<uint32_t>(z.size()));
                uint32_t p = blk + 24u;
                if (flags & 1u)
                    for (uint32_t i = 0; i < 3u * keys; ++i, p += 2) Put16(m, p, r.U32());
                if (flags & 2u) {
                    // {s16 n; w1; w2; w3; stream...}: n halfwords after the count, the next record
                    // 2n + 2 bytes on. n >= 3: a shorter record would take its width from the next
                    // record's random bytes (the width bound above).
                    for (uint32_t i = 0; i < 4u * parts; ++i) {
                        const uint32_t n = 3u + Pick(r, 7);
                        Put16(m, p, n);
                        Put16(m, p + 2, r.U32());
                        Put16(m, p + 4, r.U32());
                        Put16(m, p + 6, (r.U32() & 0x80FFu) | (Pick(r, 8) << 8));
                        for (uint32_t k = 3; k < n; ++k) Put16(m, p + 2 + 2 * k, r.U32());
                        p += 2u * n + 2u;
                    }
                    p += 8u; // the last record's w1..w3 may lie past its count
                } else {
                    for (uint32_t i = 0; i < 4u * parts * keys; ++i, p += 2) Put16(m, p, r.U32());
                }
                const uint32_t size = (p - blk) + ((r.Next() % 4u == 0) ? Pick(r, 40) : Pick(r, 3));
                m.PokeWord(blk + 4u, size);
                Put8(m, blk + 13u, flags);
                Put8(m, blk + 15u, parts);
                Put16(m, blk + 16u, keys);
                m.PokeWord(a + 44u, blk);
            } else if (c.sample == nullptr) {
                const uint32_t bank = BankPtr(m, Pick(r, kResidentBanks));
                m.PokeWord(a + 44u, ClipAt(m, bank, Pick(r, BankClips(m, bank))));
            }
            args.a[0] = a;
            args.a[1] = a + 48u;
        },
        [](NativeEnv& env, const Args& a) {
            return RunNative(env, [&](s::StanceLayer& l) { return l.anim().DecodeHeader(a.a[0], a.a[1]); });
        },
        true));

    rows.push_back(AnimRow(
        "anim_clip_select", "Clip *ClipSelect(Anim *a, u32 clip)  [RASHCDG, decode unless the same clip untouched]",
        s::kAnimClipSelectFn,
        [](CaseContext& c, Args& args) {
            Memory& m = *c.mem;
            Rng& r = *c.rng;
            const uint32_t a = PlantObject(c);
            const uint32_t bank = m.PeekWord(a + 40u);
            uint32_t idx = Pick(r, BankClips(m, bank));
            if (r.Next() % 3u == 0) {
                idx = m.PeekByte(m.PeekWord(a + 44u) + 14u); // the current clip
                if (r.Next() & 1u) PutS16(m, a + 48u, -1);
            }
            if (r.Next() % 6u == 0) m.PokeWord(a + 44u, 0);
            args.a[0] = a;
            args.a[1] = idx | ((r.Next() & 1u) ? (r.U32() & 0xFFFFFF00u) : 0u); // only the low byte counts
        },
        [](NativeEnv& env, const Args& a) {
            return RunNative(env, [&](s::StanceLayer& l) { return l.anim().ClipSelect(a.a[0], a.a[1]); });
        },
        true));

    rows.push_back(AnimRow(
        "anim_loop_restart", "ex LoopRestart(Anim *a)  [RASHCDG, a loop's wrap: frame = acc = 0]",
        s::kAnimLoopRestartFn,
        [](CaseContext& c, Args& args) {
            const uint32_t a = PlantObject(c, false);
            PutS32(*c.mem, a + 12u, static_cast<int32_t>(Pick(*c.rng, 5)));
            args.a[0] = a;
        },
        [](NativeEnv& env, const Args& a) {
            return RunNative(env, [&](s::StanceLayer& l) { return l.anim().LoopRestart(a.a[0]); });
        },
        true));

    auto clockDt = [](CaseContext& c, uint32_t a) {
        // dt within the named bound: < 1000 keys at the object's rate.
        Rng& r = *c.rng;
        int32_t dt = c.sample ? CapturedDt(r) : Range(r, -20, 300);
        if (r.Next() % 5u == 0) dt = Range(r, 0, 15);
        const int32_t rate = GetS32(*c.mem, a + 24u);
        if (rate <= 0) PutS32(*c.mem, a + 24u, 10);
        const int32_t acc = GetS32(*c.mem, a + 28u);
        if (acc < -1000 || acc > 100000) PutS32(*c.mem, a + 28u, 0);
        return dt;
    };

    rows.push_back(AnimRow(
        "anim_advance", "s32 Advance(Anim *a, s32 dt)  [RASHCDG, tick + 'is the op over' (PAST the last key)]",
        s::kAnimAdvanceFn,
        [clockDt](CaseContext& c, Args& args) {
            Rng& r = *c.rng;
            const uint32_t a = PlantObject(c, true);
            if (r.Next() % 3u == 0) { // at the end of the op at the pc
                const uint32_t pc = c.mem->PeekWord(a + 12u) % 5u;
                c.mem->PokeWord(a + 12u, pc);
                const uint32_t op = c.mem->PeekWord(a + 4u) + 12u * pc;
                const uint8_t code = c.mem->PeekByte(op + 1u);
                const int32_t keys = static_cast<int32_t>(KeysOf(*c.mem, c.mem->PeekWord(a + 44u)));
                if (code == 0) PutS32(*c.mem, a + 16u, keys - 1 + Range(r, -1, 1));
                else if (code == 1) PutS32(*c.mem, a + 16u, GetS16(*c.mem, op + 6u) + Range(r, -1, 1));
                else if (code == 3) {
                    PutS32(*c.mem, a + 16u, GetS16(*c.mem, op + 4u) + Range(r, -2, 0));
                    if (r.Next() & 1u) Put8(*c.mem, op + 0u, 1);                         // the new clip plays once
                    if (r.Next() & 1u) Put8(*c.mem, op + 2u, c.mem->PeekByte(op + 2u) | 4u); // rewrite op 0
                }
                if (code == 0 && (r.Next() & 1u)) c.mem->PokeWord(a + 20u, Pick(r, 3));
            }
            args.a[0] = a;
            args.a[1] = static_cast<uint32_t>(clockDt(c, a));
        },
        [](NativeEnv& env, const Args& a) {
            return RunNative(env, [&](s::StanceLayer& l) { return l.anim().Advance(a.a[0], static_cast<int32_t>(a.a[1])); });
        },
        true));

    rows.push_back(AnimRow(
        "anim_pose_start", "s32 PoseStart(Anim *a)  [RASHCDG, prime the op: clip, rate, first key]",
        s::kAnimPoseStartFn,
        [](CaseContext& c, Args& args) {
            const uint32_t a = PlantObject(c, true);
            PutS32(*c.mem, a + 12u, static_cast<int32_t>(Pick(*c.rng, 5)));
            if (c.rng->Next() % 10u == 0) {
                // A clip whose DMD3 RATE is 0 (read as 10): planted in the header of the op's clip.
                const uint32_t op = c.mem->PeekWord(a + 4u) + 12u * c.mem->PeekWord(a + 12u);
                Put16(*c.mem, ClipAt(*c.mem, c.mem->PeekWord(a + 40u), c.mem->PeekByte(op)) + 18u, 0);
            }
            args.a[0] = a;
        },
        [](NativeEnv& env, const Args& a) {
            return RunNative(env, [&](s::StanceLayer& l) { return l.anim().PoseStart(a.a[0]); });
        },
        true, {Capture(kFrPoseStart)}));

    rows.push_back(AnimRow(
        "anim_advance_frame", "s32 AdvanceFrame(Anim *a, s32 dt)  [RASHCDG, the op over -> next op]",
        s::kAnimAdvanceFrameFn,
        [clockDt](CaseContext& c, Args& args) {
            Rng& r = *c.rng;
            const uint32_t a = PlantObject(c, true);
            PutS32(*c.mem, a + 12u, static_cast<int32_t>(Pick(r, 5)));
            if (r.Next() % 2u == 0) { // most ops end in this call
                const uint32_t op = c.mem->PeekWord(a + 4u) + 12u * c.mem->PeekWord(a + 12u);
                const uint8_t code = c.mem->PeekByte(op + 1u);
                if (code == 1) PutS32(*c.mem, a + 16u, GetS16(*c.mem, op + 6u) + 1);
                if (code == 3) PutS32(*c.mem, a + 16u, GetS16(*c.mem, op + 4u));
            }
            args.a[0] = a;
            args.a[1] = static_cast<uint32_t>(clockDt(c, a));
        },
        [](NativeEnv& env, const Args& a) {
            return RunNative(env, [&](s::StanceLayer& l) { return l.anim().AdvanceFrame(a.a[0], static_cast<int32_t>(a.a[1])); });
        },
        true, {Capture(kFrAdvanceFrame + kFrPoseStart)}));

    rows.push_back(AnimRow(
        "anim_restart", "u32 Restart(Anim *a)  [RASHCDG, flags &= 0xE5, Seq, PoseStart]", s::kAnimRestartFn,
        [](CaseContext& c, Args& args) {
            Rng& r = *c.rng;
            const uint32_t a = PlantObject(c, true);
            if (r.Next() % 8u == 0) PutS32(*c.mem, a + 8u, Range(r, -1, 1)); // no program length
            args.a[0] = a;
        },
        [](NativeEnv& env, const Args& a) {
            return RunNative(env, [&](s::StanceLayer& l) { return l.anim().Restart(a.a[0]); });
        },
        true, {Capture(kFrRestart + kFrPoseStart)}));

    auto startArgs = [](CaseContext& c, Args& args, uint32_t a) {
        Memory& m = *c.mem;
        Rng& r = *c.rng;
        const uint32_t bank = m.PeekWord(a + 40u);
        args.a[0] = a;
        args.a[1] = Pick(r, BankClips(m, bank));
        args.a[2] = (r.Next() & 1u) ? r.U32() : (r.U32() & 0xFFu);
        args.a[3] = (r.Next() % 3u == 0) ? 0u : r.U32();
    };
    rows.push_back(AnimRow(
        "anim_hard_start", "u32 HardStart(Anim *a, clip, flags, rate, [ex])  [RASHCDG, op1 0..keys-1, op5]",
        s::kAnimHardStartFn,
        [startArgs](CaseContext& c, Args& args) {
            const uint32_t a = PlantObject(c, false);
            startArgs(c, args, a);
            args.a4 = SomeEx(c);
        },
        [](NativeEnv& env, const Args& a) {
            return RunNative(env, [&](s::StanceLayer& l) { return l.anim().HardStart(a.a[0], a.a[1], a.a[2], a.a[3], a.a4); });
        },
        true));
    rows.push_back(AnimRow(
        "anim_ranged_start", "u32 RangedStart(Anim *a, clip, flags, first, [last, rate, ex])  [RASHCDG]",
        s::kAnimRangedStartFn,
        [startArgs](CaseContext& c, Args& args) {
            Rng& r = *c.rng;
            const uint32_t a = PlantObject(c, false);
            startArgs(c, args, a);
            const int32_t keys = static_cast<int32_t>(KeysOf(*c.mem, ClipAt(*c.mem, c.mem->PeekWord(a + 40u), args.a[1])));
            args.a[3] = static_cast<uint32_t>(Range(r, -2, keys + 2)); // first
            args.a4 = static_cast<uint32_t>(Range(r, -2, keys + 2));   // last
            args.more[0] = r.U32();                                    // rate
            args.more[1] = SomeEx(c);                                  // ex
        },
        [](NativeEnv& env, const Args& a) {
            return RunNative(env, [&](s::StanceLayer& l) {
                return l.anim().RangedStart(a.a[0], a.a[1], a.a[2], a.a[3], a.a4, a.more[0], a.more[1]);
            });
        },
        true));
    rows.push_back(AnimRow(
        "anim_loop_start", "u32 LoopStart(Anim *a, clip, flags, rate, [ex])  [RASHCDG, op0 x 0xFFFF(-1), op2 -> 0]",
        s::kAnimLoopStartFn,
        [startArgs](CaseContext& c, Args& args) {
            const uint32_t a = PlantObject(c, false);
            startArgs(c, args, a);
            args.a4 = SomeEx(c);
        },
        [](NativeEnv& env, const Args& a) {
            return RunNative(env, [&](s::StanceLayer& l) { return l.anim().LoopStart(a.a[0], a.a[1], a.a[2], a.a[3], a.a4); });
        },
        true));

    rows.push_back(AnimRow(
        "anim_transition",
        "u32 Transition(Anim *a, clip, queued, flags, [once, b7, rate, ex])  [RASHCDG, blend 5 keys]",
        s::kAnimTransitionFn,
        [](CaseContext& c, Args& args) {
            Memory& m = *c.mem;
            Rng& r = *c.rng;
            const uint32_t a = PlantObject(c, true);
            const uint32_t bank = m.PeekWord(a + 40u);
            // The running op 0 must name a valid clip (queued: its key count is read).
            Put8(m, m.PeekWord(a + 4u), Pick(r, BankClips(m, bank)));
            const uint32_t q = r.U32() % 10u;
            args.a[0] = a;
            args.a[1] = Pick(r, BankClips(m, bank));
            args.a[2] = q < 4 ? 0u : q < 8 ? 1u : q == 8 ? 2u : r.U32();
            args.a[3] = (r.Next() & 1u) ? r.U32() : (r.U32() & 0xFFu);
            args.a4 = (r.Next() & 1u) ? Pick(r, 2) : r.U32();
            args.more[0] = (r.Next() & 1u) ? Pick(r, 2) : r.U32();
            args.more[1] = r.U32();
            args.more[2] = SomeEx(c);
        },
        [](NativeEnv& env, const Args& a) {
            return RunNative(env, [&](s::StanceLayer& l) {
                return l.anim().Transition(a.a[0], a.a[1], a.a[2], a.a[3], a.a4, a.more[0], a.more[1], a.more[2]);
            });
        },
        true, {Capture(kFrTransition + kFrRestart + kFrPoseStart)}));

    // ======================================================================== A3: the pass
    {
        Row r = AnimRow(
            "anim_pass", "void AnimationPass(Desc *d)  [RASHCDG, once a frame: pose (seam), then the clock]",
            s::kAnimPassFn,
            [](CaseContext& c, Args& args) {
                Memory& m = *c.mem;
                Rng& r = *c.rng;
                const std::vector<CapturedImage>& imgs = Images();
                int32_t dt = 5;
                if (!imgs.empty()) {
                    const CapturedImage& img = imgs[Pick(r, static_cast<uint32_t>(imgs.size()))];
                    dt = img.dt;
                    for (uint32_t slot = 0; slot < 23; ++slot) {
                        const uint32_t a = ObjAt(m, slot);
                        const uint32_t owner = m.PeekWord(a);
                        if (owner == 0u) { m.PokeWord(a + 36u, m.PeekWord(a + 36u) & ~2u); continue; }
                        if (slot >= img.objs.size() || img.objs[slot].obj.empty()) continue;
                        PlantCaptured(m, a, img.objs[slot]);
                        Put8(m, owner + 9u, img.objs[slot].owner[9]);
                    }
                }
                if (c.sample == nullptr) {
                    // The clock perturbed, the pose state (clip, channels, blend buffer) kept as captured.
                    const int32_t dts[] = {5, 10, 15, 30, 0};
                    dt = (r.Next() % 3u == 0) ? Range(r, 0, 90) : dts[Pick(r, 5)];
                    for (uint32_t slot = 0; slot < 21; ++slot) {
                        const uint32_t a = ObjAt(m, slot);
                        uint32_t f = m.PeekWord(a + 36u);
                        if (r.Next() % 4u == 0) f ^= 2u;
                        if (r.Next() % 4u == 0) f ^= 8u;
                        if (r.Next() % 3u == 0) f ^= 0x10u;
                        if (r.Next() % 4u == 0) f ^= 4u;
                        m.PokeWord(a + 36u, f);
                        const int32_t rate = Range(r, 1, 60);
                        if (r.Next() & 1u) PutS32(m, a + 24u, rate);
                        PutS32(m, a + 28u, Range(r, 0, 2 * GetS32(m, a + 24u)));
                        if (r.Next() & 1u) PutS32(m, a + 16u, GetS32(m, a + 16u) + Range(r, -1, 3));
                        if (r.Next() % 4u == 0) PutS32(m, a + 12u, static_cast<int32_t>(Pick(r, 3)));
                        const uint32_t owner = m.PeekWord(a);
                        Put8(m, owner + 9u, (m.PeekByte(owner + 9u) & ~3u) | Pick(r, 4));
                    }
                    const uint32_t q = r.U32() % 20u;
                    if (q == 0) m.PokeWord(kDesc + 8u, 0);
                    else if (q == 1) PutS32(m, kDesc + 12u, Range(r, -2, 22));
                }
                m.PokeWord(m.PeekWord(kGameStatePtr) + 28u, static_cast<uint32_t>(dt));
                args.a[0] = kDesc;
            },
            [](NativeEnv& env, const Args& a) {
                return RunNative(env, [&](s::StanceLayer& l) { return l.anim().Pass(a.a[0]); });
            },
            true,
            {OracleCallee{s::kAnimApplyFrameFn, 1, 0, {}, kFrPass},
             Capture(kFrPass + kFrAdvanceFrame + kFrPoseStart)});
        r.maxSteps = 50'000'000;
        rows.push_back(r);
    }
    // ======================================================================== the stance layer
    rows.push_back(AnimRow(
        "stance_set_rider_state", "u32 SetRiderState(ev, Entity *e, p)  [RASHCDG 0x800C2FF4, a leaf]",
        s::kSetRiderStateFn,
        [](CaseContext& c, Args& args) {
            const StanceCase sc = PlantStanceCase(c, true);
            args.a[0] = (c.rng->Next() % 4u == 0) ? c.rng->U32() : sc.ev;
            args.a[1] = sc.rider;
            args.a[2] = sc.p;
        },
        [](NativeEnv& env, const Args& a) {
            return RunNative(env, [&](s::StanceLayer& l) { return l.SetRiderState(a.a[0], a.a[1], a.a[2]); });
        },
        true));

    rows.push_back(AnimRow(
        "stance_path", "u16 StancePath(Rider *r, u16 ev)  [RASHCDG 0x800C37B0, a leaf, never entered in a run]",
        s::kStancePathFn,
        [](CaseContext& c, Args& args) {
            Rng& r = *c.rng;
            const StanceCase sc = PlantStanceCase(c, true);
            if (r.Next() % 3u == 0) Put16(*c.mem, sc.rider + 0x220u, Pick(r, 32));
            args.a[0] = sc.rider;
            args.a[1] = (r.Next() % 4u == 0) ? (Pick(r, 40) | (r.U32() & 0xFFFF0000u)) : sc.ev;
        },
        [](NativeEnv& env, const Args& a) {
            return RunNative(env, [&](s::StanceLayer& l) { return l.Path(a.a[0], a.a[1]); });
        },
        true));

    rows.push_back(AnimRow(
        "stance_leave", "s32 StanceLeave(ev, Rider *r, p)  [RASHCDG 0x800C4500]", s::kStanceLeaveFn,
        [](CaseContext& c, Args& args) {
            const StanceCase sc = PlantStanceCase(c, true);
            if (c.rng->Next() % 3u == 0) Put16(*c.mem, sc.rider + 0x220u, 92u + Pick(*c.rng, 80)); // category 3
            args.a[0] = sc.ev;
            args.a[1] = sc.rider;
            args.a[2] = sc.p;
        },
        [](NativeEnv& env, const Args& a) {
            return RunNative(env, [&](s::StanceLayer& l) { return l.Leave(a.a[0], a.a[1], a.a[2]); });
        },
        true, {Leave(kFrLeave)}));

    rows.push_back(AnimRow(
        "stance_enter", "s32 StanceEnter(ev, Rider *r, p)  [RASHCDG 0x800C4454]", s::kStanceEnterFn,
        [](CaseContext& c, Args& args) {
            StanceCase sc = PlantStanceCase(c, true);
            if (c.rng->Next() % 4u == 0) sc.ev = 92u + Pick(*c.rng, 80); // category 3
            args.a[0] = sc.ev;
            args.a[1] = sc.rider;
            args.a[2] = sc.p;
        },
        [](NativeEnv& env, const Args& a) {
            return RunNative(env, [&](s::StanceLayer& l) { return l.Enter(a.a[0], a.a[1], a.a[2]); });
        },
        true, {Enter(kFrEnter)}));

    {
        Row r = AnimRow(
            "stance_gate", "s32 StanceGate(ev, Rider *r, u32 *p)  [RASHCDG 0x800C3E9C, accept + start the clip]",
            s::kStanceGateFn,
            [](CaseContext& c, Args& args) {
                const StanceCase sc = PlantStanceCase(c, true);
                const uint32_t pp = c.scratch + 16u;
                c.mem->PokeWord(pp, sc.p);
                args.a[0] = sc.ev;
                args.a[1] = sc.rider;
                args.a[2] = pp;
            },
            [](NativeEnv& env, const Args& a) {
                return RunNative(env, [&](s::StanceLayer& l) { return l.Gate(a.a[0], a.a[1], a.a[2]); });
            },
            true, {Capture(kFrGate + kFrTransition + kFrRestart + kFrPoseStart)});
        r.maxSteps = 20'000'000;
        rows.push_back(r);
    }

    {
        Row r = AnimRow(
            "stance_event", "s32 StanceEvent(u32 ev, Rider *r, u32 p)  [RASHCDG 0x800C4550, gate + commit]",
            s::kStanceEventFn,
            [](CaseContext& c, Args& args) {
                const StanceCase sc = PlantStanceCase(c, true);
                args.a[0] = sc.ev;
                args.a[1] = sc.rider;
                args.a[2] = sc.p;
            },
            [](NativeEnv& env, const Args& a) {
                return RunNative(env, [&](s::StanceLayer& l) { return l.Event(a.a[0], a.a[1], a.a[2]); });
            },
            true,
            {Capture(kFrEvent + kFrGate + kFrTransition + kFrRestart + kFrPoseStart), Leave(kFrEvent + kFrLeave),
             Enter(kFrEvent + kFrEnter)});
        r.maxSteps = 50'000'000;
        rows.push_back(r);
    }
    // ======================================================================== the knock-off and the launch
    {
        Row r = AnimRow(
            "rider_launch", "u32 RiderLaunch(Rider *r)  [RASHCDG 0x80091468, the 54-entry jump table by stance]",
            s::kRiderLaunchFn,
            [](CaseContext& c, Args& args) {
                Rng& rg = *c.rng;
                const CrashCase cc = PlantCrash(c);
                const uint32_t q = rg.U32() % 12u;
                const uint32_t st[] = {38, 39, 40, 41, 42, 89, 88, 90, 91, 43u + Pick(rg, 45), 11, 224};
                Put16(*c.mem, cc.rider + 0x220u, st[q]);
                args.a[0] = cc.rider;
                args.a[1] = rg.U32(); // RiderLaunch's own a1: dead (POSE_C never reads it)
            },
            [](NativeEnv& env, const Args& a) {
                return RunRider(env, [&](s::RiderLayer& l) { return l.Launch(a.a[0]); });
            },
            true, LaunchCallees(0));
        r.maxSteps = 50'000'000;
        rows.push_back(r);
    }
    {
        std::vector<OracleCallee> callees = LaunchCallees(48);
        callees.push_back(OracleCallee{s::kTakedownFn, 1, 0, {}, 48});
        callees.push_back(OracleCallee{s::kRiderOffSoundFn, 2, 0, {}, 48});
        callees.push_back(OracleCallee{s::kArrestFn, 3, 0, {}, 48});
        callees.push_back(Leave(48 + kFrEvent + kFrLeave));
        callees.push_back(Enter(48 + kFrEvent + kFrEnter));
        Row r = AnimRow(
            "rider_knock_off", "u32 RiderKnockOff(Rider *r)  [RASHCDG 0x80090D84, the fall stance + the police test]",
            s::kRiderKnockOffFn,
            [](CaseContext& c, Args& args) {
                const CrashCase cc = PlantCrash(c);
                args.a[0] = cc.rider;
            },
            [](NativeEnv& env, const Args& a) {
                return RunRider(env, [&](s::RiderLayer& l) { return l.KnockOff(a.a[0]); });
            },
            true, callees);
        r.maxSteps = 50'000'000;
        rows.push_back(r);
    }
}
