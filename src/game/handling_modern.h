#pragma once
// The player's handling: Original (the original's controls, untouched) or Modern (GTA San Andreas-style riding).
// OURS: nothing here is the original's, and nothing here edits a ported function.
//
// WHAT THE ORIGINAL DOES (bike.h): the pad's steering reaches the bike in one of two
// ways. A DIGITAL pad (0x41: the keyboard, the d-pad, a stick through the bindings) sets flagsA +0x230 bits 0x100 /
// 0x200; BikeAimTarget 0x80072FB4 then gives BikeApplySteering 0x80074570 a constant ramp rate +0x248 and the steering's
// lateral response +0x1E8 (the heading rate, 16.16 rad/s, the integrator 0x8007F0BC turns +0x210 by it) runs at that
// rate until it clamps at the stat block's +0xE8; on release a timed "servo" move eases the angle back. An ANALOGUE pad
// (0x73, the DualShock's ANALOG mode; its left stick X through AxisCurve 0x8001CA58 into 0x800CE544) takes the speed-band
// path of BikeAimTarget instead: target +0xC4 = response(speed) x axis, and +0x1E8 chases it at the same ramp rate. The
// lean +0x33A (and with it the drawn roll of +0x1B0) is an ALGEBRAIC function of +0x1E8 and the speed (BikeSolveSteer
// 0x80074170): no dynamics of its own. So the digital pad gives the heading rate a trapezoid - constant-rate ramps with
// corners - and the bike's roll the same trapezoid.
//
// WHAT MODERN DOES (after GTA SA's CBike, reVC Bike.cpp / SA handling.cfg and the gta-sa-vr / gta-sa-vr-quest
// projects' native\src\Driving.cpp). Three layers, all OUTSIDE the ported step:
//   1. THE INPUT. Whatever steers (the keys, a stick, the VR handlebars) becomes SA's m_fSteerInput: a first-order lag
//      toward the input (SA: 0.2 per 1/50 s step, tau 0.09 s; the VR bars 0.35 per step, tau 0.046 s), then SA's
//      squared response (sign x s^2 - gentle near the centre, and a zero-slope start so the heading rate rises as an S,
//      not as a ramp). The speed dependence is the original's own (BikeAimTarget's response by speed band). The
//      result is fed to the ORIGINAL as what a DualShock in ANALOG mode would send (device 0x73, the stick's X byte
//      through the game's own AxisCurve and ENV.EN curve, the throttle / brake as the right stick's Y) - the ported
//      reader, BikeAimTarget and the rest run exactly as they do for a console analogue pad.
//   2. THE TURN. The original's analogue path still RAMPS the heading-rate state +0x1E8 toward its target at a constant
//      rate +0x248 (about 34 deg/s per second on the player's machine): a step of the stick gives a linear heading-rate
//      ramp with corners, which is the stiffness. After the ported step (never inside it) the layer moves +0x1E8 (and
//      the rider's copy) toward the SAME target the original chases - BikeAimTarget's response(speed) x the axis the
//      reader stored, re-derived from the stat block exactly as 0x800734E8..0x8007351C does - through a critically damped
//      spring (tau 0.06 s: a step settles in ~0.3 s along an S, the heading rate's own change continuous), clamped to the original's own limit stats[+0xE8]. As in SA (its speed cap doubles while the bike
//      leans the way it steers), the lean widens the response up to twice while it builds toward 45 deg: a held turn
//      tightens, as the original's held key keeps ramping. The next frame's BikeApplySteering starts from it
//      (its ramp adds at most one step), BikeSolveSteer derives the steer and the lean from it, the integrator turns by
//      it. The ceiling (the most the bike can ever turn) is the original's +0xE8; the way there is SA's.
//   3. THE PICTURE. The drawn bike (and the rider on it, the head camera, the VR hands) leans like SA's bike: toward
//      a target lean clamped to SA's fMaxLean (45 deg), through SA's fDesLean lag (0.93 per 1/50 s step, tau 0.28 s) -
//      the roll trails the turn with weight instead of snapping with it. Render only: +0x1B0 in the guest is untouched
//      (race_render.h GameView::playerLean). lean_model=game takes the game's own roll of +0x1B0 as the target.
//      The default target is SA's own (lean_model=sa): asin(lateral acceleration / g) clamped to
//      fMaxLean (reVC CBike::ProcessControl "Process leaning"), the acceleration of the turn the bike is ASKED to make -
//      speed x the turn layer's target heading rate - so the lean leads the turn as SA's does (its tyres turn the bike
//      within a fraction of a second), and a moderate turn at speed leans far over. In the VR head view Modern's lean is
//      drawn whole (not the Visual bike lean %), and the view keeps HandlingSettings::cameraRollPct of it against the
//      horizon lock. The turn layer widens its response with the same SA lean and straightens twice as fast.
// Below 3 units/s (standing, crawling) Modern hands the pad back to the original's digital one: only the digital
// throttle's bit lets a stopped bike turn on the spot (BikeSteerDriver's (flagsA & 0x42) == 2), so a bike stopped against
// a wall under the analogue pad could not turn away.
// Original = none of it: ApplyToPad leaves the pad as it is, the visual roll is not offered.
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "game/race_session.h" // PadState
#include "game/wheelie.h"       // WheelieSettings

namespace rr::game {

enum class HandlingMode : int { kOriginal = 0, kModern = 1 };
const char* HandlingModeName(HandlingMode m); // "original" / "modern"

// The settings ([handling] of rrgame_settings.ini, the F10 overlay's and the VR menu's Handling rows, --handling*).
struct HandlingSettings {
    HandlingMode desktop = HandlingMode::kOriginal; // an interactive desktop race: the original as it was
    HandlingMode vr = HandlingMode::kModern;        // an interactive VR race: first person is where the trapezoid hurts
    // Modern's tuning, percent of the SA value (100 = SA's own):
    int steerLagPct = 100;    // the input lag (SA tau 0.09 s; the VR bars 0.046 s); 0 = none
    int curvePct = 100;       // the response exponent 1 + curvePct / 100: 100 = SA's square (2.0), 0 = linear (1.0)
    int turnLagPct = 100;     // the turn layer's spring (tau 0.06 s at 100); 0 = off (the original's constant-rate ramp)
    int leanLagPct = 100;     // the drawn lean's lag (SA tau 0.28 s); 0 = none
    int maxLeanDeg = 45;      // SA fMaxLean (PCJ-600 45, Sanchez 48)
    // The drawn lean's target. SA (lean_model=sa, the default): SA's own - asin(the lateral acceleration of
    // the turn the bike is asked to make / g), clamped to fMaxLean: speed x the heading rate the turn layer drives to (its
    // target, which leads the turn), the heading-rate state +0x1E8 where the layer is not driving. game
    // (lean_model=game): the game's own roll of +0x1B0, clamped to fMaxLean.
    bool leanFromTurn = true;
    // The VR head view under Modern keeps this % of the drawn lean as the view's roll against the horizon
    // lock (the lock levels the rest: 100 % lock -> the view rolls cameraRollPct % of the drawn lean; 0 = the lock alone)
    int cameraRollPct = 40;
    // The wheelies (game/wheelie.h) - their keys live in this section too
    WheelieSettings wheelie;

    bool Parse(const std::string& key, const std::string& value);
    std::string Serialize() const; // the [handling] section's lines
    std::string Describe() const;  // one log line
    HandlingMode ModeFor(bool vrRun) const { return vrRun ? vr : desktop; }
};
HandlingSettings& Handling(); // this process's (rrgame: handling_settings.h StartHandling)

// A scripted run's steering (a test input, like --autosteer; --steer-script): "F v; F v; ..." - from frame F on, v is
// held: L / R (the steering keys), 0 (nothing), or a number -1..1 (a stick's X, left negative). The same inputs reach
// both modes: Original sees a stick through the bindings as the digital left / right (|x| >= 1/3).
struct SteerScript {
    struct Step {
        long frame;
        char key;    // 'L', 'R', '0' or 's' (stick)
        float stick; // key 's'
    };
    std::vector<Step> steps;
    bool Parse(const std::string& text); // false on a malformed step
    const Step* At(long frame) const;    // the step in force, or null before the first
};

// What main.cpp hands the layer each frame, before the ported frame runs.
struct HandlingPadIn {
    const uint8_t* ram = nullptr; // the arena (2 MB, guest addresses & 0x1FFFFF)
    uint32_t bike = 0;            // player 0's bike entity
    bool vrRun = false;           // a VR host (its Handling default and the handlebars)
    bool barsHeld = false;        // VR Handlebars mode with a hand on the bars: pad.device.lx is the bars' angle
    bool paused = false;          // game state 3 / 4 (the pause menu owns the pad)
    bool gamepad = false;         // a controller is connected: `stickX` is its left stick
    uint8_t stickX = 0x80;        // 0x00 full left .. 0xFF full right
    long frame = 0;
};

class ModernHandling {
public:
    ModernHandling() = default;
    ModernHandling(const ModernHandling&) = delete;
    ModernHandling& operator=(const ModernHandling&) = delete;
    ~ModernHandling();
    // A race starts: the state and the metrics from zero (the script and the CSV path stay).
    void BeginRace();
    // Before the frame: the scripted steering (both modes), then - Modern, the rider seated, not paused - the steering
    // shaped and handed to the original as the analogue pad. Original: the pad is returned exactly as it came.
    void ApplyToPad(const HandlingPadIn& in, PadState& pad);
    // After the frame (the ported step ran): the drawn lean, the metrics, the per-frame CSV (--handling-log).
    // `dt` is the frame's 16.16 seconds (FrameLog::dt), `routeDistance` the player's (RaceSession::RouteDistance(0)).
    void AfterFrame(uint8_t* ram, uint32_t bike, int32_t dt, double routeDistance, long frame);
    // The drawn roll the picture should have this frame (radians, the sign of vr_visual_lean.h's roll): Modern, the
    // rider seated and the state warm. False: draw the original's own.
    bool VisualRoll(float& radians) const;
    // The run's summary line ("handling: ...") for the log and the gates; Engaged: Modern ran, or a test input asked
    // (Original with neither adds no line, so its log stays the log it always was).
    std::string Summary() const;
    bool Engaged() const { return modernSeen_ || !script.steps.empty() || !csvPath.empty(); }

    SteerScript script;
    std::string csvPath; // --handling-log: one row per frame
    // Where this run's settings came from (rrgame's StartHandling), for the race's first line
    std::string settingsSource;
    // The VR head view's camera roll under Modern (HandlingSettings::cameraRollPct as 0..1) while the drawn
    // lean is Modern's (VisualRoll valid), else 0 (the horizon lock alone)
    float CameraRollKeep() const;

private:
    bool tableReady_ = false;
    // the turn layer (TurnLayer): the heading-rate state it keeps, whether it wrote this frame
    bool turnWarm_ = false, turnWritten_ = false, turnLeanWarm_ = false;
    double turnLat_ = 0.0, turnVel_ = 0.0;
    float turnLean_ = 0.0f; // SA's lean (the game's roll through fDesLean, 45 deg) that widens the lock
    double turnTarget_ = 0.0; // the heading rate the layer drove to this frame (16.16 rad/s)
    void TurnLayer(uint8_t* ram, uint32_t bike, bool seated);
    // The modes this race rode (the menu can switch them mid-race), the race's first line
    bool announced_ = false;
    HandlingMode lastMode_ = HandlingMode::kOriginal;
    long modeFrames_[2] = {};
    int switches_ = 0;
    uint32_t statsBike_ = 0;
    int32_t axisOfByte_[256] = {}; // the game's AxisCurve over its own ENV.EN curve, every X byte (16.16)
    float steer_ = 0.0f;           // SA's m_fSteerInput
    float lastDt_ = 1.0f / 60.0f;
    HandlingMode mode_ = HandlingMode::kOriginal;
    bool modernSeen_ = false;
    bool shapedThisFrame_ = false;
    float input_ = 0.0f, axis_ = 0.0f; // this frame's raw input and the axis handed to the original
    uint8_t lx_ = 0x80;
    // the picture
    bool leanWarm_ = false, leanValid_ = false;
    float lean_ = 0.0f;       // SA's m_fLeanLRAngle (radians, drawn)
    float lastYaw_ = 0.0f;
    bool haveYaw_ = false;
    // the metrics
    struct Metrics {
        long frames = 0, riding = 0, shaped = 0, turned = 0;
        double maxRate = 0, maxAlat = 0, maxOrigRoll = 0, maxDrawnRoll = 0;
        double sumJerk2 = 0, maxJerk = 0, sumAcc2 = 0, maxAcc = 0; // of the heading-rate state
        double sumRollAcc2 = 0, maxRollAcc = 0;                    // of the drawn roll
        long jerkN = 0, rollN = 0;
        double distance = 0, firstDistance = -1;
        double sumSpeed = 0;
        // the steps: from an input's onset (|input| crossing 0.5) to a 10 deg/s turn its way, from its release back
        // under 2 deg/s
        double sumOnset = 0, sumRelease = 0;
        long onsets = 0, releases = 0;
    } m_;
    int onsetSign_ = 0;       // an onset being timed (+-1), 0 none
    long onsetFrames_ = 0, releaseFrames_ = 0;
    bool releaseTimed_ = false;
    float lastInput_ = 0.0f;
    double rate_[3] = {}; // the last three heading-rate states (deg/s)
    int rateN_ = 0;
    double roll_[3] = {};
    int rollN_ = 0;
    std::FILE* csv_ = nullptr;
    void BuildTable(const uint8_t* ram);
    uint8_t ByteFor(float axis) const;
};

// The layer's instance for player 0 (rrgame's race loop, the renderer's and the VR grips' visual roll).
ModernHandling& PlayerHandling();

} // namespace rr::game
