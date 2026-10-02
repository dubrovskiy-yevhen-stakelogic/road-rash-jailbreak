#pragma once
// The VR race's time step on the display's clock - OURS, no original code. The rule is GT2's (the gt2-play project,
// tools\gt2game\xr_field_pacing.h): the game advances on OpenXR's predicted display time, never on the loop's wall
// clock; fitted here to a loop that steps BEFORE it waits for the frame it draws.
//
// WHY: a race stepped by the loop's wall clock (steady_clock between two loop turns, the 1/300 s fraction carried)
// judders. xrWaitFrame returns at an irregular point of the display period ("interval avg 13.92 / max 20.55 ms" at
// 72 Hz on the Quest), so one turn steps 2..7 ticks while every displayed frame is exactly one period after the one
// before: measured on the Quest, the world 5 m ahead jumps 49 mm RMS in the eye (7 in the mock), the eye's jerk is
// 30417 m/s^3 (1221 in the mock).
//
// THE RULE (DisplayPacing, PacingMode::kDisplay) - GT2's order: wait for the frame, then advance the game to its time:
//   * the race loop opens the frame (xrWaitFrame + xrBeginFrame) BEFORE the step (main.cpp), and Frame(clock) takes
//     its predictedDisplayTime D and predictedDisplayPeriod P;
//   * Step() steps the race to D in whole ticks (the fraction stays: the carry), counted from a base display time, so
//     no error accumulates; the frame is drawn one tick behind D between the last two steps (smooth motion's alpha,
//     vr_comfort.h InterpAlpha, from the carry after the step). A missed display (D grows by a multiple of P) is simply
//     a longer step: the frame shows the world where it is at D.
//   * Without an open frame (Step with frameOpen false) the target is the predicted D + P of the last frame.
//   * A refresh change (72 -> 90 Hz from the VR menu) is just the next P: the prediction takes it at once.
//   * No catch-up bursts: a gap longer than kMaxCatchUpPeriods display periods (a stall, the system menu) is not
//     stepped - the step covers one period and the rest is dropped; the host's hold (the headset off, not focused)
//     and the VR menu's hold call Reset(): the next step starts from the display's own time.
//   * The frame's dt for the filters (the head view's stabiliser, the bike shake's low-pass) is the display time since
//     the frame before (FrameSeconds), not the configured rate.
//
// DEVELOPMENT / verification:
//   RRJB_VR_PACING=wallclock  the step on the loop's wall clock (the control)
//   --vr-mock-timing "<spec>" the scripted mock's display clock made irregular like the headset's (implies
//                             --vr-mock-hz 72 when none is given): "jitter MS" (xrWaitFrame returns up to MS late: the
//                             loop's wall clock), "miss N" (every N-th frame the display skips a period), "switch FRAME
//                             HZ" (the refresh rate changes), "hold FRAME MS" (the host held the game: the display jumps
//                             MS, the timing reset flagged) - joined by ';'
//   --vr-judder-log <csv>     the motion meter's CSV (vr_comfort.h MotionMeter) with the pacing's columns, per head-view
//   RRJB_JUDDER_LOG=<csv>     frame; on the Quest put the flag in rrgame_args.txt - a relative path lands in the app's
//                             external files folder (adb pull /sdcard/Android/data/<package>/files/<csv>)
#include <cstdint>
#include <string>
#include <vector>

namespace rrgame {

// One compositor frame's clock (ns).
struct DisplayClock {
    bool valid = false;
    int64_t time = 0;   // xrWaitFrame's predictedDisplayTime (the mock: its synthetic display clock)
    int64_t period = 0; // predictedDisplayPeriod
    int64_t loop = 0;   // the loop's wall clock when xrWaitFrame returned (steady_clock; the mock: synthetic)
};

enum class PacingMode { kDisplay, kWallClock };
PacingMode VrPacingMode(); // RRJB_VR_PACING=wallclock: kWallClock (the control)
const char* PacingModeName(PacingMode m);
int64_t SteadyNowNs();

// One frame's numbers for the judder CSV.
struct PacingRow {
    double displayMs = 0, loopMs = 0, periodMs = 0, carry = 0, drawnErrMs = 0;
    int ticks = 0, missed = 0;
    PacingMode mode = PacingMode::kDisplay;
};

class DisplayPacing {
public:
    static constexpr double kTicksPerSecond = 300.0; // RaceStep's unit
    static constexpr int kMaxCatchUpPeriods = 4;

    explicit DisplayPacing(PacingMode mode = VrPacingMode()) : mode_(mode) {}
    PacingMode Mode() const { return mode_; }
    // The host held the game (TakeTimingReset) or the VR menu held the race: no time passes; the next step starts
    // from the display's time.
    void Reset();
    // The ticks to step now, clamped to [minTicks, maxTicks]. `loopNow`: the loop's wall clock (ns) - read only in the
    // wall-clock mode (the control); `ratePeriod`: the configured display period (s) - the steps before the first
    // frame's clock is known (they are counted into the display clock's base: the ticks equal the wall-clock carry's);
    // `frameOpen`: the frame last given to Frame is the one this step is drawn in (its D is the target).
    int Step(int64_t loopNow, int minTicks, int maxTicks, double ratePeriod, bool frameOpen);
    // After the frame's xrWaitFrame (invalid: nothing known, the prediction goes on from the last one).
    void Frame(const DisplayClock& clock);
    // Smooth motion's alpha for the frame last given to Frame (`smooth` false: 1, the last step) - also the drawn
    // game time's accounting (Totals).
    float Alpha(bool smooth);
    // The display time since the frame before (s), within [0.25, kMaxCatchUpPeriods] periods; `fallback` without one.
    double FrameSeconds(double fallback) const;
    int64_t FrameLoop() const { return lastLoop_; } // the last frame's loop clock (the mock's wall clock)
    const PacingRow& Row() const { return row_; }
    std::string Totals() const;

private:
    double TicksOf(int64_t t) const { return static_cast<double>(t - base_) * (kTicksPerSecond * 1e-9); }
    PacingMode mode_;
    // the display clock
    bool haveDisplay_ = false, based_ = false, reset_ = false;
    long preSteps_ = 0;     // steps before the first clock
    double preCarry_ = 0.0; // their carry
    double preTicks_ = 0.0; // their ticks
    int64_t lastDisplay_ = 0, lastPeriod_ = 0, lastLoop_ = 0, prevDisplay_ = 0, prevLoop_ = 0, base_ = 0;
    double stepped_ = 0.0; // ticks stepped since base_ (display mode)
    double carry_ = 0.0;   // the frame's carry (InterpAlpha)
    // the wall-clock mode (the control)
    double wallCarry_ = 0.0;
    int64_t wallLast_ = 0;
    bool wallHave_ = false;
    // the last step
    int lastTicks_ = 0;
    bool steppedSinceFrame_ = false, skipNextErr_ = true;
    double total_ = 0.0;     // every tick stepped (both modes)
    double lastDrawn_ = 0.0; // the drawn game time of the frame before (ticks)
    int64_t lastDrawnDisplay_ = 0;
    PacingRow row_;
    // the run's counters
    size_t frames_ = 0, steps_ = 0, missed_ = 0, missedFrames_ = 0, resets_ = 0, drops_ = 0, backwards_ = 0, errN_ = 0;
    double dropped_ = 0.0, displaySum_ = 0.0, displayMin_ = 1e9, displayMax_ = 0.0, loopSum_ = 0.0, loopMax_ = 0.0;
    size_t displayN_ = 0, loopN_ = 0;
    double errSq_ = 0.0, errMax_ = 0.0;
    size_t ticksHist_[9] = {};
    std::vector<std::string> refreshChanges_;
};
DisplayPacing& ProductPacing();

// ---------------------------------------------------------------- the command line (vr_settings.cpp ApplyVrFlag)
bool ApplyPacingFlag(int argc, char** argv, int& i); // --vr-mock-timing, --vr-judder-log
std::string JudderLogPath();                        // --vr-judder-log, else RRJB_JUDDER_LOG, else ""
bool MockTimingOn();

// The scripted mock's display clock (MockDisplay, game_host_vr.cpp): one call per mock BeginFrame.
class MockDisplayClock {
public:
    DisplayClock Next(double baseHz);
    bool TakeHeld(); // a "hold" happened (the host's timing reset)
    double Hz() const { return hz_; }

private:
    long frame_ = 0;
    double t_ = 1e12, hz_ = 72.0; // ns
    uint32_t rng_ = 0x2545F491u;
    int64_t lastLoop_ = 0;
    bool held_ = false;
};
MockDisplayClock& ProductMockClock();

} // namespace rrgame
