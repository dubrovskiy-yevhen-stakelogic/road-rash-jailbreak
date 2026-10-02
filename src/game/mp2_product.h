#pragma once
// The remaining two-player functions in the product: the PORTED cell-state copy SLUS
// 0x8001339C and class-50 release RASHCDG 0x800A3ECC (src\game\sim\mp_world.h, run by the streamer's callees,
// stream_session.cpp), the PORTED two-player shadow SLUS 0x80026960 (src\game\sim\shadow2p.h, run at the emitter's
// tail, shadow_product.cpp, its sprite drawn by fx_draw.h), the unlit ramp of the model light when game_state+4 bit 4
// is set (race_scene.h SetModelLit) and player 2's own sidecar rig (race_scene.h LoadSidecarFor).
//
// RRJB_MP2=off: none of them (the negative control) - the streamer answers 0x800A3ECC (not run) and skips
// the copy, the two-player shadow is the renderer's own approximation, the model light falls back to a plain lambert
// in two-player mode, and player 2's machine takes player 1's rig (or none).
#include <cstddef>
#include <cstdint>
#include <string>

#include "game/sim/road_query.h"
#include "game/sim/shadow.h"

namespace rr::game {

struct ShadowFrame;

bool Mp2On();

struct Mp2Totals {
    size_t cellCopyCalls = 0, cellCopyRan = 0; // 0x8001339C called / in two-player mode with both slots
    size_t class50Calls = 0, class50Freed = 0; // 0x800A3ECC called / a record released
    size_t shadowCalls = 0, shadowPackets = 0, shadowRefused = 0, shadowDrawn = 0;
    size_t unlitViews = 0;                     // view-frames drawn with the unlit ramp (bit 4 set)
    size_t rig2Frames = 0;                     // player 2's machine drawn as its own rig
};
Mp2Totals& Mp2();

// The streamer's two callees (stream_session.cpp ProductStreamCallees): the tail of the region-0 relocation
// 0x800135E8 (its a0 `slotPlus4` = the slot + 4 -> {body, id}, a1 `p` passed through) and CellRelease's two-player
// partner. Both false (nothing run) with RRJB_MP2=off.
bool Mp2CellCopy(rr::sim::GuestRam& g, uint32_t slotPlus4, uint32_t p);
bool Mp2Class50Release(rr::sim::GuestRam& g, uint32_t id, uint32_t p);

// The emitter's tail in a two-player race (shadow_product.cpp ShadowTail, after EmitterShadow reported the two-player
// form): the PORTED Shadow2p 0x80026960 through EmitterShadow2p; each packet it links is read back as an FxPacket of
// `view` (its OTZ * 4 the depth) into `out.sprites`, which the frame draws with the effects (fx_draw.h, the sheet's
// sprite 0). Nothing with RRJB_MP2=off or RRJB_SHADOW=ours.
void Mp2ShadowTail(rr::sim::GuestRam& g, const rr::sim::shadow::Gte& gte, uint32_t obj, uint32_t view, uint32_t w24,
                   ShadowFrame& out);

// One line for the run's log.
std::string Mp2Report();

} // namespace rr::game
