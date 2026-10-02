// Bench rows for the riders' voices, src\game\sim\speech.h.
// Included by verify_physics.cpp inside its anonymous namespace after rows_engine_note.inc and
// rows_fight.inc, whose helpers (EnRun, EnPlantSpu, sf::PlantWorld, fg::Stir, fg::Bench) it uses.
//
// Every port is a GUEST-ADDRESS port and runs on the clone's RAM. Its callees:
//   * PORTED here or elsewhere and run natively: ObjSoundStop/Start, GetSoundPitch, PlaySound3D, GetRCnt,
//     PatchBank (sound_frame.h / sound_engine.h), Pick, CanEngage, NextMove (fight.h), AiProject and
//     AiPushCommand (ai.h - its stance request on the oracle, as in rows_fight.inc);
//   * executed by the ORACLE and witnessed: ComputePlace SLUS 0x800138E8 (ported on a host view), the
//     heap's malloc 0x8001447C, the CD streamer 0x80023148, SpuFree 0x8004F998, the stream buffer
//     release 0x80030FA0, and LoadBank's upload 0x8001E938.
//
// Named bounds (each keeps the case away from hardware the bench cannot answer, never from a branch of
// the function under test):
//   * the upload 0x8001E938 ends in a DMA transfer (channel 4), which traps on both machines: the load
//     rows keep the slot's old bank out of 0..count (so BankFree does nothing) and either fill the bank
//     table (LoadBank -> -1) or plant a taken word past it (-> -2). The upload itself is the product's.
//   * the CD streamer 0x80023148 is reached only with the stream record's +0x14 bit 1 set, its "busy"
//     early return (-1) - the CD is not touched. The stream itself is the product's.
namespace sp {

constexpr uint32_t kComputePlace = 0x800138E8, kMalloc = 0x8001447C, kStream = 0x80023148;
constexpr uint32_t kUpload = 0x8001E938, kSpuFree = 0x8004F998, kRelease = 0x80030FA0;
constexpr uint32_t kSlotFindFn = 0x8001A2E4, kBankLoadFn = 0x8001A0C0, kUploadedFn = 0x80016464;
constexpr uint32_t kInitFn = 0x8001A424, kSpeechFn = 0x8001A760, kLoadBankFn = 0x8001ED28;
constexpr uint32_t kBankFreeFn = 0x8001EE28;

struct Bench final : rr::sim::SpeechCallees {
    NativeEnv& env;
    explicit Bench(NativeEnv& e) : env(e) {}
    bool Ok() const { return env.failure.empty(); }
    bool ComputePlace(uint32_t bike, int32_t mode, uint32_t& v0) override {
        v0 = env.CallOracle(kComputePlace, bike, static_cast<uint32_t>(mode), 0, 0);
        return Ok();
    }
    bool PushCommand(uint32_t cmd, int32_t mode, uint32_t e) override { // PORTED (ai.h), as rows_fight.inc runs it
        uint8_t rec[8];
        env.clone->mem.ReadBlock(cmd, rec, 8);
        fg::Bench fb(env);
        const bool ok = fb.PushCommand(rec, cmd, mode, e);
        env.clone->mem.WriteBlock(cmd, rec, 8);
        return ok;
    }
    bool Malloc(uint32_t bytes, uint32_t a1, uint32_t& v0) override {
        v0 = env.CallOracle(kMalloc, bytes, a1, 0, 0);
        return Ok();
    }
    bool StreamRequest(uint32_t st, uint32_t n, uint32_t a2, uint32_t a3, uint32_t& v0) override {
        v0 = env.CallOracle(kStream, st, n, a2, a3);
        return Ok();
    }
    bool UploadSamples(uint32_t bank, uint32_t src, uint32_t cb, uint32_t arg, uint32_t a4, uint32_t& v0) override {
        const uint32_t a[5] = {bank, src, cb, arg, a4};
        v0 = env.CallOracleArgs(kUpload, a, 5);
        return Ok();
    }
    bool SpuFree(uint32_t addr) override {
        env.CallOracle(kSpuFree, addr, 0, 0, 0);
        return Ok();
    }
    bool ReleaseBuffer(uint32_t index) override {
        env.CallOracle(kRelease, index, 0, 0, 0);
        return Ok();
    }
};

std::vector<OracleCallee> Seams(uint32_t pushFrame) {
    return {OracleCallee{kComputePlace, 2}, OracleCallee{kMalloc, 2},   OracleCallee{kStream, 4},
            OracleCallee{kUpload, 4, 1},     OracleCallee{kSpuFree, 1}, OracleCallee{kRelease, 1},
            OracleCallee{kAiStance, 3, 0, {}, pushFrame}};
}

// The categories a slot or a candidate can hold: the record ids of AUDTAUNT.STR (3, 4, 6, 7, 22, 23, 32,
// 33), the default bank's 11 and 15, the used-once 68/69, a character's own 0..2 / 16..18 / 32..34, the
// empty 255 and the used-up -1.
uint32_t SomeCategory(Rng& r) {
    static const uint32_t k[] = {3, 4, 5, 6, 7, 11, 15, 22, 23, 32, 33, 68, 69, 0, 1, 2, 16, 17, 18, 34, 255,
                                 0xFFFFFFFFu, 0x16, 0x26};
    return k[r.Next() % (sizeof(k) / sizeof(k[0]))];
}

// The nine slots and their category table, inside the ranges the original's own writers keep: a
// slot's bank is a registered bank slot 0..8 of this machine or -1..-3, its state 0..4, its sound
// indices 0 / 1 (88/89/108 for the default bank's), its header block a word-aligned buffer.
void PlantSlots(CaseContext& c, bool keepBanks) {
    Rng& r = *c.rng;
    for (uint32_t i = 0; i < 9; ++i) {
        const uint32_t t = rr::sim::kSpeechSlots + 32u * i;
        if (!keepBanks) {
            const uint64_t roll = r.Next() % 6u;
            EnPut(c, t + 0, roll == 0 ? static_cast<uint32_t>(-1 - static_cast<int32_t>(r.U32() % 3u)) : r.U32() % 9u);
        }
        EnPut(c, t + 4, (r.U32() % 40u) | ((0x81u + r.U32() % 27u) << 8));
        EnPut(c, t + 8, r.U32() % 5u);
        EnPut(c, t + 12, SomeCategory(r));
        EnPut(c, t + 16, SomeCategory(r));
        const bool def = r.Next() % 4u == 0;
        EnPut(c, t + 20, def ? 88u : 0u);
        EnPut(c, t + 24, def ? 89u + (r.Next() % 2u) * 19u : 1u);
        EnPut(c, t + 28, c.scratch + 0x1000u + 0x40u * i);
        EnPut(c, rr::sim::kSpeechCategories + 4u * i, SomeCategory(r));
    }
    EnPut(c, c.gp + rr::sim::kSpeechToggleGp, r.U32() % 2u);
}

// ---------------------------------------------------------------- RiderSpeech's world
// Dump-derived: the snapshot's own speech slots, listener and object slots, a live speaker; the
// hardware counter redrawn. Randomised: fg::Stir's bikes (a speaker, a bike near it and a partner),
// sf::PlantWorld's listener and slots, the speech table redrawn, and every field RiderSpeech branches on.
uint32_t PlantSpeech(CaseContext& c, int32_t& crash) {
    Rng& r = *c.rng;
    Memory& m = *c.mem;
    EnPlantSpu(c);
    crash = static_cast<int32_t>(r.Next() % 2u);
    const uint32_t pool0 = m.PeekWord(kPoolTable);
    if (c.sample != nullptr) return r.U32() % 18u;
    const fg::World w = fg::Stir(c, r.Next() % 2u == 0, true);
    sf::PlantWorld(c);
    PlantSlots(c, false);
    uint32_t me = w.me;
    if (me == 0) me = pool0;
    const uint32_t h = GetS16(m, me + 0xACu) & 0xFFFFu;
    const uint32_t gs = m.PeekWord(kGameStatePtr);
    // the listener's bike near the speaker or far from it (the 0xF000 test)
    const uint32_t B = m.PeekWord(kPlayerArr);
    if (B != me) {
        const int32_t far = (r.Next() % 4u == 0) ? 0x10000 + static_cast<int32_t>(r.U32() % 0x40000u) : 0;
        EnPut(c, me + 324u, m.PeekWord(B + 324u) + static_cast<uint32_t>(static_cast<int32_t>(r.U32() % 0x1E000u) - 0xF000 + far));
    }
    // the speaker's runtime record: kind (low nibble 0..2), character (high nibble 0..6)
    const uint32_t rd = m.PeekWord(me + 1084u);
    EnPut8(c, rd + 1u, static_cast<uint8_t>(((r.U32() % 7u) << 4) | (r.U32() % 3u)));
    EnPut8(c, gs + 4u, static_cast<uint8_t>(r.Next() % 5u == 0 ? 33u : (r.Next() % 2u ? 0x22u : 0x23u)));
    EnPut16(c, me + 870u, static_cast<uint16_t>(r.U32()));
    EnPut(c, gs + 12u, r.U32() % 0x1000000u);
    if (r.Next() % 3u == 0) EnPut16(c, me + 870u, static_cast<uint16_t>(static_cast<int32_t>(m.PeekWord(gs + 12u)) >> 8));
    EnPut(c, me + 480u, r.U32() % 0x400000u);
    // the provocation's target: the bike Stir put near the speaker, under AI control with a top op 4..16
    if (w.t != 0 && r.Next() % 2u == 0) {
        EnPut(c, w.t + 324u, m.PeekWord(me + 324u) + (r.U32() % 0x28000u) - 0x14000u);
        EnPut(c, w.t + 560u, m.PeekWord(w.t + 560u) | 0x08000000u);
    }
    // the listener's "an AI's taunt is playing"
    const uint32_t L = m.PeekWord(c.gp + kGpListener);
    EnPut(c, L + 44u, r.U32() % 2u);
    // one case in four the speaker's line is already on one of the listener's cue slots
    if (r.Next() % 4u == 0) {
        const uint32_t first = m.PeekWord(L + 36u), n = m.PeekWord(L + 40u);
        if (n != 0 && first + n <= 8u) {
            const uint32_t e = m.PeekWord(c.gp + rr::sim::kObjSlotsGp) + 44u * (first + r.U32() % n);
            EnPut16(c, e, static_cast<uint16_t>(h));
            EnPut(c, e + 8u, r.U32() % 4u);
        }
    }
    // one case in four the speaker and the bikes near it share a piece whose road id has high bits
    if (r.Next() % 4u == 0) {
        const uint32_t piece = (1u + r.U32() % 3u) << 16 | (r.U32() & 0xFFFFu);
        for (uint32_t b : {me, B, w.t}) {
            if (b == 0) continue;
            EnPut(c, b + 360u, piece);
            if (r.Next() % 2u) EnPut(c, b + 336u, m.PeekWord(me + 336u));
        }
    }
    return h;
}

} // namespace sp

void AddSpeechRows(std::vector<Row>& rows) {
    using namespace sp;
    // ---- SLUS 0x8001A2E4 SpeechSlotFind(id)
    {
        Row row{"speech_slot_find", "int SpeechSlotFind(u32 id)  [SLUS, the slot a taunt record goes to]", kSlotFindFn,
                [](CaseContext& c, Args& a) {
                    Rng& r = *c.rng;
                    EnPlantSpu(c);
                    if (!c.sample) PlantSlots(c, false);
                    EnPut(c, c.gp + kGpDefaultBank, r.Next() % 2u ? 0u : r.U32() % 9u);
                    // the engine record's bike, or none (the place band then stays 2)
                    const uint32_t E = c.mem->PeekWord(c.gp + rr::sim::kEngineRecGp);
                    if (E != 0 && r.Next() % 5u == 0) EnPut(c, E, 0);
                    if (r.Next() % 8u == 0) EnPut(c, c.gp + rr::sim::kEngineRecGp, 0);
                    static const uint32_t ids[] = {3, 4, 6, 7, 22, 23, 32, 33, 11, 15, 0x10, 0x11, 0x21, 0x20};
                    a.a[0] = (r.Next() % 6u == 0 ? r.U32() : ids[r.Next() % 14u] | ((0x81u + r.U32() % 27u) << 8));
                },
                nullptr};
        row.nativeEx = [](NativeEnv& env, const Args& a) {
            EnRun run(env);
            Bench b(env);
            return run.Done(env, static_cast<uint32_t>(rr::sim::SpeechSlotFind(run.s, b, a.a[0])));
        };
        row.oracleCallees = Seams(0);
        row.oracleFrame = 32;
        rows.push_back(row);
    }

    // ---- SLUS 0x8001ED28 LoadBank / 0x8001EE28 BankFree (bounded: no upload - see the header)
    {
        Row row{"load_bank", "int LoadBank(Bank*, src, slot, cb, arg, a5)  [SLUS, no upload: -1 / -2]", kLoadBankFn,
                [](CaseContext& c, Args& a) {
                    Rng& r = *c.rng;
                    Memory& m = *c.mem;
                    const uint32_t count = m.PeekWord(kSoundSys + 0x00), table = m.PeekWord(kSoundSys + 0x04);
                    for (uint32_t k = 0; k <= count + 3u && k < 20u; ++k)
                        if (m.PeekWord(table + 4u * k) == 0) EnPut(c, table + 4u * k, c.scratch + 0x200u + 16u * k);
                    a.a[0] = c.scratch + 0x100;
                    a.a[1] = c.scratch + 0x140;
                    a.a[2] = r.Next() % 2u ? 0xFFFFFFFFu : r.U32() % (count + 3u);
                    a.a[3] = rr::sim::kSpeechUploadCb;
                    a.a4 = r.U32();
                    a.more[0] = 1;
                },
                nullptr};
        row.nativeEx = [](NativeEnv& env, const Args& a) {
            EnRun run(env);
            Bench b(env);
            return run.Done(env, static_cast<uint32_t>(rr::sim::LoadBank(run.s, b, a.a[0], a.a[1],
                                                                          static_cast<int32_t>(a.a[2]), a.a[3], a.a4,
                                                                          a.more[0])));
        };
        row.oracleCallees = Seams(0);
        row.oracleFrame = 48;
        rows.push_back(row);
    }
    {
        Row row{"bank_free", "void BankFree(int slot)  [SLUS -> UnloadBank 0x8001EA54 -> SpuFree]", kBankFreeFn,
                [](CaseContext& c, Args& a) {
                    Rng& r = *c.rng;
                    Memory& m = *c.mem;
                    const uint32_t count = m.PeekWord(kSoundSys + 0x00);
                    a.a[0] = r.Next() % 5u == 0 ? r.U32() : static_cast<uint32_t>(static_cast<int32_t>(r.U32() % (count + 3u)) - 1);
                    if (r.Next() % 8u == 0) EnPut(c, kSoundSys + 0x04, 0);
                },
                nullptr, /*compareV0=*/false};
        row.nativeEx = [](NativeEnv& env, const Args& a) {
            EnRun run(env);
            Bench b(env);
            rr::sim::BankFree(run.s, b, static_cast<int32_t>(a.a[0]));
            return run.Done(env, 0);
        };
        row.oracleCallees = Seams(0);
        row.oracleFrame = 32;
        rows.push_back(row);
    }

    // ---- SLUS 0x8001A0C0 SpeechBankLoad(rec, id, buffer)
    {
        Row row{"speech_bank_load", "int SpeechBankLoad(rec, id, buf)  [SLUS, a taunt record into its slot]", kBankLoadFn,
                [](CaseContext& c, Args& a) {
                    Rng& r = *c.rng;
                    Memory& m = *c.mem;
                    EnPlantSpu(c);
                    if (!c.sample) sf::PlantWorld(c);
                    PlantSlots(c, true);
                    // the bound: every slot's old bank is -1 (the table then full) or past the count (a taken word)
                    const uint32_t count = m.PeekWord(kSoundSys + 0x00), table = m.PeekWord(kSoundSys + 0x04);
                    const bool full = r.Next() % 2u == 0;
                    for (uint32_t i = 0; i < 9; ++i)
                        EnPut(c, rr::sim::kSpeechSlots + 32u * i, full ? 0xFFFFFFFFu : count + 1u + r.U32() % 2u);
                    for (uint32_t k = 0; k <= count + 2u && k < 20u; ++k)
                        if (m.PeekWord(table + 4u * k) == 0 && (full || k > count))
                            EnPut(c, table + 4u * k, c.scratch + 0x200u + 16u * k);
                    // the record (+0x20 of an AUDTAUNT record): two ids, the bank header
                    const uint32_t rec = c.scratch + 0x100;
                    static const uint32_t ids[] = {3, 4, 6, 7, 22, 23, 32, 33, 11, 15, 0x10, 0x11, 0x21, 0x20};
                    const uint32_t id = ids[r.Next() % 14u];
                    EnPut(c, rec + 0, id);
                    EnPut(c, rec + 4, r.Next() % 3u == 0 ? 255u : id + 1u);
                    for (uint32_t k = 8; k < 0x60; k += 4) EnPut(c, rec + k, r.U32());
                    a.a[0] = rec;
                    a.a[1] = r.Next() % 6u == 0 ? r.U32() : (id | ((0x81u + r.U32() % 27u) << 8));
                    a.a[2] = r.U32() % 8u;
                    if (r.Next() % 3u == 0) EnPut(c, rr::sim::kSpeechSlots + 8u, 3);
                },
                nullptr};
        row.nativeEx = [](NativeEnv& env, const Args& a) {
            EnRun run(env);
            Bench b(env);
            return run.Done(env, rr::sim::SpeechBankLoad(run.s, b, a.a[0], a.a[1], a.a[2]));
        };
        row.oracleCallees = Seams(0);
        row.oracleFrame = 64;
        row.compareSpuControl = true;
        rows.push_back(row);
    }

    // ---- SLUS 0x80016464 SpeechUploaded(arg): LoadBank's callback
    {
        Row row{"speech_uploaded", "void SpeechUploaded(u32 arg)  [SLUS, slot ready + buffer release]", kUploadedFn,
                [](CaseContext& c, Args& a) {
                    Rng& r = *c.rng;
                    a.a[0] = r.Next() % 6u == 0 ? 0u : (((r.U32() % 9u) << 16) | (r.U32() % 8u) | 0x80000000u);
                },
                nullptr, /*compareV0=*/false};
        row.nativeEx = [](NativeEnv& env, const Args& a) {
            EnRun run(env);
            Bench b(env);
            rr::sim::SpeechUploaded(run.s, b, a.a[0]);
            return run.Done(env, 0);
        };
        row.oracleCallees = Seams(0);
        row.oracleFrame = 24;
        rows.push_back(row);
    }

    // ---- SLUS 0x8001A424 SpeechInit()
    {
        Row row{"speech_init", "void SpeechInit(void)  [SLUS, categories, blocks, the stream, slots 5 / 8]", kInitFn,
                [](CaseContext& c, Args&) {
                    Rng& r = *c.rng;
                    Memory& m = *c.mem;
                    EnPlantSpu(c);
                    const uint32_t gs = m.PeekWord(kGameStatePtr);
                    static const uint8_t kinds[] = {0x22, 0x23, 0x21, 0x03, 0x02, 33};
                    EnPut8(c, gs + 4u, kinds[r.Next() % 6u]);
                    if (!c.sample) {
                        PlantSlots(c, false);
                        EnPut(c, gs + 48u, r.Next() % 4u == 0 ? 2u : 1u);
                        EnPut(c, 0x8005B1F8u, 1u + r.U32() % 18u);
                        EnPut(c, m.PeekWord(kPoolTable) + 2372u, r.U32() % 16u);
                        EnPut(c, c.gp + kGpDefaultBank, r.U32() % 3u);
                    }
                    // the stream record: no file, or a file whose stream is busy (the named bound)
                    EnPut(c, rr::sim::kSpeechFile + 0, r.Next() % 4u == 0 ? 0xFFFFFFFFu : r.U32() % 16u);
                    EnPut(c, rr::sim::kSpeechFile + 0x14u, m.PeekWord(rr::sim::kSpeechFile + 0x14u) | 2u);
                },
                nullptr, /*compareV0=*/false};
        row.nativeEx = [](NativeEnv& env, const Args&) {
            EnRun run(env);
            Bench b(env);
            rr::sim::SpeechInit(run.s, b);
            return run.Done(env, 0);
        };
        row.oracleCallees = Seams(0);
        row.oracleFrame = 40;
        rows.push_back(row);
    }

    // ---- SLUS 0x8001A760 RiderSpeech(h, crash)
    {
        Row row{"rider_speech", "void RiderSpeech(u32 h, int crash)  [SLUS, the voice line + the provocation]", kSpeechFn,
                [](CaseContext& c, Args& a) {
                    int32_t crash = 0;
                    a.a[0] = PlantSpeech(c, crash);
                    a.a[1] = static_cast<uint32_t>(crash);
                },
                nullptr, /*compareV0=*/false};
        row.nativeEx = [](NativeEnv& env, const Args& a) {
            EnRun run(env);
            Bench b(env);
            rr::sim::RiderSpeech(run.s, b, a.a[0], static_cast<int32_t>(a.a[1]), env.sp);
            return run.Done(env, 0);
        };
        row.oracleCallees = Seams(40);
        row.oracleFrame = 128;
        row.compareSpuControl = true;
        rows.push_back(row);
    }
}

const CoverageRange* SpeechCoverage(const char* rowName) {
    struct Named {
        const char* name;
        CoverageRange range;
    };
    static const Named kRanges[] = {
        {"speech_slot_find", {sp::kSlotFindFn, 0x8001A424}}, {"speech_bank_load", {sp::kBankLoadFn, 0x8001A2E4}},
        {"speech_init", {sp::kInitFn, 0x8001A760}},          {"rider_speech", {sp::kSpeechFn, 0x8001B244}},
        {"load_bank", {sp::kLoadBankFn, 0x8001EE28}},        {"bank_free", {sp::kBankFreeFn, 0x8001EE94}},
        {"speech_uploaded", {sp::kUploadedFn, 0x800164B4}},
    };
    for (const Named& n : kRanges)
        if (std::strcmp(n.name, rowName) == 0) return &n.range;
    return nullptr;
}
