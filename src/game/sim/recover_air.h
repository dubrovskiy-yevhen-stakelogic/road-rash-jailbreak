#pragma once
// The rider pass's per-rider step of a rider off the bike, and the flying rider (recover.h for the
// conventions):
//
//   RASHCDG 0x8008F404  RiderGroundStep  the rider pass's step of a rider with mount >= 2 and a live
//                                        word (0x8007DD60 / 0x8007DDBC): unless the stance's kind is 5,
//                                        the re-bind on +0x228 bit 1, then the flying step (bit 30) or
//                                        the ground step (unless bit 26); for a PLAYER's rider the view
//                                        record's shake timer +0x318 and, past 0x8000, a camera shake
//                                        (two or three Rand draws, one GTE OP, Normalize)
//   RASHCDG 0x800976C4  RiderAirStep     the flying rider (+0x228 bit 30): the player's edge record
//                                        0x800D43C0 + 28 h and the road-side / junction wall test (a
//                                        bounce off it through 0x800A966C), the velocity +0x1C8 drag,
//                                        its length +0x1E0 and the heading +0x1C2, the rows, and for a
//                                        player's rider off the road the up vector +0x20A re-probed
//   RASHCDG 0x800A966C  RiderAirHit      the flying rider hitting a wall `dir`: Bounce 0x80084564 and
//                                        the tumble axes +0x1B0 / +0x1B6 / +0x1BC; v0 0 / 1 / 2
//   SLUS    0x8003E338  JunctionMargin   the junction arm pair of an entity on a junction core: the
//                                        24-byte record of its object's +0x68 list whose road pair is
//                                        the two arms NodeWedge 0x8003EB58 names, or 0
//   SLUS    0x80038550  RoadEdgeProbe    the road's up vector at an entity's (along, lateral) blended
//                                        between its slice and the next (0x80037A30), into `out`
//                                        (normalised); v0 1, or 0 at a missing road
//   RASHCDG 0x800B208C  RiderLand        the flying rider meets the ground (the collision pass's kind
//                                        1/2 tail): the deepest corner under the up vector, the dust,
//                                        then a stuck rider stopped, a steep or fast hit bounced
//                                        (0x800A966C, sound 55), or the landing: the contact height
//                                        +0x1FC, the tumble axis flipped by the edge probe, the heading
//                                        from +0x210, the lying stance started, the velocity re-aimed,
//                                        the pad rumble; the view's +0x314 floored at 0 on bit 19
#include <cstdint>

#include "game/sim/recover.h"

namespace rr::sim {

constexpr uint32_t kRiderGroundStepFn = 0x8008F404;
constexpr uint32_t kRiderAirStepFn    = 0x800976C4;
constexpr uint32_t kRiderAirHitFn     = 0x800A966C;
constexpr uint32_t kJunctionMarginFn  = 0x8003E338; // SLUS
constexpr uint32_t kRoadEdgeProbeFn   = 0x80038550; // SLUS
constexpr uint32_t kRiderLandFn       = 0x800B208C;

// The callees the ports below reach through RecoverCallees (the bench declares these).
constexpr uint32_t kAirRoadRebindFn   = 0x800374D4; // SLUS RoadRebind(e, 1)           road_runtime.h
constexpr uint32_t kAirSettleFn       = 0x8009246C; // RiderSettle(R)
constexpr uint32_t kAirGroundFn       = 0x80097BCC; // RiderOnGround(R, dt)            recover_ground.h
constexpr uint32_t kAirRowsFromUpFn   = 0x8007EA64; // RowsFromUp(R)
constexpr uint32_t kAirStanceEventFn  = 0x800C4550; // StanceEvent(ev, r, p)           stance.h
constexpr uint32_t kAirBounceFn       = 0x80084564; // Bounce(n, d, *speed, vel, [k, floor, flat])
constexpr uint32_t kAirNodeWedgeFn    = 0x8003EB58; // SLUS NodeWedge(p, node, &a, &b, [hint])
constexpr uint32_t kAirNeighboursFn   = 0x80037A30; // SLUS NeighboursForward(p, in, out, dirs, [max])
constexpr uint32_t kAirRoadMissingFn  = 0x800394F0; // SLUS NextObjectMissing(cursor, dir) road_query.h
constexpr uint32_t kAirCornerMinFn    = 0x800B6F40; // CornerMin(c, n, q, outDepth, [outCount]) collision.h
constexpr uint32_t kAirEffectBurstFn  = 0x80027778; // SLUS EffectBurst(e, kind, life, tag)
constexpr uint32_t kAirImpulseFn      = 0x800A8DF0; // ApplyImpulse(e, v, flag)          collision.h
constexpr uint32_t kAirSoundFn        = 0x80017BA0; // SLUS PlaySound3D(x, z, id, bank)
constexpr uint32_t kAirRangedStartFn  = 0x8005C018; // RangedStart(anim, clip, a2, a3, [3 on stack])
constexpr uint32_t kAirSetRiderStateFn = 0x800C2FF4; // SetRiderState(ev, r, p)         stance.h
constexpr uint32_t kAirPadRumbleFn    = 0x800B658C; // PadRumble(e, other, speed, k, [div]) collision.h

// Frames (`addiu sp,sp,-N` at each entry).
constexpr uint32_t kRiderGroundStepFrame = 48;
constexpr uint32_t kRiderAirStepFrame    = 64;
constexpr uint32_t kRiderAirHitFrame     = 72;
constexpr uint32_t kJunctionMarginFrame  = 48;
constexpr uint32_t kRoadEdgeProbeFrame   = 200;
constexpr uint32_t kRiderLandFrame       = 104;

// The player's edge record (the wall beside the road): 0x800D43C0 + 28 * the player's bike handle.
constexpr uint32_t kAirEdgeRecords = 0x800D43C0;
constexpr uint32_t kAirEdgeStride  = 28;

// void 0x8008F404(Rider *R, s32 dt)
bool RiderGroundStep(GuestRam& g, uint32_t R, int32_t dt, uint32_t sp, const BikeTables& t, RecoverCallees& c);
// void 0x800976C4(Rider *R, s32 dt)
bool RiderAirStep(GuestRam& g, uint32_t R, int32_t dt, uint32_t sp, const BikeTables& t, RecoverCallees& c);
// s32 0x800A966C(Rider *R, const s16 dir[3], s32 k)
bool RiderAirHit(GuestRam& g, uint32_t R, uint32_t dir, int32_t k, uint32_t sp, const BikeTables& t,
                 RecoverCallees& c, uint32_t& v0);
// Record *SLUS 0x8003E338(Entity *e) - one argument.
bool JunctionMargin(GuestRam& g, uint32_t e, uint32_t sp, const BikeTables& t, RecoverCallees& c, uint32_t& v0);
// s32 SLUS 0x80038550(Entity *e, s32 along, s32 lateral, Cursor *cur, [s16 out[3]]) - the fifth
// argument arrives at sp+16.
bool RoadEdgeProbe(GuestRam& g, uint32_t e, int32_t along, int32_t lateral, uint32_t cur, uint32_t out,
                   uint32_t sp, const BikeTables& t, RecoverCallees& c, uint32_t& v0);

// void 0x800B208C(Rider *R) - one argument.
bool RiderLand(GuestRam& g, uint32_t R, uint32_t sp, const BikeTables& t, RecoverCallees& c);

} // namespace rr::sim
