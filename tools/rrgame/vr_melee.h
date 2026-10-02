#pragma once
// VR physical combat - OURS: the original has the pad only. In the headset a blow is thrown with the
// body: the tracked fist, or the weapon in the hand, has to really TOUCH a rival rider, as in GTA San Andreas VR
// (the gta-sa-vr-quest project, native\src\Melee.cpp: the swept fist / weapon segment, the calm-to-arm rule, the
// strike window, the tracking-jump guard) and Vice City VR. The combat mode is a setting (vr_melee_settings.h):
// Physical (the VR default), Gesture (vr_handlebars.h's motion punches) or Buttons only; the buttons always work.
//
//   * The colliders (world units; one world unit = one metre times the world-scale setting, vr_rig.h): per hand the
//     FIST - a capsule from the controller's grip pose (the centre of the closed hand) 7 cm along the aim pose's
//     forward (the knuckles), radius "Fist size"; live while the grip button closes the hand (>= 50 %) and the hand is
//     not holding the handlebars. The WEAPON in hand (riderDef +0x2E < 9), in the hand that holds it
//     (VrHandlebars::WeaponHand - the side the rider's weapon object hangs on, else the right): a capsule from the grip
//     along the grip's forward (the way weapon_draw.h's tracked hand points it) as long as the weapon model's own
//     farthest vertex from its grip (BBLEVEL1.GEO model 800, group = the weapon), times "Weapon length", radius 5 cm.
//   * The targets: every other rider live on his bike (pool 0: the rivals, the police on bikes - the only riders the
//     original's fight code can hit; FightUpdate's target is a pool-0 slot), his body as the renderer poses it - the
//     17 parts of BBLEVEL1.GEO model 150 group 0 walked with the PORTED pose's part slots exactly as
//     head_camera.h / rider_pose_draw.h do - one capsule per part from the part's own vertex extent (its long axis,
//     the mean half-width of the other two as the radius); optionally the bike ("Blows on the bike count").
//   * The test is CONTINUOUS: between the last frame and this one the hand's segment and every target capsule are
//     swept together in up to 24 sub-steps, so a fast swing cannot pass through a body between two frames; only an
//     ENTRY counts (not a hand already inside). A contact is a blow when the collider's speed RELATIVE TO THE TOUCHED
//     PART (both ride at racing speed) is at least the minimum (the fist's; a weapon 2/3 of it) and at least 30 % of
//     it goes into the body (not a graze); the hand must have been calm (or 0.45 s past its last blow) since, and the
//     same hand does not hit the same rider twice within 0.5 s. Paused, the chase view, the menu: nothing.
//   * The blow goes to the game as PadState::blow (race_session.h) and is applied on THAT rider by the original's fight
//     code (src\game\fight_physical.h): damage, reaction stance, knock-off, hit sound, HUD, grudges are the game's.
//     "Strength from swing speed" scales the blow's base damage by speed / 4 m/s (0.5 .. 1.5).
//   * Feedback: a haptic thump on the hand at contact, 0.55 + speed / 10 (up to 1), 120 ms.
//   * Drawn: in the Stick mode the tracked hands (the GT2 hands, vr_hands_draw.h) in the head view with the rider's own
//     forearms and gloves hidden ("Hands in Stick mode"; the Handlebars mode draws its own); the weapon in hand in the
//     tracked hand even before the rider's weapon object exists (weapon_draw.h WeaponHandOverride::unheld); with "Show
//     colliders" the fists, the weapon and the near riders' capsules as lines, a contact as a red cross.
//   * The desktop VR mock takes a SCRIPT (--vr-melee-script) of blows aimed at the nearest rider, fed as controller
//     poses through the same path as a headset's (vr_melee.cpp ParseScript).
#include "game/race_session.h"
#include "platform/xr/xr_math.h"
#include "render/mat4.h"
#include "rrvfs/disc_image.h"

#include <cstdint>
#include <memory>
#include <string>

namespace rrgame {

class VrHost;
class VrHandlebars;

// A tracked hand (vr_holsters.h) as this frame's UpdateHands saw it (world; the grip pose's axes, the aim's
// forward; `local` the seat's space, metres) and a segment's nearest rider (VrMelee::Touching).
struct MeleeHandView {
    bool valid = false, grabbed = false;
    float grip = 0.0f, trigger = 0.0f;
    float pos[3] = {}, right[3] = {}, up[3] = {}, fwd[3] = {}, aim[3] = {}, local[3] = {};
};
struct MeleeTouch {
    uint32_t bike = 0; // the rider's bike (pool 0)
    size_t index = 0;  // its slot in the race's bikes
    int part = -1;     // the capsule touched (0..16, 17 the bike)
    float gap = 0.0f;  // world units from the segment's surface (radius included) to the capsule's
};

class VrMelee {
public:
    VrMelee();
    ~VrMelee();
    // The rider model's part capsules, the weapons' lengths (BBLEVEL1.GEO models 150 / 800, RASHCDG.BIN's programs) and
    // the mock's script (--vr-melee-script, only with the desktop mock).
    bool Load(const rr::DiscImage& disc, bool mock);
    // Once per frame after the step: every other rider's body as the renderer poses it (the previous frame's kept for
    // the sweep) and the player's weapon in hand.
    void UpdateTargets(const rr::game::RaceSession& s);
    // In the stereo frame, after VrHandlebars::UpdateHands: the hands, the sweep, the blows, the haptics, the weapon in
    // the tracked hand. `headView`: the view is the rider's head (the only view whose hands are where the rider is).
    void UpdateHands(VrHost& vr, const rr::xr::WorldAnchor& anchor, const VrHandlebars& bars, bool headView, bool paused,
                     long frame);
    // The game's input for the next step: one pending blow (PadState::blow).
    void ApplyToPad(rr::game::PadState& pad);
    // The rider's sub-meshes not drawn in the head view: the forearms and gloves when the Stick mode draws the hands.
    uint32_t HiddenRiderParts(bool headView) const;
    // The Stick mode's hands and the debug colliders into the bound eye framebuffer (single-pass safe: uViewProj).
    void Draw(const rr::render::Mat4& viewProj);
    std::string Totals() const;
    // hand h as this frame's UpdateHands left it; the live rider nearest the segment root-tip (world)
    // within `radius` of one of its capsules (false: none).
    bool Hand(int h, MeleeHandView& out) const;
    bool Touching(const float root[3], const float tip[3], float radius, MeleeTouch& out) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace rrgame
