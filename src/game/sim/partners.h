#pragma once
// The last partner resolvers of the collision pass's kind loop and the two reactions the rider pass's
// pool loop still asked for. Transcribed from our own
// disassembly of the player's own images:
//
//   RASHCDG.BIN  SHA-1 cfe43a7786759f2cb9c57751cf99e84d1074782c, loaded at 0x8005B5E8
//   SLUS_010.53  SHA-1 67ed165a2c517d4e6106fb0dfa324d66dd9a76f1, text at 0x80010000
//
// and accepted only by `rrverify phys` rows (tools\rrverify\rows_partners.inc). Memory model and stack
// rule: collision.h / resolvers.h - guest addresses through GuestRam, `sp` the stack pointer AT ENTRY,
// every local a callee reads through a pointer kept at the original's frame address. Ported leaves are
// called natively (InCameraBox, ApplyImpulse, ResponseFar, FirstPointInsideBox, FaceNormal, Scale,
// NoteBigVolume, MulAdd, Asin, LaunchLift, FixMul); PedHit 0x800A9868 goes through
// CollisionCallees::Unported, the sound through PlaySound3D. False: the view faulted or a leaf refused.
#include <cstdint>

#include "game/sim/coll_serve.h"

namespace rr::sim {

namespace partners {
constexpr uint32_t kRiderVsTraffic = 0x800B2844;  // (rider, car)
constexpr uint32_t kRiderNone = 0x800B2AF8;       // (rider, rider): `jr ra`
constexpr uint32_t kRiderVsShape = 0x800B2B00;    // (rider, shape)
constexpr uint32_t kTrafficVsTraffic = 0x800B2D44; // (car, car)
constexpr uint32_t kLaunch = 0x80084BE8;          // (bike, k)
constexpr uint32_t kTurnFacing = 0x8007ED64;      // (bike, ang)
constexpr uint32_t kPropVsShape = 0x800B2E64;     // (prop, shape): camera_collide.h's PropVsShape
constexpr uint32_t kRiderGrab = 0x800C29F0;       // (rider, bike): BikeVsRider's grab
constexpr uint32_t kPedVoice = 0x8001B44C;        // SLUS (x, z, e, flag): BikeVsRider's pedestrian voice
} // namespace partners

// What a call did, for the product's counters (nullptr: not wanted).
struct PartnerTrace {
    int pushes = 0;   // ApplyImpulse ran (a box was moved)
    int pedAsks = 0;  // PedHit 0x800A9868 asked
    int pedHits = 0;  // ... and answered non-zero
    int sounds = 0;   // PlaySound3D (id 19, the body hit)
    int carMarks = 0; // the car's +0x1FD bits 4 / 7 set (the side-hit mark)
};

// RASHCDG 0x800B2844 RiderVsTraffic(rider, car) - 692 B, frame 88: in the camera box (20.0, 28.0) the first
// rider corner inside the car's box (FirstPointInsideBox mask 16; 8 = none: return), the car's face normal,
// the rider pushed out by depth + 0.125 (ApplyImpulse flag 1), codes face | 0x100 / corner | 0x200; outside
// the box ResponseFar(rider, car) and its push applied to whom it names, the normal = -car row +0x1B6.
// With a contact: the heading dot (FixMul of +0x128 / +0x12C) times the car's speed against the rider's
// speed and the side the car is on clears the second code when the rider is not closing; PedHit(rider, n,
// car speed, car +0x1C2, [&car->AC, 0]) and, when it hits and either speed is above 1.0059, the sound 19.
// The second code's low byte 3: the car's +0x1FD |= 0x80 (unless bit 4 already) | 0x10.
bool RiderVsTraffic(GuestRam& g, uint32_t e, uint32_t car, uint32_t sp, const BikeTables& t, CollisionCallees& c,
                    PartnerTrace* tr = nullptr);

// RASHCDG 0x800B2B00 RiderVsShape(rider, shape) - 408 B, frame 80: a pool-6 volume of height > 2.0 whose +8
// is not 1 goes on the big-volume list (NoteBigVolume); the first rider corner inside the shape's box (8:
// return), the face normal, the push out by depth + 0.125; then PedHit(rider, n, 0, n, [shape, 0]) when the
// rider is a pool-1 entity or its speed class clamp(|+0x1E0| >> 17, 0, 4) is positive (else 1), and on
// non-zero the sound 19 at the rider.
bool RiderVsShape(GuestRam& g, uint32_t e, uint32_t shape, uint32_t sp, const BikeTables& t, CollisionCallees& c,
                  PartnerTrace* tr = nullptr);

// RASHCDG 0x800B2D44 TrafficVsTraffic(car, other) - 68 B, frame 56: ResponseFar(car, other) and its push
// applied to whom it names (ApplyImpulse flag 1).
bool TrafficVsTraffic(GuestRam& g, uint32_t car, uint32_t other, uint32_t sp, const BikeTables& t,
                      PartnerTrace* tr = nullptr);

// RASHCDG 0x80084BE8 Launch(bike, k) - 432 B, frame 40: the box centre +0xB8 = MulAdd(+0x1F8, +0x20A,
// +0x300); by the contact partner's pool (*+0x340 >> 5): 3 (a car) angle 170 / k 0xB1333, 5 (a shape) by
// its model kind (8: 568 / 0xB1333, 6: 341 / 0x6A51E, else 455 / 0x8DC28), else 56 / 0x6A51E. k == 0
// launches the latched direction +0x360 (latched from +0x1C2 / +0x1E0 with flagsC bit 25 the first time),
// else +0x1C2 itself: LaunchLift(dir, +0x240, cos(angle + 1024 - Asin(dir.y << 4)), k); flagsC |= 0xC00.
bool Launch(GuestRam& g, uint32_t e, int32_t k, uint32_t sp, const BikeTables& t);

// RASHCDG 0x8007ED64 TurnFacing(bike, ang) - 488 B, frame 40: r = 3/4 of the length +0x134 (negated for
// ang > 0); the box centre +0xB8 = +0x1F8 + FixMul(row +0x1B6 << 4, FixMul(r, sin ang << 4)) +
// FixMul(row +0x210 << 4, FixMul(r, 1.0 - cos ang << 4)), per component.
void TurnFacing(GuestRam& g, uint32_t e, int32_t ang);

// RASHCDG 0x800B2E64 as the collision pass reaches it: camera_collide.h's PropVsShape plus the one stack word
// it leaves out - FirstPointInsideBox (frame 56) saves the caller's s2 = shape + 0x104 at its sp + 24, i.e.
// sp - 88 - 32, and PropTopple(e, 0, n, -1) reads exactly that word as its depth (its sp + 48). Written
// first here: the native leaves below never touch it.
bool PropVsShapePass(GuestRam& g, uint32_t e, uint32_t shape, uint32_t sp, CollisionCallees& c, uint32_t& v0);

// RASHCDG 0x800C29F0 RiderGrab(rider, bike) - 1196 B, frame 56 (BikeVsRider 0x800AD04C asks for it when a
// pool-1 rider is under the bike). d = DotLcm(bike row +0x1BC, rider heading +0x1C2). |d| > 0xDDB2 (head on or
// from behind): the bike's owner +0x354 and the passenger's owner (+0x358), each when its +0x25C < 2, take the
// latched heading +0x360 into +0x1C8; d > 0 also gives them the speed FixMul(1.2, rider +0x1E0) and +0x228 |=
// 0x204000, and a bike with +0x268 == 0 gets +0x270 = 0x2DD62D, +0x278 = 0, +0x26C = -8.0; d <= 0: bike +0x238
// |= 0x880 and the owners +0x228 |= 0x208000. Otherwise (from the side) the one on the side DotLcm(row +0x1B0,
// heading) points at (the bike when > 0 or with no passenger): its owner, if +0x25C < 2, +0x228 bits 17/18
// cleared and |= 0x8000 | (0x20000 << side) with a passenger, else |= 0x8000 and the bike's +0x238 |= 0x840 |
// (side ? 0x8000 : 0x10000). Each grabbed one: FightStat(rider +0x254, 1, 0), FightStat(it, 1, 1) and, for a
// player's +0x254, the HUD flag 0x800D6224[min(h, 1)] = 1. Last: bike +0x43C's byte +0x25 -= its +0x24 >> 3
// (floored at 0).
void RiderGrab(GuestRam& g, uint32_t rider, uint32_t bike, uint32_t sp);

// SLUS 0x8001B44C PedVoice(x, z, e, flag) - 17 instructions, frame 24 (SLUS_010.53 SHA-1
// 67ed165a2c517d4e6106fb0dfa324d66dd9a76f1): flag 0 does nothing; else PlaySound3D(x, z, id, *(gp + 1912))
// with id 104 when e's model record word **(e + 0x60) is 430, else 103. False: the sound refused.
bool PedVoice(GuestRam& g, int32_t x, int32_t z, uint32_t e, uint32_t flag, CollisionCallees& c);

// The group's serve (coll_serve.h): false when `fn` is not one of the above.
bool ServePartners(GuestRam& g, const CollCall& call, const BikeTables& t, CollisionCallees& c, uint32_t& v0,
                   bool& ok);

// The product's counters of every call served through ServePartners (the bench ignores them).
struct PartnerTotals {
    size_t riderTraffic = 0, riderShape = 0, trafficTraffic = 0, riderNone = 0, launches = 0, turns = 0;
    size_t propShape = 0, propContacts = 0, propTopples = 0; // 0x800B2E64: calls, a corner inside, PropTopple
    size_t grabs = 0, grabbed = 0;                           // 0x800C29F0: calls, riders it took hold of
    size_t voices = 0, voiced = 0;                           // 0x8001B44C: calls, sounds played
    PartnerTrace rt, rs, tt; // summed per resolver
};
PartnerTotals& PartnerCounters();

} // namespace rr::sim
