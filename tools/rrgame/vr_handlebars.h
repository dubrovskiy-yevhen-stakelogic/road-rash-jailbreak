#pragma once
// Riding with the hands on the handlebars - the VR steering mode "Handlebars" (vr_bars_settings.h),
// after GTA San Andreas VR (the gta-sa-vr-quest project, native\src\Driving.cpp: the motorcycle's two-hand /
// one-hand bar solver, the twist-grip throttle, the grab hysteresis) and GT2 VR (the gt2-play project,
// src\platform\xr\vr_driving.cpp: the grab by the grip button near the rim, the reference taken at the grab so the
// bars never jump). OURS - the original has a pad only.
//
//   * The grips are the rider's own gloves (vr_bar_grips.h), in the bike's frame: they lean and turn with the bike as
//     the game draws it. A hand GRABS its grip (left hand the left grip) when the grip button goes past 65 % within
//     20 cm of it; it lets go below 30 %.
//   * The bar angle: two hands - the angle of the line between them about the bike's up axis (the right grip forward
//     = the bars turned left); one hand (when allowed) - that hand's travel mirrored to the other grip. Referenced at
//     the grab, clamped to the full lock (30 degrees / sensitivity), a dead zone, then the ORIGINAL's analogue path:
//     the pad becomes the DualShock in ANALOG mode (device 0x73, pad_product.h) with the left stick X byte = the bars,
//     which the PORTED pad reader turns into the steering axis 0x800CE544 (AxisCurve 0x8001CA58 with ENV.EN's own
//     dead zone and curve) and BikeAimTarget scales by the speed band (flagsA bit 20, bike.h) - the
//     proportional steering the console's analogue controller had, not a pulsed digital key.
//   * The throttle and the brake: the analogue right stick Y byte of the same device (axis 0x800CE540, BikeStep D1:
//     up = drive, down = brake): the right grip twisted toward you (twist throttle, 1.5 .. 35 degrees of wrist roll
//     about the grip's axis, referenced at the grab) or the right trigger, the brake the left trigger (a lever).
//   * Fists: a hand off the bars swung faster than 1.8 m/s punches on its side - right R1 (combat action 1), left L1
//     (action 2); swung mostly downward it kicks (R2, action 3). The buttons A / X / B keep their bindings.
//   * Haptics: a pulse on the grab, a thump on the swinging hand when the blow lands, the road through the held grips
//     (with the speed); the game's own rumble (hits, crashes) comes on top through the host.
//   * The gloves: drawn procedurally (vr_hands_draw.h) at the controllers - closed round the grip while holding (the
//     glove then sits on the bar, turned with it, the right one rolled by the twist), following the grip button
//     otherwise. A held hand is RIGID on the grip of the bike as the renderer drew it this frame (its model matrix -
//     heading, lean, pitch - and part slots - the fork's turn), the fist's centre on the grip point, as GTA SA VR's
//     bikeHandsFollowTilt; the controller then only steers and twists. The rider's own forearms and gloves are not
//     drawn in this mode's head view (they would be a second pair of hands). In the Stick mode nothing of this runs:
//     no gloves, the pad alone.
//   * The desktop VR mock takes a SCRIPT instead of controllers (--vr-bars-script): per frame the hands' states in the
//     bars' own terms (grab, turn the bars, twist, release, swing), turned into controller poses in the seat's space
//     and fed through the same solver as a headset's.
#include "game/race_session.h"
#include "platform/xr/xr_math.h"
#include "platform/xr/xr_pad.h"
#include "render/mat4.h"
#include "rrvfs/disc_image.h"
#include "vr_bar_grips.h"
#include "vr_hands_draw.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace rrgame {

class VrHost;

class VrHandlebars {
public:
    VrHandlebars();
    ~VrHandlebars();
    // The rider model (the gloves) and the mock's script (--vr-bars-script, only with the desktop mock).
    bool Load(const rr::DiscImage& disc, bool mock);
    // Once per frame after the step: the bike's frame and the grips (vr_bar_grips.h).
    // `headView`: the head view is chosen - the bike then takes the visual lean (vr_visual_lean.h).
    void UpdateBike(const uint8_t* ram, uint32_t bike, uint32_t rider, const float* seat, bool headView = false);
    // The visual lean of this frame's drawn bike (off: none): main.cpp turns the head camera and the renderer's
    // player bike by it (GameView::playerLean).
    const VisualLean& Lean() const;
    // [vr] eye_on_bike: the head view's eye fixed on the DRAWN bike where the rider's head `headEye`
    // (world, the record's own lean) was on the first seated head-view frame; false: the option is off (or no bike) -
    // the caller keeps the animated head (turned by Lean()).
    // `back` world units behind that point along the drawn bike's forward ([vr] seat_back_cm).
    bool SeatEye(const float headEye[3], float out[3], float back = 0.0f);
    // the model's wheel contacts (vr_horizon.h) in the bike's frame ([0] up, [1] fwd, world units)
    bool WheelContacts(float front[2], float rear[2]) const;
    // the fork slot the head view draws the player's bike with while the bars are held - turned to the
    // angle the hands command (BarGrips::SetForkTurn); false: the game's own slot (the renderer keeps it).
    bool ForkOverride(rr::PartMatrix& out) const;
    // In the stereo frame, after LocateStereo: the hands in the world, the grabs, the bars, the throttle, the swings,
    // the haptics. `headView`: the view is the rider's head (the grips are within reach only there); `paused`: the
    // pause menu; `landed`: the frame's landed blows (FightCounts::landed).
    void UpdateHands(VrHost& vr, const rr::xr::WorldAnchor& anchor, bool headView, bool paused, long frame, size_t landed);
    // The game's input for the next step: in the Handlebars mode the analogue device (the bars or the left stick, the
    // throttle and the brake) and the swings' combat bits. The Stick mode: nothing changes.
    void ApplyToPad(rr::game::PadState& pad, long frame);
    // The rider's sub-meshes not drawn in the head view in the Handlebars mode: the forearms and the gloves.
    uint32_t HiddenRiderParts(bool headView) const;
    // The gloves (and the grip markers) into the bound eye framebuffer, depth-tested against the scene.
    // `drawnModel` / `drawnParts` - the player's bike exactly as the renderer drew it this frame (race_render.h
    // DrawnMachine: the model matrix with the lean, the part slots; parts null = the rest pose); a held hand is seated
    // on the grip of THAT bike. Null model: this frame's own read of the same record (BarGrips::Model).
    void Draw(const rr::render::Mat4& viewProj, const rr::render::Mat4* drawnModel = nullptr,
              const rr::PartMatrix* drawnParts = nullptr);
    bool Active() const; // the Handlebars mode
    // ---- VR physical combat (vr_melee.h): the hands as this frame's UpdateHands left them
    struct HandView {
        bool grabbed = false;              // on its grip
        float grip = 0.0f, trigger = 0.0f; // the buttons
        rr::xr::WorldEye world{};          // the grip pose in the world
        float aimForward[3] = {0, 0, 1};   // the aim pose's forward (the fingers)
        float local[3] = {0, 0, 0};        // the grip pose in the seat's space, metres
        // the drawn bike's frame (right / up / fwd, metres; the lean and the pitch as drawn): the hand, and the grip it
        // holds or reaches for where the bars are turned now (the height setting included)
        float bike[3] = {0, 0, 0};
        float gripBike[3] = {0, 0, 0};
        bool bikeValid = false;
    };
    bool Hand(int h, HandView& out) const;  // false: the Stick mode (no hands tracked here) or not tracked
    int WeaponHand() const;                 // 0 left / 1 right: the hand the player's weapon is drawn in
    void Haptics(float out[2]) const;       // the levels UpdateHands gave the host this frame (0 in the Stick mode)
    std::string Totals() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace rrgame
