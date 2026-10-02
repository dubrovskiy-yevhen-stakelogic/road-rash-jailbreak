#pragma once
// Maximum detail: every animated model animates in full at every distance. OURS, render-only.
//
// THE ORIGINAL'S RULE (our own disassembly; RASHCDG.BIN SHA-1 cfe43a7786759f2cb9c57751cf99e84d1074782c, SLUS_010.53
// SHA-1 67ed165a2c517d4e6106fb0dfa324d66dd9a76f1). LodChoice 0x800667C4, run every frame by ViewPass 0x8008CFDC on every
// rider (pool 0's riders, the passengers) and every pedestrian (pool 2, 0x8008D36C..), picks the object's LOD from its
// view distance +0x2C against the model's LOD table +0x64 and with it the ANIMATION's detail: the part mask +0x6E0 =
// 0x80052390[LOD] ([LOD + 1] for a kind-4 model, the pedestrians) - 0x1FFFF all 17 parts, 0xDB6F 12, 0x129 only the
// slots 0 3 5 8 (Pose 0x8005D63C skips a part outside the mask: it keeps whatever pose it last had) - and the
// interpolation between keys (+0x24 bit 2) only at LOD 0 (LOD 1 too for a player's rider or a passenger). Measured in
// the product: a pedestrian animates in full only within ~15 world units of the camera, with 12 parts
// to ~21, with 4 beyond; a rival rider (the table 0x80054198: 6 / 12 / 20 units) without interpolation beyond 6, with 12
// parts beyond 12, with 4 beyond 20.
// Two further gates exist and never bite in a race: AnimationPass 0x8005E1D8 poses only an object whose owner's
// +0x09 bit 0 or 1 is set (ModelVisible 0x80067AC4 drew it in view 0 / 1), and PedRelease 0x800CB4F8 (0x800CB558)
// holds the clip of a pedestrian whose +0x09 & 3 is 0 - but bit 1 is the instance binder's (`+9 |= 3`, SLUS 0x8002FE88),
// ModelVisible clears only the bit of the view it tests, a one-player race tests view 0 only, and a two-player race has
// no pedestrians (their sheet cap 0x8005AE10[1] is 0). Measured: every rider and pedestrian had
// +0x09 bit 1 set in every traced frame, no hold.
// Maximum detail draws every model at LOD 0 wherever it is, so the coarse animation shows: 13 of 17 parts frozen in
// an old pose, a clip that steps from key to key.
//
// WHAT THIS DOES. The decision is left where it is: the simulation keeps its LOD bytes, +0x6E0, bit 2 and every part
// slot (the fight code, the sounds, the melee colliders read those). Only the DRAWN pose is replaced:
//   * AnimDetailSnapshot (RaceSession::AnimationPass, right before AnimationPass runs, only while maximum detail draws or
//     the trace is on): a read-only copy of every animation object and its program as the pass finds them - the clock
//     the original poses from.
//   * AnimDetailPose: for a playing object, the PORTED ApplyFrame's pose half (Sample 0x8005E5A4 + Pose 0x8005D63C, or
//     TransitionBlend 0x8005D36C for a blend op) run on that copy with the LOD-0 part mask (0x80052390[0], [1] for a
//     kind-4 model: what LodChoice writes at LOD 0) and interpolation on - the pose the original computes for this
//     object drawn at LOD 0 this frame. A stopped or held object: the arena's slots (the original shows those too).
//   The ported functions run on the arena's own addresses (their clip and table reads are there); every byte they may
//   write - the object, its program, the owner's 17 part slots and its root words +0x1C..+0x21 - is saved first and
//   put back before the call returns (one thread: the simulation never sees it). RRJB_ANIM_DETAIL_CHECK=1 hashes the
//   whole arena around every call (must be equal) and compares the pose with the game's own where the game posed at
//   LOD 0 this frame (must be equal). RRJB_ANIM_DETAIL=off: the negative control (the game's slots drawn).
//   A blend's part mask is taken as the LOD-0 mask (the capture ANDs the mask of its moment into it).
#include <cstdint>
#include <string>

#include "game/rider_pose.h"

namespace rr::game {

// The renderer sets it per view (maximum detail on / off); off, nothing is copied and nothing is computed.
void SetAnimDetail(bool on);
bool AnimDetailOn();

// RaceSession::AnimationPass, before AnimationPass 0x8005E1D8 (reads only).
void AnimDetailSnapshot(const uint8_t* ram);

// What the original does to this object's animation this frame (for the trace), and what was drawn.
struct AnimDetailInfo {
    bool object = false;   // an animation object whose owner is `owner`, in this frame's copy
    bool playing = false;  // flags bit 1
    bool posed = false;    // AnimationPass posed it this frame (owner +9 bits 0/1 last frame, or flags bit 4 clear)
    bool interp = false;   // flags bit 2
    bool held = false;     // a pedestrian's held clip (+0x235 bit 3)
    uint32_t mask = 0, lod0Mask = 0;
    uint8_t visible = 0;   // owner +0x09 & 3 at the pass
    bool ours = false;     // the full pose was computed and handed out
    bool Full() const { return playing && posed && interp && (mask & lod0Mask) == lod0Mask; }
};

// The full-detail pose of `owner`'s animation object (+0x21C). False: draw the arena's slots (no copy yet, no
// object, not playing and not held, or a port refused). Computed once per copy; later views get the same pose.
bool AnimDetailPose(uint8_t* ram, uint32_t owner, RiderPoseView& out, AnimDetailInfo* info = nullptr);
// Only the description (no pose), for the trace of a run that does not draw at maximum detail.
AnimDetailInfo AnimDetailDescribe(const uint8_t* ram, uint32_t owner);

std::string AnimDetailLine();

} // namespace rr::game
