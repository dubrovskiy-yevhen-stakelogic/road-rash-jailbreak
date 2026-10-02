#pragma once
// The junction records in the product: the rider pass's first pool loop
// (RASHCDG 0x8007B840, region [0x8007B894, 0x8007C23C), bike_react.h RiderPassPoolLoop) asks, for a bike on
// a slice whose margin byte (SLCT +0x30 / +0x31) is 0 - a junction core - for the junction's arm-pair record
// through SLUS 0x8003E338 (0x8007BAD0). That function is PORTED (recover_air.h JunctionMargin, bench row
// `junction_margin`); its only callee NodeWedge SLUS 0x8003EB58 is PORTED too (road_runtime.h). With the
// record, the bike's two edge planes are the record's (+4 point, +16 normal) instead of none, so a bike
// inside a junction fan meets the fan's own walls as on the console.
//
// DEVELOPMENT switch, the negative control: RRJB_JUNCTION=off answers 0 (no record).
#include <cstdint>
#include <string>

#include "game/sim/integrator.h"
#include "game/sim/road_query.h"

namespace rr::game {

// False under RRJB_JUNCTION=off (read once).
bool JunctionOn();

// SLUS 0x8003E338(e) at the pool loop's stack pointer `sp`, natively. False: the port refused (a guest
// fault); `v0` is then 0.
bool PoolJunctionMargin(rr::sim::GuestRam& g, uint32_t e, uint32_t sp, const rr::sim::BikeTables& t, uint32_t& v0);

// The run's counters line.
std::string JunctionTotals();

} // namespace rr::game
