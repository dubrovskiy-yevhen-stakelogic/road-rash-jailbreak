#pragma once
// Holsters, the inventory in the hands and the swing's rules in VR - OURS, after GTA San Andreas VR (the
// gta-sa-vr-quest project, native\src\Holster.*: body-relative weapon points, a fresh squeeze within reach
// draws, the grip hysteresis 65 / 30 %; PhysicalWeapon.*: released away from its socket the weapon goes back to it,
// the Vice City "grip lock" keeps it). The original has a pad only; everything here is switchable (vr_weapon_settings.h).
//
//   * THE INVENTORY is the game's own (src\game\weapon_inventory.h): the owned weapons are the rider record's mask
//     +0x2C; a rival's WeaponSteal RASHCDG 0x800BFF04 clears the stolen weapon there - it vanishes from the hand and
//     from its holster here; the player's own steal (a bare-fisted blow, command 32) sets it - the weapon arrives in the
//     hand that struck (HolsterHintHand) or the weapon hand; the career carries the fields between races (the original's
//     CareerResult copy-back). The weapon in the hand is +0x2E: drawing writes it as PickWeapon 0x800B9340 selects a
//     weapon, holstering empties it as PickWeapon's unarmed branch (9, no swings); both wait while a weapon OBJECT is in
//     the hand (a fight stance) - the fight code's.
//   * THE HOLSTERS: one on each hip, in the seat's space (the player's body: "Holster height" below the eye, "Holster
//     width" out to each side, "Holster forward"). A weapon is assigned to each (VR menu -> Controls -> Weapons and
//     holsters, from the weapons owned; an empty holster takes a newly owned weapon). The race starts with the weapon
//     the record carries in its holster. A hand within "Holster reach" squeezing its grip afresh (<= 30 % -> >= 65 %)
//     DRAWS that holster's weapon; releasing the grip (<= 30 %) puts it BACK - at a holster within reach it goes to THAT
//     holster (the assignments swap), elsewhere to its own (or, with "grip lock", it stays in the hand). One weapon in
//     the hands at a time (the game has one current weapon): drawing the other puts the first back. A hand holding a
//     weapon does not take the handlebars. Drawn on the hips in the head view (look down), a ring round each
//     (green when a free hand is within reach).
//   * THE SWING (vr_melee.cpp Detect asks SwingTrack / SwingVeto): a contact is a blow only when the weapon's touching
//     point is at least "Weapon swing speed" (3 m/s) against the rider, the swing has TRAVELLED ("Weapon swing travel"
//     25 cm of the weapon's far end, "Punch travel" 10 cm of the knuckles, in the seat's space - the player's own
//     motion, not a rider riding into a still weapon), "Into the target" of it goes into the body, and the hand's last
//     blow is "Blow cooldown" ago.
//   * THE BUTTONS: in the Handlebars mode A / X / B do not attack (only the hands; "Buttons attack in Handlebars mode");
//     the Stick mode keeps them. With the prod or the stun gun (weapons 6 / 7) in a tracked hand, B discharges it: a
//     blow (the original's fight code, command 148) on the rider its far end touches (within 15 cm); touching none, B is
//     the ORIGINAL's attack with them ("Prod / stun gun reach"): combat action 6 (L1 + d-pad Up) on the pad
//     bits - CombatDecode's command 148, the fight code's own target and ReachTest reach - watched until it lands (a
//     hard thump on the hand) or does not (a buzz).
//   * The desktop VR mock takes a SCRIPT (--vr-holster-script, with --vr-melee-script) of holster actions on top of the
//     melee script's hands (vr_holsters.cpp ParseScript).
#include "game/race_session.h"
#include "platform/gamepad_state.h"
#include "platform/xr/xr_math.h"
#include "platform/xr/xr_pad.h"
#include "render/mat4.h"
#include "rrvfs/disc_image.h"

#include <cstdint>
#include <memory>
#include <string>

namespace rrgame {

class VrHost;
class VrMelee;
class VrHandlebars;

// ---- the hooks other VR modules call (a no-op answer when no race runs)
int HolsterWeaponHand(int fallback); // vr_melee.cpp: the hand holding the player's weapon (holsters on), else fallback
bool HolsterHandBusy(int hand);      // vr_handlebars.cpp: this hand holds a weapon - it does not take its grip
void HolsterHintHand(int hand);      // a weapon about to arrive in the player's hand (a steal) goes to this hand
int HolsterHeldWeapon();             // the weapon a hand holds (-1 none) and that hand (-1)
int HolsterHeldHand();
// vr_melee.cpp: the swing's travel (the collider's far end in the seat's space, metres) and the veto of a contact
void SwingTrack(int hand, int kind, const float tipSeat[3], float dt, bool reset);
// (null: a blow - noted as the hand's last; `category` 0 too slow, 1 a short swing, 2 not into the body, 3 cooldown)
const char* SwingVeto(int hand, int kind, float speed, float into, double clock, int& category);
// vr_melee.cpp ScriptHands: the holster script's actions over the mock's scripted hands (seat-space poses, grips)
void HolsterScriptHands(long frame, bool paused, rr::xr::HandPose poses[2], float grip[2]);
// The menu's rows (vr_weapon_settings.cpp)
std::string HolsterWeaponLabel(int weapon);
std::string HolsterOwnedLabel();
int CycleHolsterWeapon(int current, int other, int direction);

class VrHolsters {
public:
    VrHolsters();
    ~VrHolsters();
    // The weapons' shapes (vr_weapon_calib.h) and the mock's script (only with the desktop mock).
    bool Load(const rr::DiscImage& disc, bool mock);
    // After the step: the game's inventory (owned, in hand), a steal, an arrival.
    void UpdateGame(const rr::game::RaceSession& s);
    // In the stereo frame after VrMelee::UpdateHands: the draws and the returns, the calibration page, the weapon in
    // the hand (weapon_draw.h's override) and on the hips (weapon_draw.h's extra weapons).
    void UpdateHands(VrHost& vr, const rr::xr::WorldAnchor& anchor, const VrMelee& melee, const VrHandlebars& bars,
                     bool headView, bool paused, long frame);
    // Before the pad bits: the Handlebars mode's buttons off, B with the prod in the hand a discharge.
    void FilterButtons(rr::platform::GamepadState& gp, const VrHost& vr, const VrMelee& melee, long frame);
    // Before the step: the weapon in hand written to the record; a discharge's blow (PadState::blow when free).
    void ApplyToPad(rr::game::PadState& pad, rr::game::RaceSession& s, long frame);
    // The holsters' rings into the bound eye framebuffer.
    void Draw(const rr::render::Mat4& viewProj);
    std::string Totals() const;
    struct Impl; // (public: the hooks above reach the running race's state)

private:
    std::unique_ptr<Impl> impl_;
};

} // namespace rrgame
