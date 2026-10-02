#pragma once
// The two glow sprites of the primitive emitter SLUS 0x800251E4, transcribed
// from our own disassembly of SLUS_010.53 (SHA-1 67ed165a2c517d4e6106fb0dfa324d66dd9a76f1, text at
// 0x80010000) and accepted only by `rrverify phys` rows (tools\rrverify\rows_fxdraw.inc).
//
//   SLUS 0x80027B80 WeaponGlow   a weapon object (DOD3 kind 5) at LOD 6 / 7 / 8 in a swing: one POLY_FT4
//                                (0x2F, raw) in SCREEN space on the model draw's projected vertex 14 / 4 / 1,
//                                sprite 3 / 10 / 4 of the effect sheet, its width the screen spread of two
//                                vertex pairs scaled by the vertex's depth; *(gp+0x8A4) = the vertex
//   SLUS 0x80028534 BikeLight    a bike (kind 2, headlight bit 18 of +0x24, LOD 0): one POLY_FT4 (0x2E,
//                                colour 0x000060) around the midpoint of two projected vertices of the
//                                primitive the class table 0x800537D8 names, sprite 5, turned by
//                                EffectQuadSpin 0x80029048, sorted three buckets nearer
//
// Both read the model draw's buffers (*(0x8005ACB0) camera-space vertices, *(0x8005ACB4) screen points)
// the ModelDraw of the same object has just filled, the depth ranges RenderModels 0x800674D4 left in the
// scratchpad, and link into the frame's ordering table like the effect emitters (effects.h); the packet
// heap's overflow arm SLUS 0x80021C98 is not ported (the call refuses).
#include <cstdint>

#include "game/sim/effects.h"

namespace rr::sim {

constexpr uint32_t kFxWeaponGlowFn = 0x80027B80; // SLUS
constexpr uint32_t kFxBikeLightFn  = 0x80028534; // SLUS
constexpr uint32_t kFxGlowVertex   = 0x8005B530; // gp+0x8A4: the vertex WeaponGlow read
constexpr uint32_t kFxLightClass   = 0x800537D8; // {s8 primitive, s8 part set, ...} 6 bytes per class +0xB4
constexpr uint32_t kFxAnimTables   = 0x8005AD4C; // -> 12-byte animation records (+8 the frame list)

// The emitter's two tests (0x80025218..0x800252EC and 0x80025360..): `w24` is the object's +0x24 as
// the emitter read it on entry.
bool WeaponGlowWanted(GuestRam& g, uint32_t obj);
bool BikeLightWanted(GuestRam& g, uint32_t obj, uint32_t w24);

void WeaponGlow(GuestRam& g, FxEnv& env, uint32_t obj);
void BikeLight(GuestRam& g, FxEnv& env, uint32_t obj);

} // namespace rr::sim
