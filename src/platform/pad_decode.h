#pragma once
// Device state -> PhysicalPad (input_bindings.h), pure functions: `rrgame --padmapcheck` feeds them synthetic
// states; gamepad_win32.h feeds them the live ones. The DualSense's HID reports are decoded in dualsense.h.
#include "platform/input_bindings.h"

#include <cstdint>

namespace rr::platform {

// XINPUT_GAMEPAD, field for field (xinput.h), so the check needs no XInput header.
struct XInputRaw {
    uint16_t buttons = 0;
    uint8_t leftTrigger = 0, rightTrigger = 0;
    int16_t lx = 0, ly = 0, rx = 0, ry = 0; // up / right positive
};
namespace xinput {
constexpr uint16_t kUp = 0x0001, kDown = 0x0002, kLeft = 0x0004, kRight = 0x0008, kStart = 0x0010, kBack = 0x0020;
constexpr uint16_t kLThumb = 0x0040, kRThumb = 0x0080, kLShoulder = 0x0100, kRShoulder = 0x0200;
constexpr uint16_t kA = 0x1000, kB = 0x2000, kX = 0x4000, kY = 0x8000;
} // namespace xinput
PhysicalPad DecodeXInput(const XInputRaw& s);

// The JOYINFOEX fields the WinMM path reads (axes 0..0xFFFF, POV in hundredths of a degree or 0xFFFF centred).
// Buttons follow the PlayStation-style DirectInput layout Sony's pads and most generic pads report:
// 1 Square, 2 Cross, 3 Circle, 4 Triangle, 5 L1, 6 R1, 7 L2, 8 R2, 9 Select, 10 Start, 11 L3, 12 R3, 13 Home,
// 14 Touchpad. The axes: X / Y the left stick, Z the right X, R (Rz) the right Y.
struct WinMMRaw {
    uint32_t buttons = 0;
    uint32_t pov = 0xFFFF;
    uint32_t x = 0x7FFF, y = 0x7FFF, z = 0x7FFF, r = 0x7FFF;
};
PhysicalPad DecodeWinMM(const WinMMRaw& s, bool sonyVendor);

// A signed axis (+-32767, up or right positive as `up`) as a PlayStation stick byte.
uint8_t StickByte(int v, bool up);

} // namespace rr::platform
