#pragma once
// The AI rider's strike: op 9's arm of the AI command pass (AiCmdPassStrike 0x800BBE00) calls it when the
// target is within reach beside it. Ported from our own disassembly of the player's own overlay:
//
//   RASHCDG.BIN  SHA-1 cfe43a7786759f2cb9c57751cf99e84d1074782c, loaded at 0x8005B5E8
//
// and accepted only by the `rrverify phys` row `strike` (tools\rrverify\rows_strike.inc).
// Memory model and callees are fight.h's: guest addresses on a GuestRam view, the stance event and the
// clip test through fight::Callees, the fight machine's pieces (FightStep, FightRestart, ReachTest,
// ApplyHit) the PORTED ones of fight.cpp.
#include <cstdint>

#include "game/sim/fight.h"

namespace rr::sim::fight {

// RASHCDG 0x800C1370 Strike(me, t) - 524 B, frame 56. The road lateral of `t` from `me` (the road offsets
// when both ride the same road piece, else AiProject 0x800B6AAC of t's position on me's side axis) and
// `me`'s along-distance on t's heading axis; then:
//   * `me`'s rider already in a fight stance (category 3): riderDef +0x3C = 71 and FightStep - its strike
//     frame arms the reach test with flags 32;
//   * else, |along| < 0xFFFF and |lateral| < riderDef+8 + me+0x130: riderDef +0x3C = 71 and FightRestart
//     (a new swing on the lateral's side), flags 48;
//   * armed: ReachTest(me, t, |lateral|, along, &flags) and on a hit ApplyHit(me, t, side, flags);
//   * not armed, the rider on the bike (+0x25C == 1), not in a fight stance, and (stance 16 only) its clip
//     done: the stance event 16 (the idle lean toward the side, p = side | 2).
// False: a callee refused or the view faulted; nothing written is to be trusted.
bool Strike(GuestRam& g, Callees& c, uint32_t me, uint32_t t);

// What one Strike call did (the product's counters): which branch armed it, whether the reach test hit.
struct StrikeTrace {
    bool stepped = false;   // the FightStep branch ran
    bool restarted = false; // the FightRestart branch ran
    bool armed = false;     // flags != 0: the reach test ran
    bool hit = false;       // ReachTest true: ApplyHit ran
    bool lean = false;      // the stance event 16 ran
};
bool Strike(GuestRam& g, Callees& c, uint32_t me, uint32_t t, StrikeTrace& trace);

} // namespace rr::sim::fight
