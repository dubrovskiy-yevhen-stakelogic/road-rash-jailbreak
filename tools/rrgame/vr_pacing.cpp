// The VR race's time step on the display's clock (vr_pacing.h).
#include "vr_pacing.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <stdexcept>

namespace rrgame {

namespace {

struct MockTimingSpec {
    bool on = false;
    double jitterMs = 0.0;
    long missEvery = 0;
    std::vector<std::pair<long, double>> switches; // frame, Hz
    std::vector<std::pair<long, double>> holds;    // frame, ms
};
MockTimingSpec& Spec() {
    static MockTimingSpec s;
    return s;
}
std::string& JudderFlag() {
    static std::string p;
    return p;
}

void ParseSpec(const std::string& text) {
    MockTimingSpec s;
    s.on = true;
    std::stringstream all(text);
    std::string part;
    while (std::getline(all, part, ';')) {
        std::stringstream in(part);
        std::string what;
        if (!(in >> what)) continue;
        if (what == "jitter") {
            if (!(in >> s.jitterMs) || s.jitterMs < 0.0 || s.jitterMs > 50.0) throw std::runtime_error("--vr-mock-timing: jitter MS (0..50)");
        } else if (what == "miss") {
            if (!(in >> s.missEvery) || s.missEvery < 2) throw std::runtime_error("--vr-mock-timing: miss N (N >= 2)");
        } else if (what == "switch") {
            long f = 0;
            double hz = 0.0;
            if (!(in >> f >> hz) || f < 0 || hz < 20.0 || hz > 300.0) throw std::runtime_error("--vr-mock-timing: switch FRAME HZ");
            s.switches.emplace_back(f, hz);
        } else if (what == "hold") {
            long f = 0;
            double ms = 0.0;
            if (!(in >> f >> ms) || f < 1 || ms <= 0.0) throw std::runtime_error("--vr-mock-timing: hold FRAME MS");
            s.holds.emplace_back(f, ms);
        } else {
            throw std::runtime_error("--vr-mock-timing: unknown item '" + what + "'");
        }
    }
    std::sort(s.switches.begin(), s.switches.end());
    Spec() = s;
}

} // namespace

PacingMode VrPacingMode() {
    static const PacingMode m = [] {
        const char* e = std::getenv("RRJB_VR_PACING");
        return e != nullptr && std::strcmp(e, "wallclock") == 0 ? PacingMode::kWallClock : PacingMode::kDisplay;
    }();
    return m;
}

const char* PacingModeName(PacingMode m) {
    return m == PacingMode::kWallClock ? "the loop's wall clock (RRJB_VR_PACING=wallclock)"
                                       : "the display's clock (predictedDisplayTime)";
}

int64_t SteadyNowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// ================================================================ the pacing
void DisplayPacing::Reset() {
    if (!based_ && !wallHave_) return; // nothing stepped on a clock yet: nothing to hold
    reset_ = true;
    skipNextErr_ = true;
    ++resets_;
}

int DisplayPacing::Step(int64_t loopNow, int minTicks, int maxTicks, double ratePeriod, bool frameOpen) {
    ++steps_;
    steppedSinceFrame_ = true;
    int ticks = 0;
    if (mode_ == PacingMode::kWallClock) { // the step on the loop's wall clock, the control
        double elapsed = 0.0;
        if (wallHave_ && !reset_) elapsed = static_cast<double>(loopNow - wallLast_) * 1e-9;
        wallLast_ = loopNow;
        wallHave_ = true;
        reset_ = false;
        wallCarry_ = std::min(wallCarry_ + std::max(0.0, elapsed) * kTicksPerSecond, 60.0);
        ticks = static_cast<int>(wallCarry_);
        wallCarry_ -= ticks;
    } else if (!based_) { // no frame's clock yet: the configured rate, carried (the base counts these steps)
        preCarry_ += kTicksPerSecond * (ratePeriod > 0.0 ? ratePeriod : 1.0 / 72.0);
        ticks = static_cast<int>(preCarry_);
        preCarry_ -= ticks;
        ++preSteps_;
    } else {
        // the open frame's display time, or the next frame's predicted one
        const int64_t target = frameOpen ? lastDisplay_ : lastDisplay_ + lastPeriod_;
        const double periodTicks = static_cast<double>(lastPeriod_) * (kTicksPerSecond * 1e-9);
        if (reset_) { // from the display's own time: this step covers one period
            base_ = target - lastPeriod_;
            stepped_ = 0.0;
            reset_ = false;
        }
        double want = TicksOf(target) - stepped_;
        if (want > kMaxCatchUpPeriods * periodTicks + 1.0) { // a stall: not stepped, no burst
            dropped_ += want - periodTicks;
            ++drops_;
            stepped_ = TicksOf(target) - periodTicks;
            want = periodTicks;
            skipNextErr_ = true;
        }
        ticks = static_cast<int>(std::floor(want + 1e-6));
    }
    ticks = std::clamp(ticks, minTicks, maxTicks);
    if (mode_ == PacingMode::kDisplay) {
        if (based_) stepped_ += ticks;
        else preTicks_ += ticks;
    }
    total_ += ticks;
    lastTicks_ = ticks;
    ++ticksHist_[std::min(ticks, 8)];
    return ticks;
}

void DisplayPacing::Frame(const DisplayClock& c) {
    row_ = PacingRow{};
    row_.mode = mode_;
    if (!c.valid) return;
    ++frames_;
    if (haveDisplay_) {
        const int64_t delta = c.time - lastDisplay_;
        if (delta <= 0) {
            ++backwards_;
        } else {
            const double ms = static_cast<double>(delta) * 1e-6;
            row_.displayMs = ms;
            displaySum_ += ms;
            displayMin_ = std::min(displayMin_, ms);
            displayMax_ = std::max(displayMax_, ms);
            ++displayN_;
            const double ratio = lastPeriod_ > 0 ? static_cast<double>(delta) / static_cast<double>(lastPeriod_) : 1.0;
            if (ratio > 1.5) {
                if (ratio > kMaxCatchUpPeriods + 0.5) skipNextErr_ = true; // a hold / a stall, not a missed frame
                else {
                    row_.missed = static_cast<int>(std::lround(ratio)) - 1;
                    missed_ += static_cast<size_t>(row_.missed);
                    ++missedFrames_;
                }
            }
        }
        if (lastPeriod_ > 0 && std::llabs(c.period - lastPeriod_) * 100 > lastPeriod_ && refreshChanges_.size() < 8) {
            char b[96];
            std::snprintf(b, sizeof(b), "%.1f -> %.1f Hz at frame %zu", 1e9 / static_cast<double>(lastPeriod_),
                          c.period > 0 ? 1e9 / static_cast<double>(c.period) : 0.0, frames_);
            refreshChanges_.push_back(b);
        }
        if (c.loop > lastLoop_ && lastLoop_ != 0) {
            const double ms = static_cast<double>(c.loop - lastLoop_) * 1e-6;
            row_.loopMs = ms;
            loopSum_ += ms;
            loopMax_ = std::max(loopMax_, ms);
            ++loopN_;
        }
    }
    prevDisplay_ = haveDisplay_ ? lastDisplay_ : 0;
    prevLoop_ = lastLoop_;
    lastDisplay_ = c.time;
    lastPeriod_ = c.period > 0 ? c.period : (lastPeriod_ > 0 ? lastPeriod_ : 13888889);
    lastLoop_ = c.loop;
    haveDisplay_ = true;
    if (mode_ == PacingMode::kDisplay && !based_) {
        // the first clock: the steps before it ran one configured period each, the first of them ending at this
        // display - the base is that many periods back, so the ticks go on exactly as the carry had them
        base_ = lastDisplay_ - static_cast<int64_t>(std::max<long>(preSteps_, 1)) * lastPeriod_;
        stepped_ = preTicks_;
        based_ = true;
    }
    row_.periodMs = static_cast<double>(lastPeriod_) * 1e-6;
}

float DisplayPacing::Alpha(bool smooth) {
    // the carry: the frame's display time past the last step (ticks)
    if (mode_ == PacingMode::kWallClock) carry_ = wallCarry_;
    else carry_ = based_ && haveDisplay_ ? TicksOf(lastDisplay_) - stepped_ : 0.0;
    row_.carry = carry_;
    row_.ticks = lastTicks_;
    float a = 1.0f;
    if (smooth && lastTicks_ > 0) { // vr_comfort.h InterpAlpha: the drawn time one tick behind the frame's time
        const double v = 1.0 - (1.0 - carry_) / static_cast<double>(lastTicks_);
        a = static_cast<float>(v < 0.0 ? 0.0 : v > 1.0 ? 1.0 : v);
    }
    if (steppedSinceFrame_ && haveDisplay_) {
        const double drawn = total_ - static_cast<double>(lastTicks_) * (1.0 - static_cast<double>(a)); // ticks
        if (!skipNextErr_ && lastDrawnDisplay_ != 0 && lastDisplay_ > lastDrawnDisplay_) {
            const double drawnMs = (drawn - lastDrawn_) * 1000.0 / kTicksPerSecond;
            const double dispMs = static_cast<double>(lastDisplay_ - lastDrawnDisplay_) * 1e-6;
            const double err = drawnMs - dispMs;
            row_.drawnErrMs = err;
            errSq_ += err * err;
            errMax_ = std::max(errMax_, std::abs(err));
            ++errN_;
        }
        lastDrawn_ = drawn;
        lastDrawnDisplay_ = lastDisplay_;
        skipNextErr_ = false;
    }
    steppedSinceFrame_ = false;
    return a;
}

double DisplayPacing::FrameSeconds(double fallback) const {
    if (!haveDisplay_ || prevDisplay_ == 0 || lastPeriod_ <= 0) return fallback;
    const double p = static_cast<double>(lastPeriod_) * 1e-9;
    const double s = static_cast<double>(lastDisplay_ - prevDisplay_) * 1e-9;
    return std::clamp(s, 0.25 * p, kMaxCatchUpPeriods * p);
}

std::string DisplayPacing::Totals() const {
    if (steps_ == 0) return {};
    std::string hist;
    for (int t = 0; t <= 8; ++t) {
        if (ticksHist_[t] == 0) continue;
        char b[32];
        std::snprintf(b, sizeof(b), "%s%d%s:%zu", hist.empty() ? "" : " ", t, t == 8 ? "+" : "", ticksHist_[t]);
        hist += b;
    }
    std::string changes;
    for (const std::string& c : refreshChanges_) changes += (changes.empty() ? "" : ", ") + c;
    char b[900];
    std::snprintf(b, sizeof(b),
                  "vr pacing: the race stepped on %s; %zu step(s), ticks per step {%s}; %zu frame(s): the "
                  "display interval avg %.3f / min %.3f / max %.3f ms, the loop's interval avg %.3f / max %.3f ms; missed "
                  "displays %zu (on %zu frame(s)), refresh changes %zu%s%s%s, clock backwards %zu; resets %zu, stalls not "
                  "stepped %zu (%.1f ticks dropped); the drawn game time against the display time, frame to frame: error "
                  "RMS %.3f ms, max %.3f ms (%zu)\n",
                  PacingModeName(mode_), steps_, hist.c_str(), frames_, displayN_ ? displaySum_ / static_cast<double>(displayN_) : 0.0,
                  displayN_ ? displayMin_ : 0.0, displayMax_, loopN_ ? loopSum_ / static_cast<double>(loopN_) : 0.0, loopMax_,
                  missed_, missedFrames_, refreshChanges_.size(), changes.empty() ? "" : " (", changes.c_str(),
                  changes.empty() ? "" : ")", backwards_, resets_, drops_, dropped_,
                  errN_ ? std::sqrt(errSq_ / static_cast<double>(errN_)) : 0.0, errMax_, errN_);
    return b;
}

DisplayPacing& ProductPacing() {
    static DisplayPacing p;
    return p;
}

// ================================================================ the command line
bool ApplyPacingFlag(int argc, char** argv, int& i) {
    const std::string a = argv[i];
    if (a == "--vr-mock-timing") {
        if (i + 1 >= argc) throw std::runtime_error("--vr-mock-timing needs a spec");
        ParseSpec(argv[++i]);
        return true;
    }
    if (a == "--vr-judder-log") {
        if (i + 1 >= argc) throw std::runtime_error("--vr-judder-log needs a file");
        JudderFlag() = argv[++i];
        return true;
    }
    return false;
}

std::string JudderLogPath() {
    if (!JudderFlag().empty()) return JudderFlag();
    const char* e = std::getenv("RRJB_JUDDER_LOG");
    return e != nullptr ? e : "";
}

bool MockTimingOn() { return Spec().on; }

// ================================================================ the mock's display clock
DisplayClock MockDisplayClock::Next(double baseHz) {
    const MockTimingSpec& s = Spec();
    double hz = baseHz > 0.0 ? baseHz : 72.0;
    for (const auto& sw : s.switches)
        if (frame_ >= sw.first) hz = sw.second;
    hz_ = hz;
    const double period = 1e9 / hz;
    if (frame_ > 0) {
        t_ += period;
        if (s.missEvery > 0 && frame_ % s.missEvery == 0) t_ += period; // the display skipped one
        for (const auto& h : s.holds)
            if (h.first == frame_) {
                t_ += h.second * 1e6;
                held_ = true;
            }
    }
    rng_ = rng_ * 1664525u + 1013904223u;
    const double u = static_cast<double>(rng_ >> 8) / 16777216.0; // [0, 1)
    DisplayClock c;
    c.valid = true;
    c.time = static_cast<int64_t>(std::llround(t_));
    c.period = static_cast<int64_t>(std::llround(period));
    // xrWaitFrame returns up to `jitter` late in the period: the loop's wall clock
    c.loop = std::max<int64_t>(lastLoop_ + 1, static_cast<int64_t>(std::llround(t_ - period - s.jitterMs * 1e6 * (1.0 - u))));
    lastLoop_ = c.loop;
    ++frame_;
    return c;
}

bool MockDisplayClock::TakeHeld() {
    const bool h = held_;
    held_ = false;
    return h;
}

MockDisplayClock& ProductMockClock() {
    static MockDisplayClock c;
    return c;
}

} // namespace rrgame
