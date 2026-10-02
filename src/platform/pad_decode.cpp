#include "platform/pad_decode.h"

namespace rr::platform {

uint8_t StickByte(int v, bool up) {
    const int b = up ? 128 - v / 256 : 128 + v / 256;
    return static_cast<uint8_t>(b < 0 ? 0 : (b > 255 ? 255 : b));
}

PhysicalPad DecodeXInput(const XInputRaw& s) {
    PhysicalPad p;
    p.connected = true;
    p.style = PadStyle::Xbox;
    p.source = "XInput";
    struct Pair {
        uint16_t bit;
        PadButton b;
    };
    static constexpr Pair kMap[] = {
        {xinput::kA, PadButton::Cross},       {xinput::kB, PadButton::Circle},      {xinput::kX, PadButton::Square},
        {xinput::kY, PadButton::Triangle},    {xinput::kLShoulder, PadButton::L1},  {xinput::kRShoulder, PadButton::R1},
        {xinput::kBack, PadButton::Select},   {xinput::kStart, PadButton::Start},   {xinput::kLThumb, PadButton::L3},
        {xinput::kRThumb, PadButton::R3},     {xinput::kUp, PadButton::DUp},        {xinput::kDown, PadButton::DDown},
        {xinput::kLeft, PadButton::DLeft},    {xinput::kRight, PadButton::DRight},
    };
    for (const Pair& m : kMap)
        if (s.buttons & m.bit) p.buttons |= Bit(m.b);
    p.l2 = s.leftTrigger;
    p.r2 = s.rightTrigger;
    if (p.l2 > kTriggerDown) p.buttons |= Bit(PadButton::L2);
    if (p.r2 > kTriggerDown) p.buttons |= Bit(PadButton::R2);
    p.lx = StickByte(s.lx, false);
    p.ly = StickByte(s.ly, true);
    p.rx = StickByte(s.rx, false);
    p.ry = StickByte(s.ry, true);
    AddStickDirections(p);
    return p;
}

PhysicalPad DecodeWinMM(const WinMMRaw& s, bool sonyVendor) {
    PhysicalPad p;
    p.connected = true;
    p.style = sonyVendor ? PadStyle::PlayStation : PadStyle::Generic;
    p.source = "WinMM";
    static constexpr PadButton kMap[14] = {PadButton::Square, PadButton::Cross, PadButton::Circle, PadButton::Triangle,
                                           PadButton::L1,     PadButton::R1,    PadButton::L2,     PadButton::R2,
                                           PadButton::Select, PadButton::Start, PadButton::L3,     PadButton::R3,
                                           PadButton::Home,   PadButton::Touchpad};
    for (unsigned i = 0; i < 14; ++i)
        if (s.buttons & (1u << i)) p.buttons |= Bit(kMap[i]);
    // no reliable analogue trigger axis through WinMM: the trigger's button is its whole travel
    p.l2 = p.Down(PadButton::L2) ? 255 : 0;
    p.r2 = p.Down(PadButton::R2) ? 255 : 0;
    p.lx = static_cast<uint8_t>(s.x >> 8);
    p.ly = static_cast<uint8_t>(s.y >> 8);
    p.rx = static_cast<uint8_t>(s.z >> 8);
    p.ry = static_cast<uint8_t>(s.r >> 8);
    if ((s.pov & 0xFFFFu) != 0xFFFFu) { // hundredths of a degree, clockwise from up
        const uint32_t v = s.pov;
        if (v >= 31500 || v <= 4500) p.buttons |= Bit(PadButton::DUp);
        if (v >= 4500 && v <= 13500) p.buttons |= Bit(PadButton::DRight);
        if (v >= 13500 && v <= 22500) p.buttons |= Bit(PadButton::DDown);
        if (v >= 22500 && v <= 31500) p.buttons |= Bit(PadButton::DLeft);
    }
    AddStickDirections(p);
    return p;
}

} // namespace rr::platform
