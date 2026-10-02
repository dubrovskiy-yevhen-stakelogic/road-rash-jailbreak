// The VR steering settings (vr_bars_settings.h).
#include "vr_bars_settings.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <stdexcept>

namespace rrgame {

namespace {

std::string Trim(const std::string& s) {
    const size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    const size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

bool Int(const std::string& v, int lo, int hi, int& out) {
    if (v.empty()) return false;
    char* end = nullptr;
    const long n = std::strtol(v.c_str(), &end, 10);
    if (end == nullptr || *end != '\0' || n < lo || n > hi) return false;
    out = static_cast<int>(n);
    return true;
}

bool Bool(const std::string& v, bool& out) {
    if (v == "1" || v == "on" || v == "true" || v == "yes") return out = true, true;
    if (v == "0" || v == "off" || v == "false" || v == "no") return out = false, true;
    return false;
}

std::string& Script() {
    static std::string s;
    return s;
}

} // namespace

bool BarsSettings::Parse(const std::string& key, const std::string& value) {
    const std::string v = Trim(value);
    if (key == "steering") {
        if (v == "stick") return steering = kStick, true;
        if (v == "handlebars" || v == "bars") return steering = kHandlebars, true;
        return false;
    }
    if (key == "bars_sensitivity") return Int(v, 50, 200, sensitivity);
    if (key == "bars_deadzone") return Int(v, 0, 20, deadzone);
    if (key == "bars_height_cm") return Int(v, -30, 30, heightCm);
    if (key == "bike_visual_lean") return Int(v, 0, 100, visualLean);
    bool* flag = key == "twist_throttle" ? &twistThrottle : key == "motion_punches" ? &motionPunches
               : key == "one_hand_steering" ? &oneHand : key == "bars_hands_follow_tilt" ? &handsFollowTilt
               : key == "eye_on_bike" ? &eyeOnBike : nullptr;
    return flag != nullptr && Bool(v, *flag);
}

std::string BarsSettings::Serialize() const {
    std::ostringstream o;
    o << "steering=" << (steering == kHandlebars ? "handlebars" : "stick") << "\n"
      << "bars_sensitivity=" << sensitivity << "\n"
      << "bars_deadzone=" << deadzone << "\n"
      << "twist_throttle=" << (twistThrottle ? 1 : 0) << "\n"
      << "motion_punches=" << (motionPunches ? 1 : 0) << "\n"
      << "one_hand_steering=" << (oneHand ? 1 : 0) << "\n"
      << "bars_height_cm=" << heightCm << "\n"
      << "bars_hands_follow_tilt=" << (handsFollowTilt ? 1 : 0) << "\n"
      << "bike_visual_lean=" << visualLean << "\n"
      << "eye_on_bike=" << (eyeOnBike ? 1 : 0) << "\n";
    return o.str();
}

std::string BarsSettings::Describe() const {
    if (steering != kHandlebars) return "steering stick";
    char b[360];
    std::snprintf(b, sizeof(b),
                  "steering HANDLEBARS (full lock %.0f deg, dead zone %d%%, twist throttle %s, motion punches %s, one hand "
                  "%s, bars %+d cm, hands follow the bike's tilt %s, visual lean %d%%, eye %s)",
                  double(FullLockDegrees()), deadzone, twistThrottle ? "on" : "off", motionPunches ? "on" : "off",
                  oneHand ? "steers" : "holds only", heightCm, handsFollowTilt ? "on" : "off", visualLean,
                  eyeOnBike ? "on the bike" : "the rider's head");
    return b;
}

std::vector<BarsMenuRow> BarsMenuRows(const BarsSettings& s) {
    char b[64];
    std::vector<BarsMenuRow> r;
    std::snprintf(b, sizeof(b), "%d %% (full lock %.0f deg)", s.sensitivity, double(s.FullLockDegrees()));
    r.push_back({"Bars sensitivity", b});
    std::snprintf(b, sizeof(b), "%d %%", s.deadzone);
    r.push_back({"Bars dead zone", b});
    r.push_back({"Throttle", s.twistThrottle ? "Twist grip + right trigger" : "Right trigger"});
    r.push_back({"Motion punches", s.motionPunches ? "On (swing a free hand)" : "Off (buttons only)"});
    r.push_back({"One-hand steering", s.oneHand ? "On" : "Off (both hands steer)"});
    // (the bars' height, the visual lean and the eye: the VR menu's Riding position page - vr_menu.cpp)
    r.push_back({"Hands follow bike tilt", s.handsFollowTilt ? "On (the bike's frame)" : "Off (controller wrist)"});
    return r;
}

bool BarsMenuActivate(BarsSettings& s, int row, int direction) {
    const int d = direction == 0 ? 1 : direction;
    switch (row) {
    case 0: s.sensitivity = std::clamp(s.sensitivity + 10 * d, 50, 200); return true;
    case 1: s.deadzone = std::clamp(s.deadzone + d, 0, 20); return true;
    case 2: s.twistThrottle = !s.twistThrottle; return true;
    case 3: s.motionPunches = !s.motionPunches; return true;
    case 4: s.oneHand = !s.oneHand; return true;
    case 5: s.handsFollowTilt = !s.handsFollowTilt; return true;
    default: return false;
    }
}

bool ApplyBarsFlag(int argc, char** argv, int& i, BarsSettings& s) {
    static const char* const kKeys[][2] = {{"--vr-steering", "steering"},
                                           {"--vr-bars-sensitivity", "bars_sensitivity"},
                                           {"--vr-bars-deadzone", "bars_deadzone"},
                                           {"--vr-twist-throttle", "twist_throttle"},
                                           {"--vr-motion-punches", "motion_punches"},
                                           {"--vr-one-hand", "one_hand_steering"},
                                           {"--vr-bars-height", "bars_height_cm"},
                                           {"--vr-bars-follow-tilt", "bars_hands_follow_tilt"},
                                           {"--vr-bike-lean", "bike_visual_lean"},
                                           {"--vr-eye-on-bike", "eye_on_bike"}};
    const std::string a = argv[i];
    if (a == "--vr-bars-script") {
        if (i + 1 >= argc) throw std::runtime_error(a + " needs a file or an inline script");
        Script() = argv[++i];
        return true;
    }
    for (const auto& k : kKeys)
        if (a == k[0]) {
            if (i + 1 >= argc) throw std::runtime_error(a + " needs a value");
            const std::string v = argv[++i];
            if (!s.Parse(k[1], v)) throw std::runtime_error(a + ": bad value '" + v + "'");
            return true;
        }
    return false;
}

const std::string& BarsScriptArgument() { return Script(); }

} // namespace rrgame
