#include "game/sim/coll_serve.h"

#include "game/sim/solid.h" // the solid roadside objects
#include "game/sim/partners.h" // the pool loop's partner resolvers

namespace rr::sim {

bool ServeCollNative(GuestRam& g, const CollCall& call, const BikeTables& t, CollisionCallees& c, uint32_t& v0,
                     bool& ok) {
    return ServeContactPair(g, call, t, c, v0, ok) || ServeImpactSolve(g, call, t, c, v0, ok) ||
           ServeHitSpeed(g, call, t, c, v0, ok) || ServeBikeReact(g, call, t, c, v0, ok) ||
           ServeResolvers(g, call, t, c, v0, ok) || ServeSolid(g, call, t, c, v0, ok) ||
           ServePartners(g, call, t, c, v0, ok);
}

} // namespace rr::sim
