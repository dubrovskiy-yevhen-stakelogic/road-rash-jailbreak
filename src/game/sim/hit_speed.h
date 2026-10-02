#pragma once
// The speed hand-over of a hit. Transcribed from our own disassembly of the
// player's own images:
//
//   RASHCDG.BIN  SHA-1 cfe43a7786759f2cb9c57751cf99e84d1074782c, loaded at 0x8005B5E8
//   SLUS_010.53  SHA-1 67ed165a2c517d4e6106fb0dfa324d66dd9a76f1, text at 0x80010000
//
// and accepted only by `rrverify phys` rows (tools\rrverify\rows_coll_speed.inc). Memory model and stack rule:
// collision.h (`sp` is the stack pointer AT ENTRY; each port builds its frame below it as the prologue does and
// writes the frame words the original writes - the outgoing stack arguments, the vectors a callee is handed by
// pointer, the home-slot spills - at the original's addresses).
//
// Every callee of the four is PORTED and runs inline: FixMul, FixDiv, DotLcm, RatAtan2, Scale, Normalize,
// Blend16, the GTE OP (`OuterProduct`, integrator.h), MulAdd16 / Bounce / Spin / MassExchange (crash.h), and
// the four call each other natively (HitOutcome and ImpactTurn -> HitSpeed / TakePartnerHeading). None
// reaches CollisionCallees, so none takes it. Each returns false - the caller must not trust anything it
// wrote - on a GuestRam fault or where the console raises Normalize's overflow exception.
#include <cstdint>

#include "game/sim/coll_serve.h"

namespace rr::sim {

// ------------------------------------------------------------------------ RASHCDG 0x80080D1C
// s32 HitSpeed(Bike *e, s16 dir[3], s32 *pSpeed, Shape *shape, [sp+16] const s16 nrm[3], [+20] a5 closing
//              speed x 0.0559, [+24] a6 signed hit angle, [+28] a7 angle limit, [+32] a8 candidate speed,
//              [+36] a9 one degree, [+40] a10 no-crash-test) - 4192 bytes, frame 80, returns 0 or 1.
// The new speed of a hit: AIR (reflect, halve), SLOW (stop), EXCH (the exchange speed
// and a knock on the +0x26C integrator), MAIN (tell the rider; CRASH on class 4 with a bad lean, or slide
// along the obstacle; then BUMP or GLANCE), and the tail: the new speed into +0x240 (or *pSpeed and the
// velocity for a crashed / sliding bike), the latched normal into +0x334. Four GTE OP (two pairs
// re-projecting the heading into the ground plane). Guest quirks reproduced: `lw 480(shape)` at 0x80081C80
// for a car partner (pool 3) reads car + 0x28C - the NEXT traffic slot's +0x8C; `(e->f358)->f354` at
// 0x80081264 without a null test; the sign-armed FixDiv idioms and the `divu 0x80000000` reciprocal.
bool HitSpeed(GuestRam& g, uint32_t e, uint32_t dir, uint32_t pSpeed, uint32_t shape, uint32_t nrm, int32_t a5,
              int32_t a6, int32_t a7, int32_t a8, int32_t a9, int32_t a10, uint32_t sp, const BikeTables& t,
              uint32_t& v0);

// ------------------------------------------------------------------------ RASHCDG 0x80080B10
// s32 TakePartnerHeading(Bike *e, s16 dir[3], s32 *pSpeed, const s16 src[3], [sp+16] s32 speed) - 524
// bytes, frame 32, one OP. Below 2.0: flagsC bits 4/5 cleared, bit 27 set, 0. Otherwise dir := src; a
// crashed / sliding bike takes *pSpeed and the velocity, anything else +0x240 = speed, the yaw rates zeroed
// (the passenger's with no null test beyond +0x358 itself) and +0x2A4 from the re-projected side row.
bool TakePartnerHeading(GuestRam& g, uint32_t e, uint32_t dir, uint32_t pSpeed, uint32_t src, int32_t speed,
                        uint32_t sp, const BikeTables& t, uint32_t& v0);

// ------------------------------------------------------------------------ RASHCDG 0x80083928
// s32 HitOutcome(Bike *e, Entity *o, const s16 n[3]) - 1544 bytes, frame 152: the class
// of a hit by a full entity `o` (the solver's car): latch, Bounce + Spin for a crashed or floor-landing
// airborne bike, else the closing speed and angle, the class (flagsC bits 0..5, or -1 = +0x234 |= 0x4000),
// the momentum exchange (MassExchange) and the hand-over to HitSpeed / TakePartnerHeading, then flagsC
// 0x1000 / 0x800 and the partner's byte +0x1FD bit 4.
bool HitOutcome(GuestRam& g, uint32_t e, uint32_t o, uint32_t n, uint32_t sp, const BikeTables& t, uint32_t& v0);

// ------------------------------------------------------------------------ RASHCDG 0x80083F30
// s32 ImpactTurn(Bike *e, Shape *shape, s16 n[3], s32 mode) - 1588 bytes, frame 120:
// the same for a static or small partner by `mode` (2 rider / prop, 0 / 1 a side face +-10 degrees, which
// REWRITES n through Blend16, 3 tall, 4 a wall, 5 a post - only 5 reaches class 8); `shape` is spilled to the
// caller's home slot entry+4 and re-read there, as the original does.
bool ImpactTurn(GuestRam& g, uint32_t e, uint32_t shape, uint32_t n, int32_t mode, uint32_t sp, const BikeTables& t,
                uint32_t& v0);

} // namespace rr::sim
