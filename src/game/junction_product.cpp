#include "game/junction_product.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "game/sim/junction.h"

namespace rr::game {

namespace {

struct Totals {
    uint64_t calls = 0;    // pool-loop asks (a live bike on a zero-margin slice)
    uint64_t records = 0;  // asks answered with a record: the fan's edge planes applied
    uint64_t refused = 0;  // the port refused (a guest fault)
    uint64_t off = 0;      // asks answered 0 by the negative control
};
Totals& Tally() {
    static Totals t;
    return t;
}

} // namespace

bool JunctionOn() {
    static const bool on = [] {
        const char* v = std::getenv("RRJB_JUNCTION");
        return !(v != nullptr && std::strcmp(v, "off") == 0);
    }();
    return on;
}

bool PoolJunctionMargin(rr::sim::GuestRam& g, uint32_t e, uint32_t sp, const rr::sim::BikeTables& t, uint32_t& v0) {
    Totals& n = Tally();
    ++n.calls;
    v0 = 0;
    if (!JunctionOn()) {
        ++n.off;
        return true;
    }
    if (!rr::sim::JunctionMarginNative(g, e, sp, t, v0)) {
        ++n.refused;
        v0 = 0;
        return false;
    }
    if (v0 != 0) ++n.records;
    return true;
}

std::string JunctionTotals() {
    const Totals& n = Tally();
    char b[320];
    std::snprintf(b, sizeof(b),
                  "junction margin SLUS 0x8003E338 (PORTED, the rider pass's pool loop)%s: %llu ask(s), %llu answered "
                  "with the fan's arm-pair record, %llu refused, %llu answered 0 by RRJB_JUNCTION=off\n",
                  JunctionOn() ? "" : " SWITCHED OFF (RRJB_JUNCTION=off, the negative control)",
                  static_cast<unsigned long long>(n.calls), static_cast<unsigned long long>(n.records),
                  static_cast<unsigned long long>(n.refused), static_cast<unsigned long long>(n.off));
    return b;
}

} // namespace rr::game
