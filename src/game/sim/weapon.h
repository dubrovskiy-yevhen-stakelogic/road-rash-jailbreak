#pragma once
// Weapons in the hand: the weapon object a rider draws when an armed
// stance starts, the armed stance's overlay clip on it, and the effect record it carries - ported
// function by function from our own disassembly of the player's own images:
//
//   RASHCDG.BIN  SHA-1 cfe43a7786759f2cb9c57751cf99e84d1074782c, loaded at 0x8005B5E8
//   SLUS_010.53  SHA-1 67ed165a2c517d4e6106fb0dfa324d66dd9a76f1, text at 0x80010000
//
// and accepted only by `rrverify phys` rows (tools\rrverify\rows_weapon.inc).
//
//   RASHCDG 0x800958F0 WeaponObject(r, side)  127 instructions, frame 48: the first free of the eight
//           172-byte weapon model objects 0x800CF018 (bit i of *(0x8005AD50)) becomes the rider's
//           (r+0x23B = i); a nunchaku or a chain (weapon 0 / 4, the two with moving parts) also gets
//           an animation object (SLUS ViewSlot 0x80012884 -> r+0x22C, on the weapons bank
//           *(0x800CE19C) through SLUS BankSwitch 0x80012858); the model's LOD becomes the weapon in
//           hand (SLUS LodSelect 0x8001298C: group k of model 800 is weapon k); the object is seated on
//           the rider (SLUS Attach 0x80012838, kind 10 - the right hand's part - or 7 for side 0x100,
//           the left); with swings left (rd+0x2F) the effect record 0x800273EC(obj, 0, 1500, 6, 0)
//           and r+0x23C bit 7. v0 1, or 0 when no slot / no animation object was free.
//   RASHCDG 0x800C2F84 OverlayClip(ev, r, flags, rate)  23 instructions, frame 32: on the weapon's
//           animation object r+0x22C (none: v0 untouched), the same stance restarts the clip
//           (0x8005BD74) and an armed stance 145..185 hard-starts clip ev - 145 of the weapons bank
//           (0x8005BF6C, ex 0); any other stance: v0 = 0.
//   SLUS    0x800273EC ObjectEffect(e, a1, a2, a3, [flags])  84 instructions, frame 48: one effect
//           record (the 20 at 0x800D39B0) in state a3 with a1 in bits 14..21, bit 10, byte +60 =
//           flags, +0x30 the race clock, +0x34 = a2, the spray jitter (0x8002705C, speed 120),
//           +0x24/+0x28/+0x2C/+0x3D zero, +0x3E = 30, appended to e's chain (0x800271CC). With
//           flags bit 1 only for a live (handle class < 2) player's own entity. The disassembly's
//           callers name it an "object sound"; what it writes is an effect record.
//
// The callees that live elsewhere are called natively: ViewSlot / Attach (population.h), LodSelect
// (traffic_bind.h), the effect helpers (spine.h), BankSwitch / Restart / HardStart (anim.h). Only
// FreeFarRider (RASHCDG 0x8008CC94, recover_walk.h - it needs the recovery layer's callees) and the
// animation machine's pose side are asked for through `WeaponCallees`.
#include <cstdint>

#include "game/sim/anim.h"
#include "game/sim/road_query.h"
#include "game/sim/spine.h"

namespace rr::sim::weapon {

constexpr uint32_t kWeaponObjectFn = 0x800958F0;
constexpr uint32_t kOverlayClipFn  = 0x800C2F84;
constexpr uint32_t kObjectEffectFn = 0x800273EC; // SLUS

constexpr uint32_t kObjBits     = 0x8005AD50; // bit i: weapon model object i is taken; 0xFF = all
constexpr uint32_t kObjSlots    = 0x800CF018; // 8 x 172-byte model objects (model 800, LOD = weapon)
constexpr uint32_t kObjBytes    = 172;
constexpr uint32_t kAnimDesc    = 0x800CE170; // the animation descriptor (+8 used, +12 capacity)
constexpr uint32_t kWeaponBank  = 0x800CE19C; // the descriptor's bank table entry 3: ANIMTBLW
constexpr uint32_t kFreeFarFlag = 0x8005B254; // FreeFarRider is tried only while this is non-zero
constexpr uint32_t kWeaponModel = 800;        // BBLEVEL<n>.GEO, 10 groups: weapon 0..8 and 9

struct WeaponCallees {
    virtual ~WeaponCallees() = default;
    // RASHCDG 0x8008CC94 FreeFarRider() (ported in recover_walk.h), frame 24 below WeaponObject's.
    virtual bool FreeFarRider(uint32_t& v0) = 0;
};

// RASHCDG 0x800958F0. False when a callee refused or the view faulted.
bool WeaponObject(GuestRam& g, WeaponCallees& c, uint32_t r, uint32_t side, const SpineIo& io, uint32_t& v0);

// RASHCDG 0x800C2F84 on `anim` (an animation machine on the same view). `v0` in: the caller's v0.
bool OverlayClip(AnimMachine& anim, uint32_t ev, uint32_t r, uint32_t flags, uint32_t rate, uint32_t& v0);

// SLUS 0x800273EC.
void ObjectEffect(GuestRam& g, uint32_t e, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t flags, const SpineIo& io);

} // namespace rr::sim::weapon
