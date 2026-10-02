// The player's handling layer (handling_modern.h). OURS.
#include "game/handling_modern.h"

#include "game/sim/input.h" // AxisCurve 0x8001CA58 (PORTED): the lx byte -> axis table

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <sstream>

namespace rr::game {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr float kGravity = 9.81f;          // m/s^2
constexpr float kMetresPerUnit = 1.024f;   // head_camera.h: one world unit = 1.024 m
// GTA SA's per-step factors at its 50 Hz timestep, as continuous time constants: x += (t - x) * k per 0.02 s step
// is tau = -0.02 / ln(1 - k); SA's fDesLean f = 0.93^ts: tau = -0.02 / ln(0.93).
constexpr float kSaStep = 0.02f;
const float kSteerTau = -kSaStep / std::log(1.0f - 0.2f);  // CBike::ProcessControlInputs, m_fSteerInput (0.0896 s)
const float kBarsTau = -kSaStep / std::log(1.0f - 0.35f);  // the VR handlebars' lag (reVC Bike.cpp 1866; 0.0464 s)
const float kLeanTau = -kSaStep / std::log(0.93f);         // fDesLean 0.93 (PCJ-600 / FCR-900; 0.2756 s)
constexpr float kTurnTau = 0.06f; // the turn layer: SA's yaw follows the front wheel's angle within ~0.2..0.3 s (tyre forces)
constexpr float kTurnMaxLean = 0.7853982f; // the turn layer's own lean state: SA's fMaxLean (45 deg) and fDesLean lag
constexpr double kGravityUnits = 0x9D087 / 65536.0; // the game's g, 9.8135 units / s^2
constexpr double kUnwindTau = 0.5; // the turn layer's spring straightening (toward a smaller turn or the
                                   // other way): half the time constant - SA's bike stands up and counter-steers briskly
// The arena's guest words (pad_reader.h, bike.h).
constexpr uint32_t kSteerCurve = 0x800D3978u;  // the steering axis' six halfwords (ENV.EN +0x98)
constexpr uint32_t kEnvCfg = 0x800D38E0u;       // ENV.EN (the dead zone +0xB0, the segment width +0xB2)
constexpr uint32_t kAxes = 0x800CE540u;         // {axis0, axis1} per player
constexpr float kStickDead = 0.12f;             // a resting controller stick
constexpr int32_t kModernMinSpeed = 3 << 16;    // 3 units/s (~11 km/h), 16.16: below it the original's digital pad

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
inline uint16_t U16(const uint8_t* ram, uint32_t a) { return static_cast<uint16_t>(S16(ram, a)); }

// DEVELOPMENT RRJB_HANDLING_LAYER=off: the layer is not called at all (every entry returns at once) - the control that
// Original is the game without the layer (tests\run_gates.ps1)
bool LayerOff() {
    static const bool off = [] {
        const char* v = std::getenv("RRJB_HANDLING_LAYER");
        return v != nullptr && std::strcmp(v, "off") == 0;
    }();
    return off;
}

bool Seated(const uint8_t* ram, uint32_t bike) {
    if (ram == nullptr || !InRam(bike)) return false;
    const uint32_t rider = U32(ram, bike + 0x354u);
    if (!InRam(rider)) return false;
    return S16(ram, rider + 0x140u) != 0 && U32(ram, rider + 0x25Cu) < 2u;
}

float Smooth(float x, float target, float dt, float tau) {
    if (!(tau > 1e-6f)) return target;
    return x + (target - x) * (1.0f - std::exp(-dt / tau));
}

// The bike's roll as vr_visual_lean.h measures it: the up (model -y, row 1 of +0x1B0 negated) against the level plane
// through the forward (row 2), signed about the forward; and its yaw, atan2(fwd.x, fwd.z) (+ = a right turn: x is the
// bike's right when it faces +z in these Y-down axes). False when the rows are not a frame.
bool BikeAngles(const uint8_t* ram, uint32_t bike, float& roll, float& yaw) {
    float fwd[3], up[3];
    for (uint32_t k = 0; k < 3; ++k) {
        fwd[k] = static_cast<float>(S16(ram, bike + 0x1B0u + 12u + 2u * k)) / 4096.0f;
        up[k] = -static_cast<float>(S16(ram, bike + 0x1B0u + 6u + 2u * k)) / 4096.0f;
    }
    const auto len = [](const float a[3]) { return std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]); };
    const float lf = len(fwd), lu = len(up);
    if (!(lf > 0.5f) || !(lu > 0.5f)) return false;
    for (int k = 0; k < 3; ++k) {
        fwd[k] /= lf;
        up[k] /= lu;
    }
    float lvl[3] = {0.0f, -1.0f, 0.0f};
    const float wf = lvl[1] * fwd[1];
    for (int k = 0; k < 3; ++k) lvl[k] -= fwd[k] * wf;
    const float ll = len(lvl);
    if (!(ll > 1e-4f)) return false;
    for (float& c : lvl) c /= ll;
    const float cr[3] = {lvl[1] * up[2] - lvl[2] * up[1], lvl[2] * up[0] - lvl[0] * up[2], lvl[0] * up[1] - lvl[1] * up[0]};
    roll = std::atan2(cr[0] * fwd[0] + cr[1] * fwd[1] + cr[2] * fwd[2], lvl[0] * up[0] + lvl[1] * up[1] + lvl[2] * up[2]);
    yaw = std::atan2(fwd[0], fwd[2]);
    return true;
}

bool ParseInt(const std::string& v, int lo, int hi, int& out) {
    char* end = nullptr;
    const long n = std::strtol(v.c_str(), &end, 10);
    if (end == v.c_str() || *end != '\0' || n < lo || n > hi) return false;
    out = static_cast<int>(n);
    return true;
}

bool ParseMode(const std::string& v, HandlingMode& out) {
    if (v == "original" || v == "0") out = HandlingMode::kOriginal;
    else if (v == "modern" || v == "1") out = HandlingMode::kModern;
    else return false;
    return true;
}

} // namespace

const char* HandlingModeName(HandlingMode m) { return m == HandlingMode::kModern ? "modern" : "original"; }

// ---------------------------------------------------------------- settings
bool HandlingSettings::Parse(const std::string& key, const std::string& value) {
    if (key == "mode") return ParseMode(value, desktop);
    if (key == "vr_mode") return ParseMode(value, vr);
    if (key == "steer_lag") return ParseInt(value, 0, 300, steerLagPct);
    if (key == "curve") return ParseInt(value, 0, 200, curvePct);
    if (key == "turn_lag") return ParseInt(value, 0, 300, turnLagPct);
    if (key == "lean_lag") return ParseInt(value, 0, 300, leanLagPct);
    if (key == "max_lean") return ParseInt(value, 10, 60, maxLeanDeg);
    if (key == "lean_model") {
        if (value == "sa") leanFromTurn = true;
        else if (value == "game") leanFromTurn = false;
        else return false;
        return true;
    }
    if (key == "camera_roll") return ParseInt(value, 0, 100, cameraRollPct);
    return wheelie.Parse(key, value); // the wheelie keys (game/wheelie.h)
}

std::string HandlingSettings::Serialize() const {
    std::ostringstream o;
    o << "mode=" << HandlingModeName(desktop) << "\n"
      << "vr_mode=" << HandlingModeName(vr) << "\n"
      << "steer_lag=" << steerLagPct << "\n"
      << "curve=" << curvePct << "\n"
      << "turn_lag=" << turnLagPct << "\n"
      << "lean_lag=" << leanLagPct << "\n"
      << "max_lean=" << maxLeanDeg << "\n"
      << "lean_model=" << (leanFromTurn ? "sa" : "game") << "\n"
      << "camera_roll=" << cameraRollPct << "\n"
      << wheelie.Serialize(); // the wheelie keys (game/wheelie.h)
    return o.str();

}

std::string HandlingSettings::Describe() const {
    char b[320];
    std::snprintf(b, sizeof(b),
                  "handling settings: desktop %s, VR %s; modern: steer lag %d%%, curve %d%%, turn lag %d%%, lean lag "
                  "%d%%, max lean %d deg, lean model %s, VR camera roll %d%%",
                  HandlingModeName(desktop), HandlingModeName(vr), steerLagPct, curvePct, turnLagPct, leanLagPct,
                  maxLeanDeg, leanFromTurn ? "SA" : "game", cameraRollPct);
    return b;
}

HandlingSettings& Handling() {
    static HandlingSettings s;
    return s;
}

ModernHandling& PlayerHandling() {
    static ModernHandling h;
    return h;
}

// ---------------------------------------------------------------- the script
bool SteerScript::Parse(const std::string& text) {
    steps.clear();
    std::string item;
    std::istringstream in(text);
    while (std::getline(in, item, ';')) {
        std::istringstream one(item);
        long f = -1;
        std::string v;
        if (!(one >> f)) {
            if (item.find_first_not_of(" \t") == std::string::npos) continue;
            return false;
        }
        if (!(one >> v) || f < 0) return false;
        Step s{f, '0', 0.0f};
        if (v == "L" || v == "l") s.key = 'L';
        else if (v == "R" || v == "r") s.key = 'R';
        else if (v == "0") s.key = '0';
        else {
            char* end = nullptr;
            const float x = std::strtof(v.c_str(), &end);
            if (end == v.c_str() || *end != '\0' || x < -1.0f || x > 1.0f) return false;
            s.key = 's';
            s.stick = x;
        }
        steps.push_back(s);
    }
    std::stable_sort(steps.begin(), steps.end(), [](const Step& a, const Step& b) { return a.frame < b.frame; });
    return true;
}

const SteerScript::Step* SteerScript::At(long frame) const {
    const Step* cur = nullptr;
    for (const Step& s : steps) {
        if (s.frame > frame) break;
        cur = &s;
    }
    return cur;
}

ModernHandling::~ModernHandling() {
    if (csv_) std::fclose(csv_);
}

void ModernHandling::BeginRace() {
    tableReady_ = false;
    steer_ = 0.0f;
    lastDt_ = 1.0f / 60.0f;
    shapedThisFrame_ = modernSeen_ = false;
    turnWarm_ = turnWritten_ = turnLeanWarm_ = false;
    turnLat_ = turnVel_ = 0.0;
    turnLean_ = 0.0f;
    turnTarget_ = 0.0;
    announced_ = false;
    lastMode_ = HandlingMode::kOriginal;
    modeFrames_[0] = modeFrames_[1] = 0;
    switches_ = 0;
    input_ = axis_ = 0.0f;
    lx_ = 0x80;
    leanWarm_ = leanValid_ = haveYaw_ = false;
    lean_ = lastYaw_ = 0.0f;
    m_ = Metrics{};
    rateN_ = rollN_ = 0;
    onsetSign_ = 0;
    onsetFrames_ = releaseFrames_ = 0;
    releaseTimed_ = false;
    lastInput_ = 0.0f;
}

// ---------------------------------------------------------------- the input
void ModernHandling::BuildTable(const uint8_t* ram) {
    uint16_t curve[6];
    for (uint32_t k = 0; k < 6; ++k) curve[k] = U16(ram, kSteerCurve + 2u * k);
    uint8_t cfg[180];
    for (uint32_t k = 0; k < sizeof(cfg); ++k) cfg[k] = ram[(kEnvCfg + k) & 0x1FFFFFu];
    for (int b = 0; b < 256; ++b) axisOfByte_[b] = rr::sim::AxisCurve(static_cast<uint8_t>(b), curve, cfg);
    tableReady_ = true;
    int first = 0;
    for (int b = 128; b < 256; ++b)
        if (axisOfByte_[b] != 0) {
            first = b;
            break;
        }
    std::printf("handling: the game's steering curve (ENV.EN, AxisCurve 0x8001CA58): dead zone %u, segment %u, points "
                "%u %u %u %u %u %u; the first live byte right 0x%02X = %.4f\n",
                unsigned(U16(ram, kEnvCfg + 0xB0u)), unsigned(U16(ram, kEnvCfg + 0xB2u)), unsigned(curve[0]),
                unsigned(curve[1]), unsigned(curve[2]), unsigned(curve[3]), unsigned(curve[4]), unsigned(curve[5]), first,
                first ? axisOfByte_[first] / 65536.0 : 0.0);
    if (std::getenv("RRJB_HANDLING_STATS") != nullptr && InRam(U32(ram, statsBike_ + 0x22Cu))) { // DEVELOPMENT
        const uint32_t st = U32(ram, statsBike_ + 0x22Cu);
        std::printf("handling: stats");
        for (uint32_t o : {4u, 0xE0u, 0xE4u, 0xE8u, 0xF0u, 308u, 312u, 316u, 356u, 360u, 364u, 368u, 372u, 376u, 380u, 384u,
                           388u, 392u, 396u, 416u, 0x118u, 0x11Cu})
            std::printf(" +%u=%d", o, S32(ram, st + o));
        std::printf("\n");
    }
}

uint8_t ModernHandling::ByteFor(float axis) const {
    const double want = static_cast<double>(std::clamp(axis, -1.0f, 1.0f)) * 65536.0;
    int best = 0x80;
    double bestErr = 1e30;
    for (int b = 0; b < 256; ++b) {
        const double err = std::fabs(axisOfByte_[b] - want);
        // equal errors: the byte nearer the centre (the smallest deflection that gives it)
        if (err < bestErr || (err == bestErr && std::abs(b - 128) < std::abs(best - 128))) {
            bestErr = err;
            best = b;
        }
    }
    return static_cast<uint8_t>(best);
}

void ModernHandling::ApplyToPad(const HandlingPadIn& in, PadState& pad) {
    shapedThisFrame_ = false;
    if (LayerOff()) return;
    mode_ = Handling().ModeFor(in.vrRun);
    // the mode this race rides, in the log from its first frame, and every switch the menus make mid-race
    // (the settings are read each frame: a menu change applies on the next frame)
    if (!announced_) {
        std::printf("handling: frame %ld - this race rides %s (%s run; %s)\n", in.frame, HandlingModeName(mode_),
                    in.vrRun ? "a VR" : "a desktop", settingsSource.empty() ? "the settings in memory" : settingsSource.c_str());
        announced_ = true;
        lastMode_ = mode_;
    } else if (mode_ != lastMode_) {
        std::printf("handling: frame %ld - the %s handling switched %s -> %s (a menu)\n", in.frame, in.vrRun ? "VR" : "desktop",
                    HandlingModeName(lastMode_), HandlingModeName(mode_));
        ++switches_;
        lastMode_ = mode_;
    }
    ++modeFrames_[mode_ == HandlingMode::kModern ? 1 : 0];
    // the scripted steering (a test input): both modes see the same keys / stick
    bool scriptStick = false;
    float scriptX = 0.0f;
    if (const SteerScript::Step* s = script.At(in.frame)) {
        pad.left = s->key == 'L';
        pad.right = s->key == 'R';
        if (s->key == 's') {
            scriptStick = true;
            scriptX = s->stick;
            if (mode_ == HandlingMode::kOriginal) { // the bindings' stick directions (past a third of the travel)
                pad.left = scriptX <= -1.0f / 3.0f;
                pad.right = scriptX >= 1.0f / 3.0f;
            }
        }
    }
    const float keys = (pad.right ? 1.0f : 0.0f) - (pad.left ? 1.0f : 0.0f);
    input_ = keys;
    if (mode_ != HandlingMode::kModern) return; // Original: the pad as it came
    modernSeen_ = true;
    if (in.ram == nullptr || in.paused || !Seated(in.ram, in.bike)) {
        steer_ = 0.0f; // off the bike / paused: the original's pad (the walk, the menu), the lag restarts from centre
        return;
    }
    // Standing or crawling (under kModernMinSpeed): the original's digital pad. Only its keys turn a stopped bike on
    // the spot while it revs (BikeSteerDriver's `(flagsA & 0x42) == 2` gate: the digital throttle's bit 1, which the
    // analogue reader never sets) - a bike stopped nose to a wall under the analogue pad cannot turn away from it.
    if (S32(in.ram, in.bike + 0x1E0u) < kModernMinSpeed) {
        steer_ = 0.0f;
        return;
    }
    statsBike_ = in.bike;
    if (!tableReady_) BuildTable(in.ram);
    const HandlingSettings& hs = Handling();
    // the input: the bars in the hands, the analogue pad (F5), the scripted stick, the keys, a controller's stick
    float u = keys, tau = kSteerTau;
    const auto fromByte = [](uint8_t b) { return std::clamp((static_cast<float>(b) - 127.5f) / 127.5f, -1.0f, 1.0f); };
    if (pad.device.analog) {
        u = fromByte(pad.device.lx);
        if (in.barsHeld) tau = kBarsTau;
    } else if (scriptStick) {
        u = scriptX;
    } else if (in.gamepad && std::fabs(fromByte(in.stickX)) > kStickDead) {
        // a controller's stick past a small dead zone (rescaled): its analogue X, not the bindings' digital left / right
        // it also produced (past a third of the travel, input_bindings.h); a resting stick leaves the keys / d-pad
        const float x = fromByte(in.stickX);
        u = std::copysign((std::fabs(x) - kStickDead) / (1.0f - kStickDead), x);
    }
    input_ = u;
    steer_ = std::clamp(Smooth(steer_, u, lastDt_, tau * static_cast<float>(hs.steerLagPct) / 100.0f), -1.0f, 1.0f);
    // SA's response: sign x |s|^e, e = 2 at 100 % (the square), 1 (linear) at 0 %
    const float e = 1.0f + static_cast<float>(hs.curvePct) / 100.0f;
    float a = std::copysign(std::pow(std::fabs(steer_), e), steer_);
    axis_ = a;
    lx_ = ByteFor(a);
    const bool wasAnalog = pad.device.analog;
    pad.device.analog = true;
    pad.device.lx = lx_;
    if (!wasAnalog) { // the keys' throttle / brake as the right stick's Y (the brake wins), as VrHandlebars does
        pad.device.ly = 0x80;
        pad.device.ry = pad.brake ? 0xFF : (pad.throttle ? 0x00 : 0x80);
    }
    shapedThisFrame_ = true;
    ++m_.shaped;
}

// ---------------------------------------------------------------- the picture and the metrics
void ModernHandling::AfterFrame(uint8_t* ram, uint32_t bike, int32_t dt, double routeDistance, long frame) {
    const float dts = dt > 0 ? static_cast<float>(dt) / 65536.0f : 1.0f / 60.0f;
    lastDt_ = std::clamp(dts, 1.0f / 240.0f, 0.1f);
    if (LayerOff()) return;
    ++m_.frames;
    turnWritten_ = false;
    if (ram == nullptr || !InRam(bike)) return;
    float roll = 0.0f, yaw = 0.0f;
    const bool frameOk = BikeAngles(ram, bike, roll, yaw);
    const bool seated = frameOk && Seated(ram, bike);
    float omega = 0.0f; // rad/s, the heading's own change this frame (the 1/4096-turn steps of the integrator included)
    if (frameOk && haveYaw_) {
        float d = yaw - lastYaw_;
        while (d > static_cast<float>(kPi)) d -= 2.0f * static_cast<float>(kPi);
        while (d < -static_cast<float>(kPi)) d += 2.0f * static_cast<float>(kPi);
        omega = d / lastDt_;
    }
    haveYaw_ = frameOk;
    lastYaw_ = yaw;
    const float speed = static_cast<float>(S32(ram, bike + 0x1E0u)) / 65536.0f; // units/s
    // the heading-rate state the integrator turned by this frame (+0x1E8 plus the lateral force +0x2E8, 16.16 rad/s)
    const double rateRad = (static_cast<double>(S32(ram, bike + 0x1E8u)) + S32(ram, bike + 0x2E8u)) / 65536.0;
    const double rate = rateRad * 180.0 / kPi;
    const float alat = static_cast<float>(speed * kMetresPerUnit * rateRad); // m/s^2, of the turn the state makes
    const HandlingSettings& hs = Handling();
    // the drawn lean (Modern): SA's m_fLeanLRAngle - toward SA's own target for the asked turn (lean_model
    // game: the game's roll), clamped to fMaxLean, through fDesLean's lag
    const float maxLean = static_cast<float>(hs.maxLeanDeg) * static_cast<float>(kPi) / 180.0f;
    const bool modernSeated = mode_ == HandlingMode::kModern && seated;
    if (!hs.leanFromTurn) { // lean_model=game: the widening's lean follows the game's roll, before the turn layer
        if (modernSeated) {
            const float t = std::clamp(roll, -kTurnMaxLean, kTurnMaxLean);
            turnLean_ = turnLeanWarm_ ? Smooth(turnLean_, t, lastDt_, kLeanTau) : t;
            turnLeanWarm_ = true;
        } else {
            turnLeanWarm_ = false;
            turnLean_ = 0.0f;
        }
    }
    TurnLayer(ram, bike, seated);
    // SA's lean target (reVC CBike::ProcessControl "Process leaning": lean = the lateral acceleration / g,
    // clamped to fMaxLean, asin) for the turn the bike is ASKED to make - speed x the heading rate the turn layer drove to
    // this frame (its target leads the heading-rate state by the spring's ~0.1..0.2 s, as SA's tyres turn the bike at
    // once), the state +0x1E8 where the layer did not drive (the lateral force +0x2E8 of an impact is no turn)
    const double askedRate = (turnWritten_ ? turnTarget_ : static_cast<double>(S32(ram, bike + 0x1E8u))) / 65536.0;
    const double askedG = static_cast<double>(speed) * askedRate / kGravityUnits;
    const auto saLean = [askedG](float cap) {
        const double s = std::sin(static_cast<double>(cap));
        return static_cast<float>(std::asin(std::clamp(askedG, -s, s)));
    };
    if (hs.leanFromTurn) { // the widening's lean: SA's own (45 deg, fDesLean), after the layer (next frame's widening)
        if (modernSeated) {
            // of the turn the bike MAKES (the state +0x1E8, as SA's lean comes from the velocity's actual change): the
            // target would feed the widening back into itself and double a key's turn within half a second (the
            // scripted driver of the fairness check then ran 1/20 into the verge at frame 892)
            const double madeG = static_cast<double>(speed) * (static_cast<double>(S32(ram, bike + 0x1E8u)) / 65536.0) / kGravityUnits;
            const double cap = std::sin(static_cast<double>(kTurnMaxLean));
            const float t = static_cast<float>(std::asin(std::clamp(madeG, -cap, cap)));
            turnLean_ = turnLeanWarm_ ? Smooth(turnLean_, t, lastDt_, kLeanTau) : t;
            turnLeanWarm_ = true;
        } else {
            turnLeanWarm_ = false;
            turnLean_ = 0.0f;
        }
    }
    if (modernSeated) {
        const float target = hs.leanFromTurn ? saLean(maxLean) : std::clamp(roll, -maxLean, maxLean);
        if (!leanWarm_) lean_ = target;
        lean_ = Smooth(lean_, target, lastDt_, kLeanTau * static_cast<float>(hs.leanLagPct) / 100.0f);
        leanWarm_ = true;
        leanValid_ = true;
    } else {
        leanWarm_ = false;
        leanValid_ = false;
    }
    const float drawn = leanValid_ ? lean_ : roll;
    // the metrics: riding - seated, moving, the steering driver's own path ran (flagsA 0x80000), no crash / impact
    const uint32_t fa = U32(ram, bike + 0x230u), fb = U32(ram, bike + 0x234u), fc = U32(ram, bike + 0x238u);
    const bool riding = seated && speed > 1.0f && (fa & 0x80000u) != 0 && (fc & 0x600u) == 0 && (fb & 0x40u) == 0;
    // the steps (on the riding frames): the onset of an input and its release
    if (riding) {
        const bool on = std::fabs(input_) >= 0.5f, was = std::fabs(lastInput_) >= 0.5f;
        if (on && (!was || (input_ > 0) != (lastInput_ > 0))) {
            onsetSign_ = input_ > 0 ? 1 : -1;
            onsetFrames_ = 0;
            releaseTimed_ = false;
        }
        if (!on && was) {
            onsetSign_ = 0;
            releaseTimed_ = true;
            releaseFrames_ = 0;
        }
        if (onsetSign_ != 0) {
            if (onsetSign_ * rate >= 10.0) {
                m_.sumOnset += static_cast<double>(onsetFrames_) * lastDt_;
                ++m_.onsets;
                onsetSign_ = 0;
            } else if (++onsetFrames_ > 90) {
                onsetSign_ = 0; // never got there in 1.5 s: not a step of this kind
            }
        }
        if (releaseTimed_) {
            if (std::fabs(rate) <= 2.0) {
                m_.sumRelease += static_cast<double>(releaseFrames_) * lastDt_;
                ++m_.releases;
                releaseTimed_ = false;
            } else if (++releaseFrames_ > 90) {
                releaseTimed_ = false;
            }
        }
    } else {
        onsetSign_ = 0;
        releaseTimed_ = false;
    }
    lastInput_ = input_;
    if (riding) {
        ++m_.riding;
        m_.maxRate = std::max(m_.maxRate, std::fabs(rate));
        m_.maxAlat = std::max(m_.maxAlat, std::fabs(static_cast<double>(alat)) / kGravity);
        m_.maxOrigRoll = std::max(m_.maxOrigRoll, std::fabs(static_cast<double>(roll)) * 180.0 / kPi);
        m_.maxDrawnRoll = std::max(m_.maxDrawnRoll, std::fabs(static_cast<double>(drawn)) * 180.0 / kPi);
        m_.sumSpeed += speed;
        const double h = lastDt_;
        rate_[0] = rate_[1];
        rate_[1] = rate_[2];
        rate_[2] = rate;
        if (++rateN_ >= 3) {
            const double acc = (rate_[2] - rate_[1]) / h;
            const double jerk = (rate_[2] - 2.0 * rate_[1] + rate_[0]) / (h * h);
            m_.sumAcc2 += acc * acc;
            m_.maxAcc = std::max(m_.maxAcc, std::fabs(acc));
            m_.sumJerk2 += jerk * jerk;
            m_.maxJerk = std::max(m_.maxJerk, std::fabs(jerk));
            ++m_.jerkN;
        }
        roll_[0] = roll_[1];
        roll_[1] = roll_[2];
        roll_[2] = static_cast<double>(drawn) * 180.0 / kPi;
        if (++rollN_ >= 3) {
            const double racc = (roll_[2] - 2.0 * roll_[1] + roll_[0]) / (h * h);
            m_.sumRollAcc2 += racc * racc;
            m_.maxRollAcc = std::max(m_.maxRollAcc, std::fabs(racc));
            ++m_.rollN;
        }
    } else {
        rateN_ = rollN_ = 0;
    }
    if (m_.firstDistance < 0) m_.firstDistance = routeDistance;
    m_.distance = routeDistance - m_.firstDistance;
    // the per-frame CSV
    if (!csvPath.empty() && csv_ == nullptr) {
        csv_ = std::fopen(csvPath.c_str(), "w");
        if (csv_)
            std::fprintf(csv_, "frame,t,mode,seated,input,steer,axis,lx,axis_guest,speed,lat,lat_force,w2a4,rate_state_dps,"
                               "yaw_deg,omega_dps,alat_g,orig_roll_deg,drawn_roll_deg,lean33a,steer27c,rate248,flagsA,route\n");
    }
    if (csv_) {
        std::fprintf(csv_, "%ld,%.4f,%s,%d,%.4f,%.4f,%.4f,%u,%.4f,%.3f,%d,%d,%d,%.3f,%.3f,%.3f,%.4f,%.3f,%.3f,%d,%d,%d,0x%08X,%.2f\n",
                     frame, static_cast<double>(m_.frames) * lastDt_, HandlingModeName(mode_), seated ? 1 : 0,
                     static_cast<double>(input_), static_cast<double>(steer_), static_cast<double>(axis_),
                     unsigned(shapedThisFrame_ ? lx_ : 0x80), S32(ram, kAxes + 4u) / 65536.0, static_cast<double>(speed),
                     S32(ram, bike + 0x1E8u), S32(ram, bike + 0x2E8u), S32(ram, bike + 0x2A4u), rate,
                     static_cast<double>(yaw) * 180.0 / kPi, static_cast<double>(omega) * 180.0 / kPi,
                     static_cast<double>(alat) / kGravity, static_cast<double>(roll) * 180.0 / kPi,
                     static_cast<double>(drawn) * 180.0 / kPi, S16(ram, bike + 0x33Au), S32(ram, bike + 0x27Cu),
                     S32(ram, bike + 0x248u), U32(ram, bike + 0x230u), routeDistance);
    }
}

// ---------------------------------------------------------------- the turn layer (after the ported step)
void ModernHandling::TurnLayer(uint8_t* ram, uint32_t bike, bool seated) {
    const HandlingSettings& hs = Handling();
    bool on = mode_ == HandlingMode::kModern && shapedThisFrame_ && seated && hs.turnLagPct > 0;
    const uint32_t fa = U32(ram, bike + 0x230u), fb = U32(ram, bike + 0x234u), fc = U32(ram, bike + 0x238u);
    const uint32_t st = U32(ram, bike + 0x22Cu);
    // only where the original itself takes the analogue speed-band aim (BikeSteerDriver 0x80073874 / BikeAimTarget):
    // flagsA bit 20 (the pad's analogue flag), not the geometric aim (bit 27), not crashing (flagsC 0x600, flagsB 0x40),
    // no steering hold (+0x2D0), moving (BikeSteerLean's 0.5)
    on = on && (fa & 0x100000u) != 0 && (fa & 0x08000000u) == 0 && (fc & 0x600u) == 0 && (fb & 0x40u) == 0 &&
         U32(ram, bike + 0x2D0u) == 0 && S32(ram, bike + 0x1E0u) > 0x8000 && InRam(st);
    if (!on) {
        turnWarm_ = false;
        return;
    }
    const auto fixMul = [](int32_t a, int32_t b) {
        return static_cast<int32_t>((static_cast<int64_t>(a) * static_cast<int64_t>(b)) >> 16);
    };
    // BikeAimTarget's speed-band response (0x800734E8..0x8007351C), `blend` = +0x240 (flagsC & 0x600 is clear)
    const int32_t blend = S32(ram, bike + 0x240u);
    int32_t base;
    if (blend < S32(ram, st + 356u)) base = S32(ram, st + 388u);
    else if (S32(ram, st + 360u) < blend) base = S32(ram, st + 392u);
    else base = S32(ram, st + 388u) + fixMul(blend - S32(ram, st + 356u), S32(ram, st + 396u));
    const int32_t limit = std::abs(S32(ram, st + 232u));
    // SA's lean into the turn widens the lock it allows (reVC Bike.cpp: `if (sign(m_fSteerAngle) == sign(m_fLeanLRAngle))
    // f *= 2`): up to twice the response while the bike leans fully (SA's fMaxLean) the way it steers - a held turn
    // tightens as the lean builds, as the original's held key keeps ramping; clamped to the original's own +0xE8
    const int32_t axis = S32(ram, kAxes + 4u);
    const float leanFrac = (axis != 0 && (axis > 0) == (turnLean_ > 0.0f)) ? std::min(1.0f, std::fabs(turnLean_) / kTurnMaxLean) : 0.0f;
    const double widened = static_cast<double>(fixMul(base, axis)) * (1.0 + static_cast<double>(leanFrac));
    const int32_t target = static_cast<int32_t>(std::clamp(widened, -static_cast<double>(limit), static_cast<double>(limit)));
    const int32_t cur = S32(ram, bike + 0x1E8u);
    if (!turnWarm_) {
        turnLat_ = cur;
        turnVel_ = 0.0;
    }
    turnWarm_ = true;
    turnTarget_ = static_cast<double>(target); // what SA's lean leans for
    // a critically damped follower (a spring and a damper, like a tyre's force building the yaw): the heading rate's
    // own change is continuous, so a step of the target gives an S, not a corner; tau is its rise time scale.
    // Straightening (the target smaller than the turn, or the other way) is twice as brisk
    const bool unwinding = static_cast<double>(target) * turnLat_ < 0.0 || std::fabs(static_cast<double>(target)) < std::fabs(turnLat_);
    const double tau = static_cast<double>(kTurnTau) * hs.turnLagPct / 100.0 * (unwinding ? kUnwindTau : 1.0), w = 1.0 / tau,
                 h = lastDt_;
    const int steps = 4; // sub-steps of the frame (stable at any refresh rate)
    for (int k = 0; k < steps; ++k) {
        const double acc = w * w * (static_cast<double>(target) - turnLat_) - 2.0 * w * turnVel_;
        turnVel_ += acc * h / steps;
        turnLat_ += turnVel_ * h / steps;
    }
    const int32_t v = static_cast<int32_t>(std::lround(turnLat_));
    const auto put = [ram](uint32_t a, int32_t x) { std::memcpy(ram + (a & 0x1FFFFFu), &x, 4); };
    put(bike + 0x1E8u, v);
    const uint32_t rider = U32(ram, bike + 0x358u); // the copy BikeApplySteering starts from when the bike has one
    if (InRam(rider)) put(rider + 0x1E8u, v);
    turnWritten_ = true;
    ++m_.turned;
}

bool ModernHandling::VisualRoll(float& radians) const {
    if (!leanValid_) return false;
    radians = lean_;
    return true;
}

float ModernHandling::CameraRollKeep() const {
    if (!leanValid_) return 0.0f;
    return static_cast<float>(std::clamp(Handling().cameraRollPct, 0, 100)) / 100.0f;
}

std::string ModernHandling::Summary() const {
    const double jr = m_.jerkN ? std::sqrt(m_.sumJerk2 / static_cast<double>(m_.jerkN)) : 0.0;
    const double ar = m_.jerkN ? std::sqrt(m_.sumAcc2 / static_cast<double>(m_.jerkN)) : 0.0;
    const double rr = m_.rollN ? std::sqrt(m_.sumRollAcc2 / static_cast<double>(m_.rollN)) : 0.0;
    char b[1200];
    std::snprintf(b, sizeof(b),
                  "handling: %s - riding %ld of %ld frame(s), shaped %ld, turned %ld; the heading-rate state up to %.1f deg/s, its change "
                  "rms %.0f max %.0f deg/s^2, its jerk rms %.0f max %.0f deg/s^3; lateral acceleration up to %.2f g; the "
                  "drawn roll up to %.1f deg (the original's %.1f), its acceleration rms %.0f max %.0f deg/s^2; a 10 deg/s "
                  "turn %.0f ms after the input (%ld), under 2 deg/s %.0f ms after the release (%ld); distance %.1f, mean "
                  "speed %.2f; modes this race: original %ld / modern %ld frame(s), switched %d time(s)",
                  HandlingModeName(mode_), m_.riding, m_.frames, m_.shaped, m_.turned, m_.maxRate, ar, m_.maxAcc, jr, m_.maxJerk,
                  m_.maxAlat, m_.maxDrawnRoll, m_.maxOrigRoll, rr, m_.maxRollAcc,
                  m_.onsets ? 1000.0 * m_.sumOnset / static_cast<double>(m_.onsets) : -1.0, m_.onsets,
                  m_.releases ? 1000.0 * m_.sumRelease / static_cast<double>(m_.releases) : -1.0, m_.releases, m_.distance,
                  m_.riding ? m_.sumSpeed / static_cast<double>(m_.riding) : 0.0, modeFrames_[0], modeFrames_[1], switches_);
    return b;
}

} // namespace rr::game
