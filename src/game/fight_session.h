#pragma once
// The product side of combat: what race_session.cpp calls through its
// small hooks. The ported fight code itself is src\game\sim\fight.{h,cpp}; the member functions of
// RaceSession that drive it are defined in fight_session.cpp.
#include <cstddef>
#include <cstdint>
#include <string>

namespace rr::game {

// Per frame: what the fight code did (rrgame's frame log).
struct FightCounts {
    size_t decodes = 0;      // CombatDecode 0x800C2348 issued a move (the pad's action edge)
    size_t combos = 0;       // ComboInput 0x800C258C ran (the player's top command is 16)
    size_t updates = 0;      // FightUpdate 0x800C035C ran (the op-16 arm, player and AI)
    size_t refused = 0;      // ... refused at an unported callee (the arena was restored)
    size_t leaves = 0, enters = 0; // the stance layer's combat children 0x800BFD24 / 0x800BFC5C
    size_t strikeFrames = 0; // the player's FightStep reached its node's hitFrame (rider +0x222 0 -> 1)
    size_t strikes = 0;      // ApplyHit 0x800C17B0 ran for the player (its NoteHit stamped the last-blow table,
                             // which it does on a MISS too - not evidence of a hit)
    size_t landed = 0;       // ... with ReachTest's reach bit: the blow landed (FightStat(player, 2, 0) went up)
    size_t hits = 0;        // riders whose health riderDef+0x0F went down this frame
    size_t knockOffs = 0;    // ... to 0
    // The player's fight geometry as FightUpdate sees it (the log; FightUpdate's own AiProject / CanEngage
    // on the arena): along 16.16 (|x| <= 0.7 to strike), the road lateral (|x| <= 2.3), CanEngage's 0/1.
    int32_t along = 0, lat = 0, engage = -1;
    int32_t dy = 0, height = 0;   // ReachTest's |dy| < me+0x138 gate
    int32_t ownLat = 0;           // (me - target) road lateral in the player's own direction (the --chase script)
    std::string note;        // "b<i> hp a->b" per hit this frame
};

// The run's line for the pad combat gate (fight_session.cpp FightPadPass).
std::string PadGateTotals();
// The run's fight totals: the player's blows, landed / missed, riders hurt, knock-offs.
std::string FightTotals();
// VR physical combat (fight_physical.h): the contacts applied through the fight code.
std::string PhysicalBlowTotals();
// VR weapon snatching: the last grab's outcome, for the VR layer's hold (tools\rrgame\vr_snatch.h).
enum class SnatchOutcome { kNone = 0, kWait, kStolen, kDeclined, kSkipped, kRefused };
struct SnatchResult {
    uint32_t frame = 0;       // the game frame it was applied in
    uint32_t victim = 0;      // the rival's bike
    SnatchOutcome outcome = SnatchOutcome::kNone;
    int weapon = 9;           // the weapon it was about (the rival's in hand at the grab)
    uint64_t serial = 0;      // one more per grab applied
};
const SnatchResult& LastSnatch();

} // namespace rr::game
