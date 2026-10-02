#pragma once
// The VR combat settings (vr_melee.h): how the player's blows are thrown in the headset. A member of
// VrSettings (vr_settings.h), kept in its [vr] section and edited on the VR menu's Controls page ("Combat options",
// vr_menu.cpp). OURS - the original has the pad only.
//
//   * Buttons + physical (the default in VR): the buttons (A / X / B through the Touch bindings: R1,
//     L1, R2 - the original's pad path, its CombatDecode and reach test, with the weapon in hand) attack in both
//     steering modes, and a free hand still hits by contact as in Physical.
//   * Physical (only): a blow is a CONTACT - the tracked fist (the grip button closed) or the weapon in the hand touches
//     a rival rider's posed body fast enough, moving into it (vr_melee.h); it is applied on that rider through the
//     original's fight code (src\game\fight_physical.h). The combat buttons do not attack (vr_holsters.cpp
//     FilterButtons); the prod / stun gun's B keeps its own rows.
//   * Gesture: the Handlebars mode's motion punches (vr_handlebars.h) - a free hand swung fast away from
//     the body presses the combat button of its side; the original's reach test then decides. The buttons attack too.
//   * Buttons: the controllers' buttons only.
// The mode alone decides whether the buttons attack, in both steering modes.
#include <string>
#include <vector>

namespace rrgame {

struct MeleeSettings {
    enum Mode { kPhysical = 0, kGesture = 1, kButtons = 2, kButtonsPhysical = 3 };
    int mode = kButtonsPhysical;   // [vr] combat = buttons+physical | physical | gesture | buttons
    // the tracked hands hit by contact (vr_melee.h) / the combat buttons attack (vr_holsters.cpp FilterButtons)
    bool Physical() const { return mode == kPhysical || mode == kButtonsPhysical; }
    bool ButtonsAttack() const { return mode != kPhysical; }
    bool strengthFromSpeed = false; // the original's per-blow damage scaled by the contact speed (off: the original's)
    // the fist's minimum speed into the target, decimetres / s (weapons 2/3 of it, and the weapon swing speed,
    // vr_weapon_settings.h); 2.0 m/s by default (at 1.5 a tiny movement was a blow)
    int minSpeedDm = 20;
    int fistCm = 8;                // the fist collider's radius, cm (a capsule from the grip to the knuckles)
    int weaponPct = 100;           // the weapon collider's length, % of the weapon model's own (grip to tip)
    int weaponHand = 1;            // Physical: the tracked hand that holds the weapon, 0 left / 1 right (drawn and hitting)
    bool bikes = false;            // a blow on the rival's bike counts as one on its rider
    bool showHands = true;         // the Stick mode: the tracked hands drawn in the head view (the rider's own arms hidden)
    bool debug = false;            // the colliders drawn (fists, weapon, the rivals' bodies near you)
    bool snatch = true;            // a free hand closing on a rival's swinging weapon takes it (vr_snatch.h)
    bool nunchaku = true;          // the nunchaku / chain swing on a physics chain in the hand (vr_nunchaku.h)

    float MinSpeed() const { return static_cast<float>(minSpeedDm) / 10.0f; }
    bool Parse(const std::string& key, const std::string& value);
    std::string Serialize() const;
    std::string Describe() const;
};

// The "Combat options" page (vr_menu.cpp): the rows, and a row changed by left / right (-1 / +1) or A (0).
struct MeleeMenuRow {
    std::string label, value;
};
std::vector<MeleeMenuRow> MeleeMenuRows(const MeleeSettings& s);
bool MeleeMenuActivate(MeleeSettings& s, int row, int direction);
const char* MeleeModeName(int mode);

// [vr] combat_defaults=1cr marks a section written with the current combat defaults; a section without it (an older
// file) starts from Buttons + physical once (vr_settings.cpp LoadVrSettings, logged).
constexpr const char* kCombatDefaultsKey = "combat_defaults";
constexpr const char* kCombatDefaultsValue = "1cr";

// The flags (vr_settings.h ApplyVrFlag forwards them): --vr-combat buttons+physical|physical|gesture|buttons, --vr-melee-strength 0|1,
// --vr-melee-min-speed M (m/s), --vr-melee-fist-cm N, --vr-melee-weapon-pct P, --vr-melee-bikes 0|1,
// --vr-melee-hands 0|1, --vr-melee-debug 0|1, --vr-melee-weapon-hand left|right, --vr-melee-snatch 0|1,
// --vr-melee-nunchaku 0|1, and --vr-melee-script <file |
// "frame command; ..."> (the desktop VR mock's scripted blows, vr_melee.h). False when argv[i] is not one; a bad value
// throws.
bool ApplyMeleeFlag(int argc, char** argv, int& i, MeleeSettings& s);
const std::string& MeleeScriptArgument();

} // namespace rrgame
