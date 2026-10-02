#pragma once
// The DualShock's two motors, transcribed from our own disassembly of
//
//   SLUS_010.53  SHA-1 67ed165a2c517d4e6106fb0dfa324d66dd9a76f1, text 0x80010000
//
// THE PORT TABLE 0x800D7428: four 24-byte records, one per pad (the loop index of the pad reader and
// of the driver's post-processor, not a libpad port):
//   +0x00 s32  the libpad port: 0, 1 (or 0x10 without a multitap, set by 0x8001DDC4), 2, 3
//              (SLUS 0x8001C590 at boot writes k to record k)
//   +0x04 s32  the actuator state: 0 = not set up, 1 = a DualShock aligned (PadSetActAlign
//              succeeded), 2 = a controller of the first type (PadStateFindCTP1); the shell's
//              widget condition 0x02000000 (the VIBRATION chooser) needs 1
//   +0x08 s32  VIBRATION ON: 0x8001C590 sets it for records 0 and 1; the shell's commit RASHCDF
//              0x8007F37C writes (s8) player[p]+0x17 - the player's vibration option - for p < pads
//   +0x0C u8[2] the actuator buffer PadSetAct registers: [0] the small motor (0 / 1), [1] the large
//              motor's strength 0..255 (the align table gp+0x108 = 00 01 FF FF FF FF maps byte 0 to
//              actuator 0, byte 1 to actuator 1); 0x40 / on for a first-type controller
//   +0x10 s32  the small motor's stop time (game_state+0x0C ticks), 0 = none
//   +0x14 s32  the large motor's stop time, 0 = none (and the pad reader's engine rumble is skipped
//              while it is set)
//
// Every writer below reads the clock game_state+0x0C through *(0x8005B2F8).
#include <cstdint>

#include "game/sim/road_query.h"

namespace rr::sim {

constexpr uint32_t kPadPorts = 0x800D7428;         // four 24-byte records
constexpr uint32_t kPadPortBytes = 24;
constexpr uint32_t kPadIdleCounters = 0x800D6DD0;  // s32 per pad: the idle rumble's frame count
constexpr uint32_t kPadMotorSmallFn = 0x8001DC94;  // 116 B
constexpr uint32_t kPadMotorLargeFn = 0x8001DD08;  // 108 B
constexpr uint32_t kPadMotorFn = 0x8001DD74;       // 80 B, frame 32
constexpr uint32_t kPadActuatorFn = 0x8001DDC4;    // 416 B, frame 32
constexpr uint32_t kPadAlignGp = 0x108;            // gp+0x108: the align table
// libpad (SLUS), what 0x8001DDC4 calls
constexpr uint32_t kLibPadGetState = 0x80040550;   // PadGetState(port)
constexpr uint32_t kLibPadInfoMode = 0x8004061C;   // PadInfoMode(port, term, offs); term 2 = the current extended id
constexpr uint32_t kLibPadSetActAlign = 0x80040890; // PadSetActAlign(port, table)
constexpr uint32_t kLibPadSetAct = 0x80040910;     // PadSetAct(port, buffer, bytes)

// SLUS 0x8001DC94(pad, frames): the small motor. Record `pad` switched on (+0x08 != 0) and frames >= 0:
// +0x0C = 1 and +0x10 = clock + frames (frames == -1 would store 0, but a negative count never gets that
// far); else +0x0C = 0, +0x10 = 0.
void PadMotorSmall(GuestRam& g, uint32_t pad, int32_t frames);
// SLUS 0x8001DD08(pad, frames, strength): the large motor. Switched on and strength != 0: +0x0D =
// (u8) strength, +0x14 = frames == -1 ? 0 (no stop time) : clock + frames; else +0x0D = 0, +0x14 = 0.
void PadMotorLarge(GuestRam& g, uint32_t pad, int32_t frames, int32_t strength);
// SLUS 0x8001DD74(pad, smallFrames, largeFrames, strength) = DC94(pad, smallFrames) then
// DD08(pad, largeFrames, strength) - what PadRumble RASHCDG 0x800B658C and HitRumble 0x800BFD74 call.
void PadMotor(GuestRam& g, uint32_t pad, int32_t smallFrames, int32_t largeFrames, int32_t strength);

// libpad as 0x8001DDC4 sees it (the product answers for its own controller, the bench runs the guest's).
struct LibPad {
    virtual ~LibPad() = default;
    virtual int32_t GetState(uint32_t port) = 0;                          // 0x80040550
    virtual void SetAct(uint32_t port, uint32_t buffer, uint32_t bytes) = 0; // 0x80040910
    virtual int32_t SetActAlign(uint32_t port, uint32_t table) = 0;       // 0x80040890
    virtual int32_t InfoMode(uint32_t port, int32_t term, int32_t offs) = 0; // 0x8004061C
};
// SLUS 0x8001DDC4(pad, single): the actuator service the driver's post-processor calls for every pad
// (0x8001CA04) each frame. Pad 1's port is 0x10 when `single` is 0, else 1. A motor whose stop time has
// passed (0 < stop < clock) is switched off. Then by PadGetState: 0 -> state 0; 1 -> state 0 and both
// motors cleared; with state 0 the buffer is registered (PadSetAct(port, +0x0C, 2)), state 2 on a
// first-type controller, state 1 when a stable pad (6) takes the align table gp+0x108; with a state set,
// a controller without an extended mode id (PadInfoMode(port, 2, 0) == 0) gets 0x40 and "small motor
// running" in the buffer instead. Returns false when the view faulted.
bool PadActuatorService(GuestRam& g, uint32_t pad, uint32_t single, LibPad& lp);

// THE ENGINE RUMBLE, the pad reader's region [0x8001D7DC, 0x8001DA08) of SLUS 0x8001CB3C, per pad `p`
// with its controlled bike (s3) and its idle counter `cnt` (s6 = 0x800D6DD0 + 4p). Not in the attract
// mode (0x8005B220), not while PadRumble's large motor runs (+0x14 != 0, the counter zeroed):
//   * a1 = FixDiv(|bike+0x2A4|, bike+0x22C->+0xCC) with the signs the original's four arms give
//     (0x80010028); past bike+0x2BC > 0x11FFF at least 0x8000;
//   * the rider inactive (bike+0x140 == 0) or off the bike (rider+0x25C >= 2): the large motor off;
//   * a1 != 0: strength (145 a1 >> 16) + 110, the counter zeroed;
//   * a1 == 0: stopped or slow (speed +0x1E0 <= 0x23C35) and bike+0xB4 < 9 - the idle rumble, 50 for
//     600 counted frames; fast with bike+0x184 bit 0 - 0; anything else off;
//   * bike+0x184 bit 0: the strength is at least min(FixMul(speed, 7.5) >> 16, 150), the counter zeroed;
//   * the large motor at that strength with no stop time: DD08(p, -1, strength).
// `s0` is the register as the region is entered; the result is what the original leaves in s0 and the
// exit, always 0x8001DA2C (the original's other exit 0x8001DA30 has already made 0x8001DA2C's s6 += 4).
constexpr uint32_t kPadRumbleEntry = 0x8001D7DC;
constexpr uint32_t kPadMotorsOffEntry = 0x8001DA08;
constexpr uint32_t kPadRumbleExit = 0x8001DA2C;
struct PadRumbleRegs {
    uint32_t exit = kPadRumbleExit;
    uint32_t s0 = 0;
    int32_t strength = -1; // what DD08 was asked for (-1: DD08 not called)
};
PadRumbleRegs PadEngineRumble(GuestRam& g, uint32_t p, uint32_t bike, uint32_t cnt, uint32_t s0);
// [0x8001DA08, 0x8001DA2C): the pad not under the player's control, or not racing (game_state+0 != 1):
// DD08(p, -1, 0), DC94(p, -1) - both motors off - and the counter zeroed.
PadRumbleRegs PadMotorsOff(GuestRam& g, uint32_t p, uint32_t cnt, uint32_t s0);

} // namespace rr::sim
