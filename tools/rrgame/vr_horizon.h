#pragma once
// The head view's horizon and the riders near it - OURS, no original code.
//
//   * ViewPitch - "the whole picture shakes up and down on a slope". MEASURED CAUSE (1/23's downhill, --autosteer, the
//     mock at 72 Hz): the game's own pitch move of the bike (+0x268, 16.16 radians: the wheelie / stoppie -
//     a second-order move started when the longitudinal load s5 crosses +-1.0) fires again and again on a steep
//     grade: a -9 deg nose-down nod of ~0.3 s every ~1.05 s. The rows +0x1B0 are RotMatrix(-pitch, 0, -roll) x the ground
//     frame +0x204, the head view's eye rides the drawn bike (vr_handlebars.h SeatEye) and the horizon lock levels
//     the roll only - so, unfiltered, every nod turns the whole world 9 deg. Seen from the
//     original's chase camera it is the bike nodding under the rider.
//     The fix, draw-only: in the head view with smooth motion (vr_comfort.h FrameInterp, before its stabiliser) the
//     player's bike is drawn with its pitch split in two - the ROAD's grade (the ground frame's forward +0x210) through a
//     critically damped low-pass (kGradeHz), and the bike's OWN pitch (the rows' pitch over that grade: the pitch move
//     +0x268 on the road) through a slower one (kOwnHz) at a share of its amplitude - turned about the wheel that stays on the road (the rear one in a wheelie, the front one
//     in a stoppie; the model's own wheels, BarGrips::WheelContacts). The stabiliser, the camera, the eye on the bike,
//     the grips and the hands on them, the rider and the weapon all take that drawn bike, so the view pitches with the
//     road slowly and the hands stay on the grips. [vr] view_pitch (--vr-view-pitch road | low | original, the VR menu
//     -> Graphics -> "Bike pitch (first person)"):
//       road      the grade only (no nod at all);
//       low       the grade + kLowShare of the bike's own pitch move (the default);
//       original  the game's pitch as it is (the control).
//     With the player's held wheelie (src\game\wheelie.h) the ORDER is: ViewPitch takes the part of the
//     game's own pitch the held wheelie covers out of the rows whole (never filtered), then the wheelie layer
//     (vr_wheelie.cpp AddWheeliePitch) draws its whole held pitch - that part included (RemovedWheelie) - on the bike and
//     all that rides it, and the view keeps wheelie_view_pitch of it; ViewPitch filters only the rest.
//   * NearMeter - "rivals jerk back and forth close by": per displayed head-view frame, every live rival bike as drawn
//     (the arena after FrameInterp::Apply) in the eye's frame; the second difference over the display times of its
//     position (mm, scaled to the nominal period), by distance band, the along-view part apart, and the same in the
//     world (the rival's own motion).
//   * The seat forward / back (VrSettings::seatBackCm): the eye on the bike moved back along the drawn bike's forward
//     (main.cpp at SeatEye); the rider is not drawn in the head view (the Handlebars mode hides all of him, the Stick
//     mode keeps only the forearms and gloves), so the eye moved back meets nothing of him.
//
// DEVELOPMENT:
//   RRJB_VIEW_PITCH_LOG=<csv>  one row a head-view frame: the grade, the own pitch (+0x268 and from the rows), the game's
//                              pitch, the drawn target, the view's pitch and the eye's height (the meter below)
//   RRJB_NEAR_LOG=<csv>        one row per drawn rival and frame (NearMeter)
//   RRJB_VIEW_PITCH_TRACE=<csv> per step: the player's +0xB8, +0x1F8, +0x31C, +0x268, the grade and two rows (the
//                              stoppie's hitch along the road was found with it)
//   RRJB_RIVAL_SMOOTH=off      the rivals only interpolated (the control); RRJB_SMOOTH_ALONG (vr_comfort.h)
#include "platform/xr/xr_math.h"
#include "render/mat4.h"
#include "vr_pacing.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace rrgame {

class FrameInterp;

enum ViewPitchMode { kPitchRoad = 0, kPitchLow = 1, kPitchOriginal = 2 };
const char* ViewPitchName(int mode);                  // "Road only" / "Low" / "Original"
bool ParseViewPitch(const std::string& v, int& mode); // road | low | original

// The model's wheels where they touch the road, in the bike's frame (world units: up along the bike's up from the
// origin +0xB8, fwd along its forward): BarGrips::WheelContacts.
struct WheelContacts {
    bool valid = false;
    float frontUp = 0, frontFwd = 0, rearUp = 0, rearFwd = 0;
};

class ViewPitch {
public:
    static constexpr double kGradeHz = 1.5;   // the road's grade low-pass (critically damped, Hz)
    static constexpr double kOwnHz = 1.0;     // the bike's own pitch move low-pass
    static constexpr double kLowShare = 0.25; // low: the drawn share of the filtered own pitch
    static constexpr double kGradeLeashDeg = 3.0; // the filtered grade never further than this from the road's
    // After each step: the player's bike's ground forward and own pitch (the previous capture becomes the step before).
    void Capture(const uint8_t* ram, uint32_t bike);
    void Invalidate() {
        have_ = 0;
        state_ = State{};
    }
    // One displayed head-view frame, after FrameInterp::Apply and before FrameInterp::Stabilize: the drawn bike's rows
    // and origin turned to the planned pitch, written through `interp` (put back with its Restore). `alpha`: the drawn
    // time between the last two steps; `dt`: the display time since the frame before (s). False: nothing written.
    bool Apply(uint8_t* ram, uint32_t bike, int mode, float alpha, double dt, const WheelContacts& wheels, FrameInterp& interp);
    void Break() { state_.have = false; } // a frame that is not the head view: the filters start again
    // the game's own pitch this frame's Apply took out of `bike`'s drawn rows because the player's held
    // wheelie covers it (radians, 0: none) - the wheelie layer (vr_wheelie.cpp AddWheeliePitch) draws it back as part of
    // its whole held pitch. Valid from Apply until EndFrame (main.cpp: the end of the VR frame).
    float RemovedWheelie(uint32_t bike) const { return removedOn_ && bike == removedBike_ ? static_cast<float>(removed_) : 0.0f; }
    void EndFrame() { removedOn_ = false; }
    // The meter: the view as drawn (the anchor of the frame: forward / up / origin), and the drawn bike's contact height.
    // `drawnBike`: the player's bike as the renderer drew it (race_render.h DrawnMachine; for the wheelie meter)
    void Meter(long frame, const rr::xr::WorldAnchor& anchor, double dt, const rr::render::Mat4* drawnBike = nullptr);
    void MeterBreak() { meter_.have = false; }
    std::string Totals() const;

private:
    struct Cap {
        bool valid = false;
        uint32_t bike = 0;
        double grade = 0; // the ground forward's pitch (radians, nose up +)
        double own = 0;   // +0x268 (radians)
        int32_t origin[3] = {};
    };
    Cap prev_, cur_;
    int have_ = 0;
    struct State {
        bool have = false;
        uint32_t bike = 0;
        double g = 0, gv = 0, o = 0, ov = 0; // the grade and the own pitch through their low-passes, with their rates
    } state_;
    // this frame's plan (the meter's CSV)
    struct Row {
        long frame = 0;
        double grade = 0, own268 = 0, ownRows = 0, game = 0, target = 0;
        double gFront = 0, gRear = 0, dFront = 0, dRear = 0; // the wheels' contacts' world height (up +, world units):
                                                             // the game's pose and the drawn one
        bool planned = false;
    } row_;
    bool removedOn_ = false; // RemovedWheelie
    float heldNow_ = 0.0f;   // the held wheelie this frame (radians; the meter)
    size_t heldFrames_ = 0;  // the meter: frames with the held wheelie at 30 deg or more - the drawn bike's and the
    double heldDrawnMin_ = 1e9, heldDrawnMax_ = -1e9, heldViewMin_ = 1e9, heldViewMax_ = -1e9; // view's pitch over the grade
    uint32_t removedBike_ = 0;
    double removed_ = 0.0;
    size_t coveredFrames_ = 0;
    double coveredMaxDeg_ = 0.0;
    // the run's counters
    size_t frames_ = 0, planned_ = 0, skipped_ = 0;
    double ownMaxDeg_ = 0, ownDiffMaxDeg_ = 0, deltaMaxDeg_ = 0, pivotShiftMax_ = 0;
    size_t ownMoves_ = 0; // own pitch moves seen (|own| > 2 deg, counted on the rise)
    bool ownOn_ = false;
    // the meter: high-passed (x - its 0.5 Hz critically damped low-pass) view pitch (deg), eye height over the drawn
    // contact (mm) and the game's own bike pitch (deg) - what the unfiltered view would do
    struct HighPass {
        bool have = false;
        double x = 0, v = 0;
        double sq = 0, max = 0;
        size_t n = 0;
        double Feed(double value, double dt, bool warm);
    };
    struct Meter_ {
        bool have = false;
        size_t frames = 0, warm = 0;
        HighPass view, eye, game;
        double lastView = 0.0, lastGame = 0.0; // the per-frame change of the view's / the game's pitch (deg)
        bool haveLast = false;
    } meter_;
    // over the whole run (a meter break does not reset them): the view's / the game's pitch change a frame
    double stepViewMax_ = 0.0, stepGameMax_ = 0.0, stepViewSq_ = 0.0, stepGameSq_ = 0.0;
    size_t stepN_ = 0;
    // the eye's fore-aft second difference (mm per frame, along the view): the lurches of the bike along the road
    double eyeHist_[3][3] = {};
    int eyeN_ = 0;
    std::vector<float> foreAft_;
    std::FILE* csv_ = nullptr;
    bool csvTried_ = false;
    size_t csvRows_ = 0;
    double lastGamePitch_ = 0, lastContactY_ = 0;
    bool lastHave_ = false;
};
ViewPitch& ProductViewPitch();

// ---------------------------------------------------------------- the rival bikes drawn as smoothly as the player's
// The head view's own bike is drawn through FrameInterp::Stabilize's g-h filter, the rivals only interpolated:
// their own step-to-step unevenness (the AI's speed changing in steps every ~50 ms, the road's height snaps, the box
// centre's jump when a pitch move starts or ends) would stay in the picture against a smooth eye - invisible far away,
// a fore-aft jerk of a few mm..cm right next to the rider. Each live rival bike within kRange of the eye goes through the
// same filter (per displayed frame, position only - the rows as interpolated), on a kLeash leash from its drawn state;
// the rider on it follows (drawn from the bike's matrix), the shadows are moved with it. Smooth motion's frames only
// (DEVELOPMENT RRJB_RIVAL_SMOOTH=off: the control).
class RivalSmooth {
public:
    static constexpr double kG = 0.30;     // FrameInterp::kPosG
    static constexpr double kLeash = 0.06; // FrameInterp::kMaxOff (world units)
    // along the rival's forward (the player's is 0.20: a rival's drawn body stays nearer to the step the VR combat's
    // contacts are tested against - vr_melee.h UpdateTargets)
    static constexpr double kLeashAlong = 0.12;
    static constexpr double kRange = 60.0; // world units from the eye
    // After FrameInterp::Apply (and Stabilize): `bikes` the live rival bikes, `riders` theirs (0: none), `dt` the display
    // time since the frame before (s), `frame` the loop's frame. Returns the bikes written.
    size_t Apply(uint8_t* ram, const std::vector<uint32_t>& bikes, const std::vector<uint32_t>& riders, const float eye[3],
                 double dt, long frame, FrameInterp& interp);
    void Reset() { tracks_.clear(); }
    std::string Totals() const;

private:
    struct Track {
        uint32_t bike = 0;
        int n = 0;           // 0 new, 1 one sample (the velocity from the next), 2 filtering
        double x[3] = {}, v[3] = {};
        long last = -2;
    };
    std::vector<Track> tracks_;
    size_t frames_ = 0, written_ = 0, resets_ = 0, leashed_ = 0;
    double offSq_ = 0.0, offMax_ = 0.0;
    size_t offN_ = 0;
};
RivalSmooth& ProductRivalSmooth();
bool RivalSmoothOff(); // RRJB_RIVAL_SMOOTH=off

// ---------------------------------------------------------------- the rivals near the eye
class NearMeter {
public:
    // One displayed head-view frame: `bikes` the live rival bikes' entities (the player's excluded), the arena as drawn,
    // the frame's anchor, its display interval and the nominal period (s).
    void Frame(long frame, const uint8_t* ram, const std::vector<uint32_t>& bikes, const rr::xr::WorldAnchor& anchor,
               double dt, double period);
    void Break() { tracks_.clear(); }
    std::string Totals() const;

private:
    struct Track {
        uint32_t bike = 0;
        int n = 0;
        double eye[3][3] = {}, world[3][3] = {}, t[3] = {};
        long lastFrame = -2;
    };
    std::vector<Track> tracks_;
    double t_ = 0.0;
    long lastFrame_ = -2;
    static constexpr int kBands = 3; // < 5 m, 5..15 m, 15..40 m
    struct Band {
        size_t n = 0;
        double sq = 0, max = 0, alongSq = 0, worldSq = 0;
        std::vector<float> eye, along, world; // the samples (their median and 90th percentile: the RMS is the events')
    } band_[kBands];
    std::FILE* csv_ = nullptr;
    bool csvTried_ = false;
    size_t csvRows_ = 0;
};
NearMeter& ProductNearMeter();

} // namespace rrgame
