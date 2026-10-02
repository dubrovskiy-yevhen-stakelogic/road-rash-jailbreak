#pragma once
// The box/face geometry leaves and the partner resolvers. Transcribed from our own disassembly of the player's own images:
//
//   RASHCDG.BIN  SHA-1 cfe43a7786759f2cb9c57751cf99e84d1074782c, loaded at 0x8005B5E8
//   SLUS_010.53  SHA-1 67ed165a2c517d4e6106fb0dfa324d66dd9a76f1, text at 0x80010000
//
// and accepted only by `rrverify phys` rows. Memory model and stack rule: collision.h. Every callee not
// written inline goes through CollisionCallees::Unported; the product serves it (coll_serve.h).
//
// `sp` is always the stack pointer AT ENTRY. Where the original keeps a local that a callee reads or
// writes through a pointer, the port keeps it in the guest stack at the original's frame address.
// Every function returns false when the view faulted or a callee could not be served.
#include <cstdint>

#include "game/sim/coll_serve.h"

namespace rr::sim {

// ---------------------------------------------------------------------------- the geometry leaves
// RASHCDG 0x800B675C FaceNormal(corners, rows, face, s16 out[3], [sp+16] s32 *outPt) - 232 B, frame 8,
// leaf: the 6-byte table 0x8005B970 is copied into the frame and indexed by
// `face` WITHOUT a bound (a face past 5 reads the frame's own unwritten bytes and then the caller's);
// faces 0, 1, 5 give the negated row and corner 4, every other face the row itself and corner 2;
// outPt (when not 0) gets that corner.
bool FaceNormal(GuestRam& g, uint32_t corners, uint32_t rows, uint32_t face, uint32_t out, uint32_t outPt,
                uint32_t sp);

// RASHCDG 0x800B6B58 XzProject(p, s16 n[3], q) - 30 instructions, frame 8, leaf: the x/z part of
// AiProject, FixMul(n.x << 4, p.x - q.x) + FixMul(n.z << 4, p.z - q.z).
int32_t XzProject(GuestRam& g, uint32_t p, uint32_t n, uint32_t q);

// RASHCDG 0x800B6D70 PointInPoly(pt, s32 verts[][3], s16 normals[][3], n, [sp+16] tol) - 152 B,
// frame 48: 1 unless AiProject(pt, normals[i], verts[i]) < -tol for some i < n (0 then, at once).
bool PointInPoly(GuestRam& g, uint32_t pt, uint32_t verts, uint32_t normals, int32_t n, int32_t tol, uint32_t sp,
                 uint32_t& v0);

// RASHCDG 0x800B7030 FirstPointInsideBox(pts[8][3], rows, box[8][3], mask, [sp+16] *outFace,
// [sp+20] *outDepth) - 380 B, frame 56: the first point inside all six faces,
// the smallest unmasked face depth accumulated across ALL points tested (not reset per point).
bool FirstPointInsideBox(GuestRam& g, uint32_t pts, uint32_t rows, uint32_t box, uint32_t mask, uint32_t outFace,
                         uint32_t outDepth, uint32_t sp, uint32_t& v0);

// RASHCDG 0x800B71AC DeepestInsideAlongDir(pts, s16 dir[3], sign, box, [sp+16] rows, [+20] mask,
// [+24] *outFace, [+28] *outDist) - 836 B, frame 120: the inside points, then per point the nearest
// face along -dir (RayPlane, clamped up to 2048), and the point whose nearest face is FARTHEST.
bool DeepestInsideAlongDir(GuestRam& g, uint32_t pts, uint32_t dir, int32_t sign, uint32_t box, uint32_t rows,
                           uint32_t mask, uint32_t outFace, uint32_t outDist, uint32_t sp, uint32_t& v0);

// RASHCDG 0x800B7810 FaceQuad(box, rows, face, s32 quad[4][3], [sp+16] s16 edgeN[4][3]) - 2056 B,
// no frame: a six-way jump table (0x8005B978) on the face, each case copying the face's four corners
// and building its four edge normals from two rows, in its own load/store order. A face >= 6 does
// nothing.
bool FaceQuad(GuestRam& g, uint32_t box, uint32_t rows, uint32_t face, uint32_t quad, uint32_t edgeN);

// RASHCDG 0x800B74F0 DeepestThroughFace(pts, s16 dir[3], len, box, [sp+16] rows, [+20] mask,
// [+24] *outFace, [+28] *outDist) - 800 B, frame 576: per unmasked face the points in front of it whose
// ray along -dir crosses the face quad; the best point/distance carry over faces, the first face with
// DotLcm(n, dir) < 4097 answers, else the last fall-back.
bool DeepestThroughFace(GuestRam& g, uint32_t pts, uint32_t dir, int32_t len, uint32_t box, uint32_t rows,
                        uint32_t mask, uint32_t outFace, uint32_t outDist, uint32_t sp, uint32_t& v0);

// RASHCDG 0x800B2C98 NoteBigVolume(e, shape) - 172 B, frame 32: the low byte of the shape's handle
// appended to the list 0x800CCF88 (count 0x800CCF90, at most 8) when e is in the camera band and
// the id is not listed yet. v0 is not meaningful.
bool NoteBigVolume(GuestRam& g, uint32_t e, uint32_t shape, uint32_t sp);

// ---------------------------------------------------------------------------- the partner resolvers
// RASHCDG 0x800B09C4 PointResolve(e, shape) - 968 B, frame 608. For a pool 5/6
// partner a fake entity is built in the frame (sp+24) and handed to the response. Callees through
// Unported: 0x800AAD30, 0x800AA140, ImpactGate 0x800AF224, ImpactSeverity 0x800A9408.
bool PointResolve(GuestRam& g, uint32_t e, uint32_t shape, uint32_t sp, const BikeTables& t, CollisionCallees& c);

// RASHCDG 0x800B0D8C BoxResolve(e, shape) - 1064 B, frame 96. Returns the shape
// (the early return) or 1. Callees through Unported: BoxReact 0x800B11B4.
bool BoxResolve(GuestRam& g, uint32_t e, uint32_t shape, uint32_t sp, const BikeTables& t, CollisionCallees& c,
                uint32_t& v0);

// RASHCDG 0x800AE794 PoleResolve(e, shape) - 2316 B, frame 152. Callees through
// Unported: 0x800ADC74, PoleReact 0x800AF0A0.
bool PoleResolve(GuestRam& g, uint32_t e, uint32_t shape, uint32_t sp, const BikeTables& t, CollisionCallees& c);

// RASHCDG 0x800B2794 RiderWallHit(rider, n, flags) - 176 B, frame 40: PedHit
// 0x800A9868(rider, n, 0, n, [&rider->AC, 0]) through Unported and, when it answers non-zero and the
// speed class clamp(|+0x1E0| / 2^17, 0, 4) is positive, PlaySound3D(x, z, 19, 0). `flags` is not read.
bool RiderWallHit(GuestRam& g, uint32_t rider, uint32_t n, uint32_t sp, CollisionCallees& c);

// RASHCDG 0x800AD04C BikeVsRider(bike, p) - 2416 B, frame 136: the bike against a
// rider (pool 1) or a pedestrian (pool 2). Callees through Unported: 0x800AAD30, 0x800AA140, ImpactGate
// 0x800AF224, 0x800C29F0, PedHit 0x800A9868, SLUS 0x8001A760 and 0x8001B44C; PadRumble through
// CollisionCallees::Rumble; PlaySound3D.
bool BikeVsRider(GuestRam& g, uint32_t e, uint32_t p, uint32_t sp, const BikeTables& t, CollisionCallees& c);

// SLUS 0x8002E604 Dot32(s32 a[3], s32 b[3]) - 37 instructions, frame 8, leaf: FixMul(a0, b0) + FixMul(a1, b1)
// + FixMul(a2, b2), 32-bit adds (the 32-bit dot product BikeVsRider calls).
int32_t Dot32(GuestRam& g, uint32_t a, uint32_t b);

namespace rsv { // the resolvers group's own names (other groups may name the same callees)
constexpr uint32_t kFaceNormal        = 0x800B675C;
constexpr uint32_t kXzProject         = 0x800B6B58;
constexpr uint32_t kPointInPoly       = 0x800B6D70;
constexpr uint32_t kFirstPointInside  = 0x800B7030;
constexpr uint32_t kDeepestInside     = 0x800B71AC;
constexpr uint32_t kDeepestThrough    = 0x800B74F0;
constexpr uint32_t kFaceQuad          = 0x800B7810;
constexpr uint32_t kNoteBigVolume     = 0x800B2C98;
constexpr uint32_t kResponse          = 0x800AAD30; // (a, b, &code, &flags, [&push, &mag])
constexpr uint32_t kResponseFar       = 0x800AA140; // (a, b, &code, &flags, [&push])
constexpr uint32_t kImpactGate        = 0x800AF224; // (e, shape, code, flags, [imp])
constexpr uint32_t kImpactSeverity    = 0x800A9408; // (e, mag, num, den, [mode])
constexpr uint32_t kLeanPoleTest      = 0x800ADC74; // (e, shape, radius, push, [&len])
constexpr uint32_t kDot32             = 0x8002E604; // SLUS (a, b)
constexpr uint32_t kRiderGrab         = 0x800C29F0; // (rider, bike)
constexpr uint32_t kRiderSpeech       = 0x8001A760; // SLUS (bikeHandle, 1)
constexpr uint32_t kPedVoice          = 0x8001B44C; // SLUS (x, z, ped, 1)
} // namespace rsv

} // namespace rr::sim
