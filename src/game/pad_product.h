#pragma once
// The pad reader's rider controls in the product: what SLUS 0x8001CB3C
// does for player 1 each frame after the driver's post-processor has stamped the button slots.
//
// The product keeps ONE pad record, 0x800D7128: the session's post-processor transcription stamps
// slots 0..14 there (fight_session.cpp, FightPadPass) and the combat decode reads it. This hook then
//   1. writes what the driver stores for the device (SLUS 0x8001C78C..0x8001C844): the id +0x0C (0x41
//      digital, 0x73 analogue), the analogue flag +0x10 and the stick bytes +0x08 (LY), +0x09 (RY),
//      +0x0A (LX); for a 0x73 pad it stamps the four extra slots 15..18 (RY at 0 / 0xFF, LY at 0 /
//      0xFF: 0x8001C880..0x8001C8D8) with the post-processor's slot logic (0x8001C8F4..0x8001C9FC);
//   2. runs the PORTED region [0x8001CFB0, exit) (pad_reader.h): the analogue axes 0x800CE540, the
//      merge into the bike's flagsA +0x230 on the bike, the walk bits of rider +0x228 off it;
//   3. does to the record what the reader's first loop does to the driver's record once it has
//      copied it (0x8001CBA4..0x8001CBE8): every slot's press code +6 = 0, a negative stamp = 0 - so
//      a press code lives for the one frame it was made in, as on the console.
// OURS: the record the region's slot-6 write goes to is 0x800D7128 (the product has no separate
// driver record; in the original that write lands in the driver's record and reaches the frame copy
// on the next frame's copy).
#include <cstddef>
#include <cstdint>

namespace rr::game {

struct PadDevice {
    bool analog = false;                          // a 0x73 pad (the DualShock's ANALOG mode)
    uint8_t ly = 0x80, ry = 0x80, lx = 0x80;      // 0 = full up / left, 0xFF = full down / right
};

struct PadControlsResult {
    bool ran = false;       // the player's bike under pad control (flagsA bit 27 clear)
    bool ok = true;         // false: the view faulted
    uint32_t exit = 0;      // the original's exit of the region
    uint32_t riderFlags = 0; // rider +0x228 after
    uint32_t mount = 0;      // rider +0x25C
    double riderToBike = 0;  // |rider +0xB8 - bike +0xB8| in the x/z plane, world units (the log)
};

// `p`: the player - its records 0x800D7128 + 192p, its axes 0x800CE540 + 8p (two players: mp_session.cpp).
PadControlsResult RunPadControls(uint8_t* ram, uint32_t gp, uint32_t bike, const PadDevice& dev, uint32_t p = 0);

// A TEST SCRIPT (OURS, like rrgame's --autosteer): while the player's rider is off the bike and
// walking under pad control, the pad bits that walk him back to it - forward (Cross) and a turn
// (-1 = the pad's Left, +1 = Right) toward the bike, from the rider's heading +0x1C2 in the x/z
// plane. Returns false when the rider is on the bike (the caller keeps its own pad); off the bike but
// not walking by hand it returns true with nothing held, so RiderRecover's own walk is not interrupted.
bool WalkToBike(const uint8_t* ram, uint32_t bike, bool& forward, int& turn);
// Frames on which WalkToBike held nothing so RiderRecover's own walk ran (RRJB_WALKFREE=off: the script
// before, which left the caller's steering keys on and so switched the rider to the manual walk).
size_t WalkFreeFrames();

// The same test script's other half: the player on the bike (mount < 2) and standing (|+0x1E0| below
// 1.0 unit/s) after the countdown - hold the throttle so a pure-pursuit steering that waits for the
// heading to line up (a stopped bike cannot turn) gets going again.
bool StandingStart(const uint8_t* ram, uint32_t bike);

} // namespace rr::game
