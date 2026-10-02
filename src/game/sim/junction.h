#pragma once
// The junction margin record, run whole natively:
//
//   SLUS 0x8003E338 JunctionMargin (recover_air.h, bench row `junction_margin` with NodeWedge on the oracle)
//   with its only callee SLUS 0x8003EB58 NodeWedge (road_runtime.h, PORTED) served natively - the
//   composition the product's rider pass pool loop (0x8007BAD0) runs; bench row `junction_margin_native`.
#include <cstdint>

#include "game/sim/integrator.h"
#include "game/sim/road_query.h"

namespace rr::sim {

// Record *SLUS 0x8003E338(Entity *e) at stack pointer `sp`. False: a guest fault (v0 is then 0).
bool JunctionMarginNative(GuestRam& g, uint32_t e, uint32_t sp, const BikeTables& t, uint32_t& v0);

} // namespace rr::sim
