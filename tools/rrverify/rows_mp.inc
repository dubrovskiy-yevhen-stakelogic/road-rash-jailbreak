// Bench rows for two players on one screen. Included by verify_physics.cpp
// inside its anonymous namespace, after rows_hud.inc, whose HUD planting (hudrows::PlantHud) and seams it
// reuses.
//
// The native code: src\game\sim\split_view.{h,cpp} (the VIEWS.VI layout's three SLUS leaves) and
// src\game\sim\hud.cpp's HudFrame, here on its TWO-PLAYER arms - which rows_hud.inc names as a bound
// ("one player: a second player's bike pointer is 0 in every capture") and which the product now runs:
// the per-player loop over player 2's item records, state, timers and slides, the split-screen divider
// POLY_F4 (split mode != 1), the four BACKGRND.TIM tiles (items 105..108, split mode 2) and the radar's
// "the other player" target.
//
// INPUT FAMILIES. hud_frame's two, then player 2 added on top: game_state+0x30 = 2, a split race type,
// the split mode 0 / 1 / 2 with gp+0x7E8 at that mode's VIEWS.VI record (the boot's load is in rr-race
// at 0x800D6C68), player 2's bike (pool slot 1) in 0x8005B268+4 and 0x8005B21C, and its item records -
// player 1's copied into the case's scratch block with the loader's pre-chain relocated - and per-player
// records copied from player 1's and stirred. The OT slot of player 2, *(0x8005B594), is rr-race's own.
// Named bound: the split mode is 0..2 (the chooser's three VIEWS.VI records); the item buffer is the
// scratch block, where the original mallocs it after player 1's.
namespace mprows {

namespace s = rr::sim;

constexpr uint32_t kPlayer2Bike = 0x8005B21C;

// ---------------------------------------------------------------- SLUS 0x8001B67C / 0x8001B670 / 0x8001C498
void SetupMode(CaseContext& c, Args& a) {
    Rng& r = *c.rng;
    const uint32_t k = r.U32() % 6u;
    a.a[0] = k < 3u ? k : (k == 3u ? r.U32() % 16u : r.U32()); // the three records, then anything
    if (r.Next() % 2u) c.mem->PokeWord(s::kViewsFile, r.U32() % 3u);
    if (r.Next() % 3u == 0) c.mem->PokeWord(c.gp + s::kViewsPtrGp, r.U32());
}

uint32_t NModeSet(NativeEnv& env, const Args& a) {
    s::GuestRam g(env.clone->mem.ram().data(), env.gp);
    s::ViewModeSet(g, a.a[0]);
    return RqDone(env, g, 0);
}
uint32_t NModeWrite(NativeEnv& env, const Args& a) {
    s::GuestRam g(env.clone->mem.ram().data(), env.gp);
    s::ViewModeWrite(g, a.a[0]);
    return RqDone(env, g, 0);
}
uint32_t NMode(NativeEnv& env, const Args&) {
    s::GuestRam g(env.clone->mem.ram().data(), env.gp);
    return RqDone(env, g, s::ViewMode(g));
}

// ---------------------------------------------------------------- RASHCDG 0x8005E848, two players
void PlantSecondPlayer(CaseContext& c) {
    hudrows::PlantHud(c);
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    auto chance = [&r](uint32_t n) { return r.Next() % n == 0; };
    const uint32_t gs = m.PeekWord(s::kHudGameStatePtr);
    m.PokeWord(gs + 0x30u, 2u);
    // the split race types the commit writes (0x10 head to head, 0x11 cops and robbers, 0x18 side car),
    // now and then another type with the split bit
    static const uint8_t kTypes[] = {0x10, 0x10, 0x10, 0x18, 0x11, 0x30, 0x12};
    hudrows::Put8(m, gs + 4u, kTypes[r.U32() % sizeof(kTypes)]);
    const uint32_t mode = chance(4) ? 1u : (chance(2) ? 0u : 2u);
    m.PokeWord(s::kHudSplitMode, mode);
    m.PokeWord(c.gp + s::kViewsPtrGp, s::kViewsRecords + s::kViewsRecordBytes * mode);
    const uint32_t b2 = hudrows::Bike(m, 1);
    m.PokeWord(s::kHudPlayerBikes + 4u, b2);
    m.PokeWord(kPlayer2Bike, b2);
    // player 2's item records: player 1's, relocated into the scratch block
    const uint32_t items1 = m.PeekWord(s::kHudItemsPtr), items2 = c.scratch;
    const uint32_t bytes = s::kHudItemCount * s::kHudItemBytes;
    for (uint32_t o = 0; o < bytes; o += 4u) {
        uint32_t w = m.PeekWord(items1 + o);
        if (o % s::kHudItemBytes == 0) { // the tag: a link into player 1's buffer moves with it
            const uint32_t link = w & 0x00FFFFFFu, lo = items1 & 0x00FFFFFFu;
            if (link >= lo && link < lo + bytes) w = (w & 0xFF000000u) | ((link - lo + items2) & 0x00FFFFFFu);
        }
        m.PokeWord(items2 + o, w);
    }
    m.PokeWord(s::kHudItemsPtr + 4u, items2);
    // player 2's state, timers and slides: player 1's, then stirred
    auto copy = [&m](uint32_t base, uint32_t n) {
        for (uint32_t o = 0; o < n; o += 4u) m.PokeWord(base + n + o, m.PeekWord(base + o));
    };
    copy(s::kHudState88, 88u);
    copy(s::kHudDash224, 224u);
    copy(s::kHudSlide72, 72u);
    const int32_t clock = static_cast<int32_t>(m.PeekWord(gs + 16u));
    for (uint32_t t = 0; t < 7; ++t)
        if (chance(3)) hudrows::RandomTimer(m, s::kHudDash224 + 224u + 32u * t, r, clock);
    for (uint32_t k = 0; k < 2; ++k)
        if (chance(2)) {
            const uint32_t sl = s::kHudSlide72 + 72u + 36u * k;
            const uint32_t st = r.U32() % 4u;
            m.PokeWord(sl, st);
            const uint32_t limit = (st == 0) ? m.PeekWord(sl + 8u) : m.PeekWord(sl + 20u);
            m.PokeWord(sl + 4u, limit == 0 ? 0u : r.U32() % limit);
        }
    if (chance(2)) m.PokeWord(s::kHudState88 + 88u + 64u, chance(3) ? 0u : hudrows::Bike(m, r.U32() % hudrows::kLive));
    m.PokeWord(s::kHudFinished + 4u, chance(6) ? 1u : 0u);
    m.PokeWord(s::kHudPanelDirty + 4u, chance(3) ? 1u : 0u);
    m.PokeWord(s::kHudTarget + 4u, chance(4) ? 0u : hudrows::Bike(m, r.U32() % hudrows::kLive));
    m.PokeWord(s::kHudOdometer + 4u, chance(8) ? r.U32() : r.U32() % 0x1000000u);
    // player 2's bike and rider: the countdown bits, its speed, its progress against player 1's
    const uint32_t rd2 = m.PeekWord(b2 + 1084u);
    hudrows::Put8(m, rd2, (m.PeekByte(rd2) & ~0xE0u) | ((r.U32() % 8u) << 5));
    m.PokeWord(b2 + 480u, static_cast<uint32_t>(static_cast<int32_t>(r.U32() % 0x6000000u) - 0x1000000));
    if (chance(2)) m.PokeWord(b2 + 324u, m.PeekWord(hudrows::Bike(m, 0) + 324u) + (r.U32() % 0x40000u) - 0x20000u);
    m.PokeWord(m.PeekWord(b2 + 852u) + 604u, r.U32() % 6u);
}

} // namespace mprows

void AddMpRows(std::vector<Row>& rows) {
    using namespace mprows;
    rows.push_back(Row{"mp_view_mode_set", "void ViewModeSet(s32 mode)  [SLUS 0x8001B67C]", s::kViewModeSetFn, SetupMode,
                       nullptr, false, NModeSet, {}});
    rows.push_back(Row{"mp_view_mode_write", "void ViewModeWrite(s32 mode)  [SLUS 0x8001B670]", s::kViewModeWriteFn,
                       SetupMode, nullptr, false, NModeWrite, {}});
    rows.push_back(Row{"mp_view_mode", "s32 ViewMode(void)  [SLUS 0x8001C498]", s::kViewModeFn, SetupMode, nullptr, true,
                       NMode, {}});
    {   // HudFrame with two players: hud_frame's seams, at its depths. The cop-mode elements (kCopMsgs
        // 0x80062F34, kCopClock 0x80062C40, kSuspect 0x8005FAC4, kArrests 0x80062610, kCopPanel 0x80063530,
        // kCopSign 0x800636F0) are PORTED (modes.h, rows mode_hud_* in rows_modes.inc) and run natively
        // inside HudFrame, exactly as in hud_frame: declaring them as oracle callees here would make the
        // guest's calls to them count against a port that does not delegate them.
        using namespace hudrows;
        std::vector<OracleCallee> seams = Seams(40, 56, 48 + 32 + 88);
        rows.push_back(Bios(HudRow(
            "mp_hud_frame", "void HudFrame(void)  [RASHCDG 0x8005E848, TWO PLAYERS: split layouts, both HUDs]",
            kHudFrameFn, [](CaseContext& c, Args&) { PlantSecondPlayer(c); },
            [](NativeEnv& env, const Args&) {
                return Run(env, [&](s::GuestRam& g, s::HudCallees& k) {
                    if (!s::HudFrame(g, k, env.sp + 88u) && env.failure.empty()) env.failure = "HudFrame refused";
                    return 0u;
                });
            },
            false, seams, 88)));
    }
}
