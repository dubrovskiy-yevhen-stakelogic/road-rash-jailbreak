// Bench rows of op 9's strike RASHCDG 0x800C1370 (src\game\sim\strike.h) and the audio
// pause SLUS 0x80018C1C (src\game\sim\race_over.h). Included at the end of verify_physics.cpp inside its
// anonymous namespace, after rows_fight.inc (its world, `Stir`, the `Bench` callees and `Run`) and
// rows_engine_note.inc (the sound machine `EnRun`, `EnPlantSpu`, `sf::PlantWorld`, `EnHandle`).
namespace sk {

// Strike runs on the fight rows' world (two live bikes beside each other, the safe-animation bound of
// rows_fight.inc), with the attacker's node a striking one half the time so that ApplyHit is reached.
void SetupStrike(CaseContext& c, Args& a) {
    const fg::World w = fg::Stir(c, false, true);
    Memory& m = *c.mem;
    if (c.rng->Next() % 2u) {
        fg::StrikingNode(c, w.me);
        // the swing's strike frame (FightStep's v0 = 1): the clip still playing (+0x24 bit 1), the node's
        // strike not yet fired (+0x222 = 0), the clip's frame at or past the node's hit frame (+11)
        if (c.rng->Next() % 2u) {
            const uint32_t R = m.PeekWord(w.me + 852u), an = m.PeekWord(R + 0x21Cu);
            const uint32_t nd = m.PeekWord(fg::Fight(m) + 12u * fg::B8(m, R + 0x239u) + 8u) +
                                12u * fg::B8(m, R + 0x23Eu + fg::B8(m, R + 0x23Au));
            fg::W8(m, R + 0x222u, 0);
            m.PokeWord(an + 0x24u, m.PeekWord(an + 0x24u) | 2u);
            m.PokeWord(an + 0x10u, fg::B8(m, nd + 11u) + c.rng->U32() % 3u);
            // the target at the attacker's height, within its reach height +0x138
            m.PokeWord(w.t + 0xBCu, m.PeekWord(w.me + 0xBCu) + (c.rng->U32() % 0x2000u) - 0x1000u);
            if (static_cast<int32_t>(m.PeekWord(w.me + 0x138u)) < 0x2000) m.PokeWord(w.me + 0x138u, 0x8000u);
        }
    }
    a.a[0] = w.me;
    a.a[1] = w.t;
}

// RaceOverSignal: the sound world of the sound-frame rows, then the sliders and their saved copy, the
// pause flag gp+1960, the two voice handles (gp+1964, the cue voice 0x800D6BC0: live, stale, 0 or -1),
// the two-up word 0x800CDAC0 and game_state +4 / +0x30 (one or two players), and the argument.
void SetupRaceOver(CaseContext& c, Args& a) {
    if (!c.sample) sf::PlantWorld(c);
    EnPlantSpu(c);
    Rng& r = *c.rng;
    for (uint32_t k = 0; k < 16; ++k)
        EnPut(c, 0x800D6C00u + 4u * k, (r.Next() % 4u == 0) ? r.U32() : r.U32() % 128u);
    EnPut(c, c.gp + 1960u, (r.Next() % 3u == 0) ? 1u : 0u);
    EnPut(c, c.gp + 1964u, (r.Next() % 3u == 0) ? 0u : EnHandle(c));
    EnPut(c, rr::sim::kCueHandle, (r.Next() % 3u == 0) ? 0u : EnHandle(c));
    EnPut(c, 0x800CDAC0u, (r.Next() % 2u) ? 2u : r.U32() % 4u);
    const uint32_t gs = c.mem->PeekWord(kGameStatePtr);
    EnPut8(c, gs + 4u, static_cast<uint8_t>(r.U32()));
    EnPut(c, gs + 48u, 1u + r.U32() % 2u);
    a.a[0] = (r.Next() % 2u) ? 0u : ((r.Next() % 4u == 0) ? r.U32() : 1u);
}

} // namespace sk

void AddStrikeRows(std::vector<Row>& rows) {
    {
        Row row{"strike", "void Strike(me, t)  [RASHCDG 0x800C1370, op 9's strike]", 0x800C1370, sk::SetupStrike, nullptr};
        row.compareV0 = false;
        row.nativeEx = [](NativeEnv& env, const Args& a) {
            return fg::Run(env, "Strike", [&](rr::sim::GuestRam& g, fg::Bench& b, uint32_t&) {
                return rr::sim::fight::Strike(g, b, a.a[0], a.a[1]);
            });
        };
        row.oracleFrame = 56; // `addiu sp,sp,-56` at 0x800C1370
        row.oracleCallees = fg::AllSeams(0);
        rows.push_back(row);
        fg::SeamDepth(rows, fg::kArrest, 72 + 32); // ApplyHit's and KnockOff's frames
    }
    {
        Row row{"race_over_signal", "void RaceOverSignal(u32 pause)  [SLUS 0x80018C1C, the audio pause / resume]",
                rr::sim::kRaceOverSignalFn, sk::SetupRaceOver, nullptr, /*compareV0=*/false};
        row.nativeEx = [](NativeEnv& env, const Args& a) {
            EnRun run(env);
            rr::sim::RaceOverSignal(run.s, a.a[0]);
            return run.Done(env, 0);
        };
        row.compareSpuControl = true;
        rows.push_back(row);
    }
}
