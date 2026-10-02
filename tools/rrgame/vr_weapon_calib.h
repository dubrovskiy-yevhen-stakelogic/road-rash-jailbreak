#pragma once
// The weapon's grip in the tracked hand - OURS, after the GTA San Andreas VR / Vice City VR weapon calibration
// (the gta-sa-vr-quest project, native\src\Calib.*: one profile per weapon, offsets and rotations edited
// live from the VR menu and saved).
//
//   * The DEFAULT grip comes from each weapon model's own geometry (BBLEVEL1.GEO model 800, group = the weapon, rest
//     pose - the soup weapon_draw.h draws): every weapon runs from its model origin (the original's attach point, the
//     handle's end) out to its farthest vertex; that direction is the long axis. The palm point is on the handle - 7 cm
//     (at most 30 % of the weapon) up the axis from the handle's end, at the centroid of the handle's cross-section
//     (the prod's grip box sits off its shaft) - and it goes into the centre of the closed fist (vr_hands_draw.h
//     kFistCentre, 1.5 cm behind the grip pose along the aim); the long axis points along the aim pose (where the
//     controller points), the model's up (-y, the game's y is down) along the hand's up.
//   * The CALIBRATION (vr_weapon_settings.h WeaponGrip, per weapon, saved in [vr]) moves the palm point in the hand's
//     frame (right / up / forward, mm) and turns the weapon about it (pitch / yaw / roll, degrees; R = Ry Rx Rz).
//   * The calibration page (VR menu -> Weapons and holsters -> Calibrate the grip) shows the chosen weapon
//     in the chosen hand, the rows change the six values, and squeezing the OTHER hand's grip takes hold of the weapon:
//     while held it follows the other hand rigidly (relative to the holding hand), released it stays - saved. With the
//     left hand chosen the rows show and edit the same (right-hand) calibration as the left hand sees it (mirrored), and
//     the "Left hand trim" rows adjust the left hand alone (below).
//   * The collider of the weapon in hand (vr_melee.cpp) is the drawn weapon's own axis, from the handle's end to the far
//     end (WeaponInHandSegment), so what is seen is what hits.
#include "render/mat4.h"
#include "rrvfs/disc_image.h"
#include "vr_weapon_settings.h"

#include <string>
#include <vector>

namespace rrgame {

// A tracked hand in the world: the grip pose's position and axes (+X right, +Y up, -Z forward as world directions)
// and the aim pose's forward. `left`: the left controller (the grip is mirrored for it).
struct HandFrameW {
    bool valid = false;
    bool left = false;
    float pos[3] = {}, right[3] = {1, 0, 0}, up[3] = {0, 1, 0}, fwd[3] = {0, 0, 1}, aim[3] = {0, 0, 1};
};

// OpenXR gives BOTH controllers' grip poses the same handedness (+X to the player's right, +Y up, -Z
// forward), so one calibration applied in each hand's own axes is NOT a mirror image: the left hand would take the
// right hand's "move right" as its own right (the palm point 2 x offR away from the mirror place - with typical
// calibrations of offR -10..-35 mm, 2..7 cm off in the left hand), its yaw and roll unmirrored, and the weapon's
// palm point on the handle unmirrored across the weapon's own axis (the prod's grip box sits off the shaft). So the
// grip ([vr] weapon_grip_<w>) is the RIGHT hand's, and the left hand holds its mirror image across the hand's sagittal
// plane: offR -> -offR, yaw -> -yaw, roll -> -roll (M R M with M = diag(-1, 1, 1) of R = Ry Rx Rz), the model's palm
// point reflected across the weapon's axis-up plane; then the left hand's own trim ([vr] weapon_grip_left_trim).
// RRJB_GRIP_MIRROR=off: the control (the calibration applied in each hand's own axes, no mirror, no trim).
bool GripMirrorOn();
WeaponGrip MirrorGrip(const WeaponGrip& g); // the same grip seen in the other hand (an involution)
// The mock's check (vr_holsters.cpp script `gripcheck W`): weapon W in both hands - the weapon's axis at the palm
// station against the drawn fist's centre (vr_hands_draw.h) in each hand's frame, the axis against the aim ray, their
// mirror difference, and, when the two hands are posed as mirror images across the plane (origin, normal) - the mock's
// rest -, the world mirror residual of both weapons' ends. One line.
std::string GripMirrorCheck(int weapon, const HandFrameW hands[2], float upm, const float planeOrigin[3],
                            const float planeNormal[3]);

struct WeaponShape {
    bool valid = false;
    float axis[3] = {-1, 0, 0}, up[3] = {0, -1, 0}, side[3] = {0, 0, 1}; // model: the long axis, its up, a x up
    float tMin = 0.0f, tMax = 0.0f; // the extent along the axis, model units
    float grip[3] = {};             // the default palm point, model units
    float gripT = 0.0f;             // its position along the axis
};

// The weapons' shapes (loaded once from the disc; the renderer draws the same soup).
bool LoadWeaponShapes(const rr::DiscImage& disc);
const WeaponShape& WeaponShapeOf(int weapon); // an invalid one for a bad id / nothing loaded
std::string DescribeWeaponShapes();

// The weapon's model matrix (model units -> world) in `hand` with the calibration `g`; `upm` world units per metre.
bool WeaponInHandMatrix(int weapon, const HandFrameW& hand, float upm, const WeaponGrip& g, rr::render::Mat4& out);
// The drawn weapon's axis in the world: the handle's end to the far end (x `lengthPct` %).
bool WeaponInHandSegment(int weapon, const HandFrameW& hand, float upm, const WeaponGrip& g, float lengthPct, float root[3],
                         float tip[3]);
// The same grip as weapon_draw.h's origin / along / side (the model origin, its long axis, the axis weapon_draw.cpp
// HandModel turns onto `side`) - for what places a weapon that way (vr_nunchaku.h's chain).
bool WeaponInHandFrame(int weapon, const HandFrameW& hand, float upm, const WeaponGrip& g, float origin[3], float along[3],
                       float side[3]);
// The palm point of the weapon in hand (world) - where the weapon is held.
bool WeaponPalmPoint(const HandFrameW& hand, float upm, const WeaponGrip& g, float out[3]);
// A weapon on a hip: its palm point at `at` (world), hanging down along the seat's `down` tilted `back` and `out`.
bool WeaponHolsteredMatrix(int weapon, const float at[3], const float down[3], const float back[3], const float out[3],
                           rr::render::Mat4& result);

// ---- the calibration page (vr_menu.cpp)
void SetCalibPageOpen(bool open, int weaponInHand, int holdingHand); // the menu opens / leaves the page
bool CalibPageOpen();
int CalibWeapon();
int CalibHand();
// Once a frame while the page is open: the other hand's grip takes hold of the weapon and moves it. True when a hold
// was released this frame (the change is to be saved).
bool CalibOtherHand(const HandFrameW hands[2], const float grip[2], float upm, WeaponsSettings& s);
bool CalibHolding();
std::vector<WeaponMenuRow> CalibMenuRows(const WeaponsSettings& s);
bool CalibMenuActivate(WeaponsSettings& s, int row, int direction, std::string& note);

} // namespace rrgame
