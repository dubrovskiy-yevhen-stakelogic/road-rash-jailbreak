#include "platform/input_bindings.h"

#include "platform/app_paths.h" // BindingsPath: next to the executable (Windows), the saves folder (Android)
#include "platform/vk_codes.h"  // the key codes (Windows' own; the same numbers elsewhere)

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

namespace rr::platform {

namespace {

// THE TABLE: every game action, the original's pad bit it presses, and its defaults. PadButton::Count / 0 = an empty slot.
struct ActionRow {
    ActionInfo info;
    PadButton pad[kBindSlots];
    uint8_t key[kBindSlots];
};
constexpr PadButton kNone = PadButton::Count;
const ActionRow kActions[kGameActions] = {
    {{GameAction::Throttle, "throttle", "Throttle", ps1::kCross, "Cross",
      "control 3: bike +0x230 0x1|0x2; on foot walk forward"},
     {PadButton::Cross, PadButton::R2}, {VK_UP, 'W'}},
    {{GameAction::Brake, "brake", "Brake", ps1::kSquare, "Square", "control 4: bike +0x230 0x20|0x40; on foot walk back"},
     {PadButton::Square, PadButton::L2}, {VK_DOWN, 'S'}},
    {{GameAction::SteerLeft, "steer_left", "Steer left", ps1::kLeft, "Left", "control 5: +0x230 0x80|0x100"},
     {PadButton::DLeft, PadButton::LsLeft}, {VK_LEFT, 'A'}},
    {{GameAction::SteerRight, "steer_right", "Steer right", ps1::kRight, "Right", "control 6: +0x230 0x80|0x200"},
     {PadButton::DRight, PadButton::LsRight}, {VK_RIGHT, 'D'}},
    {{GameAction::Attack1, "attack1", "Punch / swing (R1)", ps1::kR1, "R1",
      "combat action 1: fists command 32 (the punch), armed 142 (the swing)"},
     {PadButton::R1, kNone}, {'Z', 0}},
    {{GameAction::Attack2, "attack2", "Punch 2 (L1)", ps1::kL1, "L1", "combat action 2: fists command 36, armed 146"},
     {PadButton::L1, kNone}, {'X', 0}},
    {{GameAction::Attack3, "attack3", "Kick (R2)", ps1::kR2, "R2", "combat action 3: command 71 (the kick)"},
     {PadButton::Triangle, kNone}, {'Q', 0}},
    {{GameAction::ModUp, "mod_up", "Up (attack modifier)", ps1::kUp, "Up",
      "held with an attack: actions 4 / 5 / 6 (R2 / R1 / L1 + Up); on foot walk forward"},
     {PadButton::DUp, PadButton::LsUp}, {'R', 0}},
    {{GameAction::ModDown, "mod_down", "Down (attack modifier)", ps1::kDown, "Down",
      "held with an attack: actions 7 / 8 (R1 / R2 + Down); control 7; on foot walk back"},
     {PadButton::DDown, PadButton::LsDown}, {'F', 0}},
    {{GameAction::LookBack, "look_back", "Look back", ps1::kCircle, "Circle", "control 2, held: the look-behind view"},
     {PadButton::Circle, kNone}, {'V', 0}},
    {{GameAction::Camera, "camera", "Camera", ps1::kSelect, "Select", "control 0, pressed: the next chase camera"},
     {PadButton::Select, PadButton::Touchpad}, {'C', 0}},
    {{GameAction::Taunt, "taunt", "Taunt (L2)", ps1::kL2, "L2", "control 8: the rider's taunt"},
     {PadButton::L3, kNone}, {'T', 0}},
    {{GameAction::Pause, "pause", "Pause", ps1::kStart, "Start", "control 1: the pause menu"},
     {PadButton::Start, kNone}, {'P', 0}},
    // Slot 6 (SLUS 0x8001D6AC) sets rider +0x228 0x1000 off the bike, and RiderRecover RASHCDG 0x80092E04
    // then puts the bike back on the road beside the standing rider (no walk back). On the pad it shares Triangle with
    // the kick: off the bike the kick does nothing (the pad combat gate), on it slot 6 is read by nothing.
    {{GameAction::ToBike, "to_bike", "Back to the bike (off it)", ps1::kTriangle, "Triangle",
      "slot 6, off the bike: rider +0x228 0x1000 - the bike is put back beside the rider"},
     {PadButton::Triangle, kNone}, {VK_SPACE, 'E'}},
};

// THE VR TABLE: the Touch controllers' defaults. The Touch controllers are read BY
// POSITION into the same PhysicalPad (game_host_vr.cpp TouchToPhysical): A = Cross, B = Circle, X = Square, Y =
// Triangle, the grips L1 / R1, the triggers L2 / R2 (with their travel), the stick clicks L3 / R3, Menu = Start,
// the left stick the stick directions and the right stick the d-pad positions. On top of those positions the VR
// defaults give the layout of a motorcycle game in VR: the triggers throttle and brake, the left stick steers (and
// up / down are the combat modifiers), A / X / B the three combat actions, Y the view, the stick clicks the taunt and
// the look back, Menu the pause.
struct VrActionRow {
    GameAction action;
    PadButton pad[kBindSlots];
};
const VrActionRow kVrActions[kGameActions] = {
    {GameAction::Throttle, {PadButton::R2, kNone}},     // right trigger
    {GameAction::Brake, {PadButton::L2, kNone}},        // left trigger
    {GameAction::SteerLeft, {PadButton::LsLeft, kNone}},
    {GameAction::SteerRight, {PadButton::LsRight, kNone}},
    {GameAction::Attack1, {PadButton::Cross, kNone}},   // A: combat action 1 (R1, the punch / swing)
    {GameAction::Attack2, {PadButton::Square, kNone}},  // X: combat action 2 (L1)
    {GameAction::Attack3, {PadButton::Circle, kNone}},  // B: combat action 3 (R2, the kick)
    {GameAction::ModUp, {PadButton::LsUp, kNone}},
    {GameAction::ModDown, {PadButton::LsDown, kNone}},
    {GameAction::LookBack, {PadButton::R3, kNone}},     // right stick click
    {GameAction::Camera, {PadButton::Triangle, kNone}}, // Y: the view (head -> chase cameras -> head)
    {GameAction::Taunt, {PadButton::L3, kNone}},        // left stick click
    {GameAction::Pause, {PadButton::Start, kNone}},     // left Menu
    {GameAction::ToBike, {PadButton::Cross, kNone}},    // A: off the bike, back to it (on it A is the punch)
};

struct ButtonName {
    const char* id;
    const char* ps;
    const char* xbox;
    const char* generic;
    const char* touch; // the Touch controller at that position ("" = none there)
};
const ButtonName kButtonNames[kPadButtons] = {
    {"cross", "Cross", "A", "Button 2", "A"},
    {"circle", "Circle", "B", "Button 3", "B"},
    {"square", "Square", "X", "Button 1", "X"},
    {"triangle", "Triangle", "Y", "Button 4", "Y"},
    {"l1", "L1", "LB", "Button 5", "Left grip"},
    {"r1", "R1", "RB", "Button 6", "Right grip"},
    {"l2", "L2", "LT", "Button 7", "Left trigger"},
    {"r2", "R2", "RT", "Button 8", "Right trigger"},
    {"select", "Create (Select)", "View (Back)", "Button 9", ""},
    {"start", "Options (Start)", "Menu (Start)", "Button 10", "Menu"},
    {"l3", "L3", "Left stick click", "Button 11", "Left stick click"},
    {"r3", "R3", "Right stick click", "Button 12", "Right stick click"},
    {"dpad_up", "D-pad up", "D-pad up", "Hat up", "Right stick up"},
    {"dpad_down", "D-pad down", "D-pad down", "Hat down", "Right stick down"},
    {"dpad_left", "D-pad left", "D-pad left", "Hat left", "Right stick left"},
    {"dpad_right", "D-pad right", "D-pad right", "Hat right", "Right stick right"},
    {"lstick_up", "Left stick up", "Left stick up", "Stick up", "Left stick up"},
    {"lstick_down", "Left stick down", "Left stick down", "Stick down", "Left stick down"},
    {"lstick_left", "Left stick left", "Left stick left", "Stick left", "Left stick left"},
    {"lstick_right", "Left stick right", "Left stick right", "Stick right", "Left stick right"},
    {"touchpad", "Touchpad", "", "Button 14", ""}, // no touchpad on an Xbox pad
    {"home", "PS button", "Guide", "Button 13", ""},
};

struct KeyName {
    uint8_t vk;
    const char* id;
    const char* label;
};
const KeyName kKeyNames[] = {
    {VK_UP, "up", "Up"},          {VK_DOWN, "down", "Down"},        {VK_LEFT, "left", "Left"},
    {VK_RIGHT, "right", "Right"}, {VK_SPACE, "space", "Space"},     {VK_RETURN, "enter", "Enter"},
    {VK_TAB, "tab", "Tab"},       {VK_BACK, "backspace", "Backspace"}, {VK_SHIFT, "shift", "Shift"},
    {VK_CONTROL, "ctrl", "Ctrl"}, {VK_MENU, "alt", "Alt"},          {VK_INSERT, "insert", "Insert"},
    {VK_DELETE, "delete", "Delete"}, {VK_HOME, "home", "Home"},     {VK_END, "end", "End"},
    {VK_PRIOR, "pageup", "Page Up"}, {VK_NEXT, "pagedown", "Page Down"},
    {VK_OEM_COMMA, "comma", ","}, {VK_OEM_PERIOD, "period", "."},   {VK_OEM_2, "slash", "/"},
    {VK_OEM_1, "semicolon", ";"}, {VK_OEM_7, "quote", "'"},         {VK_OEM_4, "lbracket", "["},
    {VK_OEM_6, "rbracket", "]"},  {VK_OEM_MINUS, "minus", "-"},     {VK_OEM_PLUS, "equals", "="},
    {VK_OEM_3, "backquote", "`"}, {VK_OEM_5, "backslash", "\\"},
};

std::string Lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
std::string Trim(const std::string& s) {
    size_t a = 0, e = s.size();
    while (a < e && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (e > a && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(a, e - a);
}
std::vector<std::string> SplitList(const std::string& s) {
    std::vector<std::string> out;
    std::stringstream in(s);
    std::string item;
    while (std::getline(in, item, ',')) {
        item = Trim(item);
        if (!item.empty()) out.push_back(item);
    }
    return out;
}
bool CapturableKey(int vk) {
    if (vk == VK_ESCAPE || (vk >= VK_F1 && vk <= VK_F24)) return false; // Esc cancels; the F keys are the tool's
    if (vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON) return false;
    return vk >= 0x08;
}

} // namespace

const ActionInfo& Action(GameAction a) { return kActions[static_cast<int>(a)].info; }

void AddStickDirections(PhysicalPad& p) {
    const int dx = static_cast<int>(p.lx) - 128, dy = static_cast<int>(p.ly) - 128;
    constexpr int kThird = 42;
    if (dx < -kThird) p.buttons |= Bit(PadButton::LsLeft);
    if (dx > kThird) p.buttons |= Bit(PadButton::LsRight);
    const bool vertical = 2 * std::abs(dy) >= std::abs(dx);
    if (dy < -kThird && vertical) p.buttons |= Bit(PadButton::LsUp);
    if (dy > kThird && vertical) p.buttons |= Bit(PadButton::LsDown);
}

Bindings DefaultBindings() {
    Bindings b{};
    for (int i = 0; i < kGameActions; ++i)
        for (int s = 0; s < kBindSlots; ++s) {
            b.pad[i][s] = kActions[i].pad[s];
            b.key[i][s] = kActions[i].key[s];
        }
    b.adaptiveTriggers = true;
    return b;
}

void ResetToDefaults(Bindings& b) { b = DefaultBindings(); }

Bindings DefaultVrBindings() {
    Bindings b{};
    for (int i = 0; i < kGameActions; ++i)
        for (int s = 0; s < kBindSlots; ++s) {
            b.pad[static_cast<int>(kVrActions[i].action)][s] = kVrActions[i].pad[s];
            b.key[i][s] = kActions[i].key[s]; // unused by the Touch controllers; the keyboard keeps its own table
        }
    b.adaptiveTriggers = false;
    return b;
}

std::string BindingsPath() {
    if (const char* e = std::getenv("RRJB_CONTROLS"); e != nullptr && *e) return e;
#ifdef _WIN32
    return (ExecutableDir() / "controls.ini").string(); // next to rrgame.exe, as before
#else
    return (SavesDir() / "controls.ini").string(); // the Quest: with the memory card and the settings
#endif
}

Bindings& ActiveBindings() {
    static Bindings b = [] {
        Bindings l = DefaultBindings();
        const std::string path = BindingsPath();
        std::error_code ec;
        if (std::filesystem::exists(path, ec)) {
            std::string err;
            if (LoadBindings(path, l, &err)) std::printf("controls: %s\n", path.c_str());
            else std::fprintf(stderr, "controls: %s (the defaults are used)\n", err.c_str());
        }
        return l;
    }();
    return b;
}

Bindings& VrBindings() {
    static Bindings b = [] {
        Bindings l = DefaultVrBindings();
        const std::string path = BindingsPath();
        std::error_code ec;
        if (std::filesystem::exists(path, ec)) {
            std::string err;
            if (LoadBindings(path, l, &err, "vr_controls")) std::printf("vr controls: [vr_controls] of %s\n", path.c_str());
            else std::fprintf(stderr, "vr controls: %s (the defaults are used)\n", err.c_str());
        }
        return l;
    }();
    return b;
}

const char* PadButtonId(PadButton b) {
    return b < PadButton::Count ? kButtonNames[static_cast<int>(b)].id : "none";
}
std::string PadButtonLabel(PadButton b, PadStyle style) {
    if (b >= PadButton::Count) return "-";
    const ButtonName& n = kButtonNames[static_cast<int>(b)];
    return style == PadStyle::Xbox ? n.xbox : style == PadStyle::Generic ? n.generic : style == PadStyle::Touch ? n.touch : n.ps;
}
std::string KeyLabel(uint8_t vk) {
    if (vk == 0) return "-";
    for (const KeyName& k : kKeyNames)
        if (k.vk == vk) return k.label;
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) return std::string(1, static_cast<char>(vk));
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) return "Num " + std::to_string(vk - VK_NUMPAD0);
    char buf[16];
    std::snprintf(buf, sizeof(buf), "vk_%02X", vk);
    return buf;
}
namespace {
std::string KeyId(uint8_t vk) {
    for (const KeyName& k : kKeyNames)
        if (k.vk == vk) return k.id;
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) return std::string(1, static_cast<char>(std::tolower(vk)));
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) return "num" + std::to_string(vk - VK_NUMPAD0);
    char buf[16];
    std::snprintf(buf, sizeof(buf), "vk_%02x", vk);
    return buf;
}
} // namespace
bool ParsePadButton(const std::string& s, PadButton& out) {
    const std::string l = Lower(Trim(s));
    for (int i = 0; i < kPadButtons; ++i)
        if (l == kButtonNames[i].id) {
            out = static_cast<PadButton>(i);
            return true;
        }
    if (l == "none" || l == "-") {
        out = PadButton::Count;
        return true;
    }
    return false;
}
bool ParseKey(const std::string& s, uint8_t& out) {
    const std::string l = Lower(Trim(s));
    if (l == "none" || l == "-") {
        out = 0;
        return true;
    }
    for (const KeyName& k : kKeyNames)
        if (l == k.id) {
            out = k.vk;
            return true;
        }
    if (l.size() == 1 && ((l[0] >= 'a' && l[0] <= 'z') || (l[0] >= '0' && l[0] <= '9'))) {
        out = static_cast<uint8_t>(std::toupper(static_cast<unsigned char>(l[0])));
        return true;
    }
    if (l.size() == 4 && l.compare(0, 3, "num") == 0 && l[3] >= '0' && l[3] <= '9') {
        out = static_cast<uint8_t>(VK_NUMPAD0 + (l[3] - '0'));
        return true;
    }
    if (l.size() > 3 && l.compare(0, 3, "vk_") == 0) {
        const long v = std::strtol(l.c_str() + 3, nullptr, 16);
        if (v > 0 && v < 256) {
            out = static_cast<uint8_t>(v);
            return true;
        }
    }
    return false;
}

std::string BindingLabel(const Bindings& b, GameAction a, bool gamepad, PadStyle style) {
    std::string out;
    const int i = static_cast<int>(a);
    for (int s = 0; s < kBindSlots; ++s) {
        const std::string one = gamepad ? (b.pad[i][s] < PadButton::Count ? PadButtonLabel(b.pad[i][s], style) : "")
                                        : (b.key[i][s] != 0 ? KeyLabel(b.key[i][s]) : "");
        if (one.empty()) continue;
        if (!out.empty()) out += " / ";
        out += one;
    }
    return out.empty() ? "-" : out;
}

bool LoadBindings(const std::string& path, Bindings& b, std::string* error, const char* sectionName) {
    const std::string wanted = sectionName != nullptr ? sectionName : "controls";
    b = wanted == "vr_controls" ? DefaultVrBindings() : DefaultBindings();
    std::ifstream in(path);
    if (!in) {
        if (error) *error = "cannot read " + path;
        return false;
    }
    std::string line, section;
    int lineNo = 0;
    while (std::getline(in, line)) {
        ++lineNo;
        const std::string t = Trim(line);
        if (t.empty() || t[0] == ';' || t[0] == '#') continue;
        if (t.front() == '[') {
            section = Lower(Trim(t.substr(1, t.find(']') == std::string::npos ? std::string::npos : t.find(']') - 1)));
            continue;
        }
        if (section != wanted) continue;
        const size_t eq = t.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = Lower(Trim(t.substr(0, eq))), v = Trim(t.substr(eq + 1));
        if (k == "adaptive_triggers") {
            const std::string lv = Lower(v);
            b.adaptiveTriggers = !(lv == "0" || lv == "off" || lv == "false" || lv == "no");
            continue;
        }
        const size_t dot = k.rfind('.');
        if (dot == std::string::npos) continue;
        const std::string id = k.substr(0, dot), dev = k.substr(dot + 1);
        int ai = -1;
        for (int i = 0; i < kGameActions; ++i)
            if (id == kActions[i].info.id) ai = i;
        if (ai < 0 || (dev != "pad" && dev != "key")) continue;
        const std::vector<std::string> items = SplitList(v);
        for (int s = 0; s < kBindSlots; ++s) {
            if (dev == "pad") b.pad[ai][s] = PadButton::Count;
            else b.key[ai][s] = 0;
        }
        int s = 0;
        for (const std::string& item : items) {
            if (s >= kBindSlots) break;
            bool ok;
            if (dev == "pad") {
                PadButton pb;
                ok = ParsePadButton(item, pb);
                if (ok && pb != PadButton::Count) b.pad[ai][s++] = pb;
            } else {
                uint8_t vk;
                ok = ParseKey(item, vk);
                if (ok && vk != 0) b.key[ai][s++] = vk;
            }
            if (!ok) std::fprintf(stderr, "controls: %s line %d: unknown %s '%s'\n", path.c_str(), lineNo,
                                  dev == "pad" ? "button" : "key", item.c_str());
        }
    }
    return true;
}

bool SaveBindings(const std::string& path, const Bindings& b, const char* sectionName) {
    const std::string wanted = sectionName != nullptr ? sectionName : "controls";
    const bool vr = wanted == "vr_controls";
    std::vector<std::string> keep;
    {
        std::ifstream in(path);
        std::string line;
        bool inSection = false;
        while (in && std::getline(in, line)) {
            const std::string t = Trim(line);
            if (!t.empty() && t.front() == '[') inSection = Lower(t).rfind("[" + wanted + "]", 0) == 0;
            if (!inSection) keep.push_back(line);
        }
    }
    while (!keep.empty() && Trim(keep.back()).empty()) keep.pop_back();
    std::ofstream out(path, std::ios::trunc);
    if (!out) return false;
    for (const std::string& l : keep) out << l << "\n";
    if (!keep.empty()) out << "\n";
    if (vr)
        out << "[vr_controls]\n"
               "; The Touch controllers in VR, by position: cross = A, circle = B,\n"
               "; square = X, triangle = Y, l1 / r1 = the grips, l2 / r2 = the triggers, l3 / r3 = the stick clicks,\n"
               "; start = Menu, lstick_* = the left stick, dpad_* = the right stick. Up to two per action.\n";
    else
        out << "[controls]\n"
               "; rrgame's controls. Up to two per action, comma separated.\n"
               "; Gamepad buttons, by PlayStation position (Xbox A = cross, B = circle, X = square, Y = triangle):\n"
               ";   cross circle square triangle l1 r1 l2 r2 select start l3 r3 dpad_up dpad_down dpad_left\n"
               ";   dpad_right lstick_up lstick_down lstick_left lstick_right touchpad home\n"
               "; Keys: a..z 0..9 up down left right space enter tab backspace shift ctrl alt comma period slash ...\n";
    for (int i = 0; i < kGameActions; ++i) {
        const ActionInfo& a = kActions[i].info;
        std::string pads, keys;
        for (int s = 0; s < kBindSlots; ++s) {
            if (b.pad[i][s] < PadButton::Count) pads += (pads.empty() ? "" : ", ") + std::string(PadButtonId(b.pad[i][s]));
            if (b.key[i][s] != 0) keys += (keys.empty() ? "" : ", ") + KeyId(b.key[i][s]);
        }
        out << a.id << ".pad = " << (pads.empty() ? "none" : pads) << "\n";
        if (!vr) out << a.id << ".key = " << (keys.empty() ? "none" : keys) << "\n";
    }
    if (!vr) out << "adaptive_triggers = " << (b.adaptiveTriggers ? "on" : "off") << "\n";
    return static_cast<bool>(out);
}

BoundPad MapPad(const PhysicalPad& p, const Bindings& b) {
    BoundPad r;
    if (!p.connected) return r;
    int up = 0, down = 0; // the triggers bound to throttle / brake, folded into right Y
    for (int i = 0; i < kGameActions; ++i) {
        bool held = false;
        for (int s = 0; s < kBindSlots; ++s) {
            const PadButton pb = b.pad[i][s];
            if (pb >= PadButton::Count) continue;
            if (p.Down(pb)) held = true;
            const int travel = pb == PadButton::R2 ? p.r2 : pb == PadButton::L2 ? p.l2 : -1;
            if (travel >= 0 && kActions[i].info.action == GameAction::Throttle) up = std::max(up, travel);
            if (travel >= 0 && kActions[i].info.action == GameAction::Brake) down = std::max(down, travel);
        }
        if (held) r.word |= kActions[i].info.ps1;
    }
    r.lx = p.lx;
    r.ly = p.ly;
    const int ry = static_cast<int>(p.ry) - up / 2 + down / 2;
    r.ry = static_cast<uint8_t>(std::clamp(ry, 0, 255));
    return r;
}

bool KeyHeld(const Bindings& b, GameAction a, const bool* keys, bool arrowsReserved) {
    const int i = static_cast<int>(a);
    for (int s = 0; s < kBindSlots; ++s) {
        const uint8_t vk = b.key[i][s];
        if (vk == 0 || !keys[vk]) continue;
        if (arrowsReserved && (vk == VK_UP || vk == VK_DOWN || vk == VK_LEFT || vk == VK_RIGHT)) continue;
        return true;
    }
    return false;
}

void TriggerResistance(const Bindings& b, uint8_t& right, uint8_t& left) {
    right = left = 0;
    if (!b.adaptiveTriggers) return;
    const auto force = [&](PadButton t) -> uint8_t {
        for (int s = 0; s < kBindSlots; ++s) {
            if (b.pad[static_cast<int>(GameAction::Throttle)][s] == t) return 2; // a light throttle spring
            if (b.pad[static_cast<int>(GameAction::Brake)][s] == t) return 4;    // a firmer brake
        }
        return 0;
    };
    right = force(PadButton::R2);
    left = force(PadButton::L2);
}

void BindingCapture::Begin(GameAction a, bool gamepad, int slot) {
    active_ = a < GameAction::Count && slot >= 0 && slot < kBindSlots;
    gamepad_ = gamepad;
    armed_ = false;
    action_ = a;
    slot_ = slot;
    displaced_ = GameAction::Count;
}

BindingCapture::Result BindingCapture::Feed(Bindings& b, const PhysicalPad* pad, const bool* keys) {
    if (!active_) return Result::Idle;
    const int ai = static_cast<int>(action_);
    if (gamepad_) {
        const uint32_t down = pad != nullptr && pad->connected ? pad->buttons : 0;
        if (!armed_) {
            armed_ = down == 0;
            return Result::Waiting;
        }
        if (down == 0) return Result::Waiting;
        int pick = 0;
        while (pick < kPadButtons && (down & (1u << pick)) == 0) ++pick;
        const PadButton pb = static_cast<PadButton>(pick);
        for (int i = 0; i < kGameActions; ++i)
            for (int s = 0; s < kBindSlots; ++s)
                if (b.pad[i][s] == pb && !(i == ai && s == slot_)) {
                    b.pad[i][s] = PadButton::Count;
                    displaced_ = static_cast<GameAction>(i);
                }
        b.pad[ai][slot_] = pb;
        active_ = false;
        return Result::Captured;
    }
    if (keys == nullptr) return Result::Waiting;
    bool any = false;
    for (int vk = 1; vk < 256; ++vk) any = any || keys[vk];
    if (!armed_) {
        armed_ = !any;
        return Result::Waiting;
    }
    if (keys[VK_ESCAPE]) {
        active_ = false;
        return Result::Cancelled;
    }
    for (int vk = 1; vk < 256; ++vk) {
        if (!keys[vk] || !CapturableKey(vk)) continue;
        // the generic Shift / Ctrl / Alt come with their left / right twins: bind the generic one
        if (vk == VK_LSHIFT || vk == VK_RSHIFT || vk == VK_LCONTROL || vk == VK_RCONTROL || vk == VK_LMENU ||
            vk == VK_RMENU)
            continue;
        const uint8_t k = static_cast<uint8_t>(vk);
        for (int i = 0; i < kGameActions; ++i)
            for (int s = 0; s < kBindSlots; ++s)
                if (b.key[i][s] == k && !(i == ai && s == slot_)) {
                    b.key[i][s] = 0;
                    displaced_ = static_cast<GameAction>(i);
                }
        b.key[ai][slot_] = k;
        active_ = false;
        return Result::Captured;
    }
    return Result::Waiting;
}

void PrintControls(std::FILE* out, const Bindings& b) {
    std::fprintf(out, "%-24s %-8s %-15s %-34s %s\n", "action", "PS1 pad", "keyboard", "DualSense / PlayStation pad",
                 "Xbox pad");
    for (int i = 0; i < kGameActions; ++i) {
        const GameAction a = static_cast<GameAction>(i);
        const ActionInfo& info = Action(a);
        std::fprintf(out, "%-24s %-8s %-15s %-34s %s\n", info.label, info.ps1Name, BindingLabel(b, a, false).c_str(),
                     BindingLabel(b, a, true, PadStyle::PlayStation).c_str(),
                     BindingLabel(b, a, true, PadStyle::Xbox).c_str());
    }
    std::fprintf(out, "DualSense adaptive triggers: %s\n", b.adaptiveTriggers ? "on" : "off");
}

} // namespace rr::platform
