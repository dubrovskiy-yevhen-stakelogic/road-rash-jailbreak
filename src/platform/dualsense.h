#pragma once
// Sony DualSense (054C:0CE6) and DualSense Edge (054C:0DF2), read natively over HID - USB and Bluetooth - with the
// motors and the adaptive triggers driven through its output report. No Steam Input, no driver: the plain Windows
// HID class driver. Ported from the sibling project gt2-play (MIT, src\platform\input\dualsense*.cpp).
//
// Input reports decoded:
//   USB 0x01, 64 bytes:         [1] LX [2] LY [3] RX [4] RY [5] L2 [6] R2 [7] counter [8] d-pad + faces
//                               [9] L1 R1 L2 R2 Create Options L3 R3 [10] PS touchpad mute
//   Bluetooth 0x31, 78 bytes:   the same from [2] ([1] a sequence tag), CRC-32 (seed 0xA1) in [74..77]
//   Bluetooth 0x01 (the simple report the pad sends until an output report switches it): [1] LX [2] LY [3] RX
//                               [4] RY [5] d-pad + faces [6] shoulders [7] PS touchpad [8] L2 [9] R2
// Output: USB 0x02 (48 bytes), Bluetooth 0x31 (78 bytes with the CRC-32 seeded 0xA2): the compatible-rumble motors
// and the two trigger effect blocks (right at +10, left at +21 from the common part).
#include "platform/input_bindings.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace rr::platform {

bool DecodeDualSenseInput(const uint8_t* report, size_t size, bool bluetooth, PhysicalPad& pad);

struct DualSenseOutput {
    uint8_t small = 0, large = 0;          // the DualShock's motors: small on / off, large 0..255
    uint8_t rightForce = 0, leftForce = 0; // trigger resistance 0 (off) .. 8
    bool operator==(const DualSenseOutput&) const = default;
};
std::array<uint8_t, 78> DualSenseOutputReport(const DualSenseOutput& state, bool bluetooth, uint8_t sequence);
enum class DualSenseTransport { Unsupported, Usb, Bluetooth };
DualSenseTransport DualSenseReportTransport(uint16_t inputLength, uint16_t outputLength);
uint32_t DualSenseCrc(uint8_t seed, const uint8_t* bytes, size_t n); // the Bluetooth reports' CRC-32

// One open controller: a reader thread keeps the last decoded report, a writer thread sends the output report
// when it changes (and switches everything off 500 ms after the last Motors / Triggers call).
class DualSenseDevice {
public:
    explicit DualSenseDevice(const std::wstring& path);
    ~DualSenseDevice();
    bool Ok() const;
    bool Bluetooth() const;
    const std::wstring& Path() const;
    // 1: a decoded report (neutral when a streaming report went stale), 0: nothing received yet, -1: the device
    // went away.
    int ReadPad(PhysicalPad& pad) const;
    void Motors(uint8_t small, uint8_t large);
    void Triggers(uint8_t rightForce, uint8_t leftForce);
    uint32_t ReportsRead() const;
    uint8_t LastReportId() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// The HID interface paths of every present DualSense / DualSense Edge game-pad collection.
std::vector<std::wstring> EnumerateDualSensePaths();

// The process's open DualSense controllers. The first Count() enumerates and opens them; a background thread then
// rescans every 2 s for new ones and drops the ones whose reads failed.
class DualSenseHub {
public:
    static DualSenseHub& Get();
    size_t Count();
    std::shared_ptr<DualSenseDevice> At(size_t i);
    ~DualSenseHub();

private:
    DualSenseHub();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace rr::platform
