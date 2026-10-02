// The VR settings menu (vr_menu.h).
#include "rrformats/hd_pack.h"
#include "vr_menu.h"

#include "cheat_menu.h" // the Cheats page
#include "handling_settings.h" // the Handling page
#include "vr_bike_shake.h"     // the Graphics page's bike vibration
#include "vr_holsters.h"       // the Weapons and holsters page
#include "vr_horizon.h"        // the Graphics page's bike pitch
#include "vr_weapon_calib.h"   // the grip calibration page

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iterator>

namespace rrgame {

namespace {

enum Page { kMain = 0, kGraphics, kControls, kBars, kCombat, kCheats, kHandling, kWeapons, kCalib, kRiding }; // kCheats: cheat_menu.h; kBars: the handlebars' options (vr_bars_settings.h),
                                                              // kCombat: the blows (vr_melee_settings.h)
                                                              // kWeapons / kCalib: the holsters and the grip (vr_weapon_settings.h)
constexpr int kControlsFirst = 5; // the Controls page's rows before the bindings: Steering, Handlebar / Combat options, Handling, Weapons
// (the Weapons page's "Calibrate the grip" row: vr_weapon_settings.h kWeaponsCalibRow)
// The main page's submenu rows (Weapons and holsters and Combat options up from Controls, after the values)
// Row 6 opens "Riding position" (the seat, the eye, the bars, the lean and pitch of the view, the wheelies).
constexpr int kMainRiding = 6;
constexpr int kMainWeapons = 9, kMainCombat = 10, kMainGraphics = 11, kMainControls = 12, kMainCheats = 13, kMainQuit = 14;

// The Riding position page: the rows from [vr] first, then [handling]'s (handling_settings.h HandlingRidingRows).
enum RidingRow { kRideSeatHeight = 0, kRideSeatBack, kRideEye, kRideBarsHeight, kRideVisualLean, kRideViewPitch, kRideVrRows };

std::string OnOff(bool b) { return b ? "On" : "Off"; }

// Weapons and holsters / Combat options are reached from the main page and from Controls; Back returns to
// the row they were opened from
void ReturnTo(int from, int mainRow, int controlsRow, int& page, int& selected) {
    page = from == kMain ? kMain : kControls;
    selected = from == kMain ? mainRow : controlsRow;
}

template <size_t N>
int Step(int value, const int (&values)[N], int direction) {
    const auto it = std::find(std::begin(values), std::end(values), value);
    const int index = it == std::end(values) ? 0 : static_cast<int>(it - std::begin(values));
    return values[std::clamp(index + direction, 0, static_cast<int>(N) - 1)];
}

} // namespace

void VrMenu::Open(int page) {
    open_ = true;
    page_ = std::clamp(page, 0, static_cast<int>(kRiding));
    selected_ = 0;
    capturing_ = -1;
    capture_.Cancel();
    note_.clear();
}

std::vector<VrMenu::Row> VrMenu::Rows(const std::vector<float>& refreshRates) const {
    const VrSettings& s = VrPrefs();
    std::vector<Row> r;
    static const char* const kDist[3] = {"Original", "Extended", "Maximum"};
    static const char* const kFov[4] = {"Off", "Low", "Medium", "High"};
    static const char* const kHud[3] = {"Small", "Medium", "Large"};
    char b[64];
    switch (page_) {
    case kMain:
        r.push_back({"Resume", ""});
        r.push_back({"Recentre view", ""});
        r.push_back({"View", s.headView ? "Rider's head" : "Chase camera"});
        std::snprintf(b, sizeof(b), "%d %%", s.horizonLock);
        r.push_back({"Horizon lock", b});
        r.push_back({"Horizon lock levels", s.horizonMode == VrSettings::kHorizonOff ? "Off"
                                            : s.horizonMode == VrSettings::kHorizonRollPitch ? "Roll + pitch" : "Roll only"});
        r.push_back({"Comfort vignette", OnOff(s.comfort)});
        std::snprintf(b, sizeof(b), "Seat %+d cm", s.seatHeightCm);
        r.push_back({"Riding position", b, true}); // kMainRiding
        r.push_back({"HUD size", kHud[std::clamp(s.hudSize, 0, 2)]});
        std::snprintf(b, sizeof(b), "%d %%", s.vibration);
        r.push_back({"Vibration", b});
        r.push_back({"Weapons and holsters", s.weapons.holsters ? "Holsters, grip" : "In the hand, grip", true}); // kMainWeapons
        r.push_back({"Combat options", MeleeModeName(s.melee.mode), true});                                  // kMainCombat
        r.push_back({"Graphics and performance", "", true});
        r.push_back({"Controls", "", true});
        r.push_back({"Cheats", rr::game::Cheats().AnyOn() ? "ON" : "", true});
        r.push_back({"Quit game", ""});
        break;
    case kGraphics:
        std::snprintf(b, sizeof(b), "%d %% (next start)", s.eyeScale);
        r.push_back({"Eye resolution", b});
        std::snprintf(b, sizeof(b), "%d Hz%s", s.refreshHz, refreshRates.empty() ? " (not offered)" : "");
        r.push_back({"Refresh rate", b});
        r.push_back({"Anti-aliasing (MSAA)", s.msaa > 1 ? std::to_string(s.msaa) + "x" : "Off"});
        r.push_back({"Draw distance", kDist[std::clamp(s.drawDistance, 0, 2)]});
        r.push_back({"Maximum detail", OnOff(s.maxDetail)});
        r.push_back({"Textures", s.smoothTextures ? "Smooth + mipmaps" : "PS1 nearest"});
        r.push_back({"Foveation", kFov[std::clamp(s.foveation, 0, 3)]});
        r.push_back({"Single-pass stereo", std::string(OnOff(s.multiview)) + " (next start)"});
        r.push_back({"Smooth motion", OnOff(s.smoothMotion)});
        r.push_back({"View off the bike", s.fallFixed ? "Fixed, level" : "Chase camera"});
        r.push_back({"Bike vibration (first person)", BikeShakeName(s.bikeShake)});
        // HD media (hd_media.h, docs/HD-MEDIA.md): the pack's pictures, fonts, HUD and films
        r.push_back({"HD textures and media", std::string(OnOff(s.hdMedia)) + (rr::hd::LoadedPack() ? "" : " (no HD pack)")});
        r.push_back({"Back", ""});
        break;
    case kControls: {
        r.push_back({"Steering", s.bars.steering == BarsSettings::kHandlebars ? "Handlebars (hands)" : "Stick"});
        r.push_back({"Handlebar options", "", true});
        r.push_back({"Combat options", MeleeModeName(s.melee.mode), true});
        r.push_back({"Handling", HandlingMenuRows(true).front().value, true}); // Original / Modern
        r.push_back({"Weapons and holsters", s.weapons.holsters ? "Holsters" : "In the hand", true});
        const rr::platform::Bindings& vb = rr::platform::VrBindings();
        for (int a = 0; a < rr::platform::kGameActions; ++a) {
            const auto act = static_cast<rr::platform::GameAction>(a);
            r.push_back({rr::platform::Action(act).label,
                         capturing_ == a ? "press a button..." : rr::platform::BindingLabel(vb, act, true, rr::platform::PadStyle::Touch)});
        }
        r.push_back({"Reset controls to defaults", ""});
        r.push_back({"Back", ""});
        break;
    }
    case kBars:
        for (const BarsMenuRow& row : BarsMenuRows(s.bars)) r.push_back({row.label, row.value});
        r.push_back({"Back", ""});
        break;
    case kCombat:
        for (const MeleeMenuRow& row : MeleeMenuRows(s.melee)) r.push_back({row.label, row.value});
        for (const WeaponMenuRow& row : SwingMenuRows(s.weapons)) r.push_back({row.label, row.value});
        r.push_back({"Back", ""});
        break;
    case kWeapons:
        for (const WeaponMenuRow& row : WeaponsMenuRows(s.weapons)) r.push_back({row.label, row.value, row.submenu});
        r.push_back({"Back", ""});
        break;
    case kCalib:
        for (const WeaponMenuRow& row : CalibMenuRows(s.weapons)) r.push_back({row.label, row.value, row.submenu});
        r.push_back({"Back", ""});
        break;
    case kCheats:
        for (const CheatRow& row : CheatMenuRows()) r.push_back({row.label, row.value});
        r.push_back({"Back", ""});
        break;
    case kHandling:
        for (const HandlingRow& row : HandlingMenuRows(true)) r.push_back({row.label, row.value});
        r.push_back({"Back", ""});
        break;
    case kRiding:
        std::snprintf(b, sizeof(b), "%+d cm", s.seatHeightCm);
        r.push_back({"Seat height", b});
        if (s.seatBackCm == 0) std::snprintf(b, sizeof(b), "0 cm");
        else std::snprintf(b, sizeof(b), "%d cm %s", std::abs(s.seatBackCm), s.seatBackCm > 0 ? "back" : "forward");
        r.push_back({"Seat forward / back", b});
        r.push_back({"Eye", s.bars.eyeOnBike ? "Fixed on the bike" : "Rider's head (animated)"});
        std::snprintf(b, sizeof(b), "%+d cm", s.bars.heightCm);
        r.push_back({"Bars height", b});
        std::snprintf(b, sizeof(b), "%d %% of the lean", s.bars.visualLean);
        r.push_back({"Visual bike lean (Original)", b});
        r.push_back({"Bike pitch (first person)", ViewPitchName(s.viewPitch)});
        for (const HandlingRow& row : HandlingRidingRows()) r.push_back({row.label, row.value});
        r.push_back({"Back", ""});
        break;
    default:
        break;
    }
    return r;
}

void VrMenu::Activate(int direction, const std::vector<float>& refreshRates, VrMenuActions& out) {
    VrSettings& s = VrPrefs();
    const int d = direction == 0 ? 1 : direction;
    bool changed = true;
    switch (page_) {
    case kMain:
        switch (selected_) {
        case 0: if (direction == 0) { open_ = false; out.closed = true; } changed = false; break;
        case 1: if (direction == 0) { out.recentre = true; note_ = "View recentred"; } changed = false; break;
        case 2: s.headView = !s.headView; break;
        case 3: s.horizonLock = std::clamp(s.horizonLock + 10 * d, 0, 100); break;
        case 4: s.horizonMode = (s.horizonMode + (d > 0 ? 1 : 2)) % 3; break; // off -> roll -> roll + pitch
        case 5: s.comfort = !s.comfort; break;
        case kMainRiding: if (direction == 0) { page_ = kRiding; selected_ = 0; } changed = false; break;
        case 7: s.hudSize = std::clamp(s.hudSize + d, 0, 2); break;
        case 8: s.vibration = std::clamp(s.vibration + 25 * d, 0, 100); break;
        case kMainWeapons: if (direction == 0) { page_ = kWeapons; selected_ = 0; weaponsFrom_ = kMain; } changed = false; break;
        case kMainCombat: if (direction == 0) { page_ = kCombat; selected_ = 0; combatFrom_ = kMain; } changed = false; break;
        case kMainGraphics: if (direction == 0) { page_ = kGraphics; selected_ = 0; } changed = false; break;
        case kMainControls: if (direction == 0) { page_ = kControls; selected_ = 0; } changed = false; break;
        case kMainCheats: if (direction == 0) { page_ = kCheats; selected_ = 0; } changed = false; break;
        case kMainQuit: if (direction == 0) out.quit = true; changed = false; break;
        default: changed = false; break;
        }
        break;
    case kGraphics:
        switch (selected_) {
        case 0: {
            static const int kScales[] = {50, 60, 70, 80, 90, 100, 110, 120, 130, 140, 150, 175, 200};
            s.eyeScale = Step(s.eyeScale, kScales, d);
            note_ = "Eye resolution applies at the next start";
            break;
        }
        case 1: {
            if (refreshRates.empty()) { changed = false; break; }
            std::vector<int> rates;
            for (float f : refreshRates) rates.push_back(static_cast<int>(std::lround(f)));
            std::sort(rates.begin(), rates.end());
            auto it = std::find(rates.begin(), rates.end(), s.refreshHz);
            int index = it == rates.end() ? 0 : static_cast<int>(it - rates.begin());
            index = std::clamp(index + d, 0, static_cast<int>(rates.size()) - 1);
            s.refreshHz = rates[static_cast<size_t>(index)];
            out.refreshChanged = true;
            break;
        }
        case 2: {
            static const int kMsaa[] = {1, 2, 4};
            s.msaa = Step(s.msaa, kMsaa, d);
            out.msaaChanged = true;
            break;
        }
        case 3: s.drawDistance = std::clamp(s.drawDistance + d, 0, 2); out.graphicsChanged = true; break;
        case 4: s.maxDetail = !s.maxDetail; out.graphicsChanged = true; break;
        case 5: s.smoothTextures = !s.smoothTextures; out.graphicsChanged = true; break;
        case 6: s.foveation = std::clamp(s.foveation + d, 0, 3); out.foveationChanged = true; break;
        case 7: s.multiview = !s.multiview; note_ = "Single-pass stereo applies at the next start"; break;
        case 8: // the race drawn between its steps at the display's rate
            s.smoothMotion = !s.smoothMotion;
            note_ = s.smoothMotion ? "Smooth motion: drawn between the game's steps" : "Smooth motion off: one step a frame";
            break;
        case 9: s.fallFixed = !s.fallFixed; break; // the view while the rider is off the bike
        case 10: // vr_bike_shake.h: the player's own bike in the head view - Off, Low, Original
            s.bikeShake = std::clamp(s.bikeShake + d, 0, 2);
            note_ = s.bikeShake == kShakeOff ? "Bike vibration off: the body's pitch spring held at rest"
                    : s.bikeShake == kShakeLow ? "Bike vibration low: a slow, gentle pitch" : "Bike vibration: the original's";
            break;
        case 11: s.hdMedia = !s.hdMedia; out.graphicsChanged = true; break; // HD media (hd_media.h)
        default:
            changed = false;
            if (direction == 0) { page_ = kMain; selected_ = kMainGraphics; }
        }
        break;
    case kControls: {
        changed = false;
        const int n = rr::platform::kGameActions;
        rr::platform::Bindings& vb = rr::platform::VrBindings();
        const int action = selected_ - kControlsFirst;
        if (selected_ == 0) { // the steering: the stick or the handlebars (vr_handlebars.h)
            s.bars.steering = s.bars.steering == BarsSettings::kHandlebars ? BarsSettings::kStick : BarsSettings::kHandlebars;
            note_ = s.bars.steering == BarsSettings::kHandlebars ? "Handlebars: grab the grips with the grip buttons"
                                                                 : "Stick: the left stick steers";
            changed = true;
        } else if (selected_ == 1) {
            if (direction == 0) { page_ = kBars; selected_ = 0; }
        } else if (selected_ == 2) { // the blows (vr_melee.h)
            if (direction == 0) { page_ = kCombat; selected_ = 0; combatFrom_ = kControls; }
        } else if (selected_ == 3) { // the handling (handling_settings.h)
            if (direction == 0) { page_ = kHandling; selected_ = 0; }
        } else if (selected_ == 4) { // the holsters, the grip (vr_weapon_settings.h)
            if (direction == 0) { page_ = kWeapons; selected_ = 0; weaponsFrom_ = kControls; }
        } else if (action < n) {
            if (direction == 0) {
                capturing_ = action;
                capture_.Begin(static_cast<rr::platform::GameAction>(action), true, 0);
            }
        } else if (action == n && direction == 0) {
            vb = rr::platform::DefaultVrBindings();
            rr::platform::SaveBindings(rr::platform::BindingsPath(), vb, "vr_controls");
            note_ = "Controls reset to the VR defaults";
        } else if (action == n + 1 && direction == 0) {
            page_ = kMain;
            selected_ = kMainControls;
        }
        break;
    }
    case kBars:
        if (selected_ < static_cast<int>(BarsMenuRows(s.bars).size())) {
            changed = BarsMenuActivate(s.bars, selected_, direction);
        } else {
            changed = false;
            if (direction == 0) { page_ = kControls; selected_ = 1; }
        }
        break;
    case kCombat: {
        const int melee = static_cast<int>(MeleeMenuRows(s.melee).size());
        if (selected_ < melee) {
            changed = MeleeMenuActivate(s.melee, selected_, direction);
        } else if (selected_ < melee + static_cast<int>(SwingMenuRows(s.weapons).size())) {
            changed = SwingMenuActivate(s.weapons, selected_ - melee, direction);
        } else {
            changed = false;
            if (direction == 0) ReturnTo(combatFrom_, kMainCombat, 2, page_, selected_);
        }
        break;
    }
    case kWeapons: // the holsters (vr_holsters.h)
        if (selected_ < static_cast<int>(WeaponsMenuRows(s.weapons).size())) {
            note_.clear();
            const int r = WeaponsMenuActivate(s.weapons, selected_, direction, note_);
            changed = r == 1;
            if (r == 2) { // the grip calibration: the weapon in hand (or the last one) in the hand holding it
                page_ = kCalib;
                selected_ = 0;
                SetCalibPageOpen(true, HolsterHeldWeapon(), HolsterHeldHand());
                note_ = "Squeeze the other hand's grip to move the weapon in the hand";
            }
        } else {
            changed = false;
            if (direction == 0) ReturnTo(weaponsFrom_, kMainWeapons, 4, page_, selected_);
        }
        break;
    case kCalib: // the weapon's grip (vr_weapon_calib.h)
        if (selected_ < static_cast<int>(CalibMenuRows(s.weapons).size())) {
            note_.clear();
            changed = CalibMenuActivate(s.weapons, selected_, direction, note_);
        } else {
            changed = false;
            if (direction == 0) {
                page_ = kWeapons;
                selected_ = kWeaponsCalibRow;
                SetCalibPageOpen(false, -1, -1);
            }
        }
        break;
    case kHandling: // handling_settings.h saves the [handling] section itself
        changed = false;
        if (selected_ < static_cast<int>(HandlingMenuRows(true).size())) {
            note_.clear();
            HandlingMenuActivate(selected_, direction, true, note_);
        } else if (direction == 0) {
            page_ = kControls;
            selected_ = 3;
        }
        break;
    case kRiding: { // [vr] rows (saved by the host: `changed`), then [handling] rows (handling_settings.h saves them)
        const std::vector<HandlingRow> handling = HandlingRidingRows();
        switch (selected_) {
        case kRideSeatHeight: s.seatHeightCm = std::clamp(s.seatHeightCm + 5 * d, -40, 40); break;
        case kRideSeatBack: s.seatBackCm = std::clamp(s.seatBackCm + 5 * d, -20, 40); break; // the right trigger - back
        case kRideEye: s.bars.eyeOnBike = !s.bars.eyeOnBike; break;
        case kRideBarsHeight: s.bars.heightCm = std::clamp(s.bars.heightCm + 5 * d, -30, 30); break;
        case kRideVisualLean: s.bars.visualLean = std::clamp(s.bars.visualLean + 10 * d, 0, 100); break;
        case kRideViewPitch: // vr_horizon.h: the head view's bike pitch - the road only, low, the original's
            s.viewPitch = std::clamp(s.viewPitch + d, 0, 2);
            note_ = s.viewPitch == kPitchRoad ? "Bike pitch: the road's slope only, no wheelie / stoppie nod"
                    : s.viewPitch == kPitchLow ? "Bike pitch low: the road's slope, a quarter of the wheelie / stoppie, slow"
                                               : "Bike pitch: the original's (the view nods with the bike)";
            break;
        default:
            changed = false;
            if (selected_ - kRideVrRows < static_cast<int>(handling.size())) {
                note_.clear();
                HandlingRowActivate(handling[static_cast<size_t>(selected_ - kRideVrRows)].id, direction, true, note_);
            } else if (direction == 0) {
                page_ = kMain;
                selected_ = kMainRiding;
            }
        }
        break;
    }
    case kCheats: // cheat_menu.h saves the [cheats] section itself
        changed = false;
        if (selected_ < static_cast<int>(CheatMenuRows().size())) {
            note_.clear();
            CheatMenuActivate(selected_, direction, note_);
        } else if (direction == 0) {
            page_ = kMain;
            selected_ = kMainCheats;
        }
        break;
    default:
        changed = false;
    }
    if (changed) out.settingsChanged = true;
}

VrMenuActions VrMenu::Update(const VrMenuInput& in, const std::vector<float>& refreshRates) {
    VrMenuActions out;
    rates_ = refreshRates;
    if (!open_) return out;
    if (capturing_ >= 0) { // the controls page: the next Touch button (after everything is let go) is the binding
        rr::platform::Bindings& vb = rr::platform::VrBindings();
        const auto result = capture_.Feed(vb, in.pad, nullptr);
        if (result == rr::platform::BindingCapture::Result::Captured) {
            rr::platform::SaveBindings(rr::platform::BindingsPath(), vb, "vr_controls");
            note_ = "Saved: " + rr::platform::BindingsPath();
            capturing_ = -1;
        } else if (result == rr::platform::BindingCapture::Result::Cancelled || in.close) {
            capture_.Cancel();
            capturing_ = -1;
        }
        return out;
    }
    const int rows = static_cast<int>(Rows(refreshRates).size());
    if (in.close) {
        open_ = false;
        out.closed = true;
        return out;
    }
    if (in.up) selected_ = (selected_ + rows - 1) % rows;
    if (in.down) selected_ = (selected_ + 1) % rows;
    if (in.left) Activate(-1, refreshRates, out);
    if (in.right) Activate(+1, refreshRates, out);
    if (in.confirm) Activate(0, refreshRates, out);
    if (in.back) {
        if (page_ == kMain) {
            open_ = false;
            out.closed = true;
        } else {
            if (page_ == kCalib) {
                SetCalibPageOpen(false, -1, -1);
                selected_ = kWeaponsCalibRow;
                page_ = kWeapons;
            } else if (page_ == kCombat) { // back where it was opened from
                ReturnTo(combatFrom_, kMainCombat, 2, page_, selected_);
            } else if (page_ == kWeapons) {
                ReturnTo(weaponsFrom_, kMainWeapons, 4, page_, selected_);
            } else if (page_ == kBars || page_ == kHandling) {
                selected_ = page_ == kBars ? 1 : 3;
                page_ = kControls;
            } else if (page_ == kCheats) {
                selected_ = kMainCheats;
                page_ = kMain;
            } else if (page_ == kRiding) {
                selected_ = kMainRiding;
                page_ = kMain;
            } else {
                selected_ = page_ == kGraphics ? kMainGraphics : kMainControls;
                page_ = kMain;
            }
        }
    }
    return out;
}

std::string VrMenu::SelectedRowText() const {
    if (!open_) return {};
    const std::vector<Row> rows = Rows(rates_);
    if (selected_ < 0 || selected_ >= static_cast<int>(rows.size())) return {};
    const Row& r = rows[static_cast<size_t>(selected_)];
    return std::to_string(page_) + "/" + r.label + ": " + r.value;
}

void VrMenu::Draw(rr::render::TextOverlay& text, int w, int h, const std::string& status) const {
    if (!open_ || !text.Ready()) return;
    const std::vector<Row> rows = Rows(rates_);
    const float scale = std::clamp(static_cast<float>(h) / 560.0f, 0.8f, 2.5f);
    const float lh = text.LineHeight(scale) * 1.2f, pad = 18 * scale;
    // the calibration page shows its weapon in the hand while it is drawn (vr_weapon_calib.h)
    SetCalibPageOpen(page_ == kCalib, HolsterHeldWeapon(), HolsterHeldHand());
    static const char* const kTitles[10] = {"ROAD RASH: JAILBREAK VR - SETTINGS", "VR / GRAPHICS AND PERFORMANCE",
                                           "VR / CONTROLS  (Touch controllers)", "VR / CONTROLS / HANDLEBARS",
                                           "VR / COMBAT OPTIONS", "VR / CHEATS  (all off by default)",
                                           "VR / CONTROLS / HANDLING", "VR / WEAPONS AND HOLSTERS",
                                           "VR / WEAPONS / CALIBRATE THE GRIP", "VR / RIDING POSITION"};
    const std::string hint = page_ == kControls ? "Stick: choose   A: rebind   B: back   Menu: close"
                                                : "Stick: choose   Triggers: change   A: select   B: back   Menu: close";
    const float panelW = static_cast<float>(w) - 2 * pad;
    const float panelH = std::min(static_cast<float>(h) - 2 * pad, lh * static_cast<float>(rows.size() + 4) + 2 * pad);
    const float x0 = pad, y0 = (static_cast<float>(h) - panelH) * 0.5f;
    text.Begin(w, h);
    text.Rect(0, 0, static_cast<float>(w), static_cast<float>(h), 0.0f, 0.0f, 0.0f, 0.55f);
    text.Rect(x0, y0, panelW, panelH, 0.06f, 0.07f, 0.09f, 0.95f);
    text.Rect(x0, y0, panelW, lh + pad, 0.55f, 0.12f, 0.08f, 0.97f);
    text.Text(x0 + pad, y0 + pad * 0.5f, kTitles[std::clamp(page_, 0, static_cast<int>(kRiding))], 1.0f, 0.95f, 0.85f, 1.0f, scale);
    float y = y0 + lh + pad * 1.5f;
    const int visible = std::max(3, static_cast<int>((panelH - 4 * lh - 2 * pad) / lh + 0.05f));
    const int first = std::clamp(selected_ - visible + 1, 0, std::max(0, static_cast<int>(rows.size()) - visible));
    for (int i = first; i < static_cast<int>(rows.size()) && i < first + visible; ++i) {
        const Row& r = rows[static_cast<size_t>(i)];
        const bool sel = i == selected_;
        if (sel) text.Rect(x0 + pad * 0.5f, y - 2 * scale, panelW - pad, lh, 0.85f, 0.55f, 0.15f, 0.9f);
        const float c = sel ? 0.05f : 0.92f;
        text.Text(x0 + pad, y, r.label + (r.submenu ? "  >" : ""), c, c, c, 1.0f, scale);
        if (!r.value.empty()) {
            const std::string v = sel && !r.submenu ? "< " + r.value + " >" : r.value;
            text.Text(x0 + panelW - pad - text.TextWidth(v, scale), y, v, sel ? 0.05f : 1.0f, sel ? 0.05f : 0.8f,
                      sel ? 0.05f : 0.4f, 1.0f, scale);
        }
        y += lh;
    }
    y += lh * 0.4f;
    text.Text(x0 + pad, y, hint, 0.7f, 0.7f, 0.7f, 1.0f, scale * 0.8f);
    const std::string line = !note_.empty() ? note_ : status;
    if (!line.empty()) text.Text(x0 + pad, y + lh * 0.9f, line, 0.6f, 0.9f, 0.6f, 1.0f, scale * 0.8f);
    text.End();
}

} // namespace rrgame
