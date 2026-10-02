#pragma once
// The wheelie's picture and the VR gesture (src\game\wheelie.h). OURS, render and input only.
//   * AddWheeliePitch: the player's drawn bike (and all that rides on its VisualLean - the rider, the head camera, the VR
//     grips, the held hands and the eye) pitched nose up about its rear wheel's contact line, after the lean.
//   * WheelieViewComfort: the VR head view keeps WheelieSettings::viewPitchPct of that pitch (30 %) - the eye rides the
//     bike up and back whole, the horizon tilts only by that share.
//   * WheelieBarsInput: the Handlebars mode's gesture, after GTA SA VR's UpdateBikeLeanLocked (the gta-sa-vr-quest project,
//     native\src\Driving.cpp: both real hands on the bars raised = lean back). Each hand's height is taken over the
//     grip it holds in the DRAWN bike's frame (the lean, the pitch, the shake and the eye move the grips and the eye
//     together - a hand that stays on its grip reads 0); its rest height is taken at the two-handed grab, settles for
//     0.6 s, then follows down quickly and up only slowly. The start: BOTH hands up together - each at least three
//     quarters of WheelieSettings::liftCm, their mean liftCm - held holdMs with the throttle (game/wheelie.h's gate).
//     Under way the lift is held in the seat's space from that moment (the rising front carries the grips toward the
//     player) and leans back from half of liftCm (the release) to liftFullCm (full).
//   * WheelieStickBack: a stick pulled back (a desktop controller's left stick, the Touch left stick in VR) as 0..1 of
//     its travel.
#include "vr_visual_lean.h"

#include <cstdint>
#include <string>

namespace rrgame {

class VrHandlebars;

// `lean` gets the player bike's wheelie pitch (src\game\wheelie.h DrawnPitch) after its lean; no pitch: unchanged.
// `model` - the unleaned model matrix (race_render.cpp RecordModelMatrix's axes and origin); the overload without it
// builds one from the bike's rows +0x1B0 and box centre +0xB8. `contactUp` as VisualLean::From's.
void AddWheeliePitch(VisualLean& lean, const rr::render::Mat4& model, const uint8_t* ram, uint32_t bike, float contactUp);
void AddWheeliePitch(VisualLean& lean, const uint8_t* ram, uint32_t bike, float contactUp);
// The VR head view's camera (after the lean turned it): `fwd` / `up` turned back by (1 - keep) of the pitch.
void WheelieViewComfort(const VisualLean& lean, float fwd[3], float up[3]);
// The gesture this frame (WheelieIn barsWant / barsKeep); false when both hands are not on the bars (the Stick mode,
// a hand off) - then want is false and keep 0.
// `lowOut` / `meanOut` (optional): the lower hand's lift and the pair's mean (metres, for the logs); `legacyOut`: the
// earlier gesture's pull (a meter, and the control RRJB_WHEELIE_INPUT=legacy - game/wheelie.h).
bool WheelieBarsInput(const VrHandlebars& bars, long frame, bool& want, float& keep, float* lowOut = nullptr,
                      float* meanOut = nullptr, float* legacyOut = nullptr);
// 0..1 of the travel back from a stick's Y byte (0x80 centre, 0xFF full back / down); 0 without a controller.
float WheelieStickBack(bool connected, uint8_t ly);
// "vr wheelie: ..." (the gesture's totals) - empty when it never ran
std::string WheelieBarsTotals();

} // namespace rrgame
