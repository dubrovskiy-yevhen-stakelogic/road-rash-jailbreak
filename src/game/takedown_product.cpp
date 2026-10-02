// See takedown_product.h.
#include "game/takedown_product.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "game/sim/takedown.h"
#include "game/sim/weapon.h"

namespace rr::game {

namespace {

struct Counts {
    uint64_t takedownCalls = 0, takedownCredits = 0, takedownFaults = 0;
    uint64_t riderOffCalls = 0, riderOffMode7 = 0, riderOffStops = 0, riderOffRefused = 0;
    uint64_t leanEffects = 0, leanFaults = 0;
    uint64_t bustedCalls = 0, bustedRefused = 0;
    uint64_t seamsNamed = 0; // RRJB_TAKEDOWN=off: calls answered as named seams
};

Counts& C() {
    static Counts c;
    return c;
}

constexpr uint32_t kGameStatePtr = 0x8005B2F8;

void Off(const TakedownNote& note, const char* what) {
    ++C().seamsNamed;
    if (note) note(std::string(what) + " is not ported (RRJB_TAKEDOWN=off): its effects are absent");
}

} // namespace

bool TakedownOn() {
    static const bool on = [] {
        const char* v = std::getenv("RRJB_TAKEDOWN");
        return !(v != nullptr && std::strcmp(v, "off") == 0);
    }();
    return on;
}

uint32_t ProductTakedown(rr::sim::GuestRam& g, uint32_t B, const TakedownNote& note) {
    if (!TakedownOn()) {
        Off(note, "RASHCDG 0x800BF51C the takedown (under RiderKnockOff)");
        return 0;
    }
    ++C().takedownCalls;
    const int n = rr::sim::Takedown(g, B); // PORTED, row `takedown`
    if (g.Faulted()) {
        g.ClearFault();
        ++C().takedownFaults;
        if (note) note("RASHCDG 0x800BF51C Takedown (PORTED) met an address the console would fault on");
        return 0;
    }
    C().takedownCredits += static_cast<uint64_t>(n);
    return 0;
}

uint32_t ProductRiderOffSound(SoundRuntime& s, uint32_t h, uint32_t mode, const TakedownNote& note) {
    if (!TakedownOn()) {
        Off(note, "SLUS 0x80018440 the rider-off sound");
        return 0;
    }
    ++C().riderOffCalls;
    uint32_t v0 = 0;
    if (!s.RiderOffSound(h, mode, &v0)) { // PORTED, row `rider_off_sound`
        ++C().riderOffRefused;
        if (note) note("SLUS 0x80018440 RiderOffSound (PORTED) was not run: no sound world attached, or it faulted");
        return 0;
    }
    if (v0 == 7u) ++C().riderOffMode7;
    else if (v0 == 0xFFFFFFFFu) ++C().riderOffStops;
    return v0;
}

uint32_t ProductLeanEffect(rr::sim::GuestRam& g, uint32_t e, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4,
                           const TakedownNote& note) {
    if (!TakedownOn()) {
        Off(note, "SLUS 0x800273EC, the object sound the riding lean 0x800C3950 plays,");
        return 0;
    }
    // OURS: root counter 2 of the record's jitter (SLUS 0x8002705C) - the console's counter runs 14112
    // counts per 1/300 s tick of the race clock game_state+0x10 (as weapon_session.cpp answers it).
    rr::sim::SpineIo io;
    io.rootCounter = (g.U32(g.U32(kGameStatePtr) + 0x10u) * 14112u) & 0xFFFFu;
    rr::sim::weapon::ObjectEffect(g, e, a1, a2, a3, a4, io); // PORTED, row `weapon_object_effect`
    if (g.Faulted()) {
        g.ClearFault();
        ++C().leanFaults;
        if (note) note("SLUS 0x800273EC ObjectEffect (PORTED, the riding lean) met an address the console would fault on");
        return 0;
    }
    ++C().leanEffects;
    return 0;
}

bool ProductBustedMusic(SoundRuntime& s, const TakedownNote& note) {
    if (!TakedownOn()) {
        Off(note, "SLUS 0x8001B3C8 the busted music (0x80023148)");
        return true;
    }
    ++C().bustedCalls;
    if (!s.BustedMusic()) { // PORTED, row `busted_music`
        ++C().bustedRefused;
        if (note) note("SLUS 0x8001B3C8 BustedMusic (PORTED) was not run: no sound world attached, or it faulted");
    }
    return true;
}

std::string TakedownTotals() {
    const Counts& c = C();
    char b[520];
    std::snprintf(b, sizeof(b),
                  "the takedown port%s: Takedown 0x800BF51C %llu call(s), %llu takedown(s) credited, "
                  "%llu fault(s); RiderOffSound SLUS 0x80018440 %llu (mode 7 %llu, voices stopped %llu, not run %llu); "
                  "riding-lean ObjectEffect SLUS 0x800273EC %llu (faults %llu); BustedMusic SLUS 0x8001B3C8 %llu "
                  "(not run %llu); named seams %llu\n",
                  TakedownOn() ? "" : " SWITCHED OFF (RRJB_TAKEDOWN=off, the negative control)",
                  static_cast<unsigned long long>(c.takedownCalls), static_cast<unsigned long long>(c.takedownCredits),
                  static_cast<unsigned long long>(c.takedownFaults), static_cast<unsigned long long>(c.riderOffCalls),
                  static_cast<unsigned long long>(c.riderOffMode7), static_cast<unsigned long long>(c.riderOffStops),
                  static_cast<unsigned long long>(c.riderOffRefused), static_cast<unsigned long long>(c.leanEffects),
                  static_cast<unsigned long long>(c.leanFaults), static_cast<unsigned long long>(c.bustedCalls),
                  static_cast<unsigned long long>(c.bustedRefused), static_cast<unsigned long long>(c.seamsNamed));
    return b;
}

} // namespace rr::game
