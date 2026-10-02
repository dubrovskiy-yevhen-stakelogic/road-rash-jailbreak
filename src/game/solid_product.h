#pragma once
// Solid roadside objects in the product: poles and props.
//
// * coll_product.cpp serves the PORTED LeanPoleTest 0x800ADC74, PoleResolve 0x800AE794, TrafficVsProp
//   0x800B2D88 and the prop reactions (solid.h) under the collision pass, and hands every natively served
//   call to SolidObserve, which keeps what happened: a bike against a pole (the shape, the contact code,
//   the hit class flagsC & 0x3F and the speed +0x240 the reaction left), a prop kicked / toppled / knocked.
// * The rider/engine pass's child 0x800A2A64, the prop animation pass, runs PORTED after the rider pass
//   (race_session.cpp RiderPass -> SolidEnginePass): a knocked prop tips over, a kicked one flies and falls.
// * Every frame SolidEnginePass also MEASURES: a riding bike whose box footprint overlaps a pole's (the
//   box widened by the pole's radius, PoleResolve's own test: it is inside the pole), and the crash chain
//   of every bike that hit a pole (the rider's stance +0x220 and mount +0x25C, frame by frame, 90 frames).
//
// DEVELOPMENT switch, the measurement's negative control: RRJB_SOLID=off makes these ports seams
// (v0 = 0, no effect).
#include <cstdint>
#include <functional>
#include <string>

#include "game/sim/integrator.h"
#include "game/sim/population.h"
#include "game/sim/road_query.h"

namespace rr::game {

// False under RRJB_SOLID=off (read once).
bool SolidOn();

// A natively served collision callee (coll_product.cpp ServeCollProduct), after it ran.
void SolidObserve(rr::sim::GuestRam& g, uint32_t fn, const uint32_t* a, int n, uint32_t v0);

// RASHCDG 0x8008ACE8's second child, PropAnimPass 0x800A2A64 (PORTED, solid.h), at the rider/engine pass's
// stack pointer `sp`, its callees (BuildObb, the ground query) through world_pop_product.h's WorldProductCallees
// on `pop`; then the frame's measurement. False: the pass refused (what it wrote up to there stays).
bool SolidEnginePass(rr::sim::GuestRam& g, int32_t dt, uint32_t sp, const rr::sim::BikeTables& t,
                     rr::sim::PopulationCallees& pop, const std::function<void(const std::string&)>& note);

// The frame log's line (empty when nothing happened since the last call) and the run's totals.
std::string SolidFrameLog();
std::string SolidTotals();

} // namespace rr::game
