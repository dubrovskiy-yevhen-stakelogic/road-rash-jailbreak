// The DualSense's HID reports, as pure functions (`rrgame --padmapcheck` feeds them synthetic reports). Ported
// from gt2-play (MIT) src\platform\input\dualsense_report.cpp, decoding into our PhysicalPad.
#include "platform/dualsense.h"

#include <algorithm>

namespace rr::platform {

uint32_t DualSenseCrc(uint8_t seed, const uint8_t* bytes, size_t n) {
    uint32_t crc = 0xFFFFFFFFu;
    auto byte = [&](uint8_t b) {
        crc ^= b;
        for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ ((crc & 1) ? 0xEDB88320u : 0);
    };
    byte(seed);
    for (size_t i = 0; i < n; ++i) byte(bytes[i]);
    return ~crc;
}

bool DecodeDualSenseInput(const uint8_t* report, size_t size, bool bt, PhysicalPad& pad) {
    if (report == nullptr || size == 0) return false;
    const bool simple = bt && report[0] == 0x01;
    size_t offset = 1;
    if (simple) {
        if (size < 10) return false;
    } else if (bt) {
        if (report[0] != 0x31 || size < 78) return false;
        const uint32_t stored = uint32_t(report[74]) | uint32_t(report[75]) << 8 | uint32_t(report[76]) << 16 |
                                uint32_t(report[77]) << 24;
        if (DualSenseCrc(0xA1, report, 74) != stored) return false;
        offset = 2;
    } else if (report[0] != 0x01 || size < 64) {
        return false;
    }
    const uint8_t* p = report + offset;
    PhysicalPad r;
    r.connected = true;
    r.style = PadStyle::PlayStation;
    r.source = bt ? "DualSense Bluetooth" : "DualSense USB";
    r.lx = p[0];
    r.ly = p[1];
    r.rx = p[2];
    r.ry = p[3];
    r.l2 = p[simple ? 7 : 4];
    r.r2 = p[simple ? 8 : 5];
    const uint8_t face = p[simple ? 4 : 7], shoulders = p[simple ? 5 : 8], misc = p[simple ? 6 : 9];
    static constexpr uint32_t kHat[8] = {
        Bit(PadButton::DUp),   Bit(PadButton::DUp) | Bit(PadButton::DRight),   Bit(PadButton::DRight),
        Bit(PadButton::DRight) | Bit(PadButton::DDown), Bit(PadButton::DDown), Bit(PadButton::DDown) | Bit(PadButton::DLeft),
        Bit(PadButton::DLeft), Bit(PadButton::DLeft) | Bit(PadButton::DUp)};
    if ((face & 15) < 8) r.buttons |= kHat[face & 15];
    static constexpr PadButton kFaces[4] = {PadButton::Square, PadButton::Cross, PadButton::Circle, PadButton::Triangle};
    for (unsigned i = 0; i < 4; ++i)
        if (face & (0x10 << i)) r.buttons |= Bit(kFaces[i]);
    // L2 / R2 (bits 2 / 3) are the triggers' own "touched" bits: the digital L2 / R2 come from the travel, as
    // an XInput trigger's do, so both kinds of pad press at the same depth.
    static constexpr PadButton kOther[8] = {PadButton::L1,     PadButton::R1,    PadButton::Count, PadButton::Count,
                                            PadButton::Select, PadButton::Start, PadButton::L3,    PadButton::R3};
    for (unsigned i = 0; i < 8; ++i)
        if ((shoulders & (1u << i)) && kOther[i] != PadButton::Count) r.buttons |= Bit(kOther[i]);
    if (misc & 0x01) r.buttons |= Bit(PadButton::Home);
    if (misc & 0x02) r.buttons |= Bit(PadButton::Touchpad);
    if (r.l2 > kTriggerDown) r.buttons |= Bit(PadButton::L2);
    if (r.r2 > kTriggerDown) r.buttons |= Bit(PadButton::R2);
    AddStickDirections(r);
    pad = r;
    return true;
}

DualSenseTransport DualSenseReportTransport(uint16_t inputLength, uint16_t outputLength) {
    // Output caps describe the longest report, including reports we never send.
    if (outputLength > 1024) return DualSenseTransport::Unsupported;
    if (inputLength == 64 && outputLength >= 48) return DualSenseTransport::Usb;
    if (inputLength == 78 && outputLength >= 78) return DualSenseTransport::Bluetooth;
    return DualSenseTransport::Unsupported;
}

std::array<uint8_t, 78> DualSenseOutputReport(const DualSenseOutput& s, bool bt, uint8_t seq) {
    std::array<uint8_t, 78> r{};
    r[0] = bt ? 0x31 : 0x02;
    const size_t p = bt ? 3 : 1;
    if (bt) {
        r[1] = static_cast<uint8_t>((seq & 15) << 4);
        r[2] = 0x10;
    }
    r[p] = 0x0F; // compatible rumble, no audio haptics, update both triggers
    r[p + 2] = static_cast<uint8_t>((s.small & 1) ? 127 : 0); // the right (high-frequency) motor: the small one
    r[p + 3] = static_cast<uint8_t>(s.large / 2);             // the left (low-frequency) motor: the large one
    auto resistance = [&](size_t at, uint8_t force, unsigned start) {
        if (force == 0) {
            r[at] = 0x05; // the effect off
            return;
        }
        r[at] = 0x21; // feedback: ten zones, each a three-bit force
        uint16_t mask = 0;
        uint32_t forces = 0;
        for (unsigned zone = start; zone < 10; ++zone) {
            mask |= static_cast<uint16_t>(1u << zone);
            forces |= static_cast<uint32_t>(std::min<unsigned>(force, 8) - 1) << (3 * zone);
        }
        r[at + 1] = static_cast<uint8_t>(mask);
        r[at + 2] = static_cast<uint8_t>(mask >> 8);
        for (unsigned i = 0; i < 4; ++i) r[at + 3 + i] = static_cast<uint8_t>(forces >> (8 * i));
    };
    resistance(p + 10, s.rightForce, 2);
    resistance(p + 21, s.leftForce, 1);
    if (bt) {
        const uint32_t crc = DualSenseCrc(0xA2, r.data(), 74); // the Bluetooth output seed
        for (unsigned i = 0; i < 4; ++i) r[74 + i] = static_cast<uint8_t>(crc >> (8 * i));
    }
    return r;
}

} // namespace rr::platform
