#pragma once
// The contact and impact ports of the collision family as they are REACHED: every
// port of the collision family calls every function that is not written inline through
// `CollisionCallees::Unported(fn, a, n, sp, v0)` - in the bench that runs the original code at the
// exact stack pointer (a row proves only its own body), in the product it lands here, where each
// group's `Serve*` runs its native port when `fn` is one of its own.
//
// A Serve function returns false when `fn` is not its group's (then nothing ran); otherwise `ok` is
// the port's own result (false: the caller must not trust anything it wrote, exactly as a declining
// port) and `v0` its return value. `a[0..n)` are the o32 arguments in order, the fifth and later the
// ones the original passes at sp + 16 ..; `sp` is the stack pointer the original makes the call at.
// `c` is handed down unchanged: a served port's own unported callees come back through it.
#include <cstdint>

#include "game/sim/collision.h"
#include "game/sim/integrator.h"
#include "game/sim/road_query.h"

namespace rr::sim {

struct CollCall {
    uint32_t fn;
    const uint32_t* a;
    int n;
    uint32_t sp;
};

// contact.cpp: the bike pair (BikeVsBike 0x800AB7A0, BikePairClassify 0x800ABE78, the responses
// 0x800AA140 / 0x800AA34C / 0x800AAD30 / 0x800AA474, PairPushDir 0x800B5B48).
bool ServeContactPair(GuestRam& g, const CollCall& call, const BikeTables& t, CollisionCallees& c, uint32_t& v0,
                      bool& ok);
// impact_solve.cpp: the impact outcome solver 0x800AF3B0 and what hangs off it (FaceCrossing
// 0x800B5EB4, LandOnTop 0x800AD9BC, PropHitFace 0x800B0510), ImpactGate 0x800AF224, PoleReact
// 0x800AF0A0, BoxReact 0x800B11B4, BikeWallHit 0x800B12A0.
bool ServeImpactSolve(GuestRam& g, const CollCall& call, const BikeTables& t, CollisionCallees& c, uint32_t& v0,
                      bool& ok);
// hit_speed.cpp: the speed hand-over HitSpeed 0x80080D1C, TakePartnerHeading 0x80080B10, HitOutcome
// 0x80083928, ImpactTurn 0x80083F30.
bool ServeHitSpeed(GuestRam& g, const CollCall& call, const BikeTables& t, CollisionCallees& c, uint32_t& v0,
                   bool& ok);
// bike_react.cpp: BikeBikeReact 0x800AC130, ImpactSeverity 0x800A9408, 0x800AC56C, 0x800A8BE0, the
// traffic pair (BikeVsTraffic 0x800AC5BC, 0x800A91AC, BikeTrafficReact 0x800AC958).
bool ServeBikeReact(GuestRam& g, const CollCall& call, const BikeTables& t, CollisionCallees& c, uint32_t& v0,
                    bool& ok);
// resolvers.cpp: the box / face geometry leaves (0x800B675C, 0x800B6D70, 0x800B7030, 0x800B71AC,
// 0x800B74F0 with 0x800B7810, 0x800B6B58, 0x800B2C98) and the partner resolvers (PointResolve
// 0x800B09C4, BoxResolve 0x800B0D8C, PoleResolve 0x800AE794, BikeVsRider 0x800AD04C, RiderWallHit
// 0x800B2794).
bool ServeResolvers(GuestRam& g, const CollCall& call, const BikeTables& t, CollisionCallees& c, uint32_t& v0,
                    bool& ok);

// All of the above in turn (coll_serve.cpp): false when no group ports `fn`.
bool ServeCollNative(GuestRam& g, const CollCall& call, const BikeTables& t, CollisionCallees& c, uint32_t& v0,
                     bool& ok);

} // namespace rr::sim
