// The front end's logic, ported from RASHCDF.BIN / SLUS_010.53 (hashes and reading in shell_logic.h).
// Each function below is transcribed from our own disassembly of the player's image; the comment on
// each names the instructions a reader should check it against. Accepted only by the bench rows of
// tools\rrverify\rows_shell.inc.
#include "game/shell/shell_logic.h"

#include "game/shell/shell_screens.h"

namespace rr::shell {

namespace {

uint32_t Table(GuestRam& g, int32_t id) { return g.U32(g.U32(kScreenTablePtr) + 4u * static_cast<uint32_t>(id)); }
uint32_t GameState(GuestRam& g) { return g.U32(kGameStatePtr); }

// The navigation gate of a widget's condition word +0x00, exactly the test
// MoveNext 0x8006C460..0x8006C4FC and MovePrev make, with the mode word read once by the caller.
bool Selectable(GuestRam& g, uint32_t widget, uint32_t mode) {
    const uint16_t type = g.U16(widget + 8u);
    if (!(static_cast<uint32_t>(type) - 12u < 2u) && static_cast<int16_t>(type) != 17) return false;
    const uint32_t c = g.U32(widget);
    if ((c & 0x40000000u) && (g.U8(kMultitapByte) >> 4) != 8) return false;
    if (c & 0x02000000u) {
        const int32_t port = g.S8(kFePort);
        if (g.U32(kPorts + 24u * static_cast<uint32_t>(port) + 4u) != 1u) return false;
    }
    if ((c & 0x10000000u) && g.S8(kSession + 0x1Bu) == 0) return false;
    if ((c & 0x08000000u) && mode != 4u) return false;
    if ((c & 0x04000000u) && mode == 4u) return false;
    return true;
}

bool UiSound(ShellCallees& k, uint32_t n) { return k.Call1(kUiSound, n); }

// The modal branch shared by both cancel arms (0x8006B0FC..0x8006B1C4 and 0x8006B53C..0x8006B604):
// the abort modal takes over the screen and remembers where cancel would have gone.
void AbortModal(GuestRam& g, uint32_t screen, uint32_t backEntry) {
    const bool timeTrial = g.U8(GameState(g) + 4u) == 4u;
    const int32_t modal = timeTrial ? 55 : 54;
    g.W16(kFeNext, static_cast<uint16_t>(modal));
    g.W8(kFeDeferred, g.U8(backEntry));
    const uint16_t caller = g.U16(kFeCur);
    const uint32_t m = Table(g, modal);
    g.W16(m + 10u, caller);
    g.W8(Table(g, modal) + 14u, g.U8(screen + 14u));
    g.W8(Table(g, modal) + 15u, g.U8(screen + 15u));
    g.W8(kFeEdges, static_cast<uint8_t>(g.U8(kFeEdges) | 4u));
}

} // namespace

// ---------------------------------------------------------------------------- SLUS leaves

// SLUS 0x8001FC58: seed = seed * 0x19660D + 0x3C6EF35F at gp+2076; returns the new seed.
uint32_t Rand(GuestRam& g) {
    const uint32_t s = g.U32(g.gp() + 2076u) * 0x0019660Du + 0x3C6EF35Fu;
    g.W32(g.gp() + 2076u, s);
    return s;
}

// SLUS 0x8002D250: clears bits 0..64 of session+0xF0 one at a time.
void ClearProgress(GuestRam& g) {
    for (uint32_t i = 0; i < 65u; ++i) {
        const uint32_t a = kProgress + (i >> 3);
        g.W8(a, static_cast<uint8_t>(g.U8(a) & ~(1u << (i & 7u))));
    }
}

// SLUS 0x8002D298: the same for bits 0..17 of session+0xFC.
void ClearMissions(GuestRam& g) {
    for (uint32_t i = 0; i < 18u; ++i) {
        const uint32_t a = kMissions + (i >> 3);
        g.W8(a, static_cast<uint8_t>(g.U8(a) & ~(1u << (i & 7u))));
    }
}

// SLUS 0x8001E100: word stores of the value replicated into all four bytes, 4 bytes a step until
// the count reaches zero (the original is only ever called with multiples of 4).
void MemSet(GuestRam& g, uint32_t dst, uint8_t value, uint32_t n) {
    const uint32_t v = value | (static_cast<uint32_t>(value) << 8) | (static_cast<uint32_t>(value) << 16) |
                       (static_cast<uint32_t>(value) << 24);
    for (uint32_t a = dst; n != 0; n -= 4u, a += 4u) g.W32(a, v);
}

// SLUS 0x8001C4A8: record+0xB8 = 0x800526A4 + 60*index, record+0xB4 = index + 1.
void PadConfig(GuestRam& g, uint32_t record, int32_t index) {
    g.W32(record + 0xB8u, 0x800526A4u + 60u * static_cast<uint32_t>(index));
    g.W32(record + 0xB4u, static_cast<uint32_t>(index + 1));
}

// ---------------------------------------------------------------------------- the menu

// RASHCDF 0x8006C700: the first type 12/13 widget whose +0x12 equals `code`, or -1.
int32_t FindByCode(GuestRam& g, uint32_t screen, int32_t code) {
    const int32_t count = g.S16(screen + 8u);
    uint32_t w = g.U32(screen + 16u);
    for (int32_t i = 0; i < count; ++i, w += 120u) {
        const int16_t t = g.S16(w + 8u);
        if (t < 14 && t > 11 && static_cast<int32_t>(g.U16(w + 18u)) == code) return static_cast<int16_t>(i);
    }
    return -1;
}

// RASHCDF 0x8006C3A4 (asm read above in shell_logic.h's reading): wraps, skips what is not selectable.
bool MoveNext(GuestRam& g, ShellCallees& k, uint32_t screen) {
    const int16_t cur0 = g.S16(screen + 4u);
    int32_t i = cur0 + 1;
    if (cur0 == 32767) {
        g.W16(screen + 4u, 0);
        i = 0;
    } else if (!(i < g.S16(screen + 8u))) {
        i = 0;
    }
    const int32_t start = g.S16(screen + 4u);
    uint32_t sound = 4;
    if (i != start) {
        const uint32_t mode = g.U32(kSession);
        do {
            const uint32_t w = g.U32(screen + 16u) + 120u * static_cast<uint32_t>(i);
            if (Selectable(g, w, mode)) break;
            ++i;
            if (!(i < g.S16(screen + 8u))) i = 0;
        } while (i != start);
        sound = (i != g.S16(screen + 4u)) ? 0u : 4u;
    }
    if (!UiSound(k, sound)) return false;
    g.W16(screen + 4u, static_cast<uint16_t>(i));
    return true;
}

// RASHCDF 0x8006C558: the mirror image, walking backwards.
bool MovePrev(GuestRam& g, ShellCallees& k, uint32_t screen) {
    int32_t cur = g.S16(screen + 4u);
    if (cur == 32767) {
        g.W16(screen + 4u, 0);
        cur = g.S16(screen + 4u);
    }
    int32_t i = cur - 1;
    if (i < 0) i = g.S16(screen + 8u) - 1;
    uint32_t sound = 4;
    if (i != cur) {
        const uint32_t mode = g.U32(kSession);
        do {
            const uint32_t w = g.U32(screen + 16u) + 120u * static_cast<uint32_t>(i);
            if (Selectable(g, w, mode)) break;
            --i;
            if (i < 0) i = g.S16(screen + 8u) - 1;
        } while (i != cur);
        sound = (i != g.S16(screen + 4u)) ? 0u : 4u;
    }
    if (!UiSound(k, sound)) return false;
    g.W16(screen + 4u, static_cast<uint16_t>(i));
    return true;
}

// RASHCDF 0x80064B30: binds a type-17 widget's slider descriptor and its value slot session+0xD4.
void BindSlider(GuestRam& g, uint32_t /*screen*/, uint32_t widget) {
    g.W32(kFeSlider, 0);
    g.W32(kFeSliderDst, 0);
    if (widget == 0 || g.S16(widget + 8u) != 17 || !(g.U16(widget + 100u) < 6u)) return;
    const uint32_t slot = g.U8(widget + 0x66u);
    const uint32_t desc = kSliderDescs + 16u * slot;
    const uint32_t dst = 0x800D81ACu + 4u * slot;
    g.W32(kFeSlider, desc);
    g.W32(kFeSliderDst, dst);
    g.W32(desc + 12u, g.U32(dst));
}

// RASHCDF 0x8006B03C - the whole menu (read against the asm at 0x8006B0A4..0x8006B784).
bool MenuInput(GuestRam& g, ShellCallees& k, uint32_t screen, int32_t padFirst, int32_t padEnd, int32_t* result) {
    int32_t r = 0;
    *result = 0;
    if (g.U32(screen + 16u) == 0) return true;

    // Phase 1 - cancel, over pads 0 and 1.
    for (uint32_t p = 0; p < 2u && r == 0; ++p) {
        if (!(g.S8(PadSlot(p, kTriangle)) > 0)) continue;
        r = 2;
        const uint32_t back = kNavBack + 4u * static_cast<uint32_t>(g.S16(kFeCur));
        if (g.S32(back) == -1) {
            if (!UiSound(k, 4)) return false;
            continue;
        }
        if ((g.U16(screen) & 0x400u) && g.S8(kSession + 0x18u) != 0) {
            AbortModal(g, screen, back);
            r = 1;
            if (!UiSound(k, 3)) return false;
            continue;
        }
        g.W16(kFeNext, g.U16(back));
        if (!UiSound(k, 3)) return false;
        r = 1;
        g.W8(kFeEdges, static_cast<uint8_t>(g.U8(kFeEdges) | 4u));
    }

    // Phase 2 - the screen's own pad window.
    for (int32_t p = padFirst; p < padEnd && r == 0; ++p) {
        const uint32_t pad = static_cast<uint32_t>(p);
        if (g.S8(PadSlot(pad, kUp)) != 0) {
            r = 2;
            g.W8(kFeEdges, static_cast<uint8_t>(g.U8(kFeEdges) | 0x10u));
            if (!MovePrev(g, k, screen)) return false;
        } else if (g.S8(PadSlot(pad, kDown)) != 0) {
            r = 2;
            g.W8(kFeEdges, static_cast<uint8_t>(g.U8(kFeEdges) | 0x20u));
            if (!MoveNext(g, k, screen)) return false;
        } else if (g.S8(PadSlot(pad, kLeft)) != 0 || g.S8(PadSlot(pad, kRight)) != 0) {
            const bool down = g.S8(PadSlot(pad, kLeft)) != 0;
            r = 2;
            g.W8(kFeEdges, static_cast<uint8_t>(g.U8(kFeEdges) | (down ? 0x40u : 0x80u)));
            const uint32_t d = g.U32(kFeSlider);
            if (d == 0) continue;
            const int32_t step = g.S16(d + 8u);
            g.W32(d + 12u, static_cast<uint32_t>(g.S32(d + 12u) + (down ? -step : step)));
            const uint32_t d2 = g.U32(kFeSlider);
            const int32_t v = g.S32(d2 + 12u);
            if (down ? (v < g.S32(d2)) : (g.S32(d2 + 4u) < v)) g.W32(d2 + 12u, g.U32(d2 + (down ? 0u : 4u)));
            const uint32_t dst = g.U32(kFeSliderDst);
            if (dst != 0) g.W32(dst, g.U32(g.U32(kFeSlider) + 12u));
        } else if (g.S8(PadSlot(pad, kCross)) > 0) {
            r = 2;
            g.W8(kFeEdges, static_cast<uint8_t>(g.U8(kFeEdges) | 1u));
            const uint32_t adv = kNavAdvance + 4u * static_cast<uint32_t>(g.S16(kFeCur));
            uint32_t sound = 2;
            if (g.S32(adv) != -1) {
                g.W16(kFeNext, g.U16(adv));
                r = 1;
            } else {
                const uint32_t w = g.U32(screen + 16u) + 120u * static_cast<uint32_t>(g.S16(screen + 4u));
                const int16_t t = g.S16(w + 8u);
                if (t < 12) {
                    sound = 4;
                } else if (t < 14) {
                    const uint32_t code = g.U16(w + 18u);
                    if (g.S32(kCodeScreen + 4u * code) != -1) {
                        g.W16(kFeNext, g.U16(kCodeScreen + 4u * code));
                        r = 1;
                    } else if (g.S32(kCodeGroup + 4u * code) == -1) {
                        sound = 4;
                    } else {
                        g.W16(screen + 4u, static_cast<uint16_t>(FindByCode(g, screen, g.S16(kCodeGroup + 4u * code))));
                    }
                } else if (t == 17) {
                    const uint32_t e = kSliderCommit + 4u * g.U16(w + 100u);
                    if (g.S32(e) == -1) sound = 4;
                    else g.W16(screen + 4u, static_cast<uint16_t>(FindByCode(g, screen, g.S16(e))));
                } else {
                    sound = 4;
                }
            }
            if (!UiSound(k, sound)) return false;
        } else if (g.S8(PadSlot(pad, kTriangle)) > 0) {
            r = 2;
            const uint32_t back = kNavBack + 4u * static_cast<uint32_t>(g.S16(kFeCur));
            if (g.S32(back) == -1) {
                if (!UiSound(k, 4)) return false;
            } else if ((g.U16(screen) & 0x400u) && g.S8(kSession + 0x18u) != 0) {
                AbortModal(g, screen, back);
                r = 1;
                if (!UiSound(k, 3)) return false;
            } else {
                g.W16(kFeNext, g.U16(back));
                if (!UiSound(k, 3)) return false;
                r = 1;
                g.W8(kFeEdges, static_cast<uint8_t>(g.U8(kFeEdges) | 4u));
            }
        } else if (g.S8(PadSlot(pad, kSquare)) > 0) {
            r = 2;
            if ((g.U16(screen) & 0x100u) == 0) {
                const uint8_t cur = g.U8(kFeCur);
                g.W8(kSession + 0x10u, cur);
                const uint8_t sel = g.U8(Table(g, static_cast<int8_t>(cur)) + 4u);
                g.W16(kFeNext, 42);
                if (!UiSound(k, 2)) return false;
                g.W8(kSession + 0x0Du, sel);
                r = 1;
                g.W8(kFeEdges, static_cast<uint8_t>(g.U8(kFeEdges) | 8u));
            } else if (g.S8(kFeMusicRestart) == 0) {
                if (!UiSound(k, 4)) return false;
            } else if (g.S8(kFeMusicOk) != 0) {
                if (!k.Call1(kMusicPlay, g.U8(kFeTrack))) return false;
                g.W8(kFeMusicOn, 1);
            }
        } else if (g.S8(PadSlot(pad, kCircle)) > 0) {
            r = 2;
            if (g.U16(screen) & 0x200u) {
                if (!UiSound(k, 4)) return false;
            } else {
                if (!UiSound(k, 2)) return false;
                g.W16(kFeNext, 53);
                g.W8(kFeEdges, static_cast<uint8_t>(g.U8(kFeEdges) | 2u));
                g.W16(Table(g, 53) + 10u, g.U16(screen + 6u));
                g.W8(Table(g, 53) + 14u, g.U8(screen + 14u));
                g.W8(Table(g, 53) + 15u, g.U8(screen + 15u));
            }
        }
    }
    *result = r;
    return true;
}

// RASHCDF 0x80069418: MenuInput(s, s->f0E, s->f0E + s->f0F), both bytes read signed.
bool DefaultScreenInput(GuestRam& g, ShellCallees& k, uint32_t screen, int32_t* result) {
    const int32_t first = g.S8(screen + 14u);
    return MenuInput(g, k, screen, first, first + g.S8(screen + 15u), result);
}

// RASHCDF 0x8006C354 - THE HANDOVER: game_state+0x00 = 3 unless the
// loading picture still holds the screen.
int32_t StartRaceInput(GuestRam& g, uint32_t /*screen*/) {
    g.W32(kIdle, 0);
    const uint16_t media = g.U16(kFeMedia);
    if (media & 1u) {
        g.W16(kFeMedia, static_cast<uint16_t>(media | 4u));
        g.W8(kFeHold, 1);
    } else {
        g.W8(kFeHold, 0);
        g.W8(GameState(g), 3);
    }
    return 0;
}

// RASHCDF 0x800685BC: the mode word follows the highlighted button (rules.md 2.1).
void SetMode(GuestRam& g, uint32_t widget) {
    if (widget == 0) return;
    const int16_t t = g.S16(widget + 8u);
    if (!(t < 14) || !(t > 11)) return;
    uint32_t mode = 0;
    switch (g.U16(widget + 18u)) {
    case 2: case 3: case 5: mode = 0x20; break;
    case 6: mode = 1; break;
    case 7: case 0x1F: mode = 4; break;
    case 0x1B: mode = 0x10; break;
    case 0x1C: mode = 0x11; break;
    case 0x1D: mode = 8; break;
    case 0x1E: mode = 0x18; break;
    default: return;
    }
    g.W32(kSession, mode);
}

// RASHCDF 0x80080A70: the index of the first widget flagged 0x1000 (the item a screen starts on), else 0.
int32_t FirstItem(GuestRam& g, uint32_t screen) {
    uint32_t w = g.U32(screen + 16u);
    if (w == 0) return 0;
    const int32_t count = g.S16(screen + 8u);
    for (int32_t i = 0; i < count; ++i, w += 120u)
        if (g.U16(w + 10u) & 0x1000u) return static_cast<int16_t>(i);
    return 0;
}

// ---------------------------------------------------------------------------- choosers

// The gate every chooser walk applies to one option (0x80064680..0x80064788 and its copies).
bool OptionAvailable(GuestRam& g, uint32_t o) {
    if ((g.U8(o + 3u) & g.U8(kFeOptMask)) == 0) return false;
    const uint8_t gate = g.U8(o);
    const uint32_t bit = g.U8(o + 1u);
    const int32_t byteIndex = static_cast<int32_t>(static_cast<int8_t>(bit)) >> 3;
    if (gate == 4 && ((g.U8(kProgress + static_cast<uint32_t>(byteIndex)) >> (bit & 7u)) & 1u)) return false;
    if (gate == 8 && ((g.U8(kMissions + static_cast<uint32_t>(byteIndex)) >> (bit & 7u)) & 1u)) return false;
    const uint8_t flags = g.U8(o + 2u);
    const uint32_t port = static_cast<uint32_t>(g.S8(kFePort));
    const uint32_t padLive = g.U32(kPads + 0x10u + 192u * port);
    if ((flags & 0x40u) && padLive == 0) return false;
    if ((flags & 0x20u) && padLive != 0) return false;
    if ((flags & 0x80u) && (g.U8(kMultitapByte) >> 4) != 8) return false;
    return true;
}

// RASHCDF 0x800630C0: select the first option whose value byte equals `value`.
void ChooserSet(GuestRam& g, uint32_t c, int32_t value) {
    const int32_t n = g.S8(c);
    uint32_t o = g.U32(c + 4u);
    for (int32_t i = 0; i < n; ++i, o += 12u)
        if (g.S8(o + 1u) == value) {
            g.W8(c + 3u, static_cast<uint8_t>(i));
            return;
        }
}

// RASHCDF 0x80062D6C: from the current option on, the first "sticky" (flags 0x10) option that is
// available, else the first available one; -1 when none is.
int32_t ChooserFirstValid(GuestRam& g, uint32_t c) {
    const int32_t n = g.S8(c);
    int32_t result = -1;
    bool sticky = false;
    int32_t i = g.S8(c + 3u);
    for (int32_t seen = 0; seen < n; ++seen) {
        const uint32_t o = g.U32(c + 4u) + 12u * static_cast<uint32_t>(i);
        if ((g.U8(o + 2u) & 0x10u) || sticky) {
            sticky = true;
            if (OptionAvailable(g, o)) {
                result = i;
                break;
            }
        }
        ++i;
        if (!(i < n)) i = 0;
    }
    if (result != -1) return result;
    i = g.S8(c + 3u);
    for (int32_t seen = 0; seen < n; ++seen) {
        const uint32_t o = g.U32(c + 4u) + 12u * static_cast<uint32_t>(i);
        if (OptionAvailable(g, o)) return i;
        ++i;
        if (!(i < n)) i = 0;
    }
    return -1;
}

// RASHCDF 0x800649BC: how many options are available.
int32_t ChooserCount(GuestRam& g, uint32_t c) {
    const int32_t n = g.S8(c);
    int32_t count = 0;
    for (int32_t i = 0; i < n; ++i)
        if (OptionAvailable(g, g.U32(c + 4u) + 12u * static_cast<uint32_t>(i))) ++count;
    return count;
}

// RASHCDF 0x8006460C / 0x800647E4: the previous / next available option, wrapping; sound 1 on a
// move, 4 when nothing else is available.
bool ChooserPrev(GuestRam& g, ShellCallees& k, uint32_t c) {
    const int32_t cur = g.S8(c + 3u);
    int32_t i = cur - 1;
    if (i < 0) i = g.S8(c) - 1;
    uint32_t sound = 4;
    if (i != cur) {
        do {
            if (OptionAvailable(g, g.U32(c + 4u) + 12u * static_cast<uint32_t>(i))) break;
            --i;
            if (i < 0) i = g.S8(c) - 1;
        } while (i != cur);
        sound = (i != g.S8(c + 3u)) ? 1u : 4u;
    }
    if (!UiSound(k, sound)) return false;
    g.W8(c + 3u, static_cast<uint8_t>(i));
    return true;
}

bool ChooserNext(GuestRam& g, ShellCallees& k, uint32_t c) {
    const int32_t cur = g.S8(c + 3u);
    int32_t i = cur + 1;
    if (!(i < g.S8(c))) i = 0;
    uint32_t sound = 4;
    if (i != cur) {
        do {
            if (OptionAvailable(g, g.U32(c + 4u) + 12u * static_cast<uint32_t>(i))) break;
            ++i;
            if (!(i < g.S8(c))) i = 0;
        } while (i != cur);
        sound = (i != g.S8(c + 3u)) ? 1u : 4u;
    }
    if (!UiSound(k, sound)) return false;
    g.W8(c + 3u, static_cast<uint8_t>(i));
    return true;
}

// RASHCDF 0x8006310C: writes the current option's value byte where the chooser's kind (+2) says.
bool ChooserApply(GuestRam& g, ShellCallees& k, uint32_t c) {
    if (c == 0 || g.U32(c + 4u) == 0) return true;
    const uint32_t opt = g.U32(c + 4u) + 12u * static_cast<uint32_t>(g.S8(c + 3u));
    const uint8_t v = g.U8(opt + 1u);
    const uint32_t port = static_cast<uint32_t>(g.U8(kFePort));
    switch (g.U8(c + 2u)) {
    case 0:
        g.W8(0x800D81E1u, v);                                 // player[0]+0x09, the gang
        if (g.U8(kFeEdges) & 0xC0u) {
            const int8_t gang = g.S8(0x800D81E1u);
            const uint32_t alias = (gang != 0 && gang == 1) ? g.U32(kChoosers + 8u) : g.U32(kChoosers + 4u);
            g.W8(0x800D81E2u, 0);
            ChooserSet(g, alias, 0);
        }
        break;
    case 1: case 2: g.W8(0x800D81E2u, v); break;             // player[0]+0x0A, the alias
    case 3: case 8: case 0x0F: case 0x12: case 0x16: g.W8(0x800D80E0u, v); break; // session+0x08 race id
    case 4: case 5: case 0x10: case 0x13: case 0x17: case 0x18: g.W8(0x800D81DFu, v); break; // player[0]+0x07 bike
    case 6: g.W8(0x800D80DCu, v); break;                      // session+0x04 venue
    case 7: case 0x3E: g.W8(0x800D81DFu + 0x24u * g.U8(0x800D80EBu), v); break;
    case 9: g.W8(0x800D80EAu, v); break;
    case 10: g.W8(0x8009C5EFu, v); break;
    case 0x0B: g.W8(0x800D80E1u, v); break;
    case 0x0C: g.W8(0x800D80E2u, v); break;
    case 0x0D: g.W8(0x800D80E3u, v); break;
    case 0x0E:
        g.W8(0x800D80DFu, v);
        g.W8(0x800D80E0u, static_cast<uint8_t>(v + 0x26u));
        break;
    case 0x11: case 0x14: case 0x15: case 0x19: g.W8(0x800D8203u, v); break; // player[1]+0x07
    case 0x1A: g.W8(0x800D81EDu, v); break;
    case 0x1B: g.W8(0x800D8211u, v); break;
    case 0x1C: {
        const bool changed = g.S8(0x800D80ECu) != static_cast<int8_t>(v);
        g.W8(0x800D80ECu, v);
        if (!k.Call1(kSoundMode, v == 0 ? 1u : 0u)) return false;
        if (changed && !k.Call1(kSoundModeUi, static_cast<uint32_t>(static_cast<int32_t>(g.S8(0x800D80ECu)))))
            return false;
        break;
    }
    case 0x1D: g.W8(0x800D80EFu, v); break;
    case 0x1E: g.W8(0x800D80F1u, v); break;
    case 0x1F: g.W8(kFeTrack, v); break;
    case 0x20: {
        const uint32_t t = g.U8(kFeTrack);
        g.W8(0x800D8198u + t, v);
        const uint32_t args[2] = {g.U8(kFeTrack), static_cast<uint32_t>(static_cast<int32_t>(g.S8(0x800D8198u + t)))};
        if (!k.Call(kJukeboxSet, args, 2, nullptr)) return false;
        break;
    }
    case 0x21: {
        g.W8(kFePort, v);
        const uint32_t p = static_cast<uint32_t>(g.S8(kFePort));
        ChooserSet(g, g.U32(kChoosers + 0x88u), g.S8(0x800D81E0u + 36u * p));
        ChooserSet(g, g.U32(kChoosers + 0x8Cu), g.S8(0x800D81EFu + 36u * p));
        break;
    }
    case 0x22: g.W8(0x800D81E0u + 36u * port, v); break;
    case 0x23: g.W8(0x800D81EFu + 36u * port, v); break;
    case 0x24: g.W8(0x8009C5E9u, v); break;
    default: break;
    }
    return true;
}

// RASHCDF 0x800680E8: binds the selected chooser widget's data object into fe+0x94 (the 72-arm
// table 0x8005BAC4 on action code - 9) and the current option into fe+0xA8.
void BindChooser(GuestRam& g, uint32_t /*screen*/, uint32_t widget) {
    g.W32(kFeChooser, 0);
    g.W8(kFeValid, 0);
    if (widget == 0) return;
    if (g.S16(widget + 8u) == 13) {
        auto slot = [&](uint32_t index) { return g.U32(kChoosers + 4u * index); };
        uint32_t obj = 0;
        bool bind = true;
        const int8_t gang = g.S8(0x800D81E1u);
        switch (g.U16(widget + 18u)) {
        case 9: obj = slot(0); break;
        case 10:
            if (gang == 0) obj = slot(1);
            else if (gang == 1) obj = slot(2);
            else bind = false;
            break;
        case 12: obj = slot(3); break;
        case 13:
            if (gang == 0) obj = slot(4);
            else if (gang == 1) obj = slot(5);
            else bind = false;
            break;
        case 0x10: case 0x21: case 0x27: case 0x30: case 0x38: obj = slot(6); break;
        case 0x11: case 0x3E: obj = slot(7); break;
        case 0x12: case 0x39: obj = slot(8); break;
        case 0x15: obj = slot(11); break;
        case 0x16: obj = slot(12); break;
        case 0x17: obj = slot(13); break;
        case 0x18: obj = slot(10); break;
        case 0x1A: obj = slot(14); break;
        case 0x22: case 0x31: obj = slot(15); break;
        case 0x23: obj = slot(16); break;
        case 0x25: obj = slot(17); break;
        case 0x28: obj = slot(18); break;
        case 0x29: obj = slot(19); break;
        case 0x2B: obj = (g.S8(0x800D81DFu) > 0x11) ? slot(21) : slot(20); break;
        case 0x2D: obj = slot(22); break;
        case 0x2E: obj = slot(23); break;
        case 0x32: obj = slot(24); break;
        case 0x33: obj = slot(26); break;
        case 0x35: obj = slot(25); break;
        case 0x36: obj = slot(27); break;
        case 0x3A: obj = slot(9); break;
        case 0x48: obj = slot(29); break;
        case 0x49: obj = slot(30); break;
        case 0x4A: obj = slot(31); break;
        case 0x4B: obj = slot(28); break;
        case 0x4D: obj = slot(33); break;
        case 0x4E: obj = slot(34); break;
        case 0x4F: obj = slot(35); break;
        case 0x50: obj = slot(36); break;
        default: obj = 0; break;
        }
        if (bind) g.W32(kFeChooser, obj);
    }
    g.W32(kFeOptPrev, g.U32(kFeOpt));
    const uint32_t c = g.U32(kFeChooser);
    if (c != 0 && g.U32(c + 4u) != 0) {
        g.W32(kFeOpt, g.U32(c + 4u) + 12u * static_cast<uint32_t>(g.S8(c + 3u)));
        g.W8(kFeValid, static_cast<uint8_t>(ChooserCount(g, c)));
    }
}

// RASHCDF 0x8006B7BC with the two arms of 0x8006B800 that its fe+0x15 lets run (0x8006BA2C..):
// Left/Right step the bound chooser.
bool ObjectInput(GuestRam& g, ShellCallees& k, uint32_t screen, int32_t* result) {
    g.W8(kFeObjArm, 1);
    const int32_t first = g.S8(screen + 14u);
    const int32_t end = first + g.S8(screen + 15u);
    const uint32_t obj = g.U32(kFeChooser);
    int32_t r = 0;
    if (obj != 0) {
        for (int32_t p = first; p < end && r == 0; ++p) {
            const uint32_t pad = static_cast<uint32_t>(p);
            if (g.S8(PadSlot(pad, kLeft)) != 0) {
                if (!ChooserPrev(g, k, obj)) return false;
                r = 2;
                g.W8(kFeEdges, static_cast<uint8_t>(g.U8(kFeEdges) | 0x40u));
            } else if (g.S8(PadSlot(pad, kRight)) != 0) {
                if (!ChooserNext(g, k, obj)) return false;
                r = 2;
                g.W8(kFeEdges, static_cast<uint8_t>(g.U8(kFeEdges) | 0x80u));
            }
        }
    }
    g.W8(kFeObjArm, 0);
    *result = r;
    return true;
}

// RASHCDF 0x80064254: the option mask for a venue (1 << venue for 1..5, else 1).
void SetOptionMask(GuestRam& g, int32_t venue) {
    uint8_t m = 1;
    switch (venue) {
    case 1: m = 2; break;
    case 2: m = 4; break;
    case 3: m = 8; break;
    case 4: m = 0x10; break;
    case 5: m = 0x20; break;
    default: m = 1; break;
    }
    g.W8(kFeOptMask, m);
}

// ---------------------------------------------------------------------------- career readers

int32_t AllDone(GuestRam& g, int32_t first, int32_t last) {
    for (;; ++first) {
        if (last < first) return 1;
        if (!((g.U8(kProgress + static_cast<uint32_t>(first >> 3)) >> (first & 7)) & 1u)) return 0;
    }
}

int32_t AllDoneMissions(GuestRam& g, int32_t first, int32_t last) {
    for (;; ++first) {
        if (last < first) return 1;
        if (!((g.U8(kMissions + static_cast<uint32_t>(first >> 3)) >> (first & 7)) & 1u)) return 0;
    }
}

int32_t BeatsRecord(GuestRam& g, int32_t raceId, uint32_t value) {
    return static_cast<int32_t>(value) < g.S32(0x80053B30u + 0xB0u * static_cast<uint32_t>(raceId - 56)) ? 1 : 0;
}

// ---------------------------------------------------------------------------- screens

void ArmScreen(GuestRam& g, int32_t id) {
    const uint32_t s = Table(g, id);
    g.W16(s, static_cast<uint16_t>(g.U16(s) | 3u));
    g.W16(Table(g, id) + 2u, 0x7FFF);
}

void GotoScreen(GuestRam& g, int32_t id) {
    const uint32_t table = g.U32(kScreenTablePtr);
    for (uint32_t i = 0; i < static_cast<uint32_t>(kScreenCount); ++i) {
        const uint32_t s = g.U32(table + 4u * i);
        g.W16(s, static_cast<uint16_t>(g.U16(s) & 0xFF00u));
        g.W16(g.U32(table + 4u * i) + 2u, 0);
    }
    g.W32(kCurScreen, Table(g, static_cast<int16_t>(id)));
    g.W16(kFeCur, static_cast<uint16_t>(id));
    g.W16(kFeNext, static_cast<uint16_t>(id));
    ArmScreen(g, static_cast<int16_t>(id));
}

// RASHCDF 0x80066EF8 (frontend.md 3.4).
bool ChangeScreen(GuestRam& g, ShellCallees& /*k*/) {
    const uint32_t table = g.U32(kScreenTablePtr);
    uint16_t cur = g.U16(kFeCur);
    bool shared = false;
    if (cur == g.U16(kFeNext)) return true;
    g.W32(kIdle, 0);
    g.W32(0x80088C50u, 0);
    if (cur != 0xFFFFu) {
        uint32_t u = cur;
        do {
            const uint32_t s = g.U32(table + 4u * static_cast<uint32_t>(static_cast<int16_t>(u)));
            for (int32_t p = g.S16(kFeNext); p != -1; p = g.S16(g.U32(table + 4u * static_cast<uint32_t>(p)) + 10u)) {
                if (static_cast<int16_t>(u) == p) {
                    shared = true;
                    break;
                }
            }
            if (!shared) {
                g.W16(s, static_cast<uint16_t>(g.U16(s) & 0xFFFCu));
                if (g.U16(s + 2u) == 0) g.W16(s + 2u, 0x7FFE);
            }
            u = static_cast<uint32_t>(static_cast<int32_t>(g.S16(s + 10u)));
        } while (u != 0xFFFFFFFFu);
    }
    const uint32_t next = g.U32(g.U32(kScreenTablePtr) + 4u * static_cast<uint32_t>(g.S16(kFeNext)));
    if ((g.U16(next) & 3u) == 0 && g.U16(next + 2u) == 0) g.W16(next + 2u, 0x7FFF);
    if (g.U16(kFeCur) == 4) g.W8(kSession + 0x10u, 4);
    const int16_t c = g.S16(kFeCur);
    if (c > 4 && (c < 8 || g.U16(kFeCur) == 0x1D)) CareerSetup(g, g.S16(kFeNext));
    if (g.U16(kFeCur) == 4) {
        const uint32_t s = g.U32(g.U32(kScreenTablePtr) + 4u * static_cast<uint32_t>(g.S16(kFeNext)));
        g.W16(g.U32(g.U32(kScreenTablePtr) + 4u * static_cast<uint32_t>(g.S16(kFeNext))) + 4u,
              static_cast<uint16_t>(FirstItem(g, s)));
    }
    g.W16(kFeCur, g.U16(kFeNext));
    const uint32_t now = g.U32(g.U32(kScreenTablePtr) + 4u * static_cast<uint32_t>(g.S16(kFeNext)));
    g.W32(kCurScreen, now);
    g.W16(now, static_cast<uint16_t>(g.U16(now) | 3u));
    return true;
}

// RASHCDF 0x8006711C.
bool CommitScreen(GuestRam& g, ShellCallees& k) {
    g.W32(kFeLastItem, g.U32(kFeItem));
    const uint16_t cur = g.U16(kFeCur);
    if (cur == 5 || cur == 0x1D) {
        g.W8(kSession + 0x18u, 0);
        g.W8(kSession + 0x12u, 1);
        g.W8(kSession + 0x04u, 0);
        ClearProgress(g);
    }
    return ChangeScreen(g, k);
}

namespace {
// The part of NewGame / CareerSetup that both start with (0x800686A0.. / 0x80068858..).
void ResetCareer(GuestRam& g) {
    ClearProgress(g);
    g.W8(kSession + 0x11u, 0);
    g.W8(0x800D81DEu, 0x7F);
    g.W8(0x800D8202u, 0x7F);
    g.W8(kSession + 0x05u, 0x30);
    g.W8(kSession + 0x15u, 0);
    g.W8(kSession + 0x16u, 0);
    g.W16(kFeHub, 0x28);
    g.W8(kSession + 0x1Du, 0);
}
void ClearPlayers(GuestRam& g) {
    for (uint32_t p = 0; p < 6u; ++p) {
        const uint32_t r = kPlayers + 36u * p;
        g.W16(r + 12u, 0);
        g.W8(r + 14u, 0);
        g.W8(r + 15u, 0);
        g.W32(r + 16u, 0);
        g.W8(r + 20u, 0);
        g.W8(r + 22u, 0);
    }
}
uint32_t Chooser(GuestRam& g, uint32_t index) { return g.U32(kChoosers + 4u * index); }
void Reselect(GuestRam& g, int32_t screenId) {
    const uint32_t s = Table(g, screenId);
    g.W16(Table(g, screenId) + 4u, static_cast<uint16_t>(FirstItem(g, s)));
}
} // namespace

// RASHCDF 0x80068688 (frontend.md 7.2).
void NewGame(GuestRam& g, int32_t screenId) {
    ResetCareer(g);
    g.W8(kFe + 0x22u, 0);
    ClearPlayers(g);
    MemSet(g, kSession + 0x40u, 0, 0x80);
    MemSet(g, kSession + 0x20u, 0, 0x10);
    if (screenId == 0x18 || screenId != 0x26) {
        g.W8(kSession + 0x16u, 1);
        g.W16(kFeHub, 0x18);
    }
    g.W8(kSession + 0x13u, 0);
    g.W8(kFe + 0x23u, 0);
    ChooserSet(g, Chooser(g, 8), g.S8(kSession + 0x08u));
    ChooserSet(g, Chooser(g, 7), g.S8(0x800D81DFu));
    ChooserSet(g, Chooser(g, 13), g.S8(kSession + 0x0Bu));
    ChooserSet(g, Chooser(g, 12), g.S8(kSession + 0x0Au));
    ChooserSet(g, Chooser(g, 11), g.S8(kSession + 0x09u));
    ChooserSet(g, Chooser(g, 6), g.S8(kSession + 0x04u));
    ChooserSet(g, Chooser(g, 9), g.U8(kSession + 0x12u));
    Reselect(g, screenId);
}

// RASHCDF 0x8006883C: what landing on a mode's setup screen resets (called from ChangeScreen).
void CareerSetup(GuestRam& g, int32_t screenId) {
    ResetCareer(g);
    ClearPlayers(g);
    MemSet(g, kSession + 0x40u, 0, 0x80);
    MemSet(g, kSession + 0x20u, 0, 0x10);
    MemSet(g, kSession + 0x30u, 0, 0x10);
    auto set = [&](uint32_t a, uint8_t v) { g.W8(a, v); };
    switch (screenId) {
    case 7:
        g.W32(kSession, 0x20);
        set(kSession + 0x06u, 1);
        set(0x800D81E1u, 0);
        ChooserSet(g, Chooser(g, 0), 0);
        set(0x800D81E2u, 0);
        ChooserSet(g, Chooser(g, 1), 0);
        ChooserSet(g, Chooser(g, 2), 0);
        Reselect(g, screenId);
        [[fallthrough]];
    case 8:
        g.W32(kSession, 0x20);
        set(kSession + 0x1Bu, 1);
        set(kSession + 0x06u, 1);
        set(kSession + 0x18u, 0);
        set(kSession + 0x04u, 0);
        set(kSession + 0x08u, 4);
        if (g.S8(0x800D81E1u) == 0 || g.S8(0x800D81E1u) != 1) {
            set(0x800D81DFu, 0);
        } else {
            set(0x800D81DFu, 9);
            if (g.S8(0x800D81E2u) == 1) set(kSession + 0x1Du, 2);
        }
        ChooserSet(g, Chooser(g, 3), 4);
        ChooserSet(g, Chooser(g, 4), 0);
        ChooserSet(g, Chooser(g, 2), 9);
        break;
    case 0x18:
        set(kSession + 0x16u, 1);
        g.W16(kFeHub, 0x18);
        [[fallthrough]];
    case 0x26:
        g.W32(kSession, 4);
        set(kFe + 0x22u, 0);
        set(kSession + 0x06u, 1);
        set(kSession + 0x08u, 0x38);
        set(0x800D81DFu, 0);
        set(kSession + 0x13u, 0);
        set(kSession + 0x04u, 0);
        set(kFe + 0x23u, 0);
        set(kSession + 0x0Bu, 1);
        set(kSession + 0x0Au, 1);
        set(kSession + 0x09u, 1);
        ChooserSet(g, Chooser(g, 8), g.S8(kSession + 0x08u));
        ChooserSet(g, Chooser(g, 7), g.S8(0x800D81DFu));
        ChooserSet(g, Chooser(g, 13), g.S8(kSession + 0x0Bu));
        ChooserSet(g, Chooser(g, 12), g.S8(kSession + 0x0Au));
        ChooserSet(g, Chooser(g, 11), g.S8(kSession + 0x09u));
        ChooserSet(g, Chooser(g, 6), g.S8(kSession + 0x04u));
        ChooserSet(g, Chooser(g, 9), g.U8(kSession + 0x12u));
        break;
    case 0x1B:
        g.W32(kSession, 1);
        set(kSession + 0x1Bu, 1);
        set(kSession + 0x06u, 1);
        set(kSession + 0x18u, 0);
        set(kSession + 0x04u, 0);
        set(0x800D81E1u, 2);
        set(kSession + 0x08u, 0x26);
        set(0x800D81DFu, 0x12);
        Reselect(g, screenId);
        ClearMissions(g);
        return;
    case 0x1E:
        g.W32(kSession, 0x10);
        set(kSession + 0x06u, 2);
        set(kSession + 0x08u, 1);
        set(kSession + 0x18u, 0);
        set(0x800D81DFu, 0);
        set(0x800D8203u, 9);
        ChooserSet(g, Chooser(g, 15), g.S8(kSession + 0x08u));
        ChooserSet(g, Chooser(g, 16), g.S8(0x800D81DFu));
        ChooserSet(g, Chooser(g, 17), g.S8(0x800D8203u));
        ChooserSet(g, Chooser(g, 6), g.S8(kSession + 0x04u));
        break;
    case 0x20:
        g.W32(kSession, 0x11);
        set(kSession + 0x06u, 2);
        set(kSession + 0x08u, 0x26);
        set(kSession + 0x18u, 0);
        set(0x800D81DFu, 0x12);
        set(0x800D8203u, 9);
        ChooserSet(g, Chooser(g, 18), g.S8(kSession + 0x08u));
        ChooserSet(g, Chooser(g, 19), g.S8(0x800D81DFu));
        ChooserSet(g, (g.S8(0x800D81DFu) > 0x11) ? Chooser(g, 21) : Chooser(g, 20), g.S8(0x800D8203u));
        ChooserSet(g, Chooser(g, 6), g.S8(kSession + 0x04u));
        break;
    case 0x22:
        g.W32(kSession, 0x18);
        set(kSession + 0x08u, 1);
        set(kSession + 0x18u, 0);
        set(kSession + 0x06u, 2);
        set(0x800D81DFu, 7);
        set(0x800D8203u, 7);
        ChooserSet(g, Chooser(g, 15), g.S8(kSession + 0x08u));
        ChooserSet(g, Chooser(g, 24), g.S8(0x800D81DFu));
        ChooserSet(g, Chooser(g, 25), g.S8(0x800D8203u));
        ChooserSet(g, Chooser(g, 6), g.S8(kSession + 0x04u));
        Reselect(g, screenId);
        set(0x800D81EDu, 2);
        set(0x800D8211u, 2);
        return;
    case 0x24:
        g.W32(kSession, 8);
        set(kSession + 0x18u, 0);
        set(kSession + 0x1Bu, 1);
        set(kSession + 0x06u, 1);
        set(kSession + 0x04u, 0);
        set(kSession + 0x08u, 4);
        set(0x800D81DFu, 7);
        set(0x800D8203u, 7);
        ChooserSet(g, Chooser(g, 22), g.S8(kSession + 0x08u));
        ChooserSet(g, Chooser(g, 23), g.S8(0x800D81DFu));
        Reselect(g, screenId);
        set(0x800D81EDu, 1);
        return;
    default:
        return;
    }
    Reselect(g, screenId);
}

// ---------------------------------------------------------------------------- per-screen handlers

// RASHCDF 0x8006A8FC: the movie screens. With no movie playing (fe+0x0A bit 0 clear) the screen is
// done at once and goes to its advance target - which is what a port without the player does.
int32_t MovieInput(GuestRam& g, ShellCallees& k, uint32_t screen, bool* ok) {
    *ok = true;
    g.W32(kIdle, 0);
    int32_t r = 0;
    if (g.S16(screen + 2u) == 0) {
        if ((g.U16(kFeMedia) & 1u) == 0) {
            r = 1;
        } else {
            const int32_t first = g.S8(screen + 14u);
            for (int32_t p = first; p < first + g.S8(screen + 15u); ++p) {
                const uint32_t b = kPads + kPadStride * static_cast<uint32_t>(p);
                static const uint32_t kOff[] = {0x52, 0x4A, 0x3A, 0x42, 0x32, 0x2A, 0x1A, 0x22, 0x7A, 0x82, 0x5A, 0x62, 0x6A, 0x72};
                bool any = false;
                for (uint32_t off : kOff) any = any || g.S8(b + off) > 0;
                if (any) {
                    if (!UiSound(k, 2)) {
                        *ok = false;
                        return 0;
                    }
                    g.W16(kFeMedia, static_cast<uint16_t>(g.U16(kFeMedia) | 4u));
                }
            }
        }
    }
    if (r == 1) g.W16(kFeNext, g.U16(kNavAdvance + 4u * static_cast<uint32_t>(g.S16(kFeCur))));
    return r;
}

// RASHCDF 0x8006AC80: the legal/splash panel - a 301-frame timeout against the vblank counter.
int32_t SplashInput(GuestRam& g, ShellCallees& k, uint32_t screen, bool* ok) {
    *ok = true;
    int32_t r = 0; // a1: the entry arm (0x8006ACA4) jumps to 0x8006ACE4 with a1 still 0
    if (g.S16(screen + 2u) == 32767) {
        g.W16(kFeStamp, static_cast<uint16_t>(g.U32(kVblankCount)));
    } else if (!(g.U32(kVblankCount) - static_cast<uint32_t>(static_cast<int32_t>(g.S16(kFeStamp))) < 301u)) {
        if (!k.Call1(kSplashFade, 2)) {
            *ok = false;
            return 0;
        }
        g.W32(kIdle, 0);
        r = 1;
    }
    if (r == 1) g.W16(kFeNext, g.U16(kNavAdvance + 4u * static_cast<uint32_t>(g.S16(kFeCur))));
    return r;
}

// RASHCDF 0x80068448: the three choosers the pass below keeps valid for the current mode.
void PassChoosers(GuestRam& g) {
    auto c = [&](uint32_t i) { return g.U32(kChoosers + 4u * i); };
    const uint32_t a = 0x8009C668u, b = 0x8009C66Cu, d = 0x8009C670u;
    switch (g.U32(kSession)) {
    case 4: g.W32(a, c(8)); g.W32(b, c(7)); g.W32(d, 0); return;
    case 8: g.W32(d, 0); g.W32(a, c(22)); g.W32(b, c(23)); return;
    case 0x10: g.W32(a, c(15)); g.W32(b, c(16)); g.W32(d, c(17)); return;
    case 0x11:
        g.W32(a, c(18));
        g.W32(b, c(19));
        g.W32(d, g.S8(0x800D81DFu) < 0x12 ? c(20) : c(21));
        return;
    case 0x18: g.W32(a, c(15)); g.W32(b, c(24)); g.W32(d, c(25)); return;
    case 0x20: {
        g.W32(a, c(3));
        g.W32(b, c(4));
        const int8_t gang = g.S8(0x800D81E1u);
        if (gang == 0) {
            g.W32(d, 0);
            return;
        }
        g.W32(b, c(5));
        if (gang == 1) {
            g.W32(d, 0);
            return;
        }
        break;
    }
    default: g.W32(a, 0); break;
    }
    g.W32(b, 0);
    g.W32(d, 0);
}

// RASHCDF 0x80080ACC: the index of the first selectable widget of a screen, else 0.
int32_t FirstSelectable(GuestRam& g, uint32_t screen) {
    uint32_t w = g.U32(screen + 16u);
    if (w == 0) return 0;
    const int32_t n = g.S16(screen + 8u);
    for (int32_t i = 0; i < n; ++i, w += 120u)
        if (Selectable(g, w, g.U32(kSession))) return static_cast<int16_t>(i);
    return 0;
}

namespace {
// The "current option unavailable -> first valid" step the pass applies to each chooser.
void Revalidate(GuestRam& g, uint32_t c) {
    const uint32_t opt = g.U32(c + 4u) + 12u * static_cast<uint32_t>(g.S8(c + 3u));
    if (!OptionAvailable(g, opt)) g.W8(c + 3u, static_cast<uint8_t>(ChooserFirstValid(g, c)));
}
} // namespace

// RASHCDF 0x8006738C - the per-frame chooser pass (asm 0x8006738C..0x80067F4C).
bool ChooserPass(GuestRam& g, ShellCallees& k, uint32_t /*players*/) {
    const int8_t v = g.S8(kSession + 4u);
    if ((g.U32(kSession) & 0x20u) == 0 && v != 2) {
        if (v < 3) {
            if (v != 0) g.W8(kSession + 4u, 0);
        } else if (v != 4) {
            g.W8(kSession + 4u, 0);
        }
    }
    SetOptionMask(g, g.S8(kSession + 4u));
    PassChoosers(g);
    for (uint32_t slot : {0x8009C668u, 0x8009C66Cu, 0x8009C670u}) {
        const uint32_t c = g.U32(slot);
        if (c == 0 || g.U32(c + 4u) == 0) continue;
        Revalidate(g, c);
        if (!ChooserApply(g, k, c)) return false;
    }
    {
        const uint32_t c = g.U32(kChoosers + 4u * 14u);
        if (g.U32(kSession) == 1 && c != 0 && g.U32(c + 4u) != 0) {
            Revalidate(g, c);
            if (!ChooserApply(g, k, c)) return false;
        }
    }
    {
        const uint32_t c = g.U32(kFeChooser);
        if (c != 0 && g.U32(c + 4u) != 0) Revalidate(g, c);
    }
    if ((g.U8(kMultitapByte) >> 4) != 8) {
        if (g.U32(kSession) == 8) {
            g.W8(0x800D81EDu, 1);
        } else {
            if (g.S8(0x800D81EDu) == 1) {
                g.W8(0x800D81EDu, 2);
                ChooserSet(g, g.U32(kChoosers + 4u * 26u), 2);
            }
            if (g.S8(0x800D8211u) == 1) {
                g.W8(0x800D8211u, 2);
                ChooserSet(g, g.U32(kChoosers + 4u * 27u), 2);
            }
        }
    }
    const uint32_t cur = Table(g, g.S16(kFeCur));
    g.W32(kCurScreen, cur);
    const uint32_t w = g.U32(cur + 16u) + 120u * static_cast<uint32_t>(g.S16(cur + 4u));
    if (!Selectable(g, w, g.U32(kSession))) g.W16(g.U32(kCurScreen) + 4u, static_cast<uint16_t>(FirstSelectable(g, g.U32(kCurScreen))));
    for (uint32_t p = 0; p < 4u; ++p) {
        const uint32_t rec = kPlayers + 36u * p + 8u;
        const uint32_t x = static_cast<uint32_t>(g.U8(rec)) - 3u;
        if (g.U32(kPads + 192u * p + 0x10u) == 0) {
            if (x < 2u) g.W8(rec, 0);
        } else if (!(x < 2u)) {
            g.W8(rec, 3);
        }
    }
    {
        const uint32_t c = g.U32(kChoosers + 4u * 33u);
        if (c != 0 && g.U32(c + 4u) != 0 && !OptionAvailable(g, g.U32(c + 4u) + 12u * static_cast<uint32_t>(g.S8(c + 3u)))) {
            g.W8(c + 3u, static_cast<uint8_t>(ChooserFirstValid(g, c)));
            ChooserSet(g, c, g.S8(kFePort));
        }
    }
    {
        const uint32_t c = g.U32(kChoosers + 4u * 34u);
        if (c != 0 && g.U32(c + 4u) != 0 && !OptionAvailable(g, g.U32(c + 4u) + 12u * static_cast<uint32_t>(g.S8(c + 3u)))) {
            g.W8(c + 3u, static_cast<uint8_t>(ChooserFirstValid(g, c)));
            ChooserSet(g, c, g.S8(0x800D81E0u + 36u * static_cast<uint32_t>(g.S8(kFePort))));
        }
    }
    return true;
}

// A screen or object handler, by the address its table holds: the ported ones natively, any other
// through the seam with the screen as its only argument (the original calls them all as h(screen)).
bool CallHandler(GuestRam& g, ShellCallees& k, uint32_t handler, uint32_t screen, int32_t* result) {
    bool ok = true;
    switch (handler) {
    case 0x80069418: return DefaultScreenInput(g, k, screen, result);
    case 0x8006A8FC: *result = MovieInput(g, k, screen, &ok); return ok;
    case 0x8006AC80: *result = SplashInput(g, k, screen, &ok); return ok;
    case 0x8006C354: *result = StartRaceInput(g, screen); return true;
    case 0x8006B7BC: return ObjectInput(g, k, screen, result);
    default: {
        bool handled = false;
        const bool portedOk = CallPortedScreenHandler(g, k, handler, screen, result, &handled); // shell_screens.h
        if (handled) return portedOk;
        uint32_t v0 = 0;
        if (!k.Call1(handler, screen, &v0)) return false;
        *result = static_cast<int32_t>(v0);
        return true;
    }
    }
}

// RASHCDF 0x800667E4 - the input pass (asm 0x800667E4..0x80066C30).
bool InputPass(GuestRam& g, ShellCallees& k) {
    int32_t r = 0;
    g.W8(kFeEdges, 0);
    if (g.U32(kPads + 4u) != 0 || g.U32(kPads + 192u + 4u) != 0) g.W32(kIdle, 0);
    g.W16(kFeSweep, g.U16(kFeCur));
    g.W32(kCurScreen, Table(g, g.S16(kFeCur)));
    // The pre-input arms, jump table 0x8005B9D4: the music driver.
    const int16_t cur = g.S16(kFeCur);
    auto startMusic = [&]() -> bool {
        if (g.S8(kFeMusicOk) != 0 && g.S8(kFeMusicOn) == 0 && (g.U16(kFeMedia) & 1u) == 0) {
            if (!k.Call1(kMusicPlay, g.U8(kFeTrack))) return false;
            g.W8(kFeMusicOn, 1);
        }
        return true;
    };
    switch (cur) {
    case 0: case 1: case 2: case 6: case 11: case 12: case 15: case 16: case 19: case 20: case 22: case 58:
        if (g.S8(kFeMusicOn) != 0) {
            if (!k.Call1(kMusicStop, 1)) return false;
            g.W8(kFeMusicOn, 0);
        }
        break;
    case 3: break;
    case 40:
        g.W8(kSession + 0x13u, g.U8(kFe + 0x23u));
        if (!startMusic()) return false;
        break;
    case 4: case 5: case 29:
        g.W8(kSession + 0x1Bu, 0);
        [[fallthrough]];
    default:
        if (!startMusic()) return false;
        break;
    }
    if (g.U16(kFeMedia) & 4u) {
        if (!k.Call(kSkipMovie, nullptr, 0, nullptr)) return false;
    }
    const uint32_t scr = g.U32(kCurScreen);
    const uint32_t items = g.U32(scr + 16u);
    g.W32(kFeItem, items == 0 ? 0u : items + 120u * static_cast<uint32_t>(g.S16(scr + 4u)));
    BindChooser(g, g.U32(kCurScreen), g.U32(kFeItem));
    const uint32_t obj = g.U32(kFeChooser);
    if (obj != 0) {
        const uint32_t h = g.U32(kObjectInput + 4u * static_cast<uint32_t>(g.S8(obj + 2u)));
        if (h != 0 && !CallHandler(g, k, h, g.U32(kCurScreen), &r)) return false;
    }
    BindSlider(g, g.U32(kCurScreen), g.U32(kFeItem));
    if (g.S16(kFeSweep) != -1 && r == 0) {
        do {
            const uint32_t s = Table(g, g.S16(kFeSweep));
            g.W32(kCurScreen, s);
            const uint32_t h = g.U32(kScreenInput + 4u * static_cast<uint32_t>(g.S16(s + 6u)));
            if (h != 0 && !CallHandler(g, k, h, s, &r)) return false;
            g.W16(kFeSweep, g.U16(g.U32(kCurScreen) + 10u));
        } while (g.S16(kFeSweep) != -1 && r == 0);
    }
    // The original's development shortcut, gated on flags the shipped build leaves clear.
    const uint32_t mode = g.U32(kSession), dbg = g.U32(kDebugFlags);
    if ((mode == 0x20 && (dbg & 1u)) || (mode == 1 && (dbg & 2u)) || (mode == 8 && (dbg & 4u))) {
        const bool a = g.S8(PadSlot(0, 8)) != 0, b = g.S8(PadSlot(0, 10)) != 0;
        if (a || b) {
            g.W8(kSession + 0x11u, 0);
            g.W8(kSession + 0x05u, static_cast<uint8_t>(g.U8(kSession + 0x05u) | 0x18u));
            ClearProgress(g);
            const int8_t v = g.S8(kSession + 0x04u);
            int8_t nv;
            if (a) nv = (v == 2) ? 0 : ((v > 2 && v == 4) ? 2 : 4);
            else nv = (v != 2) ? ((v < 3 || v != 4) ? 2 : 0) : 4;
            g.W8(kSession + 0x04u, static_cast<uint8_t>(nv));
        }
    }
    if (!ChooserPass(g, k, kPlayers)) return false;
    SetMode(g, g.U32(kFeItem));
    if (!ChooserApply(g, k, g.U32(kFeChooser))) return false;
    if (!(g.S32(kIdle) < 1801) && g.S16(kFeCur) == 4) g.W16(kFeNext, 58);
    return true;
}

// ---------------------------------------------------------------------------- the card

void CardChecksumHost(const uint8_t* rec, int32_t slot, uint32_t* a, uint32_t* b) {
    uint32_t sa = 0, sb = 0, acc = 0;
    for (uint32_t i = 0; i < 476u; ++i) {
        const uint32_t x = rec[i];
        sa = sa + x + i + acc;
        sb = ((x ^ sb) << 1) + acc;
        acc += static_cast<uint32_t>(slot + 1);
    }
    *a = sa;
    *b = sb;
}

// RASHCDF 0x8005E9D0(slot, outA, outB) over ctx + 0x854 + 484*slot, ctx = *(0x8009954C).
void CardChecksum(GuestRam& g, int32_t slot, uint32_t outA, uint32_t outB) {
    const uint32_t rec = g.U32(0x8009954Cu) + 0x1E4u * static_cast<uint32_t>(slot) + 0x854u;
    uint8_t buf[476];
    g.ReadBlock(rec, buf, 476u);
    uint32_t a = 0, b = 0;
    CardChecksumHost(buf, slot, &a, &b);
    g.W32(outA, a);
    g.W32(outB, b);
}

// ---------------------------------------------------------------------------- the commit

namespace {
uint8_t RaceKind(int32_t raceId) { // RASHCDF 0x8007F2A8
    switch (raceId) {
    case 0x26: case 0x2C: case 0x32: return 9;
    case 0x27: case 0x2D: case 0x33: return 10;
    case 0x28: case 0x2E: case 0x34: return 3;
    case 0x29: case 0x2F: case 0x35: return 4;
    case 0x2A: case 0x2B: case 0x30: case 0x31: case 0x36: case 0x37: return 1;
    default: return 0;
    }
}
} // namespace

// RASHCDF 0x8007F37C (rules.md 2.3).
bool CommitSelection(GuestRam& g, ShellCallees& k) {
    if (g.U32(kDemo) != 0) return true;
    // 0x8007F304
    {
        const uint32_t gs = GameState(g);
        g.W8(gs + 5u, static_cast<uint8_t>(g.U8(gs + 5u) | 1u));
        g.W8(GameState(g) + 5u, static_cast<uint8_t>(g.U8(GameState(g) + 5u) | 2u));
        g.W32(0x8005ACC0u, 1);
        g.W32(0x8005ACC4u, 1);
        g.W8(GameState(g) + 0x38u, 3);
        g.W8(GameState(g) + 0x39u, 0);
        g.W8(GameState(g) + 10u, 0);
        g.W8(GameState(g) + 11u, 0);
    }
    for (uint32_t p = 0; p < 6u; ++p) MemSet(g, 0x800D81F0u + 36u * p, 0, 8);
    for (uint32_t p = 0; p < g.U32(GameState(g) + 0x34u); ++p) {
        PadConfig(g, kPadLive + 192u * p, g.S8(kPlayers + 36u * p + 8u));
        g.W32(kPorts + 24u * p + 8u, static_cast<uint32_t>(static_cast<int32_t>(g.S8(kPlayers + 36u * p + 0x17u))));
    }
    if (!k.Call1(kPadInstall, static_cast<uint32_t>(static_cast<int32_t>(g.S8(kSession + 0x17u))))) return false;
    uint32_t gs = GameState(g);
    switch (g.S8(kSession + 0x04u)) {
    case 0: case 1:
        g.W32(gs + 0x3Cu, 0);
        g.W16(gs + 0x3Au, 0);
        break;
    case 2: case 3:
        g.W32(gs + 0x3Cu, 1);
        g.W16(gs + 0x3Au, 1);
        break;
    case 4:
        g.W32(gs + 0x3Cu, 2);
        g.W16(gs + 0x3Au, 2);
        break;
    case 5:
        g.W16(gs + 0x3Au, 3);
        g.W32(gs + 0x3Cu, 2);
        break;
    default: break;
    }
    const uint32_t mode = g.U32(kSession);
    g.W8(GameState(g) + 4u, static_cast<uint8_t>(mode));
    gs = GameState(g);
    g.W32(gs + 0x40u, static_cast<uint32_t>(static_cast<int32_t>(g.S8(kSession + 0x08u))));
    g.W32(gs + 0x44u, g.U8(kSession + 0x11u));
    g.W8(gs + 5u, static_cast<uint8_t>((g.U8(kSession + 0x05u) & 0x10u) ? (g.U8(gs + 5u) | 1u) : (g.U8(gs + 5u) & 0xFEu)));
    gs = GameState(g);
    g.W8(gs + 5u, static_cast<uint8_t>((g.U8(kSession + 0x05u) & 0x20u) ? (g.U8(gs + 5u) | 2u) : (g.U8(gs + 5u) & 0xFDu)));
    gs = GameState(g);
    g.W32(gs + 0x48u, static_cast<uint32_t>(static_cast<int32_t>(g.S8(0x800D81DFu))));
    g.W32(gs + 0x4Cu, static_cast<uint32_t>(static_cast<int32_t>(g.S8(0x800D8203u))));
    if (g.S8(0x800D81DFu) != g.S8(0x800D81DEu)) {
        g.W8(gs + 5u, static_cast<uint8_t>(g.U8(gs + 5u) | 2u));
        g.W8(0x800D81DEu, g.U8(0x800D81DFu));
    }
    gs = GameState(g);
    g.W8(kSession + 0x1Au, g.U8(kSession + 0x19u));
    auto copyIdentity = [&](uint32_t rider, uint32_t player) {
        g.W16(rider + 0x2Cu, g.U16(player + 0x0Cu));
        g.W8(rider + 0x2Eu, g.U8(player + 0x0Eu));
        g.W8(rider + 0x2Fu, g.U8(player + 0x0Fu));
        g.W32(rider + 0x30u, g.U32(player + 0x10u));
    };
    const uint32_t venue = static_cast<uint32_t>(static_cast<int32_t>(g.S8(kSession + 0x04u)));
    switch (mode) {
    case 1:
        copyIdentity(0x800D5758u, kPlayers);
        g.W32(gs + 0x30u, 1);
        g.W8(kSession + 0x19u, 0);
        g.W8(GameState(g) + 6u, RaceKind(g.S8(kSession + 0x08u)));
        g.W8(kSession + 0x0Eu, g.U8(0x80099530u + 2u * venue));
        g.W8(GameState(g) + 7u, g.U8(kSession + 0x0Eu));
        gs = GameState(g);
        g.W32(gs + 0x48u, g.U32(0x80099518u + 4u * venue));
        g.W16(gs + 8u, g.U16(0x80099500u + 4u * venue));
        g.W8(0x800D81E2u, 2);
        break;
    case 4:
        g.W32(gs + 0x30u, 1);
        g.W32(gs + 0x48u, static_cast<uint32_t>(static_cast<int32_t>(g.S8(0x800D81DFu + 36u * g.U8(kSession + 0x13u)))));
        g.W8(kSession + 0x19u, 0);
        g.W8(gs + 5u, static_cast<uint8_t>(g.U8(gs + 5u) | 1u));
        g.W8(GameState(g) + 5u, static_cast<uint8_t>(g.U8(GameState(g) + 5u) | 2u));
        g.W32(0x8005ACC4u, static_cast<uint32_t>(static_cast<int32_t>(g.S8(kSession + 0x0Au))));
        g.W32(0x8005ACC0u, static_cast<uint32_t>(static_cast<int32_t>(g.S8(kSession + 0x0Bu))));
        if (g.U32(0x8005ACC0u) == 0) g.W8(GameState(g) + 0x38u, static_cast<uint8_t>(g.U8(GameState(g) + 0x38u) & 0xFDu));
        if (g.S8(kSession + 0x09u) == 0) g.W8(GameState(g) + 0x38u, static_cast<uint8_t>(g.U8(GameState(g) + 0x38u) & 0xFEu));
        g.W8(0x800D81E2u, 2);
        break;
    case 8:
        copyIdentity(0x800D5758u, kPlayers);
        copyIdentity(0x800D57A0u, kPlayers + 36u);
        g.W32(gs + 0x30u, 1);
        g.W8(kSession + 0x19u, 0);
        g.W8(gs + 10u, g.U8(0x800D81EDu));
        g.W8(0x800D81E2u, 2);
        g.W8(0x800D8206u, 2);
        g.W8(0x800D822Au, 2);
        g.W8(0x800D824Eu, 2);
        break;
    case 0x10:
        g.W8(GameState(g) + 5u, static_cast<uint8_t>(g.U8(GameState(g) + 5u) | 1u));
        g.W32(gs + 0x30u, 2);
        g.W8(GameState(g) + 5u, static_cast<uint8_t>(g.U8(GameState(g) + 5u) | 2u));
        g.W8(0x800D81E2u, 2);
        g.W8(0x800D8206u, 2);
        break;
    case 0x11:
        g.W32(gs + 0x30u, 2);
        g.W8(kSession + 0x0Eu, g.U8(0x8009953Cu + 2u * venue));
        g.W8(gs + 7u, g.U8(kSession + 0x0Eu));
        g.W8(GameState(g) + 5u, static_cast<uint8_t>(g.U8(GameState(g) + 5u) | 1u));
        g.W8(GameState(g) + 5u, static_cast<uint8_t>(g.U8(GameState(g) + 5u) | 2u));
        g.W8(GameState(g) + 6u, (g.S8(0x800D81DFu) < 0x12) ? 0 : 1);
        g.W8(0x800D81E2u, 2);
        g.W8(0x800D8206u, 2);
        break;
    case 0x18:
        g.W8(GameState(g) + 5u, static_cast<uint8_t>(g.U8(GameState(g) + 5u) | 1u));
        g.W32(gs + 0x30u, 2);
        g.W8(GameState(g) + 5u, static_cast<uint8_t>(g.U8(GameState(g) + 5u) | 2u));
        g.W8(GameState(g) + 10u, g.U8(0x800D81EDu));
        g.W8(GameState(g) + 11u, g.U8(0x800D8211u));
        g.W8(0x800D81E2u, 2);
        g.W8(0x800D8206u, 2);
        g.W8(0x800D822Au, 2);
        g.W8(0x800D824Eu, 2);
        break;
    case 0x20: {
        if (g.U32(kDebugFlags) & 0x20u) g.W8(kSession + 0x1Du, 4);
        copyIdentity(0x800D5758u, kPlayers);
        for (uint32_t i = 0; i < 16u; ++i) {
            const uint32_t rider = 0x800D5758u + 0x90u + 0x48u * i, id = kSession + 0x40u + 8u * i;
            g.W16(rider + 0x2Cu, g.U16(id));
            g.W8(rider + 0x2Eu, g.U8(id + 2u));
            g.W8(rider + 0x2Fu, g.U8(id + 3u));
            g.W32(rider + 0x30u, g.U32(id + 4u));
        }
        switch (g.S8(kSession + 0x04u)) {
        case 0: case 2: case 4: g.W8(GameState(g) + 4u, 0x22); break;
        case 1:
            g.W8(GameState(g) + 4u, 0x24);
            g.W16(GameState(g) + 8u, 0x8C);
            break;
        case 3: g.W8(GameState(g) + 4u, 0x21); break;
        case 5:
            g.W8(GameState(g) + 4u, 0x2C);
            g.W8(GameState(g) + 10u, 0);
            g.W8(GameState(g) + 0x39u, 0);
            g.W32(0x8005ACC0u, 0);
            if (g.S8(0x800D81E1u) == 0) g.W8(0x800D81DFu, 8);
            else if (g.S8(0x800D81E1u) == 1) g.W8(0x800D81DFu, 0x11);
            g.W32(GameState(g) + 0x48u, static_cast<uint32_t>(static_cast<int32_t>(g.S8(0x800D81DFu))));
            break;
        default: break;
        }
        g.W32(GameState(g) + 0x30u, 1);
        g.W8(kSession + 0x19u, 0);
        break;
    }
    default: break;
    }
    return true;
}

} // namespace rr::shell
