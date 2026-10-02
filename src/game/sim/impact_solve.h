#pragma once
// The impact outcome solver and the reactions that reach it. Transcribed from our own disassembly of the player's own images:
//
//   RASHCDG.BIN  SHA-1 cfe43a7786759f2cb9c57751cf99e84d1074782c, loaded at 0x8005B5E8
//   SLUS_010.53  SHA-1 67ed165a2c517d4e6106fb0dfa324d66dd9a76f1, text at 0x80010000
//
// and accepted only by `rrverify phys` rows. Memory model and stack rule: collision.h. Every callee not
// written inline goes through CollisionCallees::Unported; the product serves it (coll_serve.h).
//
// STACK ARGUMENTS. Each port takes its o32 stack arguments as C++ arguments AND stores them at
// sp + 16 .. (the caller's outgoing-argument area, where the original's caller put them), so a port
// called natively reads the same words the original would re-load from there (ImpactSolve re-reads
// its `imp` slot after zeroing it; BikeWallHit re-loads it at 0x800B1480).
#include <cstdint>

#include "game/sim/coll_serve.h"

namespace rr::sim {

namespace solve {
constexpr uint32_t kFaceCrossing  = 0x800B5EB4; // (A, B, dirV, n, [len2, P0..P3, ax1, ax2, lim1, lim2, *cross, *t])
constexpr uint32_t kLandOnTop     = 0x800AD9BC; // (e, h, cross, throw, [k])
constexpr uint32_t kPropHitFace   = 0x800B0510; // (e, prop, s16 out[3])
constexpr uint32_t kImpactSolve   = 0x800AF3B0; // (e, shape, nrm, face, [flags, imp])
constexpr uint32_t kImpactGate    = 0x800AF224; // (e, shape, code, flags, [imp])
constexpr uint32_t kPoleReact     = 0x800AF0A0; // (e, shp, dir, flags, [bit4, imp])
constexpr uint32_t kBoxReact      = 0x800B11B4; // (e, shp, code, flags, [imp, dir])
constexpr uint32_t kBikeWallHit   = 0x800B12A0; // (e, n, flags, segVal, [seg, imp])
// what these ports reach and do not port (the bench runs them from the original code)
constexpr uint32_t kFaceNormal    = 0x800B675C; // (corners, axes, face, s16 out[3], [s32 *point])
constexpr uint32_t kImpactSeverity = 0x800A9408; // (e, mag, num, den, [mode])
constexpr uint32_t kHitOutcome    = 0x80083928; // (e, car, n)
constexpr uint32_t kImpactTurn    = 0x80083F30; // (e, shape, n, mode)
constexpr uint32_t kPropKnock     = 0x800B3838; // (prop, dirF, dirR, dirU, [speed])
constexpr uint32_t kPropKick      = 0x800B2F94; // (prop, bike, dir)
constexpr uint32_t kSurfaceSound  = 0x80017B30; // SLUS (surface) -> sound id
constexpr uint32_t kLaunch        = 0x80084BE8; // (e, 0) the crash launch
constexpr uint32_t kRaiseHeading  = 0x8007E868; // (dir, speed, cos, k)
constexpr uint32_t kGetRCnt       = 0x80043F00; // SLUS (counter)
constexpr uint32_t kWallStance    = 0x80027258; // SLUS (e, side)
// ENV (DATA\ENV.EN at 0x800D38E0): +0x88 the low landing height, +0x8C the high one, +0x90 the throw speed
constexpr uint32_t kEnvLandLow    = 0x800D3968;
constexpr uint32_t kEnvLandHigh   = 0x800D396C;
constexpr uint32_t kEnvThrow      = 0x800D3970;
} // namespace solve

// RASHCDG 0x800B5EB4 FaceCrossing(A, B, dirV, n, [st[0..10]]) - 1752 B, frame 104: does
// the segment A..B cross the rectangle spanned from st[1] (P0) by the axes st[5] / st[6] with extents
// st[7] / st[8]? -1 by the slab codes or by an all-corners-one-side test (four AiProject), else 1 with
// *st[9] = the crossing side and *st[10] = st[0] + the nearer entry distance. Spills a0, a2, a3 into the
// caller's home slots sp+0 / +8 / +12.
bool FaceCrossing(GuestRam& g, uint32_t A, uint32_t B, uint32_t dirV, uint32_t n, const uint32_t (&st)[11],
                  uint32_t sp, uint32_t& v0);

// RASHCDG 0x800AD9BC LandOnTop(e, h, cross, throw, [k]) - 696 B, frame 64: +0x300 = -h, +0x234 |= 0x218000
// (and 0x20000 by `throw`), the landing pitch +0x304 and lift +0x268; with `throw` and k >= 1.0 (or
// `cross`) the crash launch 0x80084BE8 (unported). `t` needs asin and sincos.
bool LandOnTop(GuestRam& g, uint32_t e, int32_t h, uint32_t cross, uint32_t thr, int32_t k, uint32_t sp,
               const BikeTables& t, CollisionCallees& c, uint32_t& v0);

// RASHCDG 0x800B0510 PropHitFace(e, prop, out) - 1204 B, frame 72: the prop's velocity (+0x1C8 from
// +0x1E0 along +0x1C2); 0 when the two separate (SLUS 0x8002E604 Dot32, inline), else the bike row facing
// the prop copied (or negated) into out and 1.
bool PropHitFace(GuestRam& g, uint32_t e, uint32_t prop, uint32_t out, uint32_t sp, uint32_t& v0);

// RASHCDG 0x800AF3B0 ImpactSolve(e, shape, nrm, face, [flags, imp]) - 4448 B, frame 256:
// stop a crawling bike, knock a prop over, land on top of the partner, or push the bike by *imp
// and hand the hit over (HitOutcome for a car, ImpactTurn otherwise; both unported). Carries the three
// guest quirks: the prop's reversed impulse truncated to 16 bits (written back through imp), the rumble
// strength from the NEGATED prop pointer, and PropKick handed the frame's sp+64 even when nothing wrote it.
// `t` needs asin and sincos (LandOnTop).
bool ImpactSolve(GuestRam& g, uint32_t e, uint32_t shape, uint32_t nrm, uint32_t face, uint32_t flags,
                 uint32_t imp, uint32_t sp, const BikeTables& t, CollisionCallees& c, uint32_t& v0);

// RASHCDG 0x800AF224 ImpactGate(e, shape, code, flags, [imp]) - 396 B, frame 56: the
// face code and normal (FaceNormal 0x800B675C, unported) for the solver; code 0 only when riding on this
// very shape - then the normal handed on is the frame's UNWRITTEN sp+24.
bool ImpactGate(GuestRam& g, uint32_t e, uint32_t shape, uint32_t code, uint32_t flags, uint32_t imp,
                uint32_t sp, const BikeTables& t, CollisionCallees& c, uint32_t& v0);

// RASHCDG 0x800AF0A0 PoleReact(e, shp, dir, flags, [bit4, imp]) - 388 B, frame 80: the solver, then
// ImpactSeverity 0x800A9408 (unported) by the speed before it.
bool PoleReact(GuestRam& g, uint32_t e, uint32_t shp, uint32_t dir, uint32_t flags, uint32_t bit4, uint32_t imp,
               uint32_t sp, const BikeTables& t, CollisionCallees& c, uint32_t& v0);

// RASHCDG 0x800B11B4 BoxReact(e, shp, code, flags, [imp, dir]) - 236 B, frame 40: ImpactGate, then
// ImpactSeverity by |FixMul(-DotLcm(heading, dir), speed)|.
bool BoxReact(GuestRam& g, uint32_t e, uint32_t shp, uint32_t code, uint32_t flags, uint32_t imp, uint32_t dir,
              uint32_t sp, const BikeTables& t, CollisionCallees& c, uint32_t& v0);

// RASHCDG 0x800B12A0 BikeWallHit(e, n, flags, segVal, [seg, imp]) - 1108 B, frame 64: a bike against a
// road wall segment. Fast into a wall while airborne: the heading re-aimed (Asin, 0x8007E868) and the
// velocity rebuilt. Otherwise the segment's +4 word is replaced by segVal (0x640000 when negative) for
// an ImpactSolve on the segment - what pushes the bike off the wall - and restored after; on a hit the
// severity, the scrape sounds, the rumble and the stance call 0x80027258. `t` needs asin and sincos.
bool BikeWallHit(GuestRam& g, uint32_t e, uint32_t n, uint32_t flags, int32_t segVal, uint32_t seg, uint32_t imp,
                 uint32_t sp, const BikeTables& t, CollisionCallees& c);

} // namespace rr::sim
