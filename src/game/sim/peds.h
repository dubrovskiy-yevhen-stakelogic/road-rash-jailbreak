#pragma once
// The pedestrians (pool 2, docs\formats\population.md 6): the
// spawner the cell walker's kind-2 arm calls, the two per-frame passes and what they reach - the walk
// along / across the road, the edge test, the animation state machine and the "a bike is coming" test.
// Transcribed line by line from our own disassembly of the player's image
//
//   RASHCDG.BIN  SHA-1 cfe43a7786759f2cb9c57751cf99e84d1074782c, loaded at 0x8005B5E8
//   SLUS_010.53  SHA-1 67ed165a2c517d4e6106fb0dfa324d66dd9a76f1, text at 0x80010000
//
// and accepted only by `rrverify phys` rows (tools\rrverify\rows_peds.inc).
//
// MEMORY MODEL, STACK and THE CALL SEAM are recover.h's. Native here (ported elsewhere): the road layer
// (RoadGate, CursorSeat, JunctionSlice, RoadProjectView, RoadPosition, RouteBind, ProgressScalar,
// RoadWindow, RoadClass, RoadsideRun, RoadRebindBody), ModelBind, CarSetup, ViewSlot, PoolRelease,
// LodSelect, Attach, Rand, FixMul, MulAdd, MulAdd32, DotLcm, AiProject, RatAtan2, Length3, ScaleTo16 and
// the GTE OP. Through the seam (`RecoverCallees::Call`): the animation machine (BankSwitch SLUS 0x80012858,
// LoopStart 0x8005C0B0, HardStart 0x8005BF6C, ClipDone 0x8005BE58, StopIfPlaying 0x8005BE20, Resume
// 0x8005BE44), the rider-on-the-ground ports the pedestrian shares (GroundGetUp 0x8008FD5C, RiderSettle
// 0x8009246C, GroundSlide 0x8008F754, BuildObbAlt 0x8008BD2C) and the pedestrian's voice SLUS 0x8001B44C
// (NOT ported: a sound).
//
// Entity fields (572 bytes; population.md 1.2 for the shared header): +0x21C the animation object, +0x220
// the clip id playing, +0x224 the clock of the last "bike coming" reaction, +0x228 flags (bit 0 "start the
// state's clip", bits 8..15 the state, 16..23 the side, 26 / 29 / 30 knocked / thrown), +0x22C the walk
// length, +0x230 the distance walked, +0x234 the walk kind (0 stands, 1 along, 2 across), +0x235 flags (1
// clip running, 2 walking back, 4 walk set up, 8 animation held), +0x236 the class row of 0x800CCC20,
// +0x237 the held object's slot (-1 none).
#include <cstdint>

#include "game/sim/integrator.h"
#include "game/sim/recover.h"

namespace rr::sim {

// ---------------------------------------------------------------------------- the ported functions
constexpr uint32_t kPedEdgeFn    = 0x800CA42C;
constexpr uint32_t kPedWalkAFn   = 0x800CA564;
constexpr uint32_t kPedWalkBFn   = 0x800CA7C4;
constexpr uint32_t kPedStepFn    = 0x800CAA44;
constexpr uint32_t kPedStartFn   = 0x800CAAA8;
constexpr uint32_t kPedAnimFn    = 0x800CAAF0;
constexpr uint32_t kPedFaceFn    = 0x800CB02C;
constexpr uint32_t kPedsPassFn    = 0x800CB304;
constexpr uint32_t kPedsReleaseFn = 0x800CB4F8;
constexpr uint32_t kPedPropFn    = 0x800CB78C;
constexpr uint32_t kPedSpawnFn   = 0x800CB8C8;

// ---------------------------------------------------------------------------- the seams
constexpr uint32_t kPedBankSwitchFn = 0x80012858; // SLUS (a, bank)
constexpr uint32_t kPedLoopStartFn  = 0x8005C0B0; // (a, clip, flags, rate, [sp+16] ex)
constexpr uint32_t kPedHardStartFn  = 0x8005BF6C; // (a, clip, flags, rate, [sp+16] ex)
constexpr uint32_t kPedClipDoneFn   = 0x8005BE58; // (a) -> v0
constexpr uint32_t kPedHoldFn       = 0x8005BE20; // StopIfPlaying (a)
constexpr uint32_t kPedResumeFn     = 0x8005BE44; // (a)
constexpr uint32_t kPedGetUpFn      = 0x8008FD5C; // GroundGetUp (e) -> v0
constexpr uint32_t kPedSettleFn     = 0x8009246C; // RiderSettle (e)
constexpr uint32_t kPedSlideFn      = 0x8008F754; // GroundSlide (e, dt, timer)
constexpr uint32_t kPedBoxFn        = 0x8008BD2C; // BuildObbAlt (e)
constexpr uint32_t kPedVoiceFn      = 0x8001B44C; // SLUS (x, z, e, 0), NOT ported

// ---------------------------------------------------------------------------- the data
constexpr uint32_t kPedBlock      = 0x800D4B70; // +0 live, +4 next free, +8 high, +12 cap - 1, +16 base
constexpr uint32_t kPedParams     = 0x800D8740; // +4 the live cap
constexpr uint32_t kPedSwitch     = 0x8005B254; // PedPass / PedRelease run when non-zero
constexpr uint32_t kPedClassTable = 0x800CCC20; // 16 bytes a class: s16 walks, u16 idle / run / walk clip,
                                                // s32 speed, u32 rate (RASHCDG data)
constexpr uint32_t kPedReactTimer = 0x800CCC1C; // the frames between two "bike coming" reactions
constexpr uint32_t kPedStates     = 0x800D5730; // 6 x {u16 clip, u16 next state, u8 side} (RASHCDI 0x80068500)
constexpr uint32_t kPedClipIds    = 0x800D5F40; // u16 by clip id - 224: bank << 12 | clip (RASHCDI 0x80068500)
constexpr uint32_t kPedClipMap    = 0x800541D4; // SLUS, 8 bytes by clip id < 224: bank | clip << 4
constexpr uint32_t kPedObjects    = 0x800CF018; // 8 x 172-byte held objects (bit mask 0x8005AD50)
constexpr uint32_t kPedObjectMask = 0x8005AD50;
constexpr uint32_t kPedDrag       = 0x800D3964; // the thrown pedestrian's air drag

// 0x800CA42C, 78 instructions, frame 32: the walk reaches the road edge (or its length) this step?
// `step` points at the step length, which is cut to what is left; returns 1 then.
int32_t PedEdge(GuestRam& g, uint32_t e, uint32_t step);
// 0x800CA564 (152 instructions) / 0x800CA7C4 (160), frame 40: one step of the walk along / across the road.
bool PedWalkA(GuestRam& g, uint32_t e, int32_t dt, uint32_t sp, const BikeTables& t);
bool PedWalkB(GuestRam& g, uint32_t e, int32_t dt, uint32_t sp, const BikeTables& t);
// 0x800CAA44, 25 instructions, frame 24: the walk of the kind +0x234 unless the clip holds it (+0x235 bit 0).
bool PedStep(GuestRam& g, uint32_t e, int32_t dt, uint32_t sp, const BikeTables& t);
// 0x800CAAA8, 17 instructions, frame 24: the first (zero-length) step that sets the walk up.
bool PedStart(GuestRam& g, uint32_t e, uint32_t sp, const BikeTables& t);
// 0x800CAAF0, 335 instructions, frame 56: the animation state machine (states 0..5 of 0x800D5730).
bool PedAnim(GuestRam& g, uint32_t e, uint32_t sp, const BikeTables& t, RecoverCallees& c);
// 0x800CB02C, 182 instructions, frame 72: a bike heading at the pedestrian makes it step aside / run.
bool PedFace(GuestRam& g, uint32_t e, uint32_t sp, const BikeTables& t);
// 0x800CB304, 125 instructions, frame 40: WorldBikePass's pedestrian pass.
bool PedPass(GuestRam& g, int32_t dt, uint32_t sp, const BikeTables& t, RecoverCallees& c);
// 0x800CB4F8, 165 instructions, frame 48: the rider-engine pass's pedestrian pass (release, animation,
// the thrown flight).
bool PedRelease(GuestRam& g, int32_t dt, uint32_t sp, const BikeTables& t, RecoverCallees& c);
// 0x800CB78C, 48 instructions, frame 32: a held object of the 8 (LOD `lod`, attached as kind `kind`).
uint32_t PedProp(GuestRam& g, uint32_t e, int32_t kind, int32_t lod);
// 0x800CB8C8, 506 instructions, frame 96: PedSpawn(rec, onRecord, bike). `v0` = the entity or 0.
bool PedSpawn(GuestRam& g, uint32_t rec, uint32_t onRecord, uint32_t bike, uint32_t sp, const BikeTables& t,
              RecoverCallees& c, uint32_t& v0);

} // namespace rr::sim
