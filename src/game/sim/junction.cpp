#include "game/sim/junction.h"

#include "game/sim/recover.h"
#include "game/sim/recover_air.h"
#include "game/sim/road_runtime.h"

namespace rr::sim {

namespace {

// JunctionMargin's only callee, NodeWedge SLUS 0x8003EB58 (hint = the fifth argument), natively.
struct WedgeOnly final : RecoverCallees {
    GuestRam& g;
    explicit WedgeOnly(GuestRam& m) : g(m) {}
    bool Call(uint32_t fn, const uint32_t* a, int n, uint32_t sp, uint32_t& v0) override {
        if (fn != kAirNodeWedgeFn || n < 5) return false;
        v0 = static_cast<uint32_t>(NodeWedge(g, a[0], a[1], a[2], a[3], static_cast<int32_t>(a[4]), sp));
        return !g.Faulted();
    }
};

} // namespace

bool JunctionMarginNative(GuestRam& g, uint32_t e, uint32_t sp, const BikeTables& t, uint32_t& v0) {
    WedgeOnly c(g);
    v0 = 0;
    if (!JunctionMargin(g, e, sp, t, c, v0) || g.Faulted()) {
        v0 = 0;
        return false;
    }
    return true;
}

} // namespace rr::sim
