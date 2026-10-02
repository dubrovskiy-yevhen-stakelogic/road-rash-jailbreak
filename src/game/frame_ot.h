#pragma once
// The frame's ordering tables where the original keeps them.
//
// SLUS 0x8001C1AC (SHA-1 67ed165a...), run once by the loader: the frame record *(0x8005B470) (gp+0x7E4) gets
// +0xF4 = 2 ordering tables with one player (4 with two), each its own malloc(0x1450) (0x514 entries) at
// +0xF8.., +0x108 = the first, *(0x8005AE00) = 0, and ClearOTagR(+0x108, 0x514). The packet heap (+0x10C .. the
// end word *(0x8005B4D0)) is a block of its own; in rr-race the OT sits at 0x800FB974, above the heap's end
// 0x800F0460.
//
// OURS, named: where the blocks sit (the session's bump region, as the packet heap); the frame flip of +0x108
// between the tables (the renderer is not the GPU: the effects keep writing the first).
// RRJB_FXOT=top is the negative control (the effects carve their OT from the heap's top, fx_runtime.cpp, which
// moves the heap's end).
#include <cstdint>
#include <string>

#include "game/sim/road_query.h"

namespace rr::game {

constexpr uint32_t kFrameOtBytes = 0x1450, kFrameOtEntries = 0x514;

// False with RRJB_FXOT=top.
bool FrameOtsPorted();
// The blocks taken from [from, limit) for `players` (the malloc calls of 0x8001C1AC); returns the log line.
std::string ReserveFrameOts(uint32_t& from, uint32_t limit, int players);
// 0x8001C1AC's stores on the frame record `record` (after the HUD arena has placed it); false when not reserved.
bool ApplyFrameOts(rr::sim::GuestRam& g, uint32_t record);

} // namespace rr::game
