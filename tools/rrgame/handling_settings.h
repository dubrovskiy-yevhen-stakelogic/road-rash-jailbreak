#pragma once
// The handling setting of rrgame (src\game\handling_modern.h): Original / Modern (SA-style) and
// Modern's tuning - the [handling] section of rrgame_settings.ini, the command line, and the page both menus show (the
// F10 overlay's Controls page and the VR menu's Controls -> Handling). The pattern of cheat_menu.h: a scripted run (a
// frame count, a check, a hidden run) starts from Original on the desktop AND in VR and never reads the file, so the
// gates see today's races; the flags set values on top.
#include <string>
#include <vector>

#include "game/handling_modern.h"
#include "vr_visual_lean.h"

namespace rrgame {

bool LoadHandling(const std::string& path, rr::game::HandlingSettings& s);
bool SaveHandling(const std::string& path, const rr::game::HandlingSettings& s);
// A handling flag at argv[i] (advances i past its value); false when argv[i] is not one; a bad value throws
// std::runtime_error. --handling original|modern (this run, desktop and VR), --handling-desktop M, --handling-vr M,
// --handling-steer-lag P, --handling-curve P, --handling-turn-lag P, --handling-lean-lag P, --handling-max-lean DEG;
// the test inputs --steer-script "F v; ..." (handling_modern.h SteerScript) and --handling-log <csv>.
bool ApplyHandlingFlag(int argc, char** argv, int& i);
// The settings a run starts from (see the top). Returns rr::game::Handling(); prints its line.
rr::game::HandlingSettings& StartHandling(bool scripted);
// the front end's start - an interactive one reads the [handling] section now (once a process), so the
// menus over the front end show and save the file's values; a scripted one reads nothing and the menus save nothing.
// (--handling-lean-model sa|game and --handling-camera-roll P join the flags above.)
void StartHandlingFrontEnd(bool scripted);

// ---- the picture: the player bike's drawn lean under Modern (handling_modern.h VisualRoll), `scale` of it (the VR head
// view's Visual bike lean), turned about the wheels' contact line `contactUp` world units along the bike's up from its
// box centre (<= 0; HandlingContactUp estimates it from the box when the VR grips' exact one is not at hand). Off (no
// rotation) under Original or while the rider is off the bike.
VisualLean HandlingLean(const uint8_t* ram, uint32_t bike, float scale, float contactUp);
float HandlingContactUp(const uint8_t* ram, uint32_t bike);
// the bike's own roll, no rotation (VisualLean::rollDegrees; `on` false) - the drawn lean of Original
VisualLean HandlingGameRoll(const uint8_t* ram, uint32_t bike);

// ---- the page (both menus): rows, and Enter (0) / Left (-1) / Right (+1) on a row; `vrMenu` picks which mode the
// first row edits. Saves the [handling] section itself; `note` gets what to show.
enum class HandlingRowId {
    kMode, kSteerLag, kCurve, kTurnLag, kLeanLag, kMaxLean, kLeanModel, kCameraRoll,
    kWheelies, kWheelieLift, kWheelieFull, kWheelieHold, kWheelieAngle, kWheelieCars, kWheelieLoop, kWheelieViewPitch
};
struct HandlingRow {
    std::string label, value;
    HandlingRowId id = HandlingRowId::kMode;
};
// The F10 overlay's page (vrMenu false: every row) or the VR menu's Handling page (the handling itself; the VR view
// roll and the wheelie rows are on the VR "Riding position" page - HandlingRidingRows).
std::vector<HandlingRow> HandlingMenuRows(bool vrMenu);
bool HandlingMenuActivate(int row, int direction, bool vrMenu, std::string& note);
// The [handling] rows of the VR "Riding position" page: the view roll with the lean, the wheelies, the hand lift to
// start, the full-lift height, the hold to start, the angle, the view pitch in a wheelie, over cars, the loop-over.
std::vector<HandlingRow> HandlingRidingRows();
HandlingRow HandlingRowOf(HandlingRowId id, bool vrMenu);
bool HandlingRowActivate(HandlingRowId id, int direction, bool vrMenu, std::string& note);

} // namespace rrgame
