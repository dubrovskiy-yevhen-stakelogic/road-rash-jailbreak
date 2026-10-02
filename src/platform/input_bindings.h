#pragma once
// The game's controls, rebindable: which physical gamepad button or keyboard key drives which PlayStation pad
// bit.
//
// The chain, one direction only:
//   device report (XInput state / WinMM JOYINFOEX / DualSense HID report)       pad_decode.h, dualsense.h
//     -> PhysicalPad: the buttons BY POSITION, in PlayStation names, + the sticks and the triggers
//     -> MapPad(Bindings): the ORIGINAL'S pad word - the repacked word (bits 0..7 the second
//        button byte, 8..15 the first, 1 = pressed), exactly the bits a PlayStation pad would give the game
//     -> GamepadState / PadState (gamepad_win32.h, race_session.h).
// The game only ever sees PlayStation pad bits; the bindings decide which physical input sets which bit.
//
// The actions and their bits are ONE table (kActions in input_bindings.cpp): the defaults, the `--keys` text, the
// ini keys and the overlay's labels all come from it.
#include <cstdint>
#include <cstdio>
#include <string>

namespace rr::platform {

// The repacked pad word's bits (0x8001C824 puts ~hi in bits 0..7 and ~lo in bits 8..15; the
// slot masks at 0x80052658 test these). The same bits fight_session.cpp stamps into the pad record.
namespace ps1 {
constexpr uint16_t kL2 = 0x0001, kR2 = 0x0002, kL1 = 0x0004, kR1 = 0x0008;
constexpr uint16_t kTriangle = 0x0010, kCircle = 0x0020, kCross = 0x0040, kSquare = 0x0080;
constexpr uint16_t kSelect = 0x0100, kL3 = 0x0200, kR3 = 0x0400, kStart = 0x0800;
constexpr uint16_t kUp = 0x1000, kRight = 0x2000, kDown = 0x4000, kLeft = 0x8000;
} // namespace ps1

// A physical button, by position on a PlayStation-layout pad (an Xbox pad's A is Cross, and so on). The stick
// directions are buttons too (past a third of the travel), so a stick can be bound like a d-pad.
enum class PadButton : uint8_t {
    Cross, Circle, Square, Triangle, L1, R1, L2, R2, Select, Start, L3, R3,
    DUp, DDown, DLeft, DRight, LsUp, LsDown, LsLeft, LsRight, Touchpad, Home,
    Count // also "no button"
};
constexpr int kPadButtons = static_cast<int>(PadButton::Count);
constexpr uint32_t Bit(PadButton b) { return 1u << static_cast<unsigned>(b); }

// How a device names its buttons (labels only; the mapping is by position).
// Touch: the Meta Quest Touch controllers in VR (A = Cross, B = Circle, X = Square, Y = Triangle, the grips L1 / R1,
// the triggers L2 / R2, the stick clicks L3 / R3, Menu = Start, the right stick = the d-pad positions).
enum class PadStyle : uint8_t { PlayStation, Xbox, Generic, Touch };

struct PhysicalPad {
    bool connected = false;
    uint32_t buttons = 0; // Bit(PadButton)
    // PlayStation stick bytes: 0x00 = full left / up, 0x80 = centre, 0xFF = full right / down.
    uint8_t lx = 0x80, ly = 0x80, rx = 0x80, ry = 0x80;
    uint8_t l2 = 0, r2 = 0;          // the triggers' travel 0..255 (a digital trigger: 0 or 255)
    PadStyle style = PadStyle::Generic;
    const char* source = "none";     // "XInput", "DualSense USB", "DualSense Bluetooth", "WinMM"
    bool Down(PadButton b) const { return (buttons & Bit(b)) != 0; }
};

// The trigger threshold for its digital button (an XInput trigger's and a DualSense trigger's travel).
constexpr uint8_t kTriggerDown = 0x40;
// The stick directions: past a third of the travel. Up / down also need the vertical to be at least half the
// horizontal, so a hard left with a little push does not also press Up (a combat modifier in the race).
void AddStickDirections(PhysicalPad& p);

enum class GameAction : uint8_t {
    Throttle, Brake, SteerLeft, SteerRight, Attack1, Attack2, Attack3, ModUp, ModDown, LookBack, Camera, Taunt,
    Pause,
    ToBike, // Triangle - off the bike, the original brings the bike back to the rider
    Count
};
constexpr int kGameActions = static_cast<int>(GameAction::Count);

struct ActionInfo {
    GameAction action;
    const char* id;       // the ini key and a stable name for the overlay
    const char* label;    // what the overlay shows
    uint16_t ps1;         // the original's pad bit this action presses
    const char* ps1Name;  // that button's name on the PlayStation pad
    const char* note;     // what the game does with it
};
const ActionInfo& Action(GameAction a);

constexpr int kBindSlots = 2; // each action: up to two gamepad buttons and two keys
struct Bindings {
    PadButton pad[kGameActions][kBindSlots];
    uint8_t key[kGameActions][kBindSlots]; // virtual-key codes, 0 = none
    bool adaptiveTriggers = true;          // DualSense: resistance on the triggers bound to throttle / brake
    bool operator==(const Bindings&) const = default;
};
Bindings DefaultBindings();

// The bindings the game uses: loaded once from BindingsPath() (if the file exists), else the defaults. The
// overlay edits this object and calls SaveBindings(BindingsPath(), ActiveBindings()).
Bindings& ActiveBindings();
// `controls.ini` next to the executable (Windows; the saves folder on Android) - RRJB_CONTROLS overrides it.
std::string BindingsPath();
// Reads the [controls] section (or `section`); unknown keys are ignored, missing ones keep their defaults. False when
// the file cannot be read (`b` then holds the defaults).
bool LoadBindings(const std::string& path, Bindings& b, std::string* error = nullptr, const char* section = "controls");
// Rewrites the [controls] section (or `section`) only, keeping every other line of the file (a shared settings file
// stays).
bool SaveBindings(const std::string& path, const Bindings& b, const char* section = "controls");
void ResetToDefaults(Bindings& b);
// The Touch controllers' bindings in VR (the [vr_controls] section of the same file):
// the same actions and PlayStation bits, other default buttons (input_bindings.cpp kVrActions). The VR host maps the
// Touch controllers onto PhysicalPad positions and runs MapPad with these.
Bindings DefaultVrBindings();
Bindings& VrBindings();

// The original's pad word for a physical pad, and the analogue sticks as the 0x73 device stores them (left X,
// left Y, right Y; the triggers bound to throttle / brake are folded into right Y - up / down - as the analogue
// on-bike branch reads it).
struct BoundPad {
    uint16_t word = 0;
    uint8_t lx = 0x80, ly = 0x80, ry = 0x80;
};
BoundPad MapPad(const PhysicalPad& p, const Bindings& b);
// A keyboard action held (`keys` = the window's 256 held flags). `arrowsReserved`: a two-player race gives the
// arrow keys to player 2, so player 1's bindings to them are ignored.
bool KeyHeld(const Bindings& b, GameAction a, const bool* keys, bool arrowsReserved = false);

// DualSense adaptive triggers: the resistance (0 = none, 1..8) for the right / left trigger from what they are
// bound to (throttle: light, brake: firmer). 0 / 0 when the option is off.
void TriggerResistance(const Bindings& b, uint8_t& right, uint8_t& left);

// Labels and ini names.
const char* PadButtonId(PadButton b);                    // "cross", "dpad_up", ... (the ini's spelling)
std::string PadButtonLabel(PadButton b, PadStyle style); // "Cross" / "A" / "Button 2"
std::string KeyLabel(uint8_t vk);                        // "Z", "Up", "Space", ...
std::string BindingLabel(const Bindings& b, GameAction a, bool gamepad, PadStyle style = PadStyle::PlayStation);
bool ParsePadButton(const std::string& s, PadButton& out);
bool ParseKey(const std::string& s, uint8_t& out);

// Rebinding: Begin, then Feed every frame with the current physical pad and / or the keyboard. The capture waits
// until everything is released (so the button that opened it is not captured), then takes the next press. The
// captured input is removed from any other action of the same device (the result names it). Esc cancels a
// keyboard capture; the gamepad's Start (Options) is accepted as a binding like any other button.
class BindingCapture {
public:
    enum class Result { Idle, Waiting, Captured, Cancelled };
    void Begin(GameAction a, bool gamepad, int slot);
    void Cancel() { active_ = false; }
    bool Active() const { return active_; }
    Result Feed(Bindings& b, const PhysicalPad* pad, const bool* keys);
    // after Captured: the action that lost the input (Count = none)
    GameAction Displaced() const { return displaced_; }

private:
    bool active_ = false, gamepad_ = true, armed_ = false;
    GameAction action_ = GameAction::Count, displaced_ = GameAction::Count;
    int slot_ = 0;
};

// The controls table (`rrgame --keys`): action, PlayStation button, keyboard, gamepad.
void PrintControls(std::FILE* out, const Bindings& b);

} // namespace rr::platform
