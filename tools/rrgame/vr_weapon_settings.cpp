// The VR weapons' settings (vr_weapon_settings.h).
#include "vr_weapon_settings.h"

#include "vr_holsters.h"      // the owned weapons and their names for the holster rows
#include "vr_weapon_calib.h"  // the calibration page

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

bool WeaponId(const std::string& v, int& out) {
    if (v == "none" || v == "empty" || v == "-1") return out = -1, true;
    return Int(v, 0, 8, out);
}

std::string& Script() {
    static std::string s;
    return s;
}

std::string OnOff(bool b) { return b ? "On" : "Off"; }

// "a,b,c,d,e,f" (or spaces) -> six ints
bool Six(const std::string& v, int out[6]) {
    std::string t = v;
    for (char& c : t)
        if (c == ',') c = ' ';
    std::istringstream in(t);
    for (int k = 0; k < 6; ++k)
        if (!(in >> out[k])) return false;
    std::string rest;
    return !(in >> rest);
}

bool SetGrip(WeaponGrip& g, const int v[6]) {
    for (int k = 0; k < 3; ++k)
        if (std::abs(v[k]) > 300 || std::abs(v[3 + k]) > 180) return false;
    for (int k = 0; k < 3; ++k) g.offMm[k] = v[k], g.rotDeg[k] = v[3 + k];
    return true;
}

} // namespace

bool WeaponsSettings::Parse(const std::string& key, const std::string& value) {
    const std::string v = Trim(value);
    if (key == "weapon_grip_left_trim") {
        int six[6];
        return Six(v, six) && SetGrip(leftTrim, six);
    }
    if (key.rfind("weapon_grip_", 0) == 0) {
        int w = -1, six[6];
        if (!Int(key.substr(12), 0, 8, w) || !Six(v, six)) return false;
        return SetGrip(grip[w], six);
    }
    if (key == "holster_left") return WeaponId(v, left);
    if (key == "holster_right") return WeaponId(v, right);
    if (key == "holster_height_cm") return Int(v, 20, 100, heightCm);
    if (key == "holster_spread_cm") return Int(v, 10, 50, spreadCm);
    if (key == "holster_forward_cm") return Int(v, -30, 40, forwardCm);
    if (key == "holster_reach_cm") return Int(v, 8, 35, reachCm);
    if (key == "melee_weapon_speed") {
        char* end = nullptr;
        const double d = std::strtod(v.c_str(), &end);
        if (v.empty() || end == nullptr || *end != '\0' || !(d >= 0.5 && d <= 8.0)) return false;
        weaponSpeedDm = static_cast<int>(std::lround(d * 10.0));
        return true;
    }
    if (key == "melee_weapon_travel_cm") return Int(v, 0, 80, weaponTravelCm);
    if (key == "melee_fist_travel_cm") return Int(v, 0, 60, fistTravelCm);
    if (key == "melee_into_pct") return Int(v, 0, 95, intoPct);
    if (key == "melee_cooldown_ms") return Int(v, 0, 2000, cooldownMs);
    if (key == "bars_buttons_attack") { // superseded by the combat mode - an old file's value is dropped
        bool unused = false;
        return Bool(v, unused);
    }
    bool* flag = key == "holsters" ? &holsters : key == "holster_grip_lock" ? &gripLock : key == "holster_markers" ? &markers : key == "holster_hide_empty" ? &hideEmpty
               : key == "prod_button" ? &prodButton
               : key == "prod_reach" ? &prodReach : nullptr;
    return flag != nullptr && Bool(v, *flag);
}

std::string WeaponsSettings::Serialize() const {
    std::ostringstream o;
    o << "holsters=" << (holsters ? 1 : 0) << "\n"
      << "holster_left=" << left << "\n"
      << "holster_right=" << right << "\n"
      << "holster_grip_lock=" << (gripLock ? 1 : 0) << "\n"
      << "holster_height_cm=" << heightCm << "\n"
      << "holster_spread_cm=" << spreadCm << "\n"
      << "holster_forward_cm=" << forwardCm << "\n"
      << "holster_reach_cm=" << reachCm << "\n"
      << "holster_markers=" << (markers ? 1 : 0) << "\n"
      << "holster_hide_empty=" << (hideEmpty ? 1 : 0) << "\n"
      << "melee_weapon_speed=" << weaponSpeedDm / 10 << "." << weaponSpeedDm % 10 << "\n"
      << "melee_weapon_travel_cm=" << weaponTravelCm << "\n"
      << "melee_fist_travel_cm=" << fistTravelCm << "\n"
      << "melee_into_pct=" << intoPct << "\n"
      << "melee_cooldown_ms=" << cooldownMs << "\n"
      << "prod_button=" << (prodButton ? 1 : 0) << "\n"
      << "prod_reach=" << (prodReach ? 1 : 0) << "\n";
    for (int w = 0; w < 9; ++w) {
        const WeaponGrip& g = grip[w];
        if (g.IsDefault()) continue;
        o << "weapon_grip_" << w << "=" << g.offMm[0] << " " << g.offMm[1] << " " << g.offMm[2] << " " << g.rotDeg[0] << " "
          << g.rotDeg[1] << " " << g.rotDeg[2] << "\n";
    }
    if (!leftTrim.IsDefault())
        o << "weapon_grip_left_trim=" << leftTrim.offMm[0] << " " << leftTrim.offMm[1] << " " << leftTrim.offMm[2] << " "
          << leftTrim.rotDeg[0] << " " << leftTrim.rotDeg[1] << " " << leftTrim.rotDeg[2] << "\n";
    return o.str();
}

std::string WeaponsSettings::Describe() const {
    char b[600];
    std::snprintf(b, sizeof(b),
                  "weapons: holsters %s (left %d, right %d, %s, %d cm down, %d cm out, %d cm forward, reach %d cm, markers %s%s); "
                  "swing: weapon %.1f m/s at the contact and %d cm of travel, fist %d cm, %d %% into the body, %d ms "
                  "between a hand's blows; prod on B %s%s",
                  holsters ? "ON" : "off", left, right, gripLock ? "grip lock" : "a released weapon goes back", heightCm,
                  spreadCm, forwardCm, reachCm, markers ? "on" : "off", hideEmpty ? ", no-swing weapons hidden" : "", double(WeaponSpeed()), weaponTravelCm, fistTravelCm,
                  intoPct, cooldownMs, prodButton ? "on" : "off",
                  prodButton ? (prodReach ? " (the original's reach)" : " (contact only)") : "");
    std::string s = b;
    for (int w = 0; w < 9; ++w)
        if (!grip[w].IsDefault()) {
            std::snprintf(b, sizeof(b), "; grip %d %+d %+d %+d mm %+d %+d %+d deg", w, grip[w].offMm[0], grip[w].offMm[1],
                          grip[w].offMm[2], grip[w].rotDeg[0], grip[w].rotDeg[1], grip[w].rotDeg[2]);
            s += b;
        }
    if (!leftTrim.IsDefault()) {
        std::snprintf(b, sizeof(b), "; the left hand's trim %+d %+d %+d mm %+d %+d %+d deg", leftTrim.offMm[0],
                      leftTrim.offMm[1], leftTrim.offMm[2], leftTrim.rotDeg[0], leftTrim.rotDeg[1], leftTrim.rotDeg[2]);
        s += b;
    }
    return s;
}

// ------------------------------------------------------------------------------------------------ the menu rows
std::vector<WeaponMenuRow> WeaponsMenuRows(const WeaponsSettings& s) {
    std::vector<WeaponMenuRow> r;
    char b[64];
    r.push_back({"Calibrate the grip", "", true}); // kWeaponsCalibRow (first, not at the bottom)
    r.push_back({"Holsters", s.holsters ? "On (weapons on the hips)" : "Off (the weapon stays in the hand)"});
    r.push_back({"Left holster", HolsterWeaponLabel(s.left)});
    r.push_back({"Right holster", HolsterWeaponLabel(s.right)});
    r.push_back({"Releasing the grip", s.gripLock ? "Keeps it in the hand" : "Puts it back"});
    std::snprintf(b, sizeof(b), "%d cm below the eye", s.heightCm);
    r.push_back({"Holster height", b});
    std::snprintf(b, sizeof(b), "%d cm", s.spreadCm);
    r.push_back({"Holster width", b});
    std::snprintf(b, sizeof(b), "%+d cm", s.forwardCm);
    r.push_back({"Holster forward", b});
    std::snprintf(b, sizeof(b), "%d cm", s.reachCm);
    r.push_back({"Holster reach", b});
    r.push_back({"Holster rings", OnOff(s.markers)});
    r.push_back({"Weapons with no swings", s.hideEmpty ? "Hidden" : "Shown"});
    r.push_back({"Owned weapons", HolsterOwnedLabel()});
    return r;
}

int WeaponsMenuActivate(WeaponsSettings& s, int row, int direction, std::string& note) {
    const int d = direction == 0 ? 1 : direction;
    switch (row) {
    case kWeaponsCalibRow: return direction == 0 ? 2 : 0;
    case 1: s.holsters = !s.holsters; note = s.holsters ? "Holsters: grip at a hip to draw" : "Holsters off"; return 1;
    case 2: s.left = CycleHolsterWeapon(s.left, s.right, d); return 1;
    case 3: s.right = CycleHolsterWeapon(s.right, s.left, d); return 1;
    case 4: s.gripLock = !s.gripLock; return 1;
    case 5: s.heightCm = std::clamp(s.heightCm + 2 * d, 20, 100); return 1;
    case 6: s.spreadCm = std::clamp(s.spreadCm + 2 * d, 10, 50); return 1;
    case 7: s.forwardCm = std::clamp(s.forwardCm + 2 * d, -30, 40); return 1;
    case 8: s.reachCm = std::clamp(s.reachCm + d, 8, 35); return 1;
    case 9: s.markers = !s.markers; return 1;
    case 10: s.hideEmpty = !s.hideEmpty; return 1;
    default: return 0; // 11: Owned weapons (shown only)
    }
}

std::vector<WeaponMenuRow> SwingMenuRows(const WeaponsSettings& s) {
    std::vector<WeaponMenuRow> r;
    char b[64];
    std::snprintf(b, sizeof(b), "%.1f m/s", double(s.WeaponSpeed()));
    r.push_back({"Weapon swing speed", b});
    std::snprintf(b, sizeof(b), "%d cm", s.weaponTravelCm);
    r.push_back({"Weapon swing travel", b});
    std::snprintf(b, sizeof(b), "%d cm", s.fistTravelCm);
    r.push_back({"Punch travel", b});
    std::snprintf(b, sizeof(b), "%d %%", s.intoPct);
    r.push_back({"Into the target", b});
    std::snprintf(b, sizeof(b), "%.2f s", double(s.cooldownMs) / 1000.0);
    r.push_back({"Blow cooldown", b});
    // ("Buttons attack in Handlebars mode" is gone - Combat mode, the page's first row, decides)
    r.push_back({"Prod / stun gun on B", OnOff(s.prodButton)});
    r.push_back({"Prod / stun gun reach", s.prodReach ? "The original's" : "Contact only"});
    return r;
}

bool SwingMenuActivate(WeaponsSettings& s, int row, int direction) {
    const int d = direction == 0 ? 1 : direction;
    switch (row) {
    case 0: s.weaponSpeedDm = std::clamp(s.weaponSpeedDm + 5 * d, 5, 80); return true;
    case 1: s.weaponTravelCm = std::clamp(s.weaponTravelCm + 5 * d, 0, 80); return true;
    case 2: s.fistTravelCm = std::clamp(s.fistTravelCm + 5 * d, 0, 60); return true;
    case 3: s.intoPct = std::clamp(s.intoPct + 10 * d, 0, 90); return true;
    case 4: s.cooldownMs = std::clamp(s.cooldownMs + 100 * d, 0, 2000); return true;
    case 5: s.prodButton = !s.prodButton; return true;
    case 6: s.prodReach = !s.prodReach; return true;
    default: return false;
    }
}

bool ApplyWeaponsFlag(int argc, char** argv, int& i, WeaponsSettings& s) {
    static const char* const kKeys[][2] = {{"--vr-holsters", "holsters"},
                                           {"--vr-holster-left", "holster_left"},
                                           {"--vr-holster-right", "holster_right"},
                                           {"--vr-holster-grip-lock", "holster_grip_lock"},
                                           {"--vr-holster-height", "holster_height_cm"},
                                           {"--vr-holster-spread", "holster_spread_cm"},
                                           {"--vr-holster-forward", "holster_forward_cm"},
                                           {"--vr-holster-reach", "holster_reach_cm"},
                                           {"--vr-holster-markers", "holster_markers"},
                                           {"--vr-holster-hide-empty", "holster_hide_empty"},
                                           {"--vr-weapon-speed", "melee_weapon_speed"},
                                           {"--vr-weapon-travel", "melee_weapon_travel_cm"},
                                           {"--vr-fist-travel", "melee_fist_travel_cm"},
                                           {"--vr-melee-into", "melee_into_pct"},
                                           {"--vr-melee-cooldown", "melee_cooldown_ms"},
                                           {"--vr-prod-button", "prod_button"},
                                           {"--vr-prod-reach", "prod_reach"}};
    const std::string a = argv[i];
    if (a == "--vr-holster-script") {
        if (i + 1 >= argc) throw std::runtime_error(a + " needs a file or an inline script");
        Script() = argv[++i];
        return true;
    }
    if (a == "--vr-weapon-grip") { // W:offR,offU,offF,pitch,yaw,roll
        if (i + 1 >= argc) throw std::runtime_error(a + " needs W:offR,offU,offF,pitch,yaw,roll");
        const std::string v = argv[++i];
        const size_t colon = v.find(':');
        int w = -1, six[6];
        if (colon == std::string::npos || !Int(v.substr(0, colon), 0, 8, w) || !Six(v.substr(colon + 1), six) ||
            !SetGrip(s.grip[w], six))
            throw std::runtime_error(a + ": bad value '" + v + "' (W:offR,offU,offF,pitch,yaw,roll in mm / degrees)");
        return true;
    }
    if (a == "--vr-weapon-grip-left-trim") { // offR,offU,offF,pitch,yaw,roll
        if (i + 1 >= argc) throw std::runtime_error(a + " needs offR,offU,offF,pitch,yaw,roll");
        const std::string v = argv[++i];
        int six[6];
        if (!Six(v, six) || !SetGrip(s.leftTrim, six))
            throw std::runtime_error(a + ": bad value '" + v + "' (offR,offU,offF,pitch,yaw,roll in mm / degrees)");
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

const std::string& HolsterScriptArgument() { return Script(); }

} // namespace rrgame
