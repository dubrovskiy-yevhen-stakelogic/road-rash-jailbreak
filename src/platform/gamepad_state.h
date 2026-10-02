#pragma once
// One controller's state as the game reads it, without any device code: the fields the
// race reads from the bindings (the original's pad bits) and the ones the menus read by physical position. The desktop
// controllers fill it in gamepad_win32.h (XInput / DualSense / WinMM); the VR host fills it from the Touch controllers
// (tools\rrgame\game_host_vr.cpp) - the same record, so the game's loops do not know which it is.
#include "platform/input_bindings.h"
#include "platform/vk_codes.h"

#include <cstdint>

namespace rr::platform {

struct GamepadState {
    bool connected = false;
    // The race controls, from the bindings (the original's pad bits):
    bool throttle = false, brake = false, left = false, right = false, lookBack = false, camera = false;
    bool taunt = false; // L2 (speech_session.cpp)
    bool start = false; // Start: the race's pause (pause_product.h)
    bool r1 = false, l1 = false, r2 = false;  // the combat buttons: actions 1 / 2 / 3
    bool padUp = false, padDown = false;      // the d-pad Up / Down bits: the combat modifiers, the walk
    bool toBike = false; // Triangle (bit 4, slot 6): off the bike, the bike put back beside the rider
    // The menus' buttons, by physical position (PlayStation names; XInput A / B / X / Y) and the d-pad / left
    // stick up and down.
    bool up = false, down = false, cross = false, circle = false, square = false, triangle = false;
    const char* source = "none";
    bool hasMotors = false; // XInput or a DualSense: Vibrate reaches a motor
    // The analogue sticks as PlayStation bytes: 0x00 = full left / up, 0x80 = centre, 0xFF = full right / down.
    uint8_t lx = 0x80, ly = 0x80, ry = 0x80;
    uint16_t word = 0;   // the original's repacked pad word the bindings produced
    PhysicalPad raw;     // what the device reported (the overlay's rebinding capture reads it)
};

// Fills the bound and the physical fields of a GamepadState from a decoded pad (also what --padmapcheck runs).
inline GamepadState StateFromPhysical(const PhysicalPad& p, const Bindings& b) {
    GamepadState s;
    s.raw = p;
    if (!p.connected) return s;
    s.connected = true;
    s.source = p.source;
    const BoundPad m = MapPad(p, b);
    s.word = m.word;
    s.throttle = (m.word & ps1::kCross) != 0;
    s.brake = (m.word & ps1::kSquare) != 0;
    s.left = (m.word & ps1::kLeft) != 0;
    s.right = (m.word & ps1::kRight) != 0;
    s.lookBack = (m.word & ps1::kCircle) != 0;
    s.camera = (m.word & ps1::kSelect) != 0;
    s.taunt = (m.word & ps1::kL2) != 0;
    s.start = (m.word & ps1::kStart) != 0;
    s.r1 = (m.word & ps1::kR1) != 0;
    s.l1 = (m.word & ps1::kL1) != 0;
    s.r2 = (m.word & ps1::kR2) != 0;
    s.padUp = (m.word & ps1::kUp) != 0;
    s.padDown = (m.word & ps1::kDown) != 0;
    s.toBike = (m.word & ps1::kTriangle) != 0;
    s.up =p.Down(PadButton::DUp) || p.Down(PadButton::LsUp);
    s.down = p.Down(PadButton::DDown) || p.Down(PadButton::LsDown);
    s.cross = p.Down(PadButton::Cross);
    s.circle = p.Down(PadButton::Circle);
    s.square = p.Down(PadButton::Square);
    s.triangle = p.Down(PadButton::Triangle);
    s.lx = m.lx;
    s.ly = m.ly;
    s.ry = m.ry;
    return s;
}

} // namespace rr::platform
