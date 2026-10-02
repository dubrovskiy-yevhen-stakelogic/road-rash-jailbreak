// Bench rows for the front end's per-screen input handlers (and the jukebox chooser's
// object handler), ported in src\game\shell\shell_screens.{h,cpp}. Included by verify_physics.cpp
// right after rows_shell.inc, in the same anonymous namespace: the machine, the planted shell capture
// and the helpers are shellrows:: (rows_shell.inc's header says what is planted and compared).
//
// Every row calls the handler as the input pass does, h(screen), with the screen made current the
// way the shell's globals hold it, and compares v0 (the "consumed" result) and the whole machine.
//
// INPUT FAMILIES. dump-derived: the capture as it is, one pad slot (or none) pressed on pad 0 with
// the code 2 a fresh press has; randomised: the fields each handler branches on, drawn inside these
// named bounds (each the range the game's own data or code keeps):
//   * the screen's state +0x02: 32767 (its first frame, set by ArmScreen), 0, or a small count;
//   * a pad window +0x0E/+0x0F inside the four snapshot records: first 0..1, count 1..3;
//   * the edge mask fe+0x13 is 0 when a screen handler (or 0x8006BF18 from 0x8006BE84) starts: the
//     input pass clears it (0x80066814), and every arm that sets a bit - the object arm, MenuInput,
//     0x8006BF18 - also returns non-zero, which ends the pass's handler walk (0x80066A8C);
//   * selections, chooser indices, the edited port, slider slots: as rows_shell.inc names them;
//   * screen ids, the current screen, parents and deferred targets: 0..58;
//   * the keyboard (52, 56): column 0..8, row 0..4, cursor 0..7 (every arm keeps them there);
//   * the records table (52): race id 56..64 (nine 176-byte tables in 1584 bytes, frontend.md 8.5),
//     player index 0..5, times ascending as a best-times table is kept, names NUL-terminated inside
//     their 12 bytes;
//   * slider values 0..128, the trophy room's race 0..64, on/off bytes 0..1;
//   * the music track fe+0x0E is the capture's, the one the music player 0x800A0830 has loaded: a
//     different one makes MusicPlay 0x8007EDE0 (reached by MenuInput's Square arm) load it from the
//     CD (0x8007EEB8), whose critical section is a SYSCALL the bench machine does not service. The
//     jukebox rows still change it the game's way, through chooser 31 (which consumes the frame, so
//     the menu - and MusicPlay - do not run on it).
//
// THE SEAMS (declared per row): PlayUiSound 0x8007EAC0, MusicPlay 0x8007EDE0, the mixer 0x8007F20C
// (channel, value), the slider preview 0x8007EB1C (kind, value) and its stop 0x8007ECFC (), the
// jukebox SLUS 0x8002490C (track, on), and the sound-mode pair ChooserApply may reach. The two
// keyboard rows run the BIOS's strcpy / strcmp (A(19h) / A(17h), reached through the SLUS stubs
// 0x800448E4 / 0x80044904) on both machines; the port has its own.

// The port's header, at namespace scope: this file is included inside verify_physics.cpp's anonymous
// namespace, which is closed around the include and reopened (the same unnamed namespace) after it.
} // namespace
#include "game/shell/shell_screens.h"
namespace {

namespace screenrows {

using namespace shellrows;

uint32_t Chooser(const Memory& m, uint32_t i) { return m.PeekWord(sh::kChoosers + 4u * i); }

std::vector<OracleCallee> ScreenSeams() {
    std::vector<OracleCallee> v = Sounds();
    v.push_back(OracleCallee{sh::kSoundMode, 1, 0, {}, 0});
    v.push_back(OracleCallee{sh::kSoundModeUi, 1, 0, {}, 0});
    v.push_back(OracleCallee{sh::kJukeboxSet, 2, 0, {}, 0});
    v.push_back(OracleCallee{sh::kVolumeSet, 2, 0, {}, 0});
    v.push_back(OracleCallee{sh::kSoundPreview, 2, 0, {}, 0});
    v.push_back(OracleCallee{sh::kSoundPreviewStop, 0, 0, {}, 0});
    return v;
}

// The state word +0x02 inside its named bound.
void State(CaseContext& c, uint32_t s) {
    Rng& r = *c.rng;
    const uint32_t v = c.sample ? 32767u : (Chance(r, 2) ? 32767u : (Chance(r, 2) ? 0u : 1u + Pick(r, 60)));
    Put16(*c.mem, s + 2u, v);
}

// The pad window inside its named bound.
void Window(CaseContext& c, uint32_t s) {
    if (c.sample) return;
    Rng& r = *c.rng;
    if (!Chance(r, 2)) return;
    Put8(*c.mem, s + 14u, Pick(r, 2));
    Put8(*c.mem, s + 15u, 1u + Pick(r, 3));
}

void Edges(CaseContext& c) {
    Put8(*c.mem, sh::kFeEdges, 0); // the named bound above
}

// A menu screen made current with its pads, state and window drawn; the widgets, gates and the
// screen's own flags too when randomised (what MenuInput branches on, as its row draws them).
uint32_t MenuScreen(CaseContext& c, uint32_t id) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    const uint32_t s = Screen(m, id);
    MakeCurrent(m, id);
    Pads(c, !c.sample);
    State(c, s);
    Window(c, s);
    Edges(c);
    if (!c.sample) {
        ShuffleWidgets(c, s);
        ShuffleGates(c);
        Put16(m, s, Get16(m, s) ^ (static_cast<uint32_t>(Pick(r, 8)) << 8)); // flags 0x100/0x200/0x400
        Put8(m, sh::kSession + 0x18u, Pick(r, 2));
        Put8(m, m.PeekWord(sh::kGameStatePtr) + 4u, Chance(r, 2) ? 4u : 0x22u);
        Put8(m, sh::kFeMusicRestart, Pick(r, 2));
        Put8(m, sh::kFeMusicOk, Pick(r, 2));
    }
    Select(c, s);
    return s;
}

using Handler = bool (*)(sh::GuestRam&, sh::ShellCallees&, uint32_t, int32_t*);

Row HandlerRow(const char* name, const char* sig, uint32_t entry, std::function<void(CaseContext&, Args&)> setup,
               Handler h, std::vector<OracleCallee> callees) {
    return ShellRow(
        name, sig, entry, std::move(setup),
        [h](NativeEnv& env, const Args& a) {
            return Run(env, [&](sh::GuestRam& g, sh::ShellCallees& k) {
                int32_t r = 0;
                if (!h(g, k, a.a[0], &r) && env.failure.empty()) env.failure = "refused";
                return static_cast<uint32_t>(r);
            });
        },
        true, std::move(callees));
}

// The keyboard's cursor and text inside their named bounds.
void Keyboard(CaseContext& c) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    m.PokeWord(sh::kKeyCol, Pick(r, 9));
    m.PokeWord(sh::kKeyRow, Chance(r, 3) ? 4u : Pick(r, 5));
    m.PokeWord(sh::kKeyPos, Pick(r, 8));
    for (uint32_t i = 0; i < 9u; ++i) {
        const uint32_t ch = Chance(r, 3) ? 0x5Fu : (Chance(r, 8) ? 0x20u : 0x41u + Pick(r, 26));
        Put8(m, sh::kKeyBuf + i, ch);
    }
    if (Chance(r, 3)) Put8(m, sh::kKeyBuf + 8u, 0);
}

} // namespace screenrows

void AddShellScreenRows(std::vector<Row>& rows) {
    using namespace shellrows;
    using namespace screenrows;

    // ------------------------------------------------------------------ RASHCDF 0x8006AAF8
    rows.push_back(HandlerRow(
        "shell_scr_logo", "int LogoInput(Screen*)  [RASHCDF 0x8006AAF8, screen 0]", 0x8006AAF8u,
        [](CaseContext& c, Args& a) {
            PlantShell(c);
            Memory& m = *c.mem;
            Rng& r = *c.rng;
            const uint32_t s = Screen(m, 0);
            MakeCurrent(m, c.sample ? 0u : (Chance(r, 2) ? 0u : Pick(r, 59)));
            Put16(m, s + 2u, Chance(r, 2) ? 0u : (Chance(r, 2) ? 32767u : Pick(r, 10)));
            Put16(m, sh::kFeMedia, r.U32() & 0xFFFFu);
            m.PokeWord(sh::kIdle, r.U32() & 0xFFFu);
            a.a[0] = s;
        },
        [](sh::GuestRam& g, sh::ShellCallees&, uint32_t s, int32_t* r) { return sh::LogoInput(g, s, r); }, {}));

    // ------------------------------------------------------------------ RASHCDF 0x8006A838
    rows.push_back(HandlerRow(
        "shell_scr_credits", "int CreditsInput(Screen*)  [RASHCDF 0x8006A838, screens 21/50]", 0x8006A838u,
        [](CaseContext& c, Args& a) {
            PlantShell(c);
            Rng& r = *c.rng;
            const uint32_t s = MenuScreen(c, Chance(r, 2) ? 21u : 50u);
            Put16(*c.mem, sh::kFeMedia, c.sample ? 8u : (r.U32() & 0xFFFFu));
            c.mem->PokeWord(sh::kIdle, r.U32() & 0xFFFu);
            a.a[0] = s;
        },
        sh::CreditsInput, ScreenSeams()));

    // ------------------------------------------------------------------ RASHCDF 0x8006AD20
    rows.push_back(HandlerRow(
        "shell_scr_message_panel", "int MessagePanelInput(Screen*)  [RASHCDF 0x8006AD20, screen 53]", 0x8006AD20u,
        [](CaseContext& c, Args& a) {
            PlantShell(c);
            Memory& m = *c.mem;
            Rng& r = *c.rng;
            const uint32_t s = Screen(m, 53);
            MakeCurrent(m, 53);
            Pads(c, !c.sample);
            State(c, s);
            Window(c, s);
            Put16(m, s + 10u, Pick(r, 59));
            m.PokeWord(sh::kIdle, Chance(r, 3) ? 901u + Pick(r, 100) : Pick(r, 901));
            a.a[0] = s;
        },
        sh::MessagePanelInput, ScreenSeams()));

    // ------------------------------------------------------------------ RASHCDF 0x80069440
    rows.push_back(HandlerRow(
        "shell_scr_race_options", "int RaceOptionsInput(Screen*)  [RASHCDF 0x80069440, screens 25/39]", 0x80069440u,
        [](CaseContext& c, Args& a) {
            PlantShell(c);
            Memory& m = *c.mem;
            Rng& r = *c.rng;
            const uint32_t s = MenuScreen(c, Chance(r, 2) ? 25u : 39u);
            if (!c.sample) {
                for (uint32_t i = 0; i < 3u; ++i) {
                    Put8(m, sh::kSession + 9u + i, Pick(r, 2));
                    Put8(m, sh::kRaceOptSaved + i, Pick(r, 2));
                    ShuffleChooser(c, Chooser(m, 11u + i));
                }
            }
            a.a[0] = s;
        },
        sh::RaceOptionsInput, ScreenSeams()));

    // ------------------------------------------------------------------ RASHCDF 0x80069560
    rows.push_back(HandlerRow(
        "shell_scr_trophy_room", "int TrophyRoomInput(Screen*)  [RASHCDF 0x80069560, screen 26]", 0x80069560u,
        [](CaseContext& c, Args& a) {
            PlantShell(c);
            Memory& m = *c.mem;
            Rng& r = *c.rng;
            const uint32_t s = MenuScreen(c, 26);
            if (!c.sample) {
                Put8(m, sh::kSession + 8u, Pick(r, 65));
                Put8(m, sh::kFeNewGame, Pick(r, 2));
                Put8(m, sh::kSession + 0x16u, Pick(r, 2));
                Put16(m, sh::kFeHub, Pick(r, 59));
                ShuffleChooser(c, Chooser(m, 10));
            }
            a.a[0] = s;
        },
        sh::TrophyRoomInput, ScreenSeams()));

    // ------------------------------------------------------------------ RASHCDF 0x8006AE6C
    rows.push_back(HandlerRow(
        "shell_scr_options", "int OptionsInput(Screen*)  [RASHCDF 0x8006AE6C, screen 42]", 0x8006AE6Cu,
        [](CaseContext& c, Args& a) {
            PlantShell(c);
            Memory& m = *c.mem;
            Rng& r = *c.rng;
            const uint32_t s = MenuScreen(c, 42);
            if (!c.sample) {
                if (Chance(r, 3)) MakeCurrent(m, Pick(r, 59));
                Put8(m, sh::kSession + 0x10u, Pick(r, 59));
                // The four hold stamps of pad 0 the code-keyboard shortcut reads (slots 0, 8, 10, 11).
                for (uint32_t off : {20u, 84u, 100u, 108u})
                    m.PokeWord(sh::kPads + off, Chance(r, 5) ? (Chance(r, 2) ? 0u : 0xFFFFFFF0u) : 1u + Pick(r, 200));
                if (Chance(r, 2)) Put8(m, sh::kPads + 82u, Chance(r, 4) ? 0xFFu : 2u);
                const int32_t n = static_cast<int16_t>(Get16(m, s + 8u));
                const int32_t sel = static_cast<int16_t>(Get16(m, s + 4u));
                if (n > 0 && sel >= 0 && sel < n && Chance(r, 2)) {
                    const uint32_t w = m.PeekWord(s + 16u) + 120u * static_cast<uint32_t>(sel);
                    Put16(m, w + 8u, Chance(r, 4) ? 13u : 12u);
                    Put16(m, w + 18u, Chance(r, 4) ? 70u : 71u);
                }
            }
            a.a[0] = s;
        },
        sh::OptionsInput, ScreenSeams()));

    // ------------------------------------------------------------------ RASHCDF 0x8006AB58
    rows.push_back(HandlerRow(
        "shell_scr_abort_modal", "int AbortModalInput(Screen*)  [RASHCDF 0x8006AB58, screens 54/55]", 0x8006AB58u,
        [](CaseContext& c, Args& a) {
            PlantShell(c);
            Memory& m = *c.mem;
            Rng& r = *c.rng;
            const uint32_t id = Chance(r, 2) ? 54u : 55u;
            const uint32_t s = Screen(m, id);
            MakeCurrent(m, id);
            Pads(c, !c.sample);
            Window(c, s);
            Put16(m, s + 2u, c.sample ? 0u : (Chance(r, 2) ? 0u : (Chance(r, 2) ? 32767u : 1u + Pick(r, 60))));
            Put8(m, sh::kFeAbortYes, Pick(r, 2));
            Put8(m, sh::kFeDeferred, Pick(r, 59));
            Put16(m, s + 10u, Pick(r, 59));
            if (!c.sample) ShuffleChooser(c, Chooser(m, 36));
            a.a[0] = s;
        },
        sh::AbortModalInput, ScreenSeams()));

    // ------------------------------------------------------------------ RASHCDF 0x80069FF0
    rows.push_back(HandlerRow(
        "shell_scr_sound_options", "int SoundOptionsInput(Screen*)  [RASHCDF 0x80069FF0 (+0x8006A0E4), screen 48]",
        0x80069FF0u,
        [](CaseContext& c, Args& a) {
            PlantShell(c);
            Memory& m = *c.mem;
            Rng& r = *c.rng;
            const uint32_t s = MenuScreen(c, 48);
            if (!c.sample) {
                for (uint32_t i = 0; i < 7u; ++i) {
                    if (Chance(r, 2)) m.PokeWord(sh::kSession + 0xD4u + 4u * i, Pick(r, 129));
                    if (Chance(r, 2)) m.PokeWord(sh::kSliderDescs + 16u * i + 12u, Pick(r, 129));
                    if (Chance(r, 2)) m.PokeWord(sh::kSoundTemplate + 16u * i + 12u, Pick(r, 129));
                }
                Put8(m, sh::kSession + 20u, Pick(r, 2));
                m.PokeWord(sh::kSoundSaved, Pick(r, 2));
                m.PokeWord(sh::kSoundPreviewDue, Pick(r, 2));
                ShuffleChooser(c, Chooser(m, 28));
            }
            // The slider BindSlider binds before the handler: none, or slot 0..5 with its value slot.
            if (!c.sample && Chance(r, 3)) {
                m.PokeWord(sh::kFeSlider, 0);
                m.PokeWord(sh::kFeSliderDst, 0);
            } else {
                const uint32_t slot = Pick(r, 6);
                m.PokeWord(sh::kFeSlider, sh::kSliderDescs + 16u * slot);
                m.PokeWord(sh::kFeSliderDst, 0x800D81ACu + 4u * slot);
                if (!c.sample && Chance(r, 3)) Put8(m, sh::kSliderDescs + 16u * slot + 11u, Pick(r, 8));
            }
            a.a[0] = s;
        },
        sh::SoundOptionsInput, ScreenSeams()));

    // ------------------------------------------------------------------ RASHCDF 0x8006A390
    rows.push_back(HandlerRow(
        "shell_scr_controller_options", "int ControllerOptionsInput(Screen*)  [RASHCDF 0x8006A390, screen 49]",
        0x8006A390u,
        [](CaseContext& c, Args& a) {
            PlantShell(c);
            Memory& m = *c.mem;
            Rng& r = *c.rng;
            const uint32_t s = MenuScreen(c, 49);
            if (!c.sample) {
                for (uint32_t i = 0; i < 6u; ++i) {
                    Put8(m, sh::kPlayers + 36u * i + 8u, Pick(r, 6));
                    Put8(m, sh::kPlayers + 36u * i + 23u, Pick(r, 2));
                    Put8(m, sh::kPadCfgSaved + i, Pick(r, 6));
                    Put8(m, sh::kPadVibSaved + i, Pick(r, 2));
                }
                ShuffleChooser(c, Chooser(m, 34));
                ShuffleChooser(c, Chooser(m, 35));
            }
            a.a[0] = s;
        },
        sh::ControllerOptionsInput, ScreenSeams()));

    // ------------------------------------------------------------------ RASHCDF 0x8006A518
    rows.push_back(HandlerRow(
        "shell_scr_multiplayer_options", "int MultiplayerOptionsInput(Screen*)  [RASHCDF 0x8006A518, screen 51]",
        0x8006A518u,
        [](CaseContext& c, Args& a) {
            PlantShell(c);
            Memory& m = *c.mem;
            Rng& r = *c.rng;
            const uint32_t s = MenuScreen(c, 51);
            if (!c.sample) {
                Put8(m, sh::kSession + 25u, Pick(r, 3));
                Put8(m, sh::kSession + 23u, Pick(r, 3));
                Put8(m, sh::kMultiSaved + 0u, Pick(r, 3));
                Put8(m, sh::kMultiSaved + 1u, Pick(r, 3));
                ShuffleChooser(c, Chooser(m, 29));
                ShuffleChooser(c, Chooser(m, 30));
            }
            a.a[0] = s;
        },
        sh::MultiplayerOptionsInput, ScreenSeams()));

    // ------------------------------------------------------------------ the jukebox
    auto jukebox = [](CaseContext& c, Args& a) {
        PlantShell(c);
        Memory& m = *c.mem;
        Rng& r = *c.rng;
        const uint32_t s = MenuScreen(c, 47);
        if (!c.sample) {
            for (uint32_t i = 0; i < 18u; ++i) {
                Put8(m, sh::kSession + 0xC0u + i, Chance(r, 4) ? 0u : 1u);
                Put8(m, sh::kJukeSaved + i, Chance(r, 4) ? 0u : 1u);
            }
            Put8(m, sh::kFeJukeRow, Pick(r, 5));
            ShuffleChooser(c, Chooser(m, 31));
            ShuffleChooser(c, Chooser(m, 32));
            ShuffleGates(c);
        }
        a.a[0] = s;
    };
    rows.push_back(HandlerRow("shell_scr_jukebox", "int JukeboxInput(Screen*)  [RASHCDF 0x8006A610, screen 47]",
                              0x8006A610u, jukebox, sh::JukeboxInput, ScreenSeams()));
    rows.push_back(HandlerRow("shell_scr_jukebox_chooser",
                              "int JukeboxChooserInput(Screen*)  [RASHCDF 0x8006BE84, object table 0x8009CC40[31]]",
                              0x8006BE84u, jukebox, sh::JukeboxChooserInput, ScreenSeams()));
    rows.push_back(ShellRow(
        "shell_scr_chooser_updown", "int ChooserUpDown(Screen*, Chooser*, int padFirst, int padEnd)  [RASHCDF 0x8006BF18]",
        0x8006BF18u,
        [](CaseContext& c, Args& a) {
            PlantShell(c);
            Memory& m = *c.mem;
            Rng& r = *c.rng;
            const uint32_t obj = Chance(r, 8) ? 0u : PickChooser(r, m);
            if (!c.sample) {
                ShuffleChooser(c, obj);
                ShuffleGates(c);
            }
            Pads(c, !c.sample);
            Edges(c);
            a.a[0] = Screen(m, 47);
            a.a[1] = obj;
            a.a[2] = c.sample ? 0u : Pick(r, 2);
            a.a[3] = c.sample ? 1u : Pick(r, 5);
        },
        [](NativeEnv& env, const Args& a) {
            return Run(env, [&](sh::GuestRam& g, sh::ShellCallees& k) {
                int32_t r = 0;
                if (!sh::ChooserUpDown(g, k, a.a[1], static_cast<int32_t>(a.a[2]), static_cast<int32_t>(a.a[3]), &r) &&
                    env.failure.empty())
                    env.failure = "refused";
                return static_cast<uint32_t>(r);
            });
        },
        true, ScreenSeams()));

    // ------------------------------------------------------------------ RASHCDF 0x80069A8C
    {
        Row row = HandlerRow(
            "shell_scr_new_record", "int NewRecordInput(Screen*)  [RASHCDF 0x80069A8C, screen 52]", 0x80069A8Cu,
            [](CaseContext& c, Args& a) {
                PlantShell(c);
                Memory& m = *c.mem;
                Rng& r = *c.rng;
                const uint32_t s = Screen(m, 52);
                MakeCurrent(m, 52);
                Pads(c, !c.sample);
                State(c, s);
                Window(c, s);
                const uint32_t race = 56u + Pick(r, 9);
                Put8(m, sh::kSession + 8u, race);
                Put8(m, sh::kSession + 0x13u, Pick(r, 6));
                if (!c.sample) {
                    Keyboard(c);
                    for (uint32_t i = 0; i < 3u; ++i) Put8(m, sh::kSession + 9u + i, Pick(r, 2));
                    const uint32_t table = sh::kRecords + 176u * (race - 56u);
                    int32_t t = static_cast<int32_t>(Pick(r, 4000));
                    for (uint32_t e = 0; e < 8u; ++e) {
                        const uint32_t rec = table + 16u + 20u * e;
                        t += static_cast<int32_t>(Pick(r, 3000));
                        m.PokeWord(rec + 12u, static_cast<uint32_t>(t));
                        if (Chance(r, 2)) {
                            const uint32_t len = Pick(r, 12);
                            for (uint32_t i = 0; i < 12u; ++i) Put8(m, rec + i, i < len ? 0x41u + Pick(r, 26) : 0u);
                        }
                        for (uint32_t i = 16; i < 20u; ++i)
                            if (Chance(r, 2)) Put8(m, rec + i, Pick(r, 20));
                    }
                    for (uint32_t p = 0; p < 6u; ++p) {
                        m.PokeWord(sh::kPlayers + 36u * p, Pick(r, 30000));
                        Put8(m, sh::kPlayers + 36u * p + 7u, Pick(r, 20));
                    }
                    for (uint32_t i = 0; i < 16u; i += 4) m.PokeWord(sh::kRecordHead + i, r.U32());
                    // Half the time the key cursor is on "enter" (row 4, columns 7-8) and the
                    // window's first pad presses Cross, so the insertion runs.
                    if (Chance(r, 2)) {
                        m.PokeWord(sh::kKeyRow, 4);
                        m.PokeWord(sh::kKeyCol, 7u + Pick(r, 2));
                        Put8(m, sh::PadSlot(static_cast<uint32_t>(static_cast<int8_t>(m.PeekByte(s + 14u))), sh::kCross), 2);
                    }
                }
                a.a[0] = s;
            },
            sh::NewRecordInput, ScreenSeams());
        row.allowBiosCalls = true;
        rows.push_back(std::move(row));
    }

    // ------------------------------------------------------------------ RASHCDF 0x80069618
    {
        Row row = HandlerRow(
            "shell_scr_code_entry", "int CodeEntryInput(Screen*)  [RASHCDF 0x80069618, screen 56]", 0x80069618u,
            [](CaseContext& c, Args& a) {
                PlantShell(c);
                Memory& m = *c.mem;
                Rng& r = *c.rng;
                const uint32_t s = Screen(m, 56);
                MakeCurrent(m, 56);
                Pads(c, !c.sample);
                State(c, s);
                Window(c, s);
                if (!c.sample) {
                    Keyboard(c);
                    m.PokeWord(sh::kDebugFlags, Chance(r, 2) ? 0u : (r.U32() & 0xFFu));
                    // Half the time the text is one of the eight codes (read out of the table the
                    // original compares against), with the cursor on its end.
                    if (Chance(r, 2)) {
                        const uint32_t str = m.PeekWord(sh::kCodeTable + 4u * Pick(r, 8));
                        uint32_t n = 0;
                        while (n < 8u && m.PeekByte(str + n) != 0) {
                            Put8(m, sh::kKeyBuf + n, m.PeekByte(str + n));
                            ++n;
                        }
                        const uint32_t last = n == 0 ? 0u : n - 1u;
                        m.PokeWord(sh::kKeyPos, (Chance(r, 2) || n > 7u) ? last : n); // cursor bound 0..7
                        if (n < 8u) Put8(m, sh::kKeyBuf + n, Chance(r, 2) ? 0x5Fu : 0x41u);
                        m.PokeWord(sh::kKeyRow, 4);
                        m.PokeWord(sh::kKeyCol, 7u + Pick(r, 2));
                        if (Chance(r, 2))
                            Put8(m, sh::PadSlot(static_cast<uint32_t>(static_cast<int8_t>(m.PeekByte(s + 14u))), sh::kCross), 2);
                    }
                }
                a.a[0] = s;
            },
            sh::CodeEntryInput, ScreenSeams());
        row.allowBiosCalls = true;
        rows.push_back(std::move(row));
    }
}
