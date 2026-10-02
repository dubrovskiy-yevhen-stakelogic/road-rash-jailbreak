// The VR settings (vr_settings.h), stored as the [vr] section of rrgame_settings.ini next to [graphics].
#include "vr_settings.h"

#include "settings_file.h"

#include "vr_comfort.h"
#include "vr_bike_shake.h"
#include "vr_pacing.h"
#include "vr_horizon.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace rrgame {

namespace {

std::string Trim(const std::string& s) {
    const size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    const size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

bool ParseInt(const std::string& v, int& out) {
    if (v.empty()) return false;
    char* end = nullptr;
    const long n = std::strtol(v.c_str(), &end, 10);
    if (end == nullptr || *end != '\0') return false;
    out = static_cast<int>(n);
    return true;
}

bool ParseBool(const std::string& v, bool& out) {
    if (v == "1" || v == "on" || v == "true" || v == "yes") return out = true, true;
    if (v == "0" || v == "off" || v == "false" || v == "no") return out = false, true;
    return false;
}

const char* const kDistance[3] = {"original", "extended", "maximum"};

} // namespace

bool VrSettings::Parse(const std::string& key, const std::string& value) {
    const std::string v = Trim(value);
    int n = 0;
    const auto ranged = [&](int& field, int lo, int hi) {
        if (!ParseInt(v, n) || n < lo || n > hi) return false;
        field = n;
        return true;
    };
    if (key == "eye_scale") return ranged(eyeScale, 50, 200);
    if (key == "refresh") return ranged(refreshHz, 60, 144);
    if (key == "msaa") {
        if (!ParseInt(v, n) || (n != 1 && n != 2 && n != 4)) return false;
        msaa = n;
        return true;
    }
    if (key == "draw_distance") {
        for (int k = 0; k < 3; ++k)
            if (v == kDistance[k]) return drawDistance = k, true;
        return false;
    }
    if (key == "foveation") return ranged(foveation, 0, 3);
    if (key == "vibration") return ranged(vibration, 0, 100);
    if (key == "horizon_lock") return ranged(horizonLock, 0, 100);
    if (key == "horizon_mode") {
        if (v == "off") return horizonMode = kHorizonOff, true;
        if (v == "roll") return horizonMode = kHorizonRoll, true;
        if (v == "full" || v == "roll+pitch") return horizonMode = kHorizonRollPitch, true;
        return false;
    }
    if (key == "seat_height_cm") return ranged(seatHeightCm, -40, 40);
    if (key == "hud_size") return ranged(hudSize, 0, 2);
    if (key == "world_scale") return ranged(worldScale, 50, 200);
    if (key == "view") {
        if (v == "head") return headView = true, true;
        if (v == "chase") return headView = false, true;
        return false;
    }
    if (key == "bike_shake") return ParseBikeShake(v, bikeShake); // vr_bike_shake.h
    if (key == "view_pitch") return ParseViewPitch(v, viewPitch); // vr_horizon.h
    if (key == "seat_back_cm") return ranged(seatBackCm, -20, 40);
    if (key == "fall_view") {
        if (v == "fixed") return fallFixed = true, true;
        if (v == "chase") return fallFixed = false, true;
        return false;
    }
    bool* flag = key == "max_detail" ? &maxDetail : key == "textures_smooth" ? &smoothTextures : key == "hd_media" ? &hdMedia : key == "comfort" ? &comfort
               : key == "multiview" ? &multiview : key == "smooth_motion" ? &smoothMotion : nullptr;
    if (flag == nullptr) return bars.Parse(key, v) || melee.Parse(key, v) || weapons.Parse(key, v); // the steering / combat / weapons keys
    return ParseBool(v, *flag);
}

std::string VrSettings::Serialize() const {
    std::ostringstream o;
    o << "eye_scale=" << eyeScale << "\n"
      << "refresh=" << refreshHz << "\n"
      << "msaa=" << msaa << "\n"
      << "draw_distance=" << kDistance[std::clamp(drawDistance, 0, 2)] << "\n"
      << "max_detail=" << (maxDetail ? 1 : 0) << "\n"
      << "textures_smooth=" << (smoothTextures ? 1 : 0) << "\n"
      << "hd_media=" << (hdMedia ? 1 : 0) << "\n"
      << "foveation=" << foveation << "\n"
      << "vibration=" << vibration << "\n"
      << "horizon_lock=" << horizonLock << "\n"
      << "horizon_mode=" << (horizonMode == kHorizonOff ? "off" : horizonMode == kHorizonRollPitch ? "full" : "roll") << "\n"
      << "comfort=" << (comfort ? 1 : 0) << "\n"
      << "seat_height_cm=" << seatHeightCm << "\n"
      << "seat_back_cm=" << seatBackCm << "\n"
      << "hud_size=" << hudSize << "\n"
      << "view=" << (headView ? "head" : "chase") << "\n"
      << "world_scale=" << worldScale << "\n"
      << "multiview=" << (multiview ? 1 : 0) << "\n"
      << "smooth_motion=" << (smoothMotion ? 1 : 0) << "\n"
      << "fall_view=" << (fallFixed ? "fixed" : "chase") << "\n"
      << "bike_shake=" << (bikeShake == kShakeOff ? "off" : bikeShake == kShakeOriginal ? "original" : "low") << "\n"
      << "view_pitch=" << (viewPitch == kPitchRoad ? "road" : viewPitch == kPitchOriginal ? "original" : "low") << "\n"
      << bars.Serialize() << melee.Serialize() << weapons.Serialize();
    return o.str();
}

std::string VrSettings::Describe() const {
    char b[480];
    std::snprintf(b, sizeof(b),
                  "vr: eye resolution %d%%, %d Hz, MSAA %dx, draw distance %s, detail %s, textures %s, foveation %d, "
                  "vibration %d%%, horizon lock %d%% (%s), comfort vignette %s, seat %+d cm, HUD %s, view %s, world scale %d%%, "
                  "single-pass stereo %s",
                  eyeScale, refreshHz, msaa, kDistance[std::clamp(drawDistance, 0, 2)], maxDetail ? "MAXIMUM" : "original",
                  smoothTextures ? "smooth" : "PS1 nearest", foveation, vibration, horizonLock,
                  horizonMode == kHorizonOff ? "off" : horizonMode == kHorizonRollPitch ? "roll + pitch" : "roll only", comfort ? "on" : "off",
                  seatHeightCm, hudSize == 0 ? "small" : hudSize == 2 ? "large" : "medium", headView ? "head" : "chase",
                  worldScale, multiview ? "on" : "off");
    return std::string(b) + ", smooth motion " + (smoothMotion ? "on" : "off") + ", off the bike " +
           (fallFixed ? "fixed level view" : "the chase camera") + ", bike vibration " + BikeShakeName(bikeShake) + ", bike pitch " + ViewPitchName(viewPitch) + ", seat back " +
           std::to_string(seatBackCm) + " cm, " + bars.Describe() + ", " + melee.Describe() + ", " + weapons.Describe();
}

// The product's defaults (vr_settings.h ProductDefaults): a complete [vr] section, applied over the member initialisers.
// Seated play on Quest 3: the eye image at 130 %, 72 Hz, MSAA 2x, foveation medium, the rumble at half strength, the seat
// 15 cm lower, steering with the hands on the handlebars, the combat buttons + physical blows, the club on the left hip
// and the stun gun on the right, and a grip for every weapon.
const char* VrSettings::ProductDefaultsText() {
    return "eye_scale=130\n"
           "refresh=72\n"
           "msaa=2\n"
           "draw_distance=extended\n"
           "max_detail=1\n"
           "textures_smooth=1\n"
           "hd_media=1\n"
           "foveation=2\n"
           "vibration=50\n"
           "horizon_lock=100\n"
           "horizon_mode=roll\n"
           "comfort=0\n"
           "seat_height_cm=-15\n"
           "seat_back_cm=0\n"
           "hud_size=1\n"
           "view=head\n"
           "world_scale=100\n"
           "multiview=1\n"
           "smooth_motion=1\n"
           "fall_view=fixed\n"
           "bike_shake=low\n"
           "view_pitch=low\n"
           "steering=handlebars\n"
           "bars_sensitivity=100\n"
           "bars_deadzone=3\n"
           "twist_throttle=1\n"
           "motion_punches=1\n"
           "one_hand_steering=1\n"
           "bars_height_cm=0\n"
           "bars_hands_follow_tilt=1\n"
           "bike_visual_lean=50\n"
           "eye_on_bike=1\n"
           "combat=buttons+physical\n"
           "melee_strength_from_speed=0\n"
           "melee_min_speed=1.5\n"
           "melee_fist_cm=8\n"
           "melee_weapon_pct=100\n"
           "melee_weapon_hand=right\n"
           "melee_bikes=0\n"
           "melee_hands=1\n"
           "melee_debug=0\n"
           "melee_snatch=1\n"
           "melee_nunchaku=1\n"
           "holsters=1\n"
           "holster_left=1\n"
           "holster_right=7\n"
           "holster_grip_lock=0\n"
           "holster_height_cm=58\n"
           "holster_spread_cm=26\n"
           "holster_forward_cm=5\n"
           "holster_reach_cm=18\n"
           "holster_markers=1\n"
           "holster_hide_empty=1\n"
           "melee_weapon_speed=3.0\n"
           "melee_weapon_travel_cm=25\n"
           "melee_fist_travel_cm=10\n"
           "melee_into_pct=50\n"
           "melee_cooldown_ms=500\n"
           "prod_button=1\n"
           "prod_reach=1\n"
           "weapon_grip_0=-30 0 65 75 0 0\n"
           "weapon_grip_1=-30 0 65 60 0 0\n"
           "weapon_grip_2=-30 0 65 75 0 0\n"
           "weapon_grip_3=-15 -10 55 70 0 0\n"
           "weapon_grip_4=-35 0 50 65 0 0\n"
           "weapon_grip_5=-25 0 55 70 0 -170\n"
           "weapon_grip_6=-35 0 60 75 0 0\n"
           "weapon_grip_7=-10 0 30 85 0 0\n"
           "weapon_grip_8=-15 5 65 80 0 0\n";
}

VrSettings VrSettings::ProductDefaults() {
    VrSettings s;
    std::istringstream in(ProductDefaultsText());
    std::string line;
    while (std::getline(in, line)) {
        const size_t eq = line.find('=');
        if (eq != std::string::npos && !s.Parse(line.substr(0, eq), line.substr(eq + 1)))
            std::fprintf(stderr, "vr settings: the product default '%s' is not a [vr] key\n", line.c_str());
    }
    return s;
}

VrSettings& VrPrefs() {
    static VrSettings s;
    return s;
}

bool LoadVrSettings(const std::string& path, VrSettings& s, bool* rewrite) {
    if (rewrite != nullptr) *rewrite = false;
    IniPairs kv;
    if (!ReadIniSection(path, "vr", kv)) return false;
    bool from1cr = false; // the section carries combat_defaults=1cr
    for (const auto& [key, value] : kv) {
        if (key == kCombatDefaultsKey && value == kCombatDefaultsValue) from1cr = true;
        if (!s.Parse(key, value)) std::fprintf(stderr, "vr settings: %s: ignored '%s=%s'\n", path.c_str(), key.c_str(), value.c_str());
    }
    if (!kv.empty() && !from1cr && s.melee.mode != MeleeSettings::kButtonsPhysical) {
        // a section without combat_defaults (an older file) kept the buttons off in the Handlebars mode (a
        // separate row, off; the mode Buttons did not lift it). The combat starts from Buttons + physical once; the
        // section is then written back with combat_defaults, and from then on the player's own choice is kept.
        std::printf("vr settings: [vr] of %s has no combat_defaults - the combat starts from Buttons + physical (was %s)\n",
                    path.c_str(), MeleeModeName(s.melee.mode));
        s.melee.mode = MeleeSettings::kButtonsPhysical;
    }
    // the marker missing: the caller writes the section back at once, so the migration runs once
    if (rewrite != nullptr) *rewrite = !kv.empty() && !from1cr;
    return true;
}

bool SaveVrSettings(const std::string& path, const VrSettings& s) {
    // bars_buttons_attack: the retired Handlebars-mode button row's key (read and dropped)
    return WriteIniSection(path, "vr", s.Serialize(), {"bars_buttons_attack"});
}

bool ApplyVrFlag(int argc, char** argv, int& i, VrSettings& s) {
    static const char* const kKeys[][2] = {
        {"--vr-eye-scale", "eye_scale"},   {"--vr-refresh", "refresh"},           {"--vr-msaa", "msaa"},
        {"--vr-horizon-lock", "horizon_lock"}, {"--vr-horizon-mode", "horizon_mode"}, {"--vr-view", "view"},             {"--vr-comfort", "comfort"},
        {"--vr-seat", "seat_height_cm"},   {"--vr-hud", "hud_size"},              {"--vr-foveation", "foveation"},
        {"--vr-draw-distance", "draw_distance"}, {"--vr-max-detail", "max_detail"}, {"--vr-world-scale", "world_scale"},
        {"--vr-multiview", "multiview"},   {"--vr-textures-smooth", "textures_smooth"},
        {"--vr-smooth-motion", "smooth_motion"}, {"--vr-fall-view", "fall_view"},
        {"--vr-bike-shake", "bike_shake"},
        {"--vr-view-pitch", "view_pitch"}, {"--vr-seat-back", "seat_back_cm"}};
    const std::string a = argv[i];
    for (const auto& k : kKeys)
        if (a == k[0]) {
            if (i + 1 >= argc) throw std::runtime_error(a + " needs a value");
            const std::string v = argv[++i];
            if (!s.Parse(k[1], v)) throw std::runtime_error(a + ": bad value '" + v + "'");
            return true;
        }
    if (ApplyMeleeFlag(argc, argv, i, s.melee)) return true; // --vr-combat and the combat flags (vr_melee_settings.h)
    if (ApplyWeaponsFlag(argc, argv, i, s.weapons)) return true; // --vr-holsters and the weapons' flags (vr_weapon_settings.h)
    if (ApplyPacingFlag(argc, argv, i)) return true;          // --vr-mock-timing, --vr-judder-log (vr_pacing.h)
    if (ApplyComfortFlag(argc, argv, i)) return true;         // --vr-mock-hz, --vr-mock-pad (vr_comfort.h)
    return ApplyBarsFlag(argc, argv, i, s.bars); // --vr-steering and the handlebars' flags (vr_bars_settings.h)
}

} // namespace rrgame
