#pragma once
// VR comfort - OURS, no original code: smooth motion, the view off the bike and the menus' input and rumble.
//
//   * Smooth motion (FrameInterp): the race steps in whole 1/300 s ticks (RaceStep's unit), and a headset's 72 / 90 /
//     120 Hz is not a multiple of it - 300 / 72 = 4.17 ticks a frame, so the host (main.cpp, the tick carry) steps
//     4, 4, 4, 4, 4, 5, ... ticks: every sixth frame the bike and the world advance 25 % further than the others (90 Hz:
//     3, 3, 4 - 33 %; 120 Hz: 2, 3 - 50 %). Shown one step a frame, that is a 12..60 Hz judder of the whole world
//     around the rider. The simulation is left exactly as it is; what is DRAWN is the state at one display period's
//     uniform time between the last two steps (render time = the step's time + the carry - one tick): the fields the
//     renderer places objects from are written into the arena for the frame's drawing and put back byte for byte
//     before the next step (Apply / Restore). The fields: pools 0 (bikes), 2 (pedestrians), 3 (traffic), 4 (props):
//     the origin +0xB8 (3 x 16.16) and the rows +0x1B0 (9 x Q12); pool 1 (riders): +0xB8 and the pose root +0x1C
//     (3 x s16, model units - the rider off the bike is drawn at the bike + root, rider_pose_draw.cpp). A slot whose
//     handle +0xAC changed, or that moved more than kCut world units in a step, is drawn as it is (a respawn, a
//     teleport). The ported shadows' world quads are moved with their object (Shift). Headsets only by default; a
//     scripted mock takes it with --vr-mock-hz (a synthetic display clock, below). [vr] smooth_motion (default on).
//   * The view off the bike (FallView): with the original's camera the head view's rider thrown off cuts to the chase
//     camera (its crash director swings, tilts and cuts), and back to the head on the re-seat with the eye re-latched
//     on a rider still climbing on. Instead: a short black fade at both ends, and while off a fixed third-person view -
//     level (no roll, no pitch), its heading frozen at the fall, its eye following the rider's box from behind and
//     above with a 0.35 s lag. [vr] fall_view = fixed (default) | chase (the original's camera, still with the fade).
//   * The menus hold the rumble: VrHostImpl (game_host_vr.cpp) sends the Touch actuators nothing while the VR menu is
//     open, the pause menu is up, the race is not racing, or the session is not focused.
//   * The VR menu changes values with the triggers only (game_host_vr.cpp MenuHolds): with the stick's left / right
//     a drifting stick would change them by itself.
//
// DEVELOPMENT / verification:
//   --vr-mock-hz H          the scripted desktop mock steps on a synthetic H Hz display clock (the tick carry of the
//                           headset: 4 / 5 ticks at 72 Hz) instead of the fixed 5 ticks - the judder measurement
//   --vr-mock-pad "<script>" the mock's Touch controllers, "turn command [turns]; ..." (turn = the race loop's turn):
//                           chord, up, down, a, b, menu, ltrigger, rtrigger, stickleft, stickright, drift <x>,
//                           stick <x> <y> (the left stick held there, -1..1, y up)
//   --vr-mock-pad-race       the script's controllers drive the race's pad too (not only the VR menu)
//   RRJB_JUDDER_LOG=<csv>   the motion meter per displayed frame (MotionMeter); also --vr-judder-log <csv> with the
//                           display clock's columns (vr_pacing.h - it works on the Quest)
//   RRJB_VR_MENU_STICK=values  the stick's left / right change values again (the control of the menu rule)
//   RRJB_VR_HAPTICS_HOLD=off   the menus do not silence the Touch haptics (the control of the pause rule)
//   RRJB_FALL_VIEW=legacy      the original's fall: its camera, no fade, the eye re-latched on the re-seat
#include "platform/xr/xr_math.h"
#include "platform/xr/xr_pad.h"
#include "render/gl_api.h"
#include "render/mat4.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace rrgame {

// ---------------------------------------------------------------- the command line (vr_settings.cpp ApplyVrFlag)
bool ApplyComfortFlag(int argc, char** argv, int& i);
double MockDisplayHz(); // --vr-mock-hz (0: none)
bool MockPadScripted();
// --vr-mock-pad-race (a test input): the scripted Touch controllers also reach the race's pad through the VR bindings,
// as a headset's do (a scripted run otherwise polls no controller).
bool MockPadDrivesRace();
// The mock's Touch controllers at the race loop's turn `turn` (--vr-mock-pad); false when there is no script.
bool MockPad(long turn, rr::xr::XrPad& pad);
bool MenuStickChangesValues(); // RRJB_VR_MENU_STICK=values (the control)
bool HapticsHoldOff();         // RRJB_VR_HAPTICS_HOLD=off (the control)
bool FallViewLegacy();         // RRJB_FALL_VIEW=legacy (the control)
// RRJB_SMOOTH=interp | stable | both (default both): which half of smooth motion runs (the measurement's split)
bool SmoothInterpOn();
bool SmoothStableOn();
// the stabilisers' leash along the bike's forward (FrameInterp::kMaxOffAlong; DEVELOPMENT
// RRJB_SMOOTH_ALONG=<world units> - 0.06, the across leash, is the control)
double SmoothAlongLeash();
// `x` kept within `across` of `z` square to the unit `fwd` and within `along` along it
void LeashAlong(double x[3], const double z[3], const float fwd[3], double across, double along);

// ---------------------------------------------------------------- the run's counters (one report line)
struct ComfortCounters {
    // the VR menu: value changes by source; rows moved by the stick
    size_t menuTrigger = 0, menuStick = 0, menuKeys = 0, menuConfirm = 0, menuRows = 0, menuOpened = 0;
    // the haptics: turns the menus / pause held them, the actuator commands sent in them and outside
    size_t hapticHeldTurns = 0, hapticRacingTurns = 0;
    uint64_t appliesHeld = 0, appliesRacing = 0, stopsHeld = 0, stopsRacing = 0, appliesMenu = 0, appliesPause = 0;
    size_t menuTurns = 0, pauseTurns = 0;
    // the fall view
    size_t falls = 0, reseats = 0, offFrames = 0, fadeFrames = 0, headFramesAfterReseat = 0, chaseAfterReseat = 0;
    double maxEyeStepOff = 0.0, maxEyeStepReseat = 0.0, maxRollOff = 0.0; // world units / degrees
    double latchDrift = 0.0; // the eye's point on the bike after a re-seat against the first latch (world units)
    // the interpolation
    size_t interpFrames = 0, interpEntities = 0, interpCuts = 0, restores = 0, restoreMismatch = 0, stabilised = 0;
    double stabPosSq = 0.0, stabPosMax = 0.0, stabAngSq = 0.0, stabAngMax = 0.0; // the drawn bike off the game's (m, deg)
};
ComfortCounters& Comfort();
std::string ComfortTotals();

// ---------------------------------------------------------------- smooth motion
// The alpha of the drawn state between the step before (0) and the last step (1): the render time one tick behind the
// real time (the last step's time + `carry` ticks), `ticks` the last step's length.
inline float InterpAlpha(double carry, int ticks) {
    if (ticks <= 0) return 1.0f;
    const double a = 1.0 - (1.0 - carry) / static_cast<double>(ticks);
    return static_cast<float>(a < 0.0 ? 0.0 : a > 1.0 ? 1.0 : a);
}

class FrameInterp {
public:
    static constexpr double kCut = 20.0; // world units a step: a respawn / teleport, drawn as it is
    // After a step: the fields of the new state (the previous capture becomes the step before).
    void Capture(const uint8_t* ram);
    void Invalidate() {
        have_ = 0;
        stab_ = Stab{};
    }
    // The state at `alpha` written into the arena `ram` (the session's own buffer) for drawing; false when nothing to
    // interpolate (fewer than two captures, alpha 1). Every true Apply is followed by Restore before the next step.
    bool Apply(uint8_t* ram, float alpha);
    void Restore(uint8_t* ram);
    bool Applied() const { return applied_; }
    // Another draw-only field (vr_horizon.h ViewPitch) of this frame (up to 18 bytes), put back by Restore
    // with the rest (the first bytes saved for an address are the step's)
    void Write(uint8_t* ram, uint32_t addr, const void* data, uint8_t size);
    // the last step's origin +0xB8 of a captured entity (world units); false: not captured
    bool StepOrigin(uint32_t addr, double out[3]) const;
    // how far `object` is drawn from its last step this frame (the shadows' shift, Shift) - set
    void SetShift(uint32_t addr, const float d[3]) { SetMoved(addr, d); }
    // While applied: how far `object` (a pool 0..4 entity; a rider: its bike's) is drawn from its last step (world).
    bool Shift(uint32_t object, float out[3]) const;
    // The player's bike of the head view, stabilised (after Apply, same frame; put back by Restore): a g-h tracking
    // filter per displayed frame on its origin +0xB8 and on its heading / pitch / roll (the rows +0x1B0 turned
    // together), so the step-to-step noise of the game's own bike pose - the road's height snaps, the suspension's
    // pitch, the lean - is not drawn, and the eye fixed on the drawn bike (vr_handlebars.h SeatEye) is rigid on a
    // smooth bike. Zero lag at a constant speed / turn rate; ~2.5 cm per 10 m/s^2 of acceleration. `stepSeconds`: the
    // last step's length (the velocity a reset starts from); `reset`: start again (a re-seat, a cut).
    bool Stabilize(uint8_t* ram, uint32_t bike, double dt, double stepSeconds, bool reset);
    // The stabilised forward and up (world, unit) of this frame's Stabilize for the camera: the drawn bike's, or - while
    // the game's pose snaps further than the leash - the filter's own (the view turns smoothly, the bike at once).
    bool StableAxes(float fwd[3], float up[3]) const;
    static constexpr double kPosG = 0.30, kAngG = 0.30; // per displayed frame; h = g^2 / (2 - g) (critically damped)
    // the drawn bike is never further than this from the game's (a hard brake, a knock: the filter is pulled along)
    static constexpr double kMaxOff = 0.06, kMaxOffRad = 3.0 * 3.14159265358979323846 / 180.0;
    // along the bike's forward the leash is longer - the game's own one-step hitches of the bike along the
    // road (13..20 cm at a stoppie's start on 1/23) are spread over a few frames instead of lurching the eye
    static constexpr double kMaxOffAlong = 0.20;
    // (DEVELOPMENT: RRJB_SMOOTH_G=<pos>,<angle> overrides them, for tuning)

private:
    struct Entry {
        uint32_t addr = 0;
        uint16_t handle = 0;
        uint8_t pool = 0;
        int32_t pos[3] = {};
        int16_t rows[9] = {}; // pools 0, 2, 3, 4
        int16_t root[3] = {}; // pool 1
        uint32_t owner = 0;   // pool 0: its rider (+0x354)
    };
    std::vector<Entry> prev_, cur_;
    int have_ = 0;
    bool applied_ = false;
    struct Saved {
        uint32_t addr;
        uint8_t bytes[18];
        uint8_t size;
    };
    std::vector<Saved> saved_;
    struct Moved {
        uint32_t addr;
        float d[3];
    };
    std::vector<Moved> moved_;
    void Save(uint8_t* ram, uint32_t addr, uint8_t size);
    void SetMoved(uint32_t addr, const float d[3]);
    struct Stab {
        bool have = false;
        uint32_t bike = 0;
        double x[3] = {}, v[3] = {}; // the origin (world units) and its velocity (units / s)
        double a[3] = {}, w[3] = {}; // heading (unwrapped), pitch, roll (radians) and their rates
        float fwd[3] = {0, 0, 1}, up[3] = {0, -1, 0};
        bool axes = false;
    } stab_;
};
FrameInterp& ProductFrameInterp(); // the race's (race_render.cpp moves the shadows with it)

// ---------------------------------------------------------------- the view off the bike
struct CamPose {
    bool valid = false;
    float eye[3] = {0, 0, 0}, fwd[3] = {0, 0, 1}, up[3] = {0, -1, 0};
};
// Between two cameras (eye lerped, axes lerped and re-orthonormalised); `b` when they are a cut apart.
CamPose LerpCam(const CamPose& a, const CamPose& b, float t);

class FallView {
public:
    // One displayed frame of the head-view mode. `seated`: the head view can be drawn (the rider on the bike);
    // `head`: its camera (valid when seated); `chase`: the original's camera; `rider`: the rider's box (world);
    // `fixed`: the fixed third-person view off the bike (false: the original's chase camera); `dt`: seconds.
    // Returns the camera to anchor on; `fade` gets the black to draw over the eyes (0..1).
    CamPose Frame(bool seated, const CamPose& head, const CamPose& chase, const float rider[3], bool fixed, float dt,
                  float& fade);
    bool Off() const { return off_; }
    bool JustChanged() const { return changed_; }
    void Reset() { *this = FallView{}; }

private:
    bool started_ = false, off_ = false, changed_ = false;
    float fade_ = 0.0f;
    float heading_[3] = {0, 0, 1};
    float eye_[3] = {0, 0, 0};
    CamPose last_;
};

// A black over the whole bound eye image (both layers when single-pass: // rr:multiview), `alpha` 0..1.
class VrFade {
public:
    void Draw(float alpha);

private:
    GLuint program_ = 0, vao_ = 0;
    GLint alphaLoc_ = -1;
};

// ---------------------------------------------------------------- the motion meter
// Per displayed race frame of the head view: the eye's anchor and the bike as drawn. The judder is what an unevenly
// stepped or noisy pose adds to smooth motion, measured by second differences over three displayed frames:
//   world  a point 5 m ahead of the eye, fixed in the world, in the eye's frame (mm);
//   bike   the handlebars' centre (the model point 0.6 m ahead of the origin, 0.4 m up), in the eye's frame (mm);
//   jerk   the eye's third difference over the display period (m/s^3).
struct PacingRow; // vr_pacing.h
class MotionMeter {
public:
    // `pacing`: the frame's clock and step for the CSV (vr_pacing.h; null: none)
    void Frame(long frame, int ticks, float alpha, double period, const rr::xr::WorldAnchor& anchor,
               const rr::render::Mat4* bike, const PacingRow* pacing = nullptr);
    void Break() { n_ = 0; } // a frame that is not a smooth head-view frame (the menu, the pause, off the bike)
    std::string Totals() const;

private:
    struct Sample {
        float eye[3], right[3], up[3], ahead[3], bar[3];
        bool bike;
        double t; // the display time since the first sample (s)
    };
    Sample s_[4] = {};
    int n_ = 0;
    size_t frames_ = 0, worldN_ = 0, bikeN_ = 0, jerkN_ = 0;
    double worldSq_ = 0, bikeSq_ = 0, jerkSq_ = 0, worldMax_ = 0, bikeMax_ = 0, speedSum_ = 0;
    std::FILE* csv_ = nullptr;
    bool csvTried_ = false;
    size_t csvRows_ = 0;
};
MotionMeter& ProductMotionMeter();

} // namespace rrgame
