#pragma once
// The collision pass RASHCDG 0x800A4774 and what an airborne bike's landing and the road walls need
// under it, ported function by function.
// Transcribed from our own disassembly of the player's own images:
//
//   RASHCDG.BIN  SHA-1 cfe43a7786759f2cb9c57751cf99e84d1074782c, loaded at 0x8005B5E8
//   SLUS_010.53  SHA-1 67ed165a2c517d4e6106fb0dfa324d66dd9a76f1, text at 0x80010000
//
// and accepted only by `rrverify phys` rows (tools\rrverify\rows_collision.inc).
//
// THE MEMORY MODEL is road_query.h's: a guest-address view of main RAM (`GuestRam`). The pass walks
// the entity pools through the pool table 0x800CE4D0, chases bike -> rider -> passenger where the
// original does (with the original's missing null tests), and keeps the broad-phase grid, the chain
// nodes and the contact list at their guest addresses.
//
// STACK. `sp` is always the stack pointer AT THE FUNCTION'S ENTRY (road_runtime.h's rule). A port
// computes its own frame from it the way its prologue does and hands every callee the `sp` the
// original makes the call at. Where the original keeps a local that a callee reads through a pointer
// (WallContact's normal N at frame+24 and push I at frame+96, AirContact's plane normal at frame+48),
// the port keeps that local in the guest stack at the original's address, so an unported callee
// executed from the original code (the bench) reads it where it always did. Everything else lives in
// C++.
//
// A function returns false - and the caller must not trust anything it wrote - when the original
// would do something the view refuses (a load outside RAM, Normalize's overflow exception) or a
// callee could not be served. It never guesses.
#include <cstdint>

#include "game/sim/integrator.h"
#include "game/sim/road_query.h"

namespace rr::sim {

// ---------------------------------------------------------------------------- globals
constexpr uint32_t kCollDt        = 0x800CCE38; // s32, the dt the pass was called with (0x800A47D8)
constexpr uint32_t kContactList   = 0x800CCE48; // ContactRec[8], 36 bytes each
constexpr uint32_t kContactCount  = 0x800CCF68; // s32
constexpr uint32_t kChainPrev     = 0x800CCF70; // u32[2], last pass's 2-bit fields
constexpr uint32_t kChainCur      = 0x800CCF78; // u32[2]
constexpr uint32_t kChainMask     = 0x800CCF80; // u32
constexpr uint32_t kCollGridOrgX  = 0x800CCF98; // s32[2]
constexpr uint32_t kCollGridOrgZ  = 0x800CCFA0; // s32[2]
constexpr uint32_t kCollNodes     = 0x800CCFA8; // {u8 prev; u8 handle}[128] + the sentinel node 128
constexpr uint32_t kCollGrid      = 0x800CD0B0; // u8[48][24], 0x80 = empty
constexpr uint32_t kCollRadius    = 0x800CCA8C; // s32[5], per mover kind
constexpr uint32_t kCornerOrder   = 0x800CCAA0; // u32[16], WallContact's corner-order nibbles
constexpr uint32_t kCollViews     = 0x800CD898; // 1132 bytes per player
constexpr uint32_t kPoolTableAddr = 0x800CE4D0; // {base, stride, &live, &last} x 7

// ---------------------------------------------------------------------------- the callees
// What the ports here reach that is not written inline. Each returns false when the caller could
// not run it; the calling port then refuses.
struct CollisionCallees {
    virtual ~CollisionCallees() = default;
    // SLUS 0x80017BA0 PlaySound3D(x, z, id, bank) - PORTED (sound.h); the caller runs it where the
    // sound system lives.
    virtual bool PlaySound3D(int32_t x, int32_t z, int32_t id, int32_t bank) = 0;
    // SLUS 0x8002076C ReleaseContact(e) - PORTED (integrator.h); the caller resolves the refs.
    virtual bool ReleaseContact(uint32_t e) = 0;
    // RASHCDG 0x800B658C PadRumble(e, other, speed, k, [sp+16] div) - PORTED below as PadRumble; the
    // caller runs it, because its only effect is the pad motors (SLUS 0x8001DD74, not ported) and
    // because with other == 0 it reads the word at 0x354 and follows it - into the BIOS ROM on every
    // captured machine - which a RAM view cannot follow and only the caller can.
    virtual bool Rumble(uint32_t e, uint32_t other, int32_t speed, int32_t k, int32_t div, uint32_t sp) = 0;
    // Every callee that is NOT ported, by its entry address: `a` holds the o32 arguments (the first
    // four in registers, the rest on the stack at sp + 16 ..), `sp` the stack pointer the original
    // makes the call at. `v0` receives the return value.
    virtual bool Unported(uint32_t fn, const uint32_t* a, int n, uint32_t sp, uint32_t& v0) = 0;
    // The same call, with the callee-saved registers s0..s8 and the return address the ORIGINAL has at
    // that call site (the callee's prologue saves them into its frame, and a
    // later unported callee at that depth can read them as its own uninitialised stack). A caller
    // without an oracle ignores them; the default does exactly that.
    virtual bool UnportedAt(uint32_t fn, const uint32_t* a, int n, uint32_t sp, const struct GuestRegs& r,
                            uint32_t& v0);
};
// s[0..7] = s0..s7, s[8] = s8 (fp); ra = the return address into the caller.
struct GuestRegs {
    uint32_t s[9] = {};
    uint32_t ra = 0;
};
inline bool CollisionCallees::UnportedAt(uint32_t fn, const uint32_t* a, int n, uint32_t sp, const GuestRegs&,
                                         uint32_t& v0) {
    return Unported(fn, a, n, sp, v0);
}

// The unported callees, named.
namespace coll {
constexpr uint32_t kBikeVsBike      = 0x800AB7A0; // (me, other, mode)
constexpr uint32_t kPointResolve    = 0x800B09C4; // (e, shape)
constexpr uint32_t kBoxResolve      = 0x800B0D8C; // (e, shape)
constexpr uint32_t kPoleResolve     = 0x800AE794; // (e, shape)
constexpr uint32_t kBikeVsRider     = 0x800AD04C; // (bike, rider)
constexpr uint32_t kBikeVsTraffic   = 0x800AC5BC; // (bike, car)
constexpr uint32_t kRiderNone       = 0x800B2AF8; // (e, p) - jr ra
constexpr uint32_t kRiderVsTraffic  = 0x800B2844; // (e, car)
constexpr uint32_t kRiderVsShape    = 0x800B2B00; // (e, shape)
constexpr uint32_t kTrafficVsTraffic = 0x800B2D44; // (e, p)
constexpr uint32_t kTrafficVsProp   = 0x800B2D88; // (e, p)
constexpr uint32_t kPropVsShape     = 0x800B2E64; // (e, shape)
constexpr uint32_t kDeferred        = 0x800A77B0; // () - PORTED: DeferredContacts below
constexpr uint32_t kImpactTurn      = 0x80083F30; // (e, partner, n, mode)
constexpr uint32_t kRiderGetUp      = 0x800B208C; // (e) - the kind 1/2 tail's
constexpr uint32_t kPropTopple      = 0x800B3344; // (prop, point, n, mode)
constexpr uint32_t kBikeWallHit     = 0x800B12A0; // (bike, n, flags, segVal, [seg, &I])
constexpr uint32_t kRiderWallHit    = 0x800B2794; // (rider, n, flags)
constexpr uint32_t kPedHit          = 0x800A9868; // (ped, n, 0, n, [&ped->AC, 0])
constexpr uint32_t kPadMotor        = 0x8001DD74; // SLUS (pad, a, 150, b)
// the deferred handler's children (BikeWallHit unread)
constexpr uint32_t kBikeTrafficReact = 0x800AC958; // (bike, car, code, flags, [imp])
constexpr uint32_t kPoleReact       = 0x800AF0A0; // (bike, shape, n, flags, [tall, imp])
constexpr uint32_t kBoxReact        = 0x800B11B4; // (bike, shape, code, flags, [imp, n])
constexpr uint32_t kBikeBikeReact   = 0x800AC130; // (a, b, codeA, codeB)
} // namespace coll

// ---------------------------------------------------------------------------- the leaves
// RASHCDG 0x800A8DF0 ApplyImpulse(e, v, flag) - 126 instructions, no calls, no frame: the box centre
// +0xB8 and the eight corners +0xC4 moved by v (re-read for every component), the passenger's too for
// a pool-0 entity with e[+0x358] and a list node e[+0x440] (the passenger pointer re-read for every
// word); with `flag`, the per-pool "pushed" bit (pool 0: flagsC |= 0x01800000; 1: +0x228 |= 2;
// 2: +0x238 |= 2; 4: +0x250 |= 0x100).
void ApplyImpulse(GuestRam& g, uint32_t e, uint32_t v, int32_t flag);

// RASHCDG 0x800A8FE8 StaleHeading(e) - frame 48: when flagsC bit 24 is set, clears it and re-derives
// +0x1E0 = |box - +0x1D4| / dt and the heading +0x1C2 from the displacement (latching +0x35C..+0x365
// once per pass, flagsC bit 25), and recurses on a passenger. Returns 1 when it re-derived.
// `t.sqrt` is read. False: the view faulted.
bool StaleHeading(GuestRam& g, uint32_t e, const BikeTables& t, int32_t& v0);

// RASHCDG 0x800A8C78 InCameraBox(e, halfWidth, depth) - frame 56: cached in +0x140 bits 2/3; writes
// +0x2C + 4p (the forward, or lateral, camera coordinate >> 10) and +0x30 = 0x7FFFFFFF first. The
// player loop is a do-while (it runs once even with game_state+0x30 == 0).
int32_t InCameraBox(GuestRam& g, uint32_t e, int32_t halfWidth, int32_t depth);

// RASHCDG 0x800B6F40 CornerMin(c[8][3], n, q, outDepth, [sp+16] outCount) - frame 56: the corner
// deepest behind the plane (AiProject < 0), first strict minimum; *outCount (when not 0) = how many
// are behind; returns 8 (outDepth untouched) or the corner, with *outDepth = -min.
uint32_t CornerMin(GuestRam& g, uint32_t c, uint32_t n, uint32_t q, uint32_t outDepth, uint32_t outCount);

// RASHCDG 0x800B6BD0 RayPlane(p, dir, n, q) - frame 40: the parameter along `dir` from `p` to the
// plane through `q` with normal `n`, or 0x7FFF0000 when |DotLcm(n, dir)| < 64.
int32_t RayPlane(GuestRam& g, uint32_t p, uint32_t dir, uint32_t n, uint32_t q);

// RASHCDG 0x800B59F0 ContactMerge(rec) - frame 40: the newest record with the same handle pair and
// segment id absorbs `rec` (the deeper of two near-parallel ones, or the push vectors added unless
// the normals oppose). Returns 1 when nothing matched (the caller then counts `rec`), else 0.
int32_t ContactMerge(GuestRam& g, uint32_t rec);

// RASHCDG 0x800A451C ViewTrack(v, flag) - frame 48: flag != 0 re-derives the view object's +0x1E0
// and heading from its displacement since +0x1D4; flag == 0 re-aims it from its own +0x258 and
// steps +0x1D4 back. `t.sqrt` is read.
bool ViewTrack(GuestRam& g, uint32_t v, int32_t flag, const BikeTables& t);

// RASHCDG 0x800B658C PadRumble(e, other, speed, k, [sp+16] div) - frame 40: the two motor strengths
// speed*255/k and speed*150/k clamped to [0, 255/div] and [0, 150/div] (R3000 `div`), then
// SLUS 0x8001DD74 for e's pad and, on the passenger / rider conditions of 0x800B6674..0x800B6738, for a
// second pad. RumbleEnv::ReadForeignByte answers a byte load outside RAM (with other == 0 the original
// follows *(0x354), a BIOS ROM address on every captured machine); the port refuses when it cannot be
// answered.
struct RumbleEnv {
    virtual ~RumbleEnv() = default;
    virtual bool ReadForeignByte(uint32_t address, uint8_t& value) = 0;
    virtual bool PadMotor(uint32_t pad, int32_t a, int32_t b, int32_t c, uint32_t sp) = 0; // SLUS 0x8001DD74
};
bool PadRumble(GuestRam& g, uint32_t e, uint32_t other, int32_t speed, int32_t k, int32_t div, uint32_t sp,
               RumbleEnv& env);

// ---------------------------------------------------------------------------- landing
// RASHCDG 0x800B16F4 TouchDown(e, pSpeed) - frame 56: for a player the view's
// +0x2E4 = clamp(vy / 8, 0, 3.0); +0x240 = 0.9 or 0.1 of *pSpeed by the heading's sign against +0x1BC;
// flagsC bit 10 (AIRBORNE) CLEARED; the rider's +0x228 bit 13 cleared; above a +0x300 of 3.0 the
// landing sound 54 and the pad rumble; flagsC |= 0x0C000000.
bool TouchDown(GuestRam& g, uint32_t e, uint32_t pSpeed, uint32_t sp, CollisionCallees& c);

// RASHCDG 0x800B1978 AirContact(e) - 453 instructions, frame 96: the ground contact
// of a crashed (flagsC 0x200) or airborne (0x400) bike, the call the pass's kind-0 tail makes while
// flagsC & 0x600 and not flagsB bit 23. The deepest box corner (the passenger's when it is deeper)
// behind the ground plane through +0x1F8 with normal -(+0x20A); an airborne bike needs a depth past
// +0x2C4. Surface 4 or flagsB bit 20: stop dead above 5.0. Otherwise the bike is pushed out, the
// pre-contact speed latched, and then: airborne -> TouchDown (the LANDING); crashed -> a slide
// (flagsC bit 21) or a tumble re-aim, or a Bounce + Spin at speed; then ReleaseContact.
// `t` needs sincos, asin, atan, rsqrt and sqrt (Bounce, RatAtan2, Normalize).
bool AirContact(GuestRam& g, uint32_t e, uint32_t sp, const BikeTables& t, CollisionCallees& c);

// ---------------------------------------------------------------------------- the walls
// RASHCDG 0x800B3AD0 WallContact(e, force) - 1992 instructions, frame 296: the
// collision of one entity against the wall polylines of its road section. Returns 0, 1 (the view
// object) or 0x100 | corner. `t` needs rsqrt and sqrt.
bool WallContact(GuestRam& g, uint32_t e, int32_t force, uint32_t sp, const BikeTables& t, CollisionCallees& c,
                 uint32_t& v0);

// ---------------------------------------------------------------------------- the deferred reactions
// RASHCDG 0x800A7AB4 ChainReaction(e, vec, skip) - 324 B, frame 48: for every
// pool-0 handle below the bike count (re-read, compared as u16) other than e's and `skip`, when both
// are in the previous pass's contact chains (0x800CCF70), BikeVsBike on the pair (the lower handle
// first; unported, mode 1) and, when it reports a contact, e's push `vec` applied to the third bike.
// `caller`, when given, is the caller's registers at the call (its prologue spills them, and the
// unported BikeVsBike is then called with the registers the original has there); null: not reproduced.
bool ChainReaction(GuestRam& g, uint32_t e, uint32_t vec, uint32_t skip, uint32_t sp, CollisionCallees& c,
                   const GuestRegs* caller = nullptr);

// RASHCDG 0x800A77B0 DeferredContacts() - 772 B, frame 64: the contact list
// BACKWARDS, every non-bike record handed to its reaction by partner pool (8 the wall: BikeWallHit;
// 3 traffic: BikeTrafficReact; a pool-6 class-1 pole: PoleReact; anything else: BoxReact), the bike
// marked as reacted (bit handle & 31) and ChainReaction(e, &rec->push, 31) when it was in a chain; then
// FORWARDS (the count re-read) every bike x bike record of a bike that has not reacted to
// BikeBikeReact. The reactions are unported (seam).
// `caller` as for ChainReaction: its prologue spills are written and every reaction is called with the
// registers the original has at that call site.
bool DeferredContacts(GuestRam& g, uint32_t sp, CollisionCallees& c, const GuestRegs* caller = nullptr);

// ---------------------------------------------------------------------------- the pass
// RASHCDG 0x800A4774 CollisionPass(dt) - 3087 instructions, frame 112:
// the grid, the per-player origins, the pool walk 6..0 with the chain build (at most 128 nodes); then,
// when anything was inserted, the contact globals reset, the bike-against-bike phase, the kind loop
// 0..4 with its partner tables and tails (WallContact, AirContact, the impact hand-over), and the
// deferred handler after kind 0. The RaceTick calls it with the frame's dt, the prime pass with 0.
bool CollisionPass(GuestRam& g, int32_t dt, uint32_t sp, const BikeTables& t, CollisionCallees& c);
constexpr uint32_t kCollisionPassFrame = 112;

// ============================================================================ the rider pass's walk
// of the THROWN list - what makes an airborne bike fall
//
// RASHCDG 0x8007EA64 RowsFromUp(e) - 70 instructions, frame 40: the orientation rows from the up row
// +0x20A and the lean row +0x1BC with the GTE outer product (cop2 0x178000C): +0x204 = up x +0x1BC,
// Normalize; when that is the zero vector, +0x210 = +0x1B0 x up, Normalize (the identity reset
// 0x8008CF74 when that is zero too) and +0x204 = up x +0x210; otherwise +0x210 = +0x204 x up.
// False: Normalize's overflow exception.
bool RowsFromUp(GuestRam& g, uint32_t e, const BikeTables& t);

// The region [0x8007C23C, 0x8007C9A8) of the rider / engine pass RASHCDG 0x8007B840(dt) - 437
// instructions, single entry, nothing live at its exit but the pass's frame: the walk of the thrown
// list 0x8005B2D8 (flagsC & 0x400 - the airborne bikes). Per bike, the node's `next` re-read after the
// body:
//   drag a = FixMul(*(s32*)0x800D3960, dt); unless flagsB bit 20: +0x1C8 -= FixMul(a, vx),
//   +0x1CC -= FixMul(a, vy) - FixMul(+0x1E4, dt) (THE GRAVITY: +0x1E4 is g, set by the crash launch),
//   +0x1D0 -= FixMul(a, vz); +0x244 = |v|^2, +0x1E0 = +0x240 = SqrtGte(|v|^2) << 2 and the heading
//   +0x1C2 = v / |v| (ScaleTo16); the three tumble angles +0x26C/+0x268 (clamped to +-1.2217),
//   +0x2B4 (while an impact class runs, over +0x2D4 / +0x2D8) and +0x280/+0x27C (the passenger's
//   yaw rule), turned into the rows +0x1B0 by the sine table and twelve FixMul, then RowsFromUp; a
//   drop +0x300 above 3.0 latches flagsA bit 23 and, once, the rider's +0x228 bit 13 with the stance
//   event (2, rider, 10); a human's bike (flagsA bit 27 clear) runs the passenger launch and the crash
//   timer.
// `sp` is the pass's own frame (entry sp - 248), the stack pointer every callee is called at.
// `t` needs sincos, rsqrt and sqrt.
struct ThrownWalkCallees {
    virtual ~ThrownWalkCallees() = default;
    virtual bool StanceEvent(int32_t ev, uint32_t rider, int32_t p, uint32_t sp) = 0; // RASHCDG 0x800C4550, ported
    virtual bool PassengerLaunch(uint32_t e, uint32_t sp) = 0;                     // RASHCDG 0x80074D58, ported
    virtual bool CrashTimer(uint32_t e, int32_t dt, uint32_t sp) = 0;              // RASHCDG 0x80074E6C, ported
};
bool RiderPassThrownWalk(GuestRam& g, int32_t dt, uint32_t sp, const BikeTables& t, ThrownWalkCallees& c);
constexpr uint32_t kRiderPassFn = 0x8007B840;
constexpr uint32_t kRiderPassFrame = 248;           // `addiu sp,sp,-248` at 0x8007B840
constexpr uint32_t kThrownWalkEntry = 0x8007C23C;
constexpr uint32_t kThrownWalkExit = 0x8007C9A8;
constexpr uint32_t kAirDrag = 0x800D3960;           // s32, read at 0x8007C24C

} // namespace rr::sim
