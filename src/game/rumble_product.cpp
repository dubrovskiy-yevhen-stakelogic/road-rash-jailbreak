#include "game/rumble_product.h"

#include "game/pad_device.h"
#include "game/shell/handover.h"
#include "game/sim/collision.h"
#include "game/sim/pad_motor.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace rr::game {

using rr::sim::GuestRam;

namespace {
constexpr uint32_t kGameStatePtr = 0x8005B2F8;
constexpr uint32_t kPlayerBike = 0x8005B38C;   // *(0x8005B38C): player 1's bike
constexpr uint32_t kPlayer2Bike = 0x8005B21C;  // *(0x8005B21C): player 2's bike (0: none)
constexpr uint32_t kAttract = 0x8005B220;
constexpr uint32_t kPlayers = 0x800D81D8;      // six 36-byte player records (+0x17: VIBRATION)
constexpr uint32_t kMultitap = 0x800D70E1;     // the driver's multitap byte: >> 4 == 8 with a multitap

struct Runtime {
    uint8_t* ram = nullptr;
    uint32_t gp = 0;
    PadDeviceModel model;
};
Runtime& R() {
    static Runtime r;
    return r;
}

uint32_t Pads(GuestRam& g) { return std::min<uint32_t>(g.U32(g.U32(kGameStatePtr) + 0x34u), 4u); }

class Env final : public rr::sim::RumbleEnv {
public:
    explicit Env(GuestRam& g) : g_(g) {}
    // PadRumble with other == 0 follows *(0x354), the console's kernel pointer into the BIOS ROM
    // (0xBFC09914 on every captured machine); only bit 0x40 of the byte is read, and it is clear there.
    // OURS: answered 0 (the product has no BIOS), counted.
    bool ReadForeignByte(uint32_t, uint8_t& value) override {
        ++RumbleCounters().foreignBytes;
        value = 0;
        return true;
    }
    bool PadMotor(uint32_t pad, int32_t a, int32_t b, int32_t c, uint32_t) override {
        ++RumbleCounters().motorCalls;
        rr::sim::PadMotor(g_, pad, a, b, c); // SLUS 0x8001DD74, PORTED
        return !g_.Faulted();
    }

private:
    GuestRam& g_;
};
} // namespace

bool RumbleOn() {
    static const bool on = [] {
        const char* v = std::getenv("RRJB_RUMBLE");
        return !(v != nullptr && std::strcmp(v, "off") == 0);
    }();
    return on;
}

RumbleCounts& RumbleCounters() {
    static RumbleCounts c;
    return c;
}

void RumbleRaceStart(uint8_t* ram, uint32_t gp, bool fromShell) {
    if (!RumbleOn() || ram == nullptr) return;
    Runtime& r = R();
    r.ram = ram;
    r.gp = gp;
    r.model = PadDeviceModel{};
    GuestRam g(ram, gp);
    PadPortsBoot(g);                                               // SLUS 0x8001C590's table
    const uint32_t pads = Pads(g);
    RumbleCounts& c = RumbleCounters();
    ++c.races;
    c.fromShell = fromShell;
    if (fromShell) {
        for (uint32_t p = 0; p < pads; ++p)                        // RASHCDF 0x8007F428
            g.W32(rr::sim::kPadPorts + rr::sim::kPadPortBytes * p + 8u,
                  static_cast<uint32_t>(static_cast<int32_t>(g.S8(kPlayers + 36u * p + 0x17u))));
    } else {
        for (uint32_t p = 0; p < 4; ++p) g.W32(rr::sim::kPadPorts + rr::sim::kPadPortBytes * p + 8u, 1);
    }
    for (uint32_t p = 0; p < 4; ++p) {
        c.vibration[p] = g.U32(rr::sim::kPadPorts + rr::sim::kPadPortBytes * p + 8u);
        r.model.kind[p] = p < pads ? PadKind::DualShock : PadKind::None;
    }
}

void RumbleFrame(uint8_t* ram, uint32_t gp) {
    if (!RumbleOn() || ram == nullptr) return;
    Runtime& r = R();
    if (r.ram != ram) RumbleRaceStart(ram, gp, rr::shell::PendingHandover().active); // the race's first frame
    GuestRam g(ram, gp);
    RumbleCounts& c = RumbleCounters();
    const uint32_t pads = Pads(g);
    // what the last frame left in the DualShock records' buffers - what libpad sends at the vblank between
    for (uint32_t p = 0; p < pads; ++p) {
        const uint32_t rec = rr::sim::kPadPorts + rr::sim::kPadPortBytes * p;
        if (g.U32(rec + 4u) != 1u) continue;
        const uint8_t large = g.U8(rec + 0x0Du);
        if (large != 0) {
            ++c.largeFrames;
            c.largeSum += large;
            c.largeMax = std::max<size_t>(c.largeMax, large);
        }
        if (g.U8(rec + 0x0Cu) != 0) ++c.smallFrames;
    }
    // the driver's post-processor, per pad (SLUS 0x8001CA04: DDC4(pad, s8); s8 = 0 - pad 1's port 0x10, as
    // every capture's table holds it)
    size_t aligned = 0;
    for (uint32_t p = 0; p < pads; ++p) {
        rr::sim::PadActuatorService(g, p, 0, r.model);             // PORTED
        ++c.services;
        if (g.U32(rr::sim::kPadPorts + rr::sim::kPadPortBytes * p + 4u) == 1u) ++aligned;
    }
    c.aligned = std::max(c.aligned, aligned);
    // the pad reader SLUS 0x8001CB3C: nothing in state 2; its player loop only outside the attract mode
    const uint32_t gs = g.U32(kGameStatePtr);
    if (g.U8(gs) == 2u || g.U32(kAttract) != 0) return;
    uint32_t n = pads;                                             // 0x8001CE2C..0x8001CE54
    if ((g.U8(kMultitap) >> 4) != 8u && n > 2u) n = 2u;
    for (uint32_t p = 0; p < n; ++p) {
        uint32_t bike = 0, flags = 0;
        if (p == 0) {                                              // 0x8001CEB0
            bike = g.U32(kPlayerBike);
            flags = g.U32(bike + 0x230u);
        } else if (p == 1) {                                       // 0x8001CEC8
            const uint32_t b2 = g.U32(kPlayer2Bike);
            const uint32_t p1 = g.U32(kPlayerBike);
            bike = b2 != 0 ? b2 : g.U32(p1 + 0x358u);
            flags = g.U32((b2 != 0 ? b2 : p1) + 0x230u);
        } else {
            continue; // the co-op test of 0x8001CF10.. (four pads with a multitap) - not a product mode
        }
        const bool controls = ((flags >> 27) & 1u) == 0u;          // 0x8001CF00: (flags >> 27 ^ 1) & 1
        const uint32_t cnt = rr::sim::kPadIdleCounters + 4u * p;
        if (g.U8(gs) == 1u && controls && bike != 0) {
            const rr::sim::PadRumbleRegs regs = rr::sim::PadEngineRumble(g, p, bike, cnt, 0); // PORTED
            ++c.engineRegions;
            if (regs.strength == 50) ++c.idleShake;
            else if (regs.strength > 0 && (g.U32(bike + 0x184u) & 1u)) ++c.roughShake;
            else if (regs.strength > 0) ++c.lateralShake;
        } else {
            rr::sim::PadMotorsOff(g, p, cnt, 0);                   // PORTED
            ++c.offRegions;
        }
    }
}

bool ProductPadRumble(GuestRam& g, uint32_t e, uint32_t other, int32_t speed, int32_t k, int32_t div, uint32_t sp) {
    if (!RumbleOn()) return false;
    Env env(g);
    ++RumbleCounters().padRumbles;
    const bool ok = rr::sim::PadRumble(g, e, other, speed, k, div, sp, env); // PORTED, RASHCDG 0x800B658C
    if (!ok) ++RumbleCounters().padRumbleRefused;
    return ok;
}

void ProductPadMotor(GuestRam& g, uint32_t pad, int32_t a, int32_t b, int32_t c) {
    if (!RumbleOn()) return;
    ++RumbleCounters().hitRumbles;
    ++RumbleCounters().motorCalls;
    rr::sim::PadMotor(g, pad, a, b, c); // SLUS 0x8001DD74, PORTED
}

void RumbleRaceEnd(uint8_t* ram, uint32_t gp) {
    if (!RumbleOn() || ram == nullptr) return;
    GuestRam g(ram, gp);
    const uint32_t pads = Pads(g);
    for (uint32_t p = 0; p < pads; ++p) {
        rr::sim::PadMotorLarge(g, p, -1, 0);
        rr::sim::PadMotorSmall(g, p, -1);
    }
    R().ram = nullptr;
}

MotorState RumbleOutput(uint32_t pad) {
    MotorState m;
    Runtime& r = R();
    if (!RumbleOn() || r.ram == nullptr || pad >= 4) return m;
    GuestRam g(r.ram, r.gp);
    const uint32_t rec = rr::sim::kPadPorts + rr::sim::kPadPortBytes * pad;
    m.dualShock = g.U32(rec + 4u) == 1u;
    if (!m.dualShock) return m;
    m.small = g.U8(rec + 0x0Cu) != 0;
    m.large = g.U8(rec + 0x0Du);
    return m;
}

void NoteDeviceWrite() { ++RumbleCounters().deviceWrites; }

std::string RumbleTotals() {
    const RumbleCounts& c = RumbleCounters();
    char b[1600];
    char cd[300];
    std::snprintf(cd, sizeof(cd),
                  "countdown voice: StopVoice SLUS 0x80016528 run %zu time(s) on gp+1964 = 0x%08X (the handle "
                  "0x800164B4 stored at the race start; 0 in a one-player race)\n",
                  c.countdownStops, c.countdownHandle);
    if (!RumbleOn()) {
        std::snprintf(b, sizeof(b),
                      "rumble: RRJB_RUMBLE=off - PadRumble, the motors SLUS 0x8001DD74, the actuator service "
                      "0x8001DDC4 and the pad reader's rumble regions are not run (the seams stand)\n");
        return std::string(b) + cd;
    }
    const double mean = c.largeFrames ? static_cast<double>(c.largeSum) / static_cast<double>(c.largeFrames) : 0.0;
    std::snprintf(b, sizeof(b),
                  "rumble: PORTED pad motors - PadRumble 0x800B658C run %zu (refused %zu), HitRumble's motors "
                  "%zu, SLUS 0x8001DD74 %zu call(s), the kernel byte answered %zu; the actuator service 0x8001DDC4 %zu "
                  "call(s), %zu pad(s) aligned as a DualShock (pad_device.h); the pad reader's engine rumble "
                  "0x8001D7DC %zu frame(s) (the idle shake 50: %zu; the +0x184 bit 0 ground term: %zu; the lean term |+0x2A4| / stats +0xCC: %zu), motors off 0x8001DA08 %zu; "
                  "the large motor ran %zu frame(s) (mean %.1f, max %zu), the small %zu frame(s); vibration +0x08 = %u %u %u %u "
                  "(%s); controller writes %zu (a scripted run: none)\n",
                  c.padRumbles, c.padRumbleRefused, c.hitRumbles, c.motorCalls, c.foreignBytes, c.services, c.aligned,
                  c.engineRegions, c.idleShake, c.roughShake, c.lateralShake, c.offRegions, c.largeFrames, mean, c.largeMax,
                  c.smallFrames, c.vibration[0], c.vibration[1], c.vibration[2], c.vibration[3],
                  c.fromShell ? "the front end's commit, player +0x17" : "direct --race: every capture's table",
                  c.deviceWrites);
    return std::string(b) + cd;
}

} // namespace rr::game
