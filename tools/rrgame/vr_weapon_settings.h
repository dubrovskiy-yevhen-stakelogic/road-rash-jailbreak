#pragma once
// The VR weapons' settings: the holsters (vr_holsters.h), the weapon's grip in the hand per weapon
// (vr_weapon_calib.h) and the swing's thresholds (vr_melee.h's contacts). A member of VrSettings (vr_settings.h), kept
// in its [vr] section; edited on the VR menu's "Weapons and holsters" and its "Calibrate the grip" page, the swing rows
// on "Combat options" (both on the main page and under Controls). OURS - the original has the pad only.
#include <string>
#include <vector>

namespace rrgame {

// One weapon's grip in the tracked hand, as a CHANGE from its default (vr_weapon_calib.h: the default is derived from
// the weapon model's own geometry - the handle's end in the palm, the long axis along the aim).
struct WeaponGrip {
    int offMm[3] = {0, 0, 0}; // the palm point moved along the hand's right / up / forward, millimetres
    int rotDeg[3] = {0, 0, 0}; // pitch (+ tip up), yaw (+ tip left), roll (+ about the aim, counter-clockwise), degrees
    bool IsDefault() const {
        return offMm[0] == 0 && offMm[1] == 0 && offMm[2] == 0 && rotDeg[0] == 0 && rotDeg[1] == 0 && rotDeg[2] == 0;
    }
};

struct WeaponsSettings {
    // ---- the holsters (vr_holsters.h)
    bool holsters = true;     // [vr] holsters: the weapons live on the hips, drawn with a hand
    int left = -1, right = -1; // [vr] holster_left / holster_right: the weapon id on each hip, -1 empty (auto-filled)
    bool gripLock = false;    // [vr] holster_grip_lock: releasing the grip away from a holster keeps the weapon in hand
    int heightCm = 58;        // [vr] holster_height_cm: below the eye
    int spreadCm = 26;        // [vr] holster_spread_cm: out to each side
    int forwardCm = 5;        // [vr] holster_forward_cm: in front of the eye (negative: behind)
    int reachCm = 18;         // [vr] holster_reach_cm: a hand this near a holster draws / puts back
    bool markers = true;      // [vr] holster_markers: the rings round the holsters (green within reach)
    // [vr] holster_hide_empty: an owned weapon 0..7 with no swings left in its nibble of +0x30 is not offered (a quick
    // race's record carries the chain's bit 0 (CHAN) with no swings - the original never lets the player draw it)
    bool hideEmpty = true;
    // ---- the swing (vr_melee.cpp Detect; Combat options)
    int weaponSpeedDm = 30;   // [vr] melee_weapon_speed: m/s x 10 of the weapon's touching point against the rider
    int weaponTravelCm = 25;  // [vr] melee_weapon_travel_cm: the swing's travel (the weapon's far end, seat space)
    int fistTravelCm = 10;    // [vr] melee_fist_travel_cm: the punch's travel (the knuckles)
    int intoPct = 50;         // [vr] melee_into_pct: the part of the contact speed going into the body
    int cooldownMs = 500;     // [vr] melee_cooldown_ms: one hand's blows at least this far apart
    // ([vr] bars_buttons_attack, an old Handlebars-mode button row, is obsolete: the combat mode alone says whether the
    // buttons attack, vr_melee_settings.h ButtonsAttack; an old file's key is read and dropped)
    bool prodButton = true;   // [vr] prod_button: B discharges the prod / stun gun held in a tracked hand
    // [vr] prod_reach: B with the prod / stun gun touching no rider presses the ORIGINAL's attack for them
    // (combat action 6, L1 + d-pad Up: CombatDecode's command 148 - the fight code's own target and ReachTest reach);
    // off: only a touching discharge lands, B elsewhere discharges into the air
    bool prodReach = true;
    // ---- the grips (vr_weapon_calib.h): [vr] weapon_grip_<w> = offR offU offF pitch yaw roll - as the RIGHT hand holds
    // the weapon; the left hand holds its mirror image
    WeaponGrip grip[9];
    // [vr] weapon_grip_left_trim = offR offU offF pitch yaw roll: the left hand's own fine adjustment over
    // the mirrored grip of every weapon (right = the world's right as the left hand sees it; rotations about the palm)
    WeaponGrip leftTrim;

    float WeaponSpeed() const { return static_cast<float>(weaponSpeedDm) / 10.0f; }
    bool Parse(const std::string& key, const std::string& value);
    std::string Serialize() const;
    std::string Describe() const;
};

// The menu pages' rows. Values change with left / right (-1 / +1), A (0) selects.
struct WeaponMenuRow {
    std::string label, value;
    bool submenu = false;
};
// "Weapons and holsters" (the main VR page and Controls); its FIRST row opens the calibration page
// (WeaponsMenuActivate returns 2 for it).
constexpr int kWeaponsCalibRow = 0;
std::vector<WeaponMenuRow> WeaponsMenuRows(const WeaponsSettings& s);
int WeaponsMenuActivate(WeaponsSettings& s, int row, int direction, std::string& note); // 0 nothing, 1 changed, 2 open calib
// Combat options' extra rows (after vr_melee_settings.h's).
std::vector<WeaponMenuRow> SwingMenuRows(const WeaponsSettings& s);
bool SwingMenuActivate(WeaponsSettings& s, int row, int direction);

// The flags (vr_settings.h ApplyVrFlag forwards them): --vr-holsters 0|1, --vr-holster-left W|none,
// --vr-holster-right W|none, --vr-holster-grip-lock 0|1, --vr-holster-height CM, --vr-holster-spread CM,
// --vr-holster-forward CM, --vr-holster-reach CM, --vr-weapon-speed M, --vr-weapon-travel CM, --vr-fist-travel CM,
// --vr-melee-into P, --vr-melee-cooldown MS, --vr-prod-button 0|1, --vr-prod-reach 0|1,
// --vr-weapon-grip W:offR,offU,offF,pitch,yaw,roll, --vr-weapon-grip-left-trim offR,offU,offF,pitch,yaw,roll,
// and --vr-holster-script <file | "frame command; ..."> (the desktop
// mock's holster actions, vr_holsters.h). False when argv[i] is not one; a bad value throws.
bool ApplyWeaponsFlag(int argc, char** argv, int& i, WeaponsSettings& s);
const std::string& HolsterScriptArgument();

} // namespace rrgame
