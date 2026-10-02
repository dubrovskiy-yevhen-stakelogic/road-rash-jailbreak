// The VR combat settings (vr_melee_settings.h).
#include "vr_melee_settings.h"

#include <algorithm>
#include <cmath>
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

std::string OnOff(bool b) { return b ? "On" : "Off"; }

} // namespace

const char* MeleeModeName(int mode) {
    return mode == MeleeSettings::kGesture             ? "gesture"
           : mode == MeleeSettings::kButtons           ? "buttons"
           : mode == MeleeSettings::kButtonsPhysical ? "buttons+physical"
                                                       : "physical";
}

bool MeleeSettings::Parse(const std::string& key, const std::string& value) {
    const std::string v = Trim(value);
    if (key == "combat") {
        if (v == "physical") return mode = kPhysical, true;
        if (v == "gesture") return mode = kGesture, true;
        if (v == "buttons") return mode = kButtons, true;
        if (v == "buttons+physical" || v == "buttons_physical") return mode = kButtonsPhysical, true;
        return false;
    }
    if (key == kCombatDefaultsKey) return v == kCombatDefaultsValue; // the marker (LoadVrSettings reads it)
    if (key == "melee_min_speed") { // m/s, one decimal
        char* end = nullptr;
        const double d = std::strtod(v.c_str(), &end);
        if (v.empty() || end == nullptr || *end != '\0' || !(d >= 0.5 && d <= 5.0)) return false;
        minSpeedDm = static_cast<int>(std::lround(d * 10.0));
        return true;
    }
    if (key == "melee_fist_cm") return Int(v, 4, 15, fistCm);
    if (key == "melee_weapon_hand") {
        if (v == "left" || v == "0") return weaponHand = 0, true;
        if (v == "right" || v == "1") return weaponHand = 1, true;
        return false;
    }
    if (key == "melee_weapon_pct") return Int(v, 50, 150, weaponPct);
    bool* flag = key == "melee_strength_from_speed" ? &strengthFromSpeed : key == "melee_bikes" ? &bikes
               : key == "melee_hands" ? &showHands : key == "melee_debug" ? &debug
               : key == "melee_snatch" ? &snatch : key == "melee_nunchaku" ? &nunchaku : nullptr;
    return flag != nullptr && Bool(v, *flag);
}

std::string MeleeSettings::Serialize() const {
    std::ostringstream o;
    o << "combat=" << MeleeModeName(mode) << "\n"
      << kCombatDefaultsKey << "=" << kCombatDefaultsValue << "\n"
      << "melee_strength_from_speed=" << (strengthFromSpeed ? 1 : 0) << "\n"
      << "melee_min_speed=" << minSpeedDm / 10 << "." << minSpeedDm % 10 << "\n"
      << "melee_fist_cm=" << fistCm << "\n"
      << "melee_weapon_pct=" << weaponPct << "\n"
      << "melee_weapon_hand=" << (weaponHand == 0 ? "left" : "right") << "\n"
      << "melee_bikes=" << (bikes ? 1 : 0) << "\n"
      << "melee_hands=" << (showHands ? 1 : 0) << "\n"
      << "melee_debug=" << (debug ? 1 : 0) << "\n"
      << "melee_snatch=" << (snatch ? 1 : 0) << "\n"
      << "melee_nunchaku=" << (nunchaku ? 1 : 0) << "\n";
    return o.str();
}

std::string MeleeSettings::Describe() const {
    if (!Physical()) return std::string("combat ") + MeleeModeName(mode); // (the combat buttons attack)
    char b[340];
    std::snprintf(b, sizeof(b),
                  "combat PHYSICAL %s (min %.1f m/s, strength from speed %s, fist %d cm, weapon %d %% in the %s hand, bikes %s, "
                  "hands %s, snatch %s, nunchaku chain %s%s)",
                  mode == kButtonsPhysical ? "+ BUTTONS (the combat buttons attack too)" : "ONLY (the combat buttons do not attack)",
                  double(MinSpeed()), strengthFromSpeed ? "on" : "off", fistCm, weaponPct, weaponHand == 0 ? "left" : "right",
                  bikes ? "on" : "off",
                  showHands ? "on" : "off", snatch ? "on" : "off", nunchaku ? "on" : "off", debug ? ", colliders drawn" : "");
    return b;
}

std::vector<MeleeMenuRow> MeleeMenuRows(const MeleeSettings& s) {
    // (the mode alone says whether the buttons attack, in both steering modes)
    static const char* const kModes[4] = {"Physical only (no buttons)", "Gesture + buttons", "Buttons only",
                                          "Buttons + physical"};
    char b[64];
    std::vector<MeleeMenuRow> r;
    r.push_back({"Combat mode", kModes[std::clamp(s.mode, 0, 3)]});
    r.push_back({"Strength from swing speed", OnOff(s.strengthFromSpeed)});
    std::snprintf(b, sizeof(b), "%.1f m/s", double(s.MinSpeed()));
    r.push_back({"Minimum swing speed", b});
    std::snprintf(b, sizeof(b), "%d cm", s.fistCm);
    r.push_back({"Fist size", b});
    std::snprintf(b, sizeof(b), "%d %%", s.weaponPct);
    r.push_back({"Weapon length", b});
    r.push_back({"Weapon hand", s.weaponHand == 0 ? "Left" : "Right"});
    r.push_back({"Blows on the bike count", OnOff(s.bikes)});
    r.push_back({"Hands in Stick mode", OnOff(s.showHands)});
    r.push_back({"Show colliders", OnOff(s.debug)});
    r.push_back({"Snatch a weapon mid-swing", OnOff(s.snatch)});
    r.push_back({"Nunchaku / chain physics", OnOff(s.nunchaku)});
    return r;
}

bool MeleeMenuActivate(MeleeSettings& s, int row, int direction) {
    const int d = direction == 0 ? 1 : direction;
    switch (row) {
    case 0: s.mode = (s.mode + d + 4) % 4; return true; // (four modes)
    case 1: s.strengthFromSpeed = !s.strengthFromSpeed; return true;
    case 2: s.minSpeedDm = std::clamp(s.minSpeedDm + 5 * d, 5, 50); return true;
    case 3: s.fistCm = std::clamp(s.fistCm + d, 4, 15); return true;
    case 4: s.weaponPct = std::clamp(s.weaponPct + 10 * d, 50, 150); return true;
    case 5: s.weaponHand = 1 - s.weaponHand; return true;
    case 6: s.bikes = !s.bikes; return true;
    case 7: s.showHands = !s.showHands; return true;
    case 8: s.debug = !s.debug; return true;
    case 9: s.snatch = !s.snatch; return true;
    case 10: s.nunchaku = !s.nunchaku; return true;
    default: return false;
    }
}

bool ApplyMeleeFlag(int argc, char** argv, int& i, MeleeSettings& s) {
    static const char* const kKeys[][2] = {{"--vr-combat", "combat"},
                                           {"--vr-melee-strength", "melee_strength_from_speed"},
                                           {"--vr-melee-min-speed", "melee_min_speed"},
                                           {"--vr-melee-fist-cm", "melee_fist_cm"},
                                           {"--vr-melee-weapon-pct", "melee_weapon_pct"},
                                           {"--vr-melee-bikes", "melee_bikes"},
                                           {"--vr-melee-hands", "melee_hands"},
                                           {"--vr-melee-debug", "melee_debug"},
                                           {"--vr-melee-weapon-hand", "melee_weapon_hand"},
                                           {"--vr-melee-snatch", "melee_snatch"},
                                           {"--vr-melee-nunchaku", "melee_nunchaku"}};
    const std::string a = argv[i];
    if (a == "--vr-melee-script") {
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

const std::string& MeleeScriptArgument() { return Script(); }

} // namespace rrgame
