#pragma once
// The per-bike step's integrator and its contact response - the two remaining callees of
// `RASHCDG 0x80075EE0` - with every function under them
// that was not already ported. Transcribed from our own disassembly of the player's own images:
//
//   RASHCDG.BIN  SHA-1 cfe43a7786759f2cb9c57751cf99e84d1074782c, loaded at 0x8005B5E8
//   SLUS_010.53  SHA-1 67ed165a2c517d4e6106fb0dfa324d66dd9a76f1, text at 0x80010000
//
// and accepted only by `rrverify phys` rows.
//
// Rule of the whole file, as everywhere in src\game\sim: a port never follows a guest pointer. Every
// record the original reaches through a pointer is resolved by the CALLER and handed over as a byte
// window. Where the original would dereference something the caller cannot
// resolve, the port returns false instead of guessing.
#include <cstddef>
#include <cstdint>

#include "game/sim/bike.h"

namespace rr::sim {

// The game-data tables this file reads. None of them is reproduced in the repository; the caller
// points into the player's own images.
struct BikeTables {
    const int16_t* sincos = nullptr; // SLUS 0x8005624C, 4096 x {s16 sin; s16 cos}
    const uint16_t* asin = nullptr;  // SLUS 0x800527E0, 61 x u16 (see Asin below)
    const int32_t* atan = nullptr;   // SLUS 0x8005285C, RatAtan2's 18-word table
    const uint16_t* rsqrt = nullptr; // *(gp + 2260) = *(0x8005B560), the reciprocal-square-root table
    const int16_t* sqrt = nullptr;   // SLUS 0x800560CC inside a window, as SqrtGte wants it
};

// ------------------------------------------------------------------------ SLUS 0x8001FF3C
// s32 Asin(s32 x) - the ARCSINE of a 16.16 value, in the engine's 4096-per-turn unit.
//
//   |x| > 0xFFFF             -> 1024 (90 degrees), sign of x
//   |x| <  0x8000            -> linear interpolation over entries 0..4, step 2^13
//   0x8000 <= |x| < 0xFFF8   -> a binary refinement that halves the step while the interval
//                               approaches 1.0, four entries per halving (entries 0..52)
//   0xFFF8 <= |x|            -> entries 52..60, step 1
//   r = (((|x| - base) * (t[i+1] - t[i])) >> shift) + 8 + t[i]) >> 4
//
// The table is the one bike.h calls the engine's "61-element arctangent table":
// this function proves it is an ARCSINE table (1.0 maps to 90 degrees, and the entries crowd towards
// 1.0 where arcsin is steep). `x == INT32_MIN` is the one input whose "absolute value" stays negative
// and indexes half a megabyte below the table on the console; the port returns false for it.
bool Asin(int32_t x, const uint16_t* table, int32_t& out);

// ------------------------------------------------------------------------ SLUS 0x8004D2A4
// void RotMatrix(const s16 angles[3], s16 m[9]) - the rotation matrix of three angles (4096 = one
// turn), m = Rz * Ry * Rx in the 4096 = 1.0 fixed point, every product the low word of a `multu`
// shifted right 12 exactly where the original shifts it. `sincos` is the table above.
void RotMatrix(const int16_t angles[3], int16_t m[9], const int16_t* sincos);

// ------------------------------------------------------------------------ SLUS 0x8003FA40
// void MulMatrix0(const s16 a[9], const s16 b[9], s16 out[9]) - out = a * b on the GTE: three MVMVA
// (rt * ir, sf = 1, lm = 0), one per COLUMN of b, each component saturated to s16. Column by column,
// exactly as the original, so an `out` that aliases `b` behaves as it does on the console.
void MulMatrix0(const int16_t a[9], const int16_t b[9], int16_t out[9]);

// GTE OP with sf = 1 and lm = 0, as `0x80071D24` and `0x8007FA4C` issue it inline (`cop2 0x178000C`):
// `d` is the diagonal of the rotation matrix (the three `ctc2` of $0, $2, $4), `ir` IR1..IR3.
//   out = sat16((d x ir) >> 12)
void OuterProduct(const int16_t d[3], const int16_t ir[3], int16_t out[3]);

// ------------------------------------------------------------------------ SLUS 0x8002E14C
// s32 Normalize32(s32 v[3]) - normalise a 16.16 vector in place to 1.0 = 65536.
//
//   m  = max(|x|, |y|, |z|)                  (the `sra/addu/xor` absolute value)
//   s  = m ? 31 - LZCR(m) : 0
//   v  = s > 20 ? v >> (s - 20) : v << (20 - s)          (so the largest component is ~2^20)
//   n  = FixMul(x,x) + FixMul(y,y) + FixMul(z,z)
//   if (n <= 0) return n;                               (v untouched: the scaling was in registers)
//   f  = rsqrt[n >> sh] ... exactly as Normalize        (vec.h), and v[i] = FixMul(v[i], f)
//
// Returns the value the original leaves in v0: `n` on the early-out, and the table word
// `mantissa << exponent` BEFORE its final shift otherwise. `RASHCDG 0x8007FA4C` inlines the same
// body once and calls it once.
int32_t Normalize32(int32_t v[3], const uint16_t* rsqrt);

// ------------------------------------------------------------------------ SLUS 0x8002076C
// void ReleaseContact(Bike *e) - what a bike leaving its current contact does to the OTHER party.
// `e[+0x340]` is the contact record; its first halfword is an entity handle `(pool << 5) | slot`:
//
//   pool 0 (a bike):     bike[+0x230] &= ~0x02000000
//   pool 3 (traffic):    only while `(flagsB & 0x60000) == 0x40000`: the car's speed along the
//                        bike's heading is ADDED to `e[+0x240]`, raised to AT LEAST 0x23C36, copied to
//                        `+0x1E0` and turned into the velocity `+0x1C8` with Scale
//   pool 4 (a prop):     if the prop's kind `((*(u16*)(prop[0] + 14)) >> 7) & 31` is 3, 4 or 5 and
//                        prop[+0x250] has bit 9, prop[+0x22C] = 0x10000
//   anything else:       nothing
//
// The three records are resolved by the caller from the handle; each may be null, and the port
// returns false (touching nothing) when the arm it needs was not resolved.
struct ContactRefs {
    const uint8_t* contact = nullptr; // e[+0x340], or null when that word is 0 (>= 0x118 bytes)
    uint8_t* bike = nullptr;          // pool 0: *(0x8005B3A0) + 1096 * slot
    const uint8_t* traffic = nullptr; // pool 3: 0x800CF660 + 512 * slot (reads +0x1C2, +0x1E0)
    uint8_t* prop = nullptr;          // pool 4: *(0x800CD6D4) + 596 * slot
    uint16_t propKindWord = 0;        // *(u16*)(*(u32*)prop + 14)
};
bool ReleaseContact(EntityView e, const ContactRefs& refs);

// Everything else a bike's pointers lead to, resolved by the caller. Sizes are the least each window
// must cover; every one of them may be null, and a port that needs a null one returns false.
struct BikeLinks {
    uint8_t* owner = nullptr;          // e[+0x354], >= 0x260 bytes (+0x1C8, +0x228, +0x23C, +0x25C)
    uint8_t* rider = nullptr;          // e[+0x358], the pool-1 rider entity (1096), or null
    uint8_t* riderOwner = nullptr;     // rider[+0x354], >= 0x260 bytes
    const uint8_t* slice = nullptr;    // e[+0x154], the road slice (>= 32 bytes)
    const uint8_t* obstacle = nullptr; // e[+0x33C], >= 52 bytes (+0x32 is read)
    const uint8_t* stats = nullptr;    // e[+0x22C], >= 0x1BC bytes
    ContactRefs contact;               // e[+0x340] and what its handle names
    uint8_t wipeoutRateByte = 0;       // *(u8*)0x800531F1, read by 0x800723FC
};

// ------------------------------------------------------------------------ RASHCDG 0x80071D24
// void BikeCrashLaunch(Bike *e) - 436 instructions. The moment a bike leaves the road in a crash:
// it zeroes the eleven drive and steering accumulators, turns the speed into a velocity along the
// heading, and then EITHER (flagsC bit 10, "thrown") reads three Euler angles back out of the
// orientation matrix with `Asin` and `RatAtan2` into the tumble rates +0x268 / +0x2B4 / +0x27C and
// sets `+0x1E4` (the net acceleration the per-bike step integrates `+0x240` by) to
// `max(vy^2 / k, 0x9D087)` - and 0x9D087 / 65536 = 9.8135, g to four digits - OR rotates the orientation by the
// current lean and steer (`RotMatrix`, `MulMatrix0`), builds the tumble axis from two GTE outer
// products into +0x322 / +0x328 and its two angles into +0x2B4 / +0x2C4. Either way it places the
// tumble centre +0x310 below the box centre, clears the drive flags, releases the contact
// (`ReleaseContact`) and the owner's bit 13.
bool BikeCrashLaunch(EntityView e, const BikeLinks& links, const BikeTables& t);

// ------------------------------------------------------------------------ RASHCDG 0x800723FC
// void BikeWipeoutStart(Bike *e) - 358 instructions. The first frame of one of four wipeout kinds,
// chosen by flagsC bits 6, 5, 7 and 8 in that order: a spin (+0x290 / +0x294 / +0x2B0, rate from the
// global byte at 0x800531F1), a high-side (+0x2A4 clamped to +-0x1921F, +0x27C forced to +-65), a
// slide (+0x2D4 and the owner's velocity copy) and a stop (+0x28C to +-0x1921F, the velocity
// re-derived from the speed). Always clears flagsC bit 11 on the way out.
bool BikeWipeoutStart(EntityView e, const BikeLinks& links);

// The active-entity list the step appends to (`*(u32*)0x1F800000`, a cursor into scratchpad): the
// caller reads the cursor and resolves the four bytes it points at.
struct BikeActiveList {
    uint32_t cursor = 0;      // the word at 0x1F800000
    uint8_t* slot = nullptr;  // 4 bytes AT `cursor`
    bool pushed = false;      // set by the port when it appended
};

// ------------------------------------------------------------------------ RASHCDG 0x8007F0BC
// void BikeIntegrate(Bike *e, s32 dt) - 612 instructions, called twice per bike per frame by the
// per-bike step. The velocity integrator:
//
//   * a bike that is stopped, upright and unflagged is skipped outright (not even listed);
//   * crashing (flagsC & 0x600): on the first frame `BikeCrashLaunch`, then the tumble centre +0x310
//     moves by the velocity and the box centre +0xB8 hangs off it;
//   * otherwise the velocity is re-derived from the road-speed copy +0x240 along the heading, the
//     wipeout kinds are started (`BikeWipeoutStart`), and the contact point +0x1F8 is moved - along
//     the velocity, along the contact's two axes (flagsB bit 18), or through the tumble centre while
//     a wipeout runs - and the heading at +0x210 is turned: blended toward +0x204 while leaning, or
//     rotated by the yaw rate `+0x1E8 + +0x2E8` plus the steer `+0x2A4` about the axis at +0x32E;
//   * the bike is then appended to the active list.
//
// `self` is the entity's own guest address, which is what the original appends.
bool BikeIntegrate(EntityView e, int32_t dt, uint32_t self, const BikeLinks& links,
                   const BikeTables& t, BikeActiveList& list);

// ------------------------------------------------------------------------ RASHCDG 0x80075628
// s32 BikeObstacleTest(Bike *e, const s16 n[3], s32 depth) - 312 instructions. Whether a surface
// with normal `n` that the bike is `depth` into is a wall to crash into (returns 1 and sets
// flagsC |= 0xC00) or a slope to ride up, in which case the heading's vertical component is lifted
// and the other two rescaled to keep it unit length (`SqrtGte`).
bool BikeObstacleTest(EntityView e, const int16_t n[3], int32_t depth, const BikeLinks& links,
                      const BikeTables& t, int32_t& result);

// The one callee of 0x8007FA4C that is NOT ported: `SLUS 0x800374D4(e, flag)`, the road re-bind
// after a latched impact. It reaches `0x8003701C` (the road tracking), `0x800396A8`, `0x8003B61C`
// and the chunk layer, which is not ported here. Supplied by the oracle in the
// bench; absent in the product until that layer is read.
struct BikeRoadRebind {
    virtual ~BikeRoadRebind() = default;
    virtual void Rebind(int32_t flag) = 0;
};

// ------------------------------------------------------------------------ RASHCDG 0x8007FA4C
// void BikeContactFrame(Bike *e) - 873 instructions, called after the ground frame. The contact
// response: impact tests against the two ground normals (`SetImpactDirection` / `BikeObstacleTest`),
// the contact point dropped onto the averaged normal (+0x310, +0x1F8), the heading +0x210 rebuilt
// from it, the orientation rows +0x204 / +0x20A rebuilt with two outer products and a Normalize -
// through the rider's own contact when there is one - then the obstacle flag, and finally the box
// centre +0xB8 re-derived from the contact point, or a crash launched, or a road re-bind.
bool BikeContactFrame(EntityView e, const BikeLinks& links, const BikeTables& t,
                      BikeRoadRebind& rebind);

// ============================================================================ two more closed trees
// of the per-bike step.

// ------------------------------------------------------------------------ RASHCDG 0x8007EF60
// s32 BikeGripLimit(const u8 *stats, s32 a, s32 b, s32 c) - 75 instructions, region D of the step
// (called as `0x8007EF60(stats, +0x2E0, +0x28C, ENV[0x40 + 4 * surface])`):
//
//   d = FixMul(c, a)^2 - FixMul(b, stats[+0xC0])^2      (each square a FixMul of itself)
//   return d < 0 ? 0 : SignedDiv(SqrtGte(d) << 2, stats[+0xC8])
//
// with the engine's usual sign-split around the unsigned FixDiv. `sqrt` is SqrtGte's table inside
// a window (ai.h).
int32_t BikeGripLimit(const uint8_t* stats, int32_t a, int32_t b, int32_t c, const int16_t* sqrt);

// ------------------------------------------------------------------------ RASHCDG 0x80072C7C
// s32 BikePassengerSteer(Bike *e, s32 dt) - 206 instructions. The steering contribution of the
// passenger `e[+0x358]` on a two-rider bike (called only while the owner's byte `+0x23C` has bit 4):
// the passenger's `+0x2A0` walks toward a target at `stats[+0x1AC]` per second - the target read
// out of the per-handle table at 0x800CE540 while its `flagsA` bit 20 is set, the passenger's own
// left/right bits (8, 9) otherwise - clamped to +-1.0, and the answer is
// `(FixMul(+0x2A0, stats[+0x1B0]) / e[+0x240]) & (e[+0x240] > 0.5 ? ~0 : 0)`.
//
// `handleTable` is guest 0x800CE540 as a window of `handleTableBytes`; the index `(handle + 1)` or
// `(handle + 2)` (by `game_state[+0x30] < 2`) is bounded by nothing in the original, so a port
// that would read past the window refuses. `passenger` null is refused too: the original would
// read guest 0x2D0.
bool BikePassengerSteer(EntityView e, int32_t dt, uint8_t* passenger, const uint8_t* stats,
                        uint32_t numPlayers, const uint8_t* handleTable, uint32_t handleTableBytes,
                        int32_t& out);

// ------------------------------------------------------------------------ RASHCDG 0x80074C84
// void BikeSteerPass(list, s32 dt) - 53 instructions, region B of the step, run twice (once per
// list, 0x8005B298 and 0x8005B2D8). For every bike on the intrusive list, in order:
//   extra = (owner[+0x23C] & 0x10) ? BikePassengerSteer(e, dt) : 0;
//   if (flagsA & 0x80000)
//       BikeApplySteering(e, e[+0x358] != 0 && e[+0x440] != 0, extra, dt);   (0x80074570, ported)
// As for BikeSteerDriver, the CALLER walks the list and resolves each node's pointer chases; the
// original re-reads `next` after each bike, which a pre-walked list reproduces as long as nothing
// in the two callees rewrites the node (neither touches +0x440..+0x447).
struct BikeSteerPassNode {
    EntityView bike;
    uint8_t ownerFlagByte = 0;       // *(u8*)(bike[+0x354] + 0x23C)
    uint8_t* passenger = nullptr;    // bike[+0x358], or null
    const uint8_t* stats = nullptr;  // bike[+0x22C]
    uint8_t riderDefByte0 = 0;       // *(u8*)bike[+0x43C], for BikeApplySteering
};
struct BikeSteerPassEnv {
    uint32_t numPlayers = 0;              // game_state[+0x30]
    const uint8_t* handleTable = nullptr; // guest 0x800CE540, a window
    uint32_t handleTableBytes = 0;
    const int32_t* atan = nullptr;
    const int16_t* sincos = nullptr;
};
bool BikeSteerPass(BikeSteerPassNode* nodes, size_t count, int32_t dt, const BikeSteerPassEnv& env);

} // namespace rr::sim
