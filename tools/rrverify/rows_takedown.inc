// Bench rows for src\game\sim\takedown.h: the knock-off's takedown credit, the rider-off sound mode and the
// busted music. Included by verify_physics.cpp inside its anonymous
// namespace after rows_speech.inc / rows_fight.inc / rows_engine_note.inc, whose helpers it uses.
//
// Every port is a GUEST-ADDRESS port on the clone's RAM. Callees:
//   * Takedown's FightStat 0x800BFE58 and RiderOffSound's StopVoice 0x8001F7EC are PORTED and run natively
//     (the guest runs its own); the SPU store list and register file are compared for RiderOffSound.
//   * BustedMusic's CD streamer 0x80023148 is executed by the ORACLE and witnessed. Named bound, as in
//     rows_speech.inc: the stream record's +0x14 bit 1 (busy) is always set, so the streamer returns -1 at
//     once - its critical section (SYSCALL 1) and the CD are never reached. The stream itself is the
//     product's (sound_runtime.cpp).
namespace td {

constexpr uint32_t kStreamFn = 0x80023148;

// The last-blow table: four entries, each naming the victim `B` or another bike, with a blow whose age
// straddles the 240-tick window; attackers are the live bikes Stir chose (players and AI).
void SetupTakedown(CaseContext& c, Args& a) {
    const fg::World w = fg::Stir(c);
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    const uint32_t gs = m.PeekWord(kGameStatePtr);
    const uint32_t B = (r.Next() % 3u == 0) ? w.me : w.t;
    if (r.Next() % 4u == 0) m.PokeWord(gs + 16u, r.U32());
    const uint32_t now = m.PeekWord(gs + 16u);
    const uint32_t bikes[3] = {w.me, w.t, w.x};
    for (uint32_t i = 0; i < 4; ++i) {
        const uint32_t e = rr::sim::fight::kLastBlow + 12u * i;
        const uint64_t roll = r.Next() % 8u;
        if (c.sample != nullptr && roll == 0) continue; // the snapshot's own entry
        uint32_t att = bikes[r.Next() % 3u];
        if (att == 0) att = w.me;
        m.PokeWord(e + 0u, att);
        m.PokeWord(e + 4u, (roll < 5) ? B : (roll == 5 ? bikes[r.Next() % 3u] : r.U32()));
        uint32_t t;
        switch (r.Next() % 5u) {
        case 0: t = now - 240u + (r.U32() % 3u); break;                  // the edge: 240 / 239 / 238 old
        case 1: t = now - (r.U32() % 240u); break;                       // fresh
        case 2: t = now - 240u - (r.U32() % 100000u); break;             // stale
        case 3: t = now + (r.U32() % 1000u); break;                      // in the future (negative age)
        default: t = r.U32(); break;
        }
        m.PokeWord(e + 8u, t);
    }
    if (r.Next() % 3u == 0) WriteU16Bench(m, w.me + 0xACu, static_cast<uint16_t>(r.U32() % 4u));
    a.a[0] = B;
}

// The engine record array *(gp+1952) (the snapshot's, or two planted records), sometimes none; the two
// voice handles +0x24 / +0x28 live, stale, 0 or -1 (EnHandle).
void SetupRiderOff(CaseContext& c, Args& a) {
    Rng& r = *c.rng;
    Memory& m = *c.mem;
    if (!c.sample) EnPlantSystem(c);
    EnPlantSpu(c);
    uint32_t arr = m.PeekWord(c.gp + rr::sim::kEngineRecGp);
    uint32_t n = 2;
    if (c.sample == nullptr || arr == 0) {
        arr = c.scratch + en::kEng;
        EnPut(c, c.gp + rr::sim::kEngineRecGp, arr);
        for (uint32_t i = 0; i < 2u * 132u; i += 4) EnPut(c, arr + i, r.U32());
    } else {
        n = m.PeekWord(m.PeekWord(kGameStatePtr) + 0x30u);
        if (n == 0 || n > 2) n = 1;
    }
    const uint32_t h = r.U32() % n;
    const uint32_t E = arr + 132u * h;
    EnPut(c, E + 0x24u, EnHandle(c));
    EnPut(c, E + 0x28u, EnHandle(c));
    if (r.Next() % 8u == 0) EnPut(c, c.gp + rr::sim::kEngineRecGp, 0);
    a.a[0] = h;
    a.a[1] = (r.Next() % 2u) ? 0u : ((r.Next() % 2u) ? 1u : r.U32());
}

// The two gates (players < 2, gs+4 bit 0) both ways; the stream record busy (the named bound above).
void SetupBusted(CaseContext& c, Args& a) {
    Rng& r = *c.rng;
    Memory& m = *c.mem;
    const uint32_t gs = m.PeekWord(kGameStatePtr);
    if (r.Next() % 2u) EnPut(c, gs + 0x30u, (r.Next() % 3u == 0) ? r.U32() % 4u : 1u);
    if (r.Next() % 2u) EnPut8(c, gs + 4u, static_cast<uint8_t>((r.Next() % 3u == 0) ? r.U32() : (r.U32() & 0xFEu)));
    EnPut(c, rr::sim::kSpeechFile + 0x14u, m.PeekWord(rr::sim::kSpeechFile + 0x14u) | 2u);
    if (r.Next() % 2u) EnPut(c, rr::sim::kSpeechFile + 4u, r.U32());
    if (r.Next() % 2u) EnPut(c, rr::sim::kSpeechCategories, r.U32());
    if (r.Next() % 2u) EnPut(c, rr::sim::kSpeechSlots + 8u, r.U32() % 5u);
    if (r.Next() % 2u) EnPut(c, c.gp + 1944u, r.U32() % 2u);
    a.a[0] = r.U32(); // the callers' leftover a0..a1 (0x8001B3C8 takes none)
    a.a[1] = r.U32();
}

} // namespace td

void AddTakedownRows(std::vector<Row>& rows) {
    rows.push_back(Row{"takedown", "void Takedown(Bike *B)  [RASHCDG 0x800BF51C]", rr::sim::kTakedownFn,
                       td::SetupTakedown,
                       [](Memory& m, uint32_t, uint32_t gp, const Args& a) {
                           rr::sim::GuestRam g(m.ram().data(), gp);
                           rr::sim::Takedown(g, a.a[0]);
                           return 0u;
                       }});
    {
        Row row{"rider_off_sound", "void RiderOffSound(h, mode)  [SLUS 0x80018440, StopVoice x2 or mode 7]",
                rr::sim::kRiderOffSoundFn, td::SetupRiderOff, nullptr};
        row.nativeEx = [](NativeEnv& env, const Args& a) {
            EnRun run(env);
            return run.Done(env, rr::sim::RiderOffSound(run.s, a.a[0], a.a[1]));
        };
        row.compareSpuControl = true;
        rows.push_back(row);
    }
    {
        Row row{"busted_music", "void BustedMusic()  [SLUS 0x8001B3C8, -> 0x80023148 busy]", rr::sim::kBustedMusicFn,
                td::SetupBusted, nullptr};
        row.nativeEx = [](NativeEnv& env, const Args&) {
            EnRun run(env);
            sp::Bench b(env);
            bool ok = true;
            const uint32_t v = rr::sim::BustedMusic(run.s, b, ok);
            return run.Done(env, v);
        };
        row.oracleCallees = {OracleCallee{td::kStreamFn, 4}};
        row.oracleFrame = 24;
        rows.push_back(row);
    }
}
