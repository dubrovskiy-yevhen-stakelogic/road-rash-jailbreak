#pragma once
// The pad motors in the product.
//
// Everything that decides a motor is the original's, PORTED and benched (sim\pad_motor.h, rows_rumble.inc):
// PadRumble RASHCDG 0x800B658C (a landing, a bounce, a wall, a rider hit) and HitRumble 0x800BFD74 (a blow)
// through SLUS 0x8001DD74; the pad reader's engine rumble region of SLUS 0x8001CB3C (the idle shake at the
// line, the rough-ground shake, the ride's lateral term) and its motors-off region; the driver's actuator
// service SLUS 0x8001DDC4 (the stop times, the libpad set-up) against the product's controller model
// (pad_device.h: a DualShock on every racing pad). What reaches the real controller is the actuator buffer
// port-table record +0x0C / +0x0D, exactly what libpad sends a DualShock each frame:
//   * XInput: the large motor's byte (0..255, the DualShock's PWM strength) -> wLeftMotorSpeed = byte * 257
//     (the low-frequency motor, the DualShock's large one); the small motor (on / off) -> wRightMotorSpeed
//     65535 or 0 (the high-frequency motor). OURS: the linear scale; the DualShock's large motor does not
//     turn below a strength of about 0x40, an XInput pad's does - nothing is remapped for that.
//   * a WinMM (DirectInput-class) pad: no force feedback (WinMM has none; DirectInput's is not used).
// A scripted run never drives a controller (it never polls one - rrgame main.cpp) and the counters below are
// the same with or without a controller. The vibration option is the original's: the record's +0x08, which
// the shell's commit RASHCDF 0x8007F37C sets from the player's +0x17 (VIBRATION ON / OFF) and which
// DC94 / DD08 test. RRJB_RUMBLE=off: none of it runs (the negative control; the named seams come back).
#include <cstddef>
#include <cstdint>
#include <string>

#include "game/sim/road_query.h"

namespace rr::game {

bool RumbleOn();

struct RumbleCounts {
    size_t races = 0;
    size_t padRumbles = 0, padRumbleRefused = 0, hitRumbles = 0, motorCalls = 0, foreignBytes = 0;
    size_t services = 0, aligned = 0;
    size_t engineRegions = 0, offRegions = 0, idleShake = 0, roughShake = 0, lateralShake = 0;
    size_t largeFrames = 0, smallFrames = 0, largeMax = 0;
    uint64_t largeSum = 0;
    size_t deviceWrites = 0;
    size_t countdownStops = 0;      // SLUS 0x80016528 run by the HUD (countdown_voice.h; not a rumble counter)
    uint32_t countdownHandle = 0;   // what 0x800164B4 stored in gp+1964 (0 in a one-player race)
    bool fromShell = false;
    uint32_t vibration[4] = {};
};
RumbleCounts& RumbleCounters();
std::string RumbleTotals();

// At the race's start, once the session and player records are in the arena: the boot's port table
// (pad_device.h PadPortsBoot) and, for a race the front end started, the commit's +0x08 = (s8) player[p]
// +0x17 for p < game_state+0x34 (RASHCDF 0x8007F37C, the store 0x8007F428 of its loop 0x8007F404..0x8007F43C); a direct --race keeps every
// capture's table, +0x08 = 1 on all four records. Keeps `ram` for RumbleOutput.
void RumbleRaceStart(uint8_t* ram, uint32_t gp, bool fromShell);
// Each race frame, after the pad reader's rider controls (race_session.cpp): the driver's actuator service
// for every pad, then the pad reader's rumble regions for pads 0..min(pads, 2) - as SLUS 0x8001CB3C runs
// them after its per-player controls.
void RumbleFrame(uint8_t* ram, uint32_t gp);
// RASHCDG 0x800B658C PadRumble, PORTED (collision.h), with SLUS 0x8001DD74 run natively.
bool ProductPadRumble(rr::sim::GuestRam& g, uint32_t e, uint32_t other, int32_t speed, int32_t k, int32_t div,
                      uint32_t sp);
// SLUS 0x8001DD74, PORTED, for HitRumble RASHCDG 0x800BFD74 (fight_session.cpp).
void ProductPadMotor(rr::sim::GuestRam& g, uint32_t pad, int32_t a, int32_t b, int32_t c);
// At the race's end: RASHCDI 0x80063A20 LeaveRace's first loop (0x80063A4C..0x80063A84) -
// DD08(p, -1, 0), DC94(p, -1) for every pad - on the ported functions.
void RumbleRaceEnd(uint8_t* ram, uint32_t gp);

struct MotorState {
    bool dualShock = false; // the record's state 1 (aligned)
    bool small = false;
    uint8_t large = 0;
};
// Pad `pad`'s motors as the console would run them this frame (a DualShock record's buffer).
MotorState RumbleOutput(uint32_t pad);
void NoteDeviceWrite();

} // namespace rr::game
