#pragma once
// The VR steering settings (vr_handlebars.h): how the rider is steered in the headset - the left stick as
// before, or the handlebars held with the hands (the grips at the rider's own glove positions, twist throttle, motion
// punches). A member of VrSettings (vr_settings.h), kept in its [vr] section and edited on the VR menu's Controls page
// (vr_menu.cpp: the "Steering" row and its "Handlebar options" page).
#include <string>
#include <vector>

namespace rrgame {

struct BarsSettings {
    enum Steering { kStick = 0, kHandlebars = 1 };
    int steering = kStick;     // [vr] steering = stick | handlebars (default: the stick)
    int sensitivity = 100;     // % of the bar rotation that is full lock: 100 = 30 degrees, 200 = 15, 50 = 60
    int deadzone = 3;          // % of full lock ignored around the centre (on top of the original's own axis dead zone)
    bool twistThrottle = true; // the right grip twisted toward you opens the throttle (the right trigger always works)
    bool motionPunches = true; // a free hand swung fast punches (right R1 / left L1), downward kicks (R2)
    bool oneHand = true;       // one hand on the bars steers (off: both hands needed to steer)
    int heightCm = 0;          // the grips moved up / down from the rider's gloves, -30..30 cm (seated players)
    // After GTA SA VR's bikeHandsFollowTilt: a held hand takes the grip's whole live frame - the bike's heading,
    // lean and pitch and the fork's own turn - as the bike is drawn; off: it keeps the controller's wrist rotation
    // (still seated on the grip).
    bool handsFollowTilt = true;
    // After GTA SA VR's BikeVisualLeanPercent: the head view draws the player's own bike - and with it the
    // grips, the held hands and the eye - with this percentage of the original's roll, 0..100 (the simulation keeps
    // its own lean); with the horizon lock the drawn bars then stay near the player's real hands.
    int visualLean = 50;
    // the VR eye rides the drawn bike at the point the rider's head had when he sat down (latched on the
    // first seated head-view frame) instead of following the rider's animated head (the tuck, the lean into a turn):
    // the player's body - and so the real hands - then stays put on the bike. Off: the animated head.
    bool eyeOnBike = true;

    // One [vr] key (steering, bars_sensitivity, bars_deadzone, twist_throttle, motion_punches, one_hand_steering,
    // bars_height_cm, bars_hands_follow_tilt, bike_visual_lean, eye_on_bike); false when the key is not one of these or the value is bad.
    bool Parse(const std::string& key, const std::string& value);
    std::string Serialize() const; // the lines for the [vr] section
    std::string Describe() const;  // a part of the settings log line
    float FullLockDegrees() const { return 30.0f * 100.0f / static_cast<float>(sensitivity > 0 ? sensitivity : 100); }
};

// The VR menu's "Handlebar options" page (vr_menu.cpp; its Back row is the menu's): the rows, and a row changed by
// left / right (direction -1 / +1) or A (0); true when a setting changed (the menu saves the settings).
struct BarsMenuRow {
    std::string label, value;
};
std::vector<BarsMenuRow> BarsMenuRows(const BarsSettings& s);
bool BarsMenuActivate(BarsSettings& s, int row, int direction);

// The steering flags of the command line (vr_settings.h ApplyVrFlag forwards them): --vr-steering stick|handlebars,
// --vr-bars-sensitivity P, --vr-bars-deadzone P, --vr-twist-throttle 0|1, --vr-motion-punches 0|1, --vr-one-hand 0|1,
// --vr-bars-height CM, --vr-bars-follow-tilt 0|1, --vr-bike-lean P, --vr-eye-on-bike 0|1, and --vr-bars-script <file | "frame command; ..."> (the desktop VR
// mock's scripted controllers, vr_handlebars.h). False when argv[i] is not one; a bad value throws.
bool ApplyBarsFlag(int argc, char** argv, int& i, BarsSettings& s);
// The script --vr-bars-script named (empty: none).
const std::string& BarsScriptArgument();

} // namespace rrgame
