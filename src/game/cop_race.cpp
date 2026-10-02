#include "game/cop_race.h"

#include <cstring>
#include <string>
#include <vector>

#include "game/cheats.h" // the cheat menu's "no police"
#include "game/sim/ai.h"
#include "game/sim/cops.h"
#include "game/sim/modes.h" // the player cop's JailTest 0x8009DA4C

namespace rr::game {

namespace {

constexpr uint32_t kGameStatePtr = 0x8005B2F8, kAltKind = 0x800541D4, kFightPtr = 0x8005AD4C,
                   kAttackers = 0x800CCAC0, kSqrtTable = 0x800560CC;

uint8_t* Raw(uint8_t* ram, uint32_t a, uint32_t n) {
    if (a < 0x80000000u || static_cast<uint64_t>(a) + n > 0x80200000ull) return nullptr;
    return ram + (a - 0x80000000u);
}

// The idle stance AiPushCommand / AiPopCommand play for an AI rider: the session's PORTED stance event.
struct Sink final : rr::sim::AiStanceSink {
    const AiProductHooks& h;
    bool ok = true;
    explicit Sink(const AiProductHooks& x) : h(x) {}
    void PlayIdleStance(uint16_t event, uint32_t rider) override {
        if (!h.stanceEvent || !h.stanceEvent(event, rider, 2)) ok = false;
    }
};

struct ProductCops final : rr::sim::CopCallees {
    uint8_t* ram;
    rr::sim::GuestRam& g;
    const AiProductHooks& hooks;
    AiPassCounts& counts;
    std::vector<uint8_t> noFight = std::vector<uint8_t>(12u * 256u, 0);
    ProductCops(uint8_t* r, rr::sim::GuestRam& gg, const AiProductHooks& h, AiPassCounts& c)
        : ram(r), g(gg), hooks(h), counts(c) {}
    const uint8_t* Fight() {
        const uint32_t fp = g.U32(kFightPtr);
        return fp != 0u ? Raw(ram, fp, 12u * 256u) : noFight.data();
    }
    bool Named(const char* what) {
        ++counts.unported;
        if (hooks.seam) hooks.seam(std::string(what) + " is not ported: the police port runs on without it (no effect, v0 = 0)");
        return true;
    }

    bool PushCommand(uint32_t cmd, int32_t mode, uint32_t e, uint32_t) override { // 0x800BCA68, PORTED
        uint8_t* ep = Raw(ram, e, 1096);
        uint8_t* rec = Raw(ram, cmd, 8);
        const uint32_t R = g.U32(e + 852u);
        const uint8_t* alt = Raw(ram, kAltKind, 8u * 65536u);
        const uint8_t* fight = Fight();
        if (ep == nullptr || rec == nullptr || alt == nullptr || fight == nullptr) return false;
        Sink sink(hooks);
        rr::sim::AiPushEnv pe;
        pe.raceClock = g.S32(g.U32(kGameStatePtr) + 0x10u);
        pe.rider = Raw(ram, R, 640);
        pe.riderAddress = R;
        pe.altKindTable = alt;
        pe.fightRecords = fight;
        pe.stance = &sink;
        uint8_t c[8];
        std::memcpy(c, rec, 8);
        rr::sim::AiPushCommand(c, mode, ep, pe);
        std::memcpy(rec, c, 8);
        ++counts.pushes;
        return sink.ok;
    }
    bool ClearCommands(uint32_t e, uint32_t) override { // 0x800BCD10, PORTED
        uint8_t* ep = Raw(ram, e, 1096);
        if (ep == nullptr) return false;
        rr::sim::AiClearCommands(ep);
        return true;
    }
    bool PopCommand(uint32_t e, uint32_t) override { // 0x800BC8DC, PORTED
        uint8_t* ep = Raw(ram, e, 1096);
        const uint32_t R = g.U32(e + 852u);
        Sink sink(hooks);
        rr::sim::AiPopEnv pe;
        pe.gameState = Raw(ram, g.U32(kGameStatePtr), 64);
        pe.riderDef = Raw(ram, g.U32(e + 1084u), 72);
        pe.rider = Raw(ram, R, 640);
        pe.riderAddress = R;
        pe.altKindTable = Raw(ram, kAltKind, 8u * 65536u);
        pe.fightRecords = Fight();
        pe.attackerMask = reinterpret_cast<const uint16_t*>(Raw(ram, kAttackers, 16));
        pe.stance = &sink;
        if (ep == nullptr || pe.gameState == nullptr || pe.riderDef == nullptr || pe.altKindTable == nullptr)
            return false;
        rr::sim::AiPopCommand(ep, pe);
        ++counts.pops;
        return sink.ok;
    }
    bool EndRace(uint32_t e, int32_t how, uint32_t sp) override { // 0x80092C7C, PORTED (the session's)
        ++counts.arrests;
        if (hooks.endRace) return hooks.endRace(e, how, sp);
        return Named("RASHCDG 0x80092C7C EndRace (no session hook)");
    }
    bool ComputePlace(uint32_t e, int32_t mode, uint32_t, int32_t& v0) override { // SLUS 0x800138E8
        return hooks.computePlace && hooks.computePlace(e, mode, &v0);
    }
    bool BustedMusic(uint32_t, uint32_t, uint32_t) override { // SLUS 0x8001B3C8, PORTED through the hook
        if (hooks.bustedMusic) return hooks.bustedMusic();
        return Named("SLUS 0x8001B3C8 the busted music (0x80023148)");
    }
    bool JailTest(uint32_t e, uint32_t out, uint32_t, int32_t& v0) override { // 0x8009DA4C, PORTED (modes.h)
        v0 = static_cast<int32_t>(rr::sim::ModeJailTest(g, e, out));
        return !g.Faulted();
    }
    bool JailRelease(uint32_t e, uint32_t sp) override { // 0x800A0708, PORTED (modes.h, race_modes.cpp)
        if (hooks.jailRelease) return hooks.jailRelease(e, sp);
        return Named("RASHCDG 0x800A0708 the cop-mode release");
    }
    bool KnockArrest(uint32_t e, uint32_t t, int32_t how, uint32_t sp) override { // 0x80096F30, PORTED (modes.h)
        if (hooks.arrest) return hooks.arrest(e, t, static_cast<uint32_t>(how), sp);
        return Named("RASHCDG 0x80096F30 the cop-mode arrest");
    }
};

} // namespace

bool ProductCopIdle(uint8_t* ram, rr::sim::GuestRam& g, const AiProductHooks& hooks, AiPassCounts& counts,
                    uint32_t e, uint32_t cmd, int32_t dt, uint32_t sp) {
    ProductCops c(ram, g, hooks, counts);
    return rr::sim::CopIdle(g, e, cmd, dt, sp, c) && !g.Faulted();
}

int32_t ProductCopGap(rr::sim::GuestRam& g, uint32_t e, uint32_t t, uint32_t sp) {
    return rr::sim::CopDist(g, e, t, sp);
}

bool ProductCopIntercept(uint8_t* ram, rr::sim::GuestRam& g, const AiProductHooks& hooks, AiPassCounts& counts,
                         uint32_t e, uint32_t target, uint32_t sp) {
    ProductCops c(ram, g, hooks, counts);
    const int16_t* sq = reinterpret_cast<const int16_t*>(Raw(ram, kSqrtTable, 2));
    if (sq == nullptr) return false;
    ++counts.copIntercepts;
    return rr::sim::CopIntercept(g, e, target, sp, c, sq) && !g.Faulted();
}

bool ProductCopTail(uint8_t* ram, rr::sim::GuestRam& g, const AiProductHooks& hooks, AiPassCounts& counts,
                    uint32_t e, uint32_t sp) {
    ProductCops c(ram, g, hooks, counts);
    uint32_t v0 = 0;
    if (CheatCopGivesUp()) { // cheats.h, no police: the tail's own "back to the release state" instead of CopTail -
        // clear, {4, 224} (mode 1), riderDef +0x28 = 0 (cops.cpp, the out-of-reach arm) - so it never chases or arrests
        const uint32_t cmd = sp - 56u + 16u;              // CopTail's own command record slot
        const int32_t depth = g.S8(e + 946u);
        if (depth <= 0 || g.U16(e + 948u + 8u * static_cast<uint32_t>(depth)) != 4u ||
            g.U16(e + 950u + 8u * static_cast<uint32_t>(depth)) != 224u) {
            if (!c.ClearCommands(e, sp - 56u)) return false;
            g.W16(cmd, 4);
            g.W16(cmd + 2u, 224);
            g.W32(cmd + 4u, 0);
            if (!c.PushCommand(cmd, 1, e, sp - 56u)) return false;
            g.W32(g.U32(e + 1084u) + 40u, 0);
            CheatNoteCopGaveUp();
        }
        return !g.Faulted();
    }
    ++counts.copTails;
    const bool ok = rr::sim::CopTail(g, e, sp, c, v0) && !g.Faulted();
    if (v0 != 0u) ++counts.copStops;
    return ok;
}

int ShadowSteer::Decide(const uint8_t* ram, uint32_t p, bool& throttle, bool& brake) {
    auto at = [&](uint32_t a) -> const uint8_t* { return ram + (a & 0x1FFFFFu); };
    auto w32 = [&](uint32_t a) {
        int32_t v;
        std::memcpy(&v, at(a), 4);
        return v;
    };
    auto s16 = [&](uint32_t a) {
        int16_t v;
        std::memcpy(&v, at(a), 2);
        return static_cast<int32_t>(v);
    };
    const int dir = w32(p + 0x16Cu) < 0 ? -1 : 1;
    // pace: (rival - player) progress in world units, negative = the rival ahead (+0x144 falls forward)
    const int32_t gap = (w32(rival + 0x144u) - w32(p + 0x144u)) >> 12;
    const int32_t vp = w32(p + 0x1E0u), vr = w32(rival + 0x1E0u);
    throttle = gap < -2 || (gap <= 3 && vp < vr + 0x10000);
    brake = gap > 3 && vp > 0x40000;
    // steer: the player's lateral toward the rival's + offset, on the same road only
    int32_t want = 0;
    if (w32(rival + 0x168u) == w32(p + 0x168u)) {
        const int rdir = w32(rival + 0x16Cu) < 0 ? -1 : 1;
        want = rdir * dir > 0 ? w32(rival + 0x158u) : -w32(rival + 0x158u);
        want += dir * offset;
    }
    const int32_t lat = w32(p + 0x158u);
    const int64_t d = have ? static_cast<int64_t>(lat) - prevLat : 0;
    prevLat = lat;
    have = true;
    const uint32_t slice = static_cast<uint32_t>(w32(p + 0x154u));
    if (slice >= 0x80000000u && slice < 0x801FFFC0u) {   // more than 60 degrees off the road: turn back
        const int64_t tx = dir * s16(slice + 14u), tz = dir * s16(slice + 18u);
        const int64_t hx = s16(p + 0x1C2u), hz = s16(p + 0x1C6u);
        if (hx * tx + hz * tz < 2048LL * 4096LL) {
            throttle = false;
            brake = false;
            return (hz * tx - hx * tz) > 0 ? 1 : -1;
        }
    }
    const int64_t u = dir * (static_cast<int64_t>(lat) - want + 12 * d);
    if (u > 0x6000) return 1;    // measured on race 1/20: Right lowers +0x158 in the travel sense
    if (u < -0x6000) return -1;
    return 0;
}

bool ProductCopArrestScan(uint8_t* ram, rr::sim::GuestRam& g, const AiProductHooks& hooks, AiPassCounts& counts,
                          uint32_t e, uint32_t sp) {
    ProductCops c(ram, g, hooks, counts);
    return rr::sim::CopArrestScan(g, e, sp, c) && !g.Faulted();
}

} // namespace rr::game
