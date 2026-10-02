#pragma once
// The product side of the contact and impact ports: the collision pass's callees that are PORTED,
// served natively where the pass (and every port under it) asks for them through
// CollisionCallees::Unported. RaceSession's ProductCollisionCallees calls ServeCollProduct first and
// names everything it declines as a seam.
#include <cstdint>
#include <functional>
#include <map>
#include <string>

#include "game/sim/collision.h"
#include "game/sim/integrator.h"
#include "game/sim/road_query.h"

namespace rr::game {

struct CollProductEnv {
    const rr::sim::BikeTables* tables = nullptr;
    // RASHCDG 0x800C4550 the stance event (PORTED, stance.h), run by the session's stance layer.
    std::function<bool(uint32_t ev, uint32_t rider, uint32_t p, uint32_t& v0)> stanceEvent;
    // SLUS 0x80043F00 GetRCnt(0xF2000002): the product's stand-in for root counter 2 (the one
    // ImpactStatePass's sound pick already uses): OURS, the hardware timer is not modelled.
    uint32_t* rcnt = nullptr;
    std::function<void(const std::string&)> note;
    std::map<uint32_t, size_t> served; // native serves this frame, by entry
};

// True when `fn` was answered here (`v0`, `ok` set; ok false = the port declined, the caller must not
// trust what it wrote); false when it is not served, and the caller treats it as a seam.
bool ServeCollProduct(rr::sim::GuestRam& g, rr::sim::CollisionCallees& c, CollProductEnv& env, uint32_t fn,
                      const uint32_t* a, int n, uint32_t sp, uint32_t& v0, bool& ok);

// " 0x800AB7A0 x3 ..." for the frame log, and the run's totals (a process-wide tally, for the report).
std::string CollServedNote(const std::map<uint32_t, size_t>& served);
std::string CollServedTotals();

// The contact partners: false under RRJB_PARTNERS=off (read once), the negative control
// that makes 0x800B2844 / 0x800B2B00 / 0x800B2AF8 / 0x800B2D44 / 0x80084BE8 / 0x8007ED64 seams again.
bool PartnersOn();
// Its counters line (part of CollServedTotals).
std::string PartnersTotals();

} // namespace rr::game
