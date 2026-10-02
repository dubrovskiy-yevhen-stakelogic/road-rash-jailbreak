#pragma once
// The per-player part of the pad reader SLUS 0x8001CB3C that turns the frame's pad record into the
// rider's controls, transcribed from our own disassembly of
//
//   SLUS_010.53  SHA-1 67ed165a2c517d4e6106fb0dfa324d66dd9a76f1, text 0x80010000
//
// and accepted by the splice row `pad_rider_controls` of tools\rrverify\rows_feel.inc.
//
// THE REGION [0x8001CFB0, exit) of the pad reader's player loop, entered when game_state[0] == 1 and
// the player controls the bike (flagsA bit 27 clear, or the co-op test of 0x8001CF10.. for p >= 2):
//
//   * the ANALOGUE AXES (record +0x10 != 0, a 0x73 pad): 0x800CE540 + 8p = AxisCurve(rec[+9],
//     ENV.EN 0x800D3984) and +4 = AxisCurve(rec[+10], 0x800D3978) - SLUS 0x8001CA58, input.h;
//   * the rider's bike inactive (rider +0x140 == 0): exit 0x8001D7DC (the camera controls skipped);
//   * ON THE BIKE (rider +0x25C < 2): the control bits merged into flagsA +0x230 - the analogue
//     branch (slots 15..18: the stick at full travel) or the digital one (controls 3..7 through the
//     remap table rec+0xB8: throttle 0x1|0x2 (+0x4 on a fresh press), brake 0x20|0x40, the two
//     steering pairs 0x80|0x100 / 0x80|0x200 with the release edge 0x80, 0x8 and 0x400 from control 7,
//     rec+0xB4 == 2's d-pad override of the steering) - exit 0x8001D350, right after the store;
//   * OFF THE BIKE (the walk; riderDef +0x28 == 0): in a walking stance (category 8 of 0x800541D4),
//     rider +0x228's direction bits 0x100 (slot 2 or 7 held: forward), 0x200 (slot 0: left), 0x80
//     (slot 3 or 4: back), 0x400 (slot 1: right), the manual-walk pair 0x1000800 when any is held (or
//     an analogue axis is off centre), 0x1000 cleared unless slot 6 is held and set on its press (the
//     LIVE record 0x800D6DE0 + 192p gets that slot's +4 = 0xFF, +6 = 1) - exit 0x8001D700.
//
// `rec` is the frame-stable record 0x800D7128 + 192p, `bike` the player's bike (s3), `live` the live
// record 0x800D6DE0 + 192p. Returns false when the view faulted; `exit` is the original's exit pc.
#include <cstdint>

#include "game/sim/road_query.h"

namespace rr::sim {

constexpr uint32_t kPadReaderFn = 0x8001CB3C;
constexpr uint32_t kPadRegionEntry = 0x8001CFB0;
constexpr uint32_t kPadExitMerged = 0x8001D350;  // on the bike: +0x230 stored
constexpr uint32_t kPadExitCamera = 0x8001D700;  // off the bike: the camera controls next
constexpr uint32_t kPadExitIdle = 0x8001D7DC;    // the bike's rider inactive
constexpr uint32_t kPadRecordsLive = 0x800D6DE0; // the driver's records, 192 bytes per player
constexpr uint32_t kPadRecordsFrame = 0x800D7128; // the pad reader's frame-stable copy
constexpr uint32_t kPadAxesTable = 0x800CE540;   // {s32 axis0; s32 axis1} per player
constexpr uint32_t kPadEnvCfg = 0x800D38E0;      // ENV.EN (AxisCurve's dead zone +0xB0, width +0xB2)

bool PadRiderControls(GuestRam& g, uint32_t p, uint32_t rec, uint32_t bike, uint32_t live, uint32_t& exit);

// THE L2 ARM of the same player loop (the taunt), `SLUS 0x8001D51C..0x8001D5AC`, transcribed from our own
// listing (SLUS_010.53 SHA-1 67ed165a2c517d4e6106fb0dfa324d66dd9a76f1); s2 = `rec` (the loop's frame-stable
// record 0x800D7128 + 192p), s3 = `bike` (its bike), so it runs for player 2 as for player 1. In two halves
// around its one call, RiderSpeech 0x8001A760 (the caller makes it):
//   * PadTauntPress (0x8001D51C..0x8001D544): control 8's slot (remap rec+0xB8, word +0x20) - its press code
//     +0x1A > 0 asks for RiderSpeech(bike +0xAC, 0): true with `handle`; `v0` = the code (the lb);
//   * PadTauntHeld (0x8001D550..0x8001D5AC): the slot's hold stamp +0x14 > 0 sets rider (bike +0x354) +0x228
//     bit 0x800000, else clears it; then `j 0x8001D700`. The registers it leaves (v0, v1, a0) as the
//     original's two paths leave them.
constexpr uint32_t kPadTauntEntry = 0x8001D51C, kPadTauntCall = 0x8001D548, kPadTauntHeld = 0x8001D550;
constexpr uint32_t kRiderSpeechFn = 0x8001A760;
bool PadTauntPress(GuestRam& g, uint32_t rec, uint32_t bike, uint32_t& handle, uint32_t& v0);
struct PadTauntRegs {
    uint32_t v0 = 0, v1 = 0, a0 = 0;
};
PadTauntRegs PadTauntHeld(GuestRam& g, uint32_t rec, uint32_t bike);

} // namespace rr::sim
