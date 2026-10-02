// The handling setting (handling_settings.h).
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

#include "handling_settings.h"

#include "graphics_settings.h" // SettingsPath()
#include "settings_file.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <utility>

namespace rrgame {

namespace {

using rr::game::Handling;
using rr::game::HandlingMode;
using rr::game::HandlingSettings;

std::vector<std::pair<std::string, std::string>>& Overrides() { // the command line's values
    static std::vector<std::pair<std::string, std::string>> o;
    return o;
}

const char* ModeLabel(HandlingMode m) { return m == HandlingMode::kModern ? "Modern (SA-style)" : "Original"; }

std::string Pct(int p) { return std::to_string(p) + " %"; }

} // namespace

bool LoadHandling(const std::string& path, HandlingSettings& s) {
    IniPairs kv;
    if (!ReadIniSection(path, "handling", kv)) return false;
    for (const auto& [key, value] : kv)
        if (!s.Parse(key, value)) std::fprintf(stderr, "settings: %s: ignored '%s=%s'\n", path.c_str(), key.c_str(), value.c_str());
    return true;
}

bool SaveHandling(const std::string& path, const HandlingSettings& s) { return WriteIniSection(path, "handling", s.Serialize()); }

namespace {
bool& WheelieFlagGiven() { // a scripted run prints the wheelie settings only when a wheelie flag was given
    static bool given = false;
    return given;
}
} // namespace

bool ApplyHandlingFlag(int argc, char** argv, int& i) {
    const std::string a = argv[i];
    if (a.rfind("--wheelie", 0) == 0) WheelieFlagGiven() = true;

    const auto value = [&]() -> std::string {
        if (i + 1 >= argc) throw std::runtime_error(a + " needs a value");
        return argv[++i];
    };
    const auto set = [&](const char* key, const std::string& v) {
        HandlingSettings probe;
        if (!probe.Parse(key, v)) throw std::runtime_error(a + ": bad value '" + v + "'");
        Overrides().emplace_back(key, v);
    };
    if (a == "--handling") {
        const std::string v = value();
        set("mode", v);
        set("vr_mode", v);
    } else if (a == "--handling-desktop") set("mode", value());
    else if (a == "--handling-vr") set("vr_mode", value());
    else if (a == "--handling-steer-lag") set("steer_lag", value());
    else if (a == "--handling-curve") set("curve", value());
    else if (a == "--handling-turn-lag") set("turn_lag", value());
    else if (a == "--handling-lean-lag") set("lean_lag", value());
    else if (a == "--handling-max-lean") set("max_lean", value());
    else if (a == "--handling-lean-model") set("lean_model", value());   // sa | game
    else if (a == "--handling-camera-roll") set("camera_roll", value()); // 0..100
    // the wheelies (game/wheelie.h)
    else if (a == "--wheelie") set("wheelie", value()); // modern | on | off
    else if (a == "--wheelie-angle") set("wheelie_angle", value());
    else if (a == "--wheelie-loop") set("wheelie_loop", value());
    else if (a == "--wheelie-cars") set("wheelie_cars", value());
    else if (a == "--wheelie-view-pitch") set("wheelie_view_pitch", value());
    else if (a == "--wheelie-script") {
        const std::string v = value();
        if (!rr::game::PlayerWheelie().script.Parse(v)) throw std::runtime_error("--wheelie-script: bad script '" + v + "'");
    } else if (a == "--wheelie-log") rr::game::PlayerWheelie().csvPath = value();
    else if (a == "--wheelie-aim-car") { // a TEST input: from frame F, steer at the nearest car ahead
        rr::game::PlayerWheelie().aimCar = true;
        rr::game::PlayerWheelie().aimFrom = std::atol(value().c_str());
    }
    else if (a == "--steer-script") {
        const std::string v = value();
        if (!rr::game::PlayerHandling().script.Parse(v)) throw std::runtime_error("--steer-script: bad script '" + v + "'");
    } else if (a == "--handling-log") rr::game::PlayerHandling().csvPath = value();
    else return false;
    return true;
}

// the file is read ONCE a process, by whichever comes first - the front end (so the VR menu and the F10
// overlay show and edit the saved values before the first race, not the built-in defaults) or a race. A scripted run never reads or writes it.
namespace {
bool g_loaded = false, g_scripted = false;
std::string g_source; // for the race's first line (handling_modern.h settingsSource)

void LoadOnce() {
    if (g_loaded) return;
    g_loaded = true;
    HandlingSettings& s = Handling();
    s = HandlingSettings{};
    bool section = false, from1cm = false;
    {
        IniPairs kv;
        ReadIniSection(SettingsPath(), "handling", kv, &section);
        for (const auto& p : kv) from1cm = from1cm || p.first == "lean_model";
    }
    LoadHandling(SettingsPath(), s);
    g_source = section ? "[handling] of " + SettingsPath() : "the defaults: no [handling] in " + SettingsPath();
    if (section && !from1cm && s.vr != HandlingMode::kModern) {
        // A section written by an older build: its VR menu row flipped the mode on ANY trigger, so a trigger
        // pressed on a row already showing Modern saved Original unseen (such a file: vr_mode=original after
        // a race that rode Modern for 4094 frames). VR starts from Modern once more.
        s.vr = HandlingMode::kModern;
        g_source += " (VR mode Modern again: the section is from an older build)";
        std::printf("handling: [handling] of %s is from an older build - the VR handling starts from Modern again\n", SettingsPath().c_str());
    }
    if (section && !from1cm && !SettingsMarkersOff()) {
        // The migration marks itself at once: the complete section (with lean_model) is written back now, so it runs
        // once. Without this the marker waited for a change on the Handling page, and a player who never opened that
        // page had the migration - and the section's old key list - on every start.
        const bool saved = SaveHandling(SettingsPath(), s);
        std::printf("handling: [handling] of %s rewritten with its complete key list%s\n", SettingsPath().c_str(),
                    saved ? "" : " - FAILED");
    }
}
} // namespace

void StartHandlingFrontEnd(bool scripted) {
    g_scripted = scripted;
    if (!scripted) LoadOnce();
}

HandlingSettings& StartHandling(bool scripted) {
    HandlingSettings& s = Handling();
    g_scripted = scripted;
    if (scripted) {
        s = HandlingSettings{};
        s.desktop = s.vr = HandlingMode::kOriginal; // a scripted run: the original unless a flag says otherwise
        g_source = "a scripted run: Original unless a flag says otherwise";
    } else { // once a process: the front end and every race of it share them, the menus edit them
        LoadOnce();
    }
    for (const auto& o : Overrides()) s.Parse(o.first, o.second);
    std::printf("%s\n", s.Describe().c_str());
    rr::game::PlayerHandling().BeginRace();
    rr::game::PlayerWheelie().BeginRace();
    if (!scripted || WheelieFlagGiven()) std::printf("%s\n", s.wheelie.Describe().c_str());
    rr::game::PlayerHandling().settingsSource = g_source + (Overrides().empty() ? "" : ", the command line on top");
    return s;
}

float HandlingContactUp(const uint8_t* ram, uint32_t bike) {
    // the box's half height (axis 1, the model's y: +0x138, bike.h kHalfZ) below the box centre +0xB8
    if (ram == nullptr || bike < 0x80000000u || bike >= 0x801FFFF0u) return 0.0f;
    int32_t h = 0;
    std::memcpy(&h, ram + ((bike + 0x138u) & 0x1FFFFFu), 4);
    const float up = static_cast<float>(h) / 65536.0f;
    return up > 0.05f && up < 2.0f ? -up : -0.5f;
}

namespace {
// the model matrix as race_render.cpp RecordModelMatrix builds it (only its axes' directions and origin matter here)
rr::render::Mat4 BikeModel(const uint8_t* ram, uint32_t bike) {
    rr::render::Mat4 m;
    for (uint32_t c = 0; c < 3; ++c) {
        for (uint32_t k = 0; k < 3; ++k) {
            int16_t v = 0;
            std::memcpy(&v, ram + ((bike + 0x1B0u + 6u * c + 2u * k) & 0x1FFFFFu), 2);
            m.m[4 * c + k] = static_cast<float>(v) / 4096.0f;
        }
        m.m[4 * c + 3] = 0.0f;
    }
    for (uint32_t k = 0; k < 3; ++k) {
        int32_t v = 0;
        std::memcpy(&v, ram + ((bike + 0xB8u + 4u * k) & 0x1FFFFFu), 4);
        m.m[12 + k] = static_cast<float>(static_cast<double>(v) / 65536.0);
    }
    m.m[15] = 1.0f;
    return m;
}
} // namespace

VisualLean HandlingLean(const uint8_t* ram, uint32_t bike, float scale, float contactUp) {
    float roll = 0.0f;
    if (ram == nullptr || bike < 0x80000000u || bike >= 0x801FFFF0u || !rr::game::PlayerHandling().VisualRoll(roll))
        return VisualLean{};
    return VisualLean::ToRoll(BikeModel(ram, bike), std::clamp(scale, 0.0f, 1.0f) * roll, contactUp);
}

VisualLean HandlingGameRoll(const uint8_t* ram, uint32_t bike) {
    if (ram == nullptr || bike < 0x80000000u || bike >= 0x801FFFF0u) return VisualLean{};
    return VisualLean::From(BikeModel(ram, bike), 1.0f, 0.0f); // scale 1: no rotation, the roll measured
}

HandlingRow HandlingRowOf(HandlingRowId id, bool vrMenu) {
    const HandlingSettings& s = Handling();
    const rr::game::WheelieSettings& w = s.wheelie;
    switch (id) {
    case HandlingRowId::kMode: return {"Handling", ModeLabel(vrMenu ? s.vr : s.desktop), id};
    case HandlingRowId::kSteerLag: return {"  Steering smoothing (SA = 100)", Pct(s.steerLagPct), id};
    case HandlingRowId::kCurve:
        return {"  Steering response", s.curvePct == 0 ? "Linear" : (s.curvePct == 100 ? "Squared (SA)" : Pct(s.curvePct)), id};
    case HandlingRowId::kTurnLag:
        return {"  Turn smoothing (0 = original ramp)", s.turnLagPct == 0 ? "Off (original ramp)" : Pct(s.turnLagPct), id};
    case HandlingRowId::kLeanLag: return {"  Lean smoothing (SA = 100)", Pct(s.leanLagPct), id};
    case HandlingRowId::kMaxLean: return {"  Maximum drawn lean", std::to_string(s.maxLeanDeg) + " deg", id};
    case HandlingRowId::kLeanModel: return {"  Lean (SA = from the turn)", s.leanFromTurn ? "SA: from the turn" : "The game's roll", id};
    case HandlingRowId::kCameraRoll: return {"  VR view roll with the lean", Pct(s.cameraRollPct), id};
    case HandlingRowId::kWheelies:
        return {"Wheelies (pull back + throttle)",
                w.mode == rr::game::WheelieMode::kModern ? "With Modern handling"
                                                         : (w.mode == rr::game::WheelieMode::kAlways ? "Always" : "Off"),
                id};
    case HandlingRowId::kWheelieLift: return {"  Hand lift to start (both hands)", std::to_string(w.liftCm) + " cm", id};
    case HandlingRowId::kWheelieFull:
        return {"  Full-lift height", std::to_string(static_cast<int>(w.LiftFull() * 100.0f + 0.5f)) + " cm", id};
    case HandlingRowId::kWheelieHold: return {"  Hold to start", std::to_string(w.holdMs) + " ms", id};
    case HandlingRowId::kWheelieAngle: return {"  Wheelie angle (SA = 35)", std::to_string(w.holdDeg) + " deg", id};
    case HandlingRowId::kWheelieCars: return {"  Wheelie over cars (not knocked off)", w.overCars ? "On" : "Off", id};
    case HandlingRowId::kWheelieLoop: return {"  Wheelie loop-over crash", w.loopOver ? "On" : "Off", id};
    case HandlingRowId::kWheelieViewPitch: return {"  VR view pitch in a wheelie", Pct(w.viewPitchPct), id};
    }
    return {"", "", id};
}

std::vector<HandlingRow> HandlingMenuRows(bool vrMenu) {
    using I = HandlingRowId;
    // the F10 overlay's page: every row (the wheelie's hold time last, after the rows it always had); the VR menu's
    // Handling page: the handling itself - the view roll and the wheelies are on its Riding position page
    std::vector<I> ids = {I::kMode, I::kSteerLag, I::kCurve, I::kTurnLag, I::kLeanLag, I::kMaxLean, I::kLeanModel};
    if (!vrMenu)
        for (I i : {I::kCameraRoll, I::kWheelies, I::kWheelieAngle, I::kWheelieCars, I::kWheelieLoop, I::kWheelieViewPitch,
                    I::kWheelieHold})
            ids.push_back(i);
    std::vector<HandlingRow> r;
    for (I i : ids) r.push_back(HandlingRowOf(i, vrMenu));
    return r;
}

std::vector<HandlingRow> HandlingRidingRows() {
    using I = HandlingRowId;
    std::vector<HandlingRow> r;
    for (I i : {I::kCameraRoll, I::kWheelies, I::kWheelieLift, I::kWheelieFull, I::kWheelieHold, I::kWheelieAngle,
                I::kWheelieViewPitch, I::kWheelieCars, I::kWheelieLoop})
        r.push_back(HandlingRowOf(i, true));
    // (on this page the view roll is a row of its own, and the wheelie's input is the bars raised)
    r[0].label = "VR view roll with the lean";
    r[1].label = "Wheelies (raise the bars + throttle)";
    return r;
}

bool HandlingMenuActivate(int row, int direction, bool vrMenu, std::string& note) {
    const std::vector<HandlingRow> rows = HandlingMenuRows(vrMenu);
    if (row < 0 || row >= static_cast<int>(rows.size())) return false;
    return HandlingRowActivate(rows[static_cast<size_t>(row)].id, direction, vrMenu, note);
}

bool HandlingRowActivate(HandlingRowId id, int direction, bool vrMenu, std::string& note) {
    HandlingSettings& s = Handling();
    rr::game::WheelieSettings& w = s.wheelie;
    const int d = direction == 0 ? 1 : direction;
    const HandlingMode was = vrMenu ? s.vr : s.desktop;
    switch (id) {
    case HandlingRowId::kMode: {
        // Enter toggles; Left (the left trigger) = Original, Right (the right trigger) = Modern - a trigger
        // pressed "to be sure" on a row already showing Modern no longer flips it back to Original
        HandlingMode& m = vrMenu ? s.vr : s.desktop;
        if (direction < 0) m = HandlingMode::kOriginal;
        else if (direction > 0) m = HandlingMode::kModern;
        else m = m == HandlingMode::kModern ? HandlingMode::kOriginal : HandlingMode::kModern;
        break;
    }
    case HandlingRowId::kSteerLag: s.steerLagPct = std::clamp(s.steerLagPct + 25 * d, 0, 300); break;
    case HandlingRowId::kCurve: s.curvePct = std::clamp(s.curvePct + 25 * d, 0, 200); break;
    case HandlingRowId::kTurnLag: s.turnLagPct = std::clamp(s.turnLagPct + 25 * d, 0, 300); break;
    case HandlingRowId::kLeanLag: s.leanLagPct = std::clamp(s.leanLagPct + 25 * d, 0, 300); break;
    case HandlingRowId::kMaxLean: s.maxLeanDeg = std::clamp(s.maxLeanDeg + 5 * d, 20, 60); break;
    case HandlingRowId::kLeanModel: s.leanFromTurn = !s.leanFromTurn; break;
    case HandlingRowId::kCameraRoll: s.cameraRollPct = std::clamp(s.cameraRollPct + 10 * d, 0, 100); break;
    case HandlingRowId::kWheelies: { // With Modern handling -> Always -> Off (Left the other way)
        const int m = (static_cast<int>(w.mode) + (direction < 0 ? 2 : 1)) % 3;
        w.mode = static_cast<rr::game::WheelieMode>(m);
        std::printf("wheelie: the %s menu set wheelies %s\n", vrMenu ? "VR" : "F10", rr::game::WheelieModeName(w.mode));
        break;
    }
    case HandlingRowId::kWheelieLift: // the full lift stays at least 5 cm over the start
        w.liftCm = std::clamp(w.liftCm + d, 5, 40);
        w.liftFullCm = std::max(w.liftFullCm, w.liftCm + 5);
        break;
    case HandlingRowId::kWheelieFull: w.liftFullCm = std::clamp(static_cast<int>(w.LiftFull() * 100.0f + 0.5f) + 2 * d, w.liftCm + 5, 60); break;
    case HandlingRowId::kWheelieHold: w.holdMs = std::clamp(w.holdMs + 50 * d, 0, 1000); break;
    case HandlingRowId::kWheelieAngle: w.holdDeg = std::clamp(w.holdDeg + 5 * d, 20, 50); break;
    case HandlingRowId::kWheelieCars: w.overCars = !w.overCars; break;
    case HandlingRowId::kWheelieLoop: w.loopOver = !w.loopOver; break;
    case HandlingRowId::kWheelieViewPitch: w.viewPitchPct = std::clamp(w.viewPitchPct + 10 * d, 0, 100); break;
    default: return false;
    }
    const HandlingMode now = vrMenu ? s.vr : s.desktop;
    const bool modeRow = id == HandlingRowId::kMode;
    if (modeRow) // in the log (it applies from the next frame of a race, or the next race)
        std::printf("handling: the %s menu set the %s handling %s -> %s%s\n", vrMenu ? "VR" : "F10", vrMenu ? "VR" : "desktop",
                    rr::game::HandlingModeName(was), rr::game::HandlingModeName(now), g_scripted ? " (a scripted run: not saved)" : "");
    const std::string label = modeRow ? std::string("Handling: ") + ModeLabel(now) + " - on now" : "";
    if (g_scripted) note = modeRow ? label : "Set for this run (a scripted run saves nothing)";
    else if (SaveHandling(SettingsPath(), s)) note = modeRow ? label : "Saved: " + SettingsPath();
    else note = "Could not write " + SettingsPath();
    return true;
}

} // namespace rrgame
