#pragma once
// The AI command pass on guest memory - `AiRunCommands RASHCDG 0x800BA4CC` with the arms of the
// manoeuvre commands 2, 5, 6, 7, 8 and 9 and the tests under them - ported from our own disassembly
// of the player's own overlay:
//
//   RASHCDG.BIN  SHA-1 cfe43a7786759f2cb9c57751cf99e84d1074782c, loaded at 0x8005B5E8
//
// Accepted only by `rrverify phys` rows (tools\rrverify\rows_ai.inc).
//
// THE MEMORY MODEL is road_query.h's guest-address view (`GuestRam`), as population.h: every function
// takes guest addresses; the bike pool is *(0x8005B3A0) with stride 1096, game_state *(0x8005B2F8), the
// tuning words 0x80052F50.. are GLOBALS.BI's (ai_globals.h). The stack locals the original hands to a
// callee (op 2's and 0x800BC4FC's scalar) are passed by value: they live inside the stack window.
//
// `AiCommandPass` replaces ai.h's host-pointer `AiRunCommands` in the product: that port runs two of the
// nineteen arms, this one runs every arm whose handler is in this file or ported elsewhere and asks
// the caller for the rest (`AiCmdCallees`), so nothing is refused for lack of an arm.
#include <cstdint>

#include "game/sim/road_query.h"

namespace rr::sim {

constexpr uint32_t kAiArmTable = 0x8005B9B8; // the 19 words the dispatcher jumps through (0x800BA5B4)
constexpr uint32_t kAiTune     = 0x80052F50; // GLOBALS.BI file 0x00C.., 20 words (+0x00 .. +0x4C)

// The callees not written in this file. Each returns false when the caller could not run it; the port
// then returns false and nothing it wrote is to be trusted. `sp` is the stack pointer at the call.
struct AiCmdCallees {
    virtual ~AiCmdCallees() = default;
    // ---- PORTED elsewhere (ai.h host-pointer ports, stance.h); the caller runs them on the same memory
    virtual bool CmdRace(uint32_t e, uint32_t sp) = 0;                                // 0x800BA7F4
    virtual bool PopCommand(uint32_t e, uint32_t sp) = 0;                             // 0x800BC8DC
    // 0x80093CAC(e, 0, e + 0x368, &scalar, 1): the only form this pass reaches.
    virtual bool SetAimDelta(uint32_t e, int32_t scalar, uint32_t sp) = 0;
    virtual bool StanceEvent(uint32_t ev, uint32_t rider, uint32_t p, uint32_t sp) = 0; // 0x800C4550
    // ---- NOT ported (the bench runs the original for them)
    virtual bool CopIdle(uint32_t e, uint32_t cmd, int32_t dt, uint32_t sp) = 0;       // 0x800BAA2C, op 1
    virtual bool CopRelease(uint32_t e, uint32_t sp) = 0;                               // 0x80092AD4, op 1
    virtual bool LeaveRace(uint32_t e, int32_t dt, uint32_t sp) = 0;                    // 0x80092E04, op 18
    virtual bool Fight(uint32_t e, uint32_t target, int32_t dt, uint32_t sp) = 0;       // 0x800C035C, op 16
    virtual bool Intercept(uint32_t e, uint32_t target, uint32_t sp) = 0;               // 0x800BBEBC, op 17
    virtual bool Strike(uint32_t e, uint32_t target, uint32_t sp) = 0;                  // 0x800C1370, op 9
    virtual bool JailbreakFinish(uint32_t sp) = 0;                                      // 0x800C9E74, op 2
    virtual bool CopGap(uint32_t e, uint32_t target, uint32_t sp, int32_t& v0) = 0;     // 0x8009E444, op 2
};

// ---- the tests (leaves) --------------------------------------------------------------------------
// 0x800BB448: may `e` take the gap beside `t` (op 5)? `gap` = (e[+0x144] - t[+0x144]) << 4.
int32_t AiGapOk(GuestRam& g, uint32_t e, uint32_t t, int32_t gap);
// 0x800BB8FC: may `e` back off from `t` (op 6)?
int32_t AiBackOffOk(GuestRam& g, uint32_t e, uint32_t t, int32_t gap);
// 0x800BBBB8: is `t` still worth closing on (op 7)?
int32_t AiCloseOk(GuestRam& g, uint32_t e, uint32_t t, int32_t gap);
// 0x800BBD44: is `t` close enough beside `e` to pass on a side (op 8)?
int32_t AiPassOk(GuestRam& g, uint32_t e, uint32_t t, int32_t gap);
// 0x800BC1EC, frame 40: can `e` reach `t` to strike (AiChooseCommand and op 9)? AiProject inside.
int32_t AiStrikeReach(GuestRam& g, uint32_t e, uint32_t t, int32_t gap);
// 0x800BC618, frame 8: 1 or 0 - is `t`'s OTHER attacker (a player's second attacker bit, or the
// target of `t`'s own command 6/7/14..17) beside it on the side `side` (sign)? 0 when there is none.
int32_t AiOtherSide(GuestRam& g, uint32_t t, uint32_t handle, int32_t side);

// ---- the functions with callees -----------------------------------------------------------------
// 0x800BC4FC, frame 32: AimBesideRider(e, t, offset) - the aim point slides to `offset` beside `t`.
bool AiAimBeside(GuestRam& g, uint32_t e, uint32_t t, int32_t offset, uint32_t sp, AiCmdCallees& c);
// The arms: op 2 0x800BADB8 (frame 56, `cmd` = the command record), op 5 0x800BB280 (40),
// op 6 0x800BB5B8 (40), op 7 0x800BBA18 (32), op 8 0x800BBC20 (32), op 9 0x800BBE00 (40).
bool AiCmdRideOn(GuestRam& g, uint32_t e, uint32_t cmd, uint32_t sp, AiCmdCallees& c);
bool AiCmdTakeGap(GuestRam& g, uint32_t e, uint32_t target, uint32_t sp, AiCmdCallees& c);
bool AiCmdBackOff(GuestRam& g, uint32_t e, uint32_t target, uint32_t sp, AiCmdCallees& c,
                  const int16_t* sqrtTable);
bool AiCmdClose(GuestRam& g, uint32_t e, uint32_t target, uint32_t sp, AiCmdCallees& c);
bool AiCmdPass(GuestRam& g, uint32_t e, uint32_t target, uint32_t sp, AiCmdCallees& c);
bool AiCmdPassStrike(GuestRam& g, uint32_t e, uint32_t target, uint32_t sp, AiCmdCallees& c);

// 0x800BA4CC, frame 56: the command pass. Dispatches on the arm ADDRESS read out of the guest's own
// table at 0x8005B9B8 (an address that is none of the twelve arms the overlay holds refuses).
// `sqrtTable` is SLUS 0x800560CC (Length3's, for op 6); the window below it must be readable.
bool AiCommandPass(GuestRam& g, int32_t dt, uint32_t skip, uint32_t maskB, uint32_t sp, AiCmdCallees& c,
                   const int16_t* sqrtTable);

} // namespace rr::sim
