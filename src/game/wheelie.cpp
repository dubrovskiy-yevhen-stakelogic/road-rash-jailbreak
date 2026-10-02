// Wheelies (wheelie.h). OURS.
#include "game/wheelie.h"

#include "game/handling_modern.h" // Handling(): the settings live in its [handling] section
#include "game/sim/coll_serve.h"  // ServeCollNative: BikeVsTraffic 0x800AC5BC and the launch 0x80084BE8, PORTED

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <sstream>

namespace rr::game {

namespace {

constexpr float kPi = 3.14159265358979f;
constexpr float kDeg = kPi / 180.0f;
// SA's per-step lag (CBike::ProcessControlInputs m_fLeanInput += (in - x) * 0.2 per 1/50 s): tau 0.0896 s
const float kLeanTau = -0.02f / std::log(1.0f - 0.2f);
constexpr float kMinSpeed = 3.0f;       // units/s: below it the front comes down (and no wheelie starts)
constexpr float kLiftSpeed = 4.0f;      // units/s: a wheelie starts only above it
constexpr float kStartSpeed = 6.0f;     // units/s: the lean back asked for starts a wheelie only above it
// a stick's travel back (0..1): past kStickDownBinding the bindings' LsDown presses Down (a third: 42 of 128); a
// wheelie is asked for at kStickStart (near full travel) and leans back from kStickHold (half) up to kStickStart
constexpr float kStickDownBinding = 42.0f / 127.0f;
constexpr float kStickStart = 0.85f;
constexpr float kStickHold = 0.5f;
constexpr float kSpringHz = 0.9f;       // the pitch's spring toward its target (SA's stabilisation about fWheelieAng)
constexpr float kDamping = 0.6f;        // ... a slight overshoot: the bike's weight
constexpr float kBalanceOver = 20.0f;   // degrees past the hold angle: the balance point
constexpr float kLoopDeg = 80.0f;       // past the balance point the loop-over falls here
constexpr float kCreepDegPerS = 9.0f;   // held at full pull and full throttle: the pull keeps raising the nose
constexpr float kArmedDeg = 12.0f;      // "in a wheelie" for a car hit
constexpr long kFlightFrames = 360;     // the launch's flight: contacts with cars undone at most this long (6 s at 60)

constexpr uint32_t kPool3Control = 0x800CF650u, kPool3Slots = 0x800CF660u, kCarBytes = 512u;

inline bool InRam(uint32_t a) { return a >= 0x80000000u && a < 0x801FFFF0u; }
inline uint32_t U32(const uint8_t* ram, uint32_t a) {
    uint32_t v;
    std::memcpy(&v, ram + (a & 0x1FFFFFu), 4);
    return v;
}
inline int32_t S32(const uint8_t* ram, uint32_t a) { return static_cast<int32_t>(U32(ram, a)); }
inline int16_t S16(const uint8_t* ram, uint32_t a) {
    int16_t v;
    std::memcpy(&v, ram + (a & 0x1FFFFFu), 2);
    return v;
}
inline void W32(uint8_t* ram, uint32_t a, uint32_t v) { std::memcpy(ram + (a & 0x1FFFFFu), &v, 4); }

// DEVELOPMENT RRJB_WHEELIE_LAYER=off: no entry does anything (the control that the option's "off" is the game without
// the layer - tests\run_gates.ps1)
bool LayerOff() {
    static const bool off = [] {
        const char* v = std::getenv("RRJB_WHEELIE_LAYER");
        return v != nullptr && std::strcmp(v, "off") == 0;
    }();
    return off;
}

// DEVELOPMENT RRJB_WHEELIE_INPUT=legacy: the lean back read as before the deliberate start (Wheelie::BeforeHandling)
bool LegacyInput() {
    static const bool legacy = [] {
        const char* v = std::getenv("RRJB_WHEELIE_INPUT");
        return v != nullptr && std::strcmp(v, "legacy") == 0;
    }();
    return legacy;
}

bool Seated(const uint8_t* ram, uint32_t bike) {
    const uint32_t rider = U32(ram, bike + 0x354u);
    if (!InRam(rider)) return false;
    return S16(ram, rider + 0x140u) != 0 && U32(ram, rider + 0x25Cu) < 2u;
}

// On the road under its own power: no crash, no flight (flagsC 0x600), no latched impact (flagsB 0x400). (flagsC's low
// bits are the hit class: 1 bump, 2 side, 4 hard, 8 very hard, 16 / 32 side-on - HardHit.)
bool Grounded(const uint8_t* ram, uint32_t bike) {
    const uint32_t fb = U32(ram, bike + 0x234u), fc = U32(ram, bike + 0x238u);
    return (fc & 0x600u) == 0u && (fb & 0x400u) == 0u;
}
bool HardHit(const uint8_t* ram, uint32_t bike) { return (U32(ram, bike + 0x238u) & 0x3Cu) != 0u; }

// The nearest live car (pool 3) ahead within `range` units: its distance along the bike's heading and to the side
// (+ = the bike's right: the model's x). False: none.
bool NearestCar(const uint8_t* ram, uint32_t bike, float range, float& along, float& side, uint32_t& which) {
    const float bx = S32(ram, bike + 0xB8u) / 65536.0f, bz = S32(ram, bike + 0xC0u) / 65536.0f;
    const float hx = S16(ram, bike + 0x1C2u) / 4096.0f, hz = S16(ram, bike + 0x1C6u) / 4096.0f;
    const int32_t high = S32(ram, kPool3Control + 8u);
    float best = 1e9f;
    for (int32_t k = 0; k <= high && k < 16; ++k) {
        const uint32_t car = kPool3Slots + kCarBytes * static_cast<uint32_t>(k);
        if ((U32(ram, car + 0xACu) & 0xFFFFu) == 0u || (U32(ram, car + 0x140u) & 0xFFFFu) == 0u) continue;
        const float dx = S32(ram, car + 0xB8u) / 65536.0f - bx, dz = S32(ram, car + 0xC0u) / 65536.0f - bz;
        const float a = dx * hx + dz * hz, s = dx * hz - dz * hx;
        if (a < -3.0f || a > range || std::fabs(s) > std::max(a, 0.0f) + 6.0f) continue;
        if (a < best) {
            best = a;
            along = a;
            side = s;
            which = car;
        }
    }
    return best < 1e9f;
}

float Speed(const uint8_t* ram, uint32_t bike) { return static_cast<float>(S32(ram, bike + 0x240u)) / 65536.0f; }

bool ParseOnOff(const std::string& v, bool& out) {
    if (v == "on" || v == "1" || v == "true") out = true;
    else if (v == "off" || v == "0" || v == "false") out = false;
    else return false;
    return true;
}
bool ParseInt(const std::string& v, int lo, int hi, int& out) {
    char* end = nullptr;
    const long x = std::strtol(v.c_str(), &end, 10);
    if (end == v.c_str() || *end != '\0' || x < lo || x > hi) return false;
    out = static_cast<int>(x);
    return true;
}

} // namespace

const char* WheelieModeName(WheelieMode m) {
    return m == WheelieMode::kAlways ? "on" : (m == WheelieMode::kOff ? "off" : "modern");
}

bool WheelieSettings::Parse(const std::string& key, const std::string& value) {
    if (key == "wheelie") {
        if (value == "modern") mode = WheelieMode::kModern;
        else if (value == "on" || value == "always") mode = WheelieMode::kAlways;
        else if (value == "off") mode = WheelieMode::kOff;
        else return false;
        return true;
    }
    if (key == "wheelie_angle") return ParseInt(value, 20, 50, holdDeg);
    if (key == "wheelie_loop") return ParseOnOff(value, loopOver);
    if (key == "wheelie_cars") return ParseOnOff(value, overCars);
    if (key == "wheelie_view_pitch") return ParseInt(value, 0, 100, viewPitchPct);
    if (key == "wheelie_hold_ms") return ParseInt(value, 0, 1000, holdMs);
    if (key == "wheelie_lift_cm") return ParseInt(value, 5, 40, liftCm);
    if (key == "wheelie_full_cm") return ParseInt(value, 10, 60, liftFullCm);
    return false;
}

std::string WheelieSettings::Serialize() const {
    std::ostringstream o;
    o << "wheelie=" << WheelieModeName(mode) << "\n"
      << "wheelie_angle=" << holdDeg << "\n"
      << "wheelie_loop=" << (loopOver ? "on" : "off") << "\n"
      << "wheelie_cars=" << (overCars ? "on" : "off") << "\n"
      << "wheelie_view_pitch=" << viewPitchPct << "\n"
      << "wheelie_hold_ms=" << holdMs << "\n"
      << "wheelie_lift_cm=" << liftCm << "\n"
      << "wheelie_full_cm=" << liftFullCm << "\n";
    return o.str();
}

std::string WheelieSettings::Describe() const {
    char b[320];
    std::snprintf(b, sizeof(b),
                  "wheelie settings: %s, hold %d deg, loop-over %s, over cars %s, VR view pitch %d%%, the start held %d ms, "
                  "VR hands lifted %d cm to start / %.0f cm full",
                  mode == WheelieMode::kModern ? "with Modern handling" : (mode == WheelieMode::kAlways ? "always" : "off"),
                  holdDeg, loopOver ? "on" : "off", overCars ? "on" : "off", viewPitchPct, holdMs, liftCm,
                  double(LiftFull() * 100.0f));
    return b;
}

// ---------------------------------------------------------------- the script
bool WheelieScript::Parse(const std::string& text) {
    steps.clear();
    std::istringstream in(text);
    std::string item;
    while (std::getline(in, item, ';')) {
        std::istringstream one(item);
        long f = -1;
        std::string v;
        if (!(one >> f)) {
            if (item.find_first_not_of(" \t") == std::string::npos) continue;
            return false;
        }
        if (!(one >> v) || f < 0) return false;
        Step s{f, 0.0f, 'k'};
        if (v == "W") s.pull = 1.0f;
        else if (v == "N") s.throttle = 'N';
        else if (v == "B") s.throttle = 'B';
        else {
            char* end = nullptr;
            const float x = std::strtof(v.c_str(), &end);
            if (end == v.c_str() || *end != '\0' || x < 0.0f || x > 1.0f) return false;
            s.pull = x;
        }
        steps.push_back(s);
    }
    std::stable_sort(steps.begin(), steps.end(), [](const Step& a, const Step& b) { return a.frame < b.frame; });
    return true;
}

const WheelieScript::Step* WheelieScript::At(long frame) const {
    const Step* s = nullptr;
    for (const Step& x : steps)
        if (x.frame <= frame) s = &x;
    return s;
}

// ---------------------------------------------------------------- the layer
Wheelie::~Wheelie() {
    if (csv_ != nullptr) std::fclose(csv_);
}

Wheelie& PlayerWheelie() {
    static Wheelie w;
    return w;
}

void Wheelie::BeginRace() {
    on_ = engaged_ = false;
    gate_ = wanting_ = false;
    armed_ = 0.0f;
    gateFrame_ = -1;
    starts_ = wantFrames_ = shortWants_ = 0;
    pull_ = throttle_ = brake_ = lean_ = 0.0f;
    pitch_ = rate_ = target_ = creep_ = 0.0f;
    ground_ = seated_ = looped_ = flying_ = wasSeated_ = false;

    flightFrames_ = 0;
    flightCar_ = 0;
    m_ = Metrics{};
    riseClock_ = dropClock_ = -1;
    lastOrigPitch_ = 0;
    if (csv_ != nullptr) {
        std::fclose(csv_);
        csv_ = nullptr;
    }
}

void Wheelie::BeforeHandling(const WheelieIn& in, PadState& pad) {
    if (LayerOff()) return;
    frame_ = in.frame;
    bike_ = in.bike;
    vr_ = in.vrRun;
    const WheelieSettings& ws = Handling().wheelie;
    on_ = !in.twoPlayers && in.ram != nullptr && InRam(in.bike) && ws.On(in.modern);
    // the scripted input (a test input: it runs whatever the option, so its control sees the same keys)
    float scripted = 0.0f;
    if (const WheelieScript::Step* s = script.At(in.frame)) {
        scripted = s->pull;
        if (s->throttle == 'N' || s->throttle == 'B') {
            pad.throttle = false;
            pad.brake = s->throttle == 'B';
        }
    }
    // --wheelie-aim-car: a TEST input - the nearest live car ahead within 80 units, steered at by the digital keys
    if (aimCar && in.frame >= aimFrom && in.ram != nullptr && InRam(in.bike) && !in.paused) {
        float along = 0.0f, bestSide = 0.0f;
        uint32_t car = 0;
        if (NearestCar(in.ram, in.bike, 80.0f, along, bestSide, car) && along > 1.0f) {
            pad.right = bestSide > 0.4f; // (the model's x is its right: heading +z, a car at +x is on the right)
            pad.left = bestSide < -0.4f;
        }
    }
    if (!on_) {
        gate_ = wanting_ = false;
        armed_ = 0.0f;
        return;
    }
    float thr = pad.throttle ? 1.0f : 0.0f, brk = pad.brake ? 1.0f : 0.0f;
    if (pad.device.analog) { // the VR bars' twist / the analogue right stick: up = drive, down = brake
        const int ry = pad.device.ry;
        if (ry < 0x80) thr = std::max(thr, static_cast<float>(0x80 - ry) / 128.0f);
        else if (ry > 0x80) brk = std::max(brk, static_cast<float>(ry - 0x80) / 127.0f);
    }
    throttle_ = thr;
    brake_ = brk;
    // The lean-back request. `want`: this frame asks for a wheelie; `keep`: how far back while one is under way (0 lets
    // go). Not while an attack uses Down as its modifier.
    //   * the script (a test input): its value, both;
    //   * the Down action (F, the d-pad) - a digital full lean back, unless the stick pressed it (below);
    //   * a stick: asks only near full travel back (kStickStart), then leans back from half travel to full;
    //   * the VR Handlebars mode: only the bars' gesture (vr_wheelie.h).
    const bool attack = pad.r1 || pad.l1 || pad.r2;
    bool want = false;
    float keep = 0.0f;
    if (scripted > 0.0f) {
        want = true;
        keep = scripted;
    }
    const float stick = std::clamp(in.stickBack, 0.0f, 1.0f);
    if (!in.vrHandlebars && !attack) {
        const bool stickPressedDown = stick >= kStickDownBinding; // the bindings' LsDown: a third of the travel
        if (pad.padDown && !stickPressedDown) {
            want = true;
            keep = 1.0f;
        }
        if (stick >= kStickStart) want = true;
        keep = std::max(keep, std::clamp((stick - kStickHold) / (kStickStart - kStickHold), 0.0f, 1.0f));
    }
    if (in.barsKeep >= 0.0f) {
        want = want || in.barsWant;
        keep = std::max(keep, std::clamp(in.barsKeep, 0.0f, 1.0f));
    }
    // The deliberate start: asked for holdMs with the throttle open (over half) and the bike riding over kStartSpeed,
    // seated and on the ground; then held until let go - the request under its release point or the throttle shut.
    const float dt = gateFrame_ >= 0 && in.frame > gateFrame_ ? std::min(0.1f, static_cast<float>(in.frame - gateFrame_) / 60.0f) : 0.0f;
    gateFrame_ = in.frame;
    const float speed = Speed(in.ram, in.bike);
    const bool ready = !in.paused && thr >= 0.5f && brk < 0.3f && speed >= kStartSpeed && Seated(in.ram, in.bike) &&
                       Grounded(in.ram, in.bike);
    if (want && !in.paused) ++wantFrames_;
    want_ = want;
    barsLow_ = in.barsLow;
    barsMean_ = in.barsMean;
    stick_ = stick;
    if (!gate_) {
        if (want && ready) {
            armed_ += dt;
            wanting_ = true;
        } else {
            if (wanting_ && armed_ > 0.0f) ++shortWants_;
            wanting_ = false;
            armed_ = 0.0f;
        }
        if (want && ready && armed_ * 1000.0f + 0.5f >= static_cast<float>(ws.holdMs)) {
            gate_ = true;
            wanting_ = false;
            ++starts_;
            const char* by = scripted > 0.0f                        ? "the script"
                             : in.barsKeep >= 0.0f && in.barsWant ? "the VR bars' gesture"
                             : stick >= kStickStart                ? "the stick held back"
                                                                   : "the Down action";
            std::printf("wheelie: frame %ld - the lean back held %.0f ms with the throttle at %.2f and %.1f units/s: a "
                        "wheelie starts (%s)\n",
                        in.frame, double(armed_ * 1000.0f), double(thr), double(speed), by);
        }
    } else if (keep <= 0.0f || thr < 0.2f || in.paused) {
        gate_ = false;
        armed_ = 0.0f;
    }
    pull_ = gate_ && !in.paused ? keep : 0.0f;
    if (LegacyInput()) {
        // DEVELOPMENT RRJB_WHEELIE_INPUT=legacy: the lean back as it was read before the deliberate start - no gate, the
        // Down action (the stick past a third through the bindings too) a full lean back, the stick from 24 of 127, the
        // bars' earlier gesture (4 cm from the grab in the seat's space, vr_wheelie.h); the control of the gates
        float legacy = std::max(scripted, pad.padDown && !attack ? 1.0f : 0.0f);
        if (!attack) legacy = std::max(legacy, std::clamp((stick * 127.0f - 24.0f) / 90.0f, 0.0f, 1.0f));
        if (in.barsLegacy >= 0.0f) legacy = std::max(legacy, std::clamp(in.barsLegacy, 0.0f, 1.0f));
        pull_ = in.paused ? 0.0f : legacy;
    }
    if (pull_ > 0.0f && !engaged_) {
        engaged_ = true;
        std::printf("wheelie: frame %ld - the first lean back (%s; %s)\n", in.frame, Handling().wheelie.Describe().c_str(),
                    in.barsKeep >= 0.0f ? "the VR bars' gesture" : (in.vrRun ? "VR" : "desktop"));
    }
}

void Wheelie::AfterHandling(PadState& pad) {
    if (LayerOff() || !on_) return;
    // SA: the front wheel in the air steers only through the rider's weight (fWheelieSteer) - half as strong here, in
    // proportion to the pitch, on the analogue pad Modern / the bars hand the original
    if (!pad.device.analog || pitch_ <= 0.0f) return;
    const float hold = static_cast<float>(Handling().wheelie.holdDeg) * kDeg;
    const float f = 1.0f - 0.5f * std::clamp(pitch_ / hold, 0.0f, 1.0f);
    const int lx = 0x80 + static_cast<int>(std::lround(static_cast<float>(static_cast<int>(pad.device.lx) - 0x80) * f));
    pad.device.lx = static_cast<uint8_t>(std::clamp(lx, 0, 255));
}

void Wheelie::Crash(uint8_t* ram, uint32_t bike) {
    // The game's own fall, as its loop-out trigger (region C, RASHCDG 0x800762D8..0x800763FC) writes it: flagsC bit 22
    // := the road direction's sign (+0x16C), +0x2CC = 0, flagsC |= 0x800 | 0x20 (0x20: the wipeout kind BikeWipeoutStart
    // 0x800723FC takes second - bits 6, 5, 7, 8), +0x240 = max(+0x240, 1.0). Next frame A3 migrates the bike to the crash.
    uint32_t fc = U32(ram, bike + 0x238u);
    fc = (fc & ~0x400000u) | (S32(ram, bike + 0x16Cu) < 0 ? 0x400000u : 0u);
    W32(ram, bike + 0x2CCu, 0);
    W32(ram, bike + 0x238u, fc | 0x820u);
    if (S32(ram, bike + 0x240u) < 0x10000) W32(ram, bike + 0x240u, 0x10000u);
}

void Wheelie::AfterFrame(uint8_t* ram, uint32_t bike, int32_t dt, double routeDistance, long frame) {
    if (LayerOff() || ram == nullptr || !InRam(bike)) return;
    const bool logging = !csvPath.empty();
    if (!on_ && !engaged_ && !logging) return; // off: nothing (the pad and the guest untouched, nothing printed)
    event_.clear();
    const float h = std::clamp(static_cast<float>(dt) / 65536.0f, 0.0f, 0.1f);
    const WheelieSettings& ws = Handling().wheelie;
    const float hold = static_cast<float>(ws.holdDeg) * kDeg;
    const float balance = hold + kBalanceOver * kDeg;
    seated_ = Seated(ram, bike);
    ground_ = Grounded(ram, bike);
    if (wasSeated_ && !seated_) {
        ++m_.falls;
        if (m_.firstFall < 0) m_.firstFall = frame;
        event_ = "the rider OFF the bike";
    }
    wasSeated_ = seated_;
    const float speed = Speed(ram, bike);
    const uint32_t fc = U32(ram, bike + 0x238u);
    // the original's own pitch move (+0x268), for the report (what the original has: wheelie.h's top)
    {
        // (only riding: in a crash the same word holds the tumble's angle, set by BikeCrashLaunch)
        const int32_t p = seated_ && ground_ ? S32(ram, bike + 0x268u) : 0;
        if (p > 0 && lastOrigPitch_ <= 0) ++m_.origPops;
        m_.origMaxPitch = std::max(m_.origMaxPitch, static_cast<double>(p) / 65536.0 * 180.0 / kPi);
        lastOrigPitch_ = p;
        origPitch_ = static_cast<float>(p) / 65536.0f; // (16.16 radians, drawn by the game itself in +0x1B0)
    }
    // the flight after a launch over a car
    if (flying_) {
        ++flightFrames_;
        const double y = -static_cast<double>(S32(ram, bike + 0xBCu)) / 65536.0; // the box centre's height (Y down)
        if (flightFrames_ == 1) flightBase_ = y;
        m_.maxFlightHeight = std::max(m_.maxFlightHeight, y - flightBase_);
        if ((fc & 0x600u) == 0u && flightFrames_ > 2) { // the game's TouchDown cleared the flight
            flying_ = false;
            m_.landFrame = frame;
            if (seated_) ++m_.landedSeated;
            else ++m_.landedFallen;
            char b[160];
            std::snprintf(b, sizeof(b), "landed after %ld frame(s), %s, speed %.1f", flightFrames_,
                          seated_ ? "the rider seated - rides on" : "the rider OFF", double(speed));
            event_ = b;
            std::printf("wheelie: frame %ld - %s\n", frame, b);
        } else if (flightFrames_ > kFlightFrames) {
            flying_ = false;
        }
    }
    if (on_) {
        lean_ += (pull_ - lean_) * (1.0f - std::exp(-h / kLeanTau));
        const bool canRide = seated_ && ground_ && !flying_;
        if (!canRide) {
            // off the bike, crashing or in the air: the drawn pitch goes (the crash / the flight draw their own)
            const float tau = flying_ ? 0.6f : 0.12f;
            pitch_ *= std::exp(-h / tau);
            rate_ = 0.0f;
            creep_ = 0.0f;
            if (pitch_ < 0.2f * kDeg) pitch_ = 0.0f;
            if (!seated_ && looped_) { // the loop-over's fall: the rider is off (counted once)
                ++m_.loopFalls;
                looped_ = false;
            }
        } else {
            // the target: lean back x throttle x the hold angle (SA's lean-back force x (0.5 gas + 0.5), stabilised
            // about fWheelieAng); no throttle, a brake or a crawl bring the front down
            float t = lean_ * std::clamp(throttle_ / 0.7f, 0.0f, 1.0f) * hold;
            const bool starting = pitch_ <= 0.0f;
            // a hard hit (the hit class 4 / 8 / 16 / 32) slams the front down like a brake
            const bool slam = HardHit(ram, bike);
            if (speed < kMinSpeed || brake_ > 0.3f || slam || (starting && speed < kLiftSpeed)) t = 0.0f;
            // held at full pull and full throttle near the hold angle, the nose keeps rising (overdone); only with the
            // loop-over on - off, the balance point is never reached
            if (ws.loopOver && lean_ > 0.9f && throttle_ > 0.9f && pitch_ > hold - 3.0f * kDeg && t > 0.0f)
                creep_ += kCreepDegPerS * kDeg * h;
            else
                creep_ = std::max(0.0f, creep_ - 30.0f * kDeg * h);
            t += creep_;
            if (!ws.loopOver) t = std::min(t, balance - 5.0f * kDeg);
            target_ = t;
            // the timings: the first lift (the lean back past half from the ground to 30 deg), the first drop (the
            // target gone with the pitch over 20 deg, to the front wheel on the ground)
            if (m_.riseMs < 0) {
                if (riseClock_ < 0 && pitch_ < 0.5f * kDeg && pull_ >= 0.5f && t > 0.0f) riseClock_ = 0;

                else if (riseClock_ >= 0) riseClock_ += h;
                if (riseClock_ >= 0 && pitch_ >= 30.0f * kDeg) m_.riseMs = riseClock_ * 1000.0;
            }
            if (m_.dropMs < 0) {
                if (dropClock_ < 0 && t <= 0.0f && pitch_ > 20.0f * kDeg) dropClock_ = 0;
                else if (dropClock_ >= 0) dropClock_ += h;
            }
            const float w0 = 2.0f * kPi * kSpringHz * (brake_ > 0.3f || slam ? 1.6f : 1.0f);

            float acc = w0 * w0 * (t - pitch_) - 2.0f * kDamping * w0 * rate_;
            if (pitch_ > balance) { // past the balance point: gravity pulls it over, the spring cannot hold it
                acc = 25.0f * (pitch_ - balance) + (brake_ > 0.3f ? -60.0f : 0.0f) - 0.5f * rate_;
            }
            rate_ += acc * h;
            pitch_ += rate_ * h;
            if (!ws.loopOver) pitch_ = std::min(pitch_, balance - 2.0f * kDeg);
            if (pitch_ <= 0.0f && dropClock_ >= 0 && m_.dropMs < 0) m_.dropMs = dropClock_ * 1000.0;
            if (pitch_ <= 0.0f) {
                if (m_.current > 0) {
                    ++m_.drops;
                    char b[96];
                    std::snprintf(b, sizeof(b), "the front wheel down after %.2f s", double(m_.current) * double(h));
                    event_ = b;
                }
                pitch_ = 0.0f;
                rate_ = std::max(0.0f, rate_);
                m_.current = 0;
            }
            if (ws.loopOver && pitch_ >= kLoopDeg * kDeg && !looped_) {
                looped_ = true;
                ++m_.loops;
                Crash(ram, bike);
                event_ = "LOOPED OVER - the game's fall";
                std::printf("wheelie: frame %ld - looped over at %.1f deg (full pull, full throttle past the balance point "
                            "%.0f deg): the game's own fall (flagsC |= 0x820)\n",
                            frame, double(pitch_ / kDeg), double(balance / kDeg));
                pitch_ = kLoopDeg * kDeg;
                rate_ = 0.0f;
            }
        }
        // the metrics
        ++m_.frames;
        if (pitch_ > 0.0f) {
            if (m_.current == 0 && pitch_ > 0.5f * kDeg) {
                ++m_.lifts;
                if (m_.firstLiftFrame < 0) m_.firstLiftFrame = frame;
            }
            if (pitch_ > 0.5f * kDeg || m_.current > 0) ++m_.current;
            m_.longest = std::max(m_.longest, m_.current);
        }
        if (pitch_ > 10.0f * kDeg) {
            ++m_.up;
            m_.holdTime += h;
            m_.sumPitch += pitch_ / kDeg;
        }
        m_.maxPitch = std::max(m_.maxPitch, static_cast<double>(pitch_ / kDeg));
    }
    if (m_.firstDistance < 0) m_.firstDistance = routeDistance;
    m_.distance = routeDistance - m_.firstDistance;
    if (logging) {
        if (csv_ == nullptr) {
            csv_ = std::fopen(csvPath.c_str(), "w");
            if (csv_ != nullptr)
                std::fprintf(csv_, "frame,on,speed,pull,throttle,brake,lean,target_deg,pitch_deg,rate_dps,seated,ground,"
                                   "flagsA,flagsB,flagsC,orig_pitch_deg,orig_rate,load_260,flying,height,distance,car_along,"
                                   "car_side,want,gate,bars_low,bars_mean,stick,event\n");
        }
        float ca = 999.0f, cs = 0.0f;
        uint32_t cw = 0;
        NearestCar(ram, bike, 120.0f, ca, cs, cw);
        if (csv_ != nullptr)
            std::fprintf(csv_, "%ld,%d,%.3f,%.3f,%.3f,%.3f,%.4f,%.3f,%.3f,%.2f,%d,%d,0x%08X,0x%08X,0x%08X,%.3f,%.3f,%.4f,%d,%.3f,%.2f,%.2f,%.2f,%d,%d,%.3f,%.3f,%.2f,%s\n",
                         frame, on_ ? 1 : 0, double(speed), double(pull_), double(throttle_), double(brake_), double(lean_),
                         double(target_ / kDeg), double(pitch_ / kDeg), double(rate_ / kDeg), seated_ ? 1 : 0,
                         ground_ ? 1 : 0, U32(ram, bike + 0x230u), U32(ram, bike + 0x234u), fc,
                         double(S32(ram, bike + 0x268u)) / 65536.0 * 180.0 / double(kPi),
                         double(S32(ram, bike + 0x26Cu)) / 65536.0 * 180.0 / double(kPi),
                         double(S32(ram, bike + 0x260u)) / 65536.0, flying_ ? 1 : 0,
                         -double(S32(ram, bike + 0xBCu)) / 65536.0, m_.distance, double(ca), double(cs), want_ ? 1 : 0, gate_ ? 1 : 0,
                         double(barsLow_), double(barsMean_), double(stick_), event_.c_str());

    }
}

bool Wheelie::DrawnPitch(float& radians) const {
    if (LayerOff() || !on_ || !(pitch_ > 0.0f)) return false;
    // the original's own pop (+0x268, already in the rows +0x1B0 the renderer draws) is not drawn twice: ours adds
    // only what it holds above it
    const float extra = pitch_ - std::max(0.0f, origPitch_);
    if (!(extra > 0.0f)) return false;
    radians = extra;
    return true;
}

bool Wheelie::HeldPitch(float& radians) const {
    if (LayerOff() || !on_ || !(pitch_ > 0.0f)) return false;
    radians = pitch_;
    return true;
}

float Wheelie::ViewPitchKeep() const {
    return static_cast<float>(std::clamp(Handling().wheelie.viewPitchPct, 0, 100)) / 100.0f;
}

bool Wheelie::CarHookArmed(uint32_t bike) const {
    if (LayerOff() || !on_ || bike != bike_ || !Handling().wheelie.overCars) return false;
    if (flying_) return true; // the launch's flight: contacts with cars are undone (the bike passes over)
    // in a wheelie: ours, or the original's own pop (a double-tap of the throttle, wheelie.h's top)
    return seated_ && ground_ && std::max(pitch_, origPitch_) >= kArmedDeg * kDeg;

}

void Wheelie::NoteContact(uint32_t car, bool launched, bool launchOk, long) {
    ++m_.contactsJudged;
    if (!launched) {
        ++m_.flightContactsUndone;
        return;
    }
    if (!launchOk) {
        ++m_.launchFails;
        return;
    }
    ++m_.launches;
    if (m_.firstLaunchFrame < 0) m_.firstLaunchFrame = frame_ + 1;
    flying_ = true;
    flightFrames_ = 0;
    flightCar_ = car;
    std::printf("wheelie: frame %ld - a car hit at %.1f deg of wheelie: the game's launch over it (0x80084BE8, car "
                "0x%08X) instead of the original's reaction\n",
                frame_ + 1, double(pitch_ / kDeg), car);
}

std::string Wheelie::Summary() const {
    char b[1400];
    std::snprintf(b, sizeof(b),
                  "wheelie summary: %s - %ld frame(s) judged, lifts %ld (first at frame %ld), front down %ld, up "
                  "(over 10 deg) %.2f s, longest wheelie %ld frame(s), max pitch %.1f deg, mean pitch while up %.1f deg; "
                  "loop-overs %ld (the rider off after %ld); car contacts judged %ld: launches over a car %ld (first at "
                  "frame %ld, failed %ld), flight contacts undone %ld, landings seated %ld / fallen %ld (frame %ld), "
                  "flight height up to %.2f; distance %.1f; the original's own pitch moves (+0x268) %ld, up to %.1f deg; "
                  "rider falls %ld (first at frame %ld); the first lift to 30 deg in %.0f ms, the first drop from 20+ deg "
                  "in %.0f ms",
                  on_ ? Handling().wheelie.Describe().c_str() : "off this race", m_.frames, m_.lifts, m_.firstLiftFrame,
                  m_.drops, m_.holdTime, m_.longest, m_.maxPitch, m_.up > 0 ? m_.sumPitch / double(m_.up) : 0.0, m_.loops,
                  m_.loopFalls, m_.contactsJudged, m_.launches, m_.firstLaunchFrame, m_.launchFails,
                  m_.flightContactsUndone, m_.landedSeated, m_.landedFallen, m_.landFrame, m_.maxFlightHeight, m_.distance,
                  m_.origPops, m_.origMaxPitch, m_.falls, m_.firstFall, m_.riseMs, m_.dropMs);
    char c[240];
    std::snprintf(c, sizeof(c), "; the lean back asked on %ld frame(s), wheelie starts %ld (held %d ms), asks let go before it %ld",
                  wantFrames_, starts_, Handling().wheelie.holdMs, shortWants_);
    return std::string(b) + c;

}

// ---------------------------------------------------------------- the car hook
bool WheelieServeBikeVsTraffic(rr::sim::GuestRam& g, rr::sim::CollisionCallees& c, const rr::sim::BikeTables& t,
                               const uint32_t* a, int n, uint32_t sp, uint32_t& v0, bool& ok) {
    Wheelie& w = PlayerWheelie();
    if (n < 2 || !w.CarHookArmed(a[0])) return false;
    const uint32_t bike = a[0], car = a[1];
    // The guest before the original's call: its whole RAM (the port writes only guest memory - the bike, the car, the
    // contact list 0x800CCE48 / 0x800CCF68, its own frame below sp)
    static std::vector<uint8_t> before;
    before.resize(rr::sim::GuestRam::kRamSize);
    g.ReadBlock(0x80000000u, before.data(), rr::sim::GuestRam::kRamSize);
    // BikeVsTraffic's contact code, its frame word sp - 96 + 56 (bike_react.cpp CODE): zeroed first, since an early
    // return leaves it unwritten
    const uint32_t code = sp - 96u + 56u;
    g.W32(code, 0);
    const rr::sim::CollCall call{0x800AC5BCu, a, n, sp};
    if (!rr::sim::ServeCollNative(g, call, t, c, v0, ok)) return false; // (not served: never, the caller checked)
    if (!ok || g.Faulted() || g.U32(code) == 0u) return true;          // no contact: the original's call stands
    // A contact. The ORIGINAL's reaction (the push, the severity and the rider's stance, the deferred contact) is undone
    // and, the first time, the game's launch runs instead: the bike thrown up at the angle and lift it gives a thing off
    // a car. +0x340 = the car's shape (Launch reads the partner's pool from it: pool 3, a car); +0x300 = the box's lift
    // over the contact point along +0x20A as it is now (Launch re-derives +0xB8 = +0x1F8 + +0x20A x +0x300).
    g.WriteBlock(0x80000000u, before.data(), rr::sim::GuestRam::kRamSize);
    const bool inFlight = (g.U32(bike + 0x238u) & 0x600u) != 0u;
    if (inFlight) {
        w.NoteContact(car, false, false, 0);
        v0 = 0;
        ok = true;
        return true;
    }
    int32_t lift = 0;
    {
        int64_t d = 0;
        for (uint32_t k = 0; k < 3; ++k)
            d += static_cast<int64_t>(g.S32(bike + 0xB8u + 4u * k) - g.S32(bike + 0x1F8u + 4u * k)) * g.S16(bike + 0x20Au + 2u * k);
        lift = static_cast<int32_t>(d / 4096);
    }
    g.W32(bike + 0x340u, car + 0xACu);
    g.W32(bike + 0x300u, static_cast<uint32_t>(lift));
    // +0x2C4: the depth AirContact 0x800B1978 lets an AIRBORNE bike's box corner reach behind the ground plane before it
    // lands (0x800B1A48: flagsC 0x400 and depth <= +0x2C4 - no landing). The kind-0 tail runs AirContact right after
    // this partner, with the wheels still on the road: without a margin the launch would land in the same pass. A
    // quarter unit: the corners sink that far on the way down before TouchDown, whose push-out puts them back.
    g.W32(bike + 0x2C4u, 0x4000u);

    const uint32_t args[2] = {bike, 1u};
    uint32_t lv0 = 0;
    bool lok = true;
    const bool served = rr::sim::ServeCollNative(g, rr::sim::CollCall{0x80084BE8u, args, 2, sp}, t, c, lv0, lok);
    const bool launched = served && lok && !g.Faulted() && (g.U32(bike + 0x238u) & 0xC00u) == 0xC00u;
    if (!launched) { // (the launch is a seam under RRJB_PARTNERS=off): the original's call as it ran
        g.ClearFault();
        g.WriteBlock(0x80000000u, before.data(), rr::sim::GuestRam::kRamSize);
        (void)rr::sim::ServeCollNative(g, call, t, c, v0, ok);
    } else {
        // The launch's direction is the game's (LaunchLift 0x8007E868: 170/4096 of a turn above the heading's own
        // slope, by the horizon); on a steep climb that leaves the new heading at or under the road's plane and the
        // next pass lands it at once (measured: 1/20 on a 22 deg climb, the flight 1 frame, 10 % of the speed lost to
        // each TouchDown). So it is held at least 15 deg above the road's plane (the contact normal -(+0x20A), as
        // AirContact takes it): the heading +0x1C2 the thrown bike's velocity is built from (BikeCrashLaunch).
        float h[3], nrm[3];
        for (uint32_t k = 0; k < 3; ++k) {
            h[k] = static_cast<float>(g.S16(bike + 0x1C2u + 2u * k)) / 4096.0f;
            nrm[k] = -static_cast<float>(g.S16(bike + 0x20Au + 2u * k)) / 4096.0f;
        }
        const float nl = std::sqrt(nrm[0] * nrm[0] + nrm[1] * nrm[1] + nrm[2] * nrm[2]);
        if (nl > 0.5f) {
            for (float& x : nrm) x /= nl;
            const float up = h[0] * nrm[0] + h[1] * nrm[1] + h[2] * nrm[2];
            float p[3] = {h[0] - up * nrm[0], h[1] - up * nrm[1], h[2] - up * nrm[2]};
            const float pl = std::sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
            const float minSin = std::sin(15.0f * kDeg);
            if (pl > 0.1f && up < minSin) {
                const float cs = std::cos(15.0f * kDeg);
                for (uint32_t k = 0; k < 3; ++k)
                    g.W16(bike + 0x1C2u + 2u * k,
                          static_cast<uint16_t>(static_cast<int16_t>(std::lround((p[k] / pl * cs + nrm[k] * minSin) * 4096.0f))));
            }
        }
        v0 = 0;
        ok = true;
    }
    w.NoteContact(car, true, launched, 0);

    return true;
}

} // namespace rr::game
