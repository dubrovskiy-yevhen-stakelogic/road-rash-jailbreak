#pragma once
// Solid roadside objects: the leaning pole test that lets PoleResolve
// 0x800AE794 run natively, the car against a prop, and the prop reactions (kick, topple, knock) that
// make a knocked sign fall over. Transcribed from our own disassembly of the player's own images:
//
//   RASHCDG.BIN  SHA-1 cfe43a7786759f2cb9c57751cf99e84d1074782c, loaded at 0x8005B5E8
//   SLUS_010.53  SHA-1 67ed165a2c517d4e6106fb0dfa324d66dd9a76f1, text at 0x80010000
//
// and accepted only by `rrverify phys` rows (tools\rrverify\rows_solid.inc). Memory model and stack
// rule: collision.h / resolvers.h - guest addresses through GuestRam, `sp` the stack pointer AT ENTRY,
// every local a callee reads or writes through a pointer kept at the original's frame address. Ported
// leaves are called natively; nothing here calls through CollisionCallees::Unported but PlaySound3D.
// Every function returns false when the view faulted or a ported leaf refused (Asin's INT32_MIN,
// Normalize's overflow exception, Bounce).
#include <cstdint>

#include "game/sim/coll_serve.h"
#include "game/sim/recover.h"

namespace rr::sim {

namespace solid {
constexpr uint32_t kLeanPoleTest = 0x800ADC74; // (e, shape, radius, s32 push[3], [s32 *len])
constexpr uint32_t kTrafficVsProp = 0x800B2D88; // (car, prop)
constexpr uint32_t kPropKick = 0x800B2F94;      // (prop, bike, s16 dir[3])
constexpr uint32_t kPropTopple = 0x800B3344;    // (prop, point, s16 n[3], mode)
constexpr uint32_t kPropKnock = 0x800B3838;     // (prop, dirF, dirR, dirU, [speed])
constexpr uint32_t kPropUpRows = 0x800A3CBC;    // (prop)
constexpr uint32_t kSurfaceSound = 0x80017B30;  // SLUS (k)
constexpr uint32_t kLen2 = 0x8002E010;          // SLUS (s32 v[2])
constexpr uint32_t kNormalize2 = 0x8002DF14;    // SLUS (s32 v[2])
constexpr uint32_t kDot2 = 0x8002DFC0;          // SLUS (s32 a[2], s32 b[2])
constexpr uint32_t kQuatNormalize = 0x80071A28; // (s16 q[4])
constexpr uint32_t kQuatMatrixT = 0x8007198C;   // (s16 q[4], s16 out[9])
constexpr uint32_t kPropFrameRows = 0x800A40D4; // (prop)
constexpr uint32_t kPropAnimPass = 0x800A2A64;  // (dt)
constexpr uint32_t kGravity = 0x800D3964;       // s32, the props' and riders' gravity
} // namespace solid

// SLUS 0x8002E010 Len2(s32 v[2]) - 28 instructions, frame 32: SqrtGte(FixMul(x, x) + FixMul(z, z)) << 2.
int32_t Len2(GuestRam& g, uint32_t v, const BikeTables& t);
// SLUS 0x8002DF14 Normalize2(s32 v[2]) - 43 instructions, frame 32: 0 when Len2 is 0; else both words
// FixMul'd by 0x80000000 /u (|len| / 2, less one for |len| < 2) (negated for a negative length), 1.
// False (a refusal) only where the original's `addiu` would trap (a length of INT32_MIN).
bool Normalize2(GuestRam& g, uint32_t v, const BikeTables& t, int32_t& v0);
// SLUS 0x8002DFC0 Dot2(s32 a[2], s32 b[2]) - 20 instructions, frame 32: FixMul(a0, b0) + FixMul(a1, b1).
int32_t Dot2(GuestRam& g, uint32_t a, uint32_t b);

// RASHCDG 0x800ADC74 LeanPoleTest(e, shape, radius, s32 push[3], [sp+16] s32 *len) - 2848 B, frame 280:
// the leaning bike's footprint as an octagon (the box's four near corners
// and the four far ones swept back by dt x speed along the heading, the side picked by the lean and the
// yaw) against the pole's centre +0x0C/+0x14 widened by `radius`: 6 = no contact; else the push along
// the nearest edge's normal (or along the heading for the end face) into push[0], push[2] (push[1] = 0)
// and its length into *len, and the contact code 0 / 2 / 3 by the edge. Spills a1..a3 into the caller's
// home slots (sp + 4 .. 12).
bool LeanPoleTest(GuestRam& g, uint32_t e, uint32_t s, int32_t radius, uint32_t push, uint32_t outLen, uint32_t sp,
                  const BikeTables& t, uint32_t& v0);

// RASHCDG 0x800A3CBC PropUpRows(prop) - 132 instructions, frame 40: the side row +0x210 = OP(row0 +0x1B0,
// up +0x20A) normalised; when that is degenerate the forward row +0x204 = OP(up, +0x1BC) normalised (and
// when THAT is degenerate both frames reset to the identity) and +0x210 = OP(+0x204, up); else +0x204 =
// OP(up, +0x210). The GTE OP with sf = 1, lm = 0.
bool PropUpRows(GuestRam& g, uint32_t p, const BikeTables& t);

// RASHCDG 0x800B3344 PropTopple(prop, point, s16 n[3], mode) - 1268 B, frame 80: `point` given: the
// prop's corner deepest behind the plane (n, point) (CornerMin; none -> v0 0); `mode` != 0 pushes the
// prop out by that depth (ApplyImpulse, flag 1). mode 1 on a slow or already tipping prop turns its
// heading about n (the first tip: the fall side +0x220, +0x1E8 floored at 5.0, +0x250 |= 8; a standing
// one +0x250 bit 4: PropUpRows and +0x1E4 = 3.0) and returns 1. Otherwise Bounce off n (k 5, floor 6.0
// or 0 for mode 1) and, when it bounced, three Rand for the spin +0x220 / +0x228 and +0x1E8, and the
// surface sound SurfaceSound(+0xB4) + 14 at the prop. v0 1. The stale sp+48 of a `point`-less call is
// read from the guest frame as the original reads it.
bool PropTopple(GuestRam& g, uint32_t p, uint32_t point, uint32_t n, uint32_t mode, uint32_t sp, const BikeTables& t,
                CollisionCallees& c, uint32_t& v0);

// RASHCDG 0x800B2F94 PropKick(prop, bike, s16 dir[3]) - 944 B, frame 32: a toppling prop (+0x250 bit 1)
// goes to PropTopple(prop, 0, dir, 0); else the prop takes 1.1 x the bike's speed, and above 5.0 a
// heading turned by Rand() % 714 - 357, lifted by LaunchLift by its DOD3 kind, a spin rate speed / 3
// (at most 10.0), a random spin axis (two more Rand) and +0x250 |= 3; at or below 5.0 the bike's heading.
// Then SurfaceSound(+0xB4) at the prop. v0 1.
bool PropKick(GuestRam& g, uint32_t p, uint32_t bike, uint32_t dir, uint32_t sp, const BikeTables& t,
              CollisionCallees& c, uint32_t& v0);

// RASHCDG 0x800B3838 PropKnock(prop, dirF, dirR, dirU, [sp+16] speed) - 664 B, frame 64: the frame
// {dirR, dirU, dirF} (negated R and F when the prop's +0x1BC faces away), turned by Rand() % 682 - 341
// and then by -+1024 about its own rows (RotateRowPair), becomes the target quaternion +0x23C
// (MatrixQuat); +0x250 |= 0x200, bit 6 = the two quaternions' dot is negative; the tip rate +0x1E4 =
// FixMul(6443, speed - 1.0059) plus +0x22C (at most 1.0 once past 0.75), +0x1E8 = twice the first.
bool PropKnock(GuestRam& g, uint32_t p, uint32_t dirF, uint32_t dirR, uint32_t dirU, int32_t speed, uint32_t sp,
               const BikeTables& t);

// RASHCDG 0x800B2D88 TrafficVsProp(car, prop) - 216 B, frame 96: a prop inside the camera box (0x280000,
// 0x380000) with one of its corners inside the car's box: the car's face normal, the prop pushed out by
// depth + 0.125 along it (ApplyImpulse flag 1) and PropKick(prop, car, n).
bool TrafficVsProp(GuestRam& g, uint32_t car, uint32_t p, uint32_t sp, const BikeTables& t, CollisionCallees& c);

// ---------------------------------------------------------------------------- the prop animation pass
// RASHCDG 0x80071A28 QuatNormalize(s16 q[4]) - 104 instructions, frame 56: q << 2 in the frame, the
// reciprocal of SqrtGte(sum of FixMul squares) << 2 (the divu idiom), q = FixMul(q << 2, inv) >> 2.
void QuatNormalize(GuestRam& g, uint32_t q, uint32_t sp, const BikeTables& t);
// RASHCDG 0x8007198C QuatMatrixT(s16 q[4], s16 out[9]) - 38 instructions, frame 56: 0x800714A0 (q << 2
// into its frame, SLUS 0x8001005C QuatToMatrix into this frame) and the TRANSPOSE into `out`. False where
// QuatToMatrix's `add` / `sub` would trap (pose.h).
bool QuatMatrixT(GuestRam& g, uint32_t q, uint32_t out, uint32_t sp);
// RASHCDG 0x800A40D4 PropFrameRows(prop) - 69 instructions, frame 24: +0x250 bit 2 (on the road): row 0
// +0x1B0 = the frame's +0x204, rows +0x1BC / +0x1B6 = the up +0x20A / the side +0x210 (the side negated,
// both negated when the old +0x1BE was negative), a class-2 prop's pair then turned by -+227; else the
// frame copied (SLUS 0x8003FA18 CopyHalfwords 9).
void PropFrameRows(GuestRam& g, uint32_t p, uint32_t sp, const BikeTables& t);
// RASHCDG 0x800A2A64 PropAnimPass(dt) - 1030 instructions, frame 136, called by the rider/engine pass
// 0x8008ACE8 after the rider pass: every live pool-4 prop - a pushed one (+0x250 bit 8) re-seated on the
// road (0x80037450, PropSettle 0x800A3A84); a knocked one's first frame (bit 0: the tip frame +0x230 /
// +0x236 / +0x23C from its rows, the tip angles +0x21C / +0x224); a tumbling one (bit 1: the spin, the
// fall onto its side (bits 3, 5, 6), gravity *(0x800D3964), the velocity); a toppling one (bit 9: the
// slerp towards +0x23C by +0x1E8, QuatMatrixT into the rows, BuildObb); a sliding one (+0x1E0 >= 0.2:
// the drag along the up / side axes, bit 7's re-squaring, PropFrameRows). Then the heading words +0x124.
// Callees through RecoverCallees: BuildObb 0x8008BA18, and PropSettle's GroundQuery 0x800A7BF8.
bool PropAnimPass(GuestRam& g, int32_t dt, uint32_t sp, const BikeTables& t, RecoverCallees& c);

// SLUS 0x80017B30 SurfaceSound(k) over the guest's own table SLUS 0x800525C0 (sound.h SurfaceSound).
uint32_t GSurfaceSound(GuestRam& g, int32_t k);

// The group's serve (coll_serve.h): false when `fn` is not one of the above.
bool ServeSolid(GuestRam& g, const CollCall& call, const BikeTables& t, CollisionCallees& c, uint32_t& v0, bool& ok);

} // namespace rr::sim
