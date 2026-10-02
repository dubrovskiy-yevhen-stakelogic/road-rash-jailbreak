#include "game/coll_product.h"
#include "game/solid_product.h"
#include "game/wheelie.h" // the car hit in a wheelie


#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "game/sim/coll_serve.h"
#include "game/sim/effects.h"
#include "game/sim/partners.h"
#include "game/sim/stance.h"

namespace rr::game {

bool PartnersOn() {
    static const bool on = [] {
        const char* v = std::getenv("RRJB_PARTNERS");
        return !(v != nullptr && std::strcmp(v, "off") == 0);
    }();
    return on;
}

std::string PartnersTotals() {
    const rr::sim::PartnerTotals& n = rr::sim::PartnerCounters();
    char b[1400];
    std::snprintf(b, sizeof(b),
                  "the contact partners%s: rider/traffic 0x800B2844 %zu call(s): %d push(es), PedHit "
                  "asked %d, reacted %d, body-hit sound %d, car side-mark %d; rider/shape 0x800B2B00 %zu call(s): %d "
                  "push(es), PedHit asked %d, reacted %d, sound %d; traffic/traffic 0x800B2D44 %zu call(s): %d "
                  "push(es); rider/rider 0x800B2AF8 %zu; launch 0x80084BE8 %zu; facing turn 0x8007ED64 %zu; prop/shape "
                  "0x800B2E64 %zu call(s): %zu contact(s), %zu handed to PropTopple; rider grab 0x800C29F0 %zu "
                  "call(s): %zu grabbed; pedestrian voice SLUS 0x8001B44C %zu call(s): %zu sound(s)\n",
                  PartnersOn() ? "" : " [RRJB_PARTNERS=off: seams]", n.riderTraffic, n.rt.pushes, n.rt.pedAsks,
                  n.rt.pedHits, n.rt.sounds, n.rt.carMarks, n.riderShape, n.rs.pushes, n.rs.pedAsks, n.rs.pedHits,
                  n.rs.sounds, n.trafficTraffic, n.tt.pushes, n.riderNone, n.launches, n.turns, n.propShape,
                  n.propContacts, n.propTopples, n.grabs, n.grabbed, n.voices, n.voiced);
    return b;
}

namespace {
constexpr uint32_t kGetRCnt = 0x80043F00; // SLUS
// What the product runs natively: a port is served only once every callee it asks for is served too
// (or is a void seam whose absence only drops an effect). A seam answering v0 = 0 is NOT neutral for a
// value-returning callee - the face leaves' 0 means "face 0 hit" - so a port whose value-returning
// callees are still seams stays a seam itself. Each entry is enabled after its group's rows pass.
bool Enabled(uint32_t fn) {
    switch (fn) {
    // contact.cpp - rows_coll_pair.inc
    case 0x800ABE78: case 0x800AA34C: case 0x800AA140: case 0x800B5B48: case 0x800AA474: case 0x800AAD30:
    case 0x800AB7A0:
    // hit_speed.cpp - rows_coll_speed.inc (no unported callee)
    case 0x80080D1C: case 0x80080B10: case 0x80083928: case 0x80083F30:
    // bike_react.cpp - rows_coll_react.inc (void seams left: RiderSpeech SLUS 0x8001A760, SLUS 0x80027258)
    case 0x800AC56C: case 0x800A8BE0: case 0x800A9408: case 0x800AC130: case 0x800A91AC: case 0x800AC958:
    case 0x800AC5BC:
    // impact_solve.cpp - rows_coll_solve.inc (void seams left: the launch 0x80084BE8, 0x8007E868,
    // SLUS 0x80027258; PropKnock / PropKick / SLUS 0x80017B30 only on a prop, and pool 4 is empty)
    case 0x800B5EB4: case 0x800AD9BC: case 0x800B0510: case 0x800AF3B0: case 0x800AF224: case 0x800AF0A0:
    case 0x800B11B4: case 0x800B12A0:
    // resolvers.cpp - rows_coll_resolve.inc: the leaves, Dot32, PointResolve, BoxResolve. NOT PoleResolve
    // (its 0x800ADC74 returns a value and is unported), RiderWallHit and BikeVsRider (PedHit 0x800A9868
    // returns a value and is unported): they stay seams.
    case 0x800B675C: case 0x800B6B58: case 0x800B6D70: case 0x800B7030: case 0x800B71AC: case 0x800B74F0:
    case 0x800B7810: case 0x800B2C98: case 0x8002E604: case 0x800B09C4: case 0x800B0D8C:
    // PedHit 0x800A9868 is PORTED (ped_hit.h, served by the session's recover dispatch), so
    // RiderWallHit runs natively.
    case 0x800B2794:
    // BikeVsRider 0x800AD04C (benched, rows_coll_resolve.inc). The contact response 0x800AAD30 divides by the
    // rider's length +0x134 (0x800AB1F0), so it needs SpawnBike's rider box (rider_model.h): with a 0 x 0 x 0
    // box it throws the rider 8897 units.
    // Its void callees the rider grab 0x800C29F0 and the pedestrian voice SLUS 0x8001B44C: partners2 (below).
    case 0x800AD04C:
        return true;
    // Solid objects (rows_solid.inc): LeanPoleTest 0x800ADC74 is ported, so PoleResolve
    // 0x800AE794 runs natively (bikes hit poles, signs, trees); the car against a prop 0x800B2D88, the
    // prop reactions PropKick 0x800B2F94 / PropTopple 0x800B3344 / PropKnock 0x800B3838 / PropUpRows
    // 0x800A3CBC and the surface sound SLUS 0x80017B30 (the leaf the solver asks for on a prop).
    case 0x800ADC74: case 0x800AE794: case 0x800B2D88: case 0x800B2F94: case 0x800B3344: case 0x800B3838:
    case 0x800A3CBC: case 0x80017B30: case 0x80071A28: case 0x8007198C: case 0x800A40D4:
        return rr::game::SolidOn(); // RRJB_SOLID=off: the negative control (solid_product.h)
    // Contact partners (rows_partners.inc): the rider against a car 0x800B2844 / a shape
    // 0x800B2B00 / a rider 0x800B2AF8 (`jr ra`), the car against a car 0x800B2D44, the launch 0x80084BE8.
    case 0x800B2844: case 0x800B2B00: case 0x800B2AF8: case 0x800B2D44: case 0x80084BE8:
    // partners2: the prop partner 0x800B2E64 (camera_collide.h's PropVsShape; PropTopple from the solid group)
    // and BikeVsRider's rider grab 0x800C29F0 and pedestrian voice SLUS 0x8001B44C.
    case 0x800B2E64: case 0x800C29F0: case 0x8001B44C:
        return PartnersOn(); // RRJB_PARTNERS=off: the negative control
    default: return false;
    }
}

// BikeBikeGate's four uninitialised frame words (crash.h): the live game
// leaves {8, 0x800B4C58, 0x801B7F84, 0x00140000} at the gate's sp + 108 / 116 / 120 / 124 in every live
// call the crash runs recorded; native callers never write that stack, so the product plants them where
// BikeBikeReact 0x800AC130 (frame 72) will call the gate (frame 280).
void PlantGateStale(rr::sim::GuestRam& g, uint32_t sp) {
    const uint32_t gsp = sp - 72u - 280u;
    g.W32(gsp + 108, 8u);
    g.W32(gsp + 116, 0x800B4C58u);
    g.W32(gsp + 120, 0x801B7F84u);
    g.W32(gsp + 124, 0x00140000u);
}

std::map<uint32_t, size_t>& Totals() {
    static std::map<uint32_t, size_t> t;
    return t;
}
} // namespace

bool ServeCollProduct(rr::sim::GuestRam& g, rr::sim::CollisionCallees& c, CollProductEnv& env, uint32_t fn,
                      const uint32_t* a, int n, uint32_t sp, uint32_t& v0, bool& ok) {
    if (fn == rr::sim::kStanceEventFn && env.stanceEvent) {
        ok = env.stanceEvent(a[0], a[1], n > 2 ? a[2] : 0u, v0);
        ++env.served[fn];
        ++Totals()[fn];
        return true;
    }
    if (fn == kGetRCnt && env.rcnt != nullptr) {
        if (env.note)
            env.note("the contact reactions' GetRCnt(root counter 2) is answered by a deterministic counter of ours "
                     "(the same stand-in ImpactStatePass uses): the hardware timer is not modelled");
        *env.rcnt = *env.rcnt * 1103515245u + 12345u;
        v0 = (*env.rcnt >> 16) & 0xFFFFu;
        ok = true;
        ++env.served[fn];
        ++Totals()[fn];
        return true;
    }
    if (fn == rr::sim::kFxSurfaceFn && n >= 2) {
        // SLUS 0x80027258 SurfaceFx, PORTED (effects.h, rows_fx.inc `fx_surface`): the spark record a wall
        // scrape or a bike contact spawns. Its jitter reads root counter 2 twice; both reads get the
        // stand-in counter's current value (OURS; the counter is not advanced here, so its other consumers
        // see an unchanged sequence).
        rr::sim::FxEnv fx;
        fx.io.rootCounter = env.rcnt != nullptr ? (*env.rcnt >> 16) & 0xFFFFu : 0u;
        rr::sim::SurfaceFx(g, fx, a[0], a[1]);
        v0 = 0;
        ok = !g.Faulted() && !fx.refused;
        ++env.served[fn];
        ++Totals()[fn];
        return true;
    }
    if (env.tables == nullptr || !Enabled(fn)) return false;
    if (fn == 0x800AC130u) PlantGateStale(g, sp);
    // game/wheelie.h: the player's bike against a car in a wheelie - the ORIGINAL's contact test runs, and
    // on a contact its reaction is replaced by the game's own launch over the car (an alternative outcome of OURS, only
    // with the wheelie option's "over cars" on and the front wheel up; false: the port runs below as always)
    if (fn == 0x800AC5BCu && rr::game::WheelieServeBikeVsTraffic(g, c, *env.tables, a, n, sp, v0, ok)) {
        ++env.served[fn];
        ++Totals()[fn];
        return true;
    }

    if (!rr::sim::ServeCollNative(g, rr::sim::CollCall{fn, a, n, sp}, *env.tables, c, v0, ok)) return false;
    ++env.served[fn];
    ++Totals()[fn];
    rr::game::SolidObserve(g, fn, a, n, v0); // the solid objects' log and measurement (solid_product.h)
    return true;
}

std::string CollServedNote(const std::map<uint32_t, size_t>& served) {
    std::string s;
    for (const auto& [fn, k] : served) {
        char b[40];
        std::snprintf(b, sizeof(b), " 0x%08X x%zu", fn, k);
        s += b;
    }
    return s;
}

std::string CollServedTotals() {
    std::string s = "the contact/impact ports served natively under the collision pass:";
    if (Totals().empty()) return s + " none\n" + PartnersTotals();
    return s + CollServedNote(Totals()) + "\n" + PartnersTotals();
}

} // namespace rr::game
