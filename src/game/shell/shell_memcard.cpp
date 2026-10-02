// The memory-card screens (shell_memcard.h). Each function names its original, whose disassembly it
// was checked against.
#include "game/shell/shell_memcard.h"

#include "game/shell/shell_text.h"

#include <cstring>
#include <initializer_list>
#include <vector>

namespace rr::shell {

namespace {

uint32_t U(int32_t v) { return static_cast<uint32_t>(v); }
uint32_t Card(GuestRam& g) { return g.U32(kCardPtr); }
uint32_t SlotRecord(GuestRam& g, int32_t slot) { return Card(g) + 2132u + U(484 * slot); }

bool Call(ShellCallees& k, uint32_t address, std::initializer_list<uint32_t> args, uint32_t* v0 = nullptr) {
    uint32_t a[12] = {};
    int n = 0;
    for (uint32_t v : args)
        if (n < 12) a[n++] = v;
    return k.Call(address, a, n, v0);
}

// SLUS 0x8001E08C, memcpy by bytes (a leaf).
void CopyBytes(GuestRam& g, uint32_t dst, uint32_t src, uint32_t n) {
    for (uint32_t i = 0; i < n; ++i) g.W8(dst + i, g.U8(src + i));
}

} // namespace

// ---------------------------------------------------------------------------- RASHCDF 0x8006CD48
// The previous state +0x0A follows the state, except while it names one of the states whose message
// the status panel keeps showing under state 21 (jump table 0x8005BE94: 1..3, 7..19, 24, 26 keep it).
void CardPrevState(GuestRam& g) {
    const uint32_t c = Card(g);
    const int32_t prev = g.S16(c + 10u);
    if (static_cast<uint32_t>(prev) < 27u) {
        switch (prev) {
        case 1: case 2: case 3: case 7: case 8: case 9: case 10: case 11: case 12: case 13: case 14:
        case 15: case 16: case 17: case 18: case 19: case 24: case 26:
            return;
        default: break;
        }
    }
    g.W16(c + 10u, g.U16(c + 8u));
}

// ---------------------------------------------------------------------------- RASHCDF 0x8006CDA4
void CardSaveSlot(GuestRam& g, int32_t slot) {
    g.W8(Card(g) + 544u, static_cast<uint8_t>(slot));
    const int32_t s = static_cast<int16_t>(slot);
    const uint32_t rec = SlotRecord(g, s);
    g.W32(rec, 0); // in use (0x8006CDFC, the delay slot of the first copy)
    CopyBytes(g, rec + 4u, kPlayers, 216u);
    CopyBytes(g, SlotRecord(g, s) + 220u, kSession, 256u);
}

// ---------------------------------------------------------------------------- RASHCDF 0x8006CE24
void CardSeedRecords(GuestRam& g) {
    const uint32_t c = Card(g);
    g.W8(c + 545u, 1);
    CopyBytes(g, c + 548u, kRecords, 1584u);
}

// ---------------------------------------------------------------------------- RASHCDF 0x8006D10C
int32_t CardRestoreRecords(GuestRam& g) {
    CopyBytes(g, kRecords, Card(g) + 548u, 1584u);
    for (int32_t p = 5; p >= 0; --p) g.W32(kPlayers + U(36 * p), 0x7FFFFFFFu);
    g.W8(kSession + 0x1Cu, 1);
    return 2;
}

// ---------------------------------------------------------------------------- RASHCDF 0x8006CE60
// The career of `slot` into the session and player records, then the choosers, the sound options and
// game_state as that career left them, and the screen it was saved on (session +0x10) as the next one.
bool CardLoadSlot(GuestRam& g, ShellCallees& k, int32_t slot, int32_t* result) {
    const uint32_t rec = SlotRecord(g, static_cast<int16_t>(slot));
    CopyBytes(g, kPlayers, rec + 4u, 216u);
    CopyBytes(g, kSession, rec + 220u, 256u);
    uint32_t chooser = 0;
    int32_t value = 0;
    switch (g.U32(kSession)) { // jump table 0x8005BF04 over mode - 1 < 32: 1 and 8 have arms
    case 1:
        chooser = g.U32(kChoosers + 0x38u);
        value = g.S8(kSession + 7u);
        break;
    case 8:
        ChooserSet(g, g.U32(kChoosers + 0x58u), g.S8(kSession + 8u));
        chooser = g.U32(kChoosers + 0x5Cu);
        value = g.S8(kPlayers + 7u);
        break;
    default:
        ChooserSet(g, g.U32(kChoosers + 0x0Cu), g.S8(kSession + 8u));
        chooser = g.S8(kPlayers + 9u) == 0 ? g.U32(kChoosers + 0x10u) : g.U32(kChoosers + 0x14u);
        value = g.S8(kPlayers + 7u);
        break;
    }
    ChooserSet(g, chooser, value);
    if (!k.Call1(kSoundMode, g.S8(kSession + 0x14u) == 0 ? 1u : 0u)) return false;
    for (uint32_t t = 0; t < 18u; ++t)
        if (!Call(k, kJukeboxSet, {t, U(g.S8(kSession + 0xC0u + t))})) return false;
    if (g.S8(kSession + 0xC0u + g.U8(kFeTrack)) == 0) {
        // 0x8006CFF4..0x8006D034: the first track whose jukebox byte is ZERO becomes the menu's.
        for (uint32_t t = 0; t < 18u; ++t) {
            if (g.S8(kSession + 0xC0u + t) != 0) continue;
            g.W8(kFeTrack, static_cast<uint8_t>(t));
            if (g.S8(kFeMusicOk) != 0 && !k.Call1(kMusicPlay, t & 0xFFu)) return false;
            break;
        }
    }
    for (uint32_t i = 0; i < 7u; ++i)
        if (!Call(k, 0x8007F20Cu, {i, g.U32(kSession + 0xD4u + 4u * i)})) return false;
    const uint32_t gs = g.U32(kGameStatePtr);
    g.W8(gs + 4u, g.U8(kSession));
    g.W32(gs + 48u, U(g.S8(kSession + 6u)));
    g.W32(gs + 64u, U(g.S8(kSession + 8u)));
    g.W32(gs + 60u, U(g.S8(kSession + 4u)));
    g.W16(gs + 58u, static_cast<uint16_t>(g.S8(kSession + 4u)));
    g.W32(gs + 72u, U(g.S8(kPlayers + 7u)));
    const uint32_t screen = g.U32(g.U32(kScreenTablePtr) + U(4 * g.S8(kSession + 16u)));
    g.W16(screen + 4u, static_cast<uint16_t>(g.S8(kSession + 13u)));
    g.W16(kFeNext, static_cast<uint16_t>(g.S8(kSession + 16u)));
    *result = 1;
    return true;
}

// ---------------------------------------------------------------------------- RASHCDF 0x8006C770
bool CardMachine(GuestRam& g, ShellCallees& k, uint32_t s, int32_t* result) {
    int32_t done = 0, event = -1;
    bool confirm = false;
    int32_t p = g.S8(s + 14u);
    if (p < p + g.S8(s + 15u)) {
        uint32_t pad = kPads + kPadStride * U(p);
        do {
            if (g.S8(pad + 74u) > 0) { // Triangle
                event = 2;
                done = 2;
            } else if (g.S8(pad + 66u) > 0) { // Circle
                event = 1;
                done = 2;
            } else if (g.S8(pad + 82u) > 0) { // Cross
                event = 0;
                confirm = true;
                done = 2;
            } else if (g.U8(pad + 42u) != 0 || g.U8(pad + 50u) != 0) { // Up / Down: the slot cursor
                const bool up = g.U8(pad + 42u) != 0;
                done = 2;
                if (g.S16(s + 6u) != 44) {
                    const uint32_t c = Card(g);
                    uint32_t sound = 4;
                    if (g.S16(c + 8u) == 15) {
                        sound = 0;
                        if (up) {
                            const uint16_t v = static_cast<uint16_t>(g.U16(c + 14u) - 1u);
                            g.W16(c + 14u, v);
                            if (static_cast<int16_t>(v) < 0) g.W16(c + 14u, 9);
                        } else {
                            const uint16_t v = static_cast<uint16_t>(g.U16(c + 14u) + 1u);
                            g.W16(c + 14u, v);
                            if (static_cast<int16_t>(v) >= 10) g.W16(c + 14u, 0);
                        }
                    }
                    if (!k.Call1(kUiSound, sound)) return false;
                }
            }
            ++p;
            pad += kPadStride;
        } while (p < g.S8(s + 14u) + g.S8(s + 15u) && done == 0);
    }
    if (event != -1) {
        const uint32_t c = Card(g);
        const uint32_t next = g.U32(kCardTransitions + 4u * U(g.S16(c + 8u) * 3 + event));
        uint32_t sound = 4;
        if (next != 0) {
            confirm = false;
            g.W16(c + 8u, static_cast<uint16_t>(next));
            sound = event == 2 ? 3u : 2u;
        }
        if (!k.Call1(kUiSound, sound)) return false;
    }
    {
        const uint32_t c = Card(g);
        const int16_t st = g.S16(c + 8u);
        if (st != 0 && st != 21) g.W16(c + 10u, g.U16(c + 8u));
    }
    const int16_t st = g.S16(Card(g) + 8u);
    switch (static_cast<uint32_t>(static_cast<int32_t>(st)) < 27u ? st : -1) { // jump table 0x8005BE24
    case 14:
    case 26:
        g.W16(kFeNext, 42);
        done = 1;
        break;
    case 7:
    case 12:
        break;
    case 8:
        g.W8(kSession + 0x18u, 0);
        g.W8(kCardRecordsSaved, 1);
        break;
    case 10:
        if (!k.Call1(kCardFormat, 10)) return false;
        break;
    case 16:
        if (!k.Call1(kCardFormat, 0)) return false;
        break;
    case 11:
        if (!k.Call1(kCardWrite, 17)) return false;
        break;
    case 13:
        if (g.S16(s + 6u) == 45) CardSaveSlot(g, g.S16(Card(g) + 14u));
        else CardSeedRecords(g);
        if (!k.Call1(kCardWrite, 19)) return false;
        break;
    case 17:
    case 19:
        if (!k.Call1(kCardWrite, 0)) return false;
        break;
    case 21:
        CardPrevState(g);
        if (!Call(k, kCardPoll, {})) return false;
        break;
    case 15: {
        g.W32(kCardClock, 0);
        if (!Call(k, kCardPoll, {})) return false;
        const int16_t id = g.S16(s + 6u);
        auto records = [&]() { // screen 44, 0x8006CBB8 / 0x8006CC78
            const uint32_t c = Card(g);
            if (g.S8(c + 545u) != 0) {
                g.W16(c + 8u, 7);
                done = CardRestoreRecords(g);
            } else {
                g.W16(c + 8u, 25);
            }
        };
        auto saveRecords = [&](bool click) -> bool { // screen 46, 0x8006CBE4 / 0x8006CCB4
            if (g.S8(kCardRecordsSaved) != 0 || g.S8(kCardRecordsAsked) == 1) {
                g.W16(Card(g) + 8u, 14);
                return true;
            }
            const uint32_t c = Card(g);
            if (click && !k.Call1(kUiSound, 2)) return false;
            if (g.S8(c + 545u) != 0) {
                g.W16(Card(g) + 8u, 3);
                g.W8(kCardRecordsAsked, 1);
            } else {
                g.W16(Card(g) + 8u, 13);
            }
            return true;
        };
        if (confirm) {
            if (id == 43) {
                const int32_t slot = g.S16(Card(g) + 14u);
                if (g.U32(SlotRecord(g, slot)) == 0) {
                    int32_t r = 0;
                    if (!CardLoadSlot(g, k, slot, &r)) return false;
                    done = r;
                    g.W16(Card(g) + 8u, 7);
                    if (!k.Call1(kUiSound, 2)) return false;
                } else if (!k.Call1(kUiSound, 4)) {
                    return false;
                }
            } else if (id == 45) {
                if (!k.Call1(kUiSound, 2)) return false;
                const uint32_t c = Card(g);
                g.W16(c + 8u, g.U32(SlotRecord(g, g.S16(c + 14u))) != 0 ? 13 : 3);
            } else if (id == 44) {
                records();
            } else if (id == 46) {
                if (!saveRecords(true)) return false;
            }
        } else if (id == 44) {
            records();
        } else if (id == 46) {
            if (!saveRecords(false)) return false;
        }
        break;
    }
    default:
        if (!Call(k, kCardPoll, {})) return false;
        break;
    }
    *result = done;
    return true;
}

// ---------------------------------------------------------------------------- the four screen handlers
// RASHCDF 0x8006C06C (43 Load Game), 0x8006C124 (44 Load Records), 0x8006C1DC (45 Save Game),
// 0x8006C290 (46 Save Records).
bool IsCardScreenHandler(uint32_t h) {
    return h == 0x8006C06Cu || h == 0x8006C124u || h == 0x8006C1DCu || h == 0x8006C290u;
}

bool CardScreenInput(GuestRam& g, ShellCallees& k, uint32_t handler, uint32_t s, int32_t* result) {
    g.W32(kIdle, 0);
    int32_t r = 0;
    if (g.S16(s + 2u) != 0) {
        if (handler == 0x8006C290u) {
            g.W8(kCardRecordsSaved, 0);
            g.W8(kCardRecordsAsked, 0);
        }
        if (!Call(k, kCardEnter, {})) return false;
        r = 2;
        g.W32(kCardHold, 120);
        const uint32_t c = Card(g);
        g.W8(c + 7u, 0);
        g.W16(c + 8u, 21);
        if (handler == 0x8006C06Cu || handler == 0x8006C124u) {
            g.W16(c + 14u, 0);
            g.W16(c + 10u, g.U16(c + 8u));
        } else {
            g.W16(c + 10u, 21);
            g.W16(c + 14u, 0);
        }
    } else if (g.U32(kCardHold) == 0) {
        if (!CardMachine(g, k, s, &r)) return false;
        if (r == 1) {
            if (!Call(k, kCardLeave, {})) return false;
            const uint32_t c = Card(g);
            g.W16(c + 8u, 0);
            g.W8(c + 7u, 0);
        }
    }
    *result = r;
    return true;
}

// ---------------------------------------------------------------------------- RASHCDF 0x80071F70
int32_t CardModeLabel(uint32_t mode) {
    switch (mode) {
    case 1: return 0x2D;
    case 4: return 0x2F;
    case 8: return 0x128;
    case 0x10: return 0x4B;
    case 0x11: return 0x4D;
    case 0x18: return 0x51;
    case 0x20: return 0x2B;
    default: return -1;
    }
}

// ---------------------------------------------------------------------------- RASHCDF 0x80071064
// Text kind 0x2D: the card screens' status panel - a frame (the panel emitter 0x8006ECDC) and the
// message of the state in BTN_FONT, one line per 2 x the font's height. Frame 104 bytes: the rectangle
// at entry-0x50, the panel's outer record at entry-0x48 and its inner one at entry-0x38 (+0x10 there).
bool CardStatusText(GuestRam& g, ShellCallees& k, uint32_t s, uint32_t r, int32_t state, uint32_t sp) {
    const uint32_t rect = sp - 0x50u, outer = sp - 0x48u, inner = sp - 0x38u;
    g.W32(rect, g.U32(r));
    g.W32(rect + 4u, g.U32(r + 4u));
    const uint16_t x = g.U16(rect), y = g.U16(rect + 2u), w = g.U16(rect + 4u), h = g.U16(rect + 6u);
    g.W16(sp - 0x28u, 0);
    g.W16(inner, 0);
    g.W16(inner + 2u, 1);
    g.W32(inner + 4u, 0x001E1414u);
    g.W16(inner + 8u, x);
    g.W16(inner + 12u, static_cast<uint16_t>(x + w));
    g.W16(inner + 10u, static_cast<uint16_t>(y - 20u));
    g.W16(inner + 14u, static_cast<uint16_t>(y + (h - 20u)));
    g.W16(outer, 0);
    g.W16(outer + 10u, static_cast<uint16_t>(y - 26u));
    g.W16(outer + 2u, 0);
    g.W32(outer + 4u, 0x001E1414u);
    g.W16(outer + 8u, static_cast<uint16_t>(x - 10u));
    g.W16(outer + 12u, static_cast<uint16_t>(x + w + 10u));
    g.W16(outer + 14u, static_cast<uint16_t>(y + (h - 20u) + 6u));
    const uint32_t font = kFontSlots + 0x18u * g.U32(kFontMain);
    g.W8(font + 3u, 0);
    const uint32_t layer = U(g.S8(kFeLayer)) * 4u;
    auto ot = [&](uint32_t extra) { return g.U32(kOtTable) + layer + extra; };
    auto panel = [&]() { return Call(k, kPanelEmit, {s, outer, ot(0x14u)}); };
    auto text = [&](uint32_t id) {
        return Call(k, kTextId, {g.U32(kFontMain), id, rect, ot(4u), g.U32(r + 12u), g.U16(r + 10u)});
    };
    auto str = [&](uint32_t p) {
        return Call(k, kTextStr, {g.U32(kFontMain), p, rect, ot(4u), g.U32(r + 12u), g.U16(r + 10u)});
    };
    auto step = [&](int mul) {
        const uint32_t f = kFontSlots + 0x18u * g.U32(kFontMain);
        const uint32_t d = mul == 2 ? static_cast<uint32_t>(g.S16(f + 0x14u) * 2) : g.U16(f + 0x14u);
        g.W16(rect + 2u, static_cast<uint16_t>(g.U16(rect + 2u) + d));
    };
    auto lines = [&](std::initializer_list<uint32_t> ids) {
        bool first = true;
        for (uint32_t id : ids) {
            if (!first) step(2);
            first = false;
            if (!text(id)) return false;
        }
        return true;
    };
    const int16_t screen = g.S16(s + 6u);
    switch (static_cast<uint32_t>(state) < 27u ? state : -1) {
    case 0: {
        const int32_t prev = g.S16(Card(g) + 10u);
        if (prev == 0) return true;
        return CardStatusText(g, k, s, r, prev, sp - 104u);
    }
    case 1: return panel() && lines({0x95, 0x96, 0x97});
    case 2: return panel() && lines({0x99, 0x9A});
    case 3:
        if (!panel()) return false;
        if (screen == 45 && !text(0xA5)) return false;
        if (screen == 46 && !text(0xA9)) return false;
        step(2);
        return lines({0xA6, 0x97});
    case 4: case 5: case 6: case 0x17: return panel() && lines({0x9F, 0x9D});
    case 7: return panel() && lines({0xA2, 0x9D});
    case 8:
        if (!panel()) return false;
        if (screen == 45 && !text(0xA4)) return false;
        if (screen == 46 && !text(0xA8)) return false;
        step(2);
        return text(0x9D);
    case 9: return panel() && lines({0x9C, 0x9D});
    case 10: case 0x10: return panel() && text(0x98);
    case 11: case 0x11: return panel() && text(0x9B);
    case 12: case 0x12: return panel() && text(0xA0);
    case 13: case 0x13:
        if (!panel()) return false;
        if (screen == 45) return text(0xA3);
        if (screen == 46) return text(0xA7);
        return true;
    case 0x14:
        if (!panel()) return false;
        if (static_cast<uint32_t>(g.U16(kFeCur)) - 43u < 2u) {
            if (!str(0x8005C288u)) return false; // the two strings of RASHCDF's data
            step(1);
            if (!str(0x8005C2A0u)) return false; // "Saved Data Detected"
        } else if (!str(0x8005C2B4u)) {          // "Memory Card Full"
            return false;
        }
        step(2);
        return text(0x9D);
    case 0x15: {
        if (!panel()) return false;
        const int32_t prev = g.S16(Card(g) + 10u);
        if (prev != 0x15) return CardStatusText(g, k, s, r, prev, sp - 104u);
        return text(0x93);
    }
    case 0x16: return panel() && lines({0x94, 0x9D});
    case 0x19: return panel() && lines({0x62E, 0x62F, 0x9D});
    default: return true;
    }
}

// ---------------------------------------------------------------------------- RASHCDF 0x80071AF0
// Text kind 0x2E: in state 15 the ten career slots - rank, mode, venue rank and "n of 6|9" of each,
// "Empty" for a free one, the selected slot highlighted. Frame 136: the rectangle at entry-112 and the
// sprintf buffer at entry-104.
bool CardSlotsText(GuestRam& g, ShellCallees& k, uint32_t r, int32_t state, uint32_t sp) {
    if (state != 15) return true;
    const uint32_t rect = sp - 112u, buf = sp - 104u;
    const uint32_t ot = g.U32(kOtTable) + U(g.S8(kFeLayer)) * 4u + 4u;
    g.W32(rect, g.U32(r));
    g.W32(rect + 4u, g.U32(r + 4u));
    g.W8(kFontSlots + 0x18u * g.U32(kFontHdr) + 3u, 0);
    auto at = [&](uint32_t dx) { g.W16(rect, static_cast<uint16_t>(g.U16(r) + dx)); };
    static const uint32_t kHeads[4] = {172, 173, 174, 175}, kCols[4] = {0, 120, 220, 340};
    for (int i = 0; i < 4; ++i) {
        at(kCols[i]);
        if (!Call(k, kTextId, {g.U32(kFontHdr), kHeads[i], rect, ot, g.U32(r + 12u), g.U16(r + 10u)})) return false;
    }
    g.W16(rect + 2u, static_cast<uint16_t>(g.U16(rect + 2u) + 20u));
    for (int32_t row = 0; row < 10; ++row) {
        const uint32_t c = Card(g);
        const uint32_t rgb = row == g.S16(c + 14u) ? 0xB9A096u : 0x286EB4u;
        const uint32_t rec = c + 2132u + U(484 * row);
        auto id = [&](uint32_t sid, uint32_t dx) {
            at(dx);
            return Call(k, kTextId, {g.U32(kFontMain), sid, rect, ot, rgb, g.U16(r + 10u)});
        };
        if (g.U32(rec) != 0) {
            if (!id(170, 0) || !id(171, 120) || !id(171, 220) || !id(171, 340)) return false;
        } else {
            const uint32_t players = rec + 4u, session = rec + 220u;
            const int32_t mode = g.S32(session);
            int32_t rank = 295;
            uint32_t of = 6;
            if (mode == 1) {
                rank = 294;
            } else if (mode == 32) {
                rank = static_cast<int16_t>(RankNameId(g.S8(players + 9u), g.S8(players + 10u)));
                of = 9;
            }
            if (rank >= 0 && !id(U(rank), 0)) return false;
            const int32_t label = CardModeLabel(g.U32(session));
            if (label >= 0 && !id(U(label), 120)) return false;
            const int32_t venue = ModeNameId(g.S8(session + 4u), g.U32(session));
            if (venue >= 0 && !id(U(venue), 220)) return false;
            if (!Call(k, kSprintf, {buf, 0x8005C338u, g.U8(session + 17u), g.U32(g.U32(kStringTable) + 4u), of}))
                return false;
            at(340);
            if (!Call(k, kTextStr, {g.U32(kFontMain), buf, rect, ot, rgb, g.U16(r + 10u)})) return false;
        }
        g.W16(rect + 2u, static_cast<uint16_t>(g.U16(rect + 2u) + 12u));
    }
    return true;
}

// ---------------------------------------------------------------------------- the product's card layer
namespace {
constexpr uint32_t kFormatBusy = 0x80081098, kFormatDone = 0x8008109C, kFormatTries = 0x800810A0; // 0x8005ED6C
constexpr uint32_t kWriteBusy = 0x8008108C, kWriteDone = 0x80081090, kWriteTries = 0x80081094;    // 0x8005EC28
constexpr uint32_t kBlockBytes = 8192;
} // namespace

bool CardDevice::Ensure() {
    if (have_) return true;
    std::string error;
    CardImage c;
    if (!path_.empty() && LoadCardFile(path_, c, error)) {
        image_ = c;
        formatted_ = true;
    } else {
        std::FILE* f = path_.empty() ? nullptr : std::fopen(path_.c_str(), "rb");
        if (f != nullptr) { // a file that is not a card: an unformatted card
            std::fclose(f);
            image_ = CardImage{};
            formatted_ = false;
        } else { // no file: a blank formatted card, written on the first save
            image_ = FormatCard();
            formatted_ = true;
        }
    }
    image_.block = FindGameBlock(image_);
    have_ = true;
    status_ = formatted_ ? 0 : -1;
    return true;
}

// 0x8005F158 = 0x8005EB80 (the record) + 0x8005E068 (the driver: ours). The file is read again on
// every entry, so a card written by another program in between is seen.
void CardDevice::Enter(GuestRam& g) {
    g.W32(kCardPtr, kCardRecord);
    for (uint32_t i = 0; i < kCardRecordSize; ++i) g.W8(kCardRecord + i, 0);
    g.W32(kCardRecord + 0x10u, 0xFFFFFFD6u);
    for (uint32_t i = 0; i < 0x200u; i += 4u) g.W32(kCardRecord + kCardBlockAt + i, g.U32(0x80080E8Cu + i));
    for (uint32_t s = 0; s < 10u; ++s) g.W32(kCardRecord + 0x854u + 484u * s, 1);
    have_ = false;
    formatting_ = writing_ = failed_ = false;
    Ensure();
}

// 0x8005EEF8 over the product's card: the read and the ten slot checks (0x8005EAB4 with its clear)
// happen at once, where the console needs a few polls (status -4 while reading).
uint8_t CardDevice::Status(GuestRam& g) {
    const uint32_t c = kCardRecord;
    if (status_ == g.S32(c + 0x10u)) return g.U8(c);
    auto flags = [&](uint8_t f0, uint8_t f1, uint8_t f2, uint8_t f3, uint8_t f4, uint8_t f5) {
        const uint8_t v[6] = {f0, f1, f2, f3, f4, f5};
        for (uint32_t i = 0; i < 6u; ++i) g.W8(c + i, v[i]);
    };
    if (status_ == -1) {
        flags(1, 0, 0, 0, 0, 0);
    } else {
        flags(1, 0, 1, 0, 0, 0);
        image_.block = FindGameBlock(image_);
        if (image_.block <= 0) {
            bool room = false;
            for (int f = 1; f <= 15 && !room; ++f) room = (image_.raw[static_cast<size_t>(f) * 128u] & 0xF0u) == 0xA0u;
            g.W8(c + (room ? 5u : 4u), 1);
        } else {
            g.WriteBlock(c + kCardBlockAt, image_.raw.data() + static_cast<size_t>(image_.block) * kBlockBytes,
                         kCardBlockPayload);
            bool bad = false;
            for (int32_t s = 0; s < 10; ++s) { // 0x8005EAB4(s, 1)
                const uint32_t rec = c + 0x854u + U(484 * s);
                std::vector<uint8_t> host(484);
                g.ReadBlock(rec, host.data(), 484u);
                uint32_t a = 0, b = 0;
                CardChecksumHost(host.data(), s, &a, &b);
                if (a != g.U32(rec + 0x1DCu) || b != g.U32(rec + 0x1E0u)) {
                    for (uint32_t i = 0; i < 484u; ++i) g.W8(rec + i, 0);
                    g.W32(rec, 1);
                    bad = true;
                }
            }
            if (bad) g.W8(c + 6u, 1);
            flags(1, 1, 1, 1, 0, 1);
        }
    }
    g.W32(c + 0x10u, U(status_));
    return g.U8(c);
}

// 0x8005F21C: the status, then the state it implies unless a message is being shown.
void CardDevice::Poll(GuestRam& g) {
    Status(g);
    const uint32_t c = kCardRecord;
    const uint16_t st = g.U16(c + 8u);
    if (static_cast<uint16_t>(st - 4u) <= 2u || st == 8 || st == 7 || st == 9 || st == 0x19) return;
    if ((st == 0x17 || st == 3) && g.U8(c) == 1) return;
    uint16_t next;
    if (g.U8(c + 1u) == 1) {
        if (g.U8(c + 6u) == 1) {
            g.W8(c + 6u, 0);
            g.W32(kCardClock, 0);
            g.W16(c + 8u, 0x17);
            return;
        }
        next = 0x0F;
    } else if (g.U8(c) == 0) {
        next = 0x16;
    } else if (g.U8(c) == 2) {
        next = 0x15;
    } else if (g.U8(c + 2u) == 0) {
        next = 1;
    } else if (g.U8(c + 4u) == 1) {
        next = 0x14;
    } else {
        next = 2;
    }
    g.W16(c + 8u, next);
    g.W32(kCardClock, 0);
}

// 0x8005ED6C: format (10 starts it, 0 polls it).
void CardDevice::Format(GuestRam& g, uint32_t op) {
    const uint32_t c = kCardRecord;
    if (op == 0 && !formatting_) {
        if (status_ == -1) return; // still unformatted: the console keeps polling
        g.W32(kFormatDone, 1);
        g.W32(kFormatTries, 0);
        g.W32(kCardClock, 0);
        g.W32(kFormatBusy, 0);
        g.W32(c + 0x10u, 0xFFFFFFFEu);
        g.W16(c + 8u, 0x15);
        return;
    }
    formatting_ = true;
    g.W32(kFormatBusy, 1);
    g.W32(kFormatDone, 0);
    g.W32(kFormatTries, 5);
    formatting_ = false;
    g.W32(kFormatBusy, 0);
    g.W32(kCardClock, 0);
    std::string error;
    if (status_ == -1) {
        image_ = FormatCard();
        if (!path_.empty() && !SaveCardFile(path_, image_, error)) {
            note_ = "card: format failed: " + error;
            g.W32(kFormatDone, 1);
            g.W16(c + 8u, 6);
            return;
        }
        formatted_ = true;
        status_ = 0;
        note_ = "card: formatted " + path_;
        g.W16(c + 8u, 0x10);
    } else {
        g.W32(kFormatDone, 1);
        g.W16(c + 8u, 6);
    }
}

// 0x8005EC28: write the save block (0x11 creates the file, 0x13 saves; 0 polls). 0x8005F1DC seals the
// ten records (0x8005EA54) and writes the block from +0x20; OURS: the block's first 6948 bytes (what
// the game's own read asks for) - the rest of the 8 KiB, which the console fills from whatever follows
// in its memory, is left as the card has it.
void CardDevice::Write(GuestRam& g, uint32_t op) {
    const uint32_t c = kCardRecord;
    if (op == 0 && !writing_) {
        g.W32(c + 0x10u, 0xFFFFFFFEu);
        const bool create = g.U32(kCardOp) == 0x11u;
        g.W16(c + 8u, failed_ ? (create ? 5 : 4) : (create ? 9 : 8));
        g.W32(kWriteBusy, 0);
        g.W32(kWriteDone, 1);
        g.W32(kWriteTries, 0);
        g.W32(kCardClock, 0);
        return;
    }
    writing_ = true;
    g.W32(kWriteBusy, 1);
    g.W32(kWriteDone, 0);
    g.W32(kWriteTries, 3);
    g.W32(kCardOp, op);
    for (int32_t s = 0; s < 10; ++s) { // 0x8005EA54
        const uint32_t rec = c + 0x854u + U(484 * s);
        std::vector<uint8_t> host(484);
        g.ReadBlock(rec, host.data(), 484u);
        uint32_t a = 0, b = 0;
        CardChecksumHost(host.data(), s, &a, &b);
        g.W32(rec + 0x1DCu, a);
        g.W32(rec + 0x1E0u, b);
    }
    failed_ = false;
    std::string error;
    Ensure();
    image_.block = FindGameBlock(image_);
    if (image_.block <= 0) { // 0x8005E8F0: a new file - the directory frame 0x8005E858 in the first free block
        int free = -1;
        for (int f = 1; f <= 15 && free < 0; ++f)
            if ((image_.raw[static_cast<size_t>(f) * 128u] & 0xF0u) == 0xA0u) free = f;
        if (free < 0) {
            failed_ = true;
            error = "the card has no free block";
        } else {
            uint8_t* d = image_.raw.data() + static_cast<size_t>(free) * 128u;
            std::memset(d, 0, 128);
            d[0] = 0x51;
            d[5] = 0x20;
            d[8] = d[9] = 0xFF;
            std::memcpy(d + 10, kCardFileName, sizeof(kCardFileName) - 1);
            uint8_t x = 0;
            for (int i = 0; i < 127; ++i) x ^= d[i];
            d[127] = x;
            std::memset(image_.raw.data() + static_cast<size_t>(free) * kBlockBytes, 0, kBlockBytes);
            image_.block = free;
        }
    }
    if (!failed_) {
        g.ReadBlock(c + kCardBlockAt, image_.raw.data() + static_cast<size_t>(image_.block) * kBlockBytes, kCardBlockPayload);
        if (!path_.empty() && !SaveCardFile(path_, image_, error)) failed_ = true;
    }
    note_ = failed_ ? "card: write failed: " + error
                    : std::string("card: ") + (op == 0x11 ? "created " : "saved ") + kCardFileName + " in block " +
                          std::to_string(image_.block) + " of " + path_;
    writing_ = false;
    g.W32(kWriteBusy, 0);
    g.W32(kCardClock, 0);
    g.W16(c + 8u, static_cast<uint16_t>(op));
}

bool CardDevice::Call(GuestRam& g, uint32_t address, const uint32_t* args, int count, uint32_t* v0, bool* handled) {
    *handled = true;
    if (v0 != nullptr) *v0 = 0;
    const uint32_t a0 = (count > 0 && args != nullptr) ? args[0] : 0u;
    switch (address) {
    case kCardEnter: Enter(g); return true;
    case kCardLeave: return true;
    case kCardPoll: Ensure(); Poll(g); return true;
    case kCardFormat: Ensure(); Format(g, a0); return true;
    case kCardWrite: Ensure(); Write(g, a0); return true;
    default: *handled = false; return true;
    }
}

} // namespace rr::shell
