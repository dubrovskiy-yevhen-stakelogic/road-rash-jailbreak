#pragma once
// The race HUD, ported from the original MIPS code: the per-frame driver HudFrame RASHCDG 0x8005E848
// and the element functions under it, transcribed from our own
// disassembly of the player's own images:
//
//   SLUS_010.53  SHA-1 67ed165a2c517d4e6106fb0dfa324d66dd9a76f1, text at 0x80010000
//   RASHCDG.BIN  SHA-1 cfe43a7786759f2cb9c57751cf99e84d1074782c, loaded at 0x8005B5E8
//
// and accepted only by `rrverify phys` rows (tools\rrverify\rows_hud.inc).
//
// WHAT THE HUD IS, in one paragraph. The layout loader (RASHCDI 0x8005FA88, not in this
// file) turns `DASH1P.CSV` into 110 ITEM RECORDS of 36 bytes at *(0x8005ACEC) - each one a ready GPU
// packet (a sprite, a flat tile or a draw-mode word) with its screen position at +0x18 and its kind at
// +0x20 - and pre-chains them 0 -> 1 -> ... -> 109. Every frame HudFrame decides which of them to show:
// an element function rewrites the packet words of its items (the art rectangle for a digit, the width
// and colour of a bar) and then LINKS a run of items i..j into the HUD's one-slot ordering table
// *(0x8005B590 + 4p) by pointing j's tag at the list head and the head at i. An item that is not
// linked this frame is not drawn - which is the whole answer to "why the layout is not drawn at once".
// Text (rider names, the wrecked/auto labels) and the TKO skulls are extra packets allocated from the
// frame's packet heap (*(0x8005B470)+0x10C) and linked into the same list.
//
// Memory model: road_query.h's `GuestRam`. Every function takes the guest `sp` its caller would call
// it with, because two callees write their outputs into the caller's frame (RoadEndNode's node word,
// JunctionExits' record list) and because the bench compares the unported callees' call depth.
// Nothing here guesses a value: a load or store the console would not survive faults the view, and
// the caller refuses the call.
#include <cstdint>

#include "game/sim/road_query.h"

namespace rr::sim {

// ---------------------------------------------------------------------------- the addresses
// Every one read out of the functions' own `lui`/`lw` pairs.
constexpr uint32_t kHudEnabled      = 0x8005ACD8; // s32: 0 = HudFrame returns at once
constexpr uint32_t kHudFrameCount   = 0x8005ACDC; // s32: ++ per HudFrame; parity gates, &7 gates
constexpr uint32_t kHudSplitMode    = 0x8005AD14; // s32: the two-player layout (-1 in one player)
constexpr uint32_t kHudDividerSrc   = 0x8005B474; // -> the split-screen rectangle
constexpr uint32_t kHudDivider      = 0x800523B8; // SLUS data: the divider POLY_F4 packet
constexpr uint32_t kHudOt           = 0x8005B590; // u32[2]: -> the HUD ordering-table slot, per player
constexpr uint32_t kHudOt2          = 0x8005B5A0; // u32[2]: -> the second slot (the tpage reset)
constexpr uint32_t kHudItemsPtr     = 0x8005ACEC; // u32[2]: -> item record 0, per player
constexpr uint32_t kHudPlayerBikes  = 0x8005B268; // u32[2]: the player's own bike
constexpr uint32_t kHudFinished     = 0x8005B288; // s32[2]: "this player has finished"
constexpr uint32_t kHudDemo         = 0x8005B220; // s32: the attract / demo flag
constexpr uint32_t kHudArrestFsm    = 0x8005AD48; // s32: the arrest FSM (rules.md 9.4)
constexpr uint32_t kHudPanelDirty   = 0x8005AD18; // s32[2]: the player panel's values changed
constexpr uint32_t kHudTarget       = 0x8005AD0C; // u32[2]: the bike the opponent panel shows
constexpr uint32_t kHudLiveBikes    = 0x8005B1F8; // s32
constexpr uint32_t kHudFieldSize    = 0x8005B1FC; // s32: the "/16" of the place
constexpr uint32_t kHudCountdown    = 0x8005B230; // s32 16.16 seconds
constexpr uint32_t kHudOdometer     = 0x8005B380; // s32[2]
constexpr uint32_t kHudNamePool     = 0x8005B38C; // -> the pool the player panel's name is read from
constexpr uint32_t kHudPool0        = 0x8005B3A0; // -> pool-0 slot 0, stride 1096
constexpr uint32_t kHudFontIndex    = 0x8005AD2C; // s32: the HUD font (RASHCDI 0x80061528's answer)
constexpr uint32_t kHudStringTable  = 0x8005B544; // -> char *[] (GAMESTRG.LOC, rules.md 7.5)
constexpr uint32_t kHudClutTable    = 0x8005B538; // gp+2220: u16 CLUT ids by font CLUT slot
constexpr uint32_t kHudHeapPtr      = 0x8005B470; // -> the frame's packet heap; +0x10C = next free
constexpr uint32_t kHudHeapEnd      = 0x8005B4D0; // the heap's end
constexpr uint32_t kHudGameStatePtr = 0x8005B2F8;
constexpr uint32_t kHudPlayerRecs   = 0x800D81D8; // 36 bytes per slot (+0x14 nitro cap, +0x19 TKOs)
constexpr uint32_t kHudNitroBonus   = 0x800D80F5; // s8
constexpr uint32_t kHudArrowRange   = 0x800531A0; // SLUS: s32[3], the turn-arrow distance by bank
constexpr uint32_t kHudDrawModeHead = 0x800CCD60; // the tpage-reset DR_MODE HudFrame links last
constexpr uint32_t kHudPacketLength = 0x800CC68C; // RASHCDG data: s32[3], words per item kind
constexpr uint32_t kHudBarColours   = 0x800CC698; // RASHCDG data: 3 x {r,g,b,0}: red, yellow, green
constexpr uint32_t kHudArtTable     = 0x800D45D0; // 62 x 16 bytes, built by the layout loader
constexpr uint32_t kHudMessageWidth = 0x800D6120; // s32[15]: pixel widths of GAMESTRG 15..29
constexpr uint32_t kHudFontRecords  = 0x800D8078; // 4 x 24 bytes
constexpr uint32_t kHudState88      = 0x800D4A60; // 88 bytes per player: the values last drawn
constexpr uint32_t kHudDash224      = 0x800D6198; // 224 bytes per player: seven 32-byte flash timers
constexpr uint32_t kHudSlide72      = 0x800D6358; // 72 bytes per player: the two panel slides

constexpr uint32_t kHudItemBytes = 36;
constexpr int kHudItemCount = 110;

// The art table's named rows (DASH1P.CSV order).
constexpr int kArtTurnLeft = 2, kArtTurnRight = 3, kArtBigNum0 = 5, kArtSmallNum0 = 20, kArtRadarUp = 31,
              kArtNitroDim = 38, kArtText = 40, kArtWeapon0 = 41, kArtSmallSlash = 52, kArtSmallDot = 53,
              kArtWrongWay = 55, kArtCount0 = 57;

// ---------------------------------------------------------------------------- the callees
// What the HUD calls and this file does not contain. `sp` is the stack pointer the original makes
// the call at (the bench compares it, and a stack-argument callee needs it for its arguments).
struct HudCallees {
    virtual ~HudCallees() = default;
    // SLUS 0x80017BA0 PlaySound3D(0, 0, id, 0): the countdown's three cues per digit.
    virtual bool PlaySound(uint32_t sp, int32_t id) = 0;
    // SLUS 0x80016528: releases the voice handle at gp+1964 (the countdown's end).
    virtual bool StopCountdownVoice(uint32_t sp) = 0;
    // SLUS 0x800138E8 ComputePlace(bike, mode), PORTED in race.h over a host view.
    virtual bool ComputePlace(uint32_t sp, uint32_t bike, int32_t mode, int32_t* place) = 0;
    // SLUS 0x80021C98: the packet heap is full - the heap manager's answer is the new next-free.
    virtual bool HeapOverflow(uint32_t sp, uint32_t next, uint32_t bytes, uint32_t* answer) = 0;
    // What HudFrame calls that is NOT ported. The cop-mission and timed-race elements 0x80062F34 /
    // 0x80062C40 / 0x8005FAC4 / 0x80062610 / 0x80063530 / 0x800636F0 are PORTED (modes.h) and
    // run natively. `args` are the o32 arguments (the fifth and on at sp+16...).
    virtual bool Unported(uint32_t sp, uint32_t address, const uint32_t* args, int count) = 0;
};

// ---------------------------------------------------------------------------- the leaves
// SLUS 0x80013E64, 252 bytes: one step of a flash timer (a 32-byte record: +0 last toggle, +4 on
// time, +8 off time, +0x0C toggles, +0x10 phase, +0x14 start, +0x18 delay, +0x1C length).
void HudFlashTimer(GuestRam& g, uint32_t timer);
// SLUS 0x80013AF8, 112 bytes: point item `item` at art `art` (sprite: rectangle and size; tile: size).
void HudSetArtFull(GuestRam& g, uint32_t item, uint32_t art);
// The six-store sequence every element inlines: a SPRITE item takes art `art`'s uv/clut word, its
// own position and the art's size. A non-sprite item is left alone.
void HudSetArt(GuestRam& g, uint32_t item, uint32_t art);
// RASHCDG 0x8005FA68, 92 bytes: link items first..last (pre-chained) into `ot`.
void HudLinkRange(GuestRam& g, uint32_t ot, uint32_t items, int32_t first, int32_t last);
// SLUS 0x8004CE14 (libgpu SetDrawMode without a texture window), 44 bytes.
void HudSetDrawMode(GuestRam& g, uint32_t p, uint32_t dfe, uint32_t dtd, uint32_t tpage);
// RASHCDG 0x8005F834, 380 bytes: one step of a panel slide (a 36-byte record in the 72-byte one).
void HudSlide(GuestRam& g, uint32_t items, uint32_t slide, int32_t trigger);
// RASHCDG 0x8005F9B0, 184 bytes: move items first..last down by `dy`.
void HudSlideItems(GuestRam& g, uint32_t items, int16_t dy, int32_t first, int32_t last);
// SLUS 0x8002CD78 / 0x8002CDC8: draw GAMESTRG string `id` with font `font` at (x, y) into `ot`.
bool HudDrawText(GuestRam& g, HudCallees& c, uint32_t sp, int32_t font, int32_t id, int16_t x, int16_t y,
                 uint32_t ot, uint32_t colour);
bool HudDrawString(GuestRam& g, HudCallees& c, uint32_t sp, int32_t font, uint32_t str, int16_t x, int16_t y,
                   uint32_t ot, uint32_t colour);
// SLUS 0x8002D1A0, 88 bytes (with its search 0x8002D1F8): the 11-byte glyph record of `ch`, or 0.
uint32_t HudGlyph(GuestRam& g, uint32_t ch, uint32_t header, uint32_t table);
// SLUS 0x8002D0D8, 200 bytes: the advance width of a string (no packets).
int32_t HudStringWidth(GuestRam& g, int32_t font, uint32_t str);

// ---------------------------------------------------------------------------- the road side
// RASHCDG 0x80095410, 144 bytes: 1 when the bike is off its route (its route record does not name
// its slot, or its road is not on the route).
int32_t HudRouteOff(GuestRam& g, uint32_t bike);
// SLUS 0x8003B9A8, 164 bytes: the 32-byte road object of kind `kind` whose +0x10 is `id`, or 0.
uint32_t HudRoadObject(GuestRam& g, int32_t id, int32_t kind);
// SLUS 0x8003BC48, 228 bytes: the roads leaving junction `node` from `road` (up to max + 1), into
// `out` (s32 each); the count, or 0. `sp` is the caller's.
int32_t HudJunctionExits(GuestRam& g, int32_t node, int32_t road, uint32_t out, int32_t max, uint32_t sp);
// SLUS 0x8003C590, 456 bytes: the wrong-way / turn mask (rules.md 6.6): bit 0 wrong way, bit 1 off
// route, bit 4 turn right ahead, bit 6 turn left ahead, bit 7 inside a junction.
uint32_t HudWrongWayMask(GuestRam& g, uint32_t bike, uint32_t sp);
// RASHCDG 0x8008B84C, 336 bytes: the nearest rider by progress (the opponent panel's default).
uint32_t HudNearestRival(GuestRam& g, uint32_t bike);

// ---------------------------------------------------------------------------- the elements
// Arguments in the original's o32 order; `sp` is the caller's.
bool HudTko(GuestRam& g, HudCallees& c, uint32_t sp, uint32_t ot, uint32_t items, uint32_t dash, uint32_t bike,
            int32_t p, uint32_t mask);                                         // RASHCDG 0x8005F030
void HudSign(GuestRam& g, uint32_t ot, uint32_t items, uint32_t state, uint32_t bike, uint32_t mask,
             uint32_t dash, int32_t p);                                        // RASHCDG 0x8005FB4C
void HudArrestIcon(GuestRam& g, uint32_t ot, uint32_t items, uint32_t dash, uint32_t bike); // 0x8005FE58
bool HudCountdown(GuestRam& g, HudCallees& c, uint32_t sp, uint32_t ot, uint32_t items, uint32_t state,
                  uint32_t bike);                                              // RASHCDG 0x8005FF84
void HudSpeed(GuestRam& g, uint32_t ot, uint32_t items, uint32_t state, uint32_t bike); // 0x80060178
void HudOdometer(GuestRam& g, uint32_t ot, uint32_t items, uint32_t state, int32_t p);  // 0x800603E4
bool HudPlace(GuestRam& g, HudCallees& c, uint32_t sp, uint32_t ot, uint32_t items, uint32_t state,
              uint32_t bike, uint32_t mask, int32_t p, uint32_t dash);         // RASHCDG 0x800606F0
void HudPlayerPanel(GuestRam& g, uint32_t ot, uint32_t items, uint32_t state, uint32_t bike, int32_t p,
                    uint32_t slides, uint32_t dash);                           // RASHCDG 0x80060C10
int32_t HudTargetPanel(GuestRam& g, uint32_t ot, uint32_t items, uint32_t state, uint32_t bike, int32_t p,
                       uint32_t slides, uint32_t dash);                        // RASHCDG 0x8006148C
bool HudNames(GuestRam& g, HudCallees& c, uint32_t sp, uint32_t items, int32_t p, uint32_t slides); // 0x80061E50
void HudRadarDistance(GuestRam& g, uint32_t ot, uint32_t items, uint32_t state, uint32_t bike, int32_t p); // 0x80061F6C
void HudNitro(GuestRam& g, uint32_t ot, uint32_t items, uint32_t state, uint32_t bike, uint32_t dash); // 0x80062368
void HudPanelSlides(GuestRam& g, uint32_t bike, uint32_t items, uint32_t slides, int32_t p, int32_t changed); // 0x80063408
bool HudMessages(GuestRam& g, HudCallees& c, uint32_t sp, uint32_t items, uint32_t dash, uint32_t bike,
                 int32_t p);                                                   // RASHCDG 0x80062D9C

// ---------------------------------------------------------------------------- the radar strip
// The vertical strip at the top right (item 104's box) with a mark per nearby rider, drawn under a
// draw-area clip. RASHCDG 0x800C52A0 and its five children, and the libgpu / SLUS leaves under them.
constexpr uint32_t kHudRadarRects  = 0x800D63E8; // 12 bytes per player: x, y, w, h, centre x, centre y
constexpr uint32_t kHudRadarScale  = 0x8005B234; // 16.16: the strip's height / 200 units
constexpr uint32_t kHudMarkColours = 0x800CCBE0; // RASHCDG data: {r, g, b, 0} by mark kind
constexpr uint32_t kGpuVramSize    = 0x80055F70; // libgpu: s16 width, s16 height (1024 x 512)
constexpr uint32_t kGpuDrawEnv     = 0x80055F7C; // libgpu: the current DRAWENV (92 bytes; +0 clip x, y)
// SLUS 0x80049A94 / 0x80049B2C: the GP0 E3 / E4 word of a draw-area corner, clamped to VRAM.
uint32_t HudAreaTopLeft(GuestRam& g, int32_t x, int32_t y);
uint32_t HudAreaBottomRight(GuestRam& g, int32_t x, int32_t y);
// SLUS 0x8004954C SetDrawArea(DR_AREA *p, const RECT *r).
void HudSetDrawArea(GuestRam& g, uint32_t p, uint32_t rect);
// SLUS 0x8001C304, 240 bytes: a DR_AREA packet from the heap for (x, y, w, h), linked into `ot` (the
// seventh argument; the fifth and sixth are not read). `sp` is the caller's.
bool HudDrawArea(GuestRam& g, HudCallees& c, uint32_t sp, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t ot);
// SLUS 0x8001FE80: sin and cos of `angle` (4096 = one turn) in 16.16, from the table at 0x8005624C.
void HudSinCos(GuestRam& g, int32_t angle, uint32_t sinOut, uint32_t cosOut);
// SLUS 0x8002DE40: rotate the 16.16 pair at `v` (x, z) by `angle`. SLUS 0x8002DEC8: scale it by `k`.
void HudRotate2(GuestRam& g, int32_t angle, uint32_t v, uint32_t sp);
void HudScale2(GuestRam& g, uint32_t v, int32_t k);
bool HudRadarQuad(GuestRam& g, HudCallees& c, uint32_t sp, uint32_t verts, uint32_t colour, uint32_t code,
                  int32_t p);                                                  // RASHCDG 0x800C5168
bool HudRadarMark(GuestRam& g, HudCallees& c, uint32_t sp, uint32_t vec, int32_t angle, int32_t p,
                  int32_t kind);                                               // RASHCDG 0x800C5380
bool HudRadarRectArea(GuestRam& g, HudCallees& c, uint32_t sp, int32_t p);    // RASHCDG 0x800C5558
bool HudRadarFullArea(GuestRam& g, HudCallees& c, uint32_t sp, int32_t p);    // RASHCDG 0x800C5618
bool HudRadarMarks(GuestRam& g, HudCallees& c, uint32_t sp, uint32_t e, uint32_t kind, int32_t angle,
                   int32_t p);                                                 // RASHCDG 0x800C569C
bool HudRadarStrip(GuestRam& g, HudCallees& c, uint32_t sp, uint32_t bike, int32_t p); // RASHCDG 0x800C52A0

// ---------------------------------------------------------------------------- the driver
// RASHCDG 0x8005E848, 2024 bytes. False when a callee refused or an address faulted (the caller
// then restores what it snapshotted).
bool HudFrame(GuestRam& g, HudCallees& c, uint32_t sp);

} // namespace rr::sim
