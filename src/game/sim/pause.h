#pragma once
// The in-race pause: the pad poll's pause test, GameFrame's switch into and out of the stream stall
// (state 4) and the pause menu itself (state 3), ported from our own disassembly of the player's own
// executable:
//
//   SLUS_010.53  SHA-1 67ed165a2c517d4e6106fb0dfa324d66dd9a76f1, text at 0x80010000
//
// and accepted only by `rrverify phys` rows (tools\rrverify\rows_pause.inc, one per function).
//
//   the pause test     region [0x8001CBEC, 0x8001CD00) of the pad poll SLUS 0x8001CB3C
//   the stall switch   region [0x80011C70, 0x80011D88) of GameFrame SLUS 0x80011C4C
//   PauseMenu          SLUS 0x8002D2F4, run by GameFrame while game_state+0x00 is 3 or 4
//     PauseInput       SLUS 0x8002DC1C   (the main menu: RESUME / QUIT / RESTART)
//     ConfirmInput     SLUS 0x8002DD80   (the YES / NO of "QUIT GAME" and "RESTART GAME")
//     MenuUpDown       SLUS 0x8001DA7C   (the pad record's Up / Down / Cross / Triangle / Start)
//     MenuLeftRight    SLUS 0x8001DB90   (the pad record's Left / Right / Cross / Triangle / Start)
//     PauseDraw        SLUS 0x8002D718   (the title and the three items, the dark box)
//     ConfirmDraw      SLUS 0x8002D9E8   (the question and YES / NO, its own box)
//     TextInBox        SLUS 0x8002CB08   (GAMESTRG string `id` at, right-aligned in or centred in a rect)
//     BoxFrame         SLUS 0x800400F4   (four translucent POLY_F4 bands around a rectangle)
//     StringWidthById  SLUS 0x8002D0A8
//     GetDrawEnv       SLUS 0x80048FBC   (libgpu: a copy of the current DRAWENV)
//   SoundHold          SLUS 0x80020E30   (the ambient voices' pitch hold; sound_engine.h's machine)
//
// Memory model: road_query.h's GuestRam. The text itself is hud.h's PORTED HudDrawText / HudDrawString,
// the draw area hud.h's HudDrawArea. What the menu calls and this file does not contain goes through
// `PauseCallees` (the bench has the oracle execute each; the product runs the PORTED ones).
#include <cstdint>

#include "game/sim/hud.h"
#include "game/sim/road_query.h"
#include "game/sim/sound_engine.h"

namespace rr::sim {

// ---------------------------------------------------------------------------- the addresses
constexpr uint32_t kPauseGp          = 0x8005AC8C;
constexpr uint32_t kPauseModalUp     = kPauseGp + 556;  // u8 gp+556: the menu has been set up
constexpr uint32_t kPauseModalWhich  = kPauseGp + 557;  // s8 gp+557: 0 the menu, 1 quit?, 2 restart?
constexpr uint32_t kPausePadIndex    = kPauseGp + 260;  // u32 gp+260 (0x8005AD90): the pad that paused
constexpr uint32_t kPausePadLost     = kPauseGp + 2028; // u32 gp+2028 (0x8005B478): pauses a racing game
constexpr uint32_t kPauseMenuPtr     = kPauseGp + 2252; // u32 gp+2252 (0x8005B558): -> {s16 count, s16 cursor}
constexpr uint32_t kPauseMainMenu    = 0x800540D8;      // SLUS data {3, cursor}
constexpr uint32_t kPauseQuitMenu    = 0x800540DC;      // {2, cursor}: 0 YES, 1 NO
constexpr uint32_t kPauseRestartMenu = 0x800540E0;      // {2, cursor}
constexpr uint32_t kPauseBoxPrim     = 0x800540E4;      // SLUS data: POLY_F4 0x2A 0x080808 over the menu
constexpr uint32_t kPauseAskPrim     = 0x800540FC;      // SLUS data: POLY_F4 0x2A 0x404040 behind the question
constexpr uint32_t kPauseSlash       = 0x8005AEBC;      // the one-glyph string between YES and NO
constexpr uint32_t kPauseFrameOtPtr  = 0x8005B5AC;      // -> the frame's 12-slot 2D ordering table
constexpr uint32_t kPauseResultsFlag = 0x8005AF4C;      // u8: the quit arm sets 1
constexpr uint32_t kPauseFade        = 0x8005AF58;      // u32[2]: the quit arm's 0x00101010
constexpr uint32_t kPauseResultsKey  = 0x8005AF50;      // u32: a pause press in the results scene
constexpr uint32_t kPauseDemo        = 0x8005B220;      // u32: the attract flag
constexpr uint32_t kPauseFieldSize   = 0x8005B1FC;      // s32
constexpr uint32_t kPausePool0       = 0x8005B3A0;      // -> pool-0 slot 0 (1096 bytes a slot)
constexpr uint32_t kPauseOverlayMask = 0x8005ACA8;      // bit 0x10: RASHCDF resident (TextInBox mode 3)
constexpr uint32_t kPauseDrawEnv     = 0x80055F7C;      // libgpu: the current DRAWENV, 0x5C bytes
constexpr uint32_t kPausePadRecords  = 0x800D7128;      // the frame's pad records, 192 bytes each
constexpr uint32_t kSoundHoldBlock   = 0x800D7548;      // the ambient voices' block (SoundHold)

constexpr uint32_t kPadPollFnPause   = 0x8001CB3C;
constexpr uint32_t kPauseTestEntry   = 0x8001CBEC, kPauseTestExit = 0x8001CD00;
constexpr uint32_t kGameFrameFn      = 0x80011C4C;
constexpr uint32_t kStallEntry       = 0x80011C70, kStallExit = 0x80011D88;
constexpr uint32_t kPauseMenuFn      = 0x8002D2F4, kPauseInputFn = 0x8002DC1C, kConfirmInputFn = 0x8002DD80,
                   kMenuUpDownFn     = 0x8001DA7C, kMenuLeftRightFn = 0x8001DB90, kPauseDrawFn = 0x8002D718,
                   kConfirmDrawFn    = 0x8002D9E8, kTextInBoxFn = 0x8002CB08, kBoxFrameFn = 0x800400F4,
                   kStringWidthIdFn  = 0x8002D0A8, kGetDrawEnvFn = 0x80048FBC, kSoundHoldFn = 0x80020E30;
// The callees the ports reach and do not contain.
constexpr uint32_t kPausePlaySoundFn = 0x80017BA0, kPauseRaceOverFn = 0x80018C1C,
                   kPauseResultsFn   = 0x8003F708, kPauseHeapFn = 0x80021C98;

// ---------------------------------------------------------------------------- the callees
// HudCallees' PlaySound (SLUS 0x80017BA0(0, 0, id, 0)) and HeapOverflow (SLUS 0x80021C98) are the
// menu's too; these are the rest. `sp` is the stack pointer the original makes the call at.
struct PauseCallees : HudCallees {
    // SLUS 0x80020E30 SoundHold(v) - PORTED below (SoundHold), run on the sound runtime's machine.
    virtual bool SoundHold(uint32_t sp, uint32_t v) = 0;
    // SLUS 0x80018C1C RaceOverSignal(pause) - PORTED (race_over.h).
    virtual bool RaceOverSignal(uint32_t sp, uint32_t pause) = 0;
    // SLUS 0x8003F708 ResultsPrepare() - PORTED (passes.h).
    virtual bool ResultsPrepare(uint32_t sp) = 0;
};

// ---------------------------------------------------------------------------- the pad poll
// Region [0x8001CBEC, 0x8001CD00) of SLUS 0x8001CB3C, the part after the frame copy of the pad records:
// a racing game (state 1, the first-frame byte +0x03 clear) whose gp+2028 is set, or whose pad record
// shows a press code on control 1 (the record's remap +0xB8, word 1 - the Start slot 12), goes to state 3
// and gp+260 names the pad; a press in the results scene (state 6) sets *(0x8005AF50). Returns s0, the
// "state changed" flag the clock region [0x8001CD00, 0x8001CD7C) and RaceOverSignal(state == 3) read.
// NOT for state 2: the original leaves the poll before the region (0x8001CC08); the port returns 0.
uint32_t PadPauseTest(GuestRam& g);

// ---------------------------------------------------------------------------- GameFrame
// Region [0x80011C70, 0x80011D88) of SLUS 0x80011C4C, right after its stream step 0x8002305C: a stream
// stall (+0x28 != 0) takes the game to state 4 (the demo to state 2), keeping the state it left in +0x01
// and the clock in +0x2C, with RaceOverSignal(1) and SoundHold(1); the stall's end (+0x28 == 0 in state 4)
// sets +0x2A (the "recovered" line) and goes back - a racing game into the pause menu, state 3. `sp` is
// GameFrame's own (after its 64-byte prologue).
bool GameFrameStall(GuestRam& g, PauseCallees& c, uint32_t sp);

// ---------------------------------------------------------------------------- the menu
// SLUS 0x8002D2F4, frame 144. `sp` is the caller's.
bool PauseMenu(GuestRam& g, PauseCallees& c, uint32_t sp);
// SLUS 0x8002DC1C, frame 24. Returns v0: 2 nothing, 5 moved, 4 selected, 6/7 resumed -> 3.
bool PauseInput(GuestRam& g, PauseCallees& c, uint32_t sp, uint32_t* v0);
// SLUS 0x8002DD80, frame 24. Returns v0: 0 NO / back, 1 YES, 2 nothing, 5 moved, 7 Start.
bool ConfirmInput(GuestRam& g, PauseCallees& c, uint32_t sp, uint32_t* v0);
// SLUS 0x8001DA7C / 0x8001DB90, leaves. The menu is *(gp+2252).
uint32_t MenuUpDown(GuestRam& g);
uint32_t MenuLeftRight(GuestRam& g);
// SLUS 0x8002D718, frame 56.
bool PauseDraw(GuestRam& g, PauseCallees& c, uint32_t sp);
// SLUS 0x8002D9E8(id), frame 40: the question GAMESTRG `id`, YES / NO.
bool ConfirmDraw(GuestRam& g, PauseCallees& c, uint32_t sp, int32_t id);
// SLUS 0x8002CB08(font, id, rect, ot, colour, mode), frame 48: mode 0 at (rect.x, rect.y), 1 right-aligned
// in rect.w, 2 centred in rect.w, 3 the front end's word wrap (only while RASHCDF is resident: the call to
// 0x800662CC is refused here - in the race the mask bit is clear and mode 3 draws nothing).
bool TextInBox(GuestRam& g, PauseCallees& c, uint32_t sp, int32_t font, int32_t id, uint32_t rect, uint32_t ot,
               uint32_t colour, int32_t mode);
// SLUS 0x800400F4(r, g, b, ot, x0, y0, x1, y1), frame 56: four 24-byte semi-transparent POLY_F4 bands
// around (x0, y0)..(x1, y1) from the packet heap, each linked into `ot`.
bool BoxFrame(GuestRam& g, PauseCallees& c, uint32_t sp, uint32_t r, uint32_t gg, uint32_t b, uint32_t ot,
              int32_t x0, int32_t y0, int32_t x1, int32_t y1);
// SLUS 0x8002D0A8(font, id): the width of GAMESTRG string `id`.
int32_t StringWidthById(GuestRam& g, int32_t font, int32_t id);
// SLUS 0x80048FBC(out): copies the 0x5C-byte DRAWENV 0x80055F7C to `out`; returns `out`.
uint32_t GetDrawEnv(GuestRam& g, uint32_t out);

// ---------------------------------------------------------------------------- the sound side
// SLUS 0x80020E30 SoundHold(v), frame 48: *(0x800D7549) = v (unless the block's +0x59 is set and v is 0);
// with the block live (+0x00), every held voice (+0x10 + 20k, k < +0x04) is retuned through UpdateVoice
// 0x8001F6A4 to pitch 0 (v != 0) or the block's +0x08 (v == 0, which also restamps +0x60 = clock + +0x5C).
void SoundHold(SoundMachine& s, uint32_t v);

} // namespace rr::sim
