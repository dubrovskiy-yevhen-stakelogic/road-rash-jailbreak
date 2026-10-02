#pragma once
// The police chase in the product: the PORTED cop functions of
// src\game\sim\cops.{h,cpp} run on the session's guest arena with the product's callees. ai_race.cpp's
// command-pass and planner callees forward the cops' arms here (op 1, op 2's gap, op 17, the planner's
// cop tail and the cop-mode arrest scan).
#include <cstdint>

#include "game/ai_race.h"
#include "game/sim/road_query.h"

namespace rr::game {

// Each runs one PORTED function on the arena `ram` (2 MiB, guest layout) through `g`; false = a callee
// failed or an address faulted (the caller's pass then refuses, as for any other arm).
//   CopIdle 0x800BAA2C (op 1), CopDist 0x8009E444 (op 2's gap), CopIntercept 0x800BBEBC (op 17),
//   CopTail 0x8009DC90 (the planner), CopArrestScan 0x80097388 (the planner, cop mode).
// The product's callees: AiPushCommand / AiClearCommands / AiPopCommand PORTED (ai.h) on the arena,
// EndRace through `hooks.endRace` (PORTED, spine.h, the session's), ComputePlace through
// `hooks.computePlace`; the cop-mode quota test / release / arrest 0x8009DA4C / 0x800A0708 / 0x80096F30
// PORTED (sim\modes.h; the last two through `hooks.jailRelease` / `hooks.arrest`, race_modes.cpp); NOT
// ported and named in the seam list: the busted music SLUS 0x8001B3C8.
bool ProductCopIdle(uint8_t* ram, rr::sim::GuestRam& g, const AiProductHooks& hooks, AiPassCounts& counts,
                    uint32_t e, uint32_t cmd, int32_t dt, uint32_t sp);
int32_t ProductCopGap(rr::sim::GuestRam& g, uint32_t e, uint32_t t, uint32_t sp);
bool ProductCopIntercept(uint8_t* ram, rr::sim::GuestRam& g, const AiProductHooks& hooks, AiPassCounts& counts,
                         uint32_t e, uint32_t target, uint32_t sp);
bool ProductCopTail(uint8_t* ram, rr::sim::GuestRam& g, const AiProductHooks& hooks, AiPassCounts& counts,
                    uint32_t e, uint32_t sp);
bool ProductCopArrestScan(uint8_t* ram, rr::sim::GuestRam& g, const AiProductHooks& hooks, AiPassCounts& counts,
                          uint32_t e, uint32_t sp);

// DEVELOPMENT CONTROL (rrgame --shadow <bike>): a SCRIPTED player - ours, at the pad only, like
// ai_race.h's AutoSteer - that rides beside rival `bike`: it steers its road lateral +0x158 toward the
// rival's plus `offset` (in the travel sense; damped by its own drift) and paces its progress +0x144
// to the rival's (throttle while behind, brake when more than 3 units ahead). It presses no fight
// button: whatever fight follows is the AI's own. It exists so that a scripted run can put
// the player where AiPlan's reach test (0x800BEA30: within 1.5625 across at racing speed) can find it.
struct ShadowSteer {
    uint32_t rival = 0;       // the rival's pool-0 address
    int32_t offset = 0x10000; // the lateral to hold beside it (16.16)
    int32_t prevLat = 0;
    bool have = false;
    // -1 = hold Left, +1 = Right, 0 = neither (measured: Right lowers +0x158 in the travel sense).
    int Decide(const uint8_t* ram, uint32_t player, bool& throttle, bool& brake);
};

} // namespace rr::game
