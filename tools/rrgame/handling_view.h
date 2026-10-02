#pragma once
// What the rider SEES of the handling: the head view's drawn lean of the player's bike and the view's own
// roll, per drawn frame, for the log and the gates - Original against Modern in the same bend. OURS, measurement only:
// nothing here changes a picture.
//   * the drawn lean: the roll of the bike as the renderer draws it (the game's roll through a VisualLean: the VR head
//     view's Visual bike lean %, or Modern's own lean - handling_modern.h), degrees, signed as vr_visual_lean.h's roll;
//   * the view's roll: the head view's up against the level plane through its forward (the PlayStation's Y-down world),
//     signed about the forward - 0 while a horizon lock holds the view level.
// DEVELOPMENT: RRJB_HANDLING_VIEW_LOG=<csv> - one row per noted frame (frame, vr, game roll, drawn lean, view roll).
#include <string>

#include "vr_visual_lean.h"

namespace rrgame {

// The bike's drawn roll (degrees) under `lean` (off: the game's own roll `lean.rollDegrees`).
float DrawnLeanDegrees(const VisualLean& lean);
// A view frame's roll (degrees): `up` against the level plane through `fwd` (Y-down world axes), signed about `fwd`.
float ViewRollDegrees(const float fwd[3], const float up[3]);

class HandlingViewMeter {
public:
    // One drawn frame of the head view (`vr`: the VR head view, else the desktop head camera).
    void Note(long frame, bool vr, const VisualLean& lean, const float viewFwd[3], const float viewUp[3]);
    long Frames() const { return frames_; }
    // "handling view: ..." - empty when no frame was noted
    std::string Summary() const;

private:
    long frames_ = 0;
    bool vr_ = false;
    double maxGame_ = 0.0, maxDrawn_ = 0.0, maxView_ = 0.0;
    long ratioFrames_ = 0;
    double sumRatio_ = 0.0;
    long lastFrame_ = -1;
    void* csv_ = nullptr; // std::FILE*
};

HandlingViewMeter& HandlingView(); // player 0's

} // namespace rrgame
