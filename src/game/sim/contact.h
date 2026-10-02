#pragma once
// The bike pair: BikeVsBike and its classifier and responses.
// Transcribed from our own disassembly of the player's own images:
//
//   RASHCDG.BIN  SHA-1 cfe43a7786759f2cb9c57751cf99e84d1074782c, loaded at 0x8005B5E8
//   SLUS_010.53  SHA-1 67ed165a2c517d4e6106fb0dfa324d66dd9a76f1, text at 0x80010000
//
// and accepted only by `rrverify phys` rows (tools\rrverify\rows_coll_pair.inc). Memory model and stack
// rule: collision.h. Every local the original keeps in its frame is kept in the guest stack at the
// original's address (the responses pass most of them to callees by pointer, and 0x800AA474 reads two
// words of its frame it has not written). Callees of another collision group go
// through CollisionCallees::Unported; the product serves them (coll_serve.h).
#include <cstdint>

#include "game/sim/coll_serve.h"

namespace rr::sim {

namespace pair {
constexpr uint32_t kBikeVsBike    = 0x800AB7A0; // (me, other, mode), frame 152
constexpr uint32_t kClassify      = 0x800ABE78; // (a, b, &side, &angle, [&along, &across]), frame 48
constexpr uint32_t kResponseFar   = 0x800AA140; // (A, B, &outA, &outB, [vout]), frame 80
constexpr uint32_t kViewExtent    = 0x800AA34C; // (corners, rec, out[4]), frame 48
constexpr uint32_t kResponse      = 0x800AAD30; // (A, B, &outA, &outB, [vout, &remaining]), frame 200
constexpr uint32_t kHeavyResponse = 0x800AA474; // (A, B, flags, angle, [along, across, &oA, &oB, vout]), frame 264
constexpr uint32_t kPairPushDir   = 0x800B5B48; // (X, Y, out s16[3]), frame 56
// the leaves of the resolvers group and the reaction, reached through CollisionCallees::Unported
constexpr uint32_t kFaceNormal    = 0x800B675C; // (corners, axes, face, out, [0])
constexpr uint32_t kFacePoint     = 0x800B71AC; // (corners, dir, t, corners2, [axes, flags, &face, &x])
constexpr uint32_t kFaceDeepest   = 0x800B74F0; // same shape
constexpr uint32_t kBikeBikeReact = 0x800AC130; // (a, b, codeA, codeB)
} // namespace pair

// RASHCDG 0x800ABE78 BikePairClassify(a, b, &side, &angle, [sp+16] &along, [sp+20] &across) - 696 B:
// 4 when the box centres' y differ by less than a+0x138; +8 when the headings +0x124 are within 384 of
// parallel (either way); 12: the across offset (AiProject of b's centre on a's lateral row) against the
// summed widths (a passenger's added on its side), -1 when apart, +1 beside; 13: the along offset
// against 7/8 or 9/16 of the summed lengths, +2 when overlapping; 15 with speeds within 0xEFFFF -> 31.
// Reads its stack arguments from entry sp + 16 / + 20.
int32_t BikePairClassify(GuestRam& g, uint32_t a, uint32_t b, uint32_t pSide, uint32_t pAngle, uint32_t sp);

// RASHCDG 0x800AA34C ViewExtent(corners, rec, out[4]) - 296 B: {min u, min v, max u, max v} of the
// corners 0, 2, 1, 3 projected on rec+2 / rec+14 about rec+20.
void ViewExtent(GuestRam& g, uint32_t corners, uint32_t rec, uint32_t out);

// RASHCDG 0x800AA140 ResponseFar(A, B, &outA, &outB, [sp+16] vout) - 524 B, frame 80: the separation of
// two boxes in A's view plane (+0x154), the push along the shallower axis, the face codes | 0x200 from
// each heading against the plane's. Returns B, or 0 when apart.
bool ResponseFar(GuestRam& g, uint32_t A, uint32_t B, uint32_t outA, uint32_t outB, uint32_t sp, const BikeTables& t,
                 uint32_t& v0);

// RASHCDG 0x800B5B48 PairPushDir(X, Y, out s16[3]) - 876 B, frame 56: the unit direction of
// X's velocity plus Y's scaled by the mass ratio FixDiv(Y+0x13C, X+0x13C).
void PairPushDir(GuestRam& g, uint32_t X, uint32_t Y, uint32_t out, const BikeTables& t);

// RASHCDG 0x800AA474 HeavyResponse(A, B, flags, angle, [along, across, &outA, &outB, vout]) - 2236 B,
// frame 264: the side-by-side pair (class 31) resolved in the upper box's lateral plane by two
// point-in-quadrilateral passes. Returns the pushed entity or 0.
bool HeavyResponse(GuestRam& g, uint32_t A, uint32_t B, uint32_t flags, uint32_t angle, uint32_t sp,
                   const BikeTables& t, uint32_t& v0);

// RASHCDG 0x800AAD30 Response(A, B, &outA, &outB, [vout, &remaining]) - 2672 B, frame 200: the swept box
// test over up to four substeps and up to four bike/passenger pairings, the face leaves of the resolvers
// group, the push vector and the face codes; HeavyResponse for a class-31 pair found on a substep.
bool Response(GuestRam& g, uint32_t A, uint32_t B, uint32_t outA, uint32_t outB, uint32_t sp, const BikeTables& t,
              CollisionCallees& c, uint32_t& v0);

// RASHCDG 0x800AB7A0 BikeVsBike(me, other, mode) - 1752 B, frame 152: the heading
// re-derivation, the camera band with the chain bits, the classifier, one of the three responses, the
// push applied (mode 0), the mass-weighted separation, and the contact record with ChainReaction - or
// BikeBikeReact at once when the list is full or the pair is far from the camera.
bool BikeVsBike(GuestRam& g, uint32_t me, uint32_t other, int32_t mode, uint32_t sp, const BikeTables& t,
                CollisionCallees& c, uint32_t& v0);

} // namespace rr::sim
