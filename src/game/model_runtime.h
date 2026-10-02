#pragma once
// The model draw in the product: the bike and rider models loaded into the
// arena the way the race loader loads them, the bikes' and riders' part slots bound to them (so the
// crash emitter and the effect capture find their DPD3 records), and per frame the PORTED visibility
// test and model draw (src\game\sim\model_draw.h) with the emitter's effect-capture head - which is what
// gives the effect records in states 3..6 (part streaks, crash debris, police lights, weapon trails)
// their points.
//
// THE ARENA - the race loader's model part, transcribed from our own listing of RASHCDI.BIN (SHA-1
// 9a8b79d8739713c12b0a2dc4d75ef8f0a2cc0b06): every RMD3 of DATA\BBLEVEL<bank+1>.GEO but model 800 (the
// weapon arena's) through the chunk walker 0x8005C0C4 and its RMD3 / DOD3 / DPD3 / BBD3 handlers
// 0x8005CB9C / 0x8005CC4C / 0x8005CD60 / 0x8005CE78 (restated from weapon_session.cpp's transcription),
// and the model draw's three vertex buffers *(0x8005ACB0) / *(0x8005ACB4) / *(0x8005ACB8), 1200 vertices
// each as every race capture holds them (16 / 4 / 1 bytes a vertex). OURS, named: where they sit (the
// session's bump region), the registry slots they take (after the cars and model 800).
//
// THE BINDING - RegistryBind SLUS 0x8002FDEC's fields for an object that keeps its state: +0x60 the
// registry record, +0x64 the kind's LOD table (0x80054178 bikes, 0x80054198 riders), then the PORTED
// LodSelect SLUS 0x8001298C to LOD 0 (+0x00 the DOD3, +0x28 the radius, each part slot's DPD3 pointer).
// OURS, named: which model - the product draws BBLEVEL1.GEO models 100 / 150 for every bike and rider
// (race_scene.cpp), so every bike is bound to the bank's first bike model, the police to its third (the
// captures' cops: 118 for bank 0 - their lights sit on that model's polygons), every rider to 150; the
// original picks the bike by the rider's class. The pose keeps working: the DOD3 fields it reads
// (+0x0E, +0x14, +0x18) are the ones rider_pose.cpp's model records copied.
//
// THE FRAME - run by the effect runtime before its pass (fx_runtime.h's model hook), on its scratchpad:
// ViewDistance 0x8008DBCC and ModelLod 0x800667C4 on every car (RASHCDG 0x8008CFDC's pool-3 loop and the LOD
// by distance; bikes and riders stay at LOD 0, the only LOD the product draws them at), ModelVisible
// 0x80067AC4 on every top-level entity for view 0, and ModelDraw 0x80068468 on the visible ones. OURS, named:
// a car is in no cell (+0xB0 = -1; the pass that files entities into cells is not ported), which ends
// ModelVisible at its first test - such a car gets ModelVisible's LOD step (LodSelect to +0x0A) from us and
// its effect pass as before, without a model draw. The emitter SLUS 0x800251E4 is served by its head only: the effect
// capture 0x80029CA4 (PORTED, effects.h) under the original's condition and its two glow sprites (PORTED,
// fx_glow.h: WeaponGlow 0x80027B80, BikeLight 0x80028534) under theirs, in the emitter's order, into the effect
// pass's ordering table (`fx`, set by the effect runtime for the call; RRJB_FXDRAW=off: counted, not run -
// the negative control); its primitives are the renderer's (the GL scene draws the models). The draw list *(0x8005B280) is emptied before and after
// (OURS: its consumer 0x80067770 empties it as the effect pass walks it).
#include <cstdint>
#include <string>
#include <vector>

#include "game/sim/effects.h"
#include "game/shadow_product.h" // the emitter tail's model shadow SLUS 0x80025EE0
#include "game/sim/model_draw.h"
#include "game/sim/road_query.h"
#include "rrvfs/disc_image.h"

namespace rr::game {

std::string BuildModelArenaLine(rr::sim::GuestRam& g, const rr::DiscImage& disc, int bank, uint32_t& from,
                                uint32_t limit);

struct ModelBinding {
    uint32_t object = 0;
    uint32_t model = 0; // model id
};
// The ids BindModels uses for the product's machines of bank `bank`.
uint32_t ProductBikeModel(int bank, bool police);
constexpr uint32_t kProductRiderModel = 150;
std::string BindModels(rr::sim::GuestRam& g, const std::vector<ModelBinding>& list);

class ModelRuntime {
public:
    // Per frame, on `g` (the effect runtime's view, scratchpad attached, depth ranges written): the LOD
    // pass on the cars, the visibility test and the model draw on `entities`; `drawn` gets the visible
    // ones in order. False when the arena has no vertex buffers (nothing ran).
    // `view` 1 (two players): the visibility test and the draw for view 1 at its GTE offset (ofx, ofy); the
    // cars' distance / LOD step is the frame's (0x8008CFDC runs once, before the view loop) and runs for view 0.
    bool Frame(rr::sim::GuestRam& g, const std::vector<uint32_t>& entities, std::vector<uint32_t>& drawn,
               uint32_t view = 0, int32_t ofx = 192, int32_t ofy = 120);
    // Run totals, for the log.
    size_t frames = 0, visible = 0, hidden = 0, drawCalls = 0, refused = 0, captures = 0, weaponGlows = 0,
           headlights = 0, carLods = 0, maxCarLod = 0;
    uint32_t minCarDist = 0xFFFFFFFFu; // the nearest a car came to view 0 (+0x2C, 1/64 world unit)
    size_t cellless = 0;               // car frames in no cell (+0xB0 = -1): LOD applied, no model draw
    // The effect pass's environment during Frame (fx_runtime.h sets it): the glow sprites' packets go to its
    // sink. Null: the sprites are counted, not run.
    rr::sim::FxEnv* fx = nullptr;
    size_t glowPackets = 0, lightPackets = 0, glowsNotRun = 0; // the glow sprites' packets linked / calls not run
    size_t weaponEmits = 0, weaponEmitsHigh = 0; // weapon objects (kind 5) the emitter saw, at LOD 6..8
    uint32_t firstRefusedObj = 0, firstRefusedFault = 0, firstRefusedKind = 0; // the first refused ModelDraw
    ShadowFrame shadows; // shadow_product.h: the emitter tail's shadow quads, this frame's
    std::string Totals() const;
};

} // namespace rr::game
