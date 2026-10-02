#pragma once
// The controller the product's pad driver talks to (OURS): libpad's four
// answers SLUS 0x8001DDC4 (the actuator service, sim\pad_motor.h) asks for, given what is plugged in.
// Header-only on purpose: the race (rrgamecore) and the front end (rrgameshell) both use it.
//
//   * a DUALSHOCK (SCPH-1200): PadGetState = 6 (PadStateStable), PadSetActAlign accepts the align
//     table, PadInfoMode(port, 2 = the current extended mode id, 0) = 7 (non-zero: the service leaves the
//     buffer to the motor functions) - the service then keeps the record's state at 1;
//   * a FIRST-TYPE controller (what every capture was taken with - the port table's state 2 and the 0x40
//     buffer byte in rr-race / quick / rr-pack / rr-grid / retro-shell): PadGetState = 2
//     (PadStateFindCTP1), no align, PadInfoMode = 0;
//   * nothing: PadGetState = 0 (PadStateDiscon).
// A controller that changes kind answers 0 once, as a pad pulled out and plugged in again does, so the
// service sets it up again (the original leaves a record with a state set alone otherwise).
#include <cstdint>

#include "game/sim/pad_motor.h"

namespace rr::game {

enum class PadKind : uint8_t { None = 0, FirstType = 1, DualShock = 2 };

class PadDeviceModel final : public rr::sim::LibPad {
public:
    PadKind kind[4] = {PadKind::None, PadKind::None, PadKind::None, PadKind::None};
    size_t setActs = 0, aligns = 0, states = 0;

    int32_t GetState(uint32_t port) override {
        ++states;
        const uint32_t pad = PadOf(port);
        if (pad >= 4) return 0;
        if (kind[pad] != last_[pad]) {
            last_[pad] = kind[pad];
            return 0; // the pad re-plugged: the record's state goes back to 0
        }
        return kind[pad] == PadKind::DualShock ? 6 : (kind[pad] == PadKind::FirstType ? 2 : 0);
    }
    void SetAct(uint32_t, uint32_t, uint32_t) override { ++setActs; }
    int32_t SetActAlign(uint32_t port, uint32_t) override {
        ++aligns;
        const uint32_t pad = PadOf(port);
        return pad < 4 && kind[pad] == PadKind::DualShock ? 1 : 0;
    }
    int32_t InfoMode(uint32_t port, int32_t, int32_t) override {
        const uint32_t pad = PadOf(port);
        return pad < 4 && kind[pad] == PadKind::DualShock ? 7 : 0;
    }
    // libpad's port numbers as the port table holds them: 0, 0x10 or 1 (pad 1), 2, 3.
    static uint32_t PadOf(uint32_t port) {
        if (port == 0x10u) return 1;
        return port < 4u ? port : 4u;
    }

private:
    PadKind last_[4] = {PadKind::None, PadKind::None, PadKind::None, PadKind::None};
};

// The port table's values at boot, SLUS 0x8001C590 (0x8001C5A8..0x8001C5D4): record k's port = k, the
// vibration switch +0x08 = 1 for records 0 and 1. The product's arenas start from the executable alone
// (0x800D7428 is past its image), so this is written once, where the console's boot wrote it.
inline void PadPortsBoot(rr::sim::GuestRam& g) {
    for (uint32_t k = 0; k < 4; ++k) g.W32(rr::sim::kPadPorts + rr::sim::kPadPortBytes * k, k);
    g.W32(rr::sim::kPadPorts + rr::sim::kPadPortBytes + 8u, 1);
    g.W32(rr::sim::kPadPorts + 8u, 1);
}

} // namespace rr::game
