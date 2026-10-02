#pragma once
// Bike-level gameplay ported from the race overlay `RASHCDG.BIN`,
// SHA-1 cfe43a7786759f2cb9c57751cf99e84d1074782c, loaded at 0x8005B5E8 (file offset 0 = that
// address).
//
// A motorcycle is entity pool 0 (docs\formats\population.md 1): 18 slots of 1096 bytes. The port
// deliberately addresses it as raw bytes through `EntityView` instead of declaring a C struct: only
// the fields we have actually read out of the original's code are named, and a field we have not
// understood cannot be silently given a wrong type.
#include <cstdint>

namespace rr::sim {

// A little-endian view over one 1096-byte pool-0 entity.
class EntityView {
public:
    explicit EntityView(uint8_t* base) : b_(base) {}

    uint32_t U32(uint32_t off) const {
        return static_cast<uint32_t>(b_[off]) | (static_cast<uint32_t>(b_[off + 1]) << 8) |
               (static_cast<uint32_t>(b_[off + 2]) << 16) | (static_cast<uint32_t>(b_[off + 3]) << 24);
    }
    void SetU32(uint32_t off, uint32_t v) {
        b_[off] = static_cast<uint8_t>(v);
        b_[off + 1] = static_cast<uint8_t>(v >> 8);
        b_[off + 2] = static_cast<uint8_t>(v >> 16);
        b_[off + 3] = static_cast<uint8_t>(v >> 24);
    }
    uint16_t U16(uint32_t off) const {
        return static_cast<uint16_t>(static_cast<uint32_t>(b_[off]) | (static_cast<uint32_t>(b_[off + 1]) << 8));
    }
    int16_t S16(uint32_t off) const { return static_cast<int16_t>(U16(off)); }
    void SetU16(uint32_t off, uint16_t v) {
        b_[off] = static_cast<uint8_t>(v);
        b_[off + 1] = static_cast<uint8_t>(v >> 8);
    }
    uint8_t* bytes() { return b_; }

private:
    uint8_t* b_;
};

// The entity offsets this file needs, all read out of the original's own code.
namespace ent {
constexpr uint32_t kHandle   = 0x0AC; // 172 - u16, (pool << 5) | slot (population.md 1.1)
constexpr uint32_t kClass    = 0x0B4; // 180
constexpr uint32_t kObbCentre = 0x0B8; // 184 - 3 x 16.16
constexpr uint32_t kObbCorners = 0x0C4; // 196 - 8 corners of 3 x 16.16, ending at +0x123
constexpr uint32_t kHalfX    = 0x130; // 304 - 16.16 half extent along axis 0
constexpr uint32_t kHalfY    = 0x134; // 308 - 16.16 half extent along axis 2
constexpr uint32_t kHalfZ    = 0x138; // 312 - 16.16 half extent along axis 1
constexpr uint32_t kAxes     = 0x1B0; // 432 - the 3x3 orientation, three s16[3] rows
constexpr uint32_t kOwner    = 0x354; // 852 - -> a second object, whose +0x25C gates the crouch
constexpr uint32_t kStats    = 0x22C; // 556 - -> the per-rider stat block (0x80079B68, 0x80075C0C)
constexpr uint32_t kFlagsA   = 0x230; // 560 - the main per-frame flag word
constexpr uint32_t kFlagsB   = 0x234; // 564
constexpr uint32_t kFlagsC   = 0x238; // 568
constexpr uint32_t kSpeedCopy = 0x240; // 576 - cleared by the idle step
constexpr uint32_t kHeading  = 0x1C2; // 450 - s16[3], 4096 = 1.0 (written by 0x8007AE38/40/48)
constexpr uint32_t kIdleA    = 0x1E8; // 488 - idle random walk, clamped to +-1.0
constexpr uint32_t kIdleRev  = 0x25C; // 604 - the idle rev, ramped at 10000.0/s toward a ceiling
constexpr uint32_t kIdleLean = 0x268; // 616 - idle lean target, clamped to +-5719
constexpr uint32_t kIdleYaw  = 0x27C; // 636 - idle yaw, clamped to +-17157
constexpr uint32_t kIdleB    = 0x2A4; // 676 - idle random walk, clamped to +-13107
constexpr uint32_t kIdleTimer = 0x2BC; // 700 - reset to 2.0 by the idle step
constexpr uint32_t kImpact   = 0x334; // 820 - s16[3], the impact direction (0x80075B9C/A0/B4)
constexpr uint32_t kIdleTilt = 0x33A; // 826 - s16, derived from kIdleYaw
constexpr uint32_t kMode     = 0x008; // 8   - s8; the lean is only updated while this is < 2
constexpr uint32_t kSteerBias = 0x29C; // 668 - added to kSteer before the clamp
constexpr uint32_t kLeanBias = 0x2DC; // 732
constexpr uint32_t kLeanGain = 0x2E0; // 736
// The steering angle and the lean angle are the same two words the idle model walks: 0x27C is the
// steer input (kIdleYaw) and 0x33A is the lean (kIdleTilt). Named twice on purpose - the engine
// uses them for both jobs and the offsets are what is proven, not the names.
constexpr uint32_t kSteer    = kIdleYaw;
constexpr uint32_t kLean     = kIdleTilt;
constexpr uint32_t kLatAccel = kIdleA;  // 488 - the lateral response the steering model writes
constexpr uint32_t kSpeed    = 0x1E0; // 480 - 16.16, written by the engine at 0x8007A77C
constexpr uint32_t kFacing   = 0x124; // 292 - the heading angle, 4096 = one turn
constexpr uint32_t kFacingCos = 0x128; // 296 - 16.16
constexpr uint32_t kFacingSin = 0x12C; // 300 - 16.16
constexpr uint32_t kFacingX  = 0x210; // 528 - s16, the vector the heading angle is taken from
constexpr uint32_t kFacingZ  = 0x214; // 532 - s16
constexpr uint32_t kPoseA    = 0x344; // 836 - u16 rider pose accumulators
constexpr uint32_t kPoseB    = 0x346; // 838
constexpr uint32_t kPoseC    = 0x348; // 840
constexpr uint32_t kRider    = 0x358; // 856 - -> the pool-1 rider entity, or 0
constexpr uint32_t kAltOffset = 0x0F4; // 244 - 3 x 16.16 added to the centre by the alternate box
constexpr uint32_t kAltAxes  = 0x100; // 256 - a second 3x3 orientation, three s16[3] rows
constexpr uint32_t kAltKind  = 0x220; // 544 - u16, indexes the 8-byte table at 0x800541D4
constexpr uint32_t kAltState = 0x223; // 547 - s8; 1 selects the alternate box and is then doubled
constexpr uint32_t kSteerScratch = 0x0C4; // 196 - the steering target; ALSO OBB corner 0, which
                                          // 0x8008BA18 overwrites later in the same frame
constexpr uint32_t kSteerRate = 0x248; // 584 - the rate the steering servo is currently allowed
constexpr uint32_t kSteerPhase = 0x2C8; // 712 - the servo's phase accumulator, +-
constexpr uint32_t kSteerSpan = 0x2D8; // 728 - the phase span of the current steering move
constexpr uint32_t kSteerFrom = 0x284; // 644 - the steering angle the current move started from
constexpr uint32_t kDriftLimit = 0x39C; // 924 - a per-frame limit the AI writes
constexpr uint32_t kRiderDef = 0x43C; // 1084 - -> the 0x800D5758 runtime rider record
constexpr uint32_t kAimFrom  = 0x370; // 880 - 3 x 16.16, the point the AI aims FROM
constexpr uint32_t kAimTo    = 0x1F8; // 504 - 3 x 16.16, the point it aims AT (the second position)
constexpr uint32_t kAimAxis  = 0x32E; // 814 - s16[3], the axis the aim error is measured along

// ---- the engine's own fields (RASHCDG 0x80079B20).
// Three of them are words the idle model also walks, named twice on purpose exactly as kSteer is.
constexpr uint32_t kRevs     = kIdleRev; // 604 - the rev counter; the torque curve's index
constexpr uint32_t kDrive    = 0x250; // 592 - gear ratio x torque, the engine's output  [proven]
constexpr uint32_t kGear     = 0x351; // 849 - s8 gear index; stats[20 + 4*gear] is its ratio
constexpr uint32_t kThrottle = kIdleLean; // 616 - the level the engine integrates    [probable]
constexpr uint32_t kThrottleVel   = 0x26C; // 620 - its rate                          [probable]
constexpr uint32_t kThrottleAccel = 0x270; // 624 - its acceleration                  [probable]
constexpr uint32_t kThrottleFrom  = 0x274; // 628 - the level the current move started from
constexpr uint32_t kThrottleTime  = 0x278; // 632 - the time the current move has run
constexpr uint32_t kEngBrake = 0x24C; // 588 - gates the 0x8007A858 arm               [probable]
constexpr uint32_t kEngHold  = 0x254; // 596 - > 0 cancels a throttle move            [probable]
constexpr uint32_t kEngWish  = 0x2FC; // 764 - negative selects the rev-target search at 0x80079E30
constexpr uint32_t kWheelieRate  = 0x2AC; // 684
constexpr uint32_t kWheelieVel   = 0x2A8; // 680
constexpr uint32_t kWheelieAngle = 0x2A4; // 676  (= kIdleB, the idle model's fourth accumulator)
constexpr uint32_t kWheelieTo    = 0x2B0; // 688
constexpr uint32_t kWheelieTime  = 0x2D4; // 724
constexpr uint32_t kWheelieBias  = 0x28C; // 652
constexpr uint32_t kLeanSrc  = 0x212; // 530 - s16, the value the engine's final arm arctangents
constexpr uint32_t kLeanOut  = 0x34A; // 842 - s16, the angle it ramps toward that arctangent
constexpr uint32_t kCrashTimer = 0x2C0; // 704 - advanced by dt while flagsB bit 9 is set
constexpr uint32_t kCrashCount = 0x350; // 848 - s8, decremented on each wipeout
// ---- the race spine's own fields
constexpr uint32_t kLiveState = 0x140; // 320 - s16; the shared entity header's "this slot is live"
constexpr uint32_t kRaceFlags = 0x3A0; // 928 - u8; bit 0x10 keeps a police bike in the progress
                                       //       pass, bit 0x40 is cleared by it every frame
} // namespace ent

// ---------------------------------------------------------------------------- RASHCDG 0x80075B08
// s32 SetImpactDirection(EntityView bike, const s16 hit[3], const u16 *rsqrtTable)
//
// The first half of contact resolution: decide whether a contact whose direction is `hit` counts as
// an impact on this bike and, if it does, latch the reversed direction into the bike so the rest of
// the crash code can push it that way.
//
//   if (flagsC & 0x60F)    return 0;       // already crashing / arrested / finished
//   if (flagsB & 0x400)    return 0;       // an impact is already latched
//   if (hit[1] >= 2633)    return 0;       // 2633/4096 = 0.643: a normal pointing this far up is
//                                          // the ground under the wheels, not something to hit
//   d = ((heading[0]*hit[0] << 4) >> 16) + ((heading[2]*hit[2] << 4) >> 16);
//   if (d <= 0)            return 0;       // the bike is moving away from the contact
//   impact = normalise(-hit[0], 0, -hit[2]);
//   flagsB |= 0x400000;
//   return 1;
//
// The `<< 4` before the `>> 16` is a 32-bit shift of the low half of the product, so a large
// heading times a large hit vector wraps - reproduced, not smoothed over.
int32_t SetImpactDirection(EntityView bike, const int16_t hit[3], const uint16_t* rsqrtTable);

// ---------------------------------------------------------------------------- RASHCDG 0x80075BE4
// void BikeIdleStep(EntityView bike, s32 dt, const u8 *stats, u32 &randSeed)
//
// The standing-still behaviour. `RASHCDG 0x80079B20` - the engine - calls this instead of the
// drivetrain when the bike is effectively stopped: speed < 13107 (0.2), `flagsA & 0x42 == 0x42`,
// `flagsC & 0x7FF == 0`, `*(stats2 + 604) < 2`, `flagsB & 0x40000 == 0`, neither `+0x26C` nor
// `+0x270` set, and `flagsA & 0x08000000` clear (`0x80079B98..0x80079C18`).
//
// What it does, in order:
//
//   1. find the largest of the 29 words at `stats + 60` and its index `k` (a strict `<`, so ties
//      keep the first, and a stat block that is all <= 0 yields k = 0);
//   2. ceiling = stats[176] + k * stats[184];
//   3. bike.kIdleRev = min(ceiling, bike.kIdleRev + FixMul(10000.0, dt))  - the rev ramp;
//   4. if `flagsA & 0x300`, run the wobble: three clamped random walks whose step direction is the
//      sign bit 8 of flagsA (`kIdleYaw` +-17157, `kIdleA` +-1.0, `kIdleB` +-13107), and then
//      `kIdleTilt = 341 * kIdleYaw / 17159` computed with the compiler's magic-number division;
//   5. if `|kIdleYaw| < 1143`, kick it with `Rand() % 2286 - 1143`;
//   6. kick `kIdleLean` the same way, clamp it to +-5719, set `kIdleTimer = 2.0`, clear the speed
//      copy and set `flagsB |= 0x08000000`.
//
// `stats` is the block `bike[+0x22C]` points at - game data, so the caller supplies the pointer.
// `randSeed` is the engine's global LCG seed at `gp+2076`; the two `Rand()` calls advance it and
// the caller must store it back, because the original's seed is a global and the bench's whole-RAM
// diff checks that it moved by exactly the right amount.
void BikeIdleStep(EntityView bike, int32_t dt, const uint8_t* stats, uint32_t& randSeed);

// ---------------------------------------------------------------------------- RASHCDG 0x8008BA18
// void BuildObb(EntityView e, int32_t ownerIdleRev)
//
// Rebuilds the entity's oriented bounding box - the eight corners the collision broad phase reads.
//
// **This corrects `population.md` 1.2.** `+0xB8` is the box CENTRE, not the corners: the eight
// corners live at `+0xC4..+0x123`, twelve bytes further on, and the final store is
// `corner[i] = halfSum[i] + centre[i]` at `0x8008BCD0/E4/F8`.
//
//   A = axis0 * halfX;   B = axis1 * halfZ';   C = axis2 * halfY;
//   corners = { -A-C, A-C, A+C, -A+C, -A-B-C, A-B-C, A-B+C, -A-B+C } + centre
//
// so the box is a "slab": the first four corners are the A/C rectangle at the centre, the last four
// the same rectangle pushed along -B. For a motorcycle (`handle >> 5 == 0`) whose class is under 18
// and whose owner's `+0x25C` is at least 2, `halfZ` is first squashed to `(5 * halfZ) / 8`, rounding
// toward zero - the rider crouching.
//
// Two per-pool adjustments then shift part of the box along B (`s3` / `s4` in the original):
//   * pool 3 (traffic): B is replaced by `axis1 * 8192` and only the FIRST FOUR corners shift;
//   * pool 4 (roadside props) with slot >= 30: B is halved and ALL EIGHT corners shift;
//   * any other pool: nothing shifts.
//
// `ownerIdleRev` is `*(entity[+0x354] + 0x25C)`, read by the caller because it is a pointer chase;
// pass 0 when `+0x354` is null, exactly as the original's guard does.
void BuildObb(EntityView e, int32_t ownerIdleRev);

// ---------------------------------------------------------------------------- RASHCDG 0x80073D74
// void BikeSteerLean(EntityView bike, const u8 *stats, const s16 *sincos)
//
// Steering and lean, once per frame per bike, from `0x80074570` inside the per-bike step.
//
//   if (steer == 0 || speed <= 0.5)            // 0x8000 in 16.16
//       { latAccel = 0; lean = lean / 2 (toward zero); return; }
//
//   a    = clamp(steer + steerBias, +-0x16571);
//   t    = RatTan((652 * a) >> 16);            // 652 = 4 * 163, the same 163 the lean uses
//   base = FixMul(leanGain, t) + leanBias;
//   P    = FixMul(stats[+0x118], speed) + 1.0;
//   Q    = FixMul(stats[+0x11C], speed) + 1.0;
//   g    = signed FixDiv(P, Q);                // the grip/understeer ratio
//   if (g <= 64879) {                          // 0.9900 in 16.16
//       latAccel = signed FixDiv(base, FixMul(1.0 - g, speed));
//       latAccel = clamp(latAccel, +-stats[+0x0E8]);
//       g        = FixMul(g, latAccel);
//   } else {
//       latAccel = g;
//   }
//   if ((s8)bike[+0x08] < 2)
//       lean = (163 * clamp(FixMul(signed FixDiv(g, speed), stats[+0x04]), +-0xC90F)) >>> 14;
//
// Every divide is the engine's own sign-split around the UNSIGNED `FixDiv`, and the final shift is
// LOGICAL, so a negative numerator produces the original's large positive lean rather than a small
// negative one. Both are reproduced.
//
// `stats` is the block `bike[+0x22C]` points at; `sincos` is the table `RatTan` needs. Both are
// game data, so the caller supplies the pointers.
void BikeSteerLean(EntityView bike, const uint8_t* stats, const int16_t* sincos);

// ---------------------------------------------------------------------------- RASHCDG 0x80074170
// void BikeSolveSteer(EntityView bike, const u8 *stats, const s32 *atanTable, const s16 *sincos)
//
// The inverse of `BikeSteerLean`, called from `0x80074570` when the AI (or the road) asks for a
// lateral response instead of for a steering input: it takes the lateral accel already in
// `+0x1E8`, works the steering model backwards to the angle that would produce it, writes that
// angle to `+0x27C`, and - only if the angle had to be clamped to `stats[+0x0E4]` - re-runs
// `BikeSteerLean` so the rest of the state stays consistent.
//
//   g    = signed FixDiv(FixMul(stats[+0x118], speed) + 1.0,
//                        FixMul(stats[+0x11C], speed) + 1.0);
//   g    = FixMul(g, latAccel);
//   if (speed >= 6554)                                  // 0.1
//       lean = (163 * clamp(FixMul(FixMul(g, 1/speed), stats[+0x04]), +-stats[+0x0F0])) >>> 14;
//   t    = signed FixDiv(FixMul(latAccel - g, speed) - leanBias, leanGain);
//   a    = ArcTan(t);                                   // RatAtan2's tail, inlined
//   steer = ((25736 * a) >> 8) - steerBias;             // 25736/256 = 1 / (652/65536)
//   c    = clamp(steer, +-stats[+0x0E4]);
//   if (c != steer) { steer = c; BikeSteerLean(bike); }
//
// Two things here are NOT the shared helpers even though they look like them, and the port keeps
// the difference: the two reciprocals are computed inline with an ARITHMETIC `x >> 1` (the shared
// `FixDiv` uses a logical one) and the product that follows is a SIGNED `mult`, not `multu`.
void BikeSolveSteer(EntityView bike, const uint8_t* stats, const int32_t* atanTable,
                    const int16_t* sincos);

// ---------------------------------------------------------------------------- RASHCDG 0x800807F0
// void BikeRiderPose(EntityView bike, s32 dt, EntityView *rider,
//                    const s32 *atanTable, const s16 *sincos)
//
// Runs twice per bike per frame from the per-bike step (`0x80078C40`): it advances the three rider
// pose accumulators at `+0x344`/`+0x346`/`+0x348`, rebuilds the bike's collision box, pins the
// rider entity to the bike (same orientation, offset along axis 0 by the sum of the two
// half-extents) and rebuilds the rider's box too, and finally turns the heading vector at
// `+0x210`/`+0x214` into an angle plus its sine and cosine at `+0x124`/`+0x128`/`+0x12C`.
//
// `rider` is the entity `bike[+0x358]` points at; pass nullptr when that word is zero, which is
// exactly the `s0` flag the original computes at `0x80080818`. The two `...OwnerIdleRev` values are
// what `BuildObb` needs from each entity's own `+0x354` chain, resolved by the caller for the same
// reason. The pose arithmetic runs only while
// `(s8)bike[+0x08] < 2`; the box rebuild and the heading always run.
//
// The step size is `(2608 * FixMul(speed, dt)) >> 16`. Note that the original reads `dt` out of
// `a1` without ever loading it - it is this function's own second argument, kept in the register
// across the nested call - which is why the signature has to carry it.
void BikeRiderPose(EntityView bike, int32_t dt, int32_t bikeOwnerIdleRev, EntityView* rider,
                   int32_t riderOwnerIdleRev, const int32_t* atanTable, const int16_t* sincos);

// ---------------------------------------------------------------------------- RASHCDG 0x8008BD2C
// void BuildObbAlt(EntityView e, const u8 *kindTable, int32_t ownerIdleRev)
//
// The second bounding-box builder. When `(s8)e[+0x223] == 1` the box is built from the ALTERNATE
// orientation at `+0x100` and a centre offset by `+0xF4`, with the along-B half extent divided by
// three or by two depending on the halfword at `kindTable[8 * e[+0x220] + 2]`; the state byte is
// then doubled, so the alternate box is used for one frame and the ordinary one afterwards.
// For any other state byte it simply delegates to `BuildObb`.
//
//   if (e[+0x223] != 1) { BuildObb(e, ownerIdleRev); return; }
//   centre = e[+0xB8] + e[+0xF4];
//   A = altAxis0 * halfX;   B = altAxis1 * (kind == 5 ? halfZ/3 : halfZ/2);   C = altAxis2 * halfY;
//   corners = { -A+B-C, A+B-C, A+B+C, -A+B+C, -A-B-C, A-B-C, A-B+C, -A-B+C } + centre;
//   e[+0x223] *= 2;
//
// Note that the corner ORDER is not the same as `BuildObb`'s, and that both divisions round toward
// zero (the compiler's magic-number `/3` and a `srl 31 / sra 1` `/2`).
//
// `kindTable` is the game's own table at 0x800541D4 - game data, so the caller passes the pointer.
void BuildObbAlt(EntityView e, const uint8_t* kindTable, int32_t ownerIdleRev);

// ---------------------------------------------------------------------------- RASHCDG 0x80074570
// void BikeApplySteering(EntityView bike, s32 hasRider, EntityView *rider, s32 extraLateral,
//                        s32 rate, const u8 *stats, const s32 *atanTable, const s16 *sincos,
//                        u8 riderDefByte0)
//
// The steering servo, run once per bike per frame from `0x80074C84` inside the per-bike step. It
// has two completely separate halves, selected by `flagsA & 0x108000`:
//
// * `== 0x8000` - **a timed steering move**. `+0x2C8` is a phase accumulator; while it is negative
//   the move is winding down, and while it is zero a new move is started by looking a rate up in
//   the stat block by speed band (`stats[+0x164]`/`[+0x168]` as the band edges, `[+0x16C]`,
//   `[+0x170]`, `[+0x174]` as the rates) and dividing the requested angle by it into `+0x2D8`.
//   The angle then eases from `+0x284` to zero through one of three shapes, chosen by
//   `flagsA & 0x20000` / `& 0x10000`: a linear ramp, `cos(0x1921F * t)` or `cos(0x3243F * t) + 1`
//   at half amplitude. Every one of those cosines is a `lh` out of the game's own sine/cosine
//   table with a `163 * x >>> 13` index, reproduced exactly (note the LOGICAL shift).
// * anything else - **the AI / drift path**: a lateral response ramped at `+0x248` per second and
//   optionally limited so it does not cross the target in `+0xC4`, plus the `+0x39C` drift limit
//   interpolated out of five more stat words.
//
// Both halves end in `BikeSteerLean(bike)`, then optionally copy the lateral into the rider, then
// - if `extraLateral` is non-zero - add it, clamp to `stats[+0xE8]` and run `BikeSolveSteer`.
//
// `hasRider` is the caller's own flag (`a1`), not `bike[+0x358] != 0`: the original is handed it.
// `riderDefByte0` is `*(u8*)bike[+0x43C]`, a pointer chase the caller has to do.
void BikeApplySteering(EntityView bike, int32_t hasRider, EntityView* rider, int32_t extraLateral,
                       int32_t rate, const uint8_t* stats, const int32_t* atanTable,
                       const int16_t* sincos, uint8_t riderDefByte0);

// ---------------------------------------------------------------------------- RASHCDG 0x80072FB4
// void BikeAimTarget(EntityView bike, EntityView *rider, const u8 *stats, const s32 *atanTable,
//                    s32 handleScale)
//
// The AI's aim: it decides the lateral response the bike SHOULD have and leaves it in `+0xC4`, the
// field `BikeApplySteering`'s AI half then chases. Called from `0x80073874`, once per bike.
//
// Two ways in, selected by bit 27 of `flagsA`:
//
// * **bit 27 set - geometric.** `d = bike[+0x370] - bike[+0x1F8]`, the vector from where the bike
//   is to where it is meant to be. Its component along `bike[+0x32E]` (`s4`) divided by half its
//   own squared length gives a curvature, scaled by the speed and clamped to `stats[+0xE8]`. The
//   component along the heading `bike[+0x1C2]` (`s5`) and `s4` are then fed to the inlined
//   `RatAtan2` - byte-for-byte the body of `0x80020018`, same table, same eight octant arms at
//   `0x8005B628` - and the resulting angle decides, with `flagsA & 0x400000` and the
//   `speed < stats[+0xE0]/2` test, whether the bike is considered lined up.
// * **bit 27 clear, bit 20 set - by speed band.** The response is `stats[+0x184]`,
//   `stats[+0x188]` or an interpolation, times the per-handle word at
//   `handleTable[8*handle + 4]` (guest `0x800CE540`).
//
// Either way the result lands in `+0xC4`, a three-way flag dance updates `flagsA` bits 8/9/15/21,
// and `+0x248` is refreshed from three more stat words.
//
// `handleTable` is the runtime table at `0x800CE540` (8-byte records, the word at `+4` of the
// entity's own handle is the one that is read), and `rider` may be null; both are pointer chases
// the caller resolves.
void BikeAimTarget(EntityView bike, EntityView* rider, const uint8_t* stats,
                   const int32_t* atanTable, const uint8_t* handleTable);

// ---------------------------------------------------------------------------- RASHCDG 0x80073874
// One entry of the steering driver's list. The original walks an intrusive circular list whose
// node sits at `entity + 0x440`; a native port cannot follow guest pointers out of a byte view, so
// the CALLER walks the list once, resolves each entity's four pointer chases, and hands the driver
// the nodes in the original's own order. Everything that decides what happens is in the driver.
struct BikeSteerNode {
    EntityView bike;
    EntityView* rider;          // bike[+0x358], or nullptr when that word is zero
    const uint8_t* stats;       // bike[+0x22C]
    const uint8_t* handleTable; // guest 0x800CE540, for BikeAimTarget
    uint8_t ownerFlagByte;      // *(u8*)(bike[+0x354] + 0x23C)
    int32_t ownerIdleRev;       // *(s32*)(bike[+0x354] + 0x25C)
};

// void BikeSteerDriver(BikeSteerNode *nodes, size_t count, s32 dt, s32 numPlayers, s32 gs34,
//                      const u8 *aiTable, const s32 *atanTable, const s16 *sincos)
//
// `RASHCDG 0x80073874(listHead, dt)`, the per-frame steering driver. For every bike on the list, in
// order:
//
//   1. clear `flagsA` bit 20, and - only for a bike whose handle is below `game_state[+0x30]` and
//      whose `flagsA` bit 27 is clear - set it again from the AI table at
//      `0x800D7128 + 192*handle + 16`. The same table, offset by one or two records depending on
//      the player count, then sets or clears bit 20 of the RIDER's flags, but only when the owner
//      byte at `bike[+0x354] + 0x23C` has bit 4 and `game_state[+0x34] >= 2`;
//   2. clear `flagsA` bit 19, and bail out for this bike if it is crashing
//      (`flagsC & 0x1FF` with either `flagsC & 0x600` or `flagsB & 0x40`);
//   3. if `bike[+0x2D0]` is set, just run `BikeSteerLean` and move on;
//   4. otherwise pick one of three behaviours:
//      * **crash recovery** (bit 27 and bit 13 set): the impact direction at `+0x334` is dotted
//        against two rows of the `+0x204` matrix, `RatAtan2` turns that into an angle, and
//        `25736 * angle >> 8` is offset from `+-0x1921F` and divided by `stats[+0xE8]` into a
//        countdown at `+0x2D4` which then drives `BikeSolveSteer` for as many frames as it lasts;
//      * **the AI** (`blend > 0`, or the bike is stopped and revving): `BikeAimTarget`, then
//        `flagsA |= 0x80000`;
//      * **fading out** (`flagsB & 0x18000000`): every steering accumulator - `+0x1E8`, `+0x27C`,
//        `+0x2A0`, `+0x33A`, `+0x2A4`, `+0x268` and the rider's `+0x1E8` - is halved toward zero
//        and `+0x28C` is refreshed from `+0x29C`.
//
// `aiTable` is guest `0x800D7128` (192-byte records); `numPlayers` and `gs34` are
// `game_state[+0x30]` and `[+0x34]`.
void BikeSteerDriver(BikeSteerNode* nodes, size_t count, int32_t dt, int32_t numPlayers,
                     int32_t gs34, const uint8_t* aiTable, const int32_t* atanTable,
                     const int16_t* sincos);

// ---------------------------------------------------------------------------- RASHCDG 0x80079B20
// The engine and drivetrain.
//
// THE ONE CALLEE THIS PORT DOES NOT CONTAIN. `0x80079B20` calls `SLUS_010.53 0x80017BA0`
// (`PlaySound3D(x, z, id, flags)`) at `0x8007A6C0`, on the arm where the throttle integrator
// crosses zero. That emitter is the front of the SPU path and is not ported; worse, it is NOT
// invisible - it pulls on the game's shared LCG, so leaving it out would change state the bench
// compares (measured: 339 of 388 calls move 17-19 bytes of guest RAM, the first next to the seed at
// 0x8005B4A8).
//
// So the port declares it as an interface instead of faking it. In `rrverify phys` the
// implementation is the INTERPRETER executing `0x80017BA0` on the candidate's own machine, with the
// arguments this port computed; in the shipped game it will be the native audio front. Nothing here
// pretends the call did not happen.
struct EngineSound {
    virtual ~EngineSound() = default;
    // SLUS_010.53 0x80017BA0. `x` and `z` are the bike's box centre at +0xB8 / +0xC0; the engine
    // always passes id 54 and flags 0.
    virtual void PlaySound3D(int32_t x, int32_t z, int32_t id, int32_t flags) = 0;
};

// One entry of the engine's list. `0x80079B20(head, dt)` walks an intrusive list whose node is at
// `entity + 0x440` (`entity = node - 1088`), so - exactly as for `BikeSteerDriver` - the CALLER
// walks it once and hands the port the entities in the original's own order with their two pointer
// chases resolved.
struct BikeEngineNode {
    EntityView bike;
    // `bike[+0x22C]`, the per-rider stat block - which for the player is the resident `.PH`.
    // The engine indexes it well past its nominal end (the torque curve lookup's
    // index is `(revs - stats[+0xB0]) / stats[+0xB8]`, which nothing bounds), so this must point
    // into a window that covers whatever the original would have read.
    const uint8_t* stats;
    int32_t ownerIdleRev; // *(s32*)(bike[+0x354] + 0x25C), read by the standing-still guard
};

// void BikeEngineStep(BikeEngineNode *nodes, size_t count, s32 dt, u32 &randSeed,
//                     const u16 *atanTable, EngineSound &sound)
//
// For every bike on the list, in order:
//
//   1. **the standing-still arm** - under 0.2 of speed with `flagsA & 0x42 == 0x42` and five more
//      gates clear, the whole drivetrain is replaced by `BikeIdleStep` (0x80075BE4, ported);
//   2. **the rev counter.** `flagsA` bit 23 latches "over the rev limit"; while it is set the revs
//      ramp by `-10000/s`, or by `-100/s` / `-10000/s` (+ `10100` / `20000` when `flagsA & 2`)
//      depending on `flagsA & 0x40`, and are then clamped into `stats[+0xB0]..stats[+0xB4]` - and
//      the speed copy at `+0x240` becomes `revs / gearRatio`. Otherwise, with `flagsA & 2` and the
//      bike nearly stopped and `+0x2FC` negative, the revs are solved BACKWARDS out of the torque
//      curve for the demand `1.1 * -(+0x2FC) / gearRatio`; with `flagsB & 1` the revs drive the
//      speed copy, without it the speed copy drives the revs (`revs = speed * gearRatio`);
//   3. **the torque curve.** `x = revs - stats[+0xB0]`, `i = x / stats[+0xB8]`, linear
//      interpolation between `stats[+0x3C + 4i]` and the next entry, with three early-outs
//      (`revs >= stats[+0xB4]` gives 0, `x < 0.1` gives entry 0, a step under 6 units gives the
//      next entry). `drive = FixMul(gearRatio, torque)`;
//   4. **gear selection.** The gear below and the gear above are evaluated with the same curve at
//      the revs they would produce, and whichever yields more drive wins - shifting up also needs
//      `flagsA & 2`. This is the gear model;
//   5. **the throttle integrator** at `+0x268`/`+0x26C`/`+0x270`/`+0x274`/`+0x278` - a second-order
//      move whose zero crossing is what emits the sound and what `s1` above reports;
//   6. `+0x1E0`, **the bike's speed**, at 0x8007A77C;
//   7. the wheelie-style accumulator at `+0x2A4` and, while `(s8)bike[+0x08] < 2`, the lean angle
//      at `+0x34A`, ramped at 7.6/s toward the arctangent of `bike[+0x212] << 4`.
//
// `atanTable` is the game's 61-entry u16 arctangent table at guest `0x800527E0` (4096 = one turn,
// so the saturated value is 1024); `randSeed` is the global LCG seed at `gp+2076`, which
// `BikeIdleStep` advances.
void BikeEngineStep(BikeEngineNode* nodes, size_t count, int32_t dt, uint32_t& randSeed,
                    const uint16_t* atanTable, EngineSound& sound);

// ------------------------------------------------------------------------ SLUS_010.53 0x80027028
// s32 CrashTableHit(EntityView bike, s32 which, const u8 *table)
//
// Twelve instructions, and the first half of what the crash timer does: a per-class lookup that
// says whether this entity has a crash effect of kind `which` at all.
//
//   i = 2 * (3 * bike[+0xB4] + which);
//   return (s8)table[i + 2] != -1;
//
// `table` is the game's own 6-byte-per-class table at guest 0x800537D8. Nothing bounds the class,
// so the caller must hand over a window that covers whatever the original would have read.
int32_t CrashTableHit(EntityView bike, int32_t which, const uint8_t* table);

// ---------------------------------------------------------------------------- RASHCDG 0x80074E6C
// The second user of the oracle-supplied-callee seam, and a MIXED one: of the crash timer's two
// callees, `SLUS 0x80027028` is ported above, and only `SLUS 0x80027540` - 560 bytes that reach the
// SPU through `0x8002705C` -> `0x80043F00` - is left to the oracle.
struct CrashEmitter {
    virtual ~CrashEmitter() = default;
    // SLUS_010.53 0x80027540(bike, which, kind). Three arguments; the caller leaves a3 alone.
    virtual void Emit(int32_t which, int32_t kind) = 0;
};

// void BikeCrashTimer(EntityView bike, s32 dt, s32 window, const u8 *crashTable, CrashEmitter&)
//
// `RASHCDG 0x80074E6C(bike, dt)`, called once per bike from the per-bike step. It does three
// separate things:
//
//   1. while `flagsB` bit 9 is set it advances `+0x2C0` by `dt` and clears the bit once the timer
//      passes `window` - the word at guest `0x800D3974`, which is game data the caller supplies;
//   2. on the frame `flagsA` has bits 1, 2 and 3 set and `+0x350` is still positive, it WIPES OUT:
//      `+0x350` is decremented, the timer restarts, `flagsB` bit 9 is set, and the two crash
//      effects (`which` 0 and 1) are emitted for every kind the class table admits. With bit 3
//      clear instead, `flagsA` gets bits 11 and 12 unless bit 11 was already set;
//   3. `flagsA` bit 2 is consumed, and `flagsB` bit 9 is cleared whenever `flagsA` bit 1 is clear
//      or bit 6 is set.
void BikeCrashTimer(EntityView bike, int32_t dt, int32_t window, const uint8_t* crashTable,
                    CrashEmitter& emit);

// ---------------------------------------------------------------------------- RASHCDG 0x8007504C
// The GROUND FRAME of the per-bike step: 375 instructions, one `jr ra`, called twice per frame -
// from `0x80078BA8` inside the per-bike step `0x80075EE0` for every entity of the active list, and
// from `0x8007BEA4` inside the rider pass `0x8007B840`. It is what puts the bike ON the road: it
// takes a reference point, asks the world what surface is under it, and leaves behind
//
//   +0x1F8..+0x200  the CONTACT POINT: `refPoint + n * dot(contact - refPoint, n)`, 3 x 16.16,
//   +0x104          the signed distance along `n` that went into it,
//   +0x112..+0x116  `n` itself, the surface normal it used, s16[3] with 4096 = 1.0,
//   +0x10C..+0x110  a second normal and +0x118..+0x120 a second contact point - the pair the
//                   collision side reads,
//   +0x216/+0x217   the two surface bytes, +0x218/+0x23C the two surface ids,
//   `flagsA` bit 6 and `flagsB` bit 25, each set when its surface id changed this frame,
//
// and, on a bike that is stopped and not yet placed 255th, a creep speed of 0x0001C9C4 at +0x1E0
// with `Scale` (`SLUS 0x8002EE50`, ported) writing the matching velocity at +0x1C8.
//
// Two facts about the entity that this function PROVES and that a RAM capture does not show:
//
//   * `+0xF4..+0x123` is a UNION with three users inside one frame. The road-slice search
//     `SLUS 0x80036B14`, called at `SLUS 0x80037274` as `0x80036B14(e+0xAC, sp+16, e+0xF4, ...)`,
//     writes **`+0xF4` = the road-projected point** and, at `SLUS 0x80037288`, **`+0x100` = the
//     road slice it found**; this function reads exactly those two. Later in the same frame
//     `0x8008BA18` overwrites `+0xC4..+0x123` with the eight box corners, and `0x8008BD2C` reads
//     `+0xF4` as an offset and `+0x100` as a 3x3 matrix. All three readings are real; a captured
//     RAM image only ever shows the last one, which is why `+0x100` looks like a coordinate there.
//   * on the query path the two surface bytes are SWAPPED into place after the second query:
//     `sb s5,534(s1)` in the branch delay slot at 0x80075414 (word 0xA2350216) restores +0x216 to
//     the byte the FIRST query left there, and `sb v0,535(s1)` at 0x800754B4 stores the byte the
//     second query left at +0x216 into +0x217. (The first store is not a dead store to +0x217;
//     the whole-step bench row proves it.)
enum class BikeGroundSite {
    kMain,  // 0x8007509C: a1 = the caller's reference point, a2 = sp+24, a3 = e+0x10C
    kAlt,   // 0x80075348: a1 = e+0xF4,                       a2 = sp+40, a3 = e+0x10C
    kRider, // 0x80075510: a1 = 0 (the callee then uses rider+0xB8), a2 = sp+24, a3 = sp+72
};

// `RASHCDG 0x800A7BF8(entity, refPoint, outPoint, outNormal, prevId)` - the ground query, 2208
// bytes, which reaches `0x800A8498`, `0x800B6E08` and `0x800B6844` and the whole collision-world
// lookup under them. Not ported: supplied by the oracle in the bench row, absent
// in the product, where the flag that reaches it is never set.
//
// FIVE o32 arguments, the fifth at `sp+16`. `outPoint` is 3 x 16.16 and `outNormal` s16[3]; the
// callee may leave EITHER untouched (its early-out at `0x800A8460` returns -1 and writes nothing),
// so both are passed in with the caller's own current contents, exactly as the original does.
struct BikeGroundQuery {
    virtual ~BikeGroundQuery() = default;
    virtual int32_t Query(BikeGroundSite site, const int32_t* refPoint, int32_t outPoint[3],
                          int16_t outNormal[3], int32_t prevId) = 0;
};

// The three slots of the ORIGINAL's own 120-byte frame that it hands the query as output buffers,
// and which it then reads back. They are locals of `0x8007504C`, so a native port has no place to
// put them: the caller owns them and they live across the whole call, because `sp+24` is handed to
// both the first and the third query exactly as the original reuses that slot.
struct BikeGroundFrameSlots {
    int32_t pointA[3]{}; // sp+24, the first and third query's point buffer
    int32_t pointB[3]{}; // sp+40, the second query's point buffer
    int16_t normalC[3]{};// sp+72, the third query's normal buffer
};

// A road slice as this function reads it: the three s16 of the matrix row at +8 and the three
// 16.16 origin words at +20 (`docs\formats\road_chunk.md` for the layout).
// `address` is the raw guest pointer, because 0x8007546C compares it against `entity[+0x154]`.
struct BikeGroundSlice {
    uint32_t address = 0;
    int16_t row[3]{};    // slice + 8, +10, +12
    int32_t origin[3]{}; // slice + 20, +24, +28
};

// Everything the caller has to resolve for it (a native port does not chase guest pointers).
// `rider` is null when `entity[+0x358]` is 0.
struct BikeGroundEnv {
    BikeGroundQuery* query = nullptr;
    BikeGroundFrameSlots* frame = nullptr;
    BikeGroundSlice tracked;    // entity[+0x154]
    BikeGroundSlice ground;     // entity[+0x100], the slice the road search left there
    uint32_t listNode = 0;      // entity[+0x440], tested for zero at 0x800754C8
    uint8_t riderDefPlace = 0;  // (entity[+0x43C])[+0x27], the place byte read at 0x80075188
    uint8_t* rider = nullptr;   // entity[+0x358], the pool-1 rider entity
    BikeGroundSlice riderTracked; // rider[+0x154]
};

// void BikeGroundFrame(EntityView bike, const s32 refPoint[3], BikeGroundEnv&)
//
// `refPoint` is the original's `a1`: `e+0x1F8` when `flagsC & 0x600` is clear and `e+0xB8`
// otherwise at the per-bike-step call site, and always `e+0xB8` at the rider-pass one. The three
// words are copied on entry, which is what the original does too - it reads all three before its
// first store, so the alias `refPoint == e+0x1F8` is safe on both sides.
void BikeGroundFrame(EntityView bike, const int32_t refPoint[3], BikeGroundEnv& env);

} // namespace rr::sim
