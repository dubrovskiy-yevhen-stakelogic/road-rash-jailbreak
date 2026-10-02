// rrgame --settings-roundtrip-check <ini> (settings_check.h): the settings file through the game's own interactive
// paths - read at the start (the one-time migrations and their markers), one value changed on every menu (the VR
// menu's Riding position page for [vr] and [handling], the cheat page, the F10 overlay's graphics save), then read back
// into fresh settings and compared key by key. The file is changed in place: a gate runs it on a copy, twice.
#include "settings_check.h"

#include "cheat_menu.h"
#include "game/handling_modern.h"
#include "graphics_settings.h"
#include "handling_settings.h"
#include "settings_file.h"
#include "vr_menu.h"
#include "vr_settings.h"

#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

namespace rrgame {

namespace {

// the key=value lines of a Serialize() text
IniPairs PairsOf(const std::string& text) {
    IniPairs out;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        const size_t eq = line.find('=');
        if (eq != std::string::npos) out.emplace_back(line.substr(0, eq), line.substr(eq + 1));
    }
    return out;
}

// Every key the game writes for the section is in the file with the value the game holds; the file's section appears
// once. Prints one line, returns whether it held.
bool Compare(const std::string& path, const char* section, const std::string& serialized) {
    IniPairs file;
    bool present = false;
    ReadIniSection(path, section, file, &present);
    const IniPairs want = PairsOf(serialized);
    size_t found = 0, same = 0;
    std::string firstBad;
    for (const auto& [k, v] : want) {
        bool hit = false;
        for (const auto& [fk, fv] : file)
            if (fk == k) {
                hit = true;
                if (fv == v) ++same;
                else if (firstBad.empty()) firstBad = k + "=" + fv + " (held " + v + ")";
            }
        if (hit) ++found;
        else if (firstBad.empty()) firstBad = k + " missing";
    }
    std::vector<std::string> lines;
    ReadIniLines(path, lines);
    int headers = 0;
    for (std::string l : lines) { // (a byte-order mark before a header still counts it: the check sees what a reader would miss)
        if (l.rfind("\xEF\xBB\xBF", 0) == 0) l.erase(0, 3);
        headers += l == std::string("[") + section + "]" ? 1 : 0;
    }
    const bool ok = present && found == want.size() && same == want.size() && headers == 1;
    std::printf("settings round trip: [%s] %zu key(s) written, %zu in the file, %zu with the held value, %d header(s)%s%s -> %s\n",
                section, want.size(), found, same, headers, firstBad.empty() ? "" : "; first difference: ",
                firstBad.c_str(), ok ? "KEPT" : "LOST");
    return ok;
}

} // namespace

int SettingsRoundTripCheck(const std::string& path, bool startOnly) {
#ifdef _WIN32
    _putenv_s("RRJB_SETTINGS", path.c_str());
#else
    setenv("RRJB_SETTINGS", path.c_str(), 1);
#endif
    std::printf("settings round trip: %s\n", SettingsPath().c_str());
    // ---- the interactive start: the handling (once a process, with its migration), the VR settings over the product
    // defaults (with theirs), the cheats, the graphics
    StartHandlingFrontEnd(false);
    VrPrefs() = VrSettings::ProductDefaults();
    bool rewrite = false;
    LoadVrSettings(SettingsPath(), VrPrefs(), &rewrite);
    if (rewrite && !SettingsMarkersOff()) {
        const bool saved = SaveVrSettings(SettingsPath(), VrPrefs());
        std::printf("vr settings: [vr] of %s rewritten with its complete key list%s\n", SettingsPath().c_str(), saved ? "" : " - FAILED");
    }
    StartCheats(false);
    StartGraphics(false);
    std::printf("settings round trip: read - handling VR %s, wheelie lift %d cm, VR seat %+d cm, steering %s\n",
                rr::game::HandlingModeName(rr::game::Handling().vr), rr::game::Handling().wheelie.liftCm,
                VrPrefs().seatHeightCm, VrPrefs().bars.steering == BarsSettings::kHandlebars ? "handlebars" : "stick");
    if (startOnly) return 0; // (the start alone: a player who opens no menu)
    // ---- one change on every menu
    {   // the VR menu: main page row 6 (Riding position), A; the right trigger on Seat height ([vr], the host saves it);
        // down to "Hand lift to start" ([handling], saved by its page)
        VrMenu menu;
        menu.Open(0);
        const std::vector<float> rates = {72.0f, 90.0f};
        VrMenuInput step;
        for (int k = 0; k < 6; ++k) {
            step = VrMenuInput{};
            step.down = true;
            menu.Update(step, rates);
        }
        step = VrMenuInput{};
        step.confirm = true;
        menu.Update(step, rates);
        step = VrMenuInput{};
        step.right = true;
        const VrMenuActions a = menu.Update(step, rates);
        std::printf("settings round trip: VR menu %s\n", menu.SelectedRowText().c_str());
        if (a.settingsChanged) SaveVrSettings(SettingsPath(), VrPrefs()); // (the host's SaveSettings)
        for (int k = 0; k < 8; ++k) {
            step = VrMenuInput{};
            step.down = true;
            menu.Update(step, rates);
        }
        step = VrMenuInput{};
        step.right = true;
        menu.Update(step, rates);
        std::printf("settings round trip: VR menu %s\n", menu.SelectedRowText().c_str());
    }
    {   // the cheat page: No police (the page saves [cheats] itself)
        const std::vector<CheatRow> rows = CheatMenuRows();
        for (size_t r = 0; r < rows.size(); ++r)
            if (rows[r].label == "No police") {
                std::string note;
                CheatMenuActivate(static_cast<int>(r), +1, note);
                std::printf("settings round trip: cheats No police -> %s (%s)\n", CheatMenuRows()[r].value.c_str(), note.c_str());
            }
    }
    {   // the F10 overlay's graphics save (pc_overlay.cpp): MSAA 4x -> 8x
        Graphics().msaa = Graphics().msaa == 8 ? 4 : 8;
        SaveGraphics(SettingsPath(), Graphics());
        std::printf("settings round trip: graphics MSAA %dx saved\n", Graphics().msaa);
    }
    // ---- read back into fresh settings, and every key of every section
    bool ok = true;
    ok = Compare(SettingsPath(), "handling", rr::game::Handling().Serialize()) && ok;
    ok = Compare(SettingsPath(), "vr", VrPrefs().Serialize()) && ok;
    ok = Compare(SettingsPath(), "cheats", rr::game::Cheats().Serialize()) && ok;
    ok = Compare(SettingsPath(), "graphics", Graphics().Serialize()) && ok;
    {
        rr::game::HandlingSettings h;
        LoadHandling(SettingsPath(), h);
        VrSettings v = VrSettings::ProductDefaults();
        LoadVrSettings(SettingsPath(), v);
        rr::game::CheatSettings c;
        LoadCheats(SettingsPath(), c);
        GraphicsSettings g = GraphicsSettings::Modern();
        LoadGraphics(SettingsPath(), g);
        const bool same = h.Serialize() == rr::game::Handling().Serialize() && v.Serialize() == VrPrefs().Serialize() &&
                          c.Serialize() == rr::game::Cheats().Serialize() && g.Serialize() == Graphics().Serialize();
        std::printf("settings round trip: reloaded into fresh settings - %s\n", same ? "identical" : "DIFFERENT");
        ok = ok && same;
    }
    std::printf("settings round trip: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

} // namespace rrgame
