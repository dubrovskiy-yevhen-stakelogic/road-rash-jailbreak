#pragma once
// The race passes in the product: RaceTick's world pass 0x8008AC80 and
// rider / engine pass 0x8008ACE8, and the rider pass 0x8007B840 under it, run WHOLE by their ports
// (src\game\sim\passes.h) instead of the session's own ordering of their children; the activation pass
// 0x80093E6C and the downed-rider pass 0x800950E8 by their ports instead of the session's slot loops.
//
// `RRJB_PASSES=session` is the negative control: the session's previous ordering (the rider pass without
// its class-list walk, the per-bike view distance before the step, the slot loops that restore a refused
// bike on its own).
#include <cstddef>
#include <cstdint>
#include <string>

namespace rr::game {

// False with RRJB_PASSES=session.
bool PassesWhole();

struct PassTotals {
    size_t worldPasses = 0, engineWholePasses = 0, riderPasses = 0, riderPassRefused = 0;
    size_t classBikes = 0, classFrames = 0, classRefused = 0;       // the class-list walk [0x8007C9DC, 0x8007DCD4)
    size_t activationPasses = 0, activationRefused = 0;             // 0x80093E6C, a refused pass restores the arena
    size_t downedPasses = 0, downedRefused = 0;                     // 0x800950E8
    size_t hazardPasses = 0, hazardRefused = 0;                     // 0x800A13C4
    size_t sessionWorld = 0, sessionEngine = 0;                     // RRJB_PASSES=session frames
    size_t clockFrames = 0, firstFrames = 0, resultsPrepared = 0;   // the frame's clock, main's first-race-frame arm, 0x8003F708
};
PassTotals& PassRunTotals();
std::string PassTotalsLine();

} // namespace rr::game
