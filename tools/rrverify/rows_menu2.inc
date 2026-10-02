// Bench rows for the panel films and the logos - widget types 2..6 and 8 - ported in
// src\game\shell\shell_panel.{h,cpp}. Included last by verify_physics.cpp,
// after rows_menu.inc (whose menurows:: helpers it uses); the machine is shellrows:: (the planted capture
// of the running shell).
//
// INPUT FAMILIES. dump-derived: the capture as it is, one widget of the row's type, the screen live and
// current, the attract idle at the gate's 151. Randomised, inside these named bounds:
//   * the film state fe+0x0A bits 0..2 any of the 8 combinations; the handle fe+0xB4 -1 or 0..3; the
//     channel fe+0xB8 0 or 1..3; the attract idle 0x8005ACAC in 0..300;
//   * the current screen fe+0x00 the widget's screen or another; its live bit set or clear;
//   * the session's mode from {0, 1, 4, 8, 0x10, 0x11, 0x18, 0x20, 2, 0x40} and venue 0..7; the screen's
//     selection inside its bound (rows_shell.inc Select);
//   * the bound chooser fe+0x94 none or one of the 37 with its current option in [0, count) - the option
//     lists are the capture's (the kind-4 film records and the kind-0x30 pictures the bikes' carry);
//   * a film record's name index < 42 (the resource-name array's length);
//   * the edited port fe+0x16 0..3 and that player's layout byte +0x08 in -2..6 (the diagram's table
//     has 5 entries);
//   * the result panel's pictures fe+0xAC / +0xB0 300 (none) or a FourCC of the capture's sprite table.
// NAMED BOUND - the film library RASHCDF 0x8005F36C / 0x8005F484 / 0x8005F7E0 drives the CD stream and
// the MDEC (hardware registers the bench machine does not have: the guest traps on them). Every case
// replaces each entry with `jr ra; li v0, R` in the case's RAM, on both machines (the pause rows'
// device, rows_pause.inc) - R drawn 0 or 1 per case for the start and the picture call - so the calls,
// their arguments and what the port does with the answers are compared; the library's own work is the
// product's PanelFilms (OURS). The file layer SLUS 0x8001458C / 0x8001460C gets the same treatment
// (0x8001458C answering -1 or 0..3), for the same reason: its directory search reads the CD.
// The sprite emitter 0x800700F0 runs as the original: a picture record not resident in the capture's
// VRAM (a shared slot's other occupant) would need its decode 0x8007A400 / LoadImage, which wait on the
// GPU and the CD - the logo rows keep every sprite record the emitter reaches in the state the capture
// holds with the 0x100000..0x800000 slot bits cleared (a plain loaded record: 0x8007A400's
// "already in VRAM" arm), which is named, not hidden: the emitter's slot bookkeeping is rows_menu.inc's.

} // namespace
#include "game/shell/shell_panel.h"
namespace {

namespace menu2rows {

using namespace shellrows;
using namespace menurows;

constexpr uint32_t kJrRa = 0x03E00008u;

// `jr ra; li v0, r` over a function's first two words, in the case's RAM.
void Stub(Memory& m, uint32_t entry, int32_t r) {
    m.PokeWord(entry, kJrRa);
    m.PokeWord(entry + 4u, 0x24020000u | (static_cast<uint32_t>(r) & 0xFFFFu));
}

void Stubs(CaseContext& c) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    const bool rnd = c.sample == nullptr;
    Stub(m, sh::kFilmStart, rnd ? static_cast<int32_t>(Pick(r, 2)) : 1);
    Stub(m, sh::kFilmPicture, rnd ? static_cast<int32_t>(Pick(r, 2)) : 1);
    Stub(m, sh::kFilmStop, 0);
    Stub(m, sh::kFileOpen, rnd ? static_cast<int32_t>(Pick(r, 5)) - 1 : 0);
    Stub(m, sh::kFileClose, 0);
}

// A screen that has a widget of one of `types`, with that widget.
std::pair<uint32_t, uint32_t> PickWidget(CaseContext& c, std::initializer_list<int> types) {
    const auto v = WidgetsOf(*c.mem, types);
    if (v.empty()) return {0u, 0u};
    return v[c.sample ? 0u : Pick(*c.rng, static_cast<uint32_t>(v.size()))];
}

// The film state and the gate's inputs.
void FilmState(CaseContext& c, uint32_t s) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    Put16(m, s, Get16(m, s) | 2u);
    const uint16_t id = Get16(m, s + 6u);
    MakeCurrent(m, id);
    m.PokeWord(sh::kIdle, 151u);
    if (c.sample) return;
    if (Chance(r, 4)) Put16(m, s, Get16(m, s) & ~2u);
    if (Chance(r, 4)) MakeCurrent(m, PickScreenId(r, m));
    m.PokeWord(sh::kIdle, Chance(r, 3) ? 151u + Pick(r, 3) - 1u : Pick(r, 301));
    Put16(m, sh::kFeMedia, (Get16(m, sh::kFeMedia) & ~7u) | Pick(r, 8));
    m.PokeWord(sh::kFilmHandle, Chance(r, 3) ? 0xFFFFFFFFu : Pick(r, 4));
    Put16(m, sh::kFilmChannel, Chance(r, 2) ? 0u : 1u + Pick(r, 3));
}

// The session, the selection and the bound chooser the handlers read.
void PanelContext(CaseContext& c, uint32_t s) {
    if (c.sample) return;
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    static const uint32_t kModes[] = {0, 1, 4, 8, 0x10, 0x11, 0x18, 0x20, 2, 0x40};
    m.PokeWord(sh::kSession, kModes[Pick(r, 10)]);
    Put8(m, sh::kSession + 4u, Pick(r, 8));
    Select(c, s);
    if (Chance(r, 4)) {
        m.PokeWord(sh::kFeChooser, 0);
    } else {
        const uint32_t obj = PickChooser(r, m);
        m.PokeWord(sh::kFeChooser, obj);
        const int32_t n = static_cast<int8_t>(m.PeekByte(obj));
        if (n > 0) Put8(m, obj + 3u, Pick(r, static_cast<uint32_t>(n)));
    }
    Put8(m, sh::kFePort, Pick(r, 4));
    for (uint32_t p = 0; p < 4u; ++p) Put8(m, sh::kPlayers + 36u * p + 8u, static_cast<uint32_t>(static_cast<int32_t>(Pick(r, 9)) - 2));
    static const uint32_t kPics[] = {300u, 0x31434142u, 0x474C4F35u, 0x474C5454u};
    m.PokeWord(sh::kFe + 0xACu, kPics[Pick(r, 4)]);
    m.PokeWord(sh::kFe + 0xB0u, kPics[Pick(r, 4)]);
}

// A film record: the widget's own, or the bound chooser's kind-4 entry; its name index in bounds.
uint32_t FilmRecord(CaseContext& c, uint32_t w) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    uint32_t rec = w + 0x10u;
    if (!c.sample && Chance(r, 2)) {
        // A bike chooser's current option's kind-4 record (chooser slots 4 / 5 carry them).
        for (int t = 0; t < 8; ++t) {
            const uint32_t obj = ChooserObj(m, Chance(r, 2) ? 4u : 5u);
            if (obj == 0 || m.PeekWord(obj + 4u) == 0) continue;
            const int32_t n = static_cast<int8_t>(m.PeekByte(obj));
            if (n <= 0) continue;
            const uint32_t opt = m.PeekWord(obj + 4u) + 12u * Pick(r, static_cast<uint32_t>(n));
            const int32_t cnt = static_cast<int16_t>(Get16(m, opt + 6u));
            uint32_t e = m.PeekWord(opt + 8u);
            for (int32_t i = 0; i < cnt; ++i, e += 28u)
                if (Get16(m, e) == 4u) {
                    rec = e + 4u;
                    break;
                }
            break;
        }
    }
    if (m.PeekWord(rec) >= 42u) m.PokeWord(rec, c.sample ? 21u : Pick(r, 42));
    if (!c.sample && Chance(r, 3)) m.PokeWord(rec + 0x10u, Pick(r, 100));
    return rec;
}

// Every sprite record in the plain "loaded, in VRAM" state the capture's resident pictures have (the
// header's named bound).
void PlainSprites(CaseContext& c) {
    Memory& m = *c.mem;
    for (uint32_t i = 0; i < 300u; ++i) {
        const uint32_t rec = sh::kSpriteRecs + 0x24u * i;
        const uint32_t f = m.PeekWord(rec);
        if (f & 0x10000000u) m.PokeWord(rec, (f & ~0x00F00000u) | 0x20000000u);
    }
}

std::vector<OracleCallee> CloseSeams() {
    return {OracleCallee{sh::kFilmStop, 1, 0, {}, 0}, OracleCallee{sh::kFileClose, 1, 0, {}, 0}};
}
std::vector<OracleCallee> FilmSeams() {
    std::vector<OracleCallee> v = CloseSeams();
    v.push_back(OracleCallee{sh::kSplashFade, 1, 0, {}, 0});
    v.push_back(OracleCallee{sh::kSprintf, 3, 0, {OracleFrameOut{0, 32}}, 0});
    v.push_back(OracleCallee{sh::kFileOpen, 2, 0, {OracleFrameOut{0, 32}}, 0});
    v.push_back(OracleCallee{sh::kFilmStart, 4, 5, {}, 0});
    v.push_back(OracleCallee{sh::kFilmPicture, 4, 1, {}, 0});
    return v;
}
std::vector<OracleCallee> EmitSeams() {
    return {OracleCallee{sh::kSpriteEmit, 3, 0, {OracleFrameOut{1, 12}}, 0}, OracleCallee{sh::kPacketWrap, 2, 0, {}, 0},
            OracleCallee{sh::kSpriteUpload, 1, 0, {}, 0}};
}

template <class F>
Row PanelRow(const char* name, const char* sig, uint32_t entry, uint32_t frame, std::function<void(CaseContext&, Args&)> setup,
             F call, std::vector<OracleCallee> seams) {
    return ShellRow(
        name, sig, entry, std::move(setup),
        [call, frame](NativeEnv& env, const Args& a) {
            return Run(env, [&](sh::GuestRam& g, sh::ShellCallees& k) {
                uint32_t r = 0;
                if (!call(g, k, a, env.sp + frame, &r) && env.failure.empty()) env.failure = "refused";
                return r;
            });
        },
        true, std::move(seams), frame);
}

} // namespace menu2rows

void AddMenu2Rows(std::vector<Row>& rows) {
    using namespace shellrows;
    using namespace menu2rows;
    using G = sh::GuestRam;
    using K = sh::ShellCallees;

    // ------------------------------------------------------------------ RASHCDF 0x80064154 / 0x800641D4
    auto optionSetup = [](CaseContext& c, Args& a) {
        PlantShell(c);
        const auto sw = PickWidget(c, {13});
        PanelContext(c, sw.first);
        if (c.sample) c.mem->PokeWord(sh::kFeChooser, ChooserObj(*c.mem, 4u));
        static const uint32_t kKinds[] = {0x30u, 0x31u, 4u, 8u, 0u};
        a.a[0] = c.sample ? 0x30u : (Chance(*c.rng, 4) ? (c.rng->U32() & 0xFFFFFu) : kKinds[Pick(*c.rng, 5)]);
    };
    rows.push_back(PanelRow("shell_option_record", "void* OptionRecord(u16 kind)  [RASHCDF 0x80064154]", 0x80064154u, 0,
                            optionSetup, [](G& g, K&, const Args& a, uint32_t, uint32_t* r) {
                                *r = sh::OptionRecord(g, a.a[0]);
                                return true;
                            }, {}));
    rows.push_back(PanelRow("shell_option_film", "void* OptionFilm()  [RASHCDF 0x800641D4]", 0x800641D4u, 0, optionSetup,
                            [](G& g, K&, const Args&, uint32_t, uint32_t* r) {
                                *r = sh::OptionFilm(g);
                                return true;
                            }, {}));

    // ------------------------------------------------------------------ RASHCDF 0x8006FE6C
    rows.push_back(PanelRow("shell_film_close", "s32 FilmClose()  [RASHCDF 0x8006FE6C]", 0x8006FE6Cu, sh::kFilmCloseFrame,
                            [](CaseContext& c, Args&) {
                                PlantShell(c);
                                Stubs(c);
                                const auto sw = PickWidget(c, {6});
                                FilmState(c, sw.first);
                                if (c.sample) Put16(*c.mem, sh::kFeMedia, 1u);
                            },
                            [](G& g, K& k, const Args&, uint32_t, uint32_t* r) { return sh::FilmClose(g, k, r); },
                            CloseSeams()));

    // ------------------------------------------------------------------ RASHCDF 0x8006FEF4 / 0x80070018 / 0x8006E6F4
    auto filmSetup = [](CaseContext& c, Args& a) {
        PlantShell(c);
        Stubs(c);
        const auto sw = PickWidget(c, {2, 3, 4, 5, 6});
        FilmState(c, sw.first);
        PanelContext(c, sw.first);
        a.a[0] = sw.first;
        a.a[1] = FilmRecord(c, sw.second);
        if (c.sample) Put16(*c.mem, sh::kFeMedia, 0u);
    };
    rows.push_back(PanelRow("shell_film_open", "s32 FilmOpen(FilmRec*)  [RASHCDF 0x8006FEF4]", 0x8006FEF4u, sh::kFilmOpenFrame,
                            [filmSetup](CaseContext& c, Args& a) {
                                filmSetup(c, a);
                                a.a[0] = a.a[1];
                            },
                            [](G& g, K& k, const Args& a, uint32_t sp, uint32_t* r) { return sh::FilmOpen(g, k, a.a[0], sp, r); },
                            FilmSeams()));
    rows.back().allowBiosCalls = true; // sprintf reaches strlen through the A0 vector
    rows.push_back(PanelRow("shell_film_step", "s32 FilmStep(FilmRec*)  [RASHCDF 0x80070018]", 0x80070018u,
                            sh::kFilmStepFrame + sh::kFilmOpenFrame,
                            [filmSetup](CaseContext& c, Args& a) {
                                filmSetup(c, a);
                                a.a[0] = a.a[1];
                                if (c.sample) Put16(*c.mem, sh::kFeMedia, 1u);
                            },
                            [](G& g, K& k, const Args& a, uint32_t sp, uint32_t* r) { return sh::FilmStep(g, k, a.a[0], sp, r); },
                            FilmSeams()));
    rows.back().allowBiosCalls = true; // sprintf reaches strlen through the A0 vector
    rows.push_back(PanelRow("shell_film_gate", "s32 FilmGate(Screen*, FilmRec*)  [RASHCDF 0x8006E6F4]", 0x8006E6F4u,
                            sh::kFilmGateFrame + sh::kFilmStepFrame + sh::kFilmOpenFrame,
                            [filmSetup](CaseContext& c, Args& a) {
                                filmSetup(c, a);
                                if (!c.sample && Chance(*c.rng, 12)) a.a[1] = 0;
                            },
                            [](G& g, K& k, const Args& a, uint32_t sp, uint32_t* r) {
                                return sh::FilmGate(g, k, a.a[0], a.a[1], sp, r);
                            },
                            FilmSeams()));
    rows.back().allowBiosCalls = true; // sprintf reaches strlen through the A0 vector

    // ------------------------------------------------------------------ RASHCDF 0x8006E4D8
    rows.push_back(PanelRow("shell_widget_film", "s32 FilmWidget(Screen*, Widget*)  [RASHCDF 0x8006E4D8, types 2..6]",
                            0x8006E4D8u, sh::kFilmWidgetFrame + sh::kFilmGateFrame + sh::kFilmStepFrame + sh::kFilmOpenFrame,
                            [](CaseContext& c, Args& a) {
                                PlantShell(c);
                                Stubs(c);
                                const auto sw = PickWidget(c, {2, 3, 4, 5, 6});
                                FilmState(c, sw.first);
                                PanelContext(c, sw.first);
                                if (!c.sample && Chance(*c.rng, 3)) Put16(*c.mem, sw.second + 8u, 2u + Pick(*c.rng, 5));
                                if (c.sample) Put16(*c.mem, sh::kFeMedia, 0u);
                                const uint32_t own = sw.second + 0x10u;
                                if (c.mem->PeekWord(own) >= 42u) c.mem->PokeWord(own, 21u);
                                a.a[0] = sw.first;
                                a.a[1] = sw.second;
                            },
                            [](G& g, K& k, const Args& a, uint32_t sp, uint32_t* r) {
                                return sh::FilmWidget(g, k, a.a[0], a.a[1], sp, r);
                            },
                            FilmSeams()));
    rows.back().allowBiosCalls = true; // sprintf reaches strlen through the A0 vector

    // ------------------------------------------------------------------ RASHCDF 0x80075BD0 / 0x8006E8FC
    rows.push_back(PanelRow("shell_pad_diagram", "s32 PadDiagram(Screen*, Widget*, s16 xy[2])  [RASHCDF 0x80075BD0]",
                            0x80075BD0u, 48u,
                            [](CaseContext& c, Args& a) {
                                PlantShell(c);
                                PlainSprites(c);
                                const auto sw = PickWidget(c, {8});
                                DrawState(c, sw.first);
                                PanelContext(c, sw.first);
                                a.a[0] = sw.first;
                                a.a[1] = sw.second;
                                a.a[2] = sw.second + 0x10u;
                            },
                            [](G& g, K& k, const Args& a, uint32_t sp, uint32_t* r) {
                                return sh::PadDiagram(g, k, a.a[0], a.a[2], sp, r);
                            },
                            EmitSeams()));
    rows.back().allowBiosCalls = true;
    rows.push_back(PanelRow("shell_widget_logo", "s32 LogoWidget(Screen*, Widget*)  [RASHCDF 0x8006E8FC, type 8]", 0x8006E8FCu,
                            72u + 48u,
                            [](CaseContext& c, Args& a) {
                                PlantShell(c);
                                PlainSprites(c);
                                const auto sw = PickWidget(c, {8});
                                DrawState(c, sw.first);
                                PanelContext(c, sw.first);
                                if (!c.sample && Chance(*c.rng, 3)) Put16(*c.mem, sw.second + 0x14u, 0x2Eu + Pick(*c.rng, 9));
                                if (!c.sample && Chance(*c.rng, 4)) Put16(*c.mem, sw.second + 0x16u, Get16(*c.mem, sw.second + 0x16u) ^ 0x1000u);
                                if (!c.sample && Chance(*c.rng, 4)) Put16(*c.mem, sh::kFeMedia, Pick(*c.rng, 8));
                                a.a[0] = sw.first;
                                a.a[1] = sw.second;
                            },
                            [](G& g, K& k, const Args& a, uint32_t sp, uint32_t* r) {
                                return sh::LogoWidget(g, k, a.a[0], a.a[1], sp, r);
                            },
                            EmitSeams()));
    rows.back().allowBiosCalls = true;
}
