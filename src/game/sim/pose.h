#pragma once
// The POSE side of the rider animation machine, transcribed from our own
// disassembly of the player's own images:
//
//   RASHCDG.BIN  SHA-1 cfe43a7786759f2cb9c57751cf99e84d1074782c, loaded at 0x8005B5E8
//   SLUS_010.53  SHA-1 67ed165a2c517d4e6106fb0dfa324d66dd9a76f1, text at 0x80010000
//
// and accepted only by `rrverify phys` rows (tools\rrverify\rows_pose.inc).
//
//   RASHCDG 0x8005E558 Bits           the MSB-first bit reader of a coded channel
//   RASHCDG 0x8005E5A4 Sample         steps every channel of the current clip to `frame` / `frame + 1`
//   RASHCDG 0x8005D63C Pose           root (SetRoot) + one QuatToMatrix per part, the stance-42 tilt
//   RASHCDG 0x8005CB70 TransitionCapture   the blend's FROM / TO pose into the blend buffer
//   RASHCDG 0x8005D36C TransitionBlend     slerp FROM -> TO over the blend op
//   RASHCDG 0x8005D2A8 ApplyFrame     op 3 -> blend, else Sample + Pose; flags byte | 0x10; facing toggle
//   RASHCDG 0x80066A60 SetRoot        owner +0x1C/+0x1E/+0x20
//   RASHCDG 0x800714FC QuatMul        1.14 quaternion product
//   RASHCDG 0x800710C0 Slerp          Asin + the sine table + four sign-armed FixDivs
//   SLUS    0x8001005C QuatToMatrix   s32[4] (1.14 << 2) -> the 4.12 3x3 of a part slot
//
// `AnimPose` IS an `AnimPoseSeam`: the clock (anim.h) and the stance layer (stance.h) call these two
// entry points through it, so a product that hands them an `AnimPose` runs the whole machine with no
// seam at all.
//
// MEMORY MODEL: road_query.h's `GuestRam`. Everything the original writes outside its own stack frame
// - the channels, the owner's root words, the part slots behind owner +4, the blend buffer, the op's
// play flags - is written at its guest address in the original's order. What the original keeps in its
// OWN frame (the lerped quaternion, the root triple, QuatMul's and Slerp's outputs when they point
// there) is host-local here: nothing outside the frame can observe it. The GTE is not modelled as a
// register file: the four MVMVAs of the stance-42 tilt are computed natively (sf = 1, lm = 0, IR
// saturated to s16), and only their stores are made; the GTE's registers are never read back by the
// pose.
//
// Refusals: `Failed()` when the view faults, or when QuatToMatrix's trapping `add`/`sub` would overflow
// (the console raises an exception there; the port refuses instead of guessing).
#include <cstdint>

#include "game/sim/anim.h"
#include "game/sim/road_query.h"

namespace rr::sim {

constexpr uint32_t kPoseBitsFn         = 0x8005E558;
constexpr uint32_t kPoseSampleFn       = 0x8005E5A4;
constexpr uint32_t kPoseFn             = 0x8005D63C;
constexpr uint32_t kPoseBlendFn        = 0x8005D36C;
constexpr uint32_t kPoseSetRootFn      = 0x80066A60;
constexpr uint32_t kPoseQuatMulFn      = 0x800714FC;
constexpr uint32_t kPoseSlerpFn        = 0x800710C0;
constexpr uint32_t kPoseQuatToMatrixFn = 0x8001005C; // SLUS

// Data the pose reads (all resident in the images; the repository stores none of it).
constexpr uint32_t kPoseSeatTable   = 0x800CC1C4; // u32 part mask; s16 root offset at +4/+6/+8; s16[4] quats at +10 + 8k
constexpr uint32_t kPoseMirrorSlots = 0x800CC1B0; // u8[20]: part k's slot when the clip plays mirrored
constexpr uint32_t kPoseSinCos      = 0x8005624C; // SLUS {s16 sin, s16 cos}[4096]
constexpr uint32_t kPoseAsinTable   = 0x800527E0; // SLUS, Asin's 61 x u16 (integrator.h)
constexpr uint32_t kPoseStanceTable = 0x800541D4; // SLUS, 8-byte stance records (+2 the category)
constexpr uint32_t kPosePartMasks   = 0x80052390; // SLUS, the LOD part masks 0x800667C4 writes to +0x6E0

class AnimPose final : public AnimPoseSeam {
public:
    explicit AnimPose(GuestRam& g) : g_(g) {}

    bool Failed() const { return refused_ || g_.Faulted(); }

    // ---- the seam's two entry points
    uint32_t TransitionCapture(uint32_t a, uint32_t op, uint32_t blend) override; // 0x8005CB70
    uint32_t ApplyFrame(uint32_t a) override;                                     // 0x8005D2A8

    // ---- the functions under them, each one guest function
    uint32_t Bits(uint32_t p, uint32_t pos, uint32_t w);   // 0x8005E558, `p`/`pos` guest addresses
    uint32_t Sample(uint32_t a, uint32_t t);               // 0x8005E5A4
    uint32_t Pose(uint32_t a);                             // 0x8005D63C
    uint32_t Blend(uint32_t a);                            // 0x8005D36C
    uint32_t SetRoot(uint32_t owner, uint32_t v);          // 0x80066A60, `v` a guest s16[3]
    uint32_t QuatMul(uint32_t a, uint32_t b, uint32_t out);            // 0x800714FC, guest s16[4]s
    uint32_t Slerp(int32_t t, uint32_t q0, uint32_t q1, uint32_t out); // 0x800710C0, guest s16[4]s
    uint32_t QuatToMatrix(uint32_t m, uint32_t q);         // SLUS 0x8001005C, `q` a guest s32[4]

    // Host forms of the leaves for the callers above, whose operands live in their own frames.
    void QuatMulHost(const int16_t a[4], const int16_t b[4], int16_t out[4]) const;
    bool SlerpWeights(int32_t t, const int16_t q0[4], const int16_t q1[4], int32_t& w0, int32_t& w1);
    void QuatToMatrixHost(uint32_t m, const int32_t q[4]);

private:
    int16_t SinAt(uint32_t i) { return g_.S16(kPoseSinCos + 4u * (i & 0xFFFu)); }
    int16_t CosAt(uint32_t i) { return g_.S16(kPoseSinCos + 4u * (i & 0xFFFu) + 2u); }
    bool AsinOf(int32_t x, int32_t& out);
    void PoseRoot(uint32_t owner, int16_t x, int16_t y, int16_t z);

    GuestRam& g_;
    bool refused_ = false;
};

// R3000 `div` (signed): every corner case defined, no exception (r3000.cpp 0x1A).
inline int32_t GuestDiv(int32_t n, int32_t d) {
    if (d == 0) return n >= 0 ? -1 : 1;
    if (static_cast<uint32_t>(n) == 0x80000000u && d == -1) return n;
    return n / d;
}

} // namespace rr::sim
