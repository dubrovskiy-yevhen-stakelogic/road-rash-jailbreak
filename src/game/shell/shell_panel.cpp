// The films in the panels and the logos (shell_panel.h), PORTED from RASHCDF.BIN - see the header.
#include "game/shell/shell_panel.h"

#include "game/shell/shell_text.h"

#include <initializer_list>

namespace rr::shell {

namespace {

uint32_t U(int32_t v) { return static_cast<uint32_t>(v); }
uint32_t S16(GuestRam& g, uint32_t a) { return U(g.S16(a)); } // lh: the sign-extended halfword

bool CallK(ShellCallees& k, uint32_t address, std::initializer_list<uint32_t> args, uint32_t* v0 = nullptr) {
    uint32_t a[12] = {};
    int n = 0;
    for (uint32_t v : args)
        if (n < 12) a[n++] = v;
    return k.Call(address, a, n, v0);
}

// The highlighted item of a screen: items + 0x78 * (s16)+0x04 (the original computes it for any index).
uint32_t Highlighted(GuestRam& g, uint32_t s) { return g.U32(s + 0x10u) + U(g.S16(s + 4u) * 0x78); }

// The mode's film (0x8006E614..0x8006E6BC, the jump table 0x8005C00C): the trophy room's for action group
// 2, else by session+0x00: 1 / 0x11 Five-O, 4 Time Trial, 8 / 0x18 Side Car, 0x10 Head-to-Head, 0x20
// Jail Break; any other mode has none (0).
uint32_t ModeFilm(GuestRam& g, uint32_t s, uint32_t rec) {
    if (ActionGroup(g, Highlighted(g, s)) == 2) {
        g.W32(rec, 19);
        return rec;
    }
    uint32_t name = 0;
    switch (g.U32(kSession)) {
    case 1: case 0x11: name = 13; break;
    case 4: name = 14; break;
    case 8: case 0x18: name = 17; break;
    case 0x10: name = 15; break;
    case 0x20: name = 12; break;
    default: return 0;
    }
    g.W32(rec, name);
    return rec;
}

} // namespace

// ------------------------------------------------------------------------ RASHCDF 0x80064154 / 0x800641D4
// The bound chooser fe+0x94's current option (options +4, index (s8)+3, 12 bytes each): its display list
// (+8, (s16)+6 entries of 28 bytes). 0x80064154 finds the entry whose u16 +2 is `kind`, 0x800641D4 the
// one whose u16 +0 is 4; each answers the entry + 4, or 0.
uint32_t OptionRecord(GuestRam& g, uint32_t kind) {
    const uint32_t c = g.U32(kFeChooser);
    if (c == 0 || g.U32(c + 4u) == 0) return 0;
    const uint32_t opt = g.U32(c + 4u) + U(g.S8(c + 3u) * 12);
    const int32_t n = g.S16(opt + 6u);
    uint32_t e = g.U32(opt + 8u);
    for (int32_t i = 0; i < n; ++i, e += 28u)
        if (g.U16(e + 2u) == (kind & 0xFFFFu)) return e + 4u;
    return 0;
}

uint32_t OptionFilm(GuestRam& g) {
    const uint32_t c = g.U32(kFeChooser);
    if (c == 0 || g.U32(c + 4u) == 0) return 0;
    const uint32_t opt = g.U32(c + 4u) + U(g.S8(c + 3u) * 12);
    const int32_t n = g.S16(opt + 6u);
    uint32_t e = g.U32(opt + 8u);
    for (int32_t i = 0; i < n; ++i, e += 28u)
        if (g.U16(e) == 4u) return e + 4u;
    return 0;
}

// ------------------------------------------------------------------------ RASHCDF 0x8006FE6C
bool FilmClose(GuestRam& g, ShellCallees& k, uint32_t* v0) {
    *v0 = 1;
    if (!(g.U16(kFeMedia) & 1u)) return true;
    if (!CallK(k, kFilmStop, {S16(g, kFilmChannel)})) return false;
    if (!CallK(k, kFileClose, {g.U32(kFilmHandle)})) return false;
    const uint16_t m = g.U16(kFeMedia);
    g.W32(kFilmHandle, 0xFFFFFFFFu);
    g.W16(kFeMedia, static_cast<uint16_t>(m & 0xFFF8u));
    return true;
}

// ------------------------------------------------------------------------ RASHCDF 0x8006FEF4
// Record: +0 the name index, +4 the library's mode word, +8 (u16) the sound channel, +0x0A the start's
// last argument, +0x0C / +0x0E x / y, +0x10 the picture count.
bool FilmOpen(GuestRam& g, ShellCallees& k, uint32_t rec, uint32_t sp, uint32_t* v0) {
    *v0 = 0;
    uint32_t busy = 0;
    if (!CallK(k, kSplashFade, {0}, &busy)) return false;
    if (busy != 0 || (g.U16(kFeMedia) & 1u)) return true;
    const uint32_t path = sp - kFilmOpenFrame + 40u;
    if (!CallK(k, kSprintf, {path, kFilmPathFmt, g.U32(kFilmNames + 4u * g.U32(rec))})) return false;
    uint32_t handle = 0;
    if (!CallK(k, kFileOpen, {path, 0}, &handle)) return false;
    g.W32(kFilmHandle, handle);
    *v0 = 1;
    if (static_cast<int32_t>(handle) < 0) return true;
    g.W32(rec + 0x10u, 0);
    g.W16(kFeMedia, static_cast<uint16_t>(g.U16(kFeMedia) | 1u));
    const uint16_t channel = g.U16(rec + 8u);
    g.W16(kFilmChannel, channel);
    uint32_t started = 0;
    if (!CallK(k, kFilmStart,
               {g.U32(kFilmHandle), g.U32(rec + 4u), U(static_cast<int16_t>(channel)), S16(g, rec + 0x0Cu),
                S16(g, rec + 0x0Eu), 0x800u, 0x40u, 0x400u, g.U16(rec + 0x0Au)},
               &started))
        return false;
    if (started == 0) {
        uint32_t r = 0;
        if (!FilmClose(g, k, &r)) return false;
        *v0 = 0;
        return true;
    }
    g.W32(rec + 0x10u, g.U32(rec + 0x10u) + 1u);
    return true;
}

// ------------------------------------------------------------------------ RASHCDF 0x80070018
bool FilmStep(GuestRam& g, ShellCallees& k, uint32_t rec, uint32_t sp, uint32_t* v0) {
    const uint16_t m = g.U16(kFeMedia);
    if (m & 4u) return FilmClose(g, k, v0);
    if (!(m & 1u)) {
        if (m & 2u) return FilmOpen(g, k, rec, sp - kFilmStepFrame, v0);
        g.W16(kFeMedia, static_cast<uint16_t>(m | 2u));
        *v0 = 0;
        return true;
    }
    uint32_t more = 0;
    if (!CallK(k, kFilmPicture, {S16(g, rec + 0x0Cu), S16(g, rec + 0x0Eu), S16(g, kFilmChannel), g.U32(rec + 4u),
                                 g.U32(rec + 0x10u)},
               &more))
        return false;
    *v0 = 1;
    if (more == 0) {
        *v0 = 0;
        g.W16(kFeMedia, static_cast<uint16_t>((g.U16(kFeMedia) & 0xFFFEu) | 2u));
    }
    g.W32(rec + 0x10u, g.U32(rec + 0x10u) + 1u);
    return true;
}

// ------------------------------------------------------------------------ RASHCDF 0x8006E6F4
bool FilmGate(GuestRam& g, ShellCallees& k, uint32_t s, uint32_t rec, uint32_t sp, uint32_t* v0) {
    *v0 = 1;
    if (rec == 0) return true;
    const uint32_t inner = sp - kFilmGateFrame;
    if (!(g.U16(s) & 2u) || g.S32(kIdle) < kFilmIdle) return FilmClose(g, k, v0);
    uint32_t r = 0;
    if (g.U16(kFeMedia) & 1u) return FilmStep(g, k, rec, inner, &r);
    if (g.S16(s + 6u) == g.S16(kFeCur)) return FilmOpen(g, k, rec, inner, &r);
    return true;
}

// ------------------------------------------------------------------------ RASHCDF 0x8006E4D8
bool FilmWidget(GuestRam& g, ShellCallees& k, uint32_t s, uint32_t w, uint32_t sp, uint32_t* v0) {
    *v0 = 1;
    const uint32_t own = w + 0x10u;
    uint32_t rec = 0;
    switch (static_cast<int16_t>(g.U16(w + 8u) - 2u)) {
    case 0: rec = own; break;
    case 1:
        rec = own;
        g.W32(own, ActionGroup(g, Highlighted(g, s)) == 3 ? 20u : 11u);
        break;
    case 2:
        rec = OptionFilm(g);
        if (rec == 0) return true;
        g.W16(rec + 0x0Cu, g.U16(own + 0x0Cu));
        g.W16(rec + 0x0Eu, g.U16(own + 0x0Eu));
        break;
    case 3: rec = ModeFilm(g, s, own); break;
    case 4: {
        rec = OptionFilm(g);
        if (rec != 0) {
            g.W16(rec + 0x0Cu, g.U16(own + 0x0Cu));
            g.W16(rec + 0x0Eu, g.U16(own + 0x0Eu));
            break;
        }
        const int32_t group = ActionGroup(g, Highlighted(g, s));
        if (group == 6 || group == 4 || group == 5 || group == 7) break;
        rec = ModeFilm(g, s, own);
        break;
    }
    default: break;
    }
    if (rec == 0) return true;
    return FilmGate(g, k, s, rec, sp - kFilmWidgetFrame, v0);
}

// ------------------------------------------------------------------------ RASHCDF 0x80075BD0
// The controller page's picture: the diagram of the edited port's layout (player[fe+0x16]+0x08: DG01,
// DG02, DG03, AN01, AN02; any other value DG01) while the highlighted item's action group is 8 or -1,
// else BAC1 - a local sprite record {FourCC, 0x808080, x, y} at sp - 32, emitted into the backdrop table
// 0x8009CCE4 + layer * 4 + 0x1C. v0 is the emitter's.
bool PadDiagram(GuestRam& g, ShellCallees& k, uint32_t s, uint32_t xy, uint32_t sp, uint32_t* v0) {
    const uint32_t local = sp - 48u + 16u;
    g.W16(local + 8u, g.U16(xy));
    g.W32(local + 4u, 0x808080u);
    g.W16(local + 10u, g.U16(xy + 2u));
    const uint32_t ot = g.U32(0x8009CCE4u) + U(g.S8(s + 0x0Cu) * 4) + 0x1Cu;
    const int32_t group = ActionGroup(g, Highlighted(g, s));
    uint32_t id = 0x31434142u; // BAC1
    if (group == 8 || group == -1) {
        static const uint32_t kLayouts[5] = {0x31304744u, 0x32304744u, 0x33304744u, 0x31304E41u, 0x32304E41u};
        const int32_t layout = g.S8(kPlayers + U(g.S8(kFePort) * 36) + 8u);
        id = static_cast<uint32_t>(layout) < 5u ? kLayouts[layout] : kLayouts[0];
    }
    g.W32(local, id);
    return CallK(k, kSpriteEmit, {s, local, ot}, v0);
}

// ------------------------------------------------------------------------ RASHCDF 0x8006E8FC
bool LogoWidget(GuestRam& g, ShellCallees& k, uint32_t s, uint32_t w, uint32_t sp, uint32_t* v0) {
    *v0 = 1;
    if ((g.U16(w + 10u) & 0x20u) && !(g.U16(s) & 2u)) return true;
    const uint32_t f = w + 0x10u; // {s16 x, s16 y, u16 kind, u16 flags}
    const uint32_t local = sp - 72u + 16u;
    auto fill = [&](uint32_t id) {
        g.W16(local + 8u, g.U16(f));
        g.W32(local + 4u, 0x808080u);
        g.W16(local + 10u, g.U16(f + 2u));
        g.W32(local, id);
        return local;
    };
    uint32_t rec = 0;
    switch (g.U16(f + 4u)) {
    case 0x30: { // the option's picture: the chooser of the highlighted item bound for the lookup only
        const uint32_t chooser = g.U32(kFeChooser), opt = g.U32(kFeOpt);
        const uint8_t valid = g.U8(kFeValid);
        BindChooser(g, s, Highlighted(g, s));
        rec = OptionRecord(g, g.U16(f + 4u));
        if (rec != 0) {
            g.W16(rec + 8u, g.U16(f));
            g.W16(rec + 10u, g.U16(f + 2u));
        }
        g.W32(kFeChooser, chooser);
        g.W32(kFeOpt, opt);
        g.W8(kFeValid, valid);
        break;
    }
    case 0x31: { // the mode's logo
        if ((g.U16(f + 6u) & 0x1000u) && !(g.U16(Highlighted(g, s) + 10u) & 0x100u)) return true;
        fill(0);
        if (ActionGroup(g, Highlighted(g, s)) == 2) {
            rec = fill(0x474C5954u); // GLYT
            break;
        }
        switch (g.U32(kSession)) {
        case 1: case 0x11: rec = fill(0x474C4F35u); break; // GLO5
        case 4: rec = fill(0x474C5454u); break;             // GLTT
        case 8: case 0x18: rec = fill(0x474C4353u); break;  // GLCS
        case 0x10: rec = fill(0x474C4848u); break;          // GLHH
        case 0x20:
            switch (g.S8(kSession + 4u)) {
            case 1: rec = fill(0x504C5447u); break; // PLTG
            case 3: rec = fill(0x504C5444u); break; // PLTD
            case 5: rec = fill(0x504C424Au); break; // PLBJ
            default: rec = fill(0x474C424Au); break; // GLBJ
            }
            break;
        default: rec = 0; break;
        }
        break;
    }
    case 0x32: return PadDiagram(g, k, s, f, sp - 72u, v0);
    case 0x33: rec = fill(ActionGroup(g, Highlighted(g, s)) == 3 ? 0x474C4E43u : 0x474C5252u); break; // GLNC / GLRR
    case 0x34:
    case 0x35: {
        const uint32_t id = g.U32(kFe + (g.U16(f + 4u) == 0x34u ? 0xACu : 0xB0u));
        if (id != 300u) rec = fill(id);
        break;
    }
    default: break;
    }
    if (rec == 0) return true;
    return CallK(k, kSpriteEmit, {s, rec, g.U32(kOtTable) + U(g.S8(kFeLayer) * 4) + 0x18u}, v0);
}

} // namespace rr::shell
