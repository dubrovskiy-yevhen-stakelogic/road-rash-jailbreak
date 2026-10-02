// Bench rows of the pause: the pad poll's pause test, GameFrame's stall switch and the pause menu
// (src\game\sim\pause.h). Included at the end of verify_physics.cpp inside
// its anonymous namespace, after rows_engine_note.inc (the sound machine `EnRun`, `EnPlantSpu`,
// `sf::PlantWorld`, `EnHandle`) and rows_hud.inc.
//
// THE SEAMS: PlaySound3D 0x80017BA0, RaceOverSignal 0x80018C1C, SoundHold 0x80020E30 (ported, row
// `pause_sound_hold`; the menu rows take the original), ResultsPrepare 0x8003F708, the heap manager
// 0x80021C98 - executed by the oracle on the clone and witnessed.
//
// TWO REGION ROWS. The pause test [0x8001CBEC, 0x8001CD00) of the pad poll and the stall switch
// [0x80011C70, 0x80011D88) of GameFrame are not functions: each case's RAM gets the region's exit words
// replaced by `jr ra` (the stall switch: `lw ra, 56(sp)` first, its slot holding the bench's sentinel - on
// both machines, so the comparison sees the same words), and the guest run
// starts at the region's first instruction with the snapshot's registers. The pause test's delay slot
// is `move v0, s0`, so the row's v0 comparison is the region's output flag s0.
// Named bounds: the pause test is not entered with state 2 (0x8001CC00 leaves the poll first); the
// pad index gp+260 and the pad count game_state+0x34 name one of the four frame records 0x800D7128;
// TextInBox mode 3 runs with the overlay mask's RASHCDF bit clear (the race's own value, 0x5); the
// SoundHold block holds at most three voices (two in every capture).

namespace pauserows {

namespace s = rr::sim;

bool Chance(Rng& r, uint32_t n) { return r.Next() % n == 0; }
uint32_t Pick(Rng& r, std::initializer_list<uint32_t> v) { return *(v.begin() + r.Next() % v.size()); }
void Put8(Memory& m, uint32_t a, uint32_t v) {
    const uint8_t b = static_cast<uint8_t>(v);
    m.WriteBlock(a, &b, 1);
}
void Put16(Memory& m, uint32_t a, uint32_t v) { WriteU16Bench(m, a, static_cast<uint16_t>(v)); }

constexpr uint32_t kRemap = 0x800526A4; // SLUS: 5 control -> slot configurations of 60 bytes (the 6th is other data)

// The pad records: remaps, and press codes on the slots the pause reads (0 Left, 1 Right, 2 Up, 3 Down,
// 6 Triangle, 7 Cross, 12 Start) and now and then any slot.
void PlantPads(CaseContext& c, bool derived) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    for (uint32_t p = 0; p < 4; ++p) {
        const uint32_t rec = s::kPausePadRecords + 192u * p;
        if (!derived) m.PokeWord(rec + 0xB8u, kRemap + 60u * (Chance(r, 3) ? r.U32() % 5u : 0u));
        for (uint32_t k = 0; k < 19; ++k) Put8(m, rec + 26u + 8u * k, 0);
        static const uint32_t kSlots[] = {0, 1, 2, 3, 6, 7, 12, 12};
        const uint32_t presses = derived ? (Chance(r, 2) ? 1u : 0u) : r.U32() % 4u;
        for (uint32_t k = 0; k < presses; ++k) {
            const uint32_t slot = Chance(r, 8) ? r.U32() % 19u : kSlots[r.U32() % 8u];
            Put8(m, rec + 26u + 8u * slot, Pick(r, {1u, 1u, 2u, 0xFFu, r.U32() & 0xFFu}));
        }
    }
}

// The menu's state: the modal bytes, the three menus, the pausing pad, the state byte and the stall
// clocks, the race type, the two riders' places the quit arm reads.
void PlantMenu(CaseContext& c) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    const bool derived = c.sample != nullptr;
    const uint32_t gs = m.PeekWord(kGameStatePtr);
    PlantPads(c, derived);
    Put8(m, s::kPauseModalUp, derived ? Pick(r, {0u, 1u, 1u}) : Pick(r, {0u, 1u, 1u, 1u, r.U32() & 0xFFu}));
    Put8(m, s::kPauseModalWhich, derived ? Pick(r, {0u, 0u, 1u, 2u}) : Pick(r, {0u, 0u, 1u, 2u, r.U32() & 0xFFu}));
    for (uint32_t k = 0; k < 3; ++k) {
        const uint32_t menu = s::kPauseMainMenu + 4u * k;
        if (!derived && Chance(r, 6)) Put16(m, menu, r.U32() % 5u);
        Put16(m, menu + 2u, derived ? r.U32() % (k == 0 ? 3u : 2u) : Pick(r, {0u, 1u, 2u, 3u, 0xFFFFu, r.U32()}));
    }
    m.PokeWord(s::kPausePadIndex, derived ? 0u : r.U32() % 4u);
    m.PokeWord(s::kPausePadLost, Chance(r, 6) ? 1u : 0u);
    m.PokeWord(s::kPauseMenuPtr, Pick(r, {0u, s::kPauseMainMenu, s::kPauseQuitMenu, s::kPauseRestartMenu}));
    Put8(m, gs, derived ? Pick(r, {3u, 3u, 4u}) : Pick(r, {3u, 3u, 3u, 4u, 1u, 6u, 5u}));
    Put8(m, gs + 1u, Pick(r, {1u, 3u, 6u, 0u}));
    Put8(m, gs + 4u, derived ? m.PeekByte(gs + 4u)
                             : Pick(r, {0x22u, 0x00u, 0x02u, 0x20u, 0x08u, 0x10u, 0x11u, 0x04u, 0x24u, 0x01u, r.U32() & 0xFFu}));
    const uint32_t clock = m.PeekWord(gs + 0x0Cu);
    m.PokeWord(gs + 0x2Cu, clock - Pick(r, {0u, 3000u, 3001u, 3002u, r.U32() % 6000u, r.U32()}));
    Put16(m, gs + 0x2Au, Chance(r, 3) ? Pick(r, {1u, r.U32() & 0xFFFFu}) : 0u);
    const uint32_t pool = m.PeekWord(s::kPausePool0);
    for (uint32_t k = 0; k < 2; ++k) {
        const uint32_t rd = m.PeekWord(pool + 1096u * k + 1084u);
        Put8(m, rd + 39u, Pick(r, {0u, 1u + r.U32() % 18u, 0xFFu, 248u + r.U32() % 8u, r.U32() & 0xFFu}));
        m.PokeWord(rd + 40u, Chance(r, 2) ? 0u : r.U32() % 100000u);
    }
    m.PokeWord(s::kPauseFieldSize, Pick(r, {m.PeekWord(s::kPauseFieldSize), 1u + r.U32() % 18u, r.U32()}));
}

// The oracle side of every callee the ports reach.
struct PauseOracle final : s::PauseCallees {
    NativeEnv& env;
    explicit PauseOracle(NativeEnv& e) : env(e) {}
    bool PlaySound(uint32_t, int32_t id) override {
        env.CallOracle(s::kPausePlaySoundFn, 0, 0, static_cast<uint32_t>(id), 0);
        return env.failure.empty();
    }
    bool StopCountdownVoice(uint32_t) override {
        env.CallOracle(0x80016528u, 0, 0, 0, 0);
        return env.failure.empty();
    }
    bool ComputePlace(uint32_t, uint32_t bike, int32_t mode, int32_t* place) override {
        *place = static_cast<int32_t>(env.CallOracle(0x800138E8u, bike, static_cast<uint32_t>(mode), 0, 0));
        return env.failure.empty();
    }
    bool HeapOverflow(uint32_t, uint32_t next, uint32_t bytes, uint32_t* answer) override {
        *answer = env.CallOracle(s::kPauseHeapFn, next, bytes, 0, 0);
        return env.failure.empty();
    }
    bool Unported(uint32_t, uint32_t address, const uint32_t* args, int count) override {
        env.CallOracleArgs(address, args, count);
        return env.failure.empty();
    }
    bool SoundHold(uint32_t, uint32_t v) override {
        env.CallOracle(s::kSoundHoldFn, v, 0, 0, 0);
        return env.failure.empty();
    }
    bool RaceOverSignal(uint32_t, uint32_t v) override {
        env.CallOracle(s::kPauseRaceOverFn, v, 0, 0, 0);
        return env.failure.empty();
    }
    bool ResultsPrepare(uint32_t) override {
        env.CallOracle(s::kPauseResultsFn, 0, 0, 0, 0);
        return env.failure.empty();
    }
};

std::vector<OracleCallee> Seams() {
    return {OracleCallee{s::kPausePlaySoundFn, 4}, OracleCallee{s::kPauseRaceOverFn, 1},
            OracleCallee{s::kSoundHoldFn, 1},      OracleCallee{s::kPauseResultsFn, 0},
            OracleCallee{s::kPauseHeapFn, 2}};
}

// Runs `f(g, callees)` on the clone's RAM; a refusal or a fault fails the case.
template <class F>
uint32_t Run(NativeEnv& env, const char* what, F&& f) {
    s::GuestRam g(env.clone->mem.ram().data(), env.gp);
    g.SetScratchpad(env.clone->mem.scratchpad().data());
    PauseOracle callees(env);
    uint32_t v0 = 0;
    const bool ok = f(g, callees, v0);
    if ((!ok || g.Faulted()) && env.failure.empty()) {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "%s refused (fault 0x%08X)", what, g.FaultAddress());
        env.failure = buf;
    }
    return v0;
}

Row PauseRow(const char* name, const char* sig, uint32_t entry, std::function<void(CaseContext&, Args&)> setup,
             std::function<uint32_t(NativeEnv&, const Args&)> native, bool v0) {
    Row r{name, sig, entry, std::move(setup), nullptr, v0, std::move(native), Seams()};
    r.maxSteps = 20'000'000;
    r.allowBiosCalls = true; // the text path's strlen A(1Bh) through SLUS 0x800448F4 (rows_hud.inc)
    return r;
}

// The region rows' exit: `jr ra` and a delay slot.
void PatchExit(Memory& m, uint32_t exit, uint32_t delay) {
    m.PokeWord(exit, 0x03E00008u);
    m.PokeWord(exit + 4u, delay);
}

} // namespace pauserows

void AddPauseRows(std::vector<Row>& rows) {
    using namespace pauserows;

    // ------------------------------------------------------------------ SLUS 0x8001CB3C region
    rows.push_back(PauseRow(
        "pause_test", "region [0x8001CBEC, 0x8001CD00) of 0x8001CB3C()  [SLUS, the pad poll's pause test; v0 = its s0]",
        s::kPauseTestEntry,
        [](CaseContext& c, Args&) {
            Memory& m = *c.mem;
            Rng& r = *c.rng;
            const bool derived = c.sample != nullptr;
            PatchExit(m, s::kPauseTestExit, 0x02001021u); // move v0, s0
            const uint32_t gs = m.PeekWord(kGameStatePtr);
            PlantPads(c, derived);
            Put8(m, gs, derived ? Pick(r, {1u, 1u, 6u}) : Pick(r, {1u, 1u, 1u, 3u, 6u, 0u, 4u, 5u, 7u, 0x81u}));
            Put8(m, gs + 3u, Chance(r, 4) ? Pick(r, {1u, 0x80u, r.U32() & 0xFFu}) : 0u);
            if (!derived) {
                Put8(m, gs + 1u, r.U32());
                Put8(m, gs + 2u, r.U32());
            }
            m.PokeWord(gs + 0x34u, derived ? m.PeekWord(gs + 0x34u) : Pick(r, {1u, 1u, 2u, 0u, 3u, 4u}));
            m.PokeWord(s::kPausePadLost, Chance(r, 6) ? 1u : 0u);
            m.PokeWord(s::kPausePadIndex, r.U32());
            m.PokeWord(s::kPauseResultsKey, Chance(r, 2) ? 0u : r.U32());
        },
        [](NativeEnv& env, const Args&) {
            return Run(env, "PadPauseTest", [&](s::GuestRam& g, s::PauseCallees&, uint32_t& v0) {
                v0 = s::PadPauseTest(g);
                return true;
            });
        },
        true));

    // ------------------------------------------------------------------ SLUS 0x80011C4C region
    rows.push_back(PauseRow(
        "pause_stall", "region [0x80011C70, 0x80011D88) of GameFrame 0x80011C4C()  [SLUS, the stream stall's state switch]",
        s::kStallEntry,
        [](CaseContext& c, Args&) {
            Memory& m = *c.mem;
            Rng& r = *c.rng;
            // the region calls RaceOverSignal / SoundHold, so `ra` is the region's own by the exit: the exit
            // reloads it from GameFrame's slot (56(sp), where its prologue keeps it), which holds the sentinel
            m.PokeWord(s::kStallExit, 0x8FBF0038u);      // lw ra, 56(sp)
            m.PokeWord(s::kStallExit + 4u, 0);
            PatchExit(m, s::kStallExit + 8u, 0);
            m.PokeWord(c.sp + 56u, kSentinel);
            const uint32_t gs = m.PeekWord(kGameStatePtr);
            Put16(m, gs + 0x28u, Pick(r, {0u, 0u, 1u, 2u, 3u, r.U32() & 0xFFFFu}));
            Put8(m, gs, Pick(r, {1u, 1u, 4u, 4u, 3u, 6u, r.U32() & 0xFFu}));
            Put8(m, gs + 1u, Pick(r, {1u, 1u, 3u, 6u, 0u, r.U32() & 0xFFu}));
            Put16(m, gs + 0x2Au, r.U32());
            m.PokeWord(gs + 0x2Cu, r.U32());
            m.PokeWord(gs + 0x0Cu, c.sample != nullptr ? m.PeekWord(gs + 0x0Cu) : r.U32());
            m.PokeWord(s::kPauseDemo, Chance(r, 6) ? Pick(r, {1u, r.U32()}) : 0u);
        },
        [](NativeEnv& env, const Args&) {
            return Run(env, "GameFrameStall", [&](s::GuestRam& g, s::PauseCallees& k, uint32_t&) {
                return s::GameFrameStall(g, k, env.sp);
            });
        },
        false));

    // ------------------------------------------------------------------ SLUS 0x8002D2F4
    rows.push_back(PauseRow(
        "pause_menu", "void PauseMenu(void)  [SLUS 0x8002D2F4, the pause menu while game_state is 3 or 4]", s::kPauseMenuFn,
        [](CaseContext& c, Args&) { PlantMenu(c); },
        [](NativeEnv& env, const Args&) {
            return Run(env, "PauseMenu", [&](s::GuestRam& g, s::PauseCallees& k, uint32_t&) { return s::PauseMenu(g, k, env.sp); });
        },
        false));

    // ------------------------------------------------------------------ SLUS 0x8002DC1C
    rows.push_back(PauseRow(
        "pause_input", "s32 PauseInput(void)  [SLUS 0x8002DC1C, RESUME / QUIT / RESTART]", s::kPauseInputFn,
        [](CaseContext& c, Args&) {
            PlantMenu(c);
            c.mem->PokeWord(s::kPauseMenuPtr, s::kPauseMainMenu);
        },
        [](NativeEnv& env, const Args&) {
            return Run(env, "PauseInput", [&](s::GuestRam& g, s::PauseCallees& k, uint32_t& v0) {
                return s::PauseInput(g, k, env.sp, &v0);
            });
        },
        true));

    // ------------------------------------------------------------------ SLUS 0x8002DD80
    rows.push_back(PauseRow(
        "pause_confirm", "s32 ConfirmInput(void)  [SLUS 0x8002DD80, YES / NO]", s::kConfirmInputFn,
        [](CaseContext& c, Args&) {
            PlantMenu(c);
            Rng& r = *c.rng;
            c.mem->PokeWord(s::kPauseMenuPtr, Pick(r, {s::kPauseQuitMenu, s::kPauseRestartMenu}));
        },
        [](NativeEnv& env, const Args&) {
            return Run(env, "ConfirmInput", [&](s::GuestRam& g, s::PauseCallees& k, uint32_t& v0) {
                return s::ConfirmInput(g, k, env.sp, &v0);
            });
        },
        true));

    // ------------------------------------------------------------------ SLUS 0x8001DA7C / 0x8001DB90
    for (int lr = 0; lr < 2; ++lr)
        rows.push_back(PauseRow(
            lr ? "pause_nav_lr" : "pause_nav_ud",
            lr ? "s32 MenuLeftRight(void)  [SLUS 0x8001DB90]" : "s32 MenuUpDown(void)  [SLUS 0x8001DA7C]",
            lr ? s::kMenuLeftRightFn : s::kMenuUpDownFn,
            [](CaseContext& c, Args&) {
                PlantMenu(c);
                Rng& r = *c.rng;
                c.mem->PokeWord(s::kPauseMenuPtr, Pick(r, {s::kPauseMainMenu, s::kPauseQuitMenu, s::kPauseRestartMenu}));
            },
            lr ? std::function<uint32_t(NativeEnv&, const Args&)>([](NativeEnv& env, const Args&) {
                return Run(env, "MenuLeftRight", [&](s::GuestRam& g, s::PauseCallees&, uint32_t& v0) {
                    v0 = s::MenuLeftRight(g);
                    return true;
                });
            })
               : std::function<uint32_t(NativeEnv&, const Args&)>([](NativeEnv& env, const Args&) {
                     return Run(env, "MenuUpDown", [&](s::GuestRam& g, s::PauseCallees&, uint32_t& v0) {
                         v0 = s::MenuUpDown(g);
                         return true;
                     });
                 }),
            true));

    // ------------------------------------------------------------------ SLUS 0x8002D718
    rows.push_back(PauseRow(
        "pause_draw", "void PauseDraw(void)  [SLUS 0x8002D718, the title and the three items]", s::kPauseDrawFn,
        [](CaseContext& c, Args&) { PlantMenu(c); },
        [](NativeEnv& env, const Args&) {
            return Run(env, "PauseDraw", [&](s::GuestRam& g, s::PauseCallees& k, uint32_t&) { return s::PauseDraw(g, k, env.sp); });
        },
        false));

    // ------------------------------------------------------------------ SLUS 0x8002D9E8
    rows.push_back(PauseRow(
        "pause_confirm_draw", "void ConfirmDraw(s32 id)  [SLUS 0x8002D9E8, the question and YES / NO]", s::kConfirmDrawFn,
        [](CaseContext& c, Args& a) {
            PlantMenu(c);
            Rng& r = *c.rng;
            c.mem->PokeWord(s::kPauseMenuPtr, Pick(r, {s::kPauseQuitMenu, s::kPauseRestartMenu, s::kPauseMainMenu}));
            a.a[0] = c.sample != nullptr ? Pick(r, {5u, 7u}) : Pick(r, {5u, 7u, r.U32() % 181u});
        },
        [](NativeEnv& env, const Args& a) {
            return Run(env, "ConfirmDraw", [&](s::GuestRam& g, s::PauseCallees& k, uint32_t&) {
                return s::ConfirmDraw(g, k, env.sp, static_cast<int32_t>(a.a[0]));
            });
        },
        false));

    // ------------------------------------------------------------------ SLUS 0x8002CB08
    rows.push_back(PauseRow(
        "pause_text_in_box", "void TextInBox(s32 font, s32 id, RECT *r, u32 *ot, u32 colour, s32 mode)  [SLUS 0x8002CB08]",
        s::kTextInBoxFn,
        [](CaseContext& c, Args& a) {
            Memory& m = *c.mem;
            Rng& r = *c.rng;
            const bool derived = c.sample != nullptr;
            a.a[0] = Chance(r, 8) ? 0xFFFFFFFFu : m.PeekWord(s::kHudFontIndex);
            a.a[1] = derived ? Pick(r, {0u, 2u, 3u, 4u}) : r.U32() % 181u;
            const uint32_t rect = c.scratch + 64u;
            Put16(m, rect, derived ? 86u : Pick(r, {86u, r.U32() % 400u, r.U32()}));
            Put16(m, rect + 2u, derived ? 72u : Pick(r, {72u, r.U32() % 260u, r.U32()}));
            Put16(m, rect + 4u, derived ? 212u : Pick(r, {212u, r.U32() % 400u, r.U32()}));
            Put16(m, rect + 6u, r.U32());
            a.a[2] = rect;
            a.a[3] = m.PeekWord(s::kPauseFrameOtPtr) + 4u * (r.U32() % 12u);
            a.a4 = Pick(r, {0x14506Eu, 0x348A32u, r.U32() & 0xFFFFFFu});
            a.more[0] = derived ? 2u : Pick(r, {0u, 1u, 2u, 2u, 3u, 4u, r.U32()});
        },
        [](NativeEnv& env, const Args& a) {
            return Run(env, "TextInBox", [&](s::GuestRam& g, s::PauseCallees& k, uint32_t&) {
                return s::TextInBox(g, k, env.sp, static_cast<int32_t>(a.a[0]), static_cast<int32_t>(a.a[1]), a.a[2], a.a[3],
                                    a.a4, static_cast<int32_t>(a.more[0]));
            });
        },
        false));

    // ------------------------------------------------------------------ SLUS 0x800400F4
    rows.push_back(PauseRow(
        "pause_box_frame", "void BoxFrame(u8 r, u8 g, u8 b, u32 *ot, s16 x0, s16 y0, s16 x1, s16 y1)  [SLUS 0x800400F4]",
        s::kBoxFrameFn,
        [](CaseContext& c, Args& a) {
            Memory& m = *c.mem;
            Rng& r = *c.rng;
            const bool derived = c.sample != nullptr;
            a.a[0] = derived ? 0u : r.U32();
            a.a[1] = derived ? 0u : r.U32();
            a.a[2] = derived ? 0x80u : r.U32();
            a.a[3] = m.PeekWord(s::kPauseFrameOtPtr) + 4u * (derived ? 6u : r.U32() % 12u);
            a.a4 = derived ? 0x56u : Pick(r, {0x56u, r.U32() % 400u, r.U32()});
            a.more[0] = derived ? 0x41u : Pick(r, {0x41u, r.U32() % 260u, r.U32()});
            a.more[1] = derived ? 0x12Au : Pick(r, {0x12Au, r.U32() % 400u, r.U32()});
            a.more[2] = derived ? 0xAFu : Pick(r, {0xAFu, r.U32() % 260u, r.U32()});
        },
        [](NativeEnv& env, const Args& a) {
            return Run(env, "BoxFrame", [&](s::GuestRam& g, s::PauseCallees& k, uint32_t&) {
                return s::BoxFrame(g, k, env.sp, a.a[0], a.a[1], a.a[2], a.a[3], static_cast<int32_t>(a.a4),
                                   static_cast<int32_t>(a.more[0]), static_cast<int32_t>(a.more[1]),
                                   static_cast<int32_t>(a.more[2]));
            });
        },
        false));

    // ------------------------------------------------------------------ SLUS 0x8002D0A8
    rows.push_back(PauseRow(
        "pause_width_id", "s16 StringWidthById(s32 font, s32 id)  [SLUS 0x8002D0A8]", s::kStringWidthIdFn,
        [](CaseContext& c, Args& a) {
            Rng& r = *c.rng;
            a.a[0] = Chance(r, 8) ? 0xFFFFFFFFu : c.mem->PeekWord(s::kHudFontIndex);
            a.a[1] = r.U32() % 181u;
        },
        [](NativeEnv& env, const Args& a) {
            return Run(env, "StringWidthById", [&](s::GuestRam& g, s::PauseCallees&, uint32_t& v0) {
                v0 = static_cast<uint32_t>(s::StringWidthById(g, static_cast<int32_t>(a.a[0]), static_cast<int32_t>(a.a[1])));
                return true;
            });
        },
        true));

    // ------------------------------------------------------------------ SLUS 0x80048FBC
    rows.push_back(PauseRow(
        "pause_get_drawenv", "DRAWENV *GetDrawEnv(DRAWENV *out)  [SLUS 0x80048FBC, libgpu]", s::kGetDrawEnvFn,
        [](CaseContext& c, Args& a) {
            Rng& r = *c.rng;
            if (c.sample == nullptr)
                for (uint32_t k = 0; k < 0x5Cu; k += 4) c.mem->PokeWord(s::kPauseDrawEnv + k, r.U32());
            a.a[0] = c.scratch + 4u * (r.U32() % 64u);
        },
        [](NativeEnv& env, const Args& a) {
            return Run(env, "GetDrawEnv", [&](s::GuestRam& g, s::PauseCallees&, uint32_t& v0) {
                v0 = s::GetDrawEnv(g, a.a[0]);
                return true;
            });
        },
        true));

    // ------------------------------------------------------------------ SLUS 0x80020E30
    {
        Row row{"pause_sound_hold", "void SoundHold(u32 v)  [SLUS 0x80020E30, the ambient voices' pitch hold]",
                s::kSoundHoldFn,
                [](CaseContext& c, Args& a) {
                    if (!c.sample) sf::PlantWorld(c);
                    EnPlantSpu(c);
                    Rng& r = *c.rng;
                    Memory& m = *c.mem;
                    const uint32_t blk = s::kSoundHoldBlock;
                    Put8(m, blk, Chance(r, 4) ? 0u : Pick(r, {1u, r.U32() & 0xFFu}));
                    Put8(m, blk + 0x59u, Chance(r, 3) ? Pick(r, {1u, r.U32() & 0xFFu}) : 0u);
                    m.PokeWord(blk + 4u, Pick(r, {2u, 2u, 1u, 0u, 3u}));
                    m.PokeWord(blk + 8u, Pick(r, {0x5CEu, r.U32() % 0x4000u, r.U32()}));
                    for (uint32_t k = 0; k < 3; ++k) {
                        const uint32_t v = blk + 20u * k;
                        EnPut(c, v + 0x10u, Chance(r, 4) ? 0u : EnHandle(c));
                        EnPut(c, v + 0x14u, Pick(r, {0x31u, r.U32() % 128u, r.U32()}));
                        EnPut(c, v + 0x18u, Pick(r, {0u, 0x7Fu, r.U32() % 128u, r.U32()}));
                    }
                    m.PokeWord(blk + 0x5Cu, r.U32() % 1000u);
                    m.PokeWord(blk + 0x60u, r.U32());
                    a.a[0] = Pick(r, {0u, 1u, 1u, r.U32()});
                },
                nullptr, /*compareV0=*/false};
        row.nativeEx = [](NativeEnv& env, const Args& a) {
            EnRun run(env);
            s::SoundHold(run.s, a.a[0]);
            return run.Done(env, 0);
        };
        row.compareSpuControl = true;
        rows.push_back(row);
    }
}
