// The per-screen input handlers, ported from RASHCDF.BIN (hash and reading
// in shell_screens.h). Each function names the instruction range it was transcribed from; the asm is
// the reference (the Ghidra C of this range hides delay-slot stores, several of which order a store
// before a seam call and are kept in that order here).
#include "game/shell/shell_screens.h"

#include "game/shell/shell_memcard.h"

namespace rr::shell {

namespace {

uint32_t Chooser(GuestRam& g, uint32_t index) { return g.U32(kChoosers + 4u * index); }
uint32_t Pad(int32_t p) { return kPads + kPadStride * static_cast<uint32_t>(p); }
int32_t PadEnd(GuestRam& g, uint32_t screen) { return g.S8(screen + 14u) + g.S8(screen + 15u); }
bool UiSound(ShellCallees& k, uint32_t n) { return k.Call1(kUiSound, n); }
void Edge(GuestRam& g, uint32_t bits) { g.W8(kFeEdges, static_cast<uint8_t>(g.U8(kFeEdges) | bits)); }
uint16_t Advance(GuestRam& g, int32_t id) { return g.U16(kNavAdvance + 4u * static_cast<uint32_t>(id)); }
uint16_t Back(GuestRam& g, int32_t id) { return g.U16(kNavBack + 4u * static_cast<uint32_t>(id)); }
bool Call2(ShellCallees& k, uint32_t address, uint32_t a0, uint32_t a1) {
    const uint32_t a[2] = {a0, a1};
    return k.Call(address, a, 2, nullptr);
}
uint32_t SignedByte(int8_t v) { return static_cast<uint32_t>(static_cast<int32_t>(v)); }

// BIOS A(19h) strcpy and A(17h) strcmp, reached through the SLUS stubs 0x800448E4 / 0x80044904 (both
// `li t2,0xA0; jr t2; li t1,n`). Every pointer handed to them here is non-null. A string running off
// RAM faults the view and stops the walk.
void StrCpy(GuestRam& g, uint32_t dst, uint32_t src) {
    for (uint32_t i = 0; i < 0x10000u; ++i) {
        const uint8_t c = g.U8(src + i);
        g.W8(dst + i, c);
        if (c == 0 || g.Faulted()) return;
    }
}
int32_t StrCmp(GuestRam& g, uint32_t a, uint32_t b) {
    for (uint32_t i = 0; i < 0x10000u; ++i) {
        const uint8_t x = g.U8(a + i), y = g.U8(b + i);
        if (x != y) return static_cast<int32_t>(x) - static_cast<int32_t>(y);
        if (x == 0 || g.Faulted()) return 0;
    }
    return 0;
}

// RASHCDF 0x8006B800(screen, obj, padFirst, padEnd) with fe+0x15 set: its entry test (0x8006B83C
// obj == 0 -> 0) and the two arms 0x8006BA40 / 0x8006BA6C the flag lets run (the third test,
// 0x8006BA98, skips everything else while fe+0x15 is set).
bool ObjectArms(GuestRam& g, ShellCallees& k, uint32_t obj, int32_t first, int32_t end, int32_t* result) {
    int32_t r = 0;
    if (obj != 0) {
        for (int32_t p = first; p < end && r == 0; ++p) {
            if (g.S8(Pad(p) + 0x1Au) != 0) {
                if (!ChooserPrev(g, k, obj)) return false;
                Edge(g, 0x40u);
                r = 2;
            } else if (g.S8(Pad(p) + 0x22u) != 0) {
                if (!ChooserNext(g, k, obj)) return false;
                Edge(g, 0x80u);
                r = 2;
            }
        }
    }
    *result = r;
    return true;
}

// ---------------------------------------------------------------- the keyboard of screens 52 and 56
// The arms both keyboards share, instruction for instruction (0x80069618's copy named; 0x80069A8C's
// is the same code at other addresses).

// 0x80069618 entry / 0x80069A8C entry: the first frame of the screen.
void KeyReset(GuestRam& g) {
    g.W32(kIdle, 0);
    g.W32(kKeyCol, 0);
    g.W32(kKeyRow, 0);
    g.W32(kKeyPos, 0);
}

// 0x800698B0 / 0x80069E14: the cursor one character right, up to 7.
void KeyCursorRight(GuestRam& g) {
    const int32_t pos = g.S32(kKeyPos);
    if (pos < 7) g.W32(kKeyPos, static_cast<uint32_t>(pos + 1));
}
// 0x800697B4 / 0x80069BD8: one character left, down to 0.
void KeyCursorLeft(GuestRam& g) {
    const int32_t pos = g.S32(kKeyPos);
    if (pos > 0) g.W32(kKeyPos, static_cast<uint32_t>(pos - 1));
}
// 0x80069884 / 0x80069DE8: the key cap under the cursor into the text, then right.
void KeyType(GuestRam& g) {
    const uint32_t row = g.U32(kKeyRow);
    const uint8_t c = g.U8(kKeyGrid + row * 9u + g.U32(kKeyCol));
    g.W8(kKeyBuf + g.U32(kKeyPos), c);
    KeyCursorRight(g);
}
// 0x80069788 / 0x80069BA8: a space (only while the cursor is inside the 8 characters), then right.
void KeySpace(GuestRam& g) {
    const uint32_t pos = g.U32(kKeyPos);
    if (!(pos < 8u)) return;
    g.W8(kKeyBuf + pos, 0x20);
    KeyCursorRight(g);
}
// 0x800697A4 / 0x80069BC8: the character blanked back to '_', then left.
void KeyRubout(GuestRam& g) {
    g.W8(kKeyBuf + g.U32(kKeyPos), 0x5F);
    KeyCursorLeft(g);
}
// 0x800698CC..0x80069944 (Right), 0x80069948..0x800699BC (Left), 0x800699C0..0x800699F0 (Down),
// 0x800699F4..0x80069A1C (Up). Row 4 has double-width keys at columns 0-1, 3-4, 5-6, 7-8.
void KeyRight(GuestRam& g) {
    const int32_t col = g.S32(kKeyCol);
    int32_t v;
    if (col == 0 || (col >= 3 && col < 8)) v = col + ((g.S32(kKeyRow) != 4) ? 1 : 2);
    else v = g.S32(kKeyCol) + 1;
    g.W32(kKeyCol, static_cast<uint32_t>(v));
    if (!(g.S32(kKeyCol) < 9)) g.W32(kKeyCol, 0);
}
void KeyLeft(GuestRam& g) {
    const int32_t col = g.S32(kKeyCol);
    int32_t v;
    if (col == 1 || (col > 0 && col < 9 && col >= 4)) v = col - ((g.S32(kKeyRow) != 4) ? 1 : 2);
    else v = g.S32(kKeyCol) - 1;
    g.W32(kKeyCol, static_cast<uint32_t>(v));
    if (g.S32(kKeyCol) < 0) g.W32(kKeyCol, 8);
}
void KeyDown(GuestRam& g) {
    const int32_t row = g.S32(kKeyRow) + 1;
    g.W32(kKeyRow, static_cast<uint32_t>(row));
    if (!(row < 5)) g.W32(kKeyRow, 0);
}
void KeyUp(GuestRam& g) {
    const int32_t row = g.S32(kKeyRow) - 1;
    g.W32(kKeyRow, static_cast<uint32_t>(row));
    if (row < 0) g.W32(kKeyRow, 4);
}

// The four direction arms, in the order both loops test them (Right, Left, Down, Up); true when one
// of them took the pad (the loops then return 2).
bool KeyMove(GuestRam& g, uint32_t pad) {
    if (g.S8(pad + 0x22u) != 0) KeyRight(g);
    else if (g.S8(pad + 0x1Au) != 0) KeyLeft(g);
    else if (g.S8(pad + 0x32u) != 0) KeyDown(g);
    else if (g.S8(pad + 0x2Au) != 0) KeyUp(g);
    else return false;
    return true;
}

// 0x80069BF0..0x80069DE4: the new record into the Time Trial's records table (SLUS 0x80053A88, nine
// 176-byte tables for race ids 56..64: a 16-byte header, then eight 20-byte entries {char name[12];
// s32 time; u8 bike; u8 session+0x09; u8 session+0x0A; u8 session+0x0B}).
void KeyEnterRecord(GuestRam& g) {
    g.W8(kKeyBuf + 8u, 0);
    const int32_t race = g.S8(kSession + 8u);
    const uint32_t player = kPlayers + 36u * g.U8(kSession + 0x13u);
    const uint32_t table = kRecords + 176u * static_cast<uint32_t>(race - 56);
    const int32_t time = g.S32(player);
    int32_t slot = 0;
    for (int32_t i = 7; i >= 0; --i)
        if (!(g.S32(table + 20u * static_cast<uint32_t>(i) + 28u) < time)) slot = i;
    for (int32_t i = 7; slot < i; --i) {
        const uint32_t dst = table + 20u * static_cast<uint32_t>(i), src = dst - 20u;
        g.W8(dst + 35u, g.U8(src + 35u));
        g.W8(dst + 33u, g.U8(src + 33u));
        g.W8(dst + 34u, g.U8(src + 34u));
        g.W32(dst + 28u, g.U32(src + 28u));
        g.W8(dst + 32u, g.U8(src + 32u));
        StrCpy(g, dst + 16u, src + 16u);
    }
    const uint32_t e = table + 20u * static_cast<uint32_t>(slot);
    g.W8(e + 32u, g.U8(player + 7u));
    g.W32(e + 28u, g.U32(player));
    g.W8(e + 35u, g.U8(kSession + 11u));
    g.W8(e + 33u, g.U8(kSession + 9u));
    g.W8(e + 34u, g.U8(kSession + 10u));
    for (uint32_t i = 0; i < 8u; ++i)
        if (g.U8(kKeyBuf + i) == 0x5F) g.W8(kKeyBuf + i, 0x20);
    StrCpy(g, e + 16u, kKeyBuf);
    if (slot == 0)
        for (uint32_t w = 0; w < 4u; ++w) g.W32(table + 4u * w, g.U32(kRecordHead + 4u * w));
    g.W16(kFeNext, 26);
}

// 0x800697CC..0x80069880: the text is cut at the cursor, compared with the eight codes of SLUS
// 0x800540B8, and a match sets that bit of the flag word 0x8005AE7C; either way the screen advances.
void KeyEnterCode(GuestRam& g, uint32_t screen) {
    const uint32_t pos = g.U32(kKeyPos);
    if (g.U8(kKeyBuf + pos) == 0x5F) g.W8(kKeyBuf + pos, 0);
    else g.W8(kKeyBuf + pos + 1u, 0);
    StrCpy(g, kKeyCopy, kKeyBuf);
    for (uint32_t i = 0; i < 8u; ++i) {
        if (StrCmp(g, kKeyCopy, g.U32(kCodeTable + 4u * i)) == 0) {
            g.W32(kDebugFlags, g.U32(kDebugFlags) | (1u << i));
            break;
        }
    }
    g.W16(kFeNext, Advance(g, g.S16(screen + 6u)));
}

} // namespace

// ---------------------------------------------------------------------------- RASHCDF 0x8006AAF8
// Screen 0, the EA logo movie: done the frame it is armed (+0x02 == 0) unless a movie is playing
// (fe+0x0A bit 0); done means the advance target.
bool LogoInput(GuestRam& g, uint32_t screen, int32_t* result) {
    int32_t r = 0;
    const int16_t state = g.S16(screen + 2u);
    g.W32(kIdle, 0);
    if (state == 0) r = (g.U16(kFeMedia) & 1u) == 0 ? 1 : 0;
    if (r == 1) g.W16(kFeNext, Advance(g, g.S16(kFeCur)));
    *result = r;
    return true;
}

// ---------------------------------------------------------------------------- RASHCDF 0x8006A838
// Screens 21 and 50, the credits: the menu, and when it takes nothing and fe+0x0A bit 3 (set by the
// credits roll when it ends) is up, the advance target. The first frame clears that bit.
bool CreditsInput(GuestRam& g, ShellCallees& k, uint32_t screen, int32_t* result) {
    g.W32(kIdle, 0);
    if (g.S16(screen + 2u) == 32767) g.W16(kFeMedia, static_cast<uint16_t>(g.U16(kFeMedia) & 0xFFF7u));
    int32_t r = 0;
    if (!DefaultScreenInput(g, k, screen, &r)) return false;
    if (r == 0 && (g.U16(kFeMedia) & 8u)) {
        g.W16(kFeNext, Advance(g, g.S16(kFeCur)));
        r = 1;
    }
    *result = r;
    return true;
}

// ---------------------------------------------------------------------------- RASHCDF 0x8006AD20
// Screen 53, the message panel (MenuInput's Circle arm opens it over the caller): Cross, Circle,
// Triangle or slot 13 (any non-zero code) closes it back to its parent, and so does an idle counter
// of 901 frames. It takes every frame (never returns 0).
bool MessagePanelInput(GuestRam& g, ShellCallees& k, uint32_t screen, int32_t* result) {
    if (g.S16(screen + 2u) == 32767) g.W32(kIdle, 0);
    int32_t r = 0;
    for (int32_t p = g.S8(screen + 14u); p < PadEnd(g, screen) && r == 0; ++p) {
        const uint32_t pad = Pad(p);
        if (g.S8(pad + 0x52u) != 0 || g.S8(pad + 0x42u) != 0 || g.S8(pad + 0x4Au) != 0 || g.S8(pad + 0x7Au) != 0) {
            if (!UiSound(k, 3)) return false;
            g.W16(kFeNext, g.U16(screen + 10u));
            r = 1;
        }
    }
    if (r == 0) {
        if (!(g.S32(kIdle) < 901)) {
            g.W32(kIdle, 0);
            if (!UiSound(k, 3)) return false;
            r = 1;
            g.W16(kFeNext, g.U16(screen + 10u));
        }
        if (r == 0) r = 2;
    }
    *result = r;
    return true;
}

// ---------------------------------------------------------------------------- RASHCDF 0x80069440
// Screens 25 and 39, the race-options overlays: the first frame saves session+0x09..0x0B; a cancel
// that left (edge mask exactly 4, result 1) puts them back and re-points choosers 11..13 at them.
bool RaceOptionsInput(GuestRam& g, ShellCallees& k, uint32_t screen, int32_t* result) {
    if (g.S16(screen + 2u) == 32767) {
        g.W8(kRaceOptSaved + 0u, g.U8(kSession + 9u));
        g.W8(kRaceOptSaved + 1u, g.U8(kSession + 10u));
        g.W8(kRaceOptSaved + 2u, g.U8(kSession + 11u));
    }
    int32_t r = 0;
    if (!DefaultScreenInput(g, k, screen, &r)) return false;
    if (g.U8(kFeEdges) == 4u && r == 1) {
        for (uint32_t i = 0; i < 3u; ++i) {
            const uint8_t v = g.U8(kRaceOptSaved + i);
            g.W8(kSession + 9u + i, v);
            ChooserSet(g, Chooser(g, 11u + i), static_cast<int8_t>(v));
        }
    }
    *result = r == 0 ? 2 : r;
    return true;
}

// ---------------------------------------------------------------------------- RASHCDF 0x80069560
// Screen 26, the trophy room: the first frame points chooser 10 at the session's race; leaving it
// (result 1) goes to the hub fe+0x04, and starts a new game when fe+0x22 says so (screen 24 or 38 by
// session+0x16).
bool TrophyRoomInput(GuestRam& g, ShellCallees& k, uint32_t screen, int32_t* result) {
    if (g.S16(screen + 2u) == 32767) {
        const uint8_t race = g.U8(kSession + 8u);
        g.W8(kFeTrophyRace, race);
        ChooserSet(g, Chooser(g, 10), static_cast<int8_t>(race));
    }
    const int32_t first = g.S8(screen + 14u);
    int32_t r = 0;
    if (!MenuInput(g, k, screen, first, first + g.S8(screen + 15u), &r)) return false;
    if (r == 1) {
        g.W16(kFeNext, g.U16(kFeHub));
        if (g.S8(kFeNewGame) != 0) NewGame(g, g.S8(kSession + 0x16u) != 0 ? 24 : 38);
    }
    *result = r;
    return true;
}

// ---------------------------------------------------------------------------- RASHCDF 0x8006AE6C
// Screen 42, the options menu. Two things before the menu: holding pad 0's slots 0, 8, 10 and 11
// (hold stamps > 0) while pressing Cross on the widget with action code 71 opens the code keyboard,
// screen 56; Triangle returns to the screen MenuInput's Square arm stored in session+0x10 (when the
// back table has an entry for the current screen at all).
bool OptionsInput(GuestRam& g, ShellCallees& k, uint32_t screen, int32_t* result) {
    int32_t r = 0;
    const uint32_t items = g.U32(screen + 16u);
    if (items != 0) {
        const uint32_t w = items + 120u * static_cast<uint32_t>(static_cast<int32_t>(g.S16(screen + 4u)));
        if (g.S32(kPads + 84u) > 0 && g.S32(kPads + 100u) > 0 && g.S32(kPads + 108u) > 0 && g.S32(kPads + 20u) > 0 &&
            g.S8(kPads + 82u) > 0 && g.S16(w + 8u) == 12 && g.U16(w + 18u) == 71) {
            g.W16(kFeNext, 56);
            r = 1;
        }
        for (int32_t p = g.S8(screen + 14u); r == 0 && p < PadEnd(g, screen); ++p) {
            if (g.S8(Pad(p) + 0x4Au) > 0 && g.S32(kNavBack + 4u * static_cast<uint32_t>(g.S16(kFeCur))) != -1) {
                if (!UiSound(k, 3)) return false;
                r = 1;
                g.W16(kFeNext, static_cast<uint16_t>(g.S8(kSession + 0x10u)));
            }
        }
    }
    if (r == 0) {
        const int32_t first = g.S8(screen + 14u);
        if (!MenuInput(g, k, screen, first, first + g.S8(screen + 15u), &r)) return false;
    }
    *result = r;
    return true;
}

// ---------------------------------------------------------------------------- RASHCDF 0x8006AB58
// Screens 54 and 55, the abort-game modal (MenuInput's cancel arm opens it). Not its first frame
// (+0x02 != 0): the answer is reset to No. Cross on No returns to the parent, on Yes goes where the
// cancel was going (fe+0x18); Triangle returns to the parent. It takes every frame.
bool AbortModalInput(GuestRam& g, ShellCallees& k, uint32_t screen, int32_t* result) {
    if (g.S16(screen + 2u) != 0) {
        g.W8(kFeAbortYes, 0);
        ChooserSet(g, Chooser(g, 36), 0);
    }
    int32_t r = 0;
    for (int32_t p = g.S8(screen + 14u); p < PadEnd(g, screen) && r == 0; ++p) {
        const uint32_t pad = Pad(p);
        if (g.S8(pad + 0x52u) > 0) {
            if (g.S8(kFeAbortYes) == 0) g.W16(kFeNext, g.U16(screen + 10u));
            else g.W16(kFeNext, static_cast<uint16_t>(g.S8(kFeDeferred)));
            r = 1;
            if (!UiSound(k, 2)) return false;
        } else if (g.S8(pad + 0x4Au) > 0) {
            g.W16(kFeNext, g.U16(screen + 10u));
            r = 1;
            if (!UiSound(k, 3)) return false;
        }
    }
    *result = r == 0 ? 2 : r;
    return true;
}

// ---------------------------------------------------------------------------- RASHCDF 0x80069FF0
// Screen 48, sound options (0x8006A0E4 is its own call of 0x80069418, not a separate function). The
// seven sliders' descriptors 0x8009C548 are rebuilt from the template 0x80088BD0 and session+0xD4 on
// the first frame, and each value is pushed to the mixer 0x8007F20C. Up/Down stops the preview and
// asks for the next slider's; Left/Right pushes the edited value (slider kind -> mixer channel) and
// replays the preview; a confirm that left keeps the values, a cancel that left restores them and
// the stereo setting session+0x14.
bool SoundOptionsInput(GuestRam& g, ShellCallees& k, uint32_t screen, int32_t* result) {
    if (g.S16(screen + 2u) == 32767) {
        g.W32(kSoundSaved, SignedByte(g.S8(kSession + 20u)));
        for (uint32_t i = 0; i < 7u; ++i) {
            const uint32_t tmpl = kSoundTemplate + 16u * i, desc = kSliderDescs + 16u * i;
            g.W32(tmpl + 12u, g.U32(kSession + 0xD4u + 4u * i));
            for (uint32_t w = 0; w < 16u; w += 4u) g.W32(desc + w, g.U32(tmpl + w));
            if (!Call2(k, kVolumeSet, i, g.U32(kSession + 0xD4u + 4u * i))) return false;
        }
        g.W32(kSoundPreviewDue, 1);
    }
    if (g.U32(kSoundPreviewDue) != 0) {
        const uint32_t slider = g.U32(kFeSlider);
        g.W32(kSoundPreviewDue, 0);
        if (slider != 0 && !Call2(k, kSoundPreview, SignedByte(g.S8(slider + 11u)), g.U32(g.U32(kFeSliderDst))))
            return false;
    }
    int32_t r = 0;
    if (!DefaultScreenInput(g, k, screen, &r)) return false;
    switch (g.U8(kFeEdges)) {
    case 0x10: case 0x20:
        if (g.U32(kFeSlider) != 0 && !k.Call(kSoundPreviewStop, nullptr, 0, nullptr)) return false;
        g.W32(kSoundPreviewDue, 1);
        break;
    case 0x40: case 0x80: {
        // The jump table 0x8005BDBC: slider kind 0..5 -> mixer channel.
        static const uint32_t kChannel[6] = {5, 3, 0, 1, 2, 4};
        const int32_t kind = g.S8(g.U32(kFeSlider) + 11u);
        if (static_cast<uint32_t>(kind) < 6u && !Call2(k, kVolumeSet, kChannel[kind], g.U32(g.U32(kFeSliderDst))))
            return false;
        const uint32_t slider = g.U32(kFeSlider);
        if (slider != 0 && !Call2(k, kSoundPreview, SignedByte(g.S8(slider + 11u)), g.U32(g.U32(kFeSliderDst))))
            return false;
        break;
    }
    case 1:
        if (r != 1) break;
        if (!k.Call(kSoundPreviewStop, nullptr, 0, nullptr)) return false;
        for (uint32_t i = 0; i < 7u; ++i) {
            const uint32_t v = g.U32(kSliderDescs + 16u * i + 12u);
            g.W32(kSoundTemplate + 16u * i + 12u, v);
            g.W32(kSession + 0xD4u + 4u * i, v);
            if (!Call2(k, kVolumeSet, i, v)) return false;
        }
        break;
    case 4: {
        if (r != 1) break;
        if (!k.Call(kSoundPreviewStop, nullptr, 0, nullptr)) return false;
        const uint8_t stereo = g.U8(kSoundSaved);
        g.W8(kSession + 20u, stereo);
        ChooserSet(g, Chooser(g, 28), static_cast<int8_t>(stereo));
        for (uint32_t i = 0; i < 7u; ++i) {
            const uint32_t v = g.U32(kSoundTemplate + 16u * i + 12u);
            g.W32(kSession + 0xD4u + 4u * i, v);
            if (!Call2(k, kVolumeSet, i, v)) return false;
        }
        break;
    }
    default: break;
    }
    *result = r;
    return true;
}

// ---------------------------------------------------------------------------- RASHCDF 0x8006A390
// Screen 49, controller options: the first frame saves every player's pad configuration +0x08 and
// vibration +0x17; a cancel that left restores them and re-points choosers 34/35 at the edited port's.
bool ControllerOptionsInput(GuestRam& g, ShellCallees& k, uint32_t screen, int32_t* result) {
    if (g.S16(screen + 2u) == 32767) {
        for (uint32_t i = 0; i < 6u; ++i) {
            g.W8(kPadCfgSaved + i, g.U8(kPlayers + 36u * i + 8u));
            g.W8(kPadVibSaved + i, g.U8(kPlayers + 36u * i + 23u));
        }
    }
    int32_t r = 0;
    if (!DefaultScreenInput(g, k, screen, &r)) return false;
    if (g.U8(kFeEdges) == 4u && r == 1) {
        for (uint32_t i = 0; i < 6u; ++i) {
            g.W8(kPlayers + 36u * i + 8u, g.U8(kPadCfgSaved + i));
            g.W8(kPlayers + 36u * i + 23u, g.U8(kPadVibSaved + i));
        }
        ChooserSet(g, Chooser(g, 34), g.S8(kPlayers + 36u * static_cast<uint32_t>(g.S8(kFePort)) + 8u));
        ChooserSet(g, Chooser(g, 35), g.S8(kPlayers + 36u * static_cast<uint32_t>(g.S8(kFePort)) + 23u));
    }
    *result = r;
    return true;
}

// ---------------------------------------------------------------------------- RASHCDF 0x8006A518
// Screen 51, multiplayer options: session+0x19 and +0x17 saved on the first frame, restored (with
// choosers 30 and 29) by a cancel that left.
bool MultiplayerOptionsInput(GuestRam& g, ShellCallees& k, uint32_t screen, int32_t* result) {
    if (g.S16(screen + 2u) == 32767) {
        g.W8(kMultiSaved + 0u, g.U8(kSession + 25u));
        g.W8(kMultiSaved + 1u, g.U8(kSession + 23u));
    }
    int32_t r = 0;
    if (!DefaultScreenInput(g, k, screen, &r)) return false;
    if (g.U8(kFeEdges) == 4u && r == 1) {
        const uint8_t a = g.U8(kMultiSaved + 0u);
        g.W8(kSession + 25u, a);
        ChooserSet(g, Chooser(g, 30), static_cast<int8_t>(a));
        const uint8_t b = g.U8(kMultiSaved + 1u);
        g.W8(kSession + 23u, b);
        ChooserSet(g, Chooser(g, 29), static_cast<int8_t>(b));
    }
    *result = r;
    return true;
}

// ---------------------------------------------------------------------------- RASHCDF 0x8006BF18
// (screen, chooser, padFirst, padEnd): Down steps the chooser to its next option, Up to its previous.
bool ChooserUpDown(GuestRam& g, ShellCallees& k, uint32_t chooser, int32_t padFirst, int32_t padEnd, int32_t* result) {
    int32_t r = 0;
    if (chooser != 0) {
        for (int32_t p = padFirst; p < padEnd && r == 0; ++p) {
            if (g.S8(Pad(p) + 0x32u) != 0) {
                if (!ChooserNext(g, k, chooser)) return false;
                Edge(g, 0x20u);
                r = 2;
            } else if (g.S8(Pad(p) + 0x2Au) != 0) {
                if (!ChooserPrev(g, k, chooser)) return false;
                Edge(g, 0x10u);
                r = 2;
            }
        }
    }
    *result = r;
    return true;
}

// ---------------------------------------------------------------------------- RASHCDF 0x8006BE84
// The jukebox's track chooser (kind 31, object table 0x8009CC40[31]): Up/Down pick the track, which
// is applied at once (fe+0x0E) and the track's on/off chooser 32 follows session+0xC0[track].
bool JukeboxChooserInput(GuestRam& g, ShellCallees& k, uint32_t screen, int32_t* result) {
    const int32_t first = g.S8(screen + 14u);
    int32_t r = 0;
    if (!ChooserUpDown(g, k, Chooser(g, 31), first, first + g.S8(screen + 15u), &r)) return false;
    if (g.U8(kFeEdges) & 0x30u) {
        if (!ChooserApply(g, k, Chooser(g, 31))) return false;
        ChooserSet(g, Chooser(g, 32), g.S8(kSession + 0xC0u + g.U8(kFeTrack)));
    }
    *result = r;
    return true;
}

// ---------------------------------------------------------------------------- RASHCDF 0x8006A610
// Screen 47, the jukebox: first frame - row cursor fe+0x06 = 2, both choosers on the current track,
// the 18 on/off bytes session+0xC0 saved. Then the track chooser; else Left/Right on the on/off
// chooser 32 (0x8006B800 with fe+0x15 set) applied at once; else the menu with fe+0x1E set (its
// Square arm then restarts the music). Up/Down move the row cursor (0..4); a cancel that left
// restores the 18 bytes and pushes each to SLUS 0x8002490C.
bool JukeboxInput(GuestRam& g, ShellCallees& k, uint32_t screen, int32_t* result) {
    if (g.S16(screen + 2u) == 32767) {
        g.W8(kFeJukeRow, 2);
        ChooserSet(g, Chooser(g, 31), g.U8(kFeTrack));
        ChooserSet(g, Chooser(g, 32), g.S8(kSession + 0xC0u + g.U8(kFeTrack)));
        for (uint32_t i = 0; i < 18u; ++i) g.W8(kJukeSaved + i, g.U8(kSession + 0xC0u + i));
    }
    int32_t r = 0;
    if (!JukeboxChooserInput(g, k, screen, &r)) return false;
    if (r == 0) {
        const uint32_t onOff = Chooser(g, 32);
        g.W8(kFeObjArm, 1);
        const int32_t first = g.S8(screen + 14u);
        if (!ObjectArms(g, k, onOff, first, first + g.S8(screen + 15u), &r)) return false;
        g.W8(kFeObjArm, 0);
        if (!ChooserApply(g, k, Chooser(g, 32))) return false;
        if (r == 0) {
            g.W8(kFeMusicRestart, 1);
            if (!DefaultScreenInput(g, k, screen, &r)) return false;
            g.W8(kFeMusicRestart, 0);
        }
    }
    switch (g.U8(kFeEdges)) {
    case 0x10: {
        const int8_t v = static_cast<int8_t>(g.U8(kFeJukeRow) - 1u);
        g.W8(kFeJukeRow, static_cast<uint8_t>(v));
        if (v < 0) g.W8(kFeJukeRow, 0);
        break;
    }
    case 0x20: {
        const int8_t v = static_cast<int8_t>(g.U8(kFeJukeRow) + 1u);
        g.W8(kFeJukeRow, static_cast<uint8_t>(v));
        if (!(v < 5)) g.W8(kFeJukeRow, 4);
        break;
    }
    case 4:
        if (r != 1) break;
        for (uint32_t i = 0; i < 18u; ++i) {
            const uint8_t v = g.U8(kJukeSaved + i);
            g.W8(kSession + 0xC0u + i, v);
            if (!Call2(k, kJukeboxSet, i, SignedByte(static_cast<int8_t>(v)))) return false;
        }
        break;
    default: break;
    }
    *result = r;
    return true;
}

// ---------------------------------------------------------------------------- RASHCDF 0x80069A8C
// Screen 52, the new-record name keyboard (Time Trial, after a race that beat the table's eighth
// time). Triangle leaves to the trophy room 26 without writing; Cross types the key cap, or on row 4:
// columns 0-1 cursor left, 2 cursor right, 3-4 space, 5-6 rub out, 7-8 enter (the record is inserted,
// then screen 26). Directions move the key cursor. It takes every frame.
bool NewRecordInput(GuestRam& g, ShellCallees& k, uint32_t screen, int32_t* result) {
    if (g.S16(screen + 2u) == 32767) KeyReset(g);
    int32_t r = 0;
    for (int32_t p = g.S8(screen + 14u); p < PadEnd(g, screen) && r == 0; ++p) {
        const uint32_t pad = Pad(p);
        if (g.S8(pad + 0x4Au) > 0) {
            if (!UiSound(k, 3)) return false;
            g.W16(kFeNext, 26);
            r = 1;
        } else if (g.S8(pad + 0x52u) > 0) {
            r = 2;
            if (g.U32(kKeyRow) != 4u) {
                KeyType(g);
            } else {
                switch (g.U32(kKeyCol)) {
                case 0: case 1: KeyCursorLeft(g); break;
                case 2: KeyCursorRight(g); break;
                case 3: case 4: KeySpace(g); break;
                case 5: case 6: KeyRubout(g); break;
                case 7: case 8: KeyEnterRecord(g); r = 1; break;
                default: break;
                }
            }
        } else if (KeyMove(g, pad)) {
            r = 2;
        }
    }
    *result = r == 0 ? 2 : r;
    return true;
}

// ---------------------------------------------------------------------------- RASHCDF 0x80069618
// Screen 56, the CODE keyboard (reached from the options menu, see OptionsInput): the same keyboard
// over a text that starts as "________" (0x8005BD5C). Triangle leaves to the back target, enter
// compares the text with the eight codes and sets the matching bit of 0x8005AE7C, then advances.
bool CodeEntryInput(GuestRam& g, ShellCallees& k, uint32_t screen, int32_t* result) {
    if (g.S16(screen + 2u) == 32767) {
        KeyReset(g);
        g.W32(kKeyBuf + 0u, g.U32(kKeyBlank + 0u));
        g.W32(kKeyBuf + 4u, g.U32(kKeyBlank + 4u));
        g.W8(kKeyBuf + 8u, g.U8(kKeyBlank + 8u));
    }
    int32_t r = 0;
    for (int32_t p = g.S8(screen + 14u); p < PadEnd(g, screen) && r == 0; ++p) {
        const uint32_t pad = Pad(p);
        if (g.S8(pad + 0x4Au) > 0) {
            if (!UiSound(k, 3)) return false;
            g.W16(kFeNext, Back(g, g.S16(screen + 6u)));
            r = 1;
        } else if (g.S8(pad + 0x52u) > 0) {
            r = 2;
            if (g.U32(kKeyRow) != 4u) {
                KeyType(g);
            } else {
                switch (g.U32(kKeyCol)) {
                case 0: case 1: KeyCursorLeft(g); break;
                case 2: KeyCursorRight(g); break;
                case 3: case 4: KeySpace(g); break;
                case 5: case 6: KeyRubout(g); break;
                case 7: case 8: KeyEnterCode(g, screen); r = 1; break;
                default: break;
                }
            }
        } else if (KeyMove(g, pad)) {
            r = 2;
        }
    }
    *result = r == 0 ? 2 : r;
    return true;
}

// ---------------------------------------------------------------------------- the dispatch
bool CallPortedScreenHandler(GuestRam& g, ShellCallees& k, uint32_t handler, uint32_t screen, int32_t* result,
                             bool* handled) {
    *handled = true;
    switch (handler) {
    case 0x8006AAF8: return LogoInput(g, screen, result);
    case 0x8006A838: return CreditsInput(g, k, screen, result);
    case 0x8006AD20: return MessagePanelInput(g, k, screen, result);
    case 0x80069440: return RaceOptionsInput(g, k, screen, result);
    case 0x80069560: return TrophyRoomInput(g, k, screen, result);
    case 0x8006AE6C: return OptionsInput(g, k, screen, result);
    case 0x8006AB58: return AbortModalInput(g, k, screen, result);
    case 0x80069FF0: return SoundOptionsInput(g, k, screen, result);
    case 0x8006A390: return ControllerOptionsInput(g, k, screen, result);
    case 0x8006A518: return MultiplayerOptionsInput(g, k, screen, result);
    case 0x8006A610: return JukeboxInput(g, k, screen, result);
    case 0x80069A8C: return NewRecordInput(g, k, screen, result);
    case 0x80069618: return CodeEntryInput(g, k, screen, result);
    case 0x8006BE84: return JukeboxChooserInput(g, k, screen, result);
    case 0x8006C06C: case 0x8006C124: case 0x8006C1DC: case 0x8006C290: // shell_memcard.h
        return CardScreenInput(g, k, handler, screen, result);
    default: *handled = false; return true;
    }
}

} // namespace rr::shell
