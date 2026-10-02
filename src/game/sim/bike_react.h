#pragma once
// The bike-bike and bike-traffic reactions, and the rider pass's first pool loop. Transcribed from our own disassembly of the player's own images:
//
//   RASHCDG.BIN  SHA-1 cfe43a7786759f2cb9c57751cf99e84d1074782c, loaded at 0x8005B5E8
//   SLUS_010.53  SHA-1 67ed165a2c517d4e6106fb0dfa324d66dd9a76f1, text at 0x80010000
//
// and accepted only by `rrverify phys` rows. Memory model and stack rule: collision.h. Every callee not
// written inline goes through CollisionCallees::Unported; the product serves it (coll_serve.h).
#include <cstdint>

#include "game/sim/coll_serve.h"

namespace rr::sim {

namespace react {
constexpr uint32_t kSideFromCos     = 0x800AC56C; // (c, fl, other), 80 B leaf
constexpr uint32_t kRememberHandle  = 0x800A8BE0; // (h, set), 104 B leaf
constexpr uint32_t kImpactSeverity  = 0x800A9408; // (e, mag, num, den, [mode]), frame 56
constexpr uint32_t kBikeBikeReact   = 0x800AC130; // (a, b, codeA, codeB), frame 72
constexpr uint32_t kFaceNormal      = 0x800B675C; // (corners, rows, face, out16, [outPt]) - another group's
constexpr uint32_t kBikeBikeGate    = 0x80081D7C; // PORTED (crash.h), run natively
constexpr uint32_t kHitSpeed        = 0x80080D1C; // (e, dir, pSpeed, partner, [nrm, k, ang, lim, out, a9, 0])
constexpr uint32_t kTakePartnerHeading = 0x80080B10; // (e, dir, pSpeed, partnerDir, [out])
constexpr uint32_t kRiderSpeech     = 0x8001A760; // SLUS (h, crash)
constexpr uint32_t kStanceEvent     = 0x800C4550; // (ev, rider, p)
constexpr uint32_t kGetRCnt         = 0x80043F00; // SLUS (id)
constexpr uint32_t kImpactSeverityFrame = 56;
constexpr uint32_t kBikeBikeReactFrame = 72;
constexpr uint32_t kGateFrame       = 280;
constexpr uint32_t kGateNrmSlot     = 144;        // HitSpeed's nrm[k] at the gate's sp + 144 + 6k
} // namespace react

// RASHCDG 0x800AC56C SideFromCos(c, fl, other) - 80 B, leaf: nearly parallel
// (c > 0xDDB2) -> other ^ 2 when other <u 4, else 4; nearly opposite (c < -0xDDB2) -> other; else
// (fl & 2) ? 3 : 1.
uint32_t SideFromCos(int32_t c, uint32_t fl, uint32_t other);

// RASHCDG 0x800A8BE0 RememberHandle(h, set) - 104 B, leaf: the 6 x 5-bit "handles I have hit" set at
// *set; (h & 0xFFFF) + 1 is compared unmasked against each 5-bit entry, inserted masked. Returns 1
// (present or inserted) or 0 (full). False: the view faulted.
bool RememberHandle(GuestRam& g, uint32_t h, uint32_t set, uint32_t& v0);

// RASHCDG 0x800A9408 ImpactSeverity(e, mag, num, den, [sp+16] mode) - 612 B, frame 56:
// the severity 0..4 = clamp(5 mag / *(e->+0x22C + 0xE0), 0, 4) (R3000 `div`); the stance
// event 0x800C4550 (unported) for a riding rider; the damage byte of e->+0x43C when flagsC & 0x22C.
bool ImpactSeverity(GuestRam& g, uint32_t e, int32_t mag, int32_t num, int32_t den, int32_t mode, uint32_t sp,
                    CollisionCallees& c, uint32_t& v0);

// RASHCDG 0x800AC130 BikeBikeReact(a, b, codeA, codeB) - 1084 B, frame 72: the
// face normal (FaceNormal 0x800B675C, unported, into the frame at sp+24), BikeBikeGate 0x80081D7C
// run NATIVELY (crash.h) with its four callees and its stale frame words taken from the guest stack at
// the gate's frame, the two severities, the impact sound, the rumble and the remembered handles.
// `t` needs what BikeBikeGate needs (atan, sqrt, sincos, asin, rsqrt).
bool BikeBikeReact(GuestRam& g, uint32_t a, uint32_t b, uint32_t codeA, uint32_t codeB, uint32_t sp,
                   const BikeTables& t, CollisionCallees& c);

// ---------------------------------------------------------------------------- the traffic pair
namespace react {
constexpr uint32_t kTrafficSideShove = 0x800A91AC; // (code, flags, bike, car, [imp]), frame 64
constexpr uint32_t kBikeVsTraffic   = 0x800AC5BC; // (bike, car), frame 96
constexpr uint32_t kBikeTrafficReact = 0x800AC958; // (bike, car, code, flags, [imp]), frame 80
constexpr uint32_t kImpactGate      = 0x800AF224; // (bike, shape, code, flags, [imp]) - another group's
constexpr uint32_t kXzDot           = 0x800B6B58; // (p, row, q) - another group's
constexpr uint32_t kResponse        = 0x800AAD30; // (bike, car, &code, &flags, [&imp, &push]) - another group's
constexpr uint32_t kResponseFar     = 0x800AA140; // (bike, car, &code, &flags, [&imp]) - another group's
constexpr uint32_t kSurfaceFx       = 0x80027258; // SLUS (e, kind) - not ported
} // namespace react

// RASHCDG 0x800A91AC TrafficSideShove(code, flags, bike, car, [sp+16] imp) - 604 B, frame 64:
// on a car's side face (flags 0x200 | 1 or 3) with the contact point in the
// outer quarter of the car's half width and the bike heading the right way, the push that takes the
// point out to the side plus w/8 replaces (near the car's length) or is added to imp[3]; returns the
// flags (0x202 / 0x200 when it pushed). `q` lives in the frame at sp - 64 + 16 as in the original.
bool TrafficSideShove(GuestRam& g, uint32_t code, uint32_t flags, uint32_t bike, uint32_t car, uint32_t imp,
                      uint32_t sp, uint32_t& v0);

// RASHCDG 0x800AC958 BikeTrafficReact(bike, car, code, flags, [sp+16] imp) - 1780 B, frame 80:
// ImpactGate 0x800AF224 (unported), the side class from the car's face normal
// (FaceNormal 0x800B675C into the frame at sp - 80 + 24) or 0x800B6B58, the severity, the sound, the
// rumble and the car's speed; then the bike riding ON the car (flagsB bit 15, one GTE OP), and
// SLUS 0x80027258 (unported). THE UNINITIALISED READ: on the !(flags & 0x200) path the face
// normal's slots sp+24 / sp+28 are read without being written - the port reads them from the guest
// frame, where the original finds them. `t` needs sincos, atan and rsqrt. `caller`, when given, is the caller's
// s0..s8 and the calls are made REGISTER-FAITHFUL (UnportedAt, with s1..s4 and ra as the original has them at
// each call site): ImpactGate's solver reads what the callee-saved spills leave below it.
bool BikeTrafficReact(GuestRam& g, uint32_t bike, uint32_t car, uint32_t code, uint32_t flags, uint32_t imp,
                      uint32_t sp, const BikeTables& t, CollisionCallees& c, const GuestRegs* caller = nullptr);

// RASHCDG 0x800AC5BC BikeVsTraffic(bike, car) - 924 B, frame 96: StaleHeading,
// InCameraBox, the response 0x800AAD30 or 0x800AA140 (unported) into the frame's code / flags / imp /
// push slots (sp - 96 + 56 / 60 / 24 / 64), the side shove, the push along the car's heading
// (ApplyImpulse to both), then BikeTrafficReact now, or a ContactRec deferred. `t` needs sincos,
// atan, sqrt and rsqrt.
bool BikeVsTraffic(GuestRam& g, uint32_t bike, uint32_t car, uint32_t sp, const BikeTables& t,
                   CollisionCallees& c);

// ============================================================================ the rider pass's
// FIRST POOL LOOP - the region [0x8007B894, 0x8007C23C) of RASHCDG 0x8007B840(dt), 618 instructions,
// single entry (the fall-through after `bltz` at 0x8007B88C), exit into the thrown walk (collision.h
// RiderPassThrownWalk, whose entry is this region's exit). Over EVERY pool-0 slot from
// *(0x800CE4D0), the count in the pass's frame slot sp+164 (re-read and stored per slot), the stride
// re-read from 0x800CE4D4 per slot. Per slot:
//   flagsC bit 25 (the latch): +0x1C2 / +0x1E0 restored from +0x360 / +0x35C;
//   flagsC & 0x08001800: the list migration 0x80071BCC(e, dt);
//   (s16)+0x140 != 0 (live):
//     unless the rider's +0x228 bit 19: the ROAD EDGE - the slice margins SLCT +0x30 / +0x31 against the
//       lateral +0x158 (or 0x8003E338's junction record when the margin byte is 0), the player's
//       record 0x800D43C0 + 28 h, the two edge planes (the frame's four pointer slots sp+24..+36, the
//       depth sp+160); past an edge: the edge normal into +0x334, then for a crashed / airborne bike
//       ImpactTurn(e, e+0xAC, e+0x334, 3), its sound (+0x142) and BikeRecoverEnd on flagsC bit 12;
//       otherwise the speed +0x240 by ClampLerpMin, +0x2A4 scaled, +0x2B8 / +0x2BC capped at 0.95
//       and flagsA bit 13 (and 14) - the off-road slow-down;
//     flagsB & 0x18000 == 0x8000: the contact released (the launch 0x80084BE8(e, 1) first on bit 17
//       of a bike not airborne), the four accumulators and flagsB bits cleared;
//     flagsC bit 23 (re-seat on the road): the re-bind SLUS 0x800374D4, BikeGroundFrame(e, e+0xB8), on
//       bit 26 the pitch / roll / yaw re-derived from the rows and BikeSteerLean, then the tumble
//       centre +0x310 and the passenger's box, +0x34A for a bike (lb +8 < 2), flagsC &= 0xFA7FFFFF;
//     flagsB bit 16 cleared.
// `sp` is the pass's own frame (entry sp - 248), the stack pointer every callee is called at; the
// region keeps its locals (sp+24..+36, sp+160, sp+164) in the guest frame where the original does.
// `t` needs asin and atan.
struct PoolLoopCallees {
    virtual ~PoolLoopCallees() = default;
    virtual bool ListMigrate(uint32_t e, int32_t dt, uint32_t sp) = 0;          // RASHCDG 0x80071BCC, PORTED
    virtual bool JunctionMargin(uint32_t e, uint32_t sp, uint32_t& v0) = 0;     // SLUS 0x8003E338 (1), not ported
    virtual bool ImpactTurn(uint32_t e, uint32_t partner, uint32_t n, int32_t mode, uint32_t sp,
                            uint32_t& v0) = 0;                                  // RASHCDG 0x80083F30 (4)
    virtual bool PlaySound3D(int32_t x, int32_t z, int32_t id, int32_t bank) = 0; // SLUS 0x80017BA0, PORTED
    virtual bool RecoverEnd(uint32_t e, int32_t dt, uint32_t sp) = 0;           // RASHCDG 0x80072994, PORTED
    virtual bool Launch(uint32_t e, int32_t k, uint32_t sp) = 0;                // RASHCDG 0x80084BE8 (2), not ported
    virtual bool ReleaseContact(uint32_t e) = 0;                                // SLUS 0x8002076C, PORTED
    virtual bool RoadRebind(uint32_t e, int32_t flag, uint32_t sp) = 0;         // SLUS 0x800374D4, PORTED
    virtual bool GroundFrame(uint32_t e, uint32_t ref, uint32_t sp) = 0;        // RASHCDG 0x8007504C, PORTED
    virtual bool SteerLean(uint32_t e, uint32_t sp) = 0;                        // RASHCDG 0x80073D74, PORTED
    virtual bool TurnFacing(uint32_t e, int32_t ang, uint32_t sp) = 0;          // RASHCDG 0x8007ED64 (2), not ported
};
bool RiderPassPoolLoop(GuestRam& g, int32_t dt, uint32_t sp, const BikeTables& t, PoolLoopCallees& c);
constexpr uint32_t kPoolLoopEntry = 0x8007B894;
constexpr uint32_t kPoolLoopExit = 0x8007C23C;
namespace react {
constexpr uint32_t kListMigrate = 0x80071BCC, kJunctionMargin = 0x8003E338, kRecoverEnd = 0x80072994;
constexpr uint32_t kLaunch = 0x80084BE8, kRoadRebind = 0x800374D4, kGroundFrame = 0x8007504C;
constexpr uint32_t kSteerLean = 0x80073D74, kTurnFacing = 0x8007ED64;
} // namespace react

} // namespace rr::sim
