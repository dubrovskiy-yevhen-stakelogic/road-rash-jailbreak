// Bench rows for the engine note. Included by verify_physics.cpp inside
// its anonymous namespace, so everything defined above that point is visible here.
//
// Every row here is a GUEST-ADDRESS port (src\game\sim\sound_engine.h): the native side runs
// straight on the clone's RAM through `rr::sim::GuestRam`, and reaches the hardware through
// `EnIo`, which answers from the clone's OWN `Cpu::ConcessionLoad/Store` - the same code the
// interpreter answers the guest's loads and stores from. So a register the guest reads back is
// read back from the same kind of register file on both sides, and `compareSpuControl` then
// compares the two files after the case.
//
// Named bounds on the input families, each one a range the ORIGINAL's own code produces:
//   * libspu's base pointer `*(0x8005A41C)` stays 0x1F801C00 and GetRCnt's `*(0x800549B8)` stays
//     0x1F801100 (their values in every capture; nothing in the game writes either after init);
//   * a voice's channel field `+0x1C` is 0..31, so every register write `SpuSetVoiceAttr` makes
//     lands in the SPU's own window - channels 24..31 land ON the control block (0x1F801D80+),
//     which is exactly what the register file is for, and are driven one voice in six;
//   * the voice lists, the ring indices and the bank count stay in the ranges `PlantSoundState`
//     documents (the original bounds none of them);
//   * `*(0x8005A440) != 0` comes with a non-zero `*(0x8005A448)`: the other combination is the
//     console's own `break 7` at 0x8004F0D4.

namespace en {
constexpr uint32_t kEngineStartFn = 0x800167A4, kEngineNoteFn = 0x800169D0, kRoadNoteFn = 0x80016E4C;
constexpr uint32_t kStopVoiceFn = 0x8001F7EC, kSetLoopFn = 0x8001F874, kPitchModFn = 0x8001F900;
constexpr uint32_t kGetPitchFn = 0x8001F934, kReleaseFn = 0x8001FB58, kUpdateVoiceFn = 0x80019C54;
constexpr uint32_t kUpdateVoice2Fn = 0x8001F6A4, kAnyVoiceFn = 0x800506A8, kGetRCntFn = 0x80043F00;
constexpr uint32_t kFxBurstFn = 0x80027778, kFxSprayFn = 0x80027974;
constexpr uint32_t kSetKeyFn = 0x80050D08, kProgramFn = 0x8001EB7C, kServiceFn = 0x8001EE94;
constexpr uint32_t kTickFn = 0x80019990, kSetupFn = 0x8001654C, kResetFn = 0x80019D9C;
constexpr uint32_t kNoteFrame = 96;  // `addiu sp,sp,-96` at 0x800169D0 and at 0x80016E4C

// The scratch block's layout for a fully planted engine world (8192 bytes).
constexpr uint32_t kTable = 0x0000;     // bank pointer table, 16 slots
constexpr uint32_t kBankEng = 0x0040;   // the engine bank (5 sounds, 512-byte window)
constexpr uint32_t kBankRoad = 0x0240;  // the road bank (25 sounds, 768)
constexpr uint32_t kBankDef = 0x0540;   // the default bank (120 sounds, 512 - most offsets 0)
constexpr uint32_t kVoices = 0x0740;    // 32 voice records x 44 = 1408
constexpr uint32_t kEng = 0x0CC0;       // two engine records x 132
constexpr uint32_t kListen = 0x0DD0;    // four listener records x 72
constexpr uint32_t kParams = 0x0EF0;    // the 20-byte parameter record
constexpr uint32_t kSndPar = 0x0F08;    // a SoundParams record for the UpdateVoice rows
constexpr uint32_t kRider = 0x0F20;     // 0x260 bytes
constexpr uint32_t kStats = 0x1180;     // 0x1C0 bytes (EngineSetup reads up to +0x190)
constexpr uint32_t kBike = 0x1400;      // 1096 bytes
constexpr uint32_t kStatsBytes = 0x1C0;
} // namespace en

// ---------------------------------------------------------------- the native side's machine
class EnIo final : public rr::sim::SoundIo {
public:
    explicit EnIo(Cpu& cpu) : cpu_(cpu) {}
    bool Load16(uint32_t a, uint16_t& out) override {
        uint32_t v = 0;
        if (!cpu_.ConcessionLoad(a, 2, v)) return false;
        out = static_cast<uint16_t>(v);
        return true;
    }
    bool Store16(uint32_t a, uint16_t v) override { return cpu_.ConcessionStore(a, 2, v); }

private:
    Cpu& cpu_;
};

// The two effect spawners of RoadNote, supplied by the oracle.
class EnFx final : public rr::sim::EngineEffects {
public:
    explicit EnFx(NativeEnv& env) : env_(env) {}
    void Burst(uint32_t bike, int32_t a1, int32_t a2, int32_t a3) override {
        env_.CallOracle(en::kFxBurstFn, bike, static_cast<uint32_t>(a1), static_cast<uint32_t>(a2),
                        static_cast<uint32_t>(a3));
    }
    void Spray(uint32_t bike, uint32_t pos, int32_t kind) override {
        const uint32_t a[3] = {bike, pos, static_cast<uint32_t>(kind)};
        env_.CallOracleArgs(en::kFxSprayFn, a, 3);
    }

private:
    NativeEnv& env_;
};

struct EnRun {
    rr::sim::GuestRam g;
    EnIo io;
    EnFx fx;
    rr::sim::SoundMachine s;
    explicit EnRun(NativeEnv& env)
        : g(env.clone->mem.ram().data(), env.gp), io(env.clone->cpu), fx(env),
          s{g, env.clone->mem.ram().data(), io, &fx} {}
    // A fault or an unserved access is the console's crash or a hole in the port: the case fails.
    uint32_t Done(NativeEnv& env, uint32_t v) {
        if (s.failed && env.failure.empty()) {
            char buf[160];
            std::snprintf(buf, sizeof(buf), "the port could not serve: %s (0x%08X)",
                          s.failWhat ? s.failWhat : "?", s.failAddress);
            env.failure = buf;
        }
        return RqDone(env, g, v);
    }
};

// ---------------------------------------------------------------- planting
void EnPut(CaseContext& c, uint32_t a, uint32_t v) { c.mem->PokeWord(a, v); }
void EnPut16(CaseContext& c, uint32_t a, uint16_t v) { WriteU16Bench(*c.mem, a, v); }
void EnPut8(CaseContext& c, uint32_t a, uint8_t v) { c.mem->WriteBlock(a, &v, 1); }

// libspu's globals and the hardware state: the shadow flag, the address alignment, the dirty mask,
// the RAM shadow of the two register pairs, the register file itself and root counter 2.
void EnPlantSpu(CaseContext& c) {
    Rng& r = *c.rng;
    EnPut(c, rr::sim::kSpuShadowFlag, (r.Next() % 3u == 0) ? (r.U32() | 1u) : (r.U32() & ~1u));
    EnPut(c, rr::sim::kSpuDirtyMask, r.U32());
    if (r.Next() % 4u == 0) {
        EnPut(c, rr::sim::kSpuAlignOn, 0);
    } else {
        static const uint32_t kUnits[4] = {8, 16, 3, 1};
        const uint32_t unit = kUnits[r.Next() % 4u];
        EnPut(c, rr::sim::kSpuAlignOn, 2);
        EnPut(c, rr::sim::kSpuAlignUnit, unit);
        EnPut(c, rr::sim::kSpuAlignMask, (r.Next() % 4u == 0) ? r.U32() : unit - 1u);
    }
    EnPut(c, rr::sim::kSpuAlignShift, (r.Next() % 4u == 0) ? r.U32() : 3u);
    for (uint32_t i = 192; i < 208; ++i) EnPut16(c, rr::sim::kSpuShadow + 2u * i, static_cast<uint16_t>(r.U32()));
    if (c.spuControl != nullptr)
        for (uint32_t i = 0; i < 16; ++i)
            if (r.Next() % 2u == 0) c.spuControl[i] = static_cast<uint16_t>(r.U32());
    if (c.rootCounter != nullptr) *c.rootCounter = r.U32() & 0xFFFFu;
}

// One bank at `a`: `count` sounds, one 16-byte record each (one descriptor), a
// fraction of the offsets zero, the descriptors aligned (the original rejects the rest before any
// of the functions here see them).
void EnPlantBank(CaseContext& c, uint32_t a, uint32_t window, uint32_t count, uint32_t zeroOneIn) {
    Rng& r = *c.rng;
    for (uint32_t i = 0; i < window; i += 4) EnPut(c, a + i, 0);
    EnPut(c, a + 0, 2);
    const uint8_t hdr[4] = {static_cast<uint8_t>(count), static_cast<uint8_t>(count), static_cast<uint8_t>(count), 0x7F};
    c.mem->WriteBlock(a + 4, hdr, 4);
    EnPut(c, a + 8, r.U32() & 0x7FFF0u);
    uint32_t next = (0x10u + 4u * count + 3u) & ~3u;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t off = 0;
        if (zeroOneIn == 0 || r.Next() % zeroOneIn != 0) {
            if (next + 16u > window) next = (0x10u + 4u * count + 3u) & ~3u;
            off = next;
            next += 16;
        }
        EnPut(c, a + 0x10u + 4u * i, off);
        if (off == 0 || off + 16u > window) continue;
        const uint8_t rec[4] = {1, static_cast<uint8_t>(r.U32()), static_cast<uint8_t>(r.U32()), 0x7F};
        c.mem->WriteBlock(a + off, rec, 4);
        EnPut8(c, a + off + 4, static_cast<uint8_t>(r.U32()));        // volume
        EnPut8(c, a + off + 5, static_cast<uint8_t>(r.U32()));
        EnPut16(c, a + off + 6, static_cast<uint16_t>(r.U32()));      // adsr1
        EnPut16(c, a + off + 8, static_cast<uint16_t>(r.U32()));      // adsr2
        EnPut16(c, a + off + 10, static_cast<uint16_t>(r.U32()));     // pitch
        EnPut(c, a + off + 12, r.U32() & 0x7FFF0u);                   // spuAddr
    }
}

// A descriptor address somewhere in the planted banks (for voice `+0x08`).
uint32_t EnSomeDesc(CaseContext& c) {
    const uint32_t banks[3] = {c.scratch + en::kBankEng, c.scratch + en::kBankRoad, c.scratch + en::kBankDef};
    const uint32_t b = banks[c.rng->Next() % 3u];
    const uint32_t count = c.mem->PeekByte(b + 4);
    for (int tries = 0; tries < 4 && count != 0; ++tries) {
        const uint32_t o = c.mem->PeekWord(b + 0x10u + 4u * static_cast<uint32_t>(c.rng->Next() % count));
        if (o != 0) return b + o + 4u;
    }
    return b + 0x10u; // an aligned word inside the bank - arithmetic, not a crash
}

// The sound-system record and a 32-record voice table, inside the original's own ranges.
void EnPlantSystem(CaseContext& c) {
    Rng& r = *c.rng;
    const uint32_t voices = c.scratch + en::kVoices;
    const uint32_t S = kSoundSys;
    const int32_t freeTop = (r.Next() % 3u == 0) ? static_cast<int32_t>(r.U32() % 24u) : -1;
    EnPut(c, S + 0x174, static_cast<uint32_t>(freeTop));
    for (uint32_t i = 0; i < 24; ++i)
        EnPut(c, S + 0xB0 + 4u * i, (static_cast<int32_t>(i) <= freeTop) ? r.U32() % 24u : 0xFFFFFFFFu);
    const uint32_t head = r.U32() % 25u;
    const uint32_t tail = (r.Next() % 6u == 0) ? head : r.U32() % 25u;
    EnPut(c, S + 0x178, head);
    EnPut(c, S + 0x17C, tail);
    for (uint32_t i = 0; i < 25; ++i) EnPut(c, S + 0x110 + 4u * i, (r.Next() % 8u == 0) ? 0xFFFFFFFFu : r.U32() % 24u);
    const bool full = r.Next() % 6u == 0;
    for (uint32_t i = 0; i < 22; ++i)
        EnPut(c, S + 0x58 + 4u * i, (!full && r.Next() % 3u == 0) ? 0xFFFFFFFFu : r.U32() % 24u);
    EnPut(c, S + 0x14, r.U32());
    EnPut(c, S + 0x18, r.U32());
    EnPut(c, S + 0x184, (r.Next() % 5u == 0) ? r.U32() : 0u);
    EnPut(c, c.gp + kGpSoundSerial, (r.Next() % 8u == 0) ? 0x07FFFFFFu : (r.U32() & 0x07FFFFFFu));
    // the banks: slot 0 default, then the road and engine banks at small slots
    const uint32_t table = c.scratch + en::kTable;
    EnPlantBank(c, c.scratch + en::kBankEng, 512, 5 + (r.Next() % 4u == 0 ? r.U32() % 6u : 0u), 8);
    EnPlantBank(c, c.scratch + en::kBankRoad, 768, 25, 10);
    EnPlantBank(c, c.scratch + en::kBankDef, 512, 110, 4);
    const uint32_t count = 2u + r.U32() % 7u;
    EnPut(c, S + 0x00, count);
    EnPut(c, S + 0x04, table);
    EnPut(c, S + 0x0C, voices);
    for (uint32_t i = 0; i < 16; ++i) {
        const uint64_t roll = r.Next() % 6u;
        uint32_t p = c.scratch + ((roll < 2) ? en::kBankEng : (roll < 4) ? en::kBankRoad : en::kBankDef);
        if (roll == 5) p = 0;
        EnPut(c, table + 4u * i, p);
    }
    EnPut(c, table + 0, c.scratch + en::kBankDef);
    EnPut(c, table + 4, c.scratch + en::kBankRoad);
    EnPut(c, table + 8, c.scratch + en::kBankEng);
    EnPut(c, c.gp + kGpDefaultBank, (r.Next() % 8u == 0) ? r.U32() % (count + 2u) : 0u);
    EnPut(c, c.gp + kGpMutedBank, (r.Next() % 6u == 0) ? r.U32() % (count + 2u) : 1u);
    for (uint32_t v = 0; v < 32; ++v) {
        const uint32_t a = voices + 44u * v;
        for (uint32_t k = 0; k < 44; k += 4) EnPut(c, a + k, 0);
        EnPut(c, a + 0x04, r.U32() & ((r.Next() % 12u == 0) ? 0u : 0x07FFFFFFu));
        EnPut(c, a + 0x08, EnSomeDesc(c));
        EnPut(c, a + 0x10, r.U32() % 3u);
        EnPut(c, a + 0x1C, (v < 24 && r.Next() % 6u != 0) ? v : r.U32() % 32u);
        EnPut(c, a + 0x20, r.U32());
        EnPut(c, a + 0x24, r.U32());
        EnPut(c, a + 0x28, r.U32());
    }
}

// A handle: usually live (the named voice's own serial), sometimes stale, sometimes 0 or -1.
uint32_t EnHandle(CaseContext& c) {
    Rng& r = *c.rng;
    const uint64_t roll = r.Next() % 10u;
    if (roll == 0) return 0;
    if (roll == 1) return 0xFFFFFFFFu;
    const uint32_t voices = c.mem->PeekWord(kSoundSys + 0x0C);
    // A channel field of 24..31 names a record PAST the 24-voice table. The planted table has 32
    // records, so that read is defined; the live one is followed by heap, so live cases stay < 24.
    const bool planted = voices == c.scratch + en::kVoices;
    const uint32_t k = (planted && r.Next() % 5u == 0) ? r.U32() % 32u : r.U32() % 24u;
    const uint32_t serial = (roll < 8) ? (c.mem->PeekWord(voices + 44u * k + 4u) & 0x07FFFFFFu) : (r.U32() & 0x07FFFFFFu);
    return (k << 27) | serial;
}

// The whole engine world for EngineNote / EngineStart / RoadNote. Returns the player index.
//
// Dump-derived: the snapshot's OWN sound system, banks, voices and listener, and its live engine
// record, driven by one of the real captured bikes (its 1096-byte image copied into the scratch
// block, with the snapshot's live rider and stat block behind it); one case in six re-arms the
// first frame (E+0x04 = 0 - the 8A.6 leak and, with the live empty free stack, the non-adjacent
// L3/L4 arm).
// Randomised: every one of those planted, with every scalar the three functions read randomised.
int32_t EnPlantEngineWorld(CaseContext& c) {
    Rng& r = *c.rng;
    Memory& m = *c.mem;
    const uint32_t bike = c.scratch + en::kBike;
    const uint32_t rider = c.scratch + en::kRider;
    const uint32_t stats = c.scratch + en::kStats;
    int32_t p = 0;
    uint32_t E = 0;
    const uint32_t liveBike = m.PeekWord(kPlayerArr);
    if (c.sample) {
        E = m.PeekWord(c.gp + rr::sim::kEngineRecGp);
        std::vector<uint8_t> buf(0x260);
        m.ReadBlock(m.PeekWord(liveBike + 0x354), buf.data(), 0x260);
        m.WriteBlock(rider, buf.data(), 0x260);
        m.ReadBlock(m.PeekWord(liveBike + 0x22C), buf.data(), en::kStatsBytes);
        m.WriteBlock(stats, buf.data(), en::kStatsBytes);
        if (c.sample->entity.size() >= 1096) m.WriteBlock(bike, c.sample->entity.data(), 1096);
        if (r.Next() % 6u == 0) EnPut8(c, E + 4, 0);
        if (r.Next() % 6u == 0) EnPut(c, E + 0x14, 0);
        if (r.Next() % 6u == 0) EnPut(c, E + 0x18, 0);
    } else {
        EnPlantSystem(c);
        // the listener array: four records, or NULL one case in ten (Sound3DParams's first arm)
        const uint32_t L = c.scratch + en::kListen;
        for (uint32_t k = 0; k < 4; ++k) {
            const uint32_t a = L + 72u * k;
            for (uint32_t i = 0; i < 72; i += 4) EnPut(c, a + i, 0);
            EnPut(c, a + 0x00, r.U32() % 4096u);
            EnPut(c, a + 0x04, static_cast<uint32_t>(r.World()));
            EnPut(c, a + 0x08, static_cast<uint32_t>(r.World()));
            EnPut(c, a + 0x0C, static_cast<uint32_t>(r.World() >> 4));
            EnPut(c, a + 0x10, static_cast<uint32_t>(r.World() >> 4));
            EnPut(c, a + 0x14, r.U32() % 256u);
        }
        EnPut(c, c.gp + kGpListener, (r.Next() % 10u == 0) ? 0u : L);
        const uint32_t gs = m.PeekWord(kGameStatePtr);
        EnPut(c, gs + 0x30, 1u + r.U32() % 2u);
        p = static_cast<int32_t>(r.U32() % 2u);
        const uint32_t arr = c.scratch + en::kEng;
        EnPut(c, c.gp + rr::sim::kEngineRecGp, arr);
        E = arr + 132u * static_cast<uint32_t>(p);
        for (uint32_t k = 0; k < 2; ++k)
            for (uint32_t i = 0; i < 132; i += 4) EnPut(c, arr + 132u * k + i, r.U32());
        // the bike: a real captured image with its scalars randomised
        m.WriteBlock(bike, c.any->entity.data(), std::min<size_t>(c.any->entity.size(), 1096));
        for (uint32_t i = 0; i < 0x260; i += 4) EnPut(c, rider + i, r.U32());
        for (uint32_t i = 0; i < en::kStatsBytes; i += 4)
            EnPut(c, stats + i, (r.Next() % 8u == 0) ? 0u : r.U32() >> (r.Next() % 20u));
        EnPut(c, rider + 0x25C, (r.Next() % 4u != 0) ? r.U32() % 5u : r.U32());
        static const uint32_t kBikeWords[] = {0x24, 0xB8, 0xBC, 0xC0, 0x184, 0x1C8, 0x1D0, 0x1E0, 0x1FC, 0x234,
                                              0x238, 0x24C, 0x250, 0x25C, 0x28C, 0x2A4, 0x2B8, 0x2BC};
        for (uint32_t off : kBikeWords) {
            const uint64_t roll = r.Next() % 4u;
            EnPut(c, bike + off, roll == 0 ? r.U32() : static_cast<uint32_t>(r.S32() >> (r.Next() % 24u)));
        }
        // the two "how far apart" tests RoadNote makes (|+0xBC - +0x1FC| < 16384, bike and rider)
        // are almost never true of two random words, so half the time they are planted close
        if (r.Next() % 2u == 0)
            EnPut(c, bike + 0x1FC, c.mem->PeekWord(bike + 0xBC) + static_cast<uint32_t>(static_cast<int32_t>(r.U32() % 40000u) - 20000));
        if (r.Next() % 2u == 0)
            EnPut(c, rider + 0x1FC, c.mem->PeekWord(rider + 0xBC) + static_cast<uint32_t>(static_cast<int32_t>(r.U32() % 40000u) - 20000));
        if (r.Next() % 2u == 0) EnPut(c, bike + 0x28C, static_cast<uint32_t>(static_cast<int32_t>(r.U32() % 60000u) - 30000));
        EnPut8(c, bike + 0x216, static_cast<uint8_t>((r.Next() % 3u == 0) ? r.U32() : r.U32() % 5u));
        EnPut8(c, bike + 0x351, static_cast<uint8_t>((r.Next() % 8u == 0) ? r.U32() : r.U32() % 6u));
        EnPut16(c, bike + 0x1E2, static_cast<uint16_t>(r.U32() % 32u));
        // the parameter record and the gear table
        const uint32_t P = c.scratch + en::kParams;
        EnPut(c, P + 0, 0xD3D + (r.Next() % 3u == 0 ? r.U32() % 0x2000u : 0u) - 0x400u * (r.Next() % 2u));
        EnPut(c, P + 4, 0xE0 + r.U32() % 64u);
        EnPut(c, P + 8, 0x80 + r.U32() % 64u);
        EnPut(c, P + 12, (r.Next() % 2u) ? 0x10000u : r.U32() >> 12);
        EnPut(c, P + 16, (r.Next() % 3u) ? 3u : r.U32());
        for (uint32_t g = 0; g < 10; ++g) EnPut(c, rr::sim::kGearTable + 4u * g, r.U32() >> (r.Next() % 16u));
        EnPut(c, rr::sim::kEngineSlider, r.U32() % 128u);
        EnPut(c, rr::sim::kEffectsSlider, (r.Next() % 4u == 0) ? r.U32() : r.U32() % 128u);
        EnPut(c, c.gp + rr::sim::kRoadMuteGp, (r.Next() % 4u == 0) ? 1u : ((r.Next() % 4u == 0) ? r.U32() : 0u));
        EnPut(c, c.gp + kRandSeedGpOffset, r.U32());
        // the engine record: the fields that steer the arms, inside the original's ranges half the time
        EnPut(c, E + 0x00, bike);
        EnPut8(c, E + 0x04, static_cast<uint8_t>(r.Next() % 4u == 0 ? 0 : 1));
        EnPut(c, E + 0x08, r.Next() % 4u == 0 ? r.U32() % 12u : 2u);
        EnPut(c, E + 0x2C, P);
        EnPut(c, E + 0x10, EnHandle(c));
        EnPut(c, E + 0x14, r.Next() % 3u == 0 ? 0u : EnHandle(c));
        EnPut(c, E + 0x18, r.Next() % 3u == 0 ? 0u : EnHandle(c));
        EnPut(c, E + 0x1C, EnHandle(c));
        EnPut(c, E + 0x20, EnHandle(c));
        EnPut(c, E + 0x24, r.Next() % 3u == 0 ? 0u : EnHandle(c));
        EnPut(c, E + 0x28, r.Next() % 3u == 0 ? 0u : EnHandle(c));
        EnPut(c, E + 0x30, r.U32() % 0x1000u);
        EnPut(c, E + 0x3C, r.Next() % 2u ? 0x21u : r.U32() >> 8);
        EnPut(c, E + 0x44, r.U32() >> (r.Next() % 16u));
        EnPut(c, E + 0x48, r.U32() >> (r.Next() % 16u));
        EnPut(c, E + 0x4C, r.U32() % 12u);
        // -1 ("no road voice") is the state after EngineStart, and the only one in which RoadNote
        // plays its one-shot at the rider (0x8001708C), so it is drawn a third of the time
        EnPut(c, E + 0x54, (r.Next() % 3u == 0) ? 0xFFFFFFFFu : static_cast<uint32_t>(static_cast<int32_t>(r.U32() % 24u) - 1));
        EnPut(c, E + 0x58, (r.Next() % 3u == 0) ? 0xFFFFFFFFu : static_cast<uint32_t>(static_cast<int32_t>(r.U32() % 24u) - 1));
        EnPut(c, E + 0x5C, r.Next() % 2u ? 0u : r.U32() % 6u);
        EnPut(c, E + 0x60, r.U32() % 160u);
        EnPut(c, E + 0x6C, static_cast<uint32_t>(static_cast<int32_t>(r.U32() % 7u) - 1));
        EnPut(c, E + 0x70, r.U32() % 128u);
        EnPut(c, E + 0x74, r.U32() % 256u);
        EnPut(c, E + 0x78, r.Next() % 2u ? 0x10000u : r.U32() >> 12);
        EnPut(c, E + 0x7C, r.Next() % 3u == 0 ? r.U32() % 8u : 0u);
    }
    // common: the bike's pointers, and the SPU side
    EnPut(c, bike + 0x354, rider);
    EnPut(c, bike + 0x22C, stats);
    EnPut(c, E + 0x00, bike);
    EnPlantSpu(c);
    return p;
}

// ---------------------------------------------------------------- the rows
void AddEngineNoteRows(std::vector<Row>& rows) {
    using rr::sim::SoundParams;

    // ---- SLUS 0x800506A8, libspu's register-pair read-modify-write. THE row the register file
    // exists for: both halves are read before anything else, on every path.
    {
        Row row{"spu_set_any_voice", "u32 _SpuSetAnyVoice(on, mask, r, r1)  [SLUS libspu, reads the SPU]",
                en::kAnyVoiceFn,
                [](CaseContext& c, Args& a) {
                    EnPlantSpu(c);
                    Rng& r = *c.rng;
                    static const int32_t kOn[6] = {0, 1, 8, 2, -1, 1};
                    a.a[0] = static_cast<uint32_t>((r.Next() % 8u == 0) ? r.S32() : kOn[r.Next() % 6u]);
                    a.a[1] = r.U32() >> (r.Next() % 8u);
                    // r in 192..204, so both halves stay inside the register file (and the shadow)
                    const uint32_t reg = (r.Next() % 2u) ? ((r.Next() % 2u) ? 204u : 200u) : 192u + r.U32() % 13u;
                    a.a[2] = reg;
                    a.a[3] = (r.Next() % 6u == 0) ? reg : reg + 1u;
                },
                nullptr};
        row.nativeEx = [](NativeEnv& env, const Args& a) {
            EnRun run(env);
            const uint32_t v = rr::sim::SpuSetAnyVoice(run.s, static_cast<int32_t>(a.a[0]), a.a[1], a.a[2], a.a[3]);
            return run.Done(env, v);
        };
        row.compareSpuControl = true;
        rows.push_back(row);
    }

    // ---- SLUS 0x80043F00 GetRCnt
    {
        Row row{"get_rcnt", "u32 GetRCnt(u32 id)  [SLUS, root counter id&0xFFFF when < 3]", en::kGetRCntFn,
                [](CaseContext& c, Args& a) {
                    EnPlantSpu(c);
                    Rng& r = *c.rng;
                    a.a[0] = (r.Next() % 2u) ? 0xF2000002u : ((r.Next() % 2u) ? (r.U32() & 0xFFFF0000u) | (r.U32() % 3u) : r.U32());
                },
                nullptr};
        row.nativeEx = [](NativeEnv& env, const Args& a) {
            EnRun run(env);
            return run.Done(env, rr::sim::GetRCnt(run.s, a.a[0]));
        };
        rows.push_back(row);
    }

    // ---- SLUS 0x8001FB58 ReleaseVoice
    {
        Row row{"release_voice", "void ReleaseVoice(Voice *v)  [SLUS, reserved -> steal ring]", en::kReleaseFn,
                [](CaseContext& c, Args& a) {
                    const uint32_t voices = c.sample ? c.mem->PeekWord(kSoundSys + 0x0C) : (EnPlantSystem(c), c.scratch + en::kVoices);
                    a.a[0] = voices + 44u * static_cast<uint32_t>(c.rng->U32() % 24u);
                },
                nullptr};
        row.nativeEx = [](NativeEnv& env, const Args& a) {
            EnRun run(env);
            return run.Done(env, rr::sim::ReleaseVoice(run.s, a.a[0]));
        };
        rows.push_back(row);
    }

    // ---- SLUS 0x8001F934 GetSoundPitch
    {
        Row row{"get_sound_pitch", "u16 GetSoundPitch(bank, sound, desc)  [SLUS, the `>` bound]", en::kGetPitchFn,
                [](CaseContext& c, Args& a) {
                    if (!c.sample) EnPlantSystem(c);
                    Rng& r = *c.rng;
                    const int32_t count = static_cast<int32_t>(c.mem->PeekWord(kSoundSys));
                    a.a[0] = static_cast<uint32_t>(static_cast<int32_t>(r.U32() % static_cast<uint32_t>(count + 3)) - 1);
                    a.a[1] = static_cast<uint32_t>((r.Next() % 6u == 0) ? r.S32() : static_cast<int32_t>(r.U32() % 30u) - 1);
                    a.a[2] = (r.Next() % 4u == 0) ? r.U32() % 3u : 0u;
                },
                nullptr};
        row.nativeEx = [](NativeEnv& env, const Args& a) {
            EnRun run(env);
            return run.Done(env, rr::sim::GetSoundPitch(run.s, static_cast<int32_t>(a.a[0]),
                                                        static_cast<int32_t>(a.a[1]), static_cast<int32_t>(a.a[2])));
        };
        rows.push_back(row);
    }

    // ---- SLUS 0x8001F7EC StopVoice - including StopVoice(0) (8A.6)
    {
        Row row{"stop_voice", "void StopVoice(u32 handle)  [SLUS, KeyOff + Release + EON on]", en::kStopVoiceFn,
                [](CaseContext& c, Args& a) {
                    if (!c.sample) EnPlantSystem(c);
                    EnPlantSpu(c);
                    a.a[0] = EnHandle(c);
                },
                nullptr};
        row.nativeEx = [](NativeEnv& env, const Args& a) {
            EnRun run(env);
            return run.Done(env, rr::sim::StopVoice(run.s, a.a[0]));
        };
        row.compareSpuControl = true;
        rows.push_back(row);
    }

    // ---- SLUS 0x8001F874 SetVoiceLoopOffset
    {
        Row row{"set_voice_loop_offset", "void SetVoiceLoopOffset(s32 off, u32 handle)  [SLUS, LSAX]", en::kSetLoopFn,
                [](CaseContext& c, Args& a) {
                    if (!c.sample) EnPlantSystem(c);
                    EnPlantSpu(c);
                    Rng& r = *c.rng;
                    a.a[0] = (r.Next() % 4u == 0) ? r.U32() : static_cast<uint32_t>(r.U32() % 5u) << 7;
                    a.a[1] = EnHandle(c);
                },
                nullptr};
        row.nativeEx = [](NativeEnv& env, const Args& a) {
            EnRun run(env);
            return run.Done(env, rr::sim::SetVoiceLoopOffset(run.s, static_cast<int32_t>(a.a[0]), a.a[1]));
        };
        row.compareSpuControl = true;
        rows.push_back(row);
    }

    // ---- SLUS 0x8001F900 SetVoicePitchMod - PMON, no serial test
    {
        Row row{"set_voice_pitch_mod", "void SetVoicePitchMod(u32 handle, s32 on)  [SLUS, PMON]", en::kPitchModFn,
                [](CaseContext& c, Args& a) {
                    EnPlantSpu(c);
                    Rng& r = *c.rng;
                    a.a[0] = (r.Next() % 4u == 0) ? r.U32() : (r.U32() % 32u) << 27;
                    a.a[1] = (r.Next() % 3u == 0) ? r.U32() : r.U32() % 2u;
                },
                nullptr};
        row.nativeEx = [](NativeEnv& env, const Args& a) {
            EnRun run(env);
            return run.Done(env, rr::sim::SetVoicePitchMod(run.s, a.a[0], static_cast<int32_t>(a.a[1])));
        };
        row.compareSpuControl = true;
        rows.push_back(row);
    }

    // ---- SLUS 0x80019C54 UpdateVoice and its copy 0x8001F6A4
    for (int copy = 0; copy < 2; ++copy) {
        Row row{copy ? "update_voice2" : "update_voice",
                copy ? "void UpdateVoice(u32 h, SoundParams*)  [SLUS 0x8001F6A4, the copy RoadNote calls]"
                     : "void UpdateVoice(u32 h, SoundParams*)  [SLUS, VOLL|VOLR|PITCH live]",
                copy ? en::kUpdateVoice2Fn : en::kUpdateVoiceFn,
                [](CaseContext& c, Args& a) {
                    if (!c.sample) EnPlantSystem(c);
                    EnPlantSpu(c);
                    Rng& r = *c.rng;
                    const uint32_t P = c.scratch + en::kSndPar;
                    EnPut(c, P + 0, (r.Next() % 3u == 0) ? r.U32() : r.U32() % 0x4000u);
                    EnPut(c, P + 4, static_cast<uint32_t>(r.S32() >> (r.Next() % 24u)));
                    EnPut(c, P + 8, (r.Next() % 6u == 0) ? r.U32() : r.U32() % 256u);
                    a.a[0] = EnHandle(c);
                    a.a[1] = P;
                },
                nullptr};
        row.nativeEx = [](NativeEnv& env, const Args& a) {
            EnRun run(env);
            SoundParams p;
            p.pitch = run.g.S32(a.a[1] + 0);
            p.volume = run.g.S32(a.a[1] + 4);
            p.pan = run.g.S32(a.a[1] + 8);
            return run.Done(env, rr::sim::UpdateVoice(run.s, a.a[0], p));
        };
        row.compareSpuControl = true;
        rows.push_back(row);
    }

    // ---- StartVoice with noReverb != 0: the arm the emitter rows of verify_physics.cpp bound away,
    // driven here, because the register file answers 0x800506A8's reads.
    {
        Row row{"start_voice_noreverb", "s32 StartVoice(bank, id, noReverb, reserved, SoundParams*)  [SLUS, EON off]",
                kStartVoice,
                [](CaseContext& c, Args& a) {
                    if (!c.sample) EnPlantSystem(c);
                    EnPlantSpu(c);
                    Rng& r = *c.rng;
                    const uint32_t P = c.scratch + en::kSndPar;
                    EnPut(c, P + 0, (r.Next() % 2u) ? 0xFFFFFFFFu : r.U32() % 0x4000u);
                    EnPut(c, P + 4, static_cast<uint32_t>(r.S32() >> (r.Next() % 24u)));
                    EnPut(c, P + 8, r.U32() % 256u);
                    const int32_t count = static_cast<int32_t>(c.mem->PeekWord(kSoundSys));
                    a.a[0] = r.U32() % static_cast<uint32_t>(count + 1);
                    a.a[1] = r.U32() % 30u;
                    a.a[2] = (r.Next() % 4u == 0) ? 0u : ((r.Next() % 2u) ? 1u : r.U32());
                    a.a[3] = r.U32() % 2u;
                    a.a4 = P;
                },
                nullptr};
        row.nativeEx = [](NativeEnv& env, const Args& a) {
            EnRun run(env);
            SoundParams p;
            p.pitch = run.g.S32(a.a4 + 0);
            p.volume = run.g.S32(a.a4 + 4);
            p.pan = run.g.S32(a.a4 + 8);
            return run.Done(env, rr::sim::GuestStartVoice(run.s, static_cast<int32_t>(a.a[0]), static_cast<int32_t>(a.a[1]),
                                                          static_cast<int32_t>(a.a[2]), static_cast<int32_t>(a.a[3]), p));
        };
        row.compareSpuControl = true;
        rows.push_back(row);
    }

    // ---- SLUS 0x80050D08 SpuSetKey: KON / KOFF, or their RAM shadow
    {
        Row row{"spu_set_key", "u32 SpuSetKey(on, mask)  [SLUS libspu, KON/KOFF 0x1F801D88..8E]", en::kSetKeyFn,
                [](CaseContext& c, Args& a) {
                    EnPlantSpu(c);
                    Rng& r = *c.rng;
                    for (uint32_t i = 0; i < 8; i += 2) EnPut16(c, rr::sim::kSpuKeyShadow + i, static_cast<uint16_t>(r.U32()));
                    EnPut(c, rr::sim::kSpuKeyDirty, r.U32());
                    EnPut(c, rr::sim::kSpuKeyedMask, r.U32());
                    static const int32_t kOn[5] = {0, 1, 1, 0, 2};
                    a.a[0] = static_cast<uint32_t>((r.Next() % 8u == 0) ? r.S32() : kOn[r.Next() % 5u]);
                    a.a[1] = r.U32() >> (r.Next() % 8u);
                },
                nullptr};
        row.nativeEx = [](NativeEnv& env, const Args& a) {
            EnRun run(env);
            return run.Done(env, rr::sim::SpuSetKey(run.s, static_cast<int32_t>(a.a[0]), a.a[1]));
        };
        row.compareSpuControl = true;
        rows.push_back(row);
    }

    // ---- SLUS 0x8001EB7C ProgramVoice
    {
        Row row{"program_voice", "void ProgramVoice(Voice*, pitch, volL, volR)  [SLUS, attr 0x60093]", en::kProgramFn,
                [](CaseContext& c, Args& a) {
                    const uint32_t voices = c.sample ? c.mem->PeekWord(kSoundSys + 0x0C) : (EnPlantSystem(c), c.scratch + en::kVoices);
                    EnPlantSpu(c);
                    Rng& r = *c.rng;
                    a.a[0] = voices + 44u * (r.U32() % 24u);
                    a.a[1] = r.U32() >> (r.Next() % 20u);
                    a.a[2] = r.U32();
                    a.a[3] = r.U32();
                },
                nullptr};
        row.nativeEx = [](NativeEnv& env, const Args& a) {
            EnRun run(env);
            return run.Done(env, rr::sim::ProgramVoice(run.s, a.a[0], a.a[1], a.a[2], a.a[3]));
        };
        row.compareSpuControl = true;
        rows.push_back(row);
    }

    // ---- SLUS 0x8001EE94 SoundService: the key commit, from the vsync interrupt
    {
        Row row{"voice_service", "void SoundService(void)  [SLUS, KOFF, program, KON; not named sound_* - the bench skips those]", en::kServiceFn,
                [](CaseContext& c, Args&) {
                    if (!c.sample) EnPlantSystem(c);
                    EnPlantSpu(c);
                    Rng& r = *c.rng;
                    EnPut(c, kSoundSys + 0x10, (r.Next() % 8u == 0) ? 1u : 0u);
                    EnPut(c, kSoundSys + 0x14, (r.Next() % 5u == 0) ? 0u : r.U32() >> (r.Next() % 16u));
                    EnPut(c, kSoundSys + 0x18, (r.Next() % 3u == 0) ? 0u : r.U32() >> (r.Next() % 16u));
                    EnPut(c, rr::sim::kSpuKeyedMask, r.U32());
                },
                nullptr};
        row.nativeEx = [](NativeEnv& env, const Args&) {
            EnRun run(env);
            return run.Done(env, rr::sim::SoundService(run.s));
        };
        row.compareSpuControl = true;
        rows.push_back(row);
    }

    // ---- SLUS 0x80019990 AudioVSyncTick: the ramps, the four retunes, SoundService
    {
        Row row{"audio_vsync_tick", "void AudioVSyncTick(void)  [SLUS, 60 Hz engine retune]", en::kTickFn,
                [](CaseContext& c, Args&) {
                    const int32_t p = EnPlantEngineWorld(c);
                    Rng& r = *c.rng;
                    const uint32_t arr = c.mem->PeekWord(c.gp + rr::sim::kEngineRecGp);
                    EnPut(c, rr::sim::kAudioGate, (r.Next() % 8u == 0) ? (r.U32() & ~4u) : (r.U32() | 4u));
                    const uint32_t toggle = (r.Next() % 4u == 0) ? 1u : 0u;
                    EnPut(c, c.gp + rr::sim::kTickToggleGp, toggle);
                    const uint32_t gs = c.mem->PeekWord(kGameStatePtr);
                    if (!c.sample) {
                        // both records valid: the one the world was built around, copied
                        std::vector<uint8_t> rec(132);
                        c.mem->ReadBlock(arr + 132u * static_cast<uint32_t>(p), rec.data(), 132);
                        c.mem->WriteBlock(arr + 132u * static_cast<uint32_t>(1 - p), rec.data(), 132);
                        if (r.Next() % 4u == 0) EnPut(c, gs + 0x30, 2u);
                    } else if (toggle == 0 && r.Next() % 4u == 0) {
                        // the live array has ONE record: a two-player tick may only service record 0
                        EnPut(c, gs + 0x30, 2u);
                    }
                    EnPut(c, kSoundSys + 0x10, (r.Next() % 10u == 0) ? 1u : 0u);
                    EnPut(c, rr::sim::kSpuKeyedMask, r.U32());
                    // the ramps, half the time in the overshooting regime 8.2 describes
                    const uint32_t E = c.mem->PeekWord(c.gp + rr::sim::kEngineRecGp) +
                                       132u * c.mem->PeekWord(c.gp + rr::sim::kTickToggleGp);
                    if (r.Next() % 2u == 0) {
                        EnPut(c, E + 0x30, r.U32() % 0x1000u);
                        EnPut(c, E + 0x34, r.U32() % 0x1000u);
                        EnPut(c, E + 0x38, static_cast<uint32_t>(static_cast<int32_t>(r.U32() % 0x800u) - 0x400));
                        EnPut(c, E + 0x60, r.U32() % 160u);
                        EnPut(c, E + 0x64, r.U32() % 160u);
                        EnPut(c, E + 0x68, static_cast<uint32_t>(static_cast<int32_t>(r.U32() % 128u) - 64));
                    }
                    if (r.Next() % 4u == 0) EnPut(c, c.mem->PeekWord(E) + 0x234, r.U32());
                },
                nullptr};
        row.nativeEx = [](NativeEnv& env, const Args&) {
            EnRun run(env);
            return run.Done(env, rr::sim::AudioVSyncTick(run.s));
        };
        row.compareSpuControl = true;
        rows.push_back(row);
    }

    // ---- SLUS 0x80019D9C AudioReset: the sound globals and the seven volume sliders
    {
        Row row{"audio_reset", "void AudioReset(void)  [SLUS, sliders = 7*v>>4 of the EXE's defaults]", en::kResetFn,
                [](CaseContext& c, Args&) {
                    if (c.sample) return; // the snapshot's own globals and defaults
                    Rng& r = *c.rng;
                    for (uint32_t k = 0; k < 8; ++k) EnPut(c, 0x80052638u + 4u * k, r.U32() >> (r.Next() % 24u));
                    for (uint32_t off = 1872; off < 1968; off += 4) EnPut(c, c.gp + off, r.U32());
                },
                nullptr};
        row.nativeEx = [](NativeEnv& env, const Args&) {
            EnRun run(env);
            return run.Done(env, rr::sim::AudioReset(run.s));
        };
        rows.push_back(row);
    }

    // ---- SLUS 0x8001654C EngineSetup: the per-bike constants, once per race
    {
        Row row{"engine_setup", "void EngineSetup(void)  [SLUS, gear table + revs/slide scales]", en::kSetupFn,
                [](CaseContext& c, Args&) {
                    Rng& r = *c.rng;
                    Memory& m = *c.mem;
                    const uint32_t bike = c.scratch + en::kBike;
                    const uint32_t stats = c.scratch + en::kStats;
                    const uint32_t gs = m.PeekWord(kGameStatePtr);
                    const uint32_t liveBike = m.PeekWord(kPlayerArr);
                    if (c.sample) {
                        // one of the real captured bikes, with the snapshot's live stat block
                        std::vector<uint8_t> buf(en::kStatsBytes);
                        m.ReadBlock(m.PeekWord(liveBike + 0x22C), buf.data(), en::kStatsBytes);
                        m.WriteBlock(stats, buf.data(), en::kStatsBytes);
                        if (c.sample->entity.size() >= 1096) m.WriteBlock(bike, c.sample->entity.data(), 1096);
                        EnPut(c, bike + 0x22C, stats);
                        EnPut(c, kPlayerArr, bike);
                        return;
                    }
                    m.WriteBlock(bike, c.any->entity.data(), std::min<size_t>(c.any->entity.size(), 1096));
                    for (uint32_t i = 0; i < en::kStatsBytes; i += 4)
                        EnPut(c, stats + i, (r.Next() % 6u == 0) ? 0u : r.U32() >> (r.Next() % 24u));
                    for (uint32_t k : {0xB4u, 0xCCu, 0x190u})
                        if (r.Next() % 6u == 0) EnPut(c, stats + k, r.U32() % 8u); // the small-divisor arms
                    EnPut(c, bike + 0x22C, stats);
                    const uint32_t players = (r.Next() % 8u == 0) ? 0u : 1u + r.U32() % 2u;
                    EnPut(c, gs + 0x30, players);
                    EnPut(c, kPlayerArr, bike);
                    EnPut(c, kPlayerArr + 4, bike);
                    for (uint32_t p = 0; p < 2; ++p)
                        EnPut(c, gs + 0x48 + 4u * p, static_cast<uint32_t>(static_cast<int32_t>(r.U32() % 200u) - 9));
                    EnPut(c, c.gp + rr::sim::kEngineRecGp, c.scratch + en::kEng);
                },
                nullptr, /*compareV0=*/false};
        row.nativeEx = [](NativeEnv& env, const Args&) {
            EnRun run(env);
            rr::sim::EngineSetup(run.s);
            return run.Done(env, 0);
        };
        rows.push_back(row);
    }

    // ---- SLUS 0x800167A4 EngineStart
    {
        Row row{"engine_start", "void EngineStart(int p)  [SLUS, five layers, the adjacency test]", en::kEngineStartFn,
                [](CaseContext& c, Args& a) {
                    const int32_t p = EnPlantEngineWorld(c);
                    const uint32_t E = c.mem->PeekWord(c.gp + rr::sim::kEngineRecGp) + 132u * static_cast<uint32_t>(p);
                    // mostly the first-frame state, so the function does its work
                    if (c.rng->Next() % 6u != 0) EnPut8(c, E + 4, 0);
                    a.a[0] = static_cast<uint32_t>(p);
                },
                nullptr, /*compareV0=*/false};
        row.nativeEx = [](NativeEnv& env, const Args& a) {
            EnRun run(env);
            rr::sim::EngineStart(run.s, static_cast<int32_t>(a.a[0]));
            return run.Done(env, 0);
        };
        row.compareSpuControl = true;
        rows.push_back(row);
    }

    // ---- SLUS 0x800169D0 EngineNote (calls EngineStart on the first frame)
    {
        Row row{"engine_note", "void EngineNote(int p)  [SLUS, revs -> level, load -> cross-fade]", en::kEngineNoteFn,
                [](CaseContext& c, Args& a) { a.a[0] = static_cast<uint32_t>(EnPlantEngineWorld(c)); },
                nullptr, /*compareV0=*/false};
        row.nativeEx = [](NativeEnv& env, const Args& a) {
            EnRun run(env);
            // the original is called at the bench's own sp: env.sp is that when oracleFrame is 0
            rr::sim::EngineNote(run.s, static_cast<int32_t>(a.a[0]), env.sp);
            return run.Done(env, 0);
        };
        row.compareSpuControl = true;
        rows.push_back(row);
    }

    // ---- SLUS 0x80016E4C RoadNote: pulls the LCG, calls PlaySound3D, and hands two visual-effect
    // spawners to the oracle, at RoadNote's own frame depth.
    {
        Row row{"road_note", "void RoadNote(int p)  [SLUS, road/skid voices, Rand, effects on the seam]", en::kRoadNoteFn,
                [](CaseContext& c, Args& a) { a.a[0] = static_cast<uint32_t>(EnPlantEngineWorld(c)); },
                nullptr, /*compareV0=*/false};
        row.nativeEx = [](NativeEnv& env, const Args& a) {
            EnRun run(env);
            rr::sim::RoadNote(run.s, static_cast<int32_t>(a.a[0]));
            return run.Done(env, 0);
        };
        OracleCallee burst;
        burst.address = en::kFxBurstFn;
        burst.arity = 4;
        OracleCallee spray;
        spray.address = en::kFxSprayFn;
        spray.arity = 3;
        row.oracleCallees = {burst, spray};
        row.oracleFrame = en::kNoteFrame;
        row.compareSpuControl = true;
        rows.push_back(row);
    }
}

// ================================================================ AudioFrame and the object sounds
//
// Fourteen rows for src\game\sim\sound_frame.h. Every function runs on the rr-race machine's own
// world: the dump-derived family is the snapshot exactly as captured (its listener, its eight
// object/cue slots, its voice table, its collision grid and pools), the random family is the same
// world with every field these functions branch on redrawn, inside the ranges named below.
//
// Named bounds on the random family, each a property of every input the original's own callers
// produce:
//   * one player (the listener array is `malloc(72 * numPlayers)`, RASHCDI 0x800628E4, so a second
//     record does not exist on this machine);
//   * an object slot names a handle the query can return with the class mask 9 the original passes
//     (0x800184CC) - a live bike (class 0), the traffic record (class 3) - or a police record
//     (class 5, 29..31, appended by 0x8001856C), or is free (0xE0);
//   * a police slot's kind is 0, 1 or 6 (AudioFrame stores 6 over it before it reads the kind, so
//     only kind 2 - a rider's cue, which RiderSpeech only puts on a BIKE handle - would dereference
//     the police record's +0x354), and the traffic record's +0xB4 is planted 0 and its slot's kind
//     is never 2 (its +0x354 is not a rider pointer);
//   * a slot's voice handle is EnHandle's (live, stale, 0 or -1 - all the original produces).
namespace sf {
constexpr uint32_t kAudioFrameFn = 0x80018FAC, kObjStartFn = 0x8001769C, kObjStopFn = 0x80017814;
constexpr uint32_t kObjUpdateFn = 0x800179D8, kObjClaimFn = 0x80017F64, kObjPickFn = 0x800184AC;
constexpr uint32_t kSpeechCueFn = 0x8001B244, kQueryFn = 0x8008AE94, kReverbFn = 0x8001F054;
constexpr uint32_t kReleaseAllFn = 0x8001F0D4, kResetStateFn = 0x8001E5A8, kResetListsFn = 0x8001FBD4;
constexpr uint32_t kPatchBankFn = 0x8001EAA4, kRecordsInitFn = 0x80063448;
constexpr uint32_t kAudioFrameFrame = 64; // `addiu sp,sp,-64` at 0x80018FB0
constexpr uint32_t kOverlayBase = 0x8005B5E8;

uint32_t Obj(Rng& r, uint32_t& kindOut) {
    const uint64_t roll = r.Next() % 8u;
    if (roll < 4) { // a bike
        kindOut = static_cast<uint32_t>(r.Next() % 7u);
        return r.U32() % 18u;
    }
    if (roll == 4) { // the traffic record
        static const uint32_t k[5] = {0, 1, 3, 4, 5};
        kindOut = k[r.Next() % 5u];
        return 0x60;
    }
    if (roll == 5) { // a police record
        static const uint32_t k[3] = {0, 1, 6};
        kindOut = k[r.Next() % 3u];
        return 0xBD + r.U32() % 3u;
    }
    kindOut = static_cast<uint32_t>(r.Next() % 7u);
    return 0xE0;
}

// The world the object-sound functions read, redrawn around the snapshot's (see the bounds above).
void PlantWorld(CaseContext& c) {
    Rng& r = *c.rng;
    Memory& m = *c.mem;
    const uint32_t gs = m.PeekWord(kGameStatePtr);
    const int32_t clockC = static_cast<int32_t>(m.PeekWord(gs + 0x0C));
    const int32_t clockT = static_cast<int32_t>(m.PeekWord(gs + 0x10));
    const uint32_t L = m.PeekWord(c.gp + kGpListener);
    const uint32_t slots = m.PeekWord(c.gp + rr::sim::kObjSlotsGp);
    const uint32_t bike0 = m.PeekWord(kPlayerArr);
    // the listener record's object half
    EnPut(c, L + 0x18, r.U32() % 3u);
    EnPut(c, L + 0x1C, (r.Next() % 3u == 0) ? 0xFFFFFFFFu : r.U32() % 48u);
    EnPut(c, L + 0x20, (r.Next() % 4u == 0) ? r.U32() : r.U32() % 0x5400u);
    const uint32_t nObj = (r.Next() % 2u) ? 5u : r.U32() % 6u;
    uint32_t nCue = (r.Next() % 2u) ? 3u : r.U32() % (9u - nObj);
    if (nObj + nCue > 8u) nCue = 8u - nObj;
    EnPut(c, L + 0x24, nObj);
    EnPut(c, L + 0x28, nCue);
    EnPut(c, L + 0x2C, r.U32() % 2u);
    for (uint32_t k = 0; k < 2; ++k) {
        const bool armed = r.Next() % 3u == 0;
        EnPut(c, L + 0x30 + 4u * k, r.U32() % 110u);
        EnPut(c, L + 0x38 + 4u * k, armed ? bike0 + 1096u * (r.U32() % 18u) : 0u);
        EnPut(c, L + 0x40 + 4u * k, r.U32() % 4u);
    }
    // the eight slots
    for (uint32_t k = 0; k < 8; ++k) {
        const uint32_t e = slots + rr::sim::kObjSlotBytes * k;
        uint32_t kind = 0;
        const uint32_t id = Obj(r, kind);
        EnPut16(c, e + 0x00, static_cast<uint16_t>(id));
        EnPut(c, e + 0x04, r.U32() % 2u);
        EnPut(c, e + 0x08, kind);
        EnPut(c, e + 0x0C, m.PeekWord(bike0 + 0xB8) + static_cast<uint32_t>(r.World()));
        EnPut(c, e + 0x10, m.PeekWord(bike0 + 0xC0) + static_cast<uint32_t>(r.World()));
        EnPut(c, e + 0x14, static_cast<uint32_t>(r.World() >> 4));
        EnPut(c, e + 0x18, static_cast<uint32_t>(r.World() >> 4));
        EnPut(c, e + 0x1C, r.U32() % 0x2000u);
        EnPut(c, e + 0x20, EnHandle(c));
        EnPut(c, e + 0x24, static_cast<uint32_t>(clockC + static_cast<int32_t>(r.U32() % 200u) - 100));
        EnPut(c, e + 0x28, static_cast<uint32_t>(static_cast<int32_t>(r.U32() % 10u) - 1));
    }
    const uint32_t traffic = m.PeekWord(0x800CE4D0u + 3u * 16u);
    EnPut(c, traffic + 0xB4, 0);
    // the cue voice and the cue bank slots
    EnPut(c, rr::sim::kCueHandle + 0x00, (r.Next() % 2u) ? EnHandle(c) : 0u);
    EnPut(c, rr::sim::kCueHandle + 0x04, static_cast<uint32_t>(clockT + static_cast<int32_t>(r.U32() % 100u) - 50));
    EnPut(c, rr::sim::kCueHandle + 0x08, r.U32() % 2u);
    EnPut(c, rr::sim::kCueHandle + 0x0C, static_cast<uint32_t>(static_cast<int32_t>(r.U32() % 10u) - 1));
    EnPut(c, rr::sim::kCueHandle + 0x10, r.U32() % 2u);
    EnPut(c, rr::sim::kCueHandle + 0x14, static_cast<uint32_t>(clockT + static_cast<int32_t>(r.U32() % 100u) - 50));
    for (uint32_t k = 0; k < 9; ++k) {
        EnPut(c, rr::sim::kCueSlots + 32u * k + 0x00, static_cast<uint32_t>(static_cast<int32_t>(r.U32() % 4u) - 1));
        EnPut(c, rr::sim::kCueSlots + 32u * k + 0x08, r.U32() % 5u);
    }
    EnPut(c, 0x800D6C58u, (r.Next() % 2u) ? 23u : r.U32() % 32u);
    EnPut(c, c.gp + rr::sim::kPendingCueGp, r.U32() % 2u);
    EnPut(c, c.gp + rr::sim::kSirenSoundGp, (r.Next() % 3u == 0) ? 0xFFFFFFFFu : r.U32() % 25u);
    EnPut(c, c.gp + rr::sim::kCueBankGp, (r.Next() % 3u == 0) ? 0xFFFFFFFFu : r.U32() % 3u);
    EnPut(c, c.gp + rr::sim::kRoadMuteGp, (r.Next() % 6u == 0) ? 1u : 0u);
    EnPut(c, c.gp + rr::sim::kFrameCounterGp, r.U32());
    EnPut(c, 0x8005AD48u, (r.Next() % 2u) ? 8u : r.U32() % 16u);
    EnPut(c, rr::sim::kViewRecords + 0x228, (r.Next() % 2u) ? 2u : 0x32u);
    EnPut(c, rr::sim::kViewRecords + 0x304, r.U32() % 2u);
    // the police records the pick may append
    EnPut(c, rr::sim::kCopRecordsOn, r.U32() % 2u);
    const uint32_t cops = m.PeekWord(rr::sim::kCopRecordsPtr);
    for (uint32_t k = 0; k < 3; ++k) {
        EnPut16(c, cops + 280u * k + 0xAC, static_cast<uint16_t>((r.Next() % 2u) ? 0xBDu + k : 0u));
        EnPut8(c, cops + 280u * k + 8, static_cast<uint8_t>((r.Next() % 3u == 0) ? 9u : r.U32() % 12u));
    }
    // the game-state flag the cue and siren arms test, and the player's reverb bits
    static const uint8_t kFlags[4] = {0x22, 0x23, 0x21, 0x03};
    EnPut8(c, gs + 4, kFlags[r.Next() % 4u]);
    EnPut8(c, gs + 6, static_cast<uint8_t>(r.U32() % 18u));
    const uint32_t rd = m.PeekWord(bike0 + 0x43C);
    EnPut8(c, rd + 1, static_cast<uint8_t>(r.U32()));
    EnPut(c, bike0 + 0x24, (m.PeekWord(bike0 + 0x24) & ~0x60u) | ((r.U32() % 4u) << 5));
    EnPut16(c, bike0 + 0x172, static_cast<uint16_t>(r.U32() % 64u));
    // Half the time the view (the query's origin) sits on another bike, so the query finds the
    // objects binned around it - the collision grid itself is the snapshot's, only the origin
    // moves - and then, half of that, the finish-line cue's gate (gs+6, the rider def's kind 2,
    // the progress gap) is opened on that bike.
    if (r.Next() % 2u) {
        const uint32_t other = bike0 + 1096u * (1u + r.U32() % 17u);
        EnPut(c, rr::sim::kViewRecords + 0xB8, m.PeekWord(other + 0xB8) + r.U32() % 0x40000u);
        EnPut(c, rr::sim::kViewRecords + 0xC0, m.PeekWord(other + 0xC0) + r.U32() % 0x40000u);
        // and the listener with it, so the nearest-first sort sees distances of its own scale
        EnPut(c, L + 4, m.PeekWord(other + 0xB8) + static_cast<uint32_t>(r.World()));
        EnPut(c, L + 8, m.PeekWord(other + 0xC0) + static_cast<uint32_t>(r.World()));
        if (r.Next() % 2u) {
            EnPut8(c, gs + 6, static_cast<uint8_t>(m.PeekWord(other + 0xAC)));
            EnPut8(c, rd + 1, static_cast<uint8_t>((r.U32() & 0xF0u) | 2u));
            EnPut(c, bike0 + 0x144, m.PeekWord(other + 0x144) + r.U32() % 0x40000u - 0x20000u);
        }
    }
    // One case in three a small crowd around the view: four bikes moved next to it and chained into
    // the collision grid's cell under it (nodes 120..123 {next, id}, the old chain after them) - the
    // only way the nearest-first sort gets several distances to order
    if (r.Next() % 3u == 0) {
        const int32_t vx = static_cast<int32_t>(m.PeekWord(rr::sim::kViewRecords + 0xB8));
        const int32_t vz = static_cast<int32_t>(m.PeekWord(rr::sim::kViewRecords + 0xC0));
        const int32_t xc = ((vx - static_cast<int32_t>(m.PeekWord(rr::sim::kGridOriginX))) >> 21) + 11;
        const int32_t zc = ((vz - static_cast<int32_t>(m.PeekWord(rr::sim::kGridOriginZ))) >> 21) + 11;
        if (xc >= 0 && xc < 24 && zc >= 0 && zc < 24) {
            const uint32_t cell = rr::sim::kGridCells + static_cast<uint32_t>(xc + zc * 24);
            const uint8_t oldHead = m.PeekByte(cell);
            for (uint32_t n = 120; n < 124; ++n) {
                const uint32_t k = 1u + r.U32() % 17u;
                EnPut8(c, rr::sim::kGridNodes + 2u * n, static_cast<uint8_t>(n < 123 ? n + 1 : oldHead));
                EnPut8(c, rr::sim::kGridNodes + 2u * n + 1u, static_cast<uint8_t>(k));
                EnPut(c, bike0 + 1096u * k + 0xB8, static_cast<uint32_t>(vx + (r.World() >> 3)));
                EnPut(c, bike0 + 1096u * k + 0xC0, static_cast<uint32_t>(vz + (r.World() >> 3)));
            }
            EnPut8(c, cell, 120);
        }
    }
    // the riders' on-the-bike states and the bikes' +0xB4, which the object passes branch on
    for (uint32_t k = 0; k < 18; ++k) {
        const uint32_t b = bike0 + 1096u * k;
        if (r.Next() % 4u == 0) EnPut(c, b + 0xB4, r.U32() % 30u);
        if (r.Next() % 6u == 0) EnPut(c, m.PeekWord(b + 0x354) + 0x25C, 1u + r.U32() % 4u);
    }
}

// RASHCDI.BIN out of the player's own disc extract. A RASHCDI function is benched by copying ITS OWN
// bytes back to ITS OWN address on both machines before the case (the rr-race machine holds RASHCDG
// in that window) - sound for a leaf whose branches are relative and whose every data reference is
// an absolute address, which is what 0x80063448 is (no jal, no computed jump, no gp).
const std::vector<uint8_t>& Rashcdi() {
    static const std::vector<uint8_t> image = [] {
        std::vector<uint8_t> v;
        std::string error;
        if (!rr::interp::ReadWholeFile("work\\disc_us\\RASHCDI.BIN", v, error)) {
            std::fprintf(stderr, "records_init: work\\disc_us\\RASHCDI.BIN not readable (%s); the row will fail\n",
                         error.c_str());
            v.clear();
        }
        return v;
    }();
    return image;
}
void InjectLeaf(CaseContext& c, uint32_t from, uint32_t to) {
    const std::vector<uint8_t>& img = Rashcdi();
    const uint32_t off = from - kOverlayBase;
    if (img.size() < off + (to - from)) return;
    c.mem->WriteBlock(from, img.data() + off, to - from);
}
} // namespace sf

void AddSoundFrameRows(std::vector<Row>& rows) {
    // ---- SLUS 0x8001FBD4 ResetVoiceLists
    {
        Row row{"reset_voice_lists", "void ResetVoiceLists(void)  [SLUS, 22 reserved / 24 free / 25 ring]", sf::kResetListsFn,
                [](CaseContext& c, Args&) {
                    if (c.sample) return;
                    Rng& r = *c.rng;
                    for (uint32_t off = 0x58; off < 0x188; off += 4) EnPut(c, kSoundSys + off, r.U32());
                },
                nullptr, /*compareV0=*/false};
        row.nativeEx = [](NativeEnv& env, const Args&) {
            EnRun run(env);
            rr::sim::ResetVoiceLists(run.s);
            return run.Done(env, 0);
        };
        rows.push_back(row);
    }

    // ---- SLUS 0x8001E5A8 ResetSoundState
    {
        Row row{"reset_sound_state", "void ResetSoundState(void)  [SLUS, the record's header, 127, lists]", sf::kResetStateFn,
                [](CaseContext& c, Args&) {
                    if (c.sample) return;
                    Rng& r = *c.rng;
                    for (uint32_t off = 0; off < 0x188; off += 4) EnPut(c, kSoundSys + off, r.U32());
                },
                nullptr, /*compareV0=*/false};
        row.nativeEx = [](NativeEnv& env, const Args&) {
            EnRun run(env);
            rr::sim::ResetSoundState(run.s);
            return run.Done(env, 0);
        };
        rows.push_back(row);
    }

    // ---- SLUS 0x8001EAA4 PatchBank(bank, base)
    {
        Row row{"patch_bank", "void PatchBank(Bank*, u32 base)  [SLUS, descriptor SPU addresses += base]", sf::kPatchBankFn,
                [](CaseContext& c, Args& a) {
                    Rng& r = *c.rng;
                    if (c.sample) {
                        // one of the snapshot's own resident banks
                        const uint32_t table = c.mem->PeekWord(kSoundSys + 0x04);
                        a.a[0] = c.mem->PeekWord(table + 4u * (r.U32() % 3u));
                    } else {
                        EnPlantBank(c, c.scratch + en::kBankRoad, 768, 1 + r.U32() % 40u, 5);
                        // a record may hold more than one descriptor: up to 3, inside the window
                        const uint32_t b = c.scratch + en::kBankRoad;
                        const uint32_t o = c.mem->PeekWord(b + 0x10);
                        if (o != 0 && o + 40u <= 768u && r.Next() % 3u == 0)
                            EnPut8(c, b + o, static_cast<uint8_t>(1 + r.U32() % 3u));
                        a.a[0] = b;
                    }
                    a.a[1] = (r.Next() % 2u) ? 0x1010u + (r.U32() & 0x7FFF0u) : r.U32();
                },
                nullptr, /*compareV0=*/false};
        row.nativeEx = [](NativeEnv& env, const Args& a) {
            EnRun run(env);
            rr::sim::PatchBank(run.s, a.a[0], a.a[1]);
            return run.Done(env, 0);
        };
        rows.push_back(row);
    }

    // ---- SLUS 0x8001F0D4 ReleaseAllVoices
    {
        Row row{"release_all_voices", "void ReleaseAllVoices(void)  [SLUS, release + key off every voice, service]",
                sf::kReleaseAllFn,
                [](CaseContext& c, Args&) {
                    if (!c.sample) EnPlantSystem(c);
                    EnPlantSpu(c);
                    Rng& r = *c.rng;
                    EnPut(c, kSoundSys + 0x10, (r.Next() % 8u == 0) ? 1u : 0u);
                    EnPut(c, kSoundSys + 0x14, (r.Next() % 3u == 0) ? 0u : r.U32() >> (r.Next() % 16u));
                    EnPut(c, rr::sim::kSpuKeyedMask, r.U32());
                },
                nullptr};
        row.nativeEx = [](NativeEnv& env, const Args&) {
            EnRun run(env);
            return run.Done(env, rr::sim::ReleaseAllVoices(run.s));
        };
        row.compareSpuControl = true;
        rows.push_back(row);
    }

    // ---- RASHCDI 0x80063448 SoundRecordsInit(p): the loader's leaf that makes the first EngineNote
    // of a race NOT start the idle loop twice
    {
        Row row{"records_init", "void SoundRecordsInit(int p)  [RASHCDI leaf, handles -1, slots 0xE0]", sf::kRecordsInitFn,
                [](CaseContext& c, Args& a) {
                    sf::InjectLeaf(c, 0x80063448u, 0x80063500u);
                    Rng& r = *c.rng;
                    if (c.sample) {
                        a.a[0] = 0;
                        return;
                    }
                    // two players' records of our own, in the scratch block
                    const uint32_t E = c.scratch + 0x0000, L = c.scratch + 0x0200, S0 = c.scratch + 0x0400;
                    for (uint32_t i = 0; i < 0x0A00; i += 4) EnPut(c, c.scratch + i, r.U32());
                    EnPut(c, c.gp + rr::sim::kEngineRecGp, E);
                    EnPut(c, c.gp + kGpListener, L);
                    EnPut(c, c.gp + rr::sim::kObjSlotsGp, S0);
                    EnPut(c, c.gp + rr::sim::kObjSlotsGp + 4, S0 + 0x200);
                    for (uint32_t p = 0; p < 2; ++p) {
                        const uint32_t n = r.U32() % 9u;
                        EnPut(c, L + 72u * p + 0x24, n);
                        EnPut(c, L + 72u * p + 0x28, (r.Next() % 4u == 0) ? 0u : r.U32() % (9u - n));
                    }
                    a.a[0] = r.U32() % 2u;
                },
                nullptr, /*compareV0=*/false};
        row.nativeEx = [](NativeEnv& env, const Args& a) {
            EnRun run(env);
            rr::sim::SoundRecordsInit(run.s, static_cast<int32_t>(a.a[0]));
            return run.Done(env, 0);
        };
        rows.push_back(row);
    }

    // ---- SLUS 0x8001F054 SetReverbDepth(l, r) -> libspu 0x800505F8
    {
        Row row{"set_reverb_depth", "void SetReverbDepth(u16 l, u16 r)  [SLUS -> libspu 0x800505F8, 0x1F801D84/86]",
                sf::kReverbFn,
                [](CaseContext& c, Args& a) {
                    EnPlantSpu(c);
                    Rng& r = *c.rng;
                    a.a[0] = (r.Next() % 2u) ? r.U32() % 0x5000u : r.U32();
                    a.a[1] = (r.Next() % 2u) ? a.a[0] : r.U32();
                },
                nullptr};
        row.nativeEx = [](NativeEnv& env, const Args& a) {
            EnRun run(env);
            return run.Done(env, rr::sim::SetReverbDepth(run.s, a.a[0], a.a[1]));
        };
        row.compareSpuControl = true;
        rows.push_back(row);
    }

    // ---- SLUS 0x8001B244 SpeechCue(kind)
    {
        Row row{"speech_cue", "void SpeechCue(int kind)  [SLUS, the one cue voice 0x800D6BC0]", sf::kSpeechCueFn,
                [](CaseContext& c, Args& a) {
                    if (!c.sample) sf::PlantWorld(c);
                    EnPlantSpu(c);
                    Rng& r = *c.rng;
                    static const int32_t kKinds[6] = {3, 4, 4, 3, 0, 7};
                    a.a[0] = static_cast<uint32_t>((r.Next() % 4u == 0) ? static_cast<int32_t>(r.U32() % 32u)
                                                                        : kKinds[r.Next() % 6u]);
                    if (r.Next() % 2u) EnPut(c, rr::sim::kCueSlots + 0xC8u, 2);
                    if (r.Next() % 2u) EnPut(c, rr::sim::kCueSlots + 0xC0u, r.U32() % 3u);
                },
                nullptr, /*compareV0=*/false};
        row.nativeEx = [](NativeEnv& env, const Args& a) {
            EnRun run(env);
            rr::sim::SpeechCue(run.s, static_cast<int32_t>(a.a[0]));
            return run.Done(env, 0);
        };
        row.compareSpuControl = true;
        rows.push_back(row);
    }

    // ---- RASHCDG 0x8008AE94 SpatialQuery(hits, &count, pos, reach, mask, self)
    {
        Row row{"spatial_query", "void SpatialQuery(u16*, s32*, pos, reach, mask, self)  [RASHCDG, the collision grid]",
                sf::kQueryFn,
                [](CaseContext& c, Args& a) {
                    Rng& r = *c.rng;
                    Memory& m = *c.mem;
                    const uint32_t hits = c.scratch + 0x100, count = c.scratch + 0x180, pos = c.scratch + 0x1A0;
                    for (uint32_t i = 0; i < 0x40; i += 4) EnPut(c, hits + i, r.U32());
                    EnPut(c, count, 1u + r.U32() % 16u);
                    const uint32_t bike0 = m.PeekWord(kPlayerArr);
                    const uint32_t anchor = (r.Next() % 3u == 0) ? rr::sim::kViewRecords + 0xB8u
                                                                 : bike0 + 1096u * (r.U32() % 18u) + 0xB8u;
                    EnPut(c, pos + 0, m.PeekWord(anchor + 0) + static_cast<uint32_t>(c.sample ? 0 : r.World()));
                    EnPut(c, pos + 4, r.U32());
                    EnPut(c, pos + 8, m.PeekWord(anchor + 8) + static_cast<uint32_t>(c.sample ? 0 : r.World()));
                    a.a[0] = hits;
                    a.a[1] = count;
                    a.a[2] = pos;
                    a.a[3] = (r.Next() % 2u) ? 0x800000u : r.U32() % 0x4000000u;
                    // the class mask, sp+16: classes 0..3 are the live pools; 4..6 have no base here
                    a.a4 = ((r.Next() % 2u) ? 9u : r.U32()) & 0xFu;
                    a.more[0] = (r.Next() % 2u) ? (m.PeekWord(bike0 + 0xAC) & 0xFFFFu) : r.U32(); // self, sp+20
                    if (r.Next() % 6u == 0) EnPut(c, m.PeekWord(kGameStatePtr) + 0x30, 2);
                },
                nullptr, /*compareV0=*/false};
        row.nativeEx = [](NativeEnv& env, const Args& a) {
            EnRun run(env);
            rr::sim::SpatialQuery(run.s, a.a[0], a.a[1], a.a[2], static_cast<int32_t>(a.a[3]), a.a4, a.more[0]);
            return run.Done(env, 0);
        };
        rows.push_back(row);
    }

    // ---- the object slots: Start, Stop, Update, Claim
    {
        Row row{"obj_sound_start", "void ObjSoundStart(slot, p, bank, sound)  [SLUS, 3D + reserved voice]", sf::kObjStartFn,
                [](CaseContext& c, Args& a) {
                    if (!c.sample) sf::PlantWorld(c);
                    EnPlantSpu(c);
                    Rng& r = *c.rng;
                    a.a[0] = r.U32() % 8u;
                    a.a[1] = 0;
                    a.a[2] = (r.Next() % 4u == 0) ? static_cast<uint32_t>(static_cast<int32_t>(r.U32() % 5u) - 1) : 1u;
                    a.a[3] = (r.Next() % 2u) ? 11u : r.U32() % 30u;
                },
                nullptr, /*compareV0=*/false};
        row.nativeEx = [](NativeEnv& env, const Args& a) {
            EnRun run(env);
            rr::sim::ObjSoundStart(run.s, static_cast<int32_t>(a.a[0]), static_cast<int32_t>(a.a[1]),
                                   static_cast<int32_t>(a.a[2]), static_cast<int32_t>(a.a[3]), env.sp);
            return run.Done(env, 0);
        };
        row.compareSpuControl = true;
        rows.push_back(row);
    }
    {
        Row row{"obj_sound_stop", "void ObjSoundStop(slot, p)  [SLUS, recursive over the cue slots]", sf::kObjStopFn,
                [](CaseContext& c, Args& a) {
                    if (!c.sample) sf::PlantWorld(c);
                    EnPlantSpu(c);
                    a.a[0] = c.rng->U32() % 8u;
                    a.a[1] = 0;
                },
                nullptr, /*compareV0=*/false};
        row.nativeEx = [](NativeEnv& env, const Args& a) {
            EnRun run(env);
            rr::sim::ObjSoundStop(run.s, static_cast<int32_t>(a.a[0]), static_cast<int32_t>(a.a[1]));
            return run.Done(env, 0);
        };
        row.compareSpuControl = true;
        rows.push_back(row);
    }
    {
        Row row{"obj_sound_update", "void ObjSoundUpdate(slot, p)  [SLUS, UpdateVoice 0x8001F6A4]", sf::kObjUpdateFn,
                [](CaseContext& c, Args& a) {
                    if (!c.sample) sf::PlantWorld(c);
                    EnPlantSpu(c);
                    a.a[0] = c.rng->U32() % 8u;
                    a.a[1] = 0;
                },
                nullptr, /*compareV0=*/false};
        row.nativeEx = [](NativeEnv& env, const Args& a) {
            EnRun run(env);
            rr::sim::ObjSoundUpdate(run.s, static_cast<int32_t>(a.a[0]), static_cast<int32_t>(a.a[1]), env.sp);
            return run.Done(env, 0);
        };
        row.compareSpuControl = true;
        rows.push_back(row);
    }
    {
        Row row{"obj_sound_claim", "void ObjSoundClaim(u16 handle, p)  [SLUS, sound 11 on a cue slot]", sf::kObjClaimFn,
                [](CaseContext& c, Args& a) {
                    if (!c.sample) sf::PlantWorld(c);
                    EnPlantSpu(c);
                    uint32_t kind = 0;
                    uint32_t id = sf::Obj(*c.rng, kind);
                    if (id == 0xE0 || (id >> 5) == 5) id = c.rng->U32() % 18u; // the callers pass a live object
                    a.a[0] = id;
                    a.a[1] = 0;
                },
                nullptr, /*compareV0=*/false};
        row.nativeEx = [](NativeEnv& env, const Args& a) {
            EnRun run(env);
            rr::sim::ObjSoundClaim(run.s, a.a[0], static_cast<int32_t>(a.a[1]), env.sp);
            return run.Done(env, 0);
        };
        row.compareSpuControl = true;
        rows.push_back(row);
    }

    // ---- SLUS 0x800184AC ObjSoundPick(p): the nearest objects, every fourth frame
    {
        Row row{"obj_sound_pick", "void ObjSoundPick(int p)  [SLUS, query + sort + slot turnover]", sf::kObjPickFn,
                [](CaseContext& c, Args& a) {
                    if (!c.sample) sf::PlantWorld(c);
                    EnPlantSpu(c);
                    a.a[0] = 0;
                },
                nullptr, /*compareV0=*/false};
        row.nativeEx = [](NativeEnv& env, const Args& a) {
            EnRun run(env);
            rr::sim::ObjSoundPick(run.s, static_cast<int32_t>(a.a[0]), env.sp);
            return run.Done(env, 0);
        };
        row.compareSpuControl = true;
        row.maxSteps = 20'000'000;
        rows.push_back(row);
    }

    // ---- SLUS 0x80018FAC AudioFrame: the whole per-frame sound pass. RoadNote's two effect
    // spawners are the oracle's, at the depth the original reaches them (AudioFrame 64 + RoadNote 96).
    {
        Row row{"audio_frame", "void AudioFrame(void)  [SLUS, listener, reverb, engine, objects, cue]", sf::kAudioFrameFn,
                [](CaseContext& c, Args&) {
                    Rng& r = *c.rng;
                    if (!c.sample) {
                        sf::PlantWorld(c);
                        // the engine record, one case in four re-armed for a race's first frame
                        // (the loader's -1 handles, or the zeroes a fresh record would hold)
                        const uint32_t E = c.mem->PeekWord(c.gp + rr::sim::kEngineRecGp);
                        if (r.Next() % 4u == 0) {
                            EnPut8(c, E + 4, 0);
                            const uint32_t fill = (r.Next() % 2u) ? 0xFFFFFFFFu : 0u;
                            for (uint32_t off : {0x10u, 0x14u, 0x18u, 0x24u, 0x28u}) EnPut(c, E + off, fill);
                        }
                    }
                    EnPlantSpu(c);
                },
                nullptr, /*compareV0=*/false};
        row.nativeEx = [](NativeEnv& env, const Args&) {
            EnRun run(env);
            rr::sim::AudioFrame(run.s, env.sp + sf::kAudioFrameFrame);
            return run.Done(env, 0);
        };
        OracleCallee burst;
        burst.address = en::kFxBurstFn;
        burst.arity = 4;
        burst.frame = en::kNoteFrame;
        OracleCallee spray;
        spray.address = en::kFxSprayFn;
        spray.arity = 3;
        spray.frame = en::kNoteFrame;
        row.oracleCallees = {burst, spray};
        row.oracleFrame = sf::kAudioFrameFrame;
        row.compareSpuControl = true;
        row.maxSteps = 40'000'000;
        rows.push_back(row);
    }

    // ---- the music: the shuffle, the stream voice, the key-on of a handle list
    {
        Row row{"music_pick_shuffle", "u32 MusicPickShuffle(void)  [SLUS 0x80024B20, ALBUM.ALB's 18 tracks]", 0x80024B20u,
                [](CaseContext& c, Args&) {
                    EnPlantSpu(c); // root counter 2 drawn per case
                    if (c.sample) return;
                    Rng& r = *c.rng;
                    // named bound: the count is the executable's 18, or smaller (never past the table)
                    EnPut(c, rr::sim::kAlbumTable, (r.Next() % 4u == 0) ? r.U32() % 19u : 18u);
                    const uint64_t mode = r.Next() % 4u;
                    for (uint32_t i = 0; i < 18; ++i) {
                        uint32_t f = r.U32() & ~3u;
                        if (mode == 0) f |= 1u | (r.U32() & 2u);          // all selectable
                        else if (mode == 1) f |= 3u;                      // all played: a new cycle
                        else if (mode == 2) f |= (r.Next() % 8u == 0) ? 1u : 0u; // few selectable
                        else f |= r.U32() & 3u;
                        EnPut(c, rr::sim::kAlbumTable + 16u + 12u * i, f);
                    }
                },
                nullptr};
        row.nativeEx = [](NativeEnv& env, const Args&) {
            EnRun run(env);
            return run.Done(env, rr::sim::MusicPickShuffle(run.s));
        };
        rows.push_back(row);
    }
    {
        Row row{"stream_voice_start", "u32 StreamVoiceStart(variant, SoundParams*, spuAddr)  [SLUS 0x8001F37C]", 0x8001F37Cu,
                [](CaseContext& c, Args& a) {
                    if (!c.sample) EnPlantSystem(c);
                    EnPlantSpu(c);
                    Rng& r = *c.rng;
                    for (uint32_t k = 0; k < 3; ++k)
                        EnPut(c, kSoundSys + 0x1C + 20u * k, (r.Next() % 3u == 0) ? r.U32() : ((r.Next() % 2u) ? 1u : 0u));
                    EnPut(c, kSoundSys + 0x184, (r.Next() % 5u == 0) ? r.U32() : 0u);
                    const uint32_t P = c.scratch + en::kSndPar;
                    EnPut(c, P + 0, (r.Next() % 2u) ? 0x5CEu : r.U32());
                    EnPut(c, P + 4, (r.Next() % 2u) ? r.U32() % 128u : static_cast<uint32_t>(r.S32() >> (r.Next() % 24u)));
                    EnPut(c, P + 8, (r.Next() % 3u == 0) ? r.U32() : ((r.Next() % 2u) ? 0u : 127u) + r.U32() % 130u);
                    a.a[0] = (r.Next() % 2u) ? 0u : ((r.Next() % 2u) ? 1u : r.U32());
                    a.a[1] = P;
                    a.a[2] = (r.Next() % 2u) ? 0x434E0u + 0x10000u * (r.U32() % 2u) : r.U32();
                },
                nullptr};
        row.nativeEx = [](NativeEnv& env, const Args& a) {
            EnRun run(env);
            rr::sim::SoundParams p;
            p.pitch = run.g.S32(a.a[1] + 0);
            p.volume = run.g.S32(a.a[1] + 4);
            p.pan = run.g.S32(a.a[1] + 8);
            return run.Done(env, rr::sim::StreamVoiceStart(run.s, static_cast<int32_t>(a.a[0]), p, a.a[2]));
        };
        row.compareSpuControl = true;
        rows.push_back(row);
    }
    {
        Row row{"key_on_handles", "void KeyOnHandles(int n, u32* handles)  [SLUS 0x8001F544]", 0x8001F544u,
                [](CaseContext& c, Args& a) {
                    if (!c.sample) EnPlantSystem(c);
                    Rng& r = *c.rng;
                    const uint32_t H = c.scratch + 0x100;
                    for (uint32_t k = 0; k < 8; ++k) EnPut(c, H + 4u * k, EnHandle(c));
                    a.a[0] = (r.Next() % 6u == 0) ? static_cast<uint32_t>(-static_cast<int32_t>(r.U32() % 3u)) : r.U32() % 9u;
                    a.a[1] = H;
                },
                nullptr};
        row.nativeEx = [](NativeEnv& env, const Args& a) {
            EnRun run(env);
            return run.Done(env, rr::sim::KeyOnHandles(run.s, static_cast<int32_t>(a.a[0]), a.a[1]));
        };
        rows.push_back(row);
    }
}
