#pragma once
// The per-bike step `RASHCDG 0x80075EE0`, region by region, and the
// rider pass's heading writer `RASHCDG 0x8007AC04`. Transcribed from our own
// disassembly of the player's own images:
//
//   RASHCDG.BIN  SHA-1 cfe43a7786759f2cb9c57751cf99e84d1074782c, loaded at 0x8005B5E8
//   SLUS_010.53  SHA-1 67ed165a2c517d4e6106fb0dfa324d66dd9a76f1, text at 0x80010000
//
// and accepted only by `rrverify phys` rows (tools\rrverify\rows_bike_step.inc).
//
// THE MEMORY MODEL is road_query.h's: a guest-address view of main RAM (`GuestRam`). These
// functions walk the intrusive bike lists (`{prev, next}` at entity +0x440, entity = node - 1088)
// and chase entity -> stat block -> other player pointers exactly where the original does, so they
// run on the guest addresses themselves - which is also what lets the bench SPLICE a native region
// into the running original: the region reads and writes the very RAM the
// original's other regions leave and read. Static game tables that are indexed by a masked or
// provably bounded index (the sine/cosine table, RatAtan2's table, the reciprocal-square-root
// table) are handed over as host pointers in `BikeTables`, as the integrator gets them; every table
// or global the original indexes WITHOUT a bound (ENV.EN by the surface byte, the per-handle pad
// table) is read through the view at its guest address.
//
// A function returns false - and the caller must not trust anything it wrote - when the original
// would do something the view refuses (a load outside RAM, an arithmetic-overflow trap in
// `Normalize`, a list that never closes). It never guesses a value.
#include <cstdint>

#include "game/sim/integrator.h"
#include "game/sim/road_query.h"
#include "game/sim/road_runtime.h"

namespace rr::sim {

// ------------------------------------------------------------------------ RASHCDG 0x8008CF74
// void BikeResetOrientation(Bike *e) - 26 instructions, a leaf. The identity: both 3x3 s16
// orientations (+0x1B0 and +0x204) set to I (4096 on the diagonal) and the heading +0x1C2 to
// (0, 0, 4096). The stores are in the original's order; +0x1C2/+0x1C4 are the values it READS
// BACK from +0x1BC/+0x1BE after zeroing them.
void BikeResetOrientation(GuestRam& g, uint32_t e);

// ------------------------------------------------------------------------ RASHCDG 0x8007AC04
// void BikeHeadingPass(list, dt) - 783 instructions, called by the rider pass `0x8007B840` on the
// riding list 0x8005B298 (at 0x8007DDDC) and on 0x8005B350 (0x8007DDEC). `dt` is dead (a1 is
// never read). Per bike on the list, in order:
//
//   * flagsB & 0x3000: +0x2A4 (the lean) and +0x33C := 0;
//   * the heading +0x1C2 := the ground facing +0x210 turned toward +0x204 by the lean (Blend16 by
//     (cos, -sin) of the lean, Normalize, the identity reset 0x8008CF74 on a zero vector, and the
//     aim axis +0x32E := GTE OP(diag +0x20A, heading)) - or a plain copy of +0x210/+0x204 (and
//     flagsB &= ~0x400) while the lean is 0 - when the bike moves or is flagged so;
//   * +0x2DC/+0x2E0/+0x2E4 := 157 x the orientation's +0x206/+0x20C/+0x212 (gravity along the
//     three axes), +0x2F8 the along-heading part turned by the lean;
//   * +0x29C := -(25736 * RatAtan2(+0x2DC, +0x2E0)) >> 8, the slope angle in 16.16 radians;
//   * the steering pair: flagsC & 0x14C -> +0x27C from +0x28C, else +0x28C from +0x27C, each with a
//     stat-block ramp +0x2A0 over stats[+0x128..+0x12C] times stats[+0xEC];
//   * the displayed roll +0x288 (smoothed for players and while 0x8005B220 is set), the display
//     orientation +0x1B0 := RotMatrix(-pitch, 0, -roll) x (+0x204 matrix) on the GTE, the box
//     centre +0xB8 from the contact point +0x1F8 (lifted by the pitch), and +0x244 := +0x240^2.
//
// `list` is the guest address of the list head. Returns false when the view faulted, `Normalize`
// would have raised the console's overflow exception, or the list did not close within 4096 nodes
// (the original would loop for ever).
bool BikeHeadingPass(GuestRam& g, uint32_t list, const BikeTables& t);

// ============================================================================ the step's regions
// Each region of RASHCDG 0x80075EE0 is single-entry / single-exit and carries nothing across its
// boundaries but `dt` and constants, so each is a function of guest memory alone.
// The addresses are the region's [entry, exit).
constexpr uint32_t kBikeStepFn = 0x80075EE0;
constexpr uint32_t kRideListHead = 0x8005B298;  // the riding list
constexpr uint32_t kEnvTable = 0x800D38E0;      // ENV.EN, indexed by the SIGNED surface byte +0x216
constexpr uint32_t kOptionByte = 0x800D80F1;    // s8, the trailing-player option
constexpr uint32_t kPlayerBikes = 0x8005B268;   // player p's bike at +4p
constexpr uint32_t kGameStatePtr = 0x8005B2F8;  // -> game_state; +0x30 the player count

// ------------------------------------------------------------------------ region C
// [0x80076208, 0x80076804), 383 instructions, no calls: walk 1 of the riding list. Per bike:
// flagsA bit 4 (an AI past stats[+0xD0] of lean); the loop-out trigger (flagsC bit 22 from the road
// direction +0x16C, +0x2CC = 0, flagsC |= 0x820 or 0x800 | 0x20/0x40, +0x240 at least 1.0); the
// rider-off trigger (flagsC |= 0x880); the passive resistance +0x2F0/+0x2F4 from ENV and the stat
// block, with the trailing-player term; +0x2FC = +0x2F8 + +0x2F0; +0x2EC; the hard-braking latch
// flagsB bit 7 (a compiler divide by 3850 inside); the brake capacity +0x258 (an inline `divu`
// reciprocal, not FixDiv).
bool BikeStepRegionC(GuestRam& g);

// ------------------------------------------------------------------------ region F
// [0x80078478, 0x80078AAC), 397 instructions, only FixMul / FixDiv: walk 4 of the riding list. Per
// moving, unlatched bike: the lean +0x2A4 either runs a damped second-order response while the bike
// spins its rear (flagsB bit 3) past stats[+0xF4] of steer on the side it leans (+0x2AC seeded on
// the skid edge, bit 4), or decays at 5.0/s and snaps to 0 under 655; a human with only the front
// locked gets the speed-scheduled +0x2CC (stats[+0x164..+0x19C]) or its countdown; then the lateral
// force +0x2E8 = +0x1E8 x a gain ramped over stats[+0xDC]. A stopped or latched bike only loses
// flagsB bit 19. Every bike: flagsB bit 4 cleared, the steering ramp armed on flagsB bit 24 (over
// +0x2D0, +0x294 = -(+0x290 / +0x2D0) sign-split) and run while +0x290 and +0x294 disagree in sign
// and |+0x27C| < stats[+0xE4], +0x27C clamped to +-stats[+0xE4] in any frame the ramp is active.
bool BikeStepRegionF(GuestRam& g, int32_t dt);

// ------------------------------------------------------------------------ region A
// [0x80075F18, 0x800761C8), 172 instructions: the pass over EVERY pool-0 slot 0..*(*0x800CE4DC)
// (dormant ones included), stride re-read from 0x800CE4D4 per slot. Per slot: A1, the AI's flag
// hygiene (flagsB bit 27 cleared; in a wipeout +0x39C = 2 * stats[+0xE0]); A2, the aim-point glide
// while (s16)+0x3B0 != 0 (+0x38C by FixMul(dt, +0x388) toward the sign of +0x3B0, clamped to
// [0, 1.0], +0x370..+0x378 += +0x37C..+0x384 x +0x38C, the timer +0x3B0 counted toward 0 by
// ((dt << 8) + 0x8000) >> 16 and, at 0 from above, re-armed from Length3(+0x37C) unless the top
// AI command is 3, 4 with a timer of 1, or 5..17); A3, the list migration `0x80071BCC(e, dt)` when
// flagsC & 0x08001800 - PORTED as BikeListMigrate below, whose rider-layer callees are not; the
// caller supplies it.
//
// The callees of the regions that are NOT written inline here. The caller supplies each one: the
// bench by the PORTED function on its own view of the same RAM (the list migration, the passenger
// launch, the crash timer, the steering pair) or by the oracle (the stance event, and the rider
// layer under the migration and the launch), the product by its seam objects and its own ported
// calls. Each returns false when the caller could not run it; the region then refuses.
struct BikeStepCallees {
    virtual ~BikeStepCallees() = default;
    // RASHCDG 0x80071BCC(e, dt), the list migration (region A). PORTED: BikeListMigrate (below),
    // called by the caller from the step's own frame (sp - 136).
    virtual bool ListMigrate(uint32_t e, int32_t dt) = 0;
    // RASHCDG 0x80074D58(e), the passenger launch (region D3; its `dt` in a1 is dead). PORTED:
    // BikePassengerLaunch (below).
    virtual bool PassengerLaunch(uint32_t e) = 0;
    // RASHCDG 0x80074E6C(e, dt), BikeCrashTimer (region D3). PORTED (bike.h); the caller runs it
    // on its own view, with its own supply of the crash emitter SLUS 0x80027540.
    virtual bool CrashTimer(uint32_t e, int32_t dt) = 0;
    // RASHCDG 0x800C4550(ev, rider, p), the stance event (region E3). Not ported.
    virtual bool StanceEvent(int32_t ev, uint32_t rider, int32_t p) = 0;
    // RASHCDG 0x80073874(list, dt) BikeSteerDriver and 0x80074C84(list, dt) BikeSteerPass (region
    // B). Both PORTED (bike.h, integrator.h); the caller walks `list` in its own view of the same
    // RAM and runs them.
    virtual bool SteerDriver(uint32_t list, int32_t dt) = 0;
    virtual bool SteerPass(uint32_t list, int32_t dt) = 0;
};

// ------------------------------------------------------------------------ region B
// [0x800761C8, 0x80076208), 16 instructions: exactly four calls, in this order -
// BikeSteerDriver(0x8005B298, dt), BikeSteerDriver(0x8005B2D8, dt), BikeSteerPass(0x8005B298, dt),
// BikeSteerPass(0x8005B2D8, dt) (0x800761D4, 0x800761E8, 0x800761F4, 0x80076200). What region C
// then reads in s1 is the constant 0x8005B298.
constexpr uint32_t kThrownListHead = 0x8005B2D8;
bool BikeStepRegionB(int32_t dt, BikeStepCallees& calls);
// `t.sqrt` is SqrtGte's table window (Length3); the other tables are not read.
bool BikeStepRegionA(GuestRam& g, int32_t dt, const BikeTables& t, BikeStepCallees& calls);

// ------------------------------------------------------------------------ region D
// [0x80076804, 0x800774C8), 817 instructions: walk 2 of the riding list. Per bike: D0, the
// steering-ramp hold (+0x2D0 counts down by dt, throttle and brake amounts zeroed, nothing else);
// D1, the longitudinal command - the AI's (a latched full brake on flagsB bit 19, else a brake-point
// test on +0x394/+0x398, else a chase of the commanded speed +0x39C clamped to
// [-(+0x258) after +0x2F0, stats[+0xBC]]), the analogue axis at 0x800CE540 + 8 * handle, or none;
// D2, that command turned into the pad's encoding in flagsA bits 0/1 and 5/6 (with the AI's drive
// capped by BikeGripLimit 0x8007EF60 on flagsA bit 4); D3, a human's: the passenger launch
// 0x80074D58, BikeCrashTimer 0x80074E6C and, with a second player, the tow (flagsC bit 28, +0x39C
// and flagsA bit 24); D4, the throttle and brake amounts +0x24C/+0x254 slewed by the stat block's
// four rates, +0x2B8/+0x2BC = -1.0 while moving and braking / on throttle, and the coasting swap
// of +0x2F0 into +0x2F4 and the brake override. `t.sqrt` is SqrtGte's window (BikeGripLimit).
bool BikeStepRegionD(GuestRam& g, int32_t dt, const BikeTables& t, BikeStepCallees& calls);

// ------------------------------------------------------------------------ region E
// [0x800774C8, 0x80078478), 1004 instructions: walk 3 of the riding list. Per bike: E0, "drive
// allowed" (not off the road model heading away from the road), the throttle and brake amounts
// clamped to their capacities, the friction-circle ratios +0x2B8/+0x2BC; E1, the lateral margin,
// the longitudinal load (from the steer's cosine), and the GRIP-LOSS FALL (flagsB |= 0x400, a
// +-80 degree target +0x2B0, +0x2C4, +0x2A8, +0x2E8); E2, a latched fall's settle (the heading
// back to the ground facing, +0x2FC = -10 x the margin) or, unlatched, wheelspin (flagsB 0x8/0x10,
// MulAdd into +0x310 on the edge) and front lock (0x4/0x10); E3, the pitch moves (a wheelie on
// stats[+0x108]/[+0x10C], firing the stance event 0x800C4550(22, rider, 8) when one starts from
// rest, and a stoppie on stats[+0x110]); E4, +0x260, THE NET ACCELERATION +0x1E4 = drive - brake +
// passive and THE SPEED +0x240 = max(0, +0x240 + FixMul(+0x1E4, dt)) (or the tow's +0x39C), the
// stop, flagsB bits 12/13, the downhill clamp off the road model (ClampLerpMin), and the pitch
// move's second-order set-up in +0x268..+0x278. `t.sincos` and `t.atan` are read.
bool BikeStepRegionE(GuestRam& g, int32_t dt, const BikeTables& t, BikeStepCallees& calls);

// ============================================================================ the regions' callees
// `sp` is always the
// value of the stack pointer AT THE FUNCTION'S ENTRY, as in road_runtime.h: each function computes
// its own frame from it the way its prologue does, and hands its callees the `sp` the original makes
// the call at, so that a caller that supplies an unported callee from the original code (the bench)
// runs it on exactly the same stack bytes. Nothing is written to the stack by these ports.

// ------------------------------------------------------------------------ RASHCDG 0x8007F08C
// void BikeRideArmReset(Bike *e) - 12 instructions, a leaf, called only by the riding arm of the
// list migration: flagsA &= ~0x00800000; if (e[+0x358]) e[+0x358][+0x1E8] = e[+0x1E8].
void BikeRideArmReset(GuestRam& g, uint32_t e);

// ------------------------------------------------------------------------ SLUS 0x8002ED94
// void RotateRowPair(s16 a[3], s16 b[3], s32 ang) - 47 instructions: the two s16 rows turned in their
// own plane by `ang` (4096 per turn, masked to 12 bits): with S = sin << 4, C = cos << 4 from the
// table at 0x8005624C, tmp = Blend16(a, b, C, -S) into the callee's frame, then b = Blend16(a, b,
// S, C) IN PLACE (Blend16 reads all six inputs before its first store), then a = tmp.
void RotateRowPair(GuestRam& g, uint32_t a, uint32_t b, int32_t ang, const int16_t* sincos);

// The rider layer - RASHCDG 0x80090D84 RiderKnockOff(R) and 0x80091468 RiderLaunch(R), one argument
// each. NOT ported: they reach the rider animation machine and the stance
// layer. `sp` is the stack pointer the original calls them at. Return false when the caller could
// not run the callee; the port then refuses.
struct BikeRiderLayerCallees {
    virtual ~BikeRiderLayerCallees() = default;
    virtual bool RiderKnockOff(uint32_t rider, uint32_t sp) = 0;
    virtual bool RiderLaunch(uint32_t rider, uint32_t sp) = 0;
};

// ------------------------------------------------------------------------ RASHCDG 0x80072994
// void BikeRecoverEnd(Bike *e) - 186 instructions, frame 32 (it runs on the impact frame itself,
// so the name is ours and loose). e[+0x3A4] = 0, flagsB &=
// 0xC0018200; for the passenger's rider (only when the owner's +0x23C has bit 4) and then e's own
// rider: RiderKnockOff when rider[+0x228] bit 15, and RiderLaunch when its stance +0x220 is one of
// {89, 39, 88, 40, 38} (bit 16 set first when the knock-off ran); flagsC &= 0xFFFC7F1F, +0x2D4 = 0;
// then flagsC bit 4 (the lean re-armed at -+3.0, the steering servo reset) or, with flagsC & 0xF,
// the bit-1 -> bit-2 promotion for an unseated rider and, off a crash, MulAdd(+0x1F8, +0x210, +0x134,
// +0x310) and the facing turned by -(652 * FixMul(+0x1E8, +0x228)) >> 16 (RotateRowPair); finally
// flagsC &= ~0x1000. `t.sincos` is read.
bool BikeRecoverEnd(GuestRam& g, uint32_t e, uint32_t sp, const BikeTables& t,
                    BikeRiderLayerCallees& calls);

// ------------------------------------------------------------------------ RASHCDG 0x80071BCC
// void BikeListMigrate(Bike *e, s32 dt) - 86 instructions, frame 40: the
// head is chosen by flagsC (dormant 0x8005B270; flagsC & 0x600: BikeCrashLaunch on bit 11,
// BikeRecoverEnd on 0x1FF with bit 12, then 0x8005B2D8 on bit 10 else 0x8005B378; flagsC & 0x1FF:
// BikeWipeoutStart on bit 11, BikeRecoverEnd on bit 12, 0x8005B350; otherwise BikeRideArmReset and
// the riding list 0x8005B298), the node is unlinked and inserted right after the head, flagsC bit
// 27 cleared. `dt` is dead in the original (only forwarded to 0x80072994, which never reads it),
// so it is not taken. The two crash callees are PORTED (integrator.h) and resolved by the caller.
struct BikeListMigrateCallees : BikeRiderLayerCallees {
    virtual bool CrashLaunch(uint32_t e) = 0;   // RASHCDG 0x80071D24, ported
    virtual bool WipeoutStart(uint32_t e) = 0;  // RASHCDG 0x800723FC, ported
};
bool BikeListMigrate(GuestRam& g, uint32_t e, uint32_t sp, const BikeTables& t,
                     BikeListMigrateCallees& calls);

// ------------------------------------------------------------------------ RASHCDG 0x80074D58
// void BikePassengerLaunch(Bike *e) - 69 instructions, frame 32: gated on flagsA
// bit 10 and the passenger bike's rider seated (+0x25C < 2 unsigned); the passenger P = e[+0x358]
// gets flagsC bit 3 around RiderKnockOff(P[+0x354]), its rider the heading, bit 21 and e[+0x1E0] +
// 0xD6944; a pitch move is set up on e when +0x268 is 0; flagsA bit 10 is consumed.
bool BikePassengerLaunch(GuestRam& g, uint32_t e, uint32_t sp, BikeRiderLayerCallees& calls);

// ============================================================================ regions G, H, I, J
// The callees of the tail of the step. Every one of them but three is PORTED; the caller resolves
// each bike's pointer chases for the ported ones (integrator.h, bike.h, race.h), exactly as it does
// for region B's steering pair. `sp` is the stack pointer the original makes the call at (the step's
// own sp, after its 136-byte prologue).
struct BikeStepTailCallees {
    virtual ~BikeStepTailCallees() = default;
    // RASHCDG 0x8007F0BC BikeIntegrate(e, dt) (region G): ported; the caller resolves the links and
    // the scratchpad active-list cursor at 0x1F800000 and stores the advanced cursor back.
    virtual bool Integrate(uint32_t e, int32_t dt) = 0;
    // RASHCDG 0x8007504C BikeGroundFrame(e, ref) (region H): ported; `ref` is the guest address
    // e+0x1F8 or e+0xB8. Its ground query RASHCDG 0x800A7BF8 is ported too (ground.h); its two
    // output buffers are the original ground frame's frame slots at sp - 120 + 24 / 40 / 72.
    virtual bool GroundFrame(uint32_t e, uint32_t ref, uint32_t sp) = 0;
    // RASHCDG 0x8007FA4C BikeContactFrame(e) (region H): ported, and so is its road re-bind
    // SLUS 0x800374D4 (road_runtime.h RoadRebind, called at sp - 136).
    virtual bool ContactFrame(uint32_t e, uint32_t sp) = 0;
    // The six unread functions under region H's four road passes (road_runtime.h).
    virtual RoadRuntimeCallees& RoadSeams() = 0;
    // RASHCDG 0x800807F0 BikeRiderPose(e, dt) (region I): ported.
    virtual bool RiderPose(uint32_t e, int32_t dt) = 0;
    // RASHCDG 0x80093E6C, the activation pass, and 0x800950E8, the downed-rider pass (region J; no
    // arguments). NOT ported.
    virtual bool ActivationPass(uint32_t sp) = 0;
    virtual bool DownedRiderPass(uint32_t sp) = 0;
    // RASHCDG 0x80092C7C EndRace(bike, reason) (region J): ported; its three callees are not.
    virtual bool EndRace(uint32_t bike, int32_t reason, uint32_t sp) = 0;
};

// ------------------------------------------------------------------------ region G
// [0x80078AAC, 0x80078B54), 42 instructions: the scratchpad active list reset (*(0x1F800000) =
// 0x1F800004), then for every pool-0 slot with (s16)+0x140 != 0: +0x1D4..+0x1DC := +0xB8..+0xC0,
// the same on the passenger e[+0x358] (its pointer re-read for each word), and BikeIntegrate(e, dt),
// which appends the bike to the list. The stride is re-read per slot. The view must have the
// scratchpad attached.
bool BikeStepRegionG(GuestRam& g, int32_t dt, BikeStepTailCallees& calls);

// ------------------------------------------------------------------------ region H
// [0x80078B54, 0x80078C10), 47 instructions: the four road passes (RoadTrackPass 0x80037338,
// RoadClassPass 0x8003E150, RouteCheckPass 0x8003AE24, ProgressPass 0x8003B520), then
// BikeGroundFrame(e, (flagsC & 0x600) ? e+0xB8 : e+0x1F8) and then BikeContactFrame(e) for every
// entity of the active list 0x1F800004 .. *(0x1F800000), the end re-read after every call. `sp` is
// the step's own sp. The view must have the scratchpad attached.
bool BikeStepRegionH(GuestRam& g, uint32_t sp, BikeStepTailCallees& calls);

// ------------------------------------------------------------------------ region I
// [0x80078C10, 0x80078C58), 18 instructions: BikeRiderPose(e, dt) for every pool-0 slot with
// (s16)+0x140 != 0.
bool BikeStepRegionI(GuestRam& g, int32_t dt, BikeStepTailCallees& calls);

// ------------------------------------------------------------------------ region J
// [0x80078C58, 0x80078D84), 75 instructions: 0x80093E6C(), 0x800950E8(), then for each player p <
// game_state[+0x30] (re-read per player, unsigned): bike b = *(0x8005B268 + 4p), view v =
// 0x800CD898 + 1132p, rider r = b[+0x354]; while r[+0x25C] < 3 the odometer *(0x8005B380 + 4p) +=
// FixMul(b[+0x1E0], dt) >> 8; when r[+0x228] bit 19 and v[+0x21C] != 6: EndRace(b, 10) if v[+0x314]
// > 3.0 or (r[+0x25C] < 3 and b[+0x2C + 4p] >= 4801), else v[+0x314] += dt.
bool BikeStepRegionJ(GuestRam& g, int32_t dt, uint32_t sp, BikeStepTailCallees& calls);

// ------------------------------------------------------------------------ RASHCDG 0x80075EE0
// void BikeStep(s32 dt) - the whole step, 2997 instructions: the ten regions in order
// (nothing but `dt` and constants crosses a region boundary). `sp` is the stack
// pointer at the step's entry; the step's own frame is sp - 136. The view must have the scratchpad
// attached. `t` needs sincos, atan, rsqrt and the sqrt window.
bool BikeStep(GuestRam& g, int32_t dt, uint32_t sp, const BikeTables& t, BikeStepCallees& calls,
              BikeStepTailCallees& tail);
constexpr uint32_t kBikeStepFrame = 136; // `addiu sp,sp,-136` at 0x80075EE0

} // namespace rr::sim
