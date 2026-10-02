// The in-race pause, ported from our own disassembly (SLUS_010.53 SHA-1
// 67ed165a2c517d4e6106fb0dfa324d66dd9a76f1) - see pause.h.
#include "game/sim/pause.h"

namespace rr::sim {
namespace {

constexpr uint32_t kGsPtr = 0x8005B2F8;
constexpr uint32_t kMask24 = 0x00FFFFFFu;
constexpr uint32_t kGreen = 0x348A32, kGrey = 0x505A50, kRed = 0x2B2B6F, kGold = 0x14506E, kCdBlue = 0x2020AA;

int16_t S16v(uint32_t v) { return static_cast<int16_t>(static_cast<uint16_t>(v)); }

// libgpu AddPrim SLUS 0x8004CDA4 (inlined by the menu's own box links as `| len << 24`).
void AddPrim(GuestRam& g, uint32_t ot, uint32_t p) {
    g.W32(p, (g.U32(p) & 0xFF000000u) | (g.U32(ot) & kMask24));
    g.W32(ot, (g.U32(ot) & 0xFF000000u) | (p & kMask24));
}

// The two SLUS-data boxes: `*prim = *slot | 0x05000000; *slot = prim & 0xFFFFFF` (0x8002D764 / 0x8002DA28).
void LinkBox(GuestRam& g, uint32_t prim, uint32_t slot) {
    g.W32(prim, g.U32(slot) | 0x05000000u);
    g.W32(slot, prim & kMask24);
}

// hud.cpp's HeapTake, as 0x800400F4 inlines it (0x80040140..0x80040188).
bool HeapTake(GuestRam& g, HudCallees& c, uint32_t sp, uint32_t bytes, uint32_t* at) {
    uint32_t heap = g.U32(kHudHeapPtr);
    const uint32_t next = g.U32(heap + 268u);
    if (!(next + bytes < g.U32(kHudHeapEnd))) {
        uint32_t answer = 0;
        if (!c.HeapOverflow(sp, next, bytes, &answer)) return false;
        heap = g.U32(kHudHeapPtr);
        g.W32(heap + 268u, answer);
    }
    heap = g.U32(kHudHeapPtr);
    *at = g.U32(heap + 268u);
    g.W32(heap + 268u, *at + bytes);
    return true;
}

int32_t Font(GuestRam& g) { return g.S32(kHudFontIndex); }
uint32_t FrameOt(GuestRam& g) { return g.U32(kPauseFrameOtPtr); }

// The resume both menu arms share (0x8002DC60 / 0x8002DCC4, then 0x8002DCF8).
bool Resume(GuestRam& g, PauseCallees& c, uint32_t frame) {
    const uint32_t gs = g.U32(kGsPtr);
    g.W8(kPauseModalWhich, 0);
    g.W8(kPauseModalUp, 0);
    g.W32(kPausePadIndex, 0);
    g.W16(gs + 0x2Au, 0);
    g.W8(gs, 1);
    return c.PlaySound(frame, 107) && c.RaceOverSignal(frame, 0) && c.SoundHold(frame, 0);
}

} // namespace

// ---------------------------------------------------------------------------- the pad poll
uint32_t PadPauseTest(GuestRam& g) {
    uint32_t gs = g.U32(kGsPtr);
    const int8_t st = g.S8(gs);
    if (st == 2) return 0;                                              // 0x8001CC00: leaves the poll
    if (st == 1 && g.U32(kPausePadLost) != 0) {                         // 0x8001CC18..0x8001CC34
        g.W8(gs, 3);
        return 1;
    }
    uint32_t s0 = 0;                                                    // 0x8001CC44
    gs = g.U32(kGsPtr);
    if (g.U32(gs + 0x34u) == 0) return 0;                               // 0x8001CC50
    uint32_t rec = kPausePadRecords;
    for (uint32_t s4 = 0;;) {
        const uint32_t idx = g.U32(g.U32(rec + 0xB8u) + 4u);            // control 1 through the remap
        if (g.S8(rec + (idx << 3) + 26u) != 0) {                        // its slot's press code
            if ((g.U32(gs) & 0xFF0000FFu) == 1u) {                      // racing, not the first frame
                g.W8(gs, 3);
                g.W32(kPausePadIndex, s4);
                s0 = 1;
            } else if (g.S8(gs) == 6) {
                g.W32(kPauseResultsKey, 1);
            }
        }
        gs = g.U32(kGsPtr);                                             // 0x8001CCD8
        ++s4;
        rec += 192u;
        if (!(s4 < g.U32(gs + 0x34u))) break;
    }
    return s0;
}

// ---------------------------------------------------------------------------- GameFrame
bool GameFrameStall(GuestRam& g, PauseCallees& c, uint32_t sp) {
    const uint32_t gs = g.U32(kGsPtr);
    if (g.S16(gs + 0x28u) != 0) {                                       // 0x80011C84
        if (g.U32(kPauseDemo) != 0) {                                   // the attract run goes to the shell
            g.W8(gs, 2);
            g.W16(g.U32(kGsPtr) + 0x28u, 0);
            return !g.Faulted();
        }
        if (g.S8(gs) == 4) return !g.Faulted();
        g.W8(gs + 1u, g.U8(gs));                                        // the state it leaves
        g.W8(g.U32(kGsPtr), 4);
        const uint32_t gs3 = g.U32(kGsPtr);
        const uint32_t clock = g.U32(gs3 + 0x0Cu);
        g.W16(gs3 + 0x2Au, 0);
        g.W32(gs3 + 0x2Cu, clock);                                      // the stall's start
        return c.RaceOverSignal(sp, 1) && c.SoundHold(sp, 1) && !g.Faulted();
    }
    if (g.S8(gs) != 4) return !g.Faulted();                             // 0x80011D0C
    g.W16(gs + 0x2Au, 1);                                               // "CD ERROR RECOVERED."
    if (g.S8(gs + 1u) == 1) {                                           // a racing game: into the pause menu
        g.W8(gs, 3);
        return c.RaceOverSignal(sp, 1) && c.SoundHold(sp, 1) && !g.Faulted();
    }
    g.W8(gs, g.U8(gs + 1u));
    if (!c.RaceOverSignal(sp, g.S8(g.U32(kGsPtr) + 1u) == 3 ? 1u : 0u)) return false;
    return c.SoundHold(sp, g.S8(g.U32(kGsPtr) + 1u) == 3 ? 1u : 0u) && !g.Faulted();
}

// ---------------------------------------------------------------------------- the leaves
uint32_t MenuUpDown(GuestRam& g) {
    const uint32_t rec = kPausePadRecords + ((g.U32(kPausePadIndex) * 3u) << 6);
    if (g.S8(rec + 122u) > 0 && g.U32(kPausePadLost) == 0) return 7;   // Start
    if (g.S8(rec + 42u) != 0) {                                         // Up
        const uint32_t m = g.U32(kPauseMenuPtr);
        const uint32_t v = (g.U16(m + 2u) - 1u) & 0xFFFFu;
        g.W16(m + 2u, static_cast<uint16_t>(v));
        if (S16v(v) < 0) g.W16(m + 2u, static_cast<uint16_t>(g.U16(m) - 1u));
        return 5;
    }
    if (g.S8(rec + 50u) != 0) {                                         // Down
        const uint32_t m = g.U32(kPauseMenuPtr);
        const uint32_t v = (g.U16(m + 2u) + 1u) & 0xFFFFu;
        g.W16(m + 2u, static_cast<uint16_t>(v));
        if (!(S16v(v) < g.S16(m))) g.W16(m + 2u, 0);
        return 5;
    }
    if (g.S8(rec + 82u) > 0) return 4;                                  // Cross
    if (g.S8(rec + 74u) > 0) return 6;                                  // Triangle
    return 2;
}

uint32_t MenuLeftRight(GuestRam& g) {
    const uint32_t rec = kPausePadRecords + ((g.U32(kPausePadIndex) * 3u) << 6);
    if (g.S8(rec + 122u) > 0) return 7;                                 // Start
    if (g.S8(rec + 26u) != 0) {                                         // Left
        const uint32_t m = g.U32(kPauseMenuPtr);
        const uint32_t v = (g.U16(m + 2u) - 1u) & 0xFFFFu;
        g.W16(m + 2u, static_cast<uint16_t>(v));
        if (S16v(v) < 0) g.W16(m + 2u, static_cast<uint16_t>(g.U16(m) - 1u));
        return 5;
    }
    if (g.S8(rec + 34u) != 0) {                                         // Right
        const uint32_t m = g.U32(kPauseMenuPtr);
        const uint32_t v = (g.U16(m + 2u) + 1u) & 0xFFFFu;
        g.W16(m + 2u, static_cast<uint16_t>(v));
        if (!(S16v(v) < g.S16(m))) g.W16(m + 2u, 0);
        return 5;
    }
    if (g.S8(rec + 82u) > 0) return 4;
    if (g.S8(rec + 74u) > 0) return 6;
    return 2;
}

int32_t StringWidthById(GuestRam& g, int32_t font, int32_t id) {
    return HudStringWidth(g, font, g.U32(g.U32(kHudStringTable) + 4u * static_cast<uint32_t>(id)));
}

uint32_t GetDrawEnv(GuestRam& g, uint32_t out) {
    for (uint32_t k = 0; k < 0x5Cu; ++k) g.W8(out + k, g.U8(kPauseDrawEnv + k));
    return out;
}

// ---------------------------------------------------------------------------- drawing
bool TextInBox(GuestRam& g, PauseCallees& c, uint32_t sp, int32_t font, int32_t id, uint32_t rect, uint32_t ot,
               uint32_t colour, int32_t mode) {
    const uint32_t frame = sp - 48u;
    const uint32_t slot = 4u * static_cast<uint32_t>(id);
    if (mode == 0)
        return HudDrawString(g, c, frame, font, g.U32(g.U32(kHudStringTable) + slot), g.S16(rect), g.S16(rect + 2u), ot,
                             colour);
    if (mode == 1 || mode == 2) {
        const int32_t w = HudStringWidth(g, font, g.U32(g.U32(kHudStringTable) + slot));
        uint32_t x;
        if (mode == 1) {
            x = g.U16(rect) + g.U16(rect + 4u) - static_cast<uint32_t>(w);
        } else {
            const int32_t half = static_cast<int32_t>(static_cast<int16_t>(g.U16(rect + 4u))) >> 1;
            x = g.U16(rect) + static_cast<uint32_t>(half) - static_cast<uint32_t>(w >> 1);
        }
        return HudDrawString(g, c, frame, font, g.U32(g.U32(kHudStringTable) + slot), S16v(x), g.S16(rect + 2u), ot,
                             colour);
    }
    if (mode == 3 && (g.U32(kPauseOverlayMask) & 0x10u) != 0) {        // RASHCDF's word wrap 0x800662CC
        const uint32_t args[6] = {static_cast<uint32_t>(font), static_cast<uint32_t>(id), rect, 0, ot, colour};
        return c.Unported(frame, 0x800662CCu, args, 6);
    }
    return !g.Faulted();
}

bool BoxFrame(GuestRam& g, PauseCallees& c, uint32_t sp, uint32_t r, uint32_t gg, uint32_t b, uint32_t ot,
              int32_t x0, int32_t y0, int32_t x1, int32_t y1) {
    const uint32_t frame = sp - 56u;
    uint32_t p = 0;
    if (!HeapTake(g, c, frame, 0x60u, &p)) return false;
    const uint32_t L = static_cast<uint32_t>(x0 - 4), R = static_cast<uint32_t>(x1 + 4);
    const uint32_t T = static_cast<uint32_t>(y0 - 3), B = static_cast<uint32_t>(y1 + 3);
    const uint32_t X0 = static_cast<uint32_t>(x0), Y0 = static_cast<uint32_t>(y0);
    const uint32_t X1 = static_cast<uint32_t>(x1), Y1 = static_cast<uint32_t>(y1);
    // {x, y} of the four corners of each band, in the packet's vertex order.
    const uint32_t bands[4][8] = {{L, T, R, T, L, Y0, R, Y0},        // above
                                  {L, B, R, B, L, Y1, R, Y1},        // below
                                  {L, Y0, X0, Y0, L, Y1, X0, Y1},    // left
                                  {X1, Y0, R, Y0, X1, Y1, R, Y1}};   // right
    for (uint32_t k = 0; k < 4; ++k) {
        const uint32_t q = p + 24u * k;
        g.W8(q + 3u, 5);
        g.W8(q + 7u, 0x28);
        for (uint32_t v = 0; v < 8; ++v) g.W16(q + 8u + 2u * v, static_cast<uint16_t>(bands[k][v]));
        g.W8(q + 4u, static_cast<uint8_t>(r));
        g.W8(q + 5u, static_cast<uint8_t>(gg));
        g.W8(q + 6u, static_cast<uint8_t>(b));
        g.W8(q + 7u, static_cast<uint8_t>(g.U8(q + 7u) | 2u));        // SetSemiTrans 0x8004CDE4(q, 1)
        AddPrim(g, ot, q);                                             // 0x8004CDA4
    }
    return !g.Faulted();
}

bool PauseDraw(GuestRam& g, PauseCallees& c, uint32_t sp) {
    const uint32_t frame = sp - 56u, rect = frame + 24u;
    const uint32_t ot = FrameOt(g);
    LinkBox(g, kPauseBoxPrim, ot + 0x18u);
    g.W16(rect, 86);
    g.W16(rect + 2u, 72);
    g.W16(rect + 4u, 212);
    g.W16(rect + 6u, 110);
    if (!TextInBox(g, c, frame, Font(g), 0, rect, ot + 0x14u, kGold, 2)) return false;
    g.W16(rect + 2u, static_cast<uint16_t>(g.U16(rect + 2u) + 22u));
    const int16_t cur = g.S16(kPauseMainMenu + 2u);
    uint32_t col[3];
    if (cur == 0) col[0] = kGreen, col[1] = kGrey, col[2] = kGrey;
    else if (cur == 1) col[0] = kGrey, col[1] = kRed, col[2] = kGrey;
    else if (cur == 2) col[0] = kGrey, col[1] = kGrey, col[2] = kGold;
    else return !g.Faulted();
    for (int32_t k = 0; k < 3; ++k) {
        if (k > 0) g.W16(rect + 2u, static_cast<uint16_t>(g.U16(rect + 2u) + 15u));
        if (!TextInBox(g, c, frame, Font(g), 2 + k, rect, FrameOt(g) + 0x14u, col[k], 2)) return false;
    }
    return !g.Faulted();
}

bool ConfirmDraw(GuestRam& g, PauseCallees& c, uint32_t sp, int32_t id) {
    const uint32_t frame = sp - 40u;
    LinkBox(g, kPauseAskPrim, FrameOt(g) + 0x10u);
    if (!HudDrawText(g, c, frame, Font(g), id, 0x60, 0x9B, FrameOt(g) + 0x0Cu, kGold)) return false;
    const int32_t w = StringWidthById(g, Font(g), id);
    const int16_t cur = g.S16(g.U32(kPauseMenuPtr) + 2u);
    uint32_t yes, no;
    if (cur == 0) yes = kGrey, no = kGreen;
    else if (cur == 1) yes = kRed, no = kGrey;
    else return !g.Faulted();
    if (!HudDrawText(g, c, frame, Font(g), 9, S16v(static_cast<uint32_t>(w + 0x66)), 0x9B, FrameOt(g) + 0x0Cu, yes))
        return false;
    const uint32_t x = static_cast<uint32_t>(w) + 106u + static_cast<uint32_t>(StringWidthById(g, Font(g), 9));
    if (!HudDrawString(g, c, frame, Font(g), kPauseSlash, S16v(x), 0x9B, FrameOt(g) + 0x0Cu, kGrey)) return false;
    return HudDrawText(g, c, frame, Font(g), 10, S16v(x + 4u), 0x9B, FrameOt(g) + 0x0Cu, no) && !g.Faulted();
}

// ---------------------------------------------------------------------------- the menu's input
bool PauseInput(GuestRam& g, PauseCallees& c, uint32_t sp, uint32_t* v0) {
    const uint32_t frame = sp - 32u;
    uint32_t s1 = MenuUpDown(g);
    bool ok = true;
    switch (s1) {
    case 4: {
        const int16_t cur = g.S16(kPauseMainMenu + 2u);
        if (cur == 1) {                                                 // QUIT: its YES / NO, at NO
            g.W8(kPauseModalWhich, 1);
            ok = c.PlaySound(frame, 60);
            g.W16(kPauseQuitMenu + 2u, 0);
        } else if (cur == 2) {                                          // RESTART: the same
            g.W8(kPauseModalWhich, 2);
            ok = c.PlaySound(frame, 60);
            g.W16(kPauseRestartMenu + 2u, 0);
        } else {                                                        // RESUME
            s1 = 3;
            ok = Resume(g, c, frame);
        }
        break;
    }
    case 5:
        ok = c.PlaySound(frame, 107);
        break;
    case 6:
    case 7:                                                             // Triangle / Start: resume
        s1 = 3;
        ok = Resume(g, c, frame);
        break;
    default:
        break;
    }
    *v0 = s1;
    return ok && !g.Faulted();
}

bool ConfirmInput(GuestRam& g, PauseCallees& c, uint32_t sp, uint32_t* v0) {
    const uint32_t frame = sp - 24u;
    uint32_t s0 = MenuLeftRight(g);
    bool ok = true;
    switch (s0) {
    case 4: {
        const int16_t cur = g.S16(g.U32(kPauseMenuPtr) + 2u);
        if (cur == 0) {                                                 // NO
            s0 = 0;
            ok = c.PlaySound(frame, 73);
        } else if (cur == 1) {                                          // YES
            s0 = 1;
            ok = c.PlaySound(frame, 66);
        }
        break;
    }
    case 5:
        ok = c.PlaySound(frame, 107);
        break;
    case 6:                                                             // Triangle: back to the menu
        g.W8(kPauseModalWhich, 0);
        ok = c.PlaySound(frame, 73);
        break;
    default:
        break;
    }
    *v0 = s0;
    return ok && !g.Faulted();
}

// ---------------------------------------------------------------------------- the menu
bool PauseMenu(GuestRam& g, PauseCallees& c, uint32_t sp) {
    const uint32_t frame = sp - 144u;
    uint32_t s1 = 2;
    if (g.U8(kPauseModalUp) == 0) {                                     // 0x8002D6B0: the menu's first frame
        g.W8(kPauseModalWhich, 0);
        g.W8(kPauseModalUp, 0);
        g.W16(kPauseMainMenu + 2u, 0);                                  // the cursor on RESUME
        if (!c.PlaySound(frame, 107)) return false;
        if (!c.SoundHold(frame, g.S8(g.U32(kGsPtr)) == 3 ? 1u : 0u)) return false;
        g.W8(kPauseModalUp, 1);
        return !g.Faulted();
    }
    if (!BoxFrame(g, c, frame, 0, 0, 0x80, FrameOt(g) + 0x18u, 0x56, 0x41, 0x12A, 0xAF)) return false;
    const int8_t which = g.S8(kPauseModalWhich);
    if (which == 1) {                                                   // "QUIT GAME"
        g.W32(kPauseMenuPtr, kPauseQuitMenu);
        if (!ConfirmInput(g, c, frame, &s1)) return false;
        if (s1 == 1) {
            s1 = 3;
            const uint32_t gs = g.U32(kGsPtr), pool = g.U32(kPausePool0);
            g.W8(kPauseModalWhich, 0);
            g.W8(kPauseModalUp, 0);
            g.W32(kPausePadIndex, 0);
            g.W8(kPauseResultsFlag, 1);
            const uint32_t type = g.U8(gs + 4u);
            g.W16(gs + 0x2Au, 0);
            if (type & 0x10u) {                                         // two players: each keeps a real place
                for (uint32_t k = 0; k < 2; ++k) {
                    const uint32_t rd = g.U32(pool + 1096u * k + 1084u);
                    if (g.U32(rd + 40u) == 0 || g.S32(kPauseFieldSize) < static_cast<int32_t>(g.U8(rd + 39u)))
                        g.W8(rd + 39u, 0xFF);
                    else
                        g.W8(rd + 39u, g.U8(rd + 39u));
                }
            } else if ((type & 4u) || (type & 0x22u)) {                 // the player's result: quit (255)
                g.W8(g.U32(pool + 1084u) + 39u, 0xFF);
            }
            if (!c.ResultsPrepare(frame)) return false;
            g.W8(g.U32(kGsPtr), 6);                                     // the results scene
            g.W32(kPauseFade, 0x00101010u);
            g.W32(kPauseFade + 4u, 0x00101010u);
        } else if (s1 == 0) {
            g.W8(kPauseModalWhich, 0);
        }
        if (!ConfirmDraw(g, c, frame, 5)) return false;
    } else if (which < 2) {
        if (which == 0) {
            g.W32(kPauseMenuPtr, kPauseMainMenu);
            if (!PauseInput(g, c, frame, &s1)) return false;
        }
    } else if (which == 2) {                                            // "RESTART GAME"
        g.W32(kPauseMenuPtr, kPauseRestartMenu);
        if (!ConfirmInput(g, c, frame, &s1)) return false;
        if (s1 == 1) {
            s1 = 3;
            const uint32_t gs = g.U32(kGsPtr);
            g.W8(kPauseModalWhich, 0);
            g.W8(kPauseModalUp, 0);
            g.W32(kPausePadIndex, 0);
            g.W16(gs + 0x2Au, 0);
            g.W8(gs, 5);                                                // main's post block: the race again
        } else if (s1 == 0) {
            g.W8(kPauseModalWhich, 0);
        }
        if (!ConfirmDraw(g, c, frame, 7)) return false;
    }
    if (!PauseDraw(g, c, frame)) return false;
    const uint32_t gs = g.U32(kGsPtr);
    if (g.S8(gs) == 4) {                                                // the stream stall's lines
        const int32_t waited = g.S32(gs + 0x0Cu) - g.S32(gs + 0x2Cu);
        if (!(waited < 3001)) {
            if (!HudDrawText(g, c, frame, Font(g), 11, 65, 120, FrameOt(g) + 8u, kCdBlue)) return false;
            if (!HudDrawText(g, c, frame, Font(g), 12, 75, 140, FrameOt(g) + 8u, kCdBlue)) return false;
        } else if (!HudDrawText(g, c, frame, Font(g), 14, 75, 120, FrameOt(g) + 8u, kCdBlue)) {
            return false;
        }
    }
    if (g.S16(g.U32(kGsPtr) + 0x2Au) != 0 &&
        !HudDrawText(g, c, frame, Font(g), 13, 100, 160, FrameOt(g) + 8u, kGreen))
        return false;
    if (g.U8(g.U32(kGsPtr) + 4u) & 0x10u) {                             // two players: the whole screen's area
        GetDrawEnv(g, frame + 32u);
        if (!HudDrawArea(g, c, frame, g.S16(frame + 32u), g.S16(frame + 34u), 384, 240, FrameOt(g) + 0x18u))
            return false;
    }
    if (s1 != 3) g.W8(kPauseModalUp, 1);
    return !g.Faulted();
}

// ---------------------------------------------------------------------------- the sound side
void SoundHold(SoundMachine& s, uint32_t v) {
    GuestRam& g = s.m;
    const uint32_t blk = kSoundHoldBlock;
    if (g.U8(blk + 0x59u) != 0 && v == 0) return;                       // 0x80020E54
    g.W8(blk + 1u, static_cast<uint8_t>(v));                            // 0x80020E70 (the delay slot)
    if (g.U8(blk) == 0) return;
    SoundParams p;
    if (v == 0) {
        p.pitch = g.S32(blk + 8u);
        g.W32(blk + 0x60u, g.U32(g.U32(kGsPtr) + 0x0Cu) + g.U32(blk + 0x5Cu));
    } else {
        p.pitch = 0;
    }
    if (g.U32(blk + 4u) == 0) return;
    uint32_t r = blk;
    for (uint32_t k = 0;;) {
        if (g.U32(r + 0x10u) != 0) {
            p.volume = g.S32(r + 0x14u);
            p.pan = g.S32(r + 0x18u);
            UpdateVoice(s, g.U32(r + 0x10u), p);                        // 0x8001F6A4
        }
        ++k;
        r += 20u;
        if (!(k < g.U32(blk + 4u))) break;
        if (s.Faulted()) return;
    }
}

} // namespace rr::sim
