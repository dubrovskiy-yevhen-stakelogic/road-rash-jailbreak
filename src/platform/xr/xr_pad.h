#pragma once
// The Touch controllers as the game's PlayStation pad.
//
// The game reads one pad model: the 16 buttons of the PlayStation halfword (psx-spx order:
// Select, L3, R3, Start, Up, Right, Down, Left, L2, R2, L1, R1, Triangle, Circle, Cross, Square) and the DualShock's
// stick bytes (0x00 = full left / up, 0x80 = centre, 0xFF = full right / down). `XrPad` carries exactly that, plus
// the same named fields rr::platform::GamepadState (platform/gamepad_win32.h) has, so that the product's pad code can
// take either source with FillPadState below - this header does not include the Windows one.
//
// The Touch mapping (OURS, chosen to match the desktop controller's positions, gamepad_win32.h):
//   right trigger  Cross   = throttle        (and the analogue: right stick Y + triggers, as XInput's RT)
//   left trigger   Square  = brake           (LT)
//   left stick     d-pad and left stick bytes (steering; up / down in menus)
//   right stick    right stick bytes
//   A  Cross       B  Circle (look behind)   X  Square       Y  Triangle (menu back)
//   left grip  L1  right grip  R1            left stick click  L2 (taunt)   right stick click  Select (camera)
//   left Menu  Start (pause)
#include <cstdint>

namespace rr::xr {

// Bits of `XrPad::buttons`, active high (the console's halfword is active low; the product's pad code inverts).
enum PadBit : uint16_t {
    kPadSelect = 1u << 0, kPadL3 = 1u << 1, kPadR3 = 1u << 2, kPadStart = 1u << 3,
    kPadUp = 1u << 4, kPadRight = 1u << 5, kPadDown = 1u << 6, kPadLeft = 1u << 7,
    kPadL2 = 1u << 8, kPadR2 = 1u << 9, kPadL1 = 1u << 10, kPadR1 = 1u << 11,
    kPadTriangle = 1u << 12, kPadCircle = 1u << 13, kPadCross = 1u << 14, kPadSquare = 1u << 15,
};

// The Touch controllers' own buttons, unmapped (`XrPad::touch`): what rrgame's VR host binds by position
// (tools\rrgame\game_host_vr.cpp, platform/input_bindings.h [vr_controls]).
enum TouchBit : uint16_t {
    kTouchA = 1u << 0, kTouchB = 1u << 1, kTouchX = 1u << 2, kTouchY = 1u << 3,
    kTouchMenu = 1u << 4, kTouchLeftStick = 1u << 5, kTouchRightStick = 1u << 6,
};

struct XrPad {
    bool connected = false;         // at least one Touch controller is active in a FOCUSED session
    uint16_t buttons = 0;           // PadBit (rrvrtest's fixed mapping)
    uint16_t touch = 0;             // TouchBit: the raw buttons
    uint8_t lx = 0x80, ly = 0x80, rx = 0x80, ry = 0x80;
    float leftTrigger = 0, rightTrigger = 0, leftGrip = 0, rightGrip = 0; // 0..1
    float stick[2][2] = {{0, 0}, {0, 0}}; // [left / right][x / y], -1..1, y up positive (raw)
    // The GamepadState names (gamepad_win32.h), derived from `buttons` and the sticks by Derive().
    bool throttle = false, brake = false, left = false, right = false, lookBack = false, camera = false;
    bool up = false, down = false, cross = false, circle = false, square = false, triangle = false, taunt = false;
    bool start = false;
    const char* source = "none";

    void Derive() {
        throttle = cross = (buttons & kPadCross) != 0;
        brake = square = (buttons & kPadSquare) != 0;
        left = (buttons & kPadLeft) != 0;
        right = (buttons & kPadRight) != 0;
        up = (buttons & kPadUp) != 0;
        down = (buttons & kPadDown) != 0;
        lookBack = circle = (buttons & kPadCircle) != 0;
        triangle = (buttons & kPadTriangle) != 0;
        camera = (buttons & kPadSelect) != 0;
        taunt = (buttons & kPadL2) != 0;
        start = (buttons & kPadStart) != 0;
        source = connected ? "OpenXR Touch" : "none";
    }
};

// Copies the fields a desktop GamepadState-like record has (template: no dependency on the Windows header).
template <class T>
void FillPadState(const XrPad& p, T& s) {
    s.connected = p.connected;
    s.throttle = p.throttle; s.brake = p.brake; s.left = p.left; s.right = p.right;
    s.lookBack = p.lookBack; s.camera = p.camera; s.up = p.up; s.down = p.down;
    s.cross = p.cross; s.circle = p.circle; s.square = p.square; s.triangle = p.triangle;
    s.taunt = p.taunt; s.start = p.start; s.source = p.source;
    s.lx = p.lx; s.ly = p.ly; s.ry = p.ry;
}

// A tracked controller pose (grip and aim) in the session's reference space.
struct HandPose {
    bool gripValid = false, aimValid = false;
    float grip[7] = {0, 0, 0, 0, 0, 0, 1}; // position xyz, orientation xyzw
    float aim[7] = {0, 0, 0, 0, 0, 0, 1};
};

} // namespace rr::xr
